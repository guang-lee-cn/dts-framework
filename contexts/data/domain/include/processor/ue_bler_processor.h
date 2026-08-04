#pragma once

#include "data_construct.h"

namespace dts {

// UE 管道加工子类：透传
class UeBlerProcessor : public DataConstruct {
public:
    int Extra(char* raw, char* dest) override;
    int Hton() override;
};

}  // namespace dts
