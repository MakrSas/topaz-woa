# Gives the root-enumerated TopazGpuW device the Adreno interrupt (GIC SPI 177 = GSIV 209, level) as a
# PnP resource requirement: LogConf\BasicConfigVector = IO_RESOURCE_REQUIREMENTS_LIST with one
# interrupt descriptor (the INF LogConfigOverride is ignored for devcon-created root devices).
# Enum\ is writable only by SYSTEM, so the .reg is imported from a one-shot SYSTEM task.
$dev = Get-PnpDevice -FriendlyName '*Adreno 610*' | Select-Object -First 1
if (-not $dev) { Write-Host 'TopazGpuW device not found'; exit 1 }
$b = New-Object byte[] 72
[BitConverter]::GetBytes([uint32]72).CopyTo($b, 0)     # ListSize; InterfaceType 0 (Internal), bus/slot 0
[BitConverter]::GetBytes([uint32]1).CopyTo($b, 28)     # AlternativeLists
[BitConverter]::GetBytes([uint16]1).CopyTo($b, 32)     # IO_RESOURCE_LIST.Version
[BitConverter]::GetBytes([uint16]1).CopyTo($b, 34)     # Revision
[BitConverter]::GetBytes([uint32]1).CopyTo($b, 36)     # Count
$b[40] = 0; $b[41] = 2; $b[42] = 1                     # Option, Type = CmResourceTypeInterrupt, DeviceExclusive
[BitConverter]::GetBytes([uint16]0).CopyTo($b, 44)     # Flags: level sensitive
[BitConverter]::GetBytes([uint32]209).CopyTo($b, 48)   # MinimumVector
[BitConverter]::GetBytes([uint32]209).CopyTo($b, 52)   # MaximumVector
$hex = ($b | ForEach-Object { '{0:x2}' -f $_ }) -join ','
$reg = "Windows Registry Editor Version 5.00`r`n`r`n[HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Enum\$($dev.InstanceId)\LogConf]`r`n" +
       "`"BasicConfigVector`"=hex(a):$hex`r`n"
Set-Content -Path C:\topaz\gpuw-irq.reg -Value $reg -Encoding Unicode
schtasks /create /tn tgpuw-irq /ru SYSTEM /sc once /st 00:00 /tr "reg import C:\topaz\gpuw-irq.reg" /f | Out-Null
schtasks /run /tn tgpuw-irq | Out-Null
Start-Sleep 3
schtasks /delete /tn tgpuw-irq /f | Out-Null
reg query "HKLM\SYSTEM\CurrentControlSet\Enum\$($dev.InstanceId)\LogConf"
