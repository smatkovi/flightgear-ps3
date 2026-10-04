/* ps3gl: OpenGL 1.x fixed-function subset on top of the PS3's RSX (PSL1GHT librsx).
   Window-system side of the library; the GL calls themselves are in <GL/gl.h>. */
#ifndef PS3GL_H
#define PS3GL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Set the video mode, bring up the RSX and load the fixed-function shaders. */
/* Antialiasing (default on); only before the first ps3glInit() */
void ps3glSetAntialiasing(int on);
void ps3glInit(void);
/* Present the frame and start the next one. */
void ps3glSwapBuffers(void);
/* Size of the display in pixels. */
void ps3glGetSize(int *w, int *h);
/* One line of RSX memory and draw statistics. */
void ps3glStats(char *buf, int len);
/* Append a line to the debug log (USRDIR/ps3gl.log when it can be opened). */
void ps3glLog(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif
