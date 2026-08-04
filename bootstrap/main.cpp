#include <csignal>
#include <cstdio>

#include <unistd.h>

#include "dts_startup.h"

using namespace dts;

namespace {
volatile sig_atomic_t g_running = 1;

void OnSignal(int) {
    g_running = 0;
}
}  // namespace

// 进程入口：上电 + 常驻，SIGINT/SIGTERM（Ctrl+C / kill）退出
// 用法：dts <进程生成配置.json>（gen_detmw.py 产物，如 build/generated/detmw/cpf-dts/cpf-dts.json）
int main(int argc, char* argv[]) {
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    if (dts_startup(argc > 1 ? argv[1] : nullptr) != 0) {
        std::printf("=== dts startup failed ===\n");
        return 1;
    }

    std::printf("=== dts running, Ctrl+C / kill to exit ===\n");
    while (g_running) {
        pause();  // 挂起等信号，信号处理置 g_running=0 后退出
    }

    dts_startup_shutdown();
    std::printf("=== done ===\n");
    return 0;
}
