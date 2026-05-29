param(
    [Parameter(Mandatory = $false)]
    [string]$DriveLetter = "D",

    [Parameter(Mandatory = $false)]
    [string]$PayloadRoot = ".\.artifacts\live-usb-payload\kobox-live",

    [Parameter(Mandatory = $false)]
    [string]$IsoPath = ".\.artifacts\ubuntu-26.04-desktop-amd64.iso",

    [Parameter(Mandatory = $false)]
    [switch]$SkipIso
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$payload = Resolve-Path -LiteralPath (Join-Path $repoRoot $PayloadRoot)
$iso = Resolve-Path -LiteralPath (Join-Path $repoRoot $IsoPath)
$letter = $DriveLetter.TrimEnd(":")
$volume = Get-Volume -DriveLetter $letter
$driveRoot = "${letter}:\"

if ($volume.DriveType -ne "Removable") {
    throw "$driveRoot is not a removable volume"
}

$isoSize = (Get-Item -LiteralPath $iso).Length
if (-not $SkipIso -and $volume.FileSystem -eq "FAT32" -and $isoSize -gt 4GB) {
    throw "$driveRoot is FAT32 and cannot store the Ubuntu ISO. Run .\tools\install_ventoy_usb.ps1 first, then check the removable drive letter with Get-Volume and rerun this script."
}

if (-not $SkipIso) {
    Copy-Item -LiteralPath $iso -Destination (Join-Path $driveRoot (Split-Path -Leaf $iso)) -Force
}

$targetPayload = Join-Path $driveRoot "kobox-live"
if (Test-Path -LiteralPath $targetPayload) {
    Remove-Item -LiteralPath $targetPayload -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $targetPayload | Out-Null
robocopy $payload $targetPayload /MIR /R:1 /W:1 | Out-Null
if ($LASTEXITCODE -gt 7) {
    throw "robocopy failed with exit code $LASTEXITCODE"
}

Write-Host "copied payload to $targetPayload"
