# ⚙ Multi-Threaded Task Scheduler

A production-grade, modular task scheduler built in **C++17** with a real-time **WebSocket** connected **web dashboard**. No external libraries required.

---

## 📋 Table of Contents

- [Overview](#overview)
- [Features](#features)
- [Project Structure](#project-structure)
- [File Descriptions](#file-descriptions)
- [Tech Stack](#tech-stack)
- [How It Works](#how-it-works)
- [Task States](#task-states)
- [Build & Run](#build--run)
- [Usage Example](#usage-example)
- [WebSocket Server](#websocket-server)
- [Dashboard Guide](#dashboard-guide)
- [API Reference](#api-reference)
- [Synchronization Design](#synchronization-design)
- [Unit Tests](#unit-tests)

---

## Overview

This project implements a **multi-threaded task scheduler** from scratch using only the C++17 standard library and POSIX sockets. Tasks are submitted with priorities, dependencies, delays, and optional recurrence. A background dispatcher thread promotes tasks when they are ready, and a fixed thread pool executes them concurrently.

A browser-based **real-time dashboard** connects to the C++ backend over **WebSocket** and visualizes every task moving through its lifecycle — PENDING → READY → RUNNING → DONE — with live worker thread status, a log stream, and throughput chart.

---

## Features

| Feature | Description |
|---|---|
| **Thread Pool** | Fixed-size pool of `std::thread` workers, configurable count |
| **Priority Scheduling** | `HIGH → NORMAL → LOW`; FIFO tie-breaking within same priority |
| **Task Dependencies** | A task waits until all listed dependency IDs reach `DONE` |
| **Delayed Tasks** | `notBefore` timestamp — task won't run before a set time |
| **Periodic Tasks** | Set a `period` — task automatically reschedules after completion |
| **Cancellation** | Cancel any `PENDING` or `READY` task by ID |
| **Exception Safety** | Exceptions caught per-task; status set to `FAILED`, worker unaffected |
| **Completion Callbacks** | `onComplete(TaskId, bool success)` fires after every task |
| **Graceful Shutdown** | Drain mode (finish queued work) or immediate stop |
| **WebSocket Backend** | Real-time push from C++ to browser — zero polling |
| **Web Dashboard** | Interactive control panel with live pipeline view |
| **Zero Dependencies** | Only C++17 STL + POSIX sockets — nothing to install |

---

## Project Structure

```
TaskScheduler/
│
├── types.hpp                ← TaskId, Priority, TaskStatus enums
├── task.hpp                 ← Task descriptor struct (move-only)
├── thread_pool.hpp/cpp      ← Worker thread pool
├── priority_queue.hpp/cpp   ← Thread-safe min-heap
├── dispatcher.hpp/cpp       ← PENDING → READY promotion thread
├── task_scheduler.hpp/cpp   ← Public API facade
├── scheduler.hpp            ← Umbrella header (include this only)
│
├── main.cpp                 ← Demo: all features demonstrated
├── test_scheduler.cpp       ← 23 unit tests
│
├── ws_server.cpp            ← WebSocket server (real-time backend)
├── server.cpp               ← REST HTTP server (alternative backend)
│
├── scheduler_dashboard.html ← Web frontend (open in any browser)
│
├── build.sh                 ← One-command build script
└── README.md                ← This file
```

---

## File Descriptions

### Core Library

| File | Purpose |
|---|---|
| `types.hpp` | Defines `TaskId` (uint64), `Clock`, `Priority` enum (HIGH/NORMAL/LOW), `TaskStatus` enum (PENDING/READY/RUNNING/DONE/FAILED/CANCELLED) |
| `task.hpp` | The `Task` struct — holds `fn`, `onComplete`, `priority`, `notBefore`, `period`, `deps`, `status`. Move-only because of `std::atomic<TaskStatus>` |
| `thread_pool.hpp/cpp` | Fixed-size pool of `std::thread` workers. Workers block on `std::condition_variable`, wake on `enqueue()`, execute work, sleep again |
| `priority_queue.hpp/cpp` | Thread-safe min-heap. Lower priority integer = higher importance. Equal priorities served FIFO via insertion timestamp |
| `dispatcher.hpp/cpp` | Owns the task registry. Background thread scans PENDING tasks every 100ms and promotes them to READY when deps are met and time has passed |
| `task_scheduler.hpp/cpp` | Public facade. Wires `Dispatcher` + `ThreadPool`. Handles execution, callbacks, periodic rescheduling |
| `scheduler.hpp` | Umbrella header — `#include "scheduler.hpp"` gives you the full API |

### Applications

| File | Purpose |
|---|---|
| `main.cpp` | Demonstrates all 6 features: priority ordering, dependencies, delayed tasks, periodic tasks, cancellation, exception handling |
| `test_scheduler.cpp` | 23 unit tests covering every feature. Uses a simple CHECK macro — no external framework needed |
| `ws_server.cpp` | WebSocket server (RFC 6455) built on BSD sockets. Accepts browser connections, receives commands, pushes real-time events back |
| `server.cpp` | Alternative REST HTTP server. Endpoints: `POST /tasks`, `GET /tasks`, `DELETE /tasks/:id`, `GET /stats` |
| `scheduler_dashboard.html` | Full browser dashboard. Auto-connects to WebSocket backend. Falls back to local JS simulation if server is offline |

### Build

| File | Purpose |
|---|---|
| `build.sh` | Shell script that compiles all three targets with one command instead of typing long `g++` commands manually |

---

## Tech Stack

### C++ Backend

| Technology | Usage |
|---|---|
| C++17 | Core language standard |
| `std::thread` | Worker threads and background dispatcher |
| `std::mutex` / `std::shared_mutex` | Task registry protection |
| `std::condition_variable` | Worker sleep/wake without busy-waiting |
| `std::atomic<TaskStatus>` | Lock-free status transitions via CAS |
| `std::priority_queue` | Min-heap for priority task ordering |
| `std::unordered_map` | O(1) task registry lookup |
| `std::chrono` | Delayed and periodic task timing |
| BSD Sockets | Raw TCP for the WebSocket server |
| SHA-1 + Base64 | WebSocket handshake (RFC 6455, hand-rolled) |
| pthread | Underlying thread library (`-pthread` flag) |

### Frontend

| Technology | Usage |
|---|---|
| HTML5 / CSS3 | Dashboard layout, animations, scanline effect |
| Vanilla JavaScript | Scheduler simulation + WebSocket client |
| WebSocket API | Real-time connection to C++ backend |
| Canvas API | Throughput chart (tasks/sec) |
| Google Fonts | Share Tech Mono + Barlow typefaces |

---

## How It Works

```
User calls sched.submit(task)
         │
         ▼
  Dispatcher::addTask()
  ┌─────────────────────────────────┐
  │  Assign ID (_nextId++)          │
  │  Store in task registry         │
  │  If no deps + time OK → READY   │
  │  Else → PENDING                 │
  └─────────────────────────────────┘
         │
         ▼
  Dispatcher background thread (every 100ms)
  ┌─────────────────────────────────┐
  │  Scan all PENDING tasks         │
  │  Check: all deps DONE?          │
  │  Check: Clock::now() >= notBefore│
  │  If both true → READY           │
  │  Push to PriorityTaskQueue      │
  └─────────────────────────────────┘
         │
         ▼
  TaskScheduler::tryFeedPool()
  ┌─────────────────────────────────┐
  │  Pop from PriorityTaskQueue     │
  │  CAS: READY → RUNNING           │
  │  ThreadPool::enqueue(task)      │
  └─────────────────────────────────┘
         │
         ▼
  Worker Thread (executeTask)
  ┌─────────────────────────────────┐
  │  Call task.fn()                 │
  │  Catch any exceptions           │
  │  Set status DONE or FAILED      │
  │  Fire onComplete callback       │
  │  Reschedule if periodic         │
  │  Wake dispatcher for next batch │
  └─────────────────────────────────┘
```

---

## Task States

```
                 ┌─────────┐
   submit()  --> │ PENDING │ <--- deps not met / time not reached
                 └────┬────┘
                      │  deps met + time reached
                      ▼
                 ┌─────────┐
                 │  READY  │ <--- sitting in PriorityTaskQueue
                 └────┬────┘
                      │  worker picks up
                      ▼
                 ┌─────────┐
                 │ RUNNING │ <--- task.fn() executing
                 └────┬────┘
              ┌───────┴────────┐
              ▼                ▼
         ┌────────┐       ┌────────┐
         │  DONE  │       │ FAILED │
         └────────┘       └────────┘

  cancel() on PENDING or READY --> CANCELLED
```

| State | Color | Meaning |
|---|---|---|
| PENDING | Gray | Waiting for dependencies or scheduled time |
| READY | Amber | In priority queue, waiting for free worker |
| RUNNING | Blue | Worker thread executing `task.fn()` |
| DONE | Green | Completed successfully |
| FAILED | Red | `task.fn()` threw an exception |
| CANCELLED | Gray | Cancelled before execution started |

---

## Build & Run

### Option 1 — Build Script (Recommended)

```bash
bash build.sh
```

Output:
```
=== TaskScheduler Build ===
[1/3] Building demo...      → scheduler_demo
[2/3] Building tests...     → run_tests
[3/3] Building WebSocket server... → ws_server

✓ Done.
```

### Option 2 — Manual

```bash
# Demo
g++ -std=c++17 -O2 -pthread \
    dispatcher.cpp priority_queue.cpp thread_pool.cpp task_scheduler.cpp \
    main.cpp -I. -o scheduler_demo

# Tests
g++ -std=c++17 -O2 -pthread \
    dispatcher.cpp priority_queue.cpp thread_pool.cpp task_scheduler.cpp \
    test_scheduler.cpp -I. -o run_tests

# WebSocket server
g++ -std=c++17 -O2 -pthread \
    dispatcher.cpp priority_queue.cpp thread_pool.cpp task_scheduler.cpp \
    ws_server.cpp -I. -o ws_server
```

### Run

```bash
./scheduler_demo                  # feature demonstration
./run_tests                       # 23 unit tests
./ws_server                       # start backend
# then open scheduler_dashboard.html in browser
```

---

## Usage Example

```cpp
#include "scheduler.hpp"
using namespace scheduler;

int main() {
    TaskScheduler sched(4);   // 4 worker threads

    // Simple task
    sched.submit("hello", [] {
        std::cout << "Hello from worker!\n";
    }, Priority::HIGH);

    // Task with dependency
    Task a;
    a.name     = "fetch";
    a.priority = Priority::HIGH;
    a.fn       = [] { /* fetch data */ };
    TaskId idA = sched.submit(std::move(a));

    Task b;
    b.name = "process";
    b.deps = { idA };          // waits until fetch is DONE
    b.fn   = [] { /* process */ };
    sched.submit(std::move(b));

    // Delayed task (runs after 5 seconds)
    Task c;
    c.name      = "cleanup";
    c.notBefore = Clock::now() + std::chrono::seconds(5);
    c.fn        = [] { /* cleanup */ };
    sched.submit(std::move(c));

    // Periodic task (repeats every 1 second)
    Task d;
    d.name   = "heartbeat";
    d.period = std::chrono::seconds(1);
    d.fn     = [] { std::cout << "ping\n"; };
    sched.submit(std::move(d));

    sched.waitAll();
    sched.shutdown();
}
```

---

## WebSocket Server

`ws_server.cpp` implements RFC 6455 WebSocket from scratch using BSD sockets.

### Start
```bash
./ws_server
# ws://localhost:9001
```

### Events pushed C++ → Browser

| Event | Fired when |
|---|---|
| `task_submitted` | Task registered |
| `task_status` | Status changes to RUNNING / DONE / FAILED / CANCELLED |
| `worker_update` | Worker picks up or finishes a task |
| `stats` | Every 500ms + after any change |
| `log` | Every scheduler event |
| `reset` | Scheduler reset |

### Commands Browser → C++

| Command | Fields | Action |
|---|---|---|
| `submit` | `name, priority, delay, period, deps, fail, duration` | Submit new task |
| `cancel` | `id` | Cancel task by ID |
| `reset` | — | Clear all tasks, restart scheduler |

---

## Dashboard Guide

Open `scheduler_dashboard.html` in any browser. The **⚡ WS LIVE** badge in the header confirms connection.

### Left Panel
- **Submit Task form** — maps directly to the `Task` struct fields
- **Worker Threads** — 4 cards showing busy/idle state with progress bars
- **Demo Scenarios** — one-click pre-built examples

### Center Panel
- **Pipeline columns** — PENDING / READY / RUNNING / DONE / FAILED
- **Task table** — ID, name, priority badge, status badge, progress bar, deps, cancel button

### Right Panel
- **Live log** — color-coded events pushed from C++ instantly
- **Throughput chart** — tasks completed per second over last 30 seconds

### See all 4 states clearly
```
1. Submit task-A  | Duration: 5000ms | No deps
2. Submit task-B  | Duration: 5000ms | Depends on: 1
3. Submit task-C  | Duration: 5000ms | Depends on: 2

Watch:
task-A: PENDING → READY → RUNNING (5s) → DONE
task-B: PENDING (waiting for A) → READY → RUNNING → DONE
task-C: PENDING (waiting for B) → READY → RUNNING → DONE
```

> **Important:** In `ws_server.cpp` change `sleep_for(80ms)` to
> `sleep_for(std::chrono::milliseconds(duration))` so the Duration
> field from the form is used by the real C++ worker thread.

---

## API Reference

```cpp
TaskScheduler sched(4);               // 4 worker threads

TaskId id = sched.submit(task);       // submit full Task object
TaskId id = sched.submit("name", fn, Priority::HIGH);  // quick submit

bool ok = sched.cancel(id);           // cancel PENDING or READY task

TaskStatus s = sched.statusOf(id);    // query current status
sched.waitFor(id);                    // block until task terminal
sched.waitAll();                      // block until all tasks done

sched.shutdown(true);                 // graceful shutdown (drain=true)

auto stats = sched.stats();
// stats.total / pending / ready / running / done / failed / cancelled
size_t n = sched.workerCount();
```

---

## Synchronization Design

Three layers of thread safety prevent all race conditions:

| Layer | Mechanism | Protects |
|---|---|---|
| Task registry | `std::shared_mutex` | Multiple readers, one writer |
| Ready queue | `std::mutex` | Push/pop operations |
| Status transitions | `std::atomic` + CAS | READY → RUNNING (only one winner) |

The critical CAS prevents two workers grabbing the same task:
```cpp
TaskStatus expected = TaskStatus::READY;
task.status.compare_exchange_strong(expected, TaskStatus::RUNNING);
// Only ONE thread succeeds — others see expected != READY and skip
```

---

## Unit Tests

```bash
./run_tests
```

```
══════════════════════════════════════════
  Task Scheduler – Unit Tests
══════════════════════════════════════════
[TEST] simple task executes
[TEST] high-priority task runs before low
[TEST] dependency chain A → B → C
[TEST] delayed task runs after notBefore
[TEST] periodic task fires multiple times
[TEST] cancel PENDING task
[TEST] failed task sets status FAILED
[TEST] ThreadPool::pendingCount reflects queue depth
[TEST] PriorityTaskQueue dequeues in priority order
══════════════════════════════════════════
  Passed: 23  Failed: 0
══════════════════════════════════════════
```

---

## Author

**Harsh** — Multi-Threaded Task Scheduler, C++17
