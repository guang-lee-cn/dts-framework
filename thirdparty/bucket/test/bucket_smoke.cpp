// BucketSegment 跨进程冒烟：一个 reader + N 个 writer 进程。
//   bucket_smoke r <name> <cap> <total> <timeout_s>             // reader：建段，收 total 条，查 seq 全集
//   bucket_smoke w <name> <cap> <count> <size> <first_seq> [delay_us]
// 载荷前 4 字节 = seq(LE u32)，writer 按 [first_seq, first_seq+count) 分配，reader 收满后查全。
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "bucket/BucketSegment.h"

using bucket::BucketSegment;
using namespace std::chrono;

static bool reader_mode(const char* name, size_t cap, uint32_t total, int timeout_s)
{
    auto* seg = BucketSegment::Create(name, cap);
    if (seg == nullptr)
    {
        std::printf("[smoke r] create failed\n");
        return false;
    }

    std::vector<uint8_t> seen(total, 0);
    uint32_t recv = 0;
    uint32_t over = 0;
    auto deadline = steady_clock::now() + seconds(timeout_s);
    std::vector<uint8_t> buf(cap);
    while (recv < total && steady_clock::now() < deadline)
    {
        uint32_t len = 0;
        if (!seg->Read(buf.data(), static_cast<uint32_t>(buf.size()), len, deadline))
        {
            continue;  // 超时由外层 deadline 兜底
        }
        if (len < 4)
        {
            continue;
        }
        uint32_t seq = 0;
        std::memcpy(&seq, buf.data(), 4);
        if (seq < total)
        {
            seen[seq] = 1;
        }
        else
        {
            over++;
        }
        recv++;
    }

    std::uint64_t missing = 0;
    for (uint32_t i = 0; i < total; i++)
    {
        if (!seen[i])
        {
            missing++;
        }
    }
    std::printf("[smoke r] recv=%u/%u missing=%llu over_range=%u -> %s\n",
                recv, total, static_cast<unsigned long long>(missing), over,
                (recv == total && missing == 0) ? "PASS" : "FAIL");

    BucketSegment::Remove(name);
    delete seg;
    return recv == total && missing == 0;
}

static bool writer_mode(const char* name, uint32_t count, uint32_t size,
                        uint32_t first_seq, int delay_us)
{
    auto* seg = BucketSegment::Open(name);
    if (seg == nullptr)
    {
        std::printf("[smoke w] open failed\n");
        return false;
    }

    std::vector<uint8_t> pkt(size, 0);
    uint32_t fail = 0;
    auto t0 = steady_clock::now();
    for (uint32_t i = 0; i < count; i++)
    {
        std::memcpy(pkt.data(), &i, 4);
        // 每条 seq = first_seq + i
        std::memcpy(pkt.data(), &first_seq, 4);
        if (!seg->Write(pkt.data(), size, steady_clock::now() + seconds(10)))
        {
            fail++;
            std::printf("[smoke w] write fail at %u\n", i);
            break;
        }
        if (delay_us > 0)
        {
            std::this_thread::sleep_for(microseconds(delay_us));
        }
        first_seq++;
    }
    auto t1 = steady_clock::now();
    double ms = duration<double, std::milli>(t1 - t0).count();
    std::printf("[smoke w] wrote %u x %uB in %.1fms fail=%u\n", count, size, ms, fail);

    delete seg;
    return fail == 0;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: %s r <name> <cap> <total> <timeout_s> | w <name> <cap> <count> <size> <first_seq> [delay_us]\n",
                    argv[0]);
        return 2;
    }

    if (argv[1][0] == 'r' && argc >= 6)
    {
        return reader_mode(argv[2], std::strtoull(argv[3], nullptr, 10),
                           std::strtoul(argv[4], nullptr, 10),
                           std::atoi(argv[5])) ? 0 : 1;
    }
    if (argv[1][0] == 'w' && argc >= 7)
    {
        int delay_us = argc > 7 ? std::atoi(argv[7]) : 0;
        return writer_mode(argv[2],
                           std::strtoul(argv[4], nullptr, 10),
                           std::strtoul(argv[5], nullptr, 10),
                           std::strtoul(argv[6], nullptr, 10),
                           delay_us) ? 0 : 1;
    }
    return 2;
}
