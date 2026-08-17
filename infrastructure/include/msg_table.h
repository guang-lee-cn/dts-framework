#pragma once

#include <cstddef>
#include <cstdint>

namespace dts {

// ============================================================
// 三级消息路由：
//   第一层 sessionType（string）：**线程内固定**（如 "DTS"）
//   第二层 sessionInst（string）：**业务组**——同一线程可挂多个组，组间消息互不干扰
//   第三层 msgId（uint32）：**组内具体业务**，msgId 在 (sessionType, sessionInst) 内唯一
// 第一层 + 第二层在订阅期完成（生成路由头 + bootstrap RegisterSubRoutes 决定端点进哪个
// 线程 mailbox，mailbox 消息携带 sessionInst）；第三层在本文件落地：每个业务线程的
// {data|task|log}_msg_handler.cpp 声明**业务组数组**（每组一张 {msgId, 处理函数} 表），
// 分发 = 按 sessionInst 选组 → 组内按 msgId 查表。新增业务流程 = 组内表加一行。
// ============================================================

// 消息处理函数：第三层路由（msgId）命中后的业务入口。
// data = 消息载荷（线程已移所有权，允许原地修改；无载荷时为空指针）
// datalen = 载荷字节数
using MsgHandlerFn = void (*)(void* data, uint32_t datalen);

// 消息表条目：一行一个消息流。name = 可读名（console get_handlers / 诊断用）
struct MsgHandler {
    uint32_t msgId;
    MsgHandlerFn fn;
    const char* name;
};

// 静态消息表：包装 const 数组，线性查找（表长 ≤ 数十，查找成本可忽略）。
// 用法：业务组内 `constexpr MsgHandler kXxxHandlers[] = {...}` + 本类包装。
class MsgTable {
public:
    MsgTable() = default;
    MsgTable(const MsgHandler* entries, size_t count) : m_entries(entries), m_count(count) {}

    // 按 msgId 分发；命中且 fn 有效返回 true，未命中返回 false（调用方决定忽略/告警）
    bool Dispatch(uint32_t msgId, void* data, uint32_t len) const {
        const MsgHandler* h = Find(msgId);
        if (h == nullptr || h->fn == nullptr) {
            return false;
        }
        h->fn(data, len);
        return true;
    }

    // 查表：返回条目指针（未命中返回 nullptr）
    const MsgHandler* Find(uint32_t msgId) const {
        for (size_t i = 0; i < m_count; ++i) {
            if (m_entries[i].msgId == msgId) {
                return &m_entries[i];
            }
        }
        return nullptr;
    }

    size_t Count() const { return m_count; }
    const MsgHandler& At(size_t i) const { return m_entries[i]; }

private:
    const MsgHandler* m_entries = nullptr;
    size_t m_count = 0;
};

// 业务组登记：sessionType（线程内固定）+ sessionInst（业务组）→ 组内消息表。
// 一个线程声明一个组数组（kXxxGroups），bootstrap 统一登记（ctl get_handlers 展示）。
struct SessionMsgTable {
    const char* sessionType;
    const char* sessionInst;
    const MsgHandler* entries;
    size_t count;
};

// 数组条目数辅助：constexpr 表用 sizeof 展开
template <size_t N>
inline constexpr size_t HandlerCount(const MsgHandler (&)[N]) {
    return N;
}

template <size_t N>
inline constexpr size_t SessionGroupCount(const SessionMsgTable (&)[N]) {
    return N;
}

// constexpr 字符串相等（C++17 constexpr 循环；编译期组校验用，运行时同样可用）
inline constexpr bool CStrEq(const char* a, const char* b) {
    if (a == b) {
        return true;
    }
    if (a == nullptr || b == nullptr) {
        return false;
    }
    for (; *a != '\0' && *b != '\0'; ++a, ++b) {
        if (*a != *b) {
            return false;
        }
    }
    return *a == *b;
}

// 编译期查重：同一消息表内 msgId 必须唯一（msgId 在 (sessionType, sessionInst) 内唯一）。
// 用法：static_assert(MsgIdsUnique(kXxxHandlers, HandlerCount(kXxxHandlers)), "...")。
// 防扩展时重复加行导致查表静默遮蔽（MsgTable::Dispatch 取第一个匹配）。
inline constexpr bool MsgIdsUnique(const MsgHandler* entries, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        for (size_t j = i + 1; j < count; ++j) {
            if (entries[i].msgId == entries[j].msgId) {
                return false;
            }
        }
    }
    return true;
}

// 编译期组校验：组数组必须满足
//   ① sessionType 非空且**全部相同**（线程内 sessionType 固定）
//   ② sessionInst 非空且**互不相同**（业务组唯一，分发按组定位不歧义）
// 用法：static_assert(SessionGroupsValid(kXxxGroups, SessionGroupCount(kXxxGroups)), "...")
inline constexpr bool SessionGroupsValid(const SessionMsgTable* groups, size_t count) {
    if (groups == nullptr || count == 0) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        if (groups[i].sessionType == nullptr || groups[i].sessionInst == nullptr ||
            groups[i].entries == nullptr) {
            return false;
        }
        if (!CStrEq(groups[0].sessionType, groups[i].sessionType)) {
            return false;  // sessionType 线程内固定
        }
        for (size_t j = i + 1; j < count; ++j) {
            if (CStrEq(groups[i].sessionInst, groups[j].sessionInst)) {
                return false;  // 业务组唯一
            }
        }
    }
    return true;
}

// 运行时按 sessionInst 选组（分发第一步）；未命中返回 nullptr。
// sessionInst = nullptr（线程本地消息，如 TIMER）不命中任何业务组
inline const SessionMsgTable* FindSessionTable(const SessionMsgTable* groups, size_t count,
                                               const char* sessionInst) {
    if (groups == nullptr || sessionInst == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < count; ++i) {
        if (groups[i].sessionInst != nullptr && CStrEq(groups[i].sessionInst, sessionInst)) {
            return &groups[i];
        }
    }
    return nullptr;
}

}  // namespace dts
