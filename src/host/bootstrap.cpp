// Host 首次配置生成：本机发现、发行包校验和显式 VM 范围登记。
#include "bootstrap.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <string_view>

#include <windows.h>

#include "satsuma/core/config.hpp"
#include "satsuma/core/errors.hpp"
#include "satsuma/core/hardware_identity.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/sha256.hpp"
#include "satsuma/core/version.hpp"
#include "vmrun_provider.hpp"

namespace satsuma::host {
namespace {

std::wstring environment_value(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0) {
        return {};
    }
    std::vector<wchar_t> buffer(size);
    const DWORD copied = GetEnvironmentVariableW(name, buffer.data(), size);
    return copied > 0 && copied < size ? std::wstring(buffer.data(), copied) : L"";
}

// 优先使用安装注册信息，再检查标准安装目录；不执行当前目录中的同名程序。
std::filesystem::path locate_vmrun() {
    for (const DWORD view : {RRF_SUBKEY_WOW6464KEY, RRF_SUBKEY_WOW6432KEY}) {
        std::array<wchar_t, 32768> buffer{};
        DWORD bytes = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
        if (RegGetValueW(
                HKEY_LOCAL_MACHINE, L"SOFTWARE\\VMware, Inc.\\VMware Workstation",
                L"InstallPath", RRF_RT_REG_SZ | view, nullptr, buffer.data(), &bytes) ==
            ERROR_SUCCESS) {
            const auto candidate = std::filesystem::path(buffer.data()) / L"vmrun.exe";
            if (std::filesystem::is_regular_file(candidate)) {
                return candidate;
            }
        }
    }
    for (const wchar_t* variable : {L"ProgramFiles(x86)", L"ProgramFiles"}) {
        const std::wstring directory = environment_value(variable);
        if (directory.empty()) {
            continue;
        }
        const auto candidate = std::filesystem::path(directory) /
            L"VMware" / L"VMware Workstation" / L"vmrun.exe";
        if (std::filesystem::is_regular_file(candidate)) {
            return candidate;
        }
    }
    throw Error("Cannot locate VMware vmrun.exe; supply --vmrun <absolute-path>");
}

// 目录仅在恰好含一个 VMX 时可简写；不扫描或选中用户未指定的其他 VM。
std::filesystem::path resolve_vmx(const std::wstring& supplied) {
    std::filesystem::path path = std::filesystem::absolute(supplied).lexically_normal();
    if (std::filesystem::is_directory(path)) {
        std::vector<std::filesystem::path> candidates;
        for (const auto& entry : std::filesystem::directory_iterator(path)) {
            if (entry.is_regular_file() &&
                _wcsicmp(entry.path().extension().c_str(), L".vmx") == 0) {
                candidates.push_back(entry.path());
            }
        }
        if (candidates.size() != 1) {
            throw Error("VM directory must contain exactly one VMX: " + path_to_utf8(path));
        }
        path = candidates.front();
    }
    if (!std::filesystem::is_regular_file(path) ||
        _wcsicmp(path.extension().c_str(), L".vmx") != 0) {
        throw Error("Not a VMX file: " + path_to_utf8(path));
    }
    return std::filesystem::canonical(path);
}

// VMX uuid.bios 保存原始 SMBIOS 字节；按 Agent 使用的 SMBIOS 2.6+ 顺序转换。
std::string vmx_hardware_id(const std::filesystem::path& path) {
    if (std::filesystem::file_size(path) > 1024 * 1024) {
        throw Error("VMX is too large: " + path_to_utf8(path));
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw Error("Cannot read VMX: " + path_to_utf8(path));
    }
    const std::regex uuid_line(R"vmx(^\s*uuid\.bios\s*=\s*"([^"]*)"\s*(#.*)?$)vmx",
                               std::regex::icase);
    const std::regex reflected(R"vmx(^\s*smbios\.reflectHost\s*=\s*"TRUE"\s*(#.*)?$)vmx",
                               std::regex::icase);
    const std::regex raw_uuid("^[0-9a-fA-F]{2}([ -][0-9a-fA-F]{2}){15}$");
    std::string compact;
    std::string line;
    while (std::getline(input, line)) {
        if (line.starts_with("\xef\xbb\xbf")) {
            line.erase(0, 3);
        }
        if (std::regex_match(line, reflected)) {
            throw Error("Cannot auto-bind a VM reflecting the Host SMBIOS: " + path_to_utf8(path));
        }
        std::smatch match;
        if (!std::regex_match(line, match, uuid_line)) {
            continue;
        }
        const std::string raw = match[1].str();
        if (!compact.empty() || !std::regex_match(raw, raw_uuid)) {
            throw Error("Invalid or duplicate uuid.bios in VMX: " + path_to_utf8(path));
        }
        for (const char character : raw) {
            if (character != ' ' && character != '-') {
                compact.push_back(character);
            }
        }
    }
    if (compact.empty()) {
        throw Error("VMX requires uuid.bios for automatic binding: " + path_to_utf8(path));
    }
    constexpr std::array<std::size_t, 16> order{
        3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15,
    };
    std::string uuid;
    for (std::size_t index = 0; index < order.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            uuid.push_back('-');
        }
        uuid.append(compact, order[index] * 2, 2);
    }
    return normalize_hardware_id(uuid);
}

// 读取 PE 版本资源，不启动交付的 Agent 程序。
std::string agent_binary_version(const std::filesystem::path& binary) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(binary.c_str(), &ignored);
    if (size == 0) {
        throw Error("Agent binary has no Windows version resource: " + path_to_utf8(binary));
    }
    std::vector<std::byte> buffer(size);
    if (!GetFileVersionInfoW(binary.c_str(), 0, size, buffer.data())) {
        throw Error("Cannot read Agent binary version resource");
    }
    VS_FIXEDFILEINFO* version = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<void**>(&version), &length) ||
        length < sizeof(VS_FIXEDFILEINFO) || version->dwSignature != 0xfeef04bd) {
        throw Error("Invalid Agent binary version resource");
    }
    const std::string result = std::to_string(HIWORD(version->dwFileVersionMS)) + "." +
        std::to_string(LOWORD(version->dwFileVersionMS)) + "." +
        std::to_string(HIWORD(version->dwFileVersionLS));
    if (result != kVersion) {
        throw Error("Agent binary version must match this Host: " + result);
    }
    // 所有 Satsuma 发行资源均使用 0409/1200；拒绝误选同版本的 Host 二进制。
    wchar_t* original_name = nullptr;
    if (!VerQueryValueW(
            buffer.data(), L"\\StringFileInfo\\040904b0\\OriginalFilename",
            reinterpret_cast<void**>(&original_name), &length) ||
        length == 0 || std::wstring_view(original_name) != L"SatsumaVM.exe") {
        throw Error("--agent-binary must be a SatsumaVM release binary");
    }
    return result;
}

}  // namespace

nlohmann::json initialize_lab(
    const std::vector<std::wstring>& arguments,
    const std::filesystem::path& executable_directory) {
    std::map<std::wstring, std::wstring> options;
    std::vector<std::wstring> vm_paths;
    const std::set<std::wstring> allowed{
        L"--config", L"--lab-id", L"--vmx", L"--base-snapshot", L"--vmrun",
        L"--vmci-port", L"--agent-binary",
    };
    for (std::size_t index = 0; index < arguments.size(); index += 2) {
        const std::wstring& name = arguments[index];
        if (!allowed.contains(name) || index + 1 == arguments.size() ||
            arguments[index + 1].empty() || arguments[index + 1].starts_with(L"--")) {
            throw Error("init requires known --name value pairs; only --vmx may repeat");
        }
        if (name == L"--vmx") {
            vm_paths.push_back(arguments[index + 1]);
        } else if (!options.emplace(name, arguments[index + 1]).second) {
            throw Error("Duplicate init option: " + path_to_utf8(name));
        }
    }
    for (const wchar_t* required : {L"--config", L"--base-snapshot"}) {
        if (!options.contains(required)) {
            throw Error("Missing init option: " + path_to_utf8(required));
        }
    }
    if (vm_paths.empty()) {
        throw Error("init requires at least one --vmx <file-or-directory>");
    }
    const auto output = std::filesystem::absolute(options.at(L"--config")).lexically_normal();
    if (std::filesystem::exists(output)) {
        throw Error("init will not overwrite an existing configuration: " + path_to_utf8(output));
    }
    const std::string lab_id = options.contains(L"--lab-id")
        ? path_to_utf8(options.at(L"--lab-id")) : make_id("lab");
    validate_identifier(lab_id, "lab_id");
    std::uint32_t port = kDefaultVmciPort;
    if (options.contains(L"--vmci-port")) {
        const std::string value = path_to_utf8(options.at(L"--vmci-port"));
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), port);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
            port == 0 || port == std::numeric_limits<std::uint32_t>::max()) {
            throw Error("--vmci-port must be between 1 and 4294967294");
        }
    }
    const auto binary = std::filesystem::absolute(options.contains(L"--agent-binary")
        ? std::filesystem::path(options.at(L"--agent-binary"))
        : executable_directory / L"SatsumaVM.exe");
    const std::string version = agent_binary_version(binary);
    const std::string digest = sha256_file(binary);
    const auto vmrun = std::filesystem::absolute(options.contains(L"--vmrun")
        ? std::filesystem::path(options.at(L"--vmrun")) : locate_vmrun());
    const auto state = output.parent_path() / (output.stem().native() + L".state");
    const auto archive = output.parent_path() / (output.stem().native() + L".archive");
    for (const auto& directory : {state, archive}) {
        if (std::filesystem::exists(directory) &&
            (!std::filesystem::is_directory(directory) || !std::filesystem::is_empty(directory))) {
            throw Error("init requires a fresh state/archive location: " + path_to_utf8(directory));
        }
    }
    nlohmann::json value = {
        {"schema_version", 1}, {"lab_id", lab_id},
        {"provider", {{"type", "vmware_workstation"}, {"vmrun", path_to_utf8(vmrun)}}},
        {"host", {{"archive_root", path_to_utf8(archive)}}},
        {"transport", {{"state_root", path_to_utf8(state)}, {"vmci_port", port}}},
        {"vms", nlohmann::json::array()},
    };
    std::set<std::string> hardware_ids;
    const std::string base = path_to_utf8(options.at(L"--base-snapshot"));
    for (const auto& supplied : vm_paths) {
        const auto vmx = resolve_vmx(supplied);
        const std::string hardware_id = vmx_hardware_id(vmx);
        if (!hardware_ids.insert(hardware_id).second) {
            throw Error("Duplicate VM or SMBIOS UUID in selected VMX files: " + hardware_id);
        }
        // ID 取自硬件，不随参数顺序改变，也不包含 gateway/client 等业务角色。
        value["vms"].push_back({
            {"id", "vm-" + hardware_id}, {"hardware_id", hardware_id},
            {"vmx", path_to_utf8(vmx)}, {"agent_version", version}, {"agent_sha256", digest},
            {"snapshots", {{"base", base}, {"ai_prefix", "satsuma-ai-"}, {"max_ai_snapshots", 8}}},
        });
    }

    // 先使用正式加载器校验暂存配置；最终发布不得替换并发创建的同名配置。
    const auto temporary = output.parent_path() / path_from_utf8(".tmp-" + make_id("init"));
    try {
        write_json_atomic(temporary, value);
        const LabConfig config = load_lab_config(temporary);
        const vmware::VmrunProvider provider(config.provider.vmrun);
        for (const auto& vm : config.vms) {
            const auto snapshots = provider.list_snapshots(vm.vmx);
            if (std::find(snapshots.begin(), snapshots.end(), base) == snapshots.end()) {
                throw Error("Base snapshot not found for " + path_to_utf8(vm.vmx) + ": " + base);
            }
        }
        if (!MoveFileExW(temporary.c_str(), output.c_str(), MOVEFILE_WRITE_THROUGH)) {
            throw Error("Cannot publish new configuration; Win32 error " +
                        std::to_string(GetLastError()));
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
    return {
        {"status", "initialized"}, {"config", path_to_utf8(output)},
        {"gateway_started", false}, {"readiness_verified", false},
        {"agent_bootstrap", {{"lab_id", lab_id}, {"host_cid", 2}, {"vmci_port", port}}},
        {"vms", value.at("vms")},
    };
}

}  // namespace satsuma::host
