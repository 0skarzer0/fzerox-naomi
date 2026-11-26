# Plan del port de F-Zero X a la NAOMI 2

No hay ningún port de F-Zero X a Dreamcast: la capa Dreamcast la hacemos nosotros. Partimos de la decompilación
(inspectredc/fzerox, CC0) y, para la parte de la PVR, de la capa Dreamcast del port de Diddy Kong Racing de Bruceleeto
(CC0), que ya funciona en la placa con dc2naomi (repo dkr-naomi). G-Diffuser (port a PC, MIT) y gdx-3ds (port a 3DS)
sirven de consulta: cómo resolvieron el little-endian, los datos de la ROM y el rendimiento.

## Decisiones

- **KallistiOS 9926b70a** con el parche `NAOMI_HOST` de dkr-naomi (`kos/kos-9926b70a-naomi_host.patch`, copiado tal
  cual): la que pide la capa de Diddy y la que ya va en placa.
- **Decompilación sin tocar** en un commit fijo; nuestros cambios, en `port/` (y, si hace falta tocar el juego, en un
  parche aparte, todo bajo `NAOMI_HOST`).
- **Versión US rev0**, sin Expansion Kit (64DD).
- **Renderer:** el de Diddy (combinador, texturas, recorte, mallas nativas, FSAA) con un decodificador F3DEX2 delante.
  F-Zero X usa casi solo el modo de 1 ciclo y ninguna mezcla con `TEXEL1`.
- **Audio:** HLE del microcódigo de audio en la CPU (`audio_hle.c` de Diddy o `n64_audio_hle.c` de G-Diffuser;
  comprobar ABI1/ABI2) y salida por `snd_stream`.
- **60 fps:** el juego hace su lógica a 60 Hz con 30 naves (Diddy a 30 Hz con interpolación). Plan B: lógica a 60 y
  dibujo a 30.

## Fases

0. **Compilar** (este repo): `build_fzerox.sh` saca los assets de la ROM con las herramientas de la decomp y compila
   todo el código del juego para SH-4. Lista de lo que falta para enlazar (libultra, símbolos de segmentos).
1. **Enlazar y arrancar sin dibujar:** libultra sobre KallistiOS (hilos, colas de mensajes, DMA desde la "ROM",
   VI, mandos), segmentos y overlays enlazados juntos; log por consola en Flycast.
2. **Renderer F3DEX2→PVR:** título y menús.
3. **Audio.**
4. **Una carrera en Flycast** y medida en placa (`naomi_med`).
5. **Rendimiento:** pista y naves por el camino nativo (como gdx-3ds y Diddy), hasta 60 fps o el plan B.
6. **Mueble:** mandos (volante, pedales, palanca = Z/R, turbo por combinación), partidas en la VMU (el juego guarda
   en SRAM), perfil de dc2naomi y prueba en placa.

## Estado

### Fase 0 (2026-11-26): hecha

`build_fzerox.sh` deja, en WSL (Ubuntu 22.04, sh-elf-gcc 15.3.1, KOS de dkr-naomi):

- **Todo el código del juego compila para SH-4 sin errores**: 96 ficheros (`src/` sin libultra ni el 64DD), los 9 de
  `libultra/gu` (matrices, C puro) y los 28 bloques de assets que genera Torch. `-std=gnu89`, `NON_MATCHING` y
  `AVOID_UB`; ninguna función se queda en ensamblador de MIPS.
- **Tamaño** del enlace parcial: 0,7 MB de código, 4,5 MB de datos (los assets), 2,8 MB de BSS. Unos 8 MB en total:
  cabe en los ~28 MB. Los assets van dentro del binario (como en MK64): un `1ST_READ.BIN` de ~5 MB.
- **Endianness de los assets:** las listas de visualización (`Gfx`) y los vértices (`Vtx`) salen como C tipado:
  correctos en little-endian. Las texturas sin comprimir son `u16[]`: también. Lo que va como bytes (`u8[]`, MIO0
  comprimido, audio) está en big-endian: habrá que darle la vuelta al descomprimir o al leerlo.
- **350 símbolos sin definir** (`$WORK/port-build/undefined.txt`), lo que tiene que dar la fase 1:
  - 223 de segmentos (`*_ROM_START`, `*_VRAM`, `*_BSS_START`...): los pone el script del enlazador de la decomp.
    Habrá que generar los nuestros a partir de los yaml de splat (`yamls/us/rev0/`), con los overlays enlazados juntos.
  - 63 de libultra `os*`: hilos, colas de mensajes, DMA de la "ROM" (`osEPiStartDma`), VI, mandos, caché, tiempo,
    `osPfs*` (Controller Pak, fantasmas), `osMotor*` (Rumble Pak) y RSP (`osSpTask*`).
  - el 64DD que queda en `src/sys/rom/` (`Leo*`, `Mfs_*`): stubs que digan "no hay disco".
  - microcódigos (`gspF3DEX2_fifo*`, `aspMain*`, `rspboot*`) y direcciones segmentadas (`D_1000000`, `D_A00x000`...):
    sin código detrás, solo símbolos.
  - de libultra/libc: `mio0Decode`, `guNormalize`, `bcopy`, `__Printf`, `sprintf`; el resto (`memcpy`, `sqrtf`...)
    lo da newlib/libgcc al enlazar con KallistiOS.

Trampas encontradas (ya resueltas en el script):
- Torch usa `FetchContent(... OVERRIDE_FIND_PACKAGE)`: necesita CMake ≥ 3.24. El 3.22 de Ubuntu 22.04 falla con
  "Failed to get the hash for HEAD" (parece la red, pero no lo es). El script instala `cmake` en el venv.
- El Makefile de la decomp no arranca sin `binutils-mips-linux-gnu`, aunque la extracción no las use.
- Los `*.incbin.c` no se compilan sueltos: los incluye el `.c` de su bloque.

### Referencias para la fase 1

- [cruisn-usa-dreamcast](https://github.com/MoneyShift64/cruisn-usa-dreamcast) (Cruis'n USA, decomp de N64 en
  Dreamcast, 30 fps): `dc/game/plat_dc.c` (libultra sobre KallistiOS), `port/segments.c` y `port/gfx.c` (Fast3D →
  PVR; su truco del combinador: evaluar cada vértice con TEXEL0 = 0 y = 1 y dibujar textura × color + desplazamiento).
  **Sin licencia: solo para leer, no se copia código.**
- G-Diffuser `port/` (MIT): rama `g-diffuser` de su fork de la decomp (55 commits sobre la decomp) para ver qué
  tocaron del juego para little-endian.
- dkr-naomi / Bruceleeto `dreamcast/` (CC0): renderer, `audio_hle.c`, `frame_interp.c`.
