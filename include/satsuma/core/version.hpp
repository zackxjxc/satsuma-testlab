// Satsuma 构建版本常量。
#pragma once

#include <string_view>

#include "satsuma/build_identity.hpp"

#ifndef SATSUMA_VERSION
#error "SATSUMA_VERSION must be defined by the build"
#endif
#ifndef SATSUMA_BUILD_NUMBER
#error "SATSUMA_BUILD_NUMBER must be defined by the build"
#endif
#ifndef SATSUMA_BUILD_ATTEMPT
#error "SATSUMA_BUILD_ATTEMPT must be defined by the build"
#endif
#ifndef SATSUMA_GIT_COMMIT
#error "SATSUMA_GIT_COMMIT must be defined by the build"
#endif

namespace satsuma {

inline constexpr std::string_view kVersion = SATSUMA_VERSION;
inline constexpr std::string_view kBuildNumber = SATSUMA_BUILD_NUMBER;
inline constexpr std::string_view kBuildAttempt = SATSUMA_BUILD_ATTEMPT;
inline constexpr std::string_view kGitCommit = SATSUMA_GIT_COMMIT;
inline constexpr std::string_view kBuildId = SATSUMA_BUILD_ID;

}  // namespace satsuma
