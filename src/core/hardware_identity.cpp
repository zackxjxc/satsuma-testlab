// Windows SMBIOS 硬件身份读取与校验实现。
#include "satsuma/core/hardware_identity.hpp"

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include <windows.h>

#include "satsuma/core/errors.hpp"

namespace satsuma {
namespace {

// 检查 SMBIOS 未设置 UUID 使用的全 0 或全 FF 哨兵值。
[[nodiscard]] bool is_missing_uuid(const std::array<std::uint8_t, 16>& uuid) {
    bool all_zero = true;
    bool all_ff = true;
    for (const std::uint8_t byte : uuid) {
        all_zero = all_zero && byte == 0;
        all_ff = all_ff && byte == 0xff;
    }
    return all_zero || all_ff;
}

// 按 SMBIOS 2.6+ 字节序输出 System UUID。
[[nodiscard]] std::string format_smbios_uuid(const std::array<std::uint8_t, 16>& uuid) {
    const std::array<std::size_t, 16> order{
        3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15,
    };
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < order.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            output << '-';
        }
        output << std::setw(2) << static_cast<unsigned int>(uuid[order[index]]);
    }
    return output.str();
}

}  // namespace

std::string normalize_hardware_id(const std::string_view value) {
    if (value.size() != 36) {
        throw Error("hardware_id must be a canonical UUID");
    }
    std::string normalized;
    normalized.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        const bool separator = index == 8 || index == 13 || index == 18 || index == 23;
        const unsigned char character = static_cast<unsigned char>(value[index]);
        if (separator) {
            if (character != '-') {
                throw Error("hardware_id must be a canonical UUID");
            }
            normalized.push_back('-');
            continue;
        }
        if (std::isxdigit(character) == 0) {
            throw Error("hardware_id must be a canonical UUID");
        }
        normalized.push_back(static_cast<char>(std::tolower(character)));
    }
    if (normalized == "00000000-0000-0000-0000-000000000000" ||
        normalized == "ffffffff-ffff-ffff-ffff-ffffffffffff") {
        throw Error("hardware_id must not use an unset SMBIOS UUID");
    }
    return normalized;
}

std::string read_smbios_hardware_id() {
    const DWORD provider =
        ('R' << 24) | ('S' << 16) | ('M' << 8) | 'B';
    const UINT size = GetSystemFirmwareTable(provider, 0, nullptr, 0);
    if (size < 8) {
        throw Error("Cannot read SMBIOS firmware table");
    }
    std::vector<std::uint8_t> buffer(size);
    if (GetSystemFirmwareTable(provider, 0, buffer.data(), size) != size) {
        throw Error("Cannot read complete SMBIOS firmware table");
    }

    const std::uint32_t table_size =
        static_cast<std::uint32_t>(buffer[4]) |
        (static_cast<std::uint32_t>(buffer[5]) << 8) |
        (static_cast<std::uint32_t>(buffer[6]) << 16) |
        (static_cast<std::uint32_t>(buffer[7]) << 24);
    if (table_size > buffer.size() - 8) {
        throw Error("SMBIOS firmware table has an invalid length");
    }

    const std::uint8_t* current = buffer.data() + 8;
    const std::uint8_t* const end = current + table_size;
    while (current + 4 <= end) {
        const std::uint8_t type = current[0];
        const std::size_t formatted_length = current[1];
        if (formatted_length < 4 || current + formatted_length > end) {
            throw Error("SMBIOS structure has an invalid length");
        }
        if (type == 1 && formatted_length >= 24) {
            std::array<std::uint8_t, 16> uuid{};
            for (std::size_t index = 0; index < uuid.size(); ++index) {
                uuid[index] = current[8 + index];
            }
            if (is_missing_uuid(uuid)) {
                throw Error("SMBIOS System UUID is not set");
            }
            return normalize_hardware_id(format_smbios_uuid(uuid));
        }

        current += formatted_length;
        while (current + 1 < end && (current[0] != 0 || current[1] != 0)) {
            ++current;
        }
        if (current + 1 >= end) {
            break;
        }
        current += 2;
        if (type == 127) {
            break;
        }
    }
    throw Error("SMBIOS System Information structure is unavailable");
}

}  // namespace satsuma
