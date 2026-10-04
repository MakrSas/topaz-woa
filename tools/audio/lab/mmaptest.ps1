param([long]$Pa, [int]$Msw = 0, [int]$Prop = 0, [int]$Pool = 3)
. C:\topaz\stage\lab.ps1
$a = @('cmd','1','1','0x0100100c', ('0x{0:x}' -f ($Pool -bor (1 -shl 16))), ('0x{0:x}' -f $Prop), ('0x{0:x}' -f $Pa), ('0x{0:x}' -f $Msw), '0x18000')
& $global:TA @a
