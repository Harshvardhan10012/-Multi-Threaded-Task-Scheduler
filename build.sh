#!/bin/bash
# ─────────────────────────────────────────────────────────
#  TaskScheduler — Build Script
#  Run from the folder containing all .cpp / .hpp files
# ─────────────────────────────────────────────────────────

set -e
CXX="g++"
FLAGS="-std=c++17 -O2 -pthread"
CORE="dispatcher.cpp priority_queue.cpp thread_pool.cpp task_scheduler.cpp"

echo "=== TaskScheduler Build ==="

echo "[1/3] Building demo..."
$CXX $FLAGS $CORE main.cpp -I. -o scheduler_demo
echo "      → scheduler_demo"

echo "[2/3] Building tests..."
$CXX $FLAGS $CORE test_scheduler.cpp -I. -o run_tests
echo "      → run_tests"

echo "[3/3] Building WebSocket server..."
$CXX $FLAGS $CORE ws_server.cpp -I. -o ws_server
echo "      → ws_server"

echo ""
echo "✓ Done. Run:"
echo "   ./scheduler_demo          # demo"
echo "   ./run_tests               # unit tests"
echo "   ./ws_server               # backend for dashboard"
echo "   open scheduler_dashboard.html  # frontend"
