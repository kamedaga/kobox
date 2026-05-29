param(
    [Parameter(Mandatory = $false)]
    [int]$DiskNumber = 1,

    [Parameter(Mandatory = $false)]
    [string]$ExpectedFriendlyName = "General UDisk",

    [Parameter(Mandatory = $false)]
    [string]$ExpectedBusType = "USB"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$ventoyRoot = Resolve-Path -LiteralPath (Join-Path $repoRoot ".artifacts\ventoy-1.1.12")
$ventoyExe = Join-Path $ventoyRoot "Ventoy2Disk.exe"

if (-not (Test-Path -LiteralPath $ventoyExe)) {
    throw "Ventoy2Disk.exe was not found: $ventoyExe"
}

$disk = Get-Disk -Number $DiskNumber
if ($disk.IsBoot -or $disk.IsSystem) {
    throw "Refusing to touch boot/system disk $DiskNumber"
}
if ($disk.BusType.ToString() -ne $ExpectedBusType) {
    throw "Disk $DiskNumber BusType is $($disk.BusType), expected $ExpectedBusType"
}
if ($disk.FriendlyName -ne $ExpectedFriendlyName) {
    throw "Disk $DiskNumber FriendlyName is '$($disk.FriendlyName)', expected '$ExpectedFriendlyName'"
}

Write-Host "Ventoy target confirmed:"
Write-Host "  DiskNumber   : $($disk.Number)"
Write-Host "  FriendlyName : $($disk.FriendlyName)"
Write-Host "  BusType      : $($disk.BusType)"
Write-Host "  Size         : $([math]::Round($disk.Size / 1GB, 2)) GiB"
Write-Host ""
Write-Host "Launching Ventoy with administrator privileges."
Write-Host "In Ventoy, select the USB disk above and run Install. This will erase the USB."

Start-Process -FilePath $ventoyExe -WorkingDirectory $ventoyRoot -Verb RunAs -Wait

Write-Host ""
Write-Host "Ventoy finished or was closed. Replug the USB if Windows does not refresh the new volume."
Get-Disk -Number $DiskNumber | Select-Object Number,FriendlyName,BusType,Size,PartitionStyle,OperationalStatus,IsBoot,IsSystem | Format-Table -AutoSize
Get-Volume | Where-Object { $_.DriveType -eq "Removable" } | Select-Object DriveLetter,FileSystemLabel,FileSystem,DriveType,Size,SizeRemaining | Format-Table -AutoSize
