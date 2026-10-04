. C:\topaz\stage\lab.ps1
$T = $global:TA; $SG = 0x4001
function Apm([long]$op, [long[]]$dw) { $a = @('apm', '1', '1', ('0x{0:x}' -f $op)) + ($dw | % { '0x{0:x}' -f ($_ -band 0xFFFFFFFFL) }); & $T @a }
"== GRAPH_STOP"; Apm 0x01001003 @(1, 0x08001005, 8, 0, 1, $SG)
"== GRAPH_CLOSE"; Apm 0x01001004 @(1, 0x08001005, 8, 0, 1, $SG)
