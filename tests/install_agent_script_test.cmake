# 验证安装脚本编码、语法和最小线性安装边界。
if(NOT DEFINED SCRIPT OR NOT EXISTS "${SCRIPT}")
    message(FATAL_ERROR "SCRIPT must name the install-agent.ps1 file")
endif()
if(NOT DEFINED POWERSHELL OR NOT EXISTS "${POWERSHELL}")
    message(FATAL_ERROR "POWERSHELL must name Windows PowerShell 5.1")
endif()

file(READ "${SCRIPT}" script_hex HEX)
string(TOLOWER "${script_hex}" script_hex)
string(SUBSTRING "${script_hex}" 0 6 script_bom)
if(NOT script_bom STREQUAL "efbbbf")
    message(FATAL_ERROR "install-agent.ps1 must retain its UTF-8 BOM")
endif()

file(READ "${SCRIPT}" script_text)
string(FIND "${script_text}" "\r\n" crlf_position)
if(NOT crlf_position EQUAL -1)
    message(FATAL_ERROR "install-agent.ps1 must retain LF line endings")
endif()

# PowerShell 续行只能使用反引号，前置反斜杠会成为额外的位置参数。
string(FIND "${script_hex}" "5c60" invalid_continuation_position)
if(NOT invalid_continuation_position EQUAL -1)
    message(FATAL_ERROR "install-agent.ps1 contains a backslash before a continuation backtick")
endif()

set(parser_command [=[
$tokens = $null
$parseErrors = $null
$scriptAst = [Management.Automation.Language.Parser]::ParseFile(
    $env:SATSUMA_INSTALL_SCRIPT_TO_PARSE,
    [ref]$tokens,
    [ref]$parseErrors)
if ($parseErrors.Count -gt 0) {
    $parseErrors | ForEach-Object { [Console]::Error.WriteLine($_.Message) }
    exit 1
}

# 从 AST 执行纯参数引用函数，覆盖空格、引号和尾部反斜杠。
$functions = $scriptAst.FindAll({
    param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst]
}, $true)
$converter = $functions | Where-Object Name -eq 'ConvertTo-WindowsCommandLineArgument'
Set-Item -LiteralPath Function:\ConvertTo-WindowsCommandLineArgument -Value $converter.Body.GetScriptBlock()
$quoted = ConvertTo-WindowsCommandLineArgument 'C:\path with space\'
$escaped = ConvertTo-WindowsCommandLineArgument 'value"quoted'
if ($quoted -cne '"C:\path with space\\"' -or $escaped -cne '"value\"quoted"') {
    [Console]::Error.WriteLine('Windows command-line quoting changed')
    exit 1
}
$reader = $functions | Where-Object Name -eq 'Read-SatsumaInstallConfig'
Set-Item -LiteralPath Function:\Read-SatsumaInstallConfig -Value $reader.Body.GetScriptBlock()
$missingPath = Join-Path ([IO.Path]::GetTempPath()) ([Guid]::NewGuid().ToString('N') + '.json')
$automatic = Read-SatsumaInstallConfig $missingPath
if ($automatic.schema_version -ne 1 -or
    $automatic.PSObject.Properties.Name -contains 'lab_id' -or
    $automatic.PSObject.Properties.Name -contains 'vm_id') {
    throw 'Installation without external JSON must generate an unbound Agent config'
}
]=])
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "SATSUMA_INSTALL_SCRIPT_TO_PARSE=${SCRIPT}"
        "${POWERSHELL}" -NoProfile -NonInteractive -Command "${parser_command}"
    RESULT_VARIABLE parser_result
    OUTPUT_VARIABLE parser_output
    ERROR_VARIABLE parser_error
)
if(NOT parser_result EQUAL 0)
    message(FATAL_ERROR
        "Windows PowerShell parser rejected install-agent.ps1:\n${parser_output}${parser_error}")
endif()

# 保留可恢复但不扩张为完整安装事务的关键步骤。
foreach(required_text IN ITEMS
    "Global\\SatsumaVM-Install"
    "D:\\SatsumaTestLab"
    "[IO.DriveType]::Fixed"
    "AvailableFreeSpace -lt 2GB"
    "Join-Path $env:ProgramData 'SatsumaTestLab'"
    "Join-Path $InstallRoot 'agent'"
    "Join-Path $agentRoot 'bin'"
    "Join-Path $InstallRoot 'work'"
    "storage_root"
    "Set-SatsumaProtectedAcl"
    "BUILTIN\\Administrators"
    "SatsumaVM.new.exe"
    "SatsumaVM.bak.exe"
    "--validate-config"
    "Get-FileHash"
    "Stop-SatsumaService"
    "Remove-LegacySatsumaTask"
    "Unregister-ScheduledTask"
    "--install-service"
    "Get-CimInstance"
    "Win32_Service"
    "& $targetAgent --version"
    "服务状态"
    "EXE 路径"
    "Agent 版本"
    "$installSucceeded = $true")
    string(FIND "${script_text}" "${required_text}" required_position)
    if(required_position EQUAL -1)
        message(FATAL_ERROR "install-agent.ps1 is missing required linear step: ${required_text}")
    endif()
endforeach()

# 这些机制属于已取消的完整事务范围，或会破坏目录权限边界。
foreach(forbidden_text IN ITEMS
    "Everyone"
    "Export-ScheduledTask"
    "Register-ScheduledTask"
    ".service-update.lock"
    ".install.lock"
    "Invoke-Expression"
    "Test-SatsumaTaskOwnership"
    "previousPresence"
    "serviceCommitted")
    string(FIND "${script_text}" "${forbidden_text}" forbidden_position)
    if(NOT forbidden_position EQUAL -1)
        message(FATAL_ERROR "install-agent.ps1 contains cancelled transaction text: ${forbidden_text}")
    endif()
endforeach()

# 候选预检必须发生在首次停服调用之前。
string(FIND "${script_text}" "& $newAgent --config $newConfig --validate-config" validate_position)
string(FIND "${script_text}" "    Stop-SatsumaService\n" stop_position)
if(validate_position EQUAL -1 OR stop_position EQUAL -1 OR validate_position GREATER stop_position)
    message(FATAL_ERROR "candidate validation must precede the first Service stop")
endif()

# 旧任务迁移必须发生在正式文件切换之前。
string(FIND "${script_text}" "    Remove-LegacySatsumaTask\n" migration_position)
string(FIND "${script_text}" "Move-Item -LiteralPath $targetAgent -Destination $backupAgent" switch_position)
if(migration_position EQUAL -1 OR switch_position EQUAL -1 OR migration_position GREATER switch_position)
    message(FATAL_ERROR "legacy scheduled task migration must precede the file switch")
endif()

# 安装成功后必须删除本次 new/bak 和配置暂存文件。
string(FIND "${script_text}" "$installSucceeded = $true" success_position)
string(FIND "${script_text}" "$backupAgent, $newAgent, $backupConfig, $newConfig" cleanup_position)
if(success_position EQUAL -1 OR cleanup_position EQUAL -1 OR success_position GREATER cleanup_position)
    message(FATAL_ERROR "successful installation must clean all staging and backup files")
endif()
