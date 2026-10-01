@echo off
net session >nul 2>&1 || (powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'" & exit /b)
chcp 65001 >nul
rem TopazModem: copies the modem firmware (fw\ next to this script) to C:\topaz\fw, arms one
rem modem boot (C:\topaz\modem.arm, the driver deletes it when it starts) and installs the driver.
rem The modem must be cold: boot Windows WITHOUT running "Modem test" in the UEFI menu.
cd /d "%~dp0"
set L=%~dp0install.log
echo [%date% %time%] install TopazModem >> "%L%"
if exist fw\image\modem.mdt (
  echo copying firmware to C:\topaz\fw ...
  xcopy /e /i /y /q fw C:\topaz\fw >> "%L%" 2>&1
) else (
  echo no fw\ next to the script, keeping C:\topaz\fw >> "%L%"
)
if not exist C:\topaz\fw\image\modem.mdt (
  echo C:\topaz\fw\image\modem.mdt missing! >> "%L%"
  type "%L%"
  pause
  exit /b 1
)
echo armed > C:\topaz\modem.arm
certutil -addstore -f Root topaz-woa-test.cer >> "%L%" 2>&1
certutil -addstore -f TrustedPublisher topaz-woa-test.cer >> "%L%" 2>&1
devcon.exe remove Root\TopazModem >> "%L%" 2>&1
devcon.exe install TopazModem.inf Root\TopazModem >> "%L%" 2>&1
echo devcon exit %errorlevel% >> "%L%"
type "%L%"
echo.
echo Wait ~2 minutes, then copy C:\TopazModem.log to the flash drive (copy-log.cmd).
pause
