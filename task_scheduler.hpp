#pragma once

/**
 * @file task_scheduler.hpp
 * @brief Public API for the multi-threaded task scheduler.
 *
 * TaskScheduler wires together:
 *   - Dispatcher  – task registry + PENDING → READY promotion
 *   - ThreadPool  – worker threads that execute READY tasks
 *
 * Usage example:
 * @code
 *   scheduler::TaskScheduler sched(4);          // 4 worker threads
 *
 *   // Simple lambda
 *   auto id = sched.submit("hello", [] { std::cout << "hi\n"; });
 *
 *   // Full task with deps, priority, period …
 *   scheduler::Task t;
 *   t.name     = "periodic";
 *   t.priority = scheduler::Priority::HIGH;
 *   t.period   = std::chrono::seconds(1);
 *   t.fn       = [] { doWork(); };
 *   auto pid = sched.submit(std::move(t));
 *
 *   sched.waitAll();
 *   sched.shutdown();
 * @endcode
 */

#include "dispatcher.hpp"
#include "task.hpp"
#include "thread_pool.hpp"
#include "types.hpp"

#include <string>
#include <functional>

namespace scheduler {

/**
 * @brief High-level task scheduler facade.
 *
 * Thread-safe: all public methods may be called from any thread simultaneously.
 */
class TaskScheduler {
public:
    /**
     * @param numWorkers  Worker thread count (0 → hardware_concurrency).
     */
    explicit TaskScheduler(size_t numWorkers = std::thread::hardware_concurrency());
    ~TaskScheduler();

    // Non-copyable, non-movable
    TaskScheduler(const TaskScheduler&) = delete;
    TaskScheduler& operator=(const TaskScheduler&) = delete;

    // ── Submit ────────────────────────────────────────────────────────────────

    /**
     * @brief Submit a fully configured Task.
     * @return The assigned TaskId (also written into task.id).
     */
    TaskId submit(Task task);

    /**
     * @brief Convenience overload – submit a named lambda.
     */
    TaskId submit(std::string name,
                  std::function<void()> fn,
                  Priority priority = Priority::NORMAL);

    // ── Cancel ────────────────────────────────────────────────────────────────

    /**
     * @brief Cancel a PENDING or READY task.
     * @return true if cancellation succeeded.
     */
    bool cancel(TaskId id);

    // ── Query ─────────────────────────────────────────────────────────────────

    /// Current status of a task.
    TaskStatus statusOf(TaskId id) const;

    /**
     * @brief Block until the task reaches a terminal state
     *        (DONE / FAILED / CANCELLED).
     */
    void waitFor(TaskId id) const;

    /**
     * @brief Block until every submitted task reaches a terminal state.
     */
    void waitAll() const;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    /**
     * @brief Stop the scheduler.
     * @param drain  If true (default), finish all queued work first.
     *               If false, stop immediately and discard pending tasks.
     */
    void shutdown(bool drain = true);

    // ── Stats ─────────────────────────────────────────────────────────────────

    /// Aggregate counts by status.
    using Stats = Dispatcher::Snapshot;
    Stats stats() const;

    /// Number of worker threads.
    size_t workerCount() const noexcept;

private:
    /// Drain the dispatcher's ready queue into the thread pool.
    void tryFeedPool();

    /// Execute one task: run fn, update status, fire callback, reschedule if periodic.
    void executeTask(TaskId id);

    Dispatcher _dispatcher;
    ThreadPool _pool;
};

} // namespace scheduler
