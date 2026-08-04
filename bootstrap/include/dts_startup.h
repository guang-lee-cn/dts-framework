#pragma once

namespace dts {

// 进程上电：申请 task/data/log 线程 + 各线程内存空间 + detmw 配置加载 + 静态路由注册
// cfg_path：进程生成配置 JSON（gen_detmw.py 产物，detmw_init 加载）
int dts_startup(const char* cfg_path);
void dts_startup_shutdown();

}  // namespace dts
