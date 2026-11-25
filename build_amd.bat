@echo off
rem Builds the opengl32.dll (Release, x64) into bin\AMD, for the AMD download. Same source as build.bat;
rem bin\x64 and bin\Win32 (and their obj folders) are not touched.
setlocal

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo vswhere.exe not found - is Visual Studio 2022 installed?
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set "MSBUILD=%%i"
if not defined MSBUILD (
    echo MSBuild not found.
    exit /b 1
)

rem (A path ending in a backslash right before a closing quote is read wrong: two of them.)
"%MSBUILD%" "%~dp0MXCamProxy.vcxproj" /m /nologo /v:minimal /p:Configuration=Release /p:Platform=x64 "/p:OutDir=%~dp0bin\AMD\\" "/p:IntDir=%~dp0obj\AMD\\" || exit /b 1

echo.
echo Built:
echo   AMD (64-bit): %~dp0bin\AMD\opengl32.dll
