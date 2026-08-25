# Por qué nuestro APO no funcionó, y qué haría falta

Registro de lo aprendido intentando meter la cadena DSP dentro del motor de
audio de Windows. Sirve para retomarlo sin repetir el camino.

## Qué llegó a funcionar

El APO **carga y se ejecuta**: Windows lo instancia y llama a `LockForProcess`.
Eso costó corregir cinco cosas, todas invisibles desde fuera:

| Problema | Solución |
|---|---|
| Ruta de registro equivocada | Va en `HKLM\SOFTWARE\Classes\AudioEngine\AudioProcessingObjects`, no en `MMDevices\...` como dicen los ejemplos INF de la documentación (esa clave no existe en Windows 11) |
| DLL en Program Files | `audiodg.exe` corre como SYSTEM con token restringido y no la carga desde ahí. Va en `System32` |
| Faltaban 12 valores de registro | Windows necesita saber cómo instanciar el APO: conexiones, interfaces, instancias |
| `Flags = 15` | Equalizer APO usa `13`: sin `SAMPLESPERFRAME_MUST_MATCH` |
| Efectos desactivados en el dispositivo | `Disable_SysFx = 1` hace que Windows ignore cualquier APO. **Vive en `FxProperties`, no en `Properties`** — buscarlo donde uno espera no da error, simplemente no aparece nunca |

## Dónde se atascó

Carga, pero al procesar el driver Realtek rechaza lo que devuelve y **la cadena
de audio del dispositivo muere**: el equipo se queda sin sonido, de forma
reproducible. Desinstalar lo devuelve.

## La causa probable

Equalizer APO funciona en ese mismo dispositivo, y su código fuente revela dos
diferencias de fondo:

**1. Extiende `CBaseAudioProcessingObject`.** Nosotros implementamos las cuatro
interfaces COM a mano porque esa clase base vive en el WDK, que no estaba
instalado (el SDK trae los headers `audioenginebaseapo.h` y
`baseaudioprocessingobject.h`, pero no la librería que la implementa). Esa clase
resuelve la negociación de formato y la validación de conexiones, que es
exactamente la parte donde falla lo nuestro.

**2. Encadena en vez de ocupar.** Guarda el CLSID del APO que hubiera en ese
slot, lo instancia como "child APO" y lo llama primero, procesando después su
salida. Nosotros solo ocupábamos un slot libre.

Su `IsInputFormatSupported` acepta el formato si coincide en frecuencia y bits
con la salida, y rechaza solo si haría falta reducir canales.

## Qué haría falta para terminarlo

1. Instalar el **WDK** (Windows Driver Kit) para poder enlazar con
   `CBaseAudioProcessingObject`.
2. Reescribir el APO heredando de esa clase, implementando solo
   `IsInputFormatSupported`, `APOProcess` y `ValidateAndCacheConnectionInfo`
   como recomienda Microsoft.
3. Implementar el encadenado de child APO para no dejar fuera los efectos del
   fabricante.

## Por qué la solución actual usa Equalizer APO

Equalizer APO ya resuelve todo lo anterior y funciona. La app escribe su
configuración, así que se conserva la interfaz propia y se gana el objetivo
principal: **cero delay**.

Lo que se pierde: Equalizer APO solo hace filtros, delay y convolución. **No
tiene procesamiento dinámico** (ni compresor, ni expansor, ni puerta), así que
el realce dinámico de pasos --- que subía la banda solo cuando estaba floja y se
apagaba ante un disparo --- no tiene equivalente. Tampoco soporta plugins VST,
que habría sido la vía para reutilizar la cadena DSP tal cual.

Esa es la razón de fondo por la que los disparos suben junto con los pasos: con
filtros fijos, ambos comparten la banda de 2-5 kHz y nada los distingue ahí. La
única palanca real es atenuar 700-1500 Hz, donde el arma tiene cuerpo y los
pasos casi no.
