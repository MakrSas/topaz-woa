@echo off
rem Copies C:\TopazWifi.log next to this script (the flash drive).
copy /y C:\TopazWifi.log "%~dp0TopazWifi.log"
echo copied to %~dp0TopazWifi.log
pause
