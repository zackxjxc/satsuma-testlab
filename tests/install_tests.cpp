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

void test_attempt_directory_acl(const std::filesystem::path& root) {
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID administrators{};
    expect(AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
        DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &administrators) != FALSE,
        "cannot create Administrators SID");
    BOOL elevated{};
    const BOOL membership_ok = CheckTokenMembership(nullptr, administrators, &elevated);
    FreeSid(administrators);
    expect(membership_ok != FALSE, "cannot inspect test token membership");
    if (!elevated) {
        rejected([&] { satsuma::vm::create_agent_attempt_directory(root / L"denied-attempt"); },
            "non-administrator created a private Agent attempt in an untrusted directory");
        std::cout << "Private attempt inherited-ACL test requires an elevated test token\n";
        return;
    }

    const auto staging = root / L"staging";
    std::filesystem::create_directory(staging);
    PSECURITY_DESCRIPTOR descriptor{};
    expect(ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;OICIIO;FA;;;WD)",
        SDDL_REVISION_1, &descriptor, nullptr) != FALSE,
        "cannot prepare inherited-write ACL fixture");
    PACL acl{};
    PSID owner{};
    BOOL present{}, defaulted{};
    expect(GetSecurityDescriptorOwner(descriptor, &owner, &defaulted) != FALSE &&
        GetSecurityDescriptorDacl(descriptor, &present, &acl, &defaulted) != FALSE && present,
        "cannot read inherited-write ACL fixture");
    auto staging_name = satsuma::windows_file_path(staging);
    const DWORD set_result = SetNamedSecurityInfoW(staging_name.data(), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        owner, nullptr, acl, nullptr);
    LocalFree(descriptor);
    expect(set_result == ERROR_SUCCESS, "cannot set inherited-write ACL fixture");

    const auto attempt = staging / L"job-private";
    satsuma::vm::create_agent_attempt_directory(attempt);
    satsuma::vm::validate_install_tree(attempt);
    const auto log = attempt / L"stdout.log";
    { std::ofstream output(log); output << "preserve existing evidence"; }
    // 子文件也不能继承到普通用户写权限。
    auto log_name = satsuma::windows_file_path(log);
    descriptor = nullptr;
    expect(GetNamedSecurityInfoW(log_name.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, &acl, nullptr, &descriptor) == ERROR_SUCCESS && acl,
        "cannot inspect private attempt file ACL");
    bool inherited_user_access = false;
    for (DWORD index = 0; index < acl->AceCount; ++index) {
        void* raw{};
        expect(GetAce(acl, index, &raw) != FALSE, "cannot inspect private file ACE");
        const auto* header = static_cast<ACE_HEADER*>(raw);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
        const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
        const auto sid = const_cast<DWORD*>(&ace->SidStart);
        if (!IsWellKnownSid(sid, WinLocalSystemSid) &&
            !IsWellKnownSid(sid, WinBuiltinAdministratorsSid)) inherited_user_access = true;
    }
    LocalFree(descriptor);
    expect(!inherited_user_access, "attempt file inherited a non-administrator allow ACE");
    const auto original_hash = satsuma::sha256_file(log);
    rejected([&] { satsuma::vm::create_agent_attempt_directory(attempt); },
        "existing attempt directory was reused");
    expect(satsuma::sha256_file(log) == original_hash, "existing attempt evidence was changed");
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
        test_attempt_directory_acl(root);
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
