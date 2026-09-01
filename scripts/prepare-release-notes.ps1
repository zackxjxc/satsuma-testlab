# Generated with AI assistance: validates a release tag and extracts its changelog section.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern("^v[0-9]+\.[0-9]+\.[0-9]+$")]
    [string]$Tag,

    [Parameter(Mandatory = $true)]
    [string]$ProjectFile,

    [Parameter(Mandatory = $true)]
    [string]$ChangeLog,

    [Parameter(Mandatory = $true)]
    [string]$OutputFile
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$version = $Tag.Substring(1)
$projectText = [IO.File]::ReadAllText([IO.Path]::GetFullPath($ProjectFile))
$projectMatch = [regex]::Match(
    $projectText,
    "project\(SatsumaTestLab\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)\s+LANGUAGES")
if (-not $projectMatch.Success) {
    throw "Cannot read the Satsuma project version from $ProjectFile"
}
if ($projectMatch.Groups[1].Value -cne $version) {
    throw "Release tag $Tag does not match project version $($projectMatch.Groups[1].Value)"
}

$changeLogText = [IO.File]::ReadAllText([IO.Path]::GetFullPath($ChangeLog))
$escapedVersion = [regex]::Escape($version)
$sectionMatch = [regex]::Match(
    $changeLogText,
    "(?ms)^## $escapedVersion - [0-9]{4}-[0-9]{2}-[0-9]{2}\s*.*?(?=^## |\z)")
if (-not $sectionMatch.Success) {
    throw "Cannot find a changelog section for version $version"
}

$outputPath = [IO.Path]::GetFullPath($OutputFile)
$outputParent = [IO.Path]::GetDirectoryName($outputPath)
if (-not [string]::IsNullOrEmpty($outputParent)) {
    [IO.Directory]::CreateDirectory($outputParent) | Out-Null
}
$utf8NoBom = [Text.UTF8Encoding]::new($false)
[IO.File]::WriteAllText($outputPath, $sectionMatch.Value.Trim() + "`n", $utf8NoBom)
