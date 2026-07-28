// UTF-8 路径转换和共享根目录边界校验实现。
#include "satsuma/core/path.hpp"

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <thread>
#include <vector>

#include <windows.h>

#include "satsuma/core/errors.hpp"

namespace satsuma {
namespace {

constexpr std::chrono::seconds kPathRenameTimeout{2}; // 共享层瞬时占用最长等待时间
constexpr std::chrono::milliseconds kPathRenameRetryDelay{10}; // 路径改名重试间隔

// 判断路径改名错误是否来自可恢复的 Windows 瞬时占用。
[[nodiscard]] bool is_transient_rename_error(const DWORD error) noexcept {
    return error == ERROR_ACCESS_DENIED ||
        error == ERROR_SHARING_VIOLATION ||
        error == ERROR_LOCK_VIOLATION;
}

// 比较 Windows 路径组件时忽略大小写。
[[nodiscard]] bool component_equal(const std::filesystem::path& left, const std::filesystem::path& right) {
    const std::wstring left_value = left.native();
    const std::wstring right_value = right.native();
    return _wcsicmp(left_value.c_str(), right_value.c_str()) == 0;
}

// 验证组合后的绝对路径仍以可信根目录开头。
void ensure_under_root(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    auto root_part = root.begin();
    auto candidate_part = candidate.begin();
    for (; root_part != root.end(); ++root_part, ++candidate_part) {
        if (candidate_part == candidate.end() || !component_equal(*root_part, *candidate_part)) {
            throw Error("Resolved path escapes the configured root");
        }
    }
}

}  // namespace

std::filesystem::path path_from_utf8(const std::string_view value) {
    if (value.empty()) {
        return {};
    }

    const int wide_size = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (wide_size <= 0) {
        throw Error("Invalid UTF-8 path");
    }

    std::wstring wide(static_cast<std::size_t>(wide_size), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            wide.data(),
            wide_size) != wide_size) {
        throw Error("Failed to convert a UTF-8 path");
    }
    return std::filesystem::path(wide);
}

std::string path_to_utf8(const std::filesystem::path& value) {
    const std::wstring& wide = value.native();
    if (wide.empty()) {
        return {};
    }

    const int utf8_size = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        wide.data(),
        static_cast<int>(wide.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (utf8_size <= 0) {
        throw Error("Failed to measure a UTF-8 path");
    }

    std::string utf8(static_cast<std::size_t>(utf8_size), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            wide.data(),
            static_cast<int>(wide.size()),
            utf8.data(),
            utf8_size,
            nullptr,
            nullptr) != utf8_size) {
        throw Error("Failed to convert a path to UTF-8");
    }
    return utf8;
}

void rename_path_with_retry(
    const std::filesystem::path& source,
    const std::filesystem::path& destination) {
    const auto deadline = std::chrono::steady_clock::now() + kPathRenameTimeout;
    DWORD rename_error = ERROR_SUCCESS; // 最后一次 MoveFileExW 错误
    for (;;) {
        if (MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
            return;
        }
        rename_error = GetLastError();
        if (!is_transient_rename_error(rename_error) ||
            std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(kPathRenameRetryDelay);
    }
    throw Error(
        "Cannot rename path to " + path_to_utf8(destination) +
        ": Win32 error " + std::to_string(rename_error));
}

void validate_relative_path(const std::filesystem::path& relative) {
    if (relative.empty() || relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) {
        throw Error("Task path must be a non-empty relative path: " + path_to_utf8(relative));
    }

    for (const auto& component : relative) {
        if (component == L".." || component == L".") {
            throw Error("Task path contains a forbidden component: " + path_to_utf8(relative));
        }
    }
}

std::filesystem::path resolve_under_root(
    const std::filesystem::path& root,
    const std::filesystem::path& relative) {
    validate_relative_path(relative);
    const std::filesystem::path absolute_root = std::filesystem::absolute(root).lexically_normal();
    const std::filesystem::path candidate = (absolute_root / relative).lexically_normal();
    ensure_under_root(absolute_root, candidate);

    // 拒绝运行目录下已有的重解析点，避免 junction 或符号链接绕过根目录。
    std::filesystem::path current = absolute_root;
    for (const auto& component : relative) {
        current /= component;
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            break;
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            throw Error("Task path crosses a reparse point: " + path_to_utf8(relative));
        }
    }
    return candidate;
}

}  // namespace satsuma
