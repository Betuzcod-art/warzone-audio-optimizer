# Paquete para distribuir

Lo que se le envia a otra persona. Sustituye a `installer/`, que instalaba
VB-CABLE y corresponde a la arquitectura anterior (la que tenia delay).

## Diferencia con `installer/`

| | `installer/` (obsoleto) | `dist/` (actual) |
|---|---|---|
| Motor | VB-CABLE + app propia | Equalizer APO |
| Delay | ~20 ms | ninguno |
| Requiere | driver virtual | Equalizer APO instalado antes |

## Contenido

- `WarzoneAudioOptimizer.exe` -- la app
- `INSTALAR.cmd` -- se autoeleva, comprueba que Equalizer APO este
  instalado, copia la app, concede permiso de escritura sobre la carpeta
  de configuracion y crea los accesos directos
- `DESINSTALAR.cmd` -- vacia el config de Equalizer APO antes de borrar,
  para no dejar filtros aplicados sin app que los controle
- `ARREGLAR AUDIO.cmd` -- reinicia los servicios de audio de Windows
- `LEEME - EMPIEZA AQUI.txt` -- instrucciones para el usuario final

## El punto que mas falla

Equalizer APO pregunta durante su instalacion sobre que dispositivos
actuar. Si el usuario no marca sus auriculares, todo se instala sin error
y no se oye ningun cambio. El LEEME lo advierte tres veces por eso.

## Regenerar el ZIP

```powershell
Compress-Archive -Path "dist\Warzone Audio Optimizer\*" `
  -DestinationPath "Warzone Audio Optimizer.zip" -Force
```
