# sia8159 speaker amp on (I2C 0x2b, stock regmap writes during playback start, ALGO 0x05 = 1)
. C:\topaz\stage\lab.ps1
foreach ($p in @(@(1,0x7c),@(2,0x21),@(5,0x01),@(3,0x30),@(4,0xc5),@(6,0x48),@(7,0x5d),@(8,0x8a),@(9,0x0e),@(10,0xa4))) {
  & $global:TA i2c 0x2b $p[0] $p[1]
}
& $global:TA i2c 0x2b 0 r11
