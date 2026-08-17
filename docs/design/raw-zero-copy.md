# raw 零拷贝通道设计（DataSharing / FixedBytesType）

> 2026-08-17 · 目标：突破 data 接收瓶颈 950 帧/s（32MB/s），支撑 100 帧 32K/100ms（1000 帧/s）
> 背景：容量核算（worklog 2026-08-17）——确认负载 1000 帧/s 超当前上限 ~5%，且零余量。

## 1. 瓶颈根因（bench 定位）

detmw 原 `BytesType`（`sequence<octet>` 语义）：`is_plain=false`、`is_bounded=false`
→ FastDDS 按 CDR 处理：发送 serialize + 接收 deserialize，每帧 32K 两次 memcpy + 完整
序列化框架开销（RTPS 封装 / history 管理 / 分配），接收侧上限 950 帧/s。

## 2. 方案：定长 plain 通道（FixedBytesType）

| 机制 | 说明 |
|---|---|
| `FixedBytesType`（新） | 固定大小（配置 `plain_size`），`is_plain=true` + `is_bounded=true`，serialize/deserialize 纯 memcpy（无 CDR 封装） |
| DataSharing | plain + bounded → FastDDS 默认 AUTO 自动启用（需 SHM 传输）：**发送侧 `loan_sample()` 共享内存零拷贝**，接收侧 take 拷贝出池（省 CDR/分配） |
| 发送侧 loan 尝试/回退 | `Send()` 对 plain topic 先 `loan_sample(void*&)`：成功 → memcpy 进共享 buffer + `write(loan)`（零拷贝）；失败（无 SHM/非 DataSharing）→ 回退普通 `write(vec)`（功能不回归） |
| 接收侧 | `take_next_sample(void* buffer)` 拷贝形态：listener 预分配定长 buffer（`plainSize`），无 loan 归还负担 |
| 严格定长语义 | plain 通道消息必须 == `plain_size`（Send 校验，不匹配拒绝——配置错误显式暴露） |

**配置**：topic 条目可选 `"plain_size": N`（gen_detmw.py 透传全字段，无需改生成器）：

```json
{ "session_type":"DTS", "session_inst":"data", "msg_id":9,
  "role":"subscribe", "thread":"data", "plain_size":32768 }
```

缺省（无 plain_size）= 原 BytesType 通道，完全兼容。

**示例（sandbox 验证配置，plain-dts / plain-pub，1KB 定长）**：
`quality/test/config/plain/` + `quality/test/run_plain.sh`（ctest plain_smoke，收全量断言）。

## 3. 关键决策点（商用落地前置，需真实环境验证）

1. **SHM 段容量 vs FastDDS 3.6 Bug 8**：DataSharing 池走 SHM 段；内置段 512KB 装不下
   100 帧 32K 突发（3.2MB）。加大段 = 自定义 `SharedMemTransportDescriptor`
   （`use_builtin_transports=false` 触发析构 SEGV，Bug 8 / 上游 #6114 未修）。
   **决策结论（2026-08-17，见 [fastdds-upgrade-assessment.md](fastdds-upgrade-assessment.md)）**：
   上游 3.6.2/master/2.14.6 均未修复（SHM 代码零改动）→ **自研桶 = 确定路线**；
   升级 FastDDS 已排除；内置段小突发仅过渡。
2. **真实硬件测量**：沙箱无 SHM、32K UDP 不可达，**loan/DataSharing 收益必须在目标
   环境实测**。测量步骤：
   ```bash
   # 目标环境（/dev/shm 可用）：
   ctest --test-dir build -R plain_smoke          # 功能（含 loan 路径日志 "loan/zero-copy"）
   # 吞吐：plain 通道 32K 压测（perf 链 + plain_size=32768 配置），对比 BytesType 基线 950/s
   ```
3. **多进程 plain 通道一致性**：同 topic 各进程 `plain_size` 必须一致（类型名含尺寸，
   不一致将 discovery 不匹配——显式失败而非静默）。

## 4. 已落地代码

- `thirdparty/detmw/src/detmw_fixed_type.h`：FixedBytesType（单测可见）
- `thirdparty/detmw/src/detmw_fastdds.cpp`：per-topic 类型表 / plain 通道 loan 尝试回退 /
  listener 定长预分配；类型所有权修复（TypeSupport shared_ptr 唯一拥有，曾双持 SEGV）
- `thirdparty/detmw/src/detmw.cpp`：配置 `plain_size` 解析 → 传输工厂
- `gen_detmw.py`：三线程路由头**恒定生成**（空表允许）——修纯单线程进程编不过
- 测试：`unit_fixed_type`（序列化 roundtrip / plain 语义）、`plain_smoke`（双进程
  1KB 定长收全量 + 干净退出）

## 5. 实测（沙箱，UDP fallback）

```
type up topic=DTS_data_9 type=detmw::FixedBytes_1024 plain=1   # plain 注册成功
published topic=DTS_data_9 len=1024 (loan/zero-copy)           # loan 路径生效
reader[0] recv_total=10                                        # 收全量
```
注：沙箱 loan 成功但无 SHM，实际共享内存收益待真实环境确认。
