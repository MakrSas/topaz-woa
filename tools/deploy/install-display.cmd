@echo off
net session >nul 2>&1 || (powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'" & exit /b)
chcp 65001 >nul
rem TopazDisplay installer: install the adapter, and only if it is running disable BasicDisplay
rem (both would scan out of the same framebuffer). Undo: uninstall.cmd
cd /d "%~dp0"
set L=%~dp0install.log
echo [%date% %time%] install TopazDisplay >> "%L%"
certutil -addstore -f Root topaz-woa-test.cer >> "%L%" 2>&1
certutil -addstore -f TrustedPublisher topaz-woa-test.cer >> "%L%" 2>&1
devcon.exe remove Root\TopazDisplay >> "%L%" 2>&1
devcon.exe install TopazDisplay.inf Root\TopazDisplay >> "%L%" 2>&1
echo devcon install exit %errorlevel% >> "%L%"
timeout /t 3 >nul
devcon.exe status Root\TopazDisplay > "%TEMP%\tdstat.txt" 2>&1
type "%TEMP%\tdstat.txt" >> "%L%"
findstr /c:"Driver is running" "%TEMP%\tdstat.txt" >nul
if errorlevel 1 (
  echo TopazDisplay is NOT running - BasicDisplay left enabled >> "%L%"
) else (
  devcon.exe disable ROOT\BasicDisplay >> "%L%" 2>&1
  echo BasicDisplay disabled >> "%L%"
)
copy /y C:\TopazDisplay.log "%~dp0TopazDisplay.log" >nul 2>&1
type "%L%"
echo.
echo If the screen goes black: Win+R  %~dp0uninstall.cmd  Enter, then Alt+Y
pause
