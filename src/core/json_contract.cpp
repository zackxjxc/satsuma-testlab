// JSON 交换契约的公共字段校验实现。
#include "satsuma/core/json_contract.hpp"

#include <algorithm>

#include <nlohmann/json.hpp>

#include "satsuma/core/errors.hpp"

namespace satsuma {

void reject_unknown_fields(
    const nlohmann::json& value,
    const std::initializer_list<std::string_view> allowed_fields,
    const std::string_view context) {
    if (!value.is_object()) {
        throw Error(std::string(context) + " must be an object");
    }
    for (auto field = value.cbegin(); field != value.cend(); ++field) {
        const std::string_view name = field.key();
        if (std::find(allowed_fields.begin(), allowed_fields.end(), name) ==
            allowed_fields.end()) {
            throw Error("Unknown field in " + std::string(context) + ": " + std::string(name));
        }
    }
}

std::string required_non_empty_string(
    const nlohmann::json& value,
    const char* field,
    const std::string_view empty_field_context) {
    if (!value.contains(field) || !value.at(field).is_string()) {
        throw Error(std::string("Missing or invalid string field: ") + field);
    }
    const std::string result = value.at(field).get<std::string>();
    if (result.empty()) {
        throw Error(std::string(empty_field_context) + " must not be empty: " + field);
    }
    return result;
}

}  // namespace satsuma
