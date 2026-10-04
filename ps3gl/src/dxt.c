/* DXT compression of textures: the large mipmap levels go to SPU threads
   (up to six, rows of blocks split between them), the small ones and
   everything without SPUs are done on the PPU with the same encoder. */
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <sys/spu.h>
#include <lv2/spu.h>

#define STBD_FABS(x) __builtin_fabs(x)
#define STB_DXT_STATIC
#define STB_DXT_IMPLEMENTATION
#include "stb_dxt.h"
#include "dxt.h"

/* The SPU program, embedded at build time (build/gen/dxt_spu_bin.c) */
extern const unsigned char dxt_spu_bin[];

#define MAX_SPUS 6
#define MAX_JOBS 64

static sysSpuImage image;
static int spus;            /* SPU threads available: 0 = compress on the PPU */

void dxt_init(void)
{
    static int done;
    if (done) return;
    done = 1;
    sysSpuInitialize(MAX_SPUS, 0);      /* fails harmlessly if already done */
    if (sysSpuImageImport(&image, dxt_spu_bin, SPU_IMAGE_PROTECT) == 0)
        spus = MAX_SPUS;
}

uint32_t dxt_level_size(uint32_t w, uint32_t h, int alpha)
{
    return ((w + 3) / 4) * ((h + 3) / 4) * (alpha ? 16 : 8);
}

/* Any size, on the PPU: partial blocks repeat the last row and column */
static void compress_ppu(const uint8_t *src, uint32_t w, uint32_t h, uint32_t brow0, uint32_t brows,
                         int alpha, uint8_t *out)
{
    uint32_t bx, by, x, y, bs = alpha ? 16 : 8;
    uint8_t block[64];
    for (by = brow0; by < brow0 + brows; by++)
        for (bx = 0; bx < (w + 3) / 4; bx++) {
            for (y = 0; y < 4; y++)
                for (x = 0; x < 4; x++) {
                    uint32_t sx = bx * 4 + x < w ? bx * 4 + x : w - 1;
                    uint32_t sy = by * 4 + y < h ? by * 4 + y : h - 1;
                    memcpy(block + (y * 4 + x) * 4, src + ((size_t)sy * w + sx) * 4, 4);
                }
            stb_compress_dxt_block(out, block, alpha, STB_DXT_NORMAL);
            out += bs;
        }
}

/* Runs the jobs on a group of SPU threads; 0 if that worked */
static int run_spus(dxt_job *jobs, int njobs)
{
    static const char name[] = "ps3gl-dxt";
    sysSpuThreadGroupAttribute ga;
    sysSpuThreadAttribute ta;
    sysSpuThreadArgument arg;
    sys_spu_group_t group;
    sys_spu_thread_t th;
    u32 cause, status;
    int n = njobs < spus ? njobs : spus, i;

    ga.nsize = sizeof name;
    ga.name = name;
    ga.type = SPU_THREAD_GROUP_TYPE_NORMAL;
    ga.option.ct = 0;
    if (sysSpuThreadGroupCreate(&group, n, 100, &ga) != 0) return -1;
    ta.name = name;
    ta.nsize = sizeof name;
    ta.option = SPU_THREAD_ATTR_NONE;
    for (i = 0; i < n; i++) {
        arg.arg0 = (u64)(uintptr_t)jobs;
        arg.arg1 = njobs;
        arg.arg2 = (u64)n << 32 | i;   /* the SPU's crt0 passes three arguments on */
        arg.arg3 = 0;
        if (sysSpuThreadInitialize(&th, group, i, &image, &ta, &arg) != 0) {
            sysSpuThreadGroupDestroy(group);
            return -1;
        }
    }
    if (sysSpuThreadGroupStart(group) != 0) {
        sysSpuThreadGroupDestroy(group);
        return -1;
    }
    sysSpuThreadGroupJoin(group, &cause, &status);
    sysSpuThreadGroupDestroy(group);
    return 0;
}

void dxt_compress_chain(const uint8_t *rgba, uint32_t w, uint32_t h, int n, int alpha, uint8_t *out)
{
    static dxt_job jobs[MAX_JOBS] __attribute__((aligned(128)));
    int njobs = 0, i, k;
    const uint8_t *src = rgba;
    uint8_t *dst = out;

    for (i = 0; i < n; i++) {
        uint32_t lw = w >> i ? w >> i : 1, lh = h >> i ? h >> i : 1;
        uint32_t brows = (lh + 3) / 4;
        /* SPUs take whole rows of blocks of the levels that are at least
           16 pixels wide (DMA sizes) and 4 high; split between the threads */
        if (spus && lw >= 16 && lh >= 4 && njobs + spus <= MAX_JOBS) {
            uint32_t per = (brows + spus - 1) / spus, row;
            for (k = 0, row = 0; row < brows; k++, row += per) {
                dxt_job *j = &jobs[njobs++];
                j->src = (uint64_t)(uintptr_t)(src + (size_t)row * 4 * lw * 4);
                j->dst = (uint64_t)(uintptr_t)(dst + (size_t)row * (lw / 4) * (alpha ? 16 : 8));
                j->width = lw;
                j->brows = row + per <= brows ? per : brows - row;
                j->alpha = alpha;
                j->pad = 0;
            }
        } else {
            compress_ppu(src, lw, lh, 0, brows, alpha, dst);
        }
        src += (size_t)lw * lh * 4;
        dst += dxt_level_size(lw, lh, alpha);
    }
    if (njobs && run_spus(jobs, njobs) != 0) {
        for (k = 0; k < njobs; k++)     /* no SPUs after all */
            compress_ppu((const uint8_t *)(uintptr_t)jobs[k].src, jobs[k].width, jobs[k].brows * 4, 0,
                         jobs[k].brows, alpha, (uint8_t *)(uintptr_t)jobs[k].dst);
    }
}
