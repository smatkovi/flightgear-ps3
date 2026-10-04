/* ps3gl: the few GLU entry points FlightGear, SimGear and PLIB use. */
#include <math.h>
#include <string.h>
#include <GL/gl.h>
#include <GL/glu.h>

void gluOrtho2D(GLdouble l, GLdouble r, GLdouble b, GLdouble t) { glOrtho(l, r, b, t, -1.0, 1.0); }

void gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble zn, GLdouble zf)
{
    GLdouble t = zn * tan(fovy * M_PI / 360.0);
    glFrustum(-t * aspect, t * aspect, -t, t, zn, zf);
}

void gluLookAt(GLdouble ex, GLdouble ey, GLdouble ez, GLdouble cx, GLdouble cy, GLdouble cz,
               GLdouble ux, GLdouble uy, GLdouble uz)
{
    GLdouble f[3] = { cx - ex, cy - ey, cz - ez }, s[3], u[3], len;
    GLfloat m[16];
    len = sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (len == 0.0) return;
    f[0] /= len; f[1] /= len; f[2] /= len;
    s[0] = f[1] * uz - f[2] * uy; s[1] = f[2] * ux - f[0] * uz; s[2] = f[0] * uy - f[1] * ux;
    len = sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    if (len == 0.0) return;
    s[0] /= len; s[1] /= len; s[2] /= len;
    u[0] = s[1] * f[2] - s[2] * f[1]; u[1] = s[2] * f[0] - s[0] * f[2]; u[2] = s[0] * f[1] - s[1] * f[0];
    m[0] = (GLfloat)s[0]; m[4] = (GLfloat)s[1]; m[8] = (GLfloat)s[2];   m[12] = 0;
    m[1] = (GLfloat)u[0]; m[5] = (GLfloat)u[1]; m[9] = (GLfloat)u[2];   m[13] = 0;
    m[2] = (GLfloat)-f[0]; m[6] = (GLfloat)-f[1]; m[10] = (GLfloat)-f[2]; m[14] = 0;
    m[3] = 0; m[7] = 0; m[11] = 0; m[15] = 1;
    glMultMatrixf(m);
    glTranslated(-ex, -ey, -ez);
}

static void xform(GLdouble *o, const GLdouble *m, const GLdouble *v)
{
    int i;
    for (i = 0; i < 4; i++) o[i] = m[i] * v[0] + m[4 + i] * v[1] + m[8 + i] * v[2] + m[12 + i] * v[3];
}

GLint gluProject(GLdouble ox, GLdouble oy, GLdouble oz, const GLdouble *model, const GLdouble *proj,
                 const GLint *vp, GLdouble *wx, GLdouble *wy, GLdouble *wz)
{
    GLdouble in[4] = { ox, oy, oz, 1.0 }, e[4], c[4];
    xform(e, model, in);
    xform(c, proj, e);
    if (c[3] == 0.0) return GL_FALSE;
    *wx = vp[0] + (c[0] / c[3] * 0.5 + 0.5) * vp[2];
    *wy = vp[1] + (c[1] / c[3] * 0.5 + 0.5) * vp[3];
    *wz = c[2] / c[3] * 0.5 + 0.5;
    return GL_TRUE;
}

const GLubyte *gluErrorString(GLenum err) { (void)err; return (const GLubyte *)"GL error"; }
