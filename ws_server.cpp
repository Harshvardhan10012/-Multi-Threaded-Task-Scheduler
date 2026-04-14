/**
 * ws_server.cpp  —  WebSocket bridge for TaskScheduler
 *
 * Uses a minimal hand-rolled WebSocket server (RFC 6455) over raw BSD sockets
 * — zero external dependencies beyond what you already have.
 *
 * Protocol  (server → client, JSON frames):
 *   { "event": "task_submitted",  "task":   { id, name, priority, deps, status } }
 *   { "event": "task_status",     "id": N,  "status": "RUNNING"|"DONE"|"FAILED"|"CANCELLED" }
 *   { "event": "worker_update",   "workers": [ {id, busy, taskName} … ] }
 *   { "event": "stats",           "stats":  { total, pending, ready, running, done, failed, cancelled } }
 *   { "event": "log",             "msg": "…", "level": "info"|"warn"|"error" }
 *
 * Protocol  (client → server, JSON frames):
 *   { "cmd": "submit",  "name":"…", "priority":"HIGH|NORMAL|LOW",
 *                       "delay":0, "period":0, "deps":[…], "fail":false }
 *   { "cmd": "cancel",  "id": N }
 *   { "cmd": "reset"              }
 *
 * Build:
 *   g++ -std=c++17 -O2 -pthread \
 *       dispatcher.cpp priority_queue.cpp thread_pool.cpp task_scheduler.cpp \
 *       ws_server.cpp -I. -o ws_server
 *   ./ws_server
 *
 * Then open scheduler_dashboard.html — it auto-connects to ws://localhost:9001
 */

#include "scheduler.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace scheduler;
using namespace std::chrono_literals;

// ═══════════════════════════════════════════════════════════════
//  Tiny SHA-1 (for WebSocket handshake only)
// ═══════════════════════════════════════════════════════════════

static uint32_t sha1_rotl(uint32_t v, int n) { return (v << n) | (v >> (32-n)); }

static std::string sha1(const std::string& msg) {
    uint32_t h[5] = {0x67452301,0xEFCDAB89,0x98BADCFE,0x10325476,0xC3D2E1F0};
    std::vector<uint8_t> data(msg.begin(), msg.end());
    uint64_t bit_len = data.size() * 8;
    data.push_back(0x80);
    while (data.size() % 64 != 56) data.push_back(0);
    for (int i = 7; i >= 0; --i) data.push_back((bit_len >> (i*8)) & 0xff);

    for (size_t i = 0; i < data.size(); i += 64) {
        uint32_t w[80];
        for (int j = 0; j < 16; ++j)
            w[j] = (data[i+j*4]<<24)|(data[i+j*4+1]<<16)|(data[i+j*4+2]<<8)|data[i+j*4+3];
        for (int j = 16; j < 80; ++j)
            w[j] = sha1_rotl(w[j-3]^w[j-8]^w[j-14]^w[j-16], 1);
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4];
        for (int j = 0; j < 80; ++j) {
            uint32_t f,k;
            if      (j<20){f=(b&c)|(~b&d);k=0x5A827999;}
            else if (j<40){f=b^c^d;        k=0x6ED9EBA1;}
            else if (j<60){f=(b&c)|(b&d)|(c&d);k=0x8F1BBCDC;}
            else          {f=b^c^d;        k=0xCA62C1D6;}
            uint32_t tmp=sha1_rotl(a,5)+f+e+k+w[j];
            e=d;d=c;c=sha1_rotl(b,30);b=a;a=tmp;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;
    }
    std::string out(20,'\0');
    for (int i = 0; i < 5; ++i)
        for (int j = 3; j >= 0; --j)
            out[i*4+(3-j)] = (h[i]>>(j*8))&0xff;
    return out;
}

static std::string base64(const std::string& in) {
    static const char t[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int v=0,bits=0;
    for (unsigned char c : in) {
        v=(v<<8)|c; bits+=8;
        while (bits>=6) { bits-=6; out+=t[(v>>bits)&63]; }
    }
    if (bits>0) { out+=t[(v<<(6-bits))&63]; }
    while (out.size()%4) out+='=';
    return out;
}

// ═══════════════════════════════════════════════════════════════
//  WebSocket frame encode / decode
// ═══════════════════════════════════════════════════════════════

static std::string ws_frame(const std::string& payload) {
    std::string f;
    f += char(0x81);                    // FIN + opcode TEXT
    size_t n = payload.size();
    if (n <= 125)        { f += char(n); }
    else if (n <= 65535) { f += char(126); f += char(n>>8); f += char(n&0xff); }
    else { f += char(127);
        for (int i=7;i>=0;--i) f += char((n>>(i*8))&0xff);
    }
    f += payload;
    return f;
}

struct WsMsg { bool ok; std::string payload; };

static WsMsg ws_decode(const std::string& buf) {
    if (buf.size() < 2) return {false,{}};
    // uint8_t fin  = (buf[0] >> 7) & 1;  // unused
    // uint8_t opcode = buf[0] & 0x0f;    // unused
    bool masked = (buf[1] >> 7) & 1;
    size_t plen = buf[1] & 0x7f;
    size_t hdr = 2;
    if (plen == 126) { if (buf.size()<4) return {false,{}}; plen=(uint8_t(buf[2])<<8)|uint8_t(buf[3]); hdr=4; }
    else if (plen == 127) { hdr=10; /* simplified */ }
    if (masked) {
        if (buf.size() < hdr+4+plen) return {false,{}};
        const char* mask = buf.data()+hdr;
        std::string out(plen,'\0');
        for (size_t i=0;i<plen;++i) out[i]=buf[hdr+4+i]^mask[i%4];
        return {true,out};
    }
    if (buf.size() < hdr+plen) return {false,{}};
    return {true, buf.substr(hdr,plen)};
}

// ═══════════════════════════════════════════════════════════════
//  Tiny JSON helpers
// ═══════════════════════════════════════════════════════════════

static std::string jstr(const std::string& s) {
    std::string o="\"";
    for (char c:s){ if(c=='"')o+="\\\""; else if(c=='\\')o+="\\\\"; else o+=c; }
    return o+'"';
}

static std::string jfield(const std::string& body, const std::string& key, const std::string& def="") {
    auto pos=body.find('"'+key+'"'); if(pos==std::string::npos)return def;
    pos=body.find(':',pos); if(pos==std::string::npos)return def;
    ++pos; while(pos<body.size()&&body[pos]==' ')++pos;
    if(body[pos]=='"'){++pos;auto e=body.find('"',pos);return body.substr(pos,e-pos);}
    auto e=body.find_first_of(",}\n",pos);
    std::string v=body.substr(pos,e-pos);
    while(!v.empty()&&(v.back()==' '||v.back()=='\t'))v.pop_back();
    return v;
}

static int jint(const std::string& body, const std::string& key, int def=0) {
    auto v=jfield(body,key,""); return v.empty()?def:std::stoi(v);
}

static bool jbool(const std::string& body, const std::string& key) {
    return jfield(body,key,"false")=="true";
}

static std::vector<TaskId> jdeps(const std::string& body) {
    std::vector<TaskId> deps;
    auto p=body.find("\"deps\""); if(p==std::string::npos)return deps;
    auto l=body.find('[',p), r=body.find(']',p);
    if(l==std::string::npos||r==std::string::npos)return deps;
    std::istringstream ss(body.substr(l+1,r-l-1));
    std::string tok;
    while(std::getline(ss,tok,',')){ tok.erase(0,tok.find_first_not_of(" \t")); if(!tok.empty())deps.push_back(std::stoull(tok));}
    return deps;
}

// ═══════════════════════════════════════════════════════════════
//  Client connection pool
// ═══════════════════════════════════════════════════════════════

struct Client { int fd; };

static std::mutex              g_clients_mu;
static std::vector<Client>     g_clients;

static void broadcast(const std::string& json) {
    auto frame = ws_frame(json);
    std::lock_guard lock(g_clients_mu);
    for (auto it = g_clients.begin(); it != g_clients.end(); ) {
        if (::send(it->fd, frame.data(), frame.size(), MSG_NOSIGNAL) <= 0) {
            ::close(it->fd);
            it = g_clients.erase(it);
        } else ++it;
    }
}

static void broadcastLog(const std::string& msg, const std::string& level="info") {
    broadcast("{\"event\":\"log\",\"msg\":" + jstr(msg) + ",\"level\":\""+level+"\"}");
}

// ═══════════════════════════════════════════════════════════════
//  Task registry (name + priority store for broadcast)
// ═══════════════════════════════════════════════════════════════

struct TRec { std::string name, priority; std::vector<TaskId> deps; };
static std::mutex                          g_rec_mu;
static std::unordered_map<TaskId, TRec>    g_rec;

static std::string taskJson(TaskId id, const std::string& status) {
    std::lock_guard lock(g_rec_mu);
    auto it = g_rec.find(id);
    std::string name = it!=g_rec.end()?it->second.name:"task-"+std::to_string(id);
    std::string pri  = it!=g_rec.end()?it->second.priority:"NORMAL";
    std::string deps = "[";
    if (it!=g_rec.end()) for(size_t i=0;i<it->second.deps.size();++i){if(i)deps+=",";deps+=std::to_string(it->second.deps[i]);}
    deps += "]";
    return "{\"id\":"+std::to_string(id)+",\"name\":"+jstr(name)+",\"priority\":"+jstr(pri)+",\"status\":"+jstr(status)+",\"deps\":"+deps+"}";
}

// ═══════════════════════════════════════════════════════════════
//  Scheduler + worker state
// ═══════════════════════════════════════════════════════════════

static TaskScheduler* g_sched = nullptr;

// Worker tracking (per-worker task name)
static std::mutex                              g_worker_mu;
static std::unordered_map<TaskId, std::string> g_running; // taskId → name

static void broadcastWorkers() {
    // Derive worker states from running tasks
    auto stats = g_sched->stats();
    std::string w = "[";
    {
        std::lock_guard lock(g_worker_mu);
        int wi = 0;
        for (auto& [id, name] : g_running) {
            if (wi++) w += ",";
            w += "{\"id\":"+std::to_string(wi-1)+",\"busy\":true,\"taskName\":"+jstr(name)+"}";
        }
        for (size_t i = g_running.size(); i < 4; ++i) {
            if (wi++) w += ",";
            w += "{\"id\":"+std::to_string(i)+",\"busy\":false,\"taskName\":\"\"}";
        }
    }
    w += "]";
    broadcast("{\"event\":\"worker_update\",\"workers\":"+w+"}");
}

static void broadcastStats() {
    auto s = g_sched->stats();
    broadcast("{\"event\":\"stats\",\"stats\":{"
        "\"total\":"     +std::to_string(s.total)+","
        "\"pending\":"   +std::to_string(s.pending)+","
        "\"ready\":"     +std::to_string(s.ready)+","
        "\"running\":"   +std::to_string(s.running)+","
        "\"done\":"      +std::to_string(s.done)+","
        "\"failed\":"    +std::to_string(s.failed)+","
        "\"cancelled\":" +std::to_string(s.cancelled)+"}}");
}

static TaskId submitTask(const std::string& name, const std::string& priority,
                         int delay, int period, bool fail,
                         const std::vector<TaskId>& deps) {
    Task t;
    t.name     = name;
    t.priority = (priority=="HIGH")?Priority::HIGH:(priority=="LOW")?Priority::LOW:Priority::NORMAL;
    t.deps     = deps;
    if (delay > 0) t.notBefore = Clock::now() + std::chrono::milliseconds(delay);
    if (period > 0) t.period   = std::chrono::milliseconds(period);

    t.fn = [fail, name]() {
        std::this_thread::sleep_for(80ms);  // simulated work
        if (fail) throw std::runtime_error("forced failure");
    };

    t.onComplete = [name](TaskId id, bool ok) {
        {
            std::lock_guard lock(g_worker_mu);
            g_running.erase(id);
        }
        std::string status = ok ? "DONE" : "FAILED";
        broadcast("{\"event\":\"task_status\",\"id\":"+std::to_string(id)+",\"status\":\""+status+"\"}");
        broadcastLog((ok?"✓ Done: ":"✗ Failed: ")+name, ok?"info":"error");
        broadcastWorkers();
        broadcastStats();
    };

    TaskId id = g_sched->submit(std::move(t));

    {
        std::lock_guard lock(g_rec_mu);
        g_rec[id] = {name.empty()?"task-"+std::to_string(id):name, priority, deps};
    }
    {
        std::lock_guard lock(g_worker_mu);
        g_running[id] = name;   // mark as running immediately for worker display
    }

    // broadcast submitted event
    broadcast("{\"event\":\"task_submitted\",\"task\":"+taskJson(id,"PENDING")+"}");
    broadcastLog("Submitted \""+name+"\" ["+priority+"]"+(deps.empty()?"":" deps:"+std::to_string(deps.size())));
    broadcastStats();
    return id;
}

// ═══════════════════════════════════════════════════════════════
//  Handle one client connection
// ═══════════════════════════════════════════════════════════════

static void handleClient(int fd) {
    // ── WebSocket handshake ───────────────────────────────────
    char buf[4096]; int n;
    std::string req;
    while ((n = ::recv(fd, buf, sizeof(buf)-1, 0)) > 0) {
        buf[n] = 0; req += buf;
        if (req.find("\r\n\r\n") != std::string::npos) break;
    }

    // Extract Sec-WebSocket-Key
    std::string key;
    auto kpos = req.find("Sec-WebSocket-Key:");
    if (kpos == std::string::npos) { ::close(fd); return; }
    kpos += 18;
    while (req[kpos] == ' ') ++kpos;
    auto kend = req.find("\r\n", kpos);
    key = req.substr(kpos, kend - kpos);

    std::string accept = base64(sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
    std::string resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
    ::send(fd, resp.data(), resp.size(), 0);

    // Register client
    {
        std::lock_guard lock(g_clients_mu);
        g_clients.push_back({fd});
    }

    std::cout << "[ws] client connected (fd=" << fd << ")\n";
    broadcastLog("Browser connected", "info");

    // Send current state snapshot
    broadcastStats();
    broadcastWorkers();

    // ── Message loop ─────────────────────────────────────────
    std::string rbuf;
    while (true) {
        char tmp[4096];
        int r = ::recv(fd, tmp, sizeof(tmp), 0);
        if (r <= 0) break;
        rbuf.append(tmp, r);

        auto msg = ws_decode(rbuf);
        if (!msg.ok) continue;
        rbuf.clear();

        // Parse command
        std::string cmd = jfield(msg.payload, "cmd");

        if (cmd == "submit") {
            std::string name     = jfield(msg.payload, "name");
            std::string priority = jfield(msg.payload, "priority", "NORMAL");
            int         delay    = jint(msg.payload, "delay", 0);
            int         period   = jint(msg.payload, "period", 0);
            bool        fail     = jbool(msg.payload, "fail");
            auto        deps     = jdeps(msg.payload);
            submitTask(name, priority, delay, period, fail, deps);

        } else if (cmd == "cancel") {
            TaskId id = (TaskId)jint(msg.payload, "id", 0);
            bool ok   = g_sched->cancel(id);
            broadcast("{\"event\":\"task_status\",\"id\":"+std::to_string(id)+",\"status\":\"CANCELLED\"}");
            broadcastLog(std::string("Cancel #")+std::to_string(id)+": "+(ok?"ok":"too late"),
                         ok?"info":"warn");
            broadcastStats();

        } else if (cmd == "reset") {
            // Restart scheduler
            delete g_sched;
            g_sched = new TaskScheduler(4);
            {
                std::lock_guard lock(g_rec_mu);
                g_rec.clear();
            }
            {
                std::lock_guard lock(g_worker_mu);
                g_running.clear();
            }
            broadcast("{\"event\":\"reset\"}");
            broadcastLog("Scheduler reset", "info");
            broadcastStats();
        }
    }

    // Deregister
    {
        std::lock_guard lock(g_clients_mu);
        g_clients.erase(std::remove_if(g_clients.begin(), g_clients.end(),
            [fd](const Client& c){ return c.fd == fd; }), g_clients.end());
    }
    ::close(fd);
    std::cout << "[ws] client disconnected (fd=" << fd << ")\n";
}

// ═══════════════════════════════════════════════════════════════
//  main
// ═══════════════════════════════════════════════════════════════

int main() {
    constexpr int PORT = 9001;

    TaskScheduler sched(4);
    g_sched = &sched;

    // Status push thread: broadcasts stats + worker state every 500ms
    std::thread pusher([&]{
        while (true) {
            std::this_thread::sleep_for(500ms);
            {
                std::lock_guard lock(g_clients_mu);
                if (g_clients.empty()) continue;
            }
            broadcastStats();
            broadcastWorkers();
        }
    });
    pusher.detach();

    // TCP server
    int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(PORT);
    ::bind(srv, (sockaddr*)&addr, sizeof(addr));
    ::listen(srv, 16);

    std::cout << "╔══════════════════════════════════════════╗\n"
              << "║  TaskScheduler  WebSocket Server         ║\n"
              << "║  ws://localhost:" << PORT << "                   ║\n"
              << "╚══════════════════════════════════════════╝\n\n"
              << "Open scheduler_dashboard.html in your browser.\n\n";

    while (true) {
        sockaddr_in cli{}; socklen_t len = sizeof(cli);
        int fd = ::accept(srv, (sockaddr*)&cli, &len);
        if (fd < 0) continue;
        std::thread([fd]{ handleClient(fd); }).detach();
    }
}
