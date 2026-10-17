@echo off
rem Builds build\winncdu.exe (GUI) and build\winncdu-cli.exe (terminal), Release, using the Visual Studio C++ toolchain.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo Visual Studio with the "Desktop development with C++" workload is required.
  exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo No Visual Studio installation with C++ tools found.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cmake -S "%~dp0." -B "%~dp0build" -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build "%~dp0build" || exit /b 1
echo.
echo Built: %~dp0build\winncdu.exe      (windowed app)
echo        %~dp0build\winncdu-cli.exe  (terminal app)
