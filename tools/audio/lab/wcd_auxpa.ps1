# WCD937x AUX PA on (after the SoundWire ports run): PDM watchdog, AUX PA, interrupt masks (stock values)
. C:\topaz\stage\swr.ps1
$va = 0x0a740000
foreach ($p in @(@(0x3467,0x05), @(0x3128,0x80), @(0x346B,0x4C), @(0x346C,0x7F), @(0x346D,0x0F))) { Swr-Wr $va 1 $p[0] $p[1]; Start-Sleep -Milliseconds 1 }
"aux pa on"
