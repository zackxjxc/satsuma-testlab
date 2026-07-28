[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDirectory,

    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [Parameter(Mandatory = $true)]
    [ValidatePattern("^[0-9A-Za-z][0-9A-Za-z.-]*$")]
    [string]$Version
)

Write-Host "AI 编写的发布脚本：生成支持 UTF-8 中文文件名的 ZIP。"

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

# 所有清理目标都固定在构建目录内，避免参数错误扩大删除范围。
function Assert-ChildPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Parent,

        [Parameter(Mandatory = $true)]
        [string]$Child
    )

    $parentPrefix = [IO.Path]::GetFullPath($Parent).TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    $childPath = [IO.Path]::GetFullPath($Child)
    if (-not $childPath.StartsWith($parentPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Package path escapes the build directory: $childPath"
    }
}

$buildPath = [IO.Path]::GetFullPath($BuildDirectory)
if (-not (Test-Path -LiteralPath $buildPath -PathType Container)) {
    throw "Build directory does not exist: $buildPath"
}

$packageName = "SatsumaTestLab-$Version-windows-x64"
$packageDirectory = Join-Path $buildPath "packages"
$stagingDirectory = Join-Path (Join-Path $buildPath "_package") $packageName
$packagePath = Join-Path $packageDirectory "$packageName.zip"
Assert-ChildPath -Parent $buildPath -Child $stagingDirectory
Assert-ChildPath -Parent $buildPath -Child $packagePath

if (Test-Path -LiteralPath $stagingDirectory) {
    Remove-Item -LiteralPath $stagingDirectory -Recurse -Force
}
New-Item -ItemType Directory -Path $stagingDirectory -Force | Out-Null
New-Item -ItemType Directory -Path $packageDirectory -Force | Out-Null

try {
    $cmake = Get-Command cmake -ErrorAction Stop
    & $cmake.Source --install $buildPath --config $Configuration --prefix $stagingDirectory
    if ($LASTEXITCODE -ne 0) {
        throw "CMake install failed with exit code $LASTEXITCODE"
    }

    if (Test-Path -LiteralPath $packagePath) {
        Remove-Item -LiteralPath $packagePath -Force
    }
    Compress-Archive `
        -LiteralPath $stagingDirectory `
        -DestinationPath $packagePath `
        -CompressionLevel Optimal

    # 重新读取 ZIP，确认发布工具能按 Unicode 名称识别所有中文文档。
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($packagePath)
    try {
        $entryNames = [Collections.Generic.HashSet[string]]::new(
            [StringComparer]::Ordinal)
        foreach ($entry in $archive.Entries) {
            [void]$entryNames.Add($entry.FullName)
        }
        $expectedDocuments = @(
            "$packageName/更新日志.md",
            "$packageName/贡献指南.md",
            "$packageName/安全策略.md",
            "$packageName/第三方声明.md",
            "$packageName/docs/AI操作契约.md",
            "$packageName/docs/架构.md",
            "$packageName/docs/开发指南.md",
            "$packageName/docs/协议.md",
            "$packageName/docs/用户指南.md"
        )
        foreach ($document in $expectedDocuments) {
            if (-not $entryNames.Contains($document)) {
                throw "Release archive is missing a Unicode document entry: $document"
            }
        }
    } finally {
        $archive.Dispose()
    }
} finally {
    if (Test-Path -LiteralPath $stagingDirectory) {
        Remove-Item -LiteralPath $stagingDirectory -Recurse -Force
    }
}

Write-Host "Release package: $packagePath"
