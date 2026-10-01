@echo off
net session >nul 2>&1 || (powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'" & exit /b)
chcp 65001 >nul
rem TopazBattery, step 1 (runs from the flash drive): ONLY copies the package to C: and puts
rem TopazBattery-install.cmd on the desktop. Nothing is installed or removed here, so losing the
rem flash drive (the old driver drops the OTG boost that powers the hub) cannot break anything.
set STAGE=C:\topaz\stage\TopazBattery
if not exist "%STAGE%" mkdir "%STAGE%"
xcopy /y /q "%~dp0*" "%STAGE%\" >nul
if errorlevel 1 (
  echo COPY TO %STAGE% FAILED - nothing was changed.
  pause
  exit /b 1
)
copy /y "%STAGE%\install-local.cmd" "C:\Users\Public\Desktop\TopazBattery-install.cmd" >nul
echo Copied to %STAGE%:
dir /b "%STAGE%"
echo.
echo Desktop: TopazBattery-install.cmd (installs from C:, works without the flash drive).
echo Starting it now...
start "TopazBattery install" cmd /c ""%STAGE%\install-local.cmd" "%~dp0.""
exit /b 0
