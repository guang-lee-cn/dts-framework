# 接口契约：detmw 通信层

> 版本：v2 · 2026-08-04 · 模式：重构（全 C++ + 控制通道 + 双 API + TransportInterface 隔离）
> 依据：docs/design/dts-strategy.md D2/D7/D8/D10

## 0. 定位（D10，2026-08-04 定稿）

- **detmw 不独立 .so 交付**——同工程编译。独立交付会把可替换的传输实现和接口绑死，无价值。
- **核心价值 = 底层 DDS 可替换**，靠内部 `TransportInterface` 抽象隔离，不靠 C ABI。
- **全 C++**：无 C ABI 迁就，上层接口用 std::string，无双份结构体。

## 1. 消息寻址契约

**统一寻址键 `detmw_endpoint`**（C++ 结构体，唯一类型，覆盖进程内线程间 + 进程间）：

```cpp
struct detmw_endpoint {
    std::string session_type;
    std::string session_inst;
    uint32_t msg_id;

    bool operator==(const detmw_endpoint&) const;   // 内置
    std::string ToString() const;                    // "DTS.task.7"，日志
};
struct detmw_endpoint_hash { ... };                  // unordered_map 键
```

- 发布方与订阅方共享同一 endpoint，键即约定
- 回调注册 key 必须是完整三元组（同一 msgId 可挂不同 sessionInst，禁止退化为 msgId 单键）
- **infrastructure 的 SessionKey 删除**，统一用 detmw_endpoint（消除双份维护）

## 2. 消息 API（C++）

```cpp
typedef void (*detmw_recv_fn)(void* user_ctx, const uint8_t* data, uint32_t len);

// 生命周期
detmw_handle* detmw_init(const char* cfg_path);   // 加载分进程生成配置 JSON
void detmw_destroy(detmw_handle* h);

// 收：按 endpoint 建 reader + 注册常态回调（SEDP 内部完成）
// 调用方心智 = "我要收来自这个端点的消息"，一个调用搞定
int detmw_subscribe(detmw_handle* h, const detmw_endpoint& src,
                    detmw_recv_fn fn, void* ctx);

// 发（进程外）：走 DDS，序列化 + 传输
int detmw_publish(detmw_handle* h, const detmw_endpoint& dst,
                  const uint8_t* data, uint32_t len);

// 调试
int detmw_dump(detmw_handle* h, char* buf, size_t cap);
```

**约束**：
- `detmw_init` 后须 `detmw_destroy`
- `detmw_subscribe` 失败返回非 0，调用方不得保留悬挂回调上下文
- 参数语义：subscribe 的 endpoint 是"来源"，publish 的 endpoint 是"目的"（方向由动词 + 参数名表达）

## 3. 进程内直通 API（D7/D8）✅ 已落地 2026-08-16

```cpp
// 发（进程内）：目标为本进程某线程。命中本进程订阅者 → 直接调订阅回调（mailbox 直投），
// 免 DDS 序列化/传输/反序列化；未命中 → 回退 transport（对端可能在别的进程）。
int publish_internal(const detmw_endpoint& dst, const uint8_t* data, uint32_t len);
```

**实现（detmw.cpp）**：`Communicator::subscribe()` 成功时把 (endpoint → 回调) 登记进
`Impl::localSubs`（mutex 保护）；`publish_internal()` 命中 localSubs 即逐订阅者调用回调
（`OnRouteMsg` → mailbox.Send，一次拷贝 + 移所有权），返回 0；未命中回退 `transport->Send`。

**约束**：
- 仅用于"目标在本进程某线程"的消息；进程外一律用 `publish_external`
- 接收侧无感知：消息同样进目标线程 mailbox（与 DDS 消息无差别）
- 调用方负责选对 API，detmw 不做本地/外部映射判断
- **接收统一入口**：无论 `publish_external` / `publish_internal`，目标线程都经订阅回调
  （bootstrap 的 OnRouteMsg 路由）进 mailbox，全系统唯一投递点。直通只发生在 transport
  之上的 Communicator 层（命中本进程订阅者），业务代码不得旁路订阅回调另开投递入口
- **外部不可观测**：直通消息不再出现在 DDS 通道上，外部进程订阅同一 endpoint 收不到
  （任务激活 msg2 即此语义；外部往返走 msg7/msg8）

## 4. 控制通道（D2）

**语义**：外部服务控制指令走独立 session/msgId，不占业务路由。

```cpp
#define DETMW_CTRL_SESSION_INST "control"   // 保留 session_inst，与业务隔离
```

**约束**：
- 控制消息独立 endpoint，不得与业务路由混用
- 接收侧：control 线程消费，只读查询业务线程状态，不阻塞业务（D5）

## 5. 传输隔离层（D10，换 DDS 的唯一切换点）

**内部接口**（detmw 公共实现调用，不对外暴露）：

```cpp
// 传输抽象：换 FastDDS/CycloneDDS/自研桶 只重写此接口实现
class TransportInterface {
public:
    virtual ~TransportInterface() = default;
    virtual int CreateReader(const detmw_endpoint& ep, detmw_recv_fn fn, void* ctx) = 0;
    virtual int CreateWriter(const detmw_endpoint& ep) = 0;
    virtual int Send(const detmw_endpoint& ep, const uint8_t* data, uint32_t len) = 0;
};
```

**约束**：
- detmw 公共实现（配置加载/topic 拼接/路由）依赖 `TransportInterface`，不依赖 FastDDS
- `detmw_fastdds.cpp` = TransportInterface 的 FastDDS 实现（替换点）
- 新增传输（自研桶）实现同一接口，detmw 公共层零改动

## 6. detmw 组成与交付（D10）

| 类别 | 内容 | 换 DDS 时 |
|---|---|---|
| 对外接口 | detmw.h（init/subscribe/publish/destroy） | 不变 |
| 传输抽象 | TransportInterface（本文件 §5） | 稳定 |
| 传输实现 | detmw_fastdds.cpp | **变**（替换点） |
| 配置层 | detmw.c + cJSON | 不变 |
| 生成工具 | gen_detmw.py | 不变 |

**交付**：同工程编译，不独立 so。

## 7. 接口版本管理

- 数据消息 API 稳定，禁止破坏性变更
- 新增 API（publish_inner/控制通道）向后兼容追加
- 配置 JSON 变更：新增字段必须可缺省

## 8. 使用示例

```cpp
// 订阅（bootstrap RouteToThread 模式）
detmw_endpoint ep{"DTS", "task", 7};
if (detmw_subscribe(mw, ep, OnRouteMsg, rc) != 0) {
    // 失败处理：不保留 rc
}

// 进程外发布
detmw_endpoint dst{"DTS", "task", 8};
detmw_publish(mw, dst, buf, len);

// 进程内直通（task -> data 本地线程）
detmw_publish_inner(mw, MSG_ID_DATA_TASK_ACTIVE, buf, len);
```

## 9. 物理约束

- 公有函数 ≤ 9：当前 5 个（init/destroy/subscribe/publish/publish_inner）+ dump，预留扩展位
- detmw_handle 为不透明句柄
- TransportInterface 3 纯虚（CreateReader/CreateWriter/Send）——满足"纯数据类型不受限"
