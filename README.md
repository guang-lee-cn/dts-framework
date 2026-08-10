# dts-framework

基于 Fast-DDS 的确定性通信中间件框架（C++17）：限界上下文分层 + 确定性调度 + 控制面 + 数据工厂。

## 架构

```
外部:  cmd(运维, dts CLI)        外部服务(nfoam/SPA/网管, DDS)
          │                            │
detmw  [Communicator / TransportInterface]   ← DDS 收发 + 控制通道
          │  订阅回调 OnRouteMsg（统一路由）
          ▼
infra  [mailbox]  [dts::log 门面 + TsRotatingSink]  [console/control/CommandExecutor]
          │
contexts:  task / data / log（每 context = 1 detsched 线程）
          │
detsched [线程工厂 / 调度域 / 注册表]   ← 承载业务线程
          ▲
bootstrap [Run/Stop + Process]   ← 装配/下电/生命周期
```

- **detmw**：进程间通信中间件，`TransportInterface` 隔离 DDS（换实现只改一处）
- **infrastructure**：mailbox / 日志 / 控制面（console socket + control 执行 + 命令表）
- **contexts**：task/data/log 三个限界上下文，每 context = 一个 detsched 线程，内部三层（interface → application → domain）
- **data 数据工厂**：rawData → 500 dataId 切片 → 合并上报（ReportAggregator）
- **detsched**：确定性调度（SCHED_FIFO/RR 域，BKG 低优先级运维）

## 关键能力

| 模块 | 能力 |
|---|---|
| detmw v2 | 双 API（publish_external DDS / publish_internal 进程内）+ 统一 endpoint 寻址 + TransportInterface 隔离 |
| 日志 | dts::log 门面（业务零 spdlog 直接调用）+ TsRotatingSink（时间戳文件+5MB 切分+总量删旧）|
| 控制面 | CommandExecutor 命令表 + console socket（AF_UNIX 长连接）+ control 执行线程 + `dts-cli.py`（login REPL / exec / ps）|
| data 数据工厂 | 500 dataId 切分（4-200B 随机）+ DataMemManager 静态池（hash 槽位 + TTL）+ ReportAggregator 合并上报 |
| 性能 | data 收 950 raw/s（30 MB/s，raw 32K），瓶颈定位 FastDDS deserialize（见 [bench 报告](docs/design/data-bench-report.md)）|

## 构建

```bash
cmake -B build && cmake --build build -j
ctest --test-dir build              # integration / cross_process / s_level_perf
```

依赖：Fast-DDS 3.x（/usr/local）、spdlog（pkg-config）、C++17。

## 运行

```bash
# dts 进程（cfg = gen_detmw.py 产物）
./build/cpf_dts build/generated/detmw/cpf-dts/cpf-dts.json
# 另终端：登录 console（交互）
python3 tools/dts-cli.py login cpf-dts
# dts> help / get_threads / set_log_level debug
```

## 文档

- [总体架构 + 子系统接口](docs/design/dts-architecture.md)
- [data 子系统设计](docs/design/data-subsystem.md)
- [data bench 报告](docs/design/data-bench-report.md)
- [当前上下文 / 进展](docs/CONTEXT.md)
- [决策记录](docs/design/dts-strategy.md)（D1-D10）

## License

[MIT](LICENSE)
