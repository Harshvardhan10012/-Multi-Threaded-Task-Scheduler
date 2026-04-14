#pragma once

/**
 * @file priority_queue.hpp
 * @brief Thread-safe priority queue for ready task IDs.
 *
 * Tasks with higher priority (lower enum value) dequeue first.
 * Equal-priority tasks follow FIFO order via a submission timestamp.
 */

#include "types.hpp"

#include <chrono>
#include <mutex>
#include <optional>
#include <queue>
#include <vector>

namespace scheduler {

/**
 * @brief A thread-safe, min-heap priority queue for TaskIds.
 *
 * Each entry records the task's Priority and its insertion time so
 * that equal-priority tasks are served in FIFO order.
 */
class PriorityTaskQueue {
public:
    /// Push a task ID onto the queue.
    void push(TaskId id, Priority priority);

    /**
     * @brief Pop the highest-priority task ID.
     * @return The task ID, or std::nullopt if the queue is empty.
     */
    std::optional<TaskId> pop();

    /// Returns true when the queue contains no items.
    bool empty() const;

    /// Number of items currently in the queue.
    size_t size() const;

private:
    struct Entry {
        int       prio;     ///< Lower = higher priority
        TimePoint inserted; ///< Tie-break by arrival time (FIFO)
        TaskId    id;

        bool operator>(const Entry& o) const noexcept {
            if (prio != o.prio) return prio > o.prio;
            return inserted > o.inserted;
        }
    };

    using Heap = std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>>;

    mutable std::mutex _mu;
    Heap               _heap;
};

} // namespace scheduler
