@echo off
cd /d "%~dp0"
set "ASTRO_GAME=%~1"
if not defined ASTRO_GAME set "ASTRO_GAME=%~dp0Games\PPSA01325"
if not exist "%ASTRO_GAME%\eboot.bin" if exist "D:\Programs\PPSA01325\eboot.bin" set "ASTRO_GAME=D:\Programs\PPSA01325"
if not exist "%ASTRO_GAME%\eboot.bin" (
  echo Usage: %~nx0 "path to Astro Playroom game folder"
  pause
  exit /b 2
)
if not exist "_TempData" mkdir "_TempData"
kyty_emulator.exe --game "%ASTRO_GAME%" --screen-width 2560 --screen-height 1440 --dlss --dlss-mfg 4x --dlss-sharpness 0.35 --directstorage --playgo-hack > "_TempData\Astro-DirectStorage-last-run.log" 2>&1
set "ASTRO_EXIT=%ERRORLEVEL%"
echo Exit code: %ASTRO_EXIT% >> "_TempData\Astro-DirectStorage-last-run.log"
if not "%ASTRO_EXIT%"=="0" pause
exit /b %ASTRO_EXIT%
