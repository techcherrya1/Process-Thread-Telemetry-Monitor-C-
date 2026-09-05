// monitor_win.cpp — Windows port of monitor.cpp
//
// A lightweight process/thread telemetry tool.
//
// - Launches a target program as a child process (CreateProcess, the
//   Windows equivalent of fork+exec).
// - A background "collector" thread samples the child's CPU time
//   (GetProcessTimes), thread count (Toolhelp32 snapshot), and working-set
//   memory (GetProcessMemoryInfo) every N ms.
// - A background "writer" thread drains those samples into a CSV trace
//   log, connected to the collector via a mutex + condition_variable
//   producer-consumer queue (the "thread synchronization" part).
// - The main thread waits on the child (CreateProcess -> run ->
//   WaitForSingleObject -> exit), then stops the workers and analyzes the
//   trace for basic patterns/anomalies.
//
// Usage: monitor.exe <program> [args...]
// Example: monitor.exe workload.exe 4 4000
//
// Build (MinGW-w64):   g++ -std=c++17 -O2 -static -o monitor.exe monitor_win.cpp -lpsapi
// Build (MSVC cl.exe): cl /EHsc /std:c++17 monitor_win.cpp /link psapi.lib

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <chrono>
#include <cmath>
#include <algorithm>

#if defined(_MSC_VER)
#pragma comment(lib, "psapi.lib")
#endif

struct Sample {
    long long timestamp_ms;
    int threads;
    double cpu_percent;
    long rss_kb;
};

// Combines process kernel+user FILETIMEs into a single 64-bit tick count
// (100-nanosecond units, same layout Windows itself uses).
static unsigned long long filetimeToTicks(const FILETIME &ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

// Reads combined kernel+user CPU time for the process, in 100ns ticks.
bool readCpuTicks(HANDLE hProcess, unsigned long long &ticks) {
    FILETIME creation, exit, kernel, user;
    if (!GetProcessTimes(hProcess, &creation, &exit, &kernel, &user)) return false;
    ticks = filetimeToTicks(kernel) + filetimeToTicks(user);
    return true;
}

// Counts live threads belonging to pid via a Toolhelp snapshot.
bool readThreadCount(DWORD pid, int &threads) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    threads = 0;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid) threads++;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return true;
}

// Reads working-set memory (Windows' analog of RSS) in KB.
bool readRssKb(HANDLE hProcess, long &rssKb) {
    PROCESS_MEMORY_COUNTERS pmc;
    if (!GetProcessMemoryInfo(hProcess, &pmc, sizeof(pmc))) return false;
    rssKb = static_cast<long>(pmc.WorkingSetSize / 1024);
    return true;
}

// Thread-safe queue connecting the collector (producer) and writer
// (consumer) threads.
class TraceBuffer {
public:
    void push(const Sample &s) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            queue_.push(s);
        }
        cv_.notify_one();
    }

    // Blocks until a sample is available, or returns false once stopped
    // and drained.
    bool pop(Sample &out) {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait(lock, [this] { return !queue_.empty() || stopped_; });
        if (queue_.empty()) return false;
        out = queue_.front();
        queue_.pop();
        return true;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            stopped_ = true;
        }
        cv_.notify_all();
    }

private:
    std::queue<Sample> queue_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool stopped_ = false;
};

// Producer: samples the target process on a fixed interval until it exits
// or `running` is cleared.
void collectorThread(HANDLE hProcess, DWORD pid, int sample_ms, TraceBuffer &buf, std::atomic<bool> &running) {
    unsigned long long prevTicks = 0;
    readCpuTicks(hProcess, prevTicks);
    auto prevTime = std::chrono::steady_clock::now();

    while (running.load()) {
        // Sleep in small slices so we notice the process exiting promptly.
        std::this_thread::sleep_for(std::chrono::milliseconds(sample_ms));

        if (WaitForSingleObject(hProcess, 0) == WAIT_OBJECT_0) {
            break; // process has exited
        }

        unsigned long long ticks;
        int threads;
        long rssKb;
        if (!readCpuTicks(hProcess, ticks) || !readThreadCount(pid, threads) || !readRssKb(hProcess, rssKb)) {
            break; // process has likely exited between checks
        }

        auto now = std::chrono::steady_clock::now();
        double elapsedSec = std::chrono::duration<double>(now - prevTime).count();
        double cpuPct = 0.0;
        if (elapsedSec > 0) {
            // Ticks are 100ns units -> seconds is ticks * 1e-7.
            double deltaCpuSec = static_cast<double>(ticks - prevTicks) * 1e-7;
            cpuPct = (deltaCpuSec / elapsedSec) * 100.0;
        }
        prevTicks = ticks;
        prevTime = now;

        long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        buf.push(Sample{nowMs, threads, cpuPct, rssKb});
    }
    running.store(false);
    buf.stop();
}

// Consumer: drains samples to a CSV trace log.
void writerThread(TraceBuffer &buf, const std::string &path) {
    std::ofstream out(path);
    out << "timestamp_ms,threads,cpu_percent,rss_kb\n";
    Sample s;
    while (buf.pop(s)) {
        out << s.timestamp_ms << "," << s.threads << ","
            << std::fixed << std::setprecision(2) << s.cpu_percent << ","
            << s.rss_kb << "\n";
        out.flush();
    }
}

struct Stats {
    int samples = 0;
    double avgCpu = 0, peakCpu = 0;
    int avgThreads = 0, peakThreads = 0;
    long startRss = 0, endRss = 0, peakRss = 0;
    std::vector<std::string> anomalies;
};

// Basic trace analysis: averages/peaks, plus a simple anomaly flag for
// sudden thread-count jumps.
Stats analyzeTrace(const std::string &path) {
    Stats st;
    std::ifstream f(path);
    std::string line;
    std::getline(f, line); // skip header

    double sumCpu = 0;
    long sumThreads = 0;
    int prevThreads = -1;
    int idx = 0;

    while (std::getline(f, line)) {
        std::istringstream iss(line);
        std::string tsStr, thStr, cpuStr, rssStr;
        std::getline(iss, tsStr, ',');
        std::getline(iss, thStr, ',');
        std::getline(iss, cpuStr, ',');
        std::getline(iss, rssStr, ',');
        if (tsStr.empty()) continue;

        int threads = std::stoi(thStr);
        double cpu = std::stod(cpuStr);
        long rss = std::stol(rssStr);

        if (st.samples == 0) st.startRss = rss;
        st.endRss = rss;
        st.peakRss = std::max(st.peakRss, rss);
        st.peakCpu = std::max(st.peakCpu, cpu);
        st.peakThreads = std::max(st.peakThreads, threads);
        sumCpu += cpu;
        sumThreads += threads;
        st.samples++;

        if (prevThreads >= 0 && std::abs(threads - prevThreads) >= 2) {
            st.anomalies.push_back(
                "sample " + std::to_string(idx) + ": thread count jumped from " +
                std::to_string(prevThreads) + " to " + std::to_string(threads));
        }
        prevThreads = threads;
        idx++;
    }

    if (st.samples > 0) {
        st.avgCpu = sumCpu / st.samples;
        st.avgThreads = static_cast<int>(std::round(static_cast<double>(sumThreads) / st.samples));
    }
    return st;
}

// CreateProcess needs one command-line string, not an argv array, and the
// buffer it's given must be writable. Build "arg0 arg1 arg2 ..." from argv,
// quoting any argument that contains whitespace.
static std::string buildCommandLine(int argc, char **argv, int startIdx) {
    std::ostringstream oss;
    for (int i = startIdx; i < argc; ++i) {
        std::string a = argv[i];
        bool needsQuotes = a.find(' ') != std::string::npos || a.empty();
        if (i > startIdx) oss << ' ';
        if (needsQuotes) oss << '"' << a << '"';
        else oss << a;
    }
    return oss.str();
}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <program> [args...]\n";
        std::cerr << "Example: " << argv[0] << " workload.exe 4 4000\n";
        return 1;
    }

    std::string cmdLine = buildCommandLine(argc, argv, 1);
    std::vector<char> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back('\0'); // CreateProcessA needs a mutable, NUL-terminated buffer

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    BOOL ok = CreateProcessA(
        nullptr,        // use the command line to resolve the executable
        cmdBuf.data(),  // mutable command line buffer
        nullptr, nullptr,
        FALSE,          // don't inherit handles
        0,
        nullptr, nullptr,
        &si, &pi);

    if (!ok) {
        std::cerr << "CreateProcess failed (error " << GetLastError() << ") for: " << cmdLine << "\n";
        return 1;
    }

    HANDLE hProcess = pi.hProcess;
    DWORD pid = pi.dwProcessId;
    CloseHandle(pi.hThread); // don't need the thread handle

    std::cout << "[monitor] launched target pid=" << pid << " (" << argv[1] << ")\n";

    TraceBuffer buf;
    std::atomic<bool> running{true};
    std::thread collector(collectorThread, hProcess, pid, 200, std::ref(buf), std::ref(running));
    std::thread writer(writerThread, std::ref(buf), "trace.csv");

    WaitForSingleObject(hProcess, INFINITE); // parent blocks until child exits
    DWORD exitCode = 0;
    GetExitCodeProcess(hProcess, &exitCode);
    std::cout << "[monitor] target pid=" << pid << " exited (status " << exitCode << ")\n";

    running.store(false);
    buf.stop();
    collector.join();
    writer.join();
    CloseHandle(hProcess);

    std::cout << "[monitor] trace written to trace.csv\n\n";

    Stats st = analyzeTrace("trace.csv");
    std::cout << "=== Trace Analysis Summary ===\n";
    std::cout << "Samples collected : " << st.samples << "\n";
    std::cout << "Avg CPU usage     : " << std::fixed << std::setprecision(1) << st.avgCpu << "%\n";
    std::cout << "Peak CPU usage    : " << st.peakCpu << "%\n";
    std::cout << "Avg thread count  : " << st.avgThreads << "\n";
    std::cout << "Peak thread count : " << st.peakThreads << "\n";
    std::cout << "RSS memory        : " << st.startRss << " KB -> " << st.endRss
               << " KB (peak " << st.peakRss << " KB)\n";
    if (st.anomalies.empty()) {
        std::cout << "Anomalies         : none detected\n";
    } else {
        std::cout << "Anomalies detected (" << st.anomalies.size() << "):\n";
        for (auto &a : st.anomalies) std::cout << "  - " << a << "\n";
    }
    return 0;
}
