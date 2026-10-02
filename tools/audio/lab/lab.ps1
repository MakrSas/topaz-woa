# shared helpers for the TopazAudio lab scripts (dot-source this)
$global:TA = "C:\topaz\stage\TopazAudio\taudio.exe"
function Get-Reg([long]$a, [int]$n = 1) { & $global:TA rd ('0x{0:x}' -f $a) $n }
function Set-Reg([long]$a, [long]$v) { & $global:TA wr ('0x{0:x}' -f $a) ('0x{0:x}' -f ($v -band 0xFFFFFFFFL)) | Out-Null }
