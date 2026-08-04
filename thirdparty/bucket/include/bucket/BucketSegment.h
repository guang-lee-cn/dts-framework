// 自研共享内存桶（自研桶传输的传输核心）：单读多写、变长消息、futex 唤醒。
//
// 布局：[Header | ring bytes]，POSIX shm（shm_open + mmap）。
// 计数语义：write_pos/read_pos 单调累加，物理偏移 = pos % capacity。
// 写入互斥：robust 进程共享 mutex（写者崩溃后下一位写者可接管）。
// 崩溃安全：写者先写数据、后发布 write_pos，崩溃的半条消息对读者不可见。
// 唤醒：读者 FUTEX_WAIT 在 wake_word；写者在段满时 FUTEX_WAIT 在 drain_word。
//
// 使用方（BucketChannelResource/BucketTransport）只依赖 Read/Write/Open/Create/Remove。
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace bucket {

// 共享内存头部布局（跨进程一致，定义见 BucketSegment.cpp）
struct Header;

class BucketSegment {
public:
    ~BucketSegment();

    BucketSegment(const BucketSegment&) = delete;
    BucketSegment& operator=(const BucketSegment&) = delete;

    // 首次创建段（O_EXCL）。name 为 POSIX shm 名（以 '/' 开头）。失败返回 nullptr。
    static BucketSegment* Create(const char* name, size_t capacity);

    // 打开已存在段。失败返回 nullptr。
    static BucketSegment* Open(const char* name);

    // 输入通道所有者独占调用：若段为残留（原 owner 进程已死）则重置环并接管。
    // 返回 false 表示段已由其他存活进程持有。
    bool ClaimOwnership();

    // 删除段（调用方需保证无人再使用）。返回 false 表示段不存在。
    static bool Remove(const char* name);

    // 写一条消息。段满时阻塞至 deadline；成功返回 true，超时/段失效返回 false。
    // 幂等性由上层（RTPS 可靠层）保证，此处不做部分写。
    bool Write(const uint8_t* data, uint32_t len,
               std::chrono::steady_clock::time_point deadline);

    // 读一条消息到 out。段空时阻塞至 deadline，超时返回 false。
    // 消费条目后推进 read_pos（即使 len > max 也消费，返回实际 len，调用方校验）。
    bool Read(uint8_t* out, uint32_t max, uint32_t& len,
              std::chrono::steady_clock::time_point deadline);

    const char* name() const { return name_.c_str(); }
    size_t capacity() const { return capacity_; }

private:
    BucketSegment(std::string name, int fd, Header* hdr);

    bool wait_wake(std::chrono::steady_clock::time_point deadline);
    bool wait_drain(std::chrono::steady_clock::time_point deadline);

    std::string name_;
    int shm_fd_ = -1;
    Header* hdr_ = nullptr;
    uint8_t* ring_ = nullptr;
    size_t capacity_ = 0;
    uint64_t read_pos_ = 0;  // 本进程读侧游标（仅读者使用）
};

}  // namespace bucket
