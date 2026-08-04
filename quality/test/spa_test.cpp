#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "detmw.h"
#include "dts_def.h"

using namespace dts;

// spa 测试进程：发 <32k 大帧（小区级 + UE 级）给 dts data 线程(msg3)
// 用法：spa_test <spa.json>
int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s <spa.json>\n", argv[0]);
        return 1;
    }
    detmw::Communicator comm(argv[1]);

    std::printf("[spa] waiting for static discovery...\n");
    std::this_thread::sleep_for(std::chrono::seconds(3));

    CellPrbData cell{100, 2000};
    UeBlerData ue{200, 15};
    BigFrameHeader hdr{};
    hdr.magic = BIG_FRAME_MAGIC;
    hdr.dataType = TYPE_CELL;
    hdr.dataCount = 2;
    uint32_t off = sizeof(BigFrameHeader) + 2 * sizeof(BigFrameHeader::Entry);
    hdr.entries[0] = {DATA_ID_CELL_PRB, 0, off, sizeof(CellPrbData)};
    off += sizeof(CellPrbData);
    hdr.entries[1] = {DATA_ID_UE_BLER, 0, off, sizeof(UeBlerData)};
    hdr.totalLen = off + sizeof(UeBlerData);

    std::vector<uint8_t> frame(hdr.totalLen);
    std::memcpy(frame.data(), &hdr, sizeof(BigFrameHeader));
    std::memcpy(frame.data() + sizeof(BigFrameHeader), hdr.entries,
                2 * sizeof(BigFrameHeader::Entry));
    std::memcpy(frame.data() + hdr.entries[0].offset, &cell, sizeof(cell));
    std::memcpy(frame.data() + hdr.entries[1].offset, &ue, sizeof(ue));

    std::printf("[spa] sending %zu bytes to data thread (msg3)\n", frame.size());
    int rc = comm.publish_external(detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_DATA,
                                                   MSG_ID_AGENT_DATA},
                                   frame.data(), static_cast<uint32_t>(frame.size()));
    std::this_thread::sleep_for(std::chrono::seconds(1));

    std::printf("[spa] %s\n", rc == 0 ? "PASS" : "FAIL");
    return rc == 0 ? 0 : 1;
}
