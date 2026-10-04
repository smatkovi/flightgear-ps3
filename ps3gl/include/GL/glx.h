/* ps3gl: just enough GLX for code that asks "is there a current context"
   and for pbuffer-typed declarations to compile. There are no pbuffers. */
#ifndef PS3GL_GLX_H
#define PS3GL_GLX_H
#include <X11/Xlib.h>
#include <GL/gl.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef void *GLXContext;
typedef void *GLXFBConfig;
typedef XID GLXPbuffer;
typedef XID GLXDrawable;
GLXContext glXGetCurrentContext(void);
#ifdef __cplusplus
}
#endif
#endif
