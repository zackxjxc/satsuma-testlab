[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDirectory,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,

    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [Parameter(Mandatory = $true)]
    [ValidatePattern("^[0-9A-Za-z][0-9A-Za-z.-]*$")]
    [string]$Version
)

Write-Host "AI 编写的发布脚本：生成便携目录和支持 UTF-8 中文文件名的 ZIP。"

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

# 所有清理目标都固定在发布根目录内，避免参数错误扩大删除范围。
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
        throw "Release path escapes the output directory: $childPath"
    }
}

$buildPath = [IO.Path]::GetFullPath($BuildDirectory)
if (-not (Test-Path -LiteralPath $buildPath -PathType Container)) {
    throw "Build directory does not exist: $buildPath"
}
$outputPath = [IO.Path]::GetFullPath($OutputDirectory)
if ([IO.Path]::GetPathRoot($outputPath) -eq $outputPath) {
    throw "Output directory cannot be a filesystem root: $outputPath"
}

$packageName = "SatsumaTestLab-$Version-windows-x64"
$preparingDirectory = Join-Path $outputPath ".$packageName.preparing"
$releaseDirectory = Join-Path $outputPath $packageName
$partialPackagePath = Join-Path $outputPath "$packageName.partial.zip"
$packagePath = Join-Path $outputPath "$packageName.zip"
Assert-ChildPath -Parent $outputPath -Child $preparingDirectory
Assert-ChildPath -Parent $outputPath -Child $releaseDirectory
Assert-ChildPath -Parent $outputPath -Child $partialPackagePath
Assert-ChildPath -Parent $outputPath -Child $packagePath

New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
if (Test-Path -LiteralPath $preparingDirectory) {
    Remove-Item -LiteralPath $preparingDirectory -Recurse -Force
}
New-Item -ItemType Directory -Path $preparingDirectory -Force | Out-Null

try {
    $cmake = Get-Command cmake -ErrorAction Stop
    & $cmake.Source --install $buildPath --config $Configuration --prefix $preparingDirectory --component SatsumaRuntime
    if ($LASTEXITCODE -ne 0) {
        throw "CMake install failed with exit code $LASTEXITCODE"
    }

    if (Test-Path -LiteralPath $releaseDirectory) {
        Remove-Item -LiteralPath $releaseDirectory -Recurse -Force
    }
    Move-Item -LiteralPath $preparingDirectory -Destination $releaseDirectory

    if (Test-Path -LiteralPath $partialPackagePath) {
        Remove-Item -LiteralPath $partialPackagePath -Force
    }
    Compress-Archive `
        -LiteralPath $releaseDirectory `
        -DestinationPath $partialPackagePath `
        -CompressionLevel Optimal

    # 重新读取 ZIP，确认发布工具能按 Unicode 名称识别所有中文文档。
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($partialPackagePath)
    try {
        $entryNames = [Collections.Generic.HashSet[string]]::new(
            [StringComparer]::Ordinal)
        foreach ($entry in $archive.Entries) {
            [void]$entryNames.Add($entry.FullName)
        }
        $expectedDocuments = @(
            "$packageName/README.md",
            "$packageName/AI-START-HERE.md",
            "$packageName/CHANGELOG.md",
            "$packageName/CODE_OF_CONDUCT.md",
            "$packageName/CONTRIBUTING.md",
            "$packageName/SECURITY.md",
            "$packageName/THIRD_PARTY_NOTICES.md",
            "$packageName/docs/AI操作契约.md",
            "$packageName/docs/首次配置.md",
            "$packageName/docs/架构.md",
            "$packageName/docs/开发指南.md",
            "$packageName/docs/协议.md",
            "$packageName/docs/用户指南.md",
            "$packageName/skills/satsuma-testlab/SKILL.md",
            "$packageName/skills/satsuma-testlab/references/setup.md",
            "$packageName/skills/satsuma-testlab/references/task-authoring.md",
            "$packageName/skills/satsuma-testlab/references/operations.md"
        )
        foreach ($document in $expectedDocuments) {
            if (-not $entryNames.Contains($document)) {
                throw "Release archive is missing a required document entry: $document"
            }
        }
        $expectedTemplates = @(
            "$packageName/config/lab.template.json",
            "$packageName/config/agent.template.json"
        )
        foreach ($template in $expectedTemplates) {
            if (-not $entryNames.Contains($template)) {
                throw "Release archive is missing a configuration template: $template"
            }
        }
    } finally {
        $archive.Dispose()
    }

    if (Test-Path -LiteralPath $packagePath) {
        Remove-Item -LiteralPath $packagePath -Force
    }
    Move-Item -LiteralPath $partialPackagePath -Destination $packagePath
} catch {
    if (Test-Path -LiteralPath $preparingDirectory) {
        Remove-Item -LiteralPath $preparingDirectory -Recurse -Force
    }
    if (Test-Path -LiteralPath $partialPackagePath) {
        Remove-Item -LiteralPath $partialPackagePath -Force
    }
    throw
}

Write-Host "Release directory: $releaseDirectory"
Write-Host "Release package: $packagePath"
