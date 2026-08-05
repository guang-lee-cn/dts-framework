#include <chrono>
#include <cstdio>
#include <thread>

#include "run.h"
#include "kafka_mock.h"
#include "scheduler_mock.h"
#include "spa_mock.h"

using namespace dts;

// 集成测试：Run 装配 + mock 对端（调度/SPA/网管），运行数秒验证端到端 + 优雅退出
int main() {
    std::printf("=== integration test start ===\n");

    // Run 常驻阻塞，放后台线程；主线程 mock 对端 + 数秒后 Stop() 优雅退出
    int runRc = 0;
    std::thread app([&] { runRc = Run(DTS_TEST_CFG); });

    KafkaMock kafka;
    kafka.Start();
    SchedulerMock scheduler;
    scheduler.Start();
    SpaMock spa;
    spa.Start();

    std::this_thread::sleep_for(std::chrono::seconds(3));

    spa.Stop();  // 先停 agent 读线程（日志调用方），Run 的 Shutdown 销毁线程池前必须停干净
    Stop();      // 停 Run（内部 p.Stop + dts::log::Shutdown）
    app.join();

    std::printf("=== integration test done (run_rc=%d) ===\n", runRc);
    return runRc == 0 ? 0 : 1;
}
