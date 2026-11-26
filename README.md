# fzerox-naomi

F-Zero X en la **NAOMI 2** (mueble del Crazy Taxi), a partir de la [decompilación de N64](https://github.com/inspectredc/fzerox).
No hay ningún port de F-Zero X a Dreamcast: la capa Dreamcast se hace aquí. El método es el de Mario Kart 64 en
[dc2naomi](https://github.com/0skarzer0/dc2naomi) (`homebrew/mk64/`) y el de Diddy Kong Racing en
[dkr-naomi](https://github.com/0skarzer0/dkr-naomi): el juego se compila contra una KallistiOS parcheada para NAOMI
(`NAOMI_HOST`) y dc2naomi lo empaqueta como imagen de NAOMI.

**Estado: fase 0 hecha** (2026-11-26, ver [docs/plan.md](docs/plan.md)). El script saca los assets de la ROM y compila
todo el código del juego para SH-4 sin errores; todavía no hay ejecutable. Siguiente: libultra sobre KallistiOS y
los segmentos, para enlazarlo y arrancarlo en Flycast.

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

Opciones en la cabecera del script (`WORK`, `KOS`, `JOBS`). `KOS=$HOME/dkr-build/kos-dkr` reutiliza la KallistiOS ya
compilada por dkr-naomi (es la misma versión con el mismo parche).

## Agradecimientos

- [inspectredc](https://github.com/inspectredc/fzerox) y el resto de la decompilación de F-Zero X.
- [Bruceleeto](https://github.com/Bruceleeto/Diddy-Kong-Racing): el port de Diddy Kong Racing a Dreamcast, base del
  renderer.
- [Zorkats](https://github.com/Zorkats/G-Diffuser) (G-Diffuser, port a PC) y
  [cruxxxxxx](https://github.com/cruxxxxxx/gdx-3ds) (port a 3DS): referencia para el little-endian y el rendimiento.
- [HarbourMasters](https://github.com/HarbourMasters/Torch) (Torch) y [splat](https://github.com/ethteck/splat).
- [KallistiOS](https://github.com/KallistiOS/KallistiOS).
