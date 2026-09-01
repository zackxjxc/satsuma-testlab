// 使用 Draft 2020-12 验证器检查公开 JSON 示例与 Schema 的真实一致性。
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>

namespace {

[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot open JSON document: " + path.string());
    }
    std::ostringstream content;
    content << stream.rdbuf();
    return content.str();
}

[[nodiscard]] jsoncons::json load_json(const std::filesystem::path& path) {
    return jsoncons::json::parse(read_text(path));
}

}  // namespace

int main(const int argc, char* argv[]) {
    try {
        if (argc < 3) {
            throw std::runtime_error("Expected a schema path and at least one instance path");
        }

        const std::filesystem::path schema_path = argv[1];
        const auto schema = jsoncons::jsonschema::make_json_schema(
            load_json(schema_path),
            jsoncons::jsonschema::evaluation_options{}.require_format_validation(true));

        for (int index = 2; index < argc; ++index) {
            const std::filesystem::path instance_path = argv[index];
            jsoncons::json instance = load_json(instance_path);
            if (!schema.is_valid(instance)) {
                throw std::runtime_error(
                    "Instance does not satisfy schema " + schema_path.string() + ": " +
                    instance_path.string());
            }

            instance["unexpected_schema_contract_field"] = true;
            if (schema.is_valid(instance)) {
                throw std::runtime_error(
                    "Schema accepted an unknown top-level field: " + schema_path.string());
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Schema contract test failed: " << error.what() << '\n';
        return 1;
    }
}
