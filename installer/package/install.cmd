@echo off
setlocal
set "APP_DIR=%ProgramFiles%\Warzone Audio Optimizer"
set "PAYLOAD=%TEMP%\WarzoneAudioOptimizerPayload"

set "IS_REPAIR=0"
if exist "%APP_DIR%\WarzoneAudioOptimizer.exe" set "IS_REPAIR=1"

if exist "%PAYLOAD%" rmdir /S /Q "%PAYLOAD%"
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Expand-Archive -LiteralPath '%~dp0payload.zip' -DestinationPath '%PAYLOAD%' -Force"

if not exist "%APP_DIR%" mkdir "%APP_DIR%"
copy /Y "%PAYLOAD%\WarzoneAudioOptimizer.exe" "%APP_DIR%\WarzoneAudioOptimizer.exe" >nul

echo.
echo VB-CABLE es un driver de audio virtual donationware de VB-Audio.
echo Origen: www.vb-cable.com -- si te resulta util, considera donar en vb-audio.com
echo.

if "%IS_REPAIR%"=="1" (
    choice /C YN /N /M "Ya existe una instalacion previa. Reinstalar/reparar el driver VB-CABLE tambien? [Y/N] "
    if errorlevel 2 goto skip_vbcable
)

if exist "%PAYLOAD%\VBCABLE_Setup_x64.exe" (
    echo Instalando VB-CABLE. Completa el instalador del driver cuando te lo pida.
    start /wait "VB-CABLE Driver" "%PAYLOAD%\VBCABLE_Setup_x64.exe"
)
:skip_vbcable

powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$shell = New-Object -ComObject WScript.Shell; $desktop = [Environment]::GetFolderPath('CommonDesktopDirectory'); $shortcut = $shell.CreateShortcut((Join-Path $desktop 'Warzone Audio Optimizer.lnk')); $shortcut.TargetPath = '%APP_DIR%\WarzoneAudioOptimizer.exe'; $shortcut.WorkingDirectory = '%APP_DIR%'; $shortcut.Description = 'Warzone Audio Optimizer'; $shortcut.Save(); $start = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\Warzone Audio Optimizer.lnk'; $shortcut = $shell.CreateShortcut($start); $shortcut.TargetPath = '%APP_DIR%\WarzoneAudioOptimizer.exe'; $shortcut.WorkingDirectory = '%APP_DIR%'; $shortcut.Save()"

powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "New-ItemProperty -Path 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\WarzoneAudioOptimizer' -Name DisplayName -Value 'Warzone Audio Optimizer' -PropertyType String -Force | Out-Null; New-ItemProperty -Path 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\WarzoneAudioOptimizer' -Name UninstallString -Value '%APP_DIR%\uninstall.cmd' -PropertyType String -Force | Out-Null; New-ItemProperty -Path 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\WarzoneAudioOptimizer' -Name InstallLocation -Value '%APP_DIR%' -PropertyType String -Force | Out-Null"
copy /Y "%~dp0uninstall.cmd" "%APP_DIR%\uninstall.cmd" >nul

echo.
echo Installation complete. Restart Windows after installing VB-CABLE.
choice /C YN /N /M "Open the application now? [Y/N] "
if errorlevel 2 goto done
start "Warzone Audio Optimizer" "%APP_DIR%\WarzoneAudioOptimizer.exe"
:done
exit /b 0
