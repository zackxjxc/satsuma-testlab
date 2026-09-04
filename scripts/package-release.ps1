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

# AI-maintained release packaging: preserve the single-file Guest delivery contract.

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

# Compute a SHA-256 digest without relying on PowerShell module auto-loading.
function Get-Sha256Hex {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $stream = [IO.File]::OpenRead([IO.Path]::GetFullPath($Path))
    try {
        $algorithm = [Security.Cryptography.SHA256]::Create()
        try {
            $digest = $algorithm.ComputeHash($stream)
            return [BitConverter]::ToString($digest).Replace("-", "").ToLowerInvariant()
        } finally {
            $algorithm.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
}

function Get-BinaryIdentity {
    param([string]$Path, [string]$Component)

    $versionOutput = & $Path --version --json
    if ($LASTEXITCODE -ne 0) {
        throw "Cannot read build identity from ${Path}: exit code $LASTEXITCODE"
    }
    $identity = ($versionOutput -join "`n") | ConvertFrom-Json
    if ($identity.component -cne $Component -or $identity.version -cne $Version) {
        throw "Executable identity does not match this release: $Path"
    }
    if ($identity.git_commit -cnotmatch '^[0-9a-f]{40}(-dirty)?$' -or
        $identity.build_number -cnotmatch '^[0-9A-Za-z._-]+$' -or
        $identity.build_attempt -cnotmatch '^[0-9A-Za-z._-]+$') {
        throw "Executable has missing or invalid build provenance; reconfigure and rebuild: $Path"
    }
    return $identity
}

function Write-ReleaseIdentity {
    param([string]$Directory)

    $guestRelativePath = 'SatsumaGuestAgent-Install/SatsumaVM.exe'
    $hostRelativePath = 'SatsumaHost/SatsumaHost.exe'
    $guestPath = Join-Path $Directory $guestRelativePath
    $hostPath = Join-Path $Directory $hostRelativePath
    $guestIdentity = Get-BinaryIdentity -Path $guestPath -Component 'SatsumaVM'
    $hostIdentity = Get-BinaryIdentity -Path $hostPath -Component 'SatsumaHost'
    foreach ($field in @('version', 'build_number', 'build_attempt', 'git_commit')) {
        if ($guestIdentity.$field -cne $hostIdentity.$field) {
            throw "Host and Guest came from different builds ($field); rebuild both before packaging"
        }
    }
    $buildId = "$Version-$($guestIdentity.build_number).$($guestIdentity.build_attempt)-g$($guestIdentity.git_commit)"
    $guestHash = Get-Sha256Hex -Path $guestPath
    $hostHash = Get-Sha256Hex -Path $hostPath
    $sourceDirty = $guestIdentity.git_commit.EndsWith('-dirty', [StringComparison]::Ordinal)
    $sourceState = if ($sourceDirty) { '包含未提交改动（dirty），不是该提交的原样构建。' } else { '构建时工作区干净。' }
    $buildInfo = @"
# Satsuma TestLab $Version 构建信息

- 构建 ID：$buildId
- 构建号：$($guestIdentity.build_number)，尝试号：$($guestIdentity.build_attempt)
- Git 提交：$($guestIdentity.git_commit)
- 源码状态：$sourceState
- Guest SHA-256：$guestHash
- Host SHA-256：$hostHash

相同版本号可能对应不同构建。使用同一完整发行包中的 Host 与 Guest；身份检查出现哈希不匹配时，先核对本文件和 release-manifest.json，不要直接把 Guest 自报哈希写入预期配置。

release-manifest.json 记录本包所有文件（清单自身除外）的相对路径、大小及 SHA-256。构建 ID 用于定位源码；SHA-256 才能确认具体文件内容。重新编译相同源码也可能产生不同 EXE，应继续核对哈希。

双击升级仍只按版本优先级和 EXE 哈希判断；构建号不参与版本高低比较。Guest 只需复制 SatsumaVM.exe，无需复制本文件或清单。
"@
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [IO.File]::WriteAllText((Join-Path $Directory 'BUILD-INFO.md'), $buildInfo.Replace("`r`n", "`n") + "`n", $utf8NoBom)
    $fileRecords = @(foreach ($file in Get-ChildItem -LiteralPath $Directory -File -Recurse | Sort-Object FullName) {
        $relativePath = $file.FullName.Substring($Directory.Length).TrimStart([IO.Path]::DirectorySeparatorChar).Replace('\', '/')
        [ordered]@{
            path = $relativePath
            size_bytes = $file.Length
            sha256 = Get-Sha256Hex -Path $file.FullName
        }
    })
    $manifest = [ordered]@{
        schema_version = 1
        product = 'Satsuma TestLab'
        version = $Version
        build_id = $buildId
        build_number = $guestIdentity.build_number
        build_attempt = $guestIdentity.build_attempt
        git_commit = $guestIdentity.git_commit
        working_tree_dirty = $sourceDirty
        configuration = $Configuration
        packaged_utc = [DateTime]::UtcNow.ToString('o')
        binaries = @(
            [ordered]@{ path = $hostRelativePath; component = 'SatsumaHost'; sha256 = $hostHash },
            [ordered]@{ path = $guestRelativePath; component = 'SatsumaVM'; sha256 = $guestHash }
        )
        excluded_from_file_list = @('release-manifest.json')
        files = $fileRecords
    }
    $manifestJson = $manifest | ConvertTo-Json -Depth 8
    [IO.File]::WriteAllText((Join-Path $Directory 'release-manifest.json'), $manifestJson.Replace("`r`n", "`n") + "`n", $utf8NoBom)
    Write-Host "Release build: $buildId"
    Write-Host "Guest SHA-256: $guestHash"
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
$checksumPath = Join-Path $outputPath "$packageName.zip.sha256"
Assert-ChildPath -Parent $outputPath -Child $preparingDirectory
Assert-ChildPath -Parent $outputPath -Child $releaseDirectory
Assert-ChildPath -Parent $outputPath -Child $partialPackagePath
Assert-ChildPath -Parent $outputPath -Child $packagePath
Assert-ChildPath -Parent $outputPath -Child $checksumPath

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
    Write-ReleaseIdentity -Directory $preparingDirectory

    if (Test-Path -LiteralPath $releaseDirectory) {
        Remove-Item -LiteralPath $releaseDirectory -Recurse -Force
    }
    Move-Item -LiteralPath $preparingDirectory -Destination $releaseDirectory

    if (Test-Path -LiteralPath $partialPackagePath) {
        Remove-Item -LiteralPath $partialPackagePath -Force
    }
    # Compress-Archive 在 Windows PowerShell 5.1 不会保留 LiteralPath 的根目录，
    # 会产生与发行包布局契约不一致的 ZIP。逐文件写入 .NET ZIP，以固定根目录。
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $packageStream = [IO.File]::Open(
        $partialPackagePath,
        [IO.FileMode]::CreateNew,
        [IO.FileAccess]::Write)
    try {
        $packageArchive = New-Object IO.Compression.ZipArchive(
            $packageStream,
            [IO.Compression.ZipArchiveMode]::Create,
            $false)
        try {
            foreach ($packageFile in Get-ChildItem -LiteralPath $releaseDirectory -File -Recurse) {
                $relativePath = $packageFile.FullName.Substring($releaseDirectory.Length)
                if ($relativePath.StartsWith([IO.Path]::DirectorySeparatorChar)) {
                    $relativePath = $relativePath.Substring(1)
                }
                $entryName = "$packageName/$($relativePath.Replace('\', '/'))"
                [IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                    $packageArchive,
                    $packageFile.FullName,
                    $entryName,
                    [IO.Compression.CompressionLevel]::Optimal) | Out-Null
            }
        } finally {
            $packageArchive.Dispose()
        }
    } finally {
        $packageStream.Dispose()
    }

    # 重新读取 ZIP，确认发布工具能按 Unicode 名称识别所有中文文档。
    $archive = [IO.Compression.ZipFile]::OpenRead($partialPackagePath)
    try {
        $entryNames = [Collections.Generic.HashSet[string]]::new(
            [StringComparer]::Ordinal)
        foreach ($entry in $archive.Entries) {
            [void]$entryNames.Add($entry.FullName)
        }
        $expectedDocuments = @(
            "$packageName/README.md",
            "$packageName/BUILD-INFO.md",
            "$packageName/release-manifest.json",
            "$packageName/AI-START-HERE.md",
            "$packageName/CHANGELOG.md",
            "$packageName/CODE_OF_CONDUCT.md",
            "$packageName/CONTRIBUTING.md",
            "$packageName/LICENSE",
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
        $expectedGuestInstaller = @(
            "$packageName/SatsumaGuestAgent-Install/SatsumaVM.exe"
        )
        foreach ($installerFile in $expectedGuestInstaller) {
            if (-not $entryNames.Contains($installerFile)) {
                throw "Release archive is missing a Guest installer file: $installerFile"
            }
        }
        foreach ($entryName in $entryNames) {
            if ($entryName -match '(^|/)install-agent\.ps1$' -or
                ($entryName.StartsWith("$packageName/SatsumaGuestAgent-Install/") -and
                 -not $entryName.EndsWith('/') -and
                 $entryName -ne "$packageName/SatsumaGuestAgent-Install/SatsumaVM.exe")) {
                throw "Guest delivery must contain only SatsumaVM.exe: $entryName"
            }
        }
        if (-not $entryNames.Contains("$packageName/SatsumaHost/SatsumaHost.exe")) {
            throw "Release archive is missing SatsumaHost/SatsumaHost.exe"
        }
    } finally {
        $archive.Dispose()
    }

    if (Test-Path -LiteralPath $packagePath) {
        Remove-Item -LiteralPath $packagePath -Force
    }
    Move-Item -LiteralPath $partialPackagePath -Destination $packagePath
    $packageHash = Get-Sha256Hex -Path $packagePath
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [IO.File]::WriteAllText(
        $checksumPath,
        "$packageHash  $packageName.zip`n",
        $utf8NoBom)
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
Write-Host "Release checksum: $checksumPath"
