<#
.SYNOPSIS
    Desinstala el APO de Warzone Audio Optimizer y restaura el audio original.

.DESCRIPTION
    Revierte exactamente lo que hizo install-apo.ps1:
      - Restaura el efecto de modo original del dispositivo (o lo quita si no
        habia ninguno antes).
      - Reactiva los efectos del dispositivo si Windows los desactivo por
        fallos de carga del APO.
      - Restaura DisableProtectedAudioDG a su estado previo.
      - Desregistra y borra la DLL.
      - Recarga el motor de audio.

    ESTE ES EL SCRIPT DE RESCATE. Si te quedaste sin audio tras instalar,
    ejecuta este como administrador.

    Esta escrito para funcionar incluso si el backup se perdio: en ese caso
    barre TODOS los dispositivos quitando la asociacion del APO, porque
    dejarte sin audio por no encontrar un backup seria el peor resultado
    posible.

.NOTES
    Requiere ejecutarse como Administrador.
#>

#Requires -RunAsAdministrator

# Continue, no Stop: esto es rescate. Si un paso falla, los siguientes deben
# ejecutarse igual -- parar a la primera podria dejar el audio roto.
$ErrorActionPreference = 'Continue'

$ApoClsid   = '{E76EE61C-E30D-4C52-994D-F54422E9A2C9}'
$DllName    = 'WarzoneAudioApo.dll'
$InstallDir = Join-Path $env:ProgramFiles 'Warzone Audio Optimizer'
$BackupKey  = 'HKLM:\SOFTWARE\WarzoneAudioOptimizer\ApoBackup'
$RenderRoot = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render'
$AudioKey   = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio'

# Los tres slots donde el APO pudo instalarse. El instalador elige el que
# estuviera libre, asi que hay que revisarlos todos.
$PkeyStreamEffect   = '{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D},5'
$PkeyModeEffect     = '{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D},6'
$PkeyEndpointEffect = '{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D},7'
$AllSlots = @($PkeyStreamEffect, $PkeyModeEffect, $PkeyEndpointEffect)

$PkeyDisableSysFx = '{1DA5D803-D492-4EDD-8C23-E0C0FFEE7F0E},5'

function Write-Step($text) { Write-Host "`n==> $text" -ForegroundColor Cyan }
function Write-Ok($text)   { Write-Host "    $text" -ForegroundColor Green }
function Write-Warn($text) { Write-Host "    $text" -ForegroundColor Yellow }

# ---------------------------------------------------------------------------
# Acceso a FxProperties con el permiso MINIMO necesario.
#
# Estas claves pertenecen a SYSTEM y dan a Administradores solo "SetValue,
# ReadKey". Los cmdlets Set-ItemProperty / Remove-ItemProperty abren la clave
# pidiendo escritura completa (incluye CreateSubKey) y Windows lo deniega.
# Pedir exactamente SetValue -- que tambien cubre borrar valores -- funciona
# sin tocar ninguna ACL.
# ---------------------------------------------------------------------------
function Open-AudioKeyForWrite {
    param([string] $EndpointGuid, [string] $SubKeyName)
    $path = "SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\$EndpointGuid\$SubKeyName"
    try {
        return [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey(
            $path,
            [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,
            [System.Security.AccessControl.RegistryRights]::SetValue)
    } catch {
        return $null
    }
}

function Set-AudioRegistryValue {
    param([string] $EndpointGuid, [string] $SubKeyName, [string] $ValueName, $Value,
          [Microsoft.Win32.RegistryValueKind] $Kind = [Microsoft.Win32.RegistryValueKind]::String)
    $key = Open-AudioKeyForWrite $EndpointGuid $SubKeyName
    if (-not $key) { return $false }
    try { $key.SetValue($ValueName, $Value, $Kind); return $true }
    catch { return $false }
    finally { $key.Close() }
}

function Remove-AudioRegistryValue {
    param([string] $EndpointGuid, [string] $SubKeyName, [string] $ValueName)
    $key = Open-AudioKeyForWrite $EndpointGuid $SubKeyName
    if (-not $key) { return $false }
    try { $key.DeleteValue($ValueName, $false); return $true }
    catch { return $false }
    finally { $key.Close() }
}

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
$touchedPaths = @()

if ($backup -and $backup.EndpointGuid) {
    $endpointPath = Join-Path $RenderRoot $backup.EndpointGuid
    $fxPath = Join-Path $endpointPath 'FxProperties'
    $touchedPaths += $endpointPath

    # El instalador guardo en que slot entro. Si el backup es de una version
    # anterior y no lo tiene, caemos a MFX, que es donde instalaba entonces.
    $usedSlot = $PkeyModeEffect
    if ($backup.PSObject.Properties.Name -contains 'UsedSlotKey') {
        $usedSlot = $backup.UsedSlotKey
    }

    if ($backup.PSObject.Properties.Name -contains 'ReplacedValue') {
        # Habiamos pisado un efecto del fabricante: devolverlo a su sitio.
        if (Set-AudioRegistryValue $backup.EndpointGuid 'FxProperties' $usedSlot $backup.ReplacedValue) {
            Write-Ok "Efecto original del fabricante restaurado: $($backup.ReplacedValue)"
        } else {
            Write-Warn 'No se pudo restaurar el efecto original (permisos?)'
        }
    } else {
        # Entramos en un slot que estaba libre: basta con vaciarlo.
        if (Remove-AudioRegistryValue $backup.EndpointGuid 'FxProperties' $usedSlot) {
            Write-Ok 'Asociacion del APO eliminada (el slot estaba libre al instalar)'
        } else {
            Write-Warn 'No habia nada que eliminar en ese slot'
        }
    }

    # Si la clave FxProperties no existia antes, la quitamos si quedo vacia.
    if ($backup.HadFxKey -eq 0) {
        $remaining = Get-Item -Path $fxPath -ErrorAction SilentlyContinue
        if ($remaining -and $remaining.ValueCount -eq 0) {
            Remove-Item -Path $fxPath -Force -ErrorAction SilentlyContinue
        }
    }
} else {
    Write-Warn 'No se encontro backup. Barriendo todos los dispositivos...'
    # Sin backup no sabemos en que slot entramos, asi que revisamos los tres.
    # El filtro por CLSID es lo que garantiza no tocar efectos de terceros.
    Get-ChildItem $RenderRoot -ErrorAction SilentlyContinue | ForEach-Object {
        $fxPath = Join-Path $_.PSPath 'FxProperties'
        $props = Get-ItemProperty -Path $fxPath -ErrorAction SilentlyContinue
        if (-not $props) { return }
        foreach ($slot in $AllSlots) {
            if ($props.PSObject.Properties.Name -contains $slot) {
                if ($props.$slot -eq $ApoClsid) {
                    if (Remove-AudioRegistryValue $_.PSChildName 'FxProperties' $slot) {
                        Write-Ok "Asociacion eliminada de $($_.PSChildName)"
                        $script:touchedPaths += $_.PSPath
                    } else {
                        Write-Warn "No se pudo limpiar $($_.PSChildName) (permisos?)"
                    }
                }
            }
        }
    }
}

# ---------------------------------------------------------------------------
# 2. Reactivar los efectos si Windows los desactivo
# ---------------------------------------------------------------------------
# Cuando un APO falla al cargar repetidamente, Windows pone Disable_SysFx=1 y
# el dispositivo se queda SIN NINGUN efecto -- incluidos los del fabricante.
# Si no se limpia, el audio "funciona" pero sin los efectos que tenia antes,
# y es un sintoma dificil de diagnosticar despues.
Write-Step 'Comprobando si Windows desactivo los efectos del dispositivo...'

$reenabled = 0
Get-ChildItem $RenderRoot -ErrorAction SilentlyContinue | ForEach-Object {
    $props = Get-ItemProperty -Path (Join-Path $_.PSPath 'Properties') -ErrorAction SilentlyContinue
    if ($props -and $props.PSObject.Properties.Name -contains $PkeyDisableSysFx) {
        if ($props.$PkeyDisableSysFx -ne 0) {
            if (Set-AudioRegistryValue $_.PSChildName 'Properties' $PkeyDisableSysFx 0 ([Microsoft.Win32.RegistryValueKind]::DWord)) {
                $script:reenabled++
            }
        }
    }
}
if ($reenabled -gt 0) {
    Write-Ok "Efectos reactivados en $reenabled dispositivo(s)"
} else {
    Write-Ok 'Ningun dispositivo tenia los efectos desactivados'
}

# ---------------------------------------------------------------------------
# 3. Restaurar la verificacion de firma de APOs
# ---------------------------------------------------------------------------
Write-Step 'Restaurando la verificacion de firma de APOs...'

if ($backup -and $backup.HadDisableFlag -eq 1) {
    Write-Warn 'DisableProtectedAudioDG ya existia antes de instalar; se deja como estaba.'
} else {
    Remove-ItemProperty -Path $AudioKey -Name 'DisableProtectedAudioDG' -ErrorAction SilentlyContinue
    Write-Ok 'DisableProtectedAudioDG eliminado (proteccion de audio restaurada)'
}

# ---------------------------------------------------------------------------
# 4. Desregistrar la DLL
# ---------------------------------------------------------------------------
Write-Step 'Desregistrando la DLL...'

$targetDll = Join-Path $InstallDir $DllName
if (Test-Path $targetDll) {
    Start-Process -FilePath 'regsvr32.exe' -ArgumentList '/s', '/u', "`"$targetDll`"" -Wait -ErrorAction SilentlyContinue
    Write-Ok 'COM desregistrado'
} else {
    Write-Warn 'La DLL ya no estaba en su sitio'
    # Aun asi limpiamos el registro por si quedo huerfano.
    Remove-Item -Path "Registry::HKEY_CLASSES_ROOT\CLSID\$ApoClsid" -Recurse -Force -ErrorAction SilentlyContinue
}

# ---------------------------------------------------------------------------
# 5. Recargar el motor de audio
# ---------------------------------------------------------------------------
Write-Step 'Recargando el motor de audio...'
try {
    Restart-Service -Name 'AudioEndpointBuilder' -Force
    Write-Ok 'Motor de audio reiniciado'
    Start-Sleep -Seconds 3
} catch {
    Write-Warn "No se pudo reiniciar: $($_.Exception.Message)"
    Write-Warn 'Reinicia Windows para completar la desinstalacion.'
}

# El borrado va DESPUES del reinicio: mientras audiodg tenga la DLL cargada,
# el archivo esta bloqueado y no se puede eliminar.
if (Test-Path $targetDll) {
    Remove-Item $targetDll -Force -ErrorAction SilentlyContinue
    if (Test-Path $targetDll) {
        Write-Warn 'La DLL sigue bloqueada; se podra borrar tras reiniciar Windows.'
    } else {
        Write-Ok 'DLL eliminada'
    }
}

Remove-Item -Path $BackupKey -Recurse -Force -ErrorAction SilentlyContinue

# ---------------------------------------------------------------------------
# 6. Verificar que quedo limpio
# ---------------------------------------------------------------------------
Write-Step 'Verificando...'

$leftovers = 0
Get-ChildItem $RenderRoot -ErrorAction SilentlyContinue | ForEach-Object {
    $props = Get-ItemProperty -Path (Join-Path $_.PSPath 'FxProperties') -ErrorAction SilentlyContinue
    if (-not $props) { return }
    foreach ($slot in $AllSlots) {
        if ($props.PSObject.Properties.Name -contains $slot) {
            if ($props.$slot -eq $ApoClsid) { $script:leftovers++ }
        }
    }
}

if ($leftovers -eq 0) {
    Write-Ok 'Ningun dispositivo apunta ya al APO'
} else {
    Write-Warn "Quedan $leftovers referencias al APO. Vuelve a ejecutar este script."
}

$stillLoaded = $false
$audiodg = Get-Process -Name 'audiodg' -ErrorAction SilentlyContinue
if ($audiodg) {
    try {
        foreach ($m in $audiodg.Modules) {
            if ($m.ModuleName -eq $DllName) { $stillLoaded = $true; break }
        }
    } catch { }
}
if ($stillLoaded) {
    Write-Warn 'La DLL sigue cargada en memoria; se soltara al reiniciar Windows.'
} else {
    Write-Ok 'La DLL no esta cargada en el motor de audio'
}

Write-Host @'

================================================================
  Desinstalacion completada
================================================================

  Tu audio deberia funcionar como antes de instalar el APO.

  Si todavia no oyes nada:
    1. Reinicia Windows (algunos cambios del motor de audio solo
       se aplican del todo tras reiniciar).
    2. Comprueba en Configuracion > Sonido que el dispositivo de
       salida correcto sigue seleccionado.

'@ -ForegroundColor White
