// 自研 mini HTTP/SSE 服务器（观测面专用，~200 行，零第三方依赖）。
// 路由：GET /（静态 index.html）· GET /api/stats（JSON 快照）· GET /api/stream（SSE 流）。
// 非流式连接 Connection: close（短连接足够）；SSE 连接长驻，Broadcast 广播 + 慢连接剔除。
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace web {

class HttpSseServer {
public:
    // stats 提供者：填充 JSON 文本（消费线程外调用，需自身线程安全）
    using StatsFn = std::function<std::string()>;

    HttpSseServer() = default;
    ~HttpSseServer();

    HttpSseServer(const HttpSseServer&) = delete;
    HttpSseServer& operator=(const HttpSseServer&) = delete;

    // port：监听端口（0.0.0.0）；index_path：观测页文件；stats：/api/stats 内容提供者
    int Start(uint16_t port, const std::string& index_path, StatsFn stats);
    void Stop();

    // 广播一条 SSE 事件（event 名 + JSON 载荷）；任意线程可调；写失败的连接就地剔除
    void Broadcast(const std::string& event, const std::string& json);

    uint16_t port() const { return m_port; }

private:
    void AcceptLoop();
    void ServeConn(int fd);
    void ServeStream(int fd);  // SSE：挂到广播集合，阻塞读对端关闭

    int m_listenFd = -1;
    uint16_t m_port = 0;
    std::string m_index;
    StatsFn m_stats;
    std::thread m_accept;
    std::atomic<bool> m_running{false};

    std::mutex m_connMutex;         // SSE 连接集（广播写 + 关闭清理互斥）
    std::set<int> m_streams;
};

}  // namespace web
