#pragma once

namespace dts {

// 进程运行（组合根）：装配 detmw + 业务线程 + console/control，常驻阻塞运行，
// 直到 Stop() 被调用后下电退出。返回 0 正常退出；非 0 装配失败。
// cfg_path：进程生成配置 JSON（gen_detmw.py 产物）。
int Run(const char* cfg_path);

// 停止运行：置停止标志，Run() 的内部循环退出后执行下电。信号处理回调调用。
void Stop();

}  // namespace dts
