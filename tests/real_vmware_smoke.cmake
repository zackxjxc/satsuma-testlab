# 对用户指定的真实 VM 执行破坏性生命周期验收。
if(NOT CONFIRM STREQUAL "I_UNDERSTAND_VM_WILL_BE_RESET")
    message(FATAL_ERROR "Set the exact real VMware confirmation token before running this target")
endif()
if(NOT DEFINED HOST_EXE OR NOT EXISTS "${HOST_EXE}")
    message(FATAL_ERROR "SatsumaHost executable is missing")
endif()
if(NOT DEFINED LAB_CONFIG OR NOT EXISTS "${LAB_CONFIG}")
    message(FATAL_ERROR "A real lab.json path is required")
endif()
if(NOT DEFINED VM_ID OR VM_ID STREQUAL "" OR NOT DEFINED SNAPSHOT OR SNAPSHOT STREQUAL "")
    message(FATAL_ERROR "A real VM ID and snapshot name are required")
endif()

# 执行单个 Host 命令并保留完整诊断。
function(run_host_step label)
    execute_process(
        COMMAND "${HOST_EXE}" ${ARGN} --config "${LAB_CONFIG}"
        RESULT_VARIABLE step_result
        OUTPUT_VARIABLE step_output
        ERROR_VARIABLE step_error
    )
    if(NOT step_result EQUAL 0)
        message(FATAL_ERROR "${label} failed: ${step_error}\n${step_output}")
    endif()
    message(STATUS "${label} passed")
endfunction()

run_host_step("Snapshot list" snapshot list --vm "${VM_ID}")
run_host_step("Hard stop" vm stop --id "${VM_ID}" --mode hard)
run_host_step("Snapshot restore" vm restore --id "${VM_ID}" --snapshot "${SNAPSHOT}")
run_host_step("VM start" vm start --id "${VM_ID}")

message(STATUS "Real VMware lifecycle smoke completed for ${VM_ID}")
