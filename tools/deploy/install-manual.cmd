@echo off
net session >nul 2>&1 || (powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'" & exit /b)
chcp 65001 >nul
rem Run as Administrator. Installs test cert + TopazTouch root device, log -> %~dp0install.log
cd /d "%~dp0"
set L=%~dp0install.log
echo [%date% %time%] manual install >> "%L%"
certutil -addstore -f Root topaz-woa-test.cer >> "%L%" 2>&1
certutil -addstore -f TrustedPublisher topaz-woa-test.cer >> "%L%" 2>&1
devcon.exe remove Root\TopazTouch >> "%L%" 2>&1
devcon.exe install TopazTouch.inf Root\TopazTouch >> "%L%" 2>&1
echo devcon exit %errorlevel% >> "%L%"
type "%L%"
echo.
echo Driver log: C:\TopazTouch.log
pause
