/* ps3gl: OpenGL 1.x fixed-function subset on top of the PS3's RSX (PSL1GHT librsx).

   The fixed-function pipeline is one vertex program (ffp.vcg: transform, one
   light, colour material, fog, texture matrix) and a handful of fragment
   programs, one per texture environment mode. Raster state (depth, blend,
   alpha test, cull, stencil, ...) maps 1:1 onto RSX state, whose enums are the
   GL ones. Geometry from glBegin/glEnd and from client arrays is written as
   interleaved 12-float vertices into a scratch buffer in RSX memory.

   Not implemented: selection, glBitmap/raster ops, read-back, clip planes,
   line stipple, more than one light, more than one texture unit,
   mipmaps (level 0 is used with linear filtering). */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <malloc.h>
#include <unistd.h>

#include <ppu-types.h>
#include <rsx/rsx.h>
#include <sysutil/video.h>
#include <sysutil/sysutil.h>

#include <GL/gl.h>
#include <GL/glx.h>
#include "ps3gl.h"
#include "shaders.h"

#define CB_SIZE         0x100000
#define HOST_SIZE       (4 * 1024 * 1024)
#define LABEL_INDEX     255
#define VB_BYTES        (24 * 1024 * 1024)
#define VTX_FLOATS      12
#define VTX_BYTES       (VTX_FLOATS * 4)
#define MAX_TEX         8192
#define MAX_FREEQ       4096
#define MV_DEPTH        32
#define PR_DEPTH        8
#define TX_DEPTH        8
#define ATTR_DEPTH      16

/* ---------------------------------------------------------------- RSX side */

static gcmContextData *ctx;
static gcmSurface surf[2];
static u32 cur_fb, first_flip = 1, label_val = 1;
static videoResolution vmode;
static int scr_w, scr_h;

static u8 *vb;          /* per-frame vertex scratch, RSX memory */
static u32 vb_off;

static rsxVertexProgram *vpo;
static void *vp_ucode;
enum { FP_NOTEX, FP_MODULATE, FP_REPLACE, FP_DECAL, FP_BLEND, FP_ADD, FP_COUNT };
static rsxFragmentProgram *fpo[FP_COUNT];
static u32 fp_ofs[FP_COUNT];
static int fp_loaded = -1;

static struct {
    rsxProgramConst *mvp[4], *mv[3], *nm[3], *tm[2];
    rsxProgramConst *lightPos, *lightAmb, *lightDif, *lightSpec;
    rsxProgramConst *matAmb, *matDif, *matSpec, *matEmi;
    rsxProgramConst *cmMask, *misc, *fogP, *fogW, *fogC, *envC;
} uc;

static void *free_q[MAX_FREEQ];
static int free_n;

static FILE *log_fp;

void ps3glLog(const char *fmt, ...)
{
    va_list ap;
    if (!log_fp) log_fp = fopen("/dev_hdd0/game/FGFS00910/USRDIR/ps3gl.log", "w");
    va_start(ap, fmt);
    if (log_fp) { vfprintf(log_fp, fmt, ap); fputc('\n', log_fp); fflush(log_fp); }
    va_end(ap);
    va_start(ap, fmt);
    vprintf(fmt, ap); putchar('\n');
    va_end(ap);
}

static void wait_finish(void)
{
    u32 timeout = 0;
    rsxSetWriteBackendLabel(ctx, LABEL_INDEX, label_val);
    rsxFlushBuffer(ctx);
    while (*(vu32 *)gcmGetLabelAddress(LABEL_INDEX) != label_val) {
        usleep(30);
        if (++timeout > 200000) { ps3glLog("ps3gl: RSX label timeout"); break; }
    }
    ++label_val;
}

/* A method this PSL1GHT version has no wrapper for. */
static void rsx_method1(u32 method, u32 value)
{
    while (ctx->current + 2 > ctx->end) rsxSetNopCommand(ctx, 2);   /* lets librsx wrap the buffer */
    ctx->current[0] = (1 << 18) | method;
    ctx->current[1] = value;
    ctx->current += 2;
}

#define NV40TCL_LINE_WIDTH  0x03b8      /* 6.3 fixed point */

static void send_line_width(GLfloat w)
{
    u32 v = (u32)(w * 8.0f + 0.5f);
    rsx_method1(NV40TCL_LINE_WIDTH, v < 8 ? 8 : (v > 0x1ff ? 0x1ff : v));
}

/* ---------------------------------------------------------------- GL state */

typedef struct { GLfloat amb[4], dif[4], spec[4], pos[4]; } Light;

typedef struct {
    /* enables */
    GLboolean tex2d, lighting, light[8], fog, depth_test, blend, alpha_test,
              cull, color_material, scissor_test, stencil_test, po_fill,
              po_line, normalize, texgen_s, texgen_t;
    /* colour buffer */
    GLenum blend_src, blend_dst, alpha_func;
    GLfloat alpha_ref, clear_color[4];
    GLboolean cmask[4];
    /* depth */
    GLenum depth_func;
    GLboolean depth_mask;
    GLfloat clear_depth;
    /* stencil */
    GLenum st_func, st_fail, st_zfail, st_zpass;
    GLint st_ref, clear_stencil;
    GLuint st_mask, st_wmask;
    /* current */
    GLfloat color[4], normal[3], texcoord[2];
    /* fog */
    GLenum fog_mode;
    GLfloat fog_density, fog_start, fog_end, fog_color[4];
    /* lighting */
    Light lt[8];
    GLfloat mat_amb[4], mat_dif[4], mat_spec[4], mat_emi[4], shininess, scene_amb[4];
    GLenum cm_mode, shade_model;
    /* polygon */
    GLenum cull_face, front_face, poly_front, poly_back;
    GLfloat po_factor, po_units;
    /* line, point */
    GLfloat line_width, point_size;
    /* texture */
    GLuint bound_tex;
    GLenum texenv_mode, texgen_mode;
    GLfloat texenv_color[4];
    /* transform */
    GLenum matrix_mode;
    /* viewport, scissor */
    GLint vp[4], sc[4];
} State;

static State S;
static State attr_stack[ATTR_DEPTH];
static GLbitfield attr_mask[ATTR_DEPTH];
static int attr_top;

static GLfloat mv_stack[MV_DEPTH][16], pr_stack[PR_DEPTH][16], tx_stack[TX_DEPTH][16];
static int mv_top, pr_top, tx_top;

#define D_MATRIX  1
#define D_LIGHT   2
#define D_FOG     4
#define D_TEX     8
#define D_TEXMAT  16
#define D_ALL     31
static unsigned dirty = D_ALL;

typedef struct {
    gcmTexture t;
    void *mem;
    u32 bytes;
    GLboolean used, has_data;
    GLenum wrap_s, wrap_t, min_f, mag_f;
    GLint ifmt;
    u16 w, h;           /* stored size of level 0 */
    u8 shift;           /* level 0 was scaled down by 2^shift (MAX_TEX_DIM) */
    u8 swz;             /* swizzled: power-of-two size, room for all mipmap levels */
    u8 levels;          /* mipmap levels uploaded so far (from level 0 on) */
    u8 fresh;           /* new storage the RSX has not used yet */
    u8 chain;           /* the storage has room for all mipmap levels */
} Tex;

/* Larger textures are scaled down: plenty for 720p, and the mipmap chains
   have to fit into the RSX's 256 MB. */
#define MAX_TEX_DIM 1024
/* Mipmap chains (a third more) only while textures take less than this:
   the rest of the RSX's 256 MB is for frame buffers, vertices and lists. */
#define TEX_CHAIN_BUDGET (150u * 1024 * 1024)
static Tex tex[MAX_TEX];

typedef struct { GLboolean on; GLint size; GLenum type; GLsizei stride; const GLubyte *ptr; } Array;
typedef struct { Array v, n, c, t; } Arrays;
static Arrays A, A_stack[ATTR_DEPTH];
static int A_top;

static GLint unpack_align = 4, unpack_row_len;

/* immediate mode */
static GLfloat *imm;
static int imm_n, imm_cap;
static GLenum imm_mode;

/* display lists: recorded geometry batches */
/* display lists: the geometry of a list is compiled into one vertex buffer in
   RSX memory, so calling it costs no copying and no main memory */
typedef struct { GLenum mode; int first, n; } Batch;
typedef struct {
    GLboolean used;
    int nb, cap_b;
    Batch *b;
    u8 *vram;           /* vertices in RSX memory once compiled */
    GLfloat *tmp;       /* vertices while compiling (or when RSX memory ran out) */
    int nv, cap_v;
} DList;
static DList *lists;
static GLuint lists_cap, lists_top;     /* slots allocated / highest slot handed out + 1 */
static GLuint *lists_free;              /* single slots returned by glDeleteLists */
static int lists_nfree, lists_cap_free;
static GLuint list_cur;                 /* list being compiled, 0 if none */

/* statistics for ps3glStats() */
static u32 st_tex_bytes, st_list_bytes, st_draws, st_verts, st_lists_alive, st_vram_fail, st_mem_fail;

static const GLfloat ident[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

/* ---------------------------------------------------------------- matrices */

static GLfloat *cur_mat(void)
{
    switch (S.matrix_mode) {
    case GL_PROJECTION: dirty |= D_MATRIX; return pr_stack[pr_top];
    case GL_TEXTURE:    dirty |= D_TEXMAT; return tx_stack[tx_top];
    default:            dirty |= D_MATRIX; return mv_stack[mv_top];
    }
}

/* r = a * b, column-major; r may alias a */
static void mat_mul(GLfloat *r, const GLfloat *a, const GLfloat *b)
{
    GLfloat t[16];
    int c, row;
    for (c = 0; c < 4; c++)
        for (row = 0; row < 4; row++)
            t[c * 4 + row] = a[0 * 4 + row] * b[c * 4 + 0] + a[1 * 4 + row] * b[c * 4 + 1]
                           + a[2 * 4 + row] * b[c * 4 + 2] + a[3 * 4 + row] * b[c * 4 + 3];
    memcpy(r, t, sizeof t);
}

static void mat_row(GLfloat *out, const GLfloat *m, int i)
{
    out[0] = m[i]; out[1] = m[4 + i]; out[2] = m[8 + i]; out[3] = m[12 + i];
}

void glMatrixMode(GLenum mode) { S.matrix_mode = mode; }
void glLoadIdentity(void) { memcpy(cur_mat(), ident, sizeof ident); }
void glLoadMatrixf(const GLfloat *m) { memcpy(cur_mat(), m, 16 * sizeof(GLfloat)); }
void glMultMatrixf(const GLfloat *m) { GLfloat *c = cur_mat(); mat_mul(c, c, m); }

void glLoadMatrixd(const GLdouble *m)
{
    GLfloat *c = cur_mat();
    int i;
    for (i = 0; i < 16; i++) c[i] = (GLfloat)m[i];
}

void glMultMatrixd(const GLdouble *m)
{
    GLfloat f[16];
    int i;
    for (i = 0; i < 16; i++) f[i] = (GLfloat)m[i];
    glMultMatrixf(f);
}

void glPushMatrix(void)
{
    switch (S.matrix_mode) {
    case GL_PROJECTION:
        if (pr_top < PR_DEPTH - 1) { memcpy(pr_stack[pr_top + 1], pr_stack[pr_top], 64); pr_top++; }
        break;
    case GL_TEXTURE:
        if (tx_top < TX_DEPTH - 1) { memcpy(tx_stack[tx_top + 1], tx_stack[tx_top], 64); tx_top++; }
        break;
    default:
        if (mv_top < MV_DEPTH - 1) { memcpy(mv_stack[mv_top + 1], mv_stack[mv_top], 64); mv_top++; }
        break;
    }
}

void glPopMatrix(void)
{
    switch (S.matrix_mode) {
    case GL_PROJECTION: if (pr_top > 0) pr_top--; dirty |= D_MATRIX; break;
    case GL_TEXTURE:    if (tx_top > 0) tx_top--; dirty |= D_TEXMAT; break;
    default:            if (mv_top > 0) mv_top--; dirty |= D_MATRIX; break;
    }
}

void glTranslatef(GLfloat x, GLfloat y, GLfloat z)
{
    GLfloat *m = cur_mat();
    m[12] += m[0] * x + m[4] * y + m[8] * z;
    m[13] += m[1] * x + m[5] * y + m[9] * z;
    m[14] += m[2] * x + m[6] * y + m[10] * z;
    m[15] += m[3] * x + m[7] * y + m[11] * z;
}
void glTranslated(GLdouble x, GLdouble y, GLdouble z) { glTranslatef((GLfloat)x, (GLfloat)y, (GLfloat)z); }

void glScalef(GLfloat x, GLfloat y, GLfloat z)
{
    GLfloat *m = cur_mat();
    int i;
    for (i = 0; i < 4; i++) { m[i] *= x; m[4 + i] *= y; m[8 + i] *= z; }
}
void glScaled(GLdouble x, GLdouble y, GLdouble z) { glScalef((GLfloat)x, (GLfloat)y, (GLfloat)z); }

void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
{
    GLfloat r[16], len = sqrtf(x * x + y * y + z * z);
    GLfloat a = angle * (GLfloat)(M_PI / 180.0), c = cosf(a), s = sinf(a), t = 1.0f - c;
    if (len == 0.0f) return;
    x /= len; y /= len; z /= len;
    r[0] = t * x * x + c;     r[1] = t * x * y + s * z; r[2] = t * x * z - s * y; r[3] = 0;
    r[4] = t * x * y - s * z; r[5] = t * y * y + c;     r[6] = t * y * z + s * x; r[7] = 0;
    r[8] = t * x * z + s * y; r[9] = t * y * z - s * x; r[10] = t * z * z + c;    r[11] = 0;
    r[12] = 0; r[13] = 0; r[14] = 0; r[15] = 1;
    glMultMatrixf(r);
}
void glRotated(GLdouble a, GLdouble x, GLdouble y, GLdouble z) { glRotatef((GLfloat)a, (GLfloat)x, (GLfloat)y, (GLfloat)z); }

void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
    GLfloat m[16];
    memcpy(m, ident, sizeof m);
    m[0] = (GLfloat)(2.0 / (r - l));
    m[5] = (GLfloat)(2.0 / (t - b));
    m[10] = (GLfloat)(-2.0 / (f - n));
    m[12] = (GLfloat)(-(r + l) / (r - l));
    m[13] = (GLfloat)(-(t + b) / (t - b));
    m[14] = (GLfloat)(-(f + n) / (f - n));
    glMultMatrixf(m);
}

void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
    GLfloat m[16];
    memset(m, 0, sizeof m);
    m[0] = (GLfloat)(2.0 * n / (r - l));
    m[5] = (GLfloat)(2.0 * n / (t - b));
    m[8] = (GLfloat)((r + l) / (r - l));
    m[9] = (GLfloat)((t + b) / (t - b));
    m[10] = (GLfloat)(-(f + n) / (f - n));
    m[11] = -1.0f;
    m[14] = (GLfloat)(-2.0 * f * n / (f - n));
    glMultMatrixf(m);
}

/* ---------------------------------------------------------------- raster state -> RSX */

static void send_viewport(void)
{
    f32 scale[4], offset[4];
    int x = S.vp[0], w = S.vp[2], h = S.vp[3], y = scr_h - S.vp[1] - h;
    scale[0] = w * 0.5f;  scale[1] = h * -0.5f; scale[2] = 0.5f; scale[3] = 0.0f;
    offset[0] = x + w * 0.5f; offset[1] = y + h * 0.5f; offset[2] = 0.5f; offset[3] = 0.0f;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > scr_w) w = scr_w - x;
    if (y + h > scr_h) h = scr_h - y;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    rsxSetViewport(ctx, x, y, w, h, 0.0f, 1.0f, scale, offset);
}

static void send_scissor(void)
{
    int x = 0, y = 0, w = scr_w, h = scr_h;
    if (S.scissor_test) {
        x = S.sc[0]; w = S.sc[2]; h = S.sc[3]; y = scr_h - S.sc[1] - h;
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > scr_w) w = scr_w - x;
        if (y + h > scr_h) h = scr_h - y;
        if (w < 0) w = 0;
        if (h < 0) h = 0;
    }
    rsxSetScissor(ctx, x, y, w, h);
}

static void send_cmask(void)
{
    rsxSetColorMask(ctx, (S.cmask[0] ? GCM_COLOR_MASK_R : 0) | (S.cmask[1] ? GCM_COLOR_MASK_G : 0)
                       | (S.cmask[2] ? GCM_COLOR_MASK_B : 0) | (S.cmask[3] ? GCM_COLOR_MASK_A : 0));
}

static u32 argb(const GLfloat *c)
{
    u32 v = 0;
    static const int sh[4] = { 16, 8, 0, 24 };
    int i;
    for (i = 0; i < 4; i++) {
        GLfloat f = c[i] < 0 ? 0 : (c[i] > 1 ? 1 : c[i]);
        v |= (u32)(f * 255.0f + 0.5f) << sh[i];
    }
    return v;
}

static void send_clear_values(void)
{
    GLfloat d = S.clear_depth < 0 ? 0 : (S.clear_depth > 1 ? 1 : S.clear_depth);
    rsxSetClearColor(ctx, argb(S.clear_color));
    rsxSetClearDepthStencil(ctx, ((u32)(d * 16777215.0f) << 8) | (S.clear_stencil & 0xff));
}

/* Everything the RSX holds as raster state, from S. */
static void send_raster(void)
{
    rsxSetDepthTestEnable(ctx, S.depth_test);
    rsxSetDepthFunc(ctx, S.depth_func);
    rsxSetDepthWriteEnable(ctx, S.depth_mask);
    rsxSetBlendEnable(ctx, S.blend);
    rsxSetBlendFunc(ctx, S.blend_src, S.blend_dst, S.blend_src, S.blend_dst);
    rsxSetBlendEquation(ctx, GCM_FUNC_ADD, GCM_FUNC_ADD);
    rsxSetAlphaTestEnable(ctx, S.alpha_test);
    rsxSetAlphaFunc(ctx, S.alpha_func, (u32)(S.alpha_ref * 255.0f + 0.5f));
    rsxSetCullFaceEnable(ctx, S.cull);
    rsxSetCullFace(ctx, S.cull_face);
    rsxSetFrontFace(ctx, S.front_face);
    rsxSetShadeModel(ctx, S.shade_model);
    rsxSetFrontPolygonMode(ctx, S.poly_front);
    rsxSetBackPolygonMode(ctx, S.poly_back);
    rsxSetPolygonOffsetFillEnable(ctx, S.po_fill);
    rsxSetPolygonOffsetLineEnable(ctx, S.po_line);
    rsxSetPolygonOffset(ctx, S.po_factor, S.po_units);
    rsxSetPointSize(ctx, S.point_size);
    send_line_width(S.line_width);
    rsxSetStencilTestEnable(ctx, S.stencil_test);
    rsxSetStencilFunc(ctx, S.st_func, S.st_ref, S.st_mask);
    rsxSetStencilOp(ctx, S.st_fail, S.st_zfail, S.st_zpass);
    rsxSetStencilMask(ctx, S.st_wmask);
    send_cmask();
    send_clear_values();
    send_viewport();
    send_scissor();
}

/* ---------------------------------------------------------------- vertex program constants */

static void set_c(rsxProgramConst *c, const GLfloat *v)
{
    if (c) rsxSetVertexProgramParameter(ctx, vpo, c, v);
}

static void flush_matrix(void)
{
    GLfloat mvp[16], r[4], inv[9], det;
    const GLfloat *mv = mv_stack[mv_top];
    int i;

    mat_mul(mvp, pr_stack[pr_top], mv);
    for (i = 0; i < 4; i++) { mat_row(r, mvp, i); set_c(uc.mvp[i], r); }
    for (i = 0; i < 3; i++) { mat_row(r, mv, i); set_c(uc.mv[i], r); }

    /* normal matrix = inverse transpose of the upper 3x3; rows of it are the
       cofactor rows of mv divided by the determinant */
    inv[0] = mv[5] * mv[10] - mv[6] * mv[9];
    inv[1] = mv[6] * mv[8] - mv[4] * mv[10];
    inv[2] = mv[4] * mv[9] - mv[5] * mv[8];
    inv[3] = mv[2] * mv[9] - mv[1] * mv[10];
    inv[4] = mv[0] * mv[10] - mv[2] * mv[8];
    inv[5] = mv[1] * mv[8] - mv[0] * mv[9];
    inv[6] = mv[1] * mv[6] - mv[2] * mv[5];
    inv[7] = mv[2] * mv[4] - mv[0] * mv[6];
    inv[8] = mv[0] * mv[5] - mv[1] * mv[4];
    det = mv[0] * inv[0] + mv[1] * inv[1] + mv[2] * inv[2];
    if (det == 0.0f) det = 1.0f;
    /* inv[] holds cofactors C[col][row] of the column-major matrix; the normal
       matrix row i is (C[0][i], C[1][i], C[2][i]) / det */
    for (i = 0; i < 3; i++) {
        r[0] = inv[i] / det; r[1] = inv[3 + i] / det; r[2] = inv[6 + i] / det; r[3] = 0;
        set_c(uc.nm[i], r);
    }
}

static void flush_light(void)
{
    static const GLfloat zero[4] = { 0, 0, 0, 0 };
    GLfloat v[4];
    const Light *l = &S.lt[0];
    int on = S.lighting && S.light[0], i;

    for (i = 0; i < 4; i++) v[i] = S.scene_amb[i] + (on ? l->amb[i] : 0.0f);
    set_c(uc.lightPos, l->pos);
    set_c(uc.lightAmb, v);
    set_c(uc.lightDif, on ? l->dif : zero);
    set_c(uc.lightSpec, on ? l->spec : zero);
    set_c(uc.matAmb, S.mat_amb);
    set_c(uc.matDif, S.mat_dif);
    set_c(uc.matSpec, S.mat_spec);
    set_c(uc.matEmi, S.mat_emi);

    v[0] = v[1] = v[2] = v[3] = 0;
    if (S.color_material) {
        switch (S.cm_mode) {
        case GL_AMBIENT:             v[0] = 1; break;
        case GL_DIFFUSE:             v[1] = 1; break;
        case GL_EMISSION:            v[2] = 1; break;
        case GL_SPECULAR:            v[3] = 1; break;
        default:                     v[0] = v[1] = 1; break;    /* AMBIENT_AND_DIFFUSE */
        }
    }
    set_c(uc.cmMask, v);

    v[0] = S.lighting ? 1.0f : 0.0f;
    v[1] = S.shininess;
    v[2] = (S.texgen_s && S.texgen_t && S.texgen_mode == GL_SPHERE_MAP) ? 1.0f : 0.0f;
    v[3] = 0;
    set_c(uc.misc, v);
}

static void flush_fog(void)
{
    GLfloat p[4] = { 0, 0, 0, 0 }, w[4] = { 0, 0, 0, 1 };
    if (S.fog) {
        GLfloat d = S.fog_end - S.fog_start;
        p[0] = S.fog_density;
        p[1] = S.fog_end;
        p[2] = d != 0.0f ? 1.0f / d : 0.0f;
        w[3] = 0;
        if (S.fog_mode == GL_LINEAR) w[0] = 1;
        else if (S.fog_mode == GL_EXP2) w[2] = 1;
        else w[1] = 1;
    }
    set_c(uc.fogP, p);
    set_c(uc.fogW, w);
    set_c(uc.fogC, S.fog_color);
}

static u8 gcm_wrap(GLenum w)
{
    switch (w) {
    case GL_REPEAT:          return GCM_TEXTURE_REPEAT;
    case GL_MIRRORED_REPEAT: return GCM_TEXTURE_MIRRORED_REPEAT;
    default:                 return GCM_TEXTURE_CLAMP_TO_EDGE;
    }
}

static void flush_tex(void)
{
    Tex *t = &tex[S.bound_tex];
    int fp = FP_NOTEX;

    if (S.tex2d && S.bound_tex && t->has_data) {
        switch (S.texenv_mode) {
        case GL_REPLACE: fp = FP_REPLACE; break;
        case GL_DECAL:   fp = FP_DECAL; break;
        case GL_BLEND:   fp = FP_BLEND; break;
        case GL_ADD:     fp = FP_ADD; break;
        default:         fp = FP_MODULATE; break;
        }
    }
    if (fp != fp_loaded) {
        rsxLoadFragmentProgramLocation(ctx, fpo[fp], fp_ofs[fp], GCM_LOCATION_RSX);
        fp_loaded = fp;
    }
    if (fp != FP_NOTEX) {
        int mips = t->levels > 1 && t->min_f != GL_NEAREST && t->min_f != GL_LINEAR;
        u8 min = t->min_f == GL_NEAREST ? GCM_TEXTURE_NEAREST : GCM_TEXTURE_LINEAR;
        if (mips) {
            switch (t->min_f) {
            case GL_NEAREST_MIPMAP_NEAREST: min = GCM_TEXTURE_NEAREST_MIPMAP_NEAREST; break;
            case GL_LINEAR_MIPMAP_NEAREST:  min = GCM_TEXTURE_LINEAR_MIPMAP_NEAREST; break;
            case GL_NEAREST_MIPMAP_LINEAR:  min = GCM_TEXTURE_NEAREST_MIPMAP_LINEAR; break;
            default:                        min = GCM_TEXTURE_LINEAR_MIPMAP_LINEAR; break;
            }
        }
        t->fresh = 0;
        rsxLoadTexture(ctx, 0, &t->t);
        /* LODs are 4.8 fixed point; anisotropic filtering keeps runway
           markings and terrain sharp at shallow angles */
        rsxTextureControl(ctx, 0, GCM_TRUE, 0, mips ? (u16)((t->levels - 1) << 8) : 0,
                          mips ? GCM_TEXTURE_MAX_ANISO_8 : GCM_TEXTURE_MAX_ANISO_1);
        rsxTextureFilter(ctx, 0, 0, min,
                         t->mag_f == GL_NEAREST ? GCM_TEXTURE_NEAREST : GCM_TEXTURE_LINEAR,
                         GCM_TEXTURE_CONVOLUTION_QUINCUNX);
        rsxTextureWrapMode(ctx, 0, gcm_wrap(t->wrap_s), gcm_wrap(t->wrap_t),
                           GCM_TEXTURE_CLAMP_TO_EDGE, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
    } else {
        rsxTextureControl(ctx, 0, GCM_FALSE, 0, 0, GCM_TEXTURE_MAX_ANISO_1);
    }
    set_c(uc.envC, S.texenv_color);
}

static void flush_state(void)
{
    if (dirty & D_MATRIX) flush_matrix();
    if (dirty & D_LIGHT) flush_light();
    if (dirty & D_FOG) flush_fog();
    if (dirty & D_TEX) flush_tex();
    if (dirty & D_TEXMAT) {
        GLfloat r[4];
        mat_row(r, tx_stack[tx_top], 0); set_c(uc.tm[0], r);
        mat_row(r, tx_stack[tx_top], 1); set_c(uc.tm[1], r);
    }
    dirty = 0;
}

/* ---------------------------------------------------------------- geometry submission */

/* Room for n more vertices in the list being compiled, as a batch of mode. */
static GLfloat *list_reserve(GLenum mode, int n)
{
    DList *l = &lists[list_cur];
    Batch *b = l->nb ? &l->b[l->nb - 1] : NULL;
    if (l->nv + n > l->cap_v) {
        int cap = l->cap_v ? l->cap_v * 2 : 256;
        GLfloat *tmp;
        while (cap < l->nv + n) cap *= 2;
        tmp = (GLfloat *)realloc(l->tmp, (size_t)cap * VTX_BYTES);
        if (!tmp) {             /* main memory full: these vertices are lost */
            if (!st_mem_fail++) {
                struct mallinfo mi = mallinfo();
                ps3glLog("ps3gl: out of main memory for a display list of %d vertices (heap %luK, %luK from the system)",
                         cap, (unsigned long)mi.uordblks >> 10, (unsigned long)mi.arena >> 10);
            }
            return NULL;
        }
        l->tmp = tmp;
        l->cap_v = cap;
    }
    /* independent primitives continue the previous batch */
    if (b && b->mode == mode && b->first + b->n == l->nv &&
        (mode == GL_TRIANGLES || mode == GL_QUADS || mode == GL_LINES || mode == GL_POINTS)) {
        b->n += n;
    } else {
        if (l->nb == l->cap_b) {
            Batch *nb = (Batch *)realloc(l->b, (l->cap_b ? l->cap_b * 2 : 4) * sizeof(Batch));
            if (!nb) {
                if (!st_mem_fail++) ps3glLog("ps3gl: out of main memory for a display list");
                return NULL;
            }
            l->b = nb;
            l->cap_b = l->cap_b ? l->cap_b * 2 : 4;
        }
        b = &l->b[l->nb++];
        b->mode = mode;
        b->first = l->nv;
        b->n = n;
    }
    l->nv += n;
    return l->tmp + (size_t)(l->nv - n) * VTX_FLOATS;
}

static void list_add(GLenum mode, const GLfloat *v, int n)
{
    GLfloat *dst = list_reserve(mode, n);
    if (dst) memcpy(dst, v, (size_t)n * VTX_BYTES);
}

/* Room for n vertices in the scratch buffer. Draining the RSX to reuse the
   buffer mid-frame is slow but only happens when a frame exceeds VB_BYTES. */
static GLfloat *vb_reserve(int n)
{
    if (vb_off + (u32)n * VTX_BYTES > VB_BYTES) {
        wait_finish();
        vb_off = 0;
    }
    return (GLfloat *)(vb + vb_off);
}

/* Draw n vertices that already sit in RSX memory at addr. */
static void draw_rsx(GLenum mode, const u8 *addr, int n)
{
    u32 o;
    flush_state();
    rsxAddressToOffset((void *)addr, &o);
    rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_POS,    0, o,      VTX_BYTES, 3, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_NORMAL, 0, o + 12, VTX_BYTES, 3, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_COLOR0, 0, o + 24, VTX_BYTES, 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX0,   0, o + 40, VTX_BYTES, 2, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxDrawVertexArray(ctx, mode + 1, 0, n);    /* GCM_TYPE_x == GL_x + 1 */
    st_draws++;
    st_verts += n;
}

static void vb_submit(GLenum mode, int n)
{
    draw_rsx(mode, vb + vb_off, n);
    vb_off += (u32)n * VTX_BYTES;
}

static void draw(GLenum mode, const GLfloat *v, int n)
{
    if (n <= 0 || n * VTX_BYTES > VB_BYTES) return;
    if (list_cur) { list_add(mode, v, n); return; }
    memcpy(vb_reserve(n), v, n * VTX_BYTES);
    vb_submit(mode, n);
}

/* ---------------------------------------------------------------- immediate mode */

void glBegin(GLenum mode) { imm_mode = mode; imm_n = 0; }

void glEnd(void)
{
    draw(imm_mode, imm, imm_n);
    imm_n = 0;
}

static inline void imm_vertex(GLfloat x, GLfloat y, GLfloat z)
{
    GLfloat *v;
    if (imm_n == imm_cap) {
        imm_cap = imm_cap ? imm_cap * 2 : 4096;
        imm = (GLfloat *)realloc(imm, imm_cap * VTX_BYTES);
    }
    v = imm + imm_n * VTX_FLOATS;
    v[0] = x; v[1] = y; v[2] = z;
    v[3] = S.normal[0]; v[4] = S.normal[1]; v[5] = S.normal[2];
    v[6] = S.color[0]; v[7] = S.color[1]; v[8] = S.color[2]; v[9] = S.color[3];
    v[10] = S.texcoord[0]; v[11] = S.texcoord[1];
    imm_n++;
}

void glVertex2f(GLfloat x, GLfloat y) { imm_vertex(x, y, 0); }
void glVertex2i(GLint x, GLint y) { imm_vertex((GLfloat)x, (GLfloat)y, 0); }
void glVertex2d(GLdouble x, GLdouble y) { imm_vertex((GLfloat)x, (GLfloat)y, 0); }
void glVertex2fv(const GLfloat *v) { imm_vertex(v[0], v[1], 0); }
void glVertex3f(GLfloat x, GLfloat y, GLfloat z) { imm_vertex(x, y, z); }
void glVertex3d(GLdouble x, GLdouble y, GLdouble z) { imm_vertex((GLfloat)x, (GLfloat)y, (GLfloat)z); }
void glVertex3fv(const GLfloat *v) { imm_vertex(v[0], v[1], v[2]); }
void glVertex3dv(const GLdouble *v) { imm_vertex((GLfloat)v[0], (GLfloat)v[1], (GLfloat)v[2]); }

void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { S.color[0] = r; S.color[1] = g; S.color[2] = b; S.color[3] = a; }
void glColor3f(GLfloat r, GLfloat g, GLfloat b) { glColor4f(r, g, b, 1.0f); }
void glColor4fv(const GLfloat *c) { glColor4f(c[0], c[1], c[2], c[3]); }
void glColor3fv(const GLfloat *c) { glColor4f(c[0], c[1], c[2], 1.0f); }
void glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a) { glColor4f(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f); }
void glColor3ub(GLubyte r, GLubyte g, GLubyte b) { glColor4f(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f); }
void glColor4ubv(const GLubyte *c) { glColor4ub(c[0], c[1], c[2], c[3]); }
void glColor3d(GLdouble r, GLdouble g, GLdouble b) { glColor4f((GLfloat)r, (GLfloat)g, (GLfloat)b, 1.0f); }
void glColor4d(GLdouble r, GLdouble g, GLdouble b, GLdouble a) { glColor4f((GLfloat)r, (GLfloat)g, (GLfloat)b, (GLfloat)a); }
void glNormal3f(GLfloat x, GLfloat y, GLfloat z) { S.normal[0] = x; S.normal[1] = y; S.normal[2] = z; }
void glNormal3fv(const GLfloat *n) { glNormal3f(n[0], n[1], n[2]); }
void glTexCoord2f(GLfloat s, GLfloat t) { S.texcoord[0] = s; S.texcoord[1] = t; }
void glTexCoord2fv(const GLfloat *t) { glTexCoord2f(t[0], t[1]); }
void glTexCoord2d(GLdouble s, GLdouble t) { glTexCoord2f((GLfloat)s, (GLfloat)t); }

void glRectf(GLfloat x1, GLfloat y1, GLfloat x2, GLfloat y2)
{
    glBegin(GL_QUADS);
    glVertex2f(x1, y1); glVertex2f(x2, y1); glVertex2f(x2, y2); glVertex2f(x1, y2);
    glEnd();
}
void glRecti(GLint x1, GLint y1, GLint x2, GLint y2) { glRectf((GLfloat)x1, (GLfloat)y1, (GLfloat)x2, (GLfloat)y2); }

/* ---------------------------------------------------------------- client arrays */

static Array *client_array(GLenum cap)
{
    switch (cap) {
    case GL_VERTEX_ARRAY:        return &A.v;
    case GL_NORMAL_ARRAY:        return &A.n;
    case GL_COLOR_ARRAY:         return &A.c;
    case GL_TEXTURE_COORD_ARRAY: return &A.t;
    default:                     return NULL;
    }
}

void glEnableClientState(GLenum cap) { Array *a = client_array(cap); if (a) a->on = GL_TRUE; }
void glDisableClientState(GLenum cap) { Array *a = client_array(cap); if (a) a->on = GL_FALSE; }

static void set_array(Array *a, GLint size, GLenum type, GLsizei stride, const GLvoid *ptr)
{
    a->size = size; a->type = type; a->stride = stride; a->ptr = (const GLubyte *)ptr;
}
void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p) { set_array(&A.v, size, type, stride, p); }
void glNormalPointer(GLenum type, GLsizei stride, const GLvoid *p) { set_array(&A.n, 3, type, stride, p); }
void glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p) { set_array(&A.c, size, type, stride, p); }
void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p) { set_array(&A.t, size, type, stride, p); }

void glPushClientAttrib(GLbitfield mask) { (void)mask; if (A_top < ATTR_DEPTH) A_stack[A_top++] = A; }
void glPopClientAttrib(void) { if (A_top > 0) A = A_stack[--A_top]; }

/* element i of a float or double array, n components into out */
static inline void fetch_f(const Array *a, int i, int n, GLfloat *out)
{
    int k;
    if (a->type == GL_DOUBLE) {
        const GLdouble *p = (const GLdouble *)(a->ptr + (size_t)i * (a->stride ? a->stride : a->size * 8));
        for (k = 0; k < n; k++) out[k] = (GLfloat)p[k];
    } else {
        const GLfloat *p = (const GLfloat *)(a->ptr + (size_t)i * (a->stride ? a->stride : a->size * 4));
        for (k = 0; k < n; k++) out[k] = p[k];
    }
}

static inline void fetch_vertex(int i, GLfloat *v)
{
    v[2] = 0;
    fetch_f(&A.v, i, A.v.size < 3 ? A.v.size : 3, v);
    if (A.n.on) fetch_f(&A.n, i, 3, v + 3);
    else { v[3] = S.normal[0]; v[4] = S.normal[1]; v[5] = S.normal[2]; }
    if (A.c.on) {
        if (A.c.type == GL_UNSIGNED_BYTE) {
            const GLubyte *p = A.c.ptr + (size_t)i * (A.c.stride ? A.c.stride : A.c.size);
            v[6] = p[0] / 255.0f; v[7] = p[1] / 255.0f; v[8] = p[2] / 255.0f;
            v[9] = A.c.size > 3 ? p[3] / 255.0f : 1.0f;
        } else {
            v[9] = 1.0f;
            fetch_f(&A.c, i, A.c.size, v + 6);
        }
    } else { v[6] = S.color[0]; v[7] = S.color[1]; v[8] = S.color[2]; v[9] = S.color[3]; }
    if (A.t.on) fetch_f(&A.t, i, 2, v + 10);
    else { v[10] = S.texcoord[0]; v[11] = S.texcoord[1]; }
}

void glArrayElement(GLint i)
{
    GLfloat v[VTX_FLOATS];
    GLfloat sc[4], sn[3], st[2];
    if (!A.v.on) return;
    fetch_vertex(i, v);
    memcpy(sn, S.normal, sizeof sn); memcpy(sc, S.color, sizeof sc); memcpy(st, S.texcoord, sizeof st);
    memcpy(S.normal, v + 3, sizeof sn); memcpy(S.color, v + 6, sizeof sc); memcpy(S.texcoord, v + 10, sizeof st);
    imm_vertex(v[0], v[1], v[2]);
    if (!A.n.on) memcpy(S.normal, sn, sizeof sn);
    if (!A.c.on) memcpy(S.color, sc, sizeof sc);
    if (!A.t.on) memcpy(S.texcoord, st, sizeof st);
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    GLfloat *dst;
    int i;
    if (!A.v.on || count <= 0 || count * VTX_BYTES > VB_BYTES) return;
    dst = list_cur ? list_reserve(mode, count) : vb_reserve(count);
    if (!dst) return;
    for (i = 0; i < count; i++) fetch_vertex(first + i, dst + i * VTX_FLOATS);
    if (!list_cur) vb_submit(mode, count);
}

void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices)
{
    GLfloat *dst;
    int i;
    if (!A.v.on || count <= 0 || count * VTX_BYTES > VB_BYTES) return;
    dst = list_cur ? list_reserve(mode, count) : vb_reserve(count);
    if (!dst) return;
    for (i = 0; i < count; i++) {
        unsigned idx = type == GL_UNSIGNED_SHORT ? ((const GLushort *)indices)[i]
                     : type == GL_UNSIGNED_INT   ? ((const GLuint *)indices)[i]
                     :                             ((const GLubyte *)indices)[i];
        fetch_vertex((int)idx, dst + i * VTX_FLOATS);
    }
    if (!list_cur) vb_submit(mode, count);
}

/* ---------------------------------------------------------------- display lists */

static void defer_free(void *p);

GLuint glGenLists(GLsizei range)
{
    GLuint first, i;
    if (range <= 0) return 0;
    if (range == 1 && lists_nfree) {
        first = lists_free[--lists_nfree];
    } else {
        if (lists_top == 0) lists_top = 1;      /* list 0 does not exist */
        if (lists_top + range > lists_cap) {
            GLuint cap = lists_cap ? lists_cap : 1024;
            while (lists_top + range > cap) cap *= 2;
            lists = (DList *)realloc(lists, cap * sizeof(DList));
            if (!lists) { ps3glLog("ps3gl: out of memory for display lists"); exit(1); }
            memset(lists + lists_cap, 0, (cap - lists_cap) * sizeof(DList));
            lists_cap = cap;
        }
        first = lists_top;
        lists_top += range;
    }
    for (i = first; i < first + range; i++) lists[i].used = GL_TRUE;
    return first;
}

static void list_clear(DList *l)
{
    if (l->vram) {
        defer_free(l->vram);
        st_list_bytes -= (u32)l->nv * VTX_BYTES;
        st_lists_alive--;
    }
    free(l->tmp);
    free(l->b);
    l->vram = NULL;
    l->tmp = NULL;
    l->b = NULL;
    l->nb = l->cap_b = l->nv = l->cap_v = 0;
}

static int list_valid(GLuint list) { return list > 0 && list < lists_top && lists[list].used; }

void glNewList(GLuint list, GLenum mode)
{
    (void)mode;
    if (!list_valid(list)) return;
    list_clear(&lists[list]);
    list_cur = list;
}

void glEndList(void)
{
    DList *l;
    if (!list_cur) return;
    l = &lists[list_cur];
    list_cur = 0;
    if (!l->nv) return;
    l->vram = (u8 *)rsxMemalign(128, (u32)l->nv * VTX_BYTES);
    if (!l->vram) {             /* keep it in main memory, drawn by copying */
        GLfloat *fit = (GLfloat *)realloc(l->tmp, (size_t)l->nv * VTX_BYTES);
        if (fit) {              /* the buffer was made for at least 256 vertices */
            l->tmp = fit;
            l->cap_v = l->nv;
        }
        if (!st_vram_fail++) {
            struct mallinfo mi = mallinfo();
            ps3glLog("ps3gl: RSX memory full, display lists stay in main memory (heap %luK)",
                     (unsigned long)mi.uordblks >> 10);
        }
        return;
    }
    memcpy(l->vram, l->tmp, (size_t)l->nv * VTX_BYTES);
    free(l->tmp);
    l->tmp = NULL;
    l->cap_v = 0;
    st_list_bytes += (u32)l->nv * VTX_BYTES;
    st_lists_alive++;
}

void glCallList(GLuint list)
{
    DList *l;
    int i;
    if (!list_valid(list) || list == list_cur) return;
    l = &lists[list];
    for (i = 0; i < l->nb; i++) {
        const Batch *b = &l->b[i];
        if (list_cur)           /* nested: copy into the list being compiled */
            list_add(b->mode, l->vram ? (const GLfloat *)(l->vram + (size_t)b->first * VTX_BYTES)
                                      : l->tmp + (size_t)b->first * VTX_FLOATS, b->n);
        else if (l->vram)
            draw_rsx(b->mode, l->vram + (size_t)b->first * VTX_BYTES, b->n);
        else
            draw(b->mode, l->tmp + (size_t)b->first * VTX_FLOATS, b->n);
    }
}

void glDeleteLists(GLuint list, GLsizei range)
{
    GLuint i;
    for (i = list; i < list + (GLuint)range; i++) {
        if (!list_valid(i)) continue;
        list_clear(&lists[i]);
        lists[i].used = GL_FALSE;
        if (lists_nfree == lists_cap_free) {
            lists_cap_free = lists_cap_free ? lists_cap_free * 2 : 1024;
            lists_free = (GLuint *)realloc(lists_free, lists_cap_free * sizeof(GLuint));
        }
        lists_free[lists_nfree++] = i;
    }
}

GLboolean glIsList(GLuint list) { return list_valid(list); }

/* ---------------------------------------------------------------- enable / disable */

static void set_cap(GLenum cap, GLboolean on)
{
    switch (cap) {
    case GL_TEXTURE_2D:     S.tex2d = on; dirty |= D_TEX; break;
    case GL_LIGHTING:       S.lighting = on; dirty |= D_LIGHT; break;
    case GL_FOG:            S.fog = on; dirty |= D_FOG; break;
    case GL_COLOR_MATERIAL: S.color_material = on; dirty |= D_LIGHT; break;
    case GL_NORMALIZE:      S.normalize = on; break;
    case GL_TEXTURE_GEN_S:  S.texgen_s = on; dirty |= D_LIGHT; break;
    case GL_TEXTURE_GEN_T:  S.texgen_t = on; dirty |= D_LIGHT; break;
    case GL_DEPTH_TEST:     S.depth_test = on; rsxSetDepthTestEnable(ctx, on); break;
    case GL_BLEND:          S.blend = on; rsxSetBlendEnable(ctx, on); break;
    case GL_ALPHA_TEST:     S.alpha_test = on; rsxSetAlphaTestEnable(ctx, on); break;
    case GL_CULL_FACE:      S.cull = on; rsxSetCullFaceEnable(ctx, on); break;
    case GL_SCISSOR_TEST:   S.scissor_test = on; send_scissor(); break;
    case GL_STENCIL_TEST:   S.stencil_test = on; rsxSetStencilTestEnable(ctx, on); break;
    case GL_POLYGON_OFFSET_FILL: S.po_fill = on; rsxSetPolygonOffsetFillEnable(ctx, on); break;
    case GL_POLYGON_OFFSET_LINE: S.po_line = on; rsxSetPolygonOffsetLineEnable(ctx, on); break;
    default:
        if (cap >= GL_LIGHT0 && cap <= GL_LIGHT7) { S.light[cap - GL_LIGHT0] = on; dirty |= D_LIGHT; }
        break;
    }
}

void glEnable(GLenum cap) { set_cap(cap, GL_TRUE); }
void glDisable(GLenum cap) { set_cap(cap, GL_FALSE); }

GLboolean glIsEnabled(GLenum cap)
{
    switch (cap) {
    case GL_TEXTURE_2D:     return S.tex2d;
    case GL_LIGHTING:       return S.lighting;
    case GL_FOG:            return S.fog;
    case GL_COLOR_MATERIAL: return S.color_material;
    case GL_DEPTH_TEST:     return S.depth_test;
    case GL_BLEND:          return S.blend;
    case GL_ALPHA_TEST:     return S.alpha_test;
    case GL_CULL_FACE:      return S.cull;
    case GL_SCISSOR_TEST:   return S.scissor_test;
    case GL_STENCIL_TEST:   return S.stencil_test;
    case GL_NORMALIZE:      return S.normalize;
    default:
        if (cap >= GL_LIGHT0 && cap <= GL_LIGHT7) return S.light[cap - GL_LIGHT0];
        return GL_FALSE;
    }
}

/* ---------------------------------------------------------------- per-fragment state */

void glBlendFunc(GLenum s, GLenum d) { S.blend_src = s; S.blend_dst = d; rsxSetBlendFunc(ctx, s, d, s, d); }
void glAlphaFunc(GLenum f, GLclampf ref) { S.alpha_func = f; S.alpha_ref = ref; rsxSetAlphaFunc(ctx, f, (u32)(ref * 255.0f + 0.5f)); }
void glDepthFunc(GLenum f) { S.depth_func = f; rsxSetDepthFunc(ctx, f); }
void glDepthMask(GLboolean m) { S.depth_mask = m; rsxSetDepthWriteEnable(ctx, m); }
void glDepthRange(GLclampd n, GLclampd f) { (void)n; (void)f; }
void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { S.cmask[0] = r; S.cmask[1] = g; S.cmask[2] = b; S.cmask[3] = a; send_cmask(); }
void glStencilFunc(GLenum f, GLint ref, GLuint mask) { S.st_func = f; S.st_ref = ref; S.st_mask = mask; rsxSetStencilFunc(ctx, f, ref, mask); }
void glStencilOp(GLenum fail, GLenum zfail, GLenum zpass) { S.st_fail = fail; S.st_zfail = zfail; S.st_zpass = zpass; rsxSetStencilOp(ctx, fail, zfail, zpass); }
void glStencilMask(GLuint m) { S.st_wmask = m; rsxSetStencilMask(ctx, m); }
void glCullFace(GLenum f) { S.cull_face = f; rsxSetCullFace(ctx, f); }
void glFrontFace(GLenum f) { S.front_face = f; rsxSetFrontFace(ctx, f); }
void glShadeModel(GLenum m) { S.shade_model = m; rsxSetShadeModel(ctx, m); }
void glPolygonOffset(GLfloat factor, GLfloat units) { S.po_factor = factor; S.po_units = units; rsxSetPolygonOffset(ctx, factor, units); }
void glPointSize(GLfloat s) { S.point_size = s; rsxSetPointSize(ctx, s); }
void glLineWidth(GLfloat w) { S.line_width = w; send_line_width(w); }
void glLineStipple(GLint factor, GLushort pattern) { (void)factor; (void)pattern; }

void glPolygonMode(GLenum face, GLenum mode)
{
    if (face != GL_BACK) { S.poly_front = mode; rsxSetFrontPolygonMode(ctx, mode); }
    if (face != GL_FRONT) { S.poly_back = mode; rsxSetBackPolygonMode(ctx, mode); }
}

void glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    S.vp[0] = x; S.vp[1] = y; S.vp[2] = w; S.vp[3] = h;
    send_viewport();
}

void glScissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
    S.sc[0] = x; S.sc[1] = y; S.sc[2] = w; S.sc[3] = h;
    send_scissor();
}

void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a)
{
    S.clear_color[0] = r; S.clear_color[1] = g; S.clear_color[2] = b; S.clear_color[3] = a;
    send_clear_values();
}
void glClearDepth(GLclampd d) { S.clear_depth = (GLfloat)d; send_clear_values(); }
void glClearStencil(GLint s) { S.clear_stencil = s; send_clear_values(); }

void glClear(GLbitfield mask)
{
    u32 m = 0;
    if (mask & GL_COLOR_BUFFER_BIT) m |= GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A;
    if (mask & GL_DEPTH_BUFFER_BIT) m |= GCM_CLEAR_Z;
    if (mask & GL_STENCIL_BUFFER_BIT) m |= GCM_CLEAR_S;
    if (m) rsxClearSurface(ctx, m);
}

/* ---------------------------------------------------------------- lighting, material, fog */

static void copy4(GLfloat *d, const GLfloat *s) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; }

void glLightfv(GLenum light, GLenum pname, const GLfloat *p)
{
    Light *l;
    if (light < GL_LIGHT0 || light > GL_LIGHT7) return;
    l = &S.lt[light - GL_LIGHT0];
    switch (pname) {
    case GL_AMBIENT:  copy4(l->amb, p); break;
    case GL_DIFFUSE:  copy4(l->dif, p); break;
    case GL_SPECULAR: copy4(l->spec, p); break;
    case GL_POSITION: {
        const GLfloat *m = mv_stack[mv_top];
        int i;
        for (i = 0; i < 4; i++)
            l->pos[i] = m[i] * p[0] + m[4 + i] * p[1] + m[8 + i] * p[2] + m[12 + i] * p[3];
        break;
    }
    default: break;
    }
    dirty |= D_LIGHT;
}
void glLightf(GLenum light, GLenum pname, GLfloat p) { (void)light; (void)pname; (void)p; }
void glLighti(GLenum light, GLenum pname, GLint p) { (void)light; (void)pname; (void)p; }

void glLightModelfv(GLenum pname, const GLfloat *p)
{
    if (pname == GL_LIGHT_MODEL_AMBIENT) { copy4(S.scene_amb, p); dirty |= D_LIGHT; }
}
void glLightModeli(GLenum pname, GLint p) { (void)pname; (void)p; }
void glLightModelf(GLenum pname, GLfloat p) { (void)pname; (void)p; }

void glMaterialfv(GLenum face, GLenum pname, const GLfloat *p)
{
    (void)face;
    switch (pname) {
    case GL_AMBIENT:   copy4(S.mat_amb, p); break;
    case GL_DIFFUSE:   copy4(S.mat_dif, p); break;
    case GL_SPECULAR:  copy4(S.mat_spec, p); break;
    case GL_EMISSION:  copy4(S.mat_emi, p); break;
    case GL_SHININESS: S.shininess = p[0]; break;
    case GL_AMBIENT_AND_DIFFUSE: copy4(S.mat_amb, p); copy4(S.mat_dif, p); break;
    default: break;
    }
    dirty |= D_LIGHT;
}
void glMaterialf(GLenum face, GLenum pname, GLfloat p)
{
    (void)face;
    if (pname == GL_SHININESS) { S.shininess = p; dirty |= D_LIGHT; }
}
void glColorMaterial(GLenum face, GLenum mode) { (void)face; S.cm_mode = mode; dirty |= D_LIGHT; }

void glFogf(GLenum pname, GLfloat p)
{
    switch (pname) {
    case GL_FOG_MODE:    S.fog_mode = (GLenum)p; break;
    case GL_FOG_DENSITY: S.fog_density = p; break;
    case GL_FOG_START:   S.fog_start = p; break;
    case GL_FOG_END:     S.fog_end = p; break;
    default: break;
    }
    dirty |= D_FOG;
}
void glFogi(GLenum pname, GLint p) { glFogf(pname, (GLfloat)p); }
void glFogfv(GLenum pname, const GLfloat *p)
{
    if (pname == GL_FOG_COLOR) { copy4(S.fog_color, p); dirty |= D_FOG; }
    else glFogf(pname, p[0]);
}

/* ---------------------------------------------------------------- textures */

static void defer_free(void *p)
{
    if (!p) return;
    if (free_n == MAX_FREEQ) {      /* queue full: the RSX has to finish with them now */
        int i;
        wait_finish();
        for (i = 0; i < free_n; i++) rsxFree(free_q[i]);
        free_n = 0;
    }
    free_q[free_n++] = p;
}

void glGenTextures(GLsizei n, GLuint *out)
{
    static GLuint next = 1;
    int i, tries;
    for (i = 0; i < n; i++) {
        out[i] = 0;
        for (tries = 0; tries < MAX_TEX; tries++) {
            GLuint id = next;
            next = next + 1 < MAX_TEX ? next + 1 : 1;
            if (!tex[id].used) {
                memset(&tex[id], 0, sizeof(Tex));
                tex[id].used = GL_TRUE;
                tex[id].wrap_s = tex[id].wrap_t = GL_REPEAT;
                tex[id].min_f = GL_NEAREST_MIPMAP_LINEAR;
                tex[id].mag_f = GL_LINEAR;
                out[i] = id;
                break;
            }
        }
    }
}

void glDeleteTextures(GLsizei n, const GLuint *ids)
{
    int i;
    for (i = 0; i < n; i++) {
        GLuint id = ids[i];
        if (id == 0 || id >= MAX_TEX || !tex[id].used) continue;
        defer_free(tex[id].mem);
        st_tex_bytes -= tex[id].bytes;
        memset(&tex[id], 0, sizeof(Tex));
        if (S.bound_tex == id) { S.bound_tex = 0; dirty |= D_TEX; }
    }
}

void glBindTexture(GLenum target, GLuint id)
{
    if (target != GL_TEXTURE_2D || id >= MAX_TEX) return;
    if (id && !tex[id].used) {
        memset(&tex[id], 0, sizeof(Tex));
        tex[id].used = GL_TRUE;
        tex[id].wrap_s = tex[id].wrap_t = GL_REPEAT;
        tex[id].min_f = GL_NEAREST_MIPMAP_LINEAR;
        tex[id].mag_f = GL_LINEAR;
    }
    if (S.bound_tex != id) { S.bound_tex = id; dirty |= D_TEX; }
}

void glTexParameteri(GLenum target, GLenum pname, GLint p)
{
    Tex *t = &tex[S.bound_tex];
    if (target != GL_TEXTURE_2D) return;
    switch (pname) {
    case GL_TEXTURE_WRAP_S:     t->wrap_s = p; break;
    case GL_TEXTURE_WRAP_T:     t->wrap_t = p; break;
    case GL_TEXTURE_MIN_FILTER: t->min_f = p; break;
    case GL_TEXTURE_MAG_FILTER: t->mag_f = p; break;
    default: return;
    }
    dirty |= D_TEX;
}
void glTexParameterf(GLenum target, GLenum pname, GLfloat p) { glTexParameteri(target, pname, (GLint)p); }
void glTexParameterfv(GLenum target, GLenum pname, const GLfloat *p) { (void)target; (void)pname; (void)p; }

void glTexEnvi(GLenum target, GLenum pname, GLint p)
{
    if (target == GL_TEXTURE_ENV && pname == GL_TEXTURE_ENV_MODE) { S.texenv_mode = p; dirty |= D_TEX; }
}
void glTexEnvf(GLenum target, GLenum pname, GLfloat p) { glTexEnvi(target, pname, (GLint)p); }
void glTexEnvfv(GLenum target, GLenum pname, const GLfloat *p)
{
    if (target == GL_TEXTURE_ENV && pname == GL_TEXTURE_ENV_COLOR) { copy4(S.texenv_color, p); dirty |= D_TEX; }
    else glTexEnvi(target, pname, (GLint)p[0]);
}

void glTexGeni(GLenum coord, GLenum pname, GLint p)
{
    (void)coord;
    if (pname == GL_TEXTURE_GEN_MODE) { S.texgen_mode = p; dirty |= D_LIGHT; }
}
void glTexGenfv(GLenum coord, GLenum pname, const GLfloat *p) { (void)coord; (void)pname; (void)p; }

void glPixelStorei(GLenum pname, GLint p)
{
    if (pname == GL_UNPACK_ALIGNMENT) unpack_align = p;
    else if (pname == GL_UNPACK_ROW_LENGTH) unpack_row_len = p;
}
void glPixelTransferf(GLenum pname, GLfloat p) { (void)pname; (void)p; }

/* GL_PROXY_TEXTURE_2D: what the last proxy query would have created */
static GLint proxy_w, proxy_h, proxy_ifmt;

static int ilog2(u32 v) { int r = 0; while (v >>= 1) r++; return r; }
static u32 lvl_dim(u32 d, int i) { return d >> i ? d >> i : 1; }

/* Bytes of the levels before level n of a w x h mipmap chain */
static u32 chain_offset(u32 w, u32 h, int n)
{
    u32 o = 0;
    int i;
    for (i = 0; i < n; i++) o += lvl_dim(w, i) * lvl_dim(h, i) * 4;
    return o;
}

/* One source texel as A R G B */
static void texel(GLubyte *d, const GLubyte *s, int comps, GLenum format, GLint ifmt)
{
    switch (comps) {
    case 4: d[0] = s[3]; d[1] = s[0]; d[2] = s[1]; d[3] = s[2]; break;
    case 3: d[0] = 255;  d[1] = s[0]; d[2] = s[1]; d[3] = s[2]; break;
    case 2: d[0] = s[1]; d[1] = d[2] = d[3] = s[0]; break;
    default:
        if (format == GL_ALPHA) { d[0] = s[0]; d[1] = d[2] = d[3] = 255; }
        else if (format == GL_INTENSITY || ifmt == GL_INTENSITY) d[0] = d[1] = d[2] = d[3] = s[0];
        else { d[0] = 255; d[1] = d[2] = d[3] = s[0]; }
        break;
    }
}

/* Converts a source image into a W x H ARGB level at dst (averaging boxes
   of the source if it is larger), linear or swizzled. The RSX swizzle is a
   Morton order: x bits at the even and y bits at the odd positions up to
   the smaller dimension, the rest of the larger one above. */
static void store_level(GLubyte *dst, u32 W, u32 H, int swz, const GLubyte *src, u32 w, u32 h,
                        u32 stride, int comps, GLenum format, GLint ifmt)
{
    static u32 xs[MAX_TEX_DIM * 4], ys[MAX_TEX_DIM * 4];
    u32 fx = w / W, fy = h / H, x, y, i, j, k;
    int m = ilog2(W < H ? W : H);
    GLubyte px[4];

    if (swz) {
        for (x = 0; x < W; x++) {
            u32 o = 0;
            for (k = 0; k < (u32)m; k++) o |= ((x >> k) & 1) << (2 * k);
            xs[x] = o | (x >> m) << (2 * m);
        }
        for (y = 0; y < H; y++) {
            u32 o = 0;
            for (k = 0; k < (u32)m; k++) o |= ((y >> k) & 1) << (2 * k + 1);
            ys[y] = o | (y >> m) << (2 * m);
        }
    }
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            GLubyte *d = dst + 4 * (swz ? (xs[x] | ys[y]) : y * W + x);
            if (fx == 1 && fy == 1) {
                texel(d, src + (size_t)y * stride + (size_t)x * comps, comps, format, ifmt);
            } else {
                u32 sum[4] = { 0, 0, 0, 0 }, n = fx * fy;
                for (j = 0; j < fy; j++)
                    for (i = 0; i < fx; i++) {
                        texel(px, src + (size_t)(y * fy + j) * stride + (size_t)(x * fx + i) * comps,
                              comps, format, ifmt);
                        sum[0] += px[0]; sum[1] += px[1]; sum[2] += px[2]; sum[3] += px[3];
                    }
                d[0] = sum[0] / n; d[1] = sum[1] / n; d[2] = sum[2] / n; d[3] = sum[3] / n;
            }
        }
    }
}

void glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border,
                  GLenum format, GLenum type, const GLvoid *pixels)
{
    Tex *t = &tex[S.bound_tex];
    int comps, rowlen, stride, lvl;
    u32 W, H, size;
    GLubyte *tmp;
    (void)border;

    if (target == GL_PROXY_TEXTURE_2D) {
        int ok = w > 0 && h > 0 && (w >> level) <= 4096 && (h >> level) <= 4096;
        proxy_w = ok ? w : 0;
        proxy_h = ok ? h : 0;
        proxy_ifmt = ok ? ifmt : 0;
        return;
    }
    if (target != GL_TEXTURE_2D || S.bound_tex == 0 || level < 0) return;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return;
    if (type != GL_UNSIGNED_BYTE && pixels) return;

    switch (format) {
    case GL_RGBA:            comps = 4; break;
    case GL_RGB:             comps = 3; break;
    case GL_LUMINANCE_ALPHA: comps = 2; break;
    case GL_LUMINANCE: case GL_ALPHA: case GL_INTENSITY: comps = 1; break;
    default: return;
    }
    rowlen = unpack_row_len > 0 ? unpack_row_len : w;
    stride = (rowlen * comps + unpack_align - 1) / unpack_align * unpack_align;

    if (level > 0) {
        /* a mipmap level (PLIB sends them all): into the chain, in order */
        lvl = level - t->shift;
        if (!t->chain || !t->mem || !pixels || lvl <= 0 || lvl != t->levels) return;
        if (lvl > ilog2(t->w > t->h ? t->w : t->h)) return;
        W = lvl_dim(t->w, lvl);
        H = lvl_dim(t->h, lvl);
        if ((u32)w != W || (u32)h != H) return;
        size = W * H * 4;
        tmp = (GLubyte *)malloc(size);
        if (!tmp) return;
        store_level(tmp, W, H, 1, (const GLubyte *)pixels, w, h, stride, comps, format, ifmt);
        if (!t->fresh) wait_finish();       /* the RSX may be reading this texture */
        memcpy((GLubyte *)t->mem + chain_offset(t->w, t->h, lvl), tmp, size);
        free(tmp);
        t->levels = lvl + 1;
        t->t.mipmap = t->levels;
        rsxInvalidateTextureCache(ctx, GCM_INVALIDATE_TEXTURE);
        dirty |= D_TEX;
        return;
    }

    /* level 0: new storage */
    t->shift = 0;
    while ((w >> t->shift) > MAX_TEX_DIM || (h >> t->shift) > MAX_TEX_DIM) t->shift++;
    W = lvl_dim(w, t->shift);
    H = lvl_dim(h, t->shift);
    t->swz = (W & (W - 1)) == 0 && (H & (H - 1)) == 0 && pixels != NULL;
    size = W * H * 4;
    t->chain = 0;
    if (t->swz && st_tex_bytes - (t->mem ? t->bytes : 0) + size * 4 / 3 < TEX_CHAIN_BUDGET) {
        size = chain_offset(W, H, ilog2(W > H ? W : H) + 1);
        t->chain = 1;
    }
    if (t->mem && t->bytes == size && t->w == W && t->h == H) {
        wait_finish();      /* same size: the RSX may still be reading the old texels */
    } else {
        defer_free(t->mem);
        st_tex_bytes -= t->bytes;
        t->bytes = 0;
        t->mem = rsxMemalign(128, size);
        if (!t->mem) {
            {
                struct mallinfo mi = mallinfo();
                ps3glLog("ps3gl: out of RSX memory for a %ux%u texture (textures %uK, lists %uK, heap %luK)",
                         W, H, st_tex_bytes >> 10, st_list_bytes >> 10, (unsigned long)mi.uordblks >> 10);
            }
            t->has_data = GL_FALSE;
            dirty |= D_TEX;
            return;
        }
        t->bytes = size;
        st_tex_bytes += size;
        t->fresh = 1;
    }
    t->w = W;
    t->h = H;
    t->levels = 1;

    if (!pixels) {
        memset(t->mem, 0, (size_t)W * H * 4);
    } else if (t->swz) {
        /* swizzle in main memory: the PPU writes RSX memory best in order */
        tmp = (GLubyte *)malloc((size_t)W * H * 4);
        if (tmp) {
            store_level(tmp, W, H, 1, (const GLubyte *)pixels, w, h, stride, comps, format, ifmt);
            memcpy(t->mem, tmp, (size_t)W * H * 4);
            free(tmp);
        } else {
            store_level((GLubyte *)t->mem, W, H, 1, (const GLubyte *)pixels, w, h, stride, comps, format, ifmt);
        }
    } else {
        store_level((GLubyte *)t->mem, W, H, 0, (const GLubyte *)pixels, w, h, stride, comps, format, ifmt);
    }

    t->t.format = GCM_TEXTURE_FORMAT_A8R8G8B8 | (t->swz ? GCM_TEXTURE_FORMAT_SWZ : GCM_TEXTURE_FORMAT_LIN);
    t->t.mipmap = 1;
    t->t.dimension = GCM_TEXTURE_DIMS_2D;
    t->t.cubemap = GCM_FALSE;
    t->t.remap = (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT)
               | (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT)
               | (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT)
               | (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_A_SHIFT)
               | (GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_B_SHIFT)
               | (GCM_TEXTURE_REMAP_COLOR_G << GCM_TEXTURE_REMAP_COLOR_G_SHIFT)
               | (GCM_TEXTURE_REMAP_COLOR_R << GCM_TEXTURE_REMAP_COLOR_R_SHIFT)
               | (GCM_TEXTURE_REMAP_COLOR_A << GCM_TEXTURE_REMAP_COLOR_A_SHIFT);
    t->t.width = W;
    t->t.height = H;
    t->t.depth = 1;
    t->t.location = GCM_LOCATION_RSX;
    t->t.pitch = t->swz ? 0 : W * 4;
    rsxAddressToOffset(t->mem, &t->t.offset);
    t->ifmt = ifmt;
    t->has_data = GL_TRUE;
    rsxInvalidateTextureCache(ctx, GCM_INVALIDATE_TEXTURE);
    dirty |= D_TEX;
}

void glTexImage1D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLint border, GLenum format,
                  GLenum type, const GLvoid *pixels)
{ (void)target; (void)level; (void)ifmt; (void)w; (void)border; (void)format; (void)type; (void)pixels; }

void glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *out)
{
    Tex *t = &tex[S.bound_tex];
    if (target == GL_PROXY_TEXTURE_2D) {
        switch (pname) {
        case GL_TEXTURE_WIDTH:  *out = proxy_w >> level; break;
        case GL_TEXTURE_HEIGHT: *out = proxy_h >> level; break;
        case GL_TEXTURE_INTERNAL_FORMAT: *out = proxy_ifmt; break;
        default: *out = 0; break;
        }
        return;
    }
    switch (pname) {
    case GL_TEXTURE_WIDTH:  *out = t->has_data ? (t->t.width >> level) : 0; break;
    case GL_TEXTURE_HEIGHT: *out = t->has_data ? (t->t.height >> level) : 0; break;
    case GL_TEXTURE_INTERNAL_FORMAT: *out = t->ifmt; break;
    case GL_TEXTURE_RED_SIZE: case GL_TEXTURE_GREEN_SIZE: case GL_TEXTURE_BLUE_SIZE:
    case GL_TEXTURE_ALPHA_SIZE: *out = 8; break;
    default: *out = 0; break;
    }
}

GLboolean glIsTexture(GLuint id) { return id && id < MAX_TEX && tex[id].used; }

/* ---------------------------------------------------------------- attribute stack */

void glPushAttrib(GLbitfield mask)
{
    if (attr_top >= ATTR_DEPTH) return;
    attr_stack[attr_top] = S;
    attr_mask[attr_top++] = mask;
}

#define CP(f) S.f = o->f
#define CPA(f) memcpy(S.f, o->f, sizeof S.f)

void glPopAttrib(void)
{
    const State *o;
    GLbitfield m;
    if (attr_top == 0) return;
    o = &attr_stack[--attr_top];
    m = attr_mask[attr_top];

    if (m & GL_ENABLE_BIT) {
        CP(tex2d); CP(lighting); CPA(light); CP(fog); CP(depth_test); CP(blend); CP(alpha_test);
        CP(cull); CP(color_material); CP(scissor_test); CP(stencil_test); CP(po_fill); CP(po_line);
        CP(normalize); CP(texgen_s); CP(texgen_t);
    }
    if (m & GL_COLOR_BUFFER_BIT) {
        CP(blend); CP(alpha_test); CP(blend_src); CP(blend_dst); CP(alpha_func); CP(alpha_ref);
        CPA(clear_color); CPA(cmask);
    }
    if (m & GL_DEPTH_BUFFER_BIT) { CP(depth_test); CP(depth_func); CP(depth_mask); CP(clear_depth); }
    if (m & GL_STENCIL_BUFFER_BIT) {
        CP(stencil_test); CP(st_func); CP(st_fail); CP(st_zfail); CP(st_zpass); CP(st_ref);
        CP(st_mask); CP(st_wmask); CP(clear_stencil);
    }
    if (m & GL_CURRENT_BIT) { CPA(color); CPA(normal); CPA(texcoord); }
    if (m & GL_FOG_BIT) { CP(fog); CP(fog_mode); CP(fog_density); CP(fog_start); CP(fog_end); CPA(fog_color); }
    if (m & GL_LIGHTING_BIT) {
        CP(lighting); CPA(light); CP(color_material); CPA(lt); CPA(mat_amb); CPA(mat_dif);
        CPA(mat_spec); CPA(mat_emi); CP(shininess); CPA(scene_amb); CP(cm_mode); CP(shade_model);
    }
    if (m & GL_POLYGON_BIT) {
        CP(cull); CP(cull_face); CP(front_face); CP(poly_front); CP(poly_back); CP(po_fill);
        CP(po_line); CP(po_factor); CP(po_units);
    }
    if (m & GL_LINE_BIT) CP(line_width);
    if (m & GL_POINT_BIT) CP(point_size);
    if (m & GL_TEXTURE_BIT) {
        CP(tex2d); CP(bound_tex); CP(texenv_mode); CPA(texenv_color); CP(texgen_s); CP(texgen_t);
        CP(texgen_mode);
    }
    if (m & GL_TRANSFORM_BIT) { CP(matrix_mode); CP(normalize); }
    if (m & GL_VIEWPORT_BIT) CPA(vp);
    if (m & GL_SCISSOR_BIT) { CP(scissor_test); CPA(sc); }

    send_raster();
    dirty |= D_LIGHT | D_FOG | D_TEX;
}

/* ---------------------------------------------------------------- queries */

void glGetIntegerv(GLenum pname, GLint *p)
{
    switch (pname) {
    case GL_VIEWPORT:           memcpy(p, S.vp, sizeof S.vp); break;
    case GL_SCISSOR_BOX:        memcpy(p, S.sc, sizeof S.sc); break;
    case GL_MAX_TEXTURE_SIZE:   *p = 4096; break;
    case GL_MAX_TEXTURE_UNITS:  *p = 1; break;
    case GL_MAX_LIGHTS:         *p = 8; break;
    case GL_MAX_CLIP_PLANES:    *p = 6; break;
    case GL_MATRIX_MODE:        *p = S.matrix_mode; break;
    case GL_DEPTH_BITS:         *p = 24; break;
    case GL_STENCIL_BITS:       *p = 8; break;
    case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: *p = 8; break;
    case GL_ALPHA_BITS:         *p = 0; break;
    case GL_TEXTURE_BINDING_2D: *p = S.bound_tex; break;
    case GL_BLEND_SRC:          *p = S.blend_src; break;
    case GL_BLEND_DST:          *p = S.blend_dst; break;
    case GL_DEPTH_FUNC:         *p = S.depth_func; break;
    case GL_SHADE_MODEL:        *p = S.shade_model; break;
    case GL_UNPACK_ALIGNMENT:   *p = unpack_align; break;
    case GL_MODELVIEW_STACK_DEPTH:  *p = mv_top + 1; break;
    case GL_PROJECTION_STACK_DEPTH: *p = pr_top + 1; break;
    case GL_MAX_MODELVIEW_STACK_DEPTH: *p = MV_DEPTH; break;
    case GL_RENDER_MODE:        *p = GL_RENDER; break;
    default:                    *p = 0; break;
    }
}

void glGetFloatv(GLenum pname, GLfloat *p)
{
    switch (pname) {
    case GL_MODELVIEW_MATRIX:  memcpy(p, mv_stack[mv_top], 64); break;
    case GL_PROJECTION_MATRIX: memcpy(p, pr_stack[pr_top], 64); break;
    case GL_TEXTURE_MATRIX:    memcpy(p, tx_stack[tx_top], 64); break;
    case GL_CURRENT_COLOR:     copy4(p, S.color); break;
    case GL_FOG_COLOR:         copy4(p, S.fog_color); break;
    case GL_COLOR_CLEAR_VALUE: copy4(p, S.clear_color); break;
    case GL_LINE_WIDTH:        *p = S.line_width; break;
    case GL_POINT_SIZE:        *p = S.point_size; break;
    case GL_FOG_DENSITY:       *p = S.fog_density; break;
    case GL_FOG_START:         *p = S.fog_start; break;
    case GL_FOG_END:           *p = S.fog_end; break;
    case GL_DEPTH_RANGE:       p[0] = 0; p[1] = 1; break;
    case GL_LINE_WIDTH_RANGE: case GL_POINT_SIZE_RANGE: p[0] = 1; p[1] = 1; break;
    default: { GLint i = 0; glGetIntegerv(pname, &i); *p = (GLfloat)i; break; }
    }
}

void glGetDoublev(GLenum pname, GLdouble *p)
{
    GLfloat f[16];
    int i, n = (pname == GL_MODELVIEW_MATRIX || pname == GL_PROJECTION_MATRIX || pname == GL_TEXTURE_MATRIX) ? 16 : 1;
    memset(f, 0, sizeof f);
    glGetFloatv(pname, f);
    for (i = 0; i < n; i++) p[i] = f[i];
}

void glGetBooleanv(GLenum pname, GLboolean *p)
{
    if (pname == GL_DEPTH_WRITEMASK) *p = S.depth_mask;
    else *p = glIsEnabled(pname);
}

const GLubyte *glGetString(GLenum name)
{
    switch (name) {
    case GL_VENDOR:     return (const GLubyte *)"ps3gl";
    case GL_RENDERER:   return (const GLubyte *)"RSX (NV47)";
    case GL_VERSION:    return (const GLubyte *)"1.1 ps3gl";
    case GL_EXTENSIONS: return (const GLubyte *)"";
    default:            return (const GLubyte *)"";
    }
}

GLenum glGetError(void) { return GL_NO_ERROR; }

/* ---------------------------------------------------------------- accepted and ignored */

void glHint(GLenum target, GLenum mode) { (void)target; (void)mode; }
void glFlush(void) {}
void glFinish(void) {}
void glClipPlane(GLenum plane, const GLdouble *eq) { (void)plane; (void)eq; }
void glInitNames(void) {}
void glLoadName(GLuint n) { (void)n; }
void glPushName(GLuint n) { (void)n; }
void glPopName(void) {}
GLint glRenderMode(GLenum mode) { (void)mode; return 0; }
void glSelectBuffer(GLsizei size, GLuint *buf) { (void)size; (void)buf; }
void glRasterPos2i(GLint x, GLint y) { (void)x; (void)y; }
void glRasterPos2f(GLfloat x, GLfloat y) { (void)x; (void)y; }
void glRasterPos3f(GLfloat x, GLfloat y, GLfloat z) { (void)x; (void)y; (void)z; }
void glRasterPos3fv(const GLfloat *v) { (void)v; }
void glBitmap(GLsizei w, GLsizei h, GLfloat xo, GLfloat yo, GLfloat xm, GLfloat ym, const GLubyte *bm)
{ (void)w; (void)h; (void)xo; (void)yo; (void)xm; (void)ym; (void)bm; }
void glDrawPixels(GLsizei w, GLsizei h, GLenum format, GLenum type, const GLvoid *px)
{ (void)w; (void)h; (void)format; (void)type; (void)px; }
void glDrawBuffer(GLenum mode) { (void)mode; }
void glReadBuffer(GLenum mode) { (void)mode; }
void glCopyTexSubImage2D(GLenum target, GLint level, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h)
{ (void)target; (void)level; (void)xo; (void)yo; (void)x; (void)y; (void)w; (void)h; }
void glCopyTexImage2D(GLenum target, GLint level, GLenum ifmt, GLint x, GLint y, GLsizei w, GLsizei h, GLint border)
{ (void)target; (void)level; (void)ifmt; (void)x; (void)y; (void)w; (void)h; (void)border; }

/* No read-back: callers get zeros (black screenshots, far depth). */
void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, GLvoid *px)
{
    size_t bpp = (format == GL_RGBA ? 4 : format == GL_RGB ? 3 : 1) * (type == GL_FLOAT ? 4 : 1);
    (void)x; (void)y;
    if (px && w > 0 && h > 0) memset(px, 0, (size_t)w * h * bpp);
}

void glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid *px)
{ (void)target; (void)level; (void)format; (void)type; (void)px; }

GLXContext glXGetCurrentContext(void) { return ctx ? (GLXContext)ctx : NULL; }

/* ---------------------------------------------------------------- init, frame */

void ps3glGetSize(int *w, int *h) { *w = scr_w; *h = scr_h; }

static u32 last_draws, last_verts;

void ps3glStats(char *buf, int len)
{
    snprintf(buf, len, "rsx: tex %uK, lists %uK in %u, draws %u, verts %u%s%s",
             st_tex_bytes >> 10, st_list_bytes >> 10, st_lists_alive, last_draws, last_verts,
             st_vram_fail ? ", RSX MEMORY FULL" : "", st_mem_fail ? ", MAIN MEMORY FULL" : "");
}

static void load_programs(void)
{
    rsxLoadVertexProgram(ctx, vpo, vp_ucode);
    fp_loaded = -1;
    dirty = D_ALL;
}

static void defaults(void)
{
    static const GLfloat white[4] = { 1, 1, 1, 1 }, black[4] = { 0, 0, 0, 1 };
    int i;
    memset(&S, 0, sizeof S);
    S.blend_src = GL_ONE; S.blend_dst = GL_ZERO;
    S.alpha_func = GL_ALWAYS;
    S.cmask[0] = S.cmask[1] = S.cmask[2] = S.cmask[3] = GL_TRUE;
    S.depth_func = GL_LESS; S.depth_mask = GL_TRUE; S.clear_depth = 1.0f;
    S.st_func = GL_ALWAYS; S.st_fail = S.st_zfail = S.st_zpass = GL_KEEP;
    S.st_mask = S.st_wmask = 0xff;
    copy4(S.color, white);
    S.normal[2] = 1.0f;
    S.fog_mode = GL_EXP; S.fog_density = 1.0f; S.fog_end = 1.0f;
    for (i = 0; i < 8; i++) {
        copy4(S.lt[i].amb, black);
        copy4(S.lt[i].dif, i == 0 ? white : black);
        copy4(S.lt[i].spec, i == 0 ? white : black);
        S.lt[i].pos[2] = 1.0f;
    }
    S.mat_amb[0] = S.mat_amb[1] = S.mat_amb[2] = 0.2f; S.mat_amb[3] = 1.0f;
    S.mat_dif[0] = S.mat_dif[1] = S.mat_dif[2] = 0.8f; S.mat_dif[3] = 1.0f;
    copy4(S.mat_spec, black); copy4(S.mat_emi, black);
    S.scene_amb[0] = S.scene_amb[1] = S.scene_amb[2] = 0.2f; S.scene_amb[3] = 1.0f;
    S.cm_mode = GL_AMBIENT_AND_DIFFUSE; S.shade_model = GL_SMOOTH;
    S.cull_face = GL_BACK; S.front_face = GL_CCW;
    S.poly_front = S.poly_back = GL_FILL;
    S.line_width = 1.0f; S.point_size = 1.0f;
    S.texenv_mode = GL_MODULATE; S.texgen_mode = GL_EYE_LINEAR;
    S.matrix_mode = GL_MODELVIEW;
    S.vp[2] = S.sc[2] = scr_w; S.vp[3] = S.sc[3] = scr_h;
    memcpy(mv_stack[0], ident, sizeof ident);
    memcpy(pr_stack[0], ident, sizeof ident);
    memcpy(tx_stack[0], ident, sizeof ident);
}

static int set_mode(u32 id)
{
    videoConfiguration cfg;
    if (videoGetResolutionAvailability(VIDEO_PRIMARY, id, VIDEO_ASPECT_AUTO, 0) != 1) return 0;
    if (videoGetResolution(id, &vmode)) return 0;
    memset(&cfg, 0, sizeof cfg);
    cfg.resolution = (u8)id;
    cfg.format = VIDEO_BUFFER_FORMAT_XRGB;
    cfg.aspect = VIDEO_ASPECT_AUTO;
    cfg.pitch = (u32)vmode.width * 4;
    return videoConfigure(VIDEO_PRIMARY, &cfg, NULL, 0) == 0;
}

static rsxProgramConst *vconst(const char *name)
{
    rsxProgramConst *c = rsxVertexProgramGetConst(vpo, name);
    if (!c) ps3glLog("ps3gl: vertex program has no constant '%s'", name);
    return c;
}

void ps3glInit(void)
{
    static const u32 modes[] = { VIDEO_RESOLUTION_720, VIDEO_RESOLUTION_480, VIDEO_RESOLUTION_576, VIDEO_RESOLUTION_1080 };
    static const struct { const unsigned char *data; } fps[FP_COUNT] = {
        { fp_notex_fpo }, { fp_modulate_fpo }, { fp_replace_fpo }, { fp_decal_fpo }, { fp_blend_fpo }, { fp_add_fpo }
    };
    void *host;
    u32 pitch, color_ofs[2], z_ofs, size, i;
    void *zbuf, *ucode;
    char name[8];
    int ok = 0;

    if (ctx) {      /* already up (the hangar ran first): just reset the GL state */
        defaults();
        rsxSetSurface(ctx, &surf[cur_fb]);
        send_raster();
        load_programs();
        return;
    }
    host = memalign(1024 * 1024, HOST_SIZE);
    rsxInit(&ctx, CB_SIZE, HOST_SIZE, host);
    for (i = 0; i < sizeof modes / sizeof modes[0] && !ok; i++) ok = set_mode(modes[i]);
    if (!ok) { ps3glLog("ps3gl: no usable video mode"); exit(1); }
    scr_w = vmode.width;
    scr_h = vmode.height;

    rsxSetWriteBackendLabel(ctx, LABEL_INDEX, label_val);
    rsxSetWaitLabel(ctx, LABEL_INDEX, label_val);
    ++label_val;
    wait_finish();
    gcmSetFlipMode(GCM_FLIP_VSYNC);

    pitch = (u32)scr_w * 4;
    for (i = 0; i < 2; i++) {
        void *fb = rsxMemalign(64, scr_h * pitch);
        rsxAddressToOffset(fb, &color_ofs[i]);
        gcmSetDisplayBuffer(i, color_ofs[i], pitch, scr_w, scr_h);
    }
    zbuf = rsxMemalign(64, scr_h * pitch);
    rsxAddressToOffset(zbuf, &z_ofs);
    for (i = 0; i < 2; i++) {
        gcmSurface *sf = &surf[i];
        memset(sf, 0, sizeof *sf);
        /* A8R8G8B8, not X8R8G8B8: with no alpha channel in the target RPCS3 feeds
           alpha = 1 into GL_SRC_ALPHA blending (the alpha test still sees the real
           value). The display ignores the alpha byte. */
        sf->colorFormat = GCM_SURFACE_A8R8G8B8;
        sf->colorTarget = GCM_SURFACE_TARGET_0;
        sf->colorLocation[0] = GCM_LOCATION_RSX;
        sf->colorOffset[0] = color_ofs[i];
        sf->colorPitch[0] = pitch;
        sf->colorLocation[1] = sf->colorLocation[2] = sf->colorLocation[3] = GCM_LOCATION_RSX;
        sf->colorPitch[1] = sf->colorPitch[2] = sf->colorPitch[3] = 64;
        sf->depthFormat = GCM_SURFACE_ZETA_Z24S8;
        sf->depthLocation = GCM_LOCATION_RSX;
        sf->depthOffset = z_ofs;
        sf->depthPitch = pitch;
        sf->type = GCM_SURFACE_TYPE_LINEAR;
        sf->antiAlias = GCM_SURFACE_CENTER_1;
        sf->width = scr_w;
        sf->height = scr_h;
    }

    vb = (u8 *)rsxMemalign(128, VB_BYTES);
    if (!vb) { ps3glLog("ps3gl: no RSX memory for the vertex buffer"); exit(1); }

    vpo = (rsxVertexProgram *)ffp_vpo;
    rsxVertexProgramGetUCode(vpo, &vp_ucode, &size);
    for (i = 0; i < FP_COUNT; i++) {
        void *buf;
        fpo[i] = (rsxFragmentProgram *)fps[i].data;
        rsxFragmentProgramGetUCode(fpo[i], &ucode, &size);
        buf = rsxMemalign(64, size);
        memcpy(buf, ucode, size);
        rsxAddressToOffset(buf, &fp_ofs[i]);
    }
    for (i = 0; i < 4; i++) { sprintf(name, "mvp%u", i); uc.mvp[i] = vconst(name); }
    for (i = 0; i < 3; i++) { sprintf(name, "mv%u", i); uc.mv[i] = vconst(name); }
    for (i = 0; i < 3; i++) { sprintf(name, "nm%u", i); uc.nm[i] = vconst(name); }
    for (i = 0; i < 2; i++) { sprintf(name, "tm%u", i); uc.tm[i] = vconst(name); }
    uc.lightPos = vconst("lightPos"); uc.lightAmb = vconst("lightAmb");
    uc.lightDif = vconst("lightDif"); uc.lightSpec = vconst("lightSpec");
    uc.matAmb = vconst("matAmb"); uc.matDif = vconst("matDif");
    uc.matSpec = vconst("matSpec"); uc.matEmi = vconst("matEmi");
    uc.cmMask = vconst("cmMask"); uc.misc = vconst("misc");
    uc.fogP = vconst("fogP"); uc.fogW = vconst("fogW"); uc.fogC = vconst("fogC");
    uc.envC = vconst("envC");

    defaults();
    rsxSetSurface(ctx, &surf[cur_fb]);
    for (i = 0; i < 8; i++) rsxSetViewportClip(ctx, i, scr_w, scr_h);
    rsxSetZMinMaxControl(ctx, 0, 1, 1);
    rsxSetUserClipPlaneControl(ctx, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE,
                               GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE,
                               GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE);
    rsxSetColorMaskMrt(ctx, 0);
    send_raster();
    load_programs();
    ps3glLog("ps3gl: %dx%d, vertex buffer %d MB", scr_w, scr_h, VB_BYTES >> 20);
}

void ps3glSwapBuffers(void)
{
    int i;
    u32 timeout = 0;

    wait_finish();
    for (i = 0; i < free_n; i++) rsxFree(free_q[i]);
    free_n = 0;

    if (first_flip) gcmResetFlipStatus();
    else {
        while (gcmGetFlipStatus()) {
            usleep(200);
            if (++timeout > 50000) break;
        }
        gcmResetFlipStatus();
    }
    gcmSetFlip(ctx, cur_fb);
    rsxFlushBuffer(ctx);
    gcmSetWaitFlip(ctx);
    first_flip = 0;

    cur_fb ^= 1;
    rsxSetSurface(ctx, &surf[cur_fb]);
    vb_off = 0;
    last_draws = st_draws; last_verts = st_verts;
    st_draws = st_verts = 0;
    send_raster();
    load_programs();
    sysUtilCheckCallback();
}
