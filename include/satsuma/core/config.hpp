// Satsuma Host 和 VM 配置模型。
#pragma once

#include <filesystem>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "satsuma/core/protocol.hpp"

namespace satsuma {

// VMware Workstation Provider 配置。
struct ProviderConfig {
    std::string type;               // Provider 类型
    std::filesystem::path vmrun;    // vmrun.exe 路径
};

// Host 监听和归档配置。
struct HostConfig {
    std::filesystem::path archive_root; // Guest 不可见的归档根目录
};

// Host 常驻网关的 VMCI 监听与本地持久状态。
struct HostTransportConfig {
    std::filesystem::path state_root; // 仅 Host 可见的协议事实源
    std::uint32_t vmci_port{0};       // VMCI REP 监听端口
};

// Guest 连接 Host 网关使用的 VMCI 地址。
struct AgentTransportConfig {
    std::uint32_t host_cid{2};        // VMware Host 的 VMCI CID
    std::uint32_t vmci_port{0};       // 与 Host 网关一致的端口
    int request_timeout_ms{10'000};   // 单次 RPC 有限等待
};

// 单台 VM 的快照所有权和配额策略。
struct SnapshotConfig {
    std::string base;           // 用户只读基础快照
    std::string ai_prefix;      // AI 派生快照固定前缀
    int max_ai_snapshots{0};    // AI 派生快照数量上限
};

// 单台虚拟机的稳定配置。
struct VmConfig {
    std::string id;               // 虚拟机稳定 ID
    std::string hardware_id;      // 可选 SMBIOS UUID 绑定
    std::filesystem::path vmx;    // VMX 文件路径
    std::string agent_version;    // 快照中的 Agent 版本
    SnapshotConfig snapshots;     // 快照保护策略
};

// Host 使用的实验室配置。
struct LabConfig {
    std::string lab_id;                 // 实验室稳定 ID
    ProviderConfig provider;            // VMware Provider 配置
    HostConfig host;                    // Host 配置
    HostTransportConfig transport;      // Host VMCI 网关配置
    std::vector<VmConfig> vms;          // 允许的虚拟机列表
};

// VM Agent 的本机配置。
struct AgentConfig {
    int protocol_version{kRunManifestProtocolVersion}; // VMCI 任务协议版本
    std::string lab_id;                     // 实验室稳定 ID
    std::string vm_id;                      // 当前虚拟机的稳定标识，未绑定时暂用硬件 ID
    std::string hardware_id;                // 当前 SMBIOS UUID
    std::string previous_hardware_id;       // 变更前的 SMBIOS UUID
    std::string previous_vm_id;             // 变更前缓存的 VM 标识
    bool vm_id_configured{false};            // agent.json 是否显式声明 vm_id
    bool identity_unbound{false};            // 当前硬件尚未绑定 VM 标识
    std::string agent_version;              // 当前 Agent 语义版本
    std::string last_update_id;              // 最近成功应用的更新 ID
    std::filesystem::path storage_root;     // 安装器选定的统一本地存储根
    std::filesystem::path mirror_root;     // Guest 本地 VMCI 持久化镜像
    std::filesystem::path local_work_root;  // Guest 本地执行根目录
    AgentTransportConfig transport;         // Host VMCI endpoint
    int poll_interval_ms{1000};             // 无任务时的轮询间隔
    int reconnect_interval_ms{1000};        // VMCI 网关异常后的重试间隔
};

// 读取并验证 Host 实验室配置。
[[nodiscard]] LabConfig load_lab_config(const std::filesystem::path& path);

// 读取并验证 VM Agent 配置。
[[nodiscard]] AgentConfig load_agent_config(const std::filesystem::path& path);

// 查找已配置的虚拟机；不存在时返回空。
[[nodiscard]] const VmConfig* find_vm(const LabConfig& config, const std::string& vm_id);

}  // namespace satsuma
