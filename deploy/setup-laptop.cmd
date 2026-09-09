@echo off
REM One-time setup for a Windows PC or laptop that will run the sf4e lobby
REM server permanently. Run it once from the sf4e-server folder; it asks for
REM administrator rights itself. It:
REM   1. opens Windows Firewall for the server's UDP ports
REM   2. stops the machine from sleeping or hibernating on mains power, and
REM      makes closing the lid do nothing
REM   3. registers a task that starts the server whenever you sign in, so a
REM      reboot (Windows Update, power cut) brings it back by itself
REM   4. starts the server now

net session >nul 2>&1
if errorlevel 1 (
  echo Requesting administrator rights...
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)

cd /d "%~dp0"
echo.
echo [1/4] Windows Firewall
netsh advfirewall firewall delete rule name="sf4e lobby" >nul 2>&1
netsh advfirewall firewall add rule name="sf4e lobby" dir=in action=allow protocol=UDP localport=23400-23420,24001-24020,25001-25080 >nul
echo       allowed UDP 23400-23420, 24001-24020 and 25001-25080

echo [2/4] Power: never sleep on mains, lid does nothing
powercfg /change standby-timeout-ac 0 >nul
powercfg /change hibernate-timeout-ac 0 >nul
powercfg /change monitor-timeout-ac 15 >nul
powercfg /setacvalueindex SCHEME_CURRENT SUB_BUTTONS LIDACTION 0 >nul
powercfg /setactive SCHEME_CURRENT >nul
echo       done. Keep the laptop plugged in.

echo [3/4] Start the server at every sign-in
schtasks /delete /tn "sf4e lobby server" /f >nul 2>&1
schtasks /create /tn "sf4e lobby server" /tr "\"%~dp0run-server.cmd\"" /sc onlogon /rl highest /f >nul
echo       task "sf4e lobby server" registered

echo [4/4] Starting the server now
schtasks /run /tn "sf4e lobby server" >nul
timeout /t 3 >nul
tasklist /fi "IMAGENAME eq LobbyServer.exe" | find /i "LobbyServer.exe" >nul
if errorlevel 1 (
  echo       WARNING: LobbyServer.exe is not running. Open run-server.cmd by hand to see why.
) else (
  echo       running.
)

echo.
echo Setup complete. This machine's addresses:
ipconfig | findstr /i "IPv4"
echo.
echo Next: make sure your router sends the UDP ports above to this machine
echo (see SERVER.md, "Running it at home"). If you also want the server to
echo survive a reboot without anyone signing in, turn on automatic sign-in:
echo   press Win+R, type netplwiz, untick "Users must enter a user name..."
echo.
pause
