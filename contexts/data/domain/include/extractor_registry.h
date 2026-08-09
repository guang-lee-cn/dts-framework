#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "data_ids.h"
#include "data_model.h"
#include "extractor.h"

namespace dts::data {

// 加工类注册规格：dataId → Extra + cache 布局 + needcache/周期
struct ExtractorSpec {
    uint16_t dataId;
    uint16_t dataType;     // DataType::CELL / UE
    uint32_t cacheSize;    // sizeof(dataId 结构)
    uint32_t periodTicks;  // 上报周期
    bool needCache;
    std::unique_ptr<Extractor> proc;  // 加工实例
};

// 注册表：初始化静态注册全集（REGISTER_DATAID_EXTRACTOR 宏，全大写）；无动态口。
// 两级路由：dataType → dataId → ExtractorSpec
class ExtractorRegistry {
public:
    static ExtractorRegistry& Instance();

    // 登记（重名返回 false）
    bool Register(uint16_t dataId, uint16_t dataType, uint32_t cacheSize,
                  uint32_t periodTicks, bool needCache, std::unique_ptr<Extractor> proc);

    // 查询：(dataType, dataId) → spec
    const ExtractorSpec* Find(uint16_t dataType, uint16_t dataId) const;

    // 遍历（按 dataType 过滤）；激活集过滤用
    void ForEachByDomain(uint16_t dataType, const std::function<void(const ExtractorSpec&)>& fn) const;

private:
    ExtractorRegistry() = default;
    mutable std::mutex m_mutex;
    // key = (dataType<<16) | dataId
    std::unordered_map<uint32_t, std::unique_ptr<ExtractorSpec>> m_table;
};

}  // namespace dts::data

// 加工类注册宏（全大写）：dataId | C<Cache> 子类 | cache 结构 | dataType | needcache(周期取自 dataId spec)
// 用法：在 .cpp 用 REGISTER_DATAID_EXTRACTOR(1, CCellData01, CellData01, CELL)
#define REGISTER_DATAID_EXTRACTOR(DataId, ClassName, CacheStruct, DataTypeEnum) \
    namespace {                                                                 \
    struct ClassName##_reg {                                                    \
        ClassName##_reg() {                                                     \
            const auto* _s = dts::data::SpecOf(DataId);                         \
            dts::data::ExtractorRegistry::Instance().Register(                  \
                DataId, static_cast<uint16_t>(dts::data::DataType::DataTypeEnum), \
                sizeof(CacheStruct), _s->periodTicks, _s->needCache,            \
                std::make_unique<ClassName>());                                 \
        }                                                                       \
    };                                                                          \
    static ClassName##_reg g_##ClassName##_reg;                                 \
    }
