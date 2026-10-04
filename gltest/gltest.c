/* ps3gl self-test: a lit, textured, fogged cube drawn with client arrays, a
   colour-material cube drawn in immediate mode from a display list, an
   alpha-blended 2D overlay and an alpha-tested quad. Tilt the controller to
   turn the cubes; START exits. Frame times go to the ps3gl log. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <sys/process.h>
#include <sys/systime.h>
#include <GL/gl.h>
#include <GL/glu.h>
#include <ps3gl.h>
#include "../port/ps3pad.h"

SYS_PROCESS_PARAM(1001, 0x100000);

static const GLfloat cube_v[24][3] = {
    {-1,-1, 1},{ 1,-1, 1},{ 1, 1, 1},{-1, 1, 1},  { 1,-1,-1},{-1,-1,-1},{-1, 1,-1},{ 1, 1,-1},
    { 1,-1, 1},{ 1,-1,-1},{ 1, 1,-1},{ 1, 1, 1},  {-1,-1,-1},{-1,-1, 1},{-1, 1, 1},{-1, 1,-1},
    {-1, 1, 1},{ 1, 1, 1},{ 1, 1,-1},{-1, 1,-1},  {-1,-1,-1},{ 1,-1,-1},{ 1,-1, 1},{-1,-1, 1} };
static const GLfloat cube_n[6][3] = { {0,0,1},{0,0,-1},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0} };
static const GLfloat quad_t[4][2] = { {0,0},{1,0},{1,1},{0,1} };

static GLuint checker(void)
{
    static GLubyte px[64 * 64 * 3];
    GLuint id;
    int x, y;
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++) {
            GLubyte *p = px + (y * 64 + x) * 3;
            int on = ((x >> 3) ^ (y >> 3)) & 1;
            p[0] = on ? 255 : 60; p[1] = on ? 220 : 60; p[2] = on ? 120 : 200;
        }
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, 3, 64, 64, 0, GL_RGB, GL_UNSIGNED_BYTE, px);
    return id;
}

/* RGBA disc with transparent corners, for the alpha test */
static GLuint disc(void)
{
    static GLubyte px[32 * 32 * 4];
    GLuint id;
    int x, y;
    for (y = 0; y < 32; y++)
        for (x = 0; x < 32; x++) {
            GLubyte *p = px + (y * 32 + x) * 4;
            int in = (x - 16) * (x - 16) + (y - 16) * (y - 16) < 14 * 14;
            p[0] = 255; p[1] = 255; p[2] = 255; p[3] = in ? 255 : 0;
        }
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, 4, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return id;
}

static double now(void)
{
    u64 s = 0, ns = 0;
    sysGetCurrentTime(&s, &ns);
    return (double)s + (double)ns * 1e-9;
}

int main(void)
{
    static GLfloat arr_v[24][3], arr_n[24][3], arr_t[24][2];
    static const GLfloat lpos[4] = { 0.4f, 0.6f, 0.7f, 0.0f }, lamb[4] = { 0.2f, 0.2f, 0.2f, 1 };
    static const GLfloat ldif[4] = { 1, 1, 1, 1 }, fogc[4] = { 0.5f, 0.6f, 0.8f, 1 };
    GLuint tex_c, tex_d, list;
    int w, h, i, frames = 0;
    float ax = 20, ay = 30;
    double t0;

    ps3glInit();
    ps3glGetSize(&w, &h);
    for (i = 0; i < 24; i++) {
        int k;
        for (k = 0; k < 3; k++) { arr_v[i][k] = cube_v[i][k]; arr_n[i][k] = cube_n[i / 4][k]; }
        arr_t[i][0] = quad_t[i % 4][0]; arr_t[i][1] = quad_t[i % 4][1];
    }
    tex_c = checker();
    tex_d = disc();

    /* second cube: immediate mode into a display list, colour per face */
    list = glGenLists(1);
    glNewList(list, GL_COMPILE);
    glBegin(GL_QUADS);
    for (i = 0; i < 24; i++) {
        if (i % 4 == 0) {
            glColor3f(0.3f + 0.7f * ((i / 4) & 1), 0.3f + 0.7f * ((i / 8) & 1), 0.3f + 0.7f * ((i / 16) & 1));
            glNormal3fv(cube_n[i / 4]);
        }
        glVertex3fv(cube_v[i]);
    }
    glEnd();
    glEndList();

    glClearColor(fogc[0], fogc[1], fogc[2], 1);
    glEnable(GL_CULL_FACE);
    glLightfv(GL_LIGHT0, GL_AMBIENT, lamb);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, ldif);
    glEnable(GL_LIGHT0);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    glFogf(GL_FOG_START, 4.0f);
    glFogf(GL_FOG_END, 14.0f);
    glFogfv(GL_FOG_COLOR, fogc);

    t0 = now();
    for (;;) {
        const ps3pad_state *pad;
        ps3pad_poll();
        pad = ps3pad_get();
        if (pad->buttons & (1u << PS3PAD_START)) break;
        ay += 0.7f + 3.0f * pad->axis[PS3PAD_TILT_ROLL] + 3.0f * pad->axis[PS3PAD_LX];
        ax += 0.4f + 3.0f * pad->axis[PS3PAD_TILT_PITCH] + 3.0f * pad->axis[PS3PAD_LY];

        glViewport(0, 0, w, h);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        gluPerspective(50.0, (double)w / h, 0.5, 50.0);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glLightfv(GL_LIGHT0, GL_POSITION, lpos);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_LIGHTING);
        glEnable(GL_FOG);

        /* textured cube from client arrays */
        glPushMatrix();
        glTranslatef(-1.8f, 0, -7);
        glRotatef(ax, 1, 0, 0);
        glRotatef(ay, 0, 1, 0);
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, tex_c);
        glColor3f(1, 1, 1);
        glEnable(GL_COLOR_MATERIAL);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_NORMAL_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glVertexPointer(3, GL_FLOAT, 0, arr_v);
        glNormalPointer(GL_FLOAT, 0, arr_n);
        glTexCoordPointer(2, GL_FLOAT, 0, arr_t);
        glDrawArrays(GL_QUADS, 0, 24);
        glDisableClientState(GL_VERTEX_ARRAY);
        glDisableClientState(GL_NORMAL_ARRAY);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glDisable(GL_TEXTURE_2D);
        glPopMatrix();

        /* colour-material cube from the display list, further away (more fog) */
        glPushMatrix();
        glTranslatef(1.8f, 0, -9);
        glRotatef(ay, 0, 1, 0);
        glRotatef(ax, 1, 0, 0);
        glCallList(list);
        glPopMatrix();

        /* 2D overlay */
        glDisable(GL_LIGHTING);
        glDisable(GL_FOG);
        glDisable(GL_DEPTH_TEST);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        gluOrtho2D(0, w, 0, h);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColor4f(0, 0, 0, 0.5f);
        glRecti(20, 20, 320, 100);              /* translucent box, bottom left */
        glColor4f(1, 1, 1, 0.5f);
        glRecti(340, 20, 640, 100);             /* translucent white box next to it */
        glBlendFunc(GL_ONE, GL_ONE);            /* additive, ignores alpha: red tint */
        glColor4f(0.4f, 0, 0, 1);
        glRecti(660, 20, 960, 100);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_TEXTURE_2D);                /* texture alpha: yellow disc, corners clear */
        glBindTexture(GL_TEXTURE_2D, tex_d);
        glColor4f(1, 1, 0, 1);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2i(980, 20);
        glTexCoord2f(1, 0); glVertex2i(1060, 20);
        glTexCoord2f(1, 1); glVertex2i(1060, 100);
        glTexCoord2f(0, 1); glVertex2i(980, 100);
        glEnd();
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_BLEND);

        glColor3f(0, 1, 0);
        glLineWidth(3.0f);
        glBegin(GL_LINE_LOOP);                  /* green outline around it */
        glVertex2i(20, 20); glVertex2i(320, 20); glVertex2i(320, 100); glVertex2i(20, 100);
        glEnd();

        glEnable(GL_TEXTURE_2D);                /* red disc: alpha test cuts the corners */
        glBindTexture(GL_TEXTURE_2D, tex_d);
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(GL_GREATER, 0.5f);
        glColor3f(1, 0.2f, 0.2f);
        glBegin(GL_TRIANGLE_STRIP);
        glTexCoord2f(0, 0); glVertex2i(40, 30);
        glTexCoord2f(1, 0); glVertex2i(100, 30);
        glTexCoord2f(0, 1); glVertex2i(40, 90);
        glTexCoord2f(1, 1); glVertex2i(100, 90);
        glEnd();
        glDisable(GL_ALPHA_TEST);
        glDisable(GL_TEXTURE_2D);

        ps3glSwapBuffers();
        if (++frames % 300 == 0) {
            double t = now();
            ps3glLog("gltest: %d frames, %.1f fps, tilt %.2f %.2f", frames, 300.0 / (t - t0),
                     pad->axis[PS3PAD_TILT_ROLL], pad->axis[PS3PAD_TILT_PITCH]);
            t0 = t;
        }
    }
    return 0;
}
