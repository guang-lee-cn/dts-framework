#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "dts_def.h"
#include "data_construct.h"

namespace dts {

class TdMap;
class ReportCache;

struct DataFeedStat {
    uint32_t matchedDataIds = 0;
    uint32_t tracked = 0;
    uint32_t dropped = 0;
    uint32_t cached = 0;
};

// 数据工厂：无锁（data 线程独占，pub-sub 保障数据流串行）。
// 数据带 DataType 进入 -> 匹配所有注册该 DataType 的 dataId -> 拆分被跟踪的。
class DataFactory {
public:
    static constexpr uint32_t TEMP_BLOCK_SIZE = 32 * 1024;   // 每 dataId 临时缓存块
    static constexpr uint16_t MAX_DATA_IDS = 64;

    static DataFactory& Instance();

    // 注册：dataId、DataType、加工类、临时缓存偏移、结构体大小
    void RegisterProcessor(uint16_t dataId, uint16_t dataType,
                           std::unique_ptr<DataConstruct> proc,
                           uint32_t tempOffset, uint32_t structSize);

    DataFeedStat Feed(char* data, uint32_t len, const TdMap& tdmap, ReportCache& report);

private:
    DataFactory() = default;
    const BigFrameHeader* ParseHeader(char* data, uint32_t len) const;

    std::unordered_map<uint16_t, std::vector<uint16_t>> m_typeToDataIds;   // DataType -> {dataId}
    std::unordered_map<uint16_t, std::unique_ptr<DataConstruct>> m_procs;  // dataId -> 加工类
    std::array<std::array<char, TEMP_BLOCK_SIZE>, MAX_DATA_IDS> m_tempPool;
};

}  // namespace dts

// 5 参数宏：dataId 整数值 | 加工类(继承 DataConstruct) | dataId结构体 | 临时缓存偏移 | DataType
#define REGISTER_DATA_PROCESSOR(DataId, ProcessorClass, DataStruct, TempOffset, DataType) \
    void RegisterDataId_##DataId() {                                                     \
        dts::DataFactory::Instance().RegisterProcessor(                                  \
            DataId, DataType, std::make_unique<ProcessorClass>(),                        \
            TempOffset, sizeof(DataStruct));                                             \
    }

void RegisterDataId_DATA_ID_CELL_PRB();
void RegisterDataId_DATA_ID_UE_BLER();

// 领域层初始化：注册加工子类（data 线程下任务时调用）
void DataDomainInit();