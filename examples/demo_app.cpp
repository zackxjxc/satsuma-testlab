// 独立示例软件：读取输入、转换内容并生成机器可读结果。
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace {

// 保存命令行解析后的输入、输出和诊断选项。
struct Options {
    std::filesystem::path input;       // 待读取的输入文件
    std::filesystem::path result;      // JSON 结果文件
    std::filesystem::path transformed; // 转换后的文本文件
    std::string label{"demo-app"};     // 本次执行标签
    bool emit_warning{false};          // 是否输出模拟诊断警告
};

// 把 Windows 宽字符串转换为 UTF-8。
[[nodiscard]] std::string to_utf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (size <= 0) {
        throw std::runtime_error("failed to convert command line text to UTF-8");
    }
    std::string output(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            output.data(),
            size,
            nullptr,
            nullptr) != size) {
        throw std::runtime_error("failed to convert command line text to UTF-8");
    }
    return output;
}

// 解析普通软件风格的命令行参数。
[[nodiscard]] Options parse_options(const int argc, wchar_t* argv[]) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::wstring argument = argv[index];
        // 读取当前参数后紧随的必需值
        const auto require_value = [&argc, &argv, &index, &argument] {
            if (index + 1 >= argc) {
                throw std::runtime_error("missing value for argument: " + to_utf8(argument));
            }
            return std::wstring(argv[++index]);
        };

        if (argument == L"--input") {
            options.input = require_value();
        } else if (argument == L"--result") {
            options.result = require_value();
        } else if (argument == L"--transformed") {
            options.transformed = require_value();
        } else if (argument == L"--label") {
            options.label = to_utf8(require_value());
        } else if (argument == L"--emit-warning") {
            options.emit_warning = true;
        } else {
            throw std::runtime_error("unknown argument: " + to_utf8(argument));
        }
    }
    if (options.input.empty() || options.result.empty() || options.transformed.empty()) {
        throw std::runtime_error("--input, --result and --transformed are required");
    }
    return options;
}

// 读取完整的二进制输入文件。
[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open input file: " + to_utf8(path.wstring()));
    }
    std::ostringstream content;
    content << input.rdbuf();
    if (!input.good() && !input.eof()) {
        throw std::runtime_error("failed to read input file: " + to_utf8(path.wstring()));
    }
    return content.str();
}

// 完整写入输出文件，并按需创建父目录。
void write_file(const std::filesystem::path& path, const std::string& content) {
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!output) {
        throw std::runtime_error("failed to write output file: " + to_utf8(path.wstring()));
    }
}

// 计算输入内容的 FNV-1a 64 位校验值。
[[nodiscard]] std::uint64_t fnv1a64(const std::string& content) {
    std::uint64_t hash = 14'695'981'039'346'656'037ULL;
    for (const unsigned char byte : content) {
        hash ^= byte;
        hash *= 1'099'511'628'211ULL;
    }
    return hash;
}

// 把校验值格式化为固定宽度的小写十六进制。
[[nodiscard]] std::string format_hash(const std::uint64_t value) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

// 只转换 ASCII 字母，保持 UTF-8 多字节内容不变。
[[nodiscard]] std::string uppercase_ascii(std::string content) {
    std::transform(content.begin(), content.end(), content.begin(), [](const unsigned char value) {
        if (value >= 'a' && value <= 'z') {
            return static_cast<char>(value - 'a' + 'A');
        }
        return static_cast<char>(value);
    });
    return content;
}

// 统计文本行数，兼容有无末尾换行的输入。
[[nodiscard]] std::size_t count_lines(const std::string& content) {
    if (content.empty()) {
        return 0;
    }
    const std::size_t newline_count = static_cast<std::size_t>(
        std::count(content.begin(), content.end(), '\n'));
    return content.back() == '\n' ? newline_count : newline_count + 1;
}

}  // namespace

// 执行独立示例软件并返回普通进程退出码。
int wmain(const int argc, wchar_t* argv[]) {
    try {
        const Options options = parse_options(argc, argv);         // 运行参数
        const std::string input = read_file(options.input);         // 原始输入
        const std::string transformed = uppercase_ascii(input);     // 转换结果
        const std::string checksum = format_hash(fnv1a64(input));   // 内容校验值
        const std::size_t line_count = count_lines(input);          // 输入行数

        write_file(options.transformed, transformed);
        const nlohmann::json result = {
            {"schema_version", 1},
            {"status", "success"},
            {"label", options.label},
            {"input_bytes", input.size()},
            {"input_lines", line_count},
            {"fnv1a64", checksum},
            {"transformed_file", to_utf8(options.transformed.wstring())},
        };
        write_file(options.result, result.dump(2) + "\n");

        std::cout << "demo-app processed " << input.size() << " bytes across "
                  << line_count << " lines; fnv1a64=" << checksum << '\n';
        if (options.emit_warning) {
            std::cerr << "demo-app simulated warning: diagnostic stderr is operational\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SatsumaDemoApp error: " << error.what() << '\n';
        return 2;
    }
}
