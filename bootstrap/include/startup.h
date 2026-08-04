#pragma once

namespace dts {

// 进程上电（组合根）：装配 detmw + 业务线程 + console/control。
// cfg_path：进程生成配置 JSON（gen_detmw.py 产物）。
// 返回 0 成功；非 0 失败（内部已打 error 日志）。
int StartUp(const char* cfg_path);

// 进程下电：先停运维，再停业务，最后销毁 detmw（StartUp 反序）。
void ShutDown();

}  // namespace dts
