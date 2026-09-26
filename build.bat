@echo off
setlocal EnableExtensions
cd /d "%~dp0"
title RealForge DLSS 5 - compilation du moteur

rem ---------------------------------------------------------------
rem  1) SDK NVIDIA DLSS (en-tetes + bibliotheque NGX) - telechargement unique
rem ---------------------------------------------------------------
set "SDK=%~dp0third_party\DLSS"
set "RAW=https://raw.githubusercontent.com/NVIDIA/DLSS/main"
if exist "%SDK%\lib\nvsdk_ngx_d.lib" goto :have_sdk
if exist "%~dp0..\DLSS5-Studio\third_party\DLSS\lib\nvsdk_ngx_d.lib" (
  xcopy /e /i /y /q "%~dp0..\DLSS5-Studio\third_party\DLSS" "%SDK%" >nul
  if exist "%SDK%\lib\nvsdk_ngx_d.lib" goto :have_sdk
)
echo Telechargement du SDK NVIDIA DLSS depuis github.com/NVIDIA/DLSS ...
mkdir "%SDK%\include" 2>nul
mkdir "%SDK%\lib" 2>nul
for %%F in (nvsdk_ngx.h nvsdk_ngx_defs.h nvsdk_ngx_params.h nvsdk_ngx_helpers.h nvsdk_ngx_helpers_d3d.h nvsdk_ngx_helpers_cuda.h) do (
  curl -fsSL -o "%SDK%\include\%%F" "%RAW%/include/%%F" || goto :dl_fail
)
curl -fsSL -o "%SDK%\lib\nvsdk_ngx_d.lib" "%RAW%/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib" || goto :dl_fail
:have_sdk

rem ---------------------------------------------------------------
rem  2) Compilateur Visual Studio (Build Tools 2019/2022 avec C++)
rem ---------------------------------------------------------------
where cl >nul 2>nul && goto :have_cl
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :no_vs
set "VSDIR="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto :no_vs
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || goto :no_vs
:have_cl

rem ---------------------------------------------------------------
rem  3) Compilation
rem ---------------------------------------------------------------
mkdir build 2>nul
mkdir engine 2>nul
echo Compilation de engine\realforge_engine.exe ...
cl /nologo /EHsc /O2 /MD /std:c++17 /utf-8 /W3 /DUNICODE /D_UNICODE /I"%SDK%\include" src\realforge_engine.cpp /Fo"build\\" /Fe"engine\realforge_engine.exe" /link /LIBPATH:"%SDK%\lib" nvsdk_ngx_d.lib
if errorlevel 1 goto :build_fail
echo.
echo OK : engine\realforge_engine.exe est pret.
if not defined DLSS5_NOPAUSE pause
exit /b 0

:dl_fail
echo.
echo ERREUR : telechargement du SDK impossible. Verifiez la connexion, ou placez manuellement
echo les fichiers include\*.h et lib\Windows_x86_64\x64\nvsdk_ngx_d.lib du depot NVIDIA/DLSS dans
echo   %SDK%\include  et  %SDK%\lib
rmdir /s /q "%SDK%" 2>nul
pause
exit /b 1

:no_vs
echo.
echo ERREUR : compilateur C++ Microsoft introuvable.
echo Installez "Build Tools for Visual Studio 2022" avec la charge de travail
echo "Developpement Desktop en C++", par exemple :
echo   winget install Microsoft.VisualStudio.2022.BuildTools --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
echo puis relancez build.bat.
pause
exit /b 1

:build_fail
echo.
echo ERREUR de compilation : copiez le message ci-dessus et envoyez-le moi.
pause
exit /b 1
