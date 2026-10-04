// fg_os_ps3.cxx -- FlightGear's window/OS layer for the PS3 (PSL1GHT + ps3gl).
//
// There is one fixed-size "window" (the display), no mouse and no keyboard
// yet; input comes in through the PLIB joystick driver (port/jsPS3.cxx).
// The process entry point is here: it sets up the environment FlightGear
// expects (argv, $HOME, a log file), shows the hangar (port/hangar: aircraft
// and airport choice, downloads) and runs FlightGear's own main(). If
// FlightGear stops with an error while starting, the program restarts into
// the hangar, which then shows the error.
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
#include <fcntl.h>
#include <malloc.h>
#include <sys/time.h>
#include <math.h>

#include <string>
#include <algorithm>

#include <sys/process.h>
#include <sysutil/sysutil.h>

#include <plib/pu.h>

#include <ps3gl.h>
#include "../ps3pad.h"
#include "../ps3_debug.h"
#include "../hangar/hangar.h"

#include <Main/fg_os.hxx>
#include <Main/fg_props.hxx>

#define PS3_USRDIR      "/dev_hdd0/game/FGFS00910/USRDIR"
#define PS3_MAX_ARGS    64

SYS_PROCESS_PARAM(1001, 0x100000);

extern int fgfs_main(int argc, char **argv);    // bootstrap.cxx
extern double delta_time_sec;                   // main.cxx, frame time of the animations

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

static void stereo_apply();

void fgOSOpenWindow(int w, int h, int bpp, bool alpha, bool stencil, bool fullscreen)
{
    ps3glInit();
    ps3glGetSize(&WinW, &WinH);
    stereo_apply();

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

extern "C" void al_ps3_report(char *buf, int n);     // port/al_ps3.c
#include <ps3_btg_prefetch.h>
// Main thread time for terrain files (simgear/scene/tgdb/obj.cxx): reading
// (or taking them from the read-ahead thread) and building the scene graph
extern "C" { unsigned long long tl_read, tl_build, tl_max; unsigned long tl_n; }

static unsigned long long now_us()
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return (unsigned long long)tv.tv_sec * 1000000ULL + tv.tv_usec;
}

// PPU time per frame (simulation and drawing, without waiting for the
// display), to see how much of the 16.7 ms of a 60 Hz frame is left
static unsigned long long work_sum, work_max, period_start;
static unsigned long work_frames;

static void heartbeat(unsigned long frame)
{
    char st[1024];
    struct mallinfo mi = mallinfo();
    unsigned long long t = now_us();
    ps3glStats(st, sizeof st);
    ps3glLog("frame %lu: heap %luK in use, %luK from the system; %s", frame,
             (unsigned long)mi.uordblks >> 10, (unsigned long)mi.arena >> 10, st);
    if (work_frames && period_start)
        ps3glLog("ppu: %.1f ms per frame on average, at most %.1f ms; %.1f frames/s",
                 work_sum / 1000.0 / work_frames, work_max / 1000.0,
                 work_frames * 1e6 / (double)(t - period_start));
    work_sum = work_max = 0;
    work_frames = 0;
    period_start = t;
    ps3pad_report(st, sizeof st);
    ps3glLog("%s", st);
    al_ps3_report(st, sizeof st);
    ps3glLog("%s", st);
    btg_report(st, sizeof st);
    ps3glLog("%s", st);
    ps3glLog("terrain on the main thread: read %.1f ms, build %.1f ms, at most %.1f ms (%lu files)",
             tl_read / 1000.0, tl_build / 1000.0, tl_max / 1000.0, tl_n);
    tl_read = tl_build = tl_max = 0;
    tl_n = 0;
}

// What went wrong while the aircraft loaded, judging by the log (empty if nothing).
static std::string aircraft_problems()
{
    std::string log, w;
    int fd = open(PS3_USRDIR "/fgfs.log", O_RDONLY);
    char buf[16384];
    int n;
    if (fd < 0) return w;
    while ((n = read(fd, buf, sizeof buf)) > 0) log.append(buf, n);
    close(fd);
    // the aircraft is loaded before the scenery; scenery models have their
    // own problems (newer Nasal, missing effects) that are not the aircraft's
    size_t tiles = log.find("Loading tile ");
    if (tiles != std::string::npos) log.resize(tiles);
    // "pick" animations (clickable cockpits) need a mouse anyway
    bool anim = false;
    for (size_t p = 0; !anim && (p = log.find("Unknown animation type ", p)) != std::string::npos; p++)
        anim = log.compare(p + 23, 4, "pick") != 0;
    if (log.find("Failed to load aircraft from") != std::string::npos)
        w = "its 3D model could not be loaded (it was made for a newer FlightGear)";
    else if (log.find("Failed to load submodel") != std::string::npos || anim)
        w = "parts of its 3D model are missing or do not move";
    if (log.find("Nasal runtime error") != std::string::npos ||
        log.find("Nasal parse error") != std::string::npos)
        w += std::string(w.empty() ? "" : "; ") + "some of its scripts fail";
    return w;
}

// The pressure of L2/R2 for the throttle bindings (Nasal/ps3controls.nas),
// and the rumble that Nasal/ps3rumble.nas asks for
static void pad_properties()
{
    static SGPropertyNode *l2, *r2, *large, *small;
    const ps3pad_state *st = ps3pad_get();
    if (!l2) {
        l2 = fgGetNode("/input/ps3/pressure-l2", true);
        r2 = fgGetNode("/input/ps3/pressure-r2", true);
        large = fgGetNode("/input/ps3/rumble-large", true);
        small = fgGetNode("/input/ps3/rumble-small", true);
    }
    l2->setDoubleValue(st->axis[PS3PAD_L2]);
    r2->setDoubleValue(st->axis[PS3PAD_R2]);
    ps3pad_rumble(large->getFloatValue(), small->getBoolValue());
}

// START: pause and a menu over the scene
static bool menu_open;
static int menu_sel;
static unsigned menu_prev;
enum { M_RESUME, M_RUMBLE, M_STEREO, M_PARALLAX, M_CONVERGENCE, M_HANGAR, M_QUIT };

// the menu's entries: the 3D settings only when the display runs in 3D
static int menu_items(int *ids)
{
    int n = 0;
    ids[n++] = M_RESUME;
    ids[n++] = M_RUMBLE;
    ids[n++] = M_STEREO;
    if (ps3glStereo()) { ids[n++] = M_PARALLAX; ids[n++] = M_CONVERGENCE; }
    ids[n++] = M_HANGAR;
    ids[n++] = M_QUIT;
    return n;
}

// 3D strength as in GT5: parallax 1-10 is the shift of each eye's picture at
// infinity (level 10: 5.5% of the screen width between the eyes, about the
// distance of the eyes on a large TV); convergence 0.00-1.00 puts the screen
// plane from 1 m (cockpit panel) to 50 m in front of the eyes
static void stereo_apply()
{
    int p;
    float c;
    hangar_stereo_get(&p, &c);
    ps3glSetStereoParams(p * 0.0055f, powf(50.0f, c));
}

static void menu_input()
{
    unsigned b = ps3pad_get()->buttons, edge = b & ~menu_prev;
    menu_prev = b;
    if (!menu_open) {
        if (edge & (1u << PS3PAD_START)) {
            menu_open = true;
            menu_sel = 0;
            fgSetBool("/sim/freeze/master", true);
            fgSetBool("/sim/freeze/clock", true);
            ps3pad_mute(1);
            ps3pad_rumble(0.0f, 0);
        }
        return;
    }
    int ids[7], n = menu_items(ids);
    if (edge & (1u << PS3PAD_UP)) menu_sel = (menu_sel + n - 1) % n;
    if (edge & (1u << PS3PAD_DOWN)) menu_sel = (menu_sel + 1) % n;
    int dir = (edge & (1u << PS3PAD_RIGHT)) ? 1 : (edge & (1u << PS3PAD_LEFT)) ? -1 : 0;
    if (dir && (ids[menu_sel] == M_PARALLAX || ids[menu_sel] == M_CONVERGENCE)) {
        int p;
        float c;
        hangar_stereo_get(&p, &c);
        if (ids[menu_sel] == M_PARALLAX) p = std::max(1, std::min(10, p + dir));
        else c = std::max(0.0f, std::min(1.0f, floorf(c * 20.0f + 0.5f) / 20.0f + dir * 0.05f));
        hangar_stereo_set(p, c);
        stereo_apply();
    }
    bool resume = edge & ((1u << PS3PAD_START) | (1u << PS3PAD_CIRCLE));
    if (edge & (1u << PS3PAD_CROSS)) {
        switch (ids[menu_sel]) {
        case M_RESUME: resume = true; break;
        case M_RUMBLE: fgSetBool("/input/ps3/rumble", !fgGetBool("/input/ps3/rumble", true)); break;
        case M_STEREO:
            hangar_toggle_stereo();
            stereo_apply();
            if (!ps3glStereo() && menu_sel >= menu_items(ids)) menu_sel = 0;
            break;
        case M_HANGAR: hangar_mark_quit(); hangar_restart(); break;
        case M_QUIT: hangar_mark_quit(); exit(0); break;
        }
    }
    if (resume) {
        menu_open = false;
        fgSetBool("/sim/freeze/master", false);
        fgSetBool("/sim/freeze/clock", false);
        ps3pad_mute(0);
    }
}

static void menu_draw()
{
    int ids[7], n = menu_items(ids), p;
    float c;
    char par[64], conv[64];
    const char *items[7];
    hangar_stereo_get(&p, &c);
    snprintf(par, sizeof par, "3D parallax: < %d >", p);
    snprintf(conv, sizeof conv, "3D convergence: < %.2f >", c);
    for (int i = 0; i < n; i++) {
        switch (ids[i]) {
        case M_RESUME: items[i] = "Resume"; break;
        case M_RUMBLE: items[i] = fgGetBool("/input/ps3/rumble", true) ? "Rumble: on" : "Rumble: off"; break;
        case M_STEREO: items[i] = ps3glStereo() ? "3D: on" : "3D: off"; break;
        case M_PARALLAX: items[i] = par; break;
        case M_CONVERGENCE: items[i] = conv; break;
        case M_HANGAR: items[i] = "Back to the hangar"; break;
        default: items[i] = "Quit FlightGear"; break;
        }
    }
    hangar_menu_draw("Paused", items, n, menu_sel);
}

void fgOSMainLoop()
{
    unsigned long frame = 0;
    bool running = false;
    unsigned long long run_since = 0;
    while (1) {
        if (!running && fgGetBool("/sim/sceneryloaded")) {
            std::string w = aircraft_problems();
            running = true;
            hangar_mark_running(w.c_str());
            run_since = now_us();
            if (!w.empty())     // shown by Nasal/ps3hangar.nas
                fgSetString("/sim/ps3/aircraft-warning", ("This aircraft has problems: " + w + ".").c_str());
        }
#ifdef PS3_DEBUG
        ps3_syscalls_check("loop");
#endif
        unsigned long long t0 = now_us(), work;
        ps3pad_poll();
        if (running) menu_input();
        if (running && run_since && now_us() - run_since > 120000000ULL) {
            hangar_confirm_graphics();      // two minutes without freezing
            run_since = 0;
        }
        pad_properties();
        if (IdleHandler) (*IdleHandler)();
        if (NeedRedraw && DrawHandler) {
            // in 3D the scene twice, once for each eye; the second time
            // without advancing the animations (sky, panel, controls)
            int eyes = ps3glStereo() ? 2 : 1;
            for (int eye = 0; eye < eyes; eye++) {
                double dt = delta_time_sec;
                if (eyes == 2) ps3glSetEye(eye);
                if (eye) delta_time_sec = 0.0;
                (*DrawHandler)();
                delta_time_sec = dt;
                if (menu_open) menu_draw();
            }
            work = now_us() - t0;
            work_sum += work;
            if (work > work_max) work_max = work;
            work_frames++;
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
    if (status == SYSUTIL_EXIT_GAME) {
        ps3pad_rumble(0.0f, 0);
        hangar_mark_quit();
        exit(0);
    }
}

// FlightGear ends fatal errors with exit(nonzero). While an aircraft is still
// loading, go back to the hangar instead, which shows the error.
// (Linked with -Wl,--wrap=exit.)
extern "C" void __real_exit(int code) __attribute__((noreturn));
extern "C" void __wrap_exit(int code)
{
    if (code != 0 && hangar_starting()) {
        hangar_mark_failed(code);
        hangar_restart();
    }
    __real_exit(code);
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
    // be created, leave the streams alone rather than lose them. The previous
    // log is kept for the hangar's error report.
    rename(PS3_USRDIR "/fgfs.log", PS3_USRDIR "/fgfs.prev.log");
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

    hangar_run(&ps3_argc, ps3_argv, PS3_MAX_ARGS);
    int rc = fgfs_main(ps3_argc, ps3_argv);
    if (rc != 0 && hangar_starting()) {
        hangar_mark_failed(rc);
        hangar_restart();
    }
    return rc;
}
