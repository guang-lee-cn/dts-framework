# DTS 工程上下文（当前层）

> 栈式：本文件是当前上下文（新窗口从这里继续），历史在 [docs/worklog/](worklog/)（按日期）。
> 更新入口：会话内输入 `/context`，或直接改本文件。

## 当前任务（ISO 重构）

**阶段：迭代 2026-08-17 完成——确定性预算基石 + raw 零拷贝通道 v1 + 决策点 1 收敛（FastDDS 升级已排除，[评估](design/fastdds-upgrade-assessment.md)）+ 自研桶接入 detmw（DETMW_BUCKET=1，shm 不可用 fail-fast）+ bucket 测试纳入 ctest（环境自适应）+ **P0-1 上报帧超容截断修复（kCap 32K→48K，500 dataId 全量 34778B 零丢弃单测）+ P0-2 RT 严格模式（DTS_STRICT_SCHED=1 fail-fast，实测 exit=1）+ P1-1 预算阈值可配置（DTS_DATA_BUDGET_*_US 环境变量 + set_data_budget 命令）**。容量核算：100 帧 32K/100ms（1000 帧/s）超接收上限 950 帧/s ~5%。**待真实环境（/dev/shm）验证：按 [commercial-validation-checklist.md](design/commercial-validation-checklist.md) 执行；待办：P1-2 指标出口（OAM 业务组）、P1-3 远程控制通道（D2/R5）**。事件驱动语义确认：周期 tick 由外部 pub 控制，线程体现周期处理。**

## 已建成（可运行，ctest 11 项：5 单测 + integration + cross_process + plain_smoke + s_level_perf + bucket×2(环境自适应)）

- **raw 零拷贝通道 v1（2026-08-17）**：`FixedBytesType` 定长 plain 类型（配置 `plain_size`，严格定长，memcpy 序列化）+ 发送 loan_sample 尝试/回退 + 接收定长预分配；per-topic 类型表（缺省 BytesType 兼容）；类型所有权修复（TypeSupport 唯一拥有）；设计/决策点见 [docs/design/raw-zero-copy.md](design/raw-zero-copy.md)；验证：unit_fixed_type + plain_smoke（双进程 1KB 定长收全量）

- **detmw v2（D10，全 C++）**：`detmw::Communicator` + `detmw::endpoint`（==/hash/ToString），`TransportInterface` 抽象隔离底层 DDS；双 API `publish_external`（进程外）/`publish_internal`（进程内，**mailbox 直通已落地**：命中本进程订阅者免 DDS 序列化，见 contracts/detmw.md §3）
- **确定性预算（2026-08-17）**：ThreadRun **绝对期限节拍**（持续负载下 TIMER 准点，修 TTL 停摆 bug，unit_thread_tick 回归）；data 单帧处理耗时打点（3ms 预警/5ms 告警，口径=线程内含业务代码）；静默丢弃计数（池满/超容/键失败）；console `get_data_stats` 查询（实测：30 帧 max 1.5ms 含预热，稳态 0.4ms，warn/alarm=0）
- **三级消息路由（2026-08-16）**：sessionType 线程内固定 → sessionInst = **业务组**（线程可多组，mailbox 携带，借指针零拷贝）→ msgId 组内具体业务；第三层在 `{task|data|log}_msg_handler.cpp` 业务组数组表驱动（`{msgId, func(void* data, uint32_t datalen)}` 一行一消息流），`MsgTable`/`FindSessionTable`（msg_table.h）查表；编译期护栏 MsgIdsUnique + SessionGroupsValid；console `get_handlers` 按组可查
- **bootstrap Run 化**：`dts::Run(cfg)` 阻塞常驻 + `dts::Stop()`；`Process{StopSignal, comm, Worker×3, 订阅}` 自包含生命周期（装配→WaitStop→下电）；装配失败契约兑现（Communicator::good() + Start bool → Run 返回非 0，坏 cfg 实测退出非 0）
- **console 控制面（2026-08-05 落地）**：`dts::ctl` 命令表（CommandRegistry/Execute）+ `console_start/stop`（AF_UNIX 长连接会话 socket 线程）+ `control_start/stop`（执行线程，BKG 低优先级）；内置 help/get_threads/set_log_level；bootstrap 装配起停；配套 [dts-cli.py](tools/dts-cli.py)（`login` REPL / `exec` 单条 / `ps` 发现），实测通过
- **契约**：contracts/ 五份（detmw/infrastructure/contexts/bootstrap/detsched）
- **日志门面 dts::log**（已落地）：log.h/log.cpp + TsRotatingSink（target `dts_log`），变参模板转发 spdlog 保编译期检查；detmw/detsched/infrastructure/agent 全走门面，业务代码零 spdlog 直接调用；console+file 双 sink（时间戳文件+5MB 切分+总量 5G 删旧）；Run 返回回收线程池（5b44aca）
- **远程**：github.com/guang-lee-cn/dts-framework（private），refactor + master 分支

## 重构关键决策

| # | 决策 |
|---|---|
| D10 | detmw 不独立 so、全 C++、TransportInterface 隔离底层、endpoint 统一寻址（删 SessionKey） |
| D7/D8 | 进程内 mailbox 直通（publish_internal）+ 进程外 DDS（publish_external），调用方选 API 零查表 |
| D9 | 日志自定义 TsRotatingSink（继承 base_sink） |
| D5/D6 | 运维命令只进 control 线程，业务线程不加运维分支；调试指令突发不加锁，外发版本 DTS_CONSOLE_ENABLE 裁剪 |
| 命名 | 函数入参 ≤5 已入规范；StartUp/ShutDown → Run/Stop |

## 已知问题 / 待办

- [ ] **data 子系统业务化**（框架/性能链路已通，见 worklog 2026-08-09）：真实 dataId 业务模型替换 mock 切片；web 落地接 kafka（当前 CSV 占位）
- [ ] **决策**：subscribe 部分失败策略（当前降级继续，检视 R1 遗留：失败即停 vs 部分降级）
- [ ] **每线程 logger**：契约规划 dts_data/dts_task/dts_log 每线程一 logger，当前单 default logger（文件名 {} 恒为 "dts"），随 contexts 逐层落地切换
- [ ] **验证 R2**：FastDDS `delete_participant` join 接收线程假设（高吞吐压力 + ASan 确认无 UAF）
- [ ] **detsched get_threads prio 显示**：业务线程 fallback 路径句柄 m_prio=0，get_threads 显示 prio=0（console 已落地，此项是 detsched 展示问题）
- [ ] **raw 序列化突破**（突破 30 MB/s）：plain struct 固定大小 + DataSharing loan 零拷贝；或 raw 分片绕 deserialize（沙箱/无 SHM 环境 32K UDP 不可达，S 级已支持 DTS_RAW_LEN 调小验证）
- [ ] **gtest/lcov/静态检查**：当前为自研断言单测（quality/unit/），可评估迁 gtest 补覆盖率
- [ ] detsched / contexts 逐层重写（每层暴露 console 可调运维接口）

## 验证命令

```bash
cmake --build build && ctest --test-dir build   # 全量（integration/cross_process/s_level_perf）
```

## 关键文件

- 组合根：[bootstrap/src/run.cpp](bootstrap/src/run.cpp)（Run/Stop + Process/Worker）
- 日志门面：[log.h](infrastructure/include/log.h) + [log.cpp](infrastructure/src/log.cpp)（`dts_log` target）
- console 控制面：[ctl.h](infrastructure/include/ctl.h) + [console.h](infrastructure/include/console.h) + [console.cpp](infrastructure/src/console.cpp)（命令表 + console/control 线程）
- 分窗口启动栈：data → [CONTEXT-data.md](CONTEXT-data.md)（console 已落地，其启动栈 [CONTEXT-console.md](CONTEXT-console.md) 记为完成记录）
- detmw v2：[detmw.h](thirdparty/detmw/include/detmw.h) + [detmw.cpp](thirdparty/detmw/src/detmw.cpp) + [detmw_fastdds.cpp](thirdparty/detmw/src/detmw_fastdds.cpp)
- 传输抽象：[detmw_transport.h](thirdparty/detmw/include/detmw_transport.h)
- 契约：contracts/（五份）
- 设计：[docs/design/dts-strategy.md](design/dts-strategy.md)（D1-D10）+ [docs/design/sub-bootstrap.md](design/sub-bootstrap.md)
- ISO：[docs/iso-output.md](iso-output.md)（Insight/Strategy/Operation）
