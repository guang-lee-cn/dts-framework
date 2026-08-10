// webserverMock 独立进程（DDS 调优链路负载）：多 reader 并行收 data 上报(msg4)，
// 让 mock 不成瓶颈（单 reader take 限 861/s，N reader fan-out 合并覆盖全）。
//   count 模式（默认）：N reader 各 atomic++，总 = Σ（量 data publish 不丢极限）
//   csv 模式（传 out.csv）：落 CSV
// 用法：webserver_mock <tune-web.json> [run_s] [out.csv] [N_readers]
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include "detmw.h"
#include "dts_def.h"

using namespace dts;

namespace {

constexpr int kMaxReaders = 16;
std::atomic<uint64_t> g_counts[kMaxReaders];
std::atomic<uint64_t> g_bytes{0};
std::mutex g_mu;
FILE* g_fp = nullptr;
bool g_csv = false;

void OnReport(void* ctx, const uint8_t* /*data*/, uint32_t len) {
    const int idx = static_cast<int>(reinterpret_cast<intptr_t>(ctx));
    if (idx >= 0 && idx < kMaxReaders) {
        g_counts[idx].fetch_add(1);
    }
    g_bytes.fetch_add(len);
    if (!g_csv || len < 24) {
        return;
    }
    // csv 模式：落一条（首 reader 记，避免重复）
    if (idx == 0) {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_fp) {
            std::fprintf(g_fp, "%llu,%u\n",
                         static_cast<unsigned long long>(
                             std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch()).count()),
                         len);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s <tune-web.json> [run_s] [out.csv] [N_readers]\n", argv[0]);
        return 1;
    }
    const int runSec = (argc >= 3) ? std::atoi(argv[2]) : 8;
    const int nReaders = (argc >= 5) ? std::min(std::atoi(argv[4]), kMaxReaders) : 8;
    if (nReaders < 1) {
        return 1;
    }

    detmw::Communicator comm(argv[1]);
    if (!comm.good()) {
        std::printf("[web-mock] communicator init failed\n");
        return 1;
    }

    if (argc >= 4 && argv[3][0] != '\0') {
        g_csv = true;
        g_fp = std::fopen(argv[3], "w");
        if (g_fp == nullptr) {
            std::printf("[web-mock] open %s failed\n", argv[3]);
            return 1;
        }
        std::fprintf(g_fp, "t_recv_us,len\n");
    }

    // N reader 同 endpoint（DDS fan-out，各收全；合并覆盖全，量 data publish 极限）
    const auto ep = detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_DATA, MSG_ID_REPORT};
    for (int i = 0; i < nReaders; ++i) {
        if (comm.subscribe(ep, OnReport, reinterpret_cast<void*>(static_cast<intptr_t>(i))) != 0) {
            std::printf("[web-mock] subscribe reader %d failed\n", i);
            return 1;
        }
    }

    const char* mode = g_csv ? "csv" : "count";
    std::printf("[web-mock] waiting for discovery, %d readers, %ds (%s mode)...\n",
                nReaders, runSec, mode);
    const auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(runSec));
    const double durS = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    if (g_csv) {
        std::lock_guard<std::mutex> lk(g_mu);
        std::fflush(g_fp);
        std::fclose(g_fp);
        g_fp = nullptr;
    }
    uint64_t total = 0;
    for (int i = 0; i < nReaders; ++i) {
        total += g_counts[i].load();
    }
    const uint64_t bytes = g_bytes.load();
    std::printf("[web-mock] readers=%d total_recv=%llu in %.3fs -> %.0f fps, %.2f MB/s (mode=%s)\n",
                nReaders, static_cast<unsigned long long>(total), durS, total / durS,
                static_cast<double>(bytes) / durS / (1024 * 1024), mode);
    return 0;
}
