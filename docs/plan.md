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
- **Audio:** HLE propio del microcódigo de audio en la CPU (`port/src/audio_hle.c`; ABI de la familia "nead", la de
  Mario Kart 64 y Zelda, no la de Diddy) y salida por `snd_stream` en modo directo.
- **60 fps:** el juego hace su lógica a 60 Hz con 30 naves (Diddy a 30 Hz con interpolación). Plan B: lógica a 60 y
  dibujo a 30.

## Fases

0. **Compilar** (este repo): `build_fzerox.sh` saca los assets de la ROM con las herramientas de la decomp y compila
   todo el código del juego para SH-4. Lista de lo que falta para enlazar (libultra, símbolos de segmentos).
1. **Enlazar y arrancar sin dibujar:** libultra sobre KallistiOS (hilos, colas de mensajes, DMA desde la "ROM",
   VI, mandos), la memoria de N64 reproducida; log en Flycast. **Hecha** (ver abajo).
2. **Renderer F3DEX2→PVR:** título y menús. **Hecha, y la carrera ya se ve** (ver abajo).
3. **Audio.** **Hecha** en Flycast (ver abajo).
4. **Una carrera en Flycast** y medida en placa (`naomi_med`).
5. **Rendimiento:** pista y naves por el camino nativo (como gdx-3ds y Diddy), hasta 60 fps o el plan B.
6. **Mueble:** mandos (hecho el mapa `fzx`, ver la fase 3; falta probarlo), partidas en la VMU (hecho, falta
   probarlo), perfil de dc2naomi y prueba en placa.

## Estado

### Fase 3 (2026-11-27): hecha, en Flycast; suena

Música y efectos del título y de la carrera. En Flycast la salida (sacada a WAV con `tools/audio_wav.py`) es señal
continua, sin saturar, al ritmo de 32 kHz (≈31 800 cuadros/s), y el HLE gasta ≈5 % de la CPU (medida de Flycast, no
vale para la placa: allí, `gPortAudioUs` contra `gPortVblankCount`). **Sin probar en placa.**

- `port/src/audio_hle.c`: las órdenes que usa `synthesis.c` (ADPCM, 8 bits, remuestreo, envolvente y mezcla en
  seco/húmedo, mezcla, entrelazado, cargas y guardados), con los campos de `include/PR/abi.h`. La "DMEM" es un
  búfer de 4 KB: muestras en el orden del SH-4 y datos comprimidos byte a byte, así que no hay que dar la vuelta a
  nada. El remuestreo es una cúbica propia, no la tabla de la consola.
- Salida (`plat_kos.c`): `osAiSetNextBuffer` copia a una cola y un hilo rellena un stream estéreo de KOS. El stream va
  en **modo directo** (`snd_stream_set_callback_direct`: escribimos la RAM del AICA con las store queues) porque el
  camino normal usa la DMA de G2, que en la NAOMI no acaba nunca. `osAiGetLength` dice lo que queda en la cola menos
  una reserva de 1280 cuadros, y el juego (`thread.c`) ajusta con eso cuánto genera: así queda margen para el stream.
- Depuración: `gPortAudioCmds`/`Bad`/`Frames`/`Peak`/`Underruns`/`Overruns`/`Us` y los últimos 4 s de salida en
  `gPortAudioCapture` (`tools/audio_wav.py RAM ELF salida.wav`).

Trampas (todas en `decomp/fzerox-naomi.patch` salvo la primera):
- **Datos inicializados a cero en `.bss`:** GCC manda ahí `static u8 padding0[8] = {0}`, y el banco de instrumentos
  compilado (`audio_bank.c`, con desplazamientos fijos) quedaba descuadrado: instrumentos y muestras leídos 8 bytes
  o más corridos, ninguna nota. La decomp compila con `-fno-zero-initialized-in-bss`; ahora el port también
  (`GAME_CFLAGS`). Con el sonido, el título espera a la voz y entra en la demo de carrera.
- **`AudioHeap_AllocPermanent` sin `return`:** en MIPS el puntero sigue en `$v0`; en el SH-4 salía basura y la
  secuencia apuntaba a `0x3C`. Buscadas todas las funciones así (`-Wreturn-type` con `-c`, no con `-fsyntax-only`,
  que no lo avisa): las demás cubren todos los casos o nadie usa lo que devuelven.
- **Órdenes de audio (`AudioCmd`):** se escriben como palabra (`op << 24 | ...`) y se leen como bytes, y los
  argumentos de 8/16 bits se ponen en la parte alta (`QueueCmdS8`/`U16`). En little-endian van al revés.
- **`AdsrState.action`:** campos de bits y además `asByte |= 0x10/0x20`; en little-endian el primer campo es el bit 0.
- **Big-endian en las secuencias:** `gSequenceFontTableData` (bytes con `S16()`, leídos como `u16[]`) y dos lecturas
  de 16 bits de la secuencia en `seqplayer.c`.
- El banco no hace falta darlo la vuelta: Torch lo saca como estructuras de C (`Sample`, `Instrument`...).

Mandos del mueble (fase 6 adelantada, **sin probar**): mapa `fzx` del residente de dc2naomi (`DIMM_PAD_FZX`), el
esquema del usuario: START al soltarlo (si no se usó para otra cosa), palanca abajo = A (acelerar), palanca arriba =
B+A (turbo), freno = Z, acelerador = R, volante = stick (2,5x en el centro, como MK64); con START pulsado: palanca =
stick arriba/abajo, freno = C-abajo, acelerador = Z+R, volante a la derecha = C-derecha y a la izquierda =
C-arriba. El port lee el mando de Dreamcast así: A, B, X = C-abajo, Y = C-arriba, C = C-derecha, Z = C-izquierda,
gatillos = Z/R.

Pendiente visto: la **demo del título** se ve sin circuito (solo el HUD); la carrera normal sí se ve.

**Primera prueba en placa (2026-11-27, .251):** arranca y se juega (el usuario probó; contadores leídos por la
NetDIMM con `pk.py`): vblank a 60/s, pero el título iba a **~14 cuadros/s** (fase 5) y **sin sonido**: la cola de
salida funcionaba (302 s enviados, el HLE solo un 1,5 % de CPU) pero con pico 0, sin secuencias ni bancos cargados.
Causa: `AudioLoad_Init` pone a cero el contexto de audio (búfer de órdenes, posiciones) y vuelve a crear la cola;
en N64 el hilo de audio (20) acaba esa inicialización antes de que corra el del juego (10), que nada más arrancar
encola las secuencias (`Audio_GuitarSeqStart`). En KOS ese orden no está garantizado (prioridades que envejecen, el
audio se bloquea en sus DMA) y en la placa las órdenes se perdían; en Flycast salía bien por suerte. Arreglos: el hilo
del juego espera a `gAudioContextInitialized` (`sys_gfx.c`), y `osStartThread` cede el turno a un hilo nuevo con más
prioridad que el que lo arranca hasta que se bloquea, como en N64. Imagen nueva subida, **sin reprobar**.

**La nave no corría** (en placa "va lento" y, con un turbo o una placa de aceleración, desaparecían nave y pista):
el juego lee constantes de física de cada nave **de la cabecera y el arranque del cartucho** (`ROM_READ(0x0A0)`...
en `racer.c`, y en dos overlays), un truco contra copias. En el SH-4 esa dirección daba 0: `unk_1BC = 0` y
`unk_324 = 1/0 = inf`, velocidad máxima de unos 17 km/h, y con un turbo la física pasaba a NaN (todo fuera de la
imagen). Ahora `ROM_READ` llama a `fzx_cart_read` (los 4 KB de `BOOT.BIN`, que `build_fzerox.sh` saca de la ROM del
usuario) e `IO_READ` (el juego solo mira `PI_STATUS` antes) devuelve 0: antes leía la memoria de vídeo de la NAOMI,
con riesgo de quedarse esperando. En Flycast la nave pasa de 700 km/h por las placas y el cartel de la carrera dice
"GP RACE / NOVICE" (el "BAC9D" también salía de ahí). Prueba sin mandos: `make PORT_EXTRA=-DFZX_AUTOPAD` (en carrera,
A siempre y B cada 2 s).

**Rendimiento, primera pasada (2026-11-27, solo Flycast):** en placa el título iba a ~14 cuadros/s. Medido con un
perfil por cuadro en el log (`fzx: perfil (us/cuadro)`: lista, hash de texturas, estados, vértices, envío, audio)
y con **muestreo de PC** (`plat_kos.c` guarda PC y PR en cada vblank en `gPortPcProf`; `tools/pcprof.py RAM ELF`
los agrupa por función; vale igual con un volcado de la placa). Lo que costaba y lo hecho:
- **Hash de texturas** (60-70 % del título): FNV byte a byte de cada textura en cada cambio de estado. Ahora por
  palabras, una de cada 8 en las grandes, y como mucho una vez por textura y cuadro (memo por dirección).
- **Combinador por vértice**: cada vértice de cada triángulo evaluaba el combinador dos veces reconvirtiendo
  prim/env (8 divisiones por entrada). Ahora se "compila" por estado (índices a una tabla de registros) y el
  resultado en pantalla de cada vértice se reutiliza mientras no cambie lo que usa (`sScrGen`).
- **Copias**: `draw_tri` trabaja con punteros (solo copia al recortar), `gb_triangle` escribe en la cola sin
  `memcpy`, la cola va a la PVR en bloques de 256 unidades (antes, una llamada por cada 32 bytes), `gb_set_state`
  no recompila si el estado es igual, `fzx_is_mio0` con memo.
Resultado en Flycast: título 16,5 -> 7 ms de lista (el cuadro lo marca el vblank); un menú pesado 33,8 -> 19,4 ms;
la carrera con 60 % de tiempo libre. En placa la memoria pesa más que en Flycast (el hash, sobre todo), así que la
mejora real hay que medirla allí.
(Un muestreo del hash, 1 palabra de cada 8, se probó y se quitó: dos textos casi vacíos en la misma dirección de
la arena daban el mismo hash.)

Más arreglos de la misma noche:
- **La demo del título** ya se ve entera (circuitos, 2 y 4 jugadores en pantalla partida): el circuito negro era la
  física en NaN de `ROM_READ`.
- **Cadenas en EUC-JP:** la decomp compila las cadenas del juego en EUC-JP (su asm-processor convierte desde
  UTF-8) y el port las dejaba en UTF-8: el cursor "Ｍ" del menú de pausa salía como 7 glifos ("VVMMVVV" encima de
  "Continue"). Ahora `-fexec-charset=EUC-JP` (Shift-JIS para `leo_fault.c`, como en la decomp).
- CI con la TLUT apagada se lee como intensidad (como el RDP).

- **Partidas en la VMU:** la SRAM (récords, fantasmas, lo desbloqueado) se carga de `FZEROX.SRM` al arrancar y se
  guarda 2 s después de la última escritura del juego (65 bloques). Guardar, visto en Flycast; cargar, sin probar.
- `G_BRANCH_Z` hecho (Z de pantalla contra el umbral), aunque en las carreras vistas el juego no lo usa.

Pendiente: en pausa la nave se ve celeste y medio transparente; niebla y tijera (`gDPSetScissor`: la animación del
menú de pausa y la pantalla partida de VS) sin hacer; medir en placa y probar los mandos `fzx` y la VMU.

**Segunda prueba en placa (2026-11-27, mañana):** menú principal y música bien; la selección de nave lentísima
(~18 cuadros/s: 52 ms de lista); en carrera lento pero jugable; sin sonido de turbo; el fondo se veía a través de la
carretera; la barra de energía negra; el choque y la recarga, un cuadrado/rombo con un dibujo.

Medido en la placa con el muestreo de PC (bajado por la NetDIMM con `pk.py` a trozos de 2048 palabras; **ojo**: al
leerlo hay que conservar todas las palabras, un filtro que se salte una cambia PC por PR): en la placa no quedaba
nada libre y un tercio se iba en el **color por vértice** (`combine_eval` x2 + `pack` + `to_screen`, ~950
instrucciones por vértice), un 14 % en `do_vtx` y un 8 % en recompilar el combinador. En Flycast esas pantallas iban
sobradas: Flycast no sirve para medir esto.
- El combinador se compila por estado a un **polinomio** por canal, `c0 + cs·s + ct·t + cst·s·t` (s, una componente
  de la shade; t, la textura, 0 o 1): el color de cada vértice son dos multiplicaciones por canal. Lo que no cabe
  (shade por shade, dos componentes distintas) sigue por `combine_eval`. `make PORT_EXTRA=-DFZX_POLYCHECK` calcula
  los dos y cuenta diferencias (`gPortPolyChk`): 0 en 4,5 millones de vértices, título, demo y carrera.
- `pack` sin conversiones a `unsigned` (en el SH-4 son rutinas largas); `do_vtx` multiplica por 1/255 y normaliza
  con `fsrra`; el color solo de los triángulos que no se descartan; el combinador no se recompila si no cambió;
  `trace()` es una macro (sin llamada si no se traza); fuera el cronómetro por `G_VTX`.
Resultado en la placa (demo del título): lista 17,5 -> 10,7 ms, la de 1 jugador a 60 cuadros/s y un 72 % de CPU
libre en la mezcla de título y demo. La selección de nave, sin volver a medir.

Lo visto en la carrera (todo comprobado en Flycast con el piloto automático, `FZX_AUTOPAD`, que ahora pulsa START él
solo hasta llegar a la carrera; trazas de un cuadro con `-DFZX_TRACE_MODE=1 -DFZX_TRACE_AT=n`):
- **Fondo a través de la carretera:** el cielo y las torres son translúcidos, sin Z y se dibujan los primeros; en la
  PVR la lista translúcida va después de la opaca y quedaban encima. Lo translúcido sin Z dibujado antes que nada con
  Z (`GbState.background`) va con Z casi infinita (2e-8; el plano de fondo de la PVR, a 1e-8) y prueba GEQUAL: lo
  opaco lo tapa.
- **Barra de energía negra:** la barra (rectángulos opacos) se dibujaba bien, pero el marco "ENERGY", translúcido y
  anterior en la N64, la tapaba al ir en la lista translúcida. Lo opaco sin Z dibujado después de lo 3D
  (`GbState.overlay`: el HUD) va también a la lista translúcida, sin mezclar (ONE/ZERO), y conserva el orden.
- **Choque y recarga:** el efecto carga 4 KB de una vez (8 fotogramas de 32x32) y dibuja cada fotograma con su tile;
  solo se encontraba la carga si el tile empezaba justo en su TMEM, y los demás cogían una textura vieja (la de un
  panel a rayas). Ahora un tile dentro de un `LOADBLOCK` toma su trozo (`Load.words`, la carga más reciente gana).
  Las chispas y la explosión ya se ven; la burbuja de recarga, sin ver (probablemente la misma causa).
- **Recorte:** además del plano cercano, contra w >= 0,1 y una banda de guarda de 3 pantallas, como el RSP: la PVR
  pinta mal vértices a decenas de miles de píxeles.

**Tercera prueba en placa (2026-11-27, noche): el turbo ya suena.** No se oían el turbo (el propio y el de los
rivales), el derrape, la alarma de poca energía ni la explosión; el resto de efectos de la nave sí (mismo canal 10).
Medido en la placa con contadores temporales por efecto: esas notas se repartían, con una muestra correcta (igual
byte a byte que `AUDIOTBL.BIN`) y señal hasta el mezclador, pero con volumen 0 casi todo el tiempo (el turbo, un 13 %
de las actualizaciones con volumen; el derrape y la alarma, 0 %; los que se oían, ~100 %). Flycast hacía lo mismo.
Causa: las **envolventes que van dentro de las secuencias** (`ASEQ_OP_LAYER_ENV` y `ASEQ_OP_CHAN_ENV` apuntan a
bytes de la secuencia) quedaban en el orden de N64; las de los bancos ya están en el de la SH-4 porque los bancos se
compilan desde C. `{1, 32700}` se leía `{256, -17537}` y la nota nacía muda. Arreglo (parche de la decomp,
`seqplayer.c`, `AudioSeq_SeqEnvelope`): se usa una copia convertida por dirección de origen (48 huecos de 16 puntos),
que se compara en cada uso por si en esa dirección hay otra secuencia. Probado en placa: se oyen todos.

Sigue pendiente:
- Un **panel blanco** sobre la pista de Mute City en algunos tramos; sin encontrar.
- **Lentitud de la selección de copa (Mute City en miniatura) a la de nave** (la del Blue Falcon girando va bien).
  En Flycast, la miniatura son ~3.750 triángulos por cuadro (el doble que una carrera): la mitad del tiempo en
  dibujarlos y un 12 % en cambios de estado. Medirla en placa (muestreo de PC con `FZX_AUTOPAD_HOLD`, que deja el
  piloto automático quieto en un modo de juego dado tras `FZX_AUTOPAD_EXTRA` pulsaciones de START).

### Fase 2 (2026-11-27): hecha, en Flycast; la carrera ya se ve

En Flycast se ve todo lo que se ha probado: título (logotipo sobre el cómic, "PUSH START"), menú principal,
dificultad, selección de copa y de circuito, **selección de nave con las 30 naves en 3D**, ajustes de la nave (el
Blue Falcon en 3D girando) y **la carrera de Mute City**: circuito, cielo, naves, HUD completo (vuelta, posición,
tiempo, energía, retratos, minimapa) y la cuenta atrás. La nave acelera y gira con el mando. Sin sonido (fase 3) y
**sin probar en placa**. Los fps de Flycast no valen como medida.

Renderer (`port/src/gfx_rsp.c` + `gfx_pvr.c`, interfaz en `gfx_backend.h`):
- Intérprete F3DEX2 propio (vale para las variantes "Rej" que usa el juego): pila de matrices, viewport, luces y
  texgen, recorte por el plano cercano, descarte de caras; RDP: tiles, `LOADBLOCK`/`LOADTILE`/`LOADTLUT` (sin emular la
  TMEM: se recuerda de qué dirección de RAM sale cada carga), combinador, mezcla, profundidad, rectángulos.
- Combinador evaluado por vértice con TEXEL0 = 0 y = 1: la PVR hace textura × (f1 − f0) + f0 con color base y
  desplazado (idea de cruisn-usa-dreamcast, escrita de nuevo).
- PVR: lista opaca y translúcida en modo presort (en el orden de envío, como la N64); caché de texturas en VRAM
  (clave: dirección, formato y hash del contenido), todos los formatos de N64 a ARGB1555/4444.
- Depuración: `gPortTraceFrame`/`gPortTraceMode` trazan un cuadro entero (estados, triángulos, sublistas, matrices,
  viewport) a `gPortTrace`; `gPortBadDL` guarda la historia de órdenes en la primera desconocida;
  `gPortDmaLog`/`gPortMio0Log`/`gPortWaitQ` (DMA, descompresiones y en qué cola espera cada hilo); el vigilante de
  `plat_kos.c` imprime los hilos si el juego deja de avanzar. Se leen con `tools/live_ram.py`.

Trampas de la fase (todas resueltas; las del juego, en `decomp/fzerox-naomi.patch` bajo `NAOMI_HOST`):
- **Big-endian en el juego:**
  - `*(s32*) p == 'MIO0'`: en little-endian nunca es cierto y el juego ponía las texturas a cero. Ahora,
    `MIO0_MAGIC_AT(p)` (byte a byte).
  - `Mtx`: `Mtx_t` empaqueta dos elementos por palabra de 32 bits (lo usan `guMtxF2L` y `gdSPDefMtx`), pero
    `math.c`/`camera.c` escriben por `u.i[r][c]`/`u.f[r][c]`. En little-endian ese elemento es el `c ^ 1`:
    `MTXH(c)` en `gbi.h`.
- **Segmentos que en la ROM de N64 van en MIO0** y Torch saca descomprimidos (`machine_models`, texturas de
  circuito, `course_track_gfx`): el juego solo los descomprime y, sin MIO0, dejaba el segmento vacío. Ahora se
  copian tal cual. `sys_gfx.c` llamaba a `mio0Decode` sin mirar; ahora `mio0Decode` rechaza lo que no es MIO0.
- **Texturas de 16 bits:** las de C (`u16[]`) están en little-endian y las que salen de MIO0, en big-endian.
  `mio0Decode` apunta lo que escribe (`fzx_is_mio0`) y una DMA encima lo borra. Las de 8 y 4 bits son bytes.
- **Planificador de KOS master** (envejece las prioridades): el `while (true) {}` del hilo Idle le quitaba el turno
  al hilo principal hasta bloquearlo. Ahora el Idle duerme (`fzx_idle`).
- **Cola del hilo principal llena:** con cuadros largos, los retrazados de VI llenaban sus 16 huecos y se perdían
  los eventos SP/DP. El juego creía el RSP ocupado y se paraba. El vblank deja 4 huecos.
- `LOADBLOCK`: `uls`/`ult` son el texel de inicio en la imagen (las tiras de los fondos los usan).
- Conversión de la matriz: `(s16 << 16) | u16` es `u32` en C; sin `(s32)`, los negativos salían como 65535,x.

Pendiente visto en esta fase: el cartel "GP RACE / RACE n" sale "BAC9D" (arreglado en la fase 3: `ROM_READ`), recuadros magenta en el menú de pausa,
LOD (`G_BRANCH_Z` siempre al detalle cercano), niebla (sin hacer), tijera (sin hacer). El rendimiento real se mide
en placa (fases 4 y 5).

### Fase 1 (2026-11-26): hecha, en Flycast

`build_fzerox.sh ROM [SALIDA.bin]` hace la imagen de NAOMI entera (perfiles `fzerox` / `fzerox_dbg` de dc2naomi).
En Flycast (BIOS NAOMI, imagen `fzerox_dbg`) el juego arranca, crea sus hilos, llega al **título**
(`gGameMode = 0x8000`, `GAMEMODE_FLX_TITLE`) y con START pasa al **menú principal** (`0x8007`): el mando llega, el
juego carga sus overlays con su propia DMA y genera listas de visualización (unas 1 700 tareas de gráficos y 1 900
de audio en 45 s). No hay imagen (nadie dibuja las listas) ni sonido: fases 2 y 3. **Sin probar en placa.**

**Memoria** (lo más importante de la fase; `port/gen_ld.py`). El juego reparte la RAM de la N64 a mano:
`sys_gfx.c` pone los búferes de assets "detrás de `buffers`" y `machine_global_gfx` detrás de `ovl_i3`, `segment.c`
tiene sus arenas hasta direcciones fijas (`0x801D9800`, `0x803DA800`) y los overlays comparten dirección. La primera
versión, con cada overlay en su sitio, se machacaba el código (`machine_custom_gfx` copiado encima de `save.c`:
instrucción ilegal en `Sram_ReadWrite`). Ahora se reproduce la N64:

| Zona | Qué | Cómo llega |
|---|---|---|
| `0x8c010000` | KallistiOS y la capa de NAOMI (`port/src`) | `1ST_READ.BIN` (217 KB) |
| `__fzx_n64` (`0x8c050000`, 4 MB + 64 KB) | ventana de RDRAM: cada segmento en su dirección de N64 desplazada (`0x80xxxxxx` → `__fzx_n64 + xxxxxx`), los overlays solapados | `main()` la pone a cero y copia `entry` y `main`; el resto lo copia el juego |
| `__fzx_rom_start` | la "ROM": los bytes de todos los segmentos en el orden de la ROM de N64 (código, datos y assets) | `ROM.BIN` (5,25 MB) |
| `audio_table_ROM_START` | las muestras de sonido | `AUDIOTBL.BIN` (10,7 MB) |
| BSS de KOS y montículo | | `_end` = `0x8d3afee8`: quedan ~8 MB hasta `0x8dc00000` |

- Los assets llevan su dirección segmentada de N64 como VMA y sus bytes en la "ROM". Comprobado: los 198 símbolos
  con dirección en el nombre (`D_A008000_24A270`...) caen en su dirección de N64.
- `osVirtualToPhysical`/`osPhysicalToVirtual` valen tal cual: P1 del SH-4 = físico | `0x80000000`, como KSEG0.
- Las tres direcciones fijas de `segment.c` (las arenas) pasan a `SEGMENT_VRAM_START(framebuffer1/3)` en
  `decomp/fzerox-naomi.patch` (bajo `NAOMI_HOST`).

**libultra sobre KallistiOS** (`port/src/ultra.c`, con las cabeceras de la decomp; `plat_kos.c`, con las de KOS):
- Hilos de N64 = hilos de KOS (prioridad KOS = 20 + 255 − la de N64: todos por debajo de los de KOS, incluido el que
  contesta a la NetDIMM). Colas de mensajes con la semántica de N64 sobre genwait, con `thd_pass()` al despertar a
  otro (en N64 corre ya si tiene más prioridad) y `thd_schedule(true)` en el vblank (lección de Mario Kart 64).
- DMA del PI = `memcpy` desde la "ROM" + `arch_icache_sync_range` (puede ser código: overlays). SRAM: búfer de 32 KB
  sin guardar todavía. 64DD: `osEPiReadIo` dice "no hay disco"; `osMemSize` = 4 MB (sin Expansion Pak).
- RSP: `osSpTaskStartGo` hace la tarea en el momento (`fzx_gfx_task`/`fzx_audio_task`, vacías) y manda SP (y DP).
- Mando: el 0 de Dreamcast traducido a N64 en `plat_pad_read` (provisional: A, B, X = C-izq., Y = C-arriba,
  gatillos = Z/R).
- Lo que queda en ensamblador en la decomp, en C en `port/src/stubs.c`: `mio0Decode`, `func_800AA6BC` (tamaño
  MIO0), `guNormalize`, `_Printf`, `__libm_qnan_f`; microcódigos y 64DD como símbolos vacíos.

Trampas de la fase:
- **El ABI de coma flotante:** esta KOS compila con `-m4-single`; la fase 0 usaba `-m4-single-only`. El juego se
  compila con los `KOS_CFLAGS` tal cual.
- **En SH-ELF los símbolos de C llevan `_` delante** (en MIPS no): `gen_ld.py` se lo pone a los del script de splat.
- **En ld manda la primera regla que encaja:** las de KOS (`*(.text .text.*)`...) iban antes y se llevaban el
  juego. Los objetos del juego se renombran a `.fzxg.*` (`objcopy --prefix-alloc-sections`).
- `src/audio/rom/lib/aisetnextbuf.c` (un `osAiSetNextBuffer` propio que escribe los registros del AI) fuera.
- Los datos, en el orden del fuente (`-fno-data-sections -fno-toplevel-reorder`) y `char` sin signo, como IDO.

Para probar en Flycast (en WSL; la pantalla `:99` puede estar ocupada por otra sesión): `run1.sh` de dc2naomi con
`DUMP=1` y `FC_PEDALS=1`, y `tools/live_ram.py VOLCADO.ram fzerox.elf gGameMode gPortGfxTasks` para el log de KOS,
la excepción si la hay y variables del juego.

**Siguiente (fase 2):** dibujar las listas F3DEX2 en `fzx_gfx_task` con la PVR (base: el renderer de Diddy).
Pendiente de decidir ahí: las texturas que salen de MIO0 están en big-endian y las que están en C (`u16[]`) ya en
little-endian.

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
