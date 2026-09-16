[CmdletBinding()]
param(
    [string]$PiUser = "visionseek",
    [string]$PiHost = "192.168.20.35",
    [string]$Destination = (Join-Path $HOME "Downloads\hdmirx-latest.tar.gz")
)

$ErrorActionPreference = "Stop"
$destinationDirectory = Split-Path -Parent $Destination
if (-not (Test-Path -LiteralPath $destinationDirectory)) {
    New-Item -ItemType Directory -Path $destinationDirectory | Out-Null
}

$remote = "${PiUser}@${PiHost}:~/hdmirx-results/latest.tar.gz"
Write-Host "Fetching $remote"
& scp $remote $Destination
if ($LASTEXITCODE -ne 0) {
    throw "scp failed with exit code $LASTEXITCODE"
}

$file = Get-Item -LiteralPath $Destination
$hash = Get-FileHash -Algorithm SHA256 -LiteralPath $Destination
Write-Host "Downloaded: $($file.FullName)"
Write-Host "Size:       $($file.Length) bytes"
Write-Host "SHA-256:    $($hash.Hash)"
