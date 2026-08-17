# FastDDS 升级评估 + SHM 问题路线决策（决策点 1 结论）

> 2026-08-17 · 背景：raw 零拷贝通道（DataSharing）依赖 SHM 大段，撞上 FastDDS 3.6
> 自定义 SHM 描述符析构 SEGV（本工程 Bug 8 / 上游 #6114）与内置段 512KB 容量限制
> （Bug 7）。本文回答：升级 FastDDS 能否解决？

## 1. 证据（本地上游源码仓库 /home/guang/code/opensource/Fast-DDS 直接核对）

| 检查项 | 结果 |
|---|---|
| 上游 3.x 最新版本 | **v3.6.2**（2026-08 时点；3.6.1/3.6.2 无 3.7/4.0） |
| 上游 2.x LTS 最新 | v2.14.6 |
| 3.6.0 → 3.6.2 SHM 代码差异 | **零改动**（`src/cpp/rtps/transport/shared_mem/` diff 为空） |
| 3.6.0 → 3.6.2 DataSharing 差异 | **零改动** |
| #6114 崩溃点 `WatchTask::remove_port`（master） | 与 3.6.0 **逐字相同**（仅 uncrustify 格式化） |
| 2.14.6 LTS `remove_port` | 核心遍历/erase 逻辑与 3.6.0 相同 |
| 全历史 #6114 修复提交 | **未找到**（`git log --all --grep` 无匹配） |

**结论：上游未修复（一年+），且 3.x 最新版与 master 均无修复迹象。**

## 2. 升级评估

| 路线 | 能否解决 Bug 8/段容量 | 评估 |
|---|---|---|
| 升级 3.6.2 | ❌ 不能（SHM 代码零改动） | 3.6.2 相对 3.6.0 改动集中在测试/CI/示例/security（271 文件），对 detmw 无直接收益；如需可作低风险配套升级（全量回归），但不解决容量问题 |
| 回退 2.x LTS（2.14.6） | ❌ 不能（核心逻辑相同） | 不推荐：3.x 是新主线，2.x 维护模式；namespace/API 差异大 |
| 等待上游修复 | 期望值低 | #6114 一年未修（OPEN），worklog 2026-08-03 已判"修它期望值低" |

## 3. 路线决策：自研桶传输（确定路线）

**理由**：唯一同时解决「UAF 崩溃」+「段容量 512KB」+「不依赖上游」的路径。

**现状**：`thirdparty/bucket/` 已有一版实现（~950 行核心，针对 FastDDS 3.6
TransportInterface API，locator 编码与 SharedMemTransport 对齐，桶 = 共享内存环），
因 FastDDS SHM 问题研究转向而搁置（worklog 2026-08-03）。

**接入方案（detmw 层，TransportInterface 已是唯一切换点）**：

```
FastDdsTransport 构造：
  user_transports = [BucketTransportDescriptor(段容量 ≥ 突发量)]   # raw 通道数据面走桶
  发现面：仍走 FastDDS 动态/静态 EDP（桶只做数据面传输）
```

**要点**：
- 桶段容量可配（如 16-64MB），彻底解决 100×32K 突发（3.2MB）
- 不碰 FastDDS SHM 代码 → 无 UAF 风险
- **DataSharing 与自定义 transport 不兼容**（DataSharing 仅绑定内置 SharedMemTransport）
  → 零拷贝需桶内实现（环内 buffer 借出/归还，引用计数）或接受"进环/出环 2 次 memcpy
  （无 FastDDS 序列化框架开销 + 大段容量，预计仍显著优于 950/s 基线）"
- 发现仍走 FastDDS → 跨进程互通不变（UDP/SHM 混合组网需评估 locator 匹配）

**前置工作清单**：
1. 验证 bucket 原型正确性：`bucket_rtps_test`/`bucket_smoke` 纳入 ctest + 双进程压测
2. detmw 接入：`DETMW_BUCKET=1` 环境开关（默认内置 SHM，桶作为 raw 通道可选传输）
3. 桶内零拷贝（借环 buffer）或先 2 拷贝版实测对比
4. 目标环境实测：32K 帧吞吐 vs 950/s 基线、单帧线程内耗时（5ms 预算数据点）

## 4. 与 raw-zero-copy.md 的关系

[raw-zero-copy.md](raw-zero-copy.md) 决策点 1 的"三选一"现已收敛：
- ~~升级 FastDDS~~（已证不能解决）
- **自研桶 = 确定路线**（承接 DataSharing 的容量目标：大段 + 低开销传输）
- 接受内置段小突发 = 仅过渡（≤512KB 突发场景可用，DataSharing 仍有收益）
