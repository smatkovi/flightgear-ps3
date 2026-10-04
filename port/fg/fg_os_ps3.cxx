// fg_os_ps3.cxx -- FlightGear's window/OS layer for the PS3 (PSL1GHT + ps3gl).
//
// There is one fixed-size "window" (the display), no mouse and no keyboard
// yet; input comes in through the PLIB joystick driver (port/jsPS3.cxx).
// The process entry point is here: it sets up the environment FlightGear
// expects (argv, $HOME, a log file) and runs FlightGear's own main().
//
// FlightGear runs on the primary thread. Its stack is capped at 1 MB, which is
// also what FlightGear 0.9.10 had on Windows. A second thread with a bigger
// stack does not work: newlib keeps stdout per thread, and the first printf
// from another thread crashes in libsysbase's stream lock.

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>

#include <sys/process.h>
#include <sysutil/sysutil.h>

#include <plib/pu.h>

#include <ps3gl.h>
#include "../ps3pad.h"
#include "../ps3_debug.h"

#include <Main/fg_os.hxx>
#include <Main/fg_props.hxx>

#define PS3_USRDIR      "/dev_hdd0/game/FGFS00910/USRDIR"
#define PS3_MAX_ARGS    64

SYS_PROCESS_PARAM(1001, 0x100000);

extern int fgfs_main(int argc, char **argv);    // bootstrap.cxx

static fgIdleHandler IdleHandler = 0;
static fgDrawHandler DrawHandler = 0;
static fgWindowResizeHandler WindowResizeHandler = 0;
static fgKeyHandler KeyHandler = 0;
static fgMouseClickHandler MouseClickHandler = 0;
static fgMouseMotionHandler MouseMotionHandler = 0;

static int CurrentMouseCursor = MOUSE_CURSOR_POINTER;
static bool NeedRedraw = false;
static int WinW = 0, WinH = 0;

void fgRegisterIdleHandler(fgIdleHandler func) { IdleHandler = func; }
void fgRegisterDrawHandler(fgDrawHandler func) { DrawHandler = func; NeedRedraw = true; }
void fgRegisterWindowResizeHandler(fgWindowResizeHandler func) { WindowResizeHandler = func; }
void fgRegisterKeyHandler(fgKeyHandler func) { KeyHandler = func; }
void fgRegisterMouseClickHandler(fgMouseClickHandler func) { MouseClickHandler = func; }
void fgRegisterMouseMotionHandler(fgMouseMotionHandler func) { MouseMotionHandler = func; }

void fgOSInit(int* argc, char** argv) {}

void fgOSOpenWindow(int w, int h, int bpp, bool alpha, bool stencil, bool fullscreen)
{
    ps3glInit();
    ps3glGetSize(&WinW, &WinH);

    // The display decides the size, not the options.
    fgSetInt("/sim/startup/xsize", WinW);
    fgSetInt("/sim/startup/ysize", WinH);
    if (WindowResizeHandler)
        (*WindowResizeHandler)(WinW, WinH);
}

void fgOSFullScreen() {}

void fgOSExit(int code)
{
    exit(code);
}

// One line of memory and draw statistics every HEARTBEAT frames, to the log
// file: on the console it is the only view of what the program is doing.
#define HEARTBEAT 600

static void heartbeat(unsigned long frame)
{
    char st[160];
    struct mallinfo mi = mallinfo();
    ps3glStats(st, sizeof st);
    ps3glLog("frame %lu: heap %luK in use, %luK from the system; %s", frame,
             (unsigned long)mi.uordblks >> 10, (unsigned long)mi.arena >> 10, st);
}

void fgOSMainLoop()
{
    unsigned long frame = 0;
    while (1) {
#ifdef PS3_DEBUG
        ps3_syscalls_check("loop");
#endif
        ps3pad_poll();
        if (IdleHandler) (*IdleHandler)();
        if (NeedRedraw && DrawHandler) {
            (*DrawHandler)();
            ps3glSwapBuffers();     // also runs the system callbacks (XMB quit)
            NeedRedraw = false;
            if (frame++ % HEARTBEAT == 0) heartbeat(frame - 1);
        } else {
            sysUtilCheckCallback();
            usleep(1000);
        }
    }
}

void fgRequestRedraw() { NeedRedraw = true; }
int fgGetKeyModifiers() { return KEYMOD_NONE; }
void fgWarpMouse(int x, int y) {}
void fgSetMouseCursor(int cursor) { CurrentMouseCursor = cursor; }
int fgGetMouseCursor() { return CurrentMouseCursor; }

// PUI without a window toolkit: one window, the display.
static int puGetWindowPS3() { return 1; }
static void puGetWindowSizePS3(int *w, int *h) { *w = WinW; *h = WinH; }

void fgOSPuInit()
{
    puSetWindowFuncs(puGetWindowPS3, 0, puGetWindowSizePS3, 0);
    puRealInit();
}

//
// Process entry
//

static void sysutil_callback(u64 status, u64 param, void *usrdata)
{
    if (status == SYSUTIL_EXIT_GAME)
        exit(0);
}

static int ps3_argc;
static char *ps3_argv[PS3_MAX_ARGS];

// One option per line in USRDIR/fgfs.args, e.g. --aircraft=c172p ('#' starts a comment).
static void read_args_file()
{
    char line[512];
    FILE *f = fopen(PS3_USRDIR "/fgfs.args", "r");
    if (!f) return;
    while (ps3_argc < PS3_MAX_ARGS - 1 && fgets(line, sizeof line, f)) {
        char *end = line + strlen(line);
        while (end > line && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) *--end = 0;
        if (line[0] == 0 || line[0] == '#') continue;
        ps3_argv[ps3_argc++] = strdup(line);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
#ifdef PS3_DEBUG
    ps3_syscalls_save();
#endif

    // stdout/stderr go nowhere on a console; keep them in a file. Both streams
    // append, so neither overwrites what the other wrote. If the file cannot
    // be created, leave the streams alone rather than lose them.
    FILE *log = fopen(PS3_USRDIR "/fgfs.log", "w");
    if (log) {
        fclose(log);
        if (freopen(PS3_USRDIR "/fgfs.log", "a", stdout))
            setvbuf(stdout, 0, _IOLBF, BUFSIZ);
        if (freopen(PS3_USRDIR "/fgfs.log", "a", stderr))
            setvbuf(stderr, 0, _IONBF, 0);
    }

    setenv("HOME", PS3_USRDIR, 1);
    sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sysutil_callback, 0);

    ps3_argv[ps3_argc++] = strdup("fgfs");
    ps3_argv[ps3_argc++] = strdup("--fg-root=" PS3_USRDIR "/fgdata");
    read_args_file();
    ps3_argv[ps3_argc] = 0;

    return fgfs_main(ps3_argc, ps3_argv);
}
