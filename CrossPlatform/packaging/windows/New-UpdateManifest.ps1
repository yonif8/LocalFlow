[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Version,

    [Parameter(Mandatory = $true)]
    [string] $InstallerPath,

    [Parameter(Mandatory = $true)]
    [string] $DownloadUrl,

    [ValidateSet("stable", "beta", "ci")]
    [string] $Channel = "stable",

    [Parameter(Mandatory = $true)]
    [string] $OutputPath,

    [switch] $RequireSigned
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if ($Version -notmatch '^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$') {
    throw "Version must have the form X.Y.Z or X.Y.Z-prerelease."
}
if (-not (Test-Path -LiteralPath $InstallerPath -PathType Leaf)) {
    throw "Installer does not exist: $InstallerPath"
}
$downloadUri = $null
if (-not [Uri]::TryCreate($DownloadUrl, [UriKind]::Absolute, [ref] $downloadUri) `
    -or $downloadUri.Scheme -ne "https") {
    throw "DownloadUrl must be an absolute HTTPS URL."
}

$installer = Get-Item -LiteralPath $InstallerPath
$signature = Get-AuthenticodeSignature -LiteralPath $installer.FullName
$isSigned = $signature.Status -eq [System.Management.Automation.SignatureStatus]::Valid
if ($RequireSigned -and -not $isSigned) {
    throw "Production update manifests require a valid Authenticode-signed installer."
}
$signerFingerprint = if ($isSigned) {
    $signature.SignerCertificate.GetCertHashString(
        [System.Security.Cryptography.HashAlgorithmName]::SHA256).ToUpperInvariant()
} else {
    $null
}

$manifest = [ordered]@{
    schemaVersion = 1
    product = "LocalFlow"
    channel = $Channel
    version = $Version
    publishedAt = [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ")
    platform = [ordered]@{
        os = "windows"
        architecture = "x86_64"
        minimumBuild = 19041
    }
    installer = [ordered]@{
        fileName = $installer.Name
        url = $downloadUri.AbsoluteUri
        sizeBytes = $installer.Length
        sha256 = (Get-FileHash -LiteralPath $installer.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        authenticode = [ordered]@{
            required = $RequireSigned.IsPresent
            validAtPublication = $isSigned
            signerCertificateSha256 = $signerFingerprint
        }
        silentArguments = "/CURRENTUSER /VERYSILENT /SUPPRESSMSGBOXES /NORESTART"
    }
}

$parent = Split-Path -Parent $OutputPath
if ($parent) {
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
}
$temporary = "$OutputPath.tmp-$PID"
try {
    $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $temporary -Encoding utf8NoBOM
    Move-Item -LiteralPath $temporary -Destination $OutputPath -Force
} finally {
    if (Test-Path -LiteralPath $temporary) {
        Remove-Item -LiteralPath $temporary -Force
    }
}
