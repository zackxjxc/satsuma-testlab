// VmrunProvider 结构化调用测试。
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "satsuma/core/errors.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/path.hpp"
#include "vmrun_provider.hpp"

namespace {

// 在条件不满足时终止当前测试。
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// 管理电源操作测试使用的临时 VMX 目录。
class TemporaryVmx {
public:
    // 创建带空格的 VMX 路径并区分各电源模式。
    TemporaryVmx() {
        directory_ = std::filesystem::temp_directory_path() /
                     satsuma::path_from_utf8(satsuma::make_id("vmrun-start-test"));
        std::filesystem::create_directories(directory_);
        start_path_ = create_vmx(L"VM 01.vmx");
        soft_stop_path_ = create_vmx(L"Soft VM.vmx");
        hard_stop_path_ = create_vmx(L"Hard VM.vmx");
        reconciled_stop_path_ = create_vmx(L"Stop Reconciled VM.vmx");
        failed_stop_path_ = create_vmx(L"Stop Failed VM.vmx");
        snapshot_path_ = create_vmx(L"Snapshot VM.vmx");
        snapshot_retry_path_ = create_vmx(L"Snapshot Retry VM.vmx");
        create_snapshot_path_ = create_vmx(L"Create VM.vmx");
        list_snapshots_path_ = create_vmx(L"List Snapshots VM.vmx");
        delete_snapshot_path_ = create_vmx(L"Delete VM.vmx");
        tools_running_path_ = create_vmx(L"Tools Running VM.vmx");
        tools_installed_path_ = create_vmx(L"Tools Installed VM.vmx");
        tools_unknown_path_ = create_vmx(L"Tools Unknown VM.vmx");
    }

    TemporaryVmx(const TemporaryVmx&) = delete;
    TemporaryVmx& operator=(const TemporaryVmx&) = delete;

    // 清理测试产生的临时文件。
    ~TemporaryVmx() {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    // 返回测试 VMX 的绝对路径。
    [[nodiscard]] const std::filesystem::path& start_path() const noexcept {
        return start_path_;
    }

    // 返回软关闭测试使用的 VMX 路径。
    [[nodiscard]] const std::filesystem::path& soft_stop_path() const noexcept {
        return soft_stop_path_;
    }

    // 返回硬关闭测试使用的 VMX 路径。
    [[nodiscard]] const std::filesystem::path& hard_stop_path() const noexcept {
        return hard_stop_path_;
    }

    // 返回 vmrun 非零但已关机的收敛测试路径。
    [[nodiscard]] const std::filesystem::path& reconciled_stop_path() const noexcept {
        return reconciled_stop_path_;
    }

    // 返回 vmrun 非零且仍运行的失败测试路径。
    [[nodiscard]] const std::filesystem::path& failed_stop_path() const noexcept {
        return failed_stop_path_;
    }

    // 返回快照恢复测试使用的 VMX 路径。
    [[nodiscard]] const std::filesystem::path& snapshot_path() const noexcept {
        return snapshot_path_;
    }

    // 返回快照恢复瞬时失败重试测试使用的 VMX 路径。
    [[nodiscard]] const std::filesystem::path& snapshot_retry_path() const noexcept {
        return snapshot_retry_path_;
    }

    // 返回快照创建测试使用的 VMX 路径。
    [[nodiscard]] const std::filesystem::path& create_snapshot_path() const noexcept {
        return create_snapshot_path_;
    }

    // 返回快照列表测试使用的 VMX 路径。
    [[nodiscard]] const std::filesystem::path& list_snapshots_path() const noexcept {
        return list_snapshots_path_;
    }

    // 返回快照删除测试使用的 VMX 路径。
    [[nodiscard]] const std::filesystem::path& delete_snapshot_path() const noexcept {
        return delete_snapshot_path_;
    }

    // 返回 Tools 正常运行测试使用的 VMX 路径。
    [[nodiscard]] const std::filesystem::path& tools_running_path() const noexcept {
        return tools_running_path_;
    }

    // 返回 Tools 已安装但未运行测试使用的 VMX 路径。
    [[nodiscard]] const std::filesystem::path& tools_installed_path() const noexcept {
        return tools_installed_path_;
    }

    // 返回 Tools 未知状态测试使用的 VMX 路径。
    [[nodiscard]] const std::filesystem::path& tools_unknown_path() const noexcept {
        return tools_unknown_path_;
    }

private:
    // 创建单个最小 VMX 占位文件。
    [[nodiscard]] std::filesystem::path create_vmx(const std::filesystem::path& filename) const {
        const std::filesystem::path path = directory_ / filename;
        std::ofstream output(path);
        if (!output) {
            throw std::runtime_error("cannot create temporary VMX");
        }
        output.put('\n');
        return path;
    }

    std::filesystem::path directory_;       // 临时目录
    std::filesystem::path start_path_;      // 启动测试路径
    std::filesystem::path soft_stop_path_;  // 软关闭测试路径
    std::filesystem::path hard_stop_path_;  // 硬关闭测试路径
    std::filesystem::path reconciled_stop_path_; // 已达到关机状态的非零退出路径
    std::filesystem::path failed_stop_path_; // 未达到关机状态的非零退出路径
    std::filesystem::path snapshot_path_;   // 快照恢复测试路径
    std::filesystem::path snapshot_retry_path_; // 快照恢复瞬时失败重试测试路径
    std::filesystem::path create_snapshot_path_;  // 快照创建测试路径
    std::filesystem::path list_snapshots_path_;  // 快照列表测试路径
    std::filesystem::path delete_snapshot_path_;  // 快照删除测试路径
    std::filesystem::path tools_running_path_;  // Tools 运行状态测试路径
    std::filesystem::path tools_installed_path_;  // Tools 已安装状态测试路径
    std::filesystem::path tools_unknown_path_;  // Tools 未知状态测试路径
};

}  // namespace

// 使用假 vmrun 验证 Provider 当前支持的结构化调用。
int wmain(const int argc, wchar_t* argv[]) {
    try {
        if (argc != 2) {
            throw std::runtime_error("fake vmrun path is required");
        }
        std::error_code stale_state_error;
        std::filesystem::remove(
            std::filesystem::temp_directory_path() / L"satsuma-fake-vmrun-running-vm.txt",
            stale_state_error);
        satsuma::vmware::VmrunProvider provider(
            std::filesystem::path(argv[1]),
            std::chrono::seconds(5));
        const auto paths = provider.list_running();
        expect(paths.size() == 2, "vmrun list did not return two VM paths");
        expect(paths.at(0) == L"C:\\VM Space\\VM-01.vmx", "VM path with spaces changed");
        expect(paths.at(1) == L"D:\\VM-02\\VM-02.vmx", "second VM path changed");
        const TemporaryVmx vmx;
        const auto snapshots = provider.list_snapshots(vmx.list_snapshots_path());
        expect(snapshots.size() == 3, "vmrun listSnapshots did not return three snapshots");
        expect(snapshots.at(0) == "clean", "base snapshot name changed");
        expect(snapshots.at(2) == "satsuma-ai-vm_01-ready", "AI snapshot name changed");
        expect(provider.check_tools_state(vmx.tools_running_path()) == "running",
            "vmrun checkToolsState did not preserve the running state");
        expect(provider.check_tools_state(vmx.tools_installed_path()) == "installed",
            "vmrun checkToolsState did not preserve the installed state");
        expect(provider.check_tools_state(vmx.tools_unknown_path()) == "unknown",
            "vmrun checkToolsState did not preserve the unknown state");
        expect(provider.get_guest_ip_address(vmx.tools_installed_path()) == "192.0.2.10",
            "vmrun getGuestIPAddress did not preserve the Guest address");
        provider.start(vmx.start_path());
        provider.stop(vmx.start_path(), satsuma::vmware::VmStopMode::Hard);
        provider.stop(vmx.soft_stop_path(), satsuma::vmware::VmStopMode::Soft);
        provider.stop(vmx.hard_stop_path(), satsuma::vmware::VmStopMode::Hard);
        provider.stop(vmx.reconciled_stop_path(), satsuma::vmware::VmStopMode::Hard);
        const satsuma::vmware::VmrunProvider fast_timeout_provider(
            std::filesystem::path(argv[1]),
            std::chrono::milliseconds(500));
        bool stop_failed = false;
        try {
            fast_timeout_provider.stop(
                vmx.failed_stop_path(),
                satsuma::vmware::VmStopMode::Hard);
        } catch (const satsuma::Error&) {
            stop_failed = true;
        }
        expect(stop_failed, "vmrun stop failure was hidden while the VM remained running");
        provider.stop(vmx.failed_stop_path(), satsuma::vmware::VmStopMode::Hard);
        provider.revert_to_snapshot(vmx.snapshot_path(), "Clean Base");
        provider.revert_to_snapshot(vmx.snapshot_retry_path(), "Clean Base");
        provider.create_snapshot(vmx.create_snapshot_path(), "satsuma-ai-network-ready");
        provider.delete_snapshot(vmx.delete_snapshot_path(), "satsuma-ai-obsolete");
        std::cout << "SatsumaVmrunProviderTests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SatsumaVmrunProviderTests failed: " << error.what() << '\n';
        return 1;
    }
}
