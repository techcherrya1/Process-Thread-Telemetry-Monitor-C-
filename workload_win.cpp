// workload_win.cpp — Windows port of workload.cpp
//
// A small "target" program for monitor_win.cpp to observe. It ramps worker
// threads up, then down, burning CPU in between — so the monitor has real
// thread-count and CPU fluctuation to sample and log.
//
// Usage: workload.exe [maxThreads] [totalMs]

#include <iostream>
#include <thread>
#include <vector>
#include <memory>
#include <chrono>
#include <atomic>
#include <cstdlib>
#include <process.h>   // _getpid()

void busyWorker(std::atomic<bool> &stop) {
    volatile double x = 0.0;
    while (!stop.load()) {
        for (int i = 0; i < 100000; ++i) {
            x += i * 0.000001; // burn CPU so the monitor sees non-zero usage
        }
    }
}

int main(int argc, char **argv) {
    int maxThreads = argc > 1 ? std::atoi(argv[1]) : 4;
    int totalMs    = argc > 2 ? std::atoi(argv[2]) : 4000;

    std::cout << "[workload] pid=" << _getpid()
              << " maxThreads=" << maxThreads
              << " duration=" << totalMs << "ms\n";

    std::vector<std::thread> workers;
    std::vector<std::shared_ptr<std::atomic<bool>>> stopFlags;

    int phaseMs = std::max(1, totalMs / (maxThreads * 2));

    // Ramp threads up, one at a time
    for (int i = 0; i < maxThreads; ++i) {
        auto stopFlag = std::make_shared<std::atomic<bool>>(false);
        stopFlags.push_back(stopFlag);
        workers.emplace_back(busyWorker, std::ref(*stopFlag));
        std::this_thread::sleep_for(std::chrono::milliseconds(phaseMs));
    }

    // Ramp threads down, one at a time
    for (int i = 0; i < maxThreads; ++i) {
        stopFlags[i]->store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(phaseMs));
    }

    for (auto &t : workers) t.join();

    std::cout << "[workload] done\n";
    return 0;
}
