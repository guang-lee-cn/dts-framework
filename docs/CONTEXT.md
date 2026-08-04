# DTS 工程上下文（当前层）

> 栈式：本文件是当前上下文（新窗口从这里继续），历史在 [docs/worklog/](worklog/)（按日期）。
> 更新入口：会话内输入 `/context`，或直接改本文件。
> 故障复盘：本会话踩到的全部 bug 见 [docs/bugs.md](bugs.md)（含定位流程/工具/5W1H/第一性原理）。

## 当前任务

detmw 生成路由接入 + 多进程静态发现 + S 级压测评估，已收口。S 级（1条/s×32K）达标；task 阶段性突发不可靠（FastDDS 可靠突发丢包 + 堆损坏），留作已知问题。

## 已建成（可运行）

- **detmw 配置驱动**：`detmw_init(cfg)` 加载生成 JSON，EDP=STATIC 静态发现（加载 `staticdiscovery.xml`），端点显式 entity_id/user_defined_id，预建 writer
- **生成器** [gen_detmw.py](thirdparty/detmw/tools/gen_detmw.py)：配置组 → 线程路由头（Sub/Pub 分表）+ 进程生成配置 + 组 staticdiscovery.xml
- **多进程**：`cpf_dts`/`dpf_dts` 独立 target；`dts_test` 测试进程
- **模块拆分**：`bootstrap/`（组合根，无产物）+ `infrastructure/`（含线程运行时 dts_thread）+ 删 dead 的 thread_base/itran_pubsub
- **测试链路**：nfoam(→task→回包) + spa(→data 32K) + dts_test，静态发现互发互收
- **S 级压测链路**：perf-gen(32K 突发) → dts(task→data→上报回抛) → perf-sub(分段耗时+丢包+CSV)
- **ctest 3 项全过**：`integration` + `cross_process` + `s_level_perf`（1条/s×32K 回归）
- detsched RT 降级：非 root 环境 EPERM 回退 SCHED_OTHER

## 关键决策

- 路由表按线程拆分 Sub/Pub（`kDataSubRoutes` 等），RouteToThread 遍历注册替代手写 MSG_ID
- detmw 全 RELIABLE + KEEP_LAST（depth 100→1000，**默认 timing，调参曾引入丢包已回退**）
- 静态发现用 FastDDS 默认 v2 交换格式；topic 名沿用 `sessionType_sessionInst_msgId`
- S 级数据上报通路：data 线程收 task 激活即回抛 msg4（data_msg_handler 加 1 处，flag 过）

## 已知问题（开放）

1. **32K 紧突发丢包 = SHM 段容量**（2026-08-03 定界）：默认段 512KB，32K 紧突发 count≥~80 时段满丢；**64MB 段下 32K×200 → 200/200 零丢、1MB×1 → 1/1 收到**（[复诊](docs/bugs.md)）。但 FastDDS SHM **SHM-only（`use_builtin_transports=false`）析构堆损坏**（Bug 8，与描述符无关，gdb 证 `watched_ports_` 元素写坏，社区 #6114 open 未修）→ **自定义描述符加大段的路被堵死，决策走自研桶**
2. **单订阅者 topic 双投递（已确认复现）**：单订阅者 topic（msg8 响应等）每条响应双投（bench_nfoam 去重前 100/50）；速率相关（紧突发时减少）。**真实数据流会有重复上报**
3. **TaskManager kMaxTasks=64**：业务上限，第 65+ 条任务被拒——**非中间件问题**；配置变更 >64 条需业务侧定扩容
4. **data 多业务并发**：结构上可承受（mailbox 有锁），未按真实并发规模压测

## 基准结论（2026-08-02，[docs/dds_bench_2026-08-02.md](dds_bench_2026-08-02.md)）

- **链路可靠**：S级（1/s×32K）零丢；256B/4K 突发零丢；写入侧吞吐 16-17k msg/s（32K 突发 529 MB/s）
- **时延**：节奏化（1ms）task 段 ~0.5ms / data 段 ~36us；突发 task 段 ~3.3ms
- 曾误判的"堆损坏/崩溃" = perf_gen 的 `PerfInit` 固定 memset 32768 溢出小包（**已修**，非中间件 bug）

## 真实业务基准（2026-08-03，[docs/dds_bench_2026-08-03.md](dds_bench_2026-08-03.md) 含图 + 测试表 + 丢包定位）

**A. task 双向收发**（nfoam 发 32K JSON → task parse+响应 → nfoam 收，含 >64 变更）：
- **零丢到 2000 条**（紧突发/1ms 都零丢）；往返 avg ~430-550us
- **>64 变更场景无问题**（task 消费够快，可靠背压生效）
- 单订阅者响应 topic 双投（dup 确认）

**B. data 接收**（spa 发 ≤32K struct → data 4字节拷贝模拟）：
- **零丢到 500 条**（任何速率）
- **周期+突发夹杂**（datamix）：50 周期 5/10ms + 夹 burst 20/50/100 紧突发 → **全零丢**（251/551/1051）

**task 周期包**（run_task_bench 加 10ms/100ms）：全零丢，往返随周期升高（1ms~500us、10ms~1.3ms、100ms~2.2ms）

**关键洞察**：32K SHM 丢只在"慢消费者"出现（task-create 慢→丢；parse/拷贝快→零丢）。**消费者 drain 速度决定是否触发 SHM 512KB 段满**。

## M 级消息（1MB+/10M）结论（2026-08-03）

- **1MB 单条在 64MB SHM 段下可收**（1/1，2026-08-03 复测）——早期"收不到"是 512KB 段容量限制，非分片/类型问题
- **10M 当前配置不可行**：需类型上限（BytesType 64K→更大）+ 大段（自研桶 64MB+ 可满足传输侧）
- **结论**：传输侧（段大小）由自研桶解决；类型侧上限需另改，与传输无关

## 静态 vs 动态发现对比（2026-08-02，`DETMW_DYNAMIC=1` 开关）

| 维度 | 静态 | 动态 |
|---|---|---|
| 稳态时延 task段 | 518us | 577us（噪声内无差） |
| 稳态时延 data段 | 36.7us | 36.5us（无差） |
| 单订阅者双投递 | **有**（收102/发50） | 无（51/50） |
| 32K 突发 | 间歇丢 | 持续严重丢（33-49/50） |

**结论**：静态 EDP 只改发现阶段，**稳态数据时延无提升**（实测无差）；静态 vs 动态的差异主要是 32K 突发（静态间歇、动态严重），双投递曾观测现不复现。静态的真正价值（跨机/无多播的确定性匹配）单机测不出来，需 Discovery Server / 跨机验证才有意义。

## 下一步（精确，可勾选）

- [ ] **自研桶（TransportInterface 扩展）**（最高优先，决策已定）：≥64MB 大段桶替代 FastDDS SHM，根除 Bug 7/8；方案 A（WatchTask 补丁）已 gdb 证无效作废。先做诊断复现定界已完成（见 docs/bugs.md 复诊节），下一步搭 bucket transport 骨架
- [ ] **双投递待确认**：曾观测到单订阅者双投，当前不复现；真实数据流若出现重复上报再深查
- [ ] task 突发 >64 条：业务侧决定 kMaxTasks 扩容
- [ ] 按真实规模压测 data 多业务并发（多 publisher 同 topic）
- [ ] 跨机发现（Discovery Server / initialPeers），`peers` 字段接线
- [ ] 评估 detmw 的 UDP-only / DETMW_DYNAMIC 诊断开关 + reader/writer 计数去留

## 关键文件

- 生成器：[gen_detmw.py](thirdparty/detmw/tools/gen_detmw.py)
- detmw 传输：[detmw_fastdds.cpp](thirdparty/detmw/src/detmw_fastdds.cpp)（listener 排空/QoS/静态发现）
- detmw 配置层：[detmw.c](thirdparty/detmw/src/detmw.c)
- 组合根：[bootstrap/src/dts_startup.cpp](bootstrap/src/dts_startup.cpp)
- 线程运行时：[infrastructure/src/dts_thread.cpp](infrastructure/src/dts_thread.cpp)
- 压测：[perf_gen.cpp](quality/test/perf_gen.cpp) / [perf_sub.cpp](quality/test/perf_sub.cpp) / [run_perf.sh](quality/test/run_perf.sh)
- 配置：config/detmw/dts/{cpf,dpf}/、quality/test/config/{dts,perf}/

## 验证命令

```bash
cmake --build build && ctest --test-dir build          # 全量（integration/cross_process/s_level_perf）
# 手动 S 级压测（1条/s×32K，默认 SHM）：
quality/test/run_perf.sh build/quality/dts_test build/quality/perf_gen build/quality/perf_sub \
  build/generated/detmw-perf 5 /tmp/perf.csv 3 32768 1000000
# 突发压测（256B×200 紧突发，复现丢包/崩溃）：
quality/test/run_perf.sh build/quality/dts_test build/quality/perf_gen build/quality/perf_sub \
  build/generated/detmw-perf 200 /tmp/perf.csv 3 256 0
```
