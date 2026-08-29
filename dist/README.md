# Paquete para distribuir

Lo que se le envia a otra persona. Sustituye a `installer/`, que instalaba
VB-CABLE y corresponde a la arquitectura anterior (la que tenia delay).

## Contenido

| Archivo | Que hace |
|---|---|
| `INSTALAR.cmd` | Se autoeleva e instala motor + app |
| `EqualizerAPO-x64-1.4.2.exe` | El motor. GPL-3.0, sin modificar. **No versionado** (11 MB) |
| `WarzoneAudioOptimizer.exe` | La app |
| `DESINSTALAR.cmd` | Vacia la config antes de borrar; ofrece quitar el motor |
| `ARREGLAR AUDIO.cmd` | Reinicia los servicios de audio |
| `LEEME - EMPIEZA AQUI.txt` | Instrucciones para el usuario final |
| `LICENCIAS.txt` | Aviso GPL-3.0 y enlace al codigo fuente |

Para regenerar el paquete hay que volver a poner el instalador de
Equalizer APO en la carpeta: no esta en git.

## Como instala el motor

El instalador de Equalizer APO es NSIS, asi que admite `/S`. Se instala en
silencio y despues se lanza `DeviceSelector.exe` por separado.

Esa separacion es deliberada. En modo silencioso se salta el dialogo de
seleccion de dispositivos, y **sin un dispositivo marcado todo queda
instalado sin dar ningun error y no se oye ningun cambio** -- el fallo mas
comun y el mas dificil de diagnosticar para quien lo sufre. Lanzandolo
aparte se puede poner una explicacion propia justo delante, en vez de
dejarlo enterrado entre las pantallas de un instalador ajeno.

## Si el motor ya estaba instalado, no se toca

`INSTALAR.cmd` comprueba `EqualizerAPO.dll` y salta el paso entero si
existe. Reinstalar encima podria borrar los dispositivos ya seleccionados
y dejar a alguien sin sonido procesado sin saber por que.

Efecto util: el paquete se puede probar en un equipo que ya lo tenga
funcionando sin riesgo de romperlo.

## El desinstalador vacia la config primero

Antes de borrar nada escribe un `config.txt` vacio. Si no, los filtros
seguirian aplicandose sin ninguna app que los controle, y el audio
quedaria alterado sin rastro visible de la causa.

## Licencia de terceros

Equalizer APO es GPL-3.0 y se redistribuye sin modificar. `LICENCIAS.txt`
recoge el aviso y el enlace al codigo fuente, como exige la licencia.

## Regenerar el ZIP

```powershell
Compress-Archive -Path "dist\Warzone Audio Optimizer\*" `
  -DestinationPath "$([Environment]::GetFolderPath('Desktop'))\Warzone Audio Optimizer.zip" -Force
```

## Empezar de cero

`BORRAR TODO Y EMPEZAR DE CERO.cmd` (+ `limpiar.ps1`) desinstala ambos
programas y borra su configuracion, para cuando una instalacion previa
quedo a medias.

Dos detalles que obligaron a usar un script y no un comando suelto:

- **La ruta no siempre es la misma.** Se lee `UninstallString` del
  registro en vez de asumir `C:\Program Files\EqualizerAPO`.
- **El desinstalador NSIS lanzado con `/S` se desprende del proceso
  padre**, asi que `-Wait` por si solo no espera a nada. Se pasa el
  parametro `_?=<dir>`, que lo obliga a ejecutarse en sitio, y aun asi se
  verifica el resultado antes de continuar.

Termina comprobando que `Audiosrv` y `AudioEndpointBuilder` sigan
corriendo, y arrancandolos si no. Es la secuencia que dejo el equipo sin
audio varias veces durante el desarrollo.

Hay que **reiniciar antes de reinstalar**: el APO no se descarga del motor
de audio hasta entonces, y una instalacion nueva encima queda a medias.

## El error de Qt, y por que ocurria

Sintoma en el equipo de un usuario:

> This application failed to start because no Qt platform plugin could be
> initialized.

`start /wait` no espera de verdad a un instalador NSIS: el proceso lanzado
puede desprenderse y retornar mientras la instalacion sigue copiando
archivos. El script daba por terminada la instalacion y lanzaba
`DeviceSelector.exe` en ese hueco, cuando `qt\platforms\qwindows.dll`
todavia no existia.

Contribuia la comprobacion: se miraba `EqualizerAPO.dll`, que aparece
pronto, en vez de un archivo tardio.

Arreglo: se espera al archivo concreto que hace falta
(`qt\platforms\qwindows.dll`), no al proceso, con un limite de 90 s. Si se
agota, se dice que ejecuten el instalador a mano en vez de seguir sobre
una instalacion incompleta.

Ademas:
- Una instalacion a medias (DLL si, Qt no) se detecta y se manda a limpiar
  en vez de intentar arreglarla encima.
- `DeviceSelector.exe` se lanza con `/D` fijando el directorio de trabajo:
  Qt busca sus plugins en rutas relativas y heredar el directorio de donde
  se descomprimio el ZIP puede impedir que los encuentre.
- Si aun asi no abre, se explica como hacerlo desde el menu inicio en vez
  de dejar al usuario con un error crudo.

Es el mismo fallo que ya aparecio en el desinstalador, donde se resolvio
con el parametro `_?=` de NSIS. Conviene desconfiar de `start /wait` con
cualquier instalador NSIS.
