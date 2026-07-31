# 验证真实故障恢复脚本的编码、语法、计划生成和安全门禁。
if(NOT DEFINED SCRIPT OR NOT EXISTS "${SCRIPT}")
    message(FATAL_ERROR "SCRIPT must name the real fault recovery PowerShell driver")
endif()
if(NOT DEFINED POWERSHELL OR NOT EXISTS "${POWERSHELL}")
    message(FATAL_ERROR "POWERSHELL must name PowerShell 7 for syntax parsing")
endif()
set(test_root "${CMAKE_CURRENT_BINARY_DIR}/real-fault-script-test")
file(REMOVE_RECURSE "${test_root}")
file(MAKE_DIRECTORY "${test_root}")

file(READ "${SCRIPT}" script_hex HEX)
string(TOLOWER "${script_hex}" script_hex)
string(SUBSTRING "${script_hex}" 0 6 script_prefix)
if(script_prefix STREQUAL "efbbbf")
    message(FATAL_ERROR "PowerShell 7 fault driver must remain UTF-8 without BOM")
endif()

file(READ "${SCRIPT}" script_text)
string(FIND "${script_text}" "\r\n" crlf_position)
if(NOT crlf_position EQUAL -1)
    message(FATAL_ERROR "Real fault recovery script must retain LF line endings")
endif()

set(parser_command [=[
$tokens = $null
$parseErrors = $null
$scriptAst = [Management.Automation.Language.Parser]::ParseFile(
    $env:SATSUMA_REAL_FAULT_SCRIPT_TO_PARSE,
    [ref]$tokens,
    [ref]$parseErrors)
if ($parseErrors.Count -gt 0) {
    $parseErrors | ForEach-Object { [Console]::Error.WriteLine($_.Message) }
    exit 1
}

$functions = $scriptAst.FindAll({
    param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst]
}, $true)
foreach ($name in 'Write-JsonFile', 'New-FaultPlan') {
    $definition = $functions | Where-Object Name -eq $name
    Set-Item -LiteralPath "Function:\$name" -Value $definition.Body.GetScriptBlock()
}
$script:utf8NoBom = [Text.UTF8Encoding]::new($false)
$script:VmId = 'vm_01'
$script:resolvedFixtureExe = 'C:\Satsuma Test\SatsumaTestFixture.exe'
$script:validationRoot = $env:SATSUMA_REAL_FAULT_SCRIPT_TEST_ROOT
$script:TaskSleepMs = 90000
$plan = New-FaultPlan
$planJson = [IO.File]::ReadAllText($plan.Path, [Text.Encoding]::UTF8) | ConvertFrom-Json
if ($planJson.run_id -cne $plan.RunId -or
    $planJson.steps[0].retry_safe -ne $true -or
    $planJson.steps[0].id -cne 'shared_outage' -or
    $planJson.steps[0].arguments -notcontains '--sleep-ms') {
    [Console]::Error.WriteLine('Real fault recovery plan generation changed')
    exit 1
}
]=])
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "SATSUMA_REAL_FAULT_SCRIPT_TO_PARSE=${SCRIPT}"
        "SATSUMA_REAL_FAULT_SCRIPT_TEST_ROOT=${test_root}"
        "${POWERSHELL}" -NoProfile -NonInteractive -Command "${parser_command}"
    RESULT_VARIABLE parser_result
    OUTPUT_VARIABLE parser_output
    ERROR_VARIABLE parser_error
)
if(NOT parser_result EQUAL 0)
    message(FATAL_ERROR
        "PowerShell parser rejected real fault recovery driver:\n${parser_output}${parser_error}")
endif()
file(REMOVE_RECURSE "${test_root}")

execute_process(
    COMMAND "${POWERSHELL}"
        -NoProfile
        -NonInteractive
        -File "${SCRIPT}"
        -HostExe "missing-host.exe"
        -FixtureExe "missing-fixture.exe"
        -LabConfig "missing-lab.json"
        -VmId vm_01
        -Confirm NOT_CONFIRMED
    RESULT_VARIABLE gate_result
    OUTPUT_VARIABLE gate_output
    ERROR_VARIABLE gate_error
)
if(gate_result EQUAL 0)
    message(FATAL_ERROR "Real fault recovery driver accepted an invalid confirmation")
endif()
set(gate_text "${gate_output}${gate_error}")
# PowerShell 可能按输出宽度折行较长的异常消息。
foreach(required_gate_text IN ITEMS
    "Confirmation must exactly equal"
    "I_UNDERSTAND_SHARED_FOLDER_WILL_BE_TEMPORARILY_DISABLED")
    string(FIND "${gate_text}" "${required_gate_text}" gate_text_position)
    if(gate_text_position EQUAL -1)
        message(FATAL_ERROR "Real fault recovery driver did not reject confirmation before file access")
    endif()
endforeach()

foreach(required_text IN ITEMS
    "AI-authored"
    "PSEdition -ne 'Core'"
    "I_UNDERSTAND_SHARED_FOLDER_WILL_BE_TEMPORARILY_DISABLED"
    "ArgumentList.Add"
    "disableSharedFolders"
    "enableSharedFolders"
    "finally"
    "claim schema 3"
    "summary.json")
    string(FIND "${script_text}" "${required_text}" required_position)
    if(required_position EQUAL -1)
        message(FATAL_ERROR "Real fault recovery driver is missing: ${required_text}")
    endif()
endforeach()

foreach(forbidden_text IN ITEMS
    "Start-Process"
    "Invoke-Expression"
    "Stop-Process"
    "Get-Process -Name"
    "$Args"
    "$HOME"
    "Remove-Item"
    "vm restore"
    "vm stop")
    string(FIND "${script_text}" "${forbidden_text}" forbidden_position)
    if(NOT forbidden_position EQUAL -1)
        message(FATAL_ERROR "Real fault recovery driver contains forbidden text: ${forbidden_text}")
    endif()
endforeach()
