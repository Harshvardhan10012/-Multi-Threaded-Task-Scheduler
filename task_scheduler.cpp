/**
 * @file task_scheduler.cpp
 */
#include "task_scheduler.hpp"
#include <chrono>
#include <thread>

namespace scheduler {

TaskScheduler::TaskScheduler(size_t numWorkers)
    : _pool(numWorkers == 0 ? std::thread::hardware_concurrency() : numWorkers)
{
    _dispatcher.setOnReady([this] { tryFeedPool(); });
}

TaskScheduler::~TaskScheduler() { shutdown(true); }

TaskId TaskScheduler::submit(Task task) {
    TaskId id = _dispatcher.addTask(std::move(task));
    tryFeedPool();
    return id;
}

TaskId TaskScheduler::submit(std::string name, std::function<void()> fn, Priority priority) {
    Task t;
    t.name = std::move(name);
    t.fn   = std::move(fn);
    t.priority = priority;
    return submit(std::move(t));
}

bool TaskScheduler::cancel(TaskId id)       { return _dispatcher.cancel(id); }
TaskStatus TaskScheduler::statusOf(TaskId id) const { return _dispatcher.statusOf(id); }

void TaskScheduler::waitFor(TaskId id) const {
    while (true) {
        auto s = _dispatcher.statusOf(id);
        if (s == TaskStatus::DONE || s == TaskStatus::FAILED || s == TaskStatus::CANCELLED) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void TaskScheduler::waitAll() const {
    while (true) {
        auto s = _dispatcher.snapshot();
        if (s.pending == 0 && s.ready == 0 && s.running == 0) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void TaskScheduler::shutdown(bool drain) {
    _dispatcher.stop(drain);
    _pool.shutdown(drain);
}

TaskScheduler::Stats TaskScheduler::stats() const   { return _dispatcher.snapshot(); }
size_t TaskScheduler::workerCount() const noexcept  { return _pool.size(); }

void TaskScheduler::tryFeedPool() {
    auto& rq = _dispatcher.readyQueue();
    while (true) {
        auto maybeId = rq.pop();
        if (!maybeId) break;
        TaskId id = *maybeId;
        {
            std::unique_lock lock(_dispatcher.taskMutex());
            Task* t = _dispatcher.findTask(id);
            if (!t) continue;
            TaskStatus expected = TaskStatus::READY;
            if (!t->status.compare_exchange_strong(expected, TaskStatus::RUNNING))
                continue;
        }
        _pool.enqueue([this, id] { executeTask(id); });
    }
}

void TaskScheduler::executeTask(TaskId id) {
    std::function<void()>             fn;
    std::function<void(TaskId, bool)> onComplete;
    std::optional<Duration>           period;
    Priority                          priority = Priority::NORMAL;
    std::string                       taskName;

    {
        std::shared_lock lock(_dispatcher.taskMutex());
        const Task* t = _dispatcher.findTask(id);
        if (!t) return;
        fn         = t->fn;
        onComplete = t->onComplete;
        period     = t->period;
        priority   = t->priority;
        taskName   = t->name;
    }

    bool success = false;
    std::string errMsg;
    try { fn(); success = true; }
    catch (const std::exception& e) { errMsg = e.what(); }
    catch (...) { errMsg = "unknown exception"; }

    {
        std::shared_lock lock(_dispatcher.taskMutex());
        Task* t = _dispatcher.findTask(id);
        if (t) {
            t->errorMsg = errMsg;
            t->status.store(success ? TaskStatus::DONE : TaskStatus::FAILED,
                            std::memory_order_release);
        }
    }

    if (onComplete) onComplete(id, success);

    if (success && period.has_value()) {
        Task recurring;
        recurring.name       = taskName;
        recurring.priority   = priority;
        recurring.fn         = fn;
        recurring.onComplete = onComplete;
        recurring.period     = period;
        recurring.notBefore  = Clock::now() + *period;
        _dispatcher.addTask(std::move(recurring));
    }

    _dispatcher.wakeUp();
    tryFeedPool();
}

} // namespace scheduler
