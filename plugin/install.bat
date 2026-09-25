@echo off
setlocal
cd /d "%~dp0"

set "PLUGIN=%~dp0build\XLivp.usr"
if not exist "%PLUGIN%" (
  echo Building plugin first...
  call "%~dp0build.bat" || exit /b 1
)

set "DEST=D:\Program Files\XnViewMP\Plugins"
if not exist "%DEST%" mkdir "%DEST%"

copy /Y "%PLUGIN%" "%DEST%\XLivp.usr"
if errorlevel 1 (
  echo.
  echo Copy failed - try running this script as Administrator.
  exit /b 1
)

echo Installed to: %DEST%\XLivp.usr
echo Restart XnViewMP, then open a .livp file.
endlocal
