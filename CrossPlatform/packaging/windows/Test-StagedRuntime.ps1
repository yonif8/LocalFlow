[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $StageRoot,

    [Parameter(Mandatory = $true)]
    [string] $NemoRoot,

    [Parameter(Mandatory = $true)]
    [string] $LlamaRoot
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Resolve-RequiredDirectory([string] $Path, [string] $Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "$Label does not exist: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Assert-IdenticalFile([string] $Source, [string] $Destination) {
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "Locked runtime source is missing: $Source"
    }
    if (-not (Test-Path -LiteralPath $Destination -PathType Leaf)) {
        throw "Staged runtime file is missing: $Destination"
    }
    $sourceFile = Get-Item -LiteralPath $Source
    $destinationFile = Get-Item -LiteralPath $Destination
    if ($sourceFile.Length -ne $destinationFile.Length) {
        throw "Staged runtime size differs from the locked source: $Destination"
    }
    $sourceHash = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
    $destinationHash = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash
    if ($sourceHash -cne $destinationHash) {
        throw "Staged runtime bytes differ from the locked source: $Destination"
    }
}

$StageRoot = Resolve-RequiredDirectory $StageRoot "Stage root"
$NemoRoot = Resolve-RequiredDirectory $NemoRoot "NeMo root"
$LlamaRoot = Resolve-RequiredDirectory $LlamaRoot "llama root"

foreach ($root in @($NemoRoot, $LlamaRoot)) {
    if (-not (Test-Path -LiteralPath (Join-Path $root ".localflow-runtime-lock") -PathType Leaf)) {
        throw "Runtime root was not created by the checksum-pinned bootstrap: $root"
    }
}

$manifestPath = Join-Path $PSScriptRoot "..\..\dependencies\runtime-lock.json"
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$desktop = $manifest.runtimeIsolation.desktopProcess
$polish = $manifest.runtimeIsolation.polishWorkerProcess
$desktopDestination = Join-Path $StageRoot ($desktop.windowsDestination -replace '/', '\')
$polishDestination = Join-Path $StageRoot ($polish.windowsDestination -replace '/', '\')

foreach ($name in @($desktop.windowsFiles)) {
    Assert-IdenticalFile `
        (Join-Path (Join-Path $NemoRoot "bin") $name) `
        (Join-Path $desktopDestination $name)
}
$polishFiles = @($polish.windowsCommonFiles) + @($polish.windowsX86_64CpuFiles)
foreach ($name in $polishFiles) {
    Assert-IdenticalFile `
        (Join-Path (Join-Path $LlamaRoot "bin") $name) `
        (Join-Path $polishDestination $name)
}

$forbiddenDesktopNames = @(
    "llama.dll",
    "nemo_speech_nmt.dll",
    "nemo_speech_nmt_c.dll",
    "nemo_speech_tts.dll",
    "nemo_speech_tts_c.dll"
)
foreach ($name in $forbiddenDesktopNames) {
    if (Test-Path -LiteralPath (Join-Path $desktopDestination $name)) {
        throw "An unrelated inference runtime leaked into the desktop process: $name"
    }
}
$polishFileNames = @{}
foreach ($name in $polishFiles) { $polishFileNames[$name.ToLowerInvariant()] = $true }
foreach ($name in @($desktop.windowsFiles)) {
    if ($polishFileNames.ContainsKey($name.ToLowerInvariant())) { continue }
    if (Test-Path -LiteralPath (Join-Path $polishDestination $name)) {
        throw "The NeMo runtime leaked into the isolated polish process: $name"
    }
}

Write-Host "Staged Windows inference runtimes exactly match the checksum-pinned roots."
