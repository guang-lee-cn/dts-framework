# data 子系统性能测试报告（raw 32K，500 dataId 随机 4-200B）

> 日期：2026-08-10 · 环境：WSL2 16 核（taskset 0-7）/ raw 32K / 500 dataId（4-200B 随机，Σ=32K-head）
> 方法：DETMW_DATA_BYPASS 三层隔离 + Process skip 诊断 + detmw reader/writer 硬计数 + SHM/UDP 对比

## 1. 数据结构

| 项 | 结构 | 大小 |
|---|---|---|
| **rawData 帧**（spa→data）| `[u16 dataType][u32 cellId][u32 cpId][Σ 500 切片]` | **32768 字节（32K）** |
| **dataId 切片** | 变长（mock 填充）| **4-200B 随机**（固定 seed 42，data/spa 共享 layout）|
| **加工 cache 槽** | 变长 = dataId 切片大小 | 4-200B/dataId |
| **上报帧**（data→web）| `[ReportHeader 20B][500 × (SubHeader 4B + data)]` | ≈ 32K（合并，1 publish/帧）|

extractor i 切 raw 偏移 `head + Σ(prev cacheSize)`，长度 `cacheSize[i]`。500 切片经 ReportAggregator 合并成 1 帧上报。

## 2. 瓶颈确认：data 还是 DDS？

**方法**：Process skip 诊断（sink/task 跳过计数）+ 三层 bypass。

| 诊断 | 结果 |
|---|---|
| skipSink / skipTask | **0 / 0**（无提前返回）|
| forEach（切分次数）| **稳定 500×consume**（registry 500 全遍历，非跳变）|
| data Process consume（recv/process/full）| ~5000-8000 raw/s（一致，切分非瓶颈）|

**结论：data CPU 切分不是瓶颈。**（之前 4KB raw 测得 publish 96 帧/s 是 mailbox 溢出 + forEach 跳变假象，raw 32K 后稳定，data 处理 5125-7953 帧/s）

## 3. 各层速率（raw 32K，UDP vs SHM）

| 层 | SHM | UDP | 说明 |
|---|---|---|---|
| spa 发 | 10165 raw/s（317 MB/s）| 10165 raw/s | DDS 发送，reader 全收 |
| data 收（reader）| ~6800 raw/s | ~8000 raw/s | reader→mailbox 投递 |
| **data 处理**（consume+publish）| **5125 raw/s** | **7953 raw/s** | **data CPU 非瓶颈**（切 500 + publish）|
| data publish（writer）| ≈ consume | ≈ consume | 1 publish/帧（合并）|
| **web 收**（on_data_available）| **328 fps（10 MB/s）** | **861 fps（27 MB/s）** | ⚠️ **端到端瓶颈** |

**瓶颈 = web DDS reader on_data_available**（32K 帧回调速率）+ SHM 512KB 段限（32K 帧只缓冲 16）。

## 4. 周期上报性能极限（场景 1：稳态不丢包）

不丢条件：data publish ≤ web 收（端到端每帧到 web）。

| 项 | SHM | UDP |
|---|---|---|
| **最小周期** | ≥ 3.0ms（328 Hz）| **≥ 1.16ms（861 Hz）** |
| **载荷** | 32K/帧 | 32K/帧 |
| **不丢带宽** | 10 MB/s | **27 MB/s** |
| **QoS** | reliable + KEEP_LAST depth≥1 | 同 |

**data 处理有余量**：data consume 5125-7953 raw/s，web 收 861-328 raw/s，**data 余量 4200-7000 raw/s**（data CPU 远未打满）。

## 5. 周期 + 突发（场景 2：余量吸收突发）

降周期带宽 → data 余量更大 → 突发用余量 + reader history depth 吸收。

| 项 | UDP 值 |
|---|---|
| 周期稳态 | ≤ 861 Hz（data 余量 7953-861=7092 raw/s）|
| 突发吸收 | data 处理余量（7092/s）+ reader history depth（KEEP_LAST）|
| 端到端不丢 | 突发 + 周期 ≤ web 收（861/s），或提 web（优化见 §7）|
| QoS | reliable + KEEP_LAST depth = 突发峰值（depth=1000 缓冲 1000×32K，但 SHM 限 16 帧）|

> 注：SHM 模式下 depth 受 512KB 段限（16 帧），UDP 不受 SHM 段限但 reader on_data_available 仍 861/s。

## 6. 瓶颈接口定位（代码分层）

| 层级 | 接口 | 位置 | 瓶颈 |
|---|---|---|---|
| detmw 传输 | `on_data_available → take_next_sample` | `detmw_fastdds.cpp:84` RecvListener | web reader 回调（861/s 限）|
| detmw 传输 | SHM 段 512KB（FastDDS 默认）| `detmw_fastdds.cpp` 构造 | 32K 帧只缓冲 16（Bug 7/8）|
| infrastructure | `mailbox.Send`（vector assign 拷贝）| `mailbox.h` | reader→mailbox 投递（~8000/s）|
| data domain | Process（切分+合并 publish）| `data_factory_v2.cpp` | **非瓶颈**（5125-7953/s）|
| data domain | ReportAggregator | `report_aggregator.cpp` | 非瓶颈（合并 1 publish/帧）|

## 7. 优化思路

| 优化 | 目标瓶颈 | 预期 |
|---|---|---|
| **UDP 替 SHM**（`DETMW_UDP_ONLY=1`）| SHM 512KB 段限 | web 收 328→861 fps（2.6x），已验证 |
| web on_data_available 批量 take | web reader 回调 | 减少 per-sample 回调开销 |
| SHM 段调大（待 FastDDS 修 Bug 8）| SHM 段限 | 32K 帧缓冲 16→更多 |
| mailbox 零拷贝（span 引用替代 vector assign）| reader→mailbox 投递 | 减每帧拷贝 |
| web 多 reader / 多线程消费 | web reader 单线程 | 并行收 |

## 8. 关键结论

1. **data CPU 不是瓶颈**（5125-7953 帧/s 处理，余量大）——三层隔离 + skip 诊断确认
2. **端到端瓶颈在 web DDS reader on_data_available + SHM 512KB 段限**
3. **最小周期**：UDP 1.16ms（861 Hz），27 MB/s 不丢；SHM 3ms（328 Hz），10 MB/s
4. **DDS 带宽够**（spa 发 317 MB/s，reader 全收），瓶颈在 web 收侧
5. **突发余量**：data 处理余量 4200-7000 raw/s，但 web 收限（861/s），突发端到端不丢需 data publish ≤ web 收
6. **优化首选 UDP**（2.6x，绕 SHM Bug 8），次选 web reader 批量 + 多线程

## 待办

- [ ] 精确测周期不丢边界（扫 spa interval，确认 web 收 == spa 发）
- [ ] 突发场景实测（spa 周期 + 突发，验证余量吸收）
- [ ] web reader 批量 take 优化
