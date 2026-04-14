/**
 * @file test_scheduler.cpp
 * @brief Lightweight unit tests (no external framework required).
 *
 * Build:
 *   g++ -std=c++17 -O2 -pthread \
 *       ../src/thread_pool.cpp ../src/priority_queue.cpp \
 *       ../src/dispatcher.cpp ../src/task_scheduler.cpp \
 *       test_scheduler.cpp \
 *       -I../include -o run_tests && ./run_tests
 */

#include "scheduler.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

using namespace scheduler;
using namespace std::chrono_literals;

// ── Minimal test harness ──────────────────────────────────────────────────────

static int g_passed = 0, g_failed = 0;

#define CHECK(expr)                                                      \
    do {                                                                 \
        if (!(expr)) {                                                   \
            std::cerr << "  FAIL  " #expr "  (" __FILE__ ":"            \
                      << __LINE__ << ")\n";                              \
            ++g_failed;                                                  \
        } else {                                                         \
            ++g_passed;                                                  \
        }                                                                \
    } while (false)

static void section(const char* name) {
    std::cout << "\n[TEST] " << name << "\n";
}

// ── Tests ─────────────────────────────────────────────────────────────────────

void test_simple_task() {
    section("simple task executes");
    TaskScheduler s(2);
    std::atomic<bool> ran { false };
    auto id = s.submit("t1", [&] { ran = true; });
    s.waitFor(id);
    CHECK(ran.load());
    CHECK(s.statusOf(id) == TaskStatus::DONE);
}

void test_priority_order() {
    section("high-priority task runs before low");
    // Use PriorityTaskQueue directly to test ordering without race conditions
    PriorityTaskQueue q;
    q.push(10, Priority::LOW);
    q.push(20, Priority::NORMAL);
    q.push(30, Priority::HIGH);
    q.push(40, Priority::HIGH);
    q.push(50, Priority::LOW);

    CHECK(q.pop() == 30u);  // HIGH, submitted first among HIGH
    CHECK(q.pop() == 40u);  // HIGH, submitted second
    CHECK(q.pop() == 20u);  // NORMAL
    CHECK(q.pop() == 10u);  // LOW, submitted first among LOW
    CHECK(q.pop() == 50u);  // LOW
    CHECK(!q.pop().has_value());
}

void test_dependencies() {
    section("dependency chain A → B → C");
    TaskScheduler s(4);

    std::atomic<int> seq { 0 };
    int tA = -1, tB = -1, tC = -1;

    Task a;
    a.name = "A";
    a.fn   = [&] { tA = seq++; std::this_thread::sleep_for(30ms); };
    TaskId idA = s.submit(std::move(a));

    Task b;
    b.name = "B";
    b.deps = { idA };
    b.fn   = [&] { tB = seq++; };
    TaskId idB = s.submit(std::move(b));

    Task c;
    c.name = "C";
    c.deps = { idB };
    c.fn   = [&] { tC = seq++; };
    TaskId idC = s.submit(std::move(c));

    s.waitFor(idC);

    CHECK(tA == 0);
    CHECK(tB == 1);
    CHECK(tC == 2);
}

void test_delayed_task() {
    section("delayed task runs after notBefore");
    TaskScheduler s(2);

    auto start = Clock::now();
    Task t;
    t.name      = "delayed";
    t.notBefore = start + 200ms;
    t.fn        = [] {};
    auto id = s.submit(std::move(t));
    s.waitFor(id);

    auto elapsed = Clock::now() - start;
    CHECK(elapsed >= 200ms);
}

void test_periodic_task() {
    section("periodic task fires multiple times");
    TaskScheduler s(2);

    std::atomic<int> count { 0 };
    std::atomic<TaskId> latestId { 0 };

    Task t;
    t.name   = "periodic";
    t.period = 100ms;
    t.fn     = [&] { ++count; };
    t.onComplete = [&](TaskId id, bool) {
        latestId = id;
        if (count >= 3) s.cancel(id);
    };
    s.submit(std::move(t));

    // Wait up to 2 seconds
    for (int i = 0; i < 200 && count < 3; ++i)
        std::this_thread::sleep_for(10ms);

    CHECK(count >= 3);
}

void test_cancellation() {
    section("cancel PENDING task");
    TaskScheduler s(2);

    Task t;
    t.name      = "cancel-me";
    t.notBefore = Clock::now() + 10s;   // far future → stays PENDING
    t.fn        = [] { assert(false && "should not execute"); };
    auto id = s.submit(std::move(t));

    bool ok = s.cancel(id);
    CHECK(ok);
    CHECK(s.statusOf(id) == TaskStatus::CANCELLED);
}

void test_exception_handling() {
    section("failed task sets status FAILED");
    TaskScheduler s(2);

    std::atomic<bool> cbCalled { false };
    std::atomic<bool> cbSuccess { true };

    Task t;
    t.name = "thrower";
    t.fn   = [] { throw std::runtime_error("oops"); };
    t.onComplete = [&](TaskId, bool ok) {
        cbCalled = true;
        cbSuccess = ok;
    };
    auto id = s.submit(std::move(t));
    s.waitFor(id);

    CHECK(s.statusOf(id) == TaskStatus::FAILED);
    CHECK(cbCalled.load());
    CHECK(!cbSuccess.load());
}

void test_thread_pool_pending_count() {
    section("ThreadPool::pendingCount reflects queue depth");
    ThreadPool pool(1);

    std::atomic<bool> gate { false };
    // Block the single worker
    pool.enqueue([&] { while (!gate.load()) std::this_thread::sleep_for(5ms); });

    std::this_thread::sleep_for(20ms);  // let worker pick up the blocker

    pool.enqueue([] {});
    pool.enqueue([] {});

    CHECK(pool.pendingCount() == 2);

    gate = true;
    pool.shutdown(/*drain=*/true);
}

void test_priority_queue() {
    section("PriorityTaskQueue dequeues in priority order");
    PriorityTaskQueue q;
    q.push(1, Priority::LOW);
    q.push(2, Priority::HIGH);
    q.push(3, Priority::NORMAL);

    CHECK(q.pop() == 2u);   // HIGH
    CHECK(q.pop() == 3u);   // NORMAL
    CHECK(q.pop() == 1u);   // LOW
    CHECK(!q.pop().has_value());
}

// ── Entry point ───────────────────────────────────────────────────────────────

int main() {
    std::cout << "══════════════════════════════════════════\n";
    std::cout << "  Task Scheduler – Unit Tests\n";
    std::cout << "══════════════════════════════════════════\n";

    test_simple_task();
    test_priority_order();
    test_dependencies();
    test_delayed_task();
    test_periodic_task();
    test_cancellation();
    test_exception_handling();
    test_thread_pool_pending_count();
    test_priority_queue();

    std::cout << "\n══════════════════════════════════════════\n";
    std::cout << "  Passed: " << g_passed
              << "  Failed: " << g_failed << "\n";
    std::cout << "══════════════════════════════════════════\n";

    return g_failed > 0 ? 1 : 0;
}
