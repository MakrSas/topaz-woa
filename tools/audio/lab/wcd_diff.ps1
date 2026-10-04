# read every WCD937x register of the stock dump (reg: val) from our codec (TX slave, VA bus dev 1), print differences
param([string]$Stock = "C:\topaz\stage\wcd937x_stock.txt")
. C:\topaz\stage\swr.ps1
$va = 0x0a740000
foreach ($l in Get-Content $Stock) {
  $p = $l -split ':'; $r = [Convert]::ToInt32($p[0].Trim(), 16); $s = [Convert]::ToInt32($p[1].Trim(), 16)
  $o = (Swr-Rd $va 1 $r) -band 0xFF
  if ($o -ne $s) { "{0:x4} ours {1:x2} stock {2:x2}" -f $r, $o, $s }
}
