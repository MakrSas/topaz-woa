@echo off
net session >nul 2>&1 || (powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'" & exit /b)
rem Back to Microsoft Basic Display: enable it first, then remove TopazDisplay, then reboot.
cd /d "%~dp0"
reg add "HKLM\SYSTEM\CurrentControlSet\Enum\ROOT\BASICDISPLAY\0000" /v ConfigFlags /t REG_DWORD /d 0 /f >> "%~dp0uninstall.log" 2>&1
devcon.exe enable ROOT\BasicDisplay >> "%~dp0uninstall.log" 2>&1
devcon.exe remove Root\TopazDisplay >> "%~dp0uninstall.log" 2>&1
copy /y C:\TopazDisplay.log "%~dp0TopazDisplay.log" >nul 2>&1
shutdown /r /t 5
