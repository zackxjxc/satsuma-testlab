// VmrunProvider 结构化调用测试。
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

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
        start_path_ = create_vmx(L"Client VM.vmx");
        soft_stop_path_ = create_vmx(L"Soft VM.vmx");
        hard_stop_path_ = create_vmx(L"Hard VM.vmx");
        snapshot_path_ = create_vmx(L"Snapshot VM.vmx");
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

    // 返回快照恢复测试使用的 VMX 路径。
    [[nodiscard]] const std::filesystem::path& snapshot_path() const noexcept {
        return snapshot_path_;
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
    std::filesystem::path snapshot_path_;   // 快照恢复测试路径
};

}  // namespace

// 使用假 vmrun 验证结构化 list/start/stop/revertToSnapshot 调用。
int wmain(const int argc, wchar_t* argv[]) {
    try {
        if (argc != 2) {
            throw std::runtime_error("fake vmrun path is required");
        }
        satsuma::vmware::VmrunProvider provider(
            std::filesystem::path(argv[1]),
            std::chrono::seconds(5));
        const auto paths = provider.list_running();
        expect(paths.size() == 2, "vmrun list did not return two VM paths");
        expect(paths.at(0) == L"C:\\VM Space\\Client.vmx", "VM path with spaces changed");
        expect(paths.at(1) == L"D:\\Gateway\\Gateway.vmx", "second VM path changed");
        const TemporaryVmx vmx;
        provider.start(vmx.start_path());
        provider.stop(vmx.soft_stop_path(), satsuma::vmware::VmStopMode::Soft);
        provider.stop(vmx.hard_stop_path(), satsuma::vmware::VmStopMode::Hard);
        provider.revert_to_snapshot(vmx.snapshot_path(), "Clean Base");
        std::cout << "SatsumaVmrunProviderTests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SatsumaVmrunProviderTests failed: " << error.what() << '\n';
        return 1;
    }
}
