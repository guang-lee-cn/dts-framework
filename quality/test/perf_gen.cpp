#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "detmw.h"
#include "dts_def.h"
#include "perf_packet.h"

using namespace dts;

namespace {
uint64_t NowUs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
}  // namespace

// perf 压测发送端：以最大速率连发 count 个包（task 段 = msg7 配置变更；data 段 = msg3 raw），
// 末尾 DONE 标记包。task 段回显 msg8、log 段回抛 msg6、data 段上报 msg4 由 perf_sub 量测。
// 用法：perf_gen <perf-gen.json> <count> [size] [wait_s] [delay_us]
int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: %s <perf-gen.json> <count> [size] [wait_s] [delay_us]\n", argv[0]);
        return 1;
    }
    const uint32_t count = static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 10));
    const size_t pktSize =
        (argc >= 4) ? std::max<size_t>(kPerfTsOff + 8, std::strtoul(argv[3], nullptr, 10))
                    : kPerfPacketSize;
    const int waitSecs = (argc >= 5) ? std::atoi(argv[4]) : 3;
    const uint32_t delayUs = (argc >= 6) ? std::strtoul(argv[5], nullptr, 10) : 0;
    const bool toLog = (argc >= 7) && (std::strcmp(argv[6], "log") == 0);   // log 段：发 msg5
    const bool toData = (argc >= 7) && (std::strcmp(argv[6], "data") == 0);  // data 段：发 msg3
    // 目标会话/消息：task 段 (task,7：配置变更 → task 回显 msg8) / log 段 (log,5) / data 段 (data,3)
    // 注：task→data 的 msg2 已改进程内 mailbox 直通（外部不可观测），外部 task 基准走 msg7/msg8
    const char* sessionInst = toData ? SESSION_INST_DATA : (toLog ? SESSION_INST_LOG : SESSION_INST_TASK);
    const uint32_t msgId = toData ? MSG_ID_AGENT_DATA : (toLog ? MSG_ID_LOG_COLLECT : MSG_ID_TASK_CONFIG);
    // datamix 标志在 argv[7]（mode 用 argv[6]，如 "data datamix 10 20"）
    const bool mix = (argc >= 8) && (std::strcmp(argv[7], "datamix") == 0);
    const uint32_t burstEvery = (argc >= 9) ? std::strtoul(argv[8], nullptr, 10) : 10;
    const uint32_t burstLen = (argc >= 10) ? std::strtoul(argv[9], nullptr, 10) : 20;

    detmw::Communicator comm(argv[1]);

    std::printf("[perf-gen] waiting %ds for static discovery...\n", waitSecs);
    std::this_thread::sleep_for(std::chrono::seconds(waitSecs));

    std::vector<uint8_t> pkt(pktSize);
    PerfInit(pkt.data(), pktSize);
    auto* ta = reinterpret_cast<TaskActiveMsg*>(pkt.data());

    uint64_t t0 = NowUs();
    uint32_t fail = 0;
    int lastRc = 0;
    uint32_t sent = 0;
    auto sendOne = [&](uint32_t seq, bool done) {
        ta->taskId = static_cast<uint16_t>(seq & 0xFFFF);
        PerfSetSeq(pkt.data(), done ? kPerfDoneSeq : seq);
        PerfSetTs(pkt.data(), NowUs());
        int rc = comm.publish_external(detmw::endpoint{SESSION_TYPE_DTS, sessionInst, msgId},
                                       pkt.data(), static_cast<uint32_t>(pktSize));
        lastRc = rc;
        if (rc != 0) fail++;
        sent++;
    };
    for (uint32_t i = 0; i < count; i++) {
        sendOne(i, false);
        if (delayUs > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(delayUs));
        }
        // 周期流中夹杂突发（紧突发，无间隔）
        if (mix && (i + 1) % burstEvery == 0) {
            for (uint32_t b = 0; b < burstLen; b++) {
                sendOne(count + i * 1000 + b, false);
            }
        }
    }
    // DONE 标记包（同样走全链路）
    ta->taskId = 0xFFFF;
    PerfSetSeq(pkt.data(), kPerfDoneSeq);
    PerfSetTs(pkt.data(), NowUs());
    int doneRc = comm.publish_external(detmw::endpoint{SESSION_TYPE_DTS, sessionInst, msgId},
                                       pkt.data(), static_cast<uint32_t>(pktSize));
    uint64_t t1 = NowUs();

    double secs = static_cast<double>(t1 - t0) / 1e6;
    std::printf("[perf-gen] sent %u x %zuB packets in %.3f s -> %.0f msg/s, %.1f MB/s "
                "(publish_fail=%u lastRc=%d doneRc=%d)\n",
                sent, pktSize, secs, static_cast<double>(sent) / secs,
                static_cast<double>(sent) * pktSize / 1e6 / secs, fail, lastRc, doneRc);

    std::this_thread::sleep_for(std::chrono::seconds(2));  // 等可靠投递排空
    return 0;  // comm RAII 析构销毁 detmw
}
