// webserverMock 独立进程（DDS 调优链路）：收 dts data 上报(msg4 REPORT)。
// 量通道极限：默认只计数（report 数/字节），可选落 CSV。
//   count 模式（默认）：OnReport 只 g_count++/g_bytes+=len，量 data→DDS→web 纯通道极限
//   csv 模式（传 out.csv）：每帧落 CSV，量 CSV 落地极限
// 用法：webserver_mock <tune-web.json> [run_s] [out.csv]
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#include "detmw.h"
#include "dts_def.h"

using namespace dts;

namespace {

uint64_t NowUs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// 上报报文头（与 data domain ReportHeader 对齐，独立进程复制定义避免引 domain 头）
struct WebReportHeader {
    uint32_t taskId;
    uint64_t timestampMs;
    uint32_t seq;
    uint32_t payloadLen;
};
struct WebSubHeader {
    uint16_t dataId;
    uint16_t len;
};

std::mutex g_mu;
FILE* g_fp = nullptr;
bool g_csv = false;
std::atomic<uint64_t> g_count{0};
std::atomic<uint64_t> g_bytes{0};

void OnReport(void*, const uint8_t* data, uint32_t len) {
    if (data == nullptr || len < sizeof(WebReportHeader)) {
        return;
    }
    g_count.fetch_add(1);
    g_bytes.fetch_add(len);

    if (!g_csv) {
        return;  // count 模式：到此为止，量纯通道
    }
    const auto* hdr = reinterpret_cast<const WebReportHeader*>(data);
    const uint64_t now = NowUs();
    if (len >= sizeof(WebReportHeader) + sizeof(WebSubHeader)) {
        const auto* sh = reinterpret_cast<const WebSubHeader*>(data + sizeof(WebReportHeader));
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_fp) {
            std::fprintf(g_fp, "%llu,%u,%u,%u\n",
                         static_cast<unsigned long long>(now), hdr->taskId, sh->dataId,
                         hdr->payloadLen);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s <tune-web.json> [run_s] [out.csv]\n", argv[0]);
        return 1;
    }
    const int runSec = (argc >= 3) ? std::atoi(argv[2]) : 8;

    detmw::Communicator comm(argv[1]);
    if (!comm.good()) {
        std::printf("[web-mock] communicator init failed\n");
        return 1;
    }

    if (argc >= 4) {
        g_csv = true;
        g_fp = std::fopen(argv[3], "w");
        if (g_fp == nullptr) {
            std::printf("[web-mock] open %s failed\n", argv[3]);
            return 1;
        }
        std::fprintf(g_fp, "t_recv_us,taskId,dataId,payloadLen\n");
    }

    if (comm.subscribe(detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_DATA, MSG_ID_REPORT},
                       OnReport, nullptr) != 0) {
        std::printf("[web-mock] subscribe failed\n");
        if (g_fp) std::fclose(g_fp);
        return 1;
    }

    const char* mode = g_csv ? "csv" : "count";
    std::printf("[web-mock] waiting for discovery, collecting %ds (%s mode)...\n", runSec, mode);
    const uint64_t t0 = NowUs();
    std::this_thread::sleep_for(std::chrono::seconds(runSec));
    const double durS = static_cast<double>(NowUs() - t0) / 1e6;

    if (g_csv) {
        std::lock_guard<std::mutex> lk(g_mu);
        std::fflush(g_fp);
        std::fclose(g_fp);
        g_fp = nullptr;
    }
    const uint64_t cnt = g_count.load();
    const uint64_t bytes = g_bytes.load();
    std::printf("[web-mock] report=%llu frames in %.3fs -> %.0f fps, %.2f MB/s (mode=%s)\n",
                static_cast<unsigned long long>(cnt), durS, cnt / durS,
                static_cast<double>(bytes) / durS / (1024 * 1024), mode);
    return 0;
}
