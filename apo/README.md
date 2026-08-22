# Warzone Audio Optimizer — versión APO

Procesa el audio **dentro del motor de audio de Windows**, sin cable virtual
ni dispositivo intermedio.

## Por qué existe esta versión

La app original captura el audio ya reproducido desde un cable virtual
(VB-CABLE), lo procesa y lo vuelve a reproducir. Ese camino arrastra:

- un dispositivo virtual extra en la cadena,
- dos relojes de hardware distintos (con su deriva entre ellos),
- un ring buffer entre ambos para absorber esa deriva,
- y el período del motor compartido contado **dos veces**.

Medido en el equipo de desarrollo, el dispositivo reporta
`min = def = fund = max = 480 frames` (10 ms): **no ofrece período reducido**,
así que el modo compartido no puede bajar de ahí por mucho que se le pida. Ese
era el delay que no se podía afinar — es estructural, no un parámetro mal
puesto.

Un APO no añade ninguna de esas etapas. Windows llama a `APOProcess()` con el
bloque que ya iba a reproducir, se modifica en el sitio y sigue su camino.

## Lo que debes saber antes de instalar

**El instalador desactiva la verificación de firma de APOs de Windows**
(`DisableProtectedAudioDG = 1`). Es obligatorio para cargar un APO sin
certificado WHQL — es el mismo mecanismo que usa Equalizer APO.

- **No toca Secure Boot ni el arranque**, así que el requisito de Secure Boot
  de Warzone se mantiene intacto.
- **Riesgo con anti-cheat**: no hay forma de garantizar cómo reacciona un
  anti-cheat de kernel (Ricochet) ante esta configuración. Es una decisión
  informada de quien lo instala.

## Instalación

Desde PowerShell **como administrador**:

```powershell
.\install\install-apo.ps1
```

El script:
1. Pide confirmación explícita.
2. Lista los dispositivos de salida activos, con su id y avisando cuáles ya
   tienen un efecto de fabricante que se reemplazaría.
3. **Guarda la configuración original** antes de tocar nada.
4. Copia y registra la DLL, la asocia al dispositivo elegido.
5. Reinicia el servicio de audio.

## Si te quedas sin audio

```powershell
.\install\uninstall-apo.ps1
```

Restaura el efecto original del dispositivo, devuelve la verificación de firma
a su estado previo, y desregistra la DLL. Está escrito para funcionar **incluso
si el backup se perdió**: en ese caso barre todos los dispositivos quitando la
asociación del APO.

Si aun así no hay sonido, reinicia Windows — algunos cambios del motor de audio
solo se aplican del todo tras reiniciar.

## Ajustes

El APO lee los mismos ajustes que la app de escritorio, desde
`%APPDATA%\WarzoneAudioOptimizer\settings.ini`. Los sliders de la app siguen
siendo el panel de control; el APO relee el archivo cada vez que el motor de
audio lo inicializa.

> Nota: un cambio de ajustes se aplica cuando el motor reinicializa el APO
> (cambio de dispositivo, de formato, o reinicio del servicio de audio), no
> instantáneamente como en la app.

## Detalles de implementación

La cadena DSP es **exactamente la misma** que la de la app
(`../audio-optimizer/include/dsp/`), compilada dentro de la DLL. Un solo sitio
donde afinar el sonido.

Restricciones que impone correr dentro de `audiodg.exe`:

- `APOProcess()` corre en tiempo real estricto: **prohibido** reservar memoria,
  tomar locks, hacer I/O, lanzar excepciones o tocar memoria paginable. Toda la
  reserva ocurre en `LockForProcess()`, y `WarzoneAudioChain::process()` está
  escrito para no reservar nunca (ver `reserve()`).
- **CRT enlazado estáticamente**: depender de los redistribuibles de VC dentro
  de un proceso protegido es una causa clásica de fallo de carga.
- **Sin manifiesto embebido**: Microsoft documenta que un manifiesto embebido
  activa APIs prohibidas en el entorno protegido de audio, y el APO dejaría de
  cargar en cuanto se restaure la protección.
