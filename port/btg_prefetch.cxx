// Terrain read-ahead on the PPU's second hardware thread (PS3 port).
//
// FlightGear 0.9.10 loads one tile per frame on the main thread; most of
// that is reading, unpacking and parsing its .btg files (SGBinObject). When
// a tile is queued, this thread reads its .stg files and the .btg files they
// name into SGBinObjects, a few files ahead of the main thread, which then
// only builds the scene graph from them (sgBinObjLoad, simgear/scene/tgdb).
// Tiles are loaded in the order they are queued; read-ahead for tiles the
// main thread has passed is dropped.
//
// The thread touches no PLIB objects, no properties and no stdio (no
// SG_LOG: see ps3_main_thread); malloc is thread-safe in PSL1GHT's newlib.

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <sys/thread.h>
#include <sys/mutex.h>
#include <sys/cond.h>
#include <lv2/thread.h>
#include <simgear/io/sg_binobj.hxx>
#include <ps3_btg_prefetch.h>

#define AHEAD      6                    // .btg files read ahead at most
#define STACK      (256 * 1024)
#define PRIORITY   1500                 // below the main thread (1001)

struct Item { const void *tile; unsigned seq; std::vector<std::string> stgs; };
struct Ready { SGBinObject *obj; unsigned seq; };

static bool started;
static sys_mutex_t lock, read_lock;
static sys_cond_t changed;
static std::deque<Item> todo;
static std::map<std::string, Ready> ready;
static std::map<const void *, unsigned> seq_of;
static std::string busy;                // the file being read now
static unsigned next_seq = 1, busy_seq, min_seq = 1;
static volatile uintptr_t worker_lo, worker_hi;
static unsigned long hits, misses;

extern "C" int ps3_main_thread(void)
{
    char probe;
    uintptr_t sp = (uintptr_t)&probe;
    return !(sp >= worker_lo && sp < worker_hi);
}

// The OBJECT_BASE and OBJECT entries of an .stg file: "<dir>/<name>"
static void stg_files(const std::string &stg, std::vector<std::string> &out)
{
    std::string dir = stg.substr(0, stg.rfind('/'));
    std::string text;
    char buf[4096];
    int fd = open(stg.c_str(), O_RDONLY), n;
    if (fd < 0) return;
    while ((n = read(fd, buf, sizeof buf)) > 0) text.append(buf, n);
    close(fd);
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(p, e - p);
        p = e + 1;
        // "TOKEN name ..." (no sscanf: newlib's scanf uses the thread's _REENT)
        size_t a = line.find_first_not_of(" \t\r");
        if (a == std::string::npos) continue;
        size_t b = line.find_first_of(" \t\r", a);
        if (b == std::string::npos) continue;
        std::string tok = line.substr(a, b - a);
        size_t c = line.find_first_not_of(" \t\r", b);
        if (c == std::string::npos) continue;
        size_t d = line.find_first_of(" \t\r", c);
        std::string name = line.substr(c, d == std::string::npos ? std::string::npos : d - c);
        if (tok == "OBJECT_BASE" || tok == "OBJECT")
            out.push_back(dir + "/" + name);
    }
}

static void worker(void *arg)
{
    char top;
    (void)arg;
    worker_hi = (uintptr_t)&top + 4096;
    worker_lo = worker_hi - STACK - 8192;
    sysMutexLock(lock, 0);
    for (;;) {
        while (todo.empty() || ready.size() >= AHEAD)
            sysCondWait(changed, 0);
        Item it = todo.front();
        todo.pop_front();
        for (size_t s = 0; s < it.stgs.size(); s++) {
            std::vector<std::string> files;
            sysMutexUnlock(lock);
            stg_files(it.stgs[s], files);
            sysMutexLock(lock, 0);
            for (size_t f = 0; f < files.size() && it.seq >= min_seq; f++) {
                while (ready.size() >= AHEAD && it.seq >= min_seq)
                    sysCondWait(changed, 0);
                if (it.seq < min_seq) break;
                busy = files[f];
                busy_seq = it.seq;
                sysMutexUnlock(lock);
                SGBinObject *obj = new SGBinObject;
                sysMutexLock(read_lock, 0);
                bool ok = obj->read_bin(files[f]);
                sysMutexUnlock(read_lock);
                sysMutexLock(lock, 0);
                busy.clear();
                if (ok && it.seq >= min_seq && !ready.count(files[f])) {
                    Ready r = { obj, it.seq };
                    ready[files[f]] = r;
                } else {
                    delete obj;
                }
                sysCondBroadcast(changed);
            }
        }
    }
}

static void start()
{
    sys_mutex_attr_t ma;
    sys_cond_attr_t ca;
    sys_ppu_thread_t id;
    started = true;
    sysMutexAttrInitialize(ma);
    sysMutexCreate(&lock, &ma);
    sysMutexCreate(&read_lock, &ma);
    sysCondAttrInitialize(ca);
    sysCondCreate(&changed, lock, &ca);
    sysThreadCreate(&id, worker, 0, PRIORITY, STACK, 0, (char *)"btg-read-ahead");
}

void btg_prefetch_tile(const void *tile, const std::vector<std::string> &stg_files)
{
    if (!started) start();
    sysMutexLock(lock, 0);
    Item it;
    it.tile = tile;
    it.seq = next_seq++;
    it.stgs = stg_files;
    seq_of[tile] = it.seq;
    todo.push_back(it);
    sysCondBroadcast(changed);
    sysMutexUnlock(lock);
}

void btg_begin_tile(const void *tile)
{
    if (!started) return;
    sysMutexLock(lock, 0);
    std::map<const void *, unsigned>::iterator s = seq_of.find(tile);
    if (s != seq_of.end()) {
        unsigned seq = s->second;
        seq_of.erase(s);
        min_seq = seq;          // read-ahead for earlier tiles is stale now
        while (!todo.empty() && todo.front().seq <= seq) {
            if (todo.front().seq < seq) seq_of.erase(todo.front().tile);
            todo.pop_front();   // this tile itself, not started yet: the main thread reads it
        }
        for (std::map<std::string, Ready>::iterator r = ready.begin(); r != ready.end(); ) {
            if (r->second.seq < seq) {
                delete r->second.obj;
                ready.erase(r++);
            } else {
                ++r;
            }
        }
        sysCondBroadcast(changed);
    }
    sysMutexUnlock(lock);
}

SGBinObject *btg_take(const std::string &path)
{
    SGBinObject *obj = 0;
    if (!started) return 0;
    sysMutexLock(lock, 0);
    while (busy == path)
        sysCondWait(changed, 0);
    std::map<std::string, Ready>::iterator r = ready.find(path);
    if (r != ready.end()) {
        obj = r->second.obj;
        ready.erase(r);
        sysCondBroadcast(changed);
        hits++;
    } else {
        misses++;
    }
    sysMutexUnlock(lock);
    return obj;
}

bool btg_read(SGBinObject &obj, const std::string &path)
{
    if (!started) return obj.read_bin(path);
    sysMutexLock(read_lock, 0);
    bool ok = obj.read_bin(path);
    sysMutexUnlock(read_lock);
    return ok;
}

void btg_report(char *buf, int n)
{
    snprintf(buf, n, "terrain read-ahead: %lu files ready in time, %lu read by the main thread", hits, misses);
}
