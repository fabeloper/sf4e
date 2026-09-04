@echo off
REM sf4e lobby server. Run this on the VPS as Administrator once; it opens
REM the firewall for the ports the server uses and then starts it.
REM
REM Ports (all UDP):
REM   23400        matchmaker (create / join by code)
REM   23401-23420  one session per lobby
REM   24001-24020  one GGPO relay per lobby
REM
REM Your VPS provider's own firewall (security group) must allow the same
REM UDP ranges. That is separate from Windows Firewall.

cd /d "%~dp0"

netsh advfirewall firewall show rule name="sf4e lobby" >nul 2>&1
if errorlevel 1 (
  echo Opening Windows Firewall for sf4e...
  netsh advfirewall firewall add rule name="sf4e lobby" dir=in action=allow protocol=UDP localport=23400-23420,24001-24020
)

:run
echo Starting sf4e lobby server. Close this window to stop it.
LobbyServer.exe
echo Server exited with code %ERRORLEVEL%. Restarting in 5 seconds...
timeout /t 5 >nul
goto run
