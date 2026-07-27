# AI 编写的真实 VMware Host/Agent 崩溃恢复验收驱动。
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$HostExe,

    [Parameter(Mandatory = $true)]
    [string]$FixtureExe,

    [Parameter(Mandatory = $true)]
    [string]$LabConfig,

    [Parameter(Mandatory = $true)]
    [string]$VmId,

    [ValidateSet('All', 'HostCrash', 'AgentRetrySafe', 'AgentUnsafe')]
    [string]$Scenario = 'All',

    [Parameter(Mandatory = $true)]
    [string]$Confirm,

    [ValidateRange(60, 900)]
    [int]$ScenarioTimeoutSeconds = 480,

    [ValidateRange(30000, 300000)]
    [int]$HostTaskSleepMs = 70000,

    [ValidateRange(31000, 120000)]
    [int]$AgentCrashDelayMs = 35000
)

Write-Host 'Satsuma real VMware crash recovery driver (AI-authored).'

$ErrorActionPreference = 'Stop'
$requiredConfirmation = 'I_UNDERSTAND_HOST_AND_AGENT_WILL_BE_FORCE_TERMINATED'
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)

if ($PSVersionTable.PSEdition -ne 'Core' -or $PSVersionTable.PSVersion.Major -lt 7) {
    throw 'The real crash recovery driver requires PowerShell 7 or newer.'
}
if (-not $IsWindows) {
    throw 'The real crash recovery driver only supports Windows.'
}
if ($Confirm -cne $requiredConfirmation) {
    throw "Confirmation must exactly equal $requiredConfirmation"
}

# 返回已存在普通文件的绝对路径。
function Resolve-RequiredFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $item = Get-Item -LiteralPath $Path -ErrorAction Stop
    if ($item.PSIsContainer) {
        throw "$Description is not a regular file: $Path"
    }
    return $item.FullName
}

# 使用显式 UTF-8 编码读取 JSON。
function Read-JsonFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $text = [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)
    return $text | ConvertFrom-Json -Depth 64
}

# 使用 UTF-8 无 BOM 和 LF 写入稳定 JSON 文件。
function Write-JsonFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [object]$Value
    )

    $parent = Split-Path -Parent $Path
    if (-not [string]::IsNullOrEmpty($parent)) {
        [void](New-Item -ItemType Directory -Force -Path $parent)
    }
    $json = $Value | ConvertTo-Json -Depth 64
    $json = $json.Replace("`r`n", "`n") + "`n"
    [System.IO.File]::WriteAllText($Path, $json, $utf8NoBom)
}

# 使用 UTF-8 无 BOM 保存子进程诊断文本。
function Write-TextFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [AllowEmptyString()]
        [string]$Text
    )

    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

# 有限轮询一个可瞬时失败的文件状态探针。
function Wait-ForCondition {
    param(
        [Parameter(Mandatory = $true)]
        [scriptblock]$Probe,

        [Parameter(Mandatory = $true)]
        [int]$TimeoutSeconds,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $lastError = $null
    while ([DateTime]::UtcNow -lt $deadline) {
        try {
            $value = & $Probe
            if ($null -ne $value -and $value -ne $false) {
                return $value
            }
        }
        catch {
            $lastError = $_.Exception.Message
        }
        Start-Sleep -Milliseconds 200
    }
    $suffix = if ($null -eq $lastError) { '' } else { "; last error: $lastError" }
    throw "Timed out waiting for $Description$suffix"
}

# 启动精确的 SatsumaHost 进程并异步捕获 stdout/stderr。
function Start-CapturedHost {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$ProcessArguments,

        [Parameter(Mandatory = $true)]
        [string]$Label,

        [Parameter(Mandatory = $true)]
        [string]$EvidenceDirectory
    )

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $script:resolvedHostExe
    $startInfo.WorkingDirectory = Split-Path -Parent $script:resolvedHostExe
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    foreach ($argument in $ProcessArguments) {
        [void]$startInfo.ArgumentList.Add($argument)
    }

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw "Failed to start SatsumaHost for $Label"
    }
    try {
        if (-not [System.IO.Path]::GetFullPath($process.MainModule.FileName).Equals(
                $script:resolvedHostExe,
                [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Started process path does not match SatsumaHost for $Label"
        }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        return [pscustomobject]@{
            Process = $process
            StdoutTask = $stdoutTask
            StderrTask = $stderrTask
            Label = $Label
            StdoutPath = Join-Path $EvidenceDirectory "$Label.stdout.log"
            StderrPath = Join-Path $EvidenceDirectory "$Label.stderr.log"
        }
    }
    catch {
        if (-not $process.HasExited) {
            $process.Kill($true)
            [void]$process.WaitForExit(10000)
        }
        $process.Dispose()
        throw
    }
}

# 完成进程捕获并保存机器可读命令证据。
function Complete-CapturedHost {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Handle,

        [Parameter(Mandatory = $true)]
        [int]$TimeoutSeconds
    )

    if (-not $Handle.Process.WaitForExit($TimeoutSeconds * 1000)) {
        $Handle.Process.Kill($true)
        [void]$Handle.Process.WaitForExit(10000)
        throw "SatsumaHost timed out for $($Handle.Label)"
    }
    $stdout = $Handle.StdoutTask.GetAwaiter().GetResult()
    $stderr = $Handle.StderrTask.GetAwaiter().GetResult()
    Write-TextFile -Path $Handle.StdoutPath -Text $stdout
    Write-TextFile -Path $Handle.StderrPath -Text $stderr
    $result = [pscustomobject]@{
        ExitCode = $Handle.Process.ExitCode
        ProcessId = $Handle.Process.Id
        Stdout = $stdout
        Stderr = $stderr
        StdoutPath = $Handle.StdoutPath
        StderrPath = $Handle.StderrPath
    }
    $Handle.Process.Dispose()
    return $result
}

# 精确强杀一个由本脚本启动的 Host，并确认句柄已经退出。
function Stop-CapturedHost {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Handle
    )

    if ($Handle.Process.HasExited) {
        throw "SatsumaHost exited before the injected crash for $($Handle.Label)"
    }
    $processId = $Handle.Process.Id
    $Handle.Process.Kill($true)
    if (-not $Handle.Process.WaitForExit(10000)) {
        throw "SatsumaHost PID $processId did not exit after forced termination"
    }
    $stdout = $Handle.StdoutTask.GetAwaiter().GetResult()
    $stderr = $Handle.StderrTask.GetAwaiter().GetResult()
    Write-TextFile -Path $Handle.StdoutPath -Text $stdout
    Write-TextFile -Path $Handle.StderrPath -Text $stderr
    $exitCode = $Handle.Process.ExitCode
    $Handle.Process.Dispose()
    return [pscustomobject]@{
        ExitCode = $exitCode
        ProcessId = $processId
        StdoutPath = $Handle.StdoutPath
        StderrPath = $Handle.StderrPath
    }
}

# 同步运行 Host 命令并验证允许的退出码。
function Invoke-HostCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$ProcessArguments,

        [Parameter(Mandatory = $true)]
        [int[]]$ExpectedExitCodes,

        [Parameter(Mandatory = $true)]
        [string]$Label,

        [Parameter(Mandatory = $true)]
        [int]$TimeoutSeconds
    )

    $handle = Start-CapturedHost `
        -ProcessArguments $ProcessArguments `
        -Label $Label `
        -EvidenceDirectory $script:validationRoot
    $result = Complete-CapturedHost -Handle $handle -TimeoutSeconds $TimeoutSeconds
    if ($ExpectedExitCodes -notcontains $result.ExitCode) {
        throw "Unexpected SatsumaHost exit code $($result.ExitCode) for $Label`: $($result.Stderr)"
    }
    return $result
}

# 从 Host stdout 解析唯一 JSON 文档。
function ConvertFrom-HostOutput {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Result
    )

    if ([string]::IsNullOrWhiteSpace($Result.Stdout)) {
        throw 'SatsumaHost returned empty stdout.'
    }
    return $Result.Stdout | ConvertFrom-Json -Depth 64
}

# 返回本轮运行的共享和 Host 归档事实路径。
function Get-RunPaths {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RunId
    )

    $sharedRun = Join-Path $script:sharedRoot (Join-Path 'runs' $RunId)
    $archiveRun = Join-Path $script:archiveRoot (Join-Path 'runs' $RunId)
    $stepRoot = Join-Path $sharedRun (Join-Path 'results' (Join-Path $script:VmId 'crash_step'))
    return [pscustomobject]@{
        SharedRun = $sharedRun
        ArchiveRun = $archiveRun
        Lifecycle = Join-Path $archiveRun 'lifecycle.json'
        Claim = Join-Path $sharedRun (Join-Path 'state' (Join-Path $script:VmId 'crash_step.claim.json'))
        Recovery = Join-Path $sharedRun (Join-Path 'state' (Join-Path $script:VmId 'crash_step.claim-recovery.json'))
        Result = Join-Path $stepRoot 'execution.json'
        MainEvidence = Join-Path $archiveRun `
            (Join-Path 'evidence/main/results' (Join-Path $script:VmId 'crash_step/execution.json'))
        FinallyEvidence = Join-Path $archiveRun `
            (Join-Path 'evidence/finally/results' (Join-Path $script:VmId 'final_guard/execution.json'))
    }
}

# 等待生命周期进入指定阶段。
function Wait-ForLifecyclePhase {
    param(
        [Parameter(Mandatory = $true)]
        [string]$LifecyclePath,

        [Parameter(Mandatory = $true)]
        [string]$Phase,

        [Parameter(Mandatory = $true)]
        [int]$TimeoutSeconds
    )

    return Wait-ForCondition -TimeoutSeconds $TimeoutSeconds -Description "lifecycle phase $Phase" -Probe {
        if (Test-Path -LiteralPath $LifecyclePath -PathType Leaf) {
            $state = Read-JsonFile -Path $LifecyclePath
            if ($state.phase -ceq $Phase) {
                return $state
            }
        }
        return $false
    }
}

# 等待 claim 和至少一份不可变续租 sidecar。
function Wait-ForClaimRenewal {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ClaimPath,

        [Parameter(Mandatory = $true)]
        [int]$TimeoutSeconds
    )

    $claim = Wait-ForCondition -TimeoutSeconds $TimeoutSeconds -Description 'initial step claim' -Probe {
        if (Test-Path -LiteralPath $ClaimPath -PathType Leaf) {
            return Read-JsonFile -Path $ClaimPath
        }
        return $false
    }
    $parent = Split-Path -Parent $ClaimPath
    $filter = "crash_step.claim-renewal-$($claim.job_id)-*.json"
    $renewals = Wait-ForCondition -TimeoutSeconds $TimeoutSeconds -Description 'claim renewal sidecar' -Probe {
        $matches = @(Get-ChildItem -LiteralPath $parent -Filter $filter -File -ErrorAction SilentlyContinue)
        if ($matches.Count -gt 0) {
            return $matches
        }
        return $false
    }
    return [pscustomobject]@{
        Claim = $claim
        Renewals = @($renewals)
    }
}

# 等待 SCM 重启后的新 Agent presence。
function Wait-ForNewPresence {
    param(
        [Parameter(Mandatory = $true)]
        [object]$PreviousPresence,

        [Parameter(Mandatory = $true)]
        [int]$TimeoutSeconds
    )

    return Wait-ForCondition -TimeoutSeconds $TimeoutSeconds -Description 'restarted Agent presence' -Probe {
        if (Test-Path -LiteralPath $script:presencePath -PathType Leaf) {
            $presence = Read-JsonFile -Path $script:presencePath
            if ($presence.boot_id -cne $PreviousPresence.boot_id -and
                [uint32]$presence.process_id -ne [uint32]$PreviousPresence.process_id) {
                return $presence
            }
        }
        return $false
    }
}

# 创建具有固定 run_id 和稳定字节的生命周期计划。
function New-CrashPlan {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [bool]$RetrySafe,

        [Parameter(Mandatory = $true)]
        [bool]$CrashAgent
    )

    $runTimestamp = [DateTime]::UtcNow.ToString('yyyyMMddHHmmss')
    $runSuffix = [Guid]::NewGuid().ToString('N').Substring(0, 8)
    $runId = "real_$Name`_$runTimestamp`_$runSuffix"
    $artifactDestination = "artifacts/$($script:VmId)/SatsumaTestFixture.exe"
    $fixtureArguments = if ($CrashAgent) {
        @(
            '--message', "$Name completed",
            '--sleep-ms', [string]$AgentCrashDelayMs,
            '--crash-parent-once', 'fault/agent-parent-crash.marker'
        )
    }
    else {
        @('--message', "$Name completed", '--sleep-ms', [string]$HostTaskSleepMs)
    }
    $plan = [ordered]@{
        schema_version = 1
        name = "real VMware $Name"
        run_id = $runId
        artifacts = @(
            [ordered]@{
                source = $script:resolvedFixtureExe
                vm = $script:VmId
                shared_destination = $artifactDestination
            }
        )
        steps = @(
            [ordered]@{
                id = 'crash_step'
                vm = $script:VmId
                type = 'execute'
                program = $artifactDestination
                arguments = $fixtureArguments
                timeout_seconds = 300
                retry_safe = $RetrySafe
                collect_files = @()
            }
        )
        lifecycle = [ordered]@{
            vms = @(
                [ordered]@{
                    vm = $script:VmId
                    on_success = [ordered]@{action = 'leave_running'}
                    on_failure = [ordered]@{action = 'leave_running'}
                }
            )
            finally = @(
                [ordered]@{
                    id = 'final_guard'
                    vm = $script:VmId
                    type = 'echo'
                    message = "$Name finally completed"
                    timeout_seconds = 30
                    retry_safe = $true
                }
            )
        }
    }
    $planPath = Join-Path $script:validationRoot "$runId.plan.json"
    Write-JsonFile -Path $planPath -Value $plan
    return [pscustomobject]@{
        RunId = $runId
        Path = $planPath
        Sha256 = (Get-FileHash -LiteralPath $planPath -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

# 验证成功编排的主任务与 finally 归档均存在。
function Assert-CompletedEvidence {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Paths
    )

    if (-not (Test-Path -LiteralPath $Paths.MainEvidence -PathType Leaf)) {
        throw "Main execution evidence is missing: $($Paths.MainEvidence)"
    }
    if (-not (Test-Path -LiteralPath $Paths.FinallyEvidence -PathType Leaf)) {
        throw "Finally execution evidence is missing: $($Paths.FinallyEvidence)"
    }
}

# 验证 Host 在 executing 阶段强杀后的同计划续跑。
function Invoke-HostCrashScenario {
    $plan = New-CrashPlan -Name 'host_crash' -RetrySafe $true -CrashAgent $false
    $paths = Get-RunPaths -RunId $plan.RunId
    $processArguments = @(
        'orchestrate', '--config', $script:resolvedLabConfig,
        '--plan', $plan.Path,
        '--timeout-seconds', [string]$ScenarioTimeoutSeconds
    )
    $handle = Start-CapturedHost `
        -ProcessArguments $processArguments `
        -Label "$($plan.RunId).initial" `
        -EvidenceDirectory $script:validationRoot
    try {
        [void](Wait-ForLifecyclePhase `
            -LifecyclePath $paths.Lifecycle `
            -Phase 'executing' `
            -TimeoutSeconds 180)
        $claimEvidence = Wait-ForClaimRenewal `
            -ClaimPath $paths.Claim `
            -TimeoutSeconds 120
        if (Test-Path -LiteralPath $paths.Result) {
            throw 'Host crash fixture completed before the injected Host termination.'
        }
        $terminated = Stop-CapturedHost -Handle $handle
        $handle = $null

        $resumedResult = Invoke-HostCommand `
            -ProcessArguments $processArguments `
            -ExpectedExitCodes @(0) `
            -Label "$($plan.RunId).resumed" `
            -TimeoutSeconds $ScenarioTimeoutSeconds
        $resumed = ConvertFrom-HostOutput -Result $resumedResult
        if ($resumed.status -cne 'COMPLETED' -or -not $resumed.resumed) {
            throw 'Host crash resume did not return resumed COMPLETED.'
        }
        if ((Get-FileHash -LiteralPath $plan.Path -Algorithm SHA256).Hash.ToLowerInvariant() -cne
            $plan.Sha256) {
            throw 'Host crash plan bytes changed during resume.'
        }
        $claim = Read-JsonFile -Path $paths.Claim
        $execution = Read-JsonFile -Path $paths.Result
        if ([uint32]$claim.attempt -ne 1 -or $execution.job_id -cne $claim.job_id) {
            throw 'Host crash resume changed the Agent claim owner or canonical result.'
        }
        Assert-CompletedEvidence -Paths $paths

        $terminalResult = Invoke-HostCommand `
            -ProcessArguments $processArguments `
            -ExpectedExitCodes @(0) `
            -Label "$($plan.RunId).terminal" `
            -TimeoutSeconds 30
        $terminal = ConvertFrom-HostOutput -Result $terminalResult
        if ($terminal.status -cne 'COMPLETED' -or -not $terminal.resumed) {
            throw 'Host crash terminal replay was not idempotent.'
        }

        return [ordered]@{
            name = 'HostCrash'
            status = 'passed'
            run_id = $plan.RunId
            plan_path = $plan.Path
            plan_sha256 = $plan.Sha256
            terminated_host_pid = $terminated.ProcessId
            terminated_host_exit_code = $terminated.ExitCode
            claim_job_id = $claim.job_id
            claim_boot_id = $claim.boot_id
            claim_attempt = [uint32]$claim.attempt
            renewal_count = @($claimEvidence.Renewals).Count
            lifecycle_path = $paths.Lifecycle
            main_evidence = $paths.MainEvidence
            finally_evidence = $paths.FinallyEvidence
        }
    }
    finally {
        if ($null -ne $handle) {
            if (-not $handle.Process.HasExited) {
                $handle.Process.Kill($true)
                [void]$handle.Process.WaitForExit(10000)
            }
            $handle.Process.Dispose()
        }
    }
}

# 验证 Agent 强杀后的安全重试或非安全人工门禁。
function Invoke-AgentCrashScenario {
    param(
        [Parameter(Mandatory = $true)]
        [bool]$RetrySafe
    )

    $name = if ($RetrySafe) { 'agent_retry_safe' } else { 'agent_unsafe' }
    $expectedStatus = if ($RetrySafe) { 'COMPLETED' } else { 'MANUAL_INTERVENTION_REQUIRED' }
    $expectedExitCode = if ($RetrySafe) { 0 } else { 5 }
    $plan = New-CrashPlan -Name $name -RetrySafe $RetrySafe -CrashAgent $true
    $paths = Get-RunPaths -RunId $plan.RunId
    $presenceBefore = Read-JsonFile -Path $script:presencePath
    $processArguments = @(
        'orchestrate', '--config', $script:resolvedLabConfig,
        '--plan', $plan.Path,
        '--timeout-seconds', [string]$ScenarioTimeoutSeconds
    )
    $handle = Start-CapturedHost `
        -ProcessArguments $processArguments `
        -Label $plan.RunId `
        -EvidenceDirectory $script:validationRoot
    try {
        [void](Wait-ForLifecyclePhase `
            -LifecyclePath $paths.Lifecycle `
            -Phase 'executing' `
            -TimeoutSeconds 180)
        $claimEvidence = Wait-ForClaimRenewal `
            -ClaimPath $paths.Claim `
            -TimeoutSeconds 120
        $presenceAfter = Wait-ForNewPresence `
            -PreviousPresence $presenceBefore `
            -TimeoutSeconds 120
        $hostResult = Complete-CapturedHost `
            -Handle $handle `
            -TimeoutSeconds $ScenarioTimeoutSeconds
        $handle = $null
        if ($hostResult.ExitCode -ne $expectedExitCode) {
            throw "Agent crash scenario returned Host exit code $($hostResult.ExitCode)"
        }
        $hostOutput = ConvertFrom-HostOutput -Result $hostResult
        if ($hostOutput.status -cne $expectedStatus) {
            throw "Agent crash scenario returned $($hostOutput.status), expected $expectedStatus"
        }

        $currentClaim = Read-JsonFile -Path $paths.Claim
        $archivedClaims = @(Get-ChildItem `
            -Path "$($paths.Claim).expired-attempt-1-*" `
            -File `
            -ErrorAction SilentlyContinue)
        $execution = $null
        if ($RetrySafe) {
            if ($archivedClaims.Count -ne 1) {
                throw "Safe Agent crash retained $($archivedClaims.Count) archived attempt 1 claims."
            }
            $archivedClaim = Read-JsonFile -Path $archivedClaims[0].FullName
            $execution = Read-JsonFile -Path $paths.Result
            if ([uint32]$currentClaim.attempt -ne 2 -or
                $currentClaim.job_id -ceq $claimEvidence.Claim.job_id -or
                $currentClaim.boot_id -ceq $claimEvidence.Claim.boot_id -or
                $currentClaim.session_id -ceq $claimEvidence.Claim.session_id -or
                $execution.job_id -cne $currentClaim.job_id -or
                $archivedClaim.job_id -cne $claimEvidence.Claim.job_id) {
                throw 'Safe Agent crash did not fence attempt 1 from attempt 2.'
            }
            Assert-CompletedEvidence -Paths $paths
        }
        else {
            if ($archivedClaims.Count -ne 0 -or
                [uint32]$currentClaim.attempt -ne 1 -or
                $currentClaim.job_id -cne $claimEvidence.Claim.job_id -or
                (Test-Path -LiteralPath $paths.Result) -or
                (Test-Path -LiteralPath $paths.FinallyEvidence)) {
                throw 'Unsafe Agent crash replayed work or published forbidden evidence.'
            }
            $recovery = Read-JsonFile -Path $paths.Recovery
            if ($recovery.status -cne 'manual_intervention_required') {
                throw 'Unsafe Agent crash did not publish its recovery gate.'
            }
        }

        return [ordered]@{
            name = if ($RetrySafe) { 'AgentRetrySafe' } else { 'AgentUnsafe' }
            status = 'passed'
            run_id = $plan.RunId
            plan_path = $plan.Path
            plan_sha256 = $plan.Sha256
            old_process_id = [uint32]$presenceBefore.process_id
            new_process_id = [uint32]$presenceAfter.process_id
            old_boot_id = $presenceBefore.boot_id
            new_boot_id = $presenceAfter.boot_id
            old_job_id = $claimEvidence.Claim.job_id
            current_job_id = $currentClaim.job_id
            current_attempt = [uint32]$currentClaim.attempt
            renewal_count = @($claimEvidence.Renewals).Count
            canonical_result = if ($null -eq $execution) { $null } else { $paths.Result }
            recovery_gate = if ($RetrySafe) { $null } else { $paths.Recovery }
            lifecycle_path = $paths.Lifecycle
            main_evidence = $paths.MainEvidence
            finally_evidence = if ($RetrySafe) { $paths.FinallyEvidence } else { $null }
        }
    }
    finally {
        if ($null -ne $handle) {
            if (-not $handle.Process.HasExited) {
                $handle.Process.Kill($true)
                [void]$handle.Process.WaitForExit(10000)
            }
            $handle.Process.Dispose()
        }
    }
}

$resolvedHostExe = Resolve-RequiredFile -Path $HostExe -Description 'SatsumaHost executable'
$resolvedFixtureExe = Resolve-RequiredFile -Path $FixtureExe -Description 'Crash fixture executable'
$resolvedLabConfig = Resolve-RequiredFile -Path $LabConfig -Description 'Lab config'
$lab = Read-JsonFile -Path $resolvedLabConfig
$matchingVms = @($lab.vms | Where-Object { $_.id -ceq $VmId })
if ($matchingVms.Count -ne 1) {
    throw "Lab config must contain exactly one VM with id $VmId"
}
$sharedRoot = [System.IO.Path]::GetFullPath([string]$lab.shared_folder.host_root)
$archiveRoot = [System.IO.Path]::GetFullPath([string]$lab.host.archive_root)
if (-not (Test-Path -LiteralPath $sharedRoot -PathType Container)) {
    throw "Shared root does not exist: $sharedRoot"
}
if (-not (Test-Path -LiteralPath $archiveRoot -PathType Container)) {
    throw "Archive root does not exist: $archiveRoot"
}
$presencePath = Join-Path $sharedRoot (Join-Path 'agents' "$VmId.json")
$validationTimestamp = [DateTime]::UtcNow.ToString('yyyyMMddHHmmss')
$validationSuffix = [Guid]::NewGuid().ToString('N').Substring(0, 8)
$validationId = "real-crash-$validationTimestamp-$validationSuffix"
$validationRoot = Join-Path $archiveRoot (Join-Path 'validation' $validationId)
[void](New-Item -ItemType Directory -Path $validationRoot)
$summaryPath = Join-Path $validationRoot 'summary.json'
$summary = [ordered]@{
    schema_version = 1
    type = 'real_vmware_crash_recovery'
    status = 'running'
    scenario = $Scenario
    vm_id = $VmId
    vmx = [string]$matchingVms[0].vmx
    host_executable = $resolvedHostExe
    fixture_executable = $resolvedFixtureExe
    fixture_sha256 = (Get-FileHash -LiteralPath $resolvedFixtureExe -Algorithm SHA256).Hash.ToLowerInvariant()
    lab_config = $resolvedLabConfig
    evidence_root = $validationRoot
    started_at = [DateTime]::UtcNow.ToString('o')
    preflight = $null
    scenarios = [System.Collections.Generic.List[object]]::new()
    final_check = $null
    error = $null
}
Write-JsonFile -Path $summaryPath -Value $summary

try {
    $preflightResult = Invoke-HostCommand `
        -ProcessArguments @(
            'check', '--config', $resolvedLabConfig,
            '--vm', $VmId,
            '--timeout-seconds', '180'
        ) `
        -ExpectedExitCodes @(0) `
        -Label 'preflight' `
        -TimeoutSeconds 210
    $preflight = ConvertFrom-HostOutput -Result $preflightResult
    if ($preflight.status -cne 'ready') {
        throw "Preflight did not return ready: $($preflight.status)"
    }
    $summary.preflight = [ordered]@{
        status = $preflight.status
        run_id = $preflight.run_id
        report = $preflightResult.StdoutPath
    }
    Write-JsonFile -Path $summaryPath -Value $summary

    if ($Scenario -eq 'All' -or $Scenario -eq 'HostCrash') {
        $summary.scenarios.Add((Invoke-HostCrashScenario))
        Write-JsonFile -Path $summaryPath -Value $summary
    }
    if ($Scenario -eq 'All' -or $Scenario -eq 'AgentRetrySafe') {
        $summary.scenarios.Add((Invoke-AgentCrashScenario -RetrySafe $true))
        Write-JsonFile -Path $summaryPath -Value $summary
    }
    if ($Scenario -eq 'All' -or $Scenario -eq 'AgentUnsafe') {
        $summary.scenarios.Add((Invoke-AgentCrashScenario -RetrySafe $false))
        Write-JsonFile -Path $summaryPath -Value $summary
    }

    $finalResult = Invoke-HostCommand `
        -ProcessArguments @(
            'check', '--config', $resolvedLabConfig,
            '--vm', $VmId,
            '--timeout-seconds', '180'
        ) `
        -ExpectedExitCodes @(0) `
        -Label 'final-check' `
        -TimeoutSeconds 210
    $finalCheck = ConvertFrom-HostOutput -Result $finalResult
    if ($finalCheck.status -cne 'ready') {
        throw "Final check did not return ready: $($finalCheck.status)"
    }
    $summary.final_check = [ordered]@{
        status = $finalCheck.status
        run_id = $finalCheck.run_id
        report = $finalResult.StdoutPath
    }
    $summary.status = 'passed'
    $summary.finished_at = [DateTime]::UtcNow.ToString('o')
    Write-JsonFile -Path $summaryPath -Value $summary
    Write-Output ($summary | ConvertTo-Json -Depth 64)
    exit 0
}
catch {
    $summary.status = 'failed'
    $summary.error = $_.Exception.Message
    $summary.finished_at = [DateTime]::UtcNow.ToString('o')
    Write-JsonFile -Path $summaryPath -Value $summary
    [Console]::Error.WriteLine("Real crash recovery failed: $($_.Exception.Message)")
    [Console]::Error.WriteLine("Summary: $summaryPath")
    exit 1
}
