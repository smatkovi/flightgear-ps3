/* The hangar: a start screen before FlightGear, to pick the aircraft and the
   airport, download more aircraft, and see why the last start failed. */
#ifndef HANGAR_H
#define HANGAR_H

/* Show the hangar. On return argv holds the chosen --aircraft, --airport-id
   and --runway (replacing any from fgfs.args), and the start is recorded so
   that a crash or hang during loading can be reported next time. */
void hangar_run(int *argc, char **argv, int max_args);

/* The simulator is up: the aircraft loaded. warning: problems found in the
   log (empty if none), shown in the hangar next time. */
void hangar_mark_running(const char *warning);
/* Antialiasing or 3D ran long enough without freezing the console: keep it. */
void hangar_confirm_graphics();
/* Normal quit (XMB). */
void hangar_mark_quit();
/* FlightGear stopped with an error while starting. */
void hangar_mark_failed(int code);
/* True between hangar_run() and hangar_mark_running(). */
bool hangar_starting();
/* The START menu during the flight, drawn over the scene */
void hangar_menu_draw(const char *title, const char **items, int n, int sel);
/* 3D on/off at once (video mode changes, no restart) */
void hangar_toggle_stereo();
/* 3D settings (GT5's): parallax 1-10, convergence 0.00-1.00; set saves them */
void hangar_stereo_get(int *parallax, float *convergence);
void hangar_stereo_set(int parallax, float convergence);
/* Restart the program into the hangar; does not return. */
void hangar_restart();

#endif
