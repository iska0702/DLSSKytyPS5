@echo off
setlocal
set "KYTY_STREAM_ROOT=%~dp0.."
set "KYTY_PLAYROOM_GAME=D:\Eboot\PPSA01325"
if not exist "%KYTY_PLAYROOM_GAME%\eboot.bin" (
  if exist "D:\Eboot\PPSA01325.zar" (
    set "KYTY_PLAYROOM_GAME=D:\Eboot\PPSA01325.zar"
  ) else (
  echo ASTRO's PLAYROOM was not found at %KYTY_PLAYROOM_GAME%
  exit /b 1
  )
)
pushd "%KYTY_STREAM_ROOT%\build\launcher"
"%KYTY_STREAM_ROOT%\build\kyty_emulator.exe" --game "%KYTY_PLAYROOM_GAME%" --game-patch "%KYTY_STREAM_ROOT%\build\launcher\_Patches\PPSA01325.json" --directstorage --dlss --dlss-mode Quality --screen-width 2560 --screen-height 1440 --playgo-hack %*
set "KYTY_STREAM_RESULT=%ERRORLEVEL%"
popd
exit /b %KYTY_STREAM_RESULT%
