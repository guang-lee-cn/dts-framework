#include "processor/ue_bler_processor.h"

#include <cstring>

#include "data_factory.h"
#include "dts_def.h"

namespace dts {

int UeBlerProcessor::Extra(char* raw, char* dest) {
    if (raw == nullptr || dest == nullptr) return -1;
    if (m_tempCap < sizeof(UeBlerData)) return -1;

    UeBlerData d;
    std::memcpy(&d, raw, sizeof(d));
    std::memcpy(dest, &d, sizeof(d));
    m_tempLen = sizeof(d);
    return static_cast<int>(sizeof(d));
}

int UeBlerProcessor::Hton() {
    // 骨架：透传，字节序转换后续实现
    return static_cast<int>(m_tempLen);
}

}  // namespace dts

using namespace dts;
REGISTER_DATA_PROCESSOR(DATA_ID_UE_BLER, UeBlerProcessor, UeBlerData, 0, TYPE_UE);