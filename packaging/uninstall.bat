@echo off
rem Run as administrator.
set "DEST=%CommonProgramFiles%\OFX\Plugins\PlateMask.ofx.bundle"
if exist "%DEST%" ( rmdir /s /q "%DEST%" && echo Removed "%DEST%" ) else ( echo Not installed. )
pause
