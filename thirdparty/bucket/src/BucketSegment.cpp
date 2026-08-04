#include "bucket/BucketSegment.h"

#include <linux/futex.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cstring>

namespace bucket {

// 头部置于共享内存映射起点；ring 紧随其后、按 64B 对齐。
struct Header {
    uint32_t magic;
    uint32_t reserved;
    uint64_t capacity;
    std::atomic<uint64_t> write_pos;   // 下一个待写字节偏移（单调）
    std::atomic<uint64_t> read_pos;    // 已消费偏移（单调）
    std::atomic<uint32_t> wake_word;   // 发布新数据时 ++，读者 FUTEX_WAIT
    std::atomic<uint32_t> drain_word;  // 读者消费后 ++，写者在段满时 FUTEX_WAIT
    std::atomic<uint32_t> owner_pid;   // 输入通道 owner 进程（残留检测）
    pthread_mutex_t write_lock;        // robust 进程共享 mutex（多写互斥）
};

namespace {

constexpr uint32_t kMagic = 0x42434B54;  // "BCKT"
constexpr uint64_t kEntryAlign = 8;       // 条目起始对齐

// 计算 header 之后的 ring 起始偏移（对齐 64B），并保证 capacity 为 8 的倍数。
constexpr size_t kRingAlign = 64;

size_t ring_offset()
{
    return (sizeof(Header) + kRingAlign - 1) & ~(kRingAlign - 1);
}

size_t align_down8(size_t v)
{
    return v & ~(kEntryAlign - 1);
}

long futex_wait(void* addr, uint32_t expected, const timespec* ts)
{
    return syscall(SYS_futex, addr, FUTEX_WAIT, expected, ts, nullptr, 0);
}

long futex_wake(void* addr, uint32_t n)
{
    return syscall(SYS_futex, addr, FUTEX_WAKE, n, nullptr, nullptr, 0);
}

// 相对剩余时间，<=0 返回 true（已到 deadline）
bool expired(std::chrono::steady_clock::time_point deadline,
             timespec& ts)
{
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline)
    {
        return true;
    }
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count();
    ts.tv_sec = ns / 1000000000L;
    ts.tv_nsec = ns % 1000000000L;
    return false;
}

}  // namespace

BucketSegment::BucketSegment(std::string name, int fd, Header* hdr)
    : name_(std::move(name))
    , shm_fd_(fd)
    , hdr_(hdr)
    , capacity_(hdr->capacity)
    , read_pos_(hdr->read_pos.load(std::memory_order_acquire))
{
    ring_ = reinterpret_cast<uint8_t*>(hdr_) + ring_offset();
}

BucketSegment::~BucketSegment()
{
    if (shm_fd_ >= 0)
    {
        ::munmap(hdr_, ring_offset() + capacity_);
        ::close(shm_fd_);
    }
}

BucketSegment* BucketSegment::Create(const char* name, size_t capacity)
{
    if (name == nullptr || name[0] != '/' || capacity < 1024 || capacity % kEntryAlign != 0)
    {
        return nullptr;
    }

    int fd = ::shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0666);
    if (fd < 0)
    {
        return nullptr;
    }

    size_t total = ring_offset() + capacity;
    if (::ftruncate(fd, static_cast<off_t>(total)) != 0)
    {
        ::close(fd);
        ::shm_unlink(name);
        return nullptr;
    }

    void* base = ::mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED)
    {
        ::close(fd);
        ::shm_unlink(name);
        return nullptr;
    }

    auto* hdr = new (base) Header{};
    pthread_mutexattr_t attr;
    ::pthread_mutexattr_init(&attr);
    ::pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    ::pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
    ::pthread_mutex_init(&hdr->write_lock, &attr);
    ::pthread_mutexattr_destroy(&attr);

    hdr->magic = kMagic;
    hdr->capacity = capacity;
    hdr->write_pos = 0;
    hdr->read_pos = 0;
    hdr->wake_word = 0;
    hdr->drain_word = 0;
    hdr->owner_pid = 0;

    return new BucketSegment(name, fd, hdr);
}

bool BucketSegment::ClaimOwnership()
{
    if (hdr_->magic != kMagic)
    {
        return false;
    }

    pid_t me = ::getpid();
    int rc = ::pthread_mutex_lock(&hdr_->write_lock);
    if (rc == EOWNERDEAD)
    {
        ::pthread_mutex_consistent(&hdr_->write_lock);
    }
    else if (rc != 0)
    {
        return false;
    }

    bool claimed = false;
    uint32_t owner = hdr_->owner_pid.load(std::memory_order_acquire);
    bool owner_dead = owner != 0 && static_cast<pid_t>(owner) != me &&
            (::kill(static_cast<pid_t>(owner), 0) != 0 && errno == ESRCH);
    if (owner == 0 || owner_dead)
    {
        // 残留段（原 owner 崩溃）或首次接管：重置环
        hdr_->write_pos.store(0, std::memory_order_release);
        hdr_->read_pos.store(0, std::memory_order_release);
        hdr_->owner_pid.store(static_cast<uint32_t>(me), std::memory_order_release);
        read_pos_ = 0;
        claimed = true;
    }

    ::pthread_mutex_unlock(&hdr_->write_lock);
    return claimed || owner == static_cast<uint32_t>(me);
}

BucketSegment* BucketSegment::Open(const char* name)
{
    int fd = ::shm_open(name, O_RDWR, 0666);
    if (fd < 0)
    {
        return nullptr;
    }

    struct stat st;
    if (::fstat(fd, &st) != 0 || st.st_size <= 0)
    {
        ::close(fd);
        return nullptr;
    }

    void* base = ::mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd, 0);
    if (base == MAP_FAILED)
    {
        ::close(fd);
        return nullptr;
    }

    auto* hdr = static_cast<Header*>(base);
    if (hdr->magic != kMagic || hdr->capacity == 0 ||
            static_cast<size_t>(st.st_size) != ring_offset() + hdr->capacity)
    {
        ::munmap(base, static_cast<size_t>(st.st_size));
        ::close(fd);
        return nullptr;
    }

    return new BucketSegment(name, fd, hdr);
}

bool BucketSegment::Remove(const char* name)
{
    return ::shm_unlink(name) == 0;
}

bool BucketSegment::wait_wake(std::chrono::steady_clock::time_point deadline)
{
    while (true)
    {
        if (hdr_->write_pos.load(std::memory_order_acquire) > read_pos_)
        {
            return true;
        }
        timespec ts;
        if (expired(deadline, ts))
        {
            return false;
        }
        uint32_t expected = hdr_->wake_word.load(std::memory_order_acquire);
        // 值已变化则 futex 立即返回 EAGAIN，循环重判
        futex_wait(&hdr_->wake_word, expected, &ts);
    }
}

bool BucketSegment::wait_drain(std::chrono::steady_clock::time_point deadline)
{
    uint64_t wpos = hdr_->write_pos.load(std::memory_order_relaxed);
    uint64_t rpos = hdr_->read_pos.load(std::memory_order_acquire);
    size_t need = static_cast<size_t>(wpos - rpos);  // 占用字节
    if (capacity_ - need >= kEntryAlign)
    {
        return true;  // 至少 8B（一条空消息的最小条目）
    }

    while (true)
    {
        timespec ts;
        if (expired(deadline, ts))
        {
            return false;
        }
        uint32_t expected = hdr_->drain_word.load(std::memory_order_acquire);
        futex_wait(&hdr_->drain_word, expected, &ts);
        wpos = hdr_->write_pos.load(std::memory_order_relaxed);
        rpos = hdr_->read_pos.load(std::memory_order_acquire);
        if (capacity_ - static_cast<size_t>(wpos - rpos) >= kEntryAlign)
        {
            return true;
        }
    }
}

bool BucketSegment::Write(const uint8_t* data, uint32_t len,
                          std::chrono::steady_clock::time_point deadline)
{
    if (hdr_->magic != kMagic)
    {
        return false;
    }

    size_t need = align_down8(static_cast<size_t>(len) + sizeof(uint32_t)) + kEntryAlign;
    // 单条消息不能超过容量（否则永远无法放下）
    if (need > capacity_)
    {
        return false;
    }

    int rc = ::pthread_mutex_lock(&hdr_->write_lock);
    if (rc == EOWNERDEAD)
    {
        ::pthread_mutex_consistent(&hdr_->write_lock);
        // 上一位写者崩溃：其未发布数据对读者不可见，直接接管
    }
    else if (rc != 0)
    {
        return false;
    }

    while (!wait_drain(deadline))
    {
        ::pthread_mutex_unlock(&hdr_->write_lock);
        return false;
    }

    uint64_t wpos = hdr_->write_pos.load(std::memory_order_relaxed);
    size_t off = static_cast<size_t>(wpos % capacity_);

    // 写入长度头 + 载荷（可能跨环尾，分两段拷）
    auto copy_in = [&](size_t at, const uint8_t* src, size_t n)
    {
        size_t c = capacity_;
        size_t seg = c - at;
        if (seg > n)
        {
            seg = n;
        }
        std::memcpy(ring_ + at, src, seg);
        if (seg < n)
        {
            std::memcpy(ring_, src + seg, n - seg);
        }
    };

    uint32_t hdr_len = len;
    if (off + sizeof(hdr_len) <= capacity_)
    {
        std::memcpy(ring_ + off, &hdr_len, sizeof(hdr_len));
        copy_in(off + sizeof(hdr_len), data, len);
    }
    else
    {
        copy_in(off, reinterpret_cast<const uint8_t*>(&hdr_len), sizeof(hdr_len));
        copy_in((off + sizeof(hdr_len)) % capacity_, data, len);
    }

    // 先写数据、后发布（release），崩溃时不发布 -> 读者不可见
    hdr_->write_pos.store(wpos + need, std::memory_order_release);
    hdr_->wake_word.fetch_add(1, std::memory_order_release);
    futex_wake(&hdr_->wake_word, INT32_MAX);

    ::pthread_mutex_unlock(&hdr_->write_lock);
    return true;
}

bool BucketSegment::Read(uint8_t* out, uint32_t max, uint32_t& len,
                         std::chrono::steady_clock::time_point deadline)
{
    if (hdr_->magic != kMagic)
    {
        return false;
    }

    while (true)
    {
        uint64_t wpos = hdr_->write_pos.load(std::memory_order_acquire);
        if (wpos - read_pos_ >= sizeof(uint32_t))
        {
            // 读长度头（可能跨环尾）
            size_t off = static_cast<size_t>(read_pos_ % capacity_);
            const size_t first = capacity_ - off;
            uint32_t hdr_len = 0;
            if (first >= sizeof(hdr_len))
            {
                std::memcpy(&hdr_len, ring_ + off, sizeof(hdr_len));
            }
            else
            {
                std::memcpy(&hdr_len, ring_ + off, first);
                std::memcpy(reinterpret_cast<uint8_t*>(&hdr_len) + first, ring_,
                            sizeof(hdr_len) - first);
            }

            size_t entry = align_down8(static_cast<size_t>(hdr_len) + sizeof(uint32_t)) + kEntryAlign;
            if (wpos - read_pos_ < entry)
            {
                // 条目未写完整（写者发布前崩溃不会到这一步；此处为防御）
                return false;
            }

            off = static_cast<size_t>((read_pos_ + sizeof(uint32_t)) % capacity_);
            size_t payload = hdr_len;
            if (payload > 0)
            {
                const size_t f = capacity_ - off;
                size_t n = payload > max ? max : payload;
                size_t seg = f > n ? n : f;
                std::memcpy(out, ring_ + off, seg);
                if (seg < n)
                {
                    std::memcpy(out + seg, ring_, n - seg);
                }
            }

            read_pos_ += entry;
            len = hdr_len;
            hdr_->read_pos.store(read_pos_, std::memory_order_release);
            hdr_->drain_word.fetch_add(1, std::memory_order_release);
            futex_wake(&hdr_->drain_word, INT32_MAX);
            return true;
        }

        // 空：等 wake_word
        if (!wait_wake(deadline))
        {
            return false;
        }
    }
}

}  // namespace bucket
