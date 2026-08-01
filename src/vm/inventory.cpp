// Guest 环境清单的 Windows 原生采集和缓存发布实现。
#include "inventory.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <windows.h>
#include <winternl.h>

#include "satsuma/core/errors.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/sha256.hpp"

namespace satsuma::vm {
namespace {

// 兼容尚未完成硬件绑定的旧配置，使用当前 VM ID 作为文件键。
[[nodiscard]] std::string inventory_key(const AgentConfig& config) {
    return config.hardware_id.empty() ? config.vm_id : config.hardware_id;
}

// 返回当前硬件身份对应的清单文件。
[[nodiscard]] std::filesystem::path inventory_path(const AgentConfig& config) {
    return config.shared_root / L"agents" / path_from_utf8(inventory_key(config) + ".inventory.json");
}

// 返回 Host 显式刷新请求文件。
[[nodiscard]] std::filesystem::path refresh_path(const AgentConfig& config) {
    return config.shared_root / L"agents" /
        path_from_utf8(inventory_key(config) + ".inventory-refresh.json");
}

// 从注册表读取 Windows 产品名称。
[[nodiscard]] std::string windows_product_name() {
    std::array<wchar_t, 256> value{};
    DWORD bytes = static_cast<DWORD>(value.size() * sizeof(wchar_t));
    const LSTATUS status = RegGetValueW(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
        L"ProductName",
        RRF_RT_REG_SZ,
        nullptr,
        value.data(),
        &bytes);
    if (status != ERROR_SUCCESS) {
        throw Error("Cannot read Windows product name (Win32 error " + std::to_string(status) + ")");
    }
    return path_to_utf8(std::filesystem::path(value.data()));
}

// 使用系统 ntdll 的版本查询避免兼容性清单虚报旧版本。
[[nodiscard]] RTL_OSVERSIONINFOW windows_version() {
    using RtlGetVersionFunction = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    const HMODULE module = GetModuleHandleW(L"ntdll.dll");
    const auto function = module == nullptr ? nullptr : reinterpret_cast<RtlGetVersionFunction>(
        GetProcAddress(module, "RtlGetVersion"));
    if (function == nullptr) {
        throw Error("Cannot resolve RtlGetVersion");
    }
    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (function(&version) != 0) {
        throw Error("RtlGetVersion failed");
    }
    return version;
}

// 把 IMAGE_FILE_MACHINE 常量转换为协议架构名。
[[nodiscard]] std::string architecture_name(const USHORT machine) {
    switch (machine) {
    case IMAGE_FILE_MACHINE_AMD64:
        return "x64";
    case IMAGE_FILE_MACHINE_I386:
        return "x86";
    case IMAGE_FILE_MACHINE_ARM64:
        return "arm64";
    default:
        return "unknown";
    }
}

// 返回进程与原生系统架构。
[[nodiscard]] std::pair<std::string, std::string> process_architectures() {
    USHORT process_machine = IMAGE_FILE_MACHINE_UNKNOWN;
    USHORT native_machine = IMAGE_FILE_MACHINE_UNKNOWN;
    if (!IsWow64Process2(GetCurrentProcess(), &process_machine, &native_machine)) {
        throw Error("IsWow64Process2 failed with Win32 error " + std::to_string(GetLastError()));
    }
    const USHORT effective_process = process_machine == IMAGE_FILE_MACHINE_UNKNOWN
        ? native_machine
        : process_machine;
    return {architecture_name(effective_process), architecture_name(native_machine)};
}

// 读取 PE 文件版本，作为固定解释器能力版本。
[[nodiscard]] std::string file_version(const std::filesystem::path& path) {
    DWORD unused = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &unused);
    if (size == 0) {
        return "unknown";
    }
    std::vector<std::byte> buffer(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, buffer.data())) {
        return "unknown";
    }
    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<void**>(&info), &length) ||
        info == nullptr || length < sizeof(VS_FIXEDFILEINFO)) {
        return "unknown";
    }
    return std::to_string(HIWORD(info->dwFileVersionMS)) + "." +
        std::to_string(LOWORD(info->dwFileVersionMS)) + "." +
        std::to_string(HIWORD(info->dwFileVersionLS)) + "." +
        std::to_string(LOWORD(info->dwFileVersionLS));
}

// 构造一个不依赖 PATH 的解释器能力条目。
[[nodiscard]] nlohmann::json engine_entry(
    const std::string& engine,
    const std::filesystem::path& path,
    const std::string& edition) {
    const bool available = std::filesystem::is_regular_file(path);
    return {
        {"engine", engine},
        {"available", available},
        {"version", available ? file_version(path) : ""},
        {"edition", edition},
        {"path", path_to_utf8(path)},
    };
}

// 采集全部本地固定盘容量。
[[nodiscard]] nlohmann::json fixed_drives() {
    const DWORD required = GetLogicalDriveStringsW(0, nullptr);
    if (required == 0) {
        throw Error("GetLogicalDriveStringsW failed with Win32 error " + std::to_string(GetLastError()));
    }
    std::vector<wchar_t> buffer(required + 1);
    if (GetLogicalDriveStringsW(static_cast<DWORD>(buffer.size()), buffer.data()) == 0) {
        throw Error("GetLogicalDriveStringsW failed with Win32 error " + std::to_string(GetLastError()));
    }

    nlohmann::json drives = nlohmann::json::array();
    for (const wchar_t* root = buffer.data(); *root != L'\0'; root += std::wcslen(root) + 1) {
        if (GetDriveTypeW(root) != DRIVE_FIXED) {
            continue;
        }
        ULARGE_INTEGER available{};
        ULARGE_INTEGER total{};
        if (!GetDiskFreeSpaceExW(root, &available, &total, nullptr)) {
            continue;
        }
        std::array<wchar_t, 64> file_system{};
        if (!GetVolumeInformationW(
                root,
                nullptr,
                0,
                nullptr,
                nullptr,
                nullptr,
                file_system.data(),
                static_cast<DWORD>(file_system.size()))) {
            file_system[0] = L'\0';
        }
        drives.push_back({
            {"root", path_to_utf8(std::filesystem::path(root))},
            {"type", "fixed"},
            {"file_system", path_to_utf8(std::filesystem::path(file_system.data()))},
            {"total_bytes", total.QuadPart},
            {"available_bytes", available.QuadPart},
        });
    }
    return drives;
}

}  // namespace

InventoryPublisher::InventoryPublisher(const AgentConfig& config, std::string boot_id)
    : config_(config), boot_id_(std::move(boot_id)) {
    try {
        const std::filesystem::path path = inventory_path(config_);
        const nlohmann::json existing = load_json(path);
        if (existing.value("schema_version", 0) != 1 ||
            existing.value("lab_id", std::string{}) != config_.lab_id ||
            existing.value("vm_id", std::string{}) != config_.vm_id ||
            existing.value("hardware_id", std::string{}) != inventory_key(config_) ||
            existing.value("observed_at", std::string{}).empty() ||
            !existing.contains("script_engines") || !existing.at("script_engines").is_array() ||
            !existing.contains("drives") || !existing.at("drives").is_array()) {
            return;
        }
        digest_ = sha256_file(path);
        observed_at_ = existing.at("observed_at").get<std::string>();
        handled_request_id_ = existing.value("refresh_request_id", std::string{});
        cache_ = existing;
    } catch (...) {
        cache_.reset();
        digest_.clear();
        observed_at_.clear();
        handled_request_id_.clear();
    }
}

void InventoryPublisher::synchronize() {
    std::string request_id;
    const std::filesystem::path request_file = refresh_path(config_);
    if (std::filesystem::is_regular_file(request_file)) {
        const nlohmann::json request = load_json(request_file);
        if (request.value("schema_version", 0) != 1 ||
            request.value("lab_id", std::string{}) != config_.lab_id ||
            request.value("vm_id", std::string{}) != config_.vm_id ||
            request.value("hardware_id", std::string{}) != inventory_key(config_)) {
            throw Error("Inventory refresh request identity mismatch");
        }
        request_id = request.value("request_id", std::string{});
        if (request_id.empty()) {
            throw Error("Inventory refresh request omitted request_id");
        }
    }

    if (!cache_.has_value() || (!request_id.empty() && request_id != handled_request_id_)) {
        const nlohmann::json next = collect(request_id);
        publish(next);
        cache_ = next;
        handled_request_id_ = request_id;
        return;
    }

    const std::filesystem::path path = inventory_path(config_);
    bool valid = false;
    try {
        valid = std::filesystem::is_regular_file(path) && sha256_file(path) == digest_;
    } catch (...) {
        valid = false;
    }
    if (!valid) {
        publish(*cache_);
    }
}

const std::string& InventoryPublisher::digest() const noexcept {
    return digest_;
}

const std::string& InventoryPublisher::observed_at() const noexcept {
    return observed_at_;
}

std::filesystem::path InventoryPublisher::script_engine_path(const std::string_view engine) const {
    if (!cache_.has_value()) {
        throw Error("Agent inventory has not been published in this session");
    }
    for (const auto& capability : cache_->at("script_engines")) {
        if (capability.value("engine", std::string{}) == engine) {
            if (!capability.value("available", false)) {
                throw Error("Script engine is unavailable: " + std::string(engine));
            }
            const std::filesystem::path path = path_from_utf8(
                capability.value("path", std::string{}));
            if (!std::filesystem::is_regular_file(path)) {
                throw Error("Script engine path is no longer available: " + path_to_utf8(path));
            }
            return path;
        }
    }
    throw Error("Agent inventory omitted script engine: " + std::string(engine));
}

nlohmann::json InventoryPublisher::collect(const std::string& request_id) const {
    const RTL_OSVERSIONINFOW version = windows_version();
    const auto [process_architecture, native_architecture] = process_architectures();
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (!GlobalMemoryStatusEx(&memory)) {
        throw Error("GlobalMemoryStatusEx failed with Win32 error " + std::to_string(GetLastError()));
    }

    wchar_t windows_directory[MAX_PATH]{};
    if (GetWindowsDirectoryW(windows_directory, MAX_PATH) == 0) {
        throw Error("GetWindowsDirectoryW failed with Win32 error " + std::to_string(GetLastError()));
    }
    const std::filesystem::path powershell = std::filesystem::path(windows_directory) /
        L"System32" / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe";
    const std::filesystem::path pwsh = std::filesystem::path(L"C:\\Program Files\\PowerShell\\7\\pwsh.exe");
    const std::string observed = utc_timestamp();
    nlohmann::json inventory = {
        {"schema_version", 1},
        {"lab_id", config_.lab_id},
        {"vm_id", config_.vm_id},
        {"hardware_id", inventory_key(config_)},
        {"boot_id", boot_id_},
        {"observed_at", observed},
        {"os", {
            {"product_name", windows_product_name()},
            {"version", std::to_string(version.dwMajorVersion) + "." +
                std::to_string(version.dwMinorVersion) + "." + std::to_string(version.dwBuildNumber)},
            {"build", version.dwBuildNumber},
            {"native_architecture", native_architecture},
            {"process_architecture", process_architecture},
        }},
        {"script_engines", nlohmann::json::array({
            engine_entry("windows_powershell", powershell, "Desktop"),
            engine_entry("pwsh", pwsh, "Core"),
            engine_entry("cmd", std::filesystem::path(windows_directory) / L"System32" / L"cmd.exe", "Windows"),
        })},
        {"drives", fixed_drives()},
        {"memory", {
            {"total_bytes", memory.ullTotalPhys},
            {"available_bytes", memory.ullAvailPhys},
        }},
    };
    if (!request_id.empty()) {
        inventory["refresh_request_id"] = request_id;
    }
    return inventory;
}

void InventoryPublisher::publish(const nlohmann::json& inventory) {
    const std::filesystem::path path = inventory_path(config_);
    write_json_atomic(path, inventory);
    digest_ = sha256_file(path);
    observed_at_ = inventory.at("observed_at").get<std::string>();
}

}  // namespace satsuma::vm
