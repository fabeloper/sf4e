@echo off
copy /y ....deploysetup-laptop.cmd %SRV% >nul
copy /y ....deployupnp-map.cmd %SRV% >nul
copy /y ....deployupnp-map.ps1 %SRV% >nul
copy /y ....deploy	est-server.cmd %SRV% >nul
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
copy /y ....deploysetup-laptop.cmd %SRV% >nul
copy /y ....deployupnp-map.cmd %SRV% >nul
copy /y ....deployupnp-map.ps1 %SRV% >nul
copy /y ....deploy	est-server.cmd %SRV% >nul
cd /d C:\Users\FABIPC\Documents\SF4Rollback\sf4e
copy /y ....deploysetup-laptop.cmd %SRV% >nul
copy /y ....deployupnp-map.cmd %SRV% >nul
copy /y ....deployupnp-map.ps1 %SRV% >nul
copy /y ....deploy	est-server.cmd %SRV% >nul
cmake --preset default
copy /y ....deploysetup-laptop.cmd %SRV% >nul
copy /y ....deployupnp-map.cmd %SRV% >nul
copy /y ....deployupnp-map.ps1 %SRV% >nul
copy /y ....deploy	est-server.cmd %SRV% >nul
if errorlevel 1 ( echo CONFIGURE_FAILED & exit /b 1 )
copy /y ....deploysetup-laptop.cmd %SRV% >nul
copy /y ....deployupnp-map.cmd %SRV% >nul
copy /y ....deployupnp-map.ps1 %SRV% >nul
copy /y ....deploy	est-server.cmd %SRV% >nul
cmake --build msvc-build\default
copy /y ....deploysetup-laptop.cmd %SRV% >nul
copy /y ....deployupnp-map.cmd %SRV% >nul
copy /y ....deployupnp-map.ps1 %SRV% >nul
copy /y ....deploy	est-server.cmd %SRV% >nul
if errorlevel 1 ( echo BUILD_FAILED & exit /b 1 )
copy /y ....deploysetup-laptop.cmd %SRV% >nul
copy /y ....deployupnp-map.cmd %SRV% >nul
copy /y ....deployupnp-map.ps1 %SRV% >nul
copy /y ....deploy	est-server.cmd %SRV% >nul
cd msvc-build\default
copy /y ....deploysetup-laptop.cmd %SRV% >nul
copy /y ....deployupnp-map.cmd %SRV% >nul
copy /y ....deployupnp-map.ps1 %SRV% >nul
copy /y ....deploy	est-server.cmd %SRV% >nul
cpack -G ZIP
copy /y ....deploysetup-laptop.cmd %SRV% >nul
copy /y ....deployupnp-map.cmd %SRV% >nul
copy /y ....deployupnp-map.ps1 %SRV% >nul
copy /y ....deploy	est-server.cmd %SRV% >nul
if errorlevel 1 ( echo CPACK_FAILED & exit /b 1 )
copy /y ....deploysetup-laptop.cmd %SRV% >nul
copy /y ....deployupnp-map.cmd %SRV% >nul
copy /y ....deployupnp-map.ps1 %SRV% >nul
copy /y ....deploy	est-server.cmd %SRV% >nul

copy /y ....deploysetup-laptop.cmd %SRV% >nul
copy /y ....deployupnp-map.cmd %SRV% >nul
copy /y ....deployupnp-map.ps1 %SRV% >nul
copy /y ....deploy	est-server.cmd %SRV% >nul
REM Server package: the lobby server, its DLLs, the run script and the guide.
copy /y ....deploysetup-laptop.cmd %SRV% >nul
copy /y ....deployupnp-map.cmd %SRV% >nul
copy /y ....deployupnp-map.ps1 %SRV% >nul
copy /y ....deploy	est-server.cmd %SRV% >nul
set SRV=sf4e-server
copy /y ....deploysetup-laptop.cmd %SRV% >nul
copy /y ....deployupnp-map.cmd %SRV% >nul
copy /y ....deployupnp-map.ps1 %SRV% >nul
copy /y ....deploy	est-server.cmd %SRV% >nul
if exist %SRV% rmdir /s /q %SRV%
copy /y ....deploysetup-laptop.cmd %SRV% >nul
copy /y ....deployupnp-map.cmd %SRV% >nul
copy /y ....deployupnp-map.ps1 %SRV% >nul
copy /y ....deploy	est-server.cmd %SRV% >nul
mkdir %SRV%
copy /y ....deploysetup-laptop.cmd %SRV% >nul
copy /y ....deployupnp-map.cmd %SRV% >nul
copy /y ....deployupnp-map.ps1 %SRV% >nul
copy /y ....deploy	est-server.cmd %SRV% >nul
copy /y LobbyServer.exe %SRV%\ >nul
copy /y ....deploysetup-laptop.cmd %SRV% >nul
copy /y ....deployupnp-map.cmd %SRV% >nul
copy /y ....deployupnp-map.ps1 %SRV% >nul
copy /y ....deploy	est-server.cmd %SRV% >nul
copy /y *.dll %SRV%\ >nul
copy /y ....deploysetup-laptop.cmd %SRV% >nul
copy /y ....deployupnp-map.cmd %SRV% >nul
copy /y ....deployupnp-map.ps1 %SRV% >nul
copy /y ....deploy	est-server.cmd %SRV% >nul
copy /y ..\..\deploy\run-server.cmd %SRV%\ >nul
copy /y ....deploysetup-laptop.cmd %SRV% >nul
copy /y ....deployupnp-map.cmd %SRV% >nul
copy /y ....deployupnp-map.ps1 %SRV% >nul
copy /y ....deploy	est-server.cmd %SRV% >nul
copy /y ..\..\SERVER.md %SRV%\ >nul
copy /y ....deploysetup-laptop.cmd %SRV% >nul
copy /y ....deployupnp-map.cmd %SRV% >nul
copy /y ....deployupnp-map.ps1 %SRV% >nul
copy /y ....deploy	est-server.cmd %SRV% >nul
if exist sf4e-server.zip del sf4e-server.zip
copy /y ....deploysetup-laptop.cmd %SRV% >nul
copy /y ....deployupnp-map.cmd %SRV% >nul
copy /y ....deployupnp-map.ps1 %SRV% >nul
copy /y ....deploy	est-server.cmd %SRV% >nul
powershell -NoProfile -Command "Compress-Archive -Path '%SRV%' -DestinationPath 'sf4e-server.zip' -Force"
copy /y ....deploysetup-laptop.cmd %SRV% >nul
copy /y ....deployupnp-map.cmd %SRV% >nul
copy /y ....deployupnp-map.ps1 %SRV% >nul
copy /y ....deploy	est-server.cmd %SRV% >nul
echo PACK_ALL_EXIT=%ERRORLEVEL%
copy /y ....deploysetup-laptop.cmd %SRV% >nul
copy /y ....deployupnp-map.cmd %SRV% >nul
copy /y ....deployupnp-map.ps1 %SRV% >nul
copy /y ....deploy	est-server.cmd %SRV% >nul
