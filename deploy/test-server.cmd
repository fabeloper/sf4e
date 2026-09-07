@echo off
REM Checks whether an sf4e lobby server answers. Run from any PC:
REM   test-server.cmd 192.168.1.50        (a machine on your network)
REM   test-server.cmd 81.35.60.78         (your public address, from outside)
if "%~1"=="" ( echo usage: test-server.cmd ADDRESS & exit /b 1 )
powershell -NoProfile -Command ^
  "$u = New-Object System.Net.Sockets.UdpClient; $u.Client.ReceiveTimeout = 3000;" ^
  "$b = [Text.Encoding]::ASCII.GetBytes('{\"op\":\"ping\"}'); $u.Send($b, $b.Length, '%~1', 23400) | Out-Null;" ^
  "$ep = New-Object System.Net.IPEndPoint([Net.IPAddress]::Any, 0);" ^
  "try { Write-Host ('%~1 answered: ' + [Text.Encoding]::ASCII.GetString($u.Receive([ref]$ep))) -ForegroundColor Green } catch { Write-Host '%~1: NO ANSWER (server down, or the ports are not reaching it)' -ForegroundColor Red }"
