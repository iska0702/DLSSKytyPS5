@echo off
rem Reuse the verified PLAYROOM game/patch paths; FSR takes precedence over DLSS.
call "%~dp0Run_Playroom_Streaming.cmd" --no-dlss --fsr-fg %*
exit /b %ERRORLEVEL%
