# ISO 输出：统一进程间通信中间件 detmw

模式: 新建

## Insight 结构化记录

| 维度 | 结论 |
|------|------|
| 触发事件 | itran 闭源无法外部使用，进程间通信被卡脖子，需自研替代 |
| 用户/客户 | 全基站进程（2 主控 + 5 基带，PCIe 互连），线程级使用（task/data/log 各自通信能力） |
| 当前做法 | itran（闭源静态路由 pub-sub），无法外部化、无法演进 |
| 不做会怎样 | 进程间通信依赖闭源，无法自研扩展，全基站统一通信无从谈起 |
| 长期愿景 | 统一通信中间件（rmw 类似物）：静态路由 pub-sub，传输可插拔（Fast-DDS / rudp / 共享内存 / 零拷贝），单站内 + 未来跨机群 |
| 竞品/参考 | ROS 2 rmw（抽象层思路）；Fast-DDS（底层实现）；我们 = 对齐 itran 静态路由模型 + 传输可插拔 + 单站规模 |

## Strategy 终稿

**定位**：统一进程间通信中间件（deterministic middleware）——静态路由 pub-sub，传输可插拔，替代 itran。

**核心目标（MVP，可验证）**：
1. 统一 API：`(sessionType+sessionInst+msgId) → fn(data,len)`，上层零感知底层传输
2. DDS 传输打通：Fast-DDS 上 2 进程端到端 pub-sub 跑通
3. 线程级能力：每进程 1 Participant，task/data/log 独立 writer/reader + 独立 QoS
4. 静态路由 JSON：运行时加载，每进程一份

**边界**：
- ✅ 做（MVP）：统一 API + Fast-DDS 传输（SHM 进程内 + reliable UDP 进程间）+ 静态路由 JSON + 线程级 writer/reader + 2 进程验证
- ❌ 不做（MVP）：rudp 独立通道（列为能力对照评估项）、共享内存/零拷贝（Fast-DDS 原生已含）、跨站、动态复杂拓扑

**成功标准**：
- 2 进程跨板卡端到端 pub-sub 跑通
- 上层只调统一 API，不感知 DDS 细节
- task/data/log 各自 QoS 独立生效
- JSON 配置运行时加载生效（同一套二进制，不同进程加载不同 JSON）

**关键假设**：
- Fast-DDS 最新稳定版可编译集成（Apache 2.0，暂不卡许可）
- PCIe 虚拟网口支持 Fast-DDS 收发；多播受限则 Discovery Server 兜底
- Fast-DDS 可靠 UDP 覆盖 ZTE rudp 能力（需能力对照表评估，覆盖则砍 rudp 通道）

**盲点检查结论**：
- 发现：PCIe 多播验证 → 受限则 Discovery Server
- 存量迁移：回调模型对齐 itran（session+msgId→fn），迁移平滑
- 线程级并发：Fast-DDS 原生 SHM/UDP transport selection，每进程 1 Participant 成立
- 回滚：传输抽象层下换底层不改上层 API（MVP 即验证此承诺）
- 许可：暂不卡，先跑通

## Operation 规划

| 阶段 | 核心里程碑 | 输出 |
|---|---|---|
| 0 定约 | detmw 定位/命名/范围 | 本文档 |
| 1 MVP | 统一 API + Fast-DDS + 静态路由 JSON + 线程级 + 2 进程验证 | libdetmw v1 |
| 2 发现与接入 | Discovery Server + 全基站接入 + rudp 能力评估 | 单站全通 |
| 3 传输扩展 | rudp/零拷贝按需接入，大规模跨机 | 多传输可插拔 |

**MVP 里程碑**：Fast-DDS 集成 → 统一 API → JSON 加载 → 线程级 + QoS → 发现验证 → 2 进程跑通。

**资源估算**：MVP 约 2~3 人月。

**风险 Top 3**：Fast-DDS 集成复杂度（独立 demo 先验证）；PCIe 多播/发现不通（Discovery Server 兜底）；线程级并发性能（单线程先行）。

**验收决策者**：中间件负责人（统一 API/传输抽象）；dts 业务（2 进程端到端 + 线程级 QoS）；架构组（替代 itran 平滑性）。

---

下游：detmw design 阶段加载。
