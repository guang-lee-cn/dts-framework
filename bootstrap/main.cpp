#include <csignal>
#include <cstdio>

#include "run.h"

using namespace dts;

namespace {
void OnSignal(int) {
    Stop();
}
}  // namespace

// 进程入口：信号处理 + 常驻运行，SIGINT/SIGTERM（Ctrl+C / kill）退出
// 用法：dts <进程生成配置.json>（gen_detmw.py 产物，如 build/generated/detmw/cpf-dts/cpf-dts.json）
int main(int argc, char* argv[]) {
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    if (Run(argc > 1 ? argv[1] : nullptr) != 0) {
        // CLI 提示不依赖 dts::log：Run 返回时日志线程池已回收（5b44aca），此处访问会崩
        std::printf("=== dts run failed ===\n");
        return 1;
    }
    std::printf("=== dts done ===\n");
    return 0;
}
