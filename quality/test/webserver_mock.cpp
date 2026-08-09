// webserverMock 独立进程（DDS 调优链路）：收 dts data 上报(msg4 REPORT)，落 CSV（kafka TODO）。
// 量接收吞吐/报文数；CSV：t_recv_us, taskId, dataId, payloadLen
// 用法：webserver_mock <tune-web.json> <out.csv> [run_s]
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
std::atomic<uint64_t> g_count{0};
std::atomic<uint64_t> g_bytes{0};

void OnReport(void*, const uint8_t* data, uint32_t len) {
    if (data == nullptr || len < sizeof(WebReportHeader)) {
        return;
    }
    g_count.fetch_add(1);
    g_bytes.fetch_add(len);

    const auto* hdr = reinterpret_cast<const WebReportHeader*>(data);
    const uint64_t now = NowUs();
    // 解析 payload 内所有子头（n×(SubHeader+data)）
    uint32_t off = sizeof(WebReportHeader);
    while (off + sizeof(WebSubHeader) <= len && off < sizeof(WebReportHeader) + hdr->payloadLen) {
        const auto* sh = reinterpret_cast<const WebSubHeader*>(data + off);
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (g_fp) {
                std::fprintf(g_fp, "%llu,%u,%u,%u\n",
                             static_cast<unsigned long long>(now), hdr->taskId, sh->dataId,
                             hdr->payloadLen);
            }
        }
        off += sizeof(WebSubHeader) + sh->len;
        break;  // 每帧记一条（第一个 dataId 代表），避免 CSV 膨胀
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: %s <tune-web.json> <out.csv> [run_s]\n", argv[0]);
        return 1;
    }
    const char* out = argv[2];
    const int runSec = (argc >= 4) ? std::atoi(argv[3]) : 8;

    detmw::Communicator comm(argv[1]);
    if (!comm.good()) {
        std::printf("[web-mock] communicator init failed\n");
        return 1;
    }

    g_fp = std::fopen(out, "w");
    if (g_fp == nullptr) {
        std::printf("[web-mock] open %s failed\n", out);
        return 1;
    }
    std::fprintf(g_fp, "t_recv_us,taskId,dataId,payloadLen\n");

    if (comm.subscribe(detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_DATA, MSG_ID_REPORT},
                       OnReport, nullptr) != 0) {
        std::printf("[web-mock] subscribe failed\n");
        std::fclose(g_fp);
        return 1;
    }

    std::printf("[web-mock] waiting for discovery, collecting %ds...\n", runSec);
    const uint64_t t0 = NowUs();
    std::this_thread::sleep_for(std::chrono::seconds(runSec));
    const double durS = static_cast<double>(NowUs() - t0) / 1e6;

    {
        std::lock_guard<std::mutex> lk(g_mu);
        std::fflush(g_fp);
        std::fclose(g_fp);
        g_fp = nullptr;
    }
    const uint64_t cnt = g_count.load();
    const uint64_t bytes = g_bytes.load();
    std::printf("[web-mock] report=%llu frames in %.3fs -> %.0f fps, %.2f MB/s -> %s\n",
                static_cast<unsigned long long>(cnt), durS, cnt / durS,
                static_cast<double>(bytes) / durS / (1024 * 1024), out);
    return 0;
}
