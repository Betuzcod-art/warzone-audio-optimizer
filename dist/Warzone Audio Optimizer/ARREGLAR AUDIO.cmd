@echo off
title Arreglar audio
setlocal

net session >nul 2>&1
if not %errorlevel%==0 (
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b 0
)

echo Reiniciando los servicios de audio de Windows...
net stop Audiosrv >nul 2>&1
net stop AudioEndpointBuilder >nul 2>&1
net start AudioEndpointBuilder >nul 2>&1
net start Audiosrv >nul 2>&1
echo.
sc query Audiosrv | find "RUNNING" >nul && echo [OK] Servicio de audio activo. || echo [AVISO] El servicio no arranco: reinicia Windows.
echo.
echo Prueba el sonido.
pause
