param([string]$Mode = 'clear', [string]$Dbg = '')
[Console]::OutputEncoding=[Text.Encoding]::UTF8
# no devcon restart: a PnP restart waits for DWM to release the adapter and holds the PnP lock
# (explorer/network/sshd hung). TopazGpuW v0.25+ recovers a wedged GPU on the next submit.
$logLen = (Get-Item C:\TopazGpuW.log).Length
$args2 = @($Mode); if ($Dbg) { $args2 += $Dbg }
$p = Start-Process C:\topaz\stage\TopazGpuW\d3dtest.exe -ArgumentList $args2 -NoNewWindow -PassThru -RedirectStandardOutput C:\topaz\d3d.out -RedirectStandardError C:\topaz\d3d.err
$null = $p.Handle
if (-not $p.WaitForExit(60000)) { "TIMEOUT" } else { "exit code $($p.ExitCode)" }
Get-Content C:\topaz\d3d.out, C:\topaz\d3d.err | Where-Object { $_ -notmatch '^\s+\S+\+0x' }
$fs = [IO.File]::Open('C:\TopazGpuW.log','Open','Read','ReadWrite'); $fs.Seek($logLen,0) | Out-Null
(New-Object IO.StreamReader($fs)).ReadToEnd() -split "`n" | Where-Object { $_ -match 'hang|wedged|fault|IB[12] |^\s+[0-9a-f]+[*:] ' }; $fs.Close()
