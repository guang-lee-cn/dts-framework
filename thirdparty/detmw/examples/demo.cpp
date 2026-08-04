#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <unistd.h>

#include "detmw.h"

using namespace std::chrono_literals;

void on_recv(void* ctx, const uint8_t* data, uint32_t len) {
    (void)ctx;
    std::printf("[sub] recv %u bytes: %.*s\n", len, static_cast<int>(len),
                reinterpret_cast<const char*>(data));
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
    detmw_handle* h = detmw_init(argv[2]);
    if (!h) return 1;

    if (is_pub) {
        // 等 RTPS 发现握手完成（首条不丢验证：发现后可靠投递全收）
        std::this_thread::sleep_for(2s);
        for (int i = 0; i < 5; i++) {
            char msg[64];
            int n = std::snprintf(msg, sizeof(msg), "hello-%d", i);
            detmw_publish(h, "DTS", "data", 100, reinterpret_cast<const uint8_t*>(msg),
                          static_cast<uint32_t>(n));
            std::this_thread::sleep_for(500ms);
        }
        std::this_thread::sleep_for(1s);  // 等可靠投递 ack 完成再退出
    } else {
        detmw_subscribe(h, "DTS", "data", 100, on_recv, nullptr);
        std::this_thread::sleep_for(12s);
    }

    detmw_destroy(h);
    return 0;
}
