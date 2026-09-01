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

// 验证指定调用稳定抛出异常。
template <typename Function>
void expect_error(Function&& function, const std::string& message) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

// 验证指定调用抛出的异常包含现场诊断信息。
template <typename Function>
void expect_error_contains(
    Function&& function,
    const std::string& expected,
    const std::string& message) {
    try {
        function();
    } catch (const std::exception& error) {
        expect(std::string(error.what()).find(expected) != std::string::npos, message);
        return;
    }
    throw std::runtime_error(message);
}

// 创建仅包含更新发布所需字段的实验室配置。
[[nodiscard]] satsuma::LabConfig make_config(const std::filesystem::path& root) {
    satsuma::LabConfig config;
    config.lab_id = "test_lab";
    config.transport.state_root = root / L"state";
    satsuma::VmConfig vm;
    vm.id = "vm_01";
    config.vms.push_back(std::move(vm));
    satsuma::VmConfig vm_02;
    vm_02.id = "vm_02";
    config.vms.push_back(std::move(vm_02));
    return config;
}

[[nodiscard]] std::filesystem::path update_directory(
    const std::filesystem::path& share,
    const satsuma::AgentUpdateManifest& manifest);

// 验证 Host 只向无 presence 的已登记目标发布身份迁移。
void test_host_rebind_publish(const std::filesystem::path& root) {
    const satsuma::LabConfig config = make_config(root);
    std::filesystem::create_directories(config.transport.state_root);
    const std::filesystem::path candidate = root / L"rebind-candidate.exe";
    std::ofstream(candidate, std::ios::binary) << "host-rebind-candidate";
    const satsuma::host::Controller controller(config);

    const satsuma::AgentUpdateManifest manifest =
        controller.publish_agent_update("vm_01", candidate, "0.1.1", "vm_02");
    expect(manifest.protocol_version == 2, "Host rebind did not use update protocol 2");
    expect(manifest.next_vm_id == "vm_02", "Host rebind lost its target identity");
    const std::filesystem::path directory =
        update_directory(config.transport.state_root, manifest);
    const satsuma::AgentUpdateManifest published =
        satsuma::load_agent_update_manifest(directory / L"update.json");
    expect(published.next_vm_id == manifest.next_vm_id,
        "published rebind manifest lost its target identity");

    expect_error(
        [&controller, &candidate] {
            static_cast<void>(controller.publish_agent_update(
                "vm_01", candidate, "0.1.1", "vm_01"));
        },
        "Host accepted a rebind to the current identity");
    expect_error(
        [&controller, &candidate] {
            static_cast<void>(controller.publish_agent_update(
                "vm_01", candidate, "0.1.1", "missing"));
        },
        "Host accepted an unknown rebind target");

    const std::filesystem::path target_presence =
        config.transport.state_root / L"agents" / L"vm_02.json";
    std::filesystem::create_directories(target_presence.parent_path());
    std::ofstream(target_presence, std::ios::binary) << "{}";
    expect_error(
        [&controller, &candidate] {
            static_cast<void>(controller.publish_agent_update(
                "vm_01", candidate, "0.1.1", "vm_02"));
        },
        "Host accepted a rebind target with an existing presence");
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
    std::filesystem::create_directories(config.transport.state_root);
    const std::filesystem::path candidate = root / L"candidate.exe";
    std::ofstream(candidate, std::ios::binary) << "host-update-candidate";
    const satsuma::host::Controller controller(config);

    const satsuma::AgentUpdateManifest failed_manifest =
        controller.publish_agent_update("vm_01", candidate, "0.1.1");
    const std::filesystem::path failed_directory =
        update_directory(config.transport.state_root, failed_manifest);
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
            "vm_01",
            failed_manifest.update_id,
            std::chrono::seconds(1));
    expect(observed_failure.status == "failed",
        "Host did not return the failed update result");
    expect(std::filesystem::exists(failed_directory),
        "Host deleted failed update evidence");

    const satsuma::AgentUpdateManifest success_manifest =
        controller.publish_agent_update("vm_01", candidate, "0.1.2");
    const std::filesystem::path success_directory =
        update_directory(config.transport.state_root, success_manifest);
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
            "vm_01",
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
            "vm_01",
            success_manifest.update_id,
            std::chrono::seconds(1));
    expect(observed_success.status == "succeeded",
        "Host did not return the successful update result");
    expect(!std::filesystem::exists(success_directory),
        "Host retained a successful shared update directory");

    for (const auto& entry : std::filesystem::directory_iterator(
             config.transport.state_root / L"updates" / L"vm_01")) {
        expect(!entry.path().filename().native().starts_with(L".preparing-"),
            "Host retained an update staging directory");
    }
}

// 验证旧更新队列不会让 Host 等待一个尚未被 Agent 处理的新请求。
void test_host_update_queue_guard(const std::filesystem::path& root) {
    const satsuma::LabConfig config = make_config(root / L"queue-guard");
    std::filesystem::create_directories(config.transport.state_root);
    const std::filesystem::path candidate = root / L"queue-guard-candidate.exe";
    std::ofstream(candidate, std::ios::binary) << "queue-guard-candidate";
    const satsuma::host::Controller controller(config);

    const satsuma::AgentUpdateManifest pending =
        controller.publish_agent_update("vm_01", candidate, "0.1.1");
    expect_error_contains(
        [&controller, &candidate] {
            static_cast<void>(controller.publish_agent_update(
                "vm_01", candidate, "0.1.2"));
        },
        pending.update_id,
        "Host did not identify the pending update that blocked a new publication");

    const std::filesystem::path pending_directory =
        update_directory(config.transport.state_root, pending);
    satsuma::AgentUpdateResult failed;
    failed.update_id = pending.update_id;
    failed.vm_id = pending.vm_id;
    failed.version = pending.version;
    failed.status = "failed";
    failed.rollback_status = "succeeded";
    failed.error = "injected failure";
    failed.completed_at = "2026-08-01T00:01:00.000Z";
    satsuma::write_json_atomic(pending_directory / L"result.json", failed);

    const satsuma::AgentUpdateManifest succeeded =
        controller.publish_agent_update("vm_01", candidate, "0.1.2");
    expect(std::filesystem::exists(pending_directory),
        "Host deleted failed update evidence while publishing a retry");
    const std::filesystem::path succeeded_directory =
        update_directory(config.transport.state_root, succeeded);
    satsuma::AgentUpdateResult success;
    success.update_id = succeeded.update_id;
    success.vm_id = succeeded.vm_id;
    success.version = succeeded.version;
    success.status = "succeeded";
    success.rollback_status = "none";
    success.process_id = 4321;
    success.completed_at = "2026-08-01T00:02:00.000Z";
    satsuma::write_json_atomic(succeeded_directory / L"result.json", success);

    const satsuma::AgentUpdateManifest next =
        controller.publish_agent_update("vm_01", candidate, "0.1.3");
    expect(!std::filesystem::exists(succeeded_directory),
        "Host retained successful update evidence before publishing the next update");
    expect(std::filesystem::exists(
        update_directory(config.transport.state_root, next)),
        "Host did not publish after cleaning a successful stale update");

    const std::filesystem::path invalid_directory =
        config.transport.state_root / L"updates" / L"vm_02" / L"update-invalid";
    std::filesystem::create_directories(invalid_directory);
    std::ofstream(invalid_directory / L"update.json", std::ios::binary) << "{}";
    expect_error_contains(
        [&controller, &candidate] {
            static_cast<void>(controller.publish_agent_update(
                "vm_02", candidate, "0.1.1"));
        },
        "update-invalid",
        "Host accepted an invalid legacy update directory");
}

}  // namespace

// 运行 Host 更新通道测试并清理临时目录。
int main() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        satsuma::path_from_utf8(satsuma::make_id("satsuma-host-update-test"));
    try {
        test_host_update_flow(root);
        test_host_rebind_publish(root);
        test_host_update_queue_guard(root);
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
