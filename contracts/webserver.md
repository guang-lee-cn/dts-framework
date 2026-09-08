# 接口契约：web-server（上报沉淀与观测面）

> 版本：v1 · 2026-09-08 · 模式：新增进程（D2 数据出口的 web 落地）
> 依据：docs/CONTEXT.md 待办"web 落地接 kafka（当前 CSV 占位）"

## 1. 进程定位

```
spa/agent → dts(data线程) ──DTS_data_4(DDS)──► web_server ──produce──► Kafka topic: dts.report
                                                web_server ──consume(免组协议)──► SSE ──► 浏览器
```

- **独立进程**（`webserver/`，纯 detmw 对端：不链 bootstrap/task/data/log），替代 webserver_mock 的
  "CSV 占位"角色；webserver_mock 保留给性能量测链（多 reader 计数）
- **观测即验证沉淀**：UI 展示的是从 Kafka 消费回来的数据（不是 DDS 旁路）——浏览器能看到 =
  Kafka 里真的有
- 依赖：librdkafka（`thirdparty/librdkafka/fetch.sh` 免 root 解包）+ broker
  （`tools/redpanda_up.sh`，podman 单节点 Redpanda）

## 2. 命令行与降级语义

```
web_server <cfg.json> [brokers=localhost:9092] [http_port=8080] [topic=dts.report] [web_dir=./web]
```

- **fail-fast**：broker 不可达（metadata 5s）/ topic 创建失败 / HTTP 绑定失败 → 退出非 0，
  不静默运行（kafka 是显式依赖，语义对齐 DETMW_BUCKET）
- **幂等建 topic**：启动即 CreateTopics（1 分区；已存在视为成功），消除
  "auto-create 只在首个 produce 生效"的冷启动竞态

## 3. Kafka 消息契约

| 项 | 值 |
|---|---|
| topic | `dts.report`（可配）· key = taskId（字符串） |
| value | dts 上报帧原始二进制：`ReportHeader(原生布局，含对齐填充) + n×(SubHeader + 切片)`（结构见 contexts/data/domain/include/data_model.h） |
| headers | `seq` / `ts_ms`（十进制文本，kcat/rpk 快速检视用） |
| consumer | group.id=dts-web-ui 但**只 assign 不 subscribe**（免组协议/无 rebalance），OFFSET_END 起实时观测；历史回看用 rpk |

## 4. HTTP/SSE 接口（自研 mini 服务器，零第三方依赖）

| 路由 | 内容 |
|---|---|
| `GET /` | 观测页（单文件原生 JS + canvas，无 CDN：统计卡/60 点 fps·MB/s 曲线/最近 20 帧明细） |
| `GET /api/stats` | JSON 快照：`total_frames / total_bytes / gaps(seq间断) / last_seq / last_task / fps_5s / mbps_5s` |
| `GET /api/stream` | SSE：`stats` 事件 1Hz；`frame` 事件 ≥100ms 节流（帧摘要 seq/task/ts/slices/dataId范围/len/kafka offset） |

## 5. 线程模型

detmw 接收线程（produce，异步非阻塞）· kafka 消费线程（解析→统计→SSE 节流广播）·
HTTP accept + 每连接线程（SSE 长连接，广播写失败就地剔除）· 主线程 1Hz 统计日志。
下电顺序：consumer.Stop → producer.Shutdown(flush 3s) → http.Stop。

## 6. 验证与运维

- ctest `web_smoke`（环境自适应：podman/Redpanda 不可用 → Skipped 77）：强断言
  **Kafka 高水位 == DDS 收帧数**（每帧都沉淀）
- 演示：`tools/web_demo.sh`（浏览器开 `http://localhost:8080`；WSL mirrored 模式 Windows 直达）
- 命令行消费：`podman exec dts-redpanda rpk topic consume dts.report -n 5`
- 停 broker：`tools/redpanda_down.sh`
