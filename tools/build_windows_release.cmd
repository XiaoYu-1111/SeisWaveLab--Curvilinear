@echo off
setlocal
set "SEIS_SOURCE=%~dp0.."
set "SEIS_BUILD=%~dp0..\..\out\build\x64-Release"
set "SEIS_SDK=%~dp0..\..\dependencies\windows"
if not "%~2"=="" set "SEIS_SDK=%~2"
for %%i in ("%SEIS_SOURCE%") do set "SEIS_SOURCE=%%~fi"
for %%i in ("%SEIS_BUILD%") do set "SEIS_BUILD=%%~fi"
for %%i in ("%SEIS_SDK%") do set "SEIS_SDK=%%~fi"
if not "%SEIS_BUILD:~125,1%"=="" (
  echo Build path is too long for reliable MSVC compilation. Extract the Source ZIP to a shorter path, such as C:\SeisWave.
  exit /b 5
)
set "SEIS_CUDA=OFF"
if /I "%~1"=="cuda" set "SEIS_CUDA=ON"
if not "%~1"=="" if /I not "%~1"=="cpu" if /I not "%~1"=="cuda" (
  echo Usage: build_windows_release.cmd [cpu^|cuda] [path-to-windows-SDK-folder]
  exit /b 2
)
if not exist "%SEIS_SDK%\glfw\include\GLFW\glfw3.h" (
  echo Missing GLFW SDK. Use the complete Source ZIP, or supply an SDK folder containing glfw and glew.
  exit /b 3
)
if not exist "%SEIS_SDK%\glew\include\GL\glew.h" (
  echo Missing GLEW SDK in "%SEIS_SDK%".
  exit /b 3
)
set "SEIS_VSROOT="
set "SEIS_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%SEIS_VSWHERE%" (
  echo Install Visual Studio 2022 / Build Tools with Desktop development with C++ and CMake tools.
  exit /b 4
)
if not exist "%SEIS_BUILD%" mkdir "%SEIS_BUILD%"
"%SEIS_VSWHERE%" -latest -version "[17.0,18.0)" -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%SEIS_BUILD%\vs-installation.txt"
set /p SEIS_VSROOT=<"%SEIS_BUILD%\vs-installation.txt"
if not defined SEIS_VSROOT (
  echo No supported x64 C++ build tools found.
  exit /b 4
)
call "%SEIS_VSROOT%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64
if errorlevel 1 exit /b %errorlevel%
set "PATH=%SEIS_VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%SEIS_VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cmake -S "%SEIS_SOURCE%" -B "%SEIS_BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE= -DSEIS3D_BUILD_GUI=ON -DSEIS3D_BUILD_VALIDATION=ON -DSEIS3D_ENABLE_CUDA=%SEIS_CUDA% "-DSEIS3D_GLFW_ROOT=%SEIS_SDK%/glfw" "-DSEIS3D_GLEW_ROOT=%SEIS_SDK%/glew"
if errorlevel 1 exit /b %errorlevel%
cmake --build "%SEIS_BUILD%" --config Release --parallel 4 --target SeisWaveLab_Curvilinear SeisWaveLab_MASW
if errorlevel 1 exit /b %errorlevel%
echo Build complete. Executable: "%SEIS_SOURCE%\..\out\bin\Release\SeisWaveLab_Curvilinear.exe"
exit /b 0
