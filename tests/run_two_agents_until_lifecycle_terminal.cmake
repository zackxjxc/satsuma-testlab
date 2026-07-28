# 轮询驱动两台测试 Agent，并禁止主任务早于两台诊断结果发布。
if(NOT DEFINED VM_EXE OR
   NOT DEFINED CLIENT_CONFIG OR
   NOT DEFINED GATEWAY_CONFIG OR
   NOT DEFINED SHARED_ROOT OR
   NOT DEFINED RUN_ID OR
   NOT DEFINED LIFECYCLE_STATE)
    message(FATAL_ERROR "Two-Agent lifecycle driver arguments are incomplete")
endif()

set(main_manifest "${SHARED_ROOT}/runs/${RUN_ID}/task.json")

# 主 manifest 一旦出现，两台按生命周期顺序发布的诊断结果必须都已存在。
function(assert_main_publish_gate)
    if(NOT EXISTS "${main_manifest}")
        return()
    endif()
    file(GLOB client_diagnostics
        "${SHARED_ROOT}/runs/check-*/results/client/client/execution.json")
    file(GLOB gateway_diagnostics
        "${SHARED_ROOT}/runs/check-*/results/gateway/gateway/execution.json")
    if(NOT client_diagnostics OR NOT gateway_diagnostics)
        message(FATAL_ERROR "Main manifest was published before both Agents were ready")
    endif()
endfunction()

foreach(attempt RANGE 1 300)
    assert_main_publish_gate()
    execute_process(
        COMMAND "${VM_EXE}" --config "${CLIENT_CONFIG}" --once
        RESULT_VARIABLE client_result
        OUTPUT_VARIABLE client_output
        ERROR_VARIABLE client_error
    )
    if(NOT client_result EQUAL 0)
        message(FATAL_ERROR "Client Agent lifecycle run failed: ${client_error}\n${client_output}")
    endif()

    # 给 Host 留出消费第一台结果的窗口，稳定捕获提前发布主任务的实现。
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.05)
    assert_main_publish_gate()
    execute_process(
        COMMAND "${VM_EXE}" --config "${GATEWAY_CONFIG}" --once
        RESULT_VARIABLE gateway_result
        OUTPUT_VARIABLE gateway_output
        ERROR_VARIABLE gateway_error
    )
    if(NOT gateway_result EQUAL 0)
        message(FATAL_ERROR "Gateway Agent lifecycle run failed: ${gateway_error}\n${gateway_output}")
    endif()

    assert_main_publish_gate()
    if(EXISTS "${LIFECYCLE_STATE}")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E cat "${LIFECYCLE_STATE}"
            RESULT_VARIABLE lifecycle_read_result
            OUTPUT_VARIABLE lifecycle_json
            ERROR_QUIET
        )
        if(lifecycle_read_result EQUAL 0 AND lifecycle_json MATCHES
           "\"phase\": \"(completed|failed|recovery_failed|manual_intervention_required)\"")
            return()
        endif()
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.05)
endforeach()

message(FATAL_ERROR "Host multi-VM lifecycle did not reach a terminal phase")
