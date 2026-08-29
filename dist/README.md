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
