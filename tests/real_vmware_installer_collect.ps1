# AI-authored developer fixture; collect only known test logs, never full Agent config.
$ErrorActionPreference = 'Stop'
$context = Get-Content (Join-Path $PSScriptRoot 'context.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$root = [IO.Path]::GetFullPath($context.test_root)
if ((Split-Path $root -Parent) -ne [Environment]::GetFolderPath('CommonApplicationData') -or
    (Split-Path $root -Leaf) -notmatch '^SatsumaAcceptance-[a-f0-9]{12}$' -or
    (Get-CimInstance Win32_ComputerSystemProduct).UUID -ine $context.hardware_id) { throw 'Invalid fixture scope' }
$result = Get-Content (Join-Path $root 'result.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$logs = [ordered]@{}
foreach ($file in Get-ChildItem -LiteralPath $root -File -Filter '*.log') {
    $logs[$file.Name] = [IO.File]::ReadAllText($file.FullName,[Text.Encoding]::UTF8)
}
$output = [ordered]@{result=$result;logs=$logs;service=(Get-CimInstance Win32_Service -Filter "Name='SatsumaVM'" | Select-Object Name,State,StartMode,PathName)}
[IO.File]::WriteAllText((Join-Path (Get-Location).Path 'installer-results.json'),($output | ConvertTo-Json -Depth 20),(New-Object Text.UTF8Encoding($false)))
if (-not $result.success -or -not $result.restoration) { throw 'Installer acceptance failed; see collected evidence' }
Write-Output 'Installer matrix and original-installation restoration passed.'
