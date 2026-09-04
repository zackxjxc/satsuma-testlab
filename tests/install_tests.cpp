// 安装安全边界测试；不创建、停止或删除本机服务。
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include "install.hpp"
#include "satsuma/core/id.hpp"
#include "satsuma/core/path.hpp"
#include "satsuma/core/sha256.hpp"
#include "satsuma/core/version.hpp"

namespace {
void expect(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class Action> void rejected(Action action, const char* message) {
    bool failed = false;
    try { action(); } catch (const std::exception&) { failed = true; }
    expect(failed, message);
}
}

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 3) return 2;
    const auto root = std::filesystem::temp_directory_path() /
        satsuma::path_from_utf8(satsuma::make_id("satsuma-install-test"));
    try {
        using satsuma::vm::compare_agent_versions;
        expect(compare_agent_versions("0.3.10", "0.3.9") > 0, "numeric version ordering failed");
        expect(compare_agent_versions("1.0.0", "0.99.99") > 0, "major version ordering failed");
        expect(compare_agent_versions("0.3.3", "0.3.4") < 0, "older version ordering failed");
        expect(compare_agent_versions("0.3.4", "0.3.4.0") == 0, "equivalent versions differ");
        expect(compare_agent_versions("0.3.4.1", "0.3.4") > 0, "revision ordering failed");
        for (const auto* invalid : {"", "0.3", "0.3.4.", "0.3.4-beta", "0.3.-4", "0.3.4.0.1", "0.3.999999999999"})
            rejected([&] { (void)compare_agent_versions(invalid, "0.3.4"); }, "invalid version accepted");
        std::filesystem::create_directory(root);
        const auto image = std::filesystem::absolute(argv[1]);
        expect(satsuma::vm::verify_agent_image(image, true) == satsuma::kVersion,
            "valid release image/checksum rejected");
        rejected([&] { (void)satsuma::vm::verify_agent_image(argv[2], false); },
            "Host EXE must not be accepted as Guest Agent");
        const auto copied = root / L"SatsumaVM.exe";
        std::filesystem::copy_file(image, copied);
        expect(satsuma::sha256_file(image) == satsuma::sha256_file(copied), "copy digest changed");
        expect(satsuma::vm::verify_agent_image(copied, true) == satsuma::kVersion,
            "copied image rejected");
        // PE 仍可解析版本资源，但文件字节已经改变，必须拒绝损坏介质。
        { std::ofstream output(copied, std::ios::binary | std::ios::app); output.put('x'); }
        rejected([&] { (void)satsuma::vm::verify_agent_image(copied, true); },
            "modified image checksum accepted");
        const auto fake = root / L"fake.exe";
        { std::ofstream output(fake); output << "not a PE file"; }
        rejected([&] { (void)satsuma::vm::verify_agent_image(fake, false); },
            "unknown executable accepted");
        // 同进程不同线程模拟同时双击；Windows mutex 同线程可递归，不可用于此测试。
        {
            const satsuma::vm::AgentInstallLock first;
            bool blocked = false;
            std::thread contender([&] {
                try { const satsuma::vm::AgentInstallLock second; }
                catch (const std::exception&) { blocked = true; }
            });
            contender.join();
            expect(blocked, "concurrent installer acquired machine lock");
        }
        { const satsuma::vm::AgentInstallLock released; }
        // 当前用户创建的目录不能被悄悄接管为 SYSTEM 的可执行目录。
        auto root_name = root.native();
        PSECURITY_DESCRIPTOR descriptor{};
        expect(ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;OICI;FA;;;WD)", SDDL_REVISION_1, &descriptor, nullptr) != FALSE,
            "cannot prepare unsafe security descriptor");
        PACL acl{};
        BOOL present{}, defaulted{};
        expect(GetSecurityDescriptorDacl(descriptor, &present, &acl, &defaulted) != FALSE && present,
            "cannot read unsafe security descriptor");
        expect(SetNamedSecurityInfoW(root_name.data(), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, acl, nullptr) == ERROR_SUCCESS, "cannot prepare unsafe ACL fixture");
        LocalFree(descriptor);
        rejected([&] { satsuma::vm::validate_install_tree(root); },
            "unprotected user directory accepted");
        std::filesystem::remove_all(root);
        std::cout << "Installer integrity, ownership and concurrency checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        return 1;
    }
}
