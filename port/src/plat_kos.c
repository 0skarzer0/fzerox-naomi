/*
 * plat_kos.c - la parte de KallistiOS de la capa de N64: hilos, espera, vblank, mando, log y el arranque (main).
 * Se compila con las cabeceras de KOS, nunca con las de la decomp (ver plat.h).
 */
#include <kos.h>
#include <dc/sound/stream.h>
#include <dc/spu.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"

/* ---- interrupciones y espera ---- */

unsigned plat_irq_disable(void) { return (unsigned) irq_disable(); }
void plat_irq_restore(unsigned old) { irq_restore((irq_mask_t) old); }
int plat_in_irq(void) { return irq_inside_int() ? 1 : 0; }

void plat_wait(const void *obj) { genwait_wait((void *) obj, "fzx", 0); }
int plat_wake(const void *obj, int count) { return genwait_wake_cnt(obj, count, 0); }
void plat_yield(void) { thd_pass(); }

/* ---- hilos ---- */

/* N64: 0..255, más alto = más importante. KOS: más bajo = más importante, y los hilos de KOS (por defecto 10,
 * entre ellos el que contesta a la NetDIMM) deben quedar por encima de todos los del juego. */
static prio_t map_pri(int pri) {
    if (pri < 0) pri = 0;
    if (pri > 255) pri = 255;
    return (prio_t) (20 + (255 - pri));
}

typedef struct {
    void (*entry)(void *);
    void *arg;
} start_t;

static void *thread_tramp(void *p) {
    start_t s = *(start_t *) p;
    free(p);
    s.entry(s.arg);
    plat_log("fzx: el hilo %p ha vuelto de su entrada\n", (void *) s.entry);
    return NULL;
}

void *plat_thread_start(void (*entry)(void *), void *arg, int pri, const char *label) {
    kthread_attr_t attr;
    start_t *s = malloc(sizeof(*s));
    kthread_t *t;

    s->entry = entry;
    s->arg = arg;
    memset(&attr, 0, sizeof(attr));
    attr.create_detached = true;
    attr.stack_size = 64 * 1024;
    attr.prio = map_pri(pri);
    attr.label = label;
    t = thd_create_ex(&attr, thread_tramp, s);
    if (t == NULL)
        plat_fatal("fzx: no se pudo crear el hilo %s\n", label);
    return t;
}

/* N64: osStartThread de un hilo con más prioridad lo pone a correr en el acto, y el que lo arranca no sigue hasta que
 * el nuevo se bloquea. KOS no cambia de hilo al crearlo: se cede el turno hasta que el nuevo espera (o 200 ms). */
void plat_thread_yield_until_blocked(void *thread) {
    kthread_t *t = thread;
    int i;

    for (i = 0; i < 200 && (t->state == STATE_READY || t->state == STATE_RUNNING); i++) {
        thd_pass();
        if (t->state == STATE_READY || t->state == STATE_RUNNING)
            thd_sleep(1);
    }
}

void plat_thread_set_pri(void *thread, int pri) {
    thd_set_prio(thread ? (kthread_t *) thread : thd_get_current(), map_pri(pri));
}

void *plat_thread_self(void) { return thd_get_current(); }

void plat_block_forever(void) {
    for (;;)
        thd_sleep(1000);
}

/* el bucle del hilo Idle de N64 (decomp/fzerox-naomi.patch, sys_main.c): dormir en vez de girar */
void fzx_idle(void) {
    thd_sleep(10);
}

void plat_sync_code(void *p, unsigned n) {
    if (n)
        arch_icache_sync_range((uintptr_t) p, n);
}

/* ---- tiempo y vblank ---- */

unsigned long long plat_time_ns(void) { return timer_ns_gettime64(); }

static void (*vblank_fn)(void);

/* Muestreo de PC para perfilar (tools/pcprof.py): en cada vblank, el PC y el PR interrumpidos */
#define PCPROF_N 8192
uint32_t gPortPcProf[PCPROF_N][2];
uint32_t gPortPcProfPos;
int gPortPcProfOn = 1;   /* el piloto de pruebas (FZX_AUTOPAD) lo deja a 1 solo en carrera */

static void vblank_irq(uint32_t code, void *data) {
    irq_context_t *c = irq_get_context();

    (void) code;
    (void) data;
    if (c && gPortPcProfOn) {
        uint32_t k = gPortPcProfPos++ % PCPROF_N;

        gPortPcProf[k][0] = c->pc;
        gPortPcProf[k][1] = c->pr;
    }
    if (vblank_fn)
        vblank_fn();
    /* KOS no cambia de hilo cuando una interrupción despierta a alguien: el que espera el vblank despertaría en el
     * siguiente tic (lección de Mario Kart 64, docs/juegos/mario-kart-64.md de dc2naomi). */
    thd_schedule(true);
}

void plat_vblank_install(void (*fn)(void)) {
    vblank_fn = fn;
    vblank_handler_add(vblank_irq, NULL);
}

/* ---- mando ---- */

/* Bits del mando de N64 (PR/os_cont.h) */
#define N64_A 0x8000
#define N64_B 0x4000
#define N64_Z 0x2000
#define N64_START 0x1000
#define N64_UP 0x0800
#define N64_DOWN 0x0400
#define N64_LEFT 0x0200
#define N64_RIGHT 0x0100
#define N64_L 0x0020
#define N64_R 0x0010
#define N64_CUP 0x0008
#define N64_CDOWN 0x0004
#define N64_CLEFT 0x0002
#define N64_CRIGHT 0x0001

/* Mando de Dreamcast -> N64: A = A (acelerar), B = B (turbo), X = C-abajo (freno de aire), Y = C-arriba,
 * C = C-derecha, Z = C-izquierda, gatillos = Z y R (inclinarse), stick y cruceta tal cual. Pensado para el mapa
 * "fzx" del residente de dc2naomi (DIMM_PAD_FZX), que hace del volante, los pedales, la palanca y START del mueble
 * de Crazy Taxi el esquema del usuario; con un mando de Dreamcast (Flycast) también sirve. */
int plat_pad_read(unsigned *n64_buttons, int *stick_x, int *stick_y) {
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    cont_state_t *st;
    unsigned b = 0;

    if (dev == NULL || (st = (cont_state_t *) maple_dev_status(dev)) == NULL) {
        *n64_buttons = 0;
        *stick_x = *stick_y = 0;
        return 0;
    }
    if (st->buttons & CONT_A) b |= N64_A;
    if (st->buttons & CONT_B) b |= N64_B;
    /* las C como las manda el mapa "fzx" del residente (DIMM_PAD_FZX): X = C-abajo, Y = C-arriba, C = C-derecha
     * (y Z = C-izquierda, que el mueble no usa) */
    if (st->buttons & CONT_X) b |= N64_CDOWN;
    if (st->buttons & CONT_Y) b |= N64_CUP;
    if (st->buttons & CONT_C) b |= N64_CRIGHT;
    if (st->buttons & CONT_Z) b |= N64_CLEFT;
    if (st->buttons & CONT_START) b |= N64_START;
    if (st->buttons & CONT_DPAD_UP) b |= N64_UP;
    if (st->buttons & CONT_DPAD_DOWN) b |= N64_DOWN;
    if (st->buttons & CONT_DPAD_LEFT) b |= N64_LEFT;
    if (st->buttons & CONT_DPAD_RIGHT) b |= N64_RIGHT;
    if (st->ltrig > 64) b |= N64_Z;
    if (st->rtrig > 64) b |= N64_R;
    *n64_buttons = b;
    *stick_x = st->joyx * 80 / 128;
    *stick_y = -st->joyy * 80 / 128;
    return 1;
}

/* ---- log ---- */

void plat_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

void plat_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    arch_abort();
}

/* ---- arranque ---- */

/* Memoria (port/gen_ld.py): la ventana de RDRAM de N64 (__fzx_n64), la "ROM" (todos los segmentos, ROM.BIN) y el
 * hueco de las muestras de sonido (AUDIOTBL.BIN). Nada de eso va en 1ST_READ.BIN. */
extern char __fzx_n64[], __fzx_n64_end[];
extern char __fzx_rom_start[], __fzx_rom_end[];
extern char audio_table_ROM_START[], audio_table_ROM_END[];
extern char entry_ROM_START[], entry_ROM_END[], entry_VRAM[];
extern char main_ROM_START[], main_ROM_END[], main_VRAM[];
extern void bootproc(void);

/* lo que hace el arranque de N64: el segmento de ROM a su sitio en la RDRAM */
static void load_segment(const char *name, char *rom, char *rom_end, char *vram) {
    memcpy(vram, rom, rom_end - rom);
    plat_sync_code(vram, rom_end - rom);
    plat_log("fzx: %s: %u bytes a %p\n", name, (unsigned) (rom_end - rom), (void *) vram);
}

static void load_file(const char *path, char *dst, char *end) {
    size_t want = (size_t) (end - dst);
    file_t f = fs_open(path, O_RDONLY);
    ssize_t got;

    if (f < 0)
        plat_fatal("fzx: no está %s\n", path);
    got = fs_read(f, dst, want);
    fs_close(f);
    /* ROM.BIN puede acabar hasta 15 bytes antes (el último segmento no se rellena hasta su alineación) */
    if (got <= 0 || got > (ssize_t) want || got + 16 < (ssize_t) want)
        plat_fatal("fzx: %s: leídos %d de %u bytes\n", path, (int) got, (unsigned) want);
    plat_log("fzx: %s, %u bytes en %p\n", path, (unsigned) want, (void *) dst);
}

/* ---- sonido: el AI de N64 sobre un stream de KOS ----
 * osAiSetNextBuffer copia cada búfer del juego (estéreo de 16 bits, izquierda primero) en una cola; un hilo
 * propio rellena el stream del AICA desde ella. El stream va en modo directo (escribimos nosotros en la RAM de
 * sonido con las store queues): el camino normal de snd_stream usa la DMA de G2, que en la NAOMI no acaba nunca.
 * osAiGetLength dice lo que queda en la cola menos una reserva, para que el juego (thread.c, que calcula cuánto
 * generar con eso) mantenga siempre medio búfer del stream de margen. */
#define AFIFO 8192                  /* cuadros estéreo, potencia de 2 */
#define ASTREAM_BYTES 4096          /* por canal: 2048 muestras, el stream pide de 1024 en 1024 */
#define ARESERVE 1280               /* cuadros que el juego no ve en osAiGetLength */
static uint32_t sAFifo[AFIFO];
static volatile unsigned sAHead, sATail;
static snd_stream_hnd_t sAStream = SND_STREAM_INVALID;
static unsigned sAFreq;
/* depuración (tools/live_ram.py): huecos, desbordes, cuadros recibidos, pico de amplitud */
unsigned gPortAudioUnderruns, gPortAudioOverruns, gPortAudioFrames, gPortAudioPeak;
/* los últimos 4 s de salida, para sacarlos de un volcado de RAM a un WAV (tools/audio_wav.py) */
#define ACAPTURE (32000 * 4)
uint32_t gPortAudioCapture[ACAPTURE];
unsigned gPortAudioCapturePos;

static size_t audio_fill(snd_stream_hnd_t h, uintptr_t left, uintptr_t right, size_t req) {
    static int16_t bl[2048] __attribute__((aligned(32))), br[2048] __attribute__((aligned(32)));
    unsigned n = req / 4, avail = sAHead - sATail, tail = sATail, i;

    (void) h;
    if (n > 2048)
        n = 2048;
    for (i = 0; i < n; i++) {
        uint32_t v = 0;

        if (i < avail)
            v = sAFifo[(tail + i) & (AFIFO - 1)];
        bl[i] = (int16_t) (v & 0xFFFF);
        br[i] = (int16_t) (v >> 16);
    }
    if (avail < n)
        gPortAudioUnderruns++;
    sATail = tail + (avail < n ? avail : n);
    spu_memload_sq(left & 0x1FFFFF, bl, n * 2);
    spu_memload_sq(right & 0x1FFFFF, br, n * 2);
    return n * 4;
}

static void *audio_thread(void *p) {
    (void) p;
    for (;;) {
        if (sAStream != SND_STREAM_INVALID)
            snd_stream_poll(sAStream);
        thd_sleep(5);
    }
    return NULL;
}

void plat_audio_start(unsigned freq) {
    static int inited;

    if (freq == sAFreq)
        return;
    if (!inited) {
        if (snd_stream_init_ex(2, 0) < 0) {
            plat_log("fzx: snd_stream_init_ex falló: sin sonido\n");
            sAFreq = freq;
            return;
        }
        sAStream = snd_stream_alloc(NULL, ASTREAM_BYTES);
        if (sAStream == SND_STREAM_INVALID) {
            plat_log("fzx: snd_stream_alloc falló: sin sonido\n");
            sAFreq = freq;
            return;
        }
        snd_stream_set_callback_direct(sAStream, audio_fill);
        thd_create(true, audio_thread, NULL);
        inited = 1;
    } else {
        snd_stream_stop(sAStream);
    }
    sAFreq = freq;
    snd_stream_volume(sAStream, 255);
    snd_stream_start(sAStream, freq, 1);
    plat_log("fzx: sonido a %u Hz\n", freq);
}

void plat_audio_push(const void *buf, unsigned frames) {
    const uint32_t *src = buf;
    unsigned head = sAHead, room = AFIFO - (head - sATail), i;

    if (frames > room) {
        gPortAudioOverruns++;
        frames = room;
    }
    for (i = 0; i < frames; i++) {
        uint32_t v = src[i];
        int l = (int16_t) (v & 0xFFFF), r = (int16_t) (v >> 16);

        if (l < 0) l = -l;
        if (r < 0) r = -r;
        if ((unsigned) l > gPortAudioPeak) gPortAudioPeak = l;
        if ((unsigned) r > gPortAudioPeak) gPortAudioPeak = r;
        sAFifo[(head + i) & (AFIFO - 1)] = v;
        gPortAudioCapture[gPortAudioCapturePos] = v;
        gPortAudioCapturePos = (gPortAudioCapturePos + 1) % ACAPTURE;
    }
    gPortAudioFrames += frames;
    sAHead = head + frames;
}

unsigned plat_audio_pending_frames(void) {
    unsigned n = sAHead - sATail;

    return n > ARESERVE ? n - ARESERVE : 0;
}

/* Vigilante: si el juego deja de mandar tareas al RSP, imprime una vez la lista de hilos (estado, en qué espera)
 * al log de KOS. Para depurar bloqueos entre hilos. */
extern unsigned gPortGfxTasks, gPortAudioTasks;

static void *watchdog(void *p) {
    unsigned last = 0, stuck = 0, dumped = 0;

    (void) p;
    for (;;) {
        unsigned now = gPortGfxTasks + gPortAudioTasks;

        thd_sleep(1000);
        stuck = (now == last) ? stuck + 1 : 0;
        last = now;
        if (stuck >= 3 && !dumped) {
            plat_log("fzx: el juego no avanza (gfx %u, audio %u): hilos:\n", gPortGfxTasks, gPortAudioTasks);
            thd_pslist(printf);
            dumped = 1;
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    (void) argc;
    (void) argv;
    plat_log("fzx: F-Zero X (fzerox-naomi), arranque\n");
    memset(__fzx_n64, 0, __fzx_n64_end - __fzx_n64);   /* la RDRAM empieza a cero (BSS de todos los segmentos) */
    load_file("/cd/ROM.BIN", __fzx_rom_start, __fzx_rom_end);
    load_file("/cd/AUDIOTBL.BIN", audio_table_ROM_START, audio_table_ROM_END);
    {
        /* cabecera + arranque del cartucho (stubs.c, fzx_cart_read): las constantes de física de las naves */
        extern unsigned char fzx_cart_boot[0x1000];
        extern int fzx_cart_boot_ok;
        file_t f = fs_open("/cd/BOOT.BIN", O_RDONLY);

        if (f >= 0) {
            fzx_cart_boot_ok = fs_read(f, fzx_cart_boot, sizeof(fzx_cart_boot)) == (ssize_t) sizeof(fzx_cart_boot);
            fs_close(f);
        }
        if (!fzx_cart_boot_ok)
            plat_log("fzx: falta /cd/BOOT.BIN: las naves no correrán (física a cero)\n");
    }
    load_segment("entry", entry_ROM_START, entry_ROM_END, entry_VRAM);
    load_segment("main", main_ROM_START, main_ROM_END, main_VRAM);
    thd_create(true, watchdog, NULL);
    bootproc();           /* osInitialize + hilo Idle: a partir de aquí todo corre en los hilos del juego */
    plat_block_forever();
    return 0;
}
