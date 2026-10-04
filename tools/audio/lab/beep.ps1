# short tone at a given digital volume (codec already set up by bringup/rx_stock/wcd_aux/ports/wcd_auxpa)
param([int]$Db = -30, [int]$Hz = 1000, [double]$Amp = 0.1, [int]$Sec = 3)
. C:\topaz\stage\lab.ps1
$P = "C:\topaz\stage"
& powershell -NoProfile -ExecutionPolicy Bypass -File "$P\vol.ps1" -Db $Db
& powershell -NoProfile -ExecutionPolicy Bypass -File "$P\tone.ps1" -Hz $Hz -Amp $Amp 2>&1 | Select-String "^\s+0100" | Select -Last 1
& $global:TA i2c 0x2b 5 1 | Out-Null
Start-Sleep -Seconds $Sec
& $global:TA i2c 0x2b 5 0 | Out-Null
& powershell -NoProfile -ExecutionPolicy Bypass -File "$P\graphstop.ps1" 2>&1 | Out-Null
"beep done ($Db dB, $Hz Hz, amp $Amp, $Sec s)"
