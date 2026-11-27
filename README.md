# fzerox-naomi

F-Zero X en la **NAOMI 2** (mueble del Crazy Taxi), a partir de la [decompilación de N64](https://github.com/inspectredc/fzerox).
No hay ningún port de F-Zero X a Dreamcast: la capa Dreamcast se hace aquí. El método es el de Mario Kart 64 en
[dc2naomi](https://github.com/0skarzer0/dc2naomi) (`homebrew/mk64/`) y el de Diddy Kong Racing en
[dkr-naomi](https://github.com/0skarzer0/dkr-naomi): el juego se compila contra una KallistiOS parcheada para NAOMI
(`NAOMI_HOST`) y dc2naomi lo empaqueta como imagen de NAOMI.

**Estado: fase 3 hecha** (2026-11-27, ver [docs/plan.md](docs/plan.md)). En Flycast se ven el título, los menús, la
selección de nave en 3D y **la carrera** (Mute City, con HUD y cuenta atrás), la nave responde al mando y **suenan
la música y los efectos**. Mapa de mandos del mueble de Crazy Taxi hecho. Sin probar en placa.

## Qué hace falta

- Tu ROM: **F-Zero X (USA)**, rev0, `.z64`, MD5 `753437d0d8ada1d12f3f9cf0f0a5171f` (o un `.zip` que la contenga
  sola). En este repositorio no hay nada con copyright: ni ROM, ni assets sacados de ella, ni discos, BIOS o imágenes.
- Ubuntu 22.04 (vale WSL) con el dc-chain de KallistiOS (sh-elf GCC 14 o 15) en `/opt/toolchains/dc/sh-elf`, git, binutils-mips-linux-gnu,
  python3 ≥ 3.10 con venv y un compilador C/C++ del host (para Torch). El script instala en un venv las herramientas de
  Python de la decomp y un CMake ≥ 3.24 (el de Ubuntu 22.04 no compila Torch).

## Compilar

Desde un clon con saltos de línea LF (en WSL, no el de Windows):

```bash
./build_fzerox.sh "/ruta/F-Zero X (USA).z64"
```

Deja `build/fzerox_dc2naomict.bin` y `.bin.gz`. El disco lleva `1ST_READ.BIN` (KallistiOS y la capa de NAOMI),
`ROM.BIN` (código, datos y assets del juego) y `AUDIOTBL.BIN` (las muestras de sonido), que el programa carga en su
sitio al arrancar. Opciones en la cabecera del script (`WORK`, `KOS`, `JOBS`, `D2N`, `KOS_NAOMI_ENV`, `PROFILE`):
`KOS=$HOME/dkr-build/kos-dkr` reutiliza la KallistiOS ya compilada por dkr-naomi (misma versión, mismo parche) y
`PROFILE=fzerox_dbg` saca el log de KallistiOS en pantalla.

Hace falta un clon de dc2naomi con los perfiles `fzerox`/`fzerox_dbg` (`tools/games.py`) junto a este repo o en
`D2N`, y una KallistiOS con `KOS_SUBARCH=naomi` para el loader (`KOS_NAOMI_ENV`).

## Cómo está hecho

- `port/gen_ld.py`: el script de enlazado, a partir del de KallistiOS y del que genera splat para la decomp. Reproduce
  la RDRAM de N64 dentro de la RAM del SH-4 y deja todos los segmentos en una "ROM" (`ROM.BIN`).
- `port/src/ultra.c`: la libultra que usa el juego (hilos, mensajes, DMA, VI, mandos, RSP) sobre KallistiOS
  (`plat_kos.c`).
- `decomp/fzerox-naomi.patch`: los cambios al juego, bajo `NAOMI_HOST`.
- `port/src/gfx_rsp.c` y `gfx_pvr.c`: el renderer (listas F3DEX2 de N64 a la PVR).
- `port/src/audio_hle.c`: el microcódigo de audio en la CPU; la salida al AICA, en `plat_kos.c`.
- `tools/live_ram.py`: lee de un volcado de RAM de Flycast el log de KallistiOS y variables del juego.
- `tools/audio_wav.py`: saca a un WAV los últimos 4 s de sonido de un volcado de RAM.

## Agradecimientos

- [inspectredc](https://github.com/inspectredc/fzerox) y el resto de la decompilación de F-Zero X.
- [Bruceleeto](https://github.com/Bruceleeto/Diddy-Kong-Racing): el port de Diddy Kong Racing a Dreamcast, base del
  renderer.
- [Zorkats](https://github.com/Zorkats/G-Diffuser) (G-Diffuser, port a PC) y
  [cruxxxxxx](https://github.com/cruxxxxxx/gdx-3ds) (port a 3DS): referencia para el little-endian y el rendimiento.
- [HarbourMasters](https://github.com/HarbourMasters/Torch) (Torch) y [splat](https://github.com/ethteck/splat).
- [mupen64plus-rsp-hle](https://github.com/mupen64plus/mupen64plus-rsp-hle): referencia de cómo funciona el
  microcódigo de audio (el HLE de este repo está escrito de nuevo).
- [KallistiOS](https://github.com/KallistiOS/KallistiOS).
