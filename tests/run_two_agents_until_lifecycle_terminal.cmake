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

# 查找指定 VM 的单机诊断 manifest 和结果路径。
function(find_diagnostic vm_id manifest_output execution_output)
    set(found_manifest "")
    set(found_execution "")
    file(GLOB diagnostic_manifests
        LIST_DIRECTORIES FALSE
        "${SHARED_ROOT}/runs/check-*/task.json")
    foreach(diagnostic_manifest IN LISTS diagnostic_manifests)
        file(READ "${diagnostic_manifest}" diagnostic_json)
        string(JSON diagnostic_step_count LENGTH "${diagnostic_json}" steps)
        if(NOT diagnostic_step_count EQUAL 1)
            continue()
        endif()
        string(JSON diagnostic_vm GET "${diagnostic_json}" steps 0 vm)
        if(NOT diagnostic_vm STREQUAL vm_id)
            continue()
        endif()
        if(NOT found_manifest STREQUAL "")
            message(FATAL_ERROR "Multiple diagnostic manifests were published for ${vm_id}")
        endif()
        string(JSON diagnostic_run_id GET "${diagnostic_json}" run_id)
        set(found_manifest "${diagnostic_manifest}")
        set(found_execution
            "${SHARED_ROOT}/runs/${diagnostic_run_id}/results/${vm_id}/${vm_id}/execution.json")
    endforeach()
    set(${manifest_output} "${found_manifest}" PARENT_SCOPE)
    set(${execution_output} "${found_execution}" PARENT_SCOPE)
endfunction()

# 在单轮扫描中执行指定 Agent。
function(run_agent_once label config)
    execute_process(
        COMMAND "${VM_EXE}" --config "${config}" --once
        RESULT_VARIABLE agent_result
        OUTPUT_VARIABLE agent_output
        ERROR_VARIABLE agent_error
    )
    if(NOT agent_result EQUAL 0)
        message(FATAL_ERROR "${label} Agent lifecycle run failed: ${agent_error}\n${agent_output}")
    endif()
endfunction()

# 验证结果文件在下一个 manifest 之前完成原子发布。
function(require_file_order first_path second_path context)
    file(TIMESTAMP "${first_path}" first_timestamp "%Y%m%d%H%M%S%f" UTC)
    file(TIMESTAMP "${second_path}" second_timestamp "%Y%m%d%H%M%S%f" UTC)
    if(first_timestamp STREQUAL "" OR second_timestamp STREQUAL "" OR
       NOT first_timestamp STRLESS second_timestamp)
        message(FATAL_ERROR
            "${context} publish order is invalid: ${first_timestamp} -> ${second_timestamp}")
    endif()
endfunction()

# 第一阶段只允许 Host 发布并完成 Client 诊断。
set(client_diagnostic_manifest "")
foreach(attempt RANGE 1 300)
    if(EXISTS "${main_manifest}")
        message(FATAL_ERROR "Main manifest was published before the Client diagnostic")
    endif()
    find_diagnostic(client client_diagnostic_manifest client_diagnostic_execution)
    find_diagnostic(gateway gateway_diagnostic_manifest gateway_diagnostic_execution)
    if(NOT gateway_diagnostic_manifest STREQUAL "")
        message(FATAL_ERROR "Gateway diagnostic was published before the Client was ready")
    endif()
    if(NOT client_diagnostic_manifest STREQUAL "")
        break()
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.05)
endforeach()
if(client_diagnostic_manifest STREQUAL "")
    message(FATAL_ERROR "Host did not publish the Client diagnostic manifest")
endif()

# 暂不运行 Client，让越过生命周期顺序的发布有确定窗口暴露出来。
foreach(attempt RANGE 1 5)
    find_diagnostic(gateway gateway_diagnostic_manifest gateway_diagnostic_execution)
    if(NOT gateway_diagnostic_manifest STREQUAL "" OR EXISTS "${main_manifest}")
        message(FATAL_ERROR "Host advanced beyond the pending Client diagnostic")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.05)
endforeach()
run_agent_once(Client "${CLIENT_CONFIG}")
if(NOT EXISTS "${client_diagnostic_execution}")
    message(FATAL_ERROR "Client Agent omitted its diagnostic result")
endif()
if(EXISTS "${main_manifest}")
    message(FATAL_ERROR "Main manifest was published before the Gateway diagnostic")
endif()

# 第二阶段必须等待 Client Ready 后才允许发布 Gateway 诊断。
set(gateway_diagnostic_manifest "")
foreach(attempt RANGE 1 300)
    if(EXISTS "${main_manifest}")
        message(FATAL_ERROR "Main manifest was published before the Gateway was ready")
    endif()
    find_diagnostic(gateway gateway_diagnostic_manifest gateway_diagnostic_execution)
    if(NOT gateway_diagnostic_manifest STREQUAL "")
        break()
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.05)
endforeach()
if(gateway_diagnostic_manifest STREQUAL "")
    message(FATAL_ERROR "Host did not publish the Gateway diagnostic manifest")
endif()
require_file_order(
    "${client_diagnostic_execution}"
    "${gateway_diagnostic_manifest}"
    "Client Ready to Gateway diagnostic")

# 暂不运行 Gateway，验证主任务确实受第二台 Ready 结果约束。
foreach(attempt RANGE 1 5)
    if(EXISTS "${main_manifest}")
        message(FATAL_ERROR "Host advanced beyond the pending Gateway diagnostic")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.05)
endforeach()
run_agent_once(Gateway "${GATEWAY_CONFIG}")
if(NOT EXISTS "${gateway_diagnostic_execution}")
    message(FATAL_ERROR "Gateway Agent omitted its diagnostic result")
endif()

# 两台 Ready 后等待主任务完整发布，避免 Agent 扫描 Host 的暂存目录。
foreach(attempt RANGE 1 300)
    if(EXISTS "${main_manifest}")
        break()
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.05)
endforeach()
if(NOT EXISTS "${main_manifest}")
    message(FATAL_ERROR "Host did not publish the main manifest after both Agents were ready")
endif()
require_file_order(
    "${gateway_diagnostic_execution}"
    "${main_manifest}"
    "Gateway Ready to main manifest")
run_agent_once(Client "${CLIENT_CONFIG}")
run_agent_once(Gateway "${GATEWAY_CONFIG}")

# 主任务完成后等待 finally 原子发布，再允许两台 Agent 扫描。
set(finally_manifest "")
foreach(attempt RANGE 1 300)
    file(GLOB finally_manifests
        LIST_DIRECTORIES FALSE
        "${SHARED_ROOT}/runs/finally-*/task.json")
    list(LENGTH finally_manifests finally_manifest_count)
    if(finally_manifest_count GREATER 1)
        message(FATAL_ERROR "Host published multiple finally manifests")
    elseif(finally_manifest_count EQUAL 1)
        list(GET finally_manifests 0 finally_manifest)
        break()
    endif()
    if(EXISTS "${LIFECYCLE_STATE}")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E cat "${LIFECYCLE_STATE}"
            RESULT_VARIABLE lifecycle_read_result
            OUTPUT_VARIABLE lifecycle_json
            ERROR_QUIET
        )
        if(lifecycle_read_result EQUAL 0 AND lifecycle_json MATCHES
           "\"phase\": \"(completed|failed|recovery_failed|manual_intervention_required)\"")
            message(FATAL_ERROR "Host reached a terminal phase before publishing finally")
        endif()
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.05)
endforeach()
if(finally_manifest STREQUAL "")
    message(FATAL_ERROR "Host did not publish the finally manifest")
endif()
run_agent_once(Client "${CLIENT_CONFIG}")
run_agent_once(Gateway "${GATEWAY_CONFIG}")

# finally 完成后不再扫描任务，只等待 Host 清理并持久化终态。
foreach(attempt RANGE 1 300)
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
