# DTS data 子系统 Bench 报告 + 项目进展

> 日期：2026-08-10 · 环境：WSL2 16 核（taskset 0-7，留 8-15 给 IDE）/ 16G 内存（MEM_LIMIT 2G 守护）
> 数据来源：detmw reader/writer 硬计数（recv_total/write_total）+ Process wall-clock + web 多 reader

## 1. 测试配置

| 项 | 值 |
|---|---|
| raw 帧 | 32K（`[u16 dataType][u32 cellId][u32 cpId][Σ 500 切片]`）|
| dataId 切片 | 500 个，4-200B 随机（Σ=32758，固定 seed 42，data/spa 共享 layout）|
| 上报帧 | ReportHeader 20B + 500×(SubHeader 4B + data) ≈ 32K（合并 1 publish/帧）|
| 进程链路 | spaMock → dts(data) → webMock（跨进程 DDS，domain 5）|
| QoS | reliable + KEEP_LAST depth（DETMW_QOS_* env 可调）|

## 2. 各层吞吐（raw 32K，核心数据）

| 层 | SHM | UDP | 瓶颈？ |
|---|---|---|---|
| spa 发 | ~7000 raw/s（220 MB/s）| ~7000 raw/s | DDS 发送 |
| **data reader 收**（reader→mailbox）| **980 raw/s** | **948 raw/s** | ⚠️ **data 收瓶颈** |
| **data publish**（writer）| 980/s（=reader）| 865-948/s | data domain 非瓶颈（全 publish）|
| web 收（N=8 reader fan-out）| 1466 fps | 1667 fps | mock 已优化（不属 dts）|

**关键结论**：data publish = reader 收（每收一 raw 全 publish），**data domain 处理（切分 500 dataId + 合并 publish）不是瓶颈**。瓶颈在 reader 收（FastDDS 反序列化）。

## 3. 周期上报极限（场景 1：稳态不丢包）

不丢条件：spa ≤ data reader 收。

| 项 | 值 |
|---|---|
| **最小周期** | **≥ 1.05ms（~950 Hz）** |
| **载荷** | 32K/帧（500 dataId × 4-200B）|
| **不丢带宽** | **30 MB/s**（950 × 32K）|
| QoS | reliable + KEEP_LAST depth≥1 |
| data CPU 占用 | 非瓶颈（处理余量大）|

## 4. 突发余量（场景 2：稳态余量吸收突发）

| 项 | 值 |
|---|---|
| 周期稳态 | ≤ 950 Hz（data reader 跟上）|
| 突发吸收 | reader history depth（KEEP_LAST，缓冲突发帧）|
| 端到端不丢 | 突发 + 周期 ≤ 950/s（reader 收极限）|
| QoS | reliable + KEEP_LAST **depth = 突发峰值**（depth=1000 缓冲 1000×32K）|

## 5. 瓶颈定位（代码分层）

| 层级 | 接口 | 位置 | 瓶颈 | 速率 |
|---|---|---|---|---|
| **detmw 传输** | **FastDDS `BytesType::deserialize`**（take_next_sample 反序列化 memcpy 32K）| `detmw_fastdds.cpp` | ⚠️ **核心瓶颈** | ~950 raw/s |
| infrastructure | ~~`mailbox.Send` 32K 拷贝~~（已零拷贝）| `mailbox.h` | 已消除（+15%）| — |
| data domain | Process（切分 + 合并 publish）| `data_factory_v2.cpp` | 非瓶颈 | ≥950/s |
| data domain | ReportAggregator（1 publish/帧）| `report_aggregator.cpp` | 非瓶颈 | — |

**核心瓶颈 = FastDDS 反序列化**（DDS 协议限：BytesType=`sequence<octet>` 不支持零拷贝，每帧 memcpy 32K）。data domain / mailbox 已优化到非瓶颈。

## 6. 优化历程（已做）

| 优化 | 目标 | 效果 | 状态 |
|---|---|---|---|
| ReportAggregator 合并上报（500 dataId → 1 publish/帧）| publish 次数 | dataId 吞吐 4.6x | ✅ |
| AcquireBlock 1 次/帧（500 dataId 共享 1 block）| hash 探测 | 消费 +2x | ✅ |
| **mailbox 零拷贝**（unique_ptr 移所有权）| reader→mailbox 32K 拷贝 | **+15%**（830→950/s）| ✅ |
| web mock 多 reader（N=8 fan-out）| web 收 | mock 收全（不属 dts）| ✅ |
| CPU 守护（taskset 0-7 + MEM_LIMIT）| 防系统卡死 | 测试稳定 | ✅ |

**优化到顶**：data domain / mailbox 已无瓶颈，剩余瓶颈是 FastDDS 反序列化（DDS 协议硬限）。

## 7. QoS 扫描（depth 对 data 收无影响）

| QoS | data reader 收 | 说明 |
|---|---|---|
| depth=10 | ~950/s | depth 不影响收速率（只缓冲/吸突发）|
| depth=1000 | ~950/s | 同 |
| depth=10000 | ~950/s | 同 |
| best_effort | 同 reliable | ACK 不影响 reader 收（deserialize 限）|

QoS 调参对 data 收速率无效——瓶颈在反序列化 CPU，不在 DDS 可靠性/历史深度。QoS depth 作用是**突发吸收**（reader history 容量）。

---

## 8. 项目进展（重构整体）

| 阶段 | 内容 | 状态 |
|---|---|---|
| detmw v2 | Communicator + TransportInterface + endpoint 统一寻址 | ✅ |
| bootstrap | Run/Stop + Process + StopSignal + 装配失败契约 | ✅ |
| 日志 | dts::log 门面 + TsRotatingSink（console+file）| ✅ |
| console 控制面 | CommandExecutor + console/control 线程 + dts CLI（login REPL）| ✅ |
| **data 数据模型** | 500 dataId（4-200B 随机）+ CacheHead + ReportHeader | ✅ |
| **data 内存层** | DataMemManager（静态池，hash 槽位 + TTL）| ✅ |
| **data 工厂** | Extractor + Registry + 合并 ReportAggregator | ✅ |
| **data 任务库** | TdMapV2（握手 currentTask）| ✅ |
| **DDS 调优链路** | spa/web mock + QoS 参数化（DETMW_QOS_* env）| ✅ |
| **性能优化** | 零拷贝 mailbox + 合并 report + AcquireBlock 优化 | ✅ |
| **性能基线** | data 收 950/s（30 MB/s），瓶颈定位 FastDDS deserialize | ✅ |

**当前状态**：data 子系统 DDS 调优链路打通，性能基线建立（30 MB/s，瓶颈定位），优化到 data domain 无瓶颈。

## 9. 接下来的数据方向

| 方向 | 内容 | 收益 |
|---|---|---|
| **raw 序列化突破**（突破 30 MB/s）| plain struct 固定大小 + DataSharing loan 零拷贝；或 raw 分片（32K 拆小帧绕 deserialize 开销）| data 收 > 950/s |
| **web 落地** | webserver_mock 接 kafka（当前 CSV 占位）| 真实落盘验证 |
| **DDS QoS 深化** | transport SHM/UDP 调参、reliable times（heartbeat/acknack）、UDP buffer | 突发/延迟优化 |
| **gtest/lcov/静态检查** | data domain 单测 + 覆盖率 + cppcheck | 质量保障 |
| **detsched/contexts 逐层重写** | task/log context 重构（data 已先做）| 全量重构收尾 |
| **真实 dataId 业务模型** | 替换 mock 切片为真实业务字段（CellSch/UeSch 等）| 业务落地 |

## 10. 关键数据速查

| 指标 | 值 |
|---|---|
| data 收极限 | **950 raw/s**（FastDDS deserialize 限）|
| data 收带宽 | **30 MB/s** |
| data publish | 全（domain 非瓶颈）|
| web 收（mock N=8）| 1466-1667 fps（fan-out 收全）|
| 端到端不丢周期 | ≥ 1.05ms |
| 突发吸收 | reader history depth |
| 核心瓶颈 | FastDDS BytesType::deserialize（32K memcpy）|
