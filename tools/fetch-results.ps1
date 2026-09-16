param(
    [string]$PiHost = "192.168.20.35",
    [string]$PiUser = "visionseek",
    [string]$Destination = "$HOME\Downloads\hdmirx-latest.tar.gz"
)

$ErrorActionPreference = "Stop"
$source = "${PiUser}@${PiHost}:~/hdmirx-latest.tar.gz"

Write-Host "Fetching $source"
scp $source $Destination

if (-not (Test-Path -LiteralPath $Destination)) {
    throw "Transfer completed without creating $Destination"
}

Get-Item -LiteralPath $Destination | Format-List FullName, Length, LastWriteTime
