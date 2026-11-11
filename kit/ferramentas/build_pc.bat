@echo off
rem The PC test build of a port (host-pc): build_pc.bat <game folder> [target]
rem   build\<name>-pc\<name>_pc.exe <folder with default.xbe> [--docked]
setlocal
set "GAME=%~f1"
if "%GAME%"=="" (echo usage: build_pc.bat ^<game folder^> & exit /b 1)
set "TARGET=%~2"
for %%i in ("%~dp0..") do set "KIT=%%~fi"
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul
set "PATH=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%VSDIR%\VC\Tools\Llvm\x64\bin;%PATH%"
for %%i in ("%GAME%") do set "NAME=%%~nxi"
set "OUT=%KIT%\build\%NAME%-pc"
if not exist "%OUT%\build.ninja" (
  cmake -S "%KIT%\host-pc" -B "%OUT%" -G Ninja -DPORT_GAME_DIR="%GAME%" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ || exit /b 1
)
if "%TARGET%"=="" (cmake --build "%OUT%") else (cmake --build "%OUT%" --target %TARGET%)
