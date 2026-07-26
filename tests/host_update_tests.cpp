// Host Agent 更新发布、等待和共享清理测试。
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "controller.hpp"
#include "satsuma/core/config.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/sha256.hpp"
#include "satsuma/core/update.hpp"

namespace {

// 在条件不满足时终止当前测试。
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// 创建仅包含更新发布所需字段的实验室配置。
[[nodiscard]] satsuma::LabConfig make_config(const std::filesystem::path& root) {
    satsuma::LabConfig config;
    config.lab_id = "test_lab";
    config.shared_folder.host_root = root / L"share";
    satsuma::VmConfig vm;
    vm.id = "client";
    config.vms.push_back(std::move(vm));
    return config;
}

// 返回清单对应的共享更新目录。
[[nodiscard]] std::filesystem::path update_directory(
    const std::filesystem::path& share,
    const satsuma::AgentUpdateManifest& manifest) {
    return share / L"updates" /
        satsuma::path_from_utf8(manifest.vm_id) /
        satsuma::path_from_utf8(manifest.update_id);
}

// 验证发布原子目录、失败证据保留和成功目录清理。
void test_host_update_flow(const std::filesystem::path& root) {
    const satsuma::LabConfig config = make_config(root);
    std::filesystem::create_directories(config.shared_folder.host_root);
    const std::filesystem::path candidate = root / L"candidate.exe";
    std::ofstream(candidate, std::ios::binary) << "host-update-candidate";
    const satsuma::host::Controller controller(config);

    const satsuma::AgentUpdateManifest failed_manifest =
        controller.publish_agent_update("client", candidate, "0.1.1");
    const std::filesystem::path failed_directory =
        update_directory(config.shared_folder.host_root, failed_manifest);
    expect(std::filesystem::is_regular_file(failed_directory / L"update.json"),
        "Host did not publish update.json");
    expect(std::filesystem::is_regular_file(failed_directory / L"SatsumaVM.exe"),
        "Host did not publish the candidate");
    expect(failed_manifest.size == std::filesystem::file_size(candidate),
        "Host published the wrong candidate size");
    expect(failed_manifest.sha256 == satsuma::sha256_file(candidate),
        "Host published the wrong candidate hash");

    satsuma::AgentUpdateResult failed_result;
    failed_result.update_id = failed_manifest.update_id;
    failed_result.vm_id = failed_manifest.vm_id;
    failed_result.version = failed_manifest.version;
    failed_result.status = "failed";
    failed_result.rollback_status = "succeeded";
    failed_result.error = "injected failure";
    failed_result.completed_at = "2026-07-27T00:01:00.000Z";
    satsuma::write_json_atomic(failed_directory / L"result.json", failed_result);
    const satsuma::AgentUpdateResult observed_failure =
        controller.wait_agent_update(
            "client",
            failed_manifest.update_id,
            std::chrono::seconds(1));
    expect(observed_failure.status == "failed",
        "Host did not return the failed update result");
    expect(std::filesystem::exists(failed_directory),
        "Host deleted failed update evidence");

    const satsuma::AgentUpdateManifest success_manifest =
        controller.publish_agent_update("client", candidate, "0.1.2");
    const std::filesystem::path success_directory =
        update_directory(config.shared_folder.host_root, success_manifest);
    satsuma::AgentUpdateResult success_result;
    success_result.update_id = success_manifest.update_id;
    success_result.vm_id = success_manifest.vm_id;
    success_result.version = success_manifest.version;
    success_result.status = "succeeded";
    success_result.rollback_status = "none";
    success_result.process_id = 4321;
    success_result.completed_at = "2026-07-27T00:02:00.000Z";
    satsuma::write_json_atomic(success_directory / L"result.json", success_result);
    success_result.version = "unexpected";
    satsuma::write_json_atomic(success_directory / L"result.json", success_result);
    bool mismatched_version_rejected = false;
    try {
        static_cast<void>(controller.wait_agent_update(
            "client",
            success_manifest.update_id,
            std::chrono::seconds(1)));
    } catch (const std::exception&) {
        mismatched_version_rejected = true;
    }
    expect(mismatched_version_rejected,
        "Host accepted an update result for a different version");
    expect(std::filesystem::exists(success_directory),
        "Host deleted an update directory after a mismatched result");

    success_result.version = success_manifest.version;
    satsuma::write_json_atomic(success_directory / L"result.json", success_result);
    const satsuma::AgentUpdateResult observed_success =
        controller.wait_agent_update(
            "client",
            success_manifest.update_id,
            std::chrono::seconds(1));
    expect(observed_success.status == "succeeded",
        "Host did not return the successful update result");
    expect(!std::filesystem::exists(success_directory),
        "Host retained a successful shared update directory");

    for (const auto& entry : std::filesystem::directory_iterator(
             config.shared_folder.host_root / L"updates" / L"client")) {
        expect(!entry.path().filename().native().starts_with(L".preparing-"),
            "Host retained an update staging directory");
    }
}

}  // namespace

// 运行 Host 更新通道测试并清理临时目录。
int main() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        satsuma::path_from_utf8(satsuma::make_id("satsuma-host-update-test"));
    try {
        test_host_update_flow(root);
        std::filesystem::remove_all(root);
        std::cout << "SatsumaHostUpdateTests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SatsumaHostUpdateTests failed: " << error.what() << '\n';
        std::error_code cleanup_error;
        std::filesystem::remove_all(root, cleanup_error);
        return 1;
    }
}
