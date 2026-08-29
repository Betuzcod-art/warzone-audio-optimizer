@echo off
title Desinstalar Warzone Audio Optimizer
setlocal

net session >nul 2>&1
if not %errorlevel%==0 (
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b 0
)

set "APP_DIR=%ProgramFiles%\Warzone Audio Optimizer"
set "EAPO_CFG=%ProgramFiles%\EqualizerAPO\config"

echo Desinstalando Warzone Audio Optimizer...
echo.

rem Dejar la configuracion de Equalizer APO limpia para que el audio
rem vuelva a sonar sin procesar. Si no se hace, los filtros seguirian
rem aplicandose despues de quitar la app.
if exist "%EAPO_CFG%\config.txt" (
    echo # Configuracion vacia -- audio sin procesar.> "%EAPO_CFG%\config.txt"
    echo [OK] Filtros de audio retirados.
)

taskkill /IM WarzoneAudioOptimizer.exe /F >nul 2>&1

del /Q "%PUBLIC%\Desktop\Warzone Audio Optimizer.lnk" >nul 2>&1
del /Q "%ProgramData%\Microsoft\Windows\Start Menu\Programs\Warzone Audio Optimizer.lnk" >nul 2>&1
reg delete "HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\WarzoneAudioOptimizer" /f >nul 2>&1
rmdir /S /Q "%ProgramData%\WarzoneAudioOptimizer" >nul 2>&1

start "" /D "%ProgramFiles%" cmd /c "timeout /t 2 >nul & rmdir /S /Q \"%APP_DIR%\""

echo [OK] Desinstalado.
echo.
echo Equalizer APO NO se ha desinstalado. Si tampoco lo quieres,
echo quitalo desde Aplicaciones y caracteristicas de Windows.
echo.
pause
