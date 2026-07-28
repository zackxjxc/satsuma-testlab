// Satsuma Host/VM 任务文件协议版本。
#pragma once

#include <cstddef>
#include <cstdint>

namespace satsuma {

inline constexpr int kLegacyRunManifestProtocolVersion = 1;  // 仅支持隐式 SYSTEM
inline constexpr int kRunManifestProtocolVersion = 2;        // 支持显式任务运行身份
inline constexpr std::size_t kMaxArtifactsPerRun = 256;      // 单次运行 Artifact 数量上限
inline constexpr std::size_t kMaxStepsPerRun = 256;          // 主步骤或 finally 步骤数量上限
inline constexpr std::size_t kMaxArgumentsPerStep = 256;     // 单步骤参数数量上限
inline constexpr std::size_t kMaxCollectedFilesPerStep = 128;// 单步骤收集文件数量上限
inline constexpr std::uintmax_t kMaxArtifactBytes = 2ULL * 1024 * 1024 * 1024;
inline constexpr std::uintmax_t kMaxCollectedFileBytes = 512ULL * 1024 * 1024;
inline constexpr std::uintmax_t kMaxCollectedTotalBytes = 2ULL * 1024 * 1024 * 1024;
inline constexpr std::uint64_t kDefaultMaxOutputBytes = 64ULL * 1024 * 1024;

}  // namespace satsuma
