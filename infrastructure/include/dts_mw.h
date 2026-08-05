#pragma once

#include "detmw.h"

namespace dts {

// 全局 detmw 实例访问（组合根初始化后设置，各 context 消息处理用）。
// 返回非拥有指针：生命周期由组合根（Process::comm）独占管理，调用方不得持有或释放；
// 下电顺序固定为 DtsMwSet(nullptr) → comm 析构（Process::Stop）。
detmw::Communicator* DtsMw();
void DtsMwSet(detmw::Communicator* h);

}  // namespace dts
