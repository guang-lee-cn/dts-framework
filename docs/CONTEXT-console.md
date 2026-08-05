# DTS console 窗口 · 启动栈备忘录

> ✅ **已完成 2026-08-05**：ctl 命令表 + console/control 线程 + bootstrap 装配 + 交互式 dts CLI 全部落地。
> 登录方式：`python3 tools/dts-cli.py login cpf-dts`（REPL，# 提示符）或 `exec cpf-dts <cmd>` / `ps`。
> 实现记录见 [docs/worklog/2026-08-05.md](worklog/2026-08-05.md)（"追加：console 控制面落地"）。
>
> 用途：本窗口只做一件事——实现 infrastructure 控制面（console / control / CommandExecutor）。
> 输入：本备忘录 + [contracts/infrastructure.md](../contracts/infrastructure.md)（接口为准）+ 现状代码。
> 除接口契约外不依赖总体架构文档。日期：2026-08-05

## 当前任务（一句话）

实现 `dts::ctl` 命令表 + console socket 监听线程 + control 执行线程，接入 bootstrap 生命周期，内置 help / get_threads / set_log_level 三条命令。

## 接口契约（定稿，contracts/infrastructure.md §2/§3 为准）

### 命令表（namespace dts::ctl）

```cpp
struct Command {
    const char* name;                 // "set_log_level" / "get_threads"
    const char* usage;                // 用法提示
    int (*fn)(const std::vector<std::string>& args, std::string& out);  // 0 成功
};
class CommandRegistry {               // 进程内单例
public:
    static CommandRegistry& Instance();
    int  Register(const Command& cmd);            // 重名返回非 0
    const Command* Find(const char* name) const;
    void ForEach(const std::function<void(const Command&)>& fn) const;  // help 遍历
};
int Execute(const std::string& line, std::string& out);  // "name arg1 arg2" -> 查表 -> 执行
```

### 运维线程（namespace dts）

```cpp
// console：socket 监听线程（人类 cmd 入口）。AF_UNIX，行协议：一行一条命令，响应回写。
// 只翻译（投递 control），不执行命令。低优先级（SCHED_OTHER，不绑 RT）。
int console_start(const char* sock_path);   // 0 成功
void console_stop();
// control：运维指令执行线程。消费 console 投递命令 -> ctl::Execute。
// 只读查询业务线程（detsched::QueryThreads），不阻塞业务。低优先级。
int control_start();
void control_stop();
```

### 内置命令（首批）

- `help`：`CommandRegistry::ForEach` 列全部命令 + usage
- `get_threads`：`detsched::QueryThreads(ThreadInfo*, cap)` 全进程线程注册表，格式化输出
- `set_log_level <trace|debug|info|warn|error>`：`dts::log::SetLevel`（D6 线程安全，不加锁）

### console → control 传递

线程安全命令队列（单生产者单消费者即可，命令量低频）。

### bootstrap 装配位置

`Process` 增加 console/control 起停。`Run` 装配顺序 = 日志 → detmw → 业务线程 → console/control；
下电反序 = console/control 先停 → 业务线程 → comm → 日志。

## 项目现状（相关，均已落地）

- 日志：`dts::log::SetLevel/GetLevel`（infrastructure/include/log.h）
- 线程：`detsched::CreateThread / DeclareDomain / QueryThreads`（detsched/include/thread_api.h）
- 组合根：bootstrap/src/run.cpp `Process{StopSignal, comm, Worker×3, subs}`，`Run/Stop` 已落地
- 依赖方向：detmw ← infrastructure ← contexts ← bootstrap；detmw ← infrastructure

## 流程（接口已定死，走实现节奏）

1. 先写接口头 → 单测（CommandRegistry/Execute 纯函数）→ 实现 → 自检
2. 内置命令三条
3. bootstrap 装配 + 手动 socket 验证
4. 质量：编译零 warning（-Werror）+ ctest 3/3 + 手动验证

## 跨窗口约束

- 接收统一入口 OnRouteMsg → mailbox（本窗口不碰）
- 日志一律 `dts::log::Xxx`，不直接调 spdlog
- 函数入参 ≤5、C++17、const T&

## 验证命令

```bash
cmake --build build && ctest --test-dir build
# 手动：AF_UNIX socket 写 "help\n" 得命令列表；"get_threads\n" 得线程表；"set_log_level debug\n" 生效
```

## 完成标志

- 命令表可注册/查询/执行（单测过）
- socket 登录发命令得响应；control 线程执行；bootstrap 起停正常
- ctest 3/3 过 + 手动验证通过
- 收口：写 docs/worklog/ 当日 + 刷新本栈
