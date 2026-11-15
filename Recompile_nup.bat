@echo off
rem SwitchRecomp: drag a .nup file (a decrypted Switch 2 dump) onto this file to recompile it into the games folder.
rem The same as "Recompile a game" in SwitchRecomp.exe, with the whole log in this window.
title SwitchRecomp - Recompile a .nup
cd /d "%~dp0"
if "%~1"=="" (
  echo Drag the game's .nup file onto Recompile_nup.bat.
  echo.
  pause
  exit /b 1
)
if /i not "%~x1"==".nup" (
  echo That is not a .nup file: %~nx1
  echo.
  pause
  exit /b 1
)
if not exist "python\python.exe" call "%~dp0Install.bat"
cd /d "%~dp0"
if not exist "python\python.exe" (
  echo Python is missing. Run Install.bat once ^(needs internet^), then try again.
  pause
  exit /b 1
)
set "CC=%~dp0gcc\Library\bin\x86_64-w64-mingw32-gcc.exe"
if not exist "%CC%" set "CC=%~dp0tcc\tcc.exe"
set "PATH=%~dp0gcc\Library\bin;%~dp0tcc;%PATH%"
rem the games folder SwitchRecomp.exe uses (Settings > Games folder), else games\ here
set "GAMES=%~dp0games"
if exist "SwitchRecomp.ini" for /f "usebackq tokens=1,* delims==" %%a in ("SwitchRecomp.ini") do if /i "%%a"=="games_dir" if not "%%b"=="" set "GAMES=%%b"
if "%GAMES:~-1%"=="\" set "GAMES=%GAMES:~0,-1%"
if not exist "%GAMES%" mkdir "%GAMES%"
echo Games folder: %GAMES%
"python\python.exe" -u "switchrecomp.py" "%~f1" --target win64 --outdir "%GAMES%" --cc "%CC%" --jobs %NUMBER_OF_PROCESSORS%
if errorlevel 1 (
  echo.
  echo Recompiling failed. The lines above say why.
  pause
  exit /b 1
)
echo.
echo Done. Open SwitchRecomp.exe: the game is in the row ^(games folder^).
pause
