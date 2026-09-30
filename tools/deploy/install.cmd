@echo off
set L=C:\topaz\install.log
if exist C:\topaz\installed.flag exit /b 0
echo [%date% %time%] install start >> %L%
certutil -addstore -f Root C:\topaz\drivers\TopazTouch\topaz-woa-test.cer >> %L% 2>&1
certutil -addstore -f TrustedPublisher C:\topaz\drivers\TopazTouch\topaz-woa-test.cer >> %L% 2>&1
C:\topaz\drivers\TopazTouch\devcon.exe install C:\topaz\drivers\TopazTouch\TopazTouch.inf Root\TopazTouch >> %L% 2>&1
echo devcon exit %errorlevel% >> %L%
echo ok > C:\topaz\installed.flag
echo [%date% %time%] install done >> %L%
