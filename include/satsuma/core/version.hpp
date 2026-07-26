// Satsuma 构建版本常量。
#pragma once

#include <string_view>

#ifndef SATSUMA_VERSION
#error "SATSUMA_VERSION must be defined by the build"
#endif

namespace satsuma {

inline constexpr std::string_view kVersion = SATSUMA_VERSION;

}  // namespace satsuma
