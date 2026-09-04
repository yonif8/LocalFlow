[CmdletBinding()]
param(
    [Parameter()]
    [string] $Prefix = (Join-Path $PSScriptRoot ".runtime"),

    [Parameter()]
    [string] $Architecture = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$NemoVersion = "0.1.0"
$LlamaVersion = "b10794"
$LlamaCommit = "f9f09f02cc44d87d842dbd2d578857d92d4bb63b"

$NemoFilename = "nemo-speech-0.1.0-windows-x86_64-cpu.zip"
$NemoUrl = "https://github.com/NVIDIA/NeMo-Speech.cpp/releases/download/v0.1.0/$NemoFilename"
$NemoSize = 4730421L
$NemoSha256 = "5e4ea81046012edcd77fd8848de8eefb5a4ba38cc26f52eb544ab184695a75d6"

$LlamaFilename = "llama-b10794-bin-win-cpu-x64.zip"
$LlamaUrl = "https://github.com/ggml-org/llama.cpp/releases/download/b10794/$LlamaFilename"
$LlamaSize = 18389889L
$LlamaSha256 = "94d950efccc261b345a7e55640c746aaed125a761beeb15ead075d768d8cfd38"

$LlamaSourceFilename = "llama.cpp-$LlamaCommit.tar.gz"
$LlamaSourceUrl = "https://codeload.github.com/ggml-org/llama.cpp/tar.gz/$LlamaCommit"
$LlamaSourceSize = 37295615L
$LlamaSourceSha256 = "50b96e851f70552ae3bb7bd5192107a052e6148ccc45a180db08fc1fe7b5bc4f"

function Write-Log {
    param([Parameter(Mandatory)][string] $Message)
    [Console]::Error.WriteLine("[localflow-deps] $Message")
}

function Get-NormalizedArchitecture {
    param([string] $Requested)

    if ([string]::IsNullOrWhiteSpace($Requested)) {
        try {
            $Requested = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
        }
        catch {
            $Requested = if ($env:PROCESSOR_ARCHITEW6432) {
                $env:PROCESSOR_ARCHITEW6432
            }
            else {
                $env:PROCESSOR_ARCHITECTURE
            }
        }
    }

    switch ($Requested.Trim().ToLowerInvariant()) {
        { $_ -in @("x86_64", "x64", "amd64") } { return "x86_64" }
        { $_ -in @("aarch64", "arm64") } {
            throw "Windows ARM64 is intentionally unsupported: NeMo-Speech.cpp v0.1.0 did not publish a Windows ARM64 CPU SDK. No fallback version will be used."
        }
        default {
            throw "Unsupported Windows architecture '$Requested'; the locked runtime set supports x86_64/AMD64 only."
        }
    }
}

function Test-LockedFile {
    param(
        [Parameter(Mandatory)][string] $Path,
        [Parameter(Mandatory)][long] $ExpectedSize,
        [Parameter(Mandatory)][string] $ExpectedSha256
    )

    if (-not [IO.File]::Exists($Path)) {
        return $false
    }
    if ((Get-Item -LiteralPath $Path).Length -ne $ExpectedSize) {
        return $false
    }
    $actualHash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    return $actualHash -eq $ExpectedSha256
}

function Get-LockedDownload {
    param(
        [Parameter(Mandatory)][string] $Filename,
        [Parameter(Mandatory)][string] $Url,
        [Parameter(Mandatory)][long] $ExpectedSize,
        [Parameter(Mandatory)][string] $ExpectedSha256,
        [Parameter(Mandatory)][string] $DownloadRoot
    )

    $destination = Join-Path $DownloadRoot $Filename
    if (Test-LockedFile -Path $destination -ExpectedSize $ExpectedSize -ExpectedSha256 $ExpectedSha256) {
        Write-Log "using verified cache: $Filename"
        return $destination
    }

    if (Test-Path -LiteralPath $destination) {
        Write-Log "discarding cache entry that does not match the lock: $Filename"
        Remove-Item -LiteralPath $destination -Force
    }

    $temporary = Join-Path $DownloadRoot (".{0}.download.{1}" -f $Filename, [Guid]::NewGuid().ToString("N"))
    Write-Log "downloading locked asset: $Filename"
    try {
        Invoke-WebRequest -Uri $Url -OutFile $temporary -UseBasicParsing
        if (-not (Test-LockedFile -Path $temporary -ExpectedSize $ExpectedSize -ExpectedSha256 $ExpectedSha256)) {
            $actualSize = if ([IO.File]::Exists($temporary)) { (Get-Item -LiteralPath $temporary).Length } else { 0 }
            $actualHash = if ([IO.File]::Exists($temporary)) {
                (Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash.ToLowerInvariant()
            }
            else {
                "missing"
            }
            throw "Integrity check failed for $Filename (size $actualSize, sha256 $actualHash); expected size $ExpectedSize, sha256 $ExpectedSha256."
        }
        Move-Item -LiteralPath $temporary -Destination $destination
    }
    finally {
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Force
        }
    }
    return $destination
}

function Assert-SafeArchivePath {
    param([Parameter(Mandatory)][string] $Path)

    $normalized = $Path.Replace('\', '/')
    if ($normalized.StartsWith('/') -or $normalized -match '^[A-Za-z]:') {
        throw "Archive contains an absolute path: $Path"
    }
    foreach ($component in $normalized.Split('/')) {
        if ($component -eq '..') {
            throw "Archive contains a parent traversal: $Path"
        }
    }
}

function Assert-SafeZip {
    param([Parameter(Mandatory)][string] $Archive)

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($Archive)
    try {
        foreach ($entry in $zip.Entries) {
            Assert-SafeArchivePath -Path $entry.FullName
        }
    }
    finally {
        $zip.Dispose()
    }
}

function Assert-SafeTar {
    param([Parameter(Mandatory)][string] $Archive)

    $entries = & tar.exe -tzf $Archive
    if ($LASTEXITCODE -ne 0) {
        throw "Cannot read archive: $Archive"
    }
    foreach ($entry in $entries) {
        Assert-SafeArchivePath -Path $entry
    }
}

function Test-ExactMarker {
    param(
        [Parameter(Mandatory)][string] $Root,
        [Parameter(Mandatory)][string] $Expected
    )

    $marker = Join-Path $Root ".localflow-runtime-lock"
    if (-not [IO.File]::Exists($marker)) {
        return $false
    }
    $actual = [IO.File]::ReadAllText($marker).TrimEnd([char[]]"`r`n")
    return $actual -eq $Expected
}

function Test-NemoRoot {
    param(
        [Parameter(Mandatory)][string] $Root,
        [Parameter(Mandatory)][string] $Marker
    )

    if (-not (Test-ExactMarker -Root $Root -Expected $Marker)) {
        return $false
    }
    foreach ($relativePath in @(
        "include\nemo_speech\asr.h",
        "bin\nemo_speech_asr_c.dll",
        "bin\nemo_speech_asr.dll",
        "lib\nemo_speech_asr_c.lib",
        "share\licenses\nemo-speech\LICENSE",
        "share\licenses\nemo-speech\THIRD_PARTY_NOTICES.md"
    )) {
        if (-not [IO.File]::Exists((Join-Path $Root $relativePath))) {
            return $false
        }
    }
    return $true
}

function Test-LlamaRoot {
    param(
        [Parameter(Mandatory)][string] $Root,
        [Parameter(Mandatory)][string] $Marker,
        [Parameter(Mandatory)][string[]] $CpuLibraries
    )

    if (-not (Test-ExactMarker -Root $Root -Expected $Marker)) { return $false }
    foreach ($relativePath in @(
        "include\llama.h",
        "include\ggml.h",
        "bin\llama.dll",
        "bin\ggml.dll",
        "bin\ggml-base.dll",
        "bin\libomp.dll",
        "share\licenses\llama.cpp\LICENSE",
        "share\licenses\llama.cpp\LICENSE-LLVM-OpenMP"
    )) {
        if (-not [IO.File]::Exists((Join-Path $Root $relativePath))) { return $false }
    }
    foreach ($library in $CpuLibraries) {
        if (-not [IO.File]::Exists((Join-Path (Join-Path $Root "bin") $library))) { return $false }
    }
    return $true
}

if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
    throw "This bootstrap supports Windows only."
}

# GitHub requires TLS 1.2. Preserve any protocols the host explicitly enabled.
[Net.ServicePointManager]::SecurityProtocol =
    [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$manifestPath = Join-Path $PSScriptRoot "runtime-lock.json"
if (-not [IO.File]::Exists($manifestPath)) {
    throw "Missing lock manifest: $manifestPath"
}

$Arch = Get-NormalizedArchitecture -Requested $Architecture
if (-not [IO.Path]::IsPathRooted($Prefix)) {
    throw "-Prefix must be an absolute path."
}
$Prefix = [IO.Path]::GetFullPath($Prefix)
$PlatformRoot = Join-Path $Prefix "windows-$Arch"
$DownloadRoot = Join-Path $Prefix "downloads"
$NemoRoot = Join-Path $PlatformRoot "nemo-speech-$NemoVersion-cpu"
$LlamaRoot = Join-Path $PlatformRoot "llama-$LlamaVersion-cpu"

$LlamaCpuLibraries = @(
    "ggml-cpu-alderlake.dll",
    "ggml-cpu-cannonlake.dll",
    "ggml-cpu-cascadelake.dll",
    "ggml-cpu-cooperlake.dll",
    "ggml-cpu-haswell.dll",
    "ggml-cpu-icelake.dll",
    "ggml-cpu-ivybridge.dll",
    "ggml-cpu-piledriver.dll",
    "ggml-cpu-sandybridge.dll",
    "ggml-cpu-sapphirerapids.dll",
    "ggml-cpu-skylakex.dll",
    "ggml-cpu-sse42.dll",
    "ggml-cpu-x64.dll",
    "ggml-cpu-zen4.dll"
)

$NemoMarker = @(
    "component=nemo-speech-cpp",
    "version=$NemoVersion",
    "platform=windows",
    "architecture=$Arch",
    "archive_sha256=$NemoSha256"
) -join "`n"

$LlamaMarker = @(
    "component=llama-cpp",
    "version=$LlamaVersion",
    "commit=$LlamaCommit",
    "platform=windows",
    "architecture=$Arch",
    "runtime_sha256=$LlamaSha256",
    "source_sha256=$LlamaSourceSha256"
) -join "`n"

[IO.Directory]::CreateDirectory($PlatformRoot) | Out-Null
[IO.Directory]::CreateDirectory($DownloadRoot) | Out-Null

$lockPath = Join-Path $PlatformRoot ".bootstrap.lock"
$lockStream = $null
$workRoot = $null
try {
    try {
        $lockStream = [IO.File]::Open($lockPath, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    }
    catch {
        throw "Another bootstrap is using $PlatformRoot. If no bootstrap is running, remove $lockPath and retry."
    }

    $workRoot = Join-Path $PlatformRoot (".bootstrap-work.{0}" -f [Guid]::NewGuid().ToString("N"))
    [IO.Directory]::CreateDirectory($workRoot) | Out-Null

    if ((Test-Path -LiteralPath $NemoRoot) -and -not (Test-NemoRoot -Root $NemoRoot -Marker $NemoMarker)) {
        throw "Refusing to overwrite an incomplete or differently locked root: $NemoRoot"
    }
    if ((Test-Path -LiteralPath $LlamaRoot) -and -not (Test-LlamaRoot -Root $LlamaRoot -Marker $LlamaMarker -CpuLibraries $LlamaCpuLibraries)) {
        throw "Refusing to overwrite an incomplete or differently locked root: $LlamaRoot"
    }

    if (-not (Test-NemoRoot -Root $NemoRoot -Marker $NemoMarker)) {
        $nemoArchive = Get-LockedDownload -Filename $NemoFilename -Url $NemoUrl -ExpectedSize $NemoSize -ExpectedSha256 $NemoSha256 -DownloadRoot $DownloadRoot
        Assert-SafeZip -Archive $nemoArchive

        $nemoStage = Join-Path $workRoot "nemo-stage"
        Expand-Archive -LiteralPath $nemoArchive -DestinationPath $nemoStage
        [IO.File]::WriteAllText((Join-Path $nemoStage ".localflow-runtime-lock"), $NemoMarker, [Text.Encoding]::ASCII)

        foreach ($requiredPath in @(
            "include\nemo_speech\asr.h",
            "bin\nemo_speech_asr_c.dll",
            "lib\nemo_speech_asr_c.lib",
            "share\licenses\nemo-speech\LICENSE"
        )) {
            if (-not [IO.File]::Exists((Join-Path $nemoStage $requiredPath))) {
                throw "Locked NeMo SDK is missing $requiredPath"
            }
        }

        Move-Item -LiteralPath $nemoStage -Destination $NemoRoot
        Write-Log "installed NeMo-Speech.cpp ${NemoVersion}: $NemoRoot"
    }
    else {
        Write-Log "verified existing NeMo-Speech.cpp root: $NemoRoot"
    }

    if (-not (Test-LlamaRoot -Root $LlamaRoot -Marker $LlamaMarker -CpuLibraries $LlamaCpuLibraries)) {
        $llamaArchive = Get-LockedDownload -Filename $LlamaFilename -Url $LlamaUrl -ExpectedSize $LlamaSize -ExpectedSha256 $LlamaSha256 -DownloadRoot $DownloadRoot
        $llamaSourceArchive = Get-LockedDownload -Filename $LlamaSourceFilename -Url $LlamaSourceUrl -ExpectedSize $LlamaSourceSize -ExpectedSha256 $LlamaSourceSha256 -DownloadRoot $DownloadRoot
        Assert-SafeZip -Archive $llamaArchive
        Assert-SafeTar -Archive $llamaSourceArchive

        $llamaRuntimeRaw = Join-Path $workRoot "llama-runtime"
        $llamaSourceRaw = Join-Path $workRoot "llama-source"
        $llamaStage = Join-Path $workRoot "llama-stage"
        Expand-Archive -LiteralPath $llamaArchive -DestinationPath $llamaRuntimeRaw
        [IO.Directory]::CreateDirectory($llamaSourceRaw) | Out-Null
        & tar.exe -xzf $llamaSourceArchive -C $llamaSourceRaw --strip-components=1
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to extract locked llama source archive."
        }

        $includeRoot = Join-Path $llamaStage "include"
        $binRoot = Join-Path $llamaStage "bin"
        $licenseRoot = Join-Path $llamaStage "share\licenses\llama.cpp"
        [IO.Directory]::CreateDirectory($includeRoot) | Out-Null
        [IO.Directory]::CreateDirectory($binRoot) | Out-Null
        [IO.Directory]::CreateDirectory($licenseRoot) | Out-Null

        foreach ($headerRoot in @((Join-Path $llamaSourceRaw "include"), (Join-Path $llamaSourceRaw "ggml\include"))) {
            $headers = @(Get-ChildItem -LiteralPath $headerRoot -Filter "*.h" -File)
            if ($headers.Count -eq 0) {
                throw "Locked llama source contains no public headers in $headerRoot"
            }
            foreach ($header in $headers) {
                Copy-Item -LiteralPath $header.FullName -Destination $includeRoot
            }
        }

        $runtimeLibraries = @("llama.dll", "ggml.dll", "ggml-base.dll", "libomp.dll") + $LlamaCpuLibraries
        foreach ($library in $runtimeLibraries) {
            $sourcePath = Join-Path $llamaRuntimeRaw $library
            if (-not [IO.File]::Exists($sourcePath)) {
                throw "Locked llama runtime is missing $library"
            }
            Copy-Item -LiteralPath $sourcePath -Destination $binRoot
        }

        $sourceLicense = Join-Path $llamaSourceRaw "LICENSE"
        $openMpLicense = Join-Path $llamaRuntimeRaw "LICENSE-LLVM-OpenMP"
        foreach ($license in @($sourceLicense, $openMpLicense)) {
            if (-not [IO.File]::Exists($license)) {
                throw "Locked llama package is missing required license file: $license"
            }
        }
        Copy-Item -LiteralPath $sourceLicense -Destination (Join-Path $licenseRoot "LICENSE")
        Copy-Item -LiteralPath $openMpLicense -Destination (Join-Path $licenseRoot "LICENSE-LLVM-OpenMP")
        [IO.File]::WriteAllText((Join-Path $llamaStage ".localflow-runtime-lock"), $LlamaMarker, [Text.Encoding]::ASCII)

        Move-Item -LiteralPath $llamaStage -Destination $LlamaRoot
        Write-Log "installed llama.cpp ${LlamaVersion}: $LlamaRoot"
    }
    else {
        Write-Log "verified existing llama.cpp root: $LlamaRoot"
    }

    $escapedNemoRoot = $NemoRoot.Replace("'", "''")
    $escapedLlamaRoot = $LlamaRoot.Replace("'", "''")
    $activationPath = Join-Path $PlatformRoot "activate.ps1"
    $activation = @"
# Generated by bootstrap-windows.ps1 from runtime-lock.json.
`$env:NEMO_SPEECH_ROOT = '$escapedNemoRoot'
`$env:LLAMA_ROOT = '$escapedLlamaRoot'
"@
    [IO.File]::WriteAllText($activationPath, $activation, [Text.UTF8Encoding]::new($false))

    $env:NEMO_SPEECH_ROOT = $NemoRoot
    $env:LLAMA_ROOT = $LlamaRoot

    [Console]::WriteLine("NEMO_SPEECH_ROOT=$NemoRoot")
    [Console]::WriteLine("LLAMA_ROOT=$LlamaRoot")
    [Console]::WriteLine("For a later shell: . '$activationPath'")
}
finally {
    if ($workRoot -and (Test-Path -LiteralPath $workRoot)) {
        Remove-Item -LiteralPath $workRoot -Recurse -Force
    }
    if ($lockStream) {
        $lockStream.Dispose()
    }
    if (Test-Path -LiteralPath $lockPath) {
        Remove-Item -LiteralPath $lockPath -Force -ErrorAction SilentlyContinue
    }
}
