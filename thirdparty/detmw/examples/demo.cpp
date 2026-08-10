#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <unistd.h>

#include "detmw.h"

using namespace std::chrono_literals;
using namespace detmw;

void on_recv(void* ctx, std::unique_ptr<std::vector<uint8_t>> data) {
    const uint8_t* p = data ? data->data() : nullptr;
    uint32_t len = data ? static_cast<uint32_t>(data->size()) : 0;
    (void)ctx;
    std::printf("[sub] recv %u bytes: %.*s\n", len, static_cast<int>(len),
                reinterpret_cast<const char*>(p));
    std::fflush(stdout);
}

// 用法：detmw_demo <pub|sub> <生成配置.json>
// 配置须声明 DTS_data_100 端点（role 与角色一致），见 examples/demo/{pub,sub}.json
int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: %s <pub|sub> <config.json>\n", argv[0]);
        return 1;
    }
    bool is_pub = std::strcmp(argv[1], "pub") == 0;
    Communicator comm(argv[2]);

    if (is_pub) {
        // 等 RTPS 发现握手完成（首条不丢验证：发现后可靠投递全收）
        std::this_thread::sleep_for(2s);
        const endpoint dst{"DTS", "data", 100};
        for (int i = 0; i < 5; i++) {
            char msg[64];
            int n = std::snprintf(msg, sizeof(msg), "hello-%d", i);
            comm.publish_external(dst, reinterpret_cast<const uint8_t*>(msg),
                                  static_cast<uint32_t>(n));
            std::this_thread::sleep_for(500ms);
        }
        std::this_thread::sleep_for(1s);  // 等可靠投递 ack 完成再退出
    } else {
        const endpoint src{"DTS", "data", 100};
        comm.subscribe(src, on_recv, nullptr);
        std::this_thread::sleep_for(12s);
    }

    return 0;
}
