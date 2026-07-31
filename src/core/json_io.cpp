// JSON 文件读取和原子写入实现。
#include "satsuma/core/json_io.hpp"

#include <array>
#include <chrono>
#include <string>
#include <string_view>
#include <thread>

#include <windows.h>

#include "satsuma/core/errors.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/path.hpp"

namespace satsuma {
namespace {

// 将 Win32 错误码转换为稳定的错误文本。
[[nodiscard]] std::string win32_error(const std::string& operation, const DWORD code) {
    return operation + " failed with Win32 error " + std::to_string(code);
}

constexpr std::chrono::seconds kAtomicReplaceTimeout{2}; // 短于 claim 安全余量和锁等待上限
constexpr std::chrono::milliseconds kAtomicReplaceRetryDelay{10}; // 替换重试间隔

// VMware Shared Folder 等 UNC 通道不提供可预测的写穿时延。
[[nodiscard]] bool is_remote_json_path(const std::filesystem::path& path) {
    const std::wstring& native = path.native();
    if (native.starts_with(L"\\\\?\\UNC\\")) {
        return true;
    }
    return native.starts_with(L"\\\\") && !native.starts_with(L"\\\\?\\");
}

// 判断目标文件的短暂占用是否允许复用同一临时文件重试。
[[nodiscard]] bool is_transient_replace_error(const DWORD error) noexcept {
    return error == ERROR_ACCESS_DENIED ||
        error == ERROR_SHARING_VIOLATION ||
        error == ERROR_LOCK_VIOLATION;
}

// 在有限时间内重试原子替换，兼容共享文件系统延迟释放读取 lease。
void replace_json_file(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination) {
    const auto deadline = std::chrono::steady_clock::now() + kAtomicReplaceTimeout;
    const DWORD move_flags = MOVEFILE_REPLACE_EXISTING |
        (is_remote_json_path(destination) ? 0 : MOVEFILE_WRITE_THROUGH);
    DWORD move_error = ERROR_SUCCESS; // 最后一次 MoveFileExW 错误
    for (;;) {
        if (MoveFileExW(
                temporary.c_str(),
                destination.c_str(),
                move_flags)) {
            return;
        }
        move_error = GetLastError();
        if (!is_transient_replace_error(move_error) ||
            std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(kAtomicReplaceRetryDelay);
    }

    DeleteFileW(temporary.c_str());
    throw Error(win32_error(
        "MoveFileExW for " + path_to_utf8(destination),
        move_error));
}

}  // namespace

nlohmann::json load_json(const std::filesystem::path& path) {
    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        throw JsonIoError("Cannot open JSON file: " + path_to_utf8(path));
    }

    std::string payload; // 完整读取后关闭句柄，再解析 JSON
    std::array<char, 64 * 1024> buffer{};
    DWORD read_error = ERROR_SUCCESS;
    for (;;) {
        DWORD bytes_read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read, nullptr)) {
            read_error = GetLastError();
            break;
        }
        if (bytes_read == 0) {
            break;
        }
        payload.append(buffer.data(), bytes_read);
    }
    CloseHandle(file);

    if (read_error != ERROR_SUCCESS) {
        throw JsonIoError(
            "Cannot read JSON file: " + path_to_utf8(path) +
            " (Win32 error " + std::to_string(read_error) + ")");
    }

    try {
        return nlohmann::json::parse(payload);
    } catch (const nlohmann::json::exception& error) {
        throw Error("Invalid JSON file " + path_to_utf8(path) + ": " + error.what());
    }
}

bool is_json_atomic_temporary_file(const std::filesystem::path& path) {
    constexpr std::wstring_view prefix = L".tmp-write-";
    const std::wstring filename = path.filename().native();
    return filename.size() > prefix.size() && filename.starts_with(prefix);
}

// 根据调用方策略准备父目录并执行同目录原子替换。
static void write_json_atomic_impl(
    const std::filesystem::path& path,
    const nlohmann::json& value,
    const bool create_parent) {
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        if (create_parent) {
            std::filesystem::create_directories(parent);
        } else if (!std::filesystem::is_directory(parent)) {
            throw Error("JSON parent directory does not exist: " + path_to_utf8(parent));
        }
    }

    // 每次写入使用独立临时文件，避免并发任务互相覆盖。
    const std::filesystem::path temporary =
        parent / path_from_utf8(".tmp-" + make_id("write"));
    const std::string payload = value.dump(2) + "\n";
    const bool remote_path = is_remote_json_path(path);
    const DWORD file_flags = FILE_ATTRIBUTE_NORMAL |
        (remote_path ? 0 : FILE_FLAG_WRITE_THROUGH);

    HANDLE file = CreateFileW(
        temporary.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        file_flags,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        throw Error(win32_error(
            "CreateFileW for " + path_to_utf8(temporary),
            GetLastError()));
    }

    DWORD bytes_written = 0;
    const BOOL write_ok = WriteFile(
        file,
        payload.data(),
        static_cast<DWORD>(payload.size()),
        &bytes_written,
        nullptr);
    const DWORD write_error = write_ok ? ERROR_SUCCESS : GetLastError();
    // 远端关闭句柄后直接原子发布，避免 HGFS 的 FlushFileBuffers 长时间阻塞 Agent。
    const BOOL flush_ok = write_ok && (remote_path || FlushFileBuffers(file));
    const DWORD flush_error = flush_ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);

    if (!write_ok || bytes_written != payload.size()) {
        DeleteFileW(temporary.c_str());
        throw Error(win32_error("WriteFile", write_error));
    }
    if (!flush_ok) {
        DeleteFileW(temporary.c_str());
        throw Error(win32_error("FlushFileBuffers", flush_error));
    }

    replace_json_file(temporary, path);
}

void write_json_atomic(const std::filesystem::path& path, const nlohmann::json& value) {
    write_json_atomic_impl(path, value, true);
}

void write_json_atomic_existing_parent(
    const std::filesystem::path& path,
    const nlohmann::json& value) {
    write_json_atomic_impl(path, value, false);
}

}  // namespace satsuma
