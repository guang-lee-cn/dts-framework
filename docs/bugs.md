# DTS 故障记录（2026-08-02 会话）

> 记录本会话从"生成路由接入 → 静态发现 → S 级压测 → 多维度基准"过程中踩到的全部故障。
> 按统一结构：问题描述 / 定位流程与思路 / 使用工具 / 定位结论 / 修改代码 / 5W1H 复盘 / 第一性原理洞察。

---

## Bug 1：detmw listener 单取 —— 高吞吐必丢

### 问题描述
压测突发（256B×200 紧突发）时中间件丢包成簇 + 空洞不补，sub 收 64/200。起初被误判为"FastDDS 可靠协议丢包"。

### 定位流程与思路
1. 先建 perf 链路（gen→task→data→sub），看 end-to-end 丢包 → 丢。
2. **分层定位**：给 detmw 加 reader 收数统计（`recvCount`），对比各 reader 收数 → 损点收敛在 gen→task 一跳（reader[0] 只收 100/200）。
3. 看 listener 实现 → 发现 `on_data_available` 里只 `take_next_sample` 一次。
4. 查 FastDDS 源码确认：`on_data_available` 是"数据可用"信号，不会为剩余样本重触发。

### 使用的工具
perf 压测链路、reader 收数统计、FastDDS 源码阅读（RecvListener/DataReader 回调语义）。

### 定位结论
`on_data_available` 每次只 take 1 条样本，剩余样本堆在 reader history，被 KEEP_LAST 覆盖 → 高吞吐必丢。这是 detmw 真 bug。

### 修改代码
[detmw_fastdds.cpp](thirdparty/detmw/src/detmw_fastdds.cpp) 的 `RecvListener::on_data_available`：改排空循环（`while take_next_sample == OK`）。

### 5W1H 复盘
- **What**：listener 单取，高吞吐丢包。
- **When**：任何突发/高吞吐场景。
- **Where**：detmw 传输层 listener。
- **Why**：写回调时默认 FastDDS 会像队列一样逐条调用回调。
- **Who**：detmw 实现者（早期 MVP 代码）。
- **How**：排空循环修复。

### 第一性原理洞察
**事件驱动回调的语义是"状态变化通知"，不是"逐数据交付"。** 消费方收到"有数据"信号后必须自行排空，因为信号只保证存在性、不保证逐条对应。把"通知"误解为"交付"，是事件驱动编程最常见的故障引入点。

---

## Bug 2：MakeStaticXmlPath 丢失路径分隔符

### 问题描述
`detmw_init(cfg)` 推导 staticdiscovery.xml 路径时拼成 `.../cpf-dtsstaticdiscovery.xml`（缺 `/`），check_xml_static_discovery 失败，进程起不来。

### 定位流程与思路
1. 启动报 `file://...cpf-dtsstaticdiscovery.xml bad file`。
2. 看路径一眼看出缺分隔符 → 查 `MakeStaticXmlPath` 实现。

### 使用的工具
启动日志、源码审读。

### 定位结论
`strcpy(out + dir_len, ...)` 把刚写入的 `out[dir_len] = '/'` 覆盖了——写分隔符和拷贝文件名用了同一个偏移。

### 修改代码
[detmw.c](thirdparty/detmw/src/detmw.c) `MakeStaticXmlPath`：用 `memcpy(dir_len+1 含 '/')` + `snprintf` 拼接。

### 5W1H 复盘
- **What**：路径拼缺分隔符。
- **Where**：detmw.c 路径拼接。
- **Why**：手动偏移拼接，写 `/` 的位置又被 strcpy 覆盖。
- **How**：改成含尾斜杠的 memcpy + 追加文件名。

### 第一性原理洞察
**手动拼路径/字符串是 off-by-one 高发地，因为"分隔符"是隐式状态。** 用专门的路径 API 或一次性格式化成串，比多次偏移拼接更不容易错。凡是"先写分隔符、再追加内容"的模式都要警惕覆盖。

---

## Bug 3：QoS 调参引入投递丢包

### 问题描述
为"修"32K 突发丢包，给 detmw 加 QoS 调参（depth 1000 + acknack/nack 0 + 心跳 100ms），结果**引入**投递丢包（256B×200 从"偶发"变"稳定丢"）。

### 定位流程与思路
1. 突发丢包 → 怀疑默认 QoS 不够好 → 调参。
2. 调参后仍丢 → 建**最小 FastDDS-only 复现**（不经 detmw，双进程 + 静态发现，`fastdds_burst_repro.cpp`）。
3. 复现里**默认 QoS 200/200 零丢，调参 QoS 丢 5** → 铁证：调参是元凶。

### 使用的工具
[fastdds_burst_repro.cpp](quality/test/fastdds_burst_repro.cpp)（最小 FastDDS 复现，`REPRO_TUNED` env 开关）、交叉对比默认 vs 调参。

### 定位结论
QoS 调参（尤其 heartbeat_period 100ms + nack/acknack 0）破坏可靠投递的时序语义 → 丢包。回退调参。

### 修改代码
[detmw_fastdds.cpp](thirdparty/detmw/src/detmw_fastdds.cpp) 回退 reader/writer 的 times 调参；保留 depth 100→1000（无害，S级仍零丢）。

### 5W1H 复盘
- **What**：调参破坏可靠投递，引入丢包。
- **When**：加调参后。
- **Where**：detmw 可靠 QoS。
- **Why**：想"优化"突发，但没先确认默认 QoS 是否可靠，就动了可靠协议的核心节拍。
- **How**：回退；教训=先最小复现确认根因，再动配置。

### 第一性原理洞察
**可靠投递的 timing 参数（heartbeat/acknack/nack）是协议的正确性节拍，不是可随意调的性能旋钮。** "优化"的前提是先有基线测量、先证明默认值有问题。在没复现根因前跳到配置调整，是把"假设"当"结论"——这正是 Bug 3 和 Bug 1 误判的共同来源（没先做最小复现就归因/调整）。

---

## Bug 4：perf_gen 的 PerfInit 固定 memset 溢出小包

### 问题描述
压测小包（256B）时 gen 必现崩溃，症状五花八门：burst 期间 malloc 断言、Adapter 析构 `free(): invalid pointer`、退出时 factory 析构 SIGSEGV。一度被误判为"FastDDS 内存 bug / detmw 内存 bug"。

### 定位流程与思路
1. 5/5 必现崩溃 → 怀疑真 bug。
2. gdb 抓栈：有时在 `TopicPayloadPool::resize`（malloc 断言），有时在 `Adapter::~Adapter`（vector<Topic*> 双释放），有时在 `PublisherImpl::disable`（退出）。
3. **症状分散 = 堆被更早溢出写坏，崩溃点只是"撞上"坏堆的位置** → 换 valgrind。
4. valgrind 直接抓到：`Invalid write of size 8 ... 0 bytes after a block of size 256 alloc'd`，栈指向 `PerfInit` → memset。

### 使用的工具
gdb（栈回溯，发现症状分散）、**valgrind memcheck**（决定性定位溢出点）。

### 定位结论
[perf_packet.h](quality/test/perf_packet.h) 的 `PerfInit` 固定 `memset(pkt, 0, kPerfPacketSize=32768)`，但压测小包时 pkt 只有 256B → 溢出 32512B 写坏堆。所有"崩溃"全是它的症状，**中间件没崩过**。

### 修改代码
`PerfInit(uint8_t* pkt, size_t size)` 参数化，按实际包大小 memset；perf_gen 传 `pktSize`。

### 5W1H 复盘
- **What**：压测工具堆溢出，伪装成系统崩溃。
- **When**：压测任意 <32K 的包。
- **Where**：perf 工具初始化函数。
- **Why**：包大小参数化后，初始化函数还残留固定 32k 的常量。
- **How**：传实际 size；教训=先验证工具正确性再归因被测系统。

### 第一性原理洞察
**压测工具与被测系统同源时，工具的 bug 会伪装成系统的 bug，且症状可能随机分散（堆被早写坏，崩点不固定）。** 遇到"症状分散、位置漂移"的崩溃，第一反应应是"堆被提前写坏"，用 valgrind/ASan 这类能定位**首次写坏点**的工具，而不是在崩溃点追。这也是"常量假设残留"的经典案例：接口参数化后，内部实现没跟上。

---

## Bug 5：TaskManager kMaxTasks=64 被误判为中间件丢包

### 问题描述
压测发 200 个唯一 taskId，sub 只收到 64，一度判定为"中间件突发丢包"。

### 定位流程与思路
1. 端到端丢 136 → 先查投递层：reader[0]（gen→task）收 201/201 **零丢**。
2. 再看业务层：dts 日志 `task 64 rejected (duplicate or full)` × 136。
3. 查 [task_manager.h](contexts/task/domain/include/task_manager.h)：`kMaxTasks = 64`。

### 使用的工具
reader 收数统计（分层）、dts 日志 grep `rejected`、源码审读。

### 定位结论
64 是**业务上限**（task 线程最多 64 个 udt 采集任务实例），第 65+ 条被业务拒收（不发布 msg2）。**非中间件丢包**——中间件把 200 包全投递到了 task 线程。

### 修改代码
无（业务规则）。压测输入改为 ≤64，避免误判。

### 5W1H 复盘
- **What**：误把业务拒收当中件丢包。
- **Why**：端到端黑盒指标直接下结论，没先分层（投递层 vs 业务层）。
- **How**：分层定位。

### 第一性原理洞察
**端到端指标必须先分层归因，才能正确下结论。"丢包"可能来自投递层（真丢）、业务层（拒收）、测量层（漏计/重复计）。** 黑盒端到端数字是"结果"，不是"原因"；用白盒分层（每跳 reader 计数）才能定位到层。这是本会话多次误判的共同根因：拿端到端数字直接归因。

---

## Bug 6：单订阅者 topic 双投递（曾观测，当前不复现，待确认）

### 问题描述
早期多组数据观测：data_4/log_6（**单订阅者** topic）每条数据被重复投递 2 次（sub reader 收 102/发50）；data_2（**双订阅者**）正常。曾判定"真实数据流会有重复上报"。

### 定位流程与思路
1. S级压测 sub 收到 2× 计数（msg4_recv=10 vs 期望 5）。
2. 看 CSV：`0 0 1 1 2 2 3 3 4 4` —— 每条 seq 出现两次 = 双投递。
3. 交叉验证：data_2（双 reader）正常、data_4/log_6（单 reader）双投 → 单订阅者 topic 特定。
4. 查 staticdiscovery.xml：端点唯一、entity ID 无冲突 → 非配置问题。
5. **追查**：EDPStatic 源码确认 v1/v2 交换格式互斥（detmw 默认 v1 只走属性通道，不会双公告）；加 writer 原始发布计数确认 dts 只发一次（write_total=6/发5）。
6. **当前不复现**：配置重新生成后 3/3 + S级全对（sub reader 收 51/发50），双投消失。

### 使用的工具
CSV seq 分布分析、reader 收数对比、writer 原始发布计数（m_writeCount）、EDPStatic 源码审读、staticdiscovery.xml 审读。

### 定位结论
**早期观测到的双投递当前稳定不复现**（多次复测 51/51 正确），疑似瞬态/状态相关（配置重生成后消失）。若真实数据流再出现重复上报，按"单订阅者 topic + 静态 EDP 匹配"方向深查。记为**待确认**，不按已证实 issue 处理。

### 修改代码
[perf_sub.cpp](quality/test/perf_sub.cpp) 按 seq 去重（benchmark 侧），让指标可信。

### 5W1H 复盘
- **What**：单订阅者 topic 双投递。
- **Where**：FastDDS 静态 EDP。
- **Why**：匹配/位图对单订阅者场景重复建远端端点。
- **How**：benchmark 去重绕过；根因待查。

### 第一性原理洞察
**测量系统的误差（重复计数）会掩盖被测系统的真实缺陷。** 双投递让"收到数"虚高，正好掩盖了 32K 突发的真实丢包（见 Bug 7）。**先做唯一性验证（去重），再信计数**——单一计数指标不足以判定可靠性。

---

## Bug 7：32K 紧突发间歇丢包（FastDDS issue，count 临界 + 间歇 + topic 特定）

### 问题描述
32K 紧突发（0 间隔）**count ≥ ~80 条时间歇性近全丢**（task reader 收 1-3/101，有时全收）；≤70 条稳定零丢；256B/4K 突发零丢；S级 1/s 零丢。**不是消费者速度**（slow 2000× 仍零丢），**topic 特定**（data msg3 路径 count=200 零丢，task msg1 路径 count=80 就间歇丢）。**此前被 Bug 6 双投递虚高计数掩盖**。

### 定位流程与思路
1. 基准矩阵（去重后）32K 突发 loss=49 → 异常。
2. 分层：dts reader 计数确认损在 gen→task 首跳，sub 计数镜像。
3. **裸 FastDDS 复现**：32K×50 紧突发 50/50、1/50、50/50 → FastDDS 3.6 本身。
4. 传输隔离：SHM 下丢、UDP 缓解但不根治。
5. **扫转换点**：count 70/80/90/100 → 70 零丢、80 丢、90 零丢、100 丢（间歇）。
6. **排除消费者速度**：DTS_DATA_SLOW 2000×（慢 2000 倍）仍零丢 → 非慢消费者触发。

### 使用的工具
多维度基准（去重版）、fastdds_burst_repro 裸复现、`DETMW_UDP_ONLY` 传输隔离、dts reader 计数分层。

### 定位结论
FastDDS 3.6 大包（32K）紧突发 + **count ≥ ~80 条** → 间歇性近全丢（SHM/静态发现大突发竞态）。**临界点**：
- 稳定零丢：紧突发 ≤70 条 / 任意周期流 / 小包
- 间歇丢：紧突发 ≥80 条 32K（topic 特定）
- 不是消费者速度（slow 2000× 仍零丢）

SHM 512KB 是根本缓冲限制（M 级消息 1MB 单条都收不到，见"M 级"节）。

### 修改代码
无（FastDDS 3.6 限制）。曾尝试自定义 SHM 描述符加大段，触发 Bug 8（析构崩溃），故回退。

### 5W1H 复盘
- **What**：32K 紧突发大 count 间歇丢。
- **Where**：FastDDS SHM/静态发现大突发路径。
- **Why**：大突发 + 512KB 缓冲 + 可靠重传竞态。
- **How**：紧突发 count ≤70 规避；根治需升级 FastDDS。

### 第一性原理洞察
**"可靠"（RELIABLE）是有限资源下的保证，不是无条件恰好一次。** 可靠协议的保证边界=缓冲/资源上限（这里是 SHM 段 512KB），突发超过缓冲就退化（丢或背压）。评估一个"可靠"系统，必须先问"保证的边界在哪"。**传输的隐含资源假设（段按 1KB 消息均值）是大包突发的真正杀手**——评估前要摸清传输的资源上限，不是只看协议层。

---

## Bug 8：FastDDS 3.6 自定义 SHM 描述符析构崩溃（修复 Bug 7 时撞上）

### 问题描述
为修 Bug 7（512KB SHM 段太小），用自定义 `SharedMemTransportDescriptor`（segment_size 16MB/32MB，`use_builtin_transports=false` + user_transports）加大段。**任何自定义 SHM 描述符（16MB 或 32MB、设或不设 max_message_size）都在 participant 析构时 SIGSEGV**。

### 定位流程与思路
1. 自定义 SHM 后 integration 测试 SEGFAULT。
2. 清残留进程确认非环境干扰 → 仍崩。
3. gdb 抓栈：`SharedMemGlobal::Port::~Port` → `WatchTask::remove_port` → `PortContext` shared_ptr 指向垃圾地址（use-after-free）。
4. 交叉：16MB/32MB/仅 segment_size 都崩 → 任何自定义 SHM 描述符都触发。

### 使用的工具
gdb（析构栈）、集成测试交叉（16MB/32MB/segment-only）。

### 定位结论
FastDDS 3.6 内置 SHM WatchTask 与自定义描述符的析构存在 use-after-free。**自定义 SHM 描述符不可用**，Bug 7 的 SHM 段修复路被堵。

### 修改代码
[detmw_fastdds.cpp](thirdparty/detmw/src/detmw_fastdds.cpp) 回退自定义 SHM（保留 `DETMW_UDP_ONLY` 诊断开关）。

### 5W1H 复盘
- **What**：自定义 SHM 描述符析构崩。
- **Where**：FastDDS 3.6 SHM WatchTask/Port 析构。
- **Why**：WatchTask 后台线程与端口析构竞态（use-after-free）。
- **How**：回退；根治需 FastDDS 升级/补丁。

### 第一性原理洞察
**修复一个资源限制问题时，绕路的"正确配置"可能踩中另一层未预期 bug。** 尝试加大 SHM 段是合理的工程动作，但 FastDDS 3.6 在该路径不成熟。**评估第三方中间件时，不仅要测主路径能力，还要测"配置修改路径"的健壮性**——一个修不好的配置 bug 会直接堵死能力扩展。

---

## Bug 7/8 复诊定界（2026-08-03，gdb + 社区确认，推翻部分早期归因）

### 新增证据（最小复现 [fastdds_burst_repro.cpp](quality/test/fastdds_burst_repro.cpp) + 64MB SHM 描述符）

| 场景 | 结果 |
|---|---|
| 默认 SHM 512KB，32K×200 紧突发 | **112/200，gap**（复现 Bug 7） |
| 自定义 SHM 64MB（`useBuiltinTransports=false`），32K×200 | **200/200 零丢** |
| 自定义 SHM 64MB，1MB×1 | **1/1 收到**（推翻"M 级不可行"） |
| `useBuiltinTransports=false` + **默认** SHM 描述符（512KB） | **同样析构 SEGV** |
| `useBuiltinTransports=true` + 追加自定义 64MB 描述符 | **不崩**（但数据走内置 512KB SHM，非 64MB） |

### 定界结论（推翻早期归因）

1. **Bug 7 真因 = 段容量**：默认 `shm_implicit_segment_size = 512KB`（[SharedMemTransportDescriptor.hpp:55](SharedMemTransportDescriptor.hpp#L55)），32K 紧突发 count≥~80 时段满 → 可靠退化为丢。64MB 段下同负载 200/200 零丢 → **纯容量问题，非可靠协议、非消费者速度**。
2. **Bug 8 真因 = SHM-only participant 析构堆损坏**：触发条件是 `use_builtin_transports=false`（SHM-only），**与描述符大小无关**。gdb 栈（完整）：`SharedMemManager::Port::~Port → shared_ptr<SharedMemGlobal::Port> 析构 → SharedMemGlobal::Port::~Port(SharedMemGlobal.hpp:475) → WatchTask::remove_port(SharedMemGlobal.hpp:245) → std::shared_ptr<PortContext>::get(_M_ptr=0x3f9187d24f8287e6 垃圾)`。**`remove_port` 的 `this` 有效（0x5555555a3370），崩在 `watched_ports_` 向量元素被写坏** → 堆损坏，非 static 析构顺序。**方案 A（把 `WatchTask::get()` 换成成员 `watch_task_`）经 gdb 证无效**，作废。
3. **社区确认**：[#6114](https://github.com/eProsima/Fast-DDS/issues/6114)（`useBuiltinTransports=false` + SHM → Ctrl-C SEGV，同样归因 `remove_port`/`watched_ports_`，OPEN 未修）；相关家族 [#3082](https://github.com/eProsima/Fast-DDS/issues/3082)、[#6206](https://github.com/eProsima/Fast-DDS/issues/6206)。master 上 SharedMemGlobal.hpp 自 3.6.0 起仅 uncrustify，未修。

### 决策
自研桶（TransportInterface 扩展，不碰 FastDDS 源码）。桶段 ≥64MB 即可让 32K 突发与 1MB 全零丢（已由上面诊断证明）。FastDDS SHM 在 SHM-only 配置下 teardown 堆损坏，社区一年未修，修它期望值低。

---

## 汇总：本会话故障引入的本质（第一性原理层）

1. **事件驱动语义误读**（Bug 1）：回调=状态通知，不是逐条交付；必须排空。
2. **无基线就"优化"**（Bug 2/7）：动可靠协议参数/信任可靠保证前，必须先有最小复现 + 基线测量。
3. **工具错误伪装系统 bug**（Bug 4）：压测工具与被测系统同源时，先验证工具，再归因系统；症状分散=堆被提前写坏，用 valgrind/ASan 找**首次写坏点**。
4. **黑盒端到端直接归因**（Bug 5）：必须先分层（投递/业务/测量），再下结论。
5. **单一计数指标不可信**（Bug 6）：测量误差会掩盖被测缺陷；先做唯一性验证（去重）。
6. **手动字符串/路径拼接**（Bug 2）：off-by-one 高发，用专门 API。

**贯穿主线**：每一次误判都源于"跳过分层/最小复现/工具验证，直接拿结果归因"。正确路径是：**端到端发现问题 → 白盒分层定位到层 → 最小复现隔离根因 → 工具验证（valgrind/去重）→ 再改**。
