@echo off
setlocal
set "APP_DIR=%ProgramFiles%\Warzone Audio Optimizer"
taskkill /IM WarzoneAudioOptimizer.exe /F >nul 2>&1
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$shell = New-Object -ComObject WScript.Shell; Remove-Item ([Environment]::GetFolderPath('CommonDesktopDirectory') + '\Warzone Audio Optimizer.lnk') -Force -ErrorAction SilentlyContinue; Remove-Item ($env:ProgramData + '\Microsoft\Windows\Start Menu\Programs\Warzone Audio Optimizer.lnk') -Force -ErrorAction SilentlyContinue"
rmdir /S /Q "%APP_DIR%" 2>nul
reg delete "HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\WarzoneAudioOptimizer" /f >nul 2>&1
echo Warzone Audio Optimizer has been removed.
