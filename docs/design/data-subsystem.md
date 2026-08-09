# data 子系统架构设计（ISO 输出）

> 日期：2026-08-05 · 模式：重构（现状演示骨架 → 真实数据工厂）· 状态：ISO 定稿，进入实现
> 依据：dts-architecture.md §5.2（预留接口 + 契约）、contracts/contexts.md、用户设计讨论

## 0. Insight（洞察）

| 维度 | 结论 |
|------|------|
| 触发事件 | 数据工厂是 DTS 核心承载，现状演示骨架（2 dataId + 模拟耗时）无法支撑真实业务，需真实化 |
| 用户/客户 | 数据消费方：远端 web/网管（周期数据）；提供方：SPA/业务方（rawData 大结构）；编排方：task（任务订阅） |
| 当前做法 | 演示骨架：cell_prb/ue_bler 2 dataId，DTS_DATA_SLOW 循环拷贝模拟耗时，无真实数据模型 |
| 不做会怎样 | 数据工厂停在演示，无法承载真实业务上报，DTS 不可落地 |
| 长期愿景 | 任务驱动订阅、dataId 切片加工、周期上报、可运维可扩展（多域） |
| 竞品/参考 | 网元北向数据采集；数据面管线（DPDK 零分配 / 时间轮 / 批处理） |

## 1. 业务架构（业务目标）

**定位**：data 线程是 DTS 的**数据中枢**——接收 SPA/业务方原始测量数据（≤32k 大结构），按 dataId 切片加工，按任务（订阅）过滤分发，周期上报远端 web/网管。

**业务目标**：
| 目标 | 说明 |
|---|---|
| 原始 → 结构化切片 | rawData 按 dataType 分族，按 dataId 切出独立测量片段（CellSch.sch_ul 等） |
| 任务驱动订阅 | task 声明"跟踪哪些 dataId"，多个任务合并为激活并集，工厂只处理激活集 |
| 周期上报 | 1S / 5S 周期，任务指定周期（ms），定时器统一推送 |
| 低延迟不阻塞 | data 线程零动态分配、批处理（Extra 全加工 → Report 统一推） |
| 可运维 | console 查询（线程/任务/内存占用） |

## 2. 分层架构（模块分层 v2）

```
infrastructure（基础设施）
  TimerWheel        时间轮：1s/5s 周期定时，tick 驱动（漂移/卡死已处理）

interface（接入层）
  DataEntry         data 线程入口：转发到 application，不碰业务

application（消息编排）
  DataMsgHandlerDispatch   按 msgId 直分（任务消息/数据帧/定时）
  OnTaskActive             任务消息（建/改/删）→ TdMap 更新（无锁，data 线程内）
  OnAgentData              rawData 帧 → DataFactory 批处理
  定时上报                  1s tick → 遍历 task 上报缓存 → 推远端 → 清空

domain（业务域，零技术依赖，走 MwPort）
  TdMap                   任务信息库：taskId→(dataIdList/周期/扩展参数)；
                           dataId→taskId 集（上报分发）；激活 dataId 并集；
                           上报过滤 (dataId, 测量对象)→task。无锁（data 线程独占）
  DataFactory             数据工厂：rawData → dataType 路由 → 激活集 → 加工类命中
                           → Extra 全加工 → 周期到 Report 分发 task 上报缓存
  ExtractorRegistry       REGISTER_DATAID_EXTRACTOR 宏（全大写），静态全集，无动态口
  C<CacheName>            加工基类：默认 Extra / Hton / Report；用户重写则覆盖
  CacheHead               测量对象块头：dataType + union{CellHead{cellId,cpId}, UeHead{cellId,ueId}, ...}
  DataMemManager          内存管理专门类：加工缓存池 + 上报缓存池（业务语义见 §3.3）

外部契约
  rawData 公共结构        DataHeader{dataType,...} + CellData/UeData 全量（dts/spa 共识）
  dataId 私有切片          CellSch/CellRlc/CellPhy/UeSch/UeRlc...（dts 概念，spa 不关心）
  ReportHeader            上报报文头 {taskId, timestampMs, seq, payloadLen}
```

## 3. 数据架构

### 3.1 入数据：rawData（≤32k，dts/spa 共识）

```cpp
struct DataHeader { DataType dataType; /* cellId/cpId/ueId 等测量对象 key */ };
struct CellData { DataHeader header; CellPhyData phyData; CellMacData macData; int sch_ul,sch_dl,rlc_ul,rlc_dl; };
struct UeData   { DataHeader header; int mcc,mnc; UePhyData phyData; UeMacData macData; int sch_ul,sch_dl,rlc_ul,rlc_dl; };
```

### 3.2 切片：dataId（dts 私有，spa 不关心）

```cpp
// dataId=1 CellSch {int sch_ul, sch_dl;}    ← Extra 从 CellData 切
// dataId=2 CellRlc {int rlc_ul, rlc_dl;}
// dataId=1000 UeSch {int sch_ul, sch_dl;}   ← Extra 从 UeData 切
```

### 3.3 存储：DataMemManager 管两块（业务语义）

| 池 | 语义 | 布局 | 生命周期 |
|---|---|---|---|
| 加工缓存池 | **测量对象当前值**（数据源） | baseAddr 静态池，域×周期分块，块=[CacheHead][dataId槽]，hash64 寻址 | 常驻；**超周期 TTL 清理**（槽超 N 周期未更新→失效归还） |
| 上报缓存池 | **待推送快照**（副本） | per-task，ReportHeader + payload | 1s 定时推远端→清空 |

**关系**：加工缓存是源、上报缓存是快照，`Report` 做"源→快照"拷贝；上报集合 = **加工缓存 ∩ 当前激活集**（陈旧 dataId 不上报）。

### 3.4 寻址

- hash64 键从 `CacheHead` 提取：cell 域 `(uint64)cellId<<32 | cpId`，ue 域 `(uint64)cellId<<32 | ueId`（**默认实现**）
- **预留配置接口**：CacheHead 布局与 hash 键提取集中在一个 config（如 `data_config.h` 的宏/结构），改一两行即换用户自定义布局/键；运行时配置可经 console set
- 槽位 = 静态数组（cell 384 / ue 2000 上限，**各域独立容量**），线性探测，运行期零分配

### 3.5 上报数据流结构（每 task ≤32k）

```
总头 ReportHeader{taskId, timestampMs, seq, payloadLen}
  + n × (子头 SubHeader{dataId, len} + dataId 数据)     ← dataId 序列
```

### 3.6 容量核算（所有 dataId 按最大 400B，CacheHead 64B）

| 场景（100 dataId 分布） | cell 384 | ue 2000 | 合计 |
|---|---|---|---|
| 平均（cell 50/ue 50，周期各半） | 2×(384×~10KB)≈7.7MB | 2×(2000×~10KB)≈40MB | ~48MB |
| 全 cell | 384×(64+100×400)≈15.4MB | 0 | ~15MB |
| 全 ue | 0 | 2000×(64+100×400)≈80MB | ~80MB |

加工缓存池 **15~80MB**，加上报缓存池（per-task 32K×任务数）总量 **<100MB**（上限可配）。

## 4. 业务流转（图）

### 4.1 任务流（低频，mailbox 驱动）
```
task 线程 → publish_internal → OnRouteMsg → data mailbox → OnTaskActive
  → TdMap 更新（建/改/删任务，data 线程内无锁）
  → 激活 dataId 并集重算（task123{1,2,3}+task456{2,3,4} → {1,2,3,4}）
```

### 4.2 数据流（高频，批处理）
```
spa/业务方 → DDS → OnRouteMsg → data mailbox → OnAgentData
  → DataFactory: rawData → dataType 路由 → 通道内激活集
      → 命中 ExtractorRegistry 加工类 → Extra(rawData→槽位)   [阶段一：全加工]
      → 周期到？ → Report(加工缓存∩激活集 → task 上报缓存)    [阶段二：统一分发]
```

### 4.3 上报流（1s 定时）
```
TimerWheel 1s tick → 遍历各 task 上报缓存 → publish_external(REPORT) → 远端 → 清空
```

### 4.4 清理流
```
超周期 TTL：槽超 N 周期未更新 → 失效归还槽位
任务结束：清 TdMap dataId 列表 → 加工缓存入 free list 复用（新任务直接占用）
3 天无任务：DataMemManager 后台清理空闲段（预留，配置驱动）
```

## 5. 类设计与代码架构

```cpp
// domain 核心类（data 线程内独占，无锁）
class TdMap {
    // taskId → TaskInfo{dataIdList, periodMs, taskType, 扩展参数}
    void UpsertTask(uint32_t taskId, const TaskInfo&);   // OnTaskActive 调
    void RemoveTask(uint32_t taskId);
    // 上报过滤：某测量对象的 dataId 数据 → 跟踪它的 taskId 集
    std::vector<uint32_t> TasksOf(uint16_t dataId, uint64_t objKey) const;
    // 激活集：所有任务跟踪的 dataId 并集
    std::vector<uint16_t> ActiveDataIds(uint16_t dataType) const;
};

class DataFactory {
    // rawData 帧 → 批处理（dataType→激活集→加工类→Extra→周期到→Report）
    void Process(const uint8_t* raw, uint32_t len);
};

// 注册表（静态全集，初始化构建两级路由：dataType→dataId→Extra）
// 宏：REGISTER_DATAID_EXTRACTOR(dataId, C<Cache>, cacheStruct, addr, dataType, needcache)
class ExtractorRegistry {
    static ExtractorRegistry& Instance();
    void Register(uint16_t dataId, uint16_t dataType, const ExtractorSpec& spec);
    const ExtractorSpec* Find(uint16_t dataType, uint16_t dataId) const;
};

// 加工基类（默认实现三件；用户重写则覆盖）
class C<CacheName> {
public:
    virtual int Extra(void* rawData, void* cache);   // 默认：按注册偏移拷贝（或空）
    virtual int Hton();                              // 默认：本机字节序（无操作）
    virtual int Report();                            // 默认：cache→上报缓存
};

// 内存管理（专门类，业务语义清晰）
class DataMemManager {
    // 加工缓存池：域×周期分块，hash64 槽位，TTL 清理
    void* Acquire(uint16_t dataId, uint64_t objKey);   // 定位槽位（零分配）
    // 上报缓存池：per-task 快照
    void* ReportSlot(uint32_t taskId);
};

// 基础设施
class TimerWheel {   // 时间轮：tick 驱动，漂移/卡死已处理
    int AddPeriodic(uint32_t periodTicks, TickFn fn);
    void Cancel(int handle);
    void Tick(uint64_t nowTick);
};
```

## 6. 定时器设计（TimerWheel）

- tick 粒度 100ms（ThreadRun `wait_until` 超时驱动），1s=10 tick，5s=50 tick
- 漂移：`nowTick` 按 `steady_clock` 绝对差推进，到期时间每次设相对 now，调度延迟不累积
- 卡死：回调 try/catch + 不阻塞约定 + wait 带 stop 谓词
- 无跨线程：完全 data 线程内驱动，不额外建线程

## 7. 方案定稿 / 模拟数据（MVP 打通通道）

**模拟 dataId**：100 个（cell 50 / ue 50，各 1S/5S 周期一半），cache 结构自拟（示例几个，其余按相同模式扩展）。

**默认实现 + 配置接口**：
- CacheHead / hash64 默认值集中 `data_config.h`，改一两行可换自定义（预留运行时 console set）
- 上报结构 = `总头 + n×(子头 + dataId)`（§3.5）
- TTL 默认 N=3 周期（可配）
- 域容量独立（cell 384 / ue 2000）

**待实现确认（Operation 阶段逐个定）**：
| # | 项 |
|---|---|
| 1 | dataId 清单落 .h（100 个结构 + Sizeof()） |
| 2 | CacheHead union 最终字段（含 TTL 时间戳/周期计数） |
| 3 | 上报远端 web 连接细节（现 publish_external(REPORT) 已通，payload 按 §3.5） |

## 8. Operation（运营规划）

**阶段划分**（每阶段独立可验证）：

| 阶段 | 内容 | 里程碑 |
|---|---|---|
| 1 数据模型 | dataId 清单 .h（100 模拟）+ CacheHead + 上报结构 + data_config.h 配置接口 | 编译过，结构齐 |
| 2 内存层 | DataMemManager：加工缓存池（hash64 槽位 + TTL）+ 上报缓存池 | 槽位分配/清理单测 |
| 3 工厂核心 | ExtractorRegistry + DataFactory 批处理 + C\<Cache\> 基类 + 2 示例加工类 | 模拟 rawData → 槽位 |
| 4 任务库 | TdMap 任务消息 → 任务库 + 激活集 + 上报过滤 | task 消息建任务 |
| 5 定时上报 | TimerWheel 接入 data 线程 + 上报推送远端 | 1s 周期推送 |
| 6 打通验证 | spa mock 模拟 rawData → 全链路 → 远端收到 + console 查询 | 端到端跑通 |
| 7 完善 | TTL 清理 / free list 复用 / console set 配置命令 | 可运维 |

**Top 3 风险**：
1. hash64 槽位冲突退化（线性探测满）——预分配容量 + 探测上限 + 冲突统计
2. 批处理耗时超帧预算（100 dataId Extra 串行）——阶段 6 实测，超预算则拆分
3. 上报 ≤32k 超限（n 个子头累积）——超限拆分多条或按优先级丢弃（策略待定）

**验收**：模拟 rawData 全链路（spa → data → 加工 → 远端）+ ctest 不回归 + console 查询 data 线程/任务/内存。
**资源**：个人项目，阶段串行。
