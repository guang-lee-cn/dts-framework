#include "http_sse.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <fstream>
#include <sstream>

#include "log.h"

namespace web {

using dts::log::Error;
using dts::log::Info;

namespace {

constexpr int kBacklog = 16;
constexpr size_t kMaxReqHead = 4096;  // 请求头上限（防恶意/误粘贴长连接撑爆内存）

// 读到 \r\n\r\n 为止；返回请求首行（method + path），失败返回空
bool ReadRequestHead(int fd, std::string& firstLine) {
    std::string buf;
    char chunk[512];
    while (buf.size() < kMaxReqHead) {
        const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            return false;
        }
        buf.append(chunk, static_cast<size_t>(n));
        const size_t end = buf.find("\r\n\r\n");
        if (end != std::string::npos) {
            const size_t eol = buf.find("\r\n");
            firstLine = buf.substr(0, eol);
            return !firstLine.empty();
        }
    }
    return false;
}

void Respond(int fd, const char* status, const char* contentType, const std::string& body,
             const char* extraHead = "") {
    std::ostringstream os;
    os << "HTTP/1.1 " << status << "\r\n"
       << "Content-Type: " << contentType << "\r\n"
       << "Content-Length: " << body.size() << "\r\n"
       << "Connection: close\r\n"
       << extraHead << "\r\n"
       << body;
    const std::string out = os.str();
    (void)::send(fd, out.data(), out.size(), MSG_NOSIGNAL);
}

std::string SlurpFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) {
        return "";
    }
    std::ostringstream os;
    os << f.rdbuf();
    return os.str();
}

}  // namespace

HttpSseServer::~HttpSseServer() {
    Stop();
}

int HttpSseServer::Start(uint16_t port, const std::string& index_path, StatsFn stats) {
    m_index = SlurpFile(index_path);
    if (m_index.empty()) {
        Error("[web:http] index page missing: {}", index_path);
        return -1;
    }
    m_stats = std::move(stats);
    m_port = port;

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        Error("[web:http] socket create failed");
        return -1;
    }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
        ::listen(fd, kBacklog) < 0) {
        Error("[web:http] bind/listen failed (port={})", port);
        ::close(fd);
        return -1;
    }
    m_listenFd = fd;
    m_running.store(true);
    m_accept = std::thread(&HttpSseServer::AcceptLoop, this);
    Info("[web:http] up (http://0.0.0.0:{})", port);
    return 0;
}

void HttpSseServer::Stop() {
    if (!m_running.exchange(false)) {
        return;
    }
    if (m_listenFd >= 0) {
        ::shutdown(m_listenFd, SHUT_RDWR);
        ::close(m_listenFd);
        m_listenFd = -1;
    }
    if (m_accept.joinable()) {
        m_accept.join();
    }
    std::lock_guard<std::mutex> lk(m_connMutex);
    for (int fd : m_streams) {
        ::shutdown(fd, SHUT_RDWR);
    }
}

void HttpSseServer::AcceptLoop() {
    while (m_running.load()) {
        pollfd pfd{};
        pfd.fd = m_listenFd;
        pfd.events = POLLIN;
        if (::poll(&pfd, 1, 200) <= 0) {
            continue;
        }
        const int conn = ::accept(m_listenFd, nullptr, nullptr);
        if (conn < 0) {
            continue;
        }
        // 每连接一线程（观测面并发极低，无需事件循环）
        std::thread(&HttpSseServer::ServeConn, this, conn).detach();
    }
}

void HttpSseServer::ServeConn(int fd) {
    std::string firstLine;
    if (!ReadRequestHead(fd, firstLine)) {
        ::close(fd);
        return;
    }
    const size_t sp1 = firstLine.find(' ');
    const size_t sp2 = firstLine.rfind(' ');
    if (sp1 == std::string::npos || sp2 == sp1) {
        ::close(fd);
        return;
    }
    const std::string method = firstLine.substr(0, sp1);
    const std::string path = firstLine.substr(sp1 + 1, sp2 - sp1 - 1);

    if (method != "GET") {
        Respond(fd, "405 Method Not Allowed", "text/plain", "GET only\n");
        ::close(fd);
        return;
    }
    if (path == "/api/stream") {
        ServeStream(fd);
        return;
    }
    if (path == "/api/stats") {
        Respond(fd, "200 OK", "application/json", m_stats ? m_stats() : "{}");
        ::close(fd);
        return;
    }
    if (path == "/" || path == "/index.html") {
        Respond(fd, "200 OK", "text/html; charset=utf-8", m_index);
        ::close(fd);
        return;
    }
    Respond(fd, "404 Not Found", "text/plain", "not found\n");
    ::close(fd);
}

void HttpSseServer::ServeStream(int fd) {
    static constexpr const char* kSseHead =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n\r\n";
    if (::send(fd, kSseHead, std::strlen(kSseHead), MSG_NOSIGNAL) < 0) {
        ::close(fd);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(m_connMutex);
        m_streams.insert(fd);
    }
    Info("[web:sse] client connected (fd={}, total={})", fd, [this] {
        std::lock_guard<std::mutex> lk(m_connMutex);
        return m_streams.size();
    }());

    // 阻塞等对端关闭（recv 0）——连接生命周期由 Broadcast 的写失败与此处共同收敛
    char buf[64];
    while (m_running.load() && ::recv(fd, buf, sizeof(buf), 0) > 0) {
    }
    {
        std::lock_guard<std::mutex> lk(m_connMutex);
        m_streams.erase(fd);
    }
    ::close(fd);
    Info("[web:sse] client disconnected (fd={})", fd);
}

void HttpSseServer::Broadcast(const std::string& event, const std::string& json) {
    std::string msg = "event: " + event + "\ndata: " + json + "\n\n";
    std::lock_guard<std::mutex> lk(m_connMutex);
    for (auto it = m_streams.begin(); it != m_streams.end();) {
        if (::send(*it, msg.data(), msg.size(), MSG_NOSIGNAL) < 0) {
            ::shutdown(*it, SHUT_RDWR);  // 唤醒 ServeStream 的 recv 使其退出清理
            it = m_streams.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace web
