// 无预置实验室身份的登记、冷快照缓存隔离和兼容性测试；不依赖真实 VM。
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

#include "agent.hpp"
#include "gateway.hpp"
#include "hardware_identity.hpp"
#include "identity.hpp"
#include "vmci_channel.hpp"
#include "satsuma/core/config.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/version.hpp"

namespace {
void expect(bool value, const char* reason) {
    if (!value) { throw std::runtime_error(reason); }
}
template <class F> void rejects(F action, const char* reason) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error(reason);
}

void run(const std::filesystem::path& root) {
    const auto path = root / L"agent.json";
    const nlohmann::json minimal = {
        {"schema_version", 1}, {"storage_root", satsuma::path_to_utf8(root / L"guest")},
    };
    satsuma::write_json_atomic(path, minimal);
    auto agent = satsuma::load_agent_config(path);
    expect(agent.auto_enroll && agent.lab_id.empty() && agent.vm_id.empty(), "minimal config has fixed identity");
    expect(agent.transport.host_cid == 2 && agent.transport.vmci_port == 42510 &&
           agent.agent_version == satsuma::kVersion, "bootstrap defaults changed");
    const std::string uuid = "34124d56-cdab-2143-9876-001122334455";
    satsuma::vm::prepare_agent_hardware_identity(agent, uuid);
    expect(agent.identity_unbound, "fresh Agent was not waiting for enrollment");

    satsuma::LabConfig lab;
    lab.lab_id = "consumer-one";
    lab.transport.state_root = root / L"host-one";
    lab.transport.vmci_port = 42510;
    satsuma::VmConfig vm;
    vm.id = "vm-neutral";
    vm.hardware_id = uuid;
    vm.agent_version = satsuma::kVersion;
    vm.agent_sha256 = std::string(64, 'a');
    lab.vms.push_back(vm);
    auto host = std::make_unique<satsuma::host::Gateway>(lab);
    nlohmann::json hello = {
        {"schema_version", 1}, {"operation", "enroll"}, {"enrollment_version", 1},
        {"protocol_version", satsuma::kRunManifestProtocolVersion}, {"hardware_id", uuid},
        {"session_id", "session-one"}, {"agent_version", satsuma::kVersion},
        {"binary_sha256", std::string(64, 'b')},
    };
    const auto enrolled = host->handle({hello, {}}).metadata;
    expect(satsuma::vm::apply_agent_enrollment(agent, enrolled), "first enrollment did not update identity");
    expect(agent.lab_id == lab.lab_id && agent.vm_id == vm.id && !agent.identity_unbound,
           "Host assignment was not applied");
    expect(agent.mirror_root != agent.bootstrap_mirror_root, "enrollment reused unscoped mirror");
    expect(!satsuma::vm::apply_agent_enrollment(agent, enrolled), "identical enrollment changed scope");
    const auto original_mirror = agent.mirror_root;
    const auto original_work = agent.local_work_root;
    expect(satsuma::load_json(path) == minimal, "enrollment rewrote portable installation config");

    // 登记不能把 Agent 自报的二进制当成可信预期值；check 的校验仍会报错。
    satsuma::write_json_atomic(satsuma::host::vm_presence_path(lab, vm), {
        {"schema_version", 2}, {"protocol_version", satsuma::kRunManifestProtocolVersion},
        {"lab_id", lab.lab_id}, {"vm_id", vm.id}, {"hardware_id", uuid},
        {"agent_version", satsuma::kVersion}, {"binary_sha256", std::string(64, 'b')}, {"status", "idle"},
    });
    rejects([&] { static_cast<void>(satsuma::host::load_vm_presence(lab, vm)); }, "wrong binary passed identity check");
    auto invalid = hello;
    invalid["hardware_id"] = "34124d56-cdab-2143-9876-001122334499";
    rejects([&] { static_cast<void>(host->handle({invalid, {}})); }, "unknown VM enrolled");
    invalid = hello;
    invalid["protocol_version"] = 999;
    rejects([&] { static_cast<void>(host->handle({invalid, {}})); }, "wrong protocol enrolled");
    auto bad_reply = enrolled;
    bad_reply["enrollment_id"] = "../escape";
    rejects([&] { static_cast<void>(satsuma::vm::apply_agent_enrollment(agent, bad_reply)); }, "unsafe cache path accepted");
    expect(agent.mirror_root == original_mirror, "failed enrollment changed active scope");

    host.reset();
    host = std::make_unique<satsuma::host::Gateway>(lab);
    expect(host->handle({hello, {}}).metadata.at("enrollment_id") == enrolled.at("enrollment_id"),
           "Host restart discarded persistent enrollment scope");
    auto restored = satsuma::load_agent_config(path);
    satsuma::vm::prepare_agent_hardware_identity(restored, uuid);
    expect(restored.auto_enroll && restored.mirror_root == original_mirror, "update helper cannot locate cached scope");
    // 测试旁路没有在线通道，缓存再完整也不允许执行任何旧任务。
    satsuma::vm::Agent offline(restored);
    rejects([&] { static_cast<void>(offline.run_once()); }, "cached enrollment permitted offline execution");

    lab.transport.state_root = root / L"host-two";
    auto other = std::make_unique<satsuma::host::Gateway>(lab);
    const auto next = other->handle({hello, {}}).metadata;
    expect(satsuma::vm::apply_agent_enrollment(agent, next) &&
           agent.mirror_root != original_mirror && agent.local_work_root != original_work,
           "new consumer inherited old mirror or work data");
    nlohmann::json old_request{
        {"schema_version", 1}, {"operation", "index"}, {"lab_id", lab.lab_id},
        {"vm_id", vm.id}, {"hardware_id", uuid}, {"session_id", "session-one"},
        {"enrollment_id", enrolled.at("enrollment_id")},
    };
    rejects([&] { static_cast<void>(other->handle({old_request, {}})); }, "new Host accepted stale enrollment");
    auto clone = satsuma::load_agent_config(path);
    satsuma::vm::prepare_agent_hardware_identity(clone, "34124d56-cdab-2143-9876-001122334499");
    expect(clone.identity_unbound && clone.lab_id.empty(), "clone inherited another machine's enrollment");

    // 使用真实序列化/RPC/文件同步代码验证握手闭环，仅替换测试端点，不操作 VMware。
    const std::string endpoint = "tcp://127.0.0.1:43911";
    satsuma::transport::Server server(endpoint, [&other](const satsuma::transport::Message& request) {
        return other->handle(request);
    }, std::chrono::milliseconds(50));
    std::jthread listener([&server](std::stop_token stop) { server.run(stop); });
    auto wire = satsuma::load_agent_config(path);
    satsuma::vm::prepare_agent_hardware_identity(wire, uuid);
    satsuma::vm::VmciChannel channel(wire, "wire-session", endpoint);
    static_cast<void>(channel.enroll(wire, std::string(64, 'a')));
    channel.synchronize_inbound();
    expect(wire.enrollment_id == next.at("enrollment_id").get<std::string>() && wire.lab_id == lab.lab_id,
           "wire enrollment omitted assigned identity");
    satsuma::write_json_atomic(satsuma::vm::hardware_presence_path(wire), {
        {"schema_version", 2}, {"protocol_version", satsuma::kRunManifestProtocolVersion},
        {"lab_id", wire.lab_id}, {"vm_id", wire.vm_id}, {"hardware_id", uuid},
        {"agent_version", satsuma::kVersion}, {"binary_sha256", vm.agent_sha256}, {"status", "idle"},
    });
    channel.synchronize_outbound();
    expect(satsuma::host::load_vm_presence(lab, vm).at("vm_id").get<std::string>() == wire.vm_id,
           "enrolled Agent could not publish over the normal RPC channel");
    listener.request_stop();
}
}

int main() {
    const auto root = std::filesystem::temp_directory_path() / satsuma::path_from_utf8(satsuma::make_id("enrollment-test"));
    try {
        run(root);
        std::filesystem::remove_all(root);
        std::cout << "Enrollment tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        return 1;
    }
}
