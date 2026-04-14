#pragma once

/**
 * @file thread_pool.hpp
 * @brief Fixed-size thread pool that pulls work items off a shared queue.
 *
 * ThreadPool is intentionally simple: it knows nothing about priorities,
 * dependencies, or scheduling.  Higher-level policy belongs in Dispatcher.
 */

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace scheduler {

/**
 * @brief A classic fixed-size thread pool.
 *
 * Workers block on an internal queue and execute submitted callables
 * one at a time.  The pool can be shut down with or without draining
 * the pending queue.
 */
class ThreadPool {
public:
    /**
     * @param numWorkers  Number of worker threads to spawn.
     *                    Defaults to hardware_concurrency(); minimum 1.
     */
    explicit ThreadPool(size_t numWorkers = std::thread::hardware_concurrency());
    ~ThreadPool();

    // Non-copyable, non-movable
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /**
     * @brief Enqueue a callable for execution.
     * @throws std::runtime_error if the pool has been shut down.
     */
    void enqueue(std::function<void()> work);

    /**
     * @brief Signal all workers to stop.
     * @param drain  If true, finish queued work before stopping.
     *               If false, discard pending items immediately.
     */
    void shutdown(bool drain = true);

    /// Number of worker threads.
    size_t size() const noexcept { return _workers.size(); }

    /// Approximate number of items waiting in the queue.
    size_t pendingCount() const;

private:
    void workerLoop();

    mutable std::mutex              _mu;
    std::condition_variable         _cv;
    std::queue<std::function<void()>> _queue;
    std::vector<std::thread>        _workers;
    bool                            _stop       = false;
    bool                            _drainOnStop = true;
};

} // namespace scheduler
