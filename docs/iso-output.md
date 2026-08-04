# ISO 输出：dts-framework 重构

模式: 重构

> 生成：2026-08-04 · 目录：`/home/guang/code/dts-framework`（原 train）
> 上游：重构前置（现状评估）+ Insight 完成于重构备忘录；本文件为 Strategy 终稿 + Operation 规划落盘。

## Insight 结构化记录

| 维度 | 结论 |
|------|------|
| 触发事件 | AI 自动生成代码大部分不符合预期 → 不可控、难人为审计、可读性差 |
| 用户/客户 | 自己（后续希望落地企业）；dts 为多实例部署（主控 docker 编排 cpf / 基带直接部署 dpf 等） |
| 当前做法 | 分层五层：通信 detmw / 调度 detsched / 基础设施 infrastructure / 业务域 task·data·log / 组合根 bootstrap；现状代码可跑但分层 API 不可审计 |
| 不做会怎样 | 代码库持续不可控，无法人为 review，演进/企业化被卡死 |
| 长期愿景 | 分层清晰、可人为审计的 DDS 确定性调度框架；API 可读、可审计、可演进；企业可集成 |
| 竞品/参考 | 参考 {fmt}/nlohmann::json（API 直观）、Asio（分层切法）、BS::thread_pool（短小可读）、CycloneDDS（同领域）；Fast-DDS 为反面参考 |

## Strategy 终稿

**定位**：分层清晰、可人为审计的 DDS 确定性调度框架。

**核心原则**：框架保持不动，重构代码实现（尤其分层 API）。
- 不动：分层结构、层间依赖方向、运行时架构（线程/通信/部署）
- 重写：各层 API 定义（签名/职责）+ 内部实现，逐目录逐文件

**核心目标（SMART）**：
1. 五层 API 重写完成：detmw / detsched / infrastructure / task·data·log / bootstrap
2. 可读性达标：可人为 review、无 AI 坏味道、单文件职责单一
3. 行为不回归：cpf/dpf 重构前后压测数据一致

**边界**：
- ✅ 做：五层 API 重写、逐目录逐文件、每层独立 commit（回滚到层）
- ❌ 不做（本期）：三方库升级（Fast-DDS/spdlog 保持现状）、兼容性保证（API 变必同步改）、合规/安全备案（重构后处理）

**MVP 范围**：优先 bootstrap → infrastructure（console 模块）→ detmw 控制通道 → detsched/contexts；业务重构优先于三方库/DDS 版本选型。

**成功标准**：
- 五层 API 重写完成且可人为 review
- 每层重构后压测对比重构前基线，数据一致（cpf/dpf 不回归）
- 分支管理：baseline / refactor 并存，逐层独立 commit

**关键假设与依赖**：
- 假设：Fast-DDS/spdlog 保持现状可用；编译宏差分（CPF/DPF/BPF/BB → cpf-dts/dpf-dts 等）按现有模式
- 依赖：参考源码已拉取至 `opensource/`（fmt/json/asio/beast/oneTBB/thread-pool/Catch2 + 已有 CycloneDDS/Fast-DDS/spdlog）

**盲点检查结论**：
- 三方库先保持现状（业务重构完成后再择机选型适配）
- API 变必同步改，无商用不考虑兼容
- 合规/安全备案（三方库许可证、企业审计、实时性合规），重构后处理
- 砍半优先 contexts（业务域）；自用可读优先；企业集成走夹层转换 API（端口）
- 回滚：逐层独立 commit；分支 baseline/refactor；保留重构前压测基线

## Operation 规划

### 重构顺序（已调整：bootstrap 前置）

| 阶段 | 核心里程碑 | 输出 |
|---|---|---|
| 0 定约 | 重构范围 + console 方案定稿（内置/命令表/每实例 socket） | 本文档 |
| 1 **bootstrap 前置审核** | 组合根装配审核；装 console 人类运维通道骨架 | bootstrap 重构完成 |
| 2 infrastructure | console 模块：CommandExecutor（命令表驱动）+ socket 监听线程（低优先级域） | console 模块 v1 |
| 3 detmw | 新增**控制通道**（外部服务 DDS 控制消息 → control 线程） | detmw 控制通道 |
| 4 detsched / contexts | 逐层重写，每层暴露 console 可调运维接口 | 五层全绿 |

**资源估算**：阶段 1-2 约 1~2 人周；阶段 3 约 1 人周；阶段 4 视各层规模。

**风险 Top 3**：
1. bootstrap 装配顺序调整引入启动回归 → 每阶段压测对比重构前基线
2. detmw 控制通道与数据路由耦合 → 控制消息独立 session/msgId，不与业务路由混用
3. console 命令执行阻塞业务线程 → CommandExecutor 只做查询/级别调整，不碰业务热路径

**验收决策者**：用户本人；每层完成时人工 review API + 压测不回归（cpf/dpf 对比基线）。

### Console 运维通道设计（已确认决策）

**形态**：console 内置 dts 进程（非独立工具）。理由：docker 编排/版本管理/命令增删改集中在一个二进制。

**通信模型**：
```
执行逻辑：只有一处 CommandExecutor（进程内）
  ├─ console 人类入口：Unix socket 监听线程 → 直接调 CommandExecutor
  └─ 外部服务：detmw DDS 控制消息 → control 线程 → 调 CommandExecutor
```

**命令表驱动**：新增/删除/修改命令 = 改注册表 + 一个函数，不碰调度/传输/其他命令。

**多实例 socket**：每实例一个 Unix socket（方案 1），路径由编译宏派生默认值（CPF→`/run/dts/cpf/console.sock` 等），运行时配置可覆盖。

**参考源**：完整参考要求（分类/看什么/本地路径）见 [docs/参考项目要求.md](参考项目要求.md)。

---

下游：strategy.md 设计阶段加载（先执行重构前置：迁移策略/兼容约束/增量替换优先级）。
