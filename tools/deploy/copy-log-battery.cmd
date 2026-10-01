@echo off
rem Copies C:\TopazBattery.log next to this script (the driver keeps it open: Notepad/copy can't read it).
powershell -NoProfile -Command "$s=[IO.File]::Open('C:\TopazBattery.log','Open','Read','ReadWrite'); $d=[IO.File]::Create('%~dp0TopazBattery.log'); $s.CopyTo($d); $d.Close(); $s.Close()" && echo copied to %~dp0TopazBattery.log
pause
