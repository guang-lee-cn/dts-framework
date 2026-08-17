#include "spa_mock.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "dts_agent/dts_agent_main.h"
#include "dts_def.h"

// 复用 data domain 的 dataId 大小表（spa/data 共享 raw layout，见 data_ids.h）
#include "data_ids.h"
#include "detmw.h"
#include "dts_mw.h"

namespace dts {

namespace {

constexpr uint32_t kRawHead = sizeof(uint16_t) + sizeof(uint32_t) * 2;  // dataType+cellId+cpId = 10
constexpr uint16_t kSlices = 8;  // 小帧：只发前 8 个 dataId 切片（越界由工厂跳过）

// 新布局 raw 帧：[u16 dataType=CELL][u32 cellId][u32 cpId][Σ切片]（替代旧 BigFrameHeader）
void BuildRaw(std::vector<uint8_t>& out, uint16_t cellId, uint32_t seq) {
    uint32_t len = kRawHead;
    for (uint16_t i = 0; i < kSlices; ++i) {
        len += dts::data::kCellCacheSizes[i];
    }
    out.resize(len, 0);
    const uint16_t dataType = static_cast<uint16_t>(dts::data::DataType::CELL);
    std::memcpy(out.data(), &dataType, sizeof(dataType));
    std::memcpy(out.data() + sizeof(uint16_t), &cellId, sizeof(cellId));
    const uint32_t cpId = 0;
    std::memcpy(out.data() + sizeof(uint16_t) + sizeof(uint32_t), &cpId, sizeof(cpId));
    uint32_t off = kRawHead;
    for (uint16_t i = 0; i < kSlices; ++i) {
        std::memset(out.data() + off, static_cast<int>(seq & 0xff), dts::data::kCellCacheSizes[i]);
        off += dts::data::kCellCacheSizes[i];
    }
}

// agent 读乒乓 → 发布到 data 线程（msg3，进程外 API 但本进程内投递）
void AgentPublish(uint64_t key, const void* data, uint32_t len, void*) {
    if (DtsMw() == nullptr) {
        return;  // 组合根未就绪：丢弃（writer 持续写，后续帧会补上）
    }
    DtsMw()->publish_external(detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_DATA,
                                              static_cast<uint32_t>(key)},
                              static_cast<const uint8_t*>(data), len);
}

}  // namespace

SpaMock::~SpaMock() {
    Stop();
}

void SpaMock::Stop() {
    m_stop.store(true);
    if (m_writer.joinable()) {
        m_writer.join();
    }
    dts_agent_destroy(m_agent);
    m_agent = nullptr;
}

void SpaMock::Start() {
    // agent 独立库联编：SPA 写乒乓（2ms）+ dts_agent_main 读翻转（1s）→ AgentPublish → data 线程
    AgentCfg cfg{};
    cfg.pingpong = &m_pingpong;
    cfg.publish = AgentPublish;
    cfg.pubCtx = nullptr;
    cfg.readPeriodMs = 1000;
    cfg.outKey = MSG_ID_AGENT_DATA;
    m_agent = dts_agent_main(&cfg);

    // 握手：通知 data 当前任务（重复发，发现完成前消息可能丢；OnTask 幂等）
    std::thread([this] {
        for (int i = 0; i < 100 && DtsMw() == nullptr; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (DtsMw() == nullptr) {
            return;
        }
        const TaskRequest req{1, 1};
        for (int i = 0; i < 6; ++i) {
            DtsMw()->publish_external(detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_DATA,
                                                      MSG_ID_DATA_TASK_ACTIVE},
                                      reinterpret_cast<const uint8_t*>(&req), sizeof(req));
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }).detach();

    std::printf("[spa] writer up, writing pingpong every 2ms\n");
    m_writer = std::thread([this] { writerLoop(); });
}

void SpaMock::writerLoop() {
    uint16_t cellId = 1;
    uint32_t seq = 0;
    while (!m_stop.load()) {
        std::vector<uint8_t> frame;
        BuildRaw(frame, cellId++, seq++);
        m_pingpong.Write(frame.data(), static_cast<uint32_t>(frame.size()));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

}  // namespace dts
