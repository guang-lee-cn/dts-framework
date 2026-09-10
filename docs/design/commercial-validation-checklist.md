# 商用落地验证清单（Checklist）——真实环境执行

> 2026-08-17 · 适用：5G 基站数据采集中间件落地验收（data 100 帧 32K/100ms 负载模型）
> 沙箱已完成的验证不重复（本清单 = 沙箱不可达项 + 商用专项，全部需 /dev/shm 与目标硬件）
> 2026-09-10 修订：§1 测试数 11→12（补 `web_smoke`）并更新已在开发机转绿的项；桶/MTU 相关结论见 [worklog 2026-09-08](../worklog/2026-09-08.md)

## 0. 环境准备

- [ ] 目标硬件（x86_64 / ARM64）安装 FastDDS 3.6.2 + fastcdr（与 /usr/local 统计版一致）
- [ ] `/dev/shm` 可写且容量 ≥ 2×桶段（`df -h /dev/shm`；建议 tmpfs 挂载 ≥ 256MB）
- [ ] 确认 RT 调度权限：`ulimit -r`（无限）或进程具 CAP_SYS_NICE；无权限时**预期** detsched 降级 SCHED_OTHER + Warn（商用需 fail-fast 改造，见 §6）
- [ ] `cmake -B build && cmake --build build -j$(nproc)` 零警告

## 1. 功能回归（全量 ctest，含 bucket 真跑）

- [ ] `ctest --test-dir build --output-on-failure` —— 预期 **12/12** 过
  - **前置**：`bash tools/redpanda_up.sh`（`web_smoke` 依赖 Kafka，broker 未起则该测试失败，非代码问题）
  - `bucket_smoke` / `bucket_rtps`：2026-08-20 起开发机已 **Passed**——桶段多写者互斥 + 32K RTPS 端到端零丢；目标机属复验，非从 Skipped 转绿
  - `s_level_perf`：2026-09-08 起开发机已 **Passed**（32K tune 链路零丢包）；其成立依赖 `DETMW_UDP_MTU=1400`，目标机 LAN 下须复验 `DETMW_UDP_MTU` **默认关闭态**（IP 分片可用则不需该开关）
- [ ] `DETMW_BUCKET=1` 正常启动（不再 fail-fast）：日志含 `bucket transport enabled (segment=64MB, discovery=UDP)`
- [ ] `DETMW_BUCKET=1 ctest -R plain_smoke` —— loan/零拷贝路径日志 `(loan/zero-copy)`

## 2. 容量验证（核心：100 帧 32K/100ms = 1000 帧/s）

- [ ] **桶 32K 吞吐**：`DETMW_BUCKET=1` 下 tune 链路（spa_mock_proc 全速 32K → tune_dts → webserver_mock）：
  ```bash
  DETMW_BUCKET=1 bash quality/test/run_steady.sh \
    build/quality/tune_dts build/quality/spa_mock_proc build/quality/webserver_mock \
    build/generated/detmw-tune 8 3
  ```
  - 记录：spa 发送 msg/s、dts data reader 收帧/s（`grep reader[3] /tmp/tune_dts.log`）
  - **通过标准：data 收 ≥ 1100 帧/s**（>1000 目标 + 10% 余量）；对比 UDP 基线 950 帧/s
- [ ] **S 级稳态**：`DETMW_BUCKET=1 bash quality/test/run_slevel.sh ... 60 1000000`（60 帧 × 1/s）
  - 通过标准：web_recv == 60（零丢包）
- [ ] **突发吸收**：spa 紧突发 200×32K（`DETMW_BUCKET=1 bash run_data_bench.sh ...` 看 reader 收数）
  - 通过标准：无池满丢帧（`get_data_stats` drop_pool_full=0）、桶段不溢出

## 3. 确定性验证（5ms 预算 / ms 级抖动）

- [ ] **单帧线程内耗时**：tune 链路 32K 负载下 `dts-cli.py exec tune-dts get_data_stats`
  - 记录 `max_us`；**通过标准：max_us < 5000 且 alarm5ms=0**（warn3ms 允许零星）
- [ ] **端到端时延/抖动**（可选，需打点扩展）：pub 打点 → data 处理完成 → 上报到达，
  记录 p50/p99/p99.9；通过标准：p99.9 抖动 ms 级内
- [ ] **节拍准点**：持续负载下 `get_data_stats` ticks 增长 ≈ 10/s（绝对期限节拍不饿死）

## 4. 数据完整性

- [ ] **零静默丢弃**：S 级 + 突发后 `get_data_stats` 全零：
  `drop_pool_full=0 drop_slice_overcap=0 key_fail=0`
- [ ] **上报内容校验**：spa 帧切片带 seq 填充，web 端校验上报 payload 与发送一致
  （当前 mock 未做内容校验——需补一条断言或人工抽查 CSV）
- [ ] **崩溃恢复**：kill -9 spa_mock_proc 后重启，dts 数据链路自动恢复（握手重发幂等）；
  kill -9 dts 后重启，桶残留段被 `ClaimOwnership` 接管（无 stale 段累积）

## 5. 运维面

- [ ] console：`dts-cli.py login tune-dts` → `help / get_threads / get_handlers / get_data_stats / set_log_level` 全通
- [ ] 日志：`/var/log/dts/dts_*.log` 时间戳切分 + 总量删旧生效（5MB/5G）
- [ ] 优雅停机：SIGINT/SIGTERM → `=== dts done ===` + 退出码 0（含 DETMW_BUCKET 模式）

## 6. 商用工程化专项（部分为待实现项，验收时逐项确认）

| 项 | 状态 | 验收标准 |
|---|---|---|
| RT 权限缺失 fail-fast | **已落地（P0-2，DTS_STRICT_SCHED=1，实测 exit=1）** | 无 CAP_SYS_NICE 时严格模式拒绝启动；dev 默认降级 Warn |
| 远程 OAM 通道（DDS 控制通道） | **v1 已落地（P1-3，2026-08-20）**：DDS 命令行 → control → ctl::Execute（D3 两条传输），端点 DTS.oam.0x000C/0x000D | 网管远程配置/状态采集（端到端往返在本机待环境复验） |
| 指标出口（丢包/jitter/超时 → OAM） | **已落地（P1-2）**：DTS.oam.0x000A/0x000B 可拉取 + 10s 结构化日志 | 结构化指标可被网管拉取（spa 断言过） |
| bucket 收缓冲 | **已修复（2026-08-20）**：max_msg_size=UINT32_MAX 钳制到 maxMessageSize（曾致 4 次 OOM 崩机，见 worklog 2026-08-20） | bucket_rtps 真机 PASS（200×32K 零丢） |
| 打包（RPM/容器 + 版本锁定） | 待做 | 一键部署脚本 + FastDDS 3.6.2 锁定 |
| 看门狗/进程守护 | 待做 | dts 崩溃自动拉起 + 状态上报 |
| 内存核算 | 待做 | 目标硬件实测 RSS（本机桶修复后 sub ~208MB 基线）+ 桶段 64MB 计入 |
| 32K 帧耗时真值 | 待做 | §3 数据点入库（预算阈值按硬件档位定） |

## 7. 结论记录模板

执行完填写：
```
环境：<硬件/OS/内核>
容量：data 收 <N> 帧/s（基线 950，目标 ≥1100）  PASS/FAIL
时延：单帧 max_us=<N>，alarm5ms=<N>             PASS/FAIL
抖动：p99.9=<N> ms                               PASS/FAIL
丢包：S 级 <n>/<n>，突发 <n>/<n>                 PASS/FAIL
遗留项：<未过项 + 责任人>
```
