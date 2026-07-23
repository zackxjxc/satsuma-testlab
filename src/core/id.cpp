// 运行标识和 UTC 时间生成实现。
#include "satsuma/core/id.hpp"

#include <array>
#include <chrono>
#include <iomanip>
#include <sstream>

#include <windows.h>
#include <bcrypt.h>

#include "satsuma/core/errors.hpp"

namespace satsuma {
namespace {

// 将当前 UTC 时间转换为线程安全的 tm。
[[nodiscard]] std::tm current_utc_tm() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t value = std::chrono::system_clock::to_time_t(now);
    std::tm result{};
    if (gmtime_s(&result, &value) != 0) {
        throw Error("Failed to convert the current time to UTC");
    }
    return result;
}

// 生成用于降低标识碰撞概率的随机十六进制后缀。
[[nodiscard]] std::string random_hex() {
    std::array<unsigned char, 8> bytes{};
    const NTSTATUS status = BCryptGenRandom(
        nullptr,
        bytes.data(),
        static_cast<ULONG>(bytes.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) {
        throw Error("BCryptGenRandom failed: " + std::to_string(status));
    }

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const unsigned char byte : bytes) {
        output << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return output.str();
}

}  // namespace

std::string utc_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    const std::tm utc = current_utc_tm();
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S")
           << '.' << std::setw(3) << std::setfill('0') << milliseconds.count() << 'Z';
    return output.str();
}

std::string utc_timestamp_compact() {
    const std::tm utc = current_utc_tm();
    std::ostringstream output;
    output << std::put_time(&utc, "%Y%m%d%H%M%S");
    return output.str();
}

std::string make_id(const std::string_view prefix) {
    if (prefix.empty()) {
        throw Error("ID prefix must not be empty");
    }
    return std::string(prefix) + '-' + utc_timestamp_compact() + '-' + random_hex();
}

}  // namespace satsuma
