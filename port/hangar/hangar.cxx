// hangar.cxx -- the start screen of the PS3 port: choose the aircraft and the
// airport, download aircraft from the FlightGear archive, and report why the
// previous start failed.
//
// FlightGear 0.9.10 has no menu for any of this (aircraft and airport are
// command line options), and there is no mouse anyway. The hangar runs before
// FlightGear is initialised, draws with ps3gl and PLIB's fnt and is driven by
// the controller. Downloads come from the FlightGear archive of aircraft made
// for versions 1.0 to 1.9.1 (2005-2008); many work in 0.9.10, some partly,
// some not. A start that ends in an error, a crash or a hang is recorded and
// shown here the next time.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>

#include <string>
#include <vector>
#include <map>
#include <algorithm>

#include <GL/gl.h>
#include <GL/glu.h>
#include <plib/fnt.h>

#include <lv2/process.h>
#include <sys/process.h>

#include <ps3gl.h>
#include "../ps3pad.h"
#include "http.h"
#include "unzip.h"
#include "hangar.h"

using std::string;
using std::vector;

#define USRDIR      "/dev_hdd0/game/FGFS00910/USRDIR"
#define FGROOT      USRDIR "/fgdata"
#define AIRCRAFT    FGROOT "/Aircraft"
#define CATALOG     FGROOT "/Hangar/catalog.txt"
#define STATUS_FILE USRDIR "/hangar.status"
#define CFG_FILE    USRDIR "/hangar.cfg"
#define NOTES_FILE  USRDIR "/hangar.notes"
#define LOG_FILE    USRDIR "/fgfs.prev.log"    /* log of the previous run */
#define DL_DIR      USRDIR "/download"
#define MIRROR      "http://mirrors.ibiblio.org/flightgear/ftp/Archive/Version-1.x/Aircraft-1.9.1/"

struct Airport { const char *icao, *rwy, *name; };
static const Airport airports[] = {
    { "LOWW", "29",  "Wien-Schwechat" },
    { "LOWW", "11",  "Wien-Schwechat" },
    { "LOWW", "16",  "Wien-Schwechat" },
    { "LOWW", "34",  "Wien-Schwechat" },
    { "KSFO", "28R", "San Francisco Intl" },
    { "KSFO", "10L", "San Francisco Intl" },
    { "KOAK", "29",  "Oakland Intl" },
    /* more of the World Scenery 2.12 block 10-20 E / 40-50 N */
    { "LOWS", "16",  "Salzburg" },
    { "LOWI", "08",  "Innsbruck" },
    { "LOWG", "17C", "Graz" },
    { "LOWL", "09",  "Linz" },
    { "LOWK", "10L", "Klagenfurt" },
    { "EDDM", "08R", "Muenchen" },
    { "EDDN", "10",  "Nuernberg" },
    { "LZIB", "13",  "Bratislava" },
    { "LKTB", "10",  "Brno-Turany" },
    { "LHBP", "13L", "Budapest Ferihegy" },
    { "LJLJ", "13",  "Ljubljana" },
    { "LDZA", "05",  "Zagreb" },
    { "LDSP", "05",  "Split" },
    { "LIPZ", "04R", "Venezia Tessera" },
    { "LIPE", "12",  "Bologna" },
    { "LIRF", "16R", "Roma Fiumicino" },
};
static const int n_airports = sizeof airports / sizeof airports[0];

// Aircraft of the base package. The archive has newer versions of some of
// them; those are not offered, they would replace a version known to work.
static const char *base_dirs[] = {
    "737-300", "A-10", "bf109", "bo105", "c172", "c172p", "c310", "c310u3a",
    "Citation-Bravo", "f16", "Hunter", "j3cub", "p51d", "pa28-161", "Rascal",
    "T38", "ufo", "UIUC", "wrightFlyer1903", "Generic", "Instruments",
    "Instruments-3d", 0
};

struct Entry {
    string set;         // name for --aircraft (installed), or the first one in the archive
    string title, desc, dir, zip, fdm;
    long size;          // download size
    bool installed, base;
    Entry() : size(0), installed(false), base(false) {}
};

static vector<Entry> entries;               // installed first, then downloadable
static std::map<string, string> notes;      // set -> problem seen at the last start
static string g_set;                        // aircraft of the current start
static bool g_starting;
static bool g_batch;            /* --no-hangar: no hangar to go back to */

// ---------------------------------------------------------------- files

static string read_file(const char *path, size_t max)
{
    string s;
    int fd = open(path, O_RDONLY);
    char buf[4096];
    int n;
    if (fd < 0) return s;
    while (s.size() < max && (n = read(fd, buf, sizeof buf)) > 0) s.append(buf, n);
    close(fd);
    return s;
}

static string read_tail(const char *path, long n)
{
    string s;
    int fd = open(path, O_RDONLY);
    long size, got;
    if (fd < 0) return s;
    size = lseek(fd, 0, SEEK_END);
    lseek(fd, size > n ? size - n : 0, SEEK_SET);
    s.resize(size > n ? n : size);
    got = s.empty() ? 0 : read(fd, &s[0], s.size());
    close(fd);
    s.resize(got > 0 ? got : 0);
    return s;
}

static void write_file(const char *path, const string &s)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    if (!s.empty()) write(fd, s.data(), s.size());
    close(fd);
}

static vector<string> split(const string &s, char sep)
{
    vector<string> v;
    size_t a = 0, b;
    while ((b = s.find(sep, a)) != string::npos) { v.push_back(s.substr(a, b - a)); a = b + 1; }
    v.push_back(s.substr(a));
    return v;
}

static string trim(const string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == string::npos ? string() : s.substr(a, b - a + 1);
}

static string lower(string s)
{
    for (size_t i = 0; i < s.size(); i++) s[i] = tolower((unsigned char)s[i]);
    return s;
}

static bool icase_less(const Entry &a, const Entry &b)
{
    if (a.installed != b.installed) return a.installed;
    return strcasecmp(a.title.c_str(), b.title.c_str()) < 0;
}

static void load_notes()
{
    vector<string> lines = split(read_file(NOTES_FILE, 65536), '\n');
    notes.clear();
    for (size_t i = 0; i < lines.size(); i++) {
        size_t t = lines[i].find('\t');
        if (t != string::npos) notes[lines[i].substr(0, t)] = lines[i].substr(t + 1);
    }
}

static void save_notes()
{
    string s;
    for (std::map<string, string>::iterator i = notes.begin(); i != notes.end(); ++i)
        s += i->first + "\t" + i->second + "\n";
    write_file(NOTES_FILE, s);
}

static void set_note(const string &set, const string &text)
{
    load_notes();
    if (text.empty()) notes.erase(set);
    else notes[set] = text;
    save_notes();
}

// ---------------------------------------------------------------- aircraft lists

// Text of <tag> or <tag attr...> in an XML file, whitespace collapsed, ASCII only.
static string xml_field(const string &xml, const char *tag)
{
    string open = string("<") + tag, close = string("</") + tag + ">", out;
    size_t a = 0, b;
    for (;;) {
        a = xml.find(open, a);
        if (a == string::npos) return "";
        a += open.size();
        if (a < xml.size() && (xml[a] == '>' || xml[a] == ' ')) break;
    }
    a = xml.find('>', a);
    if (a == string::npos) return "";
    b = xml.find(close, ++a);
    if (b == string::npos) return "";
    for (size_t i = a; i < b; i++) {
        unsigned char c = xml[i];
        if (c == '&') {
            if (!xml.compare(i, 5, "&amp;")) { out += '&'; i += 4; continue; }
            if (!xml.compare(i, 4, "&lt;")) { out += '<'; i += 3; continue; }
            if (!xml.compare(i, 4, "&gt;")) { out += '>'; i += 3; continue; }
        }
        if (isspace(c)) { if (!out.empty() && out[out.size() - 1] != ' ') out += ' '; }
        else out += c < 128 ? (char)c : '?';
    }
    return trim(out);
}

static bool is_base(const string &dir)
{
    for (int i = 0; base_dirs[i]; i++) if (dir == base_dirs[i]) return true;
    return false;
}

static void scan_installed()
{
    DIR *d = opendir(AIRCRAFT);
    struct dirent *e;
    vector<string> dirs;
    if (!d) return;
    while ((e = readdir(d)) != NULL) if (e->d_name[0] != '.') dirs.push_back(e->d_name);
    closedir(d);
    for (size_t i = 0; i < dirs.size(); i++) {
        string path = string(AIRCRAFT "/") + dirs[i];
        DIR *sd = opendir(path.c_str());
        if (!sd) continue;
        while ((e = readdir(sd)) != NULL) {
            string f = e->d_name;
            if (f.size() <= 8 || f.compare(f.size() - 8, 8, "-set.xml") != 0) continue;
            Entry en;
            string xml = read_file((path + "/" + f).c_str(), 65536);
            en.set = en.title = f.substr(0, f.size() - 8);
            en.dir = dirs[i];
            en.desc = xml_field(xml, "description");
            en.fdm = xml_field(xml, "flight-model");
            en.installed = true;
            en.base = is_base(dirs[i]);
            entries.push_back(en);
        }
        closedir(sd);
    }
}

// catalog.txt: zip <tab> size <tab> dir <tab> sets <tab> fdm <tab> status <tab> description
static void load_catalog()
{
    vector<string> lines = split(read_file(CATALOG, 1 << 20), '\n');
    for (size_t i = 0; i < lines.size(); i++) {
        vector<string> f = split(lines[i], '\t');
        bool have = false;
        if (f.size() < 7 || f[0].empty() || f[0][0] == '#') continue;
        if (is_base(f[2])) continue;
        for (size_t k = 0; k < entries.size() && !have; k++)
            have = entries[k].installed && entries[k].dir == f[2];
        if (have) continue;
        Entry en;
        en.zip = f[0];
        en.size = atol(f[1].c_str());
        en.dir = f[2];
        en.set = split(f[3], ',')[0];
        en.fdm = f[4];
        en.desc = f[6];
        en.title = f[0].substr(0, f[0].rfind('_') != string::npos ? f[0].rfind('_') : f[0].size() - 4);
        entries.push_back(en);
    }
}

static void rebuild_lists()
{
    entries.clear();
    scan_installed();
    load_catalog();
    std::sort(entries.begin(), entries.end(), icase_less);
    load_notes();
}

static int find_set(const string &set)
{
    for (size_t i = 0; i < entries.size(); i++)
        if (entries[i].installed && entries[i].set == set) return (int)i;
    return -1;
}

// ---------------------------------------------------------------- the last start

// Lines from the end of fgfs.log that look like the reason for a failure.
static vector<string> log_errors(size_t max_lines)
{
    static const char *keys[] = { "error", "fatal", "failed", "exception", "unable", "can't",
                                  "cannot", "not found", "abort", "out of memory", 0 };
    static const char *noise[] = { "wav file", "audio", "sound", "data socket", "unknown.rgb",
                                   "rtt not available", "deleting", "ssgsgiheader", "untie", 0 };
    vector<string> lines = split(read_tail(LOG_FILE, 48 * 1024), '\n'), out;
    for (size_t i = 0; i < lines.size(); i++) {
        string l = trim(lines[i]), lc = lower(l);
        bool hit = false, bad = false;
        for (int k = 0; keys[k] && !hit; k++) hit = lc.find(keys[k]) != string::npos;
        for (int k = 0; noise[k] && !bad; k++) bad = lc.find(noise[k]) != string::npos;
        if (!hit || bad || l.empty()) continue;
        if (l.size() > 110) l = l.substr(0, 107) + "...";
        if (std::find(out.begin(), out.end(), l) == out.end()) out.push_back(l);
    }
    if (out.size() > max_lines) out.erase(out.begin(), out.end() - max_lines);
    return out;
}

static string msg_title;
static vector<string> msg_lines;

// Turn the status left by the previous run into a message, and remember the problem.
static void check_last_start()
{
    string st = trim(read_file(STATUS_FILE, 4096)), set, why;
    vector<string> w = split(st, ' ');
    write_file(STATUS_FILE, "idle\n");
    if (w.size() < 2) return;
    set = w[1];
    if (w[0] == "starting") {
        msg_title = "The last start did not finish";
        msg_lines.push_back("FlightGear crashed or hung while loading " + set + ".");
        msg_lines.push_back("This aircraft is probably not compatible with FlightGear 0.9.10.");
        why = "did not start last time (crash or hang)";
    } else if (w[0] == "quit-loading") {
        msg_title = "The last start was cancelled";
        msg_lines.push_back("FlightGear was closed while it was loading " + set + ".");
        msg_lines.push_back("If it hung, this aircraft is probably not compatible.");
        why = "start was cancelled while loading";
    } else if (w[0] == "failed") {
        msg_title = set + " could not be started";
        msg_lines.push_back("FlightGear stopped with an error while loading this aircraft.");
        msg_lines.push_back("It is probably not compatible with FlightGear 0.9.10.");
        why = "did not start: FlightGear stopped with an error";
    } else if (w[0] == "running") {
        msg_title = "The last flight did not end normally";
        msg_lines.push_back("FlightGear stopped while flying the " + set + ": it crashed or froze,");
        msg_lines.push_back("or the PS3 was switched off without quitting through the XMB.");
        why = "last flight ended abnormally (crash or freeze)";
    } else {
        return;
    }
    vector<string> errs = log_errors(6);
    if (!errs.empty()) {
        msg_lines.push_back("");
        msg_lines.push_back("From fgfs.log:");
        msg_lines.insert(msg_lines.end(), errs.begin(), errs.end());
    }
    set_note(set, why);
}

// ---------------------------------------------------------------- drawing

static int W, H;
static float S;                 // scale relative to 720 lines
static fntTexFont font;
static fntRenderer txt;
static bool font_ok;

static void frame_begin()
{
    glViewport(0, 0, W, H);
    glClearColor(0.07f, 0.11f, 0.20f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluOrtho2D(0, W, 0, H);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glDisable(GL_FOG);
    glDisable(GL_CULL_FACE);
    glDisable(GL_ALPHA_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

// x, y: top left, in 720-line units
static void rect(float x, float y, float w, float h, float r, float g, float b, float a)
{
    glDisable(GL_TEXTURE_2D);
    glColor4f(r, g, b, a);
    glBegin(GL_QUADS);
    glVertex2f(x * S, H - y * S);
    glVertex2f((x + w) * S, H - y * S);
    glVertex2f((x + w) * S, H - (y + h) * S);
    glVertex2f(x * S, H - (y + h) * S);
    glEnd();
}

// y: baseline, in 720-line units
static void text(float x, float y, float size, const string &s, float r, float g, float b, float a = 1)
{
    if (!font_ok || s.empty()) return;
    glEnable(GL_TEXTURE_2D);
    glColor4f(r, g, b, a);
    txt.setPointSize(size * S);
    txt.begin();
    txt.start2f(x * S, H - y * S);
    txt.puts(s.c_str());
    txt.end();
    glDisable(GL_TEXTURE_2D);
}

static float text_w(const string &s, float size)
{
    float l = 0, r = 0, b, t;
    if (!font_ok || s.empty()) return 0;
    font.getBBox(s.c_str(), size, 0, &l, &r, &b, &t);
    return r - l;
}

static vector<string> wrap(const string &s, float size, float width, size_t max_lines)
{
    vector<string> words = split(s, ' '), out;
    string line;
    for (size_t i = 0; i < words.size(); i++) {
        string t = line.empty() ? words[i] : line + " " + words[i];
        if (text_w(t, size) > width && !line.empty()) {
            out.push_back(line);
            line = words[i];
            if (out.size() == max_lines) return out;
        } else {
            line = t;
        }
    }
    if (!line.empty() && out.size() < max_lines) out.push_back(line);
    return out;
}

static string mb(long bytes)
{
    char b[32];
    snprintf(b, sizeof b, "%.1f MB", bytes / 1048576.0);
    return b;
}

static void help(const string &s)
{
    rect(0, 680, 1280, 40, 0, 0, 0, 0.35f);
    text(40, 706, 19, s, 0.75f, 0.8f, 0.9f);
}

static void box(const string &title, const vector<string> &lines, const string &keys)
{
    float h = 120 + 26 * lines.size();
    float y = (720 - h) / 2;
    rect(0, 0, 1280, 720, 0, 0, 0, 0.55f);
    rect(160, y, 960, h, 0.13f, 0.18f, 0.30f, 0.97f);
    rect(160, y, 960, 4, 0.95f, 0.65f, 0.2f, 1);
    text(190, y + 46, 28, title, 1, 1, 1);
    for (size_t i = 0; i < lines.size(); i++)
        text(190, y + 84 + 26 * i, 19, lines[i], 0.85f, 0.88f, 0.95f);
    text(190, y + h - 22, 20, keys, 0.95f, 0.75f, 0.35f);
}

// ---------------------------------------------------------------- input

static unsigned prev_buttons;
static int held[PS3PAD_BUTTONS];

static bool pressed(unsigned b, int btn) { return (b & (1u << btn)) && !(prev_buttons & (1u << btn)); }

static bool pressed_rep(unsigned b, int btn)
{
    if (!(b & (1u << btn))) { held[btn] = 0; return false; }
    held[btn]++;
    return held[btn] == 1 || (held[btn] > 22 && held[btn] % 4 == 0);
}

// ---------------------------------------------------------------- the screen

enum { SCR_MAIN, SCR_LIST, SCR_ASK_DL, SCR_DL, SCR_ASK_DEL, SCR_MSG };

static int screen, main_sel = 2, list_sel, list_top, cur = -1, airport, antialiasing = 1;
static http_dl dl;
static int dl_entry = -1;
static string dl_file;

static void message(const string &title, const vector<string> &lines)
{
    msg_title = title;
    msg_lines = lines;
    screen = SCR_MSG;
}

static void draw_main()
{
    const Entry *e = cur >= 0 ? &entries[cur] : 0;
    const Airport &a = airports[airport];
    char abuf[128];
    snprintf(abuf, sizeof abuf, "%s  %s, runway %s", a.icao, a.name, a.rwy);
    text(60, 92, 46, "FlightGear 0.9.10", 1, 1, 1);
    text(62, 128, 22, "for PlayStation 3", 0.7f, 0.75f, 0.85f);

    const char *labels[3] = { "Aircraft", "Airport", "" };
    string values[3] = { e ? e->title : "(none)", abuf, "Fly" };
    for (int i = 0; i < 3; i++) {
        float y = 200 + 80 * i;
        if (main_sel == i) rect(50, y, 1180, 60, 0.95f, 0.65f, 0.2f, 0.25f);
        if (i < 2) {
            text(80, y + 40, 24, labels[i], 0.65f, 0.7f, 0.8f);
            text(300, y + 40, 30, values[i], 1, 1, 1);
        } else {
            text(80, y + 42, 34, values[i], 1, 0.85f, 0.5f);
        }
    }
    if (e) {
        vector<string> d = wrap(e->desc.empty() ? e->title : e->desc, 21, 1100, 2);
        for (size_t i = 0; i < d.size(); i++) text(80, 490 + 28 * i, 21, d[i], 0.8f, 0.85f, 0.9f);
        std::map<string, string>::iterator n = notes.find(e->set);
        if (n != notes.end()) text(80, 560, 21, "Note: " + n->second, 1, 0.6f, 0.3f);
    }
    text(80, 640, 19, antialiasing ? "Antialiasing: on   (Triangle: off, restarts)"
                                    : "Antialiasing: off   (Triangle: on, restarts)", 0.6f, 0.65f, 0.75f);
    help("Up/Down: choose     X: select     Left/Right: change airport     START: fly");
}

static void draw_list()
{
    int ninst = 0, rows = 15;
    for (size_t i = 0; i < entries.size(); i++) ninst += entries[i].installed;
    text(60, 62, 34, "Choose an aircraft", 1, 1, 1);
    char cnt[96];
    snprintf(cnt, sizeof cnt, "%d installed, %d to download", ninst, (int)entries.size() - ninst);
    text(1220 - text_w(cnt, 19), 62, 19, cnt, 0.7f, 0.75f, 0.85f);
    if (list_sel < list_top) list_top = list_sel;
    if (list_sel >= list_top + rows) list_top = list_sel - rows + 1;
    for (int r = 0; r < rows && list_top + r < (int)entries.size(); r++) {
        int i = list_top + r;
        const Entry &e = entries[i];
        float y = 90 + 32 * r;
        bool noted = notes.find(e.set) != notes.end();
        string right = e.installed ? (e.base ? "included" : "installed") : mb(e.size);
        if (i == list_sel) rect(50, y, 1180, 30, 0.95f, 0.65f, 0.2f, 0.25f);
        if (i == cur) text(60, y + 23, 20, ">", 1, 0.85f, 0.5f);
        text(84, y + 23, 21, e.title, e.installed ? 1.0f : 0.6f, e.installed ? 1.0f : 0.8f, 1.0f);
        if (noted) text(420, y + 23, 19, "! problems", 1, 0.55f, 0.25f);
        text(1210 - text_w(right, 19), y + 23, 19, right, 0.65f, 0.7f, 0.8f);
    }
    if (list_sel < (int)entries.size()) {
        const Entry &e = entries[list_sel];
        string d = e.desc.empty() ? e.title : e.desc;
        if (!e.fdm.empty()) d += "   [" + e.fdm + "]";
        vector<string> lines = wrap(d, 20, 1150, 2);
        std::map<string, string>::iterator n = notes.find(e.set);
        for (size_t i = 0; i < lines.size(); i++) text(60, 605 + 26 * i, 20, lines[i], 0.8f, 0.85f, 0.9f);
        if (n != notes.end()) text(60, 660, 19, "Note: " + n->second, 1, 0.6f, 0.3f);
    }
    help("X: choose / download     Triangle: delete     O: back     START: fly");
}

static void draw_download()
{
    const Entry &e = entries[dl_entry];
    vector<string> l;
    char b[96];
    float frac = dl.total > 0 ? (float)dl.got / dl.total : 0;
    if (dl.total > 0) snprintf(b, sizeof b, "%s of %s", mb(dl.got).c_str(), mb(dl.total).c_str());
    else snprintf(b, sizeof b, "%s", mb(dl.got).c_str());
    l.push_back(b);
    l.push_back("");
    box("Downloading " + e.title, l, "O: cancel");
    rect(190, 360, 900, 22, 0, 0, 0, 0.5f);
    rect(190, 360, 900 * frac, 22, 0.95f, 0.65f, 0.2f, 1);
}

static void draw()
{
    frame_begin();
    switch (screen) {
    case SCR_MAIN: draw_main(); break;
    case SCR_LIST: draw_list(); break;
    case SCR_ASK_DL: {
        const Entry &e = entries[list_sel];
        vector<string> l;
        l.push_back(mb(e.size) + " from the FlightGear aircraft archive (versions 1.0 to 1.9.1).");
        l.push_back("These were made for newer FlightGear versions: many work, some only partly,");
        l.push_back("some not at all. If one does not start, the hangar will tell you.");
        draw_list();
        box("Download " + e.title + "?", l, "X: download     O: cancel");
        break;
    }
    case SCR_DL: draw_list(); draw_download(); break;
    case SCR_ASK_DEL: {
        vector<string> l;
        l.push_back("Removes the folder Aircraft/" + entries[list_sel].dir + " from the hard disk.");
        draw_list();
        box("Delete " + entries[list_sel].title + "?", l, "X: delete     O: cancel");
        break;
    }
    case SCR_MSG: draw_main(); box(msg_title, msg_lines, "X: OK"); break;
    }
}

static void present()
{
    draw();
    ps3glSwapBuffers();
}

static int unzip_entry;
static void unzip_progress(int done, int total)
{
    vector<string> l;
    char b[64];
    snprintf(b, sizeof b, "%d of %d files", done, total);
    l.push_back(b);
    frame_begin();
    draw_list();
    box("Installing " + entries[unzip_entry].title, l, "");
    ps3glSwapBuffers();
}

// Download finished: unpack, then pick the new aircraft.
static void install_download()
{
    char err[200], top[256];
    string title = entries[dl_entry].title, dir = entries[dl_entry].dir;
    vector<string> l;
    unzip_entry = dl_entry;
    unzip_progress(0, 1);
    if (unzip_all(dl_file.c_str(), AIRCRAFT, top, sizeof top, err, sizeof err, unzip_progress) < 0) {
        unlink(dl_file.c_str());
        l.push_back(err);
        message(title + " could not be installed", l);
        return;
    }
    unlink(dl_file.c_str());
    rebuild_lists();
    for (size_t i = 0; i < entries.size(); i++) {
        if (entries[i].installed && (entries[i].dir == dir || entries[i].dir == top)) {
            cur = list_sel = (int)i;
            break;
        }
    }
    l.push_back("It is selected now; choose Fly to try it.");
    l.push_back("Aircraft with several variants appear once per variant in the list.");
    message(title + " is installed", l);
}

static void start_download()
{
    const Entry &e = entries[list_sel];
    mkdir_p(DL_DIR);
    dl_entry = list_sel;
    dl_file = string(DL_DIR "/") + e.zip;
    if (http_start(&dl, (string(MIRROR) + e.zip).c_str(), dl_file.c_str()) < 0) {
        vector<string> l;
        l.push_back(dl.err);
        message("Download failed", l);
        return;
    }
    screen = SCR_DL;
}

// aircraft, airport and antialiasing for the next start
static void save_cfg()
{
    char c[200];
    snprintf(c, sizeof c, "aircraft=%s\nairport=%d\nantialiasing=%d\n",
             cur >= 0 ? entries[cur].set.c_str() : "", airport, antialiasing);
    write_file(CFG_FILE, c);
}

static void handle(unsigned b)
{
    switch (screen) {
    case SCR_MAIN:
        if (pressed_rep(b, PS3PAD_UP)) main_sel = (main_sel + 2) % 3;
        if (pressed_rep(b, PS3PAD_DOWN)) main_sel = (main_sel + 1) % 3;
        if (main_sel == 1 && pressed_rep(b, PS3PAD_LEFT)) airport = (airport + n_airports - 1) % n_airports;
        if (main_sel == 1 && pressed_rep(b, PS3PAD_RIGHT)) airport = (airport + 1) % n_airports;
        if (pressed(b, PS3PAD_TRIANGLE)) {      /* takes a restart: the render targets */
            antialiasing = !antialiasing;
            save_cfg();
            hangar_restart();
        }
        if (pressed(b, PS3PAD_CROSS)) {
            if (main_sel == 0) { screen = SCR_LIST; list_sel = cur >= 0 ? cur : 0; }
            else if (main_sel == 1) airport = (airport + 1) % n_airports;
        }
        break;
    case SCR_LIST:
        if (entries.empty()) { if (pressed(b, PS3PAD_CIRCLE)) screen = SCR_MAIN; break; }
        if (pressed_rep(b, PS3PAD_UP) && list_sel > 0) list_sel--;
        if (pressed_rep(b, PS3PAD_DOWN) && list_sel + 1 < (int)entries.size()) list_sel++;
        if (pressed_rep(b, PS3PAD_LEFT)) list_sel = std::max(0, list_sel - 15);
        if (pressed_rep(b, PS3PAD_RIGHT)) list_sel = std::min((int)entries.size() - 1, list_sel + 15);
        if (pressed(b, PS3PAD_CIRCLE)) screen = SCR_MAIN;
        if (pressed(b, PS3PAD_CROSS)) {
            if (entries[list_sel].installed) { cur = list_sel; screen = SCR_MAIN; main_sel = 2; }
            else screen = SCR_ASK_DL;
        }
        if (pressed(b, PS3PAD_TRIANGLE) && entries[list_sel].installed && !entries[list_sel].base)
            screen = SCR_ASK_DEL;
        break;
    case SCR_ASK_DL:
        if (pressed(b, PS3PAD_CROSS)) start_download();
        else if (pressed(b, PS3PAD_CIRCLE)) screen = SCR_LIST;
        break;
    case SCR_DL:
        if (pressed(b, PS3PAD_CIRCLE)) { http_cancel(&dl); screen = SCR_LIST; }
        break;
    case SCR_ASK_DEL:
        if (pressed(b, PS3PAD_CROSS)) {
            string dir = entries[list_sel].dir, set = entries[cur >= 0 ? cur : 0].set;
            remove_tree((string(AIRCRAFT "/") + dir).c_str());
            rebuild_lists();
            cur = find_set(set);
            if (cur < 0) cur = find_set("c172p");
            list_sel = std::min(list_sel, (int)entries.size() - 1);
            screen = SCR_LIST;
        } else if (pressed(b, PS3PAD_CIRCLE)) screen = SCR_LIST;
        break;
    case SCR_MSG:
        if (pressed(b, PS3PAD_CROSS) || pressed(b, PS3PAD_CIRCLE)) screen = SCR_MAIN;
        break;
    }
}

// ---------------------------------------------------------------- entry points

static const char *arg_value(int argc, char **argv, const char *opt)
{
    size_t n = strlen(opt);
    for (int i = argc - 1; i > 0; i--)
        if (!strncmp(argv[i], opt, n)) return argv[i] + n;
    return 0;
}

void hangar_run(int *argc, char **argv, int max_args)
{
    string cfg_set, a;
    vector<string> cfg = split(read_file(CFG_FILE, 4096), '\n');
    const char *from_args = arg_value(*argc, argv, "--aircraft=");

    // --no-hangar in fgfs.args (batch tests): start right away with the
    // options from fgfs.args, but still record the start
    for (int i = 1; i < *argc; i++) {
        if (strcmp(argv[i], "--no-hangar")) continue;
        for (int j = i; j < *argc; j++) argv[j] = argv[j + 1];
        (*argc)--;
        g_set = from_args ? from_args : "c172p";
        write_file(STATUS_FILE, "starting " + g_set + "\n");
        g_starting = true;
        g_batch = true;
        return;
    }

    for (size_t i = 0; i < cfg.size(); i++) {
        if (!cfg[i].compare(0, 9, "aircraft=")) cfg_set = trim(cfg[i].substr(9));
        if (!cfg[i].compare(0, 8, "airport=")) airport = atoi(cfg[i].c_str() + 8) % n_airports;
        if (!cfg[i].compare(0, 13, "antialiasing=")) antialiasing = atoi(cfg[i].c_str() + 13) != 0;
    }
    ps3glSetAntialiasing(antialiasing);
    ps3glInit();
    ps3glGetSize(&W, &H);
    S = H / 720.0f;
    font_ok = font.load(FGROOT "/Fonts/helvetica_medium.txf", GL_LINEAR, GL_LINEAR) != 0;
    if (!font_ok) font_ok = font.load(FGROOT "/Fonts/default.txf", GL_LINEAR, GL_LINEAR) != 0;
    txt.setFont(&font);

    rebuild_lists();
    check_last_start();
    cur = find_set(cfg_set);
    if (cur < 0 && from_args) cur = find_set(from_args);
    if (cur < 0) cur = find_set("c172p");
    if (cur < 0 && !entries.empty() && entries[0].installed) cur = 0;
    if (!msg_lines.empty()) screen = SCR_MSG;

    for (;;) {
        ps3pad_poll();
        unsigned b = ps3pad_get()->buttons;
        bool fly = false;
        if (screen == SCR_DL) {
            int r = http_step(&dl);
            if (r == 0) install_download();
            else if (r < 0) {
                vector<string> l;
                l.push_back(dl.err);
                http_cancel(&dl);
                message("Download failed", l);
            }
        }
        if ((screen == SCR_MAIN || screen == SCR_LIST) && pressed(b, PS3PAD_START)) fly = true;
        if (screen == SCR_MAIN && main_sel == 2 && pressed(b, PS3PAD_CROSS)) fly = true;
        if (fly && cur >= 0) break;
        handle(b);
        prev_buttons = b;
        present();
    }

    // the choice replaces aircraft and airport options from fgfs.args
    int n = 0;
    for (int i = 0; i < *argc; i++) {
        if (!strncmp(argv[i], "--aircraft=", 11) || !strncmp(argv[i], "--airport-id=", 13) ||
            !strncmp(argv[i], "--airport=", 10) || !strncmp(argv[i], "--runway=", 9))
            continue;
        argv[n++] = argv[i];
    }
    g_set = entries[cur].set;
    if (n + 3 < max_args) {
        argv[n++] = strdup(("--aircraft=" + g_set).c_str());
        argv[n++] = strdup((string("--airport-id=") + airports[airport].icao).c_str());
        argv[n++] = strdup((string("--runway=") + airports[airport].rwy).c_str());
    }
    argv[n] = 0;
    *argc = n;

    save_cfg();
    write_file(STATUS_FILE, "starting " + g_set + "\n");
    g_starting = true;

    // the loading screen comes from FlightGear; show something until then
    frame_begin();
    text(60, 380, 30, "Starting FlightGear with " + entries[cur].title + " ...", 1, 1, 1);
    ps3glSwapBuffers();
}

void hangar_mark_running(const char *warning)
{
    if (!g_starting) return;
    g_starting = false;
    write_file(STATUS_FILE, "running " + g_set + "\n");
    set_note(g_set, warning && *warning ? string("loaded with problems: ") + warning : string());
}

void hangar_mark_quit()
{
    write_file(STATUS_FILE, g_starting ? "quit-loading " + g_set + "\n" : string("quit\n"));
}

void hangar_mark_failed(int code)
{
    char b[32];
    snprintf(b, sizeof b, " %d\n", code);
    write_file(STATUS_FILE, "failed " + g_set + b);
}

bool hangar_starting() { return g_starting; }

void hangar_restart()
{
    ps3pad_rumble(0.0f, 0);
    if (g_batch) _exit(1);      /* would start the same failing flight again */
    fflush(stdout);
    fflush(stderr);
    sysProcessExitSpawn2(USRDIR "/EBOOT.BIN", NULL, NULL, NULL, 0, 1001, SYS_PROCESS_SPAWN_STACK_SIZE_1M);
    _exit(1);
}
