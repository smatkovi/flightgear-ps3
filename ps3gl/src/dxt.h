/* DXT (S3TC) compression of ps3gl's textures, on the SPUs.
   DXT1 for opaque textures (8 bytes per 4x4 block, an eighth of A8R8G8B8),
   DXT5 for textures with alpha (16 bytes per block, a quarter). */
#ifndef PS3GL_DXT_H
#define PS3GL_DXT_H

#include <stdint.h>

/* One job of the SPU program (ps3gl/spu/dxt_spu.c): compress `brows` rows
   of 4x4 blocks of an RGBA8 image `width` pixels wide. */
typedef struct {
    uint64_t src;       /* address of the first of the 4 * brows pixel rows */
    uint64_t dst;       /* address of the first block row of the output */
    uint32_t width;     /* pixels, a multiple of 16 (DMA sizes) */
    uint32_t brows;
    uint32_t alpha;     /* 1: DXT5, 0: DXT1 */
    uint32_t pad;
} __attribute__((aligned(16))) dxt_job;

#ifndef __SPU__
/* Starts the SPU side; without it everything is compressed on the PPU. */
void dxt_init(void);
/* Bytes of one compressed level */
uint32_t dxt_level_size(uint32_t w, uint32_t h, int alpha);
/* Compresses the n levels of a mipmap chain: rgba holds the levels one after
   the other (level i is max(1, w >> i) x max(1, h >> i), RGBA8, 128-byte
   aligned base), out receives them compressed, one after the other. */
void dxt_compress_chain(const uint8_t *rgba, uint32_t w, uint32_t h, int n, int alpha, uint8_t *out);
#endif

#endif
