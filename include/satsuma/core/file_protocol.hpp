// Satsuma Host/VM 任务文件协议版本。
#pragma once

namespace satsuma {

inline constexpr int kLegacyRunManifestProtocolVersion = 1;  // 仅支持隐式 SYSTEM
inline constexpr int kRunManifestProtocolVersion = 2;        // 支持显式任务运行身份

}  // namespace satsuma
