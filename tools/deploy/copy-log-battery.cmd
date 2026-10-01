@echo off
rem Copies C:\TopazBattery.log (+ the install log) next to this script. The driver keeps the log
rem open for writing, so Notepad/copy get a sharing violation; .NET with ReadWrite sharing works.
powershell -NoProfile -Command "$s=[IO.File]::Open('C:\TopazBattery.log','Open','Read','ReadWrite'); $d=[IO.File]::Create('%~dp0TopazBattery.log'); $s.CopyTo($d); $d.Close(); $s.Close()" && echo copied to %~dp0TopazBattery.log
copy /y C:\topaz\stage\install-battery.log "%~dp0install-battery.log" >nul 2>&1
pause
