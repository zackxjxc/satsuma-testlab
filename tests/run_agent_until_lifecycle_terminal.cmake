# 驱动测试 Agent，直到 Host 生命周期状态进入终态。
if(NOT DEFINED VM_EXE OR NOT DEFINED AGENT_CONFIG OR NOT DEFINED LIFECYCLE_STATE)
    message(FATAL_ERROR "Agent lifecycle driver arguments are incomplete")
endif()

foreach(attempt RANGE 1 300)
    execute_process(
        COMMAND "${VM_EXE}" --config "${AGENT_CONFIG}" --once
        RESULT_VARIABLE vm_result
        OUTPUT_VARIABLE vm_output
        ERROR_VARIABLE vm_error
    )
    if(NOT vm_result EQUAL 0)
        message(FATAL_ERROR "SatsumaVM lifecycle run failed: ${vm_error}\n${vm_output}")
    endif()

    if(EXISTS "${LIFECYCLE_STATE}")
        file(READ "${LIFECYCLE_STATE}" lifecycle_json)
        if(lifecycle_json MATCHES
           "\"phase\": \"(completed|failed|recovery_failed|manual_intervention_required)\"")
            return()
        endif()
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.1)
endforeach()

message(FATAL_ERROR "Host lifecycle did not reach a terminal phase")
