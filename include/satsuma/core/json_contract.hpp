// JSON 交换契约的公共字段校验接口。
#pragma once

#include <initializer_list>
#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

namespace satsuma {

// 拒绝对象中未列入契约的字段，避免拼写错误被静默忽略。
void reject_unknown_fields(
    const nlohmann::json& value,
    std::initializer_list<std::string_view> allowed_fields,
    std::string_view context);

// 读取非空必需字符串，并为不同契约保留稳定的错误上下文。
[[nodiscard]] std::string required_non_empty_string(
    const nlohmann::json& value,
    const char* field,
    std::string_view empty_field_context);

}  // namespace satsuma
