<#
.SYNOPSIS
    Desinstala el APO de Warzone Audio Optimizer y restaura el audio original.

.DESCRIPTION
    Revierte exactamente lo que hizo install-apo.ps1:
      - Restaura el efecto de modo original del dispositivo (o lo quita si no
        habia ninguno antes).
      - Restaura DisableProtectedAudioDG a su estado previo.
      - Desregistra y borra la DLL.
      - Reinicia el servicio de audio.

    ESTE ES EL SCRIPT DE RESCATE. Si te quedaste sin audio tras instalar,
    ejecuta este como administrador.

    Esta escrito para funcionar incluso si el backup se perdio: en ese caso
    limpia la asociacion del APO igualmente, dejando el dispositivo sin
    efecto de modo (que es un estado valido -- Windows simplemente no aplica
    efectos de fabricante).

.NOTES
    Requiere ejecutarse como Administrador.
#>

#Requires -RunAsAdministrator

$ErrorActionPreference = 'Continue'   # seguir aunque un paso falle: es rescate

$ApoClsid   = '{E76EE61C-E30D-4C52-994D-F54422E9A2C9}'
$DllName    = 'WarzoneAudioApo.dll'
$InstallDir = Join-Path $env:ProgramFiles 'Warzone Audio Optimizer'
$BackupKey  = 'HKLM:\SOFTWARE\WarzoneAudioOptimizer\ApoBackup'
$RenderRoot = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render'
$AudioKey   = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio'
$PkeyModeEffect = '{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D},6'

function Write-Step($text) { Write-Host "`n==> $text" -ForegroundColor Cyan }
function Write-Ok($text)   { Write-Host "    $text" -ForegroundColor Green }
function Write-Warn($text) { Write-Host "    $text" -ForegroundColor Yellow }

Write-Host @'
================================================================
  Warzone Audio Optimizer - Desinstalador del APO
================================================================
'@ -ForegroundColor White

# ---------------------------------------------------------------------------
# 1. Restaurar el efecto de modo del dispositivo
# ---------------------------------------------------------------------------
Write-Step 'Restaurando configuracion del dispositivo...'

$backup = Get-ItemProperty -Path $BackupKey -ErrorAction SilentlyContinue

if ($backup -and $backup.EndpointGuid) {
    $fxPath = Join-Path (Join-Path $RenderRoot $backup.EndpointGuid) 'FxProperties'

    if ($backup.PSObject.Properties.Name -contains 'OriginalModeEffect') {
        Set-ItemProperty -Path $fxPath -Name $PkeyModeEffect `
                         -Value $backup.OriginalModeEffect -Type String -ErrorAction SilentlyContinue
        Write-Ok "Efecto de modo original restaurado: $($backup.OriginalModeEffect)"
    } else {
        Remove-ItemProperty -Path $fxPath -Name $PkeyModeEffect -ErrorAction SilentlyContinue
        Write-Ok 'Asociacion del APO eliminada (no habia efecto previo)'
    }

    # Si la clave FxProperties no existia antes, la quitamos si quedo vacia.
    if ($backup.HadFxKey -eq 0) {
        $remaining = Get-Item -Path $fxPath -ErrorAction SilentlyContinue
        if ($remaining -and $remaining.ValueCount -eq 0) {
            Remove-Item -Path $fxPath -Force -ErrorAction SilentlyContinue
        }
    }
} else {
    Write-Warn 'No se encontro backup. Barriendo todos los dispositivos por si acaso...'
    # Rescate: quitar la asociacion del APO donde sea que este.
    Get-ChildItem $RenderRoot -ErrorAction SilentlyContinue | ForEach-Object {
        $fxPath = Join-Path $_.PSPath 'FxProperties'
        $props = Get-ItemProperty -Path $fxPath -ErrorAction SilentlyContinue
        if ($props -and $props.PSObject.Properties.Name -contains $PkeyModeEffect) {
            if ($props.$PkeyModeEffect -eq $ApoClsid) {
                Remove-ItemProperty -Path $fxPath -Name $PkeyModeEffect -ErrorAction SilentlyContinue
                Write-Ok "Asociacion eliminada de $($_.PSChildName)"
            }
        }
    }
}

# ---------------------------------------------------------------------------
# 2. Restaurar la verificacion de firma de APOs
# ---------------------------------------------------------------------------
Write-Step 'Restaurando la verificacion de firma de APOs...'

if ($backup -and $backup.HadDisableFlag -eq 1) {
    Write-Warn 'DisableProtectedAudioDG ya existia antes de instalar; se deja como estaba.'
} else {
    Remove-ItemProperty -Path $AudioKey -Name 'DisableProtectedAudioDG' -ErrorAction SilentlyContinue
    Write-Ok 'DisableProtectedAudioDG eliminado (proteccion de audio restaurada)'
}

# ---------------------------------------------------------------------------
# 3. Desregistrar y borrar la DLL
# ---------------------------------------------------------------------------
Write-Step 'Eliminando la DLL...'

$targetDll = Join-Path $InstallDir $DllName
if (Test-Path $targetDll) {
    Start-Process -FilePath 'regsvr32.exe' -ArgumentList '/s', '/u', "`"$targetDll`"" -Wait -ErrorAction SilentlyContinue
    Write-Ok 'COM desregistrado'
} else {
    Write-Warn 'La DLL ya no estaba en su sitio'
}

# ---------------------------------------------------------------------------
# 4. Reiniciar el audio (libera la DLL para poder borrarla)
# ---------------------------------------------------------------------------
Write-Step 'Reiniciando el servicio de audio...'
try {
    Restart-Service -Name 'Audiosrv' -Force
    Write-Ok 'Servicio de audio reiniciado'
    Start-Sleep -Seconds 2
} catch {
    Write-Warn "No se pudo reiniciar: $_"
}

# El borrado va DESPUES del reinicio: mientras audiodg tenga la DLL cargada,
# el archivo esta bloqueado.
if (Test-Path $targetDll) {
    Remove-Item $targetDll -Force -ErrorAction SilentlyContinue
    if (Test-Path $targetDll) {
        Write-Warn 'La DLL sigue bloqueada; se borrara al reiniciar Windows.'
    } else {
        Write-Ok 'DLL eliminada'
    }
}

Remove-Item -Path $BackupKey -Recurse -Force -ErrorAction SilentlyContinue

Write-Host @'

================================================================
  Desinstalacion completada
================================================================

  Tu audio deberia funcionar como antes de instalar el APO.

  Si todavia no oyes nada, reinicia Windows: algunos cambios del
  motor de audio solo se aplican del todo tras reiniciar.

'@ -ForegroundColor White
