# RX2 (speaker / AUX path) digital volume, dB -84..+40 (stock control "RX_RX2 Digital Volume", 0 = 0 dB)
param([int]$Db = -30)
. C:\topaz\stage\lab.ps1
Set-Reg 0x0a600514 ($Db -band 0xFF)
"RX2 volume $Db dB:"; Get-Reg 0x0a600514
