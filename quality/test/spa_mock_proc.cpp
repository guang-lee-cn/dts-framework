// spaMock 独立进程（DDS 调优链路）：握手 + 持续上报 rawData 给 dts data 线程。
// 流程：等静态发现 → pub msg2 TaskRequest{taskId=1,status=1} 握手 → 持续 pub msg3 rawData
// raw 帧布局（对齐 data 工厂 ExtractKey / CCellData01::Extra）：
//   [u16 dataType=0(CELL)][u32 cellId][u32 cpId][int f0][int f1]
// 用法：spa_mock_proc <tune-spa.json> [rounds] [interval_us]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "detmw.h"
#include "dts_def.h"

using namespace dts;

namespace {

struct RawFrame {
    uint16_t dataType;  // 0 = CELL
    uint32_t cellId;
    uint32_t cpId;
    int32_t f0;
    int32_t f1;
};

uint64_t NowUs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s <tune-spa.json> [rounds] [interval_us]\n", argv[0]);
        return 1;
    }
    const int rounds = (argc >= 3) ? std::atoi(argv[2]) : 1000;
    const int intervalUs = (argc >= 4) ? std::atoi(argv[3]) : 1000;  // 默认 1ms 一帧

    detmw::Communicator comm(argv[1]);
    if (!comm.good()) {
        std::printf("[spa-mock] communicator init failed\n");
        return 1;
    }

    std::printf("[spa-mock] waiting for static discovery...\n");
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // 握手：通知 data 当前任务（taskId=1, status=1 成功）
    TaskRequest req{1, 1};
    comm.publish_external(detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_DATA, MSG_ID_DATA_TASK_ACTIVE},
                          reinterpret_cast<const uint8_t*>(&req), sizeof(req));
    std::printf("[spa-mock] handshake sent (taskId=1), starting %d frames...\n", rounds);

    const uint64_t t0 = NowUs();
    uint32_t fail = 0;
    for (int i = 0; i < rounds; ++i) {
        RawFrame frame{};
        frame.dataType = 0;          // CELL
        frame.cellId = 1 + (i % 10);
        frame.cpId = 0;
        frame.f0 = i;
        frame.f1 = i * 2;
        int rc = comm.publish_external(
            detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_DATA, MSG_ID_AGENT_DATA},
            reinterpret_cast<const uint8_t*>(&frame), sizeof(frame));
        if (rc != 0) {
            ++fail;
        }
        if (intervalUs > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(intervalUs));
        }
    }
    const uint64_t dur = NowUs() - t0;
    std::this_thread::sleep_for(std::chrono::seconds(1));  // 排空可靠投递

    const double durS = static_cast<double>(dur) / 1e6;
    std::printf("[spa-mock] sent %d frames in %.3fs -> %.0f msg/s, fail=%u\n",
                rounds, durS, rounds / durS, fail);
    return 0;
}
