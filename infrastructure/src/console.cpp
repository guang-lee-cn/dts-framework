#include "console.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "ctl.h"
#include "log.h"
#include "thread_api.h"

namespace dts {

namespace {

// ---- console -> control 命令（会话线程投递，control 执行后回填响应）----
struct Job {
    std::string line;                 // 原始命令行
    std::string resp;                 // control 执行结果
    std::mutex m;
    std::condition_variable cv;
    bool done = false;                // control 已执行完（resp 有效）
};

using JobPtr = std::shared_ptr<Job>;

std::mutex g_qMutex;
std::condition_variable g_qCv;
std::deque<JobPtr> g_queue;
bool g_controlStop = false;  // 由 g_qMutex 保护

std::atomic<bool> g_consoleStop{false};

detsched::ThreadHandle g_consoleH = nullptr;
detsched::ThreadHandle g_controlH = nullptr;
int g_listenFd = -1;
std::string g_sockPath;

// 活跃会话连接：console_stop 时 shutdown 唤醒读阻塞
std::mutex g_connMutex;
std::set<int> g_activeFds;
std::vector<std::thread> g_sessions;

constexpr int kConsoleBkgPrio = 20;  // BKG 域（15-29，SCHED_OTHER）
constexpr int kPollTimeoutMs = 100;
constexpr char kRespDelim = '\x00';  // 响应结束分隔符（长连接一条响应一个 \x00）
constexpr size_t kMaxLineLen = 4096; // 单命令行上限（防恶意/误粘贴长行撑爆内存）

// 读一行：累积到 '\n' 或 EOF/错误，返回有效行（不含 '\n'）。
// 行超长（> kMaxLineLen）：丢弃该行内容（继续读到 '\n'），置 overflow 由调用方回错误。
bool ReadLine(int fd, std::string& line, bool& overflow) {
    line.clear();
    overflow = false;
    char buf[256];
    while (true) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            if (overflow) {
                return false;  // 超长行未收完即断连：不交付
            }
            return !line.empty();  // EOF：把残留行当一行
        }
        line.append(buf, static_cast<size_t>(n));
        const size_t nl = line.find('\n');
        if (nl != std::string::npos) {
            if (overflow) {
                return false;  // 超长行：整行丢弃（不执行）
            }
            line.erase(nl);
            return true;
        }
        if (line.size() > kMaxLineLen) {
            overflow = true;  // 超长：继续排空到 '\n'，之后整行丢弃
            line.clear();
        }
    }
}

void PushJob(const JobPtr& job) {
    {
        std::lock_guard<std::mutex> lk(g_qMutex);
        g_queue.push_back(job);
    }
    g_qCv.notify_one();
}

// ---- 会话线程：一个客户端连接，读行投递 -> 等 control 执行 -> 回写（长连接）----
void SessionLoop(int fd) {
    {
        std::lock_guard<std::mutex> lk(g_connMutex);
        g_activeFds.insert(fd);
    }
    while (!g_consoleStop.load()) {
        std::string line;
        bool tooLong = false;
        if (!ReadLine(fd, line, tooLong) || line.empty()) {
            break;  // EOF/关闭/超长行
        }
        auto job = std::make_shared<Job>();
        if (tooLong) {
            job->line.clear();
            job->resp = "ERR: line too long (max " + std::to_string(kMaxLineLen) + ")";
            job->done = true;
        } else {
            job->line = line;
        }
        if (!job->done) {
            PushJob(job);
        }
        {
            std::unique_lock<std::mutex> lk(job->m);
            job->cv.wait(lk, [&job] { return job->done; });
        }
        std::string resp = job->resp + kRespDelim;
        if (::send(fd, resp.data(), resp.size(), MSG_NOSIGNAL) < 0) {
            break;  // 对端已关
        }
    }
    ::close(fd);
    {
        std::lock_guard<std::mutex> lk(g_connMutex);
        g_activeFds.erase(fd);
    }
}

// ---- console 线程：accept，每连接起一个会话线程 ----
void ConsoleLoop(void*) {
    while (!g_consoleStop.load()) {
        pollfd pfd{};
        pfd.fd = g_listenFd;
        pfd.events = POLLIN;
        const int rc = ::poll(&pfd, 1, kPollTimeoutMs);
        if (rc <= 0) {
            continue;  // 超时/被打断，回查 stop
        }
        const int conn = ::accept(g_listenFd, nullptr, nullptr);
        if (conn < 0) {
            continue;
        }
        std::lock_guard<std::mutex> lk(g_connMutex);
        g_sessions.emplace_back(SessionLoop, conn);
        if (g_sessions.back().joinable()) {
            g_sessions.back().detach();  // 会话生命周期独立，console_stop 时统一 shutdown fd 退出
        }
    }
}

// ---- control 线程：消费队列 -> Execute -> 回填响应 ----
void ControlLoop(void*) {
    while (true) {
        JobPtr job;
        {
            std::unique_lock<std::mutex> lk(g_qMutex);
            g_qCv.wait(lk, [] { return g_controlStop || !g_queue.empty(); });
            if (g_controlStop && g_queue.empty()) {
                break;
            }
            job = std::move(g_queue.front());
            g_queue.pop_front();
        }
        ctl::Execute(job->line, job->resp);
        {
            std::lock_guard<std::mutex> lk(job->m);
            job->done = true;
        }
        job->cv.notify_one();
    }
}

}  // namespace

int console_start(const char* sock_path) {
    if (sock_path == nullptr) {
        dts::log::Error("[console] sock_path is null");
        return -1;
    }
    g_sockPath = sock_path;

    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        dts::log::Error("[console] socket create failed: {}", sock_path);
        return -1;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);
    ::unlink(sock_path);  // 清理残留 socket 文件
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
        ::listen(fd, 8) < 0) {
        dts::log::Error("[console] bind/listen failed: {}", sock_path);
        ::close(fd);
        return -1;
    }
    g_listenFd = fd;

    detsched::DeclareDomain(kConsoleBkgPrio);  // 幂等：声明 BKG 域
    g_consoleStop.store(false);
    g_consoleH = detsched::CreateThread("console", kConsoleBkgPrio, ConsoleLoop, nullptr);
    if (g_consoleH == nullptr) {
        dts::log::Error("[console] thread create failed");
        ::close(fd);
        g_listenFd = -1;
        return -1;
    }
    dts::log::Info("[console] up (sock={})", sock_path);
    return 0;
}

void console_stop() {
    g_consoleStop.store(true);
    if (g_listenFd >= 0) {
        ::shutdown(g_listenFd, SHUT_RDWR);  // 唤醒阻塞的 poll
    }
    if (g_consoleH != nullptr) {
        detsched::DestroyThread(g_consoleH);
        g_consoleH = nullptr;
    }
    if (g_listenFd >= 0) {
        ::close(g_listenFd);
        g_listenFd = -1;
    }
    // 唤醒并回收会话线程（shutdown 活跃连接 -> 读阻塞返回）
    {
        std::lock_guard<std::mutex> lk(g_connMutex);
        for (const int fd : g_activeFds) {
            ::shutdown(fd, SHUT_RDWR);
        }
    }
    for (auto& t : g_sessions) {
        if (t.joinable()) {
            t.join();
        }
    }
    g_sessions.clear();
    if (!g_sockPath.empty()) {
        ::unlink(g_sockPath.c_str());
        g_sockPath.clear();
    }
}

int control_start() {
    ctl::RegisterBuiltins();  // 幂等：内置命令就绪
    detsched::DeclareDomain(kConsoleBkgPrio);
    g_controlStop = false;
    g_controlH = detsched::CreateThread("control", kConsoleBkgPrio, ControlLoop, nullptr);
    if (g_controlH == nullptr) {
        dts::log::Error("[control] thread create failed");
        return -1;
    }
    dts::log::Info("[control] up");
    return 0;
}

void control_stop() {
    std::vector<JobPtr> pending;
    {
        std::lock_guard<std::mutex> lk(g_qMutex);
        g_controlStop = true;
        pending.assign(g_queue.begin(), g_queue.end());
        g_queue.clear();
    }
    g_qCv.notify_all();
    // 唤醒等待响应的会话（进程即将退出，回写失败由会话侧忽略）
    for (const auto& job : pending) {
        {
            std::lock_guard<std::mutex> lk(job->m);
            job->resp = "control stopped";
            job->done = true;
        }
        job->cv.notify_one();
    }
    if (g_controlH != nullptr) {
        detsched::DestroyThread(g_controlH);
        g_controlH = nullptr;
    }
}

}  // namespace dts
