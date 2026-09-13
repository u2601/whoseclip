// WhoseClip for Linux - see what is on this machine's clipboard, and where it
// came from. Single-machine, current-clipboard-only. Never stores content.
//
// This is a parallel implementation of the Win32 original, not a shared-source
// port. The two platforms disagree about the one thing every line touches: a
// Windows wchar_t is UTF-16 and every Win32 clipboard format is wide, while
// every X11 and GTK entry point is UTF-8. Threading one string type through
// both costs more than keeping two honest trees, so function names here mirror
// src/main.cpp and src/clip.cpp deliberately: a fix in one is easy to mirror.
#pragma once

#include <string>
#include <vector>
#include <sys/types.h>

#define WC_VERSION       "1.0.0"
#define WC_AUTHOR        "u2601"
#define WC_URL           "github.com/u2601/whoseclip"

// Bytes, not characters. The Win32 build sizes this in UTF-16 code units; four
// bytes per character is the worst UTF-8 can do, so this holds the same text.
#define PREVIEW_MAX      16384
#define SOURCEURL_MAX    1024
#define OWNER_MAX        4096

#define POS_AUTO         (-2147483647 - 1)

// 0xRRGGBB, the order the ini already uses. Win32 COLORREF is byte-swapped
// against that; storing it the readable way round removes the conversion.
typedef unsigned int WcColor;
static inline int WcR(WcColor c) { return (int)((c >> 16) & 0xFF); }
static inline int WcG(WcColor c) { return (int)((c >>  8) & 0xFF); }
static inline int WcB(WcColor c) { return (int)( c        & 0xFF); }
static inline WcColor WcRGB(int r, int g, int b) {
    return ((WcColor)(r & 0xFF) << 16) | ((WcColor)(g & 0xFF) << 8) | (WcColor)(b & 0xFF);
}

enum ClipKind { CK_EMPTY, CK_TEXT, CK_IMAGE, CK_FILES, CK_OTHER, CK_LOCKED };

// How much of the premise actually holds in the session we landed in. WhoseClip
// exists to answer "who put this here", so when the display server will not say,
// that has to be visible rather than guessed at. See ClipSessionNote().
enum SessionKind {
    SESS_NONE,        // no display server we can talk to
    SESS_X11,         // real X11: owner and content both readable
    SESS_XWAYLAND,    // Wayland session, reached over XWayland: content yes, owner no
    SESS_WAYLAND      // Wayland with no usable X bridge: nothing readable
};

// One point-in-time view of the clipboard. Replaced wholesale on every change;
// the two fixed buffers below are the only place content ever lands, and they
// are wiped with explicit_bzero before reuse and on exit.
struct ClipSnapshot {
    unsigned long      seq;        // bumped locally on every owner change
    unsigned long long tick;       // CLOCK_MONOTONIC ms at capture
    ClipKind           kind;
    std::string        kindLabel;  // "TEXT", "IMAGE 1920x1080", "FILES (3)"
    unsigned long long bytes;
    bool               bytesKnown;

    pid_t       ownerPid;
    char        ownerName[OWNER_MAX];   // "vmtoolsd"
    char        ownerPath[OWNER_MAX];   // /usr/bin/vmtoolsd
    bool        ownerKnown;
    bool        external;               // owner is a known cross-machine relay
    std::string originTag;              // "from host / another VM", "copied here"

    bool        sensitive;              // app asked not to be monitored;
    std::string sensitiveReason;        // recorded, but content is shown anyway

    bool        virtualFiles;
    unsigned    fileCount;
    std::string dropEffect;             // "copy" / "move", when the source said

    char        preview[PREVIEW_MAX];
    char        sourceUrl[SOURCEURL_MAX];

    std::vector<std::string> formats;   // X11 target atom names
    std::string blocker;                // owner that would not answer, if any
};

struct Config {
    std::string label;
    WcColor     colLocal, colForeign, colIdle, colBg, colFg, colDim;
    int         stripX, stripY;         // POS_AUTO = top-right of the work area
    bool        showStrip;
    bool        expanded;
    int         previewChars;
    std::vector<std::string> extraExternals;   // extra process names, lowercase
    std::string iniPath;
};

// config_linux.cpp
void ConfigLoad(Config& c);
void ConfigApplyArgs(Config& c, int argc, char** argv);
void ConfigSaveUi(const Config& c);

// clip_x11.cpp
bool         ClipInit();                // false if no display server at all
SessionKind  ClipSession();
const char*  ClipSessionNote();         // one line, or "" when nothing is wrong
int          ClipFd();                  // X connection fd, for the GTK main loop
bool         ClipPending();             // an owner change arrived since last call
void         ClipDrainEvents();
void         SnapshotClear(ClipSnapshot& s);
void         SnapshotCapture(const Config& cfg, ClipSnapshot& s);
void         ClipShutdown();

std::string AgeText(unsigned long long tick);
std::string SizeText(unsigned long long n);
unsigned long long NowMs();
