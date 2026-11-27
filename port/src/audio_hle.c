/*
 * audio_hle.c - el microcódigo de audio del RSP (aspMain de F-Zero X, ABI de la familia "nead": Mario Kart 64,
 * Zelda) hecho en C sobre la CPU.
 *
 * El juego (src/audio/rom/lib/synthesis.c) escribe una lista de órdenes de 64 bits con las macros de PR/abi.h;
 * osSpTaskStartGo (ultra.c) la pasa aquí entera. La "DMEM" del RSP es un búfer de 4 KB: las muestras de 16 bits se
 * guardan en el orden del SH-4 (las escribe y las lee este mismo código, o las copia de/a RAM donde el juego las
 * tiene también en el orden del SH-4) y los datos comprimidos (ADPCM, 8 bits) se copian y se leen byte a byte, así
 * que no hace falta darles la vuelta en ningún sitio.
 *
 * Los campos de cada orden salen de las macros de include/PR/abi.h de la decomp; el comportamiento de cada una,
 * del funcionamiento conocido de ese microcódigo (escrito de nuevo; el remuestreo usa una interpolación cúbica
 * propia en vez de la tabla de la consola, así que no es idéntico bit a bit).
 */
#include <string.h>
#include "plat.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef short s16;
typedef unsigned int u32;
typedef int s32;

#define DMEM_SIZE 0x1000
/* margen por los dos lados: el remuestreo lee 4 muestras antes de la entrada y algunas órdenes se pasan un poco */
static u8 sDmemBuf[DMEM_SIZE + 0x400] __attribute__((aligned(16)));
#define DMEM (sDmemBuf + 0x200)

static inline s16 *d16(u32 a) {
    return (s16 *) (DMEM + (a & (DMEM_SIZE - 1) & ~1u));
}
static inline u8 *d8(u32 a) {
    return DMEM + (a & (DMEM_SIZE - 1));
}

/* las direcciones de RAM de las órdenes son físicas (OS_K0_TO_PHYSICAL), KSEG0 o KSEG1: al P1 del SH-4 */
static inline void *ram(u32 a) {
    return (void *) ((a & 0x1FFFFFFF) | 0x80000000);
}

static inline s16 clamp16(s32 v) {
    if (v > 32767)
        return 32767;
    if (v < -32768)
        return -32768;
    return (s16) v;
}

/* estado de las órdenes */
static u16 sIn, sOut, sCount;          /* A_SETBUFF */
static s16 sBook[16 * 16];             /* A_LOADADPCM: hasta 16 predictores de orden 2 */
static u32 sLoopAddr;                  /* A_SETLOOP */
static u16 sEnvValue[3], sEnvStep[3];  /* A_ENVSETUP1/2: izquierda, derecha, reverberación */

/* ---- descompresión ---- */

static void adpcm_dec(u32 flags, u32 stateAddr) {
    s16 *state = ram(stateAddr);
    s16 last[16];
    u32 in = sIn, out = sOut;
    s32 count = (sCount + 31) & ~31;
    int i;

    if (flags & 1)
        memset(last, 0, sizeof(last));
    else
        memcpy(last, (flags & 2) ? ram(sLoopAddr) : state, sizeof(last));

    for (i = 0; i < 16; i++, out += 2)
        *d16(out) = last[i];

    while (count > 0) {
        u8 head = *d8(in++);
        int scale = head >> 4;
        int rshift = scale < 12 ? 12 - scale : 0;
        const s16 *b1 = sBook + ((head & 0xF) << 4);
        const s16 *b2 = b1 + 8;
        s16 res[16];
        int half;

        for (i = 0; i < 8; i++) {
            u8 b = *d8(in++);
            res[i * 2] = (s16) ((u16) (b & 0xF0) << 8) >> rshift;
            res[i * 2 + 1] = (s16) ((u16) (b & 0x0F) << 12) >> rshift;
        }
        /* dos mitades de 8: cada una predice con las dos últimas muestras de la anterior */
        for (half = 0; half < 2; half++) {
            const s16 *src = res + half * 8;
            /* book1 multiplica a la penúltima muestra y book2 a la última (la 1.ª mitad usa las del marco
             * anterior; la 2.ª, las de la 1.ª mitad recién hecha) */
            s16 l1 = half ? last[6] : last[14];
            s16 l2 = half ? last[7] : last[15];
            s16 *dst = last + half * 8;
            s16 tmp[8];
            int k;

            for (i = 0; i < 8; i++) {
                s32 acc = (s32) src[i] << 11;

                acc += b1[i] * l1 + b2[i] * l2;
                for (k = 0; k < i; k++)
                    acc += b2[k] * src[i - 1 - k];
                tmp[i] = clamp16(acc >> 11);
            }
            memcpy(dst, tmp, sizeof(tmp));
        }
        for (i = 0; i < 16; i++, out += 2)
            *d16(out) = last[i];
        count -= 32;
    }
    memcpy(state, last, sizeof(last));
}

/* muestras de 8 bits sin comprimir: mismo esquema de marcos de 16 muestras que el ADPCM */
static void s8_dec(u32 flags, u32 stateAddr) {
    s16 *state = ram(stateAddr);
    s16 last[16];
    u32 in = sIn, out = sOut;
    s32 count = (sCount + 31) & ~31;
    int i;

    if (flags & 1)
        memset(last, 0, sizeof(last));
    else
        memcpy(last, (flags & 2) ? ram(sLoopAddr) : state, sizeof(last));

    for (i = 0; i < 16; i++, out += 2)
        *d16(out) = last[i];
    while (count > 0) {
        for (i = 0; i < 16; i++)
            last[i] = (s16) ((u16) *d8(in++) << 8);
        for (i = 0; i < 16; i++, out += 2)
            *d16(out) = last[i];
        count -= 32;
    }
    memcpy(state, last, sizeof(last));
}

/* ---- remuestreo (tono): 4 muestras de historia + interpolación cúbica entre la 2.ª y la 3.ª ---- */

static s16 sCubic[64][4];

static void cubic_init(void) {
    int p;

    for (p = 0; p < 64; p++) {
        float t = p / 64.0f, t2 = t * t, t3 = t2 * t;
        /* Catmull-Rom */
        float w0 = -0.5f * t3 + t2 - 0.5f * t;
        float w1 = 1.5f * t3 - 2.5f * t2 + 1.0f;
        float w2 = -1.5f * t3 + 2.0f * t2 + 0.5f * t;
        float w3 = 0.5f * t3 - 0.5f * t2;

        sCubic[p][0] = (s16) (w0 * 32767.0f);
        sCubic[p][1] = (s16) (w1 * 32767.0f);
        sCubic[p][2] = (s16) (w2 * 32767.0f);
        sCubic[p][3] = (s16) (w3 * 32767.0f);
    }
}

static void resample(u32 flags, u32 pitch16, u32 stateAddr) {
    s16 *state = ram(stateAddr);   /* 4 muestras + posición fraccionaria */
    u32 pitch = pitch16 << 1;      /* Q16.16 */
    u32 ipos = (sIn >> 1) - 4, opos = sOut >> 1;
    s32 count = ((sCount + 15) & ~15) >> 1;
    u32 frac;
    int k;

    if (flags & 1) {
        for (k = 0; k < 4; k++)
            *d16((ipos + k) * 2) = 0;
        frac = 0;
    } else {
        for (k = 0; k < 4; k++)
            *d16((ipos + k) * 2) = state[k];
        frac = (u16) state[4];
    }
    while (count-- > 0) {
        const s16 *w = sCubic[frac >> 10];
        s32 acc = *d16(ipos * 2) * w[0] + *d16((ipos + 1) * 2) * w[1] + *d16((ipos + 2) * 2) * w[2] +
                  *d16((ipos + 3) * 2) * w[3];

        *d16(opos++ * 2) = clamp16(acc >> 15);
        frac += pitch;
        ipos += frac >> 16;
        frac &= 0xFFFF;
    }
    for (k = 0; k < 4; k++)
        state[k] = *d16((ipos + k) * 2);
    state[4] = (s16) frac;
}

/* ---- mezcla ---- */

static void env_mixer(u32 w0, u32 w1) {
    u32 in = ((w0 >> 16) & 0xFF) << 4;
    s32 count = (((w0 >> 8) & 0xFF) + 7) & ~7;
    s16 xors[4];
    u32 dl = ((w1 >> 24) & 0xFF) << 4, dr = ((w1 >> 16) & 0xFF) << 4;
    u32 wl = ((w1 >> 8) & 0xFF) << 4, wr = (w1 & 0xFF) << 4;
    int i;

    /* bits de inversión de fase: 3 = húmeda izq., 2 = húmeda der., 1 = seca izq., 0 = seca der. */
    xors[2] = -(s16) ((w0 >> 3) & 1);
    xors[3] = -(s16) ((w0 >> 2) & 1);
    xors[0] = -(s16) ((w0 >> 1) & 1);
    xors[1] = -(s16) (w0 & 1);
    if ((w0 >> 4) & 1) {
        u32 t = wl;
        wl = wr;
        wr = t;
    }
    while (count > 0) {
        for (i = 0; i < 8; i++) {
            s32 s = *d16(in);
            s16 l = (s16) (((s32) s * (u32) sEnvValue[0]) >> 16) ^ xors[0];
            s16 r = (s16) (((s32) s * (u32) sEnvValue[1]) >> 16) ^ xors[1];
            s16 l2 = (s16) (((s32) l * (u32) sEnvValue[2]) >> 16) ^ xors[2];
            s16 r2 = (s16) (((s32) r * (u32) sEnvValue[2]) >> 16) ^ xors[3];

            *d16(dl) = clamp16(*d16(dl) + l);
            *d16(dr) = clamp16(*d16(dr) + r);
            *d16(wl) = clamp16(*d16(wl) + l2);
            *d16(wr) = clamp16(*d16(wr) + r2);
            in += 2;
            dl += 2;
            dr += 2;
            wl += 2;
            wr += 2;
        }
        sEnvValue[0] += sEnvStep[0];
        sEnvValue[1] += sEnvStep[1];
        sEnvValue[2] += sEnvStep[2];
        count -= 8;
    }
}

static void mix(u32 countBytes, s16 gain, u32 in, u32 out) {
    s32 n = countBytes >> 1;

    while (n-- > 0) {
        *d16(out) = clamp16(*d16(out) + (((s32) *d16(in) * gain) >> 15));
        in += 2;
        out += 2;
    }
}

static void interleave(u32 countBytes, u32 out, u32 l, u32 r) {
    s32 n = countBytes >> 1;
    /* la salida puede pisar las entradas (DMEM_TEMP va antes de las dos): se copia primero */
    static s16 tl[0x800], tr[0x800];
    int i;

    if (n > 0x800)
        n = 0x800;
    for (i = 0; i < n; i++) {
        tl[i] = *d16(l + i * 2);
        tr[i] = *d16(r + i * 2);
    }
    for (i = 0; i < n; i++) {
        *d16(out + i * 4) = tl[i];
        *d16(out + i * 4 + 2) = tr[i];
    }
}

/* ---- la lista ---- */

unsigned gPortAudioCmds, gPortAudioBad;

void fzx_audio_run(const u32 *cmd, unsigned n) {
    static int inited;

    if (!inited) {
        cubic_init();
        inited = 1;
    }
    for (; n--; cmd += 2) {
        u32 w0 = cmd[0], w1 = cmd[1];
        u32 sz;

        gPortAudioCmds++;
        switch (w0 >> 24) {
            case 0: /* A_SPNOOP */
                break;
            case 1: /* A_ADPCM */
                adpcm_dec((w0 >> 16) & 0xFF, w1);
                break;
            case 2: /* A_CLEARBUFF */
                sz = (w1 + 15) & ~15;
                if ((w0 & 0xFFFF) + sz <= DMEM_SIZE)
                    memset(d8(w0 & 0xFFFF), 0, sz);
                break;
            case 5: /* A_RESAMPLE */
                resample((w0 >> 16) & 0xFF, w0 & 0xFFFF, w1);
                break;
            case 8: /* A_SETBUFF */
                sIn = w0 & 0xFFFF;
                sOut = w1 >> 16;
                sCount = w1 & 0xFFFF;
                break;
            case 10: /* A_DMEMMOVE */
                sz = ((w1 & 0xFFFF) + 15) & ~15;
                if ((w0 & 0xFFFF) + sz <= DMEM_SIZE && (w1 >> 16) + sz <= DMEM_SIZE)
                    memmove(d8(w1 >> 16), d8(w0 & 0xFFFF), sz);
                break;
            case 11: /* A_LOADADPCM */
                sz = (w0 & 0xFFFF);
                if (sz > sizeof(sBook))
                    sz = sizeof(sBook);
                memcpy(sBook, ram(w1), sz);
                break;
            case 12: /* A_MIXER */
                mix(((w0 >> 16) & 0xFF) << 4, (s16) (w0 & 0xFFFF), w1 >> 16, w1 & 0xFFFF);
                break;
            case 13: /* A_INTERLEAVE */
                interleave(((w0 >> 16) & 0xFF) << 4, w0 & 0xFFFF, w1 >> 16, w1 & 0xFFFF);
                break;
            case 15: /* A_SETLOOP */
                sLoopAddr = w1;
                break;
            case 18: /* A_ENVSETUP1 */
                sEnvValue[2] = (w0 >> 8) & 0xFF00;
                sEnvStep[2] = w0 & 0xFFFF;
                sEnvStep[0] = w1 >> 16;
                sEnvStep[1] = w1 & 0xFFFF;
                break;
            case 19: /* A_ENVMIXER */
                env_mixer(w0, w1);
                break;
            case 20: /* A_LOADBUFF */
                sz = ((w0 >> 16) & 0xFF) << 4;
                if ((w0 & 0xFFFF) + sz <= DMEM_SIZE)
                    memcpy(d8(w0 & 0xFFFF), ram(w1), sz);
                break;
            case 21: /* A_SAVEBUFF */
                sz = ((w0 >> 16) & 0xFF) << 4;
                if ((w0 & 0xFFFF) + sz <= DMEM_SIZE)
                    memcpy(ram(w1), d8(w0 & 0xFFFF), sz);
                break;
            case 22: /* A_ENVSETUP2 */
                sEnvValue[0] = w1 >> 16;
                sEnvValue[1] = w1 & 0xFFFF;
                break;
            case 23: /* A_S8DEC */
                s8_dec((w0 >> 16) & 0xFF, w1);
                break;
            default:
                if (gPortAudioBad++ < 8)
                    plat_log("fzx: orden de audio desconocida %08x %08x\n", (unsigned) w0, (unsigned) w1);
                break;
        }
    }
}
