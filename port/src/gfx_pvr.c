/*
 * gfx_pvr.c - la PVR para gfx_rsp.c: dos listas (opaca, y translúcida en el orden de envío como la N64: presort),
 * cabeceras por estado y una caché de texturas en VRAM (las texturas de N64 decodificadas a ARGB1555/4444).
 * Se compila con las cabeceras de KallistiOS (ver gfx_backend.h).
 */
#include <kos.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "gfx_backend.h"
#include "plat.h"

/* ------------------------------------------------------------------ colas de cada lista */

typedef struct {
    uint32_t w[8];   /* una cabecera o un vértice: 32 bytes */
} Unit;

#define QUEUE_UNITS (48 * 1024)

typedef struct {
    Unit *u;
    int n;
    int hdr_valid;   /* la última cabecera puesta sigue valiendo */
} Queue;

static Queue sQ[2];   /* 0 = OP, 1 = TR */

/* ------------------------------------------------------------------ texturas */

typedef struct {
    int used;
    const void *addr;
    int fmt, siz, w, h, stride, tlut_ia, be16, tlut_be16;
    const void *tlut;
    uint32_t hash;
    pvr_ptr_t vram;
    int pw, ph, pvrfmt;
    unsigned last;
    int next;   /* siguiente de la misma cubeta, +1 (0 = fin) */
} TexEntry;

#define TEX_SLOTS 768
static TexEntry sTex[TEX_SLOTS];
/* Índice por cubetas (dirección ^ hash), con listas encadenadas: buscar mira solo las texturas de la cubeta. Antes se
 * recorrían en sondeo lineal hasta las 768 entradas, saltando huecos, en cada búsqueda: en la pantalla de ajustes
 * de la nave era la mitad del cuadro en la placa. */
#define TEX_BUCKETS 1024
static int sBucket[TEX_BUCKETS];   /* primera entrada + 1 (0 = vacía) */
static int sFreeHint;

static unsigned tex_bucket(const void *addr, uint32_t h) {
    return ((uintptr_t) addr >> 3 ^ h) % TEX_BUCKETS;
}

static void tex_unlink(int i) {
    int *pp = &sBucket[tex_bucket(sTex[i].addr, sTex[i].hash)];

    while (*pp) {
        if (*pp - 1 == i) {
            *pp = sTex[i].next;
            return;
        }
        pp = &sTex[*pp - 1].next;
    }
}
static uint16_t sScratch[1024 * 1024 / 2];
static unsigned sFrame;
static GbStats sStats;
/* perfil (microsegundos acumulados; gfx_rsp.c los saca al log cada 300 cuadros) */
unsigned gPortProfHash, gPortProfState, gPortProfWait, gPortProfSubmit;
#define NOW_US() ((unsigned) timer_us_gettime64())

static int next_pot(int v) {
    int p = 8;
    while (p < v)
        p <<= 1;
    return p;
}

static inline unsigned rd16(const uint8_t *p, int be) {
    return be ? ((unsigned) p[0] << 8) | p[1] : *(const uint16_t *) p;
}

static uint32_t fnv(uint32_t h, const uint8_t *p, int n) {
    while (n--) {
        h ^= *p++;
        h *= 16777619u;
    }
    return h;
}

static int row_bytes(const GbTexture *t) {
    switch (t->siz) {
        case GB_SIZ_4b: return (t->width + 1) / 2;
        case GB_SIZ_8b: return t->width;
        case GB_SIZ_16b: return t->width * 2;
        default: return t->width * 4;
    }
}

/* por palabras de 32 bits (4 veces menos vueltas que byte a byte); filas desalineadas, byte a byte */
static uint32_t fnv32(uint32_t h, const uint8_t *p, int n) {
    if (((uintptr_t) p & 3) == 0) {
        const uint32_t *w = (const uint32_t *) p;

        for (; n >= 4; n -= 4) {
            h ^= *w++;
            h *= 16777619u;
        }
        p = (const uint8_t *) w;
    }
    return fnv(h, p, n);
}

static uint32_t tex_hash(const GbTexture *t) {
    uint32_t h = 2166136261u;
    int y, rb = row_bytes(t);

    /* Entera: un muestreo (fnv32_sparse) confundía textos casi vacíos que ocupan la misma dirección de la arena
     * uno detrás de otro ("Continue" salía con la imagen de otro texto en el menú de pausa) */
    if (t->stride == rb)
        h = fnv32(h, t->addr, rb * t->height);   /* filas seguidas: de una vez */
    else
        for (y = 0; y < t->height; y++)
            h = fnv32(h, t->addr + y * t->stride, rb);
    if (t->tlut)
        h = fnv32(h, t->tlut, t->siz == GB_SIZ_4b ? 32 : 512);
    return h;
}

/* entrada de la paleta -> ARGB1555 o ARGB4444 */
static inline uint16_t tlut_px(const GbTexture *t, int idx, int to4444) {
    unsigned v = rd16(t->tlut + idx * 2, t->tlut_be16);

    if (t->tlut_ia) {
        unsigned i = v >> 12, a = (v >> 4) & 0xF;
        return (uint16_t) (a << 12 | i << 8 | i << 4 | i);
    }
    if (to4444) {
        unsigned r = (v >> 12) & 0xF, g = (v >> 7) & 0xF, b = (v >> 2) & 0xF, a = (v & 1) ? 0xF : 0;
        return (uint16_t) (a << 12 | r << 8 | g << 4 | b);
    }
    return (uint16_t) ((v & 1) << 15 | ((v >> 11) & 0x1F) << 10 | ((v >> 6) & 0x1F) << 5 | ((v >> 1) & 0x1F));
}

/* decodifica a sScratch (pw x ph, la textura arriba a la izquierda); devuelve el formato de la PVR */
static int decode(const GbTexture *t, int pw, int ph) {
    int x, y;
    int as1555 = (t->fmt == GB_FMT_RGBA && t->siz == GB_SIZ_16b) || (t->fmt == GB_FMT_CI && !t->tlut_ia);

    memset(sScratch, 0, (size_t) pw * ph * 2);
    for (y = 0; y < t->height; y++) {
        const uint8_t *row = t->addr + y * t->stride;
        uint16_t *o = sScratch + y * pw;

        for (x = 0; x < t->width; x++) {
            unsigned v, i, a;

            switch (t->fmt * 4 + t->siz) {
                case GB_FMT_RGBA * 4 + GB_SIZ_16b:
                    v = rd16(row + x * 2, t->be16);
                    o[x] = (uint16_t) ((v & 1) << 15 | ((v >> 11) & 0x1F) << 10 | ((v >> 6) & 0x1F) << 5 |
                                       ((v >> 1) & 0x1F));
                    break;
                case GB_FMT_RGBA * 4 + GB_SIZ_32b: {
                    const uint8_t *p = row + x * 4;
                    o[x] = (uint16_t) ((p[3] >> 4) << 12 | (p[0] >> 4) << 8 | (p[1] >> 4) << 4 | (p[2] >> 4));
                    break;
                }
                case GB_FMT_IA * 4 + GB_SIZ_16b:
                    v = rd16(row + x * 2, t->be16);
                    i = v >> 12;
                    a = (v >> 4) & 0xF;
                    o[x] = (uint16_t) (a << 12 | i << 8 | i << 4 | i);
                    break;
                case GB_FMT_IA * 4 + GB_SIZ_8b:
                    v = row[x];
                    i = v >> 4;
                    a = v & 0xF;
                    o[x] = (uint16_t) (a << 12 | i << 8 | i << 4 | i);
                    break;
                case GB_FMT_IA * 4 + GB_SIZ_4b:
                    v = (x & 1) ? row[x >> 1] & 0xF : row[x >> 1] >> 4;
                    i = (v >> 1) * 15 / 7;
                    a = (v & 1) ? 0xF : 0;
                    o[x] = (uint16_t) (a << 12 | i << 8 | i << 4 | i);
                    break;
                case GB_FMT_I * 4 + GB_SIZ_8b:
                    i = row[x] >> 4;
                    o[x] = (uint16_t) (i << 12 | i << 8 | i << 4 | i);
                    break;
                case GB_FMT_I * 4 + GB_SIZ_4b:
                    i = (x & 1) ? row[x >> 1] & 0xF : row[x >> 1] >> 4;
                    o[x] = (uint16_t) (i << 12 | i << 8 | i << 4 | i);
                    break;
                case GB_FMT_CI * 4 + GB_SIZ_8b:
                    o[x] = t->tlut ? tlut_px(t, row[x], !as1555) : 0;
                    break;
                case GB_FMT_CI * 4 + GB_SIZ_4b:
                    v = (x & 1) ? row[x >> 1] & 0xF : row[x >> 1] >> 4;
                    o[x] = t->tlut ? tlut_px(t, v, !as1555) : 0;
                    break;
                default:
                    /* I/IA de 16 bits leídos como 8, YUV...: gris */
                    o[x] = 0xF888;
                    break;
            }
        }
        /* repetir la última columna/fila en el relleno hasta potencia de 2 (para que el filtro no traiga negro) */
        for (; x < pw && t->width > 0; x++)
            o[x] = o[t->width - 1];
    }
    for (; y < ph && t->height > 0; y++)
        memcpy(sScratch + y * pw, sScratch + (t->height - 1) * pw, pw * 2);
    return as1555 ? PVR_TXRFMT_ARGB1555 : PVR_TXRFMT_ARGB4444;
}

static void tex_evict_old(void) {
    int i, best = -1;
    unsigned oldest = sFrame;

    for (i = 0; i < TEX_SLOTS; i++) {
        if (sTex[i].used && sTex[i].last < oldest) {
            oldest = sTex[i].last;
            best = i;
        }
    }
    if (best >= 0) {
        pvr_mem_free(sTex[best].vram);
        tex_unlink(best);
        sTex[best].used = 0;
        sStats.tex_evicted++;
    }
}

/* Memo por dirección: si esta textura (mismos parámetros) ya se comprobó en este cuadro, no se vuelve a calcular su
 * hash (un cuadro usa la misma muchas veces). Un cambio de su contenido dentro del mismo cuadro no se vería hasta el
 * siguiente. */
#define MEMO_SLOTS 1024
static struct {
    GbTexture key;
    unsigned frame;
    TexEntry *e;
} sMemo[MEMO_SLOTS];

static int same_tex(const GbTexture *a, const GbTexture *b) {
    return a->addr == b->addr && a->fmt == b->fmt && a->siz == b->siz && a->width == b->width &&
           a->height == b->height && a->stride == b->stride && a->tlut == b->tlut && a->tlut_ia == b->tlut_ia &&
           a->be16 == b->be16 && a->tlut_be16 == b->tlut_be16;
}

static TexEntry *tex_get_hashed(const GbTexture *t);

static TexEntry *tex_get(const GbTexture *t) {
    unsigned m = ((uintptr_t) t->addr >> 3 ^ (uintptr_t) t->tlut >> 5 ^ (unsigned) t->width * 31) % MEMO_SLOTS;
    TexEntry *e;

    if (sMemo[m].frame == sFrame && sMemo[m].e != NULL && sMemo[m].e->used && same_tex(&sMemo[m].key, t)) {
        sMemo[m].e->last = sFrame;
        sStats.textures_hit++;
        return sMemo[m].e;
    }
    e = tex_get_hashed(t);
    if (e != NULL) {
        sMemo[m].key = *t;
        sMemo[m].frame = sFrame;
        sMemo[m].e = e;
    }
    return e;
}

static TexEntry *tex_get_hashed(const GbTexture *t) {
    extern unsigned gPortStage;
    gPortStage = 30;
    uint32_t h = tex_hash(t);   /* (sin cronómetro: leer el temporizador cuesta tanto como un hash pequeño) */
    unsigned b = tex_bucket(t->addr, h);
    int i, free_i = -1;
    TexEntry *e;
    size_t bytes;
    int tries;

    for (i = sBucket[b]; i; i = e->next) {
        e = &sTex[i - 1];
        if (e->addr == t->addr && e->hash == h && e->fmt == t->fmt && e->siz == t->siz && e->w == t->width &&
            e->h == t->height && e->stride == t->stride && e->tlut == t->tlut && e->tlut_ia == t->tlut_ia &&
            e->be16 == t->be16) {
            e->last = sFrame;
            sStats.textures_hit++;
            return e;
        }
    }
    for (i = 0; i < TEX_SLOTS; i++) {
        int k = (sFreeHint + i) % TEX_SLOTS;

        if (!sTex[k].used) {
            free_i = k;
            sFreeHint = (k + 1) % TEX_SLOTS;
            break;
        }
    }
    if (free_i < 0) {
        tex_evict_old();
        return NULL;
    }
    e = &sTex[free_i];
    e->pw = next_pot(t->width);
    e->ph = next_pot(t->height);
    bytes = (size_t) e->pw * e->ph * 2;
    for (tries = 0; (e->vram = pvr_mem_malloc(bytes)) == NULL && tries < 64; tries++)
        tex_evict_old();
    if (e->vram == NULL)
        return NULL;
    gPortStage = 31;
    e->pvrfmt = decode(t, e->pw, e->ph);
    gPortStage = 32;
    pvr_txr_load_ex(sScratch, e->vram, e->pw, e->ph, PVR_TXRLOAD_16BPP);
    e->used = 1;
    e->addr = t->addr;
    e->fmt = t->fmt;
    e->siz = t->siz;
    e->w = t->width;
    e->h = t->height;
    e->stride = t->stride;
    e->tlut = t->tlut;
    e->tlut_ia = t->tlut_ia;
    e->be16 = t->be16;
    e->tlut_be16 = t->tlut_be16;
    e->hash = h;
    e->last = sFrame;
    e->next = sBucket[b];
    sBucket[b] = free_i + 1;
    sStats.textures_made++;
    return e;
}

/* ------------------------------------------------------------------ estado y cabeceras */

/* profundidad (1/w) del plano de fondo de la PVR y de lo que se marca como fondo (GbState.background): más lejos
 * que lo más lejano del juego (1/w ~ 1e-5), y el fondo por delante del plano para pasar la prueba GEQUAL */
#define BG_PLANE_Z 1e-8f
#define BG_Z 2e-8f

static GbState sState;
static TexEntry *sCurTex;
static int sCurList;
static float sUScale = 1.0f, sVScale = 1.0f;
static pvr_poly_hdr_t sHdr;

static int sStateValid;   /* sState/sHdr valen para este cuadro */

/* GbState es todo palabras de 32 bits (enteros y punteros): comparar por palabras */
static int same_state(const GbState *a, const GbState *b) {
    const uint32_t *x = (const uint32_t *) a, *y = (const uint32_t *) b;
    unsigned i;

    for (i = 0; i < sizeof(GbState) / 4; i++)
        if (x[i] != y[i])
            return 0;
    return 1;
}

void gb_set_state(const GbState *st) {
    pvr_poly_cxt_t cxt;
    int list;

    if (sStateValid && same_state(st, &sState) && (!st->textured || sCurTex != NULL))
        return;   /* la misma cabecera: las colas siguen valiendo con la última que se puso */
    sStateValid = 1;
    sState = *st;
    sCurTex = st->textured ? tex_get(&st->tex) : NULL;
    list = (st->blend || st->overlay) ? 1 : 0;
    sCurList = list;
    if (sCurTex) {
        pvr_poly_cxt_txr(&cxt, list ? PVR_LIST_TR_POLY : PVR_LIST_OP_POLY, sCurTex->pvrfmt | PVR_TXRFMT_TWIDDLED,
                         sCurTex->pw, sCurTex->ph, sCurTex->vram, st->filter ? PVR_FILTER_BILINEAR : PVR_FILTER_NONE);
        cxt.txr.env = st->decal_alpha ? PVR_TXRENV_DECAL : PVR_TXRENV_MODULATEALPHA;
        cxt.txr.uv_clamp = (st->tex.clamp_s ? PVR_UVCLAMP_U : 0) | (st->tex.clamp_t ? PVR_UVCLAMP_V : 0);
        cxt.txr.uv_flip = (st->tex.mirror_s ? PVR_UVFLIP_U : 0) | (st->tex.mirror_t ? PVR_UVFLIP_V : 0);
        sUScale = 1.0f / sCurTex->pw;
        sVScale = 1.0f / sCurTex->ph;
    } else {
        pvr_poly_cxt_col(&cxt, list ? PVR_LIST_TR_POLY : PVR_LIST_OP_POLY);
    }
    cxt.gen.shading = PVR_SHADE_GOURAUD;
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.gen.specular = ((sCurTex && st->use_offset && !st->decal_alpha) || st->fog) ? PVR_SPECULAR_ENABLE
                                                                                     : PVR_SPECULAR_DISABLE;
    cxt.gen.fog_type = st->fog ? PVR_FOG_VERTEX : PVR_FOG_DISABLE;
    cxt.depth.comparison = (st->depth_test || st->background) ? PVR_DEPTHCMP_GEQUAL : PVR_DEPTHCMP_ALWAYS;
    cxt.depth.write = (st->depth_write && !st->background) ? PVR_DEPTHWRITE_ENABLE : PVR_DEPTHWRITE_DISABLE;
    if (list) {
        cxt.gen.alpha = PVR_ALPHA_ENABLE;
        cxt.blend.src = st->blend ? PVR_BLEND_SRCALPHA : PVR_BLEND_ONE;
        cxt.blend.dst = st->blend == 2 ? PVR_BLEND_ONE : (st->blend ? PVR_BLEND_INVSRCALPHA : PVR_BLEND_ZERO);
        if (sCurTex)
            cxt.txr.alpha = PVR_TXRALPHA_ENABLE;
    }
    pvr_poly_compile(&sHdr, &cxt);
    sQ[0].hdr_valid = sQ[1].hdr_valid = 0;
}

/* ------------------------------------------------------------------ triángulos */

/* Reserva la línea de caché de una unidad de la cola sin leerla de memoria (movca.l): se va a escribir entera. Sin
 * esto, cada escritura en la cola (1,5 MB que se recorren en cada cuadro, nunca en caché) lee antes la línea de la
 * RAM: en la placa, unos 200 ciclos por vértice. */
static inline void unit_alloc(Unit *u) {
    __asm__ volatile("movca.l r0, @%0" : : "r"(u) : "r0", "memory");
}

static void push(Queue *q, const void *data) {
    if (q->n < QUEUE_UNITS) {
        unit_alloc(&q->u[q->n]);
        memcpy(&q->u[q->n++], data, 32);
    }
}

void gb_triangle(const GbVert *a, const GbVert *b, const GbVert *c) {
    Queue *q = &sQ[sCurList];
    const GbVert *v[3] = { a, b, c };
    pvr_vertex_t *pv;
    float zs = sState.decal ? 1.0005f : 1.0f;
    int bg = sState.background;
    int i;

    if (sState.textured && sCurTex == NULL)
        return;   /* sin sitio en VRAM: mejor no dibujar que dibujar mal */
    if (q->n + 4 > QUEUE_UNITS)
        return;
    if (!q->hdr_valid) {
        unit_alloc(&q->u[q->n]);
        memcpy(&q->u[q->n++], &sHdr, sizeof(Unit));
        q->hdr_valid = 1;
    }
    /* los vértices se escriben directamente en la cola (sin uno local y memcpy) */
    pv = (pvr_vertex_t *) &q->u[q->n];
    for (i = 0; i < 3; i++, pv++) {
        unit_alloc((Unit *) pv);
        pv->flags = (i == 2) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        pv->x = v[i]->x;
        pv->y = v[i]->y;
        pv->z = bg ? BG_Z : v[i]->z * zs;
        pv->u = v[i]->u * sUScale;
        pv->v = v[i]->v * sVScale;
        pv->argb = v[i]->argb;
        pv->oargb = v[i]->oargb;
    }
    q->n += 3;
    sStats.tris++;
}

/* ------------------------------------------------------------------ cuadro */

void gb_init(void) {
    pvr_init_params_t params = {
        { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_0 },
        1024 * 1024,   /* búfer de vértices de la PVR */
        0,             /* sin DMA (en la NAOMI, mejor por las store queues) */
        0,             /* sin FSAA */
        1,             /* sin ordenar la lista translúcida: en el orden de envío, como la N64 */
        3,             /* opb_overflow_count */
    };
    int i;

    pvr_init(&params);
    pvr_set_zclip(BG_PLANE_Z);   /* el plano de fondo de la PVR, más lejos que cualquier cosa del juego */
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);
    for (i = 0; i < 2; i++) {
        sQ[i].u = memalign(32, QUEUE_UNITS * sizeof(Unit));
        if (sQ[i].u == NULL)
            plat_fatal("fzx: sin memoria para las colas de la PVR\n");
        sQ[i].n = 0;
    }
    plat_log("fzx: PVR lista, VRAM libre %u\n", (unsigned) pvr_mem_available());
}

void gb_frame_begin(unsigned clear_argb) {
    (void) clear_argb;
    sQ[0].n = sQ[1].n = 0;
    sQ[0].hdr_valid = sQ[1].hdr_valid = 0;
    sStateValid = 0;
    sFrame++;
}

unsigned gPortStage;

#ifdef FZX_FPS
/* Contador de cuadros por segundo arriba a la derecha (make PORT_EXTRA=-DFZX_FPS): cuántas imágenes manda el juego
 * por segundo, medido cada medio segundo. Verde a 59 o más, amarillo de 30 a 58, rojo por debajo. */
static void fps_rect(float x0, float y0, float x1, float y1, unsigned argb) {
    GbVert v[4];
    int i;

    for (i = 0; i < 4; i++) {
        v[i].x = (i & 1) ? x1 : x0;
        v[i].y = (i & 2) ? y1 : y0;
        v[i].z = 1.0f;
        v[i].u = v[i].v = 0.0f;
        v[i].argb = argb;
        v[i].oargb = 0;
    }
    gb_triangle(&v[0], &v[1], &v[2]);
    gb_triangle(&v[1], &v[3], &v[2]);
}

static void fps_number(unsigned shown, float y, unsigned col) {
    /* segmentos a b c d e f g (bit 0 = a) */
    static const unsigned char seg[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    int d, n, digits[3], nd = 0;
    const float W = 12, H = 22, T = 3, GAP = 5;
    float x;

    n = shown > 999 ? 999 : (int) shown;
    do {
        digits[nd++] = n % 10;
        n /= 10;
    } while (n > 0);
    x = 640 - 12 - nd * (W + GAP) + GAP;
    fps_rect(x - 4, y - 4, 640 - 8, y + H + 4, 0xFF000000);
    for (d = nd - 1; d >= 0; d--, x += W + GAP) {
        unsigned m = seg[digits[d]];

        if (m & 0x01) fps_rect(x, y, x + W, y + T, col);
        if (m & 0x02) fps_rect(x + W - T, y, x + W, y + H / 2, col);
        if (m & 0x04) fps_rect(x + W - T, y + H / 2, x + W, y + H, col);
        if (m & 0x08) fps_rect(x, y + H - T, x + W, y + H, col);
        if (m & 0x10) fps_rect(x, y + H / 2, x + T, y + H, col);
        if (m & 0x20) fps_rect(x, y, x + T, y + H / 2, col);
        if (m & 0x40) fps_rect(x, y + (H - T) / 2, x + W, y + (H + T) / 2, col);
    }
}

/* arriba, imágenes dibujadas por segundo (verde >= 59, amarillo >= 30, rojo menos); debajo, en azul, cuadros del
 * juego por segundo (su velocidad: con el salto de cuadros puede ir a 60 aunque se dibujen menos) */
static void fps_draw(void) {
    extern unsigned gPortFrames;
    static unsigned last, frames, lastGame, shown = 60, shownGame = 60;
    GbState st;
    unsigned now = NOW_US();

    frames++;
    if (last == 0) {
        last = now;
        lastGame = gPortFrames;
    } else if (now - last >= 500000) {
        shown = (unsigned) ((frames * 1000000ULL + (now - last) / 2) / (now - last));
        shownGame = (unsigned) (((gPortFrames - lastGame) * 1000000ULL + (now - last) / 2) / (now - last));
        frames = 0;
        last = now;
        lastGame = gPortFrames;
    }
    memset(&st, 0, sizeof(st));
    st.overlay = 1;
    gb_set_state(&st);
    fps_number(shown, 12, shown >= 59 ? 0xFF40FF40 : (shown >= 30 ? 0xFFFFE040 : 0xFFFF4040));
    fps_number(shownGame, 44, 0xFF40C0FF);
}
#endif

static unsigned sFogColor;

void gb_fog_color(unsigned rgba) {
    sFogColor = rgba;
}

void gb_frame_end(void) {
    int l, i;

#ifdef FZX_FPS
    fps_draw();
#endif
    unsigned t0 = NOW_US(), t1;

    gPortStage = 20;
    pvr_wait_ready();
    t1 = NOW_US();
    gPortProfWait += t1 - t0;
    pvr_fog_vertex_color((sFogColor >> 24) / 255.0f, ((sFogColor >> 16) & 0xFF) / 255.0f,
                         ((sFogColor >> 8) & 0xFF) / 255.0f);
    gPortStage = 21;
    pvr_scene_begin();
    for (l = 0; l < 2; l++) {
        pvr_list_begin(l ? PVR_LIST_TR_POLY : PVR_LIST_OP_POLY);
        /* en bloques (store queues directas al TA), no de 32 en 32 bytes */
        for (i = 0; i < sQ[l].n; i += 256) {
            int k = sQ[l].n - i < 256 ? sQ[l].n - i : 256;

            pvr_prim(&sQ[l].u[i], (size_t) k * 32);
        }
        pvr_list_finish();
    }
    gPortStage = 22;
    pvr_scene_finish();
    gPortStage = 23;
    gPortProfSubmit += NOW_US() - t1;
    sStats.frames++;
}

void gb_stats(GbStats *out) {
    *out = sStats;
    out->vram_free = (unsigned) pvr_mem_available();
}
