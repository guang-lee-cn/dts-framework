// bench_nfoam：task 双向收发基准（A）
// nfoam 发 32K JSON 配置(msg7) → task 简析 JSON + 响应(msg8) → nfoam 量测往返 + 丢包临界点
// 可发 >64 条（任务数 ≤64，变更消息数不限）
// 用法：bench_nfoam <cfg.json> <count> [size] [delay_us]
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#include "detmw.h"
#include "dts_def.h"
#include "perf_packet.h"

using namespace dts;

namespace {

std::mutex g_mu;
std::vector<uint64_t> g_rt;  // 往返耗时 us
std::unordered_set<uint32_t> g_seen;  // 去重：单订阅者 topic 静态 EDP 偶发双投
std::atomic<uint64_t> g_resp{0};
std::atomic<uint64_t> g_dup{0};
std::atomic<bool> g_done{false};

uint64_t NowUs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// 收到 task 响应(msg8)
void OnResp(void*, const uint8_t* data, uint32_t) {
    uint64_t seq = PerfJsonInt(data, "\"seq\"", 0);
    if (seq == kPerfDoneSeq) {
        g_done.store(true);
        return;
    }
    uint64_t tSend = PerfJsonInt(data, "\"t_send\"", 0);
    uint64_t now = NowUs();
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_seen.insert(static_cast<uint32_t>(seq)).second) {
        g_dup.fetch_add(1);
        return;  // 双投，去重
    }
    if (tSend != 0) {
        g_rt.push_back(now - tSend);
    }
    g_resp.fetch_add(1);
}

double Pct(const std::vector<uint64_t>& v, double p) {
    if (v.empty()) return 0.0;
    return static_cast<double>(v[static_cast<size_t>(p * static_cast<double>(v.size() - 1))]);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: %s <cfg.json> <count> [size] [delay_us]\n", argv[0]);
        return 1;
    }
    const uint32_t count = static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 10));
    const size_t size = (argc >= 4) ? std::strtoul(argv[3], nullptr, 10) : kPerfPacketSize;
    const uint32_t delayUs = (argc >= 5) ? std::strtoul(argv[4], nullptr, 10) : 0;

    detmw_handle* h = detmw_init(argv[1]);
    if (h == nullptr) return 1;

    if (detmw_subscribe(h, SESSION_TYPE_DTS, SESSION_INST_TASK, MSG_ID_TASK_RESPONSE, OnResp,
                        nullptr) != 0) {
        std::printf("[nfoam] subscribe response failed\n");
        detmw_destroy(h);
        return 1;
    }

    std::printf("[nfoam] waiting 3s for discovery...\n");
    std::this_thread::sleep_for(std::chrono::seconds(3));

    std::vector<uint8_t> pkt(size);
    uint64_t t0 = NowUs();
    for (uint32_t i = 0; i < count; i++) {
        PerfBuildConfigJson(pkt.data(), size, i & 0xFFFF, i, NowUs());
        detmw_publish(h, SESSION_TYPE_DTS, SESSION_INST_TASK, MSG_ID_TASK_CONFIG, pkt.data(),
                      static_cast<uint32_t>(size));
        if (delayUs > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(delayUs));
        }
    }
    // DONE 标记（同样走 task 响应）
    PerfBuildConfigJson(pkt.data(), size, 0xFFFF, kPerfDoneSeq, NowUs());
    detmw_publish(h, SESSION_TYPE_DTS, SESSION_INST_TASK, MSG_ID_TASK_CONFIG, pkt.data(),
                  static_cast<uint32_t>(size));
    uint64_t t1 = NowUs();

    for (int i = 0; i < 150 && !g_done.load(); i++) {  // 15s 超时
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));  // 排空在途

    detmw_destroy(h);

    double sendSecs = static_cast<double>(t1 - t0) / 1e6;
    uint64_t resp = g_resp.load();
    uint64_t loss = count > resp ? count - resp : 0;
    std::sort(g_rt.begin(), g_rt.end());
    auto sum = [](const std::vector<uint64_t>& v) {
        uint64_t s = 0;
        for (auto x : v) s += x;
        return s;
    };

    std::printf("[nfoam] ===== task 双向收发基准 =====\n");
    std::printf("[nfoam] send=%u size=%zuB delay=%uus  send_time=%.1fms -> %.0f msg/s\n", count,
                size, delayUs, sendSecs, static_cast<double>(count) / sendSecs);
    std::printf("[nfoam] response=%llu/%u  loss=%llu (丢包临界点)  dup(双投)=%llu\n",
                static_cast<unsigned long long>(resp), count,
                static_cast<unsigned long long>(loss),
                static_cast<unsigned long long>(g_dup.load()));
    std::printf("[nfoam] round_trip(us): n=%zu min=%llu avg=%.1f p99=%llu max=%llu\n",
                g_rt.size(),
                static_cast<unsigned long long>(g_rt.empty() ? 0 : g_rt.front()),
                g_rt.empty() ? 0.0 : static_cast<double>(sum(g_rt)) / g_rt.size(),
                static_cast<unsigned long long>(Pct(g_rt, 0.99)),
                static_cast<unsigned long long>(g_rt.empty() ? 0 : g_rt.back()));
    return 0;
}
