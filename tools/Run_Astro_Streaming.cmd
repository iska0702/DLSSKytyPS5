@echo off
setlocal
set "KYTY_STREAM_ROOT=%~dp0.."
set "KYTY_ASTRO_GAME=D:\Eboot\PPSA21567"
if not exist "%KYTY_ASTRO_GAME%\eboot.bin" (
  echo ASTRO BOT was not found at %KYTY_ASTRO_GAME%
  exit /b 1
)
pushd "%KYTY_STREAM_ROOT%\build\launcher"
"%KYTY_STREAM_ROOT%\build\kyty_emulator.exe" --game "%KYTY_ASTRO_GAME%" --game-patch "%KYTY_STREAM_ROOT%\build\launcher\_Patches\PPSA21567.json" --directstorage --dlss --dlss-mode Quality --screen-width 2560 --screen-height 1440 --playgo-hack %*
set "KYTY_STREAM_RESULT=%ERRORLEVEL%"
popd
exit /b %KYTY_STREAM_RESULT%
