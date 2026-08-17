// 单元自测：三级路由消息表（msg_table.h）—— 无外部依赖，纯逻辑断言。
// 运行：构建后直接执行；退出码 0 = 全过。
#include <cstdio>
#include <cstring>

#include "msg_table.h"

using namespace dts;

namespace {

int g_checks = 0;
int g_fails = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_fails;                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                 \
    } while (0)

// ---- 测试处理函数（模拟业务消息流） ----
int g_callsA = 0;
int g_callsB = 0;
int g_payloadLen = -1;

void OnMsgA(void* data, uint32_t len) {
    ++g_callsA;
    if (data != nullptr && len > 0) {
        g_payloadLen = static_cast<int>(len);
    }
}

void OnMsgB(void*, uint32_t) {
    ++g_callsB;
}

constexpr MsgHandler kTestHandlers[] = {
    {0x0001, OnMsgA, "msg_a"},
    {0x0002, OnMsgB, "msg_b"},
};

void TestDispatch() {
    g_callsA = 0;
    g_callsB = 0;
    g_payloadLen = -1;

    const MsgTable table(kTestHandlers, HandlerCount(kTestHandlers));
    uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};

    // 命中分发 + 载荷透传
    CHECK(table.Dispatch(0x0001, payload, sizeof(payload)));
    CHECK(g_callsA == 1);
    CHECK(g_payloadLen == 8);
    CHECK(table.Dispatch(0x0002, nullptr, 0));
    CHECK(g_callsB == 1);

    // 未命中：返回 false，不调用
    CHECK(!table.Dispatch(0xFFFF, payload, 0));
    CHECK(g_callsA == 1 && g_callsB == 1);

    // 空表：任何 msgId 都不命中
    const MsgTable empty(nullptr, 0);
    CHECK(!empty.Dispatch(0x0001, nullptr, 0));
}

void TestFindAndAt() {
    const MsgTable table(kTestHandlers, HandlerCount(kTestHandlers));
    const MsgHandler* h = table.Find(0x0002);
    CHECK(h != nullptr && h->fn == OnMsgB && std::strcmp(h->name, "msg_b") == 0);
    CHECK(table.Find(0x0100) == nullptr);
    CHECK(table.Count() == 2);
    CHECK(table.At(0).msgId == 0x0001);
    CHECK(table.At(1).msgId == 0x0002);
}

void TestSessionTable() {
    const SessionMsgTable st{"DTS", "task", kTestHandlers, HandlerCount(kTestHandlers)};
    CHECK(std::strcmp(st.sessionType, "DTS") == 0);
    CHECK(std::strcmp(st.sessionInst, "task") == 0);
    CHECK(st.count == 2 && st.entries[0].msgId == 0x0001);
}

// 编译期查重：唯一表通过（static_assert 编译期验证）；重复表 constexpr 求值为 false
static_assert(MsgIdsUnique(kTestHandlers, HandlerCount(kTestHandlers)),
              "test 表 msgId 应唯一");
static_assert(!MsgIdsUnique((const MsgHandler[]){{0x0001, OnMsgA, "a"},
                                                 {0x0001, OnMsgB, "b"}},
                            2),
              "重复 msgId 应判 false");
static_assert(MsgIdsUnique(nullptr, 0), "空表视为唯一");

void TestMsgIdsUnique() {
    // 运行时抽查（与 static_assert 同一实现）
    CHECK(MsgIdsUnique(kTestHandlers, HandlerCount(kTestHandlers)));
    const MsgHandler dup[] = {{0x0001, OnMsgA, "a"}, {0x0001, OnMsgB, "b"}};
    CHECK(!MsgIdsUnique(dup, 2));
    CHECK(MsgIdsUnique(nullptr, 0));
}

// ---- 业务组（sessionInst）语义：同一线程多组、sessionType 固定、msgId 组内唯一 ----
constexpr MsgHandler kGroupAHandlers[] = {
    {0x0011, OnMsgA, "grp_a_1"},
    {0x0012, OnMsgB, "grp_a_2"},
};
constexpr MsgHandler kGroupBHandlers[] = {
    {0x0011, OnMsgB, "grp_b_1"},  // 与 A 组同 msgId：组间允许（(sessionType, sessionInst) 内唯一）
};

constexpr SessionMsgTable kTwoGroups[] = {
    {"DTS", "grp_a", kGroupAHandlers, HandlerCount(kGroupAHandlers)},
    {"DTS", "grp_b", kGroupBHandlers, HandlerCount(kGroupBHandlers)},
};
static_assert(SessionGroupsValid(kTwoGroups, SessionGroupCount(kTwoGroups)),
              "合法组数组应通过校验");
// 非法组数组：sessionType 不一致 / sessionInst 重复 / 空组，均应判 false
static_assert(!SessionGroupsValid((const SessionMsgTable[]){{"DTS", "a", kGroupAHandlers, 2},
                                                           {"NFO", "b", kGroupBHandlers, 1}},
                                  2),
              "sessionType 不一致应判 false");
static_assert(!SessionGroupsValid((const SessionMsgTable[]){{"DTS", "a", kGroupAHandlers, 2},
                                                           {"DTS", "a", kGroupBHandlers, 1}},
                                  2),
              "sessionInst 重复应判 false");
static_assert(!SessionGroupsValid(nullptr, 0), "空组数组应判 false");

void TestGroups() {
    // 按 sessionInst 选组
    const SessionMsgTable* g = FindSessionTable(kTwoGroups, SessionGroupCount(kTwoGroups), "grp_a");
    CHECK(g != nullptr && CStrEq(g->sessionInst, "grp_a") && g->count == 2);
    const SessionMsgTable* g2 =
        FindSessionTable(kTwoGroups, SessionGroupCount(kTwoGroups), "grp_b");
    CHECK(g2 != nullptr && g2->entries[0].msgId == 0x0011);
    // 未命中 / 线程本地（nullptr）不命中
    CHECK(FindSessionTable(kTwoGroups, SessionGroupCount(kTwoGroups), "nope") == nullptr);
    CHECK(FindSessionTable(kTwoGroups, SessionGroupCount(kTwoGroups), nullptr) == nullptr);
    CHECK(FindSessionTable(nullptr, 0, "a") == nullptr);

    // 组内分发：同 msgId 不同组互不干扰（组内查表，不经全局）
    int callsA = g_callsA, callsB = g_callsB;
    CHECK(MsgTable(g->entries, g->count).Dispatch(0x0011, nullptr, 0));
    CHECK(g_callsA == callsA + 1 && g_callsB == callsB);
    CHECK(MsgTable(g2->entries, g2->count).Dispatch(0x0011, nullptr, 0));
    CHECK(g_callsB == callsB + 1 && g_callsA == callsA + 1);
}

}  // namespace

int main() {
    TestDispatch();
    TestFindAndAt();
    TestSessionTable();
    TestMsgIdsUnique();
    TestGroups();
    std::printf("msg_table_test: %d checks, %d fails\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
