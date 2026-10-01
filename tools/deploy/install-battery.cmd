@echo off
net session >nul 2>&1 || (powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'" & exit /b)
chcp 65001 >nul
rem TopazBattery installer. Removing the old driver can drop the OTG boost that powers the hub
rem with this flash drive, so everything is copied to C: first and devcon runs from there.
set SRC=%~dp0
set STAGE=C:\topaz\stage\TopazBattery
set L=C:\topaz\stage\install-battery.log
if not exist C:\topaz\stage mkdir C:\topaz\stage
if exist "%STAGE%" rmdir /s /q "%STAGE%"
xcopy /y /q "%SRC%*" "%STAGE%\" >nul
cd /d "%STAGE%"
echo [%date% %time%] install TopazBattery from %SRC% >> "%L%"
certutil -addstore -f Root topaz-woa-test.cer >> "%L%" 2>&1
certutil -addstore -f TrustedPublisher topaz-woa-test.cer >> "%L%" 2>&1
devcon.exe remove Root\TopazBattery >> "%L%" 2>&1
devcon.exe install TopazBattery.inf Root\TopazBattery >> "%L%" 2>&1
echo devcon exit %errorlevel% >> "%L%"
type "%L%"
echo.
echo Waiting 30 s for the hub/flash drive, then copying logs next to this script...
timeout /t 30 /nobreak >nul
rem the driver keeps the log open for writing: plain copy/Notepad get a sharing violation
powershell -NoProfile -Command "$s=[IO.File]::Open('C:\TopazBattery.log','Open','Read','ReadWrite'); $d=[IO.File]::Create('%SRC%TopazBattery.log'); $s.CopyTo($d); $d.Close(); $s.Close()" && echo copied TopazBattery.log || echo flash drive not back: logs stay in C:\TopazBattery.log and %L%
copy /y "%L%" "%SRC%install-battery.log" >nul 2>&1
pause
