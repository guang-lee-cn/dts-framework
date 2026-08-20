#pragma once

#include <memory>
#include <vector>

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

// DDS 控制通道入口（P1-3，D2/R5）：detmw 接收回调（网管远程命令行）-> control 命令队列。
// D3 第二条传输：与 console 殊途同归，执行仍收敛 control 线程 ctl::Execute；
// 响应由 control 线程执行完后经 DDS 发布（DTS.oam.MSG_ID_OAM_CMD_RESP），调用方不等待。
// 空载荷/超长行丢弃（Warn 日志）；control 未运行/已停时丢弃。
// 返回 0 = 已入队。
int control_submit_dds(std::unique_ptr<std::vector<uint8_t>> line);

}  // namespace dts
