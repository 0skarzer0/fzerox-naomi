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
} TexEntry;

#define TEX_SLOTS 768
static TexEntry sTex[TEX_SLOTS];
static uint16_t sScratch[1024 * 1024 / 2];
static unsigned sFrame;
static GbStats sStats;

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

static uint32_t tex_hash(const GbTexture *t) {
    uint32_t h = 2166136261u;
    int y, rb = row_bytes(t);

    for (y = 0; y < t->height; y++)
        h = fnv(h, t->addr + y * t->stride, rb);
    if (t->tlut)
        h = fnv(h, t->tlut, t->siz == GB_SIZ_4b ? 32 : 512);
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
        sTex[best].used = 0;
        sStats.tex_evicted++;
    }
}

static TexEntry *tex_get(const GbTexture *t) {
    extern unsigned gPortStage;
    gPortStage = 30;
    uint32_t h = tex_hash(t);
    unsigned slot = ((uintptr_t) t->addr >> 3 ^ h) % TEX_SLOTS;
    int i, free_i = -1;
    TexEntry *e;
    size_t bytes;
    int tries;

    for (i = 0; i < TEX_SLOTS; i++) {
        e = &sTex[(slot + i) % TEX_SLOTS];
        if (!e->used) {
            if (free_i < 0)
                free_i = (slot + i) % TEX_SLOTS;
            continue;
        }
        if (e->addr == t->addr && e->hash == h && e->fmt == t->fmt && e->siz == t->siz && e->w == t->width &&
            e->h == t->height && e->stride == t->stride && e->tlut == t->tlut && e->tlut_ia == t->tlut_ia &&
            e->be16 == t->be16) {
            e->last = sFrame;
            sStats.textures_hit++;
            return e;
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
    sStats.textures_made++;
    return e;
}

/* ------------------------------------------------------------------ estado y cabeceras */

static GbState sState;
static TexEntry *sCurTex;
static int sCurList;
static float sUScale = 1.0f, sVScale = 1.0f;
static pvr_poly_hdr_t sHdr;

void gb_set_state(const GbState *st) {
    pvr_poly_cxt_t cxt;
    int list;

    sState = *st;
    sCurTex = st->textured ? tex_get(&st->tex) : NULL;
    list = st->blend ? 1 : 0;
    sCurList = list;
    if (sCurTex) {
        pvr_poly_cxt_txr(&cxt, list ? PVR_LIST_TR_POLY : PVR_LIST_OP_POLY, sCurTex->pvrfmt | PVR_TXRFMT_TWIDDLED,
                         sCurTex->pw, sCurTex->ph, sCurTex->vram, st->filter ? PVR_FILTER_BILINEAR : PVR_FILTER_NONE);
        cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
        cxt.txr.uv_clamp = (st->tex.clamp_s ? PVR_UVCLAMP_U : 0) | (st->tex.clamp_t ? PVR_UVCLAMP_V : 0);
        cxt.txr.uv_flip = (st->tex.mirror_s ? PVR_UVFLIP_U : 0) | (st->tex.mirror_t ? PVR_UVFLIP_V : 0);
        sUScale = 1.0f / sCurTex->pw;
        sVScale = 1.0f / sCurTex->ph;
    } else {
        pvr_poly_cxt_col(&cxt, list ? PVR_LIST_TR_POLY : PVR_LIST_OP_POLY);
    }
    cxt.gen.shading = PVR_SHADE_GOURAUD;
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.gen.specular = (sCurTex && st->use_offset) ? PVR_SPECULAR_ENABLE : PVR_SPECULAR_DISABLE;
    cxt.depth.comparison = st->depth_test ? PVR_DEPTHCMP_GEQUAL : PVR_DEPTHCMP_ALWAYS;
    cxt.depth.write = st->depth_write ? PVR_DEPTHWRITE_ENABLE : PVR_DEPTHWRITE_DISABLE;
    if (list) {
        cxt.gen.alpha = PVR_ALPHA_ENABLE;
        cxt.blend.src = PVR_BLEND_SRCALPHA;
        cxt.blend.dst = st->blend == 2 ? PVR_BLEND_ONE : PVR_BLEND_INVSRCALPHA;
        if (sCurTex)
            cxt.txr.alpha = PVR_TXRALPHA_ENABLE;
    }
    pvr_poly_compile(&sHdr, &cxt);
    sQ[0].hdr_valid = sQ[1].hdr_valid = 0;
}

/* ------------------------------------------------------------------ triángulos */

static void push(Queue *q, const void *data) {
    if (q->n < QUEUE_UNITS)
        memcpy(&q->u[q->n++], data, 32);
}

void gb_triangle(const GbVert *a, const GbVert *b, const GbVert *c) {
    Queue *q = &sQ[sCurList];
    const GbVert *v[3] = { a, b, c };
    pvr_vertex_t pv;
    int i;

    if (sState.textured && sCurTex == NULL)
        return;   /* sin sitio en VRAM: mejor no dibujar que dibujar mal */
    if (q->n + 4 > QUEUE_UNITS)
        return;
    if (!q->hdr_valid) {
        push(q, &sHdr);
        q->hdr_valid = 1;
    }
    for (i = 0; i < 3; i++) {
        pv.flags = (i == 2) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        pv.x = v[i]->x;
        pv.y = v[i]->y;
        pv.z = sState.decal ? v[i]->z * 1.0005f : v[i]->z;
        pv.u = v[i]->u * sUScale;
        pv.v = v[i]->v * sVScale;
        pv.argb = v[i]->argb;
        pv.oargb = v[i]->oargb;
        push(q, &pv);
    }
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
    sFrame++;
}

unsigned gPortStage;

void gb_frame_end(void) {
    int l, i;

    gPortStage = 20;
    pvr_wait_ready();
    gPortStage = 21;
    pvr_scene_begin();
    for (l = 0; l < 2; l++) {
        pvr_list_begin(l ? PVR_LIST_TR_POLY : PVR_LIST_OP_POLY);
        for (i = 0; i < sQ[l].n; i++)
            pvr_prim(&sQ[l].u[i], 32);
        pvr_list_finish();
    }
    gPortStage = 22;
    pvr_scene_finish();
    gPortStage = 23;
    sStats.frames++;
}

void gb_stats(GbStats *out) {
    *out = sStats;
    out->vram_free = (unsigned) pvr_mem_available();
}
