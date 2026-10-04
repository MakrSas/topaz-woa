# RX SoundWire data ports as in the stock dump: master port 2 = CLSH (ch 1, sint 0x1f, hstart 3 / hstop 6,
# word length 7), master port 4 = LO / AUX (ch 1, offset1 1, sint 7). Slave ports mirror them (stock
# reads: slave 0x203 = 7, 0x220 = 1). Config goes to bank 1, frame bank 1 = 50x16 (stock 0x5000f),
# then the broadcast bank switch (SCP_FrameCtrl_B1 = row 50 idx 1 << 3 | col 16 idx 7).
. C:\topaz\stage\swr.ps1
$m = 0x0a610000
Set-Reg ($m + 0x1264) 0x0100001F; Set-Reg ($m + 0x127C) 0; Set-Reg ($m + 0x122C) 7; Set-Reg ($m + 0x1274) 0x63
Set-Reg ($m + 0x1464) 0x01000107; Set-Reg ($m + 0x147C) 0; Set-Reg ($m + 0x1474) 0
foreach ($p in @(@(0x203,7), @(0x230,1), @(0x232,0x1F), @(0x233,0), @(0x234,0), @(0x236,0x36),
                 @(0x430,1), @(0x432,7), @(0x433,0), @(0x434,1))) { Swr-Wr $m 1 $p[0] $p[1]; Start-Sleep -Milliseconds 1 }
Set-Reg ($m + 0x105C) 0x5000F
Set-Reg ($m + 0x300) ((0x0F -shl 24) -bor (15 -shl 20) -bor (15 -shl 16) -bor 0x70)   # broadcast, cmd id 15
Start-Sleep -Milliseconds 5
"rx master frame b0/b1, irq, slv:"; Get-Reg ($m + 0x101C); Get-Reg ($m + 0x105C); Get-Reg ($m + 0x200); Get-Reg ($m + 0x1090)
"slave port2 b1:"; foreach ($r in 0x230,0x232,0x236,0x430,0x432,0x434) { "{0:x}={1:x}" -f $r, (Swr-Rd $m 1 $r) }
