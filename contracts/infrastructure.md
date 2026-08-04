# 接口契约：infrastructure（基础设施）

> 版本：v1 · 2026-08-04 · 模式：重构（新增 console/control/日志模块）
> 依据：docs/design/dts-strategy.md 6.1 图 + D5/D6/D9

## 1. 模块划分

```
infrastructure/
  ├── mailbox.h / mw.h / thread_ctx.h / defs.h   ← 现有（文件名去 dts_ 前缀，符号在 dts:: 内）
  ├── console/                                  ← 新增：socket 监听（人类 cmd 入口）
  ├── control/                                  ← 新增：运维指令执行（CommandExecutor）
  └── logging/                                  ← 新增：日志门面 + TsRotatingSink
```

## 2. 控制面：CommandExecutor（D5/D6，核心复用点）

**职责**：唯一命令执行处。console 人类入口与 detmw 控制消息共用。

```cpp
namespace dts::ctl {

// 命令：注册表条目，新增/删除/修改命令 = 改表
struct Command {
    const char* name;                 // "set_log_level" / "get_threads"
    const char* usage;                // 用法提示
    int (*fn)(const std::vector<std::string>& args, std::string& out);  // 执行
};

// 命令注册表（进程内单例）：集中登记，供 console/control 共用
class CommandRegistry {
public:
    static CommandRegistry& Instance();

    // 登记/查询命令；注册失败返回非 0（重名）
    int Register(const Command& cmd);
    const Command* Find(const char* name) const;

    // 遍历（console help 用）
    void ForEach(const std::function<void(const Command&)>& fn) const;
};

// 执行器：解析 "name arg1 arg2" -> 查表 -> 执行。返回 0 成功；非 0 失败（out 带错误）
int Execute(const std::string& line, std::string& out);

}  // namespace dts::ctl
```

**约束**：
- 命令执行只发生在 control 线程（D5）；console 投递、control 执行
- 查询类命令只读；配置类命令（日志级别）用 spdlog 线程安全接口，不加锁（D6）
- 命令表驱动：新增命令 = 表加一行 + 一个函数，不碰调度/传输

## 3. 运维线程（6.1 图 CON / CTRL）

```cpp
namespace dts {

// console：socket 监听线程（人类 cmd 入口），阻塞 I/O，不执行命令
// 收到命令 -> 投递 control 线程
int console_start(const char* sock_path);   // 创建监听线程；返回 0 成功
void console_stop();

// control：运维指令执行线程（CommandExecutor），只读查询各线程，不阻塞业务
int control_start();                        // 创建执行线程；返回 0 成功
void control_stop();

}  // namespace dts
```

**约束**：
- 两线程均为低优先级（detsched Bkg 域，SCHED_OTHER，不绑 RT）
- console 线程只翻译命令（socket -> ctl::Execute），不执行
- control 线程执行命令；业务线程消息处理不加运维分支

## 4. 只读查询接口（control 查业务线程状态，D5）

```cpp
// 复用 detsched 查询（thread_api.h），control 线程只读调用：
// detsched::QueryThreads(ThreadInfo*, size_t) -> 全进程线程注册表
// detsched::DumpThreadInfo() -> 打印
```

**约束**：control 线程经 detsched 注册表查询，不直接触碰业务线程内部状态。

## 4.5 寻址键统一（D10）

**SessionKey 删除**，统一用 `detmw_endpoint`（detmw.h，C++ 结构体，==/hash/ToString 内置）。
- 不再维护两套寻址键（SessionKey C++ / endpoint C）
- 上层代码直接用 `detmw_endpoint`，路由表/回调注册同类型
- 哈希：`detmw_endpoint_hash`

## 5. 日志模块（D6/D9）

```cpp
namespace dts::log {

// 日志门面：按当前线程名路由到对应 logger。业务代码统一调用，无感。
void Log(Level lvl, const char* fmt, ...);

// 配置（来自配置文件 log 段）
struct Config {
    size_t poolSize = 65536;        // 异步队列条数
    size_t poolThreads = 1;         // 后台消费线程数
    Level level = Level::INFO;
    bool console = true;            // 终端输出
    struct File {
        bool enable = true;
        std::string dir;            // "/var/log/dts"
        std::string namePattern;    // "dts_{}-%Y%m%d%H%M%S"  {} = 线程名
        size_t maxSizeMb = 5;       // 单文件上限
        size_t maxTotalMb = 5120;   // 总量上限（满态删一增一）
    } file;
};

// 初始化：按配置建每线程 logger + TsRotatingSink
int Init(const Config& cfg);
void Shutdown();                    // spdlog::shutdown（停线程池，刷空队列）

// 运行时配置（console 指令入口）：改日志级别
void SetLevel(Level lvl);
Level GetLevel();

}  // namespace dts::log
```

**自定义 sink 职责**（TsRotatingSink，继承 base_sink）：
1. 文件名：`<dir>/<namePattern 展开线程名+时间戳>.log`
2. 单文件超 `maxSizeMb` → 切分新时间戳文件
3. 总量超 `maxTotalMb` → 删除最旧 1 个（满态删一增一）
4. 写入 msg（spdlog 已格式化）

**约束**：
- 日志格式/异步队列/线程池由 spdlog 原生负责，sink 只接管文件三件事
- 每线程一个 logger（dts_data/dts_task/dts_log），共享 1 个线程池
- 业务热路径不打日志（聚合统计，data 现状）

## 6. 版本管理

- 新增模块（console/control/logging）不破坏现有基础设施 API
- 现有文件名去前缀（dts_thread.h -> thread_ctx.h 等）在各模块重构时同步改，符号始终在 `dts::` 内

## 7. 物理约束

- 每个公有类：公有方法 ≤ 9、成员变量 ≤ 5（超出拆正交子接口）
- CommandRegistry：3 方法（Register/Find/ForEach）+ 1 内部实例 = 达标
- ctl::Execute：1 方法
