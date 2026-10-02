[Console]::OutputEncoding=[Text.Encoding]::UTF8
$logLen = (Get-Item C:\TopazGpuW.log).Length
$p = Start-Process C:\topaz\stage\TopazGpuW\d3dtest.exe -NoNewWindow -PassThru -RedirectStandardOutput C:\topaz\d3d.out -RedirectStandardError C:\topaz\d3d.err
$null = $p.Handle
if (-not $p.WaitForExit(60000)) { "TIMEOUT (pid $($p.Id))" } else { "exit code $($p.ExitCode)" }
Get-Content C:\topaz\d3d.out, C:\topaz\d3d.err
"---- KMD log since start"
$fs = [IO.File]::Open('C:\TopazGpuW.log','Open','Read','ReadWrite'); $fs.Seek($logLen,0) | Out-Null
(New-Object IO.StreamReader($fs)).ReadToEnd(); $fs.Close()
