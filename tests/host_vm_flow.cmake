# 在本机目录中模拟 VMCI 两端持久状态并验证完整 Host/VM 流程。
if(NOT DEFINED HOST_EXE OR
   NOT DEFINED VM_EXE OR
   NOT DEFINED FIXTURE_EXE OR
   NOT DEFINED VMRUN_EXE OR
   NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "Host/VM integration test arguments are incomplete")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/storage/mirror" "${TEST_ROOT}/storage/work" "${TEST_ROOT}/archive")

file(TO_CMAKE_PATH "${TEST_ROOT}/storage/mirror" state_path)
file(TO_CMAKE_PATH "${TEST_ROOT}/storage" storage_path)
file(TO_CMAKE_PATH "${TEST_ROOT}/archive" archive_path)
file(TO_CMAKE_PATH "${FIXTURE_EXE}" fixture_path)
file(TO_CMAKE_PATH "${VMRUN_EXE}" vmrun_path)
file(TO_CMAKE_PATH "${TEST_ROOT}/VM 01.vmx" vmx_path)
file(TO_CMAKE_PATH "${TEST_ROOT}/Hard VM.vmx" hard_vmx_path)
file(TO_CMAKE_PATH "${TEST_ROOT}/Reconcile VM.vmx" reconcile_vmx_path)
file(WRITE "${vmx_path}" "# Test VMX placeholder\n")
file(WRITE "${hard_vmx_path}" "# Test VMX placeholder\n")
file(WRITE "${reconcile_vmx_path}" "# Snapshot reconciliation VMX placeholder\n")

set(lab_json [=[
{
  "schema_version": 1,
  "lab_id": "integration_lab",
  "provider": {
    "type": "vmware_workstation",
    "vmrun": "@VMRUN@"
  },
  "host": {"archive_root": "@ARCHIVE@"},
  "transport": {
    "state_root": "@STATE@",
    "vmci_port": 42510
  },
  "vms": [
    {
      "id": "vm_01",
      "vmx": "@VMX@",
      "agent_version": "0.1.0",
      "snapshots": {
        "base": "clean",
        "ai_prefix": "satsuma-ai-",
        "max_ai_snapshots": 8
      }
    },
    {
      "id": "hard-stop",
      "vmx": "@HARD_VMX@",
      "agent_version": "0.1.0",
      "snapshots": {
        "base": "clean",
        "ai_prefix": "satsuma-ai-",
        "max_ai_snapshots": 8
      }
    },
    {
      "id": "snapshot-reconcile",
      "vmx": "@RECONCILE_VMX@",
      "agent_version": "0.1.0",
      "snapshots": {
        "base": "clean",
        "ai_prefix": "satsuma-ai-",
        "max_ai_snapshots": 8
      }
    }
  ]
}
]=])
string(REPLACE "@ARCHIVE@" "${archive_path}" lab_json "${lab_json}")
string(REPLACE "@STATE@" "${state_path}" lab_json "${lab_json}")
string(REPLACE "@VMRUN@" "${vmrun_path}" lab_json "${lab_json}")
string(REPLACE "@VMX@" "${vmx_path}" lab_json "${lab_json}")
string(REPLACE "@HARD_VMX@" "${hard_vmx_path}" lab_json "${lab_json}")
string(REPLACE "@RECONCILE_VMX@" "${reconcile_vmx_path}" lab_json "${lab_json}")
file(WRITE "${TEST_ROOT}/lab.json" "${lab_json}")

set(agent_json [=[
{
  "schema_version": 1,
  "protocol_version": 4,
  "lab_id": "integration_lab",
  "vm_id": "vm_01",
  "agent_version": "0.1.0",
  "transport": {"host_cid": 2, "vmci_port": 42510},
  "storage_root": "@STORAGE@",
  "mirror_root": "@STATE@",
  "poll_interval_ms": 100,
  "reconnect_interval_ms": 100
}
]=])
string(REPLACE "@STATE@" "${state_path}" agent_json "${agent_json}")
string(REPLACE "@STORAGE@" "${storage_path}" agent_json "${agent_json}")
file(WRITE "${TEST_ROOT}/agent.json" "${agent_json}")
set(ENV{SATSUMA_TEST_LOCAL_MIRROR} "1")

# 独立故障场景确认保留租约后，由测试操作者显式放弃现场。
function(force_unlock_lab context)
    execute_process(
        COMMAND "${HOST_EXE}" lab unlock
            --config "${TEST_ROOT}/lab.json"
            --force true
        RESULT_VARIABLE unlock_result
        OUTPUT_VARIABLE unlock_output
        ERROR_VARIABLE unlock_error
    )
    if(NOT unlock_result EQUAL 0 OR NOT unlock_output MATCHES "\"status\": \"unlocked\"")
        message(FATAL_ERROR "${context} lease could not be unlocked: ${unlock_error}\n${unlock_output}")
    endif()
endfunction()

set(task_json [=[
{
  "schema_version": 3,
  "name": "host-vm-integration",
  "run_id": "integration_run",
  "artifacts": [
    {
      "source": "@FIXTURE@",
      "vm": "vm_01",
      "destination": "artifacts/vm_01/SatsumaTestFixture.exe"
    }
  ],
  "steps": [
    {
      "id": "execute_fixture",
      "vm": "vm_01",
      "type": "execute",
      "program": "artifacts/vm_01/SatsumaTestFixture.exe",
      "arguments": ["--message", "hello from fixture", "--output", "generated/result.json"],
      "timeout_seconds": 10,
      "collect_files": ["generated/result.json"]
    },
    {
      "id": "timeout_fixture",
      "vm": "vm_01",
      "type": "execute",
      "program": "artifacts/vm_01/SatsumaTestFixture.exe",
      "arguments": ["--sleep-ms", "3000"],
      "timeout_seconds": 1,
      "collect_files": []
    }
  ]
}
]=])
string(REPLACE "@FIXTURE@" "${fixture_path}" task_json "${task_json}")
file(WRITE "${TEST_ROOT}/task.json" "${task_json}")

# Host 帮助和版本必须在不读取配置时可用。
execute_process(
    COMMAND "${HOST_EXE}" --help
    RESULT_VARIABLE host_help_result
    OUTPUT_VARIABLE host_help_output
    ERROR_VARIABLE host_help_error
)
string(FIND "${host_help_output}" "Usage:" host_help_usage_position)
if(NOT host_help_result EQUAL 0 OR host_help_usage_position EQUAL -1)
    message(FATAL_ERROR "SatsumaHost --help failed: ${host_help_error}\n${host_help_output}")
endif()

execute_process(
    COMMAND "${HOST_EXE}" --version
    RESULT_VARIABLE host_version_result
    OUTPUT_VARIABLE host_version_output
    ERROR_VARIABLE host_version_error
)
string(STRIP "${host_version_output}" host_version_output)
if(NOT host_version_result EQUAL 0 OR
   NOT host_version_output STREQUAL "${EXPECTED_VERSION}")
    message(FATAL_ERROR
        "SatsumaHost --version returned an unexpected result: "
        "${host_version_error}\n${host_version_output}")
endif()

# Agent 返回失败终态后，Host 必须释放短操作租约并保留失败证据。
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DUPDATE_ROOT=${state_path}/updates"
        -DVM_ID=vm_01
        -P "${CMAKE_CURRENT_LIST_DIR}/write_failed_update_result_after_publish.cmake"
    COMMAND "${HOST_EXE}" agent update
        --config "${TEST_ROOT}/lab.json"
        --vm vm_01
        --binary "${VM_EXE}"
        --version 0.3.1
        --timeout-seconds 10
    RESULTS_VARIABLE failed_update_results
    OUTPUT_VARIABLE failed_update_output
    ERROR_VARIABLE failed_update_error
)
if(NOT failed_update_results STREQUAL "0;1" OR
   NOT failed_update_output MATCHES "\"status\": \"failed\"")
    message(FATAL_ERROR
        "Failed Agent update did not return its terminal result (${failed_update_results}): "
        "${failed_update_error}\n${failed_update_output}")
endif()
execute_process(
    COMMAND "${HOST_EXE}" lab status --config "${TEST_ROOT}/lab.json"
    RESULT_VARIABLE failed_update_status_result
    OUTPUT_VARIABLE failed_update_status_output
    ERROR_VARIABLE failed_update_status_error
)
if(NOT failed_update_status_result EQUAL 0 OR
   NOT failed_update_status_output MATCHES "\"status\": \"available\"" OR
   NOT failed_update_status_output MATCHES "\"state\": \"failed\"")
    message(FATAL_ERROR
        "Failed Agent update retained its lab lease: "
        "${failed_update_status_error}\n${failed_update_status_output}")
endif()

# 命令结构和选项拼写错误必须在读取配置或调用 VMware 前失败。
execute_process(
    COMMAND "${HOST_EXE}" unknown-command
    RESULT_VARIABLE unknown_command_result
    OUTPUT_VARIABLE unknown_command_output
    ERROR_VARIABLE unknown_command_error
)
string(FIND "${unknown_command_output}" "Usage:" unknown_command_usage_position)
if(NOT unknown_command_result EQUAL 2 OR unknown_command_usage_position EQUAL -1)
    message(FATAL_ERROR
        "SatsumaHost unknown command did not return usage: "
        "${unknown_command_error}\n${unknown_command_output}")
endif()

execute_process(
    COMMAND "${HOST_EXE}" vm start --id vm_01
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE missing_config_result
    OUTPUT_VARIABLE missing_config_output
    ERROR_VARIABLE missing_config_error
)
string(FIND "${missing_config_error}" "--config" missing_config_error_position)
if(missing_config_result EQUAL 0 OR missing_config_error_position EQUAL -1)
    message(FATAL_ERROR
        "SatsumaHost vm start accepted an implicit configuration: "
        "${missing_config_error}\n${missing_config_output}")
endif()

execute_process(
    COMMAND "${HOST_EXE}" vm stop
        --config "${TEST_ROOT}/lab.json"
        --id hard-stop
        --mdoe hard
    RESULT_VARIABLE unknown_option_result
    OUTPUT_VARIABLE unknown_option_output
    ERROR_VARIABLE unknown_option_error
)
string(FIND "${unknown_option_error}" "--mdoe" unknown_option_error_position)
if(unknown_option_result EQUAL 0 OR unknown_option_error_position EQUAL -1)
    message(FATAL_ERROR
        "SatsumaHost silently ignored a misspelled option: "
        "${unknown_option_error}\n${unknown_option_output}")
endif()

execute_process(
    COMMAND "${HOST_EXE}" run --config "${TEST_ROOT}/lab.json"
    RESULT_VARIABLE missing_plan_result
    OUTPUT_VARIABLE missing_plan_output
    ERROR_VARIABLE missing_plan_error
)
string(FIND "${missing_plan_error}" "--plan" missing_plan_error_position)
if(missing_plan_result EQUAL 0 OR missing_plan_error_position EQUAL -1)
    message(FATAL_ERROR
        "SatsumaHost missing option error omitted --plan: "
        "${missing_plan_error}\n${missing_plan_output}")
endif()
if(EXISTS "${state_path}/runs")
    message(FATAL_ERROR "Rejected Host CLI calls unexpectedly created the runs directory")
endif()

set(lifecycle_task_json [=[
{
  "schema_version": 3,
  "name": "lifecycle-requires-orchestrator",
  "steps": [
    {
      "id": "echo",
      "vm": "vm_01",
      "type": "echo",
      "message": "must not run"
    }
  ],
  "lifecycle": {
    "vms": [
      {
        "vm": "vm_01",
        "restore_before": "clean",
        "on_success": {"action": "stop"},
        "on_failure": {"action": "restore", "snapshot": "clean"}
      }
    ]
  }
}
]=])
file(WRITE "${TEST_ROOT}/lifecycle-task.json" "${lifecycle_task_json}")

execute_process(
    COMMAND "${HOST_EXE}" run
        --config "${TEST_ROOT}/lab.json"
        --plan "${TEST_ROOT}/lifecycle-task.json"
    RESULT_VARIABLE lifecycle_run_result
    OUTPUT_VARIABLE lifecycle_run_output
    ERROR_VARIABLE lifecycle_run_error
)
if(lifecycle_run_result EQUAL 0)
    message(FATAL_ERROR "SatsumaHost run silently ignored lifecycle policy: ${lifecycle_run_output}")
endif()
string(FIND "${lifecycle_run_error}" "require the Host orchestrator" lifecycle_error_position)
if(lifecycle_error_position EQUAL -1)
    message(FATAL_ERROR "SatsumaHost run returned an unexpected lifecycle error: ${lifecycle_run_error}")
endif()

# 编排计划必须由调用方固定 run_id，保证 Host 崩溃后可定位同一归档。
execute_process(
    COMMAND "${HOST_EXE}" orchestrate
        --config "${TEST_ROOT}/lab.json"
        --plan "${TEST_ROOT}/lifecycle-task.json"
        --timeout-seconds 10
    RESULT_VARIABLE missing_run_id_result
    OUTPUT_VARIABLE missing_run_id_output
    ERROR_VARIABLE missing_run_id_error
)
if(missing_run_id_result EQUAL 0)
    message(FATAL_ERROR
        "SatsumaHost orchestrate accepted a lifecycle plan without run_id: ${missing_run_id_output}")
endif()
string(FIND
    "${missing_run_id_error}"
    "requires an explicit plan run_id"
    missing_run_id_error_position)
if(missing_run_id_error_position EQUAL -1)
    message(FATAL_ERROR
        "SatsumaHost returned an unexpected missing run_id error: ${missing_run_id_error}")
endif()

set(orchestration_task_json [=[
{
  "schema_version": 3,
  "name": "host-lifecycle-integration",
  "run_id": "orchestration_run",
  "steps": [
    {
      "id": "main_echo",
      "vm": "vm_01",
      "type": "echo",
      "message": "main lifecycle step"
    }
  ],
  "lifecycle": {
    "vms": [
      {
        "vm": "vm_01",
        "restore_before": "clean",
        "on_success": {"action": "stop"},
        "on_failure": {"action": "restore", "snapshot": "clean"}
      }
    ],
    "finally": [
      {
        "id": "finally_echo",
        "vm": "vm_01",
        "type": "echo",
        "message": "finally lifecycle step"
      }
    ]
  }
}
]=])
file(WRITE "${TEST_ROOT}/orchestration-task.json" "${orchestration_task_json}")
set(lifecycle_state "${archive_path}/runs/orchestration_run/lifecycle.json")

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DVM_EXE=${VM_EXE}"
        "-DAGENT_CONFIG=${TEST_ROOT}/agent.json"
        "-DLIFECYCLE_STATE=${lifecycle_state}"
        "-DSTATE_ROOT=${state_path}"
        -DINJECT_ATOMIC_JSON_TEMPORARY=ON
        -P "${CMAKE_CURRENT_LIST_DIR}/run_agent_until_lifecycle_terminal.cmake"
    COMMAND "${HOST_EXE}" orchestrate
        --config "${TEST_ROOT}/lab.json"
        --plan "${TEST_ROOT}/orchestration-task.json"
        --timeout-seconds 10
    RESULTS_VARIABLE orchestration_results
    OUTPUT_VARIABLE orchestration_output
    ERROR_VARIABLE orchestration_error
)
if(NOT orchestration_results STREQUAL "0;0")
    message(FATAL_ERROR
        "SatsumaHost orchestrate failed (${orchestration_results}): "
        "${orchestration_error}\n${orchestration_output}")
endif()
string(FIND "${orchestration_output}" "\"status\": \"COMPLETED\"" orchestration_status_position)
if(orchestration_status_position EQUAL -1)
    message(FATAL_ERROR "SatsumaHost orchestrate returned unexpected output: ${orchestration_output}")
endif()
string(FIND "${orchestration_output}" "\"cleanup_action\": \"stop\"" cleanup_action_position)
if(cleanup_action_position EQUAL -1)
    message(FATAL_ERROR "SatsumaHost orchestrate did not apply success cleanup: ${orchestration_output}")
endif()
if(NOT EXISTS "${lifecycle_state}")
    message(FATAL_ERROR "SatsumaHost orchestrate did not persist lifecycle state")
endif()
file(READ "${lifecycle_state}" lifecycle_state_json)
string(FIND "${lifecycle_state_json}" "\"phase\": \"completed\"" completed_phase_position)
string(FIND "${lifecycle_state_json}" "\"to\": \"running_finally\"" finally_phase_position)
if(completed_phase_position EQUAL -1 OR finally_phase_position EQUAL -1)
    message(FATAL_ERROR "Lifecycle state omitted required transitions: ${lifecycle_state_json}")
endif()
if(NOT EXISTS "${archive_path}/runs/orchestration_run/evidence/main/task.json" OR
   NOT EXISTS "${archive_path}/runs/orchestration_run/evidence/finally/task.json")
    message(FATAL_ERROR "SatsumaHost orchestrate did not archive main and finally evidence")
endif()
file(GLOB_RECURSE archived_atomic_temporaries
    "${archive_path}/runs/orchestration_run/evidence/.tmp-write-*"
    "${archive_path}/runs/orchestration_run/evidence/*/.tmp-write-*")
if(archived_atomic_temporaries)
    message(FATAL_ERROR
        "SatsumaHost archived atomic JSON temporary files: ${archived_atomic_temporaries}")
endif()

# 编排总等待可超过 300 秒，但 Agent 诊断仍使用自身受支持的有限上限。
string(REPLACE
    "orchestration_run"
    "orchestration_long_timeout"
    long_timeout_task_json
    "${orchestration_task_json}")
file(WRITE "${TEST_ROOT}/orchestration-long-timeout.json" "${long_timeout_task_json}")
set(long_timeout_state "${archive_path}/runs/orchestration_long_timeout/lifecycle.json")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DVM_EXE=${VM_EXE}"
        "-DAGENT_CONFIG=${TEST_ROOT}/agent.json"
        "-DLIFECYCLE_STATE=${long_timeout_state}"
        -P "${CMAKE_CURRENT_LIST_DIR}/run_agent_until_lifecycle_terminal.cmake"
    COMMAND "${HOST_EXE}" orchestrate
        --config "${TEST_ROOT}/lab.json"
        --plan "${TEST_ROOT}/orchestration-long-timeout.json"
        --timeout-seconds 301
    RESULTS_VARIABLE long_timeout_results
    OUTPUT_VARIABLE long_timeout_output
    ERROR_VARIABLE long_timeout_error
)
if(NOT long_timeout_results STREQUAL "0;0")
    message(FATAL_ERROR
        "Orchestration rejected a valid timeout above the diagnostic limit "
        "(${long_timeout_results}): ${long_timeout_error}\n${long_timeout_output}")
endif()

# 已完成的同一编排再次调用时只返回持久化终态，不重复执行任务或清理策略。
execute_process(
    COMMAND "${HOST_EXE}" orchestrate
        --config "${TEST_ROOT}/lab.json"
        --plan "${TEST_ROOT}/orchestration-task.json"
        --timeout-seconds 10
    RESULT_VARIABLE completed_resume_result
    OUTPUT_VARIABLE completed_resume_output
    ERROR_VARIABLE completed_resume_error
)
if(NOT completed_resume_result EQUAL 0)
    message(FATAL_ERROR
        "Completed orchestration was not idempotent: "
        "${completed_resume_error}\n${completed_resume_output}")
endif()
string(FIND "${completed_resume_output}" "\"resumed\": true" completed_resumed_position)
string(FIND "${completed_resume_output}" "\"status\": \"COMPLETED\"" completed_status_position)
if(completed_resumed_position EQUAL -1 OR completed_status_position EQUAL -1)
    message(FATAL_ERROR
        "Completed orchestration did not return its persisted terminal state: ${completed_resume_output}")
endif()

# 已有终态只读取归档，即使 VMware 控制程序暂时不可用也应幂等返回。
string(REPLACE
    "${vmrun_path}"
    "${TEST_ROOT}/missing-vmrun.exe"
    unavailable_lab_json
    "${lab_json}")
file(WRITE "${TEST_ROOT}/lab-vmware-unavailable.json" "${unavailable_lab_json}")
execute_process(
    COMMAND "${HOST_EXE}" orchestrate
        --config "${TEST_ROOT}/lab-vmware-unavailable.json"
        --plan "${TEST_ROOT}/orchestration-task.json"
        --timeout-seconds 10
    RESULT_VARIABLE unavailable_resume_result
    OUTPUT_VARIABLE unavailable_resume_output
    ERROR_VARIABLE unavailable_resume_error
)
if(NOT unavailable_resume_result EQUAL 0)
    message(FATAL_ERROR
        "Completed orchestration still depended on VMware: "
        "${unavailable_resume_error}\n${unavailable_resume_output}")
endif()
string(FIND "${unavailable_resume_output}" "\"status\": \"COMPLETED\"" unavailable_status_position)
if(unavailable_status_position EQUAL -1)
    message(FATAL_ERROR
        "VMware-independent terminal resume returned unexpected output: ${unavailable_resume_output}")
endif()

# 构造 Host 在 executing 阶段退出后的持久化归档和已经发布的主任务。
set(executing_resume_plan [=[
{
  "schema_version": 3,
  "name": "host-executing-resume",
  "run_id": "orchestration_executing_resume",
  "steps": [
    {
      "id": "resume_echo",
      "vm": "vm_01",
      "type": "echo",
      "message": "resume persisted execution"
    }
  ],
  "lifecycle": {
    "vms": [
      {
        "vm": "vm_01",
        "on_success": {"action": "stop"},
        "on_failure": {"action": "restore", "snapshot": "clean"}
      }
    ]
  }
}
]=])
set(executing_main_plan [=[
{
  "schema_version": 3,
  "name": "host-executing-resume",
  "run_id": "orchestration_executing_resume",
  "steps": [
    {
      "id": "resume_echo",
      "vm": "vm_01",
      "type": "echo",
      "message": "resume persisted execution"
    }
  ]
}
]=])
set(executing_plan_path "${TEST_ROOT}/orchestration-executing-resume.json")
set(executing_main_path "${TEST_ROOT}/orchestration-executing-main.json")
file(WRITE "${executing_plan_path}" "${executing_resume_plan}")
file(WRITE "${executing_main_path}" "${executing_main_plan}")
file(MAKE_DIRECTORY "${state_path}/runs/orchestration_executing_resume")
file(WRITE "${state_path}/runs/orchestration_executing_resume/task.json" [=[
{
  "schema_version": 2,
  "protocol_version": 4,
  "lab_id": "integration_lab",
  "run_id": "orchestration_executing_resume",
  "name": "host-executing-resume",
  "created_at": "2026-07-29T00:00:00.000Z",
  "artifacts": [],
  "steps": [
    {
      "id": "resume_echo",
      "vm": "vm_01",
      "type": "echo",
      "message": "resume persisted execution",
      "retry_safe": true
    }
  ]
}
]=])

set(executing_archive "${archive_path}/runs/orchestration_executing_resume")
set(executing_state "${executing_archive}/lifecycle.json")
file(MAKE_DIRECTORY "${executing_archive}")
file(COPY_FILE "${executing_plan_path}" "${executing_archive}/plan.json")
file(SHA256 "${executing_plan_path}" executing_plan_sha256)
file(WRITE "${executing_archive}/orchestration.json" [=[
{
  "schema_version": 1,
  "run_id": "orchestration_executing_resume",
  "vm_id": "vm_01",
  "main_run_id": "orchestration_executing_resume",
  "finally_run_id": "finally_executing_resume",
  "plan_sha256": "@PLAN_SHA256@"
}
]=])
file(READ "${executing_archive}/orchestration.json" executing_identity_json)
string(REPLACE "@PLAN_SHA256@" "${executing_plan_sha256}"
    executing_identity_json "${executing_identity_json}")
file(WRITE "${executing_archive}/orchestration.json" "${executing_identity_json}")
file(WRITE "${executing_state}" [=[
{
  "schema_version": 1,
  "run_id": "orchestration_executing_resume",
  "phase": "executing",
  "sequence": 4,
  "updated_at": "2026-07-27T00:00:04.000Z",
  "transitions": [
    {
      "sequence": 1,
      "from": "preparing",
      "to": "starting_vm",
      "occurred_at": "2026-07-27T00:00:01.000Z",
      "message": "start target VM"
    },
    {
      "sequence": 2,
      "from": "starting_vm",
      "to": "waiting_agent",
      "occurred_at": "2026-07-27T00:00:02.000Z",
      "message": "wait for Agent diagnostic echo"
    },
    {
      "sequence": 3,
      "from": "waiting_agent",
      "to": "deploying",
      "occurred_at": "2026-07-27T00:00:03.000Z",
      "message": "publish main task"
    },
    {
      "sequence": 4,
      "from": "deploying",
      "to": "executing",
      "occurred_at": "2026-07-27T00:00:04.000Z",
      "message": "wait for main task results"
    }
  ]
}
]=])

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DVM_EXE=${VM_EXE}"
        "-DAGENT_CONFIG=${TEST_ROOT}/agent.json"
        "-DLIFECYCLE_STATE=${executing_state}"
        -P "${CMAKE_CURRENT_LIST_DIR}/run_agent_until_lifecycle_terminal.cmake"
    COMMAND "${HOST_EXE}" orchestrate
        --config "${TEST_ROOT}/lab.json"
        --plan "${executing_plan_path}"
        --timeout-seconds 10
    RESULTS_VARIABLE executing_resume_results
    OUTPUT_VARIABLE executing_resume_output
    ERROR_VARIABLE executing_resume_error
)
if(NOT executing_resume_results STREQUAL "0;0")
    message(FATAL_ERROR
        "Persisted executing orchestration did not resume (${executing_resume_results}): "
        "${executing_resume_error}\n${executing_resume_output}")
endif()
string(FIND "${executing_resume_output}" "\"resumed\": true" executing_resumed_position)
string(FIND "${executing_resume_output}" "\"status\": \"COMPLETED\"" executing_status_position)
if(executing_resumed_position EQUAL -1 OR executing_status_position EQUAL -1 OR
   NOT EXISTS "${executing_archive}/evidence/main/task.json")
    message(FATAL_ERROR
        "Persisted executing orchestration omitted its completed evidence: ${executing_resume_output}")
endif()

# 构造主结果已完成但 Host 尚未归档的 collecting_evidence 恢复点。
string(REPLACE
    "orchestration_executing_resume"
    "orchestration_collecting_resume"
    collecting_resume_plan
    "${executing_resume_plan}")
string(REPLACE
    "finally_executing_resume"
    "finally_collecting_resume"
    collecting_resume_plan
    "${collecting_resume_plan}")
string(REPLACE
    "\"action\": \"stop\""
    "\"action\": \"leave_running\""
    collecting_resume_plan
    "${collecting_resume_plan}")
string(REPLACE
    "\"schema_version\": 1,"
    "\"schema_version\": 2,"
    collecting_resume_plan
    "${collecting_resume_plan}")
string(REPLACE
    "  \"lifecycle\": {"
    "  \"cleanup\": {\n    \"guest_work\": {\"on_success\": \"retain\", \"on_failure\": \"retain\"},\n    \"host_run\": {\"on_success\": \"retain\", \"on_failure\": \"retain\"}\n  },\n  \"lifecycle\": {"
    collecting_resume_plan
    "${collecting_resume_plan}")
string(REPLACE
    "orchestration_executing_resume"
    "orchestration_collecting_resume"
    collecting_main_plan
    "${executing_main_plan}")
set(collecting_plan_path "${TEST_ROOT}/orchestration-collecting-resume.json")
set(collecting_main_path "${TEST_ROOT}/orchestration-collecting-main.json")
file(WRITE "${collecting_plan_path}" "${collecting_resume_plan}")
file(WRITE "${collecting_main_path}" "${collecting_main_plan}")
set(collecting_manifest [=[
{
  "schema_version": 2,
  "protocol_version": 4,
  "lab_id": "integration_lab",
  "run_id": "orchestration_collecting_resume",
  "name": "host-collecting-resume",
  "created_at": "2026-07-29T00:00:00.000Z",
  "artifacts": [],
  "steps": [
    {
      "id": "resume_echo",
      "vm": "vm_01",
      "type": "echo",
      "message": "resume persisted execution",
      "retry_safe": true
    }
  ]
}
]=])
file(MAKE_DIRECTORY "${state_path}/runs/orchestration_collecting_resume")
file(WRITE "${state_path}/runs/orchestration_collecting_resume/task.json" "${collecting_manifest}")
execute_process(
    COMMAND "${VM_EXE}" --config "${TEST_ROOT}/agent.json" --once
    RESULT_VARIABLE collecting_agent_result
    OUTPUT_VARIABLE collecting_agent_output
    ERROR_VARIABLE collecting_agent_error
)
if(NOT collecting_agent_result EQUAL 0)
    message(FATAL_ERROR
        "Collecting resume fixture Agent failed: "
        "${collecting_agent_error}\n${collecting_agent_output}")
endif()

set(collecting_archive "${archive_path}/runs/orchestration_collecting_resume")
set(collecting_state "${collecting_archive}/lifecycle.json")
file(MAKE_DIRECTORY "${collecting_archive}")
file(COPY_FILE "${collecting_plan_path}" "${collecting_archive}/plan.json")
file(SHA256 "${collecting_plan_path}" collecting_plan_sha256)
file(WRITE "${collecting_archive}/orchestration.json" [=[
{
  "schema_version": 1,
  "run_id": "orchestration_collecting_resume",
  "vm_id": "vm_01",
  "main_run_id": "orchestration_collecting_resume",
  "finally_run_id": "finally_collecting_resume",
  "plan_sha256": "@PLAN_SHA256@"
}
]=])
file(READ "${collecting_archive}/orchestration.json" collecting_identity_json)
string(REPLACE "@PLAN_SHA256@" "${collecting_plan_sha256}"
    collecting_identity_json "${collecting_identity_json}")
file(WRITE "${collecting_archive}/orchestration.json" "${collecting_identity_json}")
file(WRITE "${collecting_state}" [=[
{
  "schema_version": 1,
  "run_id": "orchestration_collecting_resume",
  "phase": "collecting_evidence",
  "sequence": 5,
  "updated_at": "2026-07-27T00:00:05.000Z",
  "transitions": [
    {"sequence": 1, "from": "preparing", "to": "starting_vm",
     "occurred_at": "2026-07-27T00:00:01.000Z", "message": "start target VM"},
    {"sequence": 2, "from": "starting_vm", "to": "waiting_agent",
     "occurred_at": "2026-07-27T00:00:02.000Z", "message": "wait for Agent diagnostic echo"},
    {"sequence": 3, "from": "waiting_agent", "to": "deploying",
     "occurred_at": "2026-07-27T00:00:03.000Z", "message": "publish main task"},
    {"sequence": 4, "from": "deploying", "to": "executing",
     "occurred_at": "2026-07-27T00:00:04.000Z", "message": "wait for main task results"},
    {"sequence": 5, "from": "executing", "to": "collecting_evidence",
     "occurred_at": "2026-07-27T00:00:05.000Z", "message": "archive main task evidence"}
  ]
}
]=])
execute_process(
    COMMAND "${HOST_EXE}" orchestrate
        --config "${TEST_ROOT}/lab.json"
        --plan "${collecting_plan_path}"
        --timeout-seconds 10
    RESULT_VARIABLE collecting_resume_result
    OUTPUT_VARIABLE collecting_resume_output
    ERROR_VARIABLE collecting_resume_error
)
if(NOT collecting_resume_result EQUAL 0)
    message(FATAL_ERROR
        "Persisted collecting_evidence orchestration did not resume: "
        "${collecting_resume_error}\n${collecting_resume_output}")
endif()
string(FIND "${collecting_resume_output}" "\"resumed\": true" collecting_resumed_position)
string(FIND "${collecting_resume_output}" "\"status\": \"COMPLETED\"" collecting_status_position)
if(collecting_resumed_position EQUAL -1 OR collecting_status_position EQUAL -1 OR
   NOT EXISTS "${collecting_archive}/evidence/main/task.json")
    message(FATAL_ERROR
        "Persisted collecting_evidence resume omitted evidence: ${collecting_resume_output}")
endif()

# starting_vm 可能已经产生外部副作用，Host 重启后必须保持人工门禁。
string(REPLACE "orchestration_executing_resume" "orchestration_unsafe_resume"
    unsafe_resume_plan "${executing_resume_plan}")
set(unsafe_plan_path "${TEST_ROOT}/orchestration-unsafe-resume.json")
set(unsafe_archive "${archive_path}/runs/orchestration_unsafe_resume")
set(unsafe_state "${unsafe_archive}/lifecycle.json")
file(WRITE "${unsafe_plan_path}" "${unsafe_resume_plan}")
file(MAKE_DIRECTORY "${unsafe_archive}")
file(COPY_FILE "${unsafe_plan_path}" "${unsafe_archive}/plan.json")
file(SHA256 "${unsafe_plan_path}" unsafe_plan_sha256)
file(WRITE "${unsafe_archive}/orchestration.json" [=[
{
  "schema_version": 1,
  "run_id": "orchestration_unsafe_resume",
  "vm_id": "vm_01",
  "main_run_id": "orchestration_unsafe_resume",
  "finally_run_id": "finally_unsafe_resume",
  "plan_sha256": "@PLAN_SHA256@"
}
]=])
file(READ "${unsafe_archive}/orchestration.json" unsafe_identity_json)
string(REPLACE "@PLAN_SHA256@" "${unsafe_plan_sha256}"
    unsafe_identity_json "${unsafe_identity_json}")
file(WRITE "${unsafe_archive}/orchestration.json" "${unsafe_identity_json}")
file(WRITE "${unsafe_state}" [=[
{
  "schema_version": 1,
  "run_id": "orchestration_unsafe_resume",
  "phase": "starting_vm",
  "sequence": 1,
  "updated_at": "2026-07-27T00:00:01.000Z",
  "transitions": [
    {
      "sequence": 1,
      "from": "preparing",
      "to": "starting_vm",
      "occurred_at": "2026-07-27T00:00:01.000Z",
      "message": "start target VM"
    }
  ]
}
]=])
execute_process(
    COMMAND "${HOST_EXE}" orchestrate
        --config "${TEST_ROOT}/lab.json"
        --plan "${unsafe_plan_path}"
        --timeout-seconds 10
    RESULT_VARIABLE unsafe_resume_result
    OUTPUT_VARIABLE unsafe_resume_output
    ERROR_VARIABLE unsafe_resume_error
)
if(NOT unsafe_resume_result EQUAL 5)
    message(FATAL_ERROR
        "Unsafe persisted phase did not return the manual gate: "
        "${unsafe_resume_error}\n${unsafe_resume_output}")
endif()
string(FIND "${unsafe_resume_output}"
    "\"status\": \"MANUAL_INTERVENTION_REQUIRED\"" unsafe_resume_status_position)
file(READ "${unsafe_state}" unsafe_state_json)
string(FIND "${unsafe_state_json}"
    "\"phase\": \"manual_intervention_required\"" unsafe_state_position)
if(unsafe_resume_status_position EQUAL -1 OR unsafe_state_position EQUAL -1)
    message(FATAL_ERROR "Unsafe persisted phase did not preserve its manual gate")
endif()
force_unlock_lab("Unsafe persisted phase")

set(orchestration_failure_json [=[
{
  "schema_version": 3,
  "name": "host-lifecycle-business-failure",
  "run_id": "orchestration_failure",
  "artifacts": [
    {
      "source": "@FIXTURE@",
      "vm": "vm_01",
      "destination": "artifacts/vm_01/SatsumaTestFixture.exe"
    }
  ],
  "steps": [
    {
      "id": "timeout",
      "vm": "vm_01",
      "type": "execute",
      "program": "artifacts/vm_01/SatsumaTestFixture.exe",
      "arguments": ["--sleep-ms", "3000"],
      "timeout_seconds": 1
    }
  ],
  "lifecycle": {
    "vms": [
      {
        "vm": "vm_01",
        "on_success": {"action": "stop"},
        "on_failure": {"action": "restore", "snapshot": "clean"}
      }
    ]
  }
}
]=])
string(REPLACE "@FIXTURE@" "${fixture_path}" orchestration_failure_json "${orchestration_failure_json}")
file(WRITE "${TEST_ROOT}/orchestration-failure.json" "${orchestration_failure_json}")
set(failure_state "${archive_path}/runs/orchestration_failure/lifecycle.json")

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DVM_EXE=${VM_EXE}"
        "-DAGENT_CONFIG=${TEST_ROOT}/agent.json"
        "-DLIFECYCLE_STATE=${failure_state}"
        -P "${CMAKE_CURRENT_LIST_DIR}/run_agent_until_lifecycle_terminal.cmake"
    COMMAND "${HOST_EXE}" orchestrate
        --config "${TEST_ROOT}/lab.json"
        --plan "${TEST_ROOT}/orchestration-failure.json"
        --timeout-seconds 10
    RESULTS_VARIABLE orchestration_failure_results
    OUTPUT_VARIABLE orchestration_failure_output
    ERROR_VARIABLE orchestration_failure_error
)
if(NOT orchestration_failure_results STREQUAL "0;1")
    message(FATAL_ERROR
        "SatsumaHost did not distinguish business failure (${orchestration_failure_results}): "
        "${orchestration_failure_error}\n${orchestration_failure_output}")
endif()
string(FIND "${orchestration_failure_output}" "\"status\": \"FAILED\"" failure_status_position)
string(FIND "${orchestration_failure_output}" "\"cleanup_action\": \"restore\"" failure_cleanup_position)
if(failure_status_position EQUAL -1 OR failure_cleanup_position EQUAL -1)
    message(FATAL_ERROR "SatsumaHost did not report recovered business failure: ${orchestration_failure_output}")
endif()
file(READ "${failure_state}" failure_state_json)
string(FIND "${failure_state_json}" "\"phase\": \"failed\"" failed_phase_position)
if(failed_phase_position EQUAL -1)
    message(FATAL_ERROR "Business failure lifecycle did not reach failed: ${failure_state_json}")
endif()

string(REPLACE
    "\"snapshot\": \"clean\""
    "\"snapshot\": \"satsuma-ai-recovery-fail\""
    recovery_failure_json
    "${orchestration_failure_json}")
string(REPLACE
    "\"run_id\": \"orchestration_failure\""
    "\"run_id\": \"orchestration_recovery_failure\""
    recovery_failure_json
    "${recovery_failure_json}")
file(WRITE "${TEST_ROOT}/orchestration-recovery-failure.json" "${recovery_failure_json}")
set(recovery_failure_state "${archive_path}/runs/orchestration_recovery_failure/lifecycle.json")

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DVM_EXE=${VM_EXE}"
        "-DAGENT_CONFIG=${TEST_ROOT}/agent.json"
        "-DLIFECYCLE_STATE=${recovery_failure_state}"
        -P "${CMAKE_CURRENT_LIST_DIR}/run_agent_until_lifecycle_terminal.cmake"
    COMMAND "${HOST_EXE}" orchestrate
        --config "${TEST_ROOT}/lab.json"
        --plan "${TEST_ROOT}/orchestration-recovery-failure.json"
        --timeout-seconds 10
    RESULTS_VARIABLE recovery_failure_results
    OUTPUT_VARIABLE recovery_failure_output
    ERROR_VARIABLE recovery_failure_error
)
if(NOT recovery_failure_results STREQUAL "0;4")
    message(FATAL_ERROR
        "SatsumaHost did not classify recovery failure (${recovery_failure_results}): "
        "${recovery_failure_error}\n${recovery_failure_output}")
endif()
string(FIND "${recovery_failure_output}" "\"status\": \"RECOVERY_FAILED\"" recovery_status_position)
if(recovery_status_position EQUAL -1)
    message(FATAL_ERROR "SatsumaHost omitted RECOVERY_FAILED: ${recovery_failure_output}")
endif()
file(READ "${recovery_failure_state}" recovery_failure_state_json)
string(FIND
    "${recovery_failure_state_json}"
    "\"phase\": \"recovery_failed\""
    recovery_failed_phase_position)
if(recovery_failed_phase_position EQUAL -1)
    message(FATAL_ERROR "Recovery failure lifecycle omitted terminal state: ${recovery_failure_state_json}")
endif()
force_unlock_lab("Recovery failure")

set(manual_gate_task [=[
{
  "schema_version": 3,
  "name": "host-lifecycle-manual-gate",
  "run_id": "orchestration_manual_gate",
  "steps": [
    {
      "id": "main_echo",
      "vm": "vm_01",
      "type": "echo",
      "message": "must remain blocked",
      "retry_safe": false
    }
  ],
  "lifecycle": {
    "vms": [
      {
        "vm": "vm_01",
        "on_success": {"action": "stop"},
        "on_failure": {"action": "restore", "snapshot": "clean"}
      }
    ],
    "finally": [
      {
        "id": "must_not_run",
        "vm": "vm_01",
        "type": "echo",
        "message": "unsafe finally"
      }
    ]
  }
}
]=])
file(WRITE "${TEST_ROOT}/orchestration-manual-gate.json" "${manual_gate_task}")
set(manual_gate_state "${archive_path}/runs/orchestration_manual_gate/lifecycle.json")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DVM_EXE=${VM_EXE}"
        "-DAGENT_CONFIG=${TEST_ROOT}/agent.json"
        "-DSTATE_ROOT=${state_path}"
        "-DRUN_ID=orchestration_manual_gate"
        "-DLIFECYCLE_STATE=${manual_gate_state}"
        -P "${CMAKE_CURRENT_LIST_DIR}/inject_expired_claim_and_drive_agent.cmake"
    COMMAND "${HOST_EXE}" orchestrate
        --config "${TEST_ROOT}/lab.json"
        --plan "${TEST_ROOT}/orchestration-manual-gate.json"
        --timeout-seconds 10
    RESULTS_VARIABLE manual_gate_results
    OUTPUT_VARIABLE manual_gate_output
    ERROR_VARIABLE manual_gate_error
)
if(NOT manual_gate_results STREQUAL "0;5")
    message(FATAL_ERROR
        "SatsumaHost did not stop at the manual gate (${manual_gate_results}): "
        "${manual_gate_error}\n${manual_gate_output}")
endif()
string(FIND
    "${manual_gate_output}"
    "\"status\": \"MANUAL_INTERVENTION_REQUIRED\""
    manual_gate_status_position)
if(manual_gate_status_position EQUAL -1)
    message(FATAL_ERROR "SatsumaHost omitted manual gate status: ${manual_gate_output}")
endif()
file(READ "${manual_gate_state}" manual_gate_state_json)
string(FIND
    "${manual_gate_state_json}"
    "\"phase\": \"manual_intervention_required\""
    manual_gate_phase_position)
if(manual_gate_phase_position EQUAL -1)
    message(FATAL_ERROR "Lifecycle did not persist the manual gate: ${manual_gate_state_json}")
endif()
if(EXISTS "${archive_path}/runs/orchestration_manual_gate/evidence/finally")
    message(FATAL_ERROR "SatsumaHost executed finally after an unsafe claim gate")
endif()
force_unlock_lab("Manual intervention")

set(claim_task_template [=[
{
  "schema_version": 2,
  "protocol_version": 4,
  "lab_id": "integration_lab",
  "run_id": "@RUN_ID@",
  "name": "claim recovery",
  "created_at": "2026-07-26T00:00:00.000Z",
  "artifacts": [],
  "steps": [
    {
      "id": "echo",
      "vm": "vm_01",
      "type": "echo",
      "message": "claim recovery",
      "retry_safe": @RETRY_SAFE@
    }
  ]
}
]=])
set(claim_template [=[
{
  "schema_version": 3,
  "run_id": "@RUN_ID@",
  "vm_id": "vm_01",
  "step_id": "echo",
  "job_id": "job_old",
  "session_id": "session_old",
  "boot_id": "boot_old",
  "claimed_at": "2026-07-26T00:00:00.000Z",
  "claimed_unix_ms": 1000,
  "last_renewed_at": "2026-07-26T00:00:00.000Z",
  "last_renewed_unix_ms": 1000,
  "lease_expires_unix_ms": 2000,
  "renewal_sequence": 0,
  "retry_safe": @RETRY_SAFE@,
  "attempt": 1
}
]=])

set(safe_claim_run "claim_safe_recovery")
string(REPLACE "@RUN_ID@" "${safe_claim_run}" safe_claim_task "${claim_task_template}")
string(REPLACE "@RETRY_SAFE@" "true" safe_claim_task "${safe_claim_task}")
string(REPLACE "@RUN_ID@" "${safe_claim_run}" safe_claim "${claim_template}")
string(REPLACE "@RETRY_SAFE@" "true" safe_claim "${safe_claim}")
file(MAKE_DIRECTORY "${state_path}/runs/${safe_claim_run}/state/vm_01")
file(WRITE "${state_path}/runs/${safe_claim_run}/task.json" "${safe_claim_task}")
file(WRITE "${state_path}/runs/${safe_claim_run}/state/vm_01/echo.claim.json" "${safe_claim}")
execute_process(
    COMMAND "${VM_EXE}" --config "${TEST_ROOT}/agent.json" --once
    RESULT_VARIABLE safe_claim_result
    OUTPUT_VARIABLE safe_claim_output
    ERROR_VARIABLE safe_claim_error
)
if(NOT safe_claim_result EQUAL 0)
    message(FATAL_ERROR "Safe claim recovery failed: ${safe_claim_error}\n${safe_claim_output}")
endif()
set(safe_execution "${state_path}/runs/${safe_claim_run}/results/vm_01/echo/execution.json")
if(NOT EXISTS "${safe_execution}")
    message(FATAL_ERROR "Expired safe claim was not executed")
endif()
file(READ "${state_path}/runs/${safe_claim_run}/state/vm_01/echo.claim.json" safe_claim_after)
string(FIND "${safe_claim_after}" "\"attempt\": 2" safe_attempt_position)
if(safe_attempt_position EQUAL -1)
    message(FATAL_ERROR "Recovered safe claim did not increment attempt: ${safe_claim_after}")
endif()
set(safe_expired_claim
    "${state_path}/runs/${safe_claim_run}/state/vm_01/echo.claim.json.attempt-1.json")
if(NOT EXISTS "${safe_expired_claim}")
    message(FATAL_ERROR "Safe claim recovery did not preserve the expired claim")
endif()

set(unsafe_claim_run "claim_unsafe_recovery")
string(REPLACE "@RUN_ID@" "${unsafe_claim_run}" unsafe_claim_task "${claim_task_template}")
string(REPLACE "@RETRY_SAFE@" "false" unsafe_claim_task "${unsafe_claim_task}")
string(REPLACE "@RUN_ID@" "${unsafe_claim_run}" unsafe_claim "${claim_template}")
string(REPLACE "@RETRY_SAFE@" "false" unsafe_claim "${unsafe_claim}")
file(MAKE_DIRECTORY "${state_path}/runs/${unsafe_claim_run}/state/vm_01")
file(WRITE "${state_path}/runs/${unsafe_claim_run}/task.json" "${unsafe_claim_task}")
file(WRITE "${state_path}/runs/${unsafe_claim_run}/state/vm_01/echo.claim.json" "${unsafe_claim}")
execute_process(
    COMMAND "${VM_EXE}" --config "${TEST_ROOT}/agent.json" --once
    RESULT_VARIABLE unsafe_claim_result
    OUTPUT_VARIABLE unsafe_claim_output
    ERROR_VARIABLE unsafe_claim_error
)
if(NOT unsafe_claim_result EQUAL 0)
    message(FATAL_ERROR "Unsafe claim audit failed: ${unsafe_claim_error}\n${unsafe_claim_output}")
endif()
if(EXISTS "${state_path}/runs/${unsafe_claim_run}/results/vm_01/echo/execution.json")
    message(FATAL_ERROR "Expired unsafe claim was executed without manual approval")
endif()
set(unsafe_recovery
    "${state_path}/runs/${unsafe_claim_run}/state/vm_01/echo.claim-recovery.json")
if(NOT EXISTS "${unsafe_recovery}")
    message(FATAL_ERROR "Expired unsafe claim did not publish a recovery gate")
endif()
file(READ "${unsafe_recovery}" unsafe_recovery_json)
string(FIND
    "${unsafe_recovery_json}"
    "\"status\": \"manual_intervention_required\""
    unsafe_gate_position)
if(unsafe_gate_position EQUAL -1)
    message(FATAL_ERROR "Unsafe claim recovery gate has an unexpected format: ${unsafe_recovery_json}")
endif()
execute_process(
    COMMAND "${HOST_EXE}" report
        --config "${TEST_ROOT}/lab.json"
        --run "${unsafe_claim_run}"
    RESULT_VARIABLE unsafe_report_result
    OUTPUT_VARIABLE unsafe_report_output
    ERROR_VARIABLE unsafe_report_error
)
if(NOT unsafe_report_result EQUAL 5)
    message(FATAL_ERROR "Unsafe claim report failed: ${unsafe_report_error}\n${unsafe_report_output}")
endif()
string(FIND
    "${unsafe_report_output}"
    "\"manual_intervention_required\": true"
    unsafe_report_gate_position)
if(unsafe_report_gate_position EQUAL -1)
    message(FATAL_ERROR "Host report omitted unsafe claim gate: ${unsafe_report_output}")
endif()
execute_process(
    COMMAND "${HOST_EXE}" report
        --config "${TEST_ROOT}/lab.json"
        --run "${unsafe_claim_run}"
        --wait-seconds 1
    RESULT_VARIABLE unsafe_wait_result
    OUTPUT_VARIABLE unsafe_wait_output
    ERROR_VARIABLE unsafe_wait_error
)
if(NOT unsafe_wait_result EQUAL 5)
    message(FATAL_ERROR "Report wait ignored the manual gate: ${unsafe_wait_error}\n${unsafe_wait_output}")
endif()
string(FIND
    "${unsafe_wait_output}"
    "\"wait_status\": \"manual_intervention_required\""
    unsafe_wait_status_position)
if(unsafe_wait_status_position EQUAL -1)
    message(FATAL_ERROR "Report wait omitted manual status: ${unsafe_wait_output}")
endif()

set(pending_run "claim_wait_timeout")
string(REPLACE "@RUN_ID@" "${pending_run}" pending_task "${claim_task_template}")
string(REPLACE "@RETRY_SAFE@" "true" pending_task "${pending_task}")
string(REPLACE "@RUN_ID@" "${pending_run}" pending_claim "${claim_template}")
string(REPLACE "@RETRY_SAFE@" "true" pending_claim "${pending_claim}")
string(REPLACE
    "\"lease_expires_unix_ms\": 2000"
    "\"lease_expires_unix_ms\": 4102444800000"
    pending_claim
    "${pending_claim}")
file(MAKE_DIRECTORY "${state_path}/runs/${pending_run}/state/vm_01")
file(WRITE "${state_path}/runs/${pending_run}/task.json" "${pending_task}")
file(WRITE "${state_path}/runs/${pending_run}/state/vm_01/echo.claim.json" "${pending_claim}")
execute_process(
    COMMAND "${HOST_EXE}" report
        --config "${TEST_ROOT}/lab.json"
        --run "${pending_run}"
        --wait-seconds 1
    RESULT_VARIABLE pending_wait_result
    OUTPUT_VARIABLE pending_wait_output
    ERROR_VARIABLE pending_wait_error
)
if(NOT pending_wait_result EQUAL 3)
    message(FATAL_ERROR "Report wait did not time out: ${pending_wait_error}\n${pending_wait_output}")
endif()
string(FIND "${pending_wait_output}" "\"wait_status\": \"timeout\"" pending_wait_status_position)
if(pending_wait_status_position EQUAL -1)
    message(FATAL_ERROR "Report wait omitted timeout status: ${pending_wait_output}")
endif()

set(ENV{SATSUMA_FAKE_VMRUN_RUNNING_VMX} "${vmx_path}")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DVM_EXE=${VM_EXE}"
        "-DAGENT_CONFIG=${TEST_ROOT}/agent.json"
        -P "${CMAKE_CURRENT_LIST_DIR}/run_agent_once_after_delay.cmake"
    COMMAND "${HOST_EXE}" check
        --config "${TEST_ROOT}/lab.json"
        --vm vm_01
        --timeout-seconds 10
    RESULTS_VARIABLE check_results
    OUTPUT_VARIABLE check_output
    ERROR_VARIABLE check_error
)
if(NOT check_results STREQUAL "0;0")
    message(FATAL_ERROR "SatsumaHost active check failed (${check_results}): ${check_error}\n${check_output}")
endif()
string(FIND "${check_output}" "\"mode\": \"full\"" check_mode_position)
if(check_mode_position EQUAL -1)
    message(FATAL_ERROR "SatsumaHost active check did not return a full report: ${check_output}")
endif()
string(FIND "${check_output}" "\"status\": \"ready\"" check_status_position)
if(check_status_position EQUAL -1)
    message(FATAL_ERROR "SatsumaHost active check returned unexpected output: ${check_output}")
endif()
string(FIND "${check_output}" "\"status\": \"passed\"" check_agent_position)
if(check_agent_position EQUAL -1)
    message(FATAL_ERROR "SatsumaHost active check did not confirm the Agent: ${check_output}")
endif()
string(FIND "${check_output}" "\"name\": \"vmware_tools\"" check_tools_position)
string(FIND "${check_output}" "\"state\": \"running\"" check_tools_state_position)
string(FIND "${check_output}" "\"available_bytes\":" check_capacity_position)
if(check_tools_position EQUAL -1 OR
   check_tools_state_position EQUAL -1 OR
   check_capacity_position EQUAL -1)
    message(FATAL_ERROR "SatsumaHost active check omitted Tools or capacity details: ${check_output}")
endif()
file(GLOB diagnostic_claims "${state_path}/runs/check-*/state/vm_01/vm_01.claim.json")
list(LENGTH diagnostic_claims diagnostic_claim_count)
if(diagnostic_claim_count LESS 1)
    message(FATAL_ERROR "SatsumaHost active check did not create a diagnostic claim")
endif()
list(GET diagnostic_claims -1 diagnostic_claim_path)
file(READ "${diagnostic_claim_path}" diagnostic_claim_json)
string(FIND "${diagnostic_claim_json}" "\"retry_safe\": true" diagnostic_retry_position)
if(diagnostic_retry_position EQUAL -1)
    message(FATAL_ERROR "Diagnostic echo claim was not marked retry-safe: ${diagnostic_claim_json}")
endif()

# Host 状态目录不可用时仍返回完整机器可读失败报告。
file(WRITE "${TEST_ROOT}/blocked-state" "not a directory\n")
file(TO_CMAKE_PATH "${TEST_ROOT}/blocked-state" blocked_state_path)
string(REPLACE "${state_path}" "${blocked_state_path}" blocked_state_lab_json "${lab_json}")
file(WRITE "${TEST_ROOT}/lab-blocked-state.json" "${blocked_state_lab_json}")
execute_process(
    COMMAND "${HOST_EXE}" check
        --config "${TEST_ROOT}/lab-blocked-state.json"
        --vm vm_01
        --timeout-seconds 2
    RESULT_VARIABLE blocked_state_result
    OUTPUT_VARIABLE blocked_state_output
    ERROR_VARIABLE blocked_state_error
)
if(NOT blocked_state_result EQUAL 1)
    message(FATAL_ERROR
        "Blocked transport-state check returned ${blocked_state_result}: "
        "${blocked_state_error}\n${blocked_state_output}")
endif()
string(FIND "${blocked_state_output}" "\"mode\": \"full\"" blocked_state_mode_position)
string(FIND "${blocked_state_output}" "\"run_id\": null" blocked_state_run_position)
string(FIND
    "${blocked_state_output}"
    "Agent diagnostic was skipped because the transport state is unavailable"
    blocked_state_skip_position)
if(blocked_state_mode_position EQUAL -1 OR
   blocked_state_run_position EQUAL -1 OR
   blocked_state_skip_position EQUAL -1)
    message(FATAL_ERROR "Blocked transport-state check lost its JSON report: ${blocked_state_output}")
endif()
execute_process(
    COMMAND "${HOST_EXE}" lab status --config "${TEST_ROOT}/lab-blocked-state.json"
    RESULT_VARIABLE blocked_state_status_result
    OUTPUT_VARIABLE blocked_state_status_output
    ERROR_VARIABLE blocked_state_status_error
)
if(NOT blocked_state_status_result EQUAL 0 OR
   NOT blocked_state_status_output MATCHES "\"status\": \"available\"")
    message(FATAL_ERROR
        "Failed diagnostic retained the lab lease: "
        "${blocked_state_status_error}\n${blocked_state_status_output}")
endif()

# 归档根不可用时无法创建持久租约，必须在发布诊断任务前失败。
file(WRITE "${TEST_ROOT}/blocked-archive" "not a directory\n")
file(TO_CMAKE_PATH "${TEST_ROOT}/blocked-archive" blocked_archive_path)
string(REPLACE "${archive_path}" "${blocked_archive_path}" blocked_archive_lab_json "${lab_json}")
file(WRITE "${TEST_ROOT}/lab-blocked-archive.json" "${blocked_archive_lab_json}")
execute_process(
    COMMAND "${HOST_EXE}" check
        --config "${TEST_ROOT}/lab-blocked-archive.json"
        --vm vm_01
        --timeout-seconds 10
    RESULT_VARIABLE blocked_archive_result
    OUTPUT_VARIABLE blocked_archive_output
    ERROR_VARIABLE blocked_archive_error
)
if(NOT blocked_archive_result EQUAL 1 OR
   NOT blocked_archive_error MATCHES "coordination")
    message(FATAL_ERROR
        "Blocked archive check returned ${blocked_archive_result}: "
        "${blocked_archive_error}\n${blocked_archive_output}")
endif()
if(NOT blocked_archive_output STREQUAL "")
    message(FATAL_ERROR "Blocked archive check published unexpected output: ${blocked_archive_output}")
endif()

unset(ENV{SATSUMA_FAKE_VMRUN_RUNNING_VMX})
execute_process(
    COMMAND "${HOST_EXE}" vm start --config "${TEST_ROOT}/lab.json" --id vm_01
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE vm_start_result
    OUTPUT_VARIABLE vm_start_output
    ERROR_VARIABLE vm_start_error
)
if(NOT vm_start_result EQUAL 0)
    message(FATAL_ERROR "SatsumaHost vm start failed: ${vm_start_error}\n${vm_start_output}")
endif()
string(FIND "${vm_start_output}" "\"status\": \"started\"" vm_start_position)
if(vm_start_position EQUAL -1)
    message(FATAL_ERROR "SatsumaHost vm start returned unexpected output: ${vm_start_output}")
endif()

execute_process(
    COMMAND "${HOST_EXE}" vm stop --config "${TEST_ROOT}/lab.json" --id vm_01
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE vm_stop_result
    OUTPUT_VARIABLE vm_stop_output
    ERROR_VARIABLE vm_stop_error
)
if(NOT vm_stop_result EQUAL 0)
    message(FATAL_ERROR "SatsumaHost vm stop failed: ${vm_stop_error}\n${vm_stop_output}")
endif()
string(FIND "${vm_stop_output}" "\"status\": \"stopped\"" vm_stop_position)
if(vm_stop_position EQUAL -1)
    message(FATAL_ERROR "SatsumaHost vm stop returned unexpected output: ${vm_stop_output}")
endif()

execute_process(
    COMMAND "${HOST_EXE}" vm stop
        --config "${TEST_ROOT}/lab.json" --id hard-stop --mode hard
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE vm_hard_stop_result
    OUTPUT_VARIABLE vm_hard_stop_output
    ERROR_VARIABLE vm_hard_stop_error
)
if(NOT vm_hard_stop_result EQUAL 0)
    message(FATAL_ERROR "SatsumaHost hard stop failed: ${vm_hard_stop_error}\n${vm_hard_stop_output}")
endif()
string(FIND "${vm_hard_stop_output}" "\"mode\": \"hard\"" vm_hard_stop_position)
if(vm_hard_stop_position EQUAL -1)
    message(FATAL_ERROR "SatsumaHost hard stop returned unexpected output: ${vm_hard_stop_output}")
endif()

execute_process(
    COMMAND "${HOST_EXE}" vm restore
        --config "${TEST_ROOT}/lab.json" --id vm_01 --snapshot clean
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE vm_restore_result
    OUTPUT_VARIABLE vm_restore_output
    ERROR_VARIABLE vm_restore_error
)
if(NOT vm_restore_result EQUAL 0)
    message(FATAL_ERROR "SatsumaHost vm restore failed: ${vm_restore_error}\n${vm_restore_output}")
endif()
string(FIND "${vm_restore_output}" "\"status\": \"restored\"" vm_restore_position)
if(vm_restore_position EQUAL -1)
    message(FATAL_ERROR "SatsumaHost vm restore returned unexpected output: ${vm_restore_output}")
endif()

execute_process(
    COMMAND "${HOST_EXE}" snapshot list --config "${TEST_ROOT}/lab.json" --vm vm_01
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE snapshot_list_result
    OUTPUT_VARIABLE snapshot_list_output
    ERROR_VARIABLE snapshot_list_error
)
if(NOT snapshot_list_result EQUAL 0)
    message(FATAL_ERROR "SatsumaHost snapshot list failed: ${snapshot_list_error}\n${snapshot_list_output}")
endif()
string(FIND "${snapshot_list_output}" "\"ownership\": \"user_base\"" snapshot_list_position)
if(snapshot_list_position EQUAL -1)
    message(FATAL_ERROR "SatsumaHost snapshot list did not protect the base snapshot: ${snapshot_list_output}")
endif()

execute_process(
    COMMAND "${HOST_EXE}" snapshot delete-ai
        --config "${TEST_ROOT}/lab.json" --vm vm_01 --snapshot clean
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE base_delete_result
    OUTPUT_VARIABLE base_delete_output
    ERROR_VARIABLE base_delete_error
)
if(base_delete_result EQUAL 0)
    message(FATAL_ERROR "SatsumaHost allowed deletion of the user base snapshot: ${base_delete_output}")
endif()

execute_process(
    COMMAND "${HOST_EXE}" snapshot create-ai
        --config "${TEST_ROOT}/lab.json" --vm vm_01 --name network-ready
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE snapshot_create_result
    OUTPUT_VARIABLE snapshot_create_output
    ERROR_VARIABLE snapshot_create_error
)
if(NOT snapshot_create_result EQUAL 0)
    message(FATAL_ERROR "SatsumaHost snapshot create-ai failed: ${snapshot_create_error}\n${snapshot_create_output}")
endif()
string(FIND "${snapshot_create_output}" "\"status\": \"created\"" snapshot_create_position)
if(snapshot_create_position EQUAL -1)
    message(FATAL_ERROR "SatsumaHost snapshot create-ai returned unexpected output: ${snapshot_create_output}")
endif()
file(GLOB snapshot_metadata "${archive_path}/snapshots/vm_01/*.json")
list(LENGTH snapshot_metadata snapshot_metadata_count)
if(NOT snapshot_metadata_count EQUAL 1)
    message(FATAL_ERROR "SatsumaHost did not create exactly one snapshot metadata record")
endif()
list(GET snapshot_metadata 0 snapshot_metadata_path)
file(READ "${snapshot_metadata_path}" snapshot_metadata_json)
string(FIND "${snapshot_metadata_json}" "\"status\": \"created\"" snapshot_metadata_position)
if(snapshot_metadata_position EQUAL -1)
    message(FATAL_ERROR "Snapshot metadata did not record successful creation: ${snapshot_metadata_json}")
endif()

execute_process(
    COMMAND "${HOST_EXE}" snapshot delete-ai
        --config "${TEST_ROOT}/lab.json" --vm vm_01 --snapshot satsuma-ai-obsolete
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE snapshot_delete_result
    OUTPUT_VARIABLE snapshot_delete_output
    ERROR_VARIABLE snapshot_delete_error
)
if(NOT snapshot_delete_result EQUAL 0)
    message(FATAL_ERROR "SatsumaHost snapshot delete-ai failed: ${snapshot_delete_error}\n${snapshot_delete_output}")
endif()
string(FIND "${snapshot_delete_output}" "\"status\": \"deleted\"" snapshot_delete_position)
if(snapshot_delete_position EQUAL -1)
    message(FATAL_ERROR "SatsumaHost snapshot delete-ai returned unexpected output: ${snapshot_delete_output}")
endif()
set(deleted_metadata_path "${archive_path}/snapshots/vm_01/satsuma-ai-obsolete.json")
file(READ "${deleted_metadata_path}" deleted_metadata_json)
string(FIND "${deleted_metadata_json}" "\"status\": \"deleted\"" deleted_metadata_position)
if(deleted_metadata_position EQUAL -1)
    message(FATAL_ERROR "Snapshot deletion metadata was not finalized: ${deleted_metadata_json}")
endif()

# vmrun 报错但目标状态已经生效时，Host 必须通过重新读取快照列表完成对账。
execute_process(
    COMMAND "${HOST_EXE}" snapshot create-ai
        --config "${TEST_ROOT}/lab.json" --vm snapshot-reconcile --name late-success
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE reconcile_create_result
    OUTPUT_VARIABLE reconcile_create_output
    ERROR_VARIABLE reconcile_create_error
)
if(NOT reconcile_create_result EQUAL 0)
    message(FATAL_ERROR
        "SatsumaHost did not reconcile late snapshot creation: "
        "${reconcile_create_error}\n${reconcile_create_output}")
endif()
string(JSON reconcile_create_status GET "${reconcile_create_output}" status)
string(JSON reconcile_create_flag GET "${reconcile_create_output}" reconciled)
string(JSON reconcile_snapshot_name GET "${reconcile_create_output}" snapshot)
if(NOT reconcile_create_status STREQUAL "created" OR NOT reconcile_create_flag)
    message(FATAL_ERROR
        "Late snapshot creation returned unexpected output: ${reconcile_create_output}")
endif()
set(reconcile_metadata_path
    "${archive_path}/snapshots/snapshot-reconcile/${reconcile_snapshot_name}.json")
file(READ "${reconcile_metadata_path}" reconcile_metadata_json)
string(JSON reconcile_metadata_status GET "${reconcile_metadata_json}" status)
string(JSON reconcile_metadata_flag GET "${reconcile_metadata_json}" reconciled_after_error)
string(JSON reconcile_operation_error GET "${reconcile_metadata_json}" operation_error)
if(NOT reconcile_metadata_status STREQUAL "created" OR
   NOT reconcile_metadata_flag OR
   reconcile_operation_error STREQUAL "")
    message(FATAL_ERROR
        "Late snapshot creation metadata was not reconciled: ${reconcile_metadata_json}")
endif()
# 模拟旧事务残留，验证后续删除不会混入过期失败诊断。
string(JSON reconcile_metadata_json
    SET "${reconcile_metadata_json}" error "\"stale create failure\"")
string(JSON reconcile_metadata_json
    SET "${reconcile_metadata_json}" reconciliation_error "\"stale reconciliation failure\"")
file(WRITE "${reconcile_metadata_path}" "${reconcile_metadata_json}\n")

execute_process(
    COMMAND "${HOST_EXE}" snapshot delete-ai
        --config "${TEST_ROOT}/lab.json"
        --vm snapshot-reconcile
        --snapshot "${reconcile_snapshot_name}"
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE reconcile_delete_result
    OUTPUT_VARIABLE reconcile_delete_output
    ERROR_VARIABLE reconcile_delete_error
)
if(NOT reconcile_delete_result EQUAL 0)
    message(FATAL_ERROR
        "SatsumaHost did not reconcile late snapshot deletion: "
        "${reconcile_delete_error}\n${reconcile_delete_output}")
endif()
string(JSON reconcile_delete_status GET "${reconcile_delete_output}" status)
string(JSON reconcile_delete_flag GET "${reconcile_delete_output}" reconciled)
if(NOT reconcile_delete_status STREQUAL "deleted" OR NOT reconcile_delete_flag)
    message(FATAL_ERROR
        "Late snapshot deletion returned unexpected output: ${reconcile_delete_output}")
endif()
file(READ "${reconcile_metadata_path}" reconcile_deleted_metadata_json)
string(JSON reconcile_deleted_status GET "${reconcile_deleted_metadata_json}" status)
string(FIND "${reconcile_deleted_metadata_json}" "\"error\":" stale_error_position)
string(FIND
    "${reconcile_deleted_metadata_json}"
    "\"reconciliation_error\":"
    stale_reconciliation_error_position)
if(NOT reconcile_deleted_status STREQUAL "deleted" OR
   NOT stale_error_position EQUAL -1 OR
   NOT stale_reconciliation_error_position EQUAL -1)
    message(FATAL_ERROR
        "Late snapshot deletion metadata was not finalized: ${reconcile_deleted_metadata_json}")
endif()

execute_process(
    COMMAND "${HOST_EXE}" vm start --config "${TEST_ROOT}/lab.json" --id vm_01
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE task_vm_start_result
    OUTPUT_VARIABLE task_vm_start_output
    ERROR_VARIABLE task_vm_start_error
)
if(NOT task_vm_start_result EQUAL 0)
    message(FATAL_ERROR
        "SatsumaHost could not start the task VM: ${task_vm_start_error}\n${task_vm_start_output}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DVM_EXE=${VM_EXE}"
        "-DAGENT_CONFIG=${TEST_ROOT}/agent.json"
        -P "${CMAKE_CURRENT_LIST_DIR}/run_agent_once_after_delay.cmake"
    COMMAND "${HOST_EXE}" run --config "${TEST_ROOT}/lab.json" --plan "${TEST_ROOT}/task.json"
    RESULTS_VARIABLE host_results
    OUTPUT_VARIABLE host_output
    ERROR_VARIABLE host_error
)
if(NOT host_results STREQUAL "0;0")
    message(FATAL_ERROR "SatsumaHost run failed (${host_results}): ${host_error}\n${host_output}")
endif()

execute_process(
    COMMAND "${VM_EXE}" --config "${TEST_ROOT}/agent.json" --once
    RESULT_VARIABLE vm_result
    OUTPUT_VARIABLE vm_output
    ERROR_VARIABLE vm_error
)
if(NOT vm_result EQUAL 0)
    message(FATAL_ERROR "SatsumaVM failed: ${vm_error}\n${vm_output}")
endif()

execute_process(
    COMMAND "${HOST_EXE}" report
        --config "${TEST_ROOT}/lab.json"
        --run integration_run
        --wait-seconds 1
    RESULT_VARIABLE report_result
    OUTPUT_VARIABLE report_output
    ERROR_VARIABLE report_error
)
if(NOT report_result EQUAL 1)
    message(FATAL_ERROR "SatsumaHost report failed: ${report_error}\n${report_output}")
endif()
string(FIND "${report_output}" "\"wait_status\": \"completed\"" report_wait_position)
if(report_wait_position EQUAL -1)
    message(FATAL_ERROR "Completed report wait omitted status: ${report_output}")
endif()

string(FIND "${report_output}" "\"successful_steps\": 1" success_position)
if(success_position EQUAL -1)
    message(FATAL_ERROR "Host report did not contain one successful step: ${report_output}")
endif()
string(FIND "${report_output}" "\"failed_steps\": 1" failed_position)
if(failed_position EQUAL -1)
    message(FATAL_ERROR "Host report did not contain one timed-out step: ${report_output}")
endif()

set(result_root "${state_path}/runs/integration_run/results/vm_01/execute_fixture")
if(NOT EXISTS "${result_root}/execution.json" OR
   NOT EXISTS "${result_root}/stdout.log" OR
   NOT EXISTS "${result_root}/files/generated/result.json")
    message(FATAL_ERROR "Expected execution evidence was not created")
endif()

set(timeout_result "${state_path}/runs/integration_run/results/vm_01/timeout_fixture/execution.json")
if(NOT EXISTS "${timeout_result}")
    message(FATAL_ERROR "Timed-out execution evidence was not created")
endif()
file(READ "${timeout_result}" timeout_json)
string(FIND "${timeout_json}" "\"status\": \"timed_out\"" timed_out_position)
if(timed_out_position EQUAL -1)
    message(FATAL_ERROR "Process timeout was not recorded: ${timeout_json}")
endif()

execute_process(
    COMMAND "${HOST_EXE}" runs finalize
        --config "${TEST_ROOT}/lab.json"
        --run integration_run
    RESULT_VARIABLE finalize_result
    OUTPUT_VARIABLE finalize_output
    ERROR_VARIABLE finalize_error
)
if(NOT finalize_result EQUAL 0 OR NOT finalize_output MATCHES "\"status\": \"finalized\"")
    message(FATAL_ERROR "Completed run did not release its lease: ${finalize_error}\n${finalize_output}")
endif()
