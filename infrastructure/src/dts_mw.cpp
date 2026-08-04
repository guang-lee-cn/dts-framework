#include "dts_mw.h"

namespace dts {

namespace {
detmw_handle* g_mw = nullptr;
}  // namespace

detmw_handle* DtsMw() {
    return g_mw;
}

void DtsMwSet(detmw_handle* h) {
    g_mw = h;
}

}  // namespace dts
