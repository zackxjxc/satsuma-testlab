// Windows SMBIOS 硬件身份读取与校验接口。
#pragma once

#include <string>
#include <string_view>

namespace satsuma {

// 将 UUID 规范化为小写 RFC 4122 文本，不合法时抛出 Error。
[[nodiscard]] std::string normalize_hardware_id(std::string_view value);

// 读取 SMBIOS System UUID，作为当前虚拟机的稳定硬件身份。
[[nodiscard]] std::string read_smbios_hardware_id();

}  // namespace satsuma
