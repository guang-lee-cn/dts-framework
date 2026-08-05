# DTS data 窗口 · 启动栈备忘录

> 用途：本窗口只做一件事——data 子系统 ISO 重设计（数据工厂机制 + 业务数据模型，内部架构可重写）。
> 输入：本备忘录（预留接口 + 契约）→ ISO 流程。除接口契约外不依赖总体架构文档。
> 日期：2026-08-05

## 当前任务（一句话）

用 ISO 流程重新设计 data 线程子系统：数据工厂机制（Feed 匹配 / 缓存 / 上报协议）+ 业务数据模型扩展。内部架构允许重写，本备忘录只定接口契约与边界不变式。

## 接口契约（预留，dts-architecture.md §5.2 摘要；contracts/contexts.md 为准）

### 消息契约（msgId 在 (sessionType, sessionInst) 内唯一）

```
MSG_ID_DATA_TASK_ACTIVE  0x0002  task -> data（进程内，跟踪 taskId↔dataId）
MSG_ID_AGENT_DATA        0x0003  agent -> data（SPA 帧，BigFrameHeader / DataType）
MSG_ID_REPORT            0x0004  data -> 网管（1s 定时上报）
```

### data 线程链路（现状，机制可重写）

```
OnRouteMsg → data mailbox → ThreadRun → DataEntry → DataMsgHandlerDispatch
   ├─ TASK_ACTIVE → TdMap.Track(taskId, dataIds)
   └─ AGENT_DATA  → DataFactory.Feed(帧) → 匹配 DataType→dataId → DataConstruct(Extra→Hton→Report) → ReportCache
DataMsgHandlerTimerReport()（1s）→ ReportCache.TakeAll() → publish_external(REPORT)
```

### 预留接口（骨架现状，重设计可改签名但契约语义不变）

- 入：`DataEntry(status, msgId, msg, len)`（线程入口）、`DataMsgHandlerDispatch(...)`（按 msgId 直分）、`DataMsgHandlerTimerReport()`（1s 定时）
- 领域：`TdMap`（taskId↔dataId 多对多）、`ReportCache`（每 task 缓存 + TakeAll）、`DataFactory`（RegisterProcessor + Feed）、`DataConstruct`（Extra/Hton/Report 模板方法）、`DataDomainInit`
- 出：唯一 `publish_external(REPORT)`，报文 `ReportHeader{taskId, timestampMs, seq, payloadLen}` + payload

### 边界不变式（ISO 重设计时必须保持）

1. data 线程独占所有 domain 对象，无锁（pub-sub 保障串行）
2. domain 零技术依赖：不 include detmw/detsched/infrastructure，收发走 `dts::port::MwPort`（contracts/contexts.md §2）
3. 上报 1s 批量，唯一输出 `publish_external(REPORT)`
4. 接收统一入口 OnRouteMsg → mailbox（不得旁路）

## 项目现状（相关）

- data context 三层：`interface`(dts_data_entry) / `application`(data_msg_handler) / `domain`(现状：tdmap / report_cache / data_factory / data_construct / processor×2)
- 现状 domain 是演示级：cell_prb / ue_bler 两个加工类 + `DTS_DATA_SLOW` 耗时模拟
- 数据来源：agent（SPA 联编）经 MSG_ID_AGENT_DATA；task 经 MSG_ID_DATA_TASK_ACTIVE
- 依赖方向：detmw ← infrastructure ← contexts ← bootstrap

## 流程（ISO，重构项目先执行"重构前置"）

1. 加载 /home/guang/code/prompt/ 对应 prompt（iso.md → strategy.md；重构项目先跑"重构前置"）
2. Insight：数据工厂要解决什么业务问题、当前做法痛点、量化数据
3. Strategy：数据模型（dataId / DataType / 处理器矩阵）+ 工厂机制（Feed 匹配 / 缓存 / 上报协议）+ 盲点检查
4. Operation：接口契约更新 → 分层实现 → 集成
5. 质量：编译零 warning + ctest 3/3 + 数据链路实测（integration 覆盖 data）

## 验证命令

```bash
cmake --build build && ctest --test-dir build
# data 链路：integration 测试覆盖 TASK_ACTIVE / AGENT_DATA / REPORT 端到端
```

## 完成标志

- ISO 三阶段输出（Insight / Strategy / Operation）落盘 docs/design/ + docs/iso-output.md
- data 子系统设计定稿（含接口契约更新）+ 实现落地
- ctest 3/3 过 + data 链路实测无丢包（对比现状基线）
- 收口：写 docs/worklog/ 当日 + 刷新本栈
