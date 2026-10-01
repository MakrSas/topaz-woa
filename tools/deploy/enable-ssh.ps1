# Enables OpenSSH Server on the phone so the build host can install drivers, read logs and reboot
# without the flash-drive loop. Run through enable-ssh.cmd (elevated). Needs topaz_win.pub (the
# Mac's key) next to this script. Writes enable-ssh.log and phone-ip.txt next to it.
$ErrorActionPreference = 'Continue'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Start-Transcript -Path (Join-Path $here 'enable-ssh.log') -Append | Out-Null

# 1. sshd: the Windows optional component (Windows Update downloads it), else a bundled zip
$cap = Get-WindowsCapability -Online | Where-Object Name -like 'OpenSSH.Server*'
Write-Host "OpenSSH.Server capability: $($cap.Name) $($cap.State)"
if ($cap -and $cap.State -ne 'Installed') {
    try {
        Add-WindowsCapability -Online -Name $cap.Name -ErrorAction Stop | Out-Null
        Write-Host 'Add-WindowsCapability: ok'
    } catch {
        Write-Host "Add-WindowsCapability failed: $_"
    }
}
if (-not (Get-Service sshd -ErrorAction SilentlyContinue)) {
    $zip = Join-Path $here 'OpenSSH-ARM64.zip'
    if (Test-Path $zip) {
        Write-Host 'installing the bundled OpenSSH-ARM64.zip'
        Expand-Archive $zip -DestinationPath 'C:\Program Files' -Force
        & powershell -NoProfile -ExecutionPolicy Bypass -File 'C:\Program Files\OpenSSH-ARM64\install-sshd.ps1'
    }
}
if (-not (Get-Service sshd -ErrorAction SilentlyContinue)) {
    Write-Host 'sshd is not available (no capability, no zip) - stopping'
    Stop-Transcript | Out-Null
    exit 1
}

# 2. our key: admins use administrators_authorized_keys (Administrators + SYSTEM only), else the profile
$keyFile = Join-Path $here 'topaz_win.pub'
if (Test-Path $keyFile) {
    $key = (Get-Content $keyFile -Raw).Trim()
    New-Item -ItemType Directory -Force 'C:\ProgramData\ssh' | Out-Null
    $admin = 'C:\ProgramData\ssh\administrators_authorized_keys'
    Set-Content -Path $admin -Value $key -Encoding ascii
    icacls $admin /inheritance:r /grant '*S-1-5-32-544:F' /grant '*S-1-5-18:F' | Out-Null
    $u = Join-Path $env:USERPROFILE '.ssh'
    New-Item -ItemType Directory -Force $u | Out-Null
    Set-Content -Path (Join-Path $u 'authorized_keys') -Value $key -Encoding ascii
    Write-Host 'key installed'
} else {
    Write-Host "no $keyFile - key not installed"
}

# 3. PowerShell as the login shell
New-Item -Path 'HKLM:\SOFTWARE\OpenSSH' -Force | Out-Null
New-ItemProperty -Path 'HKLM:\SOFTWARE\OpenSSH' -Name DefaultShell -PropertyType String -Force `
    -Value 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe' | Out-Null

# 4. service on every boot + firewall (any profile: Windows may call the home network "Public")
Set-Service sshd -StartupType Automatic
Restart-Service sshd -ErrorAction SilentlyContinue
Start-Service sshd -ErrorAction SilentlyContinue
if (Get-NetFirewallRule -Name 'OpenSSH-Server-In-TCP' -ErrorAction SilentlyContinue) {
    Set-NetFirewallRule -Name 'OpenSSH-Server-In-TCP' -Enabled True -Profile Any
} else {
    New-NetFirewallRule -Name 'OpenSSH-Server-In-TCP' -DisplayName 'OpenSSH Server (sshd)' -Enabled True `
        -Direction Inbound -Protocol TCP -Action Allow -LocalPort 22 -Profile Any | Out-Null
}

# 5. where to find us
$ips = Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' }
$info = @("user=$env:USERNAME", "computer=$env:COMPUTERNAME") + ($ips | ForEach-Object { "ip=$($_.IPAddress) if=$($_.InterfaceAlias)" })
$info | Set-Content (Join-Path $here 'phone-ip.txt')
$info | ForEach-Object { Write-Host $_ }
Get-Service sshd | Format-List Name, Status, StartType
Stop-Transcript | Out-Null
