# Satsuma 核心单元测试和 Host/VM 集成测试。
function(satsuma_enable_test_warnings target)
    satsuma_enable_strict_warnings(${target})
    satsuma_treat_angle_headers_as_external(${target})
endfunction()

add_library(
    SatsumaVmTestLib
    STATIC
    ${SATSUMA_VM_LIBRARY_SOURCES}
)
target_include_directories(SatsumaVmTestLib PUBLIC ../src/vm)
target_compile_definitions(
    SatsumaVmTestLib
    PRIVATE
        SATSUMA_INTERACTIVE_TESTS
        SATSUMA_SERVICE_TESTS
        SATSUMA_UPDATE_TESTS
        SATSUMA_TEST_LOCAL_MIRROR
)
target_link_libraries(
    SatsumaVmTestLib
    PUBLIC SatsumaCore SatsumaVmci advapi32 shell32 userenv version wtsapi32
)
satsuma_enable_strict_warnings(SatsumaVmTestLib)
satsuma_treat_angle_headers_as_external(SatsumaVmTestLib)

# 端到端测试使用独立 Agent，确保生产 SatsumaVM 不包含本地镜像旁路。
add_executable(SatsumaVMTestAgent ../src/vm/main.cpp)
target_link_libraries(SatsumaVMTestAgent PRIVATE SatsumaVmTestLib)
satsuma_enable_strict_warnings(SatsumaVMTestAgent)
satsuma_treat_angle_headers_as_external(SatsumaVMTestAgent)

add_executable(SatsumaCoreTests core_tests.cpp)
target_link_libraries(SatsumaCoreTests PRIVATE SatsumaCore)

add_executable(SatsumaTestFixture fixture.cpp)
target_compile_features(SatsumaTestFixture PRIVATE cxx_std_20)
target_link_libraries(SatsumaTestFixture PRIVATE SatsumaCore advapi32)
set_target_properties(SatsumaTestFixture PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

satsuma_enable_strict_warnings(SatsumaCoreTests)
satsuma_treat_angle_headers_as_external(SatsumaCoreTests)
satsuma_enable_strict_warnings(SatsumaTestFixture)

add_test(NAME SatsumaCoreTests COMMAND SatsumaCoreTests)

add_executable(SatsumaVersionInfoTests version_info_tests.cpp)
target_compile_features(SatsumaVersionInfoTests PRIVATE cxx_std_20)
target_link_libraries(SatsumaVersionInfoTests PRIVATE version)
satsuma_enable_test_warnings(SatsumaVersionInfoTests)
add_dependencies(SatsumaVersionInfoTests SatsumaHost SatsumaVM)
add_test(
    NAME SatsumaVersionInfoTests
    COMMAND SatsumaVersionInfoTests
        "${PROJECT_VERSION_MAJOR}"
        "${PROJECT_VERSION_MINOR}"
        "${PROJECT_VERSION_PATCH}"
        "$<TARGET_FILE:SatsumaHost>"
        "$<TARGET_FILE:SatsumaVM>"
)

add_executable(SatsumaVmciTransportTests vmci_transport_tests.cpp)
target_link_libraries(SatsumaVmciTransportTests PRIVATE SatsumaHostLib)
satsuma_enable_strict_warnings(SatsumaVmciTransportTests)
satsuma_treat_angle_headers_as_external(SatsumaVmciTransportTests)
add_test(NAME SatsumaVmciTransportTests COMMAND SatsumaVmciTransportTests)
set_tests_properties(SatsumaVmciTransportTests PROPERTIES TIMEOUT 10 RUN_SERIAL TRUE)

add_test(
    NAME SatsumaSchemaFilesTest
    COMMAND "${CMAKE_COMMAND}"
        "-DSCHEMA_ROOT=${CMAKE_SOURCE_DIR}/schemas"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/schema_files_test.cmake"
)

add_executable(SatsumaSchemaContractTests schema_contract_tests.cpp)
target_compile_features(SatsumaSchemaContractTests PRIVATE cxx_std_20)
target_include_directories(
    SatsumaSchemaContractTests
    PRIVATE "${jsoncons_SOURCE_DIR}/include"
)
satsuma_enable_test_warnings(SatsumaSchemaContractTests)

add_test(
    NAME SatsumaAgentSchemaContractTests
    COMMAND SatsumaSchemaContractTests
        "${CMAKE_SOURCE_DIR}/schemas/agent.schema.json"
        "${CMAKE_SOURCE_DIR}/config/agent.template.json"
        "${CMAKE_SOURCE_DIR}/examples/agent-vm-01.json"
)
add_test(
    NAME SatsumaLabSchemaContractTests
    COMMAND SatsumaSchemaContractTests
        "${CMAKE_SOURCE_DIR}/schemas/lab.schema.json"
        "${CMAKE_SOURCE_DIR}/config/lab.template.json"
)
add_test(
    NAME SatsumaTaskSchemaContractTests
    COMMAND SatsumaSchemaContractTests
        "${CMAKE_SOURCE_DIR}/schemas/task.schema.json"
        "${CMAKE_SOURCE_DIR}/examples/hello-vm-task.json"
        "${CMAKE_SOURCE_DIR}/examples/multi-vm-task.json"
        "${CMAKE_SOURCE_DIR}/examples/windows-script-task.json"
)

add_executable(
    SatsumaVmClaimStoreTests
    claim_store_tests.cpp
)
target_link_libraries(SatsumaVmClaimStoreTests PRIVATE SatsumaVmTestLib)
satsuma_enable_test_warnings(SatsumaVmClaimStoreTests)
add_test(NAME SatsumaVmClaimStoreTests COMMAND SatsumaVmClaimStoreTests)

add_executable(
    SatsumaVmClaimRenewalTests
    claim_renewal_tests.cpp
)
target_link_libraries(SatsumaVmClaimRenewalTests PRIVATE SatsumaVmTestLib)
satsuma_enable_test_warnings(SatsumaVmClaimRenewalTests)
add_test(NAME SatsumaVmClaimRenewalTests COMMAND SatsumaVmClaimRenewalTests)

add_executable(
    SatsumaHostUpdateTests
    host_update_tests.cpp
)
target_link_libraries(SatsumaHostUpdateTests PRIVATE SatsumaHostLib)
satsuma_enable_test_warnings(SatsumaHostUpdateTests)
add_test(NAME SatsumaHostUpdateTests COMMAND SatsumaHostUpdateTests)

add_executable(
    SatsumaHostControllerTests
    host_controller_tests.cpp
)
target_link_libraries(SatsumaHostControllerTests PRIVATE SatsumaHostLib)
satsuma_enable_test_warnings(SatsumaHostControllerTests)
add_test(NAME SatsumaHostControllerTests COMMAND SatsumaHostControllerTests)

add_test(
    NAME SatsumaSelfRenameTest
    COMMAND "${CMAKE_COMMAND}"
        "-DFIXTURE_EXE=$<TARGET_FILE:SatsumaTestFixture>"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/self-rename-test"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/self_rename_test.cmake"
)

add_executable(
    SatsumaVmInteractiveTests
    interactive_process_tests.cpp
)
target_compile_definitions(
    SatsumaVmInteractiveTests
    PRIVATE SATSUMA_INTERACTIVE_TESTS
)
target_link_libraries(
    SatsumaVmInteractiveTests
    PRIVATE SatsumaVmTestLib
)
satsuma_enable_test_warnings(SatsumaVmInteractiveTests)
add_test(
    NAME SatsumaVmInteractiveTests
    COMMAND SatsumaVmInteractiveTests
        "$<TARGET_FILE:SatsumaVM>"
        "$<TARGET_FILE:SatsumaTestFixture>"
)
set_tests_properties(
    SatsumaVmInteractiveTests
    PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE
)

add_executable(
    SatsumaVmAgentTests
    vm_agent_tests.cpp
)
target_compile_definitions(SatsumaVmAgentTests PRIVATE SATSUMA_INTERACTIVE_TESTS)
target_link_libraries(
    SatsumaVmAgentTests
    PRIVATE SatsumaVmTestLib
)
satsuma_enable_test_warnings(SatsumaVmAgentTests)
add_test(
    NAME SatsumaVmAgentTests
    COMMAND SatsumaVmAgentTests
        "$<TARGET_FILE:SatsumaTestFixture>"
        "$<TARGET_FILE:SatsumaVM>"
)
set_tests_properties(SatsumaVmAgentTests PROPERTIES TIMEOUT 25)

add_executable(
    SatsumaVmAgentClaimTests
    vm_agent_claim_tests.cpp
)
target_link_libraries(
    SatsumaVmAgentClaimTests
    PRIVATE SatsumaVmTestLib
)
satsuma_enable_test_warnings(SatsumaVmAgentClaimTests)
add_test(
    NAME SatsumaVmAgentClaimTests
    COMMAND SatsumaVmAgentClaimTests "$<TARGET_FILE:SatsumaTestFixture>"
)
set_tests_properties(SatsumaVmAgentClaimTests PROPERTIES TIMEOUT 60)

add_executable(
    SatsumaVmServiceTests
    service_tests.cpp
)
target_compile_definitions(SatsumaVmServiceTests PRIVATE SATSUMA_SERVICE_TESTS)
target_link_libraries(
    SatsumaVmServiceTests
    PRIVATE SatsumaVmTestLib
)
satsuma_enable_test_warnings(SatsumaVmServiceTests)
add_test(NAME SatsumaVmServiceTests COMMAND SatsumaVmServiceTests "$<TARGET_FILE:SatsumaVM>")
set_tests_properties(SatsumaVmServiceTests PROPERTIES TIMEOUT 10)

add_executable(
    SatsumaVmUpdateTests
    update_tests.cpp
)
target_compile_definitions(SatsumaVmUpdateTests PRIVATE SATSUMA_UPDATE_TESTS)
target_link_libraries(
    SatsumaVmUpdateTests
    PRIVATE SatsumaVmTestLib
)
satsuma_enable_test_warnings(SatsumaVmUpdateTests)
add_test(NAME SatsumaVmUpdateTests COMMAND SatsumaVmUpdateTests)
set_tests_properties(SatsumaVmUpdateTests PROPERTIES TIMEOUT 60)

find_program(SATSUMA_WINDOWS_POWERSHELL powershell.exe REQUIRED)
find_program(SATSUMA_POWERSHELL_CORE pwsh)
add_test(
    NAME SatsumaInstallAgentScriptTest
    COMMAND "${CMAKE_COMMAND}"
        "-DSCRIPT=${CMAKE_SOURCE_DIR}/scripts/install-agent.ps1"
        "-DPOWERSHELL=${SATSUMA_WINDOWS_POWERSHELL}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/install_agent_script_test.cmake"
)

if(SATSUMA_POWERSHELL_CORE)
    add_test(
        NAME SatsumaRealCrashRecoveryScriptTest
        COMMAND "${CMAKE_COMMAND}"
            "-DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/real_vmware_crash_recovery.ps1"
            "-DPOWERSHELL=${SATSUMA_POWERSHELL_CORE}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/real_vmware_crash_recovery_script_test.cmake"
    )
endif()

add_test(
    NAME SatsumaDemoAppTest
    COMMAND "${CMAKE_COMMAND}"
        "-DAPP=$<TARGET_FILE:SatsumaDemoApp>"
        "-DINPUT=${CMAKE_SOURCE_DIR}/examples/demo-input.txt"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/demo-app-test"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/demo_app_test.cmake"
)

add_executable(SatsumaFakeVmrun fake_vmrun.cpp)
target_compile_features(SatsumaFakeVmrun PRIVATE cxx_std_20)
set_target_properties(SatsumaFakeVmrun PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_executable(SatsumaMultiVmFakeVmrun multi_vm_fake_vmrun.cpp)
target_compile_features(SatsumaMultiVmFakeVmrun PRIVATE cxx_std_20)
set_target_properties(SatsumaMultiVmFakeVmrun PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_executable(
    SatsumaVmrunProviderTests
    vmrun_provider_tests.cpp
)
target_link_libraries(SatsumaVmrunProviderTests PRIVATE SatsumaHostLib)
satsuma_enable_strict_warnings(SatsumaFakeVmrun)
satsuma_enable_strict_warnings(SatsumaMultiVmFakeVmrun)
satsuma_enable_strict_warnings(SatsumaVmrunProviderTests)
add_test(
    NAME SatsumaVmrunProviderTests
    COMMAND SatsumaVmrunProviderTests "$<TARGET_FILE:SatsumaFakeVmrun>"
)

add_test(
    NAME SatsumaHostVmFlow
    COMMAND "${CMAKE_COMMAND}"
        "-DHOST_EXE=$<TARGET_FILE:SatsumaHost>"
        "-DVM_EXE=$<TARGET_FILE:SatsumaVMTestAgent>"
        "-DFIXTURE_EXE=$<TARGET_FILE:SatsumaTestFixture>"
        "-DVMRUN_EXE=$<TARGET_FILE:SatsumaFakeVmrun>"
        "-DEXPECTED_VERSION=${PROJECT_VERSION}"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/host-vm-flow"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/host_vm_flow.cmake"
)

add_test(
    NAME SatsumaHostMultiVmLifecycleFlow
    COMMAND "${CMAKE_COMMAND}"
        "-DHOST_EXE=$<TARGET_FILE:SatsumaHost>"
        "-DVM_EXE=$<TARGET_FILE:SatsumaVMTestAgent>"
        "-DFIXTURE_EXE=$<TARGET_FILE:SatsumaTestFixture>"
        "-DVMRUN_EXE=$<TARGET_FILE:SatsumaMultiVmFakeVmrun>"
        # Agent 工作目录嵌套较深，保持短根目录以兼容未启用长路径的 Windows。
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/mvl"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/multi_vm_lifecycle_flow.cmake"
)
set_tests_properties(SatsumaHostMultiVmLifecycleFlow PROPERTIES TIMEOUT 420)

if(SATSUMA_ENABLE_REAL_VMWARE_SMOKE)
    add_custom_target(
        SatsumaRealVmwareSmoke
        COMMAND "${CMAKE_COMMAND}"
            "-DHOST_EXE=$<TARGET_FILE:SatsumaHost>"
            "-DLAB_CONFIG=${SATSUMA_REAL_LAB_CONFIG}"
            "-DVM_ID=${SATSUMA_REAL_VM_ID}"
            "-DSNAPSHOT=${SATSUMA_REAL_SNAPSHOT}"
            "-DCONFIRM=${SATSUMA_REAL_VMWARE_CONFIRM}"
            "-DAGENT_BINARY=$<TARGET_FILE:SatsumaVM>"
            "-DITERATIONS=${SATSUMA_REAL_VMWARE_SMOKE_ITERATIONS}"
            "-DCHECK_TIMEOUT_SECONDS=${SATSUMA_REAL_VMWARE_SMOKE_TIMEOUT_SECONDS}"
            "-DUPDATE_TIMEOUT_SECONDS=${SATSUMA_REAL_VMWARE_SMOKE_UPDATE_TIMEOUT_SECONDS}"
            "-DOUTPUT_ROOT=${CMAKE_CURRENT_BINARY_DIR}/real-vmware-smoke"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/real_vmware_smoke.cmake"
        DEPENDS SatsumaHost SatsumaVM
        USES_TERMINAL
        COMMENT "Run explicitly confirmed real VMware lifecycle smoke"
    )
endif()

if(SATSUMA_ENABLE_REAL_VMWARE_CRASH_RECOVERY)
    if(NOT SATSUMA_POWERSHELL_CORE)
        message(FATAL_ERROR "PowerShell 7 is required for real VMware crash recovery")
    endif()
    add_custom_target(
        SatsumaRealVmwareCrashRecovery
        COMMAND "${SATSUMA_POWERSHELL_CORE}"
            -NoProfile
            -NonInteractive
            -File "${CMAKE_CURRENT_SOURCE_DIR}/real_vmware_crash_recovery.ps1"
            -HostExe "$<TARGET_FILE:SatsumaHost>"
            -FixtureExe "$<TARGET_FILE:SatsumaTestFixture>"
            -LabConfig "${SATSUMA_REAL_LAB_CONFIG}"
            -VmId "${SATSUMA_REAL_VM_ID}"
            -Scenario "${SATSUMA_REAL_CRASH_SCENARIO}"
            -Confirm "${SATSUMA_REAL_CRASH_CONFIRM}"
            -ScenarioTimeoutSeconds "${SATSUMA_REAL_CRASH_TIMEOUT_SECONDS}"
        DEPENDS SatsumaHost SatsumaTestFixture
        USES_TERMINAL
        COMMENT "Run explicitly confirmed real VMware Host/Agent crash recovery"
    )
endif()
