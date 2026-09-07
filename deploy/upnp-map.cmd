@echo off
REM Forwards the server's ports on the router to this machine via UPnP.
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0upnp-map.ps1"
pause
