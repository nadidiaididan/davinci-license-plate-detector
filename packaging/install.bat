@echo off
rem Installs PlateMask.ofx.bundle into the OpenFX plugin folder used by DaVinci Resolve Studio.
rem Right-click this file and choose "Run as administrator", then restart Resolve.
setlocal
set "SRC=%~dp0PlateMask.ofx.bundle"
set "DEST=%CommonProgramFiles%\OFX\Plugins"
if not exist "%SRC%" ( echo PlateMask.ofx.bundle not found next to this script & pause & exit /b 1 )
net session >nul 2>&1
if %errorlevel% neq 0 ( echo Please right-click install.bat and choose "Run as administrator". & pause & exit /b 1 )
if not exist "%DEST%" mkdir "%DEST%"
if exist "%DEST%\PlateMask.ofx.bundle" rmdir /s /q "%DEST%\PlateMask.ofx.bundle"
xcopy /e /i /q /y "%SRC%" "%DEST%\PlateMask.ofx.bundle" >nul
if %errorlevel% neq 0 ( echo Copy failed. & pause & exit /b 1 )
echo Installed to "%DEST%\PlateMask.ofx.bundle". Restart DaVinci Resolve Studio.
echo The effect is under OpenFX ^> Filters ^> PlateMask ^> License Plate Mask.
pause
