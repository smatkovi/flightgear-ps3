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
/* Normal quit (XMB). */
void hangar_mark_quit();
/* FlightGear stopped with an error while starting. */
void hangar_mark_failed(int code);
/* True between hangar_run() and hangar_mark_running(). */
bool hangar_starting();
/* Restart the program into the hangar; does not return. */
void hangar_restart();

#endif
