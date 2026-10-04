# P9: are the SoundWire clock pads driven? func1 + pull-up: a driven clock toggles / reads 0, an undriven pad reads 1
. C:\topaz\stage\lab.ps1
function S($a, $tag) { $s = ""; foreach ($i in 1..24) { $s += ((Get-Reg $a 1) -split ' ')[-1].Substring(7) }; "$tag $s" }
Set-Reg 0x0a7c3000 0x107; Set-Reg 0x0a7c0000 0x107
S 0x0a7c3004 "rx swr clk gpio3 (pull-up):"
S 0x0a7c0004 "va swr clk gpio0 (pull-up):"
Set-Reg 0x0a7c3000 0xD04; Set-Reg 0x0a7c0000 0xD04
foreach ($m in 0x0a610000, 0x0a740000) { "master {0:x} status / irq / slv:" -f $m; Get-Reg ($m + 0x14); Get-Reg ($m + 0x200); Get-Reg ($m + 0x1090) }
