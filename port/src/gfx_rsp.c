/*
 * gfx_rsp.c - dibuja las listas de visualización F3DEX2 del juego (lo que harían el RSP y el RDP de la N64) y
 * pasa los triángulos a la PVR por gfx_backend.h. Se compila con las cabeceras de la decomp.
 *
 * - RSP: pila de matrices, viewport, luces y texgen, coordenadas de textura (gSPTexture, tiles), recorte por el
 *   plano cercano y descarte de caras.
 * - RDP: tiles y cargas de textura (no se emula la TMEM: se recuerda de qué dirección de RAM salió cada carga),
 *   paletas, combinador, modo de mezcla y profundidad, rectángulos (relleno y con textura).
 * - Combinador: se evalúa por vértice con TEXEL0 = 0 y = 1; la salida es textura * (f1 - f0) + f0, que la PVR hace
 *   como textura * color base + color desplazado (idea de cruisn-usa-dreamcast, escrita aquí de nuevo).
 */
#include "global.h"
#include "plat.h"
#include "gfx_backend.h"
#include <stdarg.h>

int vsnprintf(char* buf, size_t n, const char* fmt, va_list ap);
static void trace(const char* fmt, ...);
extern s32 gGameMode;

#define SCREEN_SCALE 2.0f   /* 320x240 de N64 -> 640x480 */
#define MAX_VTX 64
#define MTX_STACK 32

/* ------------------------------------------------------------------ direcciones */

static u32 sSeg[16];

static u32 sCntVtx, sCntMtx; /* contadores para la traza */

static void* resolve(u32 a) {
    u32 seg = a >> 24;

    if (seg >= 0x80) {
        return (void*) a;                   /* puntero de verdad (KSEG0/P1) */
    }
    if (seg == 0x0C || seg == 0x0D) {
        return (void*) (a | 0x80000000);    /* físico de nuestra RAM (el juego no usa los segmentos 12 y 13) */
    }
    return (void*) (((sSeg[seg & 0xF] + (a & 0xFFFFFF)) & 0x1FFFFFFF) | 0x80000000);
}

/* ------------------------------------------------------------------ estado del RSP */

typedef struct {
    float x, y, z, w;   /* espacio de recorte */
    float u, v;         /* texels */
    float r, g, b, a;   /* shade 0..1 */
} RVtx;

static RVtx sVtx[MAX_VTX];
/* to_screen de cada vértice cargado, reutilizado entre los triángulos que lo comparten mientras no cambie nada de
 * lo que usa (estado, combinador, tile, viewport): sScrGen sube con cada cambio; sVtxGen[i] = 0 al cargar. */
static u32 sScrGen = 1;
static u32 sVtxGen[MAX_VTX];      /* == sScrGen: sScr[i] vale */
static GbVert sScr[MAX_VTX];
static float sMv[MTX_STACK][4][4];
static s32 sMvTop;
static float sProj[4][4];
static float sMvp[4][4];
static s32 sMvpDirty = 1;
static u32 sGeom;
static struct {
    float sx, sy, tx, ty;   /* en píxeles de N64 */
} sVp = { 160, 120, 160, 120 };

static struct {
    u8 col[3];
    s8 dir[3];
} sLights[8];
static s32 sNumLights = 1;
static float sLightDir[8][3]; /* en espacio de modelo */
static float sLookat[2][3];
static s32 sLightsDirty = 1;

static struct {
    u16 s, t;
    s32 tile, on;
} sTexture;
static u32 sHalf1, sHalf2;

/* ------------------------------------------------------------------ estado del RDP */

typedef struct {
    s32 fmt, siz, line, tmem, palette;
    s32 cms, cmt, masks, maskt, shifts, shiftt;
    s32 uls, ult, lrs, lrt;
} Tile;

typedef struct {
    s32 tmem;
    const u8* addr;
    s32 stride;
    s32 valid;
} Load;

static Tile sTiles[8];
static Load sLoads[8];   /* una por tmem distinta, la última gana */
static const u8* sTlut[16];
static struct {
    u32 addr;
    s32 fmt, siz, width;
} sTimg;
static u32 sOmH, sOmL;
static u32 sCombine0, sCombine1;
static u32 sPrim, sEnv, sFill, sFog, sBlendColor;
static u8 sPrimLod;
static u32 sCimg, sZimg;

extern int fzx_is_mio0(const void* p, unsigned n);   /* stubs.c: ¿lo escribió mio0Decode? (big-endian) */

/* ------------------------------------------------------------------ matrices */

static void mtx_from_n64(float out[4][4], const u32* w) {
    s32 i;

    for (i = 0; i < 16; i++) {
        s32 hi = (i & 1) ? (s16) (w[i >> 1] & 0xFFFF) : (s16) (w[i >> 1] >> 16);
        u32 lo = (i & 1) ? (w[8 + (i >> 1)] & 0xFFFF) : (w[8 + (i >> 1)] >> 16);
        out[i >> 2][i & 3] = (float) (s32) ((hi << 16) | lo) * (1.0f / 65536.0f);   /* (s32): con lo (u32) sería sin signo */
    }
}

static void mtx_mul(float out[4][4], float a[4][4], float b[4][4]) {
    float r[4][4];
    s32 i, j;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
        }
    }
    bcopy(r, out, sizeof(r));
}

static void do_mtx(u32 w0, u32 w1) {
    sCntMtx++;
    u32 p = (w0 & 0xFF) ^ G_MTX_PUSH;
    float m[4][4];

    mtx_from_n64(m, resolve(w1));
    if (sCntMtx < 12) {
        trace(" MTX %08x p%x [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.1f %.1f %.1f %.3f]\n",
              (unsigned) w1, (unsigned) p, m[0][0], m[0][1], m[0][2], m[0][3], m[1][0], m[1][1], m[1][2], m[1][3],
              m[2][0], m[2][1], m[2][2], m[2][3], m[3][0], m[3][1], m[3][2], m[3][3]);
    }
    if (p & G_MTX_PROJECTION) {
        if (p & G_MTX_LOAD) {
            bcopy(m, sProj, sizeof(m));
        } else {
            mtx_mul(sProj, m, sProj);
        }
    } else {
        if ((p & G_MTX_PUSH) && sMvTop < MTX_STACK - 1) {
            bcopy(sMv[sMvTop], sMv[sMvTop + 1], sizeof(m));
            sMvTop++;
        }
        if (p & G_MTX_LOAD) {
            bcopy(m, sMv[sMvTop], sizeof(m));
        } else {
            mtx_mul(sMv[sMvTop], m, sMv[sMvTop]);
        }
        sLightsDirty = 1;
    }
    sMvpDirty = 1;
}

/* ------------------------------------------------------------------ luces */

static void normalize3(float* v) {
    float m = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

    if (m > 0.0f) {
        m = 1.0f / m;
        v[0] *= m;
        v[1] *= m;
        v[2] *= m;
    }
}

/* dirección del mundo -> modelo: M (3x3) por la dirección, como hace el RSP */
static void dir_to_model(float out[3], const s8 d[3]) {
    float (*m)[4] = sMv[sMvTop];
    s32 i;

    for (i = 0; i < 3; i++) {
        out[i] = m[i][0] * d[0] + m[i][1] * d[1] + m[i][2] * d[2];
    }
    normalize3(out);
}

static void lights_update(void) {
    s32 i;
    s8 la[2][3] = { { 127, 0, 0 }, { 0, 127, 0 } };

    for (i = 0; i < sNumLights; i++) {
        dir_to_model(sLightDir[i], sLights[i].dir);
    }
    dir_to_model(sLookat[0], la[0]);
    dir_to_model(sLookat[1], la[1]);
    sLightsDirty = 0;
}

/* ------------------------------------------------------------------ vértices */

static void do_vtx(u32 w0, u32 w1) {
    sCntVtx++;
    s32 n = (w0 >> 12) & 0xFF;
    s32 v0 = ((w0 >> 1) & 0x7F) - n;
    Vtx* src = resolve(w1);
    float (*m)[4];
    Tile* tl = &sTiles[sTexture.tile];
    float ss = sTexture.s * (1.0f / 65536.0f) / 32.0f;
    float ts = sTexture.t * (1.0f / 65536.0f) / 32.0f;
    s32 i;

    if (v0 < 0 || v0 + n > MAX_VTX) {
        return;
    }
    if (sMvpDirty) {
        mtx_mul(sMvp, sMv[sMvTop], sProj);
        sMvpDirty = 0;
    }
    if ((sGeom & G_LIGHTING) && sLightsDirty) {
        lights_update();
    }
    m = sMvp;
    for (i = 0; i < n; i++) {
        Vtx_t* v = &src[i].v;
        RVtx* d = &sVtx[v0 + i];
        sVtxGen[v0 + i] = 0;
        float x = v->ob[0], y = v->ob[1], z = v->ob[2];

        d->x = x * m[0][0] + y * m[1][0] + z * m[2][0] + m[3][0];
        d->y = x * m[0][1] + y * m[1][1] + z * m[2][1] + m[3][1];
        d->z = x * m[0][2] + y * m[1][2] + z * m[2][2] + m[3][2];
        d->w = x * m[0][3] + y * m[1][3] + z * m[2][3] + m[3][3];

        if (sGeom & G_LIGHTING) {
            Vtx_tn* vn = &src[i].n;
            float nx = vn->n[0], ny = vn->n[1], nz = vn->n[2];
            float r, g, b, nm;
            s32 k;

            nm = sqrtf(nx * nx + ny * ny + nz * nz);
            if (nm > 0.0f) {
                nm = 1.0f / nm;
                nx *= nm;
                ny *= nm;
                nz *= nm;
            }
            r = sLights[sNumLights].col[0];
            g = sLights[sNumLights].col[1];
            b = sLights[sNumLights].col[2];
            for (k = 0; k < sNumLights; k++) {
                float dp = nx * sLightDir[k][0] + ny * sLightDir[k][1] + nz * sLightDir[k][2];

                if (dp > 0.0f) {
                    r += dp * sLights[k].col[0];
                    g += dp * sLights[k].col[1];
                    b += dp * sLights[k].col[2];
                }
            }
            d->r = r > 255.0f ? 1.0f : r / 255.0f;
            d->g = g > 255.0f ? 1.0f : g / 255.0f;
            d->b = b > 255.0f ? 1.0f : b / 255.0f;
            d->a = vn->a / 255.0f;
            if (sGeom & G_TEXTURE_GEN) {
                float dx = nx * sLookat[0][0] + ny * sLookat[0][1] + nz * sLookat[0][2];
                float dy = nx * sLookat[1][0] + ny * sLookat[1][1] + nz * sLookat[1][2];

                d->u = (dx + 1.0f) * 0.25f * sTexture.s / 32.0f;
                d->v = (dy + 1.0f) * 0.25f * sTexture.t / 32.0f;
            } else {
                d->u = v->tc[0] * ss;
                d->v = v->tc[1] * ts;
            }
        } else {
            d->r = v->cn[0] / 255.0f;
            d->g = v->cn[1] / 255.0f;
            d->b = v->cn[2] / 255.0f;
            d->a = v->cn[3] / 255.0f;
            d->u = v->tc[0] * ss;
            d->v = v->tc[1] * ts;
        }
        (void) tl;
    }
}

/* ------------------------------------------------------------------ combinador */

typedef struct {
    float r, g, b, a;
} Col;

static Col col_of(u32 rgba) {
    Col c;

    c.r = (rgba >> 24) / 255.0f;
    c.g = ((rgba >> 16) & 0xFF) / 255.0f;
    c.b = ((rgba >> 8) & 0xFF) / 255.0f;
    c.a = (rgba & 0xFF) / 255.0f;
    return c;
}

/* prim, env y prim LOD como floats: se recalculan en combine_decode (cada cambio de estado), no en cada vértice */
static Col sPrimC, sEnvC;
static float sPrimLodF;

/* entrada del combinador de color (A/B/C/D); kind: 0 = A, 1 = B, 2 = C, 3 = D */
static float cc_in(s32 kind, s32 code, s32 ch, const Col* comb, const Col* tex, const Col* shade) {
    const Col* prim = &sPrimC;
    const Col* env = &sEnvC;
    const float* pc = &prim->r;
    const float* ec = &env->r;
    const float* sc = &shade->r;
    const float* tc = &tex->r;
    const float* cc = &comb->r;

    if (kind == 2) {
        switch (code) {
            case G_CCMUX_COMBINED: return cc[ch];
            case G_CCMUX_TEXEL0: case G_CCMUX_TEXEL1: return tc[ch];
            case G_CCMUX_PRIMITIVE: return pc[ch];
            case G_CCMUX_SHADE: return sc[ch];
            case G_CCMUX_ENVIRONMENT: return ec[ch];
            case G_CCMUX_COMBINED_ALPHA: return comb->a;
            case G_CCMUX_TEXEL0_ALPHA: case G_CCMUX_TEXEL1_ALPHA: return tex->a;
            case G_CCMUX_PRIMITIVE_ALPHA: return prim->a;
            case G_CCMUX_SHADE_ALPHA: return shade->a;
            case G_CCMUX_ENV_ALPHA: return env->a;
            case G_CCMUX_PRIM_LOD_FRAC: return sPrimLodF;
            case G_CCMUX_LOD_FRACTION: return 0.0f;
            default: return 0.0f;
        }
    }
    switch (code) {
        case G_CCMUX_COMBINED: return cc[ch];
        case G_CCMUX_TEXEL0: case G_CCMUX_TEXEL1: return tc[ch];
        case G_CCMUX_PRIMITIVE: return pc[ch];
        case G_CCMUX_SHADE: return sc[ch];
        case G_CCMUX_ENVIRONMENT: return ec[ch];
        case 6: return (kind == 1) ? 0.0f : 1.0f;   /* A,D: 1; B: CENTER (no usado) */
        case 7: return 0.0f;                         /* A: NOISE, B: K4, D: 0 */
        default: return 0.0f;
    }
}

static float ac_in(s32 kind, s32 code, const Col* comb, const Col* tex, const Col* shade) {
    switch (code) {
        case 0: return (kind == 2) ? 0.0f : comb->a;    /* C: LOD_FRACTION */
        case 1: case 2: return tex->a;
        case 3: return sPrimC.a;
        case 4: return shade->a;
        case 5: return sEnvC.a;
        case 6: return (kind == 2) ? sPrimLodF : 1.0f;
        default: return 0.0f;
    }
}

static float clamp01(float f) {
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

/* un ciclo del combinador; c: códigos [a, b, c, d] de color y [a, b, c, d] de alfa */
static Col cc_cycle(const s32* c, const s32* a, const Col* comb, const Col* tex, const Col* shade) {
    Col o;
    float* op = &o.r;
    s32 ch;

    for (ch = 0; ch < 3; ch++) {
        op[ch] = (cc_in(0, c[0], ch, comb, tex, shade) - cc_in(1, c[1], ch, comb, tex, shade)) *
                     cc_in(2, c[2], ch, comb, tex, shade) +
                 cc_in(3, c[3], ch, comb, tex, shade);
    }
    o.a = (ac_in(0, a[0], comb, tex, shade) - ac_in(1, a[1], comb, tex, shade)) * ac_in(2, a[2], comb, tex, shade) +
          ac_in(3, a[3], comb, tex, shade);
    return o;
}

static s32 sCC[2][4], sAC[2][4];
static s32 sTwoCycle, sUsesTex;

static void combine_compile(void);

static void combine_decode(void) {
    u32 w0 = sCombine0, w1 = sCombine1;
    s32 i;

    sCC[0][0] = (w0 >> 20) & 0xF; sCC[0][1] = (w1 >> 28) & 0xF; sCC[0][2] = (w0 >> 15) & 0x1F; sCC[0][3] = (w1 >> 15) & 7;
    sAC[0][0] = (w0 >> 12) & 7;   sAC[0][1] = (w1 >> 12) & 7;   sAC[0][2] = (w0 >> 9) & 7;     sAC[0][3] = (w1 >> 9) & 7;
    sCC[1][0] = (w0 >> 5) & 0xF;  sCC[1][1] = (w1 >> 24) & 0xF; sCC[1][2] = w0 & 0x1F;         sCC[1][3] = (w1 >> 6) & 7;
    sAC[1][0] = (w1 >> 21) & 7;   sAC[1][1] = (w1 >> 3) & 7;    sAC[1][2] = (w1 >> 18) & 7;    sAC[1][3] = w1 & 7;
    sTwoCycle = ((sOmH >> G_MDSFT_CYCLETYPE) & 3) == 1;
    sPrimC = col_of(sPrim);
    sEnvC = col_of(sEnv);
    sPrimLodF = sPrimLod / 255.0f;
    combine_compile();
    sUsesTex = 0;
    for (i = 0; i < (sTwoCycle ? 2 : 1); i++) {
        s32 k;

        for (k = 0; k < 4; k++) {
            s32 cc = sCC[i][k];
            s32 ac = sAC[i][k];

            if (cc == G_CCMUX_TEXEL0 || cc == G_CCMUX_TEXEL1 || (k == 2 && (cc == G_CCMUX_TEXEL0_ALPHA ||
                                                                            cc == G_CCMUX_TEXEL1_ALPHA))) {
                sUsesTex = 1;
            }
            if (ac == 1 || ac == 2) {
                sUsesTex = 1;
            }
        }
    }
}

/* El combinador "compilado": cada entrada (ciclo, canal, A/B/C/D) es un índice a sR, la tabla de registros, que
 * por vértice solo cambia en shade, textura y combinado. Mismas reglas que cc_in/ac_in, pero se resuelven una vez
 * por estado y no en cada vértice. */
enum { R_SH = 0, R_TX = 4, R_CB = 5, R_PR = 9, R_EN = 13, R_ZERO = 17, R_ONE = 18, R_PLOD = 19, R_N = 20 };
static float sR[R_N];
static u8 sCi[2][3][4], sAi[2][4];

static u8 cc_idx(s32 kind, s32 code, s32 ch) {
    if (kind == 2) {
        switch (code) {
            case G_CCMUX_COMBINED: return R_CB + ch;
            case G_CCMUX_TEXEL0: case G_CCMUX_TEXEL1: return R_TX;
            case G_CCMUX_PRIMITIVE: return R_PR + ch;
            case G_CCMUX_SHADE: return R_SH + ch;
            case G_CCMUX_ENVIRONMENT: return R_EN + ch;
            case G_CCMUX_COMBINED_ALPHA: return R_CB + 3;
            case G_CCMUX_TEXEL0_ALPHA: case G_CCMUX_TEXEL1_ALPHA: return R_TX;
            case G_CCMUX_PRIMITIVE_ALPHA: return R_PR + 3;
            case G_CCMUX_SHADE_ALPHA: return R_SH + 3;
            case G_CCMUX_ENV_ALPHA: return R_EN + 3;
            case G_CCMUX_PRIM_LOD_FRAC: return R_PLOD;
            default: return R_ZERO;
        }
    }
    switch (code) {
        case G_CCMUX_COMBINED: return R_CB + ch;
        case G_CCMUX_TEXEL0: case G_CCMUX_TEXEL1: return R_TX;
        case G_CCMUX_PRIMITIVE: return R_PR + ch;
        case G_CCMUX_SHADE: return R_SH + ch;
        case G_CCMUX_ENVIRONMENT: return R_EN + ch;
        case 6: return (kind == 1) ? R_ZERO : R_ONE;
        default: return R_ZERO;
    }
}

static u8 ac_idx(s32 kind, s32 code) {
    switch (code) {
        case 0: return (kind == 2) ? R_ZERO : R_CB + 3;
        case 1: case 2: return R_TX;
        case 3: return R_PR + 3;
        case 4: return R_SH + 3;
        case 5: return R_EN + 3;
        case 6: return (kind == 2) ? R_PLOD : R_ONE;
        default: return R_ZERO;
    }
}

static void combine_compile(void) {
    s32 c, ch, k;

    for (c = 0; c < 2; c++) {
        for (k = 0; k < 4; k++) {
            for (ch = 0; ch < 3; ch++) {
                sCi[c][ch][k] = cc_idx(k, sCC[c][k], ch);
            }
            sAi[c][k] = ac_idx(k, sAC[c][k]);
        }
    }
    sR[R_PR + 0] = sPrimC.r; sR[R_PR + 1] = sPrimC.g; sR[R_PR + 2] = sPrimC.b; sR[R_PR + 3] = sPrimC.a;
    sR[R_EN + 0] = sEnvC.r;  sR[R_EN + 1] = sEnvC.g;  sR[R_EN + 2] = sEnvC.b;  sR[R_EN + 3] = sEnvC.a;
    sR[R_ZERO] = 0.0f;
    sR[R_ONE] = 1.0f;
    sR[R_PLOD] = sPrimLodF;
}

static inline void cycle_eval(s32 c, float* o) {
    s32 ch;

    for (ch = 0; ch < 3; ch++) {
        const u8* i = sCi[c][ch];

        o[ch] = (sR[i[0]] - sR[i[1]]) * sR[i[2]] + sR[i[3]];
    }
    o[3] = (sR[sAi[c][0]] - sR[sAi[c][1]]) * sR[sAi[c][2]] + sR[sAi[c][3]];
}

/* color de salida para una shade dada, con la textura a t (0 o 1, en color y en alfa) */
static Col combine_eval(const Col* shade, float t) {
    Col o;

    sR[R_SH + 0] = shade->r;
    sR[R_SH + 1] = shade->g;
    sR[R_SH + 2] = shade->b;
    sR[R_SH + 3] = shade->a;
    sR[R_TX] = t;
    sR[R_CB + 0] = sR[R_CB + 1] = sR[R_CB + 2] = sR[R_CB + 3] = 0.0f;
    cycle_eval(0, &o.r);
    if (sTwoCycle) {
        sR[R_CB + 0] = o.r;
        sR[R_CB + 1] = o.g;
        sR[R_CB + 2] = o.b;
        sR[R_CB + 3] = o.a;
        cycle_eval(1, &o.r);
    }
    return o;
}

static u32 pack(float r, float g, float b, float a) {
    return ((u32) (clamp01(a) * 255.0f) << 24) | ((u32) (clamp01(r) * 255.0f) << 16) |
           ((u32) (clamp01(g) * 255.0f) << 8) | (u32) (clamp01(b) * 255.0f);
}

/* ------------------------------------------------------------------ estado para la PVR */

static s32 sStateDirty = 1;
static GbState sState;


static const u8* tile_texture(GbTexture* tx, Tile* t) {
    s32 i;
    const Load* ld = NULL;
    s32 w, h, bpp;

    for (i = 0; i < 8; i++) {
        if (sLoads[i].valid && sLoads[i].tmem == t->tmem) {
            ld = &sLoads[i];
        }
    }
    if (ld == NULL) {
        return NULL;
    }
    w = ((t->lrs - t->uls) >> 2) + 1;
    h = ((t->lrt - t->ult) >> 2) + 1;
    if (t->masks && !(t->cms & G_TX_CLAMP) && (1 << t->masks) < w) {
        w = 1 << t->masks;
    }
    if (t->maskt && !(t->cmt & G_TX_CLAMP) && (1 << t->maskt) < h) {
        h = 1 << t->maskt;
    }
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) {
        return NULL;
    }
    tx->addr = ld->addr;
    tx->fmt = t->fmt;
    tx->siz = t->siz;
    tx->width = w;
    tx->height = h;
    tx->stride = ld->stride ? ld->stride : t->line * 8;
    tx->clamp_s = (t->cms & G_TX_CLAMP) != 0;
    tx->clamp_t = (t->cmt & G_TX_CLAMP) != 0;
    tx->mirror_s = (t->cms & G_TX_MIRROR) != 0;
    tx->mirror_t = (t->cmt & G_TX_MIRROR) != 0;
    tx->tlut = NULL;
    tx->tlut_ia = ((sOmH >> G_MDSFT_TEXTLUT) & 3) == 3;
    if (t->fmt == G_IM_FMT_CI) {
        if (((sOmH >> G_MDSFT_TEXTLUT) & 3) == 0) {
            /* CI con la TLUT apagada: el RDP da el índice tal cual, como intensidad (los textos de los menús de
             * carrera, CI8 teñidos con el color primario) */
            tx->fmt = G_IM_FMT_I;
        } else {
            tx->tlut = sTlut[t->siz == G_IM_SIZ_4b ? (t->palette & 0xF) : 0];
        }
    }
    bpp = (t->siz == G_IM_SIZ_32b) ? 4 : (t->siz == G_IM_SIZ_16b) ? 2 : 1;
    tx->be16 = (t->siz == G_IM_SIZ_16b || t->siz == G_IM_SIZ_32b) && fzx_is_mio0(tx->addr, bpp);
    tx->tlut_be16 = tx->tlut != NULL && fzx_is_mio0(tx->tlut, 2);
    return tx->addr;
}

u32 gPortProfStates, gPortProfClipN;

static void state_update(void) {
    u32 cyc = (sOmH >> G_MDSFT_CYCLETYPE) & 3;

    gPortProfStates++;
    u32 bl;
    s32 a, m, b;

    combine_decode();
    bzero(&sState, sizeof(sState));
    sState.textured = sUsesTex && sTexture.on && tile_texture(&sState.tex, &sTiles[sTexture.tile]) != NULL;
    sState.filter = ((sOmH >> G_MDSFT_TEXTFILT) & 3) != 0;   /* G_TF_POINT = 0 */
    /* mezcla: la del 2.º ciclo en 2-ciclos, la del 1.º si no (en 1 ciclo son iguales) */
    bl = sOmL >> 16;
    if (cyc == 1) {
        a = (bl >> 8) & 3;
        m = (bl >> 4) & 3;
        b = bl & 3;
    } else {
        a = (bl >> 10) & 3;
        m = (bl >> 6) & 3;
        b = (bl >> 2) & 3;
    }
    if (m == 1 && b == 0 && a == 0) {
        sState.blend = 1;   /* CLR_IN * A_IN + CLR_MEM * (1 - A) */
    } else if (m == 1 && b == 2) {
        sState.blend = 2;   /* ... + CLR_MEM: suma */
    } else if ((sOmL & 3) == G_AC_THRESHOLD || (sOmL & ZMODE_XLU) == ZMODE_XLU) {
        sState.blend = 1;
    }
    sState.alpha_tex = 1;
    sState.use_offset = 1;
    sState.depth_test = (sGeom & G_ZBUFFER) && (sOmL & Z_CMP);
    sState.depth_write = (sGeom & G_ZBUFFER) && (sOmL & Z_UPD);
    sState.decal = (sOmL & ZMODE_DEC) == ZMODE_DEC;
    trace("S cc=%06x:%08x om=%06x:%08x g=%06x tex=%d", (unsigned) sCombine0, (unsigned) sCombine1, (unsigned) sOmH,
          (unsigned) sOmL, (unsigned) sGeom, sState.textured);
    if (sState.textured) {
        trace(" f%d s%d %dx%d st%d tl%d be%d @%08x [%02x%02x %02x%02x]", sState.tex.fmt, sState.tex.siz,
              sState.tex.width, sState.tex.height, sState.tex.stride, sState.tex.tlut != NULL, sState.tex.be16,
              (unsigned) sState.tex.addr, sState.tex.addr[0], sState.tex.addr[1], sState.tex.addr[2],
              sState.tex.addr[3]);
    }
    trace(" bl%d zt%d zw%d prim=%08x env=%08x\n", sState.blend, sState.depth_test, sState.depth_write,
          (unsigned) sPrim, (unsigned) sEnv);
    gb_set_state(&sState);
    sStateDirty = 0;
    {
        /* lo que usa to_screen además del viewport: combinador, colores, tile actual, con textura o sin ella */
        static u32 last[10];
        Tile* tl = &sTiles[sTexture.tile];
        u32 sig[10] = { sCombine0, sCombine1, sPrim, sEnv, sPrimLod | (sOmH & (3 << G_MDSFT_CYCLETYPE)),
                        (u32) sTexture.tile | ((u32) sState.textured << 8), (u32) tl->shifts | ((u32) tl->shiftt << 8),
                        (u32) tl->uls, (u32) tl->ult, 0 };

        s32 k, diff = 0;

        for (k = 0; k < 10; k++) {
            diff |= sig[k] != last[k];
            last[k] = sig[k];
        }
        if (diff) {
            sScrGen++;
        }
    }
}

/* ------------------------------------------------------------------ triángulos */

static void to_screen(GbVert* o, const RVtx* v) {
    float iw = 1.0f / v->w;
    Col shade = { v->r, v->g, v->b, v->a };
    Col f0, f1;

    o->x = (sVp.tx + v->x * iw * sVp.sx) * SCREEN_SCALE;
    o->y = (sVp.ty - v->y * iw * sVp.sy) * SCREEN_SCALE;
    o->z = iw;
    {
        /* shift del tile (0..10: dividir; 11..15: multiplicar) y su origen */
        Tile* tl = &sTiles[sTexture.tile];
        float fs = tl->shifts <= 10 ? 1.0f / (1 << tl->shifts) : (float) (1 << (16 - tl->shifts));
        float ft = tl->shiftt <= 10 ? 1.0f / (1 << tl->shiftt) : (float) (1 << (16 - tl->shiftt));

        o->u = v->u * fs - tl->uls / 4.0f;
        o->v = v->v * ft - tl->ult / 4.0f;
    }
    f0 = combine_eval(&shade, 0.0f);
    if (sState.textured) {
        f1 = combine_eval(&shade, 1.0f);
        o->argb = pack(f1.r - f0.r, f1.g - f0.g, f1.b - f0.b, f1.a - f0.a > 0.0f ? f1.a - f0.a : f0.a);
        o->oargb = pack(f0.r, f0.g, f0.b, 0.0f);
    } else {
        o->argb = pack(f0.r, f0.g, f0.b, f0.a);
        o->oargb = 0;
    }
}

/* recorte contra el plano cercano (z = -w en el espacio de recorte de N64) */
static s32 clip_near(const RVtx* in, s32 n, RVtx* out) {
    s32 i, k = 0;

    for (i = 0; i < n; i++) {
        const RVtx* a = &in[i];
        const RVtx* b = &in[(i + 1) % n];
        float da = a->z + a->w, db = b->z + b->w;

        if (da >= 0.0f) {
            out[k++] = *a;
        }
        if ((da >= 0.0f) != (db >= 0.0f)) {
            float t = da / (da - db);
            RVtx* o = &out[k++];

            o->x = a->x + (b->x - a->x) * t;
            o->y = a->y + (b->y - a->y) * t;
            o->z = a->z + (b->z - a->z) * t;
            o->w = a->w + (b->w - a->w) * t;
            o->u = a->u + (b->u - a->u) * t;
            o->v = a->v + (b->v - a->v) * t;
            o->r = a->r + (b->r - a->r) * t;
            o->g = a->g + (b->g - a->g) * t;
            o->b = a->b + (b->b - a->b) * t;
            o->a = a->a + (b->a - a->a) * t;
        }
    }
    return k;
}

u32 gPortTris;

/* ---- traza de un cuadro (para depurar): gPortTraceFrame = n.º de cuadro; se lee del volcado de RAM con
 * tools/live_ram.py --str gPortTrace ---- */
#ifndef FZX_TRACE_FRAME
#define FZX_TRACE_FRAME 0xFFFFFFFF
#endif
u32 gPortTraceFrame = FZX_TRACE_FRAME; /* n.º de cuadro a trazar (se cambia para depurar, o make PORT_EXTRA=-DFZX_TRACE_FRAME=n) */
u32 gPortTraceMode = 0;        /* o: el cuadro 30 después de entrar en este gGameMode (0 = no) */
static u32 sModeFrames;
char gPortTrace[64 * 1024];
static u32 sTraceLen;
static s32 sTracing;
static s32 sTraceTris;

static void trace(const char* fmt, ...) {
    va_list ap;
    s32 n;

    if (!sTracing || sTraceLen + 200 > sizeof(gPortTrace)) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(gPortTrace + sTraceLen, sizeof(gPortTrace) - sTraceLen, fmt, ap);
    va_end(ap);
    if (n > 0) {
        sTraceLen += n;
    }
}

static u32 sCntTri, sCntClip, sCntCull;
unsigned long long gPortProfVtxNs, gPortProfTriNs;
u32 gPortProfVtxN, gPortProfTriN;
static void draw_tri_(s32 i0, s32 i1, s32 i2);
static void draw_tri(s32 i0, s32 i1, s32 i2) {
    /* (sin cronómetro por triángulo: leer el temporizador cuesta más que el triángulo) */
    draw_tri_(i0, i1, i2);
    gPortProfTriN++;
}

static void draw_tri_(s32 i0, s32 i1, s32 i2) {
    const GbVert* sv[6];
    GbVert clipv[6];
    s32 n, i;
    float area;

    sCntTri++;
    if (i0 >= MAX_VTX || i1 >= MAX_VTX || i2 >= MAX_VTX) {
        return;
    }
    if (sVtx[i0].z + sVtx[i0].w < 0.0f || sVtx[i1].z + sVtx[i1].w < 0.0f || sVtx[i2].z + sVtx[i2].w < 0.0f) {
        /* cruza el plano cercano: el único caso que copia vértices */
        RVtx in[3], poly[6];

        gPortProfClipN++;
        in[0] = sVtx[i0];
        in[1] = sVtx[i1];
        in[2] = sVtx[i2];
        n = clip_near(in, 3, poly);
        if (n < 3) {
            sCntClip++;
            return;
        }
        if (sStateDirty) {
            state_update();
        }
        for (i = 0; i < n; i++) {
            if (poly[i].w < 1e-5f) {
                poly[i].w = 1e-5f;
            }
            to_screen(&clipv[i], &poly[i]);
            sv[i] = &clipv[i];
        }
    } else {
        const s32 idx[3] = { i0, i1, i2 };

        if (sStateDirty) {
            state_update();
        }
        n = 3;
        for (i = 0; i < 3; i++) {
            s32 k = idx[i];

            if (sVtxGen[k] != sScrGen) {
                if (sVtx[k].w < 1e-5f) {
                    RVtx v = sVtx[k];

                    v.w = 1e-5f;
                    to_screen(&sScr[k], &v);
                } else {
                    to_screen(&sScr[k], &sVtx[k]);
                }
                sVtxGen[k] = sScrGen;
            }
            sv[i] = &sScr[k];
        }
    }
    /* descarte de caras: en pantalla con y hacia abajo, las de N64 de frente van en sentido antihorario */
    area = (sv[1]->x - sv[0]->x) * (sv[2]->y - sv[0]->y) - (sv[2]->x - sv[0]->x) * (sv[1]->y - sv[0]->y);
    if (((sGeom & G_CULL_BACK) && area > 0.0f) || ((sGeom & G_CULL_FRONT) && area < 0.0f)) {
        if (sTracing && sCntCull < 3) {
            trace(" CULL (%.0f,%.0f,%.4f) (%.0f,%.0f) (%.0f,%.0f) g=%06x\n", sv[0]->x, sv[0]->y, sv[0]->z, sv[1]->x,
                  sv[1]->y, sv[2]->x, sv[2]->y, (unsigned) sGeom);
        }
        sCntCull++;
        return;
    }
    for (i = 1; i + 1 < n; i++) {
        gb_triangle(sv[0], sv[i], sv[i + 1]);
        gPortTris++;
    }
    if (sTracing && sTraceTris++ < 400) {
        trace(" T (%.0f,%.0f,%.4f) (%.0f,%.0f) (%.0f,%.0f) c=%08x o=%08x uv=%.1f,%.1f\n", sv[0]->x, sv[0]->y,
              sv[0]->z, sv[1]->x, sv[1]->y, sv[2]->x, sv[2]->y, sv[0]->argb, sv[0]->oargb, sv[0]->u, sv[0]->v);
    }
}

/* rectángulo en coordenadas de N64 (píxeles), con u, v en texels en las esquinas */
static void draw_rect(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, s32 flip) {
    RVtx c[4];
    GbVert sv[4];
    Col shade = { 1, 1, 1, 1 };
    Col f0, f1;
    s32 i;

    if (sStateDirty) {
        state_update();
    }
    f0 = combine_eval(&shade, 0.0f);
    f1 = combine_eval(&shade, 1.0f);
    for (i = 0; i < 4; i++) {
        GbVert* o = &sv[i];
        float px = (i & 1) ? x1 : x0;
        float py = (i & 2) ? y1 : y0;
        float pu = (i & 1) ? u1 : u0;
        float pv = (i & 2) ? v1 : v0;

        o->x = px * SCREEN_SCALE;
        o->y = py * SCREEN_SCALE;
        o->z = 1.0f / 65536.0f;
        if (flip) {
            o->u = (i & 2) ? u1 : u0;
            o->v = (i & 1) ? v1 : v0;
        } else {
            o->u = pu;
            o->v = pv;
        }
        if (sState.textured) {
            o->argb = pack(f1.r - f0.r, f1.g - f0.g, f1.b - f0.b, f1.a - f0.a > 0.0f ? f1.a - f0.a : f0.a);
            o->oargb = pack(f0.r, f0.g, f0.b, 0.0f);
        } else {
            o->argb = pack(f0.r, f0.g, f0.b, f0.a);
            o->oargb = 0;
        }
    }
    (void) c;
    gb_triangle(&sv[0], &sv[1], &sv[2]);
    gb_triangle(&sv[1], &sv[3], &sv[2]);
    gPortTris += 2;
    trace(" R (%.0f,%.0f)-(%.0f,%.0f) uv %.1f,%.1f-%.1f,%.1f c=%08x o=%08x\n", sv[0].x, sv[0].y, sv[3].x, sv[3].y,
          sv[0].u, sv[0].v, sv[3].u, sv[3].v, sv[0].argb, sv[0].oargb);
}

static void do_texrect(u32 w0, u32 w1, u32 h1, u32 h2, s32 flip) {
    trace(" TR %08x %08x %08x %08x\n", (unsigned) w0, (unsigned) w1, (unsigned) h1, (unsigned) h2);
    float lrx = ((w0 >> 12) & 0xFFF) / 4.0f, lry = (w0 & 0xFFF) / 4.0f;
    float ulx = ((w1 >> 12) & 0xFFF) / 4.0f, uly = (w1 & 0xFFF) / 4.0f;
    s32 tile = (w1 >> 24) & 7;
    float s = (s16) (h1 >> 16) / 32.0f, t = (s16) (h1 & 0xFFFF) / 32.0f;
    float dsdx = (s16) (h2 >> 16) / 1024.0f, dtdy = (s16) (h2 & 0xFFFF) / 1024.0f;
    u32 cyc = (sOmH >> G_MDSFT_CYCLETYPE) & 3;
    s32 saveTile = sTexture.tile, saveOn = sTexture.on;
    Tile* tl = &sTiles[tile];

    if (cyc == G_CYC_COPY >> G_MDSFT_CYCLETYPE) {
        dsdx /= 4.0f;
        lrx += 1.0f;
        lry += 1.0f;
    }
    s -= tl->uls / 4.0f;
    t -= tl->ult / 4.0f;
    sTexture.tile = tile;
    sTexture.on = 1;
    sStateDirty = 1;
    if (flip) {
        draw_rect(ulx, uly, lrx, lry, s, t, s + dtdy * (lry - uly), t + dsdx * (lrx - ulx), 1);
    } else {
        draw_rect(ulx, uly, lrx, lry, s, t, s + dsdx * (lrx - ulx), t + dtdy * (lry - uly), 0);
    }
    sTexture.tile = saveTile;
    sTexture.on = saveOn;
    sStateDirty = 1;
}

static void do_fillrect(u32 w0, u32 w1) {
    float lrx = ((w0 >> 12) & 0xFFF) / 4.0f, lry = (w0 & 0xFFF) / 4.0f;
    float ulx = ((w1 >> 12) & 0xFFF) / 4.0f, uly = (w1 & 0xFFF) / 4.0f;
    u32 cyc = (sOmH >> G_MDSFT_CYCLETYPE) & 3;

    if (sCimg == sZimg) {
        return;   /* borrar el búfer de profundidad: la PVR lo hace sola */
    }
    if (cyc == (G_CYC_FILL >> G_MDSFT_CYCLETYPE) || cyc == (G_CYC_COPY >> G_MDSFT_CYCLETYPE)) {
        u32 c = sFill & 0xFFFF;   /* RGBA5551 dos veces */
        u32 r = (c >> 11) & 0x1F, g = (c >> 6) & 0x1F, b = (c >> 1) & 0x1F;
        u32 savePrim = sPrim, s0 = sCombine0, s1 = sCombine1, om = sOmL;

        lrx += 1.0f;
        lry += 1.0f;
        /* como un rectángulo de color de primitiva, opaco */
        sPrim = (r * 255 / 31) << 24 | (g * 255 / 31) << 16 | (b * 255 / 31) << 8 | 0xFF;
        sCombine0 = 0xFFFFFF & ((0xF << 20) | (0x1F << 15) | (7 << 12) | (7 << 9) | (0xF << 5) | 0x1F);
        sCombine1 = (0xFu << 28) | (0xFu << 24) | (7 << 21) | (7 << 18) | (3 << 15) | (7 << 12) | (3 << 9) | (3 << 6) |
                    (7 << 3) | 3;
        sOmL = 0;
        sStateDirty = 1;
        draw_rect(ulx, uly, lrx, lry, 0, 0, 0, 0, 0);
        sPrim = savePrim;
        sCombine0 = s0;
        sCombine1 = s1;
        sOmL = om;
        sStateDirty = 1;
    } else {
        draw_rect(ulx, uly, lrx, lry, 0, 0, 0, 0, 0);
    }
}

/* ------------------------------------------------------------------ RDP: texturas */

static void record_load(s32 tmem, const u8* addr, s32 stride) {
    s32 i, slot = -1;

    for (i = 0; i < 8; i++) {
        if (sLoads[i].valid && sLoads[i].tmem == tmem) {
            slot = i;
        }
    }
    if (slot < 0) {
        for (i = 0; i < 8; i++) {
            if (!sLoads[i].valid) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0) {
        slot = 0;
    }
    sLoads[slot].valid = 1;
    sLoads[slot].tmem = tmem;
    sLoads[slot].addr = addr;
    sLoads[slot].stride = stride;
}

static s32 siz_bytes(s32 siz) {
    return siz == G_IM_SIZ_32b ? 4 : siz == G_IM_SIZ_16b ? 2 : 1;   /* 4b: medio byte, se trata aparte */
}

static void do_loadtile(u32 w0, u32 w1) {
    Tile* t = &sTiles[(w1 >> 24) & 7];
    s32 uls = (w0 >> 12) & 0xFFF, ult = w0 & 0xFFF;
    s32 bpp = siz_bytes(sTimg.siz);
    s32 stride = sTimg.width * bpp;
    const u8* base = resolve(sTimg.addr);

    if (sTimg.siz == G_IM_SIZ_4b) {
        stride = sTimg.width / 2;
        record_load(t->tmem, base + (ult >> 2) * stride + (uls >> 2) / 2, stride);
    } else {
        record_load(t->tmem, base + (ult >> 2) * stride + (uls >> 2) * bpp, stride);
    }
    t->uls = uls;
    t->ult = ult;
    t->lrs = (w1 >> 12) & 0xFFF;
    t->lrt = w1 & 0xFFF;
    sStateDirty = 1;
}

static void do_loadblock(u32 w0, u32 w1) {
    Tile* t = &sTiles[(w1 >> 24) & 7];
    s32 uls = (w0 >> 12) & 0xFFF, ult = w0 & 0xFFF;   /* en texels de la imagen (no 10.2) */
    const u8* base = resolve(sTimg.addr);
    s32 off = (ult * sTimg.width + uls) * siz_bytes(sTimg.siz);

    if (sTimg.siz == G_IM_SIZ_4b) {
        off = (ult * sTimg.width + uls) / 2;
    }
    record_load(t->tmem, base + off, 0);   /* filas de line * 8 bytes, como en la TMEM */
    sStateDirty = 1;
}

static void do_loadtlut(u32 w0, u32 w1) {
    Tile* t = &sTiles[(w1 >> 24) & 7];
    s32 count = (((w1 >> 14) & 0x3FF) + 1);
    s32 slot = (t->tmem - 256) / 16;
    const u8* base = resolve(sTimg.addr);
    s32 i;

    (void) w0;
    for (i = 0; i * 16 < count && slot + i < 16; i++) {
        if (slot + i >= 0) {
            sTlut[slot + i] = base + i * 32;
        }
    }
    sStateDirty = 1;
}

/* ------------------------------------------------------------------ la lista */

static u32 sUnknown[256];

/* Para depurar listas que se desvían: las últimas 32 órdenes (dirección, w0, w1) y, en la primera orden desconocida,
 * una copia de esa historia y de la pila de sublistas (gPortBadDL: n.º de cuadro, profundidad, pila[16],
 * historia[32][3]). */
static u32 sHist[32][3];
static u32 sHistPos;
u32 gPortBadDL[2 + 16 + 32 * 3];
extern u32 gPortFrames;

static void run_dl(Gfx* dl) {
    Gfx* stack[16];
    s32 sp = 0;
    s32 guard = 0;

    while (dl != NULL && guard++ < 2000000) {
        u32 w0 = dl->words.w0, w1 = dl->words.w1;
        u32 op = w0 >> 24;

        sHist[sHistPos & 31][0] = (u32) dl;
        sHist[sHistPos & 31][1] = w0;
        sHist[sHistPos & 31][2] = w1;
        sHistPos++;
        dl++;
        switch (op) {
            case G_NOOP: case G_SPNOOP: case G_RDPLOADSYNC: case G_RDPPIPESYNC: case G_RDPTILESYNC:
            case G_RDPFULLSYNC: case G_SETKEYGB: case G_SETKEYR: case G_SETCONVERT: case G_SETSCISSOR:
            case G_LOAD_UCODE: case G_CULLDL: case G_SPECIAL_1: case G_SPECIAL_2: case G_SPECIAL_3: case G_DMA_IO:
                break;
            case G_VTX: {
                unsigned long long t0 = plat_time_ns();

                do_vtx(w0, w1);
                gPortProfVtxNs += plat_time_ns() - t0;
                gPortProfVtxN += (w0 >> 12) & 0xFF;
                break;
            }
                break;
            case G_TRI1:
                draw_tri(((w0 >> 16) & 0xFF) / 2, ((w0 >> 8) & 0xFF) / 2, (w0 & 0xFF) / 2);
                break;
            case G_TRI2: case G_QUAD:
                draw_tri(((w0 >> 16) & 0xFF) / 2, ((w0 >> 8) & 0xFF) / 2, (w0 & 0xFF) / 2);
                draw_tri(((w1 >> 16) & 0xFF) / 2, ((w1 >> 8) & 0xFF) / 2, (w1 & 0xFF) / 2);
                break;
            case G_MODIFYVTX: {
                s32 where = (w0 >> 16) & 0xFF, v = (w0 & 0xFFFF) / 2;

                if (v < MAX_VTX) {
                    if (where == G_MWO_POINT_RGBA) {
                        sVtxGen[v] = 0;
                        sVtx[v].r = (w1 >> 24) / 255.0f;
                        sVtx[v].g = ((w1 >> 16) & 0xFF) / 255.0f;
                        sVtx[v].b = ((w1 >> 8) & 0xFF) / 255.0f;
                        sVtx[v].a = (w1 & 0xFF) / 255.0f;
                    } else if (where == G_MWO_POINT_ST) {
                        sVtx[v].u = (s16) (w1 >> 16) / 32.0f;
                        sVtx[v].v = (s16) (w1 & 0xFFFF) / 32.0f;
                    }
                }
                break;
            }
            case G_TEXTURE:
                sTexture.s = w1 >> 16;
                sTexture.t = w1 & 0xFFFF;
                sTexture.tile = (w0 >> 8) & 7;
                sTexture.on = (w0 & 0xFF) != 0;
                sStateDirty = 1;
                break;
            case G_POPMTX: {
                s32 n = w1 / 64;

                sMvTop -= n;
                if (sMvTop < 0) {
                    sMvTop = 0;
                }
                sMvpDirty = 1;
                sLightsDirty = 1;
                break;
            }
            case G_GEOMETRYMODE:
                sGeom = (sGeom & (w0 | 0xFF000000)) | w1;
                sStateDirty = 1;
                break;
            case G_MTX:
                do_mtx(w0, w1);
                break;
            case G_MOVEWORD: {
                s32 idx = (w0 >> 16) & 0xFF, off = w0 & 0xFFFF;

                if (idx == G_MW_SEGMENT) {
                    sSeg[(off / 4) & 0xF] = w1;
                } else if (idx == G_MW_NUMLIGHT) {
                    sNumLights = w1 / 24;
                    if (sNumLights > 7) {
                        sNumLights = 7;
                    }
                    sLightsDirty = 1;
                }
                break;
            }
            case G_MOVEMEM: {
                s32 idx = w0 & 0xFF, off = ((w0 >> 8) & 0xFF) * 8;
                const u8* p = resolve(w1);

                if (idx == G_MV_VIEWPORT) {
                    const Vp_t* vp = (const Vp_t*) p;

                    sVp.sx = vp->vscale[0] / 4.0f;
                    sVp.sy = vp->vscale[1] / 4.0f;
                    sVp.tx = vp->vtrans[0] / 4.0f;
                    sVp.ty = vp->vtrans[1] / 4.0f;
                    sScrGen++;
                    trace(" VP %08x -> %p: %d %d %d / %d %d %d\n", (unsigned) w1, (void*) p, vp->vscale[0],
                          vp->vscale[1], vp->vscale[2], vp->vtrans[0], vp->vtrans[1], vp->vtrans[2]);
                } else if (idx == G_MV_LIGHT) {
                    s32 n = off / 24 - 2;   /* 0 y 1 son LOOKATX/Y */

                    if (n >= 0 && n < 8) {
                        const Light_t* l = (const Light_t*) p;

                        sLights[n].col[0] = l->col[0];
                        sLights[n].col[1] = l->col[1];
                        sLights[n].col[2] = l->col[2];
                        sLights[n].dir[0] = l->dir[0];
                        sLights[n].dir[1] = l->dir[1];
                        sLights[n].dir[2] = l->dir[2];
                        sLightsDirty = 1;
                    }
                }
                break;
            }
            case G_DL:
                if (((w0 >> 16) & 0xFF) == G_DL_PUSH && sp < 16) {
                    stack[sp++] = dl;
                }
                dl = resolve(w1);
                trace(" DL %08x -> %08x [%08x %08x] sp%d\n", (unsigned) w1, (unsigned) dl,
                      (unsigned) dl->words.w0, (unsigned) dl->words.w1, (int) sp);
                break;
            case G_ENDDL:
                dl = sp > 0 ? stack[--sp] : NULL;
                break;
            case G_BRANCH_Z:
                dl = resolve(sHalf1);   /* siempre el detalle más cercano */
                break;
            case G_RDPHALF_1:
                sHalf1 = w1;
                break;
            case G_RDPHALF_2:
                sHalf2 = w1;
                break;
            case G_SETOTHERMODE_L: case G_SETOTHERMODE_H: {
                s32 len = (w0 & 0xFF) + 1;
                s32 sft = 32 - ((w0 >> 8) & 0xFF) - len;
                u32 mask = ((len >= 32) ? 0xFFFFFFFF : ((1u << len) - 1)) << sft;

                if (op == G_SETOTHERMODE_L) {
                    sOmL = (sOmL & ~mask) | (w1 & mask);
                } else {
                    sOmH = (sOmH & ~mask) | (w1 & mask);
                }
                sStateDirty = 1;
                break;
            }
            case G_RDPSETOTHERMODE:
                sOmH = w0 & 0xFFFFFF;
                sOmL = w1;
                sStateDirty = 1;
                break;
            case G_TEXRECT: case G_TEXRECTFLIP: {
                u32 h1 = dl[0].words.w1, h2 = dl[1].words.w1;

                dl += 2;   /* las dos medias órdenes que siguen (RDPHALF_1/2) */
                do_texrect(w0, w1, h1, h2, op == G_TEXRECTFLIP);
                break;
            }
            case G_SETPRIMDEPTH:
                break;
            case G_LOADTLUT:
                do_loadtlut(w0, w1);
                break;
            case G_SETTILESIZE: {
                trace(" TS %08x %08x\n", (unsigned) w0, (unsigned) w1);
                Tile* t = &sTiles[(w1 >> 24) & 7];

                t->uls = (w0 >> 12) & 0xFFF;
                t->ult = w0 & 0xFFF;
                t->lrs = (w1 >> 12) & 0xFFF;
                t->lrt = w1 & 0xFFF;
                sStateDirty = 1;
                break;
            }
            case G_LOADBLOCK:
                trace(" LB %08x %08x timg=%08x w%d\n", (unsigned) w0, (unsigned) w1, (unsigned) sTimg.addr, sTimg.width);
                do_loadblock(w0, w1);
                break;
            case G_LOADTILE:
                trace(" LT %08x %08x timg=%08x w%d\n", (unsigned) w0, (unsigned) w1, (unsigned) sTimg.addr, sTimg.width);
                do_loadtile(w0, w1);
                break;
            case G_SETTILE: {
                Tile* t = &sTiles[(w1 >> 24) & 7];

                t->fmt = (w0 >> 21) & 7;
                t->siz = (w0 >> 19) & 3;
                t->line = (w0 >> 9) & 0x1FF;
                t->tmem = w0 & 0x1FF;
                t->palette = (w1 >> 20) & 0xF;
                t->cmt = (w1 >> 18) & 3;
                t->maskt = (w1 >> 14) & 0xF;
                t->shiftt = (w1 >> 10) & 0xF;
                t->cms = (w1 >> 8) & 3;
                t->masks = (w1 >> 4) & 0xF;
                t->shifts = w1 & 0xF;
                sStateDirty = 1;
                break;
            }
            case G_FILLRECT:
                do_fillrect(w0, w1);
                break;
            case G_SETFILLCOLOR:
                sFill = w1;
                break;
            case G_SETFOGCOLOR:
                sFog = w1;
                break;
            case G_SETBLENDCOLOR:
                sBlendColor = w1;
                break;
            case G_SETPRIMCOLOR:
                sPrim = w1;
                sPrimLod = w0 & 0xFF;
                sStateDirty = 1;
                break;
            case G_SETENVCOLOR:
                sEnv = w1;
                sStateDirty = 1;
                break;
            case G_SETCOMBINE:
                sCombine0 = w0 & 0xFFFFFF;
                sCombine1 = w1;
                sStateDirty = 1;
                break;
            case G_SETTIMG:
                sTimg.fmt = (w0 >> 21) & 7;
                sTimg.siz = (w0 >> 19) & 3;
                sTimg.width = (w0 & 0xFFF) + 1;
                sTimg.addr = w1;
                break;
            case G_SETZIMG:
                sZimg = w1;
                break;
            case G_SETCIMG:
                sCimg = w1;
                break;
            default:
                trace(" ?? %08x %08x en %08x sp%d\n", (unsigned) w0, (unsigned) w1, (unsigned) (dl - 1), (int) sp);
                if (gPortBadDL[0] == 0) {
                    s32 k;

                    gPortBadDL[0] = gPortFrames + 1;
                    gPortBadDL[1] = sp;
                    for (k = 0; k < 16; k++) {
                        gPortBadDL[2 + k] = k < sp ? (u32) stack[k] : 0;
                    }
                    for (k = 0; k < 32; k++) {
                        gPortBadDL[18 + k * 3 + 0] = sHist[(sHistPos + k) & 31][0];
                        gPortBadDL[18 + k * 3 + 1] = sHist[(sHistPos + k) & 31][1];
                        gPortBadDL[18 + k * 3 + 2] = sHist[(sHistPos + k) & 31][2];
                    }
                }
                if (sUnknown[op]++ == 0) {
                    plat_log("fzx: gfx: orden %02x desconocida (%08x %08x)\n", (unsigned) op, (unsigned) w0,
                             (unsigned) w1);
                }
                break;
        }
    }
}

/* ------------------------------------------------------------------ la tarea */

static s32 sInit;
u32 gPortFrames;
/* perfil (us acumulados): intérprete de la lista (incluye gb_set_state/gb_triangle), y en gfx_pvr.c hash de
 * texturas, cabeceras, espera a la PVR y envío; gPortAudioUs en ultra.c */
u32 gPortProfDl;
static unsigned long long sProfT0, sProfLast;

void fzx_gfx_task(OSTask* t) {
    if (!sInit) {
        gb_init();
        sInit = 1;
    }
    /* el estado del RSP empieza de cero en cada tarea; el del RDP se queda (como en N64) */
    sMvTop = 0;
    sMvpDirty = 1;
    sLightsDirty = 1;
    sStateDirty = 1;
    bzero(sLoads, sizeof(sLoads));
    gb_frame_begin(0xFF000000);
    sModeFrames = (gPortTraceMode != 0 && (u32) gGameMode == gPortTraceMode) ? sModeFrames + 1 : 0;
    sTracing = (gPortFrames == gPortTraceFrame) || sModeFrames == 30;
    sCntTri = sCntClip = sCntCull = sCntVtx = sCntMtx = 0;
    sTraceTris = 0;
    if (sTracing) {
        trace("cuadro %u, modo %x\n", (unsigned) gPortFrames, (unsigned) gGameMode);
    }
    { extern unsigned gPortStage; gPortStage = 10; }
    sProfT0 = plat_time_ns();
    run_dl(resolve((u32) t->t.data_ptr));
    gPortProfDl += (u32) ((plat_time_ns() - sProfT0) / 1000);
    { extern unsigned gPortStage; gPortStage = 11; }
    if (sTracing) {
        trace("fin: tri %u clip %u cull %u vtx %u mtx %u\n", (unsigned) sCntTri, (unsigned) sCntClip,
              (unsigned) sCntCull, (unsigned) sCntVtx, (unsigned) sCntMtx);
        sTracing = 0;
    }
    gb_frame_end();
    gPortFrames++;
    if ((gPortFrames % 300) == 1) {
        GbStats st;

        gb_stats(&st);
        plat_log("fzx: gfx: cuadro %u, %u triángulos en total, texturas %u hechas / %u reusadas, VRAM libre %u\n",
                 (unsigned) gPortFrames, (unsigned) gPortTris, st.textures_made, st.textures_hit, st.vram_free);
        {
            extern unsigned gPortProfHash, gPortProfState, gPortProfWait, gPortProfSubmit;
            extern u32 gPortAudioUs;
            static u32 lastAudio;
            unsigned long long now = plat_time_ns();
            u32 n = sProfLast ? 300 : 1;
            u32 frame = sProfLast ? (u32) ((now - sProfLast) / 1000 / n) : 0;

            plat_log("fzx: perfil (us/cuadro): cuadro %u, lista %u (hash %u, estado %u, vtx %u en %u, tri %u en %u), "
                     "estados %u, recortados %u, espera PVR %u, envio %u, audio %u\n",
                     (unsigned) frame, (unsigned) (gPortProfDl / n), gPortProfHash / n, gPortProfState / n,
                     (unsigned) (gPortProfVtxN / n), (unsigned) (gPortProfVtxNs / 1000 / n),
                     (unsigned) (gPortProfTriN / n), (unsigned) (gPortProfTriNs / 1000 / n),
                     (unsigned) (gPortProfStates / n), (unsigned) (gPortProfClipN / n), gPortProfWait / n, gPortProfSubmit / n, (unsigned) ((gPortAudioUs - lastAudio) / n));
            gPortProfVtxNs = gPortProfTriNs = 0;
            gPortProfVtxN = gPortProfTriN = gPortProfStates = gPortProfClipN = 0;
            gPortProfDl = 0;
            gPortProfHash = gPortProfState = gPortProfWait = gPortProfSubmit = 0;
            lastAudio = gPortAudioUs;
            sProfLast = now;
        }
    }
}
