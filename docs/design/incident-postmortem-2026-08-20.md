# bucket_rtps OOM 事故复盘（2026-08-17 ~ 08-20）

> 按事故复盘纪律五段模板撰写（方法论：prompt 仓库 skills/review/incident-review.md）。
> 事实流水见 [worklog/2026-08-20.md §A](../worklog/2026-08-20.md)，本文是完整复盘。

## 1. 事实（5W1H）

| 要素 | 内容 |
|---|---|
| **What** | bucket_rtps_test 单进程 anon-rss 12.4~12.6GB（pub/sub 两进程合计吃满 WSL 21.5GB），OOM killer 连杀后虚拟机崩溃重启 |
| **When** | 4 次：2026-08-17 16:23:34、16:32:41；2026-08-20 17:21:47、17:23:44（跨两个 boot） |
| **Where** | 精确层位：**FastDDS TransportInterface 接口语义层**（`max_recv_buffer_size` 的消费契约），不是泛指的"内存泄漏"，也不是 WSL 网络层（曾被误判） |
| **Who** | bucket_rtps_test 收线程（`BucketChannelResource::run()` 的接收缓冲预分配），修复点 `BucketTransport::OpenInputChannel` |
| **Why（表层）** | 收线程按调用方传入的 `max_msg_size = UINT32_MAX` 分配接收缓冲：`std::vector buf(4GB)` 置零摸满 × 3 通道/进程 |
| **How** | 修复 = OpenInputChannel 钳制 `min(max_msg_size, 配置 maxMessageSize)`（commit 6ef520e）；受控复验 sub RSS 208MB，200×32K 零丢 PASS |

证据等级：根因链全部**铁证**（journal OOM 记录 5 条、strace `mmap(NULL, 4294971392)`×5、FastDDS 源码 NetworkFactory/SharedMemTransport 逐行核对、受控复现正反两次）。

## 2. 根因链（从现象到可修改机制）

```
FastDDS 非 secure 模式：参与者侧 max_receiver_buffer_size = UINT32_MAX（写死）
  ↓ NetworkFactory: min(transport值, 参与者值) —— 两端皆上限值，min() 形同虚设
  ↓ OpenInputChannel(locator, receiver, max_msg_size = 4294967295) 原样下发
  ↓ 桶收线程 std::vector<uint8_t> buf(max_msg_size_) 预分配 → vector(4GB) 置零摸满
  ↓ 每进程 3 输入通道（元组播/元单播/用户单播）= 12GB 常驻 × pub/sub 两进程
  ↓ WSL 21.5GB 吃满；swap=0（.wslconfig 配了 8GB 未生效）无缓冲
  ↓ OOM → VM 崩溃 → 会话断连 ×4
```

内置 SHM 同样返回 UINT32_MAX 却从不炸机：它从共享内存段内**原地读**（`OnDataReceived` 直接拿段内指针），从不按该值分配。桶传输抄了它的返回值语义（"能力声明"），却实现了 UDP 式的"按值预分配私有缓冲"——**抄了一半**。

## 3. 为什么没拦住（防线逐条检视）

| 防线 | 缺位 |
|---|---|
| 测试 | bucket 测试设计了"无 /dev/shm 自动 Skipped"，此前所有验证都走了跳过分支；开发机 /dev/shm 一直可用，08-17 16:23 是该路径**首次真跑**——"沙箱验证通过"给从未执行的代码发了通行证 |
| 护栏 | 新路径首跑无任何资源上限（无 ulimit、无内存监控），失控直接打穿到虚拟机层 |
| 证据保全 | VM 崩溃带走页缓存中未落盘的 ctest 日志，"测试到底跑没跑"长期成谜，拉长排查 |

## 4. 为什么走了弯路（前提陷阱检讨，命中 4/5）

| 陷阱 | 本案命中方式 |
|---|---|
| ① 验证过 = 没问题 | "沙箱里 Skipped 验证过机制正确" ≠ 真跑过；环境分叉未识别 |
| ② 文档记录 = 事实 | worklog 记了"Skipped（shm 不可用）"，排查时以此排除了 bucket 嫌疑——文档只记录了它看到的分支 |
| ③ 时间相关 = 因果 | 断连时刻恰能对上全天候网络噪声（CheckConnection 本 boot 283 次），真病掩护了死因 |
| ⑤ 没跑起来 = 没执行 | tee 日志缺失被误读为"命令从未执行"；实际已执行，结果随崩溃丢失 |
| 方法层遗漏 | 首轮排查只查当前 boot 的 dmesg，未跨 boot 搜 OOM 历史——`journalctl -k --since 2026-08-17 | grep 'Out of memory'` 一条命令 5 秒可锁定凶手 |

转折点：**带护栏的受控复现**（`ulimit -v 3GB` 保证杀不死机器）+ **strace 抓分配形态**（4GB mmap 一眼定罪）。"一跑就崩机"类 bug 的标准姿势：先上枷锁再复现，用分配轨迹代替猜测。

## 5. 改进措施（双环 + 资产载体）

**双环学习**：
- 第一环（行为）：OpenInputChannel 钳制收缓冲尺寸（已落地）
- 第二环（心智）："验证过"降级——验证过的永远只是"某环境 × 某路径"的组合，任一变化即失效；自定义框架接口的返回值/参数语义，必须核对其**消费方式**再复用

| 措施 | 资产载体 | 状态 |
|---|---|---|
| 桶测试脚本加 `ulimit -v` 内存护栏 | **工具**（新路径首跑必带帽，不依赖记性） | 待做 |
| 崩溃/断连类排查第一步 = 跨 boot 搜 OOM/panic 历史 | **SOP**（已写入 incident-review.md §2） | ✅ |
| `max_recv_buffer_size` 返回值 = 真实可分配上限（消费方会按它分配） | **契约**（修复处代码注释；本文档） | ✅ |
| Windows 侧确认 .wslconfig swap=8GB 为何未生效（本次放大器） | 环境配置 | 用户侧 |
