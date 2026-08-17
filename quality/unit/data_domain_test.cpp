// 单元自测：data domain 纯逻辑组件（零 DDS）——
//   DataMemManager（hash 槽位 + TTL 回收）/ ReportAggregator（合并上报）/ ExtractorRegistry。
// 运行：构建后直接执行；退出码 0 = 全过。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "data_factory_v2.h"  // ReportSink 抽象
#include "data_ids.h"
#include "data_mem_manager.h"
#include "extractor_registry.h"
#include "report_aggregator.h"

using namespace dts::data;

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

// ---- ReportSink 假实现：计数 + 记录载荷 ----
class CountingSink : public ReportSink {
public:
    void Publish(const void* data, uint32_t len) override {
        ++publishCount;
        lastLen = len;
        std::memcpy(lastPayload, data, len < sizeof(lastPayload) ? len : sizeof(lastPayload));
    }
    int publishCount = 0;
    uint32_t lastLen = 0;
    uint8_t lastPayload[64] = {0};
};

void TestMemManager() {
    DataMemManager& mem = DataMemManager::Instance();
    mem.Init();  // 幂等；按 spec 建 region（500 cell dataId → (CELL, 10tick) 1 个 region）

    const uint16_t domain = static_cast<uint16_t>(DataType::CELL);
    const uint32_t period = kPeriod1S;
    const uint64_t keyA = CellHashKey(100, 1);

    // 获取：hash 槽位，同键命中刷新 TTL
    CacheHead* a1 = mem.AcquireBlock(domain, period, keyA, 1);
    CHECK(a1 != nullptr);
    CHECK(mem.FindBlock(domain, period, keyA) == a1);  // 同键命中
    CHECK(a1->lastTick == 1);
    mem.AcquireBlock(domain, period, keyA, 5);  // 再获取刷新 TTL
    CHECK(a1->lastTick == 5);

    // 槽位独立：不同键不同块（线性探测）
    const uint64_t keyB = CellHashKey(100, 2);
    CacheHead* b1 = mem.AcquireBlock(domain, period, keyB, 6);
    CHECK(b1 != nullptr && b1 != a1);

    // SlotOf：dataId → 块内 packed 槽偏移（CacheHead 对齐后首槽）
    const size_t kHeadAlign = (sizeof(CacheHead) + 7) & ~static_cast<size_t>(7);
    const uint16_t did0 = kCellDataIdBase;  // 第一个 dataId
    void* slot0 = mem.SlotOf(a1, did0);
    CHECK(slot0 != nullptr);
    CHECK(slot0 == reinterpret_cast<uint8_t*>(a1) + kHeadAlign);
    // 未注册 dataId：返回 nullptr
    CHECK(mem.SlotOf(a1, static_cast<uint16_t>(kCellDataIdBase + kCellDataIdCount + 1)) == nullptr);

    // TTL 回收：超 kTtlPeriods × periodTicks 未更新 → 槽位归还
    mem.AcquireBlock(domain, period, keyA, 1);  // 重置 lastTick=1
    mem.EvictExpired(1 + kTtlPeriods * period);  // 恰未超期（> ttl 才算超期）
    CHECK(mem.FindBlock(domain, period, keyA) != nullptr);
    mem.EvictExpired(1 + kTtlPeriods * period + 1);  // 超期 1 tick
    CHECK(mem.FindBlock(domain, period, keyA) == nullptr);  // 已回收
}

void TestReportAggregator() {
    CountingSink sink;
    ReportAggregator agg;
    agg.SetSink(&sink);

    // 空帧不发
    agg.Begin(7);
    agg.Flush(1000, 1);
    CHECK(sink.publishCount == 0);

    // 攒切片 → Flush 一次 publish：ReportHeader + n×(SubHeader+data)
    uint8_t slice[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22};
    agg.Begin(7);
    agg.Add(100, slice, 8);
    agg.Add(101, slice, 4);
    agg.Flush(1234, 42);
    CHECK(sink.publishCount == 1);
    CHECK(sink.lastLen == sizeof(ReportHeader) + (sizeof(SubHeader) + 8) + (sizeof(SubHeader) + 4));
    const auto* hdr = reinterpret_cast<const ReportHeader*>(sink.lastPayload);
    CHECK(hdr->taskId == 7 && hdr->seq == 42 && hdr->timestampMs == 1234);
    CHECK(hdr->payloadLen == sink.lastLen - sizeof(ReportHeader));

    // 超容切片丢弃（kCap 上限，MVP 不分片）：加超量切片后 Flush 长度不超 kCap（无溢出）
    ReportAggregator big;
    CountingSink sink2;
    big.SetSink(&sink2);
    big.Begin(1);
    const uint32_t kCap = ReportAggregator::kCap;
    std::vector<uint8_t> chunk(256, 0);
    const uint32_t kOverAdd = (kCap / (sizeof(SubHeader) + 256)) + 10;  // 超出容量
    for (uint32_t i = 0; i < kOverAdd; ++i) {
        big.Add(1, chunk.data(), chunk.size());
    }
    big.Flush(0, 0);
    CHECK(sink2.publishCount == 1);
    CHECK(sink2.lastLen <= kCap);    // 帧长受 kCap 约束，未越界
    CHECK(big.Drops() == kOverAdd - (kCap - sizeof(ReportHeader)) / (sizeof(SubHeader) + 256));
    // 不静默：超容切片有计数（get_data_stats 可查）
    CHECK(big.Drops() > 0);
}

// 全量 500 dataId 合并上报回归：真实 32K raw 的切片大小（kCellCacheSizes）全量攒入，
// 不得超 kCap、不得丢切片（曾 kCap=32K 时每帧丢尾部 ~29 切片——34778 > 32768）
void TestFullCellMerge() {
    ReportAggregator agg;
    CountingSink sink;
    agg.SetSink(&sink);
    agg.Begin(1);

    uint64_t expectLen = sizeof(ReportHeader);
    std::vector<uint8_t> cache(200, 0);  // 上限缓冲（实际按 kCellCacheSizes[i] 拷贝）
    for (uint16_t i = 0; i < kCellDataIdCount; ++i) {
        const uint16_t dataId = kCellDataIdBase + i;
        const uint32_t sz = kCellCacheSizes[i];
        agg.Add(dataId, cache.data(), sz);
        expectLen += sizeof(SubHeader) + sz;
    }
    agg.Flush(0, 0);
    CHECK(agg.Drops() == 0);                              // 全量 500 切片零丢弃
    CHECK(sink.publishCount == 1);
    CHECK(sink.lastLen == expectLen);                     // 20 + 2000 + 32758 = 34778
    CHECK(sink.lastLen <= ReportAggregator::kCap);        // 48K 容量内
    // 头字段
    const auto* hdr = reinterpret_cast<const ReportHeader*>(sink.lastPayload);
    CHECK(hdr->taskId == 1 && hdr->payloadLen == sink.lastLen - sizeof(ReportHeader));
}

void TestRegistry() {
    ExtractorRegistry& reg = ExtractorRegistry::Instance();
    const uint16_t did = 0x7777;
    const uint16_t domain = static_cast<uint16_t>(DataType::CELL);
    CHECK(reg.Register(did, domain, 16, kPeriod1S, true, nullptr) == true);
    CHECK(reg.Register(did, domain, 16, kPeriod1S, true, nullptr) == false);  // 重名拒绝
    const ExtractorSpec* spec = reg.Find(domain, did);
    CHECK(spec != nullptr);
    CHECK(spec->dataId == did && spec->cacheSize == 16 && spec->periodTicks == kPeriod1S);
    CHECK(reg.Find(static_cast<uint16_t>(DataType::UE), did) == nullptr);  // 跨域不命中
}

// 帧头解析对齐回归：raw = [u16 dataType][u32 cellId][u32 cpId/ueId][...]
// 曾用 uint32_t* 错位读取 → 所有小区 hash 键错乱共享同一 cache block
void TestExtractRawKey() {
    uint8_t frame[32] = {0};
    // CELL：dataType=0, cellId=100, cpId=1
    const uint16_t cellType = static_cast<uint16_t>(DataType::CELL);
    std::memcpy(frame, &cellType, sizeof(cellType));
    const uint32_t cellId = 100;
    const uint32_t cpId = 1;
    std::memcpy(frame + sizeof(uint16_t), &cellId, sizeof(cellId));
    std::memcpy(frame + sizeof(uint16_t) + sizeof(uint32_t), &cpId, sizeof(cpId));
    CHECK(ExtractRawKey(cellType, frame, sizeof(frame)) == CellHashKey(100, 1));
    // 不同小区 → 不同键（对齐修复的核心回归：此前全部归 0/错位）
    std::memcpy(frame + sizeof(uint16_t), &cellId, sizeof(cellId));
    const uint32_t cellId2 = 101;
    std::memcpy(frame + sizeof(uint16_t), &cellId2, sizeof(cellId2));
    CHECK(ExtractRawKey(cellType, frame, sizeof(frame)) == CellHashKey(101, 1));
    // 帧不足 → 0（占位键）
    CHECK(ExtractRawKey(cellType, frame, sizeof(uint16_t) + sizeof(uint32_t) * 2 - 1) == 0);
    CHECK(ExtractRawKey(cellType, nullptr, 32) == 0);
    // UE：dataType=UE, cellId=7, ueId=9
    const uint16_t ueType = static_cast<uint16_t>(DataType::UE);
    std::memcpy(frame, &ueType, sizeof(ueType));
    const uint32_t ueId = 9;
    std::memcpy(frame + sizeof(uint16_t), &cellId2, sizeof(cellId2));
    std::memcpy(frame + sizeof(uint16_t) + sizeof(uint32_t), &ueId, sizeof(ueId));
    CHECK(ExtractRawKey(ueType, frame, sizeof(frame)) == UeHashKey(101, 9));
}

}  // namespace

int main() {
    TestMemManager();
    TestReportAggregator();
    TestFullCellMerge();
    TestRegistry();
    TestExtractRawKey();
    std::printf("data_domain_test: %d checks, %d fails\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
