/**
 * @file thread_pool.cpp
 * @brief ThreadPool implementation.
 */

#include "thread_pool.hpp"

#include <stdexcept>

namespace scheduler {

// ── Constructor / Destructor ──────────────────────────────────────────────────

ThreadPool::ThreadPool(size_t numWorkers) {
    if (numWorkers == 0) numWorkers = 1;
    _workers.reserve(numWorkers);
    for (size_t i = 0; i < numWorkers; ++i)
        _workers.emplace_back(&ThreadPool::workerLoop, this);
}

ThreadPool::~ThreadPool() {
    shutdown(/*drain=*/false);
}

// ── Public API ────────────────────────────────────────────────────────────────

void ThreadPool::enqueue(std::function<void()> work) {
    {
        std::unique_lock lock(_mu);
        if (_stop)
            throw std::runtime_error("ThreadPool: enqueue on stopped pool");
        _queue.push(std::move(work));
    }
    _cv.notify_one();
}

void ThreadPool::shutdown(bool drain) {
    {
        std::unique_lock lock(_mu);
        if (_stop) return;
        _drainOnStop = drain;
        _stop = true;
    }
    _cv.notify_all();
    for (auto& w : _workers)
        if (w.joinable()) w.join();
}

size_t ThreadPool::pendingCount() const {
    std::unique_lock lock(_mu);
    return _queue.size();
}

// ── Private ───────────────────────────────────────────────────────────────────

void ThreadPool::workerLoop() {
    while (true) {
        std::function<void()> work;
        {
            std::unique_lock lock(_mu);
            _cv.wait(lock, [this] {
                return _stop || !_queue.empty();
            });

            if (_stop && (!_drainOnStop || _queue.empty()))
                return;

            if (_queue.empty()) return;

            work = std::move(_queue.front());
            _queue.pop();
        }
        work();
    }
}

} // namespace scheduler
