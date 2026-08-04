#pragma once

#include "data_construct.h"

namespace dts {

// 小区管道加工子类：二次计算（归一化）+ 拆分到临时缓存
class CellPrbProcessor : public DataConstruct {
public:
    int Extra(char* raw, char* dest) override;
    int Hton() override;
};

}  // namespace dts
