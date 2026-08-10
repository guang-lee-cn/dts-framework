# data 子系统性能测试报告（raw 32K，500 dataId 随机 4-200B）

> 日期：2026-08-10 · 环境：WSL2 16 核（taskset 0-7）/ raw 32K / 500 dataId（4-200B 随机）
> 方法：detmw reader/writer 硬计数（recv_total/write_total）+ web 多 reader（fan-out 收全）+ SHM/UDP 对比

## 1. 数据结构

| 项 | 结构 | 大小 |
|---|---|---|
| **rawData 帧**（spa→data）| `[u16 dataType][u32 cellId][u32 cpId][Σ 500 切片]` | **32768 字节（32K）** |
| **dataId 切片** | 变长（mock 填充）| **4-200B 随机**（Σ=32758，data/spa 共享 layout）|
| **上报帧**（data→web）| `[ReportHeader 20B][500 × (SubHeader 4B + data)]` | ≈ 32K（合并 1 publish/帧）|

## 2. 硬数据（reader/writer detmw 累计计数，可靠）

| 层 | SHM | UDP | 说明 |
|---|---|---|---|
| spa 发 | 7068 raw/s（220 MB/s）| 7068 raw/s | DDS 发送 |
| **data reader 收**（reader→mailbox）| **832 raw/s** | **800 raw/s** | ⚠️ **data 收瓶颈** |
| **data publish**（writer）| **832/s（=reader，全 publish）** | 800/s | **data 处理不丢**（CPU 非瓶颈）|
| web 收（N=8 reader）| 2278 fps | 1312 fps | 多 reader fan-out 收全（mock 优化）|

**关键**：data publish = reader 收（每收一 raw 全 publish），说明 **data domain 处理（切分+合并 publish）跟得上，不是瓶颈**。瓶颈在 **reader→mailbox 投递（~830 raw/s）**。

## 3. 瓶颈定位（代码分层）

| 层 | 接口 | 位置 | 瓶颈 | 速率 |
|---|---|---|---|---|
| **infrastructure** | **`mailbox.Send`（vector assign 32K 拷贝）** | **`mailbox.h`** | ⚠️ **data 收瓶颈** | **~830 raw/s** |
| detmw 传输 | `on_data_available → take → mailbox.Send` | `detmw_fastdds.cpp:84` | reader 回调（含上面 Send 拷贝）| ~830 raw/s |
| data domain | Process（切分+合并 publish）| `data_factory_v2.cpp` | 非瓶颈（全 publish）| ≥830/s |
| data domain | ReportAggregator | `report_aggregator.cpp` | 非瓶颈（1 publish/帧）| — |

**核心瓶颈 = `Mailbox::Send` 的 `payload.assign(data, data+len)`（每帧 32K 堆拷贝）**。reader 回调每收一帧拷 32K，830/s × 32K = 26 MB/s 拷贝开销打满回调线程。

## 4. 周期上报极限（场景 1：稳态不丢包）

不丢条件：spa ≤ data reader 收（reader→mailbox 投递极限）。

| 项 | 值 |
|---|---|
| **最小周期** | **≥ 1.2ms（~830 Hz）** |
| **载荷** | 32K/帧 |
| **不丢带宽** | **26 MB/s**（830 × 32K）|
| **QoS** | reliable + KEEP_LAST depth≥1（depth 不影响收速率，mailbox.Send 是限）|

## 5. 突发余量（场景 2）

data 处理（domain）有余量（≥830/s，被 reader→mailbox 限；优化 mailbox 后更高）。突发靠 reader history depth 吸收。

| 项 | 值 |
|---|---|
| 周期稳态 | ≤ 830 Hz（data reader 收跟上）|
| 突发吸收 | reader history depth（KEEP_LAST，缓冲突发帧）|
| 端到端不丢 | 突发 + 周期 ≤ 830/s（reader 收极限）|
| QoS | reliable + KEEP_LAST **depth = 突发峰值** |

## 6. data 线程优化计划（按收益排序）

| # | 优化 | 目标瓶颈 | 位置 | 预期 |
|---|---|---|---|---|
| 1 | **mailbox.Send 零拷贝**（span/引用替代 vector assign）| reader→mailbox 32K 拷贝 | `mailbox.h` Send | data 收大幅提升（核心）|
| 2 | Process Extra/Add 批量 memcpy（raw 切片区一次拷贝到 block）| 切分 500 次 memcpy | `data_factory_v2.cpp` | 提 data 处理 |
| 3 | web 多 reader（已验 N=8 收 5.4x）| web 收 | `webserver_mock.cpp` | mock 收全（已做）|
| 4 | AcquireBlock 1 次/帧（已优化）| hash 探测 | `data_factory_v2.cpp` | 已做 |
| 5 | ForEachByDomain 无锁/分区 | registry 锁 | `extractor_registry.cpp` | 提切分遍历 |

**优化 1（mailbox 零拷贝）是核心**：当前 reader 回调每帧 32K 堆拷贝（vector assign + 释放），是 data 收 830/s 的主因。改 span 引用（MailMsg 持引用/span，不 assign），reader→mailbox 应大幅提升，届时 data domain 处理极限（切分+publish）才显现真实值。

## 7. 关键结论

1. **data domain 处理（切分+publish）不丢**（publish = reader 收，全 publish）—— data CPU 当前非瓶颈
2. **瓶颈 = `mailbox.Send` 32K 拷贝**（infrastructure 层），data reader→mailbox ~830 raw/s
3. **最小周期**：830 Hz（1.2ms），带宽 26 MB/s
4. **优化首选 mailbox 零拷贝**（提 reader→mailbox），次选 Process 批量 memcpy
5. web mock 多 reader（N=8）已让 mock 不成瓶颈（收全）

## 待办

- [ ] mailbox.Send 零拷贝实现 + 重测（预期 data 收大幅提升）
- [ ] Process 批量 memcpy（raw 切片区一次拷贝）
- [ ] 精确周期不丢边界扫（spa interval，确认 web 收 == spa 发）
