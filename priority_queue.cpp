/**
 * @file priority_queue.cpp
 * @brief PriorityTaskQueue implementation.
 */

#include "priority_queue.hpp"

namespace scheduler {

void PriorityTaskQueue::push(TaskId id, Priority priority) {
    std::unique_lock lock(_mu);
    _heap.push({ static_cast<int>(priority), Clock::now(), id });
}

std::optional<TaskId> PriorityTaskQueue::pop() {
    std::unique_lock lock(_mu);
    if (_heap.empty()) return std::nullopt;
    TaskId id = _heap.top().id;
    _heap.pop();
    return id;
}

bool PriorityTaskQueue::empty() const {
    std::unique_lock lock(_mu);
    return _heap.empty();
}

size_t PriorityTaskQueue::size() const {
    std::unique_lock lock(_mu);
    return _heap.size();
}

} 
