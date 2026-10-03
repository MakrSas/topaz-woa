# Moves an existing top-level window (default: Chrome) back and forth for $Seconds, then restores it.
# Run in the interactive session (/IT scheduled task). DWM FPS comes from C:\TopazGpuW.log / UMD TIMING.
param([int]$Seconds = 10, [string]$Process = "chrome")
Add-Type @"
using System; using System.Runtime.InteropServices;
public class W { [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
 public struct RECT { public int L, T, R, B; } }
"@
$p = Get-Process $Process -EA SilentlyContinue | ? { $_.MainWindowHandle -ne 0 } | select -First 1
if (-not $p) { Add-Content C:\topaz\movebench.log "no $Process window"; exit }
$h = $p.MainWindowHandle; [W]::ShowWindow($h, 9) | Out-Null
$r = New-Object W+RECT; [W]::GetWindowRect($h, [ref]$r) | Out-Null
$x0 = $r.L; $y0 = $r.T; $end = (Get-Date).AddSeconds($Seconds); $dx = 8; $x = $x0
while ((Get-Date) -lt $end) {
  $x += $dx; if ($x -gt $x0 + 300 -or $x -lt $x0 - 300) { $dx = -$dx }
  [W]::SetWindowPos($h, [IntPtr]::Zero, $x, $y0, 0, 0, 0x0015) | Out-Null   # NOSIZE|NOZORDER|NOACTIVATE
  Start-Sleep -Milliseconds 15
}
[W]::SetWindowPos($h, [IntPtr]::Zero, $x0, $y0, 0, 0, 0x0015) | Out-Null
