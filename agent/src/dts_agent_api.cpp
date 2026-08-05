#include "dts_agent/dts_agent_api.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "log.h"

namespace dts {

namespace {
constexpr uint32_t kAgentBufSize = 64 * 1024;
}  // namespace

struct AgentImpl {
    AgentCfg cfg;
    std::thread reader;
    std::atomic<bool> stop{false};
};

static void agentReaderLoop(AgentImpl* self) {
    std::vector<uint8_t> buf(kAgentBufSize);
    while (!self->stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(self->cfg.readPeriodMs));
        uint32_t n = self->cfg.pingpong->ReadAndFlip(buf.data(), static_cast<uint32_t>(buf.size()));
        if (n == 0) continue;
        log::Info("[agent] read {} bytes after flip, publish key={:#x}", n, self->cfg.outKey);
        if (self->cfg.publish != nullptr) {
            self->cfg.publish(self->cfg.outKey, buf.data(), n, self->cfg.pubCtx);
        }
    }
}

AgentHandle dts_agent_create(const AgentCfg* cfg) {
    auto* self = new AgentImpl();
    self->cfg = *cfg;
    self->reader = std::thread(agentReaderLoop, self);
    return self;
}

void dts_agent_destroy(AgentHandle h) {
    if (h == nullptr) return;
    h->stop.store(true);
    if (h->reader.joinable()) {
        h->reader.join();
    }
    delete h;
}

}  // namespace dts
