# DTS 工程上下文（当前层）

> 栈式：本文件是当前上下文（新窗口从这里继续），历史在 [docs/worklog/](worklog/)（按日期）。
> 更新入口：会话内输入 `/context`，或直接改本文件。

## 当前任务（ISO 重构）

**阶段：detmw v2 + bootstrap 完成（用户指定 detmw 前置），下一步 infrastructure console 模块。**

## 已建成（可运行，ctest 3/3 过）

- **detmw v2（D10，全 C++）**：`detmw::Communicator` + `detmw::endpoint`（==/hash/ToString），`TransportInterface` 抽象隔离底层 DDS；双 API `publish_external`（进程外）/`publish_internal`（进程内 task→data）
- **bootstrap Run 化**：`dts::Run(cfg)` 阻塞常驻 + `dts::Stop()`；`Process{StopSignal, comm, Worker×3, 订阅}` 自包含生命周期（装配→WaitStop→下电）；装配失败契约兑现（Communicator::good() + Start bool → Run 返回非 0，坏 cfg 实测退出非 0）
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

- [ ] **infrastructure console 模块**（下一步）：CommandExecutor 命令表 + socket 监听线程（人类入口）+ control 线程（执行），低优先级不绑 RT
- [ ] **决策**：subscribe 部分失败策略（当前降级继续，检视 R1 遗留：失败即停 vs 部分降级）
- [ ] **每线程 logger**：契约规划 dts_data/dts_task/dts_log 每线程一 logger，当前单 default logger（文件名 {} 恒为 "dts"），随 contexts 逐层落地切换
- [ ] **验证 R2**：FastDDS `delete_participant` join 接收线程假设（高吞吐压力 + ASan 确认无 UAF）
- [ ] **detmw 收尾**：`publish_internal` mailbox 直通（免序列化）——transport 内部直投 + 接收仍走 OnRouteMsg 统一路由（契约 detmw.md §3，不得旁路订阅回调）
- [ ] detsched / contexts 逐层重写（每层暴露 console 可调运维接口）
- [ ] 32K SHM 段满丢包：FastDDS 3.6 内置 SHM 512KB 过小（自研桶方案搁置，见 worklog 2026-08-03）

## 验证命令

```bash
cmake --build build && ctest --test-dir build   # 全量（integration/cross_process/s_level_perf）
```

## 关键文件

- 组合根：[bootstrap/src/run.cpp](bootstrap/src/run.cpp)（Run/Stop + Process/Worker）
- 日志门面：[log.h](infrastructure/include/log.h) + [log.cpp](infrastructure/src/log.cpp)（`dts_log` target）
- detmw v2：[detmw.h](thirdparty/detmw/include/detmw.h) + [detmw.cpp](thirdparty/detmw/src/detmw.cpp) + [detmw_fastdds.cpp](thirdparty/detmw/src/detmw_fastdds.cpp)
- 传输抽象：[detmw_transport.h](thirdparty/detmw/include/detmw_transport.h)
- 契约：contracts/（五份）
- 设计：[docs/design/dts-strategy.md](design/dts-strategy.md)（D1-D10）+ [docs/design/sub-bootstrap.md](design/sub-bootstrap.md)
- ISO：[docs/iso-output.md](iso-output.md)（Insight/Strategy/Operation）
