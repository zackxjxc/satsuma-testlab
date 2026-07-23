# Satsuma 核心单元测试和 Host/VM 集成测试。
add_executable(SatsumaCoreTests core_tests.cpp)
target_link_libraries(SatsumaCoreTests PRIVATE SatsumaCore ${SATSUMA_YLT_TARGET})

add_executable(SatsumaTestFixture fixture.cpp)
target_compile_features(SatsumaTestFixture PRIVATE cxx_std_20)
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

add_executable(
    SatsumaRpcServerTests
    rpc_server_tests.cpp
    ../src/host/rpc_server.cpp
    ../src/host/rpc_service.cpp
)
target_include_directories(SatsumaRpcServerTests PRIVATE ../src/host)
target_link_libraries(SatsumaRpcServerTests PRIVATE SatsumaCore ${SATSUMA_YLT_TARGET})
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
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/host-vm-flow"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/host_vm_flow.cmake"
)
