@echo off
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (
  echo Visual Studio C++ tools not found.
  exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1

if not exist miniz.c copy /Y "..\plugin\miniz.c" miniz.c >nul
if not exist miniz.h copy /Y "..\plugin\miniz.h" miniz.h >nul

if not exist build mkdir build

rc /nologo /fo build\LivpViewer.res LivpViewer.rc || exit /b 1

cl /nologo /O2 /GL /MT /EHsc /utf-8 /DUNICODE /D_UNICODE /DNDEBUG /W3 ^
  /I. /Fo"build\\" /Fe"build\LivpViewer.exe" ^
  LivpViewer.cpp miniz.c build\LivpViewer.res ^
  /link /LTCG /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF ^
  windowscodecs.lib ole32.lib mfplay.lib mfplat.lib mf.lib mfuuid.lib ^
  user32.lib gdi32.lib shell32.lib comdlg32.lib comctl32.lib msimg32.lib

if errorlevel 1 exit /b 1

copy /Y "build\LivpViewer.exe" "LivpViewer.exe" >nul
echo.
echo Built: %~dp0LivpViewer.exe
for %%A in (LivpViewer.exe) do echo Size: %%~zA bytes
endlocal
