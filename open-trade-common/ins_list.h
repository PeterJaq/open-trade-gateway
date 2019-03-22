#pragma once

#include "types.h"

//获取指定代码的合约/行情信息
Instrument* GetInstrument(const std::string& symbol);