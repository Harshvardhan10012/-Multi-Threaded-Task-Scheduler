/**
 * @file dispatcher.cpp
 * @brief Dispatcher implementation – background thread promoting PENDING → READY.
 */

#include "dispatcher.hpp"

#include <chrono>

namespace scheduler {

using namespace std::chrono_literals;

// ── Constructor / Destructor ──────────────────────────────────────────────────

Dispatcher::Dispatcher() {
    _thread = std::thread(&Dispatcher::dispatchLoop, this);
}

Dispatcher::~Dispatcher() {
    stop(/*drain=*/false);
}

// ── Task registration ─────────────────────────────────────────────────────────

TaskId Dispatcher::addTask(Task task) {
    std::unique_lock lock(_tasksMu);

    task.id = _nextId.fetch_add(1, std::memory_order_relaxed);
    TaskId id = task.id;
    task.status.store(TaskStatus::PENDING);

    // Fast-path: no deps, time already reached → go straight to READY
    if (task.deps.empty() && Clock::now() >= task.notBefore) {
        task.status.store(TaskStatus::READY);
        _readyQueue.push(id, task.priority);
    }

    _tasks.emplace(id, std::move(task));

    wakeUp();
    return id;
}

// ── Cancellation ──────────────────────────────────────────────────────────────

bool Dispatcher::cancel(TaskId id) {
    std::unique_lock lock(_tasksMu);
    auto it = _tasks.find(id);
    if (it == _tasks.end()) return false;

    auto& t = it->second;
    TaskStatus expected = TaskStatus::PENDING;
    if (t.status.compare_exchange_strong(expected, TaskStatus::CANCELLED))
        return true;

    expected = TaskStatus::READY;
    return t.status.compare_exchange_strong(expected, TaskStatus::CANCELLED);
}

// ── Queries ───────────────────────────────────────────────────────────────────

TaskStatus Dispatcher::statusOf(TaskId id) const {
    std::shared_lock lock(_tasksMu);
    auto it = _tasks.find(id);
    return it == _tasks.end() ? TaskStatus::CANCELLED : it->second.status.load();
}

Task* Dispatcher::findTask(TaskId id) {
    auto it = _tasks.find(id);
    return it == _tasks.end() ? nullptr : &it->second;
}

const Task* Dispatcher::findTask(TaskId id) const {
    auto it = _tasks.find(id);
    return it == _tasks.end() ? nullptr : &it->second;
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

void Dispatcher::wakeUp() {
    {
        std::unique_lock lock(_cvMu);
        _wake = true;
    }
    _cv.notify_one();
}

void Dispatcher::stop(bool drain) {
    {
        std::unique_lock lock(_cvMu);
        if (_stop) return;
        _drain = drain;
        _stop  = true;
        _wake  = true;
    }
    _cv.notify_all();
    if (_thread.joinable()) _thread.join();
}

// ── Stats ─────────────────────────────────────────────────────────────────────

Dispatcher::Snapshot Dispatcher::snapshot() const {
    std::shared_lock lock(_tasksMu);
    Snapshot s{};
    s.total = _tasks.size();
    for (auto& [id, t] : _tasks) {
        switch (t.status.load()) {
            case TaskStatus::PENDING:   ++s.pending;   break;
            case TaskStatus::READY:     ++s.ready;     break;
            case TaskStatus::RUNNING:   ++s.running;   break;
            case TaskStatus::DONE:      ++s.done;      break;
            case TaskStatus::FAILED:    ++s.failed;    break;
            case TaskStatus::CANCELLED: ++s.cancelled; break;
        }
    }
    return s;
}

// ── Private ───────────────────────────────────────────────────────────────────

void Dispatcher::dispatchLoop() {
    while (true) {
        // Sleep until woken or 100 ms timeout (for delayed tasks)
        {
            std::unique_lock lock(_cvMu);
            _cv.wait_for(lock, 100ms, [this] { return _wake; });
            _wake = false;
            if (_stop && !_drain) return;
        }

        // Promote eligible PENDING tasks
        {
            std::unique_lock lock(_tasksMu);
            promoteReady(lock);
        }

        if (_onReady) _onReady();

        if (_stop) return;
    }
}

void Dispatcher::promoteReady(std::unique_lock<std::shared_mutex>& /*lock*/) {
    const auto now = Clock::now();
    for (auto& [id, t] : _tasks) {
        if (t.status.load() != TaskStatus::PENDING) continue;
        if (now < t.notBefore) continue;

        // Check all dependencies have DONE status
        bool depsOk = true;
        for (TaskId dep : t.deps) {
            auto it = _tasks.find(dep);
            if (it == _tasks.end()) continue;   // unknown dep → treat as done
            if (it->second.status.load() != TaskStatus::DONE) {
                depsOk = false;
                break;
            }
        }
        if (!depsOk) continue;

        t.status.store(TaskStatus::READY);
        _readyQueue.push(id, t.priority);
    }
}

} // namespace scheduler
