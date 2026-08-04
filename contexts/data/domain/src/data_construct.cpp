#include "data_construct.h"

#include "report_cache.h"

namespace dts {

void DataConstruct::Process(char* raw, uint32_t rawLen, const std::vector<uint16_t>& tasks) {
    (void)rawLen;   // 骨架：rawLen 校验由工厂侧完成
    m_tempLen = 0;
    int n = Extra(raw, m_temp);   // 拆分到临时缓存
    if (n <= 0 || static_cast<uint32_t>(n) > m_tempCap) return;
    m_tempLen = static_cast<uint32_t>(n);
    int h = Hton();              // 字节序处理
    if (h <= 0 || static_cast<uint32_t>(h) > m_tempLen) return;
    for (uint16_t taskId : tasks) {
        Report(taskId, static_cast<uint32_t>(h));
    }
}

int DataConstruct::Report(uint16_t taskId, uint32_t len) {
    if (m_reportCache == nullptr || m_temp == nullptr) return -1;
    m_reportCache->Append(taskId, reinterpret_cast<const uint8_t*>(m_temp), len);
    return 0;
}

}  // namespace dts
