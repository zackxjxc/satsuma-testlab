# AI 编写的真实 VMware Shared Folder 瞬断恢复验收驱动。
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

    [Parameter(Mandatory = $true)]
    [string]$Confirm,

    [ValidateRange(120, 900)]
    [int]$ScenarioTimeoutSeconds = 300,

    [ValidateRange(10, 90)]
    [int]$OutageSeconds = 35,

    [ValidateRange(60000, 300000)]
    [int]$TaskSleepMs = 90000
)

Write-Host 'Satsuma real VMware fault recovery driver (AI-authored).'

$ErrorActionPreference = 'Stop'
$requiredConfirmation = 'I_UNDERSTAND_SHARED_FOLDER_WILL_BE_TEMPORARILY_DISABLED'
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)

if ($PSVersionTable.PSEdition -ne 'Core' -or $PSVersionTable.PSVersion.Major -lt 7) {
    throw 'The real fault recovery driver requires PowerShell 7 or newer.'
}
if (-not $IsWindows) {
    throw 'The real fault recovery driver only supports Windows.'
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
        [string]$Label
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
    $startPath = [System.IO.Path]::GetFullPath($startInfo.FileName)
    if (-not $startPath.Equals(
            $script:resolvedHostExe,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Process start path does not match SatsumaHost for $Label"
    }

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw "Failed to start SatsumaHost for $Label"
    }
    return [pscustomobject]@{
        Label = $Label
        Process = $process
        StdoutTask = $process.StandardOutput.ReadToEndAsync()
        StderrTask = $process.StandardError.ReadToEndAsync()
        StdoutPath = Join-Path $script:validationRoot "$Label.stdout.log"
        StderrPath = Join-Path $script:validationRoot "$Label.stderr.log"
    }
}

# 完成已捕获的 Host 进程并保存诊断文本。
function Complete-CapturedHost {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Handle,

        [Parameter(Mandatory = $true)]
        [int]$TimeoutSeconds
    )

    $timedOut = -not $Handle.Process.WaitForExit($TimeoutSeconds * 1000)
    if ($timedOut) {
        $Handle.Process.Kill($true)
        [void]$Handle.Process.WaitForExit(10000)
    }
    $stdout = $Handle.StdoutTask.GetAwaiter().GetResult()
    $stderr = $Handle.StderrTask.GetAwaiter().GetResult()
    Write-TextFile -Path $Handle.StdoutPath -Text $stdout
    Write-TextFile -Path $Handle.StderrPath -Text $stderr
    $result = [pscustomobject]@{
        ExitCode = $Handle.Process.ExitCode
        Stdout = $stdout
        Stderr = $stderr
        StdoutPath = $Handle.StdoutPath
        StderrPath = $Handle.StderrPath
    }
    $Handle.Process.Dispose()
    if ($timedOut) {
        throw "SatsumaHost timed out for $($Handle.Label); stdout: $($result.StdoutPath); stderr: $($result.StderrPath)"
    }
    return $result
}

# 同步执行 Host 命令并验证允许的退出码。
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

    $handle = Start-CapturedHost -ProcessArguments $ProcessArguments -Label $Label
    $result = Complete-CapturedHost -Handle $handle -TimeoutSeconds $TimeoutSeconds
    if ($ExpectedExitCodes -notcontains $result.ExitCode) {
        $message = "Unexpected SatsumaHost exit code $($result.ExitCode) for $Label" +
            "; stdout: $($result.StdoutPath); stderr: $($result.StderrPath)"
        throw $message
    }
    return $result
}

# 将单份 Host stdout 解析为 JSON。
function ConvertFrom-HostOutput {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Result
    )

    if ([string]::IsNullOrWhiteSpace($Result.Stdout)) {
        throw "SatsumaHost returned empty stdout: $($Result.StdoutPath)"
    }
    return $Result.Stdout | ConvertFrom-Json -Depth 64
}

# 使用结构化原生参数调用当前配置中的 vmrun。
function Invoke-Vmrun {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$ProcessArguments
    )

    $output = & $script:resolvedVmrun @ProcessArguments 2>&1 |
        ForEach-Object { "$_" }
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        $text = $output -join "`n"
        throw "vmrun exited with code $exitCode for $($ProcessArguments[0]): $text"
    }
    return @($output)
}

# 创建唯一 retry-safe 长任务计划。
function New-FaultPlan {
    $runTimestamp = [DateTime]::UtcNow.ToString('yyyyMMddHHmmss')
    $runSuffix = [Guid]::NewGuid().ToString('N').Substring(0, 8)
    $runId = "real_shared_outage_$runTimestamp`_$runSuffix"
    $artifactDestination = "artifacts/$($script:VmId)/SatsumaTestFixture.exe"
    $timeoutSeconds = [Math]::Ceiling($script:TaskSleepMs / 1000) + 60
    $plan = [ordered]@{
        schema_version = 1
        name = 'real VMware Shared Folder outage recovery'
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
                id = 'shared_outage'
                vm = $script:VmId
                type = 'execute'
                program = $artifactDestination
                arguments = @(
                    '--message', 'Shared Folder outage recovered',
                    '--sleep-ms', [string]$script:TaskSleepMs
                )
                timeout_seconds = [int]$timeoutSeconds
                retry_safe = $true
                collect_files = @()
            }
        )
    }
    $planPath = Join-Path $script:validationRoot "$runId.plan.json"
    Write-JsonFile -Path $planPath -Value $plan
    return [pscustomobject]@{
        RunId = $runId
        Path = $planPath
        Sha256 = (Get-FileHash -LiteralPath $planPath -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

# 返回当前 claim owner 的全部不可变续租 sidecar。
function Get-RenewalFiles {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ClaimPath,

        [Parameter(Mandatory = $true)]
        [string]$JobId
    )

    $parent = Split-Path -Parent $ClaimPath
    $filter = "shared_outage.claim-renewal-$JobId-*.json"
    return @(Get-ChildItem `
        -LiteralPath $parent `
        -Filter $filter `
        -File `
        -ErrorAction SilentlyContinue | Sort-Object Name)
}

# 验证续租序号连续且全部属于原 claim owner。
function Assert-Renewals {
    param(
        [Parameter(Mandatory = $true)]
        [object[]]$Files,

        [Parameter(Mandatory = $true)]
        [string]$JobId
    )

    for ($index = 0; $index -lt $Files.Count; $index++) {
        $renewal = Read-JsonFile -Path $Files[$index].FullName
        if ([uint64]$renewal.renewal_sequence -ne [uint64]($index + 1) -or
            $renewal.job_id -cne $JobId) {
            throw 'Shared Folder recovery produced a renewal sequence gap or changed owner.'
        }
    }
}

$resolvedHostExe = Resolve-RequiredFile -Path $HostExe -Description 'SatsumaHost executable'
$resolvedFixtureExe = Resolve-RequiredFile -Path $FixtureExe -Description 'Fault fixture executable'
$resolvedLabConfig = Resolve-RequiredFile -Path $LabConfig -Description 'Lab config'
$lab = Read-JsonFile -Path $resolvedLabConfig
$matchingVms = @($lab.vms | Where-Object { $_.id -ceq $VmId })
if ($matchingVms.Count -ne 1) {
    throw "Lab config must contain exactly one VM with id $VmId"
}
$resolvedVmrun = Resolve-RequiredFile -Path ([string]$lab.provider.vmrun) -Description 'vmrun executable'
$resolvedVmx = Resolve-RequiredFile -Path ([string]$matchingVms[0].vmx) -Description 'VMX'
$sharedRoot = [System.IO.Path]::GetFullPath([string]$lab.shared_folder.host_root)
$archiveRoot = [System.IO.Path]::GetFullPath([string]$lab.host.archive_root)
if (-not (Test-Path -LiteralPath $sharedRoot -PathType Container)) {
    throw "Shared root does not exist: $sharedRoot"
}
if (-not (Test-Path -LiteralPath $archiveRoot -PathType Container)) {
    throw "Archive root does not exist: $archiveRoot"
}
$hardwareId = [string]$matchingVms[0].hardware_id
if ([string]::IsNullOrWhiteSpace($hardwareId)) {
    throw "Lab config VM $VmId does not define hardware_id"
}
$presencePath = Join-Path $sharedRoot (Join-Path 'agents' "$hardwareId.json")
$validationTimestamp = [DateTime]::UtcNow.ToString('yyyyMMddHHmmss')
$validationSuffix = [Guid]::NewGuid().ToString('N').Substring(0, 8)
$validationId = "real-fault-$validationTimestamp-$validationSuffix"
$validationRoot = Join-Path $archiveRoot (Join-Path 'validation' $validationId)
[void](New-Item -ItemType Directory -Path $validationRoot)
$summaryPath = Join-Path $validationRoot 'summary.json'
$summary = [ordered]@{
    schema_version = 1
    type = 'real_vmware_shared_folder_recovery'
    status = 'running'
    vm_id = $VmId
    vmx = $resolvedVmx
    vmrun = $resolvedVmrun
    host_executable = $resolvedHostExe
    fixture_executable = $resolvedFixtureExe
    fixture_sha256 = (Get-FileHash -LiteralPath $resolvedFixtureExe -Algorithm SHA256).Hash.ToLowerInvariant()
    lab_config = $resolvedLabConfig
    evidence_root = $validationRoot
    outage_seconds = $OutageSeconds
    started_at = [DateTime]::UtcNow.ToString('o')
    preflight = $null
    run = $null
    final_check = $null
    error = $null
}
Write-JsonFile -Path $summaryPath -Value $summary

try {
    $preflightResult = Invoke-HostCommand `
        -ProcessArguments @(
            'check', '--config', $resolvedLabConfig,
            '--vm', $VmId,
            '--timeout-seconds', '90'
        ) `
        -ExpectedExitCodes @(0) `
        -Label 'preflight' `
        -TimeoutSeconds 120
    $preflight = ConvertFrom-HostOutput -Result $preflightResult
    if ($preflight.status -cne 'ready') {
        throw "Preflight did not return ready: $($preflight.status)"
    }
    $presenceBefore = Read-JsonFile -Path $presencePath
    $summary.preflight = [ordered]@{
        status = $preflight.status
        run_id = $preflight.run_id
        tools_state = @($preflight.checks | Where-Object { $_.name -ceq 'vmware_tools' })[0].details.state
        report = $preflightResult.StdoutPath
    }
    Write-JsonFile -Path $summaryPath -Value $summary

    $plan = New-FaultPlan
    $runResult = Invoke-HostCommand `
        -ProcessArguments @('run', '--config', $resolvedLabConfig, '--plan', $plan.Path) `
        -ExpectedExitCodes @(0) `
        -Label "$($plan.RunId).run" `
        -TimeoutSeconds 30
    $published = ConvertFrom-HostOutput -Result $runResult
    if ($published.status -cne 'prepared' -or $published.run_id -cne $plan.RunId) {
        throw 'Host did not publish the expected Shared Folder fault plan.'
    }

    $runRoot = Join-Path $sharedRoot (Join-Path 'runs' $plan.RunId)
    $claimPath = Join-Path $runRoot (Join-Path 'state' (Join-Path $VmId 'shared_outage.claim.json'))
    $resultPath = Join-Path $runRoot (
        Join-Path 'results' (Join-Path $VmId (Join-Path 'shared_outage' 'execution.json')))
    $claimEvidence = Wait-ForCondition `
        -TimeoutSeconds 120 `
        -Description 'schema 3 claim and first renewal' `
        -Probe {
            if (-not (Test-Path -LiteralPath $claimPath -PathType Leaf)) {
                return $false
            }
            $claim = Read-JsonFile -Path $claimPath
            if ([int]$claim.schema_version -ne 3) {
                throw 'Real Shared Folder recovery requires claim schema 3.'
            }
            $renewals = @(Get-RenewalFiles -ClaimPath $claimPath -JobId $claim.job_id)
            if ($renewals.Count -ge 1) {
                return [pscustomobject]@{Claim = $claim; Renewals = $renewals}
            }
            return $false
        }
    if (Test-Path -LiteralPath $resultPath) {
        throw 'Fault fixture completed before Shared Folder injection.'
    }

    $restoreRequired = $true
    $outageStartedAt = [DateTime]::UtcNow
    try {
        [void](Invoke-Vmrun -ProcessArguments @(
            'disableSharedFolders', $resolvedVmx, 'runtime'))
        Start-Sleep -Seconds 3
        $renewalsAtBaseline = @(
            Get-RenewalFiles -ClaimPath $claimPath -JobId $claimEvidence.Claim.job_id)
        $presenceAtBaseline = (Get-Item -LiteralPath $presencePath).LastWriteTimeUtc
        Start-Sleep -Seconds ($OutageSeconds - 3)
        $renewalsDuringOutage = @(
            Get-RenewalFiles -ClaimPath $claimPath -JobId $claimEvidence.Claim.job_id)
        $presenceDuringOutage = (Get-Item -LiteralPath $presencePath).LastWriteTimeUtc
        if ($renewalsDuringOutage.Count -ne $renewalsAtBaseline.Count) {
            throw 'Claim renewal changed while the Shared Folder was disabled.'
        }
        if ($presenceDuringOutage -ne $presenceAtBaseline) {
            throw 'Agent presence changed while the Shared Folder was disabled.'
        }
        if (Test-Path -LiteralPath $resultPath) {
            throw 'Canonical result appeared while the Shared Folder was disabled.'
        }
    }
    finally {
        if ($restoreRequired) {
            [void](Invoke-Vmrun -ProcessArguments @(
                'enableSharedFolders', $resolvedVmx, 'runtime'))
            $restoreRequired = $false
        }
    }
    $outageFinishedAt = [DateTime]::UtcNow

    $renewalsAfter = Wait-ForCondition `
        -TimeoutSeconds $ScenarioTimeoutSeconds `
        -Description 'claim renewal after Shared Folder recovery' `
        -Probe {
            $files = @(Get-RenewalFiles -ClaimPath $claimPath -JobId $claimEvidence.Claim.job_id)
            if ($files.Count -gt $renewalsDuringOutage.Count) {
                return $files
            }
            return $false
        }
    $execution = Wait-ForCondition `
        -TimeoutSeconds $ScenarioTimeoutSeconds `
        -Description 'canonical result after Shared Folder recovery' `
        -Probe {
            if (Test-Path -LiteralPath $resultPath -PathType Leaf) {
                return Read-JsonFile -Path $resultPath
            }
            return $false
        }
    $presenceAfter = Wait-ForCondition `
        -TimeoutSeconds 60 `
        -Description 'presence refresh after Shared Folder recovery' `
        -Probe {
            $item = Get-Item -LiteralPath $presencePath
            if ($item.LastWriteTimeUtc -gt $presenceDuringOutage) {
                return Read-JsonFile -Path $presencePath
            }
            return $false
        }

    $currentClaim = Read-JsonFile -Path $claimPath
    if ([uint32]$currentClaim.attempt -ne 1 -or
        $currentClaim.job_id -cne $claimEvidence.Claim.job_id -or
        $execution.job_id -cne $currentClaim.job_id -or
        $execution.status -cne 'exited' -or
        [uint32]$execution.exit_code -ne 0 -or
        $execution.timed_out) {
        throw 'Shared Folder recovery changed owner, retried work, or produced a failed result.'
    }
    if ($presenceAfter.boot_id -cne $presenceBefore.boot_id -or
        [uint32]$presenceAfter.process_id -ne [uint32]$presenceBefore.process_id) {
        throw 'Shared Folder recovery unexpectedly restarted the Agent.'
    }
    $archivedClaims = @(Get-ChildItem `
        -Path "$claimPath.expired-attempt-*" `
        -File `
        -ErrorAction SilentlyContinue)
    if ($archivedClaims.Count -ne 0) {
        throw 'Shared Folder recovery archived the live claim or created attempt 2.'
    }
    Assert-Renewals -Files @($renewalsAfter) -JobId $currentClaim.job_id

    $reportResult = Invoke-HostCommand `
        -ProcessArguments @(
            'report', '--config', $resolvedLabConfig,
            '--run', $plan.RunId,
            '--wait-seconds', [string]$ScenarioTimeoutSeconds
        ) `
        -ExpectedExitCodes @(0) `
        -Label "$($plan.RunId).report" `
        -TimeoutSeconds $ScenarioTimeoutSeconds
    $report = ConvertFrom-HostOutput -Result $reportResult
    if (-not $report.complete -or [uint32]$report.successful_steps -ne 1) {
        throw 'Host report did not preserve exactly one successful fault step.'
    }

    $summary.run = [ordered]@{
        status = 'passed'
        run_id = $plan.RunId
        plan_path = $plan.Path
        plan_sha256 = $plan.Sha256
        job_id = $currentClaim.job_id
        attempt = [uint32]$currentClaim.attempt
        boot_id = $currentClaim.boot_id
        process_id = [uint32]$presenceAfter.process_id
        outage_started_at = $outageStartedAt.ToString('o')
        outage_finished_at = $outageFinishedAt.ToString('o')
        renewal_count_before_outage = @($claimEvidence.Renewals).Count
        renewal_count_at_baseline = $renewalsAtBaseline.Count
        renewal_count_during_outage = $renewalsDuringOutage.Count
        renewal_count_after_recovery = @($renewalsAfter).Count
        presence_write_at_baseline = $presenceAtBaseline.ToString('o')
        presence_write_during_outage = $presenceDuringOutage.ToString('o')
        canonical_result = $resultPath
        report = $reportResult.StdoutPath
    }
    Write-JsonFile -Path $summaryPath -Value $summary

    $finalResult = Invoke-HostCommand `
        -ProcessArguments @(
            'check', '--config', $resolvedLabConfig,
            '--vm', $VmId,
            '--timeout-seconds', '90'
        ) `
        -ExpectedExitCodes @(0) `
        -Label 'final-check' `
        -TimeoutSeconds 120
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
    [Console]::Error.WriteLine("Real fault recovery failed: $($_.Exception.Message)")
    [Console]::Error.WriteLine("Summary: $summaryPath")
    exit 1
}
