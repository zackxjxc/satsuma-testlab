# 验证真实崩溃恢复脚本的编码、语法和默认关闭边界。
if(NOT DEFINED SCRIPT OR NOT EXISTS "${SCRIPT}")
    message(FATAL_ERROR "SCRIPT must name the real crash recovery PowerShell driver")
endif()
if(NOT DEFINED POWERSHELL OR NOT EXISTS "${POWERSHELL}")
    message(FATAL_ERROR "POWERSHELL must name PowerShell 7 for syntax parsing")
endif()
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
foreach ($name in 'Write-JsonFile', 'New-CrashPlan') {
    $definition = $functions | Where-Object Name -eq $name
    Set-Item -LiteralPath "Function:\$name" -Value $definition.Body.GetScriptBlock()
}
$script:utf8NoBom = [Text.UTF8Encoding]::new($false)
$script:VmId = 'vm_01'
$script:resolvedFixtureExe = 'C:\Satsuma Test\SatsumaTestFixture.exe'
$script:validationRoot = $env:SATSUMA_REAL_CRASH_SCRIPT_TEST_ROOT
$script:AgentCrashDelayMs = 35000
$script:HostTaskSleepMs = 70000
$safe = New-CrashPlan -Name 'script_test' -RetrySafe $true -CrashAgent $true
$safeJson = [IO.File]::ReadAllText($safe.Path, [Text.Encoding]::UTF8) | ConvertFrom-Json
if ($safeJson.run_id -cne $safe.RunId -or
    $safeJson.steps[0].retry_safe -ne $true -or
    $safeJson.steps[0].arguments -notcontains '--crash-parent-once' -or
    $safeJson.lifecycle.vms[0].on_failure.action -cne 'leave_running') {
    [Console]::Error.WriteLine('Real crash recovery plan generation changed')
    exit 1
}
]=])
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "SATSUMA_REAL_CRASH_SCRIPT_TO_PARSE=${SCRIPT}"
        "SATSUMA_REAL_CRASH_SCRIPT_TEST_ROOT=${test_root}"
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
