/*
 * plat_kos.c - la parte de KallistiOS de la capa de N64: hilos, espera, vblank, mando, log y el arranque (main).
 * Se compila con las cabeceras de KOS, nunca con las de la decomp (ver plat.h).
 */
#include <kos.h>
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

void plat_thread_set_pri(void *thread, int pri) {
    thd_set_prio(thread ? (kthread_t *) thread : thd_get_current(), map_pri(pri));
}

void *plat_thread_self(void) { return thd_get_current(); }

void plat_block_forever(void) {
    for (;;)
        thd_sleep(1000);
}

void plat_sync_code(void *p, unsigned n) {
    if (n)
        arch_icache_sync_range((uintptr_t) p, n);
}

/* ---- tiempo y vblank ---- */

unsigned long long plat_time_ns(void) { return timer_ns_gettime64(); }

static void (*vblank_fn)(void);

static void vblank_irq(uint32_t code, void *data) {
    (void) code;
    (void) data;
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

/* Provisional (mando de Dreamcast, para Flycast): A = A (acelerar), B = B (turbo), X = C-izquierda, Y = C-arriba,
 * gatillos = Z y R (inclinarse). El mapa del mueble (volante, pedales, palanca) irá en la fase 6. */
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
    if (st->buttons & CONT_X) b |= N64_CLEFT;
    if (st->buttons & CONT_Y) b |= N64_CUP;
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

int main(int argc, char **argv) {
    (void) argc;
    (void) argv;
    plat_log("fzx: F-Zero X (fzerox-naomi), arranque\n");
    memset(__fzx_n64, 0, __fzx_n64_end - __fzx_n64);   /* la RDRAM empieza a cero (BSS de todos los segmentos) */
    load_file("/cd/ROM.BIN", __fzx_rom_start, __fzx_rom_end);
    load_file("/cd/AUDIOTBL.BIN", audio_table_ROM_START, audio_table_ROM_END);
    load_segment("entry", entry_ROM_START, entry_ROM_END, entry_VRAM);
    load_segment("main", main_ROM_START, main_ROM_END, main_VRAM);
    bootproc();           /* osInitialize + hilo Idle: a partir de aquí todo corre en los hilos del juego */
    plat_block_forever();
    return 0;
}
