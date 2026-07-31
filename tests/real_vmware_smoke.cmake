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
if(DEFINED AGENT_BINARY AND NOT EXISTS "${AGENT_BINARY}")
    message(FATAL_ERROR "The real Agent update binary is missing")
endif()
if(DEFINED AGENT_BINARY)
    execute_process(
        COMMAND "${AGENT_BINARY}" --version
        RESULT_VARIABLE agent_version_result
        OUTPUT_VARIABLE agent_version
        ERROR_VARIABLE agent_version_error
    )
    string(STRIP "${agent_version}" agent_version)
    if(NOT agent_version_result STREQUAL "0" OR agent_version STREQUAL "")
        message(FATAL_ERROR
            "Cannot read the real Agent candidate version: ${agent_version_error}")
    endif()
endif()
if(NOT DEFINED ITERATIONS)
    set(ITERATIONS 1)
endif()
if(NOT ITERATIONS MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "ITERATIONS must be a positive integer")
endif()
if(NOT DEFINED CHECK_TIMEOUT_SECONDS)
    set(CHECK_TIMEOUT_SECONDS 180)
endif()
if(NOT CHECK_TIMEOUT_SECONDS MATCHES "^[1-9][0-9]*$" OR CHECK_TIMEOUT_SECONDS GREATER 300)
    message(FATAL_ERROR "CHECK_TIMEOUT_SECONDS must be an integer from 1 to 300")
endif()
if(NOT DEFINED UPDATE_TIMEOUT_SECONDS)
    set(UPDATE_TIMEOUT_SECONDS 300)
endif()
if(NOT UPDATE_TIMEOUT_SECONDS MATCHES "^[1-9][0-9]*$" OR
   UPDATE_TIMEOUT_SECONDS GREATER 3600)
    message(FATAL_ERROR "UPDATE_TIMEOUT_SECONDS must be an integer from 1 to 3600")
endif()
if(NOT DEFINED OUTPUT_ROOT OR OUTPUT_ROOT STREQUAL "")
    set(OUTPUT_ROOT "${CMAKE_CURRENT_BINARY_DIR}/real-vmware-smoke")
endif()

file(MAKE_DIRECTORY "${OUTPUT_ROOT}")
if(UPDATE_TIMEOUT_SECONDS GREATER CHECK_TIMEOUT_SECONDS)
    set(command_timeout_seconds "${UPDATE_TIMEOUT_SECONDS}")
else()
    set(command_timeout_seconds "${CHECK_TIMEOUT_SECONDS}")
endif()
math(EXPR command_timeout_seconds "${command_timeout_seconds} + 30")
set(smoke_failed FALSE)
set(smoke_failures "")

# 执行单个 Host 命令，记录输出、耗时和失败上下文。
function(run_host_step label log_name result_output)
    string(TIMESTAMP started_epoch "%s" UTC)
    string(TIMESTAMP started_at "%Y-%m-%dT%H:%M:%SZ" UTC)
    execute_process(
        COMMAND "${HOST_EXE}" ${ARGN} --config "${LAB_CONFIG}"
        TIMEOUT "${command_timeout_seconds}"
        RESULT_VARIABLE step_result
        OUTPUT_VARIABLE step_output
        ERROR_VARIABLE step_error
    )
    string(TIMESTAMP finished_epoch "%s" UTC)
    string(TIMESTAMP finished_at "%Y-%m-%dT%H:%M:%SZ" UTC)
    math(EXPR elapsed_seconds "${finished_epoch} - ${started_epoch}")
    file(WRITE "${OUTPUT_ROOT}/${log_name}.log"
        "label=${label}\n"
        "started_at=${started_at}\n"
        "finished_at=${finished_at}\n"
        "elapsed_seconds=${elapsed_seconds}\n"
        "result=${step_result}\n"
        "--- stdout ---\n${step_output}\n"
        "--- stderr ---\n${step_error}\n")
    if(NOT step_result STREQUAL "0")
        set(updated_failures "${smoke_failures}")
        list(APPEND updated_failures "${label}: ${step_result}")
        set(smoke_failed TRUE PARENT_SCOPE)
        set(smoke_failures "${updated_failures}" PARENT_SCOPE)
        set(${result_output} FALSE PARENT_SCOPE)
        message(WARNING "${label} failed after ${elapsed_seconds}s; see ${OUTPUT_ROOT}/${log_name}.log")
        return()
    endif()
    set(${result_output} TRUE PARENT_SCOPE)
    message(STATUS "${label} passed in ${elapsed_seconds}s")
endfunction()

run_host_step("Snapshot list" "preflight-snapshot-list" preflight_ready
    snapshot list --vm "${VM_ID}")

if(preflight_ready)
    foreach(round RANGE 1 ${ITERATIONS})
        run_host_step("Round ${round} hard stop" "round-${round}-hard-stop" stopped
            vm stop --id "${VM_ID}" --mode hard)
        if(NOT stopped)
            break()
        endif()
        run_host_step("Round ${round} snapshot restore" "round-${round}-restore" restored
            vm restore --id "${VM_ID}" --snapshot "${SNAPSHOT}")
        if(NOT restored)
            break()
        endif()
        run_host_step("Round ${round} VM start" "round-${round}-start" started
            vm start --id "${VM_ID}")
        if(NOT started)
            break()
        endif()
        if(DEFINED AGENT_BINARY)
            run_host_step("Round ${round} Agent update" "round-${round}-agent-update" updated
                agent update --vm "${VM_ID}" --binary "${AGENT_BINARY}" --version "${agent_version}"
                --timeout-seconds "${UPDATE_TIMEOUT_SECONDS}")
            if(NOT updated)
                break()
            endif()
        endif()
        run_host_step("Round ${round} full check" "round-${round}-check" checked
            check --vm "${VM_ID}" --timeout-seconds "${CHECK_TIMEOUT_SECONDS}")
    endforeach()
endif()

# 中途失败也必须释放死亡进程留下的租约，再执行统一恢复。
if(smoke_failed)
    run_host_step("Final lease unlock" "final-lease-unlock" unlocked
        lab unlock --force true)
endif()
run_host_step("Final hard stop" "final-hard-stop" finally_stopped
    vm stop --id "${VM_ID}" --mode hard)
run_host_step("Final snapshot restore" "final-restore" finally_restored
    vm restore --id "${VM_ID}" --snapshot "${SNAPSHOT}")

string(JOIN "\n" failure_summary ${smoke_failures})
file(WRITE "${OUTPUT_ROOT}/summary.txt"
    "vm_id=${VM_ID}\n"
    "snapshot=${SNAPSHOT}\n"
    "iterations=${ITERATIONS}\n"
    "check_timeout_seconds=${CHECK_TIMEOUT_SECONDS}\n"
    "update_timeout_seconds=${UPDATE_TIMEOUT_SECONDS}\n"
    "agent_version=${agent_version}\n"
    "failed=${smoke_failed}\n"
    "failures=${failure_summary}\n")

if(smoke_failed)
    message(FATAL_ERROR
        "Real VMware lifecycle smoke failed for ${VM_ID}; see ${OUTPUT_ROOT}/summary.txt")
endif()

message(STATUS
    "Real VMware lifecycle smoke completed for ${VM_ID}; final state is restored and stopped")
