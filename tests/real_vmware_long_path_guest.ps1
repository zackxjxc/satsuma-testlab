# AI-authored developer probe; explicit Win32 file APIs isolate Agent path support from PowerShell's own limits.
$ErrorActionPreference = 'Stop'
$description = Get-Content (Join-Path $PSScriptRoot 'long-path.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$work = (Get-Location).Path
foreach ($relative in @($description.source,$description.target)) {
    if ([IO.Path]::IsPathRooted($relative) -or $relative -match '(^|[/\\])\.\.([/\\]|$)') { throw 'Unsafe probe path' }
}
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class SatsumaPathProbe {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern bool CreateDirectoryW(string path, IntPtr attributes);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern bool CopyFileW(string source, string target, bool failIfExists);
}
'@
$target = $work + '\' + $description.target.Replace('/','\')
$source = $work + '\' + $description.source.Replace('/','\')
$parts = $description.target.Replace('/','\').Split('\')
$current = $work
for ($index=0; $index -lt $parts.Length-1; ++$index) {
    $current += '\' + $parts[$index]
    if (-not [SatsumaPathProbe]::CreateDirectoryW(('\\?\'+$current),[IntPtr]::Zero)) {
        $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        if ($errorCode -ne 183) { throw "CreateDirectoryW failed: $errorCode" }
    }
}
if (-not [SatsumaPathProbe]::CopyFileW(('\\?\'+$source),('\\?\'+$target),$true)) {
    throw ('CopyFileW failed: '+[Runtime.InteropServices.Marshal]::GetLastWin32Error())
}
$info = @{source=$source;target=$target;source_length=$source.Length;target_length=$target.Length;cwd=$work}
[IO.File]::WriteAllText((Join-Path $work 'long-path-result.json'),($info | ConvertTo-Json),(New-Object Text.UTF8Encoding($false)))
Write-Output 'Long-path file deployed and copied for Agent collection.'
