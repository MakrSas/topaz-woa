# SoundWire register access through a master's command FIFO (v1.6 layout: WR 0x300, RD 0x304,
# STATUS 0x30C, RD_FIFO 0x318; downstream SWR_REG_VAL_PACK = reg | id<<16 | dev<<20 | data<<24)
. C:\topaz\stage\lab.ps1
$global:SwrId = 0
function Swr-Next { $global:SwrId = ($global:SwrId % 14) + 1; $global:SwrId + 0 }
function Swr-Rd([long]$m, [int]$dev, [int]$reg) {
  $id = Swr-Next
  Set-Reg ($m + 0x304) (($dev -shl 20) -bor ($id -shl 16) -bor $reg)
  for ($t = 0; $t -lt 20; $t++) {      # the RD FIFO may still hold older answers: match the cmd id echo (bits 11:8)
    $v = [Convert]::ToInt64(((Get-Reg ($m + 0x318) 1) -split ' ')[-1], 16)
    if ((($v -shr 8) -band 0xF) -eq $id) { return ($v -band 0xFF) }
  }
  Set-Reg ($m + 0x308) 1               # flush
  -1
}
function Swr-Wr([long]$m, [int]$dev, [int]$reg, [int]$val) {
  $id = Swr-Next
  Set-Reg ($m + 0x300) (($val -shl 24) -bor ($dev -shl 20) -bor ($id -shl 16) -bor $reg)
}
