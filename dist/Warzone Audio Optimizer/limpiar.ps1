# =============================================================================
# Desinstala Equalizer APO y Warzone Audio Optimizer para partir de cero.
#
# Por que un script y no un comando suelto: la ruta de instalacion no
# siempre es la misma, y el desinstalador NSIS lanzado con /S se desprende
# del proceso padre, asi que "esperar a que termine" no funciona solo con
# -Wait. Se usa el parametro _?= de NSIS, que lo obliga a ejecutarse en
# sitio, y aun asi se comprueba el resultado antes de seguir.
# =============================================================================

$ErrorActionPreference = 'SilentlyContinue'

function Paso($t) { Write-Host "  $t" }

Write-Host ""
Write-Host "[1/4] Cerrando la aplicacion..."
Get-Process WarzoneAudioOptimizer, Editor, Configurator, DeviceSelector | Stop-Process -Force
Paso "[OK]"

Write-Host ""
Write-Host "[2/4] Buscando Equalizer APO..."
$entrada = Get-ItemProperty `
    'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
    'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*' |
    Where-Object { $_.DisplayName -like '*Equalizer APO*' } | Select-Object -First 1

if (-not $entrada) {
    Paso "No estaba instalado. Nada que quitar."
} else {
    $exe = ($entrada.UninstallString -replace '"', '').Trim()
    $dir = Split-Path $exe -Parent
    Paso "Encontrado: $($entrada.DisplayName) $($entrada.DisplayVersion)"
    Paso "Desinstalando..."

    # _?= obliga al desinstalador NSIS a no copiarse a temp, para que
    # -Wait espere de verdad a que termine.
    Start-Process $exe -ArgumentList "/S", "_?=$dir" -Wait
    Start-Sleep -Seconds 3

    if (Test-Path "$dir\EqualizerAPO.dll") {
        Paso "[AVISO] Quedan archivos. Se borran a mano."
    }
    Remove-Item $dir -Recurse -Force
    Remove-Item $entrada.PSPath -Recurse -Force

    if (Test-Path $dir) {
        Paso "[AVISO] No se pudo borrar la carpeta:"
        Paso "        $dir"
        Paso "        Borrala tu tras reiniciar."
    } else {
        Paso "[OK] Equalizer APO eliminado."
    }
}

Write-Host ""
Write-Host "[3/4] Quitando Warzone Audio Optimizer..."
$appDir = "$env:ProgramFiles\Warzone Audio Optimizer"
Remove-Item $appDir -Recurse -Force
Remove-Item "$env:ProgramData\WarzoneAudioOptimizer" -Recurse -Force
Remove-Item "$env:PUBLIC\Desktop\Warzone Audio Optimizer.lnk" -Force
Remove-Item "$env:ProgramData\Microsoft\Windows\Start Menu\Programs\Warzone Audio Optimizer.lnk" -Force
Remove-Item 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\WarzoneAudioOptimizer' -Recurse -Force
Paso "[OK]"

Write-Host ""
Write-Host "[4/4] Comprobando el audio de Windows..."
foreach ($s in 'AudioEndpointBuilder', 'Audiosrv') {
    $svc = Get-Service $s
    if ($svc.Status -ne 'Running') {
        Start-Service $s
        Start-Sleep -Seconds 2
        $svc = Get-Service $s
    }
    if ($svc.Status -eq 'Running') { Paso "[OK] $s activo" }
    else { Paso "[AVISO] $s parado -- reinicia Windows" }
}

Write-Host ""
Write-Host "=========================================================="
Write-Host "  LIMPIO. AHORA REINICIA WINDOWS."
Write-Host "=========================================================="
Write-Host ""
Write-Host "  El motor de audio no se descarga del todo hasta"
Write-Host "  reiniciar. Si instalas antes de reiniciar, la"
Write-Host "  instalacion nueva puede quedar a medias."
Write-Host ""
Write-Host "  Tras reiniciar: ejecuta INSTALAR.cmd"
Write-Host ""
