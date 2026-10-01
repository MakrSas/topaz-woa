@echo off
rem Copies C:\TopazModem.log next to this script (the flash drive).
copy /y C:\TopazModem.log "%~dp0TopazModem.log"
echo copied to %~dp0TopazModem.log
pause
