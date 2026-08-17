#include <chrono>
#include <cstdio>
#include <thread>

#include "run.h"
#include "kafka_mock.h"
#include "scheduler_mock.h"
#include "spa_mock.h"

using namespace dts;

// 集成测试：Run 装配 + mock 对端（调度/SPA/网管），运行数秒验证端到端 + 优雅退出。
// 端到端链路（本进程 DDS 回环 + 进程内直通）：
//   scheduler_mock(msg1) -> task -> publish_internal(msg2 直通) -> data 握手
//   spa_mock(握手 + raw msg3) -> data(切分+合并上报) -> msg4 -> kafka_mock 计数
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

    std::this_thread::sleep_for(std::chrono::seconds(5));

    spa.Stop();  // 先停 agent 读线程（日志调用方），Run 的 Shutdown 销毁线程池前必须停干净
    Stop();      // 停 Run（内部 p.Stop + dts::log::Shutdown）
    app.join();

    // 端到端断言：data 链路通（至少 1 帧上报被网管计数；scheduler/spa 握手幂等重发抗发现时序）
    const uint64_t reports = KafkaMock::ReceivedCount();
    std::printf("=== integration test done (run_rc=%d, reports=%llu) ===\n", runRc,
                static_cast<unsigned long long>(reports));
    return runRc == 0 && reports > 0 ? 0 : 1;
}
