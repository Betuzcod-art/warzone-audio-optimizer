<#
.SYNOPSIS
    Reinstala el APO en el dispositivo por el que SUENA de verdad y comprueba
    que Windows lo carga.

.DESCRIPTION
    Hace de una vez todo lo que hasta ahora habia que encadenar a mano:

      1. Mide que dispositivo tiene señal real (no el "predeterminado", que
         puede no ser por donde sale el audio: con software como SteelSeries
         Sonar de por medio, casi nunca coinciden).
      2. Instala el APO ahi, en un slot que SI procese.
      3. Reinicia el motor de audio.
      4. Comprueba que la DLL acabo cargada y lo dice claramente.

    Deja el audio sonando mientras se ejecuta: sin señal no se puede medir
    por donde sale, y Windows no carga el APO si no hay nada reproduciendose.

.NOTES
    Requiere ejecutarse como Administrador.
#>

#Requires -RunAsAdministrator

$ErrorActionPreference = 'Continue'

$ApoClsid   = '{E76EE61C-E30D-4C52-994D-F54422E9A2C9}'
$RenderRoot = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render'

function Write-Step($t) { Write-Host "`n==> $t" -ForegroundColor Cyan }
function Write-Ok($t)   { Write-Host "    $t" -ForegroundColor Green }
function Write-Warn($t) { Write-Host "    $t" -ForegroundColor Yellow }
function Write-Err($t)  { Write-Host "    $t" -ForegroundColor Red }

Write-Host @'
================================================================
  Warzone Audio Optimizer - Reparar y probar
================================================================
'@ -ForegroundColor White

Write-Warn 'DEJA MUSICA SONANDO durante todo el proceso.'
Write-Host ''
Read-Host 'Pulsa Enter cuando tengas audio reproduciendose'

# ---------------------------------------------------------------------------
# 1. Medir por donde sale el audio de verdad
# ---------------------------------------------------------------------------
Write-Step 'Midiendo por que dispositivo sale el audio...'

$meterCode = @'
using System;
using System.Runtime.InteropServices;
public static class WzMeters {
    [ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")] class EnumComObj { }
    [Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDeviceEnumerator {
        int EnumAudioEndpoints(int f, int s, out IMMDeviceCollection c);
        int GetDefaultAudioEndpoint(int f, int r, out IMMDevice d);
    }
    [Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDeviceCollection { int GetCount(out int n); int Item(int i, out IMMDevice d); }
    [Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDevice {
        int Activate(ref Guid iid, int ctx, IntPtr p, [MarshalAs(UnmanagedType.IUnknown)] out object o);
        int OpenPropertyStore(int a, out IntPtr s);
        int GetId([MarshalAs(UnmanagedType.LPWStr)] out string id);
        int GetState(out int st);
    }
    [Guid("C02216F6-8C67-4B5B-9D00-D008E73E0064"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IAudioMeterInformation { int GetPeakValue(out float p); }

    public static string Scan() {
        var e = (IMMDeviceEnumerator)new EnumComObj();
        IMMDeviceCollection col; e.EnumAudioEndpoints(0, 1, out col);
        int n; col.GetCount(out n);
        var sb = new System.Text.StringBuilder();
        var iid = typeof(IAudioMeterInformation).GUID;
        for (int i = 0; i < n; i++) {
            IMMDevice d; col.Item(i, out d);
            string id; d.GetId(out id);
            object o;
            if (d.Activate(ref iid, 1, IntPtr.Zero, out o) == 0) {
                float peak; ((IAudioMeterInformation)o).GetPeakValue(out peak);
                sb.AppendLine(id + "|" + peak.ToString("F6"));
            }
        }
        return sb.ToString();
    }
}
'@
Add-Type -TypeDefinition $meterCode -Language CSharp -ErrorAction SilentlyContinue

# Varias muestras durante unos segundos: un pico instantaneo puede caer justo
# en un silencio de la cancion y hacernos elegir mal.
$peaks = @{}
for ($i = 0; $i -lt 20; $i++) {
    foreach ($line in ([WzMeters]::Scan() -split "`r?`n")) {
        if (-not $line) { continue }
        $p = $line -split '\|'
        $id = $p[0]; $v = [double]$p[1]
        if (-not $peaks.ContainsKey($id) -or $peaks[$id] -lt $v) { $peaks[$id] = $v }
    }
    Start-Sleep -Milliseconds 200
}

$best = $null; $bestPeak = 0.0
foreach ($id in $peaks.Keys) {
    if ($peaks[$id] -gt $bestPeak) { $bestPeak = $peaks[$id]; $best = $id }
}

if (-not $best -or $bestPeak -le 0.0001) {
    Write-Err 'No se detecto señal en ningun dispositivo.'
    Write-Err 'Asegurate de tener musica sonando y vuelve a ejecutar.'
    exit 1
}

$guid = if ($best -match '\{[0-9a-fA-F-]+\}$') { $matches[0] } else { $null }
if (-not $guid) { Write-Err 'No se pudo identificar el dispositivo.'; exit 1 }

$props = Get-ItemProperty -Path "$RenderRoot\$guid\Properties" -ErrorAction SilentlyContinue
$name = $props.'{a45c254e-df1c-4efd-8020-67d146a850e0},14'
if (-not $name) { $name = $props.'{a45c254e-df1c-4efd-8020-67d146a850e0},2' }
$adapter = $props.'{b3f8fa53-0004-438e-9003-51a46e139bfc},6'

Write-Ok ("Suena por: {0} ({1})" -f $name, $adapter)
Write-Ok ("Nivel maximo detectado: {0:F4}" -f $bestPeak)

# ---------------------------------------------------------------------------
# 2. Instalar ahi
# ---------------------------------------------------------------------------
Write-Step 'Instalando el APO en ese dispositivo...'

$installer = Join-Path $PSScriptRoot 'install-apo.ps1'
if (-not (Test-Path $installer)) {
    Write-Err "No se encontro install-apo.ps1 junto a este script."
    exit 1
}

& $installer -DeviceGuid $guid -Unattended
if ($LASTEXITCODE -ne 0) {
    Write-Err "El instalador termino con codigo $LASTEXITCODE"
    exit 1
}

# ---------------------------------------------------------------------------
# 3. Comprobar que Windows lo carga de verdad
# ---------------------------------------------------------------------------
Write-Step 'Comprobando que Windows carga el APO...'
Write-Warn 'PON MUSICA DE NUEVO AHORA (el reinicio del audio la habra cortado).'
Read-Host 'Pulsa Enter cuando vuelva a sonar'

$loaded = $false
for ($i = 0; $i -lt 10; $i++) {
    $audiodg = Get-Process -Name 'audiodg' -ErrorAction SilentlyContinue
    if ($audiodg) {
        try {
            foreach ($m in $audiodg.Modules) {
                if ($m.ModuleName -eq 'WarzoneAudioApo.dll') { $loaded = $true; break }
            }
        } catch { }
    }
    if ($loaded) { break }
    Start-Sleep -Seconds 1
}

Write-Host ''
if ($loaded) {
    Write-Host @'
================================================================
  FUNCIONANDO
================================================================
'@ -ForegroundColor Green
    Write-Ok 'El APO esta cargado y procesando tu audio.'
    Write-Ok 'Abre la app y mueve REALCE DE PASOS: se aplica en ~1 segundo.'
} else {
    Write-Host @'
================================================================
  NO CARGO
================================================================
'@ -ForegroundColor Red
    Write-Err 'Windows no ha cargado el APO.'
    Write-Err 'Revisa C:\ProgramData\WarzoneAudioOptimizer\apo.log:'
    Write-Err '  - si hay lineas "PROCESANDO", si cargo y el problema es otro'
    Write-Err '  - si no hay nada nuevo, el audio no pasa por ese endpoint'
}

Write-Host "`n--- Ultimas lineas del log del APO ---" -ForegroundColor DarkGray
$log = 'C:\ProgramData\WarzoneAudioOptimizer\apo.log'
if (Test-Path $log) { Get-Content $log -Tail 12 } else { Write-Host '  (sin log)' }
