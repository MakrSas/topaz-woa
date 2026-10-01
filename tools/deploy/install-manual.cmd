@echo off
net session >nul 2>&1 || (powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'" & exit /b)
chcp 65001 >nul
rem Installs the test cert + the root-enumerated driver whose .inf sits next to this script.
cd /d "%~dp0"
for %%f in (*.inf) do set NAME=%%~nf
set L=%~dp0install.log
echo [%date% %time%] manual install %NAME% >> "%L%"
certutil -addstore -f Root topaz-woa-test.cer >> "%L%" 2>&1
certutil -addstore -f TrustedPublisher topaz-woa-test.cer >> "%L%" 2>&1
devcon.exe remove Root\%NAME% >> "%L%" 2>&1
devcon.exe install %NAME%.inf Root\%NAME% >> "%L%" 2>&1
echo devcon exit %errorlevel% >> "%L%"
type "%L%"
echo.
echo Driver log: C:\%NAME%.log
pause
