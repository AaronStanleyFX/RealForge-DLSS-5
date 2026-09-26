@echo off
setlocal EnableExtensions
cd /d "%~dp0"
title RealForge DLSS 5
set "LOG=%~dp0start.log"
> "%LOG%" echo [%date% %time%] Demarrage de RealForge DLSS 5 depuis %~dp0

echo.
echo   ==============================================
echo              RealForge  DLSS 5
echo   ==============================================
echo.

rem ---------------------------------------------------------------
rem  1) Python : local (dossier python\), sinon Python du systeme,
rem     sinon telechargement automatique de Python portable (~11 Mo)
rem ---------------------------------------------------------------
set "PYVER=3.12.10"
set "PYDIR=%~dp0python"
set "PY="
if exist "%PYDIR%\python.exe" set "PY="%PYDIR%\python.exe"" & goto :have_py
py -3 -c "import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)" >nul 2>nul && set "PY=py -3" && goto :have_py
python -c "import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)" >nul 2>nul && set "PY=python" && goto :have_py

echo [1/4] Python introuvable : telechargement de Python %PYVER% portable...
>> "%LOG%" echo [%time%] Python absent, telechargement
set "PYZIP=%TEMP%\realforge-python-%PYVER%.zip"
curl -fL --progress-bar -o "%PYZIP%" "https://www.python.org/ftp/python/%PYVER%/python-%PYVER%-embed-amd64.zip"
if errorlevel 1 goto :py_fail
mkdir "%PYDIR%" 2>nul
tar -xf "%PYZIP%" -C "%PYDIR%" 2>nul
if not exist "%PYDIR%\python.exe" powershell -NoProfile -Command "Expand-Archive -Force -LiteralPath '%PYZIP%' -DestinationPath '%PYDIR%'"
del "%PYZIP%" 2>nul
if not exist "%PYDIR%\python.exe" goto :py_fail
set "PY="%PYDIR%\python.exe""
:have_py
echo [1/4] Python : OK
>> "%LOG%" echo [%time%] Python : %PY%

rem ---------------------------------------------------------------
rem  2) ffmpeg (videos) : local, systeme, ancien dossier, sinon telechargement
rem ---------------------------------------------------------------
set "FFDIR=%~dp0ffmpeg\bin"
if exist "%FFDIR%\ffprobe.exe" goto :have_ff
ffmpeg -version >nul 2>nul && ffprobe -version >nul 2>nul && goto :have_ff
if not exist "%~dp0..\DLSS5-Studio\ffmpeg\bin\ffprobe.exe" goto :ff_download
mkdir "%FFDIR%" 2>nul
copy /y "%~dp0..\DLSS5-Studio\ffmpeg\bin\ff*.exe" "%FFDIR%\" >nul
if exist "%FFDIR%\ffprobe.exe" goto :have_ff

:ff_download
echo [2/4] ffmpeg introuvable : telechargement (~100 Mo, une seule fois)...
>> "%LOG%" echo [%time%] ffmpeg absent, telechargement
set "FFZIP=%TEMP%\realforge-ffmpeg.zip"
set "FFTMP=%TEMP%\realforge-ffmpeg"
curl -fL --progress-bar -o "%FFZIP%" "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip"
if errorlevel 1 curl -fL --progress-bar -o "%FFZIP%" "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip"
if errorlevel 1 goto :ff_fail
rmdir /s /q "%FFTMP%" 2>nul
mkdir "%FFTMP%"
tar -xf "%FFZIP%" -C "%FFTMP%" 2>nul
if errorlevel 1 powershell -NoProfile -Command "Expand-Archive -Force -LiteralPath '%FFZIP%' -DestinationPath '%FFTMP%'"
mkdir "%FFDIR%" 2>nul
for /r "%FFTMP%" %%F in (ffmpeg.exe ffprobe.exe) do if exist "%%F" copy /y "%%F" "%FFDIR%\" >nul
del "%FFZIP%" 2>nul
rmdir /s /q "%FFTMP%" 2>nul
if exist "%FFDIR%\ffprobe.exe" goto :have_ff
:ff_fail
echo [2/4] ATTENTION : ffmpeg indisponible. Les images fonctionnent, pas les videos.
>> "%LOG%" echo [%time%] ffmpeg : echec du telechargement
goto :ff_done
:have_ff
echo [2/4] ffmpeg : OK
>> "%LOG%" echo [%time%] ffmpeg : OK
:ff_done

rem ---------------------------------------------------------------
rem  3) Indicateur DLSS de NVIDIA (texte incruste sur les rendus)
rem ---------------------------------------------------------------
set "IND="
for /f "tokens=3" %%v in ('reg query "HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore" /v ShowDlssIndicator 2^>nul ^| find "ShowDlssIndicator"') do set "IND=%%v"
if not defined IND goto :ind_ok
if /i "%IND%"=="0x0" goto :ind_ok
echo.
echo L'indicateur DLSS de NVIDIA est active : il ecrirait du texte sur vos rendus.
choice /c ON /t 15 /d O /m "Le desactiver maintenant (autorisation administrateur Windows) ? O=oui N=non"
if errorlevel 2 goto :ind_ok
powershell -NoProfile -Command "Start-Process reg.exe -Verb RunAs -Wait -WindowStyle Hidden -ArgumentList 'add \"HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore\" /v ShowDlssIndicator /t REG_DWORD /d 0 /f'"
>> "%LOG%" echo [%time%] indicateur DLSS : desactivation demandee
:ind_ok

rem ---------------------------------------------------------------
rem  4) Moteur : compile au premier lancement, recompile s'il a change
rem ---------------------------------------------------------------
set "DLSS5_NOPAUSE=1"
if not exist "engine\realforge_engine.exe" goto :build_engine
powershell -NoProfile -Command "if ((Get-Item 'src\realforge_engine.cpp').LastWriteTime -gt (Get-Item 'engine\realforge_engine.exe').LastWriteTime) { exit 1 } else { exit 0 }"
if errorlevel 1 goto :build_engine
goto :engine_ok

:build_engine
echo [3/4] Compilation du moteur DLSS 5...
>> "%LOG%" echo [%time%] compilation du moteur
call build.bat
cd /d "%~dp0"
if exist "engine\realforge_engine.exe" goto :engine_ok
echo [3/4] ERREUR : le moteur n'a pas pu etre compile (voir les messages ci-dessus).
>> "%LOG%" echo [%time%] ERREUR compilation
echo L'interface va quand meme s'ouvrir.
goto :start_server
:engine_ok
echo [3/4] Moteur : OK
>> "%LOG%" echo [%time%] moteur : OK

rem ---------------------------------------------------------------
rem  5) Interface web : le serveur ouvre le navigateur automatiquement
rem ---------------------------------------------------------------
:start_server
echo [4/4] Ouverture de l'interface dans votre navigateur...
echo       (gardez cette fenetre ouverte pendant l'utilisation)
echo.
>> "%LOG%" echo [%time%] lancement du serveur : %PY% server.py
%PY% server.py
>> "%LOG%" echo [%time%] serveur arrete (code %errorlevel%)
echo.
echo Le serveur RealForge s'est arrete. Details : realforge.log et start.log
pause
exit /b 0

:py_fail
echo.
echo ERREUR : impossible de telecharger Python automatiquement.
echo Verifiez votre connexion Internet puis relancez start.bat.
>> "%LOG%" echo [%time%] ERREUR : Python introuvable et telechargement impossible
rmdir /s /q "%PYDIR%" 2>nul
pause
exit /b 1
