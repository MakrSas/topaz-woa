# Gives the root-enumerated TopazGpuW device the Adreno interrupt (GIC SPI 177 = GSIV 209, level) as a
# PnP resource requirement: LogConf\BasicConfigVector = IO_RESOURCE_REQUIREMENTS_LIST (REG type 10)
# with one interrupt descriptor (INF LogConfigOverride is ignored for devcon-created root devices).
# Enum\ is writable only by SYSTEM: re-launches itself as a one-shot LocalSystem service (-AsSystem);
# the Task Scheduler on this install queues tasks without running them.
param([switch]$AsSystem)
$log = 'C:\topaz\gpuw-irq.log'
if (-not $AsSystem) {
    $dev = Get-PnpDevice -FriendlyName '*Adreno 610*' | Select-Object -First 1
    if (-not $dev) { Write-Host 'TopazGpuW device not found'; exit 1 }
    Set-Content C:\topaz\gpuw-irq.id $dev.InstanceId
    Remove-Item $log -ErrorAction SilentlyContinue
    $cmd = "C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath -AsSystem"
    sc.exe create tgpuwirq binPath= $cmd type= own start= demand | Out-Null
    sc.exe start tgpuwirq | Out-Null                     # "not a service" error is expected: the command ran
    for ($i = 0; $i -lt 30 -and -not (Test-Path $log); $i++) { Start-Sleep 1 }
    sc.exe delete tgpuwirq | Out-Null
    Get-Content $log -ErrorAction SilentlyContinue
    exit 0
}
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class Reg {
  [DllImport("advapi32.dll", CharSet=CharSet.Unicode)] public static extern int RegCreateKeyExW(IntPtr hKey, string sub, int res, string cls, int opt, int sam, IntPtr sec, out IntPtr result, out int disp);
  [DllImport("advapi32.dll", CharSet=CharSet.Unicode)] public static extern int RegSetValueExW(IntPtr hKey, string name, int res, int type, byte[] data, int size);
  [DllImport("advapi32.dll")] public static extern int RegCloseKey(IntPtr hKey);
}
'@
$id = (Get-Content C:\topaz\gpuw-irq.id).Trim()
$b = New-Object byte[] 72
[BitConverter]::GetBytes([uint32]72).CopyTo($b, 0)
[BitConverter]::GetBytes([uint32]1).CopyTo($b, 28)
[BitConverter]::GetBytes([uint16]1).CopyTo($b, 32)
[BitConverter]::GetBytes([uint16]1).CopyTo($b, 34)
[BitConverter]::GetBytes([uint32]1).CopyTo($b, 36)
$b[40] = 0; $b[41] = 2; $b[42] = 1
[BitConverter]::GetBytes([uint32]209).CopyTo($b, 48)
[BitConverter]::GetBytes([uint32]209).CopyTo($b, 52)
$HKLM = [IntPtr]([int64]0x80000002 -bor 0xFFFFFFFF00000000)
$k = [IntPtr]::Zero; $disp = 0
$r1 = [Reg]::RegCreateKeyExW($HKLM, "SYSTEM\CurrentControlSet\Enum\$id\LogConf", 0, $null, 0, 0xF003F, [IntPtr]::Zero, [ref]$k, [ref]$disp)
$r2 = [Reg]::RegSetValueExW($k, 'BasicConfigVector', 0, 10, $b, $b.Length)
[Reg]::RegCloseKey($k) | Out-Null
"user $(whoami) id $id create $r1 set $r2" | Set-Content C:\topaz\gpuw-irq.log
