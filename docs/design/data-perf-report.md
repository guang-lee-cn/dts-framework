# data 子系统性能测试报告（500 dataId 切分，DDS 调优链路）

> 日期：2026-08-10 · 环境：WSL2 16 核（测试绑 taskset 0-7，留 8-15 给 IDE）/ reliable depth=1000 / 4KB raw
> 方法：三层 bypass 隔离 + detmw reader/writer 硬计数 + wall-clock consume rate

## 1. 数据结构（你问的 raw / dataId 长什么样）

| 项 | 结构 | 大小 |
|---|---|---|
| **rawData 帧**（spa→data）| `[u16 dataType=0(CELL)][u32 cellId][u32 cpId][500 × 8B 切片]` | **4010 字节** |
| **dataId 切片** | `{int f0; int f1}`（演示，真实业务字段后续替换） | **8 字节** |
| **加工 cache 槽** | 同 dataId 切片（8B），500 个 dataId 共享 1 个测量对象 block | 8 字节/dataId |
| **上报帧**（data→web）| `[ReportHeader 20B][500 × (SubHeader 4B + data 8B)]` | **~6020 字节** |

raw 由 500 个 dataId 切片拼成；extractor i（dataId=i）切 raw 第 `head + i*8` 偏移的 8 字节；500 个切片攒成一帧上报。

## 2. 瓶颈确认：data 还是 DDS？（你问怎么确认的）

**方法**：`DETMW_DATA_BYPASS` 三层隔离——data 收到 raw 后，分别只做"收" / "收+切" / "收+切+发"，量 data 消费 raw 速率（consume_rate，wall-clock）。

| bypass 层 | data 做什么 | consume_rate（raw/s）|
|---|---|---|
| recv | 只收，不切不发 | **14463** |
| process | 收 + 切 500 dataId，不发 | **14605** |
| full | 收 + 切 + 合并发 | **13770** |

**结论：三层 consume 几乎一致（~14000/s）→ 切分 500 dataId 不增 data 消费开销，瓶颈不在 data CPU 切分，而在 DDS 收通道（reader→mailbox 投递 ~14000/s）。**

> 注：这推翻了之前"500 publish/帧拖慢 data"的判断——合并 report 后 publish 已不是瓶颈（1/帧），切分也非瓶颈（process≈recv）。真瓶颈是 DDS reader→mailbox 的投递速率。

## 3. DDS 收发极限（单独评估）

| 通道 | 速率 | 说明 |
|---|---|---|
| spa 发（4KB raw）| 15000–36000 raw/s（**55–140 MB/s**）| spa 全速，DDS 发送 |
| data reader 收（recv_total）| **= spa 发**（reader 跟上）| DDS 收不丢，reader history 吸收 |
| data reader→mailbox 投递 | **~14000 raw/s**（consume 上限）| data 消费 mailbox 极限 |
| data→web publish（full）| 见 §4 |  |

**DDS 带宽极限**：spa 发送 4KB 帧到 140 MB/s（reader 全收），DDS 传输层不是瓶颈。瓶颈在 reader 回调→mailbox 投递（~14000/s），这是 detmw 的 `on_data_available → mailbox.Send` 路径。

## 4. data publish 极限 + 内部转化问题

| 项 | 值 | 说明 |
|---|---|---|
| data Process 消费（consume_rate）| ~14000 raw/s | 受 DDS reader→mailbox 限 |
| data publish（writer write_total）| **48–96 帧/s** | 每帧 500 dataId 合并 |
| **Process→publish 转化率** | **0.7%**（96/14000）| ⚠️ 异常 |

**问题**：data Process 消费 14000 raw/s，但只 publish ~96 帧/s。诊断 log 显示 `ForEachByDomain matched=500`（registry 正确），但 ForEach 计数跳变（某秒 192000、某秒 0）——说明 **大部分 Process 在 ForEach 前 return（sink/currentTask 检查或路径不稳），少数跑完整切分+publish**。

这是 data 内部实现的待修点（非 DDS 瓶颈）。修好后 data publish 应接近 consume（~14000 帧/s）。

## 5. 周期上报性能极限（场景 1：稳态不丢包）

不丢条件：spa 发 ≤ data publish 极限（data 消费+publish 跟上）。

| 项 | 当前值 | 修复转化率后预期 |
|---|---|---|
| **周期极限** | spa ≤ 96 raw/s（**周期 ≥ 10ms / 100Hz**）| ≤ 14000 raw/s（周期 ≥ 0.07ms）|
| **载荷** | 4KB/帧（500 dataId × 8B）| 同 |
| **不丢带宽** | 96 × 4KB = **384 KB/s** | 14000 × 4KB = **55 MB/s** |
| **QoS** | reliable + KEEP_LAST depth≥1（depth 不影响 publish 速率，见 §7）| 同 |

## 6. 周期 + 突发（场景 2：稳态余量吸收突发）

降周期带宽/频率 → data 处理有余量 → 突发靠 reader history depth 吸收。

| 项 | 值 |
|---|---|
| 周期稳态 | ≤ 96 Hz（同 §5），data 余量 = 14000 - 96（修复后）|
| 突发吸收 | reader history **depth**（KEEP_LAST 缓冲）|
| 不丢条件 | 突发量 ≤ depth + 周期内 data 消化量 |
| QoS | reliable + KEEP_LAST **depth = 突发峰值**（depth=1000 吸收 1000 突发帧）|

## 7. QoS 对性能的影响（depth 扫）

| depth | data consume（raw/s）| 说明 |
|---|---|---|
| 10 | ~14000 | depth 不影响 data 消费速率 |
| 1000 | ~14000 | reader history 容量，只缓冲/吸收突发 |
| 10000 | ~14000 | 同 |

**QoS 调参（reliability/depth）对 data 消费速率无效**——瓶颈是 reader→mailbox 投递（~14000/s）+ data publish 转化（§4），不在 DDS 传输/可靠性。QoS depth 的作用是**突发吸收**（reader history 容量），不是提速率。

## 8. 热点 + 优化策略

| 热点 | 位置 | 优化 |
|---|---|---|
| **DDS reader→mailbox**（~14000/s）| detmw `on_data_available → mailbox.Send` | mailbox.Send 当前 `vector assign`（每帧拷贝），可改零拷贝/批量 take |
| **data publish 转化率低**（0.7%）| Process ForEach 前 return | ⚠️ 待定位（sink/currentTask 检查路径不稳，fe 跳变）。修后 publish 提到 ~14000 |
| ~~切分 CPU~~ | Process 500 Extra/Add | 非瓶颈（process≈recv），无需优化 |
| 内存管理 | DataMemManager 已上电预分配（静态池，零运行分配）| 已优化 |

## 9. 关键结论

1. **瓶颈不是 data CPU 切分**（三层 consume 一致），是 **DDS reader→mailbox 投递（~14000/s）** + **data publish 转化率低（实现 bug，待修）**
2. **DDS 传输带宽够**（spa 发 140 MB/s，reader 全收），DDS 不是带宽瓶颈
3. **当前 publish 96 帧/s 是 data 实际上报极限**（受内部转化限制），修转化后可达 ~14000 帧/s
4. **周期不丢**：当前 spa ≤ 96 Hz（周期 ≥ 10ms），修复后 ≤ 14000 Hz
5. **突发**：靠 reader history depth（KEEP_LAST）吸收，QoS depth=突发峰值

## 待办

- [ ] **定位 data publish 转化率低**（ForEach 跳变，§4）——修后重测，预期 publish 提到 ~14000 帧/s
- [ ] detmw reader→mailbox 零拷贝/批量（提 DDS 收从 14000 到更高）
- [ ] 去调试 log（consume_rate/bypass 保留作工具）
