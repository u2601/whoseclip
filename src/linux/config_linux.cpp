// Configuration. Same file, same keys, same defaults as the Win32 build, so a
// whoseclip.ini copied from a Windows machine works here unchanged.
//
// The ini is the only thing WhoseClip ever writes. Clipboard content is not
// part of it and never touches disk.
#include "whoseclip_linux.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>

// ---------------------------------------------------------------- ini store

struct IniPair { std::string key, value; };
static std::vector<IniPair> g_ini;

static std::string Trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// A deliberately small reader: one section, key=value, ';' and '#' comments.
// That is the whole of what the shipped ini uses, and matching the Win32
// profile API any more closely would buy nothing.
static void IniRead(const std::string& path)
{
    g_ini.clear();
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        std::string t = Trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '#' || t[0] == '[') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        IniPair p;
        p.key   = Trim(t.substr(0, eq));
        p.value = Trim(t.substr(eq + 1));
        if (!p.key.empty()) g_ini.push_back(p);
    }
    fclose(f);
}

static const char* IniGet(const char* key)
{
    for (size_t i = 0; i < g_ini.size(); ++i)
        if (strcasecmp(g_ini[i].key.c_str(), key) == 0) return g_ini[i].value.c_str();
    return NULL;
}

// Parsed by hand rather than with strtol, which is not the indulgence it looks.
// On glibc 2.38 and later the compiler binds strtol and strtoul to their C23
// variants, and those symbols do not exist on older systems: one call pins the
// finished binary to a distribution released in 2023 or later. The input here is
// an ini file whose grammar we define, so there is nothing to lose by reading it
// directly, and a binary built on 24.04 then still runs on 22.04.
static bool ParseInt(const char* s, int* out)
{
    if (!s) return false;
    while (*s == ' ' || *s == '\t') s++;
    bool neg = false;
    if (*s == '+' || *s == '-') { neg = (*s == '-'); s++; }
    if (*s < '0' || *s > '9') return false;

    long long n = 0;
    for (; *s >= '0' && *s <= '9'; ++s) {
        n = n * 10 + (*s - '0');
        if (n > 4294967296LL) { n = 4294967296LL; break; }   // saturate, never wrap
    }
    long long v = neg ? -n : n;
    if (v >  2147483647LL) v =  2147483647LL;
    if (v < -2147483648LL) v = -2147483648LL;
    *out = (int)v;
    return true;
}

static bool ParseHex6(const char* s, WcColor* out)
{
    if (!s) return false;
    while (*s == ' ' || *s == '\t') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    if (s[0] == '#') s++;

    unsigned long n = 0;
    int digits = 0;
    for (; digits < 8; ++digits, ++s) {
        int d;
        if      (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else break;
        n = (n << 4) | (unsigned long)d;
    }
    if (digits == 0) return false;
    *out = (WcColor)(n & 0xFFFFFF);
    return true;
}

static int IniGetInt(const char* key, int def)
{
    int n = 0;
    const char* v = IniGet(key);
    return (v && ParseInt(v, &n)) ? n : def;
}

static WcColor IniGetColor(const char* key, WcColor def)
{
    WcColor c = 0;
    const char* v = IniGet(key);
    return (v && ParseHex6(v, &c)) ? c : def;
}

static void SplitCsv(const char* s, std::vector<std::string>& out)
{
    std::string cur;
    for (const char* p = s; ; ++p) {
        if (*p == ',' || *p == 0) {
            std::string t = Trim(cur);
            if (!t.empty()) out.push_back(t);
            cur.clear();
            if (*p == 0) break;
        } else {
            cur += *p;
        }
    }
}

// ------------------------------------------------------------------- paths

static std::string ExeDir()
{
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return std::string("./");
    buf[n] = 0;
    char* slash = strrchr(buf, '/');
    if (slash) *(slash + 1) = 0;
    return std::string(buf);
}

static bool Exists(const std::string& p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

static void MkdirP(const std::string& dir)
{
    std::string cur;
    for (size_t i = 0; i < dir.size(); ++i) {
        cur += dir[i];
        if (dir[i] == '/' && cur.size() > 1) mkdir(cur.c_str(), 0700);
    }
    mkdir(dir.c_str(), 0700);
}

// A file next to the binary wins, so a copied folder stays self-contained the
// way the Windows build is. Otherwise this is a normal XDG citizen.
static std::string ChooseIniPath()
{
    std::string local = ExeDir() + "whoseclip.ini";
    if (Exists(local)) return local;

    const char* xdg = getenv("XDG_CONFIG_HOME");
    std::string base;
    if (xdg && *xdg) {
        base = xdg;
    } else {
        const char* home = getenv("HOME");
        if (!home || !*home) return local;
        base = std::string(home) + "/.config";
    }
    return base + "/whoseclip/whoseclip.ini";
}

// -------------------------------------------------------------------- load

void ConfigLoad(Config& c)
{
    c.iniPath = ChooseIniPath();
    IniRead(c.iniPath);

    char host[256] = { 0 };
    if (gethostname(host, sizeof host - 1) != 0 || !host[0])
        snprintf(host, sizeof host, "%s", "THIS BOX");

    const char* lbl = IniGet("label");
    c.label = (lbl && *lbl) ? lbl : host;

    c.colLocal   = IniGetColor("colorLocal",   WcRGB(0x3B, 0x9E, 0xFF));
    c.colForeign = IniGetColor("colorForeign", WcRGB(0xFF, 0x9A, 0x3B));
    c.colIdle    = IniGetColor("colorIdle",    WcRGB(0x6E, 0x6E, 0x78));
    c.colBg      = IniGetColor("colorBg",      WcRGB(0x18, 0x18, 0x20));
    c.colFg      = IniGetColor("colorFg",      WcRGB(0xEB, 0xEB, 0xF0));
    c.colDim     = IniGetColor("colorDim",     WcRGB(0x96, 0x96, 0xA0));

    c.showStrip    = IniGetInt("showStrip", 1) != 0;
    c.expanded     = IniGetInt("expanded",  0) != 0;
    c.previewChars = IniGetInt("previewChars", 1500);
    if (c.previewChars < 20) c.previewChars = 20;
    if (c.previewChars > (PREVIEW_MAX / 4) - 8) c.previewChars = (PREVIEW_MAX / 4) - 8;

    c.stripX = IniGetInt("stripX", POS_AUTO);
    c.stripY = IniGetInt("stripY", POS_AUTO);

    const char* extra = IniGet("extraExternals");
    if (extra && *extra) SplitCsv(extra, c.extraExternals);
}

// -------------------------------------------------------------------- args

static WcColor ArgColor(const char* s, WcColor fallback)
{
    WcColor c = 0;
    return ParseHex6(s, &c) ? c : fallback;
}

static int ArgInt(const char* s, int fallback)
{
    int n = 0;
    return ParseInt(s, &n) ? n : fallback;
}

void ConfigApplyArgs(Config& c, int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        bool hasNext = (i + 1 < argc);
        if (!strcasecmp(a, "--label") && hasNext)              c.label = argv[++i];
        else if (!strcasecmp(a, "--preview") && hasNext)       c.previewChars = ArgInt(argv[++i], c.previewChars);
        else if (!strcasecmp(a, "--color-local") && hasNext)   c.colLocal = ArgColor(argv[++i], c.colLocal);
        else if (!strcasecmp(a, "--color-foreign") && hasNext) c.colForeign = ArgColor(argv[++i], c.colForeign);
        else if (!strcasecmp(a, "--strip"))                    c.showStrip = true;
        else if (!strcasecmp(a, "--no-strip"))                 c.showStrip = false;
        else if (!strcasecmp(a, "--expanded"))                 c.expanded = true;
        else if (!strcasecmp(a, "--collapsed"))                c.expanded = false;
    }
    if (c.previewChars < 20) c.previewChars = 20;
    if (c.previewChars > (PREVIEW_MAX / 4) - 8) c.previewChars = (PREVIEW_MAX / 4) - 8;
}

// -------------------------------------------------------------------- save

// Rewrites four keys in place and leaves everything else exactly as it was.
// The shipped ini is mostly comments explaining each setting, and a writer that
// regenerated the file would throw all of that away the first time the strip
// was moved. This is what the Win32 profile API does for free.
static void IniSetInPlace(const std::string& path, const char* keys[], const char* vals[], int n)
{
    std::vector<std::string> lines;
    bool haveFile = false;
    FILE* f = fopen(path.c_str(), "r");
    if (f) {
        haveFile = true;
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            std::string t(line);
            while (!t.empty() && (t[t.size() - 1] == '\n' || t[t.size() - 1] == '\r'))
                t.erase(t.size() - 1);
            lines.push_back(t);
        }
        fclose(f);
    }

    std::vector<bool> done((size_t)n, false);
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string t = Trim(lines[i]);
        if (t.empty() || t[0] == ';' || t[0] == '#' || t[0] == '[') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string k = Trim(t.substr(0, eq));
        for (int j = 0; j < n; ++j) {
            if (done[(size_t)j] || strcasecmp(k.c_str(), keys[j]) != 0) continue;
            lines[i] = std::string(keys[j]) + "=" + vals[j];
            done[(size_t)j] = true;
        }
    }

    if (!haveFile) lines.push_back("[whoseclip]");
    for (int j = 0; j < n; ++j)
        if (!done[(size_t)j]) lines.push_back(std::string(keys[j]) + "=" + vals[j]);

    // Write beside the target and rename, so an interrupted save cannot leave a
    // half-written config where a working one used to be.
    std::string tmp = path + ".tmp";
    FILE* o = fopen(tmp.c_str(), "w");
    if (!o) return;
    for (size_t i = 0; i < lines.size(); ++i) fprintf(o, "%s\n", lines[i].c_str());
    fclose(o);
    if (rename(tmp.c_str(), path.c_str()) != 0) unlink(tmp.c_str());
}

void ConfigSaveUi(const Config& c)
{
    if (c.iniPath.empty()) return;

    size_t slash = c.iniPath.find_last_of('/');
    if (slash != std::string::npos) MkdirP(c.iniPath.substr(0, slash));

    char vShow[16], vExp[16], vX[16], vY[16];
    snprintf(vShow, sizeof vShow, "%d", c.showStrip ? 1 : 0);
    snprintf(vExp,  sizeof vExp,  "%d", c.expanded ? 1 : 0);
    snprintf(vX,    sizeof vX,    "%d", c.stripX);
    snprintf(vY,    sizeof vY,    "%d", c.stripY);

    const char* keys[] = { "showStrip", "expanded", "stripX", "stripY" };
    const char* vals[] = { vShow, vExp, vX, vY };
    IniSetInPlace(c.iniPath, keys, vals, 4);
}
