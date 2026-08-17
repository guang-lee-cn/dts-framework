#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "detmw.h"
#include "dts_def.h"
#include "perf_packet.h"

using namespace dts;

namespace {

// 单跳量测：收到回显/回抛即记 (seq, lat = t_recv - t_send)，DONE 包（kPerfDoneSeq）置完成
struct HopStats {
    std::mutex mu;
    std::vector<uint64_t> lats;
    std::atomic<uint64_t> n{0};
    std::atomic<bool> done{false};
};

HopStats g_hop;

// 回显回调节点：task 段 msg8（TASK_RESPONSE）/ log 段 msg6（LOG_REPORT）
void OnHop(void*, std::unique_ptr<std::vector<uint8_t>> data) {
    const uint8_t* d = data ? data->data() : nullptr;
    if (!d) {
        return;
    }
    const uint32_t seq = PerfGetSeq(d);
    if (seq == kPerfDoneSeq) {
        g_hop.done.store(true);
        return;
    }
    const uint64_t tSend = PerfGetTs(d);
    const uint64_t tRecv = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    {
        std::lock_guard<std::mutex> lk(g_hop.mu);
        g_hop.lats.push_back(tRecv - tSend);
    }
    g_hop.n.fetch_add(1);
}

double Pct(const std::vector<uint64_t>& v, double p) {
    if (v.empty()) return 0.0;
    const size_t idx = static_cast<size_t>(p * static_cast<double>(v.size() - 1));
    return static_cast<double>(v[idx]);
}

}  // namespace

// perf 压测接收端：量单跳往返耗时 + 丢包，写 CSV。
// 用法：perf_sub <perf-sub.json> <out.csv> <expect_count> [mode]
//   mode: task（默认，msg7→task→msg8 外部回显；task→data msg2 已进程内直通，外部不可观测）
//         log （msg5→log→msg6 回抛）
//   data 段（msg3→data→msg4 上报）由 run_slevel.sh（tune 链路，含握手）覆盖
int main(int argc, char** argv) {
    if (argc < 4) {
        std::printf("usage: %s <perf-sub.json> <out.csv> <expect_count> [mode=task|log]\n", argv[0]);
        return 1;
    }
    const uint32_t expect = static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 10));
    const char* out_path = argv[2];
    const bool toLog = (argc >= 5) && (std::strcmp(argv[4], "log") == 0);

    detmw::Communicator comm(argv[1]);

    // task 段：收 msg8（task OnTaskConfig 回显）；log 段：收 msg6（OnCollect 回抛）
    const auto ep = toLog ? detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_LOG, MSG_ID_LOG_REPORT}
                          : detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_TASK, MSG_ID_TASK_RESPONSE};
    if (comm.subscribe(ep, OnHop, nullptr) != 0) {
        std::printf("[perf-sub] subscribe failed (mode=%s)\n", toLog ? "log" : "task");
        return 1;
    }

    std::printf("[perf-sub] waiting for discovery + DONE... (mode=%s)\n", toLog ? "log" : "task");
    for (int i = 0; i < 150 && !g_hop.done.load(); i++) {  // 15s 超时（丢包时快速返回）
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::printf("[perf-sub] loop end: done=%d, draining 2s for in-flight...\n",
                g_hop.done.load() ? 1 : 0);
    std::this_thread::sleep_for(std::chrono::seconds(2));  // 排空可靠投递在途包

    std::vector<uint64_t> lats;
    uint64_t sum = 0;
    {
        std::lock_guard<std::mutex> lk(g_hop.mu);
        lats = g_hop.lats;
        for (uint64_t v : lats) {
            sum += v;
        }
    }
    std::sort(lats.begin(), lats.end());

    const uint64_t recvN = g_hop.n.load();
    const uint64_t loss = expect > recvN ? expect - recvN : 0;

    FILE* fp = fopen(out_path, "w");
    if (fp != nullptr) {
        std::fprintf(fp, "idx,lat_us\n");
        std::lock_guard<std::mutex> lk(g_hop.mu);
        for (size_t i = 0; i < g_hop.lats.size(); ++i) {
            std::fprintf(fp, "%zu,%llu\n", i, static_cast<unsigned long long>(g_hop.lats[i]));
        }
        std::fclose(fp);
    }

    std::printf("[perf-sub] ================= summary =================\n");
    std::printf("[perf-sub] mode=%s expect=%u  recv=%llu  loss=%llu\n",
                toLog ? "log" : "task", expect, static_cast<unsigned long long>(recvN),
                static_cast<unsigned long long>(loss));
    std::printf("[perf-sub] 往返耗时(lat): n=%zu min=%llu avg=%.1f p99=%llu max=%llu (us)\n",
                lats.size(), static_cast<unsigned long long>(lats.empty() ? 0 : lats.front()),
                lats.empty() ? 0.0 : static_cast<double>(sum) / lats.size(),
                static_cast<unsigned long long>(Pct(lats, 0.99)),
                static_cast<unsigned long long>(lats.empty() ? 0 : lats.back()));
    std::printf("[perf-sub] file=%s\n", out_path);
    std::printf("[perf-sub] %s\n", loss == 0 ? "PASS (no loss)" : "FAIL (loss)");
    return loss == 0 ? 0 : 1;
}
