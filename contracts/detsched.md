# 接口契约：detsched（调度层）

> 版本：v1 · 2026-08-04 · 模式：重构（API 现有保留，改可读性 + 补运维查询）
> 依据：docs/design/dts-strategy.md（detsched 是适配器，实现 SchedPort）

## 1. 现状 API（保留，重构命名/内部）

```cpp
namespace detsched {

using ThreadFn = void (*)(void* arg);

struct ThreadParams {
    ThreadFn fn = nullptr;
    void* arg = nullptr;
    size_t stackSize = 0;
    int    schedPolicy = -1;
    int    schedPriority = -1;
    bool   explicitSched = true;
    int    cpuAffinity = -1;
    bool   detached = false;
    size_t guardSize = 0;
};

struct ThreadInfo {
    std::string name;
    int segIndex;
    int prio;
    int schedPolicy;
    int cpuAffinity;
    uint64_t tid;
    int state;
};

using ThreadHandle = ThreadHandleImpl*;

bool DeclareDomain(int prioLevel);
ThreadHandle CreateThread(const std::string& name, int prioLevel,
                          ThreadFn fn = nullptr, void* arg = nullptr);
ThreadHandle CreateThread(const std::string& name, int prioLevel, const ThreadParams& params);
void DestroyThread(ThreadHandle h);
size_t QueryThreads(ThreadInfo* out, size_t cap);   // control 线程只读查询（D5）
void   DumpThreadInfo();                            // console 命令入口

}  // namespace detsched
```

**约束**：
- 调度域分段：跨进程统一，按 sched_defs.json 分段（CORE/DATA/APP/DTS/BKG/DEF）
- 线程注册表 `m_regMutex` 保护，control 线程 QueryThreads/DumpThreadInfo 只读调用安全
- RT 降级：非 root 环境 EPERM 回退 SCHED_OTHER（现状保留）

## 2. 运维查询增强（阶段 4 重构点）

- `QueryThreads`/`DumpThreadInfo` 已满足 console 只读查询需求
- 如需按线程名查单线程（console `get_thread <name>`），补 `QueryThread(const char* name, ThreadInfo& out)`

## 3. 版本管理

- 现有 API 保留（组合根/contexts 在用），重构不改签名只改内部可读性
- 新增查询 API 向后追加

## 4. 物理约束

- ThreadParams 成员 9（fn/arg/stackSize/schedPolicy/schedPriority/explicitSched/cpuAffinity/detached/guardSize）——**超 5 上限**
  - 已确认：ThreadParams 是纯数据类型，不受"类成员 ≤5"约束（strategy §7：纯数据类型不受此限）
- ThreadInfo 为纯数据类型，不限
- 函数面 ≤ 9：DeclareDomain/CreateThread×2/DestroyThread/QueryThreads/DumpThreadInfo/QueryThread(新) = 7
