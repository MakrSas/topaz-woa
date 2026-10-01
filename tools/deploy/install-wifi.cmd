@echo off
net session >nul 2>&1 || (powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'" & exit /b)
chcp 65001 >nul
rem TopazWifi: Wi-Fi adapter for Windows (WiFiCx). It boots the modem on every Windows start, so the
rem TopazModem device is removed first (never both). Run it right after a reboot (cold modem).
rem To keep the modem off: create C:\topaz\modem.off. Log: C:\TopazWifi.log (copy-log-wifi.cmd).
cd /d "%~dp0"
set L=%~dp0install.log
echo [%date% %time%] install TopazWifi >> "%L%"
if exist fw\image\modem.mdt (
  echo copying firmware to C:\topaz\fw ...
  xcopy /e /i /y /q fw C:\topaz\fw >> "%L%" 2>&1
)
if not exist C:\topaz\fw\image\modem.mdt (
  echo C:\topaz\fw\image\modem.mdt missing: run TopazModem\install.cmd once first >> "%L%"
  type "%L%"
  pause
  exit /b 1
)
devcon.exe remove Root\TopazModem >> "%L%" 2>&1
del /f /q C:\topaz\wifi.boot >nul 2>&1
del /f /q C:\topaz\modem.off >nul 2>&1
echo armed > C:\topaz\modem.arm
certutil -addstore -f Root topaz-woa-test.cer >> "%L%" 2>&1
certutil -addstore -f TrustedPublisher topaz-woa-test.cer >> "%L%" 2>&1
devcon.exe remove Root\TopazWifi >> "%L%" 2>&1
devcon.exe install TopazWifi.inf Root\TopazWifi >> "%L%" 2>&1
echo devcon exit %errorlevel% >> "%L%"
type "%L%"
echo.
echo Wait ~30 s, open Settings - Network - Wi-Fi, then run copy-log-wifi.cmd.
pause
