# P9: 1 kHz tone graph = SH_MEM_PULL_MODE (reads a looping sine buffer in RAM) -> CODEC_DMA_SINK on
# RX_CODEC_DMA_RX_1 (stock: RX_MACRO RX2 MUX = AIF2_PB -> RX INT2 -> AUX), 48 kHz 16-bit mono.
# Needs smmu_audio.ps1 first (audio DMA stream 0x1C1). Addresses to the ADSP carry the SID offset
# 0x1C1 & 0xF = 1 in the upper word (msm-audio-ion smmu_sid_bits).
param([int]$Hz = 1000, [double]$Amp = 0.3)
. C:\topaz\stage\lab.ps1
$T = $global:TA
$SG = 0x4001; $CONT = 0x4101; $DMA = 0x7001; $PULL = 0x7002
function Apm([long]$op, [long[]]$dw) { $a = @('apm', '1', '1', ('0x{0:x}' -f $op)) + ($dw | % { '0x{0:x}' -f ($_ -band 0xFFFFFFFFL) }); & $T @a }
$b = & $T buf alloc 0x18000; $b
if ("$b" -notmatch 'buf (\d+) pa ([0-9a-f]+) size ([0-9a-f]+)') { "alloc failed"; exit 1 }
$id = [int]$Matches[1]; $pa = [Convert]::ToInt64($Matches[2], 16)
$s = & $T buf sine $id 96000 $Hz 48000 $Amp; $s
$size = 96000
# APM_CMD_SHARED_MEM_MAP_REGIONS has no apm_cmd_header: raw "cmd", reply 0x02001001 = {mem_map_handle}
"== MEM MAP"; $r = & $T cmd 1 1 0x0100100c ('0x{0:x}' -f (3 -bor (1 -shl 16))) 0 ('0x{0:x}' -f $pa) 1 0x18000; $r
$h = 0; $seen = $false
foreach ($l in $r) { if ($l -match 'opcode 02001001') { $seen = $true } elseif ($seen -and $l -match '^\s+([0-9a-f]{8})') { $h = [Convert]::ToInt64($Matches[1], 16); break } }
"mem map handle {0:x}" -f $h
if ($h -eq 0) { exit 1 }
# position buffer (mainline q6apm_map_pos_buffer: property_flag 0x2)
$pb = & $T buf alloc 0x1000; $pb
if ("$pb" -notmatch 'buf (\d+) pa ([0-9a-f]+)') { "pos alloc failed"; exit 1 }
$pid = [int]$Matches[1]; $ppa = [Convert]::ToInt64($Matches[2], 16)
$r = & $T cmd 1 1 0x0100100c ('0x{0:x}' -f (3 -bor (1 -shl 16))) 2 ('0x{0:x}' -f $ppa) 1 0x1000
$ph = 0; $seen = $false
foreach ($l in $r) { if ($l -match 'opcode 02001001') { $seen = $true } elseif ($seen -and $l -match '^\s+([0-9a-f]{8})') { $ph = [Convert]::ToInt64($Matches[1], 16); break } }
"pos buf {0} pa {1:x} handle {2:x}" -f $pid, $ppa, $ph
$open = @(
  1, 0x08001001, 48, 0,  1,  $SG, 3,  0x0800100E, 4, 2,  0x0800100F, 4, 1,  0x08001010, 4, 1,
  1, 0x08001000, 64, 0,  1,  $CONT, 4,  0x08001011, 8, 1, 3,  0x08001012, 4, 1,  0x08001013, 4, 8192,  0x08001014, 4, 2,
  1, 0x08001002, 32, 0,  1,  $SG, $CONT, 2,  0x07001006, $PULL,  0x07001023, $DMA,
  1, 0x08001003, 56, 0,  2,  $PULL, 1, 0x08001015, 8, 0, 1,  $DMA, 1, 0x08001015, 8, 1, 0,  0,
  1, 0x08001004, 24, 0,  1,  $PULL, 1, $DMA, 2,  0
)
"== GRAPH_OPEN"; Apm 0x01001000 $open
"== pull media format"; Apm 0x01001006 @($PULL, 0x0800100C, 32, 0, 1, 0x09001000, 20, 48000, 0x00010010, 0x000F0010, 0x00010001, 1)
"== pull cfg"; Apm 0x01001006 @($PULL, 0x0800100A, 28, 0, $pa, 1, $size, $h, $ppa, 1, $ph, 0)
"== dma hw ep mf"; Apm 0x01001006 @($DMA, 0x08001017, 12, 0, 48000, 0x00010010, 1, 0)
"== dma intf (RXTX, RX1, ch 0x1)"; Apm 0x01001006 @($DMA, 0x08001063, 12, 0, 1, 2, 1, 0)
"== GRAPH_PREPARE"; Apm 0x01001001 @(1, 0x08001005, 8, 0, 1, $SG)
"== GRAPH_START";   Apm 0x01001002 @(1, 0x08001005, 8, 0, 1, $SG)
Start-Sleep -Milliseconds 500
& $T recv 300
"pos buffer (frame counter, index, ts):"; & $T buf rd $pid 0 16
