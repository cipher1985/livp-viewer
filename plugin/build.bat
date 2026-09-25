@echo off
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (
  echo Visual Studio with C++ tools not found.
  exit /b 1
)

call "%VS%\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1

if not exist build mkdir build
cl /nologo /O2 /LD /EHsc /utf-8 /DUNICODE /D_UNICODE ^
  /Fo"build\\" /Fe"build\XLivp.dll" ^
  XLivp.cpp miniz.c ^
  /link /DEF:XLivp.def windowscodecs.lib ole32.lib

if errorlevel 1 exit /b 1

copy /Y "build\XLivp.dll" "build\XLivp.usr" >nul
echo.
echo Built: %~dp0build\XLivp.usr
endlocal
