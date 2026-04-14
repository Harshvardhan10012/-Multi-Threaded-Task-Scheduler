/**
 * server.cpp  —  HTTP bridge between TaskScheduler and the web frontend
 *
 * Endpoints:
 *   POST /tasks          submit a new task
 *   GET  /tasks          list all tasks with status
 *   DELETE /tasks/:id    cancel a task
 *   GET  /stats          aggregate counts
 *   GET  /workers        worker thread states
 *
 * Build:
 *   g++ -std=c++17 -O2 -pthread \
 *       dispatcher.cpp priority_queue.cpp thread_pool.cpp task_scheduler.cpp \
 *       server.cpp -I. -o server
 *   ./server
 *
 * Then open scheduler_dashboard.html in your browser.
 * The frontend will auto-connect to http://localhost:8080
 */

#include "scheduler.hpp"
#include "httplib.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace scheduler;
using namespace std::chrono_literals;


static std::string jsonStr(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out + "\"";
}

static std::string taskToJson(TaskId id, const std::string& name,
                               const std::string& priority,
                               const std::string& status,
                               const std::vector<TaskId>& deps,
                               const std::string& error = "") {
    std::ostringstream o;
    o << "{";
    o << "\"id\":" << id << ",";
    o << "\"name\":" << jsonStr(name) << ",";
    o << "\"priority\":" << jsonStr(priority) << ",";
    o << "\"status\":" << jsonStr(status) << ",";
    o << "\"deps\":[";
    for (size_t i = 0; i < deps.size(); ++i) {
        if (i) o << ",";
        o << deps[i];
    }
    o << "]";
    if (!error.empty()) o << ",\"error\":" << jsonStr(error);
    o << "}";
    return o.str();
}

// ─── Task registry (mirrors scheduler state for the API) ─────────────────────

struct TaskRecord {
    TaskId              id;
    std::string         name;
    std::string         priority;
    std::vector<TaskId> deps;
    std::string         errorMsg;
};

static std::mutex            g_mu;
static std::vector<TaskRecord> g_records;   // append-only log

// ─── Main ─────────────────────────────────────────────────────────────────────

int main() {
    constexpr int PORT = 8080;
    TaskScheduler sched(4);

    httplib::Server svr;

    // ── CORS middleware (allow browser requests) ──────────────────────────────
    auto addCors = [](httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin",  "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
    };

    svr.Options(".*", [&](const httplib::Request&, httplib::Response& res) {
        addCors(res);
        res.status = 204;
    });

    // ── POST /tasks  →  submit a task ─────────────────────────────────────────
    //
    // Body (JSON):
    // {
    //   "name":     "my-task",        // optional
    //   "priority": "HIGH",           // HIGH | NORMAL | LOW
    //   "duration": 800,              // simulated work ms (ignored by real fn)
    //   "delay":    0,                // ms before eligible to run
    //   "period":   0,                // ms between repeats (0 = run once)
    //   "deps":     [1, 2],           // task IDs that must finish first
    //   "fail":     false             // force FAILED status for testing
    // }
    svr.Post("/tasks", [&](const httplib::Request& req, httplib::Response& res) {
        addCors(res);

        // --- parse JSON manually (tiny subset) ---
        auto field = [&](const std::string& key, const std::string& def) -> std::string {
            auto pos = req.body.find("\"" + key + "\"");
            if (pos == std::string::npos) return def;
            pos = req.body.find(":", pos);
            if (pos == std::string::npos) return def;
            ++pos;
            while (pos < req.body.size() && req.body[pos] == ' ') ++pos;
            if (req.body[pos] == '"') {
                ++pos;
                auto end = req.body.find('"', pos);
                return req.body.substr(pos, end - pos);
            }
            auto end = req.body.find_first_of(",}\n", pos);
            return req.body.substr(pos, end - pos);
        };

        auto fieldInt = [&](const std::string& key, int def) -> int {
            auto v = field(key, "");
            return v.empty() ? def : std::stoi(v);
        };

        auto fieldBool = [&](const std::string& key) -> bool {
            return field(key, "false") == "true";
        };

        // parse deps array
        std::vector<TaskId> deps;
        {
            auto pos = req.body.find("\"deps\"");
            if (pos != std::string::npos) {
                auto lbr = req.body.find('[', pos);
                auto rbr = req.body.find(']', pos);
                if (lbr != std::string::npos && rbr != std::string::npos) {
                    std::string arr = req.body.substr(lbr+1, rbr-lbr-1);
                    std::istringstream ss(arr);
                    std::string tok;
                    while (std::getline(ss, tok, ',')) {
                        tok.erase(0, tok.find_first_not_of(" \t"));
                        if (!tok.empty()) deps.push_back(std::stoull(tok));
                    }
                }
            }
        }

        std::string name     = field("name", "");
        std::string priority = field("priority", "NORMAL");
        int         delay    = fieldInt("delay",    0);
        int         period   = fieldInt("period",   0);
        bool        fail     = fieldBool("fail");

        Task t;
        t.name     = name;
        t.priority = (priority == "HIGH") ? Priority::HIGH
                   : (priority == "LOW")  ? Priority::LOW
                                          : Priority::NORMAL;
        t.deps     = deps;
        if (delay > 0)
            t.notBefore = Clock::now() + std::chrono::milliseconds(delay);
        if (period > 0)
            t.period = std::chrono::milliseconds(period);

        // Simulate work (replace with real work in production)
        t.fn = [fail, name]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (fail) throw std::runtime_error("forced failure");
            std::cout << "[worker] executed: " << name << "\n";
        };

        t.onComplete = [](TaskId id, bool ok) {
            std::cout << "[callback] task " << id
                      << (ok ? " DONE" : " FAILED") << "\n";
        };

        TaskId id = sched.submit(std::move(t));

        // record
        {
            std::lock_guard lock(g_mu);
            g_records.push_back({id, name.empty() ? "task-" + std::to_string(id) : name,
                                  priority, deps, ""});
        }

        res.set_content(
            "{\"id\":" + std::to_string(id) + ",\"status\":\"submitted\"}",
            "application/json");
    });

    // ── GET /tasks  →  list all tasks ─────────────────────────────────────────
    svr.Get("/tasks", [&](const httplib::Request&, httplib::Response& res) {
        addCors(res);
        std::string json = "[";
        std::lock_guard lock(g_mu);
        for (size_t i = 0; i < g_records.size(); ++i) {
            if (i) json += ",";
            const auto& r = g_records[i];
            TaskStatus s  = sched.statusOf(r.id);
            std::string ss;
            switch (s) {
                case TaskStatus::PENDING:   ss = "PENDING";   break;
                case TaskStatus::READY:     ss = "READY";     break;
                case TaskStatus::RUNNING:   ss = "RUNNING";   break;
                case TaskStatus::DONE:      ss = "DONE";      break;
                case TaskStatus::FAILED:    ss = "FAILED";    break;
                case TaskStatus::CANCELLED: ss = "CANCELLED"; break;
            }
            json += taskToJson(r.id, r.name, r.priority, ss, r.deps, r.errorMsg);
        }
        json += "]";
        res.set_content(json, "application/json");
    });

    // ── DELETE /tasks/:id  →  cancel ──────────────────────────────────────────
    svr.Delete(R"(/tasks/(\d+))", [&](const httplib::Request& req, httplib::Response& res) {
        addCors(res);
        TaskId id = std::stoull(req.matches[1]);
        bool ok   = sched.cancel(id);
        res.set_content(
            "{\"id\":" + std::to_string(id) +
            ",\"cancelled\":" + (ok ? "true" : "false") + "}",
            "application/json");
    });

    // ── GET /stats  →  aggregate counts ───────────────────────────────────────
    svr.Get("/stats", [&](const httplib::Request&, httplib::Response& res) {
        addCors(res);
        auto s = sched.stats();
        std::ostringstream o;
        o << "{"
          << "\"total\":"     << s.total     << ","
          << "\"pending\":"   << s.pending   << ","
          << "\"ready\":"     << s.ready     << ","
          << "\"running\":"   << s.running   << ","
          << "\"done\":"      << s.done      << ","
          << "\"failed\":"    << s.failed    << ","
          << "\"cancelled\":" << s.cancelled
          << "}";
        res.set_content(o.str(), "application/json");
    });

    // ── GET /workers  →  worker states ────────────────────────────────────────
    // (Approximate: scheduler doesn't expose per-worker state publicly,
    //  so we derive busy count from running tasks)
    svr.Get("/workers", [&](const httplib::Request&, httplib::Response& res) {
        addCors(res);
        auto s = sched.stats();
        std::ostringstream o;
        o << "{"
          << "\"count\":"  << sched.workerCount() << ","
          << "\"busy\":"   << s.running
          << "}";
        res.set_content(o.str(), "application/json");
    });

    std::cout << "╔══════════════════════════════════════════╗\n";
    std::cout << "║  TaskScheduler HTTP Server               ║\n";
    std::cout << "║  Listening on http://localhost:" << PORT << "      ║\n";
    std::cout << "╚══════════════════════════════════════════╝\n\n";
    std::cout << "Open scheduler_dashboard.html in your browser.\n\n";

    svr.listen("0.0.0.0", PORT);
}
