@echo off
net session >nul 2>&1 || (powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'" & exit /b)
rem Removes the TopazDisplay adapter; Windows falls back to Microsoft Basic Display.
cd /d "%~dp0"
devcon.exe remove Root\TopazDisplay >> "%~dp0uninstall.log" 2>&1
shutdown /r /t 5
