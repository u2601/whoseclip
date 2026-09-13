// Clipboard inspection on X11. Asks the selection owner who it is and for a
// bounded look at what it is holding, then lets go. Nothing is retained beyond
// the single ClipSnapshot the caller owns.
//
// This backend also serves a Wayland session through XWayland. There the
// content still crosses the bridge, but the owner is the bridge rather than the
// application that copied, so ownership is reported as unknown instead of being
// guessed at. ClipSessionNote() says so in as many words.
#include "whoseclip_linux.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/XRes.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/select.h>

// ------------------------------------------------------------------ globals

static Display*    g_dpy       = NULL;
static Window      g_win       = None;
static int         g_xfixesEvt = -1;
static bool        g_haveXRes  = false;
static SessionKind g_session   = SESS_NONE;
static std::string g_note;
static unsigned long g_seq     = 0;
static bool        g_pending   = false;

static Atom A_CLIPBOARD, A_TARGETS, A_INCR, A_PROP, A_UTF8, A_NETWMPID;

// How long to wait for a selection owner to answer. One that does not answer in
// this long is wedged or gone; that is a finding, not a failure, and it surfaces
// as LOCKED the same way a held Win32 clipboard does.
#define SEL_TIMEOUT_MS 400

// Enough of any payload to classify, size and preview it. The rest is left in
// the owner's property: XGetWindowProperty reports what remains without
// transferring it, so a 40 MB image costs us 64 bytes and still reports 40 MB.
#define PEEK_TEXT   65536
#define PEEK_HEADER 64

// Processes that move a clipboard across a machine boundary. If one of these
// owns the selection, the content did not originate on this machine. Matched
// against the basename of /proc/<pid>/exe, so they are not subject to the
// 15-character truncation the kernel applies to /proc/<pid>/comm.
struct ExternalProc { const char* exe; const char* tag; };
static const ExternalProc kExternals[] = {
    { "vmtoolsd",                 "from host / another VM" },
    { "vmware-user",              "from host / another VM" },
    { "vmware-user-suid-wrapper", "from host / another VM" },
    { "VBoxClient",               "from the VirtualBox host" },
    { "VBoxDRMClient",            "from the VirtualBox host" },
    { "spice-vdagent",            "from the SPICE host" },
    { "xrdp-chansrv",             "from an RDP session" },
    { "x11vnc",                   "from a VNC client" },
};

// Owners that mean "the display server is relaying this", not "an application
// copied this". Under XWayland every selection is owned by the bridge, so
// naming the bridge as the source would be a confident wrong answer.
static const char* kBridges[] = { "Xwayland", "gnome-shell", "weston", "mutter-x11-frames" };

// ------------------------------------------------------------- error handler

// A window we are asking about can be destroyed between one call and the next.
// Xlib's default handler calls exit() on the resulting BadWindow, which would
// take the whole program down because someone else closed a tab.
static int g_xerr = 0;
static int XErrSink(Display* d, XErrorEvent* e) { (void)d; g_xerr = e->error_code; return 0; }
static void XErrReset() { g_xerr = 0; }
static bool XErrHit()   { return g_xerr != 0; }

// ---------------------------------------------------------------- utilities

unsigned long long NowMs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)(ts.tv_nsec / 1000000L);
}

std::string AgeText(unsigned long long tick)
{
    unsigned long long now = NowMs();
    unsigned long long d = (now > tick) ? (now - tick) / 1000ULL : 0ULL;
    char b[32];
    if (d < 60)        snprintf(b, sizeof b, "%llus", d);
    else if (d < 3600) snprintf(b, sizeof b, "%llum", d / 60);
    else               snprintf(b, sizeof b, "%lluh", d / 3600);
    return b;
}

std::string SizeText(unsigned long long n)
{
    char b[32];
    if (n < 1024)                       snprintf(b, sizeof b, "%llu B", n);
    else if (n < 1024ULL * 1024)        snprintf(b, sizeof b, "%.1f KB", (double)n / 1024.0);
    else if (n < 1024ULL * 1024 * 1024) snprintf(b, sizeof b, "%.1f MB", (double)n / (1024.0 * 1024.0));
    else                                snprintf(b, sizeof b, "%.2f GB", (double)n / (1024.0 * 1024.0 * 1024.0));
    return b;
}

static const char* BaseName(const char* path)
{
    const char* s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static bool IEquals(const char* a, const char* b) { return strcasecmp(a, b) == 0; }

static bool StartsWith(const char* s, const char* pre)
{
    return strncasecmp(s, pre, strlen(pre)) == 0;
}

// ------------------------------------------------------------------- UTF-8

// Length of the UTF-8 sequence starting at p, or 0 if it is not a valid one.
// Validation matters more than it looks: a payload advertised as UTF8_STRING
// can still be malformed, and Pango draws nothing at all for an invalid string
// rather than drawing it badly, so one stray byte would blank the whole strip.
static int Utf8Len(const unsigned char* p, size_t avail)
{
    unsigned char c = p[0];
    int n;
    if (c < 0x80)                return 1;
    else if ((c & 0xE0) == 0xC0) n = 2;
    else if ((c & 0xF0) == 0xE0) n = 3;
    else if ((c & 0xF8) == 0xF0) n = 4;
    else                         return 0;
    if ((size_t)n > avail) return 0;
    for (int i = 1; i < n; ++i) if ((p[i] & 0xC0) != 0x80) return 0;
    // Reject overlong encodings, surrogates and anything past U+10FFFF.
    if (n == 2 && (c & 0x1E) == 0) return 0;
    if (n == 3 && c == 0xE0 && (p[1] & 0x20) == 0) return 0;
    if (n == 3 && c == 0xED && (p[1] & 0x20) != 0) return 0;
    if (n == 4 && c == 0xF0 && (p[1] & 0x30) == 0) return 0;
    if (n == 4 && (c > 0xF4 || (c == 0xF4 && p[1] > 0x8F))) return 0;
    return n;
}

// Tidies clipboard text for display: control characters become spaces, runs of
// whitespace collapse, and blank lines collapse - but real line breaks are kept,
// because the expanded strip shows the copied text with its structure intact.
// The collapsed strip flattens them again at draw time. Always NUL terminates.
// maxChars counts characters rather than bytes, so previewChars means the same
// thing here as it does on the Win32 side.
static void SanitizeInto(char* dst, size_t dstMax, const char* src, size_t srcLen, int maxChars)
{
    const unsigned char* s = (const unsigned char*)src;
    size_t byteLimit = dstMax - 8;
    size_t o = 0, i = 0;
    int chars = 0;
    bool lastSpace = true;   // also trims leading whitespace
    bool lastNl    = true;   // also trims leading blank lines

    while (i < srcLen && o < byteLimit && chars < maxChars) {
        if (s[i] == 0) break;

        int n = Utf8Len(s + i, srcLen - i);
        if (n == 0) {
            i++;
            dst[o++] = '?';
            chars++;
            lastSpace = false;
            lastNl = false;
            continue;
        }

        if (n == 1) {
            char c = (char)s[i];
            if (c == '\r') {
                if (i + 1 < srcLen && s[i + 1] == '\n') { i++; continue; }   // CRLF is one break
                c = '\n';
            }
            if (c == '\n') {
                i++;
                if (lastNl) continue;
                while (o > 0 && dst[o - 1] == ' ') o--;                      // no trailing spaces
                dst[o++] = '\n';
                chars++;
                lastNl = true;
                lastSpace = true;
                continue;
            }
            if ((unsigned char)c < 32 || c == 0x7F) c = ' ';
            i++;
            if (c == ' ') {
                if (lastSpace) continue;
                lastSpace = true;
            } else {
                lastSpace = false;
                lastNl = false;
            }
            dst[o++] = c;
            chars++;
            continue;
        }

        if (o + (size_t)n >= byteLimit) break;
        memcpy(dst + o, s + i, (size_t)n);
        o += (size_t)n;
        i += (size_t)n;
        chars++;
        lastSpace = false;
        lastNl = false;
    }

    while (o > 0 && (dst[o - 1] == ' ' || dst[o - 1] == '\n')) o--;
    bool truncated = (i < srcLen && s[i] != 0);
    if (truncated && o + 3 < dstMax) { dst[o++] = '.'; dst[o++] = '.'; dst[o++] = '.'; }
    dst[o] = 0;
}

// X11 STRING is Latin-1 by specification, not UTF-8. Widening it is two lines
// and saves the strip from showing mojibake for every accented character an
// older application copies.
static std::string Latin1ToUtf8(const unsigned char* p, size_t n)
{
    std::string out;
    out.reserve(n + n / 8);
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = p[i];
        if (c < 0x80) out.push_back((char)c);
        else { out.push_back((char)(0xC0 | (c >> 6))); out.push_back((char)(0x80 | (c & 0x3F))); }
    }
    return out;
}

// text/x-moz-url is UTF-16 in the host byte order, holding "url\ntitle". It is
// the closest thing X11 has to the SourceURL line Windows carries in CF_HTML.
static std::string Utf16ToUtf8FirstLine(const unsigned char* p, size_t n)
{
    std::string out;
    for (size_t i = 0; i + 1 < n; i += 2) {
        unsigned int c = (unsigned int)p[i] | ((unsigned int)p[i + 1] << 8);
        if (c == 0 || c == '\n' || c == '\r') break;
        if (c >= 0xD800 && c <= 0xDFFF) continue;          // no surrogates in a URL
        if (c < 0x80) out.push_back((char)c);
        else if (c < 0x800) {
            out.push_back((char)(0xC0 | (c >> 6)));
            out.push_back((char)(0x80 | (c & 0x3F)));
        } else {
            out.push_back((char)(0xE0 | (c >> 12)));
            out.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (c & 0x3F)));
        }
        if (out.size() > SOURCEURL_MAX - 8) break;
    }
    return out;
}

static int HexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// file:///home/u/my%20doc.txt -> /home/u/my doc.txt
static std::string UriToPath(const std::string& uri)
{
    std::string u = uri;
    if (StartsWith(u.c_str(), "file://")) {
        size_t slash = u.find('/', 7);            // skip an optional hostname
        u = (slash == std::string::npos) ? u.substr(7) : u.substr(slash);
    }
    std::string out;
    out.reserve(u.size());
    for (size_t i = 0; i < u.size(); ++i) {
        if (u[i] == '%' && i + 2 < u.size()) {
            int hi = HexVal(u[i + 1]), lo = HexVal(u[i + 2]);
            if (hi >= 0 && lo >= 0) { out.push_back((char)((hi << 4) | lo)); i += 2; continue; }
        }
        out.push_back(u[i]);
    }
    return out;
}

// ------------------------------------------------------------------ session

static bool DetectSession()
{
    const char* type = getenv("XDG_SESSION_TYPE");
    const char* wl   = getenv("WAYLAND_DISPLAY");
    bool wayland = (wl && *wl) || (type && IEquals(type, "wayland"));

    if (!g_dpy) {
        g_session = wayland ? SESS_WAYLAND : SESS_NONE;
        g_note = wayland
            ? "Wayland session with no X bridge. Wayland does not let a background "
              "program read the clipboard at all, so there is nothing to show."
            : "No display server found. Set DISPLAY, or start this inside a desktop session.";
        return false;
    }

    // XWayland advertises itself. Trust that over the environment, which an ssh
    // session or a terminal multiplexer can easily be wrong about.
    int op = 0, ev = 0, er = 0;
    if (XQueryExtension(g_dpy, "XWAYLAND", &op, &ev, &er)) wayland = true;

    g_session = wayland ? SESS_XWAYLAND : SESS_X11;
    if (wayland) {
        g_note = "Wayland session. Content is read over XWayland, but Wayland offers no "
                 "way to ask which application owns the clipboard, so the owner shows as "
                 "unknown and cross-machine detection is off. Log in on Xorg for the lot.";
    } else {
        g_note.clear();
    }
    return true;
}

SessionKind ClipSession()     { return g_session; }
const char* ClipSessionNote() { return g_note.c_str(); }
int         ClipFd()          { return g_dpy ? ConnectionNumber(g_dpy) : -1; }

// --------------------------------------------------------------------- init

bool ClipInit()
{
    XSetErrorHandler(XErrSink);
    g_dpy = XOpenDisplay(NULL);
    if (!DetectSession()) return false;

    A_CLIPBOARD = XInternAtom(g_dpy, "CLIPBOARD", False);
    A_TARGETS   = XInternAtom(g_dpy, "TARGETS", False);
    A_INCR      = XInternAtom(g_dpy, "INCR", False);
    A_PROP      = XInternAtom(g_dpy, "WHOSECLIP_READ", False);
    A_UTF8      = XInternAtom(g_dpy, "UTF8_STRING", False);
    A_NETWMPID  = XInternAtom(g_dpy, "_NET_WM_PID", False);

    // An unmapped one-pixel window is enough to receive SelectionNotify and to
    // hold the property an owner writes into. It is never mapped and never takes
    // ownership of anything, so WhoseClip cannot become the clipboard source.
    g_win = XCreateSimpleWindow(g_dpy, DefaultRootWindow(g_dpy), -10, -10, 1, 1, 0, 0, 0);

    int err = 0;
    if (!XFixesQueryExtension(g_dpy, &g_xfixesEvt, &err)) {
        g_xfixesEvt = -1;
        if (g_note.empty())
            g_note = "XFIXES is missing, so clipboard changes are noticed by polling rather "
                     "than delivered. Everything still works, a little less promptly.";
    } else {
        XFixesSelectSelectionInput(g_dpy, g_win, A_CLIPBOARD,
                                   XFixesSetSelectionOwnerNotifyMask |
                                   XFixesSelectionWindowDestroyNotifyMask |
                                   XFixesSelectionClientCloseNotifyMask);
    }

    int major = 0, minor = 0;
    if (XResQueryVersion(g_dpy, &major, &minor)) {
        // Only XRes 1.2 knows how to map a resource back to a process id.
        g_haveXRes = (major > 1) || (major == 1 && minor >= 2);
    }

    XFlush(g_dpy);
    return true;
}

void ClipShutdown()
{
    if (!g_dpy) return;
    if (g_win != None) XDestroyWindow(g_dpy, g_win);
    XCloseDisplay(g_dpy);
    g_dpy = NULL;
    g_win = None;
}

// -------------------------------------------------------------- event pump

// Routes every event we are not waiting for. XFixes owner changes are recorded
// rather than dropped, so a change landing mid-read still triggers a recapture.
static void RouteEvent(const XEvent& e)
{
    if (g_xfixesEvt >= 0 && e.type == g_xfixesEvt + XFixesSelectionNotify) {
        const XFixesSelectionNotifyEvent* se = (const XFixesSelectionNotifyEvent*)&e;
        if (se->selection == A_CLIPBOARD) { g_pending = true; g_seq++; }
    }
}

bool ClipPending()
{
    bool p = g_pending;
    g_pending = false;
    return p;
}

void ClipDrainEvents()
{
    if (!g_dpy) return;
    while (XPending(g_dpy)) {
        XEvent e;
        XNextEvent(g_dpy, &e);
        RouteEvent(e);
    }
}

// Waits for the SelectionNotify answering our own request, keeping every other
// event moving. Returns false on timeout, meaning the owner never answered.
static bool WaitSelection(XEvent* out, int timeoutMs)
{
    unsigned long long deadline = NowMs() + (unsigned long long)timeoutMs;
    int fd = ConnectionNumber(g_dpy);
    for (;;) {
        while (XPending(g_dpy)) {
            XEvent e;
            XNextEvent(g_dpy, &e);
            if (e.type == SelectionNotify && e.xselection.requestor == g_win) {
                *out = e;
                return true;
            }
            RouteEvent(e);
        }
        unsigned long long now = NowMs();
        if (now >= deadline) return false;
        unsigned long long left = deadline - now;

        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(fd, &rd);
        struct timeval tv;
        tv.tv_sec  = (time_t)(left / 1000ULL);
        tv.tv_usec = (suseconds_t)((left % 1000ULL) * 1000ULL);
        int r = select(fd + 1, &rd, NULL, NULL, &tv);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) return false;
        XFlush(g_dpy);
    }
}

// One conversion round trip. Leaves at most `want` bytes in `out` and reports
// the payload's true total size in `total`, which XGetWindowProperty tells us
// without transferring the remainder.
// Returns 1 for data, 0 if the owner refused this target, -1 if it never answered.
static int ConvertSelection(Atom target, unsigned char** out, size_t* got,
                            unsigned long long* total, Atom* type, int* fmt, size_t want)
{
    *out = NULL; *got = 0; *total = 0; *type = None; *fmt = 0;

    XErrReset();
    XDeleteProperty(g_dpy, g_win, A_PROP);
    XConvertSelection(g_dpy, A_CLIPBOARD, target, A_PROP, g_win, CurrentTime);
    XFlush(g_dpy);

    XEvent e;
    if (!WaitSelection(&e, SEL_TIMEOUT_MS)) return -1;
    if (e.xselection.property == None) return 0;

    Atom rtype = None;
    int  rfmt = 0;
    unsigned long nitems = 0, after = 0;
    unsigned char* data = NULL;

    long words = (long)((want + 3) / 4);
    XErrReset();
    if (XGetWindowProperty(g_dpy, g_win, A_PROP, 0, words, False, AnyPropertyType,
                           &rtype, &rfmt, &nitems, &after, &data) != Success || XErrHit()) {
        if (data) XFree(data);
        return 0;
    }

    // INCR means the owner intends to stream it in chunks. We only ever want a
    // peek, and the INCR property carries a lower bound on the total, so take
    // the size and skip the stream rather than pulling megabytes to discard.
    if (rtype == A_INCR) {
        if (data && nitems >= 1 && rfmt == 32) *total = (unsigned long long)(*(const long*)data);
        if (data) XFree(data);
        XDeleteProperty(g_dpy, g_win, A_PROP);
        XFlush(g_dpy);
        *type = A_INCR;
        return 1;
    }

    // For 32-bit properties Xlib widens every item to a long, so the in-memory
    // size and the size worth reporting are not the same number.
    size_t unit  = (rfmt == 32) ? sizeof(long) : (size_t)(rfmt / 8);
    if (unit == 0) unit = 1;
    size_t bytes = (size_t)nitems * unit;
    size_t wire  = (rfmt == 32) ? (size_t)nitems * 4 : bytes;

    *out   = data;
    *got   = bytes;
    *total = (unsigned long long)wire + (unsigned long long)after;
    *type  = rtype;
    *fmt   = rfmt;

    XDeleteProperty(g_dpy, g_win, A_PROP);
    XFlush(g_dpy);
    return 1;
}

// ---------------------------------------------------------------- ownership

static bool PidFromXRes(Window w, pid_t* pid)
{
    if (!g_haveXRes || w == None) return false;
    XResClientIdSpec spec;
    spec.client = w;
    spec.mask   = XRES_CLIENT_ID_PID_MASK;

    long n = 0;
    XResClientIdValue* ids = NULL;
    XErrReset();
    // The return value is not worth testing. libXRes reports success as the
    // Success constant here, which is 0, while most Xlib query calls use 1 for
    // success - so either comparison is wrong against some build of the library.
    // The payload says plainly whether we got anything, so trust that instead.
    (void)XResQueryClientIds(g_dpy, 1, &spec, &n, &ids);
    if (XErrHit() || n <= 0 || !ids) {
        if (ids) XResClientIdsDestroy(n, ids);
        return false;
    }
    bool ok = false;
    for (long i = 0; i < n && !ok; ++i) {
        if (XResGetClientIdType(&ids[i]) == XRES_CLIENT_ID_PID) {
            pid_t p = (pid_t)XResGetClientPid(&ids[i]);
            if (p > 0) { *pid = p; ok = true; }
        }
    }
    if (ids) XResClientIdsDestroy(n, ids);
    return ok;
}

static bool PidFromNetWmPid(Window w, pid_t* pid)
{
    if (w == None) return false;
    Atom type = None;
    int fmt = 0;
    unsigned long nitems = 0, after = 0;
    unsigned char* data = NULL;
    XErrReset();
    if (XGetWindowProperty(g_dpy, w, A_NETWMPID, 0, 1, False, XA_CARDINAL,
                           &type, &fmt, &nitems, &after, &data) != Success || XErrHit()) {
        if (data) XFree(data);
        return false;
    }
    bool ok = false;
    if (data && nitems >= 1 && fmt == 32) {
        pid_t p = (pid_t)(*(const unsigned long*)data);
        if (p > 0) { *pid = p; ok = true; }
    }
    if (data) XFree(data);
    return ok;
}

// Selection owners are routinely unmapped helper windows carrying none of the
// usual properties. Walking up to the top level finds the one that does.
static bool PidFromAncestors(Window w, pid_t* pid)
{
    Window cur = w;
    for (int depth = 0; depth < 8 && cur != None; ++depth) {
        if (PidFromNetWmPid(cur, pid)) return true;
        Window root = None, parent = None, *kids = NULL;
        unsigned nkids = 0;
        XErrReset();
        if (!XQueryTree(g_dpy, cur, &root, &parent, &kids, &nkids) || XErrHit()) {
            if (kids) XFree(kids);
            return false;
        }
        if (kids) XFree(kids);
        if (parent == None || parent == root) return false;
        cur = parent;
    }
    return false;
}

static bool ProcPathFromPid(pid_t pid, char* out, size_t cch)
{
    char link[64];
    snprintf(link, sizeof link, "/proc/%d/exe", (int)pid);
    ssize_t n = readlink(link, out, cch - 1);
    if (n > 0) { out[n] = 0; return true; }

    // A process we cannot readlink - another user, or one inside a container -
    // still reports a name. The kernel truncates it to 15 characters, which is
    // still enough to match every relay name we care about.
    snprintf(link, sizeof link, "/proc/%d/comm", (int)pid);
    FILE* f = fopen(link, "r");
    if (!f) return false;
    if (!fgets(out, (int)cch, f)) { fclose(f); return false; }
    fclose(f);
    size_t l = strlen(out);
    while (l > 0 && (out[l - 1] == '\n' || out[l - 1] == '\r')) out[--l] = 0;
    return l > 0;
}

static void ResolveOwner(Window owner, const Config& cfg, ClipSnapshot& s)
{
    if (owner == None) { s.originTag = "nothing owns the clipboard"; return; }

    // Two ways of asking, tried in order, and a pid only counts if it still
    // resolves under /proc. The X server records the pid that opened the
    // connection, which is not always the process still using it: anything that
    // forks after connecting - xclip is the common example - leaves X holding a
    // pid that has already exited. A dead pid is not an answer, so fall through
    // to the next source rather than reporting the owner as known.
    pid_t pid = 0, firstPid = 0;
    char path[OWNER_MAX];
    bool resolved = false;

    if (PidFromXRes(owner, &pid)) {
        firstPid = pid;
        resolved = ProcPathFromPid(pid, path, sizeof path);
    }
    if (!resolved && PidFromAncestors(owner, &pid)) {
        if (!firstPid) firstPid = pid;
        resolved = ProcPathFromPid(pid, path, sizeof path);
    }
    if (!resolved) {
        s.ownerPid  = firstPid;
        s.originTag = "owner unknown";
        return;
    }

    snprintf(s.ownerPath, OWNER_MAX, "%s", path);
    snprintf(s.ownerName, OWNER_MAX, "%s", BaseName(path));
    s.ownerPid   = pid;
    s.ownerKnown = true;

    // Under XWayland the owner is always the bridge. Naming it would be a
    // confident wrong answer about where the content came from.
    for (size_t i = 0; i < sizeof(kBridges) / sizeof(kBridges[0]); ++i) {
        if (IEquals(kBridges[i], s.ownerName)) {
            s.ownerKnown = false;
            s.originTag  = (g_session == SESS_XWAYLAND)
                         ? "owner hidden by Wayland" : "owner is the display server";
            return;
        }
    }

    for (size_t i = 0; i < sizeof(kExternals) / sizeof(kExternals[0]); ++i) {
        if (IEquals(kExternals[i].exe, s.ownerName)) {
            s.external  = true;
            s.originTag = kExternals[i].tag;
            return;
        }
    }
    for (size_t i = 0; i < cfg.extraExternals.size(); ++i) {
        if (IEquals(cfg.extraExternals[i].c_str(), s.ownerName)) {
            s.external  = true;
            s.originTag = "from another machine";
            return;
        }
    }
    s.originTag = "copied here";
}

// ----------------------------------------------------------------- snapshot

void SnapshotClear(ClipSnapshot& s)
{
    explicit_bzero(s.preview,   sizeof(s.preview));
    explicit_bzero(s.sourceUrl, sizeof(s.sourceUrl));
    explicit_bzero(s.ownerName, sizeof(s.ownerName));
    explicit_bzero(s.ownerPath, sizeof(s.ownerPath));
    s.seq = g_seq;
    s.tick = NowMs();
    s.kind = CK_EMPTY;
    s.kindLabel.clear();
    s.bytes = 0;
    s.bytesKnown = false;
    s.ownerPid = 0;
    s.ownerKnown = false;
    s.external = false;
    s.originTag.clear();
    s.sensitive = false;
    s.sensitiveReason.clear();
    s.virtualFiles = false;
    s.fileCount = 0;
    s.dropEffect.clear();
    s.formats.clear();
    s.blocker.clear();
}

// Width and height without decoding the image: both headers put the dimensions
// at a fixed offset, so the 64-byte peek already has them.
static bool PngSize(const unsigned char* p, size_t n, long* w, long* h)
{
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    if (n < 24 || memcmp(p, sig, 8) != 0 || memcmp(p + 12, "IHDR", 4) != 0) return false;
    *w = ((long)p[16] << 24) | ((long)p[17] << 16) | ((long)p[18] << 8) | (long)p[19];
    *h = ((long)p[20] << 24) | ((long)p[21] << 16) | ((long)p[22] << 8) | (long)p[23];
    return *w > 0 && *h > 0;
}

static bool BmpSize(const unsigned char* p, size_t n, long* w, long* h)
{
    if (n < 26 || p[0] != 'B' || p[1] != 'M') return false;
    long ww = (long)((unsigned)p[18] | ((unsigned)p[19] << 8) | ((unsigned)p[20] << 16) | ((unsigned)p[21] << 24));
    long hh = (long)((int)((unsigned)p[22] | ((unsigned)p[23] << 8) | ((unsigned)p[24] << 16) | ((unsigned)p[25] << 24)));
    if (hh < 0) hh = -hh;
    *w = ww; *h = hh;
    return ww > 0 && hh > 0;
}

void SnapshotCapture(const Config& cfg, ClipSnapshot& s)
{
    SnapshotClear(s);

    if (!g_dpy) {
        s.kind = CK_LOCKED;
        s.kindLabel = "NO DISPLAY";
        s.blocker   = "no display server";
        s.originTag = "owner unknown";
        return;
    }

    ClipDrainEvents();

    // Owner first. It needs no conversation with the owning application, so we
    // still get a useful answer even when the owner will not answer anything.
    XErrReset();
    Window owner = XGetSelectionOwner(g_dpy, A_CLIPBOARD);
    if (XErrHit()) owner = None;
    ResolveOwner(owner, cfg, s);

    if (owner == None) {
        s.kind = CK_EMPTY;
        s.kindLabel = "EMPTY";
        s.tick = NowMs();
        return;
    }

    unsigned char* data = NULL;
    size_t got = 0;
    unsigned long long total = 0;
    Atom type = None;
    int fmt = 0;

    // TARGETS is the X11 analogue of EnumClipboardFormats.
    int r = ConvertSelection(A_TARGETS, &data, &got, &total, &type, &fmt, 4096);
    if (r < 0) {
        s.kind = CK_LOCKED;
        s.kindLabel = "LOCKED";
        s.blocker = s.ownerKnown ? s.ownerName : "the selection owner";
        s.tick = NowMs();
        return;
    }

    bool hasUtf8 = false, hasString = false, hasPlainUtf8 = false, hasPlain = false;
    bool hasImage = false, hasUriList = false, hasGnome = false;
    bool hasHtml = false, hasMozUrl = false, hasSecret = false;
    Atom aImage = None, aPlain = None, aGnome = None, aUri = None;
    Atom aHtml = None, aMoz = None;

    if (r == 1 && data && fmt == 32) {
        size_t n = got / sizeof(long);
        const long* atoms = (const long*)data;
        for (size_t i = 0; i < n; ++i) {
            Atom a = (Atom)atoms[i];
            if (a == None) continue;
            XErrReset();
            char* nm = XGetAtomName(g_dpy, a);
            if (XErrHit() || !nm) { if (nm) XFree(nm); continue; }

            // A TARGETS list may name the same type twice; XWayland does it for
            // every text type it bridges. Listing a duplicate teaches the reader
            // nothing and costs a line on the strip.
            bool seen = false;
            for (size_t k = 0; k < s.formats.size() && !seen; ++k)
                if (s.formats[k] == nm) seen = true;
            if (!seen) s.formats.push_back(nm);

            if (IEquals(nm, "UTF8_STRING"))                       hasUtf8 = true;
            else if (IEquals(nm, "STRING"))                       hasString = true;
            else if (IEquals(nm, "text/plain;charset=utf-8")) {   hasPlainUtf8 = true; aPlain = a; }
            else if (IEquals(nm, "text/plain"))              {   hasPlain = true; if (aPlain == None) aPlain = a; }
            else if (IEquals(nm, "text/html"))               {   hasHtml = true; aHtml = a; }
            else if (IEquals(nm, "text/uri-list"))           {   hasUriList = true; aUri = a; }
            else if (IEquals(nm, "x-special/gnome-copied-files")) { hasGnome = true; aGnome = a; }
            else if (IEquals(nm, "text/x-moz-url"))          {   hasMozUrl = true; aMoz = a; }
            else if (StartsWith(nm, "x-kde-passwordManagerHint")) hasSecret = true;
            else if (StartsWith(nm, "image/")) {
                hasImage = true;
                // Prefer PNG: it is what every toolkit offers and its header
                // gives the dimensions in the first 24 bytes.
                if (aImage == None || IEquals(nm, "image/png")) aImage = a;
            }
            XFree(nm);
        }
    }
    if (data) { XFree(data); data = NULL; }

    // An owner that refuses TARGETS may still serve plain text.
    if (r == 0 && s.formats.empty()) hasUtf8 = true;

    // Whether the copying application asked not to be monitored. Recorded as
    // metadata only: WhoseClip exists to show exactly what is on the clipboard,
    // so the content is displayed either way. Reported in the diagnostics dump.
    if (hasSecret) {
        s.sensitive = true;
        s.sensitiveReason = "marked as a password-manager secret";
    }

    if (hasGnome || hasUriList) {
        // Nautilus and friends offer the file list as text too, so this has to
        // be tested before text or a file copy would read as a wall of URIs.
        s.kind = CK_FILES;
        Atom want = hasGnome ? aGnome : aUri;
        int rr = ConvertSelection(want, &data, &got, &total, &type, &fmt, PEEK_TEXT);
        std::string listing;
        unsigned n = 0;
        if (rr == 1 && data && got > 0) {
            std::string body((const char*)data, got);
            size_t pos = 0;
            const size_t budget = (size_t)cfg.previewChars + 64;
            while (pos < body.size()) {
                size_t nl = body.find('\n', pos);
                std::string line = body.substr(pos, (nl == std::string::npos) ? std::string::npos : nl - pos);
                pos = (nl == std::string::npos) ? body.size() : nl + 1;
                while (!line.empty() && (line[line.size() - 1] == '\r')) line.erase(line.size() - 1);
                if (line.empty()) continue;
                // gnome-copied-files leads with the verb, which is the same
                // thing Windows carries as Preferred DropEffect.
                if (hasGnome && n == 0 && s.dropEffect.empty() &&
                    (IEquals(line.c_str(), "copy") || IEquals(line.c_str(), "cut"))) {
                    s.dropEffect = IEquals(line.c_str(), "cut") ? "move" : "copy";
                    continue;
                }
                n++;
                if (listing.size() < budget) {
                    if (!listing.empty()) listing += "\n";
                    listing += UriToPath(line);
                }
            }
        }
        if (data) { XFree(data); data = NULL; }
        s.fileCount = n;
        char b[64];
        snprintf(b, sizeof b, "FILES (%u)", n);
        s.kindLabel = b;
        if (total > 0) { s.bytes = total; s.bytesKnown = true; }
        if (!listing.empty())
            SanitizeInto(s.preview, PREVIEW_MAX, listing.c_str(), listing.size(), cfg.previewChars);

    } else if (hasUtf8 || hasPlainUtf8 || hasPlain || hasString) {
        s.kind = CK_TEXT;
        s.kindLabel = "TEXT";
        Atom want = hasUtf8 ? A_UTF8 : (hasPlainUtf8 || hasPlain) ? aPlain : XA_STRING;
        bool latin1 = (!hasUtf8 && !hasPlainUtf8 && !hasPlain);
        int rr = ConvertSelection(want, &data, &got, &total, &type, &fmt, PEEK_TEXT);
        if (rr == 1) {
            if (total > 0) { s.bytes = total; s.bytesKnown = true; }
            if (type == A_INCR) {
                snprintf(s.preview, PREVIEW_MAX, "%s", "(streamed on paste, not read)");
            } else if (data && got > 0) {
                if (latin1 || type == XA_STRING) {
                    std::string wide = Latin1ToUtf8(data, got);
                    SanitizeInto(s.preview, PREVIEW_MAX, wide.c_str(), wide.size(), cfg.previewChars);
                    explicit_bzero(&wide[0], wide.size());
                } else {
                    SanitizeInto(s.preview, PREVIEW_MAX, (const char*)data, got, cfg.previewChars);
                }
            }
        } else if (rr < 0) {
            s.kind = CK_LOCKED;
            s.kindLabel = "LOCKED";
            s.blocker = s.ownerKnown ? s.ownerName : "the selection owner";
        }
        if (data) { explicit_bzero(data, got); XFree(data); data = NULL; }

    } else if (hasImage) {
        s.kind = CK_IMAGE;
        s.kindLabel = "IMAGE";
        // Sixty-four bytes is every header we need. Asking for the whole image
        // would make the owner serialise megabytes we would throw away.
        int rr = ConvertSelection(aImage, &data, &got, &total, &type, &fmt, PEEK_HEADER);
        if (rr == 1) {
            if (total > 0) { s.bytes = total; s.bytesKnown = true; }
            long w = 0, h = 0;
            if (data && got > 0 && (PngSize(data, got, &w, &h) || BmpSize(data, got, &w, &h))) {
                char b[64];
                snprintf(b, sizeof b, "IMAGE %ldx%ld", w, h);
                s.kindLabel = b;
            }
        }
        if (data) { XFree(data); data = NULL; }

    } else if (!s.formats.empty()) {
        s.kind = CK_OTHER;
        s.kindLabel = "OTHER";
        std::string t = s.formats[0];
        if (s.formats.size() > 1) {
            char more[48];
            snprintf(more, sizeof more, "   (+%zu formats)", s.formats.size() - 1);
            t += more;
        }
        SanitizeInto(s.preview, PREVIEW_MAX, t.c_str(), t.size(), cfg.previewChars);

    } else {
        s.kind = CK_EMPTY;
        s.kindLabel = "EMPTY";
    }

    // The originating page, when a browser supplied one. X11 has no equivalent
    // of the SourceURL header Windows puts in CF_HTML, so the Mozilla URL
    // target is the only provenance a browser actually offers here.
    if (hasMozUrl && s.kind != CK_LOCKED) {
        int rr = ConvertSelection(aMoz, &data, &got, &total, &type, &fmt, 2048);
        if (rr == 1 && data && got > 0) {
            std::string u = Utf16ToUtf8FirstLine(data, got);
            if (!u.empty()) snprintf(s.sourceUrl, SOURCEURL_MAX, "%s", u.c_str());
        }
        if (data) { XFree(data); data = NULL; }
    }
    (void)hasHtml; (void)aHtml; (void)hasString;

    s.tick = NowMs();
}
