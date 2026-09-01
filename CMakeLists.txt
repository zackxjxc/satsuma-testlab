# Satsuma TestLab 的顶层构建配置。
cmake_minimum_required(VERSION 3.25)

project(SatsumaTestLab VERSION 0.2.1 LANGUAGES CXX)

include(GNUInstallDirs)

# 统一项目目标的 MSVC 严格警告和外部头文件降噪策略。
function(satsuma_enable_strict_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHsc)
    endif()
endfunction()

function(satsuma_treat_angle_headers_as_external target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /external:anglebrackets /external:W0)
    endif()
endfunction()

if(NOT WIN32)
    message(FATAL_ERROR "Satsuma 0.2.1 currently supports Windows only")
endif()

option(SATSUMA_BUILD_TESTS "Build Satsuma tests" ON)
option(SATSUMA_ENABLE_REAL_VMWARE_SMOKE "Expose the destructive real VMware smoke target" OFF)
option(
    SATSUMA_ENABLE_REAL_VMWARE_CRASH_RECOVERY
    "Expose the explicitly confirmed real Host/Agent crash recovery target"
    OFF
)
set(SATSUMA_REAL_LAB_CONFIG "" CACHE FILEPATH "lab.json used by real VMware targets")
set(SATSUMA_REAL_VM_ID "" CACHE STRING "VM ID used by real VMware targets")
set(SATSUMA_REAL_SNAPSHOT "" CACHE STRING "Snapshot restored by the real VMware smoke target")
set(SATSUMA_REAL_VMWARE_CONFIRM "" CACHE STRING "Explicit confirmation for destructive VMware smoke")
set(
    SATSUMA_REAL_VMWARE_SMOKE_ITERATIONS
    3
    CACHE STRING
    "Restore/start/check iterations for the real VMware smoke target"
)
set(
    SATSUMA_REAL_VMWARE_SMOKE_TIMEOUT_SECONDS
    180
    CACHE STRING
    "Agent readiness timeout for each real VMware smoke iteration"
)
set(
    SATSUMA_REAL_VMWARE_SMOKE_UPDATE_TIMEOUT_SECONDS
    300
    CACHE STRING
    "Agent update timeout for each real VMware smoke iteration"
)
set(SATSUMA_REAL_CRASH_SCENARIO "All" CACHE STRING "Real crash recovery scenario")
set_property(
    CACHE SATSUMA_REAL_CRASH_SCENARIO
    PROPERTY STRINGS All HostCrash AgentRetrySafe AgentUnsafe
)
set(
    SATSUMA_REAL_CRASH_TIMEOUT_SECONDS
    480
    CACHE STRING
    "Per-process timeout for real crash recovery"
)
set(
    SATSUMA_REAL_CRASH_CONFIRM
    ""
    CACHE STRING
    "Explicit confirmation for Host and Agent forced termination"
)

if(MSVC)
    # 使用静态 MSVC Runtime，避免 Guest 依赖与构建机完全一致的 Redistributable。
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(FetchContent)

# VMCI transport 使用固定版本的 libzmq/cppzmq；VMware 公共 vSockets ABI
# 头文件直接来自固定提交的 open-vm-tools，不在仓库中复制第三方源码。
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
set(BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC ON CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_PERF_TOOL OFF CACHE BOOL "" FORCE)
set(ENABLE_DRAFTS OFF CACHE BOOL "" FORCE)
set(ENABLE_CURVE OFF CACHE BOOL "" FORCE)
set(WITH_LIBSODIUM OFF CACHE BOOL "" FORCE)
set(WITH_LIBBSD OFF CACHE BOOL "" FORCE)
set(ENABLE_CPACK OFF CACHE BOOL "" FORCE)
set(CPPZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_VMCI ON CACHE BOOL "" FORCE)
set(POLLER select CACHE STRING "" FORCE)
set(ZMQ_HAVE_IPC OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    open_vm_tools
    GIT_REPOSITORY https://github.com/vmware/open-vm-tools.git
    GIT_TAG 3c28b6f4d9af2df2027a9df9c82e2f0196bd8bf7
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(open_vm_tools)
set(
    VMCI_INCLUDE_DIRS
    "${open_vm_tools_SOURCE_DIR}/open-vm-tools/lib/include"
    CACHE PATH "" FORCE
)

FetchContent_Declare(
    libzmq
    GIT_REPOSITORY https://github.com/zeromq/libzmq.git
    GIT_TAG 4d9c8f8ccf4f659e8397cad42f8496551f534597
    GIT_SHALLOW TRUE
    PATCH_COMMAND
        "${CMAKE_COMMAND}" -DLIBZMQ_SOURCE_DIR=<SOURCE_DIR>
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch-libzmq-vmci.cmake"
)
FetchContent_Declare(
    cppzmq
    GIT_REPOSITORY https://github.com/zeromq/cppzmq.git
    GIT_TAG c94c20743ed7d4aa37835a5c46567ab0790d4acc
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(libzmq cppzmq)

# 优先复用精确版本的包管理器依赖；未提供时下载固定提交。
find_package(nlohmann_json 3.12.0 EXACT CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG 55f93686c01528224f448c19128836e7df245f72
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

add_library(SatsumaCore STATIC
    src/core/claim.cpp
    src/core/claim_store.cpp
    src/core/config.cpp
    src/core/hardware_identity.cpp
    src/core/id.cpp
    src/core/json_contract.cpp
    src/core/json_io.cpp
    src/core/lifecycle.cpp
    src/core/path.cpp
    src/core/sha256.cpp
    src/core/snapshot.cpp
    src/core/task.cpp
    src/core/update.cpp
    src/core/windows_command_line.cpp
)

target_include_directories(SatsumaCore PUBLIC include)
target_compile_features(SatsumaCore PUBLIC cxx_std_20)
target_compile_definitions(
    SatsumaCore
    PUBLIC
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00
        WINVER=0x0A00
        SATSUMA_VERSION="${PROJECT_VERSION}"
)
target_link_libraries(SatsumaCore PUBLIC nlohmann_json::nlohmann_json PRIVATE bcrypt)

add_library(SatsumaVmci STATIC
    src/transport/vmci.cpp
)
target_include_directories(SatsumaVmci PUBLIC include PRIVATE "${VMCI_INCLUDE_DIRS}")
target_compile_features(SatsumaVmci PUBLIC cxx_std_20)
target_compile_definitions(
    SatsumaVmci
    PUBLIC ZMQ_STATIC NOMINMAX WIN32_LEAN_AND_MEAN
)
target_link_libraries(SatsumaVmci PUBLIC SatsumaCore cppzmq-static)

add_library(SatsumaHostLib STATIC
    src/host/cli.cpp
    src/host/controller.cpp
    src/host/diagnostics.cpp
    src/host/gateway.cpp
    src/host/identity.cpp
    src/host/lab_lease.cpp
    src/host/orchestrator.cpp
    src/vmware/vmrun_provider.cpp
)
target_include_directories(SatsumaHostLib PUBLIC src/host src/vmware)
target_link_libraries(SatsumaHostLib PUBLIC SatsumaCore SatsumaVmci)

add_executable(SatsumaHost src/host/main.cpp)
target_link_libraries(SatsumaHost PRIVATE SatsumaHostLib)

set(SATSUMA_VM_LIBRARY_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vm/agent.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vm/claim_renewal.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vm/hardware_identity.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vm/inventory.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vm/interactive_process.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vm/process_runner.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vm/process_environment.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vm/service.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vm/update.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vm/vmci_channel.cpp"
)
add_library(SatsumaVmLib STATIC ${SATSUMA_VM_LIBRARY_SOURCES})
target_include_directories(SatsumaVmLib PUBLIC src/vm)
target_link_libraries(
    SatsumaVmLib
    PUBLIC
        SatsumaCore
        SatsumaVmci
        advapi32
        shell32
        userenv
        version
        wtsapi32
)

add_executable(SatsumaVM src/vm/main.cpp)
target_link_libraries(SatsumaVM PRIVATE SatsumaVmLib)

set_target_properties(SatsumaHost SatsumaVM PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# 独立示例软件不链接 SatsumaCore，用于真实 Artifact 执行验收。
add_executable(SatsumaDemoApp examples/demo_app.cpp)
target_compile_features(SatsumaDemoApp PRIVATE cxx_std_20)
target_link_libraries(SatsumaDemoApp PRIVATE nlohmann_json::nlohmann_json)
set_target_properties(SatsumaDemoApp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# 为每个构建配置生成包含真实绝对 Artifact 路径的任务文件。
set(SATSUMA_DEMO_INPUT "${CMAKE_CURRENT_SOURCE_DIR}/examples/demo-input.txt")
set(SATSUMA_DEMO_TASK [=[
{
  "schema_version": 3,
  "name": "demo-app-real-vm",
  "artifacts": [
    {
      "source": "$<TARGET_FILE:SatsumaDemoApp>",
      "vm": "vm_01",
      "destination": "artifacts/vm_01/SatsumaDemoApp.exe"
    },
    {
      "source": "@DEMO_INPUT@",
      "vm": "vm_01",
      "destination": "artifacts/vm_01/demo-input.txt"
    }
  ],
  "steps": [
    {
      "id": "run_demo_app",
      "vm": "vm_01",
      "type": "execute",
      "program": "artifacts/vm_01/SatsumaDemoApp.exe",
      "arguments": [
        "--input", "artifacts/vm_01/demo-input.txt",
        "--result", "output/demo-result.json",
        "--transformed", "output/transformed.txt",
        "--label", "real-vm-artifact-test",
        "--emit-warning"
      ],
      "timeout_seconds": 30,
      "collect_files": ["output/demo-result.json", "output/transformed.txt"]
    }
  ]
}
]=])
string(REPLACE "@DEMO_INPUT@" "${SATSUMA_DEMO_INPUT}" SATSUMA_DEMO_TASK "${SATSUMA_DEMO_TASK}")
file(GENERATE
    OUTPUT "${CMAKE_BINARY_DIR}/examples/demo-app-task-$<CONFIG>.json"
    CONTENT "${SATSUMA_DEMO_TASK}"
)

foreach(target SatsumaCore SatsumaVmci SatsumaHostLib SatsumaHost SatsumaVmLib SatsumaVM SatsumaDemoApp)
    satsuma_enable_strict_warnings(${target})
endforeach()
satsuma_treat_angle_headers_as_external(SatsumaHostLib)
satsuma_treat_angle_headers_as_external(SatsumaVmLib)

if(SATSUMA_BUILD_TESTS)
    # 测试阶段使用完整 Draft 2020-12 实现验证公开 Schema 和示例；不进入生产二进制。
    set(JSONCONS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        jsoncons
        GIT_REPOSITORY https://github.com/danielaparker/jsoncons.git
        GIT_TAG v1.7.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(jsoncons)

    enable_testing()
    add_subdirectory(tests)
endif()

install(TARGETS SatsumaHost SatsumaVM RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(
    FILES
        config/lab.template.json
        config/agent.template.json
    DESTINATION config
)
install(
    FILES
        examples/hello-vm-task.json
        examples/multi-vm-task.json
        examples/windows-script-task.json
    DESTINATION examples
)
install(FILES scripts/install-agent.ps1 DESTINATION scripts)
install(DIRECTORY schemas/ DESTINATION schemas FILES_MATCHING PATTERN "*.json")
install(
    FILES
        docs/AI操作契约.md
        docs/首次配置.md
        docs/架构.md
        docs/用户指南.md
        docs/协议.md
        docs/开发指南.md
    DESTINATION docs
)
install(
    FILES
        README.md
        更新日志.md
        贡献指南.md
        安全策略.md
        第三方声明.md
    DESTINATION .
)

add_custom_target(
    SatsumaPackage
    COMMAND
        powershell.exe
        -NoProfile
        -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/scripts/package-release.ps1"
        -BuildDirectory "${CMAKE_BINARY_DIR}"
        -OutputDirectory "${CMAKE_SOURCE_DIR}/output"
        -Configuration "$<CONFIG>"
        -Version "${PROJECT_VERSION}"
    DEPENDS SatsumaHost SatsumaVM
    USES_TERMINAL
    VERBATIM
)
