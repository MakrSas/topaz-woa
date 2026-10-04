# P9: identity context bank for the audio DMA stream (DT msm-audio-ion iommus = apps_smmu 0x1C1), same
# scheme as TopazModem wlanprobe.c SmmuIdentity: free SMR + free CB >= 4, CB cloned from UEFI's
# pass-through config (SCTLR M=0), S2CR type 0 -> CB, SMR valid last. Idempotent.
param([int]$Sid = 0x1C1, [int]$Mask = 0)
. C:\topaz\stage\lab.ps1
function Vals([long]$a, [int]$n) {
  $o = @(); foreach ($l in (Get-Reg $a $n)) { $t = ($l -split ':')[1].Trim() -split '\s+'; foreach ($x in $t) { if ($x) { $o += [Convert]::ToInt64($x, 16) } } }; $o
}
$S = 0x0C600000L
$id = Vals ($S + 0x20) 2; $nsmr = $id[0] -band 0xFF; $ncb = $id[1] -band 0xFF
$psize = if ($id[1] -band 0x80000000L) { 0x10000 } else { 0x1000 }; $npage = 1 -shl ((($id[1] -shr 28) -band 7) + 1)
$smr = Vals ($S + 0x800) $nsmr; $s2 = Vals ($S + 0xC00) $nsmr
$slot = -1; $used = @{}
for ($i = 0; $i -lt $nsmr; $i++) {
  $v = ($smr[$i] -shr 31) -band 1
  if ($v -and (($smr[$i] -band 0xFFFF) -eq $Sid)) { $slot = $i; "SMR$i already has sid {0:x}" -f $Sid }
  elseif (-not $v -and $slot -lt 0) { $slot = $i }
  if ($v -and ((($s2[$i] -shr 16) -band 3) -eq 0)) { $used[[int]($s2[$i] -band 0x3F)] = 1 }
}
$cb = -1; for ($i = 4; $i -lt $ncb; $i++) { if (-not $used.ContainsKey($i)) { $cb = $i; break } }
"nsmr $nsmr ncb $ncb psize {0:x} npage $npage -> SMR $slot CB $cb" -f $psize
if ($slot -lt 0 -or $cb -lt 0) { "no free SMR/CB"; exit 1 }
$cbp = $S + ($npage + $cb) * $psize; $gr1 = $S + $psize
Set-Reg ($cbp + 0x58) 0xFFFFFFFF; Set-Reg ($cbp + 0x20) 0; Set-Reg ($cbp + 0x24) 0; Set-Reg ($cbp + 0x30) 0
Set-Reg ($cbp + 0x10) 0; Set-Reg ($cbp + 0x38) 0; Set-Reg ($cbp + 0x3C) 0
Set-Reg ($gr1 + 0x800 + 4 * $cb) 1; Set-Reg ($gr1 + 4 * $cb) 0x1F000; Set-Reg $cbp 0xE0
Set-Reg ($S + 0xC00 + 4 * $slot) $cb; Set-Reg ($S + 0x800 + 4 * $slot) (0x80000000L -bor ($Mask -shl 16) -bor $Sid)
Set-Reg ($S + 0x70) 0
"SMR / S2CR / SCTLR:"; Get-Reg ($S + 0x800 + 4 * $slot); Get-Reg ($S + 0xC00 + 4 * $slot); Get-Reg $cbp
