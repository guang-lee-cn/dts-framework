#include "processor/cell_prb_processor.h"

#include <cstring>

#include "data_factory.h"
#include "dts_def.h"

namespace dts {

int CellPrbProcessor::Extra(char* raw, char* dest) {
    if (raw == nullptr || dest == nullptr) return -1;
    if (m_tempCap < sizeof(CellPrbData)) return -1;

    CellPrbData d;
    std::memcpy(&d, raw, sizeof(d));
    d.prb %= 1000;   // 二次计算：归一化
    std::memcpy(dest, &d, sizeof(d));
    m_tempLen = sizeof(d);
    return static_cast<int>(sizeof(d));
}

int CellPrbProcessor::Hton() {
    // 骨架：透传，字节序转换后续实现
    return static_cast<int>(m_tempLen);
}

}  // namespace dts

using namespace dts;
REGISTER_DATA_PROCESSOR(DATA_ID_CELL_PRB, CellPrbProcessor, CellPrbData, 0, TYPE_CELL);