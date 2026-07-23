// Satsuma Host 和 VM 配置模型。
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace satsuma {

// VMware Workstation Provider 配置。
struct ProviderConfig {
    std::string type;               // Provider 类型
    std::filesystem::path vmrun;    // vmrun.exe 路径
};

// Host 监听和归档配置。
struct HostConfig {
    std::string listen;                 // Host 管理地址
    std::filesystem::path archive_root; // Guest 不可见的归档根目录
};

// Host 和 Guest 的共享目录映射。
struct SharedFolderConfig {
    std::filesystem::path host_root; // Host 共享根目录
    std::string guest_root;          // Guest 共享根目录 UTF-8 表示
};

// 单台 VM 的快照所有权和配额策略。
struct SnapshotConfig {
    std::string base;           // 用户只读基础快照
    std::string ai_prefix;      // AI 派生快照固定前缀
    int max_ai_snapshots{0};    // AI 派生快照数量上限
};

// 单台虚拟机的稳定配置。
struct VmConfig {
    std::string id;              // 虚拟机稳定 ID
    std::string role;            // 业务展示角色
    std::filesystem::path vmx;   // VMX 文件路径
    std::string agent_version;   // 快照中的 Agent 版本
    SnapshotConfig snapshots;    // 快照保护策略
    std::string management_ip;   // 管理网络地址
};

// Host 使用的实验室配置。
struct LabConfig {
    int schema_version{1};              // 配置 schema 版本
    std::string lab_id;                 // 实验室稳定 ID
    ProviderConfig provider;            // VMware Provider 配置
    HostConfig host;                    // Host 配置
    SharedFolderConfig shared_folder;   // 共享目录映射
    std::vector<VmConfig> vms;          // 允许的虚拟机列表
};

// VM Agent 的本机配置。
struct AgentConfig {
    int schema_version{1};                  // 配置 schema 版本
    int protocol_version{1};                // 文件协议版本
    std::string lab_id;                     // 实验室稳定 ID
    std::string vm_id;                      // 当前虚拟机稳定 ID
    std::string agent_version;              // 当前 Agent 语义版本
    std::string host;                       // Host RPC address:port
    std::filesystem::path shared_root;      // Guest 共享根目录
    std::filesystem::path local_work_root;  // Guest 本地执行根目录
    int poll_interval_ms{1000};             // 无任务时的轮询间隔
    int reconnect_interval_ms{1000};        // RPC 断线后的重连间隔
    int rpc_timeout_ms{5000};               // 单次 RPC 超时
};

// 读取并验证 Host 实验室配置。
[[nodiscard]] LabConfig load_lab_config(const std::filesystem::path& path);

// 读取并验证 VM Agent 配置。
[[nodiscard]] AgentConfig load_agent_config(const std::filesystem::path& path);

// 查找已配置的虚拟机；不存在时返回空。
[[nodiscard]] const VmConfig* find_vm(const LabConfig& config, const std::string& vm_id);

}  // namespace satsuma
