. C:\topaz\stage\lab.ps1
function Init-Master([long]$m) {
  Set-Reg ($m + 0x008) 1; Set-Reg ($m + 0x008) 1; Set-Reg ($m + 0x1044) 1; Start-Sleep -Milliseconds 2
  Set-Reg ($m + 0x101C) 0xF; Set-Reg ($m + 0x500) 1
  Set-Reg ($m + 0x1048) (0x1F -shl 17); Set-Reg ($m + 0x314) 3; Set-Reg ($m + 0x1044) 2
  Set-Reg ($m + 0x04) 2; Set-Reg ($m + 0x208) 0xFFFFFFFF; Set-Reg ($m + 0x204) 0x1FDFD; Set-Reg ($m + 0x210) 0x1FDFD
  Set-Reg ($m + 0x04) 3; Set-Reg ($m + 0x314) 0x80000003
}
Set-Reg 0x0055c004 0                      # codec held in reset while the bus starts
Init-Master 0x0a610000; Init-Master 0x0a740000
Start-Sleep -Milliseconds 50
Set-Reg 0x0055c004 2                      # release
foreach ($ms in 50, 500) {
  Start-Sleep -Milliseconds $ms
  foreach ($m in 0x0a610000, 0x0a740000) { "{0:x} irq / slv:" -f $m; Get-Reg ($m + 0x200); Get-Reg ($m + 0x1090) }
}
