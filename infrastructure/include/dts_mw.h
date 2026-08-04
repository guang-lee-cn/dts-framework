#pragma once

#include "detmw.h"

namespace dts {

// 全局 detmw 实例访问（组合根初始化后设置，各 context 消息处理用）
detmw::Communicator* DtsMw();
void DtsMwSet(detmw::Communicator* h);

}  // namespace dts
