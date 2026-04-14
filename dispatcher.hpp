#pragma once

/**
 * @file dispatcher.hpp
 * @brief Background thread that promotes PENDING tasks to READY.
 *
 * The Dispatcher owns the task registry and the PriorityTaskQueue.
 * It continuously scans PENDING tasks and promotes them when:
 *   1. Their notBefore time has passed, AND
 *   2. All dependency task IDs have reached TaskStatus::DONE.
 *
 * Once a task is READY it is pushed onto the PriorityTaskQueue so
 * that the ThreadPool can pick it up.
 */

#include "priority_queue.hpp"
#include "task.hpp"
#include "types.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>

namespace scheduler {

/**
 * @brief Manages task state and promotion from PENDING → READY.
 *
 * The Dispatcher runs its own background thread.  Call wakeUp() after
 * inserting tasks or after a task completes to trigger an immediate scan.
 */
class Dispatcher {
public:
    Dispatcher();
    ~Dispatcher();

    // Non-copyable, non-movable
    Dispatcher(const Dispatcher&) = delete;
    Dispatcher& operator=(const Dispatcher&) = delete;

    // ── Task registration ─────────────────────────────────────────────────────

    /**
     * @brief Register a task and assign it the next available ID.
     * @return The assigned TaskId.
     *
     * If the task has no dependencies and is ready to run immediately it is
     * promoted to READY and pushed onto the queue before this call returns.
     */
    TaskId addTask(Task task);

    // ── Cancellation ──────────────────────────────────────────────────────────

    /**
     * @brief Attempt to cancel a PENDING or READY task.
     * @return true if the task was successfully cancelled.
     */
    bool cancel(TaskId id);

    // ── Queries ───────────────────────────────────────────────────────────────

    TaskStatus statusOf(TaskId id) const;

    /// Non-blocking lookup of a task pointer (nullptr if not found).
    /// Caller must hold a shared lock on taskMutex() while using the pointer.
    Task* findTask(TaskId id);
    const Task* findTask(TaskId id) const;

    /// Shared mutex guarding the task registry – exposed for the scheduler.
    std::shared_mutex& taskMutex() noexcept { return _tasksMu; }

    /// Reference to the ready queue consumed by the ThreadPool dispatch.
    PriorityTaskQueue& readyQueue() noexcept { return _readyQueue; }

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    /// Wake the dispatch thread immediately (e.g. after a task completes).
    void wakeUp();

    /**
     * @brief Set a callback invoked (from the dispatch thread) whenever
     *        one or more tasks are promoted to READY.
     * TaskScheduler uses this to feed newly-ready tasks into the ThreadPool.
     */
    void setOnReady(std::function<void()> cb) { _onReady = std::move(cb); }

    void stop(bool drain = true);

    // ── Stats ─────────────────────────────────────────────────────────────────

    struct Snapshot {
        size_t total, pending, ready, running, done, failed, cancelled;
    };
    Snapshot snapshot() const;

private:
    void dispatchLoop();
    void promoteReady(std::unique_lock<std::shared_mutex>& lock);

    mutable std::shared_mutex                    _tasksMu;
    std::unordered_map<TaskId, Task>             _tasks;
    std::atomic<TaskId>                          _nextId { 1 };

    PriorityTaskQueue                            _readyQueue;

    std::mutex                                   _cvMu;
    std::condition_variable                      _cv;
    bool                                         _wake      = false;
    bool                                         _stop      = false;
    bool                                         _drain     = true;

    std::thread                                  _thread;
    std::function<void()>                        _onReady;
};

} // namespace scheduler
