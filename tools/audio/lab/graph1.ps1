# P9 A5: minimal device graph = CODEC_DMA_SINK on RX_CODEC_DMA_RX_0 (no input), open/config/prepare/start
. C:\topaz\stage\lab.ps1
$T = $global:TA
$SG = 0x4001; $CONT = 0x4101; $MOD = 0x7001
function Apm([long]$op, [long[]]$dw) { $a = @('apm', '1', '1', ('0x{0:x}' -f $op)) + ($dw | % { '0x{0:x}' -f ($_ -band 0xFFFFFFFFL) }); & $T @a }
$open = @(
  # sub-graph config: 1 SG, 3 props (perf low-latency, direction TX = device playback, scenario playback)
  1, 0x08001001, 48, 0,  1,  $SG, 3,  0x0800100E, 4, 2,  0x0800100F, 4, 1,  0x08001010, 4, 1,
  # container config: EP capability, graph pos stream, stack 8192, ADSP domain
  1, 0x08001000, 64, 0,  1,  $CONT, 4,  0x08001011, 8, 1, 3,  0x08001012, 4, 1,  0x08001013, 4, 8192,  0x08001014, 4, 2,
  # module list: SG, container, 1 module
  1, 0x08001002, 24, 0,  1,  $SG, $CONT, 1,  0x07001023, $MOD,
  # module props: port info 1 in / 0 out (+4 bytes pad to 8)
  1, 0x08001003, 32, 0,  1,  $MOD, 1, 0x08001015, 8, 1, 0,  0,
  # connections: none (+pad)
  1, 0x08001004, 8, 0,  0,  0
)
"== GRAPH_OPEN"; Apm 0x01001000 $open
"== SET_CFG hw ep media format + codec dma intf (RXTX, RX0, ch 0x3)"
Apm 0x01001006 @($MOD, 0x08001017, 12, 0, 48000, (16 -bor (2 -shl 16)), 1,  $MOD, 0x08001063, 12, 0, 1, 1, 3)
"== GRAPH_PREPARE"; Apm 0x01001001 @(1, 0x08001005, 8, 0, 1, $SG)
"== GRAPH_START";   Apm 0x01001002 @(1, 0x08001005, 8, 0, 1, $SG)
Start-Sleep -Milliseconds 300
& $T recv 300
"rx master irq / slv:"; Get-Reg 0x0a610200; Get-Reg 0x0a611090
$s = ""; foreach ($i in 1..12) { $s += ((& $T rd 0x0a7c3004 1) -split ' ')[-1].Substring(7) }; "clk gpio3: $s"
