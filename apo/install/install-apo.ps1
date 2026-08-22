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

# Property keys del motor de audio. La ",6" es PKEY_FX_ModeEffectClsid: el
# efecto de modo, que se aplica a la mezcla ya combinada -- justo donde
# queremos insertarnos.
$PkeyModeEffect = '{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D},6'
# La ",5" (stream effect) y ",7" (endpoint effect) se dejan intactas.

function Write-Step($text)  { Write-Host "`n==> $text" -ForegroundColor Cyan }
function Write-Warn($text)  { Write-Host "    $text" -ForegroundColor Yellow }
function Write-Ok($text)    { Write-Host "    $text" -ForegroundColor Green }

Write-Host @'
================================================================
  Warzone Audio Optimizer - Instalador del APO
================================================================
'@ -ForegroundColor White

Write-Warn 'Este instalador desactiva la verificacion de firma de APOs de Windows'
Write-Warn '(DisableProtectedAudioDG). Es necesario para cargar un APO sin firmar.'
Write-Warn 'No se toca Secure Boot. El riesgo frente a anti-cheat es tuyo.'
Write-Host ''
$confirm = Read-Host 'Escribe ACEPTO para continuar'
if ($confirm -ne 'ACEPTO') {
    Write-Host 'Cancelado. No se ha modificado nada.' -ForegroundColor Red
    exit 1
}

# ---------------------------------------------------------------------------
# 1. Elegir el dispositivo de salida
# ---------------------------------------------------------------------------
Write-Step 'Buscando dispositivos de salida activos...'

$devices = @()
Get-ChildItem $RenderRoot -ErrorAction SilentlyContinue | ForEach-Object {
    $endpointKey = $_.PSPath
    $state = (Get-ItemProperty -Path $endpointKey -Name 'DeviceState' -ErrorAction SilentlyContinue).DeviceState
    # DeviceState 1 = activo. Ignoramos deshabilitados/desconectados.
    if ($state -ne 1) { return }

    $propsPath = Join-Path $endpointKey 'Properties'
    $props = Get-ItemProperty -Path $propsPath -ErrorAction SilentlyContinue
    if (-not $props) { return }

    # PKEY_Device_DeviceDesc / PKEY_Device_FriendlyName. El "friendly" suele
    # incluir el nombre de la tarjeta, que es lo que permite distinguir entre
    # varios dispositivos que se llaman igual (p.ej. tres "Altavoces").
    $desc = $props.'{a45c254e-df1c-4efd-8020-67d146a850e0},2'
    $friendly = $props.'{a45c254e-df1c-4efd-8020-67d146a850e0},14'
    $name = if ($friendly) { $friendly } elseif ($desc) { $desc } else { $_.PSChildName }

    # Efecto de modo ya registrado por el fabricante, si lo hay: instalarnos
    # ahi lo REEMPLAZA, asi que hay que avisar antes.
    $fxPath = Join-Path $endpointKey 'FxProperties'
    $existingMfx = $null
    if (Test-Path $fxPath) {
        $fx = Get-ItemProperty -Path $fxPath -ErrorAction SilentlyContinue
        if ($fx -and $fx.PSObject.Properties.Name -contains $PkeyModeEffect) {
            $existingMfx = $fx.$PkeyModeEffect
        }
    }

    $devices += [PSCustomObject]@{
        Name = $name
        Desc = $desc
        Guid = $_.PSChildName
        Path = $endpointKey
        ExistingMfx = $existingMfx
    }
}

if ($devices.Count -eq 0) {
    Write-Host 'No se encontraron dispositivos de salida activos.' -ForegroundColor Red
    exit 1
}

Write-Host ''
for ($i = 0; $i -lt $devices.Count; $i++) {
    $d = $devices[$i]
    Write-Host ("  [{0}] {1}" -f $i, $d.Name) -ForegroundColor White
    # El GUID corto desempata cuando dos dispositivos comparten nombre.
    Write-Host ("      id: {0}" -f $d.Guid.Substring(1, 8)) -ForegroundColor DarkGray
    if ($d.ExistingMfx) {
        Write-Host "      OJO: ya tiene un efecto de fabricante; se reemplazara" -ForegroundColor Yellow
    }
}
Write-Host ''
Write-Warn 'Elige el dispositivo por el que escuchas REALMENTE (tus auriculares).'
Write-Warn 'Si tienes software de audio como SteelSeries Sonar, elegir uno de sus'
Write-Warn 'dispositivos virtuales puede dar resultados raros: prefiere el fisico.'
$choice = Read-Host 'Numero de dispositivo'

$index = 0
if (-not [int]::TryParse($choice, [ref]$index) -or $index -lt 0 -or $index -ge $devices.Count) {
    Write-Host 'Seleccion invalida.' -ForegroundColor Red
    exit 1
}
$device = $devices[$index]
Write-Ok "Seleccionado: $($device.Name)"

if ($device.ExistingMfx) {
    Write-Host ''
    Write-Warn "Este dispositivo ya tiene el efecto $($device.ExistingMfx)"
    Write-Warn 'Instalar aqui lo desactiva mientras el APO este puesto.'
    Write-Warn 'El desinstalador lo restaura tal cual.'
    $ok = Read-Host 'Continuar de todas formas? (S/N)'
    if ($ok -notmatch '^[SsYy]') {
        Write-Host 'Cancelado. No se ha modificado nada.' -ForegroundColor Red
        exit 1
    }
}

# ---------------------------------------------------------------------------
# 2. Copiar y registrar la DLL
# ---------------------------------------------------------------------------
Write-Step 'Instalando la DLL...'

$sourceDll = Join-Path $PSScriptRoot "..\build\$DllName"
if (-not (Test-Path $sourceDll)) {
    $sourceDll = Join-Path $PSScriptRoot $DllName
}
if (-not (Test-Path $sourceDll)) {
    Write-Host "No se encontro $DllName. Compila el proyecto primero." -ForegroundColor Red
    exit 1
}

if (-not (Test-Path $InstallDir)) {
    New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
}
$targetDll = Join-Path $InstallDir $DllName
Copy-Item $sourceDll $targetDll -Force
Write-Ok "Copiada a $targetDll"

$regsvr = Start-Process -FilePath 'regsvr32.exe' -ArgumentList '/s', "`"$targetDll`"" -Wait -PassThru
if ($regsvr.ExitCode -ne 0) {
    Write-Host "regsvr32 fallo (codigo $($regsvr.ExitCode))." -ForegroundColor Red
    exit 1
}
Write-Ok 'COM registrado'

# ---------------------------------------------------------------------------
# 3. Guardar el estado original ANTES de tocar nada
# ---------------------------------------------------------------------------
Write-Step 'Guardando configuracion original (para poder revertir)...'

if (-not (Test-Path $BackupKey)) {
    New-Item -Path $BackupKey -Force | Out-Null
}

$fxPath = Join-Path $device.Path 'FxProperties'
$hadFxKey = Test-Path $fxPath

$originalMfx = $null
if ($hadFxKey) {
    $fxProps = Get-ItemProperty -Path $fxPath -ErrorAction SilentlyContinue
    if ($fxProps -and $fxProps.PSObject.Properties.Name -contains $PkeyModeEffect) {
        $originalMfx = $fxProps.$PkeyModeEffect
    }
}

Set-ItemProperty -Path $BackupKey -Name 'EndpointGuid' -Value $device.Guid
Set-ItemProperty -Path $BackupKey -Name 'EndpointName' -Value $device.Name
Set-ItemProperty -Path $BackupKey -Name 'HadFxKey' -Value ([int]$hadFxKey)
if ($null -ne $originalMfx) {
    Set-ItemProperty -Path $BackupKey -Name 'OriginalModeEffect' -Value $originalMfx
    Write-Ok "Efecto de modo original guardado: $originalMfx"
} else {
    Remove-ItemProperty -Path $BackupKey -Name 'OriginalModeEffect' -ErrorAction SilentlyContinue
    Write-Ok 'El dispositivo no tenia efecto de modo previo'
}

$audioKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio'
$originalDisable = $null
if (Test-Path $audioKey) {
    $audioProps = Get-ItemProperty -Path $audioKey -ErrorAction SilentlyContinue
    if ($audioProps -and $audioProps.PSObject.Properties.Name -contains 'DisableProtectedAudioDG') {
        $originalDisable = $audioProps.DisableProtectedAudioDG
    }
}
Set-ItemProperty -Path $BackupKey -Name 'HadDisableFlag' -Value ([int]($null -ne $originalDisable))

# ---------------------------------------------------------------------------
# 4. Aplicar la configuracion
# ---------------------------------------------------------------------------
Write-Step 'Aplicando configuracion...'

if (-not (Test-Path $audioKey)) { New-Item -Path $audioKey -Force | Out-Null }
Set-ItemProperty -Path $audioKey -Name 'DisableProtectedAudioDG' -Value 1 -Type DWord
Write-Warn 'DisableProtectedAudioDG = 1 (verificacion de firma de APOs desactivada)'

if (-not $hadFxKey) { New-Item -Path $fxPath -Force | Out-Null }
Set-ItemProperty -Path $fxPath -Name $PkeyModeEffect -Value $ApoClsid -Type String
Write-Ok "APO asociado a $($device.Name)"

# ---------------------------------------------------------------------------
# 5. Reiniciar el servicio de audio para que cargue el APO
# ---------------------------------------------------------------------------
Write-Step 'Reiniciando el servicio de audio...'
try {
    Restart-Service -Name 'Audiosrv' -Force
    Write-Ok 'Servicio de audio reiniciado'
} catch {
    Write-Warn "No se pudo reiniciar automaticamente: $_"
    Write-Warn 'Reinicia Windows para aplicar los cambios.'
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
    sin necesidad de abrir ninguna app y sin delay perceptible.

  SI TE QUEDASTE SIN AUDIO
    Ejecuta uninstall-apo.ps1 como administrador. Restaura la
    configuracion original que se acaba de guardar.

"@ -ForegroundColor White
