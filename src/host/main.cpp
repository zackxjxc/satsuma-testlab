// SatsumaHost 命令行入口。
#include <filesystem>
#include <iostream>
#include <map>
#include <string>

#include "controller.hpp"
#include "satsuma/core/config.hpp"
#include "satsuma/core/errors.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"

namespace {

// 解析 --name value 形式的严格命令行选项。
[[nodiscard]] std::map<std::wstring, std::wstring> parse_options(
    const int argc,
    wchar_t* argv[],
    const int start) {
    std::map<std::wstring, std::wstring> options;
    for (int index = start; index < argc; index += 2) {
        if (index + 1 >= argc || std::wstring(argv[index]).rfind(L"--", 0) != 0) {
            throw satsuma::Error("Options must use --name value pairs");
        }
        const std::wstring name = std::wstring(argv[index]).substr(2);
        if (!options.emplace(name, argv[index + 1]).second) {
            throw satsuma::Error("Duplicate command-line option");
        }
    }
    return options;
}

// 读取必需命令行选项。
[[nodiscard]] std::wstring require_option(
    const std::map<std::wstring, std::wstring>& options,
    const std::wstring& name) {
    const auto match = options.find(name);
    if (match == options.end() || match->second.empty()) {
        throw satsuma::Error("Missing required command-line option");
    }
    return match->second;
}

// 输出当前首个增量支持的 CLI 用法。
void print_usage() {
    std::cout
        << "SatsumaHost 0.1.0\n"
        << "Usage:\n"
        << "  SatsumaHost run --config lab.json --plan task.json\n"
        << "  SatsumaHost report --config lab.json --run <run-id>\n";
}

}  // namespace

// 运行 Host CLI 并把业务错误转换为稳定退出码。
int wmain(const int argc, wchar_t* argv[]) {
    try {
        if (argc < 2) {
            print_usage();
            return 2;
        }

        const std::wstring command = argv[1];
        const auto options = parse_options(argc, argv, 2);
        const std::filesystem::path config_path = require_option(options, L"config");
        satsuma::host::Controller controller(satsuma::load_lab_config(config_path));

        if (command == L"run") {
            const std::filesystem::path plan_path = require_option(options, L"plan");
            const satsuma::RunManifest manifest = controller.create_run(plan_path);
            nlohmann::json output = {
                {"status", "prepared"},
                {"run_id", manifest.run_id},
                {"request_id", manifest.request_id},
            };
            std::cout << output.dump(2) << '\n';
            return 0;
        }
        if (command == L"report") {
            const std::string run_id = satsuma::path_to_utf8(require_option(options, L"run"));
            std::cout << controller.build_report(run_id).dump(2) << '\n';
            return 0;
        }

        print_usage();
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "SatsumaHost error: " << error.what() << '\n';
        return 1;
    }
}
