# Window that counts mouse/touch move events per second (touch is promoted to mouse) for $Seconds and logs them
# to C:\topaz\touchrate.log. Run with a /IT scheduled task, then drag a finger inside the window.
param([int]$Seconds = 25)
Add-Type -AssemblyName System.Windows.Forms
$f = New-Object Windows.Forms.Form
$f.Text = 'touchrate: drag here'; $f.Width = 900; $f.Height = 1400; $f.StartPosition = 'Manual'; $f.Left = 60; $f.Top = 300
$f.BackColor = 'LightYellow'
$script:n = 0; $t0 = Get-Date
$f.Add_MouseMove({ $script:n++ })
$tm = New-Object Windows.Forms.Timer; $tm.Interval = 1000
$tm.Add_Tick({
  Add-Content C:\topaz\touchrate.log "$(Get-Date -f HH:mm:ss) moves/s $script:n"
  $f.Text = "touchrate: $script:n moves/s"
  $script:n = 0
  if (((Get-Date) - $t0).TotalSeconds -gt $Seconds) { $tm.Stop(); $f.Close() }
})
$f.Add_Shown({ $tm.Start() })
[Windows.Forms.Application]::Run($f)
