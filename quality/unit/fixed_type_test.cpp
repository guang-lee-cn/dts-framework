// 单元自测：FixedBytesType（定长 plain 类型）—— 序列化 roundtrip + plain/bounded 语义。
// 背景：raw 零拷贝通道的类型基础（is_plain → DataSharing 候选；严格定长语义）。
// 运行：构建后直接执行；退出码 0 = 全过。
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "detmw_fixed_type.h"

using namespace detmw;

namespace {

int g_checks = 0;
int g_fails = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_checks;                                                     \
        if (!(cond)) {                                                  \
            ++g_fails;                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

void TestTypeSemantics() {
    FixedBytesType t(1024);
    CHECK(t.Size() == 1024);
    CHECK(t.is_bounded() == true);
    CHECK(t.is_plain(eprosima::fastdds::dds::XCDR2_DATA_REPRESENTATION) == true);
    CHECK(t.max_serialized_type_size == 1024);
    CHECK(t.calculate_serialized_size(nullptr, eprosima::fastdds::dds::XCDR2_DATA_REPRESENTATION) == 1024);
    CHECK(std::string(t.get_name()) == "detmw::FixedBytes_1024");
}

void TestSerializeRoundtrip() {
    const uint32_t kSize = 1024;
    FixedBytesType t(kSize);

    // 构造样本 + payload
    std::vector<uint8_t> src(kSize, 0);
    for (uint32_t i = 0; i < kSize; ++i) {
        src[i] = static_cast<uint8_t>(i * 31 + 7);
    }
    eprosima::fastdds::rtps::SerializedPayload_t payload;
    CHECK(t.serialize(&src, payload, eprosima::fastdds::dds::XCDR2_DATA_REPRESENTATION) == true);
    CHECK(payload.length == kSize);

    // 定长反序列化：预分配 buffer（create_data 语义）→ 内容一致
    std::unique_ptr<std::vector<uint8_t>> dst(
        static_cast<std::vector<uint8_t>*>(t.create_data()));
    CHECK(t.deserialize(payload, dst.get()) == true);
    CHECK(dst->size() == kSize);
    CHECK(std::memcmp(dst->data(), src.data(), kSize) == 0);

    // 截断 payload（对端异常/兼容）：不越界，按实际长度收
    payload.length = 100;
    std::unique_ptr<std::vector<uint8_t>> dst2(
        static_cast<std::vector<uint8_t>*>(t.create_data()));
    CHECK(t.deserialize(payload, dst2.get()) == true);
    CHECK(dst2->size() == 100);

    // create_data/delete_data 生命周期
    void* d = t.create_data();
    CHECK(d != nullptr);
    t.delete_data(d);
}

}  // namespace

int main() {
    TestTypeSemantics();
    TestSerializeRoundtrip();
    std::printf("fixed_type_test: %d checks, %d fails\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
