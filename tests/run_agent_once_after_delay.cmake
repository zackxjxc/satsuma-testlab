# 重试单次 Agent 扫描，直到 Host 发布的并发诊断任务被执行。
if(NOT DEFINED VM_EXE OR NOT DEFINED AGENT_CONFIG)
    message(FATAL_ERROR "Agent diagnostic driver arguments are incomplete")
endif()

foreach(attempt RANGE 1 100)
    execute_process(
        COMMAND "${VM_EXE}" --config "${AGENT_CONFIG}" --once
        RESULT_VARIABLE vm_result
        OUTPUT_VARIABLE vm_output
        ERROR_VARIABLE vm_error
    )
    if(NOT vm_result EQUAL 0)
        message(FATAL_ERROR "SatsumaVM diagnostic run failed: ${vm_error}\n${vm_output}")
    endif()

    string(FIND "${vm_output}" "\"executed_steps\":0" idle_position)
    if(idle_position EQUAL -1)
        return()
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.1)
endforeach()

message(FATAL_ERROR "SatsumaVM did not receive the diagnostic task")
