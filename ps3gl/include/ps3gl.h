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
/* Stereoscopic 3D (720p frame packing) if the display takes it; only before
   the first ps3glInit(). Antialiasing is off then. */
void ps3glSetStereo(int on);
/* True when the display runs in 3D: draw each frame twice, ps3glSetEye(0)
   (left) and ps3glSetEye(1) (right); a frame drawn once shows on both eyes. */
int ps3glStereo(void);
void ps3glSetEye(int eye);
/* separation: shift of each eye's picture at infinity, in screen halves
   (0.05 = 2.5% of the width); convergence: distance in eye-space units at
   which both pictures meet (the screen plane) */
void ps3glSetStereoParams(float separation, float convergence);
/* called when the RSX stops answering while antialiasing or 3D is on */
void ps3glSetHangHandler(void (*f)(void));
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
