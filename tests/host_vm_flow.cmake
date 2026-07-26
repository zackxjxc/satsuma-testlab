# 在本机目录中模拟共享文件夹并验证完整 Host/VM 流程。
if(NOT DEFINED HOST_EXE OR
   NOT DEFINED VM_EXE OR
   NOT DEFINED FIXTURE_EXE OR
   NOT DEFINED VMRUN_EXE OR
   NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "Host/VM integration test arguments are incomplete")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/share" "${TEST_ROOT}/local" "${TEST_ROOT}/archive")

file(TO_CMAKE_PATH "${TEST_ROOT}/share" share_path)
file(TO_CMAKE_PATH "${TEST_ROOT}/local" local_path)
file(TO_CMAKE_PATH "${TEST_ROOT}/archive" archive_path)
file(TO_CMAKE_PATH "${FIXTURE_EXE}" fixture_path)
file(TO_CMAKE_PATH "${VMRUN_EXE}" vmrun_path)
file(TO_CMAKE_PATH "${TEST_ROOT}/Client VM.vmx" vmx_path)
file(TO_CMAKE_PATH "${TEST_ROOT}/Hard VM.vmx" hard_vmx_path)
file(WRITE "${vmx_path}" "# Test VMX placeholder\n")
file(WRITE "${hard_vmx_path}" "# Test VMX placeholder\n")

set(lab_json [=[
{
  "schema_version": 1,
  "lab_id": "integration_lab",
  "provider": {
    "type": "vmware_workstation",
    "vmrun": "@VMRUN@"
  },
  "host": {
    "listen": "127.0.0.1:37100",
    "archive_root": "@ARCHIVE@"
  },
  "shared_folder": {
    "host_root": "@SHARE@",
    "guest_root": "@SHARE@"
  },
  "vms": [
    {
      "id": "client",
      "role": "client",
      "vmx": "@VMX@",
      "agent_version": "0.1.0",
      "snapshots": {
        "base": "clean",
        "ai_prefix": "satsuma-ai-",
        "max_ai_snapshots": 8
      },
      "management_ip": "127.0.0.1"
    },
    {
      "id": "hard-stop",
      "role": "hard-stop-test",
      "vmx": "@HARD_VMX@",
      "agent_version": "0.1.0",
      "snapshots": {
        "base": "clean",
        "ai_prefix": "satsuma-ai-",
        "max_ai_snapshots": 8
      },
      "management_ip": "127.0.0.2"
    }
  ]
}
]=])
string(REPLACE "@ARCHIVE@" "${archive_path}" lab_json "${lab_json}")
string(REPLACE "@SHARE@" "${share_path}" lab_json "${lab_json}")
string(REPLACE "@VMRUN@" "${vmrun_path}" lab_json "${lab_json}")
string(REPLACE "@VMX@" "${vmx_path}" lab_json "${lab_json}")
string(REPLACE "@HARD_VMX@" "${hard_vmx_path}" lab_json "${lab_json}")
file(WRITE "${TEST_ROOT}/lab.json" "${lab_json}")

set(agent_json [=[
{
  "schema_version": 1,
  "protocol_version": 1,
  "lab_id": "integration_lab",
  "vm_id": "client",
  "agent_version": "0.1.0",
  "host": "127.0.0.1:37100",
  "shared_root": "@SHARE@",
  "local_work_root": "@LOCAL@",
  "poll_interval_ms": 100,
  "reconnect_interval_ms": 100,
  "rpc_timeout_ms": 1000
}
]=])
string(REPLACE "@SHARE@" "${share_path}" agent_json "${agent_json}")
string(REPLACE "@LOCAL@" "${local_path}" agent_json "${agent_json}")
file(WRITE "${TEST_ROOT}/agent.json" "${agent_json}")

set(task_json [=[
{
  "schema_version": 1,
  "name": "host-vm-integration",
  "run_id": "integration_run",
  "artifacts": [
    {
      "source": "@FIXTURE@",
      "vm": "client",
      "shared_destination": "artifacts/client/SatsumaTestFixture.exe"
    }
  ],
  "steps": [
    {
      "id": "execute_fixture",
      "vm": "client",
      "type": "execute",
      "program": "artifacts/client/SatsumaTestFixture.exe",
      "arguments": ["--message", "hello from fixture", "--output", "generated/result.json"],
      "timeout_seconds": 10,
      "collect_files": ["generated/result.json"]
    },
    {
      "id": "timeout_fixture",
      "vm": "client",
      "type": "execute",
      "program": "artifacts/client/SatsumaTestFixture.exe",
      "arguments": ["--sleep-ms", "3000"],
      "timeout_seconds": 1,
      "collect_files": []
    }
  ]
}
]=])
string(REPLACE "@FIXTURE@" "${fixture_path}" task_json "${task_json}")
file(WRITE "${TEST_ROOT}/task.json" "${task_json}")

set(lifecycle_task_json [=[
{
  "schema_version": 1,
  "name": "lifecycle-requires-orchestrator",
  "steps": [
    {
      "id": "echo",
      "vm": "client",
      "type": "echo",
      "message": "must not run"
    }
  ],
  "lifecycle": {
    "vms": [
      {
        "vm": "client",
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

set(orchestration_task_json [=[
{
  "schema_version": 1,
  "name": "host-lifecycle-integration",
  "run_id": "orchestration_run",
  "steps": [
    {
      "id": "main_echo",
      "vm": "client",
      "type": "echo",
      "message": "main lifecycle step"
    }
  ],
  "lifecycle": {
    "vms": [
      {
        "vm": "client",
        "restore_before": "clean",
        "on_success": {"action": "stop"},
        "on_failure": {"action": "restore", "snapshot": "clean"}
      }
    ],
    "finally": [
      {
        "id": "finally_echo",
        "vm": "client",
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

set(orchestration_failure_json [=[
{
  "schema_version": 1,
  "name": "host-lifecycle-business-failure",
  "run_id": "orchestration_failure",
  "artifacts": [
    {
      "source": "@FIXTURE@",
      "vm": "client",
      "shared_destination": "artifacts/client/SatsumaTestFixture.exe"
    }
  ],
  "steps": [
    {
      "id": "timeout",
      "vm": "client",
      "type": "execute",
      "program": "artifacts/client/SatsumaTestFixture.exe",
      "arguments": ["--sleep-ms", "3000"],
      "timeout_seconds": 1
    }
  ],
  "lifecycle": {
    "vms": [
      {
        "vm": "client",
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

set(manual_gate_task [=[
{
  "schema_version": 1,
  "name": "host-lifecycle-manual-gate",
  "run_id": "orchestration_manual_gate",
  "steps": [
    {
      "id": "main_echo",
      "vm": "client",
      "type": "echo",
      "message": "must remain blocked",
      "retry_safe": false
    }
  ],
  "lifecycle": {
    "vms": [
      {
        "vm": "client",
        "on_success": {"action": "stop"},
        "on_failure": {"action": "restore", "snapshot": "clean"}
      }
    ],
    "finally": [
      {
        "id": "must_not_run",
        "vm": "client",
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
        "-DSHARED_ROOT=${share_path}"
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

set(claim_task_template [=[
{
  "schema_version": 1,
  "protocol_version": 1,
  "lab_id": "integration_lab",
  "run_id": "@RUN_ID@",
  "request_id": "request_claim_recovery",
  "name": "claim recovery",
  "created_at": "2026-07-26T00:00:00.000Z",
  "artifacts": [],
  "steps": [
    {
      "id": "echo",
      "vm": "client",
      "type": "echo",
      "message": "claim recovery",
      "timeout_seconds": 1,
      "retry_safe": @RETRY_SAFE@
    }
  ]
}
]=])
set(claim_template [=[
{
  "schema_version": 2,
  "run_id": "@RUN_ID@",
  "vm_id": "client",
  "step_id": "echo",
  "job_id": "job_old",
  "session_id": "session_old",
  "boot_id": "boot_old",
  "claimed_at": "2026-07-26T00:00:00.000Z",
  "claimed_unix_ms": 1000,
  "lease_expires_unix_ms": 2000,
  "retry_safe": @RETRY_SAFE@,
  "attempt": 1
}
]=])

set(safe_claim_run "claim_safe_recovery")
string(REPLACE "@RUN_ID@" "${safe_claim_run}" safe_claim_task "${claim_task_template}")
string(REPLACE "@RETRY_SAFE@" "true" safe_claim_task "${safe_claim_task}")
string(REPLACE "@RUN_ID@" "${safe_claim_run}" safe_claim "${claim_template}")
string(REPLACE "@RETRY_SAFE@" "true" safe_claim "${safe_claim}")
file(MAKE_DIRECTORY "${share_path}/runs/${safe_claim_run}/state/client")
file(WRITE "${share_path}/runs/${safe_claim_run}/task.json" "${safe_claim_task}")
file(WRITE "${share_path}/runs/${safe_claim_run}/state/client/echo.claim.json" "${safe_claim}")
execute_process(
    COMMAND "${VM_EXE}" --config "${TEST_ROOT}/agent.json" --once
    RESULT_VARIABLE safe_claim_result
    OUTPUT_VARIABLE safe_claim_output
    ERROR_VARIABLE safe_claim_error
)
if(NOT safe_claim_result EQUAL 0)
    message(FATAL_ERROR "Safe claim recovery failed: ${safe_claim_error}\n${safe_claim_output}")
endif()
set(safe_execution "${share_path}/runs/${safe_claim_run}/results/client/echo/execution.json")
if(NOT EXISTS "${safe_execution}")
    message(FATAL_ERROR "Expired safe claim was not executed")
endif()
file(READ "${share_path}/runs/${safe_claim_run}/state/client/echo.claim.json" safe_claim_after)
string(FIND "${safe_claim_after}" "\"attempt\": 2" safe_attempt_position)
if(safe_attempt_position EQUAL -1)
    message(FATAL_ERROR "Recovered safe claim did not increment attempt: ${safe_claim_after}")
endif()
file(GLOB safe_expired_claims
    "${share_path}/runs/${safe_claim_run}/state/client/echo.claim.json.expired-attempt-1-*")
list(LENGTH safe_expired_claims safe_expired_count)
if(NOT safe_expired_count EQUAL 1)
    message(FATAL_ERROR "Safe claim recovery did not preserve exactly one expired claim")
endif()

set(unsafe_claim_run "claim_unsafe_recovery")
string(REPLACE "@RUN_ID@" "${unsafe_claim_run}" unsafe_claim_task "${claim_task_template}")
string(REPLACE "@RETRY_SAFE@" "false" unsafe_claim_task "${unsafe_claim_task}")
string(REPLACE "@RUN_ID@" "${unsafe_claim_run}" unsafe_claim "${claim_template}")
string(REPLACE "@RETRY_SAFE@" "false" unsafe_claim "${unsafe_claim}")
file(MAKE_DIRECTORY "${share_path}/runs/${unsafe_claim_run}/state/client")
file(WRITE "${share_path}/runs/${unsafe_claim_run}/task.json" "${unsafe_claim_task}")
file(WRITE "${share_path}/runs/${unsafe_claim_run}/state/client/echo.claim.json" "${unsafe_claim}")
execute_process(
    COMMAND "${VM_EXE}" --config "${TEST_ROOT}/agent.json" --once
    RESULT_VARIABLE unsafe_claim_result
    OUTPUT_VARIABLE unsafe_claim_output
    ERROR_VARIABLE unsafe_claim_error
)
if(NOT unsafe_claim_result EQUAL 0)
    message(FATAL_ERROR "Unsafe claim audit failed: ${unsafe_claim_error}\n${unsafe_claim_output}")
endif()
if(EXISTS "${share_path}/runs/${unsafe_claim_run}/results/client/echo/execution.json")
    message(FATAL_ERROR "Expired unsafe claim was executed without manual approval")
endif()
set(unsafe_recovery
    "${share_path}/runs/${unsafe_claim_run}/state/client/echo.claim-recovery.json")
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
if(NOT unsafe_report_result EQUAL 0)
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
file(MAKE_DIRECTORY "${share_path}/runs/${pending_run}/state/client")
file(WRITE "${share_path}/runs/${pending_run}/task.json" "${pending_task}")
file(WRITE "${share_path}/runs/${pending_run}/state/client/echo.claim.json" "${pending_claim}")
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

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DVM_EXE=${VM_EXE}"
        "-DAGENT_CONFIG=${TEST_ROOT}/agent.json"
        -P "${CMAKE_CURRENT_LIST_DIR}/run_agent_once_after_delay.cmake"
    COMMAND "${HOST_EXE}" check
        --config "${TEST_ROOT}/lab.json"
        --vm client
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

execute_process(
    COMMAND "${HOST_EXE}" vm start --id client
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
    COMMAND "${HOST_EXE}" vm stop --id client
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
    COMMAND "${HOST_EXE}" vm stop --id hard-stop --mode hard
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
    COMMAND "${HOST_EXE}" vm restore --id client --snapshot clean
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
    COMMAND "${HOST_EXE}" snapshot list --vm client
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
    COMMAND "${HOST_EXE}" snapshot delete-ai --vm client --snapshot clean
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE base_delete_result
    OUTPUT_VARIABLE base_delete_output
    ERROR_VARIABLE base_delete_error
)
if(base_delete_result EQUAL 0)
    message(FATAL_ERROR "SatsumaHost allowed deletion of the user base snapshot: ${base_delete_output}")
endif()

execute_process(
    COMMAND "${HOST_EXE}" snapshot create-ai --vm client --name network-ready
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
file(GLOB snapshot_metadata "${archive_path}/snapshots/client/*.json")
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
    COMMAND "${HOST_EXE}" snapshot delete-ai --vm client --snapshot satsuma-ai-obsolete
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
set(deleted_metadata_path "${archive_path}/snapshots/client/satsuma-ai-obsolete.json")
file(READ "${deleted_metadata_path}" deleted_metadata_json)
string(FIND "${deleted_metadata_json}" "\"status\": \"deleted\"" deleted_metadata_position)
if(deleted_metadata_position EQUAL -1)
    message(FATAL_ERROR "Snapshot deletion metadata was not finalized: ${deleted_metadata_json}")
endif()

execute_process(
    COMMAND "${HOST_EXE}" run --config "${TEST_ROOT}/lab.json" --plan "${TEST_ROOT}/task.json"
    RESULT_VARIABLE host_result
    OUTPUT_VARIABLE host_output
    ERROR_VARIABLE host_error
)
if(NOT host_result EQUAL 0)
    message(FATAL_ERROR "SatsumaHost run failed: ${host_error}\n${host_output}")
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
if(NOT report_result EQUAL 0)
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

set(result_root "${TEST_ROOT}/share/runs/integration_run/results/client/execute_fixture")
if(NOT EXISTS "${result_root}/execution.json" OR
   NOT EXISTS "${result_root}/stdout.log" OR
   NOT EXISTS "${result_root}/files/generated/result.json")
    message(FATAL_ERROR "Expected execution evidence was not created")
endif()

set(timeout_result "${TEST_ROOT}/share/runs/integration_run/results/client/timeout_fixture/execution.json")
if(NOT EXISTS "${timeout_result}")
    message(FATAL_ERROR "Timed-out execution evidence was not created")
endif()
file(READ "${timeout_result}" timeout_json)
string(FIND "${timeout_json}" "\"timed_out\": true" timed_out_position)
if(timed_out_position EQUAL -1)
    message(FATAL_ERROR "Process timeout was not recorded: ${timeout_json}")
endif()
