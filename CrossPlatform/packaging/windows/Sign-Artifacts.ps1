[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string[]] $Path,

    [switch] $RequireSigning
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Find-SignTool {
    $fromPath = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($fromPath) {
        return $fromPath.Source
    }

    $kits = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
    $candidate = Get-ChildItem -LiteralPath $kits -Filter signtool.exe -Recurse -ErrorAction SilentlyContinue `
        | Where-Object { $_.FullName -match '[\\/]x64[\\/]signtool\.exe$' } `
        | Sort-Object FullName -Descending `
        | Select-Object -First 1
    if (-not $candidate) {
        throw "signtool.exe was not found in the Windows SDK."
    }
    return $candidate.FullName
}

function Normalize-Fingerprint([string] $Value) {
    return ($Value -replace '[^0-9A-Fa-f]', '').ToUpperInvariant()
}

$pfxBase64 = $env:WINDOWS_CODESIGN_PFX_BASE64
$pfxPassword = $env:WINDOWS_CODESIGN_PFX_PASSWORD
$expectedFingerprint = $env:WINDOWS_CODESIGN_CERT_SHA256
$timestampUrl = $env:WINDOWS_CODESIGN_TIMESTAMP_URL
$configured = @(
    @($pfxBase64, $pfxPassword, $expectedFingerprint, $timestampUrl) `
        | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
)

if ($configured.Count -eq 0 -and -not $RequireSigning) {
    Write-Warning "Code-signing secrets are absent; producing an unsigned CI smoke artifact."
    return
}
if ($configured.Count -ne 4) {
    throw "Windows signing is partially configured. Set all four WINDOWS_CODESIGN_* secrets."
}

$resolvedPaths = @()
foreach ($item in $Path) {
    if (-not (Test-Path -LiteralPath $item -PathType Leaf)) {
        throw "Signing input does not exist: $item"
    }
    $extension = [IO.Path]::GetExtension($item).ToLowerInvariant()
    if ($extension -notin @(".exe", ".dll")) {
        throw "Only PE executables and libraries may be signed: $item"
    }
    $resolvedPaths += (Resolve-Path -LiteralPath $item).Path
}
if ($resolvedPaths.Count -eq 0) {
    throw "No signing inputs were supplied."
}

$uri = $null
if (-not [Uri]::TryCreate($timestampUrl, [UriKind]::Absolute, [ref] $uri) `
    -or $uri.Scheme -notin @("http", "https")) {
    throw "WINDOWS_CODESIGN_TIMESTAMP_URL must be an absolute HTTP(S) RFC 3161 URL."
}

$temporaryPfx = Join-Path $env:RUNNER_TEMP "localflow-codesign-$PID.pfx"
$pfxBytes = $null
try {
    $pfxBytes = [Convert]::FromBase64String($pfxBase64)
    [IO.File]::WriteAllBytes($temporaryPfx, $pfxBytes)
    $securePassword = ConvertTo-SecureString $pfxPassword -AsPlainText -Force
    $pfx = Get-PfxData -FilePath $temporaryPfx -Password $securePassword
    $expected = Normalize-Fingerprint $expectedFingerprint
    if ($expected.Length -ne 64) {
        throw "WINDOWS_CODESIGN_CERT_SHA256 must contain exactly 64 hexadecimal characters."
    }
    $certificate = $pfx.EndEntityCertificates `
        | Where-Object {
            $actualFingerprint = $_.GetCertHashString(
                [System.Security.Cryptography.HashAlgorithmName]::SHA256)
            (Normalize-Fingerprint $actualFingerprint) -eq $expected
        } `
        | Select-Object -First 1
    if (-not $certificate) {
        throw "The PFX leaf certificate does not match WINDOWS_CODESIGN_CERT_SHA256."
    }
    if ($certificate.NotBefore.ToUniversalTime() -gt [DateTime]::UtcNow `
        -or $certificate.NotAfter.ToUniversalTime() -lt [DateTime]::UtcNow) {
        throw "The configured Windows code-signing certificate is not currently valid."
    }
    $codeSigningOid = "1.3.6.1.5.5.7.3.3"
    $enhancedKeyUsages = @(
        $certificate.EnhancedKeyUsageList | ForEach-Object { $_.ObjectId.Value }
    )
    if ($codeSigningOid -notin $enhancedKeyUsages) {
        throw "The configured certificate is not valid for code signing."
    }

    $signTool = Find-SignTool
    foreach ($file in $resolvedPaths) {
        & $signTool sign /fd SHA256 /td SHA256 /tr $timestampUrl `
            /f $temporaryPfx /p $pfxPassword $file
        if ($LASTEXITCODE -ne 0) {
            throw "signtool failed while signing $([IO.Path]::GetFileName($file))."
        }
        & $signTool verify /pa /all $file
        if ($LASTEXITCODE -ne 0) {
            throw "Authenticode verification failed for $([IO.Path]::GetFileName($file))."
        }
        $signature = Get-AuthenticodeSignature -LiteralPath $file
        $actualSignerFingerprint = if ($signature.SignerCertificate) {
            $signature.SignerCertificate.GetCertHashString(
                [System.Security.Cryptography.HashAlgorithmName]::SHA256)
        } else {
            ""
        }
        if ($signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid `
            -or (Normalize-Fingerprint $actualSignerFingerprint) -ne $expected `
            -or -not $signature.TimeStamperCertificate) {
            throw "The final signature, signer, or RFC 3161 timestamp is invalid."
        }
    }
} finally {
    if ($pfxBytes) {
        [Array]::Clear($pfxBytes, 0, $pfxBytes.Length)
    }
    if (Test-Path -LiteralPath $temporaryPfx) {
        Remove-Item -LiteralPath $temporaryPfx -Force
    }
}
