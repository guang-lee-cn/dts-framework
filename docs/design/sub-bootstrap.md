# 子方案：bootstrap（组合根）重构

> 阶段：重构第一阶段（bootstrap 前置）· 2026-08-04
> 原则：不动分层结构/依赖方向/运行时架构，重写装配 API + 内部实现
> 参考：[dts-strategy.md](dts-strategy.md) §6.1

## 1. 现状问题

`src/dts_startup.cpp`（127 行）单函数顺序装配，问题：

| # | 问题 | 影响 |
|---|------|------|
| P1 | 日志/detmw/声明域/路由/线程创建全在一个函数里 | 无扩展面，加线程要改函数体 |
| P2 | 业务线程（task/data/log）硬编码 | 加 console/control 线程无处挂 |
| P3 | `RouteCtx` 用 `new` 且"进程常驻不回收" | 生命周期不明，RAII 缺失（违反 C++17 RAII） |
| P4 | 全局静态 `g_taskCtx` 等 + 裸 `detmw_handle*` | 状态裸露，shutdown 手动倒序 |
| P5 | 无 console / control 装配 | R4/R5 需求缺口 |

## 2. 目标 API

保持 `dts_startup.h` 的**入口形状**（main 调用不变），函数式 + 对偶命名（namespace 已带 dts，函数名不带前缀；startup/shutdown 互为反义，无需 init/deinit 后缀）：

```cpp
#pragma once

namespace dts {

// 进程上电（组合根）：装配 detmw + 业务线程 + console/control。
// cfg_path：进程生成配置 JSON（gen_detmw.py 产物）。
// 返回 0 成功；非 0 失败（内部已打 error 日志）。
int StartUp(const char* cfg_path);

// 进程下电：先停运维，再停业务，最后销毁 detmw（StartUp 反序）。
void ShutDown();
}
```

> `main.cpp` 保持信号循环：`dts_startup` → `StartUp`，`dts_startup_shutdown` → `ShutDown`。

## 3. 装配顺序（Composition Root 四步）

```
1. 日志     InitLog()                    → spdlog 异步（现状保留）
2. 通信     detmw_init(cfg)              → 建 participant + 静态路由注册
3. 线程     创建 task/data/log（业务）     → detsched::CreateThread（现状保留）
4. 运维     创建 control（执行）+ console（socket 监听）→ 新增装配
```

装配原则：**业务先起，运维后起；下电反序（先停运维，再停业务，最后销毁 detmw）**。

## 4. 新增装配：console + control

```cpp
// infrastructure/console/  —— 归属 infrastructure（见 dts-strategy.md 6.1）
// console：socket 监听线程（人类 cmd 入口），阻塞 I/O，不执行命令
// control：运维指令执行线程（CommandExecutor），只读查询各线程，不阻塞业务
```

- console 线程：低优先级域（`SchedPrio::Bkg`，不绑 RT），`accept()` 阻塞
- control 线程：同低优先级，收 detmw 控制消息 + console 投递 → 执行
- 命令执行只发生在 control 线程；业务线程消息处理**不加运维分支**（D5）

## 5. 生命周期修复

- `RouteCtx`：改为 `std::unique_ptr` + 显式持有（进程常驻，但所有权明确，析构清理）
- 线程句柄：`vector<unique_ptr>` 统一持有，`ShutDown` 统一 `DestroyThread`
- detmw：`unique_ptr + deleter` RAII 包装
- 进程常驻状态由匿名 namespace 静态持有（函数式需跨 `StartUp`/`ShutDown`），但内部全部 RAII 管理，`ShutDown` 只做倒序释放

## 6. 验收标准

| 项 | 标准 |
|---|---|
| 编译 | 零 warning（`-Wall -Wextra`） |
| 行为不回归 | `cmake --build build && ctest` 全过（integration/cross_process/s_level_perf） |
| 可审计 | 装配 4 步清晰可读，无 AI 坏味道（无多余注释/无裸 new） |
| console 骨架 | 能 `nc -U <sock>` 连接；`help` 返回命令列表（本期最小：查询类） |
| 独立提交 | bootstrap 重构单独 commit，可回滚 |

## 7. 参考项目

- Asio（分层切法：io_context/executor/operation → 组合根/线程/执行分离）
- BS::thread_pool（短小可读 → 组合根装配的简洁性）
