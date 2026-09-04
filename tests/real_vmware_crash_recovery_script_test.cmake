# 验证真实崩溃恢复脚本的编码、语法和默认关闭边界。
if(NOT DEFINED SCRIPT OR NOT EXISTS "${SCRIPT}")
    message(FATAL_ERROR "SCRIPT must name the real crash recovery PowerShell driver")
endif()
if(NOT DEFINED POWERSHELL OR NOT EXISTS "${POWERSHELL}")
    message(FATAL_ERROR "POWERSHELL must name PowerShell 7 for syntax parsing")
endif()
foreach(required HOST_EXE FIXTURE_EXE)
    if(NOT DEFINED ${required} OR NOT EXISTS "${${required}}")
        message(FATAL_ERROR "${required} must name a built executable for plan validation")
    endif()
endforeach()
set(test_root "${CMAKE_CURRENT_BINARY_DIR}/real-crash-script-test")
file(REMOVE_RECURSE "${test_root}")
file(MAKE_DIRECTORY "${test_root}")

file(READ "${SCRIPT}" script_hex HEX)
string(TOLOWER "${script_hex}" script_hex)
string(SUBSTRING "${script_hex}" 0 6 script_prefix)
if(script_prefix STREQUAL "efbbbf")
    message(FATAL_ERROR "PowerShell 7 crash driver must remain UTF-8 without BOM")
endif()

file(READ "${SCRIPT}" script_text)
string(FIND "${script_text}" "\r\n" crlf_position)
if(NOT crlf_position EQUAL -1)
    message(FATAL_ERROR "Real crash recovery script must retain LF line endings")
endif()

set(parser_command [=[
$tokens = $null
$parseErrors = $null
$scriptAst = [Management.Automation.Language.Parser]::ParseFile(
    $env:SATSUMA_REAL_CRASH_SCRIPT_TO_PARSE,
    [ref]$tokens,
    [ref]$parseErrors)
if ($parseErrors.Count -gt 0) {
    $parseErrors | ForEach-Object { [Console]::Error.WriteLine($_.Message) }
    exit 1
}

# 只加载纯计划生成函数，验证固定 run_id、重试安全性和 Fixture 参数。
$functions = $scriptAst.FindAll({
    param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst]
}, $true)
foreach ($name in 'Write-JsonFile', 'New-CrashPlan', 'Get-FinalCheckStatus') {
    $definition = $functions | Where-Object Name -eq $name
    Set-Item -LiteralPath "Function:\$name" -Value $definition.Body.GetScriptBlock()
}
$script:utf8NoBom = [Text.UTF8Encoding]::new($false)
$script:VmId = 'vm_01'
$script:resolvedFixtureExe = $env:SATSUMA_REAL_CRASH_FIXTURE_EXE
$script:validationRoot = $env:SATSUMA_REAL_CRASH_SCRIPT_TEST_ROOT
$script:AgentCrashDelayMs = 35000
$script:HostTaskSleepMs = 70000
$safe = New-CrashPlan -Name 'script_test' -RetrySafe $true -CrashAgent $true
$safeJson = [IO.File]::ReadAllText($safe.Path, [Text.Encoding]::UTF8) | ConvertFrom-Json
if ($safeJson.run_id -cne $safe.RunId -or
    $safeJson.steps[0].retry_safe -ne $true -or
    $safeJson.steps[0].run_as -cne 'system' -or
    $safeJson.steps[0].arguments -notcontains '--crash-parent-once' -or
    $safeJson.lifecycle.vms[0].on_failure.action -cne 'leave_running') {
    [Console]::Error.WriteLine('Real crash recovery plan generation changed')
    exit 1
}

# Validate all generated plans with the actual CLI, without any VMware command.
$vmx = Join-Path $script:validationRoot 'test.vmx'
[IO.File]::WriteAllText($vmx, '')
$lab = @{
    schema_version = 1; lab_id = 'crash_plan_contract'
    provider = @{type='vmware_workstation';vmrun=$env:SATSUMA_REAL_CRASH_HOST_EXE}
    host = @{archive_root=(Join-Path $script:validationRoot 'archive')}
    transport = @{state_root=(Join-Path $script:validationRoot 'state');vmci_port=42510}
    vms = @(@{id='vm_01';vmx=$vmx;agent_version='0.3.4';snapshots=@{base='test';ai_prefix='satsuma-ai-';max_ai_snapshots=8}})
}
$labPath = Join-Path $script:validationRoot 'lab.json'
Write-JsonFile -Path $labPath -Value $lab
$plans = @($safe, (New-CrashPlan -Name 'unsafe' -RetrySafe $false -CrashAgent $true),
    (New-CrashPlan -Name 'host' -RetrySafe $true -CrashAgent $false))
foreach ($plan in $plans) {
    $value = [IO.File]::ReadAllText($plan.Path, [Text.Encoding]::UTF8) | ConvertFrom-Json
    $crashesAgent = $value.steps[0].arguments -contains '--crash-parent-once'
    $expectedIdentity = if ($crashesAgent) { 'system' } else { 'interactive_user' }
    if ($value.steps[0].run_as -cne $expectedIdentity) { exit 1 }
    & $env:SATSUMA_REAL_CRASH_HOST_EXE plan validate --config $labPath --plan $plan.Path
    if ($LASTEXITCODE -ne 0) { exit 1 }
}
$stopped = [pscustomobject]@{status='failed';checks=@(
    [pscustomobject]@{name='vm_power';status='failed';details=@{vm_id='vm_01';state='stopped'}}
)}
if ((Get-FinalCheckStatus -Check $stopped -ExpectedStopped $true) -cne 'expected_stopped') { exit 1 }
if ((Get-FinalCheckStatus -Check ([pscustomobject]@{status='ready'}) -ExpectedStopped $false) -cne 'ready') { exit 1 }
$rejected = $false
try { [void](Get-FinalCheckStatus -Check $stopped -ExpectedStopped $false) } catch { $rejected=$true }
if (-not $rejected) { exit 1 }
$stopped.checks += [pscustomobject]@{name='transport_state';status='failed';details=@{}}
$rejected = $false
try { [void](Get-FinalCheckStatus -Check $stopped -ExpectedStopped $true) } catch { $rejected=$true }
if (-not $rejected) { exit 1 }
]=])
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "SATSUMA_REAL_CRASH_SCRIPT_TO_PARSE=${SCRIPT}"
        "SATSUMA_REAL_CRASH_SCRIPT_TEST_ROOT=${test_root}"
        "SATSUMA_REAL_CRASH_HOST_EXE=${HOST_EXE}"
        "SATSUMA_REAL_CRASH_FIXTURE_EXE=${FIXTURE_EXE}"
        "${POWERSHELL}" -NoProfile -NonInteractive -Command "${parser_command}"
    RESULT_VARIABLE parser_result
    OUTPUT_VARIABLE parser_output
    ERROR_VARIABLE parser_error
)
if(NOT parser_result EQUAL 0)
    message(FATAL_ERROR
        "PowerShell parser rejected real crash recovery driver:\n${parser_output}${parser_error}")
endif()
file(REMOVE_RECURSE "${test_root}")

foreach(required_text IN ITEMS
    "AI-authored"
    "PSEdition -ne 'Core'"
    "I_UNDERSTAND_HOST_AND_AGENT_WILL_BE_FORCE_TERMINATED"
    "ArgumentList.Add"
    "Kill($true)"
    "WaitForExit(10000)"
    "ScenarioTimeoutSeconds"
    "'lab', 'recover'"
    "'lab', 'unlock'"
    "claim-recovery.json"
    "summary.json")
    string(FIND "${script_text}" "${required_text}" required_position)
    if(required_position EQUAL -1)
        message(FATAL_ERROR "Real crash recovery driver is missing: ${required_text}")
    endif()
endforeach()

foreach(forbidden_text IN ITEMS
    "Start-Process"
    "Invoke-Expression"
    "Stop-Process -Name"
    "Get-Process -Name"
    "$Args"
    "$HOME"
    "Remove-Item"
    "vm restore"
    "vm stop")
    string(FIND "${script_text}" "${forbidden_text}" forbidden_position)
    if(NOT forbidden_position EQUAL -1)
        message(FATAL_ERROR "Real crash recovery driver contains forbidden text: ${forbidden_text}")
    endif()
endforeach()
