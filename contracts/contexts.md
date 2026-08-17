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

## 3. 线程接入层入口（interface）

```cpp
namespace dts {

// task/data/log 线程入口：status 已知、msgId 已知，转发到 msg_handler。
// msg 非 const：mailbox 已移所有权给本线程，业务处理允许原地修改；无载荷时 nullptr（如 TIMER）
void TaskEntry(ThreadStatus status, uint32_t msgId, void* msg, uint32_t len);
void DataEntry(ThreadStatus status, uint32_t msgId, void* msg, uint32_t len);
void LogEntry(ThreadStatus status, uint32_t msgId, void* msg, uint32_t len);

}  // namespace dts
```

**约束**：entry 只做转发，不做业务；业务在 application/msg_handler。

## 3.1 三级路由 + 业务组消息表（msg_table.h，2026-08-16 定稿）

**路由语义（线程内）**：第一层 sessionType（string）**固定**——一个线程只服务一个
sessionType（当前 "DTS"）；第二层 sessionInst（string）= **业务组**——同一线程可挂
多个组，组间消息互不干扰；第三层 msgId（uint32）= **组内具体业务**，
msgId 在 (sessionType, sessionInst) 内唯一。

- 第一/二层在**订阅期**完成：生成路由头（gen_detmw.py 产物 `{task|data|log}_routes.h`）
  + bootstrap `RegisterSubRoutes` 决定每个 (sessionType, sessionInst, msgId) 端点进哪个线程
  mailbox；**mailbox 消息携带 sessionInst（借指针零拷贝）**
- 第三层在**业务线程消息表**完成：`{data|task|log}_msg_handler.cpp` 声明**业务组数组**
  `constexpr SessionMsgTable kXxxGroups[] = { {sessionType, sessionInst, 组内表, N}, ... }`；
  分发 = `FindSessionTable(sessionInst)` 选组 → `MsgTable::Dispatch(msgId, data, len)` 组内
  查表；处理函数签名统一 `void (*)(void* data, uint32_t datalen)`
- 编译期护栏：`static_assert(MsgIdsUnique(...))` 组内 msgId 唯一；
  `static_assert(SessionGroupsValid(...))` 组数组合法（sessionType 全部相同、sessionInst 唯一）
- 每个线程暴露 `const SessionMsgTable* XxxSessionGroups(size_t* count)`，bootstrap 统一登记，
  console `get_handlers` 命令可查（`sessionType.sessionInst → msgId → 处理函数名`）
- 线程本地消息（MSG_ID_TIMER 100ms 节拍）不属任何业务组，由分发入口直接处理

**新增一个消息流（扩展配方，三步）**：

```cpp
// 例：网管下发命令 msg9 给 task（业务组 "task"），task 响应 msg10

// ① 配置（config/detmw/dts/<proc>.json）加端点条目（构建期 gen_detmw.py 自动派生
//    订阅路由头 / 静态发现 XML / 预建 writer；缺订阅条目 = 消息到不了线程，
//    缺 publish 条目 = 发布报 "writer not precreated"）：
//    { "session_type":"DTS", "session_inst":"task", "msg_id":9,  "role":"subscribe", "thread":"task" },
//    { "session_type":"DTS", "session_inst":"task", "msg_id":10, "role":"publish",   "thread":"task" },

// ② msg_handler.cpp 业务组内消息表加一行（msgId 重复由 static_assert 编译期拦截）：
//    {MSG_ID_TASK_CMD_9, OnTaskCmd9, "task_cmd_9"},

// ③ 写处理函数（publish 外部走 publish_external，进程内线程间走 publish_internal）：
//    void OnTaskCmd9(void* data, uint32_t len) {
//        ...
//        DtsMw()->publish_external(detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_TASK,
//                                                  MSG_ID_TASK_RESPONSE_10}, out, outLen);
//    }
```

**同一线程扩展新业务组**：组数组加一项即可——如 task 线程再挂一组配置业务：

```cpp
// 新组：sessionInst="cfg"（msgId 组内独立编号，可与 "task" 组同号）
// constexpr MsgHandler kCfgHandlers[] = { {MSG_ID_CFG_LOAD, OnCfgLoad, "cfg_load"}, ... };
constexpr SessionMsgTable kTaskGroups[] = {
    {SESSION_TYPE_DTS, "task", kTaskHandlers, HandlerCount(kTaskHandlers)},
    // {SESSION_TYPE_DTS, "cfg", kCfgHandlers, HandlerCount(kCfgHandlers)},  // ← 新增组
};
// 配置里对应加 session_inst="cfg" 的端点条目（role/thread 同上）
```

框架侧零改动：订阅自动登记（含 publish_internal 本地直通表）、`get_handlers` 自动可见。
实际例子：msg7/msg8（nfoam 配置往返）、spa 握手 msg2 + 上报 msg4 均为该配方落地。

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
- 每个 msg_handler 职责单一：消息表一行一个消息流（新增 = 加一行，删除 = 删一行）
- msgId 在 (sessionType, sessionInst) 内唯一（同一线程表内不得重复，MsgTable 线性查表）
