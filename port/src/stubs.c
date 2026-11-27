/*
 * stubs.c - lo que la decomp deja en ensamblador o en el 64DD y el port no usa (o rehace en C).
 * Sin las cabeceras de la decomp a propósito: los stubs del 64DD solo tienen que enlazar (nunca se llaman con
 * osEPiReadIo diciendo "no hay disco", ver ultra.c).
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "plat.h"

typedef unsigned char u8;
typedef unsigned int u32;

/* ---- microcódigos del RSP: en N64 son binarios que se suben al RSP; aquí solo hace falta la dirección ---- */
char gspF3DEX2_fifoTextStart[8], gspF3DEX2_fifoDataStart[8];
char gspF3DLX2_Rej_fifoTextStart[8], gspF3DLX2_Rej_fifoDataStart[8];
char gspF3DFLX2_Rej_fifoTextStart[8], gspF3DFLX2_Rej_fifoDataStart[8];
char aspMainTextStart[8], aspMainDataStart[8], aspMainDataEnd[8];
char rspbootTextStart[8], rspbootTextEnd[8];

/* ---- MIO0 (asm/us/rev0/game/mio0Decode.s en la decomp) ----
 * Cabecera big-endian: "MIO0", tamaño descomprimido, desplazamiento de los datos comprimidos (pares de bytes:
 * 4 bits de longitud - 3, 12 bits de distancia - 1) y de los datos sin comprimir; los bits de control (1 = byte
 * tal cual) empiezan en +16, de 32 en 32 bits. */
static u32 be32(const u8 *p) {
    return ((u32) p[0] << 24) | ((u32) p[1] << 16) | ((u32) p[2] << 8) | p[3];
}

/* Registro de lo que ha escrito mio0Decode: esos bytes están en el orden de N64 (big-endian), mientras que los
 * assets de 16 bits compilados en C (u16[]) están en el del SH-4. El renderer (gfx_rsp.c) lo pregunta para cada
 * textura de 16 bits; una DMA encima (ultra.c) lo borra. */
#define MIO0_RANGES 256
static struct {
    unsigned lo, hi;
} sMio0[MIO0_RANGES];
static int sMio0Next;
static unsigned sMio0Lo = 0xFFFFFFFF, sMio0Hi;
static unsigned sMio0Gen = 1;   /* caja que contiene todos los rangos: descarte rápido */

void fzx_mio0_forget(const void *p, unsigned n) {
    unsigned lo = (unsigned) p, hi = lo + n;
    int i;

    if (hi <= sMio0Lo || lo >= sMio0Hi)
        return;   /* fuera de la caja: no toca ningún rango (las DMA del audio, casi todas) */
    for (i = 0; i < MIO0_RANGES; i++)
        if (sMio0[i].hi > lo && sMio0[i].lo < hi) {
            sMio0[i].lo = sMio0[i].hi = 0;
            sMio0Gen++;
        }
}

/* memo de las últimas consultas (el renderer pregunta por las mismas texturas cada cuadro); sMio0Gen (arriba)
 * cambia con cada mio0Decode o DMA encima, y entonces el memo deja de valer */
static struct {
    unsigned lo, n, gen;
    int r;
} sMio0Memo[64];

static int is_mio0_scan(unsigned lo, unsigned hi) {
    int i;

    for (i = 0; i < MIO0_RANGES; i++)
        if (lo >= sMio0[i].lo && hi <= sMio0[i].hi)
            return 1;
    return 0;
}

int fzx_is_mio0(const void *p, unsigned n) {
    unsigned lo = (unsigned) p, hi = lo + n, k = (lo >> 4) & 63;

    if (lo < sMio0Lo || hi > sMio0Hi)
        return 0;
    if (sMio0Memo[k].gen != sMio0Gen || sMio0Memo[k].lo != lo || sMio0Memo[k].n != n) {
        sMio0Memo[k].lo = lo;
        sMio0Memo[k].n = n;
        sMio0Memo[k].gen = sMio0Gen;
        sMio0Memo[k].r = is_mio0_scan(lo, hi);
    }
    return sMio0Memo[k].r;
}

static void mio0_record(const void *p, unsigned n) {
    fzx_mio0_forget(p, n);
    sMio0Gen++;
    sMio0[sMio0Next].lo = (unsigned) p;
    sMio0[sMio0Next].hi = (unsigned) p + n;
    if ((unsigned) p < sMio0Lo)
        sMio0Lo = (unsigned) p;
    if ((unsigned) p + n > sMio0Hi)
        sMio0Hi = (unsigned) p + n;
    sMio0Next = (sMio0Next + 1) % MIO0_RANGES;
}

unsigned gPortMio0Log[256][3];   /* origen, destino, tamaño de los últimos mio0Decode (depuración) */
unsigned gPortMio0Count;

void mio0Decode(u8 *src, void *dstv) {
    u8 *dst = dstv;
    u32 size = be32(src + 4);
    unsigned k = gPortMio0Count++ % 256;

    if (src[0] != 'M' || src[1] != 'I' || src[2] != 'O' || src[3] != '0') {
        /* el juego lo llama sin mirar en algún sitio (sys_gfx.c, ya parcheado): con datos que no son MIO0 escribiría
         * a ciegas */
        plat_log("fzx: mio0Decode de algo que no es MIO0 (%p -> %p)\n", (void *) src, dstv);
        return;
    }
    gPortMio0Log[k][0] = (unsigned) src;
    gPortMio0Log[k][1] = (unsigned) dst;
    gPortMio0Log[k][2] = size;
    const u8 *comp = src + be32(src + 8);
    const u8 *raw = src + be32(src + 12);
    const u8 *ctl = src + 16;
    u32 bits = 0, nbits = 0, out = 0;

    while (out < size) {
        if (nbits == 0) {
            bits = be32(ctl);
            ctl += 4;
            nbits = 32;
        }
        if (bits & 0x80000000) {
            dst[out++] = *raw++;
        } else {
            u32 v = ((u32) comp[0] << 8) | comp[1];
            u32 len = (v >> 12) + 3;
            u32 dist = (v & 0xFFF) + 1;

            comp += 2;
            while (len-- && out < size) {
                dst[out] = dst[out - dist];
                out++;
            }
        }
        bits <<= 1;
        nbits--;
    }
    mio0_record(dst, size);
}

/* tamaño descomprimido de un MIO0 (func_800AA6BC: lw $v0, 4($a0)) */
int func_800AA6BC(u8 *src) {
    return (int) be32(src + 4);
}

/* ---- los primeros 4 KB del cartucho (cabecera + IPL3) ----
 * El juego lee de ahí, con lecturas directas a la ROM (ROM_READ en racer.c y dos overlays), las constantes de física
 * de las naves: un truco contra copias. plat_kos.c los carga de BOOT.BIN (lo saca build_fzerox.sh de la ROM del
 * usuario). Son bytes de la ROM: big-endian. */
u8 fzx_cart_boot[0x1000];
int fzx_cart_boot_ok;
unsigned gPortCartReads;

unsigned int fzx_cart_read(unsigned int addr) {
    addr &= 0x0FFFFFFC;
    gPortCartReads++;
    if (addr + 4 > sizeof(fzx_cart_boot)) {
        plat_log("fzx: ROM_READ fuera de los 4 KB del arranque: %08x\n", addr);
        return 0;
    }
    return be32(fzx_cart_boot + addr);
}

/* IO_READ: el juego solo mira PI_STATUS (esperar al PI); aquí nunca está ocupado */
unsigned int fzx_io_read(unsigned int addr) {
    (void) addr;
    return 0;
}

/* ---- libultra/libm sueltas ---- */
void guNormalize(float *x, float *y, float *z) {
    float m = sqrtf(*x * *x + *y * *y + *z * *z);

    if (m != 0.0f) {
        m = 1.0f / m;
        *x *= m;
        *y *= m;
        *z *= m;
    }
}

/* _Printf de libultra: formatea y pasa el texto a prout(arg, texto, n), que devuelve el nuevo arg o NULL */
typedef char *(*outfun)(char *, const char *, size_t);

int _Printf(outfun prout, char *arg, const char *fmt, va_list args) {
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), fmt, args);

    if (n < 0)
        return n;
    if (n >= (int) sizeof(buf))
        n = sizeof(buf) - 1;
    if (prout(arg, buf, (size_t) n) == NULL)
        return -1;
    return n;
}

/* ---- 64DD: nunca hay disco ---- */
#define LEO_STUB(name) \
    int name(void) {   \
        plat_log("fzx: %s (64DD) llamado\n", #name); \
        return -1;     \
    }
LEO_STUB(LeoBootGame)
LEO_STUB(LeoByteToLBA)
LEO_STUB(LeoCACreateLeoManager)
LEO_STUB(LeoGetAAdr2)
LEO_STUB(LeoLBAToByte)
LEO_STUB(LeoReadDiskID)
LEO_STUB(LeoReadWrite)
LEO_STUB(LeoReset)
LEO_STUB(LeoResetClear)
LEO_STUB(LeoSpdlMotor)
LEO_STUB(LeoTestUnitReady)
LEO_STUB(Mfs_CreateLeoManager)
LEO_STUB(Mfs_GetFileIndex)
LEO_STUB(Mfs_InitRamArea)
LEO_STUB(Mfs_LoadFileInDir)
LEO_STUB(Mfs_ModeSelectAsync)
LEO_STUB(Mfs_SetGameCode)
LEO_STUB(func_i1_8040428C)
LEO_STUB(func_i1_804046F0)
int gMfsError;
u8 leoBootID[64];
u8 D_i1_80428618[64];

/* NaN de libultra (asm/us/rev0/data/libultra/gu/libm_vals.rodata.s): lo devuelven sinf/cosf fuera de rango */
const float __libm_qnan_f = __builtin_nanf("");
