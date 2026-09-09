@echo off
REM Update a running sf4e lobby server to the files in this folder.
REM
REM Extract the new sf4e-server.zip OVER the folder the server already runs
REM from (say yes to replacing files), then run this. It asks for
REM administrator rights itself, refreshes the firewall rule for any ports
REM this version added, and stops the old LobbyServer.exe; run-server.cmd,
REM which is still looping, starts the new one within five seconds.

net session >nul 2>&1
if errorlevel 1 (
  echo Requesting administrator rights...
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)

cd /d "%~dp0"
echo.
echo [1/3] Windows Firewall
netsh advfirewall firewall delete rule name="sf4e lobby" >nul 2>&1
netsh advfirewall firewall add rule name="sf4e lobby" dir=in action=allow protocol=UDP localport=23400-23420,24001-24020,25001-25080 >nul
echo       allowed UDP 23400-23420, 24001-24020 and 25001-25080

echo [2/3] Restarting the server
tasklist /fi "IMAGENAME eq LobbyServer.exe" | find /i "LobbyServer.exe" >nul
if errorlevel 1 (
  echo       it was not running; starting it
  schtasks /run /tn "sf4e lobby server" >nul 2>&1 || start "" run-server.cmd
) else (
  taskkill /f /im LobbyServer.exe >nul
  echo       stopped the old one; run-server.cmd restarts the new one in 5 seconds
)
timeout /t 8 >nul

echo [3/3] Checking
tasklist /fi "IMAGENAME eq LobbyServer.exe" | find /i "LobbyServer.exe" >nul
if errorlevel 1 (
  echo       WARNING: LobbyServer.exe is not running. Open run-server.cmd by hand to see why.
) else (
  echo       running.
)
echo.
echo If your router forwards specific ports rather than using DMZ, add
echo UDP 25001-25080 to it as well.
echo.
pause
