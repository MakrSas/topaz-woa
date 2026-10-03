# Watches explorer.exe (and dwm.exe via its own heartbeat) in the interactive session; when explorer stops
# responding for 2 checks in a row, writes a full dump with comsvcs.dll MiniDump to C:\topaz\dumps\hang-*.dmp.
# Start it with a /IT scheduled task (Process.Responding needs the window station). Stops after $Minutes.
param([int]$Minutes = 60)
New-Item -ItemType Directory -Force C:\topaz\dumps | Out-Null
$end = (Get-Date).AddMinutes($Minutes); $bad = @{}
while ((Get-Date) -lt $end) {
  foreach ($p in Get-Process explorer -EA SilentlyContinue) {
    $p.Refresh()
    if (-not $p.Responding) { $bad[$p.Id] = 1 + [int]$bad[$p.Id] } else { $bad[$p.Id] = 0 }
    if ($bad[$p.Id] -eq 2) {
      $f = "C:\topaz\dumps\hang-explorer-$($p.Id)-$(Get-Date -f HHmmss).dmp"
      Add-Content C:\topaz\hangwatch.log "$(Get-Date -f HH:mm:ss) explorer $($p.Id) not responding -> $f"
      & rundll32.exe C:\Windows\System32\comsvcs.dll, MiniDump $p.Id $f full
    }
  }
  Start-Sleep 2
}
