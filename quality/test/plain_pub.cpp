// plain 定长通道冒烟发送端：向 (DTS, data, 9) 发布 N 帧**严格定长**（plain_size=1024）消息。
// 验证 FixedBytesType 通道：定长校验 / 发送路径（DataSharing 可用时 loan 零拷贝，否则回退拷贝）。
// 用法：plain_pub <plain-pub.json> [count] [size]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "detmw.h"
#include "dts_def.h"

using namespace dts;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s <plain-pub.json> [count] [size]\n", argv[0]);
        return 1;
    }
    const uint32_t count = (argc >= 3) ? static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 10)) : 10;
    const uint32_t size = (argc >= 4) ? static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 10)) : 1024;

    detmw::Communicator comm(argv[1]);
    if (!comm.good()) {
        std::printf("[plain-pub] communicator init failed\n");
        return 1;
    }

    std::printf("[plain-pub] waiting for discovery... (count=%u size=%u)\n", count, size);
    std::this_thread::sleep_for(std::chrono::seconds(3));

    std::vector<uint8_t> frame(size, 0);
    uint32_t fail = 0;
    const auto ep = detmw::endpoint{SESSION_TYPE_DTS, SESSION_INST_DATA, 9};  // 9 = plain 测试通道
    for (uint32_t i = 0; i < count; ++i) {
        for (uint32_t j = 0; j < size; ++j) {
            frame[j] = static_cast<uint8_t>((i * 13 + j) & 0xFF);
        }
        if (comm.publish_external(ep, frame.data(), size) != 0) {
            ++fail;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));  // 排空

    std::printf("[plain-pub] sent %u x %uB (fail=%u)\n", count, size, fail);
    return fail == 0 ? 0 : 1;
}
