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

.PARAMETER DeviceGuid
    Endpoint donde instalar, en formato {....}. Si se indica, se salta la
    seleccion interactiva. La app lo usa para mover el APO de dispositivo
    sin obligar al usuario a elegir de una lista en la consola.

.PARAMETER Unattended
    Da por aceptadas las confirmaciones. Solo tiene efecto junto a
    -DeviceGuid: la app ya ha mostrado el aviso en su propia interfaz, asi
    que repetirlo en la consola solo estorba.

.NOTES
    Requiere ejecutarse como Administrador.
#>

#Requires -RunAsAdministrator

param(
    [string] $DeviceGuid,
    [switch] $Unattended
)

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

# Orden de preferencia al buscar hueco: MFX primero, luego SFX.
#
# EFX queda FUERA a proposito. Parecia la opcion ideal por ser la ultima de
# la cadena y la que menos estorba, pero en la practica no procesa: Microsoft
# documenta que el efecto de endpoint suele existir solo para identificacion,
# porque a esa altura el audio ya paso a modo kernel. Comprobado aqui: con el
# APO en EFX, Windows no llegaba a cargarlo nunca; en MFX carga y procesa.
$SlotPreference = @(
    @{ Key = $PkeyModeEffect;   Name = 'MFX (mezcla combinada)' },
    @{ Key = $PkeyStreamEffect; Name = 'SFX (por stream)' }
)

# PKEY_AudioEndpoint_Disable_SysFx: a 1 significa "no apliques NINGUN efecto
# en este dispositivo", y Windows ignora entonces cualquier APO instalado.
# Se pone sola si un APO falla al cargar repetidamente, y tambien al desmarcar
# las mejoras de audio del dispositivo.
#
# OJO: vive en FxProperties, NO en Properties. Buscarla en Properties (donde
# uno esperaria, y donde la buscaba este script) no da error: simplemente no
# aparece nunca, y el diagnostico dice "efectos activos" mientras estan
# apagados. Costo horas.
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

$skipPrompts = $Unattended -and $DeviceGuid

if (-not $skipPrompts) {
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
} else {
    Write-Host '  Modo desatendido (lanzado desde la aplicacion)' -ForegroundColor DarkGray
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

$device = $null

if ($DeviceGuid) {
    # Seleccion por parametro: la app ya sabe que dispositivo quiere.
    $device = $devices | Where-Object { $_.Guid -eq $DeviceGuid } | Select-Object -First 1
    if (-not $device) {
        Write-Err "El dispositivo $DeviceGuid no existe o no esta activo."
        exit 1
    }
    Write-Ok "Dispositivo indicado: $($device.Name)"
} else {
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
}

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
    if ($skipPrompts) {
        # Reemplazar un efecto del fabricante es una decision con consecuencias
        # audibles: nunca se toma sola, ni siquiera en modo desatendido.
        Write-Err 'Cancelado: se necesita confirmacion para reemplazar un efecto.'
        Write-Err 'Ejecuta este script a mano si de verdad quieres ese dispositivo.'
        exit 2
    }
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
# 3b. Carpeta de ajustes compartida
#
# El APO corre dentro de audiodg.exe COMO SYSTEM, asi que no puede leer el
# AppData del usuario: los ajustes tienen que vivir en un sitio comun. Y como
# quien escribe es la app (sin elevar) mientras quien lee es SYSTEM, hay que
# garantizar explicitamente que el usuario pueda escribir ahi -- si la carpeta
# la crease SYSTEM sin mas, el usuario podria quedarse sin permiso y los
# sliders dejarian de guardar sin ningun aviso.
# ---------------------------------------------------------------------------
Write-Step 'Preparando la carpeta de ajustes compartida...'

$SettingsDir = 'C:\ProgramData\WarzoneAudioOptimizer'
if (-not (Test-Path $SettingsDir)) {
    New-Item -ItemType Directory -Path $SettingsDir -Force | Out-Null
}
# El grupo "Usuarios" se llama distinto en cada idioma de Windows (Users,
# Usuarios, Utilisateurs...). Usar el nombre literal falla fuera del ingles,
# y encima falla en silencio. El SID S-1-5-32-545 es el mismo en todos.
$UsersSid = New-Object System.Security.Principal.SecurityIdentifier('S-1-5-32-545')

try {
    $acl = Get-Acl $SettingsDir
    $rule = New-Object System.Security.AccessControl.FileSystemAccessRule(
        $UsersSid, 'Modify',
        'ContainerInherit,ObjectInherit', 'None', 'Allow')
    $acl.SetAccessRule($rule)
    Set-Acl -Path $SettingsDir -AclObject $acl
    Write-Ok "Carpeta lista: $SettingsDir (escritura para usuarios)"
} catch {
    Write-Warn "No se pudieron ajustar permisos: $($_.Exception.Message)"
    Write-Warn 'Si los sliders no guardan, revisa los permisos de esa carpeta.'
}

# Migrar los ajustes que el usuario ya tuviera en la ubicacion antigua.
$legacy = Join-Path $env:APPDATA 'WarzoneAudioOptimizer\settings.ini'
$target = Join-Path $SettingsDir 'settings.ini'
if ((Test-Path $legacy) -and -not (Test-Path $target)) {
    Copy-Item $legacy $target -Force -ErrorAction SilentlyContinue
    if (Test-Path $target) { Write-Ok 'Ajustes existentes migrados' }
}

# Y darle permiso de escritura al ARCHIVO, no solo a la carpeta.
#
# Este script corre elevado, asi que todo lo que crea pertenece a
# Administradores y deja a los usuarios en solo lectura. La app NO corre
# elevada: sin esto, guardaria los ajustes sin exito y en silencio -- los
# sliders pareceria que no hacen nada, que es exactamente el sintoma que
# costo horas de diagnostico.
if (Test-Path $target) {
    try {
        $fileAcl = Get-Acl $target
        $fileRule = New-Object System.Security.AccessControl.FileSystemAccessRule(
            $UsersSid, 'Modify', 'Allow')
        $fileAcl.SetAccessRule($fileRule)
        Set-Acl -Path $target -AclObject $fileAcl

        # Verificar de verdad, no dar por hecho que Set-Acl basto: este es el
        # punto exacto donde un fallo silencioso deja los sliders sin efecto.
        $check = (Get-Acl $target).Access | Where-Object {
            $_.IdentityReference.Translate([System.Security.Principal.SecurityIdentifier]) -eq $UsersSid -and
            $_.FileSystemRights -match 'Modify|FullControl|Write'
        }
        if ($check) {
            Write-Ok 'El archivo de ajustes es escribible por el usuario'
        } else {
            Write-Warn 'Los permisos del archivo no quedaron aplicados.'
            Write-Warn 'Los sliders no llegaran al APO hasta arreglarlo.'
        }
    } catch {
        Write-Warn "No se pudieron ajustar permisos del archivo: $($_.Exception.Message)"
    }
}

# ---------------------------------------------------------------------------
# 4. Instalar y registrar la DLL
# ---------------------------------------------------------------------------
Write-Step 'Instalando la DLL...'

# La DLL va a System32, no a Program Files.
#
# audiodg.exe corre como SYSTEM con un token muy restringido, y no carga
# binarios desde cualquier sitio. Todos los ejemplos de APO de Microsoft los
# ponen en System32 (%SystemRoot%\System32\swapapo.dll) por este motivo. Con
# la DLL en Program Files, el objeto COM se instanciaba bien desde una app
# normal pero el motor de audio no lo cargaba nunca.
$targetDll = Join-Path $env:SystemRoot "System32\$DllName"

# Copia previa en Program Files: sirve de referencia para el desinstalador y
# para saber que version esta puesta.
if (-not (Test-Path $InstallDir)) {
    New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
}
Copy-Item $sourceDll (Join-Path $InstallDir $DllName) -Force -ErrorAction SilentlyContinue

try {
    Copy-Item $sourceDll $targetDll -Force -ErrorAction Stop
    Write-Ok "Copiada a $targetDll"
} catch {
    # Si la DLL ya estaba cargada por audiodg, el archivo esta bloqueado.
    Write-Warn 'La DLL estaba en uso; se reemplaza tras reiniciar el audio...'
    Stop-Service -Name 'AudioEndpointBuilder' -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
    try {
        Copy-Item $sourceDll $targetDll -Force -ErrorAction Stop
        Write-Ok "Copiada a $targetDll"
    } catch {
        Write-Err "No se pudo copiar a System32: $($_.Exception.Message)"
        Start-Service -Name 'AudioEndpointBuilder' -ErrorAction SilentlyContinue
        exit 1
    }
    Start-Service -Name 'AudioEndpointBuilder' -ErrorAction SilentlyContinue
}

$regsvr = Start-Process -FilePath 'regsvr32.exe' -ArgumentList '/s', "`"$targetDll`"" -Wait -PassThru
if ($regsvr.ExitCode -ne 0) {
    Write-Err "regsvr32 fallo (codigo $($regsvr.ExitCode)). No se ha tocado la configuracion de audio."
    exit 1
}
Write-Ok 'COM registrado'

# ---------------------------------------------------------------------------
# Registro ante el MOTOR DE AUDIO.
#
# Asignar el CLSID a un dispositivo no basta: Windows tambien necesita esta
# ficha para saber COMO instanciar el APO (cuantas conexiones acepta, que
# interfaces expone, cuantas instancias permite). Sin ella ve la asignacion,
# no sabe que hacer con ella, y la ignora en silencio -- el sintoma es un APO
# perfectamente instalado que nunca llega a cargarse.
#
# Se hace aqui y no en DllRegisterServer porque esta rama del registro
# tambien esta protegida: regsvr32 no siempre puede escribirla, y fallaba sin
# avisar.
# ---------------------------------------------------------------------------
Write-Step 'Registrando el APO ante el motor de audio...'

# La ruta correcta es SOFTWARE\Classes\AudioEngine\..., no la que aparece en
# los ejemplos de INF de la documentacion (MMDevices\Audio\AudioEngine\...):
# esa ni siquiera existe en Windows 11. Se confirmo mirando donde esta
# registrado un APO que si funciona en esta maquina.
$EngineKey = "HKLM:\SOFTWARE\Classes\AudioEngine\AudioProcessingObjects\$ApoClsid"

# IID de IAudioProcessingObject: la interfaz que el motor pide primero.
$IidAudioProcessingObject = '{FD7F2B29-24D0-4B5C-B177-592C39F9CA10}'

# APO_FLAG_INPLACE(1) | SAMPLESPERFRAME(2) | FRAMESPERSECOND(4) | BITSPERSAMPLE(8)
$ApoFlags = 15

try {
    if (-not (Test-Path $EngineKey)) { New-Item -Path $EngineKey -Force | Out-Null }

    New-ItemProperty -Path $EngineKey -Name 'FriendlyName' -Value 'Warzone Audio Optimizer' -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $EngineKey -Name 'Copyright' -Value 'Warzone Audio Optimizer' -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $EngineKey -Name 'MajorVersion' -Value 1 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $EngineKey -Name 'MinorVersion' -Value 1 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $EngineKey -Name 'Flags' -Value $ApoFlags -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $EngineKey -Name 'MinInputConnections' -Value 1 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $EngineKey -Name 'MaxInputConnections' -Value 1 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $EngineKey -Name 'MinOutputConnections' -Value 1 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $EngineKey -Name 'MaxOutputConnections' -Value 1 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $EngineKey -Name 'MaxInstances' -Value 0xFFFFFFFF -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $EngineKey -Name 'NumAPOInterfaces' -Value 1 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $EngineKey -Name 'APOInterface0' -Value $IidAudioProcessingObject -PropertyType String -Force | Out-Null

    # Verificar de verdad: este es exactamente el punto donde antes fallaba
    # sin decir nada.
    $check = Get-ItemProperty -Path $EngineKey -ErrorAction SilentlyContinue
    if ($check -and $check.APOInterface0 -eq $IidAudioProcessingObject) {
        Write-Ok 'APO registrado ante el motor de audio (12 valores)'
    } else {
        Write-Err 'El registro ante el motor no quedo completo.'
        Write-Err 'Windows no podra instanciar el APO.'
    }
} catch {
    Write-Err "No se pudo registrar ante el motor: $($_.Exception.Message)"
    exit 1
}

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

# Asegurar que el dispositivo tiene los efectos ACTIVADOS. Sin esto, Windows
# ignora el APO por completo aunque este perfectamente instalado y registrado.
# Se escribe siempre, no solo si la clave ya existe: puede faltar y aun asi
# heredar el comportamiento desactivado.
$fxNow = Get-ItemProperty -Path $fxPath -ErrorAction SilentlyContinue
$sysFxValue = if ($fxNow -and $fxNow.PSObject.Properties.Name -contains $PkeyDisableSysFx) {
    $fxNow.$PkeyDisableSysFx
} else { $null }

if ($sysFxValue -ne 0) {
    if (Set-AudioRegistryValue -EndpointGuid $device.Guid -SubKeyName 'FxProperties' `
                                -ValueName $PkeyDisableSysFx -Value 0 `
                                -Kind ([Microsoft.Win32.RegistryValueKind]::DWord)) {
        if ($null -eq $sysFxValue) {
            Write-Ok 'Efectos del dispositivo habilitados explicitamente'
        } else {
            Write-Ok "Efectos REACTIVADOS (estaban desactivados: valor $sysFxValue)"
        }
    } else {
        Write-Warn 'No se pudo habilitar los efectos; Windows ignorara el APO.'
    }
} else {
    Write-Ok 'Los efectos del dispositivo ya estaban activos'
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
