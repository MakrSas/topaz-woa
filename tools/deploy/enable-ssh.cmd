@echo off
net session >nul 2>&1 || (powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'" & exit /b)
chcp 65001 >nul
rem Turns on the OpenSSH server (port 22, key login, PowerShell shell, starts with Windows) so the
rem build host can work with the phone over Wi-Fi. Needs topaz_win.pub and OpenSSH-ARM64.zip next to
rem it (without the zip it tries Windows Update). Writes enable-ssh.log and phone-ip.txt here.
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0enable-ssh.ps1"
echo.
echo Done. Bring back the flash drive (enable-ssh.log, phone-ip.txt).
pause
