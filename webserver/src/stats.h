// 观测面统计：单写者（kafka 消费线程）+ 多读者（HTTP/SSE），轻量互斥足够。
// 1s 桶 × 60 滑窗，供 fps/MB·s 曲线与 /api/stats 快照。
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

#include "report_frame.h"

namespace web {

class Stats {
public:
    void OnReport(const ReportView& v) {
        std::lock_guard<std::mutex> lk(m_mu);
        m_totalFrames++;
        m_totalBytes += v.frameLen;
        if (m_hasLastSeq && v.seq != m_lastSeq + 1) {
            m_gaps++;  // seq 不连续 = 沉淀链路有丢（DDS 可靠 QoS 下应为 0）
        }
        m_lastSeq = v.seq;
        m_hasLastSeq = true;
        m_lastTaskId = v.taskId;
        AddBucket(v.frameLen);
    }

    // JSON 快照（/api/stats 与 1Hz SSE 复用）
    std::string SnapshotJson() {
        std::lock_guard<std::mutex> lk(m_mu);
        const int64_t nowSec = NowSec();
        RollTo(nowSec);
        double fps = 0;
        double mbps = 0;
        for (int i = 1; i <= kWindow; ++i) {  // 最近 5s 均值（跳过当前未满桶）
            const SecBucket& b = m_buckets[(nowSec - i + kWindow * 4) % kWindow];
            if (b.sec > 0 && nowSec - b.sec <= kWindow) {
                fps += b.frames;
                mbps += b.bytes;
            }
        }
        fps /= 5.0;
        mbps = mbps / 5.0 / 1024.0 / 1024.0;  // 字节/5s → MB/s
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "{\"total_frames\":%llu,\"total_bytes\":%llu,\"gaps\":%llu,"
                      "\"last_seq\":%u,\"last_task\":%u,\"fps_5s\":%.1f,\"mbps_5s\":%.2f}",
                      static_cast<unsigned long long>(m_totalFrames),
                      static_cast<unsigned long long>(m_totalBytes),
                      static_cast<unsigned long long>(m_gaps), m_lastSeq, m_lastTaskId, fps, mbps);
        return buf;
    }

private:
    static constexpr int kWindow = 60;

    struct SecBucket {
        int64_t sec = 0;
        uint64_t frames = 0;
        uint64_t bytes = 0;
    };

    static int64_t NowSec() {
        return std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
    }

    void RollTo(int64_t nowSec) {
        const size_t idx = static_cast<size_t>(nowSec % kWindow);
        if (m_buckets[idx].sec != nowSec) {
            m_buckets[idx] = SecBucket{nowSec, 0, 0};  // 复用槽位：新秒清零
        }
    }

    void AddBucket(uint32_t bytes) {
        const int64_t nowSec = NowSec();
        RollTo(nowSec);
        SecBucket& b = m_buckets[static_cast<size_t>(nowSec % kWindow)];
        b.frames++;
        b.bytes += bytes;
    }

    std::mutex m_mu;
    uint64_t m_totalFrames = 0;
    uint64_t m_totalBytes = 0;
    uint64_t m_gaps = 0;
    uint32_t m_lastSeq = 0;
    uint32_t m_lastTaskId = 0;
    bool m_hasLastSeq = false;
    SecBucket m_buckets[kWindow]{};
};

}  // namespace web
