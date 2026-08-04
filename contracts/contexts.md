# 接口契约：contexts（业务域）

> 版本：v1 · 2026-08-04 · 模式：重构
> 依据：docs/design/dts-strategy.md（domain 零技术依赖，通过端口访问通信/调度）

## 1. 分层边界

```
contexts/task|data|log/
  ├── interface/     ← 接入层入口（线程 entry），转发到 application
  ├── application/   ← 消息处理编排（msg_handler）
  └── domain/        ← 纯业务，零技术依赖（不 include detmw/detsched/infrastructure）
```

## 2. 业务域端口（Port）：访问通信/调度的夹层

**原则**：domain 不依赖 detmw/detsched/infrastructure 实现，只依赖本文件定义的端口接口。企业集成时换端口适配器，不改 domain。

```cpp
namespace dts::port {

// 通信端口：业务域唯一的收发入口（实现 = detmw 双 API 适配器）
struct MwPort {
    // 进程内直通（目标为本进程线程）；返回 0 成功
    int (*publishInner)(uint32_t msgId, const uint8_t* data, uint32_t len);
    // 进程外发布（走 DDS）；返回 0 成功
    int (*publish)(const char* sessionType, const char* sessionInst,
                   uint32_t msgId, const uint8_t* data, uint32_t len);
};

// 调度/线程端口：业务域查询线程状态（实现 = detsched 适配器）
struct SchedPort {
    // 全进程线程信息查询（只读）
    size_t (*queryThreads)(void* out, size_t cap);   // out = detsched::ThreadInfo*
};

}  // namespace dts::port
```

**约束**：
- domain 只调 port 函数指针，不 include 任何中间件头
- port 实现由组合根注入（bootstrap 装配时）
- 端口函数指针可替换（企业换适配器 = 换函数指针表）

## 3. 线程接入层入口（interface，现状保留）

```cpp
namespace dts {

// task/data/log 线程入口：status 已知、msgId 已知，转发到 msg_handler
void TaskEntry(ThreadStatus status, uint32_t msgId, const uint8_t* msg, uint32_t len);
void DataEntry(ThreadStatus status, uint32_t msgId, const uint8_t* msg, uint32_t len);
void LogEntry(ThreadStatus status, uint32_t msgId, const uint8_t* msg, uint32_t len);

}  // namespace dts
```

**约束**：entry 只做转发，不做业务；业务在 application/msg_handler。

## 4. 消息 ID 契约（defs.h）

同一线程内 msgId 区分消息类型；**msgId 在 (sessionType, sessionInst) 内唯一，不全局编号**（D7/D8 寻址分层）。

现有保留：
```
MSG_ID_STATUS           0x0000  状态更新（线程体消费，不转发业务）
MSG_ID_TASK_ACTIVE      0x0001  调度 -> task
MSG_ID_DATA_TASK_ACTIVE 0x0002  task -> data（进程内直通候选）
MSG_ID_AGENT_DATA       0x0003  agent -> data
MSG_ID_REPORT           0x0004  data -> 网管
MSG_ID_LOG_COLLECT      0x0005  调度 -> log
MSG_ID_LOG_REPORT       0x0006  log -> 网管
MSG_ID_TASK_CONFIG      0x0007  nfoam -> task 配置变更
MSG_ID_TASK_RESPONSE    0x0008  task -> nfoam 响应
```

## 5. 版本管理

- 新增 msgId 在 (sessionType, sessionInst) 内递增，不全局冲突
- domain 端口接口变更 = 破坏性变更，须同步改 bootstrap 装配的适配器

## 6. 使用示例

```cpp
// domain（纯业务）通过端口收发，无中间件依赖：
namespace {
const port::MwPort* g_mw = nullptr;
}
void DomainInit(const port::MwPort* mw) { g_mw = mw; }

// task -> data 本地线程（进程内直通）
g_mw->publishInner(MSG_ID_DATA_TASK_ACTIVE, data, len);

// task -> nfoam 外部（走 DDS）
g_mw->publish("DTS", "task", MSG_ID_TASK_RESPONSE, json, jsonLen);
```

## 7. 物理约束

- port 结构：纯函数指针表，无方法、无状态（天然满足成员 ≤5）
- 每个 msg_handler 职责单一：一个消息类型一个处理分支
