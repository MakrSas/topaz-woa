# WCD937x: init (wcd937x_init_reg) + RX clocks + AUX DAC/PA, values from the stock dump while playing.
# Registers go through the TX slave (VA SoundWire bus, device 1).
. C:\topaz\stage\swr.ps1
$va = 0x0a740000
function W([int]$r, [int]$v) { Swr-Wr $va 1 $r $v; Start-Sleep -Milliseconds 1 }
W 0x3103 0xD6                      # SLEEP_CTL (stock)
Start-Sleep -Milliseconds 2
W 0x312A 0x00                      # LDORXTX_CONFIG bit4 off
W 0x3029 0x85                      # BIAS_VBG_FINE_ADJ
W 0x3001 0xC0; Start-Sleep -Milliseconds 10; W 0x3001 0x80   # ANA_BIAS: enable + precharge pulse
W 0x30E2 0xD9                      # HPH_SURGE_HPHLR_SURGE_EN
W 0x30A5 0xEB                      # FLYBACK_VNEG_CTRL_1
# wcd937x_rx_clk_enable
W 0x3409 0x08; W 0x3408 0x01; W 0x3008 0x01
W 0x340D 0xBC; W 0x340E 0xBC; W 0x340F 0xBC
W 0x3408 0x03
# aux dac PRE_PMU
W 0x3408 0x07; W 0x3409 0x0C; W 0x344F 0x01
W 0x3009 0x0C; W 0x300A 0x40; W 0x300B 0x12
W 0x3008 0x41; Start-Sleep -Milliseconds 1; W 0x3008 0xC1
"wcd937x init + aux dac written"
