# DWM composition benchmark for the topaz GPU work: a window that moves every 15 ms for $Seconds seconds
# (run in the interactive session, e.g. via a scheduled task with /IT). FPS comes from C:\TopazGpuW.log.
param([int]$Seconds = 15, [switch]$Borderless, [switch]$Big)
Add-Type -AssemblyName System.Windows.Forms
$f = New-Object Windows.Forms.Form
$f.Text = 'fpsbench'; $f.Width = 500; $f.Height = 400; $f.StartPosition = 'Manual'; $f.TopMost = $true
$f.Left = 50; $f.Top = 900
if ($Big) { $f.Width = 560; $f.Height = 1000; $f.Top = 100; $f.BackgroundImage = $null }
if ($Borderless) { $f.FormBorderStyle = 'None'; $f.BackColor = 'SteelBlue' }
$dx = 6; $t0 = Get-Date
$tm = New-Object Windows.Forms.Timer; $tm.Interval = 15
$tm.Add_Tick({
  $f.Left += $dx
  if ($f.Left -gt 550 -or $f.Left -lt 20) { $script:dx = -$script:dx }
  if (((Get-Date) - $t0).TotalSeconds -gt $Seconds) { $tm.Stop(); $f.Close() }
})
$f.Add_Shown({ $tm.Start() })
[Windows.Forms.Application]::Run($f)
