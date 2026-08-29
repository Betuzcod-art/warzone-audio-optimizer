@echo off
title Limpiar instalacion previa
setlocal

net session >nul 2>&1
if not %errorlevel%==0 (
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b 0
)

if not exist "%~dp0limpiar.ps1" (
    echo [ERROR] Falta limpiar.ps1 junto a este archivo.
    echo Descomprime el ZIP entero antes de ejecutarlo.
    echo.
    pause
    exit /b 1
)

cls
echo ==========================================================
echo   BORRAR TODO Y EMPEZAR DE CERO
echo ==========================================================
echo.
echo Esto desinstala Equalizer APO y Warzone Audio Optimizer,
echo y borra su configuracion.
echo.
echo Usalo solo si algo quedo mal y quieres partir de cero.
echo Si todo te funciona, NO hace falta.
echo.
echo Tu audio volvera a la normalidad. Habra que REINICIAR.
echo.
choice /C SN /N /M "Continuar? [S/N] "
if errorlevel 2 exit /b 0
echo.

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0limpiar.ps1"
echo.
pause
