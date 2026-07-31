// VMware vmrun 结构化调用接口。
#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace satsuma::vmware {

// vmrun 关闭虚拟机时使用的电源模式。
enum class VmStopMode {
    Soft,
    Hard,
};

// 通过 CreateProcessW 调用可信 vmrun.exe。
class VmrunProvider {
public:
    // 绑定 vmrun.exe 路径和单次命令超时。
    explicit VmrunProvider(
        std::filesystem::path executable,
        std::chrono::milliseconds timeout = std::chrono::seconds(30));

    // 查询当前正在运行的 VMX 绝对路径。
    [[nodiscard]] std::vector<std::filesystem::path> list_running() const;

    // 查询目标 VMX 是否出现在 VMware 的当前运行列表中。
    [[nodiscard]] bool is_running(const std::filesystem::path& vmx) const;

    // 查询指定 VMX 的全部快照名。
    [[nodiscard]] std::vector<std::string> list_snapshots(
        const std::filesystem::path& vmx) const;

    // 查询指定 VMX 的 VMware Tools 原始状态。
    [[nodiscard]] std::string check_tools_state(
        const std::filesystem::path& vmx) const;

    // 通过 VMware Tools 获取 Guest IP，确认 Guest 通道确实可用。
    [[nodiscard]] std::string get_guest_ip_address(
        const std::filesystem::path& vmx) const;

    // 以无界面模式启动指定 VMX。
    void start(const std::filesystem::path& vmx) const;

    // 使用明确的电源模式关闭指定 VMX。
    void stop(const std::filesystem::path& vmx, VmStopMode mode) const;

    // 将指定 VMX 恢复到已有快照。
    void revert_to_snapshot(
        const std::filesystem::path& vmx,
        std::string_view snapshot_name) const;

    // 为指定 VMX 创建一个命名快照。
    void create_snapshot(
        const std::filesystem::path& vmx,
        std::string_view snapshot_name) const;

    // 删除指定 VMX 的单个命名快照。
    void delete_snapshot(
        const std::filesystem::path& vmx,
        std::string_view snapshot_name) const;

private:
    // 在有限时间内等待目标 VM 收敛到指定电源状态。
    void wait_for_running_state(
        const std::filesystem::path& vmx,
        bool expected_running) const;

    // 执行结构化参数并返回 UTF-8 stdout。
    [[nodiscard]] std::string invoke(const std::vector<std::string>& arguments) const;

    std::filesystem::path executable_;   // vmrun.exe 绝对路径
    std::chrono::milliseconds timeout_;  // 单次命令超时
};

}  // namespace satsuma::vmware
