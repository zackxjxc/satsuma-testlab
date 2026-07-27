# Satsuma 核心单元测试和 Host/VM 集成测试。
add_executable(SatsumaCoreTests core_tests.cpp)
target_link_libraries(SatsumaCoreTests PRIVATE SatsumaCore ${SATSUMA_YLT_TARGET})

add_executable(SatsumaTestFixture fixture.cpp)
target_compile_features(SatsumaTestFixture PRIVATE cxx_std_20)
target_link_libraries(SatsumaTestFixture PRIVATE SatsumaCore advapi32)
set_target_properties(SatsumaTestFixture PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(MSVC)
    target_compile_options(
        SatsumaCoreTests
        PRIVATE /W4 /permissive- /utf-8 /EHsc /external:anglebrackets /external:W0
    )
    target_compile_options(SatsumaTestFixture PRIVATE /W4 /permissive- /utf-8 /EHsc)
endif()

add_test(NAME SatsumaCoreTests COMMAND SatsumaCoreTests)

add_executable(
    SatsumaVmClaimStoreTests
    claim_store_tests.cpp
    ../src/vm/claim_store.cpp
)
target_include_directories(SatsumaVmClaimStoreTests PRIVATE ../src/vm)
target_link_libraries(SatsumaVmClaimStoreTests PRIVATE SatsumaCore)
if(MSVC)
    target_compile_options(
        SatsumaVmClaimStoreTests
        PRIVATE /W4 /permissive- /utf-8 /EHsc /external:anglebrackets /external:W0
    )
endif()
add_test(NAME SatsumaVmClaimStoreTests COMMAND SatsumaVmClaimStoreTests)

add_executable(
    SatsumaVmClaimRenewalTests
    claim_renewal_tests.cpp
    ../src/vm/claim_renewal.cpp
    ../src/vm/claim_store.cpp
)
target_include_directories(SatsumaVmClaimRenewalTests PRIVATE ../src/vm)
target_link_libraries(SatsumaVmClaimRenewalTests PRIVATE SatsumaCore)
if(MSVC)
    target_compile_options(
        SatsumaVmClaimRenewalTests
        PRIVATE /W4 /permissive- /utf-8 /EHsc /external:anglebrackets /external:W0
    )
endif()
add_test(NAME SatsumaVmClaimRenewalTests COMMAND SatsumaVmClaimRenewalTests)

add_executable(
    SatsumaHostUpdateTests
    host_update_tests.cpp
    ../src/host/controller.cpp
)
target_include_directories(SatsumaHostUpdateTests PRIVATE ../src/host)
target_link_libraries(SatsumaHostUpdateTests PRIVATE SatsumaCore)
if(MSVC)
    target_compile_options(
        SatsumaHostUpdateTests
        PRIVATE /W4 /permissive- /utf-8 /EHsc /external:anglebrackets /external:W0
    )
endif()
add_test(NAME SatsumaHostUpdateTests COMMAND SatsumaHostUpdateTests)

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
    ../src/vm/interactive_process.cpp
)
target_include_directories(SatsumaVmInteractiveTests PRIVATE ../src/vm)
target_compile_definitions(
    SatsumaVmInteractiveTests
    PRIVATE SATSUMA_INTERACTIVE_TESTS
)
target_link_libraries(
    SatsumaVmInteractiveTests
    PRIVATE SatsumaCore advapi32 userenv wtsapi32
)
if(MSVC)
    target_compile_options(
        SatsumaVmInteractiveTests
        PRIVATE /W4 /permissive- /utf-8 /EHsc /external:anglebrackets /external:W0
    )
endif()
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
    SatsumaVmAutostartTests
    autostart_tests.cpp
    ../src/vm/autostart.cpp
)
target_include_directories(SatsumaVmAutostartTests PRIVATE ../src/vm)
target_compile_definitions(SatsumaVmAutostartTests PRIVATE SATSUMA_AUTOSTART_TESTS)
target_link_libraries(SatsumaVmAutostartTests PRIVATE SatsumaCore advapi32 ole32 oleaut32 taskschd)
if(MSVC)
    target_compile_options(SatsumaVmAutostartTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
endif()
add_test(NAME SatsumaVmAutostartTests COMMAND SatsumaVmAutostartTests)

add_executable(
    SatsumaVmAgentTests
    vm_agent_tests.cpp
    ../src/vm/agent.cpp
    ../src/vm/claim_renewal.cpp
    ../src/vm/claim_store.cpp
    ../src/vm/interactive_process.cpp
    ../src/vm/process_runner.cpp
    ../src/vm/rpc_client.cpp
    ../src/vm/service.cpp
    ../src/vm/update.cpp
)
target_include_directories(SatsumaVmAgentTests PRIVATE ../src/vm)
target_compile_definitions(SatsumaVmAgentTests PRIVATE SATSUMA_INTERACTIVE_TESTS)
target_link_libraries(
    SatsumaVmAgentTests
    PRIVATE SatsumaCore ${SATSUMA_YLT_TARGET} advapi32 userenv ws2_32 wtsapi32
)
if(MSVC)
    target_compile_options(
        SatsumaVmAgentTests
        PRIVATE /W4 /permissive- /utf-8 /EHsc /external:anglebrackets /external:W0
    )
endif()
add_test(
    NAME SatsumaVmAgentTests
    COMMAND SatsumaVmAgentTests
        "$<TARGET_FILE:SatsumaTestFixture>"
        "$<TARGET_FILE:SatsumaVM>"
)
set_tests_properties(SatsumaVmAgentTests PROPERTIES TIMEOUT 15)

add_executable(
    SatsumaVmAgentClaimTests
    vm_agent_claim_tests.cpp
    ../src/vm/agent.cpp
    ../src/vm/claim_renewal.cpp
    ../src/vm/claim_store.cpp
    ../src/vm/interactive_process.cpp
    ../src/vm/process_runner.cpp
    ../src/vm/rpc_client.cpp
    ../src/vm/service.cpp
    ../src/vm/update.cpp
)
target_include_directories(SatsumaVmAgentClaimTests PRIVATE ../src/vm)
target_link_libraries(
    SatsumaVmAgentClaimTests
    PRIVATE SatsumaCore ${SATSUMA_YLT_TARGET} advapi32 userenv ws2_32 wtsapi32
)
if(MSVC)
    target_compile_options(
        SatsumaVmAgentClaimTests
        PRIVATE /W4 /permissive- /utf-8 /EHsc /external:anglebrackets /external:W0
    )
endif()
add_test(
    NAME SatsumaVmAgentClaimTests
    COMMAND SatsumaVmAgentClaimTests "$<TARGET_FILE:SatsumaTestFixture>"
)
set_tests_properties(SatsumaVmAgentClaimTests PROPERTIES TIMEOUT 20)

add_executable(
    SatsumaVmServiceTests
    service_tests.cpp
    ../src/vm/agent.cpp
    ../src/vm/claim_renewal.cpp
    ../src/vm/claim_store.cpp
    ../src/vm/interactive_process.cpp
    ../src/vm/process_runner.cpp
    ../src/vm/rpc_client.cpp
    ../src/vm/service.cpp
    ../src/vm/update.cpp
)
target_include_directories(SatsumaVmServiceTests PRIVATE ../src/vm)
target_compile_definitions(SatsumaVmServiceTests PRIVATE SATSUMA_SERVICE_TESTS)
target_link_libraries(
    SatsumaVmServiceTests
    PRIVATE SatsumaCore ${SATSUMA_YLT_TARGET} advapi32 userenv ws2_32 wtsapi32
)
if(MSVC)
    target_compile_options(
        SatsumaVmServiceTests
        PRIVATE /W4 /permissive- /utf-8 /EHsc /external:anglebrackets /external:W0
    )
endif()
add_test(NAME SatsumaVmServiceTests COMMAND SatsumaVmServiceTests "$<TARGET_FILE:SatsumaVM>")
set_tests_properties(SatsumaVmServiceTests PROPERTIES TIMEOUT 10)

add_executable(
    SatsumaVmUpdateTests
    update_tests.cpp
    ../src/vm/agent.cpp
    ../src/vm/claim_renewal.cpp
    ../src/vm/claim_store.cpp
    ../src/vm/interactive_process.cpp
    ../src/vm/process_runner.cpp
    ../src/vm/rpc_client.cpp
    ../src/vm/service.cpp
    ../src/vm/update.cpp
)
target_include_directories(SatsumaVmUpdateTests PRIVATE ../src/vm)
target_compile_definitions(SatsumaVmUpdateTests PRIVATE SATSUMA_UPDATE_TESTS)
target_link_libraries(
    SatsumaVmUpdateTests
    PRIVATE SatsumaCore ${SATSUMA_YLT_TARGET} advapi32 userenv ws2_32 wtsapi32
)
if(MSVC)
    target_compile_options(
        SatsumaVmUpdateTests
        PRIVATE /W4 /permissive- /utf-8 /EHsc /external:anglebrackets /external:W0
    )
endif()
add_test(NAME SatsumaVmUpdateTests COMMAND SatsumaVmUpdateTests)
set_tests_properties(SatsumaVmUpdateTests PROPERTIES TIMEOUT 15)

find_program(SATSUMA_WINDOWS_POWERSHELL powershell.exe REQUIRED)
add_test(
    NAME SatsumaInstallAgentScriptTest
    COMMAND "${CMAKE_COMMAND}"
        "-DSCRIPT=${CMAKE_SOURCE_DIR}/scripts/install-agent.ps1"
        "-DPOWERSHELL=${SATSUMA_WINDOWS_POWERSHELL}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/install_agent_script_test.cmake"
)

add_test(
    NAME SatsumaDemoAppTest
    COMMAND "${CMAKE_COMMAND}"
        "-DAPP=$<TARGET_FILE:SatsumaDemoApp>"
        "-DINPUT=${CMAKE_SOURCE_DIR}/examples/demo-input.txt"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/demo-app-test"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/demo_app_test.cmake"
)

add_executable(
    SatsumaHostRpcServiceTests
    host_rpc_service_tests.cpp
    ../src/host/rpc_service.cpp
)
target_include_directories(SatsumaHostRpcServiceTests PRIVATE ../src/host)
target_link_libraries(SatsumaHostRpcServiceTests PRIVATE SatsumaCore)
if(MSVC)
    target_compile_options(SatsumaHostRpcServiceTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
endif()
add_test(NAME SatsumaHostRpcServiceTests COMMAND SatsumaHostRpcServiceTests)

add_executable(SatsumaFakeVmrun fake_vmrun.cpp)
target_compile_features(SatsumaFakeVmrun PRIVATE cxx_std_20)
set_target_properties(SatsumaFakeVmrun PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_executable(
    SatsumaVmrunProviderTests
    vmrun_provider_tests.cpp
    ../src/vmware/vmrun_provider.cpp
)
target_include_directories(SatsumaVmrunProviderTests PRIVATE ../src/vmware)
target_link_libraries(SatsumaVmrunProviderTests PRIVATE SatsumaCore)
if(MSVC)
    target_compile_options(SatsumaFakeVmrun PRIVATE /W4 /permissive- /utf-8 /EHsc)
    target_compile_options(SatsumaVmrunProviderTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
endif()
add_test(
    NAME SatsumaVmrunProviderTests
    COMMAND SatsumaVmrunProviderTests "$<TARGET_FILE:SatsumaFakeVmrun>"
)

add_executable(
    SatsumaRpcServerTests
    rpc_server_tests.cpp
    ../src/host/rpc_server.cpp
    ../src/host/rpc_service.cpp
    ../src/vm/agent.cpp
    ../src/vm/claim_renewal.cpp
    ../src/vm/claim_store.cpp
    ../src/vm/interactive_process.cpp
    ../src/vm/process_runner.cpp
    ../src/vm/rpc_client.cpp
    ../src/vm/service.cpp
    ../src/vm/update.cpp
)
target_include_directories(SatsumaRpcServerTests PRIVATE ../src/host ../src/vm)
target_link_libraries(SatsumaRpcServerTests PRIVATE SatsumaCore ${SATSUMA_YLT_TARGET})
target_link_libraries(
    SatsumaRpcServerTests
    PRIVATE advapi32 userenv ws2_32 wtsapi32
)
if(MSVC)
    target_compile_options(
        SatsumaRpcServerTests
        PRIVATE /W4 /permissive- /utf-8 /EHsc /external:anglebrackets /external:W0
    )
endif()
add_test(NAME SatsumaRpcServerTests COMMAND SatsumaRpcServerTests)
set_tests_properties(SatsumaRpcServerTests PROPERTIES TIMEOUT 10)

add_test(
    NAME SatsumaHostVmFlow
    COMMAND "${CMAKE_COMMAND}"
        "-DHOST_EXE=$<TARGET_FILE:SatsumaHost>"
        "-DVM_EXE=$<TARGET_FILE:SatsumaVM>"
        "-DFIXTURE_EXE=$<TARGET_FILE:SatsumaTestFixture>"
        "-DVMRUN_EXE=$<TARGET_FILE:SatsumaFakeVmrun>"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/host-vm-flow"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/host_vm_flow.cmake"
)

if(SATSUMA_ENABLE_REAL_VMWARE_SMOKE)
    add_custom_target(
        SatsumaRealVmwareSmoke
        COMMAND "${CMAKE_COMMAND}"
            "-DHOST_EXE=$<TARGET_FILE:SatsumaHost>"
            "-DLAB_CONFIG=${SATSUMA_REAL_LAB_CONFIG}"
            "-DVM_ID=${SATSUMA_REAL_VM_ID}"
            "-DSNAPSHOT=${SATSUMA_REAL_SNAPSHOT}"
            "-DCONFIRM=${SATSUMA_REAL_VMWARE_CONFIRM}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/real_vmware_smoke.cmake"
        DEPENDS SatsumaHost
        USES_TERMINAL
        COMMENT "Run explicitly confirmed real VMware lifecycle smoke"
    )
endif()
