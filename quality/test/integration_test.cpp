#include <chrono>
#include <cstdio>
#include <thread>

#include "dts_startup.h"
#include "kafka_mock.h"
#include "scheduler_mock.h"
#include "spa_mock.h"

using namespace dts;

// 集成测试：上电 + mock 对端（调度/SPA/网管），运行数秒验证端到端 + 优雅退出
int main() {
    std::printf("=== integration test start ===\n");

    // 单进程上电：用 cpf 生成配置（含静态发现），mock 对端为空（stub），验证启动/优雅退出
    dts_startup(DTS_TEST_CFG);

    KafkaMock kafka;
    kafka.Start();
    SchedulerMock scheduler;
    scheduler.Start();
    SpaMock spa;
    spa.Start();

    std::this_thread::sleep_for(std::chrono::seconds(3));

    dts_startup_shutdown();
    std::printf("=== integration test done ===\n");
    return 0;
}
