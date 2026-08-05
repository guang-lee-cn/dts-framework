#pragma once

namespace dts {

// console：socket 监听线程（人类 cmd 入口）。AF_UNIX 长连接会话：一行一条命令，
// 响应以 '\x00' 结尾；客户端断开（EOF）结束会话。只投递（console -> control），不执行。
// 低优先级（detsched BKG 域，SCHED_OTHER）。配套客户端 tools/dts-cli.py（login/exec/ps）。
// sock_path：AF_UNIX socket 路径（多实例须派生，见 bootstrap 装配）。
int console_start(const char* sock_path);   // 0 成功
void console_stop();

// control：运维指令执行线程。消费 console 投递命令 -> ctl::Execute。
// 只读查询业务线程（detsched::QueryThreads），不阻塞业务。低优先级（BKG 域）。
int control_start();
void control_stop();

}  // namespace dts
