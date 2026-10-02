# P9 A5: LPASS codec bring-up from a fresh boot (ADSP up). Safe set only (no 0xa7e0xxx: hangs).
. C:\topaz\stage\lab.ps1
$T = $global:TA
function Init-Master([long]$m) {
  Set-Reg ($m + 0x008) 1; Set-Reg ($m + 0x008) 1; Set-Reg ($m + 0x1044) 1; Start-Sleep -Milliseconds 2
  Set-Reg ($m + 0x101C) 0xF; Set-Reg ($m + 0x500) 1
  Set-Reg ($m + 0x1048) (0x1F -shl 17); Set-Reg ($m + 0x314) 3; Set-Reg ($m + 0x1044) 2
  Set-Reg ($m + 0x04) 2; Set-Reg ($m + 0x208) 0xFFFFFFFF; Set-Reg ($m + 0x204) 0x1FDFD; Set-Reg ($m + 0x210) 0x1FDFD
  Set-Reg ($m + 0x04) 3; Set-Reg ($m + 0x314) 0x80000003
}
# 1. ADSP PRM: DCODEC vote + clocks (RX 22.5792 MHz, TX/VA 19.2 MHz, cores + NPL)
& $T prm-hw 2 | Select-String "<-|^ +0" | Out-Null
foreach ($c in @(@(0x30e,22579200), @(0x30f,22579200), @(0x30c,19200000), @(0x30d,19200000), @(0x307,19200000), @(0x308,19200000))) {
  $r = & $T prm-clk $c[0] $c[1]; "clk {0:x}: {1}" -f $c[0], ($r[-1].Trim())
}
# 2. LPI pins (TX/VA swr gpio0-2, RX swr gpio3-5), slew
Set-Reg 0x0a7c0000 0xD04; Set-Reg 0x0a7c1000 0xD06; Set-Reg 0x0a7c2000 0xD06
Set-Reg 0x0a7c3000 0xD04; Set-Reg 0x0a7c4000 0xD06; Set-Reg 0x0a7c5000 0xD06
Set-Reg 0x0a95a000 0x3F3F
# 3. VA macro (fsgen, runs on TX MCLK)
$va = 0x0a730000; Set-Reg $va 1; Set-Reg ($va + 4) 3; Set-Reg ($va + 4) 1; Set-Reg ($va + 0x80) 2
# 4. RX macro: MCLK+MCLK2, FS, SWR clock with reset; HCTL; master
$rx = 0x0a600000; Set-Reg ($rx + 0x100) 3; Set-Reg ($rx + 0x104) 3; Set-Reg ($rx + 0x104) 1
Set-Reg ($rx + 0x108) 2; Set-Reg ($rx + 0x108) 3; Set-Reg ($rx + 0x108) 1
Set-Reg 0x0a6a9098 3; Start-Sleep -Milliseconds 1; Set-Reg 0x0a6a9098 1
# 5. TX macro: MCLK, FS, SWR clock with reset (clocks the VA SoundWire master, agatti DT); HCTL 0xa7ec100
$tx = 0x0a620000; Set-Reg $tx 1; Set-Reg ($tx + 4) 3; Set-Reg ($tx + 4) 1
Set-Reg ($tx + 8) 2; Set-Reg ($tx + 8) 3; Set-Reg ($tx + 8) 1
Set-Reg 0x0a7ec100 3; Start-Sleep -Milliseconds 1; Set-Reg 0x0a7ec100 1
# 6. WCD937x reset (TLMM gpio92) held low while the buses start, then released
Set-Reg 0x0055c004 0; Set-Reg 0x0055c000 0x200
Init-Master 0x0a610000; Init-Master 0x0a740000
Start-Sleep -Milliseconds 20; Set-Reg 0x0055c004 2; Start-Sleep -Milliseconds 300
foreach ($m in 0x0a610000, 0x0a740000) { "master {0:x} irq / slv / devid:" -f $m; Get-Reg ($m + 0x200); Get-Reg ($m + 0x1090); Get-Reg ($m + 0x538) 2 }
foreach ($p in 0, 3) { $s = ""; foreach ($i in 1..12) { $s += ((& $T rd ('0x{0:x}' -f (0x0a7c0004 + 0x1000 * $p)) 1) -split ' ')[-1].Substring(7) }; "clk gpio$p : $s" }
