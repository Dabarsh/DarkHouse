// DarkHouse — minimal fork-join parallel loop (internal).
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

namespace darkhouse::detail {

// Calls fn(begin, end) on disjoint chunks that cover [0, count), on up to
// hardware_concurrency threads (at most 8), the calling thread included.
// Chunks of `grain` items are handed out dynamically, so uneven work
// balances. The first exception thrown by fn is rethrown here once every
// thread has stopped.
template <class F>
void parallelFor(std::size_t count, std::size_t grain, F&& fn) {
    grain = std::max<std::size_t>(grain, 1);
    const std::size_t chunks = (count + grain - 1) / grain;
    const std::size_t threads =
        std::min<std::size_t>({chunks, std::max(1u, std::thread::hardware_concurrency()), 8});
    if (threads <= 1) {
        if (count > 0) fn(std::size_t{0}, count);
        return;
    }

    std::atomic<std::size_t> next{0};
    std::exception_ptr failure;
    std::mutex failureMutex;
    auto worker = [&] {
        try {
            for (std::size_t chunk = next++; chunk < chunks; chunk = next++) {
                const std::size_t begin = chunk * grain;
                fn(begin, std::min(count, begin + grain));
            }
        } catch (...) {
            next = chunks;  // stop handing out work
            std::lock_guard lock(failureMutex);
            if (!failure) failure = std::current_exception();
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(threads - 1);
    for (std::size_t i = 1; i < threads; ++i) {
        try {
            pool.emplace_back(worker);
        } catch (const std::system_error&) {
            break;  // out of threads: the ones running (and this one) do the rest
        }
    }
    worker();
    for (std::thread& thread : pool) thread.join();
    if (failure) std::rethrow_exception(failure);
}

}  // namespace darkhouse::detail
