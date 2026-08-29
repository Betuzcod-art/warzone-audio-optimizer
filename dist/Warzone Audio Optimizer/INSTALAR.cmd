@echo off
title Instalar Warzone Audio Optimizer
setlocal enabledelayedexpansion

rem --- Pedir permisos de administrador si no los tiene --------------------
net session >nul 2>&1
if not %errorlevel%==0 (
    echo Pidiendo permisos de administrador...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b 0
)

set "APP_DIR=%ProgramFiles%\Warzone Audio Optimizer"
set "EAPO_DIR=%ProgramFiles%\EqualizerAPO"
set "EAPO_CFG=%EAPO_DIR%\config"
set "EAPO_SETUP=%~dp0EqualizerAPO-x64-1.4.2.exe"
set "NEEDS_REBOOT=0"

cls
echo ==========================================================
echo   WARZONE AUDIO OPTIMIZER
echo ==========================================================
echo.
echo Se instalaran dos cosas:
echo   1. Equalizer APO  - el motor que procesa el audio
echo   2. La aplicacion  - el panel para controlarlo
echo.
pause
cls

rem ===================================================================
rem  PASO 1: MOTOR DE AUDIO (Equalizer APO)
rem ===================================================================
echo [1/4] Motor de audio...
echo.

if exist "%EAPO_DIR%\EqualizerAPO.dll" (
    rem  Ya esta instalado: NO se reinstala. Reinstalar encima podria
    rem  borrar los dispositivos que el usuario ya tenga seleccionados
    rem  y dejarlo sin sonido procesado sin saber por que.
    echo       Equalizer APO ya estaba instalado. Se conserva tal cual.
    goto motor_listo
)

if not exist "%EAPO_SETUP%" (
    echo       [ERROR] Falta EqualizerAPO-x64-1.4.2.exe en esta carpeta.
    echo       Descomprime el ZIP entero antes de ejecutar el instalador,
    echo       no ejecutes este archivo desde dentro del ZIP.
    echo.
    pause
    exit /b 1
)

echo       Instalando Equalizer APO. Tarda un momento...
start /wait "" "%EAPO_SETUP%" /S
if not exist "%EAPO_DIR%\EqualizerAPO.dll" (
    echo       [ERROR] La instalacion no se completo.
    echo       Ejecuta EqualizerAPO-x64-1.4.2.exe a mano y sigue sus pasos.
    echo.
    pause
    exit /b 1
)
set "NEEDS_REBOOT=1"
echo       [OK] Motor instalado.

rem --- Seleccion de dispositivo: EL PASO QUE MAS FALLA ------------------
rem  La instalacion silenciosa se salta el dialogo de dispositivos. Sin
rem  un dispositivo marcado todo queda instalado y no se oye ningun
rem  cambio, que es el fallo mas comun y el mas confuso. Se lanza aqui
rem  el selector por separado, con la explicacion delante.
echo.
echo ==========================================================
echo   ATENCION - EL PASO MAS IMPORTANTE
echo ==========================================================
echo.
echo Se abrira una ventana con la lista de dispositivos de audio.
echo.
echo   MARCA LA CASILLA DE TUS AUDIFONOS y pulsa OK.
echo.
echo Si no marcas ninguno, todo quedara instalado pero NO se oira
echo ningun cambio.
echo.
echo Consejo: si no sabes cual es, mira cual aparece como
echo predeterminado en el icono de volumen de Windows.
echo.
pause
if exist "%EAPO_DIR%\DeviceSelector.exe" (
    start /wait "" "%EAPO_DIR%\DeviceSelector.exe"
) else (
    echo       [AVISO] No se encontro el selector de dispositivos.
    echo       Abrelo despues desde el menu inicio: "Configurator".
    pause
)

:motor_listo
cls

rem ===================================================================
rem  PASO 2: LA APLICACION
rem ===================================================================
echo [2/4] Instalando la aplicacion...
if not exist "%APP_DIR%" mkdir "%APP_DIR%"
copy /Y "%~dp0WarzoneAudioOptimizer.exe" "%APP_DIR%\WarzoneAudioOptimizer.exe" >nul
if not exist "%APP_DIR%\WarzoneAudioOptimizer.exe" (
    echo       [ERROR] No se pudo copiar la aplicacion.
    pause
    exit /b 1
)
copy /Y "%~dp0ARREGLAR AUDIO.cmd" "%APP_DIR%\" >nul 2>&1
copy /Y "%~dp0DESINSTALAR.cmd" "%APP_DIR%\" >nul 2>&1
copy /Y "%~dp0LICENCIAS.txt" "%APP_DIR%\" >nul 2>&1
echo       [OK] Aplicacion instalada.

rem ===================================================================
rem  PASO 3: PERMISOS
rem ===================================================================
echo [3/4] Permisos de configuracion...
rem  La app reescribe config.txt cada vez que se mueve un slider. Sin
rem  esto tendria que elevarse en cada cambio. Se usa el SID
rem  S-1-5-32-545 y no el nombre "Usuarios": el nombre cambia con el
rem  idioma de Windows y falla en sistemas en ingles.
icacls "%EAPO_CFG%" /grant *S-1-5-32-545:(OI)(CI)M /T >nul 2>&1
if errorlevel 1 (
    echo       [AVISO] La app quizas no pueda guardar cambios.
) else (
    echo       [OK] Concedidos.
)

rem ===================================================================
rem  PASO 4: ACCESOS DIRECTOS Y REGISTRO
rem ===================================================================
echo [4/4] Accesos directos...
powershell -NoProfile -Command ^
 "$s = New-Object -ComObject WScript.Shell;" ^
 "$d = [Environment]::GetFolderPath('CommonDesktopDirectory');" ^
 "$l = $s.CreateShortcut((Join-Path $d 'Warzone Audio Optimizer.lnk'));" ^
 "$l.TargetPath = '%APP_DIR%\WarzoneAudioOptimizer.exe';" ^
 "$l.WorkingDirectory = '%APP_DIR%'; $l.Save();" ^
 "$m = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\Warzone Audio Optimizer.lnk';" ^
 "$l = $s.CreateShortcut($m);" ^
 "$l.TargetPath = '%APP_DIR%\WarzoneAudioOptimizer.exe';" ^
 "$l.WorkingDirectory = '%APP_DIR%'; $l.Save()"
echo       [OK] Creados.

set "UNKEY=HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\WarzoneAudioOptimizer"
reg add "%UNKEY%" /v DisplayName /t REG_SZ /d "Warzone Audio Optimizer" /f >nul
reg add "%UNKEY%" /v DisplayVersion /t REG_SZ /d "1.1.0" /f >nul
reg add "%UNKEY%" /v UninstallString /t REG_SZ /d "\"%APP_DIR%\DESINSTALAR.cmd\"" /f >nul
reg add "%UNKEY%" /v InstallLocation /t REG_SZ /d "%APP_DIR%" /f >nul

echo.
echo ==========================================================
if "%NEEDS_REBOOT%"=="1" goto pedir_reinicio

echo   LISTO
echo ==========================================================
echo.
echo Abre "Warzone Audio Optimizer" desde el escritorio.
echo Los cambios se oyen al instante al mover un slider.
echo.
choice /C SN /N /M "Abrir la aplicacion ahora? [S/N] "
if errorlevel 2 exit /b 0
start "" "%APP_DIR%\WarzoneAudioOptimizer.exe"
exit /b 0

:pedir_reinicio
echo   INSTALADO - FALTA REINICIAR
echo ==========================================================
echo.
echo Windows tiene que reiniciarse para que el motor de audio
echo cargue. Hasta entonces NO se oira ningun cambio.
echo.
choice /C SN /N /M "Reiniciar Windows ahora? [S/N] "
if errorlevel 2 goto sin_reinicio
shutdown /r /t 5 /c "Reiniciando para activar el procesamiento de audio"
exit /b 0

:sin_reinicio
echo.
echo Recuerda reiniciar. Despues abre la app desde el escritorio.
echo.
pause
exit /b 0
