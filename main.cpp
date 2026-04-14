/**
 * @file main.cpp
 * @brief Demonstrates all scheduler features.
 *
 * Build via CMake (see CMakeLists.txt) or manually:
 *   g++ -std=c++17 -O2 -pthread \
 *       src/thread_pool.cpp src/priority_queue.cpp \
 *       src/dispatcher.cpp src/task_scheduler.cpp \
 *       examples/main.cpp \
 *       -Iinclude -o scheduler_demo
 */

#include "scheduler.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

using namespace scheduler;
using namespace std::chrono_literals;

// ── Thread-safe logger ────────────────────────────────────────────────────────

namespace {
std::mutex g_printMu;

void log(const std::string& msg) {
    std::lock_guard lock(g_printMu);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  Clock::now().time_since_epoch())
                  .count() % 1'000'000;
    std::cout << "[" << ms << "ms] " << msg << "\n";
}
} // namespace

// ── Test helpers ──────────────────────────────────────────────────────────────

static void printStats(const TaskScheduler& s) {
    auto st = s.stats();
    std::cout << "  Stats → total:" << st.total
              << "  done:" << st.done
              << "  failed:" << st.failed
              << "  cancelled:" << st.cancelled << "\n\n";
}

// ═════════════════════════════════════════════════════════════════════════════
//  main
// ═════════════════════════════════════════════════════════════════════════════

int main() {
    std::cout << "╔══════════════════════════════════════════╗\n"
              << "║  Multi-Threaded Task Scheduler  (C++17)  ║\n"
              << "╚══════════════════════════════════════════╝\n\n";

    constexpr size_t NUM_WORKERS = 4;
    TaskScheduler sched(NUM_WORKERS);
    std::cout << "Scheduler started with " << NUM_WORKERS << " worker threads.\n\n";

    // ── Test 1: Priority ordering ─────────────────────────────────────────────
    std::cout << "┌─ Test 1: Priority ordering ─────────────────────────────\n";
    {
        const char* labels[] = { "HIGH", "NORMAL", "LOW" };
        for (int i = 0; i < 6; ++i) {
            Priority p = static_cast<Priority>(i % 3);
            Task t;
            t.name     = std::string("pri-") + labels[i % 3] + "-" + std::to_string(i);
            t.priority = p;
            t.fn = [name = t.name, p] {
                std::this_thread::sleep_for(50ms);
                log("  Ran " + name + " [" + toString(p) + "]");
            };
            sched.submit(std::move(t));
        }
        sched.waitAll();
        printStats(sched);
    }

    // ── Test 2: Task dependencies A → B → C ───────────────────────────────────
    std::cout << "┌─ Test 2: Dependencies  A → B → C ───────────────────────\n";
    {
        Task a;
        a.name = "A";
        a.fn   = [] { std::this_thread::sleep_for(120ms); log("  Task A done"); };
        TaskId idA = sched.submit(std::move(a));

        Task b;
        b.name = "B";
        b.deps = { idA };
        b.fn   = [] { std::this_thread::sleep_for(80ms);  log("  Task B done (after A)"); };
        TaskId idB = sched.submit(std::move(b));

        Task c;
        c.name = "C";
        c.deps = { idB };
        c.fn   = [] { log("  Task C done (after B)"); };
        sched.submit(std::move(c));

        sched.waitAll();
        printStats(sched);
    }

    // ── Test 3: Delayed (scheduled) task ──────────────────────────────────────
    std::cout << "┌─ Test 3: Delayed task (300 ms) ─────────────────────────\n";
    {
        Task t;
        t.name      = "delayed";
        t.notBefore = Clock::now() + 300ms;
        t.fn        = [] { log("  Delayed task executed after 300 ms"); };
        sched.submit(std::move(t));
        sched.waitAll();
        printStats(sched);
    }

    // ── Test 4: Periodic task ─────────────────────────────────────────────────
    std::cout << "┌─ Test 4: Periodic task  (200 ms × 3) ───────────────────\n";
    {
        std::atomic<int> counter { 0 };

        Task t;
        t.name   = "heartbeat";
        t.period = 200ms;
        t.fn     = [&] { log("  Heartbeat #" + std::to_string(++counter)); };
        t.onComplete = [&](TaskId id, bool) {
            if (counter >= 3) sched.cancel(id);
        };
        sched.submit(std::move(t));

        while (counter < 3)
            std::this_thread::sleep_for(50ms);

        std::cout << "\n";
    }

    // ── Test 5: Cancellation ──────────────────────────────────────────────────
    std::cout << "┌─ Test 5: Cancellation ──────────────────────────────────\n";
    {
        Task t;
        t.name      = "soon-cancelled";
        t.notBefore = Clock::now() + 500ms;   // give us time to cancel
        t.fn        = [] { log("  This should NOT print"); };
        TaskId id   = sched.submit(std::move(t));

        bool ok = sched.cancel(id);
        log(std::string("  cancel() returned ") + (ok ? "true ✓" : "false ✗"));
        std::cout << "  Final status: " << toString(sched.statusOf(id)) << "\n\n";
    }

    // ── Test 6: Exception handling ────────────────────────────────────────────
    std::cout << "┌─ Test 6: Exception in task ─────────────────────────────\n";
    {
        Task t;
        t.name = "boom";
        t.fn   = [] { throw std::runtime_error("intentional error"); };
        t.onComplete = [](TaskId id, bool ok) {
            log("  Task " + std::to_string(id) +
                " onComplete  success=" + (ok ? "true" : "false ✓"));
        };
        TaskId id = sched.submit(std::move(t));
        sched.waitFor(id);
        std::cout << "  Final status: " << toString(sched.statusOf(id)) << "\n\n";
    }

    // ── Final stats ───────────────────────────────────────────────────────────
    std::cout << "┌─ Final Stats ────────────────────────────────────────────\n";
    printStats(sched);

    std::cout << "Shutting down gracefully…\n";
    sched.shutdown(/*drain=*/true);
    std::cout << "Done.\n";
}
