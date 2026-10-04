# after bringup + smmu_audio + rx_stock + wcd_aux: tone graph, SoundWire ports, AUX PA, amp, unmute
param([int]$Hz = 1000, [double]$Amp = 0.3)
. C:\topaz\stage\lab.ps1
$P = "C:\topaz\stage"
& powershell -NoProfile -ExecutionPolicy Bypass -File "$P\tone.ps1" -Hz $Hz -Amp $Amp 2>&1 | Select-String "buf|handle|==|^\s+0100" 
& powershell -NoProfile -ExecutionPolicy Bypass -File "$P\ports.ps1" 2>&1
& powershell -NoProfile -ExecutionPolicy Bypass -File "$P\wcd_auxpa.ps1" 2>&1
& powershell -NoProfile -ExecutionPolicy Bypass -File "$P\amp.ps1" 2>&1
Set-Reg 0x0a600518 0x04; Set-Reg 0x0a600500 0x24
"rx2 path ctl:"; Get-Reg 0x0a600500
