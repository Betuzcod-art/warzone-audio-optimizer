@echo off
title Instalar Warzone Audio Optimizer
setlocal

rem --- Pedir permisos de administrador si no los tiene --------------------
net session >nul 2>&1
if not %errorlevel%==0 (
    echo Pidiendo permisos de administrador...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b 0
)

set "APP_DIR=%ProgramFiles%\Warzone Audio Optimizer"
set "EAPO_CFG=%ProgramFiles%\EqualizerAPO\config"

cls
echo ==========================================================
echo   WARZONE AUDIO OPTIMIZER - INSTALACION
echo ==========================================================
echo.

rem --- 1. Comprobar que Equalizer APO esta instalado -----------------------
if not exist "%EAPO_CFG%\config.txt" (
    echo FALTA UN PASO PREVIO.
    echo.
    echo Esta app no procesa el audio por si sola: es el panel de control
    echo de Equalizer APO, que es el motor. Hay que instalarlo primero.
    echo.
    echo 1^) Se abrira la pagina de descarga de Equalizer APO.
    echo 2^) Descarga e instala la version de 64 bits.
    echo 3^) IMPORTANTE: cuando el instalador te muestre la lista de
    echo    dispositivos, MARCA LA CASILLA DE TUS AUDIFONOS.
    echo    Si no marcas nada, no se oira ningun cambio.
    echo 4^) Reinicia Windows.
    echo 5^) Vuelve a ejecutar este instalador.
    echo.
    pause
    start "" "https://sourceforge.net/projects/equalizerapo/files/latest/download"
    exit /b 1
)

echo [OK] Equalizer APO detectado.

rem --- 2. Copiar la aplicacion --------------------------------------------
if not exist "%APP_DIR%" mkdir "%APP_DIR%"
copy /Y "%~dp0WarzoneAudioOptimizer.exe" "%APP_DIR%\WarzoneAudioOptimizer.exe" >nul
if not %errorlevel%==0 (
    echo [ERROR] No se pudo copiar la aplicacion.
    pause
    exit /b 1
)
echo [OK] Aplicacion instalada.

rem --- 3. Permiso de escritura sobre la config de Equalizer APO ------------
rem  La app reescribe config.txt cada vez que mueves un slider. Sin esto
rem  tendria que pedir permisos de administrador en cada cambio.
rem  Se usa el SID S-1-5-32-545 en vez del nombre "Usuarios" porque el
rem  nombre cambia segun el idioma de Windows y falla en sistemas en ingles.
icacls "%EAPO_CFG%" /grant *S-1-5-32-545:(OI)(CI)M /T >nul 2>&1
if not %errorlevel%==0 (
    echo [AVISO] No se pudo dar permiso de escritura a la carpeta de
    echo         Equalizer APO. La app quizas no pueda guardar cambios.
) else (
    echo [OK] Permisos de configuracion concedidos.
)

rem --- 4. Copiar el script de rescate y crear accesos directos ------------
copy /Y "%~dp0ARREGLAR AUDIO.cmd" "%APP_DIR%\ARREGLAR AUDIO.cmd" >nul 2>&1
copy /Y "%~dp0DESINSTALAR.cmd" "%APP_DIR%\DESINSTALAR.cmd" >nul 2>&1

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
echo [OK] Acceso directo creado en el escritorio.

rem --- 5. Registrar para "Aplicaciones y caracteristicas" ------------------
set "UNKEY=HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\WarzoneAudioOptimizer"
reg add "%UNKEY%" /v DisplayName /t REG_SZ /d "Warzone Audio Optimizer" /f >nul
reg add "%UNKEY%" /v DisplayVersion /t REG_SZ /d "1.1.0" /f >nul
reg add "%UNKEY%" /v UninstallString /t REG_SZ /d "\"%APP_DIR%\DESINSTALAR.cmd\"" /f >nul
reg add "%UNKEY%" /v InstallLocation /t REG_SZ /d "%APP_DIR%" /f >nul

echo.
echo ==========================================================
echo   LISTO
echo ==========================================================
echo.
echo Abre "Warzone Audio Optimizer" desde el escritorio y mueve
echo los sliders: los cambios se oyen al instante, sin reiniciar.
echo.
choice /C SN /N /M "Abrir la aplicacion ahora? [S/N] "
if errorlevel 2 goto fin
start "" "%APP_DIR%\WarzoneAudioOptimizer.exe"
:fin
exit /b 0
