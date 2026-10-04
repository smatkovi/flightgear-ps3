/* PS3 port: terrain files are read ahead on the PPU's second hardware
   thread (port/btg_prefetch.cxx). The thread only reads and parses them
   into SGBinObjects; the scene graph (PLIB, not thread-safe) is still built
   by the main thread from those. */
#ifndef PS3_BTG_PREFETCH_H
#define PS3_BTG_PREFETCH_H

#ifdef __cplusplus
#include <string>
#include <vector>

class SGBinObject;

/* A tile was queued for loading: read the .btg files its .stg files name. */
void btg_prefetch_tile(const void *tile, const std::vector<std::string> &stg_files);
/* The main thread starts loading this tile: older read-ahead is dropped. */
void btg_begin_tile(const void *tile);
/* The terrain object read ahead for this file, or NULL (then read it with
   btg_read). Waits if the file is being read just now. Caller deletes it. */
SGBinObject *btg_take(const std::string &path);
/* SGBinObject::read_bin, not at the same time as the read-ahead thread
   (simgear/io/lowlevel keeps its error flag in a static) */
bool btg_read(SGBinObject &obj, const std::string &path);
/* hits and misses, for the log */
void btg_report(char *buf, int n);

extern "C" {
#endif

/* False on the read-ahead thread: it must not write to the log (stdio and
   FlightGear's log stream are the main thread's). */
int ps3_main_thread(void);

#ifdef __cplusplus
}
#endif
#endif
