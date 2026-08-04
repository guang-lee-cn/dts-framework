#include "dts_mw.h"

namespace dts {

namespace {
detmw::Communicator* g_mw = nullptr;
}  // namespace

detmw::Communicator* DtsMw() {
    return g_mw;
}

void DtsMwSet(detmw::Communicator* h) {
    g_mw = h;
}

}  // namespace dts
