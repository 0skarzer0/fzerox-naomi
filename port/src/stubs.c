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

void mio0Decode(u8 *src, void *dstv) {
    u8 *dst = dstv;
    u32 size = be32(src + 4);
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
}

/* tamaño descomprimido de un MIO0 (func_800AA6BC: lw $v0, 4($a0)) */
int func_800AA6BC(u8 *src) {
    return (int) be32(src + 4);
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
