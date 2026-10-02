. C:\topaz\stage\lab.ps1
$m = 0x0a610000
Set-Reg ($m + 0x208) 0xFFFFFFFF
# read 1 byte (data field = len-1 = 0), dev 0, cmd id 1, reg 0x50 (SCP_DevId_0)
Set-Reg ($m + 0x304) ((0 -shl 24) -bor (0 -shl 20) -bor (1 -shl 16) -bor 0x50)
Start-Sleep -Milliseconds 5
"fifo status 0x30C / irq 0x200 / rd fifo 0x318:"; Get-Reg ($m + 0x30C); Get-Reg ($m + 0x200); Get-Reg ($m + 0x318)
# broadcast: dev 15 ping-ish read of SCP_DevId_0
Set-Reg ($m + 0x304) ((0 -shl 24) -bor (15 -shl 20) -bor (2 -shl 16) -bor 0x50)
Start-Sleep -Milliseconds 5
Get-Reg ($m + 0x30C); Get-Reg ($m + 0x200); Get-Reg ($m + 0x318)
"mcp status / slv status:"; Get-Reg ($m + 0x104C); Get-Reg ($m + 0x1090)
