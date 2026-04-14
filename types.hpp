#pragma once

/**
 * @file types.hpp
 * @brief Core type aliases and enumerations shared across the scheduler.
 */

#include <chrono>
#include <cstdint>

namespace scheduler {

// ── Type aliases ──────────────────────────────────────────────────────────────

using TaskId    = uint64_t;
using Clock     = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Duration  = Clock::duration;

// ── Priority ──────────────────────────────────────────────────────────────────

enum class Priority : int {
    HIGH   = 0,
    NORMAL = 1,
    LOW    = 2
};

// ── Task Status ───────────────────────────────────────────────────────────────

enum class TaskStatus {
    PENDING,    ///< Waiting for dependencies or scheduled time
    READY,      ///< In the ready queue, waiting for a free worker
    RUNNING,    ///< Currently executing on a worker thread
    DONE,       ///< Completed successfully
    FAILED,     ///< Threw an exception during execution
    CANCELLED   ///< Explicitly cancelled before execution
};

/// Human-readable name for a TaskStatus value.
inline const char* toString(TaskStatus s) noexcept {
    switch (s) {
        case TaskStatus::PENDING:   return "PENDING";
        case TaskStatus::READY:     return "READY";
        case TaskStatus::RUNNING:   return "RUNNING";
        case TaskStatus::DONE:      return "DONE";
        case TaskStatus::FAILED:    return "FAILED";
        case TaskStatus::CANCELLED: return "CANCELLED";
    }
    return "UNKNOWN";
}

/// Human-readable name for a Priority value.
inline const char* toString(Priority p) noexcept {
    switch (p) {
        case Priority::HIGH:   return "HIGH";
        case Priority::NORMAL: return "NORMAL";
        case Priority::LOW:    return "LOW";
    }
    return "UNKNOWN";
}

} // namespace scheduler
