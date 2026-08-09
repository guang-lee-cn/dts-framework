#include "data_mem_manager.h"

#include <memory>
#include <new>
#include <unordered_map>
#include <vector>

#include "data_ids.h"

namespace dts::data {

namespace {

constexpr size_t kAlign = 64;  // 池基址对齐（cache line，满足所有 cache 结构对齐）

uint32_t AlignUp(uint32_t v, uint32_t align) {
    return (v + align - 1) & ~(align - 1);
}

// hash region：(域, 周期) 一块连续预分配内存，内含 slotCap 个等大块
struct RegionDesc {
    uint16_t domain = 0;
    uint32_t periodTicks = 0;
    uint32_t slotCap = 0;
    uint32_t blockSize = 0;  // 含 CacheHead + dataId 槽区，16 对齐
    uint8_t* pool = nullptr;
    size_t poolSize = 0;
    std::vector<bool> used;
    std::vector<uint64_t> keys;
    std::unordered_map<uint16_t, uint32_t> slotOffset;  // dataId → 块内偏移（CacheHead 之后）

    ~RegionDesc() {
        if (pool != nullptr) {
            ::operator delete(pool, std::align_val_t{kAlign});
        }
    }
    CacheHead* BlockAt(uint32_t idx) const {
        return reinterpret_cast<CacheHead*>(pool + static_cast<size_t>(idx) * blockSize);
    }
};

}  // namespace

struct DataMemManager::Impl {
    std::vector<RegionDesc> regions;
    std::unordered_map<uint16_t, std::pair<uint8_t*, uint32_t>> fixedSlots;  // dataId → (base, size)
    bool init = false;

    RegionDesc* FindRegion(uint16_t domain, uint32_t periodTicks) {
        for (auto& r : regions) {
            if (r.domain == domain && r.periodTicks == periodTicks) {
                return &r;
            }
        }
        return nullptr;
    }
    // 由块指针反查 region（SlotOf 用；4 个 region 线性扫）
    RegionDesc* RegionOf(const CacheHead* block) {
        for (auto& r : regions) {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(block);
            if (p >= r.pool && p < r.pool + r.poolSize) {
                return &r;
            }
        }
        return nullptr;
    }
};

DataMemManager& DataMemManager::Instance() {
    static DataMemManager inst;
    return inst;
}

void DataMemManager::Init() {
    if (m_impl != nullptr && m_impl->init) {
        return;
    }
    if (m_impl == nullptr) {
        m_impl = new Impl();
    }

    // 按 spec 表分组建 region（needcache=true）/ 固定槽（needcache=false）
    auto build = [&](const DataIdSpec* specs, size_t n) {
        for (size_t i = 0; i < n; i++) {
            const auto& s = specs[i];
            if (!s.needCache) {
                if (m_impl->fixedSlots.find(s.dataId) == m_impl->fixedSlots.end()) {
                    uint32_t sz = AlignUp(s.cacheSize, 16);
                    auto* p = static_cast<uint8_t*>(::operator new(sz, std::align_val_t{kAlign}));
                    m_impl->fixedSlots.emplace(s.dataId, std::make_pair(p, sz));
                }
                continue;
            }
            RegionDesc* r = m_impl->FindRegion(s.dataType, s.periodTicks);
            if (r == nullptr) {
                RegionDesc nr;
                nr.domain = s.dataType;
                nr.periodTicks = s.periodTicks;
                nr.slotCap = (s.dataType == static_cast<uint16_t>(DataType::CELL)) ? kCellSlotCap : kUeSlotCap;
                nr.blockSize = AlignUp(sizeof(CacheHead), 16);
                m_impl->regions.push_back(std::move(nr));
                r = m_impl->FindRegion(s.dataType, s.periodTicks);
            }
            r->slotOffset[s.dataId] = r->blockSize;  // 槽紧跟 CacheHead 之后
            r->blockSize = AlignUp(r->blockSize + s.cacheSize, 16);
        }
    };
    build(kCellSpecs, sizeof(kCellSpecs) / sizeof(kCellSpecs[0]));
    build(kUeSpecs, sizeof(kUeSpecs) / sizeof(kUeSpecs[0]));

    // 分配每个 region 的池
    for (auto& r : m_impl->regions) {
        r.poolSize = static_cast<size_t>(r.slotCap) * r.blockSize;
        r.pool = static_cast<uint8_t*>(::operator new(r.poolSize, std::align_val_t{kAlign}));
        r.used.assign(r.slotCap, false);
        r.keys.assign(r.slotCap, 0);
    }
    m_impl->init = true;
}

CacheHead* DataMemManager::AcquireBlock(uint16_t domain, uint32_t periodTicks, uint64_t hashKey,
                                        uint32_t nowTick) {
    auto* r = m_impl->FindRegion(domain, periodTicks);
    if (r == nullptr) {
        return nullptr;
    }
    const uint32_t start = static_cast<uint32_t>(hashKey % r->slotCap);
    for (uint32_t i = 0; i < r->slotCap; i++) {
        const uint32_t idx = (start + i) % r->slotCap;
        if (!r->used[idx]) {
            r->used[idx] = true;
            r->keys[idx] = hashKey;
            CacheHead* h = r->BlockAt(idx);
            h->type = static_cast<DataType>(domain);
            h->periodTicks = periodTicks;
            h->lastTick = nowTick;
            return h;
        }
        if (r->keys[idx] == hashKey) {
            CacheHead* h = r->BlockAt(idx);  // 命中：刷新 TTL
            h->lastTick = nowTick;
            return h;
        }
    }
    return nullptr;  // 区域满
}

CacheHead* DataMemManager::FindBlock(uint16_t domain, uint32_t periodTicks, uint64_t hashKey) const {
    auto* r = m_impl->FindRegion(domain, periodTicks);
    if (r == nullptr) {
        return nullptr;
    }
    const uint32_t start = static_cast<uint32_t>(hashKey % r->slotCap);
    for (uint32_t i = 0; i < r->slotCap; i++) {
        const uint32_t idx = (start + i) % r->slotCap;
        if (!r->used[idx]) {
            return nullptr;  // 探测到空槽，键不存在
        }
        if (r->keys[idx] == hashKey) {
            return r->BlockAt(idx);
        }
    }
    return nullptr;
}

void* DataMemManager::SlotOf(const CacheHead* block, uint16_t dataId) {
    auto* r = m_impl->RegionOf(block);
    if (r == nullptr) {
        return nullptr;
    }
    auto it = r->slotOffset.find(dataId);
    if (it == r->slotOffset.end()) {
        return nullptr;
    }
    return reinterpret_cast<uint8_t*>(const_cast<CacheHead*>(block)) + it->second;
}

void* DataMemManager::FixedSlot(uint16_t dataId) {
    auto it = m_impl->fixedSlots.find(dataId);
    if (it == m_impl->fixedSlots.end()) {
        return nullptr;
    }
    return it->second.first;
}

void DataMemManager::EvictExpired(uint32_t nowTick) {
    for (auto& r : m_impl->regions) {
        const uint32_t ttl = kTtlPeriods * r.periodTicks;  // N 个周期未更新即过期
        for (uint32_t idx = 0; idx < r.slotCap; idx++) {
            if (!r.used[idx]) {
                continue;
            }
            CacheHead* h = r.BlockAt(idx);
            if (nowTick - h->lastTick > ttl) {
                r.used[idx] = false;  // 归还槽位
            }
        }
    }
}

}  // namespace dts::data
