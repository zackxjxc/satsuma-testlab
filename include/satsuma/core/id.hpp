// 运行标识和 UTC 时间生成接口。
#pragma once

#include <string>
#include <string_view>

namespace satsuma {

// 生成带前缀、时间和随机后缀的唯一标识。
[[nodiscard]] std::string make_id(std::string_view prefix);

// 返回便于 JSON 记录的 ISO 8601 UTC 时间。
[[nodiscard]] std::string utc_timestamp();

// 返回适合目录名称的紧凑 UTC 时间。
[[nodiscard]] std::string utc_timestamp_compact();

}  // namespace satsuma
