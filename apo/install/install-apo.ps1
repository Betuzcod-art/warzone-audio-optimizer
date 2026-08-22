<#
.SYNOPSIS
    Instala el APO de Warzone Audio Optimizer en un dispositivo de salida.

.DESCRIPTION
    Un APO (Audio Processing Object) procesa el audio DENTRO del motor de
    Windows, sin cable virtual ni dispositivo intermedio. Eso elimina la
    latencia que la version con VB-CABLE no podia evitar.

    ANTES DE EJECUTAR, ENTIENDE ESTO:

    1. Este script activa DisableProtectedAudioDG, que DESACTIVA la
       verificacion de firma de APOs en Windows. Es obligatorio para cargar
       un APO propio sin certificado WHQL (es lo mismo que hace Equalizer
       APO). Es un cambio real en una proteccion de seguridad del sistema.

    2. NO se toca Secure Boot ni el arranque, asi que el requisito de
       Secure Boot de Warzone se mantiene intacto.

    3. RIESGO CON ANTI-CHEAT: no hay forma de garantizar como reacciona un
       anti-cheat kernel (Ricochet) a esta configuracion. Asumes ese riesgo.

    4. Si algo sale mal y te quedas sin audio, ejecuta uninstall-apo.ps1.
       El script guarda los valores originales antes de tocarlos.

.NOTES
    Requiere ejecutarse como Administrador.
#>

#Requires -RunAsAdministrator

$ErrorActionPreference = 'Stop'

$ApoClsid   = '{E76EE61C-E30D-4C52-994D-F54422E9A2C9}'
$DllName    = 'WarzoneAudioApo.dll'
$InstallDir = Join-Path $env:ProgramFiles 'Warzone Audio Optimizer'
$BackupKey  = 'HKLM:\SOFTWARE\WarzoneAudioOptimizer\ApoBackup'
$RenderRoot = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render'
$AudioKey   = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio'

# Slots de efectos del motor de audio, en el orden en que se aplican:
#   SFX (,5) -> MFX (,6) -> EFX (,7)
#
# Muchos dispositivos ya traen efectos del fabricante en SFX y/o MFX -- en un
# auricular con 7.1 virtual, por ejemplo, ESE es el efecto que crea la
# virtualizacion espacial. Pisarlo apagaria justo lo que ayuda a ubicar los
# pasos. Por eso el instalador busca un slot LIBRE (prefiriendo EFX, el mas
# tardio y el que menos interfiere) y solo propone reemplazar si no queda
# ninguno.
$PkeyStreamEffect = '{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D},5'
$PkeyModeEffect   = '{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D},6'
$PkeyEndpointEffect = '{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D},7'

# Orden de preferencia al buscar hueco: EFX primero (procesa al final, sin
# estorbar a nadie), luego MFX.
$SlotPreference = @(
    @{ Key = $PkeyEndpointEffect; Name = 'EFX (final de la cadena)' },
    @{ Key = $PkeyModeEffect;     Name = 'MFX (mezcla combinada)' }
)

# PKEY_AudioEndpoint_Disable_SysFx. Windows la pone a 1 automaticamente si un
# APO falla al cargar repetidamente (10 veces), y entonces DESACTIVA todos los
# efectos del dispositivo. Hay que vigilarla: explica el clasico "instale algo
# y ahora no funciona ningun efecto".
$PkeyDisableSysFx = '{1DA5D803-D492-4EDD-8C23-E0C0FFEE7F0E},5'

function Write-Step($text) { Write-Host "`n==> $text" -ForegroundColor Cyan }
function Write-Warn($text) { Write-Host "    $text" -ForegroundColor Yellow }
function Write-Ok($text)   { Write-Host "    $text" -ForegroundColor Green }
function Write-Err($text)  { Write-Host "    $text" -ForegroundColor Red }

# ---------------------------------------------------------------------------
# Escritura en FxProperties con el permiso MINIMO necesario.
#
# Las claves de MMDevices pertenecen a SYSTEM y conceden a Administradores
# solo "SetValue, ReadKey" -- sin CreateSubKey. Set-ItemProperty abre la clave
# pidiendo escritura COMPLETA (que incluye CreateSubKey), asi que Windows lo
# deniega aunque solo vayamos a escribir un valor. Pedir exactamente SetValue
# funciona sin tener que tomar posesion de la clave ni tocar ninguna ACL, que
# seria mucho mas invasivo y dificil de revertir.
# ---------------------------------------------------------------------------
function Set-AudioRegistryValue {
    param(
        [Parameter(Mandatory)][string] $EndpointGuid,
        [Parameter(Mandatory)][string] $SubKeyName,   # 'FxProperties' o 'Properties'
        [Parameter(Mandatory)][string] $ValueName,
        [Parameter(Mandatory)] $Value,
        [Microsoft.Win32.RegistryValueKind] $Kind = [Microsoft.Win32.RegistryValueKind]::String
    )

    $path = "SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\$EndpointGuid\$SubKeyName"
    $key = $null
    try {
        $key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey(
            $path,
            [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,
            [System.Security.AccessControl.RegistryRights]::SetValue)
        if (-not $key) { return $false }
        $key.SetValue($ValueName, $Value, $Kind)
        return $true
    } catch {
        Write-Err "No se pudo escribir ${ValueName}: $($_.Exception.Message)"
        return $false
    } finally {
        if ($key) { $key.Close() }
    }
}

Write-Host @'
================================================================
  Warzone Audio Optimizer - Instalador del APO
================================================================
'@ -ForegroundColor White

Write-Warn 'Este instalador desactiva la verificacion de firma de APOs de Windows'
Write-Warn '(DisableProtectedAudioDG). Es necesario para cargar un APO sin firmar.'
Write-Warn 'No se toca Secure Boot. El riesgo frente a anti-cheat es tuyo.'
Write-Host ''
Write-Warn 'TEN A MANO uninstall-apo.ps1 antes de continuar: si algo falla,'
Write-Warn 'es lo que te devuelve el audio.'
Write-Host ''
$confirm = Read-Host 'Escribe ACEPTO para continuar'
if ($confirm -ne 'ACEPTO') {
    Write-Host 'Cancelado. No se ha modificado nada.' -ForegroundColor Red
    exit 1
}

# ---------------------------------------------------------------------------
# 0. Limpiar una instalacion previa
#
# Sin esto, reinstalar en OTRO dispositivo sobrescribiria el backup y el
# desinstalador perderia el rastro del anterior, dejandolo asociado para
# siempre. Mover el APO de sitio es justo lo que se hace cuando el primero
# resulta no ser el dispositivo correcto, asi que este caso es la norma, no
# la excepcion.
# ---------------------------------------------------------------------------
Write-Step 'Comprobando si ya hay una instalacion previa...'

$previous = @()
Get-ChildItem $RenderRoot -ErrorAction SilentlyContinue | ForEach-Object {
    $fxProps = Get-ItemProperty -Path (Join-Path $_.PSPath 'FxProperties') -ErrorAction SilentlyContinue
    if (-not $fxProps) { return }
    foreach ($slot in @($PkeyStreamEffect, $PkeyModeEffect, $PkeyEndpointEffect)) {
        if ($fxProps.PSObject.Properties.Name -contains $slot -and $fxProps.$slot -eq $ApoClsid) {
            $script:previous += [PSCustomObject]@{ Guid = $_.PSChildName; Slot = $slot }
        }
    }
}

if ($previous.Count -gt 0) {
    $oldBackup = Get-ItemProperty -Path $BackupKey -ErrorAction SilentlyContinue
    Write-Warn "Ya hay $($previous.Count) instalacion(es) previa(s) del APO."
    Write-Warn 'Se limpiaran antes de instalar en el nuevo dispositivo.'

    foreach ($prev in $previous) {
        # Si el backup dice que ahi habiamos pisado un efecto, devolverlo.
        $restored = $false
        if ($oldBackup -and $oldBackup.EndpointGuid -eq $prev.Guid -and
            ($oldBackup.PSObject.Properties.Name -contains 'ReplacedValue')) {
            $restored = Set-AudioRegistryValue -EndpointGuid $prev.Guid -SubKeyName 'FxProperties' `
                                                -ValueName $prev.Slot -Value $oldBackup.ReplacedValue
            if ($restored) { Write-Ok "Efecto del fabricante restaurado en $($prev.Guid.Substring(1,8))" }
        }
        if (-not $restored) {
            $key = $null
            try {
                $path = "SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\$($prev.Guid)\FxProperties"
                $key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($path,
                        [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,
                        [System.Security.AccessControl.RegistryRights]::SetValue)
                if ($key) { $key.DeleteValue($prev.Slot, $false) }
                Write-Ok "Instalacion previa eliminada de $($prev.Guid.Substring(1,8))"
            } catch {
                Write-Warn "No se pudo limpiar $($prev.Guid.Substring(1,8)): $($_.Exception.Message)"
            } finally {
                if ($key) { $key.Close() }
            }
        }
    }
} else {
    Write-Ok 'No hay instalaciones previas'
}

# ---------------------------------------------------------------------------
# 1. Localizar la DLL antes de tocar nada del sistema
# ---------------------------------------------------------------------------
Write-Step 'Localizando la DLL...'

$sourceDll = $null
foreach ($candidate in @(
    (Join-Path $PSScriptRoot "..\build\$DllName"),
    (Join-Path $PSScriptRoot $DllName)
)) {
    if (Test-Path $candidate) { $sourceDll = (Resolve-Path $candidate).Path; break }
}
if (-not $sourceDll) {
    Write-Err "No se encontro $DllName."
    Write-Err 'Compila el proyecto primero (apo/build) o deja la DLL junto a este script.'
    exit 1
}
Write-Ok "Encontrada: $sourceDll"

# ---------------------------------------------------------------------------
# 2. Elegir el dispositivo de salida
# ---------------------------------------------------------------------------
Write-Step 'Buscando dispositivos de salida activos...'

$devices = @()
Get-ChildItem $RenderRoot -ErrorAction SilentlyContinue | ForEach-Object {
    $endpointKey = $_.PSPath
    $state = (Get-ItemProperty -Path $endpointKey -Name 'DeviceState' -ErrorAction SilentlyContinue).DeviceState
    # DeviceState 1 = activo. Ignoramos deshabilitados/desconectados.
    if ($state -ne 1) { return }

    $props = Get-ItemProperty -Path (Join-Path $endpointKey 'Properties') -ErrorAction SilentlyContinue
    if (-not $props) { return }

    # PKEY_Device_DeviceDesc / PKEY_Device_FriendlyName. El "friendly" suele
    # incluir el nombre de la tarjeta, que es lo que permite distinguir entre
    # varios dispositivos que se llaman igual (p.ej. tres "Altavoces").
    $desc = $props.'{a45c254e-df1c-4efd-8020-67d146a850e0},2'
    $friendly = $props.'{a45c254e-df1c-4efd-8020-67d146a850e0},14'
    $name = if ($friendly) { $friendly } elseif ($desc) { $desc } else { $_.PSChildName }

    # Que slots de efectos estan ocupados, y cual podemos usar sin pisar nada.
    $fxPath = Join-Path $endpointKey 'FxProperties'
    $fx = $null
    if (Test-Path $fxPath) { $fx = Get-ItemProperty -Path $fxPath -ErrorAction SilentlyContinue }

    function Get-SlotValue($props, $key) {
        if ($props -and $props.PSObject.Properties.Name -contains $key) { return $props.$key }
        return $null
    }

    $occupied = @()
    if (Get-SlotValue $fx $PkeyStreamEffect)   { $occupied += 'SFX' }
    if (Get-SlotValue $fx $PkeyModeEffect)     { $occupied += 'MFX' }
    if (Get-SlotValue $fx $PkeyEndpointEffect) { $occupied += 'EFX' }

    $freeSlot = $null
    foreach ($slot in $SlotPreference) {
        if (-not (Get-SlotValue $fx $slot.Key)) { $freeSlot = $slot; break }
    }

    $devices += [PSCustomObject]@{
        Name         = $name
        Guid         = $_.PSChildName
        Path         = $endpointKey
        Occupied     = $occupied
        FreeSlot     = $freeSlot
        ExistingInFree = if ($freeSlot) { $null } else { Get-SlotValue $fx $PkeyModeEffect }
    }
}

if ($devices.Count -eq 0) {
    Write-Err 'No se encontraron dispositivos de salida activos.'
    exit 1
}

Write-Host ''
for ($i = 0; $i -lt $devices.Count; $i++) {
    $d = $devices[$i]
    Write-Host ("  [{0}] {1}" -f $i, $d.Name) -ForegroundColor White
    # El id corto desempata cuando dos dispositivos comparten nombre.
    Write-Host ("      id: {0}" -f $d.Guid.Substring(1, 8)) -ForegroundColor DarkGray
    if ($d.Occupied.Count -gt 0) {
        Write-Host ("      efectos del fabricante: {0}" -f ($d.Occupied -join ', ')) -ForegroundColor DarkGray
    }
    if ($d.FreeSlot) {
        Write-Host ("      -> se instalara en {0}, sin tocar lo existente" -f $d.FreeSlot.Name) -ForegroundColor Green
    } else {
        Write-Host "      -> sin slots libres: habria que REEMPLAZAR un efecto" -ForegroundColor Yellow
    }
}
Write-Host ''
Write-Warn 'Elige el dispositivo por el que escuchas REALMENTE (tus auriculares).'
Write-Warn 'Si tienes software de audio como SteelSeries Sonar, elegir uno de sus'
Write-Warn 'dispositivos virtuales puede dar resultados raros: prefiere el fisico.'
$choice = Read-Host 'Numero de dispositivo'

$index = 0
if (-not [int]::TryParse($choice, [ref]$index) -or $index -lt 0 -or $index -ge $devices.Count) {
    Write-Err 'Seleccion invalida.'
    exit 1
}
$device = $devices[$index]
Write-Ok "Seleccionado: $($device.Name)"

# Decidir en que slot entramos.
if ($device.FreeSlot) {
    $targetSlotKey = $device.FreeSlot.Key
    $targetSlotName = $device.FreeSlot.Name
    $replacedValue = $null
    Write-Ok "Hay un slot libre: $targetSlotName"
    if ($device.Occupied.Count -gt 0) {
        Write-Ok "Los efectos del fabricante ($($device.Occupied -join ', ')) se conservan intactos"
    }
} else {
    # Sin hueco: reemplazar MFX es la opcion menos mala, pero hay que avisar
    # de que se apaga un efecto del fabricante mientras el APO este puesto.
    $targetSlotKey = $PkeyModeEffect
    $targetSlotName = 'MFX (reemplazando el existente)'
    $replacedValue = $device.ExistingInFree
    Write-Host ''
    Write-Warn 'Este dispositivo no tiene ningun slot de efectos libre.'
    Write-Warn "Habria que reemplazar el efecto de modo: $replacedValue"
    Write-Warn 'Si ese efecto es un virtualizador espacial (7.1 virtual, Atmos,'
    Write-Warn 'etc.), lo perderias mientras el APO este instalado -- y es justo'
    Write-Warn 'lo que ayuda a ubicar los pasos. Piensalo antes de aceptar.'
    Write-Warn 'El desinstalador lo restaura tal cual.'
    $ok = Read-Host 'Reemplazarlo de todas formas? (S/N)'
    if ($ok -notmatch '^[SsYy]') {
        Write-Host 'Cancelado. No se ha modificado nada.' -ForegroundColor Red
        exit 1
    }
}

# ---------------------------------------------------------------------------
# 3. Guardar el estado original ANTES de tocar nada
# ---------------------------------------------------------------------------
Write-Step 'Guardando configuracion original (para poder revertir)...'

if (-not (Test-Path $BackupKey)) { New-Item -Path $BackupKey -Force | Out-Null }

$fxPath = Join-Path $device.Path 'FxProperties'
$propsPath = Join-Path $device.Path 'Properties'
$hadFxKey = Test-Path $fxPath

Set-ItemProperty -Path $BackupKey -Name 'EndpointGuid' -Value $device.Guid
Set-ItemProperty -Path $BackupKey -Name 'EndpointName' -Value $device.Name
Set-ItemProperty -Path $BackupKey -Name 'HadFxKey' -Value ([int]$hadFxKey)
# Guardar EN QUE slot entramos: el desinstalador debe limpiar ese y no otro.
Set-ItemProperty -Path $BackupKey -Name 'UsedSlotKey' -Value $targetSlotKey

if ($null -ne $replacedValue) {
    Set-ItemProperty -Path $BackupKey -Name 'ReplacedValue' -Value $replacedValue
    Write-Ok "Efecto reemplazado guardado para restaurar: $replacedValue"
} else {
    Remove-ItemProperty -Path $BackupKey -Name 'ReplacedValue' -ErrorAction SilentlyContinue
    Write-Ok 'No se reemplaza ningun efecto (entramos en un slot libre)'
}

$originalDisable = $null
if (Test-Path $AudioKey) {
    $audioProps = Get-ItemProperty -Path $AudioKey -ErrorAction SilentlyContinue
    if ($audioProps -and $audioProps.PSObject.Properties.Name -contains 'DisableProtectedAudioDG') {
        $originalDisable = $audioProps.DisableProtectedAudioDG
    }
}
Set-ItemProperty -Path $BackupKey -Name 'HadDisableFlag' -Value ([int]($null -ne $originalDisable))

# ---------------------------------------------------------------------------
# 4. Instalar y registrar la DLL
# ---------------------------------------------------------------------------
Write-Step 'Instalando la DLL...'

if (-not (Test-Path $InstallDir)) {
    New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
}
$targetDll = Join-Path $InstallDir $DllName
Copy-Item $sourceDll $targetDll -Force
Write-Ok "Copiada a $targetDll"

$regsvr = Start-Process -FilePath 'regsvr32.exe' -ArgumentList '/s', "`"$targetDll`"" -Wait -PassThru
if ($regsvr.ExitCode -ne 0) {
    Write-Err "regsvr32 fallo (codigo $($regsvr.ExitCode)). No se ha tocado la configuracion de audio."
    exit 1
}
Write-Ok 'COM registrado'

# ---------------------------------------------------------------------------
# 5. Aplicar la configuracion de audio
# ---------------------------------------------------------------------------
Write-Step 'Aplicando configuracion...'

if (-not (Test-Path $AudioKey)) { New-Item -Path $AudioKey -Force | Out-Null }
Set-ItemProperty -Path $AudioKey -Name 'DisableProtectedAudioDG' -Value 1 -Type DWord
Write-Warn 'DisableProtectedAudioDG = 1 (verificacion de firma de APOs desactivada)'

if (-not $hadFxKey) { New-Item -Path $fxPath -Force | Out-Null }

if (-not (Set-AudioRegistryValue -EndpointGuid $device.Guid -SubKeyName 'FxProperties' `
                                  -ValueName $targetSlotKey -Value $ApoClsid)) {
    Write-Err 'No se pudo asociar el APO al dispositivo.'
    Write-Err 'Comprueba que esta ventana de PowerShell se abrio COMO ADMINISTRADOR'
    Write-Err '(el titulo debe empezar por "Administrador:").'
    Write-Err 'Ejecuta uninstall-apo.ps1 para dejar el sistema como estaba.'
    exit 1
}
Write-Ok "APO asociado a $($device.Name) en $targetSlotName"

# Si una instalacion anterior dejo los efectos desactivados, limpiarlo: si no,
# el APO cargaria bien pero Windows lo ignoraria igualmente.
$deviceProps = Get-ItemProperty -Path $propsPath -ErrorAction SilentlyContinue
if ($deviceProps -and $deviceProps.PSObject.Properties.Name -contains $PkeyDisableSysFx) {
    if ($deviceProps.$PkeyDisableSysFx -ne 0) {
        if (Set-AudioRegistryValue -EndpointGuid $device.Guid -SubKeyName 'Properties' `
                                    -ValueName $PkeyDisableSysFx -Value 0 `
                                    -Kind ([Microsoft.Win32.RegistryValueKind]::DWord)) {
            Write-Ok 'Se reactivaron los efectos del dispositivo (estaban desactivados)'
        }
    }
}

# ---------------------------------------------------------------------------
# 6. Recargar el motor de audio
# ---------------------------------------------------------------------------
Write-Step 'Recargando el motor de audio...'

# Reiniciar AudioEndpointBuilder (no solo Audiosrv): es quien reconstruye el
# grafo de endpoints y relee las propiedades FX. Audiosrv depende de el, asi
# que -Force reinicia ambos y con ellos audiodg.exe, que es donde carga el APO.
$restarted = $false
try {
    Restart-Service -Name 'AudioEndpointBuilder' -Force
    $restarted = $true
    Write-Ok 'Motor de audio reiniciado'
    Start-Sleep -Seconds 3
} catch {
    Write-Warn "No se pudo reiniciar automaticamente: $($_.Exception.Message)"
    Write-Warn 'Reinicia Windows para aplicar los cambios.'
}

# ---------------------------------------------------------------------------
# 7. Verificar que el APO realmente cargo
# ---------------------------------------------------------------------------
if ($restarted) {
    Write-Step 'Comprobando si el APO cargo...'
    Write-Host '    (reproduce audio ahora por ese dispositivo para forzar la carga)' -ForegroundColor DarkGray
    Start-Sleep -Seconds 4

    $loaded = $false
    try {
        $audiodg = Get-Process -Name 'audiodg' -ErrorAction SilentlyContinue
        if ($audiodg) {
            foreach ($m in $audiodg.Modules) {
                if ($m.ModuleName -eq $DllName) { $loaded = $true; break }
            }
        }
    } catch {
        # Enumerar modulos de audiodg puede fallar por permisos; no es
        # concluyente, asi que no lo tratamos como error.
        $loaded = $null
    }

    if ($loaded -eq $true) {
        Write-Ok 'CONFIRMADO: el APO esta cargado en el motor de audio'
    } elseif ($null -eq $loaded) {
        Write-Warn 'No se pudo inspeccionar audiodg.exe (normal por permisos).'
        Write-Warn 'Comprueba a oido si el audio suena procesado.'
    } else {
        Write-Warn 'Todavia no aparece cargado. Puede ser normal si no hay audio'
        Write-Warn 'sonando: Windows carga el APO al abrir el primer stream.'
        Write-Warn 'Reproduce algo y vuelve a comprobar. Si sigue sin sonar'
        Write-Warn 'procesado, ejecuta uninstall-apo.ps1.'
    }

    $sysFxNow = Get-ItemProperty -Path $propsPath -ErrorAction SilentlyContinue
    if ($sysFxNow -and $sysFxNow.PSObject.Properties.Name -contains $PkeyDisableSysFx) {
        if ($sysFxNow.$PkeyDisableSysFx -ne 0) {
            Write-Err 'Windows desactivo los efectos del dispositivo: el APO fallo al cargar.'
            Write-Err 'Ejecuta uninstall-apo.ps1 para dejarlo todo como estaba.'
        }
    }
}

Write-Host @"

================================================================
  Instalacion completada
================================================================

  Dispositivo : $($device.Name)
  Ajustes     : se leen de %APPDATA%\WarzoneAudioOptimizer\settings.ini
                (los mismos que edita la app de escritorio)

  COMO PROBAR
    Reproduce audio por ese dispositivo. Deberia sonar procesado,
    sin abrir ninguna app y sin delay perceptible.

  CAMBIAR AJUSTES
    Muevelos en la app de escritorio. El APO relee el archivo cuando
    el motor lo reinicializa, no al instante: para forzarlo, cambia
    el dispositivo de salida y vuelve, o reinicia el servicio de audio.

  SI TE QUEDASTE SIN AUDIO
    Ejecuta uninstall-apo.ps1 como administrador.

"@ -ForegroundColor White
