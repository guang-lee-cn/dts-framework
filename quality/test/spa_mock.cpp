#include "spa_mock.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "dts_agent/dts_agent_main.h"
#include "dts_def.h"

namespace dts {

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
    // agent 独立库联编：SPA 提供乒乓 + 发布回调（平台 pubsub），dts_agent_main 启动读线程
    AgentCfg cfg{};
    cfg.pingpong = &m_pingpong;
    cfg.publish = nullptr;  // TODO(platform): 注入平台 pubsub 发布回调（读乒乓 -> 发布到 data 线程）
    cfg.pubCtx = nullptr;
    cfg.readPeriodMs = 1000;
    cfg.outKey = MSG_ID_AGENT_DATA;
    m_agent = dts_agent_main(&cfg);

    std::printf("[spa] writer up, writing pingpong every 2ms\n");
    m_writer = std::thread([this] { writerLoop(); });
}

void SpaMock::writerLoop() {
    uint16_t cellId = 1;
    uint16_t ueId = 1;
    uint32_t prb = 1000;
    uint16_t bler = 5;
    while (!m_stop.load()) {
        std::vector<uint8_t> frame;
        // 交替写小区级 / UE 级帧（帧头带 dataType）
        buildCellFrame(frame, cellId++, prb++);
        m_pingpong.Write(frame.data(), static_cast<uint32_t>(frame.size()));
        buildUeFrame(frame, ueId++, bler++);
        m_pingpong.Write(frame.data(), static_cast<uint32_t>(frame.size()));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void SpaMock::buildCellFrame(std::vector<uint8_t>& out, uint16_t cellId, uint32_t prb) {
    CellPrbData cell{cellId, prb};
    UeBlerData other{0, 0};   // DATA_ID_UE_OTHER 未注册 -> 演示按 DataType 过滤

    BigFrameHeader hdr{};
    hdr.magic = BIG_FRAME_MAGIC;
    hdr.dataType = TYPE_CELL;
    hdr.dataCount = 2;

    uint32_t off = sizeof(BigFrameHeader) + 2 * sizeof(BigFrameHeader::Entry);
    hdr.entries[0] = {DATA_ID_CELL_PRB, 0, off, sizeof(CellPrbData)};
    off += sizeof(CellPrbData);
    hdr.entries[1] = {DATA_ID_UE_OTHER, 0, off, sizeof(UeBlerData)};
    hdr.totalLen = off + sizeof(UeBlerData);

    out.resize(hdr.totalLen);
    std::memcpy(out.data(), &hdr, sizeof(BigFrameHeader));
    std::memcpy(out.data() + sizeof(BigFrameHeader), hdr.entries, 2 * sizeof(BigFrameHeader::Entry));
    std::memcpy(out.data() + hdr.entries[0].offset, &cell, sizeof(cell));
    std::memcpy(out.data() + hdr.entries[1].offset, &other, sizeof(other));
}

void SpaMock::buildUeFrame(std::vector<uint8_t>& out, uint16_t ueId, uint16_t bler) {
    UeBlerData ue{ueId, bler};

    BigFrameHeader hdr{};
    hdr.magic = BIG_FRAME_MAGIC;
    hdr.dataType = TYPE_UE;
    hdr.dataCount = 1;

    uint32_t off = sizeof(BigFrameHeader) + sizeof(BigFrameHeader::Entry);
    hdr.entries[0] = {DATA_ID_UE_BLER, 0, off, sizeof(UeBlerData)};
    hdr.totalLen = off + sizeof(UeBlerData);

    out.resize(hdr.totalLen);
    std::memcpy(out.data(), &hdr, sizeof(BigFrameHeader));
    std::memcpy(out.data() + sizeof(BigFrameHeader), hdr.entries, sizeof(BigFrameHeader::Entry));
    std::memcpy(out.data() + hdr.entries[0].offset, &ue, sizeof(ue));
}

}  // namespace dts
