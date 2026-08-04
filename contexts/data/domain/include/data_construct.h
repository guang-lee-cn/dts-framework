#pragma once

#include <cstdint>
#include <vector>

namespace dts {

class ReportCache;

// 加工基类：raw 数据 -> Extra 拆分 -> Hton 字节序 -> Report 上报
class DataConstruct {
public:
    virtual ~DataConstruct() = default;

    // 工厂入口（模板方法）：dataId 被跟踪时由工厂调用
    void Process(char* raw, uint32_t rawLen, const std::vector<uint16_t>& tasks);

    // 加工子类实现
    virtual int Extra(char* raw, char* dest) = 0;  // 拆分 raw -> dest，返回字节数
    virtual int Hton() = 0;                        // 字节序，返回处理后字节数

    void BindTempCache(char* temp, uint32_t cap) {
        m_temp = temp;
        m_tempCap = cap;
    }
    void BindReportCache(ReportCache* cache) { m_reportCache = cache; }

protected:
    // 上报：临时缓存 -> 上报缓存
    int Report(uint16_t taskId, uint32_t len);

    char* m_temp = nullptr;
    uint32_t m_tempCap = 0;
    uint32_t m_tempLen = 0;   // Extra 写入后的有效长度，Hton 基于它处理
    ReportCache* m_reportCache = nullptr;
};

}  // namespace dts
