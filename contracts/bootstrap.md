# 接口契约：bootstrap（组合根）

> 版本：v1 · 2026-08-04 · 模式：已落地（StartUp/ShutDown）+ console/control 装配待补
> 依据：docs/design/sub-bootstrap.md

## 1. 进程上电/下电（已实现）

```cpp
namespace dts {

// 进程上电（组合根）：装配 detmw + 业务线程 + console/control。
// cfg_path：进程生成配置 JSON。
// 返回 0 成功；非 0 失败（内部已打 error 日志）。
int StartUp(const char* cfg_path);

// 进程下电：反序释放。停运维 -> 停业务 -> 销毁 detmw -> 停日志线程池。
void ShutDown();

}  // namespace dts
```

**装配顺序**（Composition Root）：
```
1. 日志 InitLog（spdlog 异步 + TsRotatingSink）
2. 通信 detmw_init(cfg)
3. 声明业务域 + 注册静态路由
4. 创建业务线程 task/data/log（detsched）
5. 运维 control + console（待补，阶段 2）
```

**下电反序**：
```
1. 停业务线程（RequestStop + DestroyThread）
2. DtsMwSet(nullptr) + mw.reset()
3. routes.clear()
4. spdlog::shutdown()
```

## 2. console/control 装配（阶段 2 待补）

```cpp
// StartUp 第 5 步内部：装配运维线程
// console：socket 监听线程（人类 cmd 入口），低优先级（detsched Bkg 域）
// control：运维指令执行线程（CommandExecutor），低优先级
// socket 路径：编译宏 CPF/DPF/BPF/BB 派生默认 + 配置覆盖（D4）
```

**约束**：
- 业务线程先起、运维后起；下电先停运维
- console/control 均为 SCHED_OTHER 低优先级，不绑 RT（D5）
- socket 路径：`/run/dts/<instance>/console.sock`，编译宏派生 + 配置覆盖

## 3. 生命周期所有权

- 业务线程：`Worker{ctx, h}` 聚合，bootstrap 持有
- console/control：bootstrap 持有句柄，ShutDown 反序释放
- detmw：unique_ptr + deleter
- 订阅 RouteCtx：unique_ptr 持有，订阅失败回滚

## 4. 配置 JSON 契约

bootstrap 读取顶层：`detmw`（现有）+ `log`（新增，infrastructure.md §5）+ `console.sock`（新增，D4）。

## 5. 版本管理

- StartUp/ShutDown 为进程唯一入口，禁止破坏性变更
- 新增装配步骤向后追加（不动既有 4 步）

## 6. 物理约束

- startup.h 公有 API ≤ 2（StartUp/ShutDown）
- main.cpp 只做：信号处理 + 调 StartUp/ShutDown + pause 循环，不承载业务
