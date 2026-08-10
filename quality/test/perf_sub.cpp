#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "detmw.h"
#include "dts_def.h"
#include "perf_packet.h"

using namespace dts;

namespace {

struct Record {
    uint32_t seq;
    uint64_t tSend;
    uint64_t tMid;
    uint64_t tRecv;
};

uint64_t NowUs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::mutex g_mu;
std::unordered_map<uint32_t, uint64_t> g_mid;  // seq -> msg2 到达时刻
std::vector<Record> g_recs;
std::unordered_set<uint32_t> g_seenMsg4;  // 去重：静态 EDP 偶发双投递
std::atomic<uint64_t> g_msg2n{0};
std::atomic<uint64_t> g_msg4n{0};
std::atomic<bool> g_done{false};

// msg2（task 段完成标记，fan-out 到 sub）
void OnMsg2(void*, std::unique_ptr<std::vector<uint8_t>> data) {
    const uint8_t* d = data ? data->data() : nullptr;
    if (!d) return;
    uint32_t seq = PerfGetSeq(d);
    if (seq == kPerfDoneSeq) return;
    uint64_t now = NowUs();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_mid[seq] = now;
    }
    g_msg2n.fetch_add(1);
}

// msg4（数据上报，含 t_send 打点）
void OnMsg4(void*, std::unique_ptr<std::vector<uint8_t>> data) {
    const uint8_t* d = data ? data->data() : nullptr;
    if (!d) return;
    uint32_t seq = PerfGetSeq(d);
    if (seq == kPerfDoneSeq) {
        g_done.store(true);
        return;
    }
    uint64_t tRecv = NowUs();
    uint64_t tSend = PerfGetTs(d);
    uint64_t tMid = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_seenMsg4.insert(seq).second) {
            return;  // 静态 EDP 双投递，去重
        }
        auto it = g_mid.find(seq);
        if (it != g_mid.end()) {
            tMid = it->second;
        }
        g_recs.push_back(Record{seq, tSend, tMid, tRecv});
    }
    g_msg4n.fetch_add(1);
}

// msg6（log 线程 pub 能力：log 收到采集即回抛），量 log 段
std::atomic<uint64_t> g_logRecv{0};
std::vector<uint64_t> g_logLat;  // 保护同 g_mu

void OnLog(void*, std::unique_ptr<std::vector<uint8_t>> data) {
    const uint8_t* d = data ? data->data() : nullptr;
    if (!d) return;
    uint32_t seq = PerfGetSeq(d);
    if (seq == kPerfDoneSeq) {
        g_done.store(true);
        return;
    }
    uint64_t tRecv = NowUs();
    uint64_t tSend = PerfGetTs(d);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_logLat.push_back(tRecv - tSend);
    }
    g_logRecv.fetch_add(1);
}

double Pct(const std::vector<uint64_t>& v, double p) {
    if (v.empty()) return 0.0;
    size_t idx = static_cast<size_t>(p * static_cast<double>(v.size() - 1));
    return static_cast<double>(v[idx]);
}

}  // namespace

// perf 压测接收端：订阅 msg2 + msg4，量 task/data 两段耗时 + 丢包，写 CSV
// 用法：perf_sub <perf-sub.json> <out.csv> <expect_count>
int main(int argc, char** argv) {
    if (argc < 4) {
        std::printf("usage: %s <perf-sub.json> <out.csv> <expect_count>\n", argv[0]);
        return 1;
    }
    const uint32_t expect = static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 10));
    const char* out_path = argv[2];

    detmw::Communicator comm(argv[1]);

    if (comm.subscribe(detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_DATA, MSG_ID_DATA_TASK_ACTIVE},
                       OnMsg2, nullptr) != 0 ||
        comm.subscribe(detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_DATA, MSG_ID_REPORT},
                       OnMsg4, nullptr) != 0 ||
        comm.subscribe(detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_LOG, MSG_ID_LOG_REPORT},
                       OnLog, nullptr) != 0) {
        std::printf("[perf-sub] subscribe failed\n");
        return 1;
    }

    std::printf("[perf-sub] waiting for static discovery + DONE... (t=%llu)\n",
                static_cast<unsigned long long>(NowUs()));
    for (int i = 0; i < 150 && !g_done.load(); i++) {  // 15s 超时（丢包时快速返回）
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::printf("[perf-sub] loop end: done=%d (t=%llu), draining 2s for in-flight...\n",
                g_done.load() ? 1 : 0, static_cast<unsigned long long>(NowUs()));
    std::this_thread::sleep_for(std::chrono::seconds(2));  // 排空可靠投递在途包

    // comm RAII 析构销毁 detmw

    std::vector<uint64_t> latTask, latData;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (const auto& r : g_recs) {
            if (r.tMid != 0) {
                latTask.push_back(r.tMid - r.tSend);
                latData.push_back(r.tRecv - r.tMid);
            }
        }
    }
    std::sort(latTask.begin(), latTask.end());
    std::sort(latData.begin(), latData.end());

    uint64_t msg2n = g_msg2n.load();
    uint64_t msg4n = g_msg4n.load();
    uint64_t lossTask = expect > msg2n ? expect - msg2n : 0;
    uint64_t lossData = expect > msg4n ? expect - msg4n : 0;

    // 按结构体字段 int 字节数拆分写 CSV
    FILE* fp = fopen(out_path, "w");
    if (fp != nullptr) {
        std::fprintf(fp, "seq,t_send_us,t_mid_us,t_recv_us,lat_task_us,lat_data_us\n");
        std::lock_guard<std::mutex> lk(g_mu);
        for (const auto& r : g_recs) {
            std::fprintf(fp, "%u,%llu,%llu,%llu,%llu,%llu\n", r.seq,
                         static_cast<unsigned long long>(r.tSend),
                         static_cast<unsigned long long>(r.tMid),
                         static_cast<unsigned long long>(r.tRecv),
                         static_cast<unsigned long long>(r.tMid ? r.tMid - r.tSend : 0),
                         static_cast<unsigned long long>(r.tMid ? r.tRecv - r.tMid : 0));
        }
        std::fclose(fp);
    }

    std::printf("[perf-sub] ================= summary =================\n");
    std::printf("[perf-sub] expect=%u  msg2_recv=%llu  msg4_recv=%llu\n", expect,
                static_cast<unsigned long long>(msg2n),
                static_cast<unsigned long long>(msg4n));
    std::printf("[perf-sub] loss task_hop=%llu  data_hop=%llu\n",
                static_cast<unsigned long long>(lossTask),
                static_cast<unsigned long long>(lossData));
    auto sum = [](const std::vector<uint64_t>& v) {
        uint64_t s = 0;
        for (auto x : v) s += x;
        return s;
    };
    std::printf("[perf-sub] task 段耗时(lat_task): n=%zu min=%llu avg=%.1f p99=%llu max=%llu (us)\n",
                latTask.size(), static_cast<unsigned long long>(latTask.empty() ? 0 : latTask.front()),
                latTask.empty() ? 0.0 : static_cast<double>(sum(latTask)) / latTask.size(),
                static_cast<unsigned long long>(Pct(latTask, 0.99)),
                static_cast<unsigned long long>(latTask.empty() ? 0 : latTask.back()));
    std::printf("[perf-sub] data 段耗时(lat_data): n=%zu min=%llu avg=%.1f p99=%llu max=%llu (us)\n",
                latData.size(), static_cast<unsigned long long>(latData.empty() ? 0 : latData.front()),
                latData.empty() ? 0.0 : static_cast<double>(sum(latData)) / latData.size(),
                static_cast<unsigned long long>(Pct(latData, 0.99)),
                static_cast<unsigned long long>(latData.empty() ? 0 : latData.back()));
    // log 段（log 线程 pub/sub 验证）
    {
        std::lock_guard<std::mutex> lk(g_mu);
        std::sort(g_logLat.begin(), g_logLat.end());
    }
    uint64_t logN = g_logRecv.load();
    std::printf("[perf-sub] log 段收发(log_pub): n=%llu", static_cast<unsigned long long>(logN));
    if (!g_logLat.empty()) {
        std::printf("  min=%llu avg=%.1f p99=%llu max=%llu (us)\n",
                    static_cast<unsigned long long>(g_logLat.front()),
                    static_cast<double>(sum(g_logLat)) / g_logLat.size(),
                    static_cast<unsigned long long>(Pct(g_logLat, 0.99)),
                    static_cast<unsigned long long>(g_logLat.back()));
    } else {
        std::printf("\n");
    }
    std::printf("[perf-sub] file=%s\n", out_path);
    std::printf("[perf-sub] %s\n", (lossTask == 0 && lossData == 0) ? "PASS (no loss)" : "FAIL (loss)");
    return (lossTask == 0 && lossData == 0) ? 0 : 1;
}
