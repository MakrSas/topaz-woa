# Enables the Windows brightness slider for the Topaz panel.
# TopazDisplay answers DXGK_BRIGHTNESS_INTERFACE, but monitor.sys only creates WmiMonitorBrightness for an internal
# connector or when the monitor's driver key has BrightnessControl bit0 = 1 (TopazDisplay cannot report an internal
# connector: dxgkrnl rejects it for a non-POST adapter). Run elevated after TopazDisplay has started once
# (the monitor DISPLAY\TPZ6225 must exist), then reboot (the value is read when the monitor devnode starts).
# Undo: -Remove. Docs: docs/NOTES_brightness.md
param([switch]$Remove)
$ErrorActionPreference = 'Stop'
$mons = Get-PnpDevice -Class Monitor -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'DISPLAY\TPZ6225*' }
if (-not $mons) { Write-Host 'No DISPLAY\TPZ6225 monitor found: install/start TopazDisplay first.'; exit 1 }
foreach ($m in $mons) {
    $drv = (Get-PnpDeviceProperty -InstanceId $m.InstanceId -KeyName DEVPKEY_Device_Driver -ErrorAction SilentlyContinue).Data
    if (-not $drv) { Write-Host "$($m.InstanceId): no driver key (ghost device), skipped"; continue }
    $key = "HKLM:\SYSTEM\CurrentControlSet\Control\Class\$drv"
    if ($Remove) {
        Remove-ItemProperty -Path $key -Name BrightnessControl -ErrorAction SilentlyContinue
        Write-Host "$($m.InstanceId): BrightnessControl removed ($drv)"
    } else {
        New-ItemProperty -Path $key -Name BrightnessControl -PropertyType DWord -Value 1 -Force | Out-Null
        Write-Host "$($m.InstanceId): BrightnessControl=1 ($drv)"
    }
}
Write-Host 'Reboot for the change to take effect.'
