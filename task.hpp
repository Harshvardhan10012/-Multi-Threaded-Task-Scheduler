#pragma once

/**
 * @file task.hpp
 * @brief Task descriptor – the unit of work submitted to the scheduler.
 */

#include "types.hpp"

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace scheduler {

/**
 * @brief Describes a single unit of work.
 *
 * Fill in the desired fields and call TaskScheduler::submit().
 * After submission the scheduler owns the Task; query its status
 * via TaskScheduler::statusOf(id).
 *
 * Tasks are move-only (the embedded std::atomic<TaskStatus> prevents copying).
 */
struct Task {
    // ── Identity ─────────────────────────────────────────────────────────────
    TaskId      id   = 0;           ///< Assigned by the scheduler on submit
    std::string name;               ///< Optional human-readable label

    // ── Work ─────────────────────────────────────────────────────────────────
    std::function<void()>             fn;           ///< The work to execute
    std::function<void(TaskId, bool)> onComplete;   ///< Called after fn; bool = success

    // ── Scheduling ───────────────────────────────────────────────────────────
    Priority    priority  = Priority::NORMAL;
    TimePoint   notBefore = Clock::now();   ///< Earliest start time
    std::optional<Duration> period;         ///< If set, task repeats with this interval

    // ── Dependencies ─────────────────────────────────────────────────────────
    std::vector<TaskId> deps;   ///< IDs that must reach DONE before this task runs

    // ── Runtime state (managed by the scheduler) ─────────────────────────────
    std::atomic<TaskStatus> status { TaskStatus::PENDING };
    std::string             errorMsg;   ///< Set when status == FAILED

    // ── Lifecycle ────────────────────────────────────────────────────────────
    Task() = default;
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    /// Manual move constructor: transfers all fields; resets status in source.
    Task(Task&& o) noexcept
        : id(o.id), name(std::move(o.name))
        , fn(std::move(o.fn)), onComplete(std::move(o.onComplete))
        , priority(o.priority)
        , notBefore(o.notBefore), period(o.period)
        , deps(std::move(o.deps))
        , status(o.status.load(std::memory_order_relaxed))
        , errorMsg(std::move(o.errorMsg))
    {}

    Task& operator=(Task&&) = delete;
};

} // namespace scheduler
