# AI-authored, non-destructive ordinary-user access checks for an isolated Guest.
$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if ($identity.IsSystem -or $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Expected a non-elevated interactive user' }
$root = Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) 'SatsumaTestLab'
$checks = @()
foreach ($relative in @('agent\bin\SatsumaVM.exe','agent\agent.json')) {
    $denied = $false
    try {
        $stream = [IO.File]::Open((Join-Path $root $relative),[IO.FileMode]::Open,[IO.FileAccess]::Write,[IO.FileShare]::ReadWrite)
        $stream.Dispose()
    } catch [UnauthorizedAccessException] { $denied = $true }
    $checks += @{path=$relative;write_denied=$denied}
    if (-not $denied) { throw "Protected file accepted write access: $relative" }
}
foreach ($relative in @('agent','mirror','work','staging')) {
    $target = Join-Path (Join-Path $root $relative) ('acceptance-'+[guid]::NewGuid().ToString('N'))
    $denied = $false
    try { [IO.File]::WriteAllText($target,'test') }
    catch [UnauthorizedAccessException] { $denied = $true }
    if (-not $denied) {
        # Only remove the exact file just created by this probe.
        Remove-Item -LiteralPath $target
        throw "Protected directory accepted a new file: $relative"
    }
    $checks += @{path=$relative;write_denied=$denied}
}
$result = @{passed=$true;user=$identity.Name;is_admin=$false;checks=$checks;cwd=(Get-Location).Path}
[IO.File]::WriteAllText((Join-Path (Get-Location).Path 'permissions.json'),($result | ConvertTo-Json -Depth 8),(New-Object Text.UTF8Encoding($false)))
Write-Output 'Ordinary-user protected file and directory writes were denied.'
