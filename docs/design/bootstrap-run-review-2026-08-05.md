# bootstrap run.cpp 代码检视报告

- 日期：2026-08-05
- 范围：`bootstrap/include/run.h`、`bootstrap/src/run.cpp`，关联核对 detmw/detsched/log/mailbox 实现与生成路由头
- 结论：组合根职责单一、下电反序清晰，核心问题在「装配失败契约未兑现」「detmw 实例双所有权」「订阅回调生命周期无显式同步」

---

## 高优先级

### R1. Run() 返回码契约与实际脱节

- **位置**：run.h:6-7（注释承诺「非 0 装配失败」）vs run.cpp:156-181
- **现象**：真实装配失败全部静默。
  - detmw.cpp:45-129：cfg 解析失败、transport 起不来时只 `log::Error` 并 return，不抛异常、无失败态；
  - `subscribe` 失败只 log（run.cpp:139），subs 为空进程照样跑。
  - 最终 cfg 文件不存在 / participant 建不起来时，Run 照常 `WaitStop()` 返回 0——进程以「成功」退出一个通信全断的空壳。
- **根因**：Communicator 构造无失败态暴露；装配失败不冒泡、不汇总。
- **建议**：
  - `Communicator` 构造暴露失败态（bool 或抛异常）；
  - `subscribe` 失败累计计数，`Process::Start()` 返回装配结果；
  - 任一失败则 Run 走下电并返回非 0。
- **待决策**：装配失败该「失败即停」还是「部分降级继续跑」？

### R2. 订阅回调生命周期无显式同步

- **位置**：run.cpp:71-78（OnRouteMsg 上下文为 `subs` 内裸 `Subscription*`）、run.cpp:107-115（下电顺序 `comm.reset()` → `subs.clear()`）
- **现象**：`on_data_available` 在 FastDDS 接收线程同步回调（detmw_fastdds.cpp:84-103）。下电时 `comm.reset()` 销毁 listener 与回调上下文，但无「回调已退出」同步点。若 `delete_participant` 期间接收线程仍在回调栈，则 `subs.clear()` 销毁的 `Subscription` 正被栈上 `OnRouteMsg` 访问（use-after-free）。
- **建议**（二选一）：
  - **验证并固化**：确认 FastDDS `delete_participant` 会 join 接收线程、在途回调必已结束，把该假设写进 `Process::Stop()` 注释；
  - **更稳**：`Subscription` 改 `shared_ptr`，detmw 注册上下文与 `subs` 各持一份，回调迟到也存活。

---

## 中优先级

### R3. detmw 实例双所有权

- **位置**：run.cpp:166-167（`comm` unique_ptr + `DtsMwSet(comm.get())`）vs dts_mw.h:8
- **现象**：`comm` 独占所有权，同时 `DtsMwSet` 向全局暴露裸指针。`Stop()` 必须手工保持 `DtsMwSet(nullptr)` → `comm.reset()` 顺序，异常路径/重入会留悬挂全局指针。detmw 同时被 Process 独占又被全局共享。
- **建议**：`DtsMw` 返回弱引用语义，或让 `Communicator` 自身即全局单例，Process 只装配不复制所有权。

### R4. SessionKey 死代码 + 与 detmw::endpoint 重复定义

- **位置**：dts_def.h:26-46
- **现象**：全仓库 grep `SessionKey/SessionKeyHash` 零使用。寻址键概念已有 `detmw::endpoint`（含 ToString、hash），此处又复制一套同构类型和 hash。两份实现必漂移。
- **建议**：删除 dts_def.h 这份，统一收口到 detmw。

### R5. 三 Worker 硬编码重复

- **位置**：run.cpp:85-87（task/data/log 三成员）、98-104（Start 三连）、108-111（Stop 三连）
- **现象**：已到「三行重复才提取」阈值，新增业务线程需复制粘贴。
- **建议**：抽 `WorkerSpec{name, entry, prio, routes}` 数组，表驱动装配。

### R6. Worker.name 死字段

- **位置**：run.cpp:119（只写不读，已 grep 确认）
- **现象**：线程名已有两份（`ctx.m_name`、detsched handle 内部），第三份必漂移。
- **建议**：删除。

### R7. publish_internal 意图与实现脱节

- **位置**：detmw.h:52（注释「mailbox 直通免序列化」）vs detmw.cpp:152-160（实现为 `transport->Send`，走 DDS 回环）
- **现象**：进程内线程间通信实际绕 DDS 再经订阅回调投递。detmw.cpp 自己注释了是过渡态，但 run.cpp 的 Subscription/Process 架构注释（run.cpp:64-68, 80-81）读起来像直通已落地，叙事不一致。
- **建议**：run.cpp 补现状说明；或从接口上先拿掉「直通」承诺，待 mailbox 直通落地再恢复。

---

## 低优先级

### R8. 装配失败反馈路径缺失

- **位置**：run.cpp:96（`DeclareDomain` 返回值不检）、121（`CreateThread` 返回 handle 不检）
- **现象**：与 R1 同根因，组合根的失败不冒泡。
- **建议**：`CreateThread` 返回 nullptr 时 log 并计入装配失败。

### R9. Process::RequestStop/WaitStop 纯转发

- **位置**：run.cpp:90-92
- **建议**：2 行包装，可直用 `stop` 成员。

### R10. Run 单次性未声明

- **位置**：run.h
- **现象**：`State()` 单例 + `log::Init` 幂等 / `Shutdown` 不可逆（log.h:29），Run 天然只能执行一次。
- **建议**：run.h 注释写明「进程生命周期内仅一次」。

---

## 建议执行顺序

1. R1 + R8：装配失败契约（失败冒泡 + 返回码兑现）——影响编排方判断，优先；
2. R2：订阅回调生命周期同步——潜在 UAF，需验证或改 shared_ptr；
3. R3：detmw 双所有权收口；
4. R5 + R6：三 Worker 表驱动 + 删死字段；
5. R4：删 SessionKey 死代码；
6. R7：叙事统一（文档先行）；
7. R9 + R10：小清理。

## 遗留问题

- 进程内直通落地方向：订阅回调已把跨线程投递集中到 mailbox，把「发往本进程 endpoint」短路为直接投递目标 mailbox、免 DDS 往返，可同时缩小 R2 风险面。是否作为下一步？
