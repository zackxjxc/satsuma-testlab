// 首次配置和已授权硬件自动绑定的隔离测试，不操作真实虚拟机。
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "bootstrap.hpp"
#include "gateway.hpp"
#include "satsuma/core/config.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/json_io.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/sha256.hpp"

namespace {

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Action>
void expect_error(Action action, const std::string& reason) {
    try {
        action();
    } catch (const std::exception& error) {
        expect(std::string(error.what()).find(reason) != std::string::npos,
               "Unexpected rejection: " + std::string(error.what()));
        return;
    }
    throw std::runtime_error("Expected rejection: " + reason);
}

void write_vmx(const std::filesystem::path& path, const std::string& raw_uuid) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << "uuid.bios = \"" << raw_uuid << "\"\r\n";
}

void test_bootstrap(const std::filesystem::path& root,
                    const std::filesystem::path& vmrun,
                    const std::filesystem::path& binary) {
    const auto vmx1 = root / L"VM Space" / L"VM 01.vmx";
    const auto vmx2 = root / L"VM 中文" / L"List Snapshots VM.vmx";
    write_vmx(vmx1, "56 4d 12 34 ab cd 43 21-98 76 00 11 22 33 44 55");
    write_vmx(vmx2, "56 4d 12 34 ab cd 43 21-98 76 00 11 22 33 44 66");
    const auto output = root / L"handoff" / L"lab.json";
    std::vector<std::wstring> options{
        L"--config", output.native(), L"--lab-id", L"bootstrap-test",
        L"--vmx", vmx1.parent_path().native(), L"--vmx", vmx2.native(),
        L"--base-snapshot", L"clean", L"--vmrun", vmrun.native(),
        L"--agent-binary", binary.native(),
    };
    const auto result = satsuma::host::initialize_lab(options, binary.parent_path());
    expect(result.at("status") == "initialized" && result.at("gateway_started") == false &&
           result.at("readiness_verified") == false, "init misreported readiness or gateway startup");
    const auto config = satsuma::load_lab_config(output);
    expect(config.vms.size() == 2 && config.transport.vmci_port == 42510,
           "init lost selected VMs or default bootstrap port");
    auto automatic = options;
    automatic[1] = (output.parent_path() / L"automatic.json").native();
    automatic.erase(automatic.begin() + 2, automatic.begin() + 4);
    const auto generated = satsuma::host::initialize_lab(automatic, binary.parent_path());
    expect(generated.at("agent_bootstrap").at("lab_id").get<std::string>().starts_with("lab-"),
           "init did not generate laboratory identity");
    const auto& vm = config.vms.front();
    expect(vm.hardware_id == "34124d56-cdab-2143-9876-001122334455",
           "VMX UUID byte order differs from Agent SMBIOS format");
    expect(vm.id == "vm-" + vm.hardware_id, "VM ID is not hardware-stable and neutral");
    expect(vm.agent_sha256 == satsuma::sha256_file(binary), "init failed to pin release binary");
    expect(!std::filesystem::exists(config.transport.state_root) &&
           !std::filesystem::exists(config.host.archive_root), "init created runtime state");
    const auto original_hash = satsuma::sha256_file(output);
    expect_error([&] { static_cast<void>(satsuma::host::initialize_lab(options, {})); },
                 "will not overwrite");
    expect(satsuma::sha256_file(output) == original_hash, "init changed existing configuration");

    auto retry = options;
    retry[1] = (output.parent_path() / L"reordered.json").native();
    retry[5] = vmx2.native();
    retry[7] = vmx1.native();
    retry.insert(retry.end(), {L"--vmci-port", L"42511"});
    const auto reordered = satsuma::host::initialize_lab(retry, {});
    expect(reordered.at("vms").at(1).at("id") == vm.id &&
           reordered.at("agent_bootstrap").at("vmci_port") == 42511,
           "VM ID depends on argument order or custom port was ignored");

    retry = options;
    retry[1] = (output.parent_path() / L"failed.json").native();
    retry[7] = vmx1.native();
    expect_error([&] { static_cast<void>(satsuma::host::initialize_lab(retry, {})); }, "Duplicate VM");
    retry[7] = vmx2.native();
    retry[9] = L"missing-baseline";
    expect_error([&] { static_cast<void>(satsuma::host::initialize_lab(retry, {})); }, "Base snapshot not found");
    expect(!std::filesystem::exists(retry[1]), "init published config after snapshot validation failed");
    for (const auto& entry : std::filesystem::directory_iterator(output.parent_path())) {
        expect(!entry.path().filename().native().starts_with(L".tmp-"), "init left partial config behind");
    }
    retry[9] = L"satsuma-ai-obsolete";
    expect_error([&] { static_cast<void>(satsuma::host::initialize_lab(retry, {})); }, "AI snapshot prefix");
    retry[9] = L"clean";
    retry.insert(retry.end(), {L"--vmci-port", L"-1"});
    expect_error([&] { static_cast<void>(satsuma::host::initialize_lab(retry, {})); }, "--vmci-port");

    // 现有文件共享协议下，第一次 index 即可取得绑定，不要求事先产生 presence。
    {
        satsuma::host::Gateway gateway(config);
        nlohmann::json request{
            {"schema_version", 1}, {"lab_id", config.lab_id},
            {"hardware_id", vm.hardware_id}, {"vm_id", vm.hardware_id},
            {"session_id", "first-session"}, {"operation", "index"}, {"after", ""},
        };
        const auto response = gateway.handle({request, {}});
        expect(response.metadata.at("files").size() == 1, "initial index omitted binding");
        const auto binding_path = config.transport.state_root / L"agents" /
            satsuma::path_from_utf8(vm.hardware_id + ".binding.json");
        const auto binding = satsuma::load_json(binding_path);
        expect(binding.at("vm_id") == vm.id && binding.at("hardware_id") == vm.hardware_id &&
               binding.at("lab_id") == config.lab_id, "initial binding has wrong identity");
        // 快照内缓存了另一消费者分配的 ID 时，仍可取得当前 Host 的权威绑定。
        request["hardware_id"] = config.vms.back().hardware_id;
        request["vm_id"] = "cached-from-previous-host";
        const auto cached = gateway.handle({request, {}});
        expect(cached.metadata.at("files").size() == 1, "cached identity could not obtain new binding");
        request["hardware_id"] = vm.hardware_id;
        request["vm_id"] = vm.hardware_id;
        auto manual = binding;
        manual["vm_id"] = "manually-bound";
        satsuma::write_json_atomic(binding_path, manual);
        static_cast<void>(gateway.handle({request, {}}));
        expect(satsuma::load_json(binding_path) == manual, "gateway overwrote manual binding");
        request["hardware_id"] = "34124d56-cdab-2143-9876-001122334499";
        request["vm_id"] = request.at("hardware_id");
        expect(gateway.handle({request, {}}).metadata.at("files").empty(),
               "gateway enrolled hardware outside selected VM scope");
        request["lab_id"] = "wrong-lab";
        expect_error([&] { static_cast<void>(gateway.handle({request, {}})); }, "lab identity mismatch");
    }
    retry = options;
    retry[1] = (output.parent_path() / L"lab.config").native();
    expect_error([&] { static_cast<void>(satsuma::host::initialize_lab(retry, {})); }, "fresh state/archive");
}

}  // namespace

int wmain(const int argc, wchar_t* argv[]) {
    if (argc != 3) {
        return 2;
    }
    const auto root = std::filesystem::temp_directory_path() /
        satsuma::path_from_utf8(satsuma::make_id("satsuma-bootstrap-test"));
    try {
        test_bootstrap(root, argv[1], argv[2]);
        std::filesystem::remove_all(root);
        std::cout << "Host bootstrap tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Host bootstrap tests failed: " << error.what() << '\n';
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        return 1;
    }
}
