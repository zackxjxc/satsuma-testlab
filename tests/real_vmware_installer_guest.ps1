# AI-authored developer fixture. Run only in an explicitly authorized disposable Guest.
# This script is not an installer and must never enter the consumer package.
$ErrorActionPreference = 'Stop'
$testRoot = [IO.Path]::GetFullPath($PSScriptRoot)
$programData = [Environment]::GetFolderPath('CommonApplicationData')
if ((Split-Path $testRoot -Parent) -ne $programData -or
    (Split-Path $testRoot -Leaf) -notmatch '^SatsumaAcceptance-[a-f0-9]{12}$') { throw 'Invalid fixture directory' }
$context = Get-Content (Join-Path $testRoot 'context.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if ($context.confirm -cne 'DISPOSABLE_VM_INSTALLER_TEST' -or
    (Get-CimInstance Win32_ComputerSystemProduct).UUID -ine $context.hardware_id -or
    -not [Security.Principal.WindowsIdentity]::GetCurrent().IsSystem) { throw 'Guest authorization mismatch' }
$resultPath = Join-Path $testRoot 'result.json'
if (Test-Path -LiteralPath $resultPath) { throw 'Refusing to rerun a completed fixture' }
$utf8 = New-Object Text.UTF8Encoding($false)
$report = [ordered]@{success=$false; checks=@(); error=$null; restoration=$null}
function Save-Report { [IO.File]::WriteAllText($resultPath, ($report | ConvertTo-Json -Depth 16), $utf8) }
function Assert-Test([string]$name, [bool]$value) {
    $report.checks += [ordered]@{name=$name;passed=$value}
    if (-not $value) { throw "Assertion failed: $name" }
}
function Hash([string]$path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Service { Get-CimInstance Win32_Service -Filter "Name='SatsumaVM'" }
function Service-Paths($service) {
    $match = [regex]::Match($service.PathName, '^"([^"]+)" --config (?:"([^"]+)"|([^\s"]+)) --service$')
    if (-not $match.Success -or $service.StartName -ne 'LocalSystem') { throw 'Unrecognized original service' }
    $configuration = if ($match.Groups[2].Success) { $match.Groups[2].Value } else { $match.Groups[3].Value }
    return @($match.Groups[1].Value,$configuration)
}
function Run-Installer([string]$name, [string]$file='SatsumaVM.exe', [string]$parameters='') {
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = Join-Path $testRoot $file
    $info.Arguments = $parameters
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = $utf8
    $info.StandardErrorEncoding = $utf8
    $process = [Diagnostics.Process]::Start($info)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $process.StandardInput.WriteLine()
    $process.StandardInput.Close()
    if (-not $process.WaitForExit(90000)) { $process.Kill(); throw "Installer timeout: $name" }
    [IO.File]::WriteAllText((Join-Path $testRoot ($name+'.log')), ($stdout.Result+$stderr.Result), $utf8)
    return $process.ExitCode
}
function Remove-OwnedService([string]$image, [string]$config) {
    $service = Service
    if (-not $service) { throw 'Expected owned service' }
    $paths = Service-Paths $service
    if ($paths[0] -ine $image -or $paths[1] -ine $config) {
        throw 'Refusing to remove an unrecognized service'
    }
    & $image --config $config --remove-service | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Owned service removal failed' }
}
function Register-Legacy([string]$image, [string]$arguments) {
    if (Get-ScheduledTask -TaskPath '\Satsuma\' -TaskName 'SatsumaVM Agent' -ErrorAction SilentlyContinue) {
        throw 'Historical task already exists'
    }
    $action = New-ScheduledTaskAction -Execute $image -Argument $arguments
    $principal = New-ScheduledTaskPrincipal -UserId SYSTEM -LogonType ServiceAccount -RunLevel Highest
    Register-ScheduledTask -TaskPath '\Satsuma\' -TaskName 'SatsumaVM Agent' -Action $action -Principal $principal | Out-Null
    $script:createdLegacy = $true
}
function Remove-FixtureLegacy {
    if ($script:createdLegacy) {
        $task = Get-ScheduledTask -TaskPath '\Satsuma\' -TaskName 'SatsumaVM Agent' -ErrorAction SilentlyContinue
        if ($task) {
            if ($task.Actions.Execute -notin @($script:installedExe, "$env:SystemRoot\System32\cmd.exe")) {
                throw 'Fixture task was changed externally'
            }
            Stop-ScheduledTask -TaskPath '\Satsuma\' -TaskName 'SatsumaVM Agent' -ErrorAction SilentlyContinue
            Unregister-ScheduledTask -TaskPath '\Satsuma\' -TaskName 'SatsumaVM Agent' -Confirm:$false
        }
        $script:createdLegacy = $false
    }
}

$backedUp = $false
$createdLegacy = $false
$createdUnknown = $false
$installRoot = Join-Path $programData 'SatsumaTestLab'
$installedExe = Join-Path $installRoot 'agent\bin\SatsumaVM.exe'
$installedConfig = Join-Path $installRoot 'agent\agent.json'
try {
    $task = Get-ScheduledTask -TaskName $context.task_name
    if ($task.Actions.Arguments -cne ('-NoProfile -ExecutionPolicy Bypass -File "'+$PSCommandPath+'"')) {
        throw 'Bootstrap task action mismatch'
    }
    Unregister-ScheduledTask -TaskName $context.task_name -Confirm:$false
    Start-Sleep -Seconds 15
    $manifest = Get-Content (Join-Path $testRoot 'fixtures.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach ($property in $manifest.files.PSObject.Properties) {
        Assert-Test ('fixture-hash-'+$property.Name) ((Hash (Join-Path $testRoot $property.Name)) -ceq $property.Value)
    }
    $original = Service
    $originalExe, $originalConfig = Service-Paths $original
    $originalRoot = Split-Path (Split-Path $originalConfig -Parent) -Parent
    if ($originalRoot -notin @($installRoot,'D:\SatsumaTestLab') -or
        $originalExe -ne (Join-Path $originalRoot 'agent\bin\SatsumaVM.exe')) { throw 'Original root outside test scope' }
    Assert-Test 'original-matches-authorized-release' ((Hash $originalExe) -ceq $context.agent_sha256)
    $originalConfigHash = Hash $originalConfig
    Assert-Test 'existing-status' ((Run-Installer 'existing') -eq 0)
    $backup = Join-Path $testRoot 'original-installation'
    if (Test-Path -LiteralPath $backup) { throw 'Backup already exists' }
    if (Get-ScheduledTask -TaskPath '\Satsuma\' -TaskName 'SatsumaVM Agent' -ErrorAction SilentlyContinue) { throw 'Preexisting legacy task; not a disposable baseline' }
    Remove-OwnedService $originalExe $originalConfig
    # Both absolute paths were checked against the authorized installation and exclusive fixture directory.
    Move-Item -LiteralPath $originalRoot -Destination $backup
    $backedUp = $true
    $corrupt = Join-Path $testRoot 'corrupt.exe'
    Copy-Item -LiteralPath (Join-Path $testRoot 'SatsumaVM.exe') -Destination $corrupt
    $stream = [IO.File]::Open($corrupt,[IO.FileMode]::Append)
    try { $stream.WriteByte(120) } finally { $stream.Dispose() }
    Assert-Test 'corrupt-first-install-rejected' ((Run-Installer 'corrupt-first' 'corrupt.exe') -eq 1)
    Assert-Test 'corrupt-created-no-root' (-not (Test-Path -LiteralPath $installRoot))
    Assert-Test 'first-install-no-arguments' ((Run-Installer 'first') -eq 0)
    $s = Service
    Assert-Test 'service-auto-system-running' ($s.State -eq 'Running' -and $s.StartMode -eq 'Auto' -and $s.StartName -eq 'LocalSystem')
    $cfg = Get-Content $installedConfig -Raw -Encoding UTF8 | ConvertFrom-Json
    Assert-Test 'neutral-local-config' (-not $cfg.PSObject.Properties['lab_id'] -and -not $cfg.PSObject.Properties['vm_id'])
    $configHash = Hash $installedConfig
    $before = (Service).ProcessId
    Assert-Test 'identical-status' ((Run-Installer 'identical') -eq 0)
    Assert-Test 'identical-no-restart-or-overwrite' ((Service).ProcessId -eq $before -and (Hash $installedConfig) -ceq $configHash)

    $mutex = New-Object Threading.Mutex($false, 'Global\SatsumaVM-Install')
    if (-not $mutex.WaitOne(0)) { throw 'Unexpected competing installer' }
    try { Assert-Test 'concurrent-process-rejected' ((Run-Installer 'concurrent') -eq 1) }
    finally { $mutex.ReleaseMutex(); $mutex.Dispose() }
    Assert-Test 'concurrency-keeps-service' ((Service).ProcessId -eq $before)
    Assert-Test 'same-version-update' ((Run-Installer 'same-version' 'same.exe') -eq 0)
    Assert-Test 'same-version-bytes-installed' ((Hash $installedExe) -ceq $manifest.files.'same.exe')
    Assert-Test 'same-version-keeps-config' ((Hash $installedConfig) -ceq $configHash)
    Assert-Test 'restore-release' ((Run-Installer 'restore-release') -eq 0)

    $probeArguments = '--simulate-local-update-failure "'+(Join-Path $testRoot 'same.exe')+'"'
    Assert-Test 'post-start-rollback-error' ((Run-Installer 'post-start-rollback' 'probe.exe' $probeArguments) -eq 1)
    Assert-Test 'post-start-rollback-state' ((Hash $installedExe) -ceq $context.agent_sha256 -and (Hash $installedConfig) -ceq $configHash -and (Service).State -eq 'Running')
    $held = [IO.File]::Open($installedExe,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    try { Assert-Test 'replacement-blocked-error' ((Run-Installer 'replacement-blocked' 'newer.exe') -eq 1) }
    finally { $held.Dispose() }
    Assert-Test 'replacement-blocked-rollback' ((Hash $installedExe) -ceq $context.agent_sha256 -and (Service).State -eq 'Running')
    Assert-Test 'newer-media-updates' ((Run-Installer 'newer' 'newer.exe') -eq 0)
    Assert-Test 'newer-bytes-installed' ((Hash $installedExe) -ceq $manifest.files.'newer.exe')
    $before = (Service).ProcessId
    Assert-Test 'higher-installed-no-downgrade' ((Run-Installer 'no-downgrade') -eq 0)
    Assert-Test 'higher-installed-preserved' ((Service).ProcessId -eq $before -and (Hash $installedExe) -ceq $manifest.files.'newer.exe')
    # Set up a lower-version fixture only within the installation created above.
    Stop-Service SatsumaVM
    Copy-Item -LiteralPath (Join-Path $testRoot 'older.exe') -Destination $installedExe -Force
    $cfg.agent_version = '0.3.3'
    [IO.File]::WriteAllText($installedConfig,($cfg | ConvertTo-Json -Depth 16),$utf8)
    Start-Service SatsumaVM
    Assert-Test 'lower-installed-updates' ((Run-Installer 'lower-installed') -eq 0)
    Assert-Test 'lower-installed-replaced' ((Hash $installedExe) -ceq $context.agent_sha256)
    $before = (Service).ProcessId
    Assert-Test 'corrupt-update-rejected' ((Run-Installer 'corrupt-update' 'corrupt.exe') -eq 1)
    Assert-Test 'corrupt-update-no-restart' ((Service).ProcessId -eq $before)

    $pending = Join-Path $installRoot 'agent\install-pending.json'
    if (Test-Path $pending) { throw 'Unexpected pending transaction after rollback' }
    [IO.File]::WriteAllText($pending,'{"stage":"acceptance-interruption"}',$utf8)
    Assert-Test 'pending-transaction-rejected' ((Run-Installer 'pending') -eq 1)
    Assert-Test 'pending-state-preserved' ((Service).ProcessId -eq $before -and (Hash $installedExe) -ceq $context.agent_sha256)
    Remove-Item -LiteralPath $pending
    Remove-OwnedService $installedExe $installedConfig
    $unknownCommand = "$env:SystemRoot\System32\cmd.exe /c exit"
    New-Service -Name SatsumaVM -BinaryPathName $unknownCommand -StartupType Manual | Out-Null
    $createdUnknown = $true
    Assert-Test 'unknown-service-rejected' ((Run-Installer 'unknown-service') -eq 1)
    Assert-Test 'unknown-service-unchanged' ((Service).PathName -ceq $unknownCommand)
    & sc.exe delete SatsumaVM | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Fixture service deletion failed' }
    $createdUnknown = $false
    Start-Sleep -Seconds 1
    & $installedExe --config $installedConfig --install-service | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Test service restoration failed' }

    Register-Legacy "$env:SystemRoot\System32\cmd.exe" '/c exit'
    Assert-Test 'unknown-task-rejected' ((Run-Installer 'unknown-task') -eq 1)
    Assert-Test 'unknown-task-unchanged' ((Get-ScheduledTask -TaskPath '\Satsuma\' -TaskName 'SatsumaVM Agent').Actions.Execute -eq "$env:SystemRoot\System32\cmd.exe")
    Remove-FixtureLegacy
    Remove-OwnedService $installedExe $installedConfig
    Register-Legacy $installedExe ('--config "'+$installedConfig+'" --watch')
    Start-ScheduledTask -TaskPath '\Satsuma\' -TaskName 'SatsumaVM Agent'
    Start-Sleep -Seconds 2
    Assert-Test 'migration-rollback-error' ((Run-Installer 'migration-rollback' 'probe.exe' '--simulate-task-delete-failure') -eq 1)
    Assert-Test 'migration-rollback-task-running' ((Get-ScheduledTask -TaskPath '\Satsuma\' -TaskName 'SatsumaVM Agent').State -eq 'Running' -and -not (Service))
    Assert-Test 'legacy-migration' ((Run-Installer 'legacy-migration') -eq 0)
    Assert-Test 'legacy-task-removed' (-not (Get-ScheduledTask -TaskPath '\Satsuma\' -TaskName 'SatsumaVM Agent' -ErrorAction SilentlyContinue))
    $createdLegacy = $false
    Assert-Test 'migrated-service-running' ((Service).State -eq 'Running' -and (Service).StartMode -eq 'Auto')
    $report.success = $true
} catch { $report.error = $_.ToString() }
finally {
    if ($backedUp) {
        try {
            Remove-FixtureLegacy
            if ($createdUnknown -and (Service).PathName -ceq $unknownCommand) {
                & sc.exe delete SatsumaVM | Out-Null
                if ($LASTEXITCODE -ne 0) { throw 'Fixture service cleanup failed' }
                Start-Sleep -Seconds 1
            }
            if (Service) { Remove-OwnedService $installedExe $installedConfig }
            $final = Join-Path $testRoot 'tested-installation'
            if (Test-Path $final) { throw 'Final evidence directory already exists' }
            if (Test-Path $installRoot) { Move-Item -LiteralPath $installRoot -Destination $final }
            if (Test-Path $originalRoot) { throw 'Original restore target already exists' }
            Move-Item -LiteralPath $backup -Destination $originalRoot
            & $originalExe --config $originalConfig --install-service | Out-Null
            if ($LASTEXITCODE -ne 0) { throw 'Original service restore failed' }
            $report.restoration = ((Hash $originalExe) -ceq $context.agent_sha256 -and
                (Hash $originalConfig) -ceq $originalConfigHash -and (Service).State -eq 'Running')
        } catch { $report.restoration = $false; $report.restore_error = $_.ToString() }
    }
    Save-Report
}
if (-not $report.success -or -not $report.restoration) { exit 1 }
