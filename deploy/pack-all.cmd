@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
cd /d C:\Users\FABIPC\Documents\SF4Rollback\sf4e
cmake --preset default
if errorlevel 1 ( echo CONFIGURE_FAILED & exit /b 1 )
cmake --build msvc-build\default
if errorlevel 1 ( echo BUILD_FAILED & exit /b 1 )
cd msvc-build\default
cpack -G ZIP
if errorlevel 1 ( echo CPACK_FAILED & exit /b 1 )

REM Server package: the lobby server, its DLLs, the run script and the guide.
set SRV=sf4e-server
if exist %SRV% rmdir /s /q %SRV%
mkdir %SRV%
copy /y LobbyServer.exe %SRV%\ >nul
copy /y *.dll %SRV%\ >nul
copy /y ..\..\deploy\run-server.cmd %SRV%\ >nul
copy /y ..\..\SERVER.md %SRV%\ >nul
if exist sf4e-server.zip del sf4e-server.zip
powershell -NoProfile -Command "Compress-Archive -Path '%SRV%' -DestinationPath 'sf4e-server.zip' -Force"
echo PACK_ALL_EXIT=%ERRORLEVEL%
