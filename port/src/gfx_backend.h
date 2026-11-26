/*
 * gfx_backend.h - entre el intérprete de listas de N64 (gfx_rsp.c, con las cabeceras de la decomp) y la PVR
 * (gfx_pvr.c, con las de KallistiOS). Solo tipos de C.
 *
 * El intérprete hace lo del RSP (matrices, luces, recorte, coordenadas de textura) y deja triángulos en pantalla
 * (640x480) con el estado del RDP ya traducido: qué textura, cómo se mezcla y la profundidad.
 */
#ifndef FZX_GFX_BACKEND_H
#define FZX_GFX_BACKEND_H

/* Un vértice ya en pantalla. z = 1/w (la PVR ordena por 1/w: más grande, más cerca). u, v en texels de la
 * textura N64 (sin dividir por su tamaño; lo hace el backend, que sabe el tamaño en VRAM). */
typedef struct {
    float x, y, z;
    float u, v;
    unsigned argb;    /* color base: textura * base */
    unsigned oargb;   /* color que se suma (offset); alfa = niebla, sin usar aún */
} GbVert;

/* Formatos de textura de N64 (G_IM_FMT_*, G_IM_SIZ_*) */
enum { GB_FMT_RGBA = 0, GB_FMT_YUV = 1, GB_FMT_CI = 2, GB_FMT_IA = 3, GB_FMT_I = 4 };
enum { GB_SIZ_4b = 0, GB_SIZ_8b = 1, GB_SIZ_16b = 2, GB_SIZ_32b = 3 };

/* Una textura tal como está en la RAM de N64 (o en la "ROM" ya copiada). */
typedef struct {
    const unsigned char *addr;     /* primer texel */
    int fmt, siz;
    int width, height;             /* en texels */
    int stride;                    /* bytes por fila en la RAM */
    const unsigned char *tlut;     /* paleta (CI), NULL si no */
    int tlut_ia;                   /* paleta IA16 en vez de RGBA16 */
    int be16;                      /* los texels de 16 bits (y la paleta) están en big-endian (salen de MIO0) */
    int tlut_be16;
    int clamp_s, clamp_t, mirror_s, mirror_t;
} GbTexture;

typedef struct {
    int textured;
    GbTexture tex;
    int filter;          /* 1 = bilineal */
    int blend;           /* 0 opaco, 1 mezcla por alfa (XLU), 2 suma */
    int alpha_tex;       /* el alfa sale de la textura (si no, del vértice) */
    int use_offset;      /* usar oargb */
    int depth_test, depth_write;
    int decal;           /* G_ZS_PRIM / decal: un poco por delante */
} GbState;

void gb_init(void);
void gb_frame_begin(unsigned clear_argb);
void gb_frame_end(void);
void gb_set_state(const GbState *st);
void gb_triangle(const GbVert *a, const GbVert *b, const GbVert *c);

/* Estadística para el log */
typedef struct {
    unsigned frames, tris, textures_made, textures_hit, tex_evicted, vram_free;
} GbStats;
void gb_stats(GbStats *out);

#endif
