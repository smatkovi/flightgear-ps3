/* SPU program: DXT compression for ps3gl (see ps3gl/src/dxt.h).
   Arguments: address and number of the jobs, and this thread's index in the
   low and the number of threads in the high 32 bits (PSL1GHT's crt0 passes
   only three of the four thread arguments on); thread i of n does jobs
   i, i + n, i + 2n, ...
   Four pixel rows at a time come in by DMA, one row of blocks goes out. */
#include <stdint.h>
#include <string.h>
#include <spu_mfcio.h>
#include <sys/spu_thread.h>

#define STBD_FABS(x) __builtin_fabs(x)
#define STB_DXT_STATIC
#define STB_DXT_IMPLEMENTATION
#include "../src/stb_dxt.h"
#include "../src/dxt.h"

#define MAX_WIDTH 1024      /* ps3gl's MAX_TEX_DIM */

static uint8_t in[4 * MAX_WIDTH * 4] __attribute__((aligned(128)));
static uint8_t out[MAX_WIDTH / 4 * 16] __attribute__((aligned(128)));
static dxt_job job;

static void dma_wait(void)
{
    mfc_write_tag_mask(1);
    mfc_read_tag_status_all();
}

int main(uint64_t jobs, uint64_t njobs, uint64_t thread)
{
    uint64_t j, index = (uint32_t)thread, nthreads = thread >> 32;
    if (!nthreads) nthreads = 1;
    for (j = index; j < njobs; j += nthreads) {
        uint32_t by, bx, r, bs, rowbytes, outbytes;
        mfc_get(&job, jobs + j * sizeof(dxt_job), sizeof(dxt_job), 0, 0, 0);
        dma_wait();
        if (job.width < 16 || job.width > MAX_WIDTH) continue;
        bs = job.alpha ? 16 : 8;
        rowbytes = job.width * 4;
        outbytes = job.width / 4 * bs;
        for (by = 0; by < job.brows; by++) {
            for (r = 0; r < 4; r++)
                mfc_get(in + r * rowbytes, job.src + (uint64_t)(by * 4 + r) * rowbytes, rowbytes, 0, 0, 0);
            dma_wait();
            for (bx = 0; bx < job.width / 4; bx++) {
                uint8_t block[64] __attribute__((aligned(16)));
                for (r = 0; r < 4; r++)
                    memcpy(block + r * 16, in + r * rowbytes + bx * 16, 16);
                stb_compress_dxt_block(out + bx * bs, block, job.alpha, STB_DXT_NORMAL);
            }
            mfc_put(out, job.dst + (uint64_t)by * outbytes, outbytes, 0, 0, 0);
            dma_wait();
        }
    }
    spu_thread_exit(0);
    return 0;
}
