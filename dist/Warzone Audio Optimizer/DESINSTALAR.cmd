@echo off
title Desinstalar Warzone Audio Optimizer
setlocal

net session >nul 2>&1
if not %errorlevel%==0 (
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b 0
)

set "APP_DIR=%ProgramFiles%\Warzone Audio Optimizer"
set "EAPO_DIR=%ProgramFiles%\EqualizerAPO"
set "EAPO_CFG=%EAPO_DIR%\config"

cls
echo ==========================================================
echo   DESINSTALAR WARZONE AUDIO OPTIMIZER
echo ==========================================================
echo.

rem  Vaciar la configuracion ANTES de borrar nada. Si no se hace, los
rem  filtros seguirian aplicandose sin ninguna app que los controle y el
rem  usuario no sabria de donde sale el sonido alterado.
if exist "%EAPO_CFG%\config.txt" (
    echo # Configuracion vacia -- audio sin procesar.> "%EAPO_CFG%\config.txt"
    echo [OK] Filtros de audio retirados. El audio vuelve a la normalidad.
)

taskkill /IM WarzoneAudioOptimizer.exe /F >nul 2>&1
del /Q "%PUBLIC%\Desktop\Warzone Audio Optimizer.lnk" >nul 2>&1
del /Q "%ProgramData%\Microsoft\Windows\Start Menu\Programs\Warzone Audio Optimizer.lnk" >nul 2>&1
reg delete "HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\WarzoneAudioOptimizer" /f >nul 2>&1
rmdir /S /Q "%ProgramData%\WarzoneAudioOptimizer" >nul 2>&1
echo [OK] Aplicacion desinstalada.
echo.

if not exist "%EAPO_DIR%\Uninstall.exe" goto fin

echo ----------------------------------------------------------
echo Equalizer APO (el motor de audio) sigue instalado.
echo.
echo Puedes dejarlo: es un programa util por si solo y no
echo modifica el audio mientras su configuracion este vacia.
echo Quitarlo requiere reiniciar Windows.
echo ----------------------------------------------------------
echo.
choice /C SN /N /M "Desinstalar tambien Equalizer APO? [S/N] "
if errorlevel 2 goto fin
echo.
echo Desinstalando Equalizer APO...
start /wait "" "%EAPO_DIR%\Uninstall.exe" /S
echo [OK] Hecho. Reinicia Windows para completarlo.

:fin
echo.
start "" /D "%ProgramFiles%" cmd /c "timeout /t 2 >nul & rmdir /S /Q \"%APP_DIR%\""
echo Desinstalacion completada.
echo.
pause
