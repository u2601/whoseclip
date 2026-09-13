// WhoseClip - tray indicator plus optional always-on-top strip showing what is
// currently on this machine's clipboard and which process put it there.
#include "whoseclip.h"
#include <shellapi.h>
#include <windowsx.h>
#include <stdio.h>
#include <wchar.h>
#include <wctype.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")

#define WM_TRAY      (WM_APP + 1)
#define ID_TRAY      1
#define IDM_STRIP    100
#define IDM_PAUSE    101
#define IDM_FORMATS  102
#define IDM_DIAG     103
#define IDM_EXIT     104
#define IDM_EXPAND   105
#define TIMER_TICK   1
#define TIMER_CAPTURE 2
#define POS_AUTO     (-2147483647 - 1)

// Applications often write the clipboard in several steps, and some reopen it
// immediately afterwards. Reading it the instant WM_CLIPBOARDUPDATE arrives puts
// us in the middle of that and makes their operation fail, so wait for the
// writer to settle. Restarting the timer also coalesces bursts into one read.
#define CAPTURE_DELAY_MS 120

// A middle dot separator, built numerically so this file stays pure ASCII. A
// literal U+00B7 here is read in the system codepage by a compiler without
// /utf-8 and comes out as mojibake.
static const wchar_t kDot[] = { L' ', 0x00B7, L' ', 0 };

static Config       g_cfg;
static ClipSnapshot g_snap;
static HINSTANCE    g_inst      = NULL;
static HWND         g_hwndMain  = NULL;
static HWND         g_hwndStrip = NULL;
static HICON        g_icon      = NULL;
static HFONT        g_fontHead  = NULL;
static HFONT        g_fontBody  = NULL;
static NOTIFYICONDATAW g_nid;
static bool         g_paused    = false;
static bool         g_listening = false;
static int          g_iconState = -1;
static int          g_dpi       = 96;
static unsigned     g_ticks     = 0;
static int          g_hover     = 0;   // 0 none, 1 expand button, 2 close button

// ---------------------------------------------------------------- DPI helpers

static void InitDpiAwareness()
{
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
        typedef BOOL(WINAPI * PSetCtx)(DPI_AWARENESS_CONTEXT);
        PSetCtx p = (PSetCtx)GetProcAddress(u, "SetProcessDpiAwarenessContext");
        if (p && p(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
    }
    SetProcessDPIAware();
}

static int DpiOf(HWND h)
{
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
        typedef UINT(WINAPI * PGetDpi)(HWND);
        PGetDpi p = (PGetDpi)GetProcAddress(u, "GetDpiForWindow");
        if (p && h) {
            UINT d = p(h);
            if (d >= 48) return (int)d;
        }
    }
    HDC dc = GetDC(NULL);
    int d = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(NULL, dc);
    return d >= 48 ? d : 96;
}

static int Sc(int v) { return MulDiv(v, g_dpi, 96); }

// ------------------------------------------------------------------- config

static std::wstring ExeDir()
{
    wchar_t p[MAX_PATH] = { 0 };
    GetModuleFileNameW(NULL, p, MAX_PATH);
    wchar_t* s = wcsrchr(p, L'\\');
    if (s) *(s + 1) = 0;
    return p;
}

static COLORREF ReadColor(const wchar_t* key, const std::wstring& ini, COLORREF def)
{
    wchar_t buf[32] = { 0 };
    GetPrivateProfileStringW(L"whoseclip", key, L"", buf, 32, ini.c_str());
    if (!buf[0]) return def;
    unsigned long v = wcstoul(buf, NULL, 16);
    return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

static void SplitCsv(const wchar_t* s, std::vector<std::wstring>& out)
{
    std::wstring cur;
    for (const wchar_t* p = s; ; ++p) {
        if (*p == L',' || *p == 0) {
            size_t a = cur.find_first_not_of(L" \t");
            size_t b = cur.find_last_not_of(L" \t");
            if (a != std::wstring::npos) out.push_back(cur.substr(a, b - a + 1));
            cur.clear();
            if (*p == 0) break;
        } else {
            cur += *p;
        }
    }
}

void ConfigLoad(Config& c)
{
    c.iniPath = ExeDir() + L"whoseclip.ini";

    wchar_t comp[MAX_COMPUTERNAME_LENGTH + 1] = { 0 };
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(comp, &n)) lstrcpynW(comp, L"THIS PC", 16);

    wchar_t buf[512] = { 0 };
    GetPrivateProfileStringW(L"whoseclip", L"label", L"", buf, 512, c.iniPath.c_str());
    c.label = buf[0] ? buf : comp;

    c.colLocal   = ReadColor(L"colorLocal",   c.iniPath, RGB(0x3B, 0x9E, 0xFF));
    c.colForeign = ReadColor(L"colorForeign", c.iniPath, RGB(0xFF, 0x9A, 0x3B));
    c.colIdle    = ReadColor(L"colorIdle",    c.iniPath, RGB(0x6E, 0x6E, 0x78));
    c.colBg      = ReadColor(L"colorBg",      c.iniPath, RGB(0x18, 0x18, 0x20));
    c.colFg      = ReadColor(L"colorFg",      c.iniPath, RGB(0xEB, 0xEB, 0xF0));
    c.colDim     = ReadColor(L"colorDim",     c.iniPath, RGB(0x96, 0x96, 0xA0));

    c.showStrip     = GetPrivateProfileIntW(L"whoseclip", L"showStrip", 1, c.iniPath.c_str()) != 0;
    c.expanded      = GetPrivateProfileIntW(L"whoseclip", L"expanded",  0, c.iniPath.c_str()) != 0;
    c.previewChars  = GetPrivateProfileIntW(L"whoseclip", L"previewChars", 1500, c.iniPath.c_str());
    if (c.previewChars < 20)  c.previewChars = 20;
    if (c.previewChars > PREVIEW_MAX - 8) c.previewChars = PREVIEW_MAX - 8;

    c.stripX = (int)GetPrivateProfileIntW(L"whoseclip", L"stripX", POS_AUTO, c.iniPath.c_str());
    c.stripY = (int)GetPrivateProfileIntW(L"whoseclip", L"stripY", POS_AUTO, c.iniPath.c_str());

    buf[0] = 0;
    GetPrivateProfileStringW(L"whoseclip", L"extraExternals", L"", buf, 512, c.iniPath.c_str());
    if (buf[0]) SplitCsv(buf, c.extraExternals);
}

void ConfigApplyArgs(Config& c)
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return;
    for (int i = 1; i < argc; ++i) {
        const wchar_t* a = argv[i];
        bool hasNext = (i + 1 < argc);
        if (!_wcsicmp(a, L"--label") && hasNext)             c.label = argv[++i];
        else if (!_wcsicmp(a, L"--preview") && hasNext)      c.previewChars = _wtoi(argv[++i]);
        else if (!_wcsicmp(a, L"--color-local") && hasNext)  { unsigned long v = wcstoul(argv[++i], NULL, 16); c.colLocal = RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF); }
        else if (!_wcsicmp(a, L"--color-foreign") && hasNext){ unsigned long v = wcstoul(argv[++i], NULL, 16); c.colForeign = RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF); }
        else if (!_wcsicmp(a, L"--strip"))                   c.showStrip = true;
        else if (!_wcsicmp(a, L"--no-strip"))                c.showStrip = false;
        else if (!_wcsicmp(a, L"--expanded"))                c.expanded = true;
        else if (!_wcsicmp(a, L"--collapsed"))               c.expanded = false;
    }
    if (c.previewChars < 20) c.previewChars = 20;
    if (c.previewChars > PREVIEW_MAX - 8) c.previewChars = PREVIEW_MAX - 8;
    LocalFree(argv);
}

// The ini is the only thing WhoseClip ever writes. Clipboard content is not
// part of it and never touches disk.
void ConfigSaveUi(const Config& c)
{
    wchar_t b[32];
    swprintf_s(b, L"%d", c.showStrip ? 1 : 0);
    WritePrivateProfileStringW(L"whoseclip", L"showStrip", b, c.iniPath.c_str());
    swprintf_s(b, L"%d", c.expanded ? 1 : 0);
    WritePrivateProfileStringW(L"whoseclip", L"expanded", b, c.iniPath.c_str());
    swprintf_s(b, L"%d", c.stripX);
    WritePrivateProfileStringW(L"whoseclip", L"stripX", b, c.iniPath.c_str());
    swprintf_s(b, L"%d", c.stripY);
    WritePrivateProfileStringW(L"whoseclip", L"stripY", b, c.iniPath.c_str());
}

// --------------------------------------------------------------- presentation

static std::wstring HostOf(const wchar_t* url)
{
    const wchar_t* p = wcsstr(url, L"://");
    p = p ? p + 3 : url;
    const wchar_t* e = p;
    while (*e && *e != L'/' && *e != L'?') ++e;
    return std::wstring(p, e);
}

static COLORREF AccentColor()
{
    if (g_paused) return g_cfg.colIdle;
    if (g_snap.kind == CK_EMPTY || g_snap.kind == CK_LOCKED) return g_cfg.colIdle;
    return g_snap.external ? g_cfg.colForeign : g_cfg.colLocal;
}

// full = the whole story, for the expanded strip and the tray tooltip, where
// there is room and the reader asked for detail. Compact = what earns its place
// in a glance: the accent colour already says "this came from another machine",
// so repeating that in words only pushes the process name out of view.
static std::wstring OriginText(bool full)
{
    if (g_snap.kind == CK_LOCKED) return L"locked by " + g_snap.blocker;

    if (g_snap.ownerKnown) {
        std::wstring t = g_snap.external ? L"via " : L"from ";
        t += g_snap.ownerName;
        if (full && g_snap.external && !g_snap.originTag.empty()) {
            t += L" (" + g_snap.originTag + L")";
        }
        return t;
    }

    // With no owning process, a page a browser recorded is the only origin
    // there is, so it is worth the space even when collapsed.
    std::wstring t = L"owner unknown";
    if (g_snap.sourceUrl[0]) {
        std::wstring hostName = HostOf(g_snap.sourceUrl);
        if (!hostName.empty()) t += L"  [" + hostName + L"]";
    }
    return t;
}

// Identity, kind and size. Short enough that it never needs truncating, which
// is why the expanded strip uses it alone and gives the origin its own line.
static std::wstring Line1Core()
{
    std::wstring t = g_cfg.label;
    t += L"     ";
    if (g_paused) { t += L"PAUSED"; return t; }
    t += g_snap.kindLabel;
    if (g_snap.bytesKnown && g_snap.kind != CK_EMPTY) {
        t += L"  " + SizeText(g_snap.bytes);
    }
    return t;
}

// Collapsed header: everything on one line, so the origin may get an ellipsis.
static std::wstring Line1()
{
    std::wstring t = Line1Core();
    if (g_paused) return t;
    t += L"     " + OriginText(false);
    return t;
}

// Expanded meta line: the full origin, never truncated, plus whatever else the
// clipboard is telling us that the collapsed strip has no room for.
static std::wstring MetaLine()
{
    if (g_paused)                return L"monitoring paused";
    if (g_snap.kind == CK_EMPTY) return L"nothing on the clipboard";

    std::wstring t = OriginText(true);
    if (!g_snap.dropEffect.empty()) t += kDot + g_snap.dropEffect;
    if (g_snap.virtualFiles)        t += kDot + std::wstring(L"bytes stream on paste");
    if (g_snap.sensitive)           t += kDot + std::wstring(L"source asked not to be monitored");
    return t;
}

// Every format currently on the clipboard. Only shown expanded: it is the most
// diagnostic thing about a clipboard and the reason a VM file copy looked
// opaque, but it is never glanceable.
static std::wstring FormatsLine()
{
    if (g_snap.formats.empty()) return std::wstring();
    std::wstring t;
    for (size_t i = 0; i < g_snap.formats.size(); ++i) {
        if (i) t += kDot;
        t += g_snap.formats[i];
    }
    return t;
}

static std::wstring Line2()
{
    if (g_paused)                 return L"monitoring paused";
    if (g_snap.kind == CK_LOCKED) return L"could not read - clipboard held by another process";
    if (g_snap.kind == CK_EMPTY)  return L"clipboard empty";
    if (g_snap.preview[0])        return g_snap.preview;
    return L"(no preview available)";
}

// Line breaks are kept in the capture buffer so the expanded strip can show the
// copied text with its structure. Anywhere that has one line to work with gets
// them flattened to a separator instead.
static std::wstring OneLine(const std::wstring& s)
{
    std::wstring t;
    t.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\n') t += kDot;
        else               t += s[i];
    }
    return t;
}

// What the collapsed strip puts on its single line. A file drop is summarised
// as the first entry plus a count, since the full list only fits when expanded.
static std::wstring CollapsedPreview()
{
    std::wstring s = Line2();
    if (g_snap.kind == CK_FILES && g_snap.fileCount > 1) {
        // The count is already in the header as FILES (n) a few pixels away, so
        // spend this line on the first name rather than repeating the number.
        size_t nl = s.find(L'\n');
        return (nl == std::wstring::npos) ? s : s.substr(0, nl);
    }
    return OneLine(s);
}

static COLORREF MixColor(COLORREF a, COLORREF b, int pctB)
{
    int r  = (GetRValue(a) * (100 - pctB) + GetRValue(b) * pctB) / 100;
    int g  = (GetGValue(a) * (100 - pctB) + GetGValue(b) * pctB) / 100;
    int bl = (GetBValue(a) * (100 - pctB) + GetBValue(b) * pctB) / 100;
    return RGB(r, g, bl);
}

// Strip geometry. Kept in one place so painting, hit-testing and height
// measurement cannot drift apart.
static int StripWidth()    { return Sc(470); }
static int StripTextLeft() { return Sc(37); }

static RECT BtnClose(int w)
{
    int s = Sc(15), m = Sc(9);
    RECT r = { w - m - s, m, w - m, m + s };
    return r;
}

static RECT BtnExpand(int w)
{
    RECT c = BtnClose(w);
    int s = Sc(15), g = Sc(6);
    RECT r = { c.left - g - s, c.top, c.left - g, c.bottom };
    return r;
}

// -------------------------------------------------------------------- tray

static HICON MakeIcon(wchar_t ch, COLORREF bg)
{
    int cx = GetSystemMetrics(SM_CXSMICON);
    int cy = GetSystemMetrics(SM_CYSMICON);
    if (cx < 16) cx = 16;
    if (cy < 16) cy = 16;

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = cx;
    bi.bmiHeader.biHeight      = -cy;      // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = NULL;
    HDC screen = GetDC(NULL);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (screen) ReleaseDC(NULL, screen);
    if (!dib || !bits) { if (dib) DeleteObject(dib); return NULL; }

    HDC dc = CreateCompatibleDC(NULL);
    HGDIOBJ oldBmp = SelectObject(dc, dib);

    RECT r = { 0, 0, cx, cy };
    HBRUSH br = CreateSolidBrush(bg);
    FillRect(dc, &r, br);
    DeleteObject(br);

    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));
    lf.lfHeight  = -(cy * 3 / 4);
    lf.lfWeight  = FW_BOLD;
    lf.lfQuality = ANTIALIASED_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    HFONT f = CreateFontIndirectW(&lf);
    HGDIOBJ oldFont = SelectObject(dc, f);

    int lum = (GetRValue(bg) * 299 + GetGValue(bg) * 587 + GetBValue(bg) * 114) / 1000;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, lum > 140 ? RGB(16, 16, 16) : RGB(255, 255, 255));
    wchar_t s[2] = { ch, 0 };
    DrawTextW(dc, s, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(dc, oldFont);
    DeleteObject(f);
    SelectObject(dc, oldBmp);
    DeleteDC(dc);

    // GDI text rendering clears the alpha channel; make the whole tile opaque.
    BYTE* px = (BYTE*)bits;
    for (int i = 0; i < cx * cy; ++i) px[i * 4 + 3] = 255;

    std::vector<BYTE> zero((size_t)(((cx + 31) / 32) * 4) * (size_t)cy, 0);
    HBITMAP mask = CreateBitmap(cx, cy, 1, 1, zero.data());

    ICONINFO ii;
    ZeroMemory(&ii, sizeof(ii));
    ii.fIcon    = TRUE;
    ii.hbmMask  = mask;
    ii.hbmColor = dib;
    HICON ic = CreateIconIndirect(&ii);

    if (mask) DeleteObject(mask);
    DeleteObject(dib);
    return ic;
}

static void UpdateTray()
{
    int state = g_paused ? 3
              : (g_snap.kind == CK_EMPTY || g_snap.kind == CK_LOCKED) ? 0
              : (g_snap.external ? 2 : 1);

    if (state != g_iconState) {
        COLORREF c = (state == 2) ? g_cfg.colForeign
                   : (state == 1) ? g_cfg.colLocal
                                  : g_cfg.colIdle;
        wchar_t ch = g_cfg.label.empty() ? L'?' : (wchar_t)towupper(g_cfg.label[0]);
        HICON ni = MakeIcon(ch, c);
        if (ni) {
            HICON old = g_icon;
            g_icon = ni;
            g_iconState = state;
            if (old) DestroyIcon(old);
        }
    }

    std::wstring tip = g_cfg.label;
    if (g_paused) {
        tip += L"  |  PAUSED";
    } else {
        tip += L"  |  " + g_snap.kindLabel;
        if (g_snap.bytesKnown && g_snap.kind != CK_EMPTY) tip += L" " + SizeText(g_snap.bytes);
        if (g_snap.kind != CK_EMPTY) tip += L"  |  " + AgeText(g_snap.tick);
        tip += L"\r\n" + OriginText(true);
        if (g_snap.kind != CK_EMPTY) {
            std::wstring p = CollapsedPreview();
            if (p.size() > 46) p = p.substr(0, 45) + L"...";
            tip += L"\r\n" + p;
        }
    }
    if (tip.size() > 127) tip.resize(127);

    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.hIcon  = g_icon;
    lstrcpynW(g_nid.szTip, tip.c_str(), 128);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

// ------------------------------------------------------------------- strip

static void MakeFonts()
{
    if (g_fontHead) { DeleteObject(g_fontHead); g_fontHead = NULL; }
    if (g_fontBody) { DeleteObject(g_fontBody); g_fontBody = NULL; }

    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");

    lf.lfHeight = -Sc(12);
    lf.lfWeight = FW_SEMIBOLD;
    g_fontHead = CreateFontIndirectW(&lf);

    lf.lfHeight = -Sc(12);
    lf.lfWeight = FW_NORMAL;
    g_fontBody = CreateFontIndirectW(&lf);
}

static int MeasureWrapped(HFONT font, const std::wstring& s, int width)
{
    if (s.empty() || !font || width < 20) return 0;
    HDC dc = GetDC(NULL);
    if (!dc) return 0;
    HGDIOBJ old = SelectObject(dc, font);
    RECT r = { 0, 0, width, 0 };
    DrawTextW(dc, s.c_str(), -1, &r,
              DT_CALCRECT | DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
    SelectObject(dc, old);
    ReleaseDC(NULL, dc);
    return r.bottom - r.top;
}

// Expanded strip rows, resolved once so painting and height measurement cannot
// drift apart. Collapsed only needs `height`.
// Rows are stacked top-down and the window height falls out of the last one, so
// the body rect can never end up shorter than the text measured for it.
// DT_EDITCONTROL drops a partially visible last line outright, which turns a
// few pixels of error into a whole missing row.
struct StripLayout {
    int textLeft, textRight, textWidth;
    int headRight;        // header stops short of the buttons
    int metaY,  metaH;    // full origin, wrapped, never truncated
    int pathY,  pathH;    // full image path of the owning process
    int rule1Y;
    int bodyY,  bodyH;    // the content, or every file on its own line
    int rule2Y;
    int fmtY,   fmtH;     // every clipboard format, wrapped
    int footY;            // source URL on the left, age on the right
    int height;
};

static StripLayout ComputeLayout(int w)
{
    StripLayout L;
    ZeroMemory(&L, sizeof(L));
    L.textLeft  = StripTextLeft();
    L.textRight = w - Sc(12);
    L.textWidth = L.textRight - L.textLeft;
    L.headRight = BtnExpand(w).left - Sc(8);

    if (!g_cfg.expanded) {
        L.height = Sc(54);
        return L;
    }

    L.metaY = Sc(28);
    L.metaH = MeasureWrapped(g_fontBody, MetaLine(), L.textWidth);
    if (L.metaH <= 0) L.metaH = Sc(16);

    L.pathY = L.metaY + L.metaH + Sc(3);
    L.pathH = g_snap.ownerKnown ? Sc(16) : 0;

    L.rule1Y = L.pathY + L.pathH + Sc(9);
    L.bodyY  = L.rule1Y + Sc(10);
    L.bodyH  = MeasureWrapped(g_fontBody, Line2(), L.textWidth);
    if (L.bodyH <= 0) L.bodyH = Sc(16);

    L.fmtH = MeasureWrapped(g_fontBody, FormatsLine(), L.textWidth);

    // Everything below the body has a fixed cost; work out what is left for the
    // body itself, and give the body the squeeze if the cap is hit.
    int belowBody = Sc(10) + Sc(8) + L.fmtH + Sc(4) + Sc(18) + Sc(8);
    int minH = Sc(110);
    int maxH = Sc(460);
    RECT wa;
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
        int cap = (wa.bottom - wa.top) * 6 / 10;
        if (cap > minH && cap < maxH) maxH = cap;
    }
    int bodyRoom = maxH - L.bodyY - belowBody;
    if (bodyRoom < Sc(18)) bodyRoom = Sc(18);
    if (L.bodyH > bodyRoom) L.bodyH = bodyRoom;

    L.rule2Y = L.bodyY + L.bodyH + Sc(10);
    L.fmtY   = L.rule2Y + Sc(8);
    L.footY  = L.fmtY + L.fmtH + Sc(4);
    L.height = L.footY + Sc(18) + Sc(8);
    if (L.height < minH) L.height = minH;
    return L;
}

static void ApplyStripSize()
{
    if (!g_hwndStrip) return;
    int w = StripWidth();
    int h = ComputeLayout(w).height;

    RECT wr;
    GetWindowRect(g_hwndStrip, &wr);
    SetWindowPos(g_hwndStrip, NULL, wr.left, wr.top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);

    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, Sc(10), Sc(10));
    if (rgn) SetWindowRgn(g_hwndStrip, rgn, TRUE);
    InvalidateRect(g_hwndStrip, NULL, TRUE);
}

static void DrawButtons(HDC mem, int w)
{
    int pw = (g_dpi >= 144) ? 2 : 1;

    for (int which = 0; which < 2; ++which) {
        RECT r    = (which == 0) ? BtnExpand(w) : BtnClose(w);
        bool hot  = (g_hover == (which == 0 ? 1 : 2));

        if (hot) {
            RECT f = r;
            InflateRect(&f, Sc(3), Sc(3));
            HBRUSH hb = CreateSolidBrush(MixColor(g_cfg.colBg, g_cfg.colFg, 16));
            FillRect(mem, &f, hb);
            DeleteObject(hb);
        }

        HPEN pen = CreatePen(PS_SOLID, pw, hot ? g_cfg.colFg : g_cfg.colDim);
        HGDIOBJ oldPen = SelectObject(mem, pen);
        int cx = (r.left + r.right) / 2;
        int cy = (r.top + r.bottom) / 2;
        int d  = Sc(4);

        if (which == 1) {
            // Close: a cross.
            MoveToEx(mem, cx - d, cy - d, NULL);
            LineTo(mem, cx + d + 1, cy + d + 1);
            MoveToEx(mem, cx + d, cy - d, NULL);
            LineTo(mem, cx - d - 1, cy + d + 1);
        } else if (g_cfg.expanded) {
            // Collapse: chevron up.
            MoveToEx(mem, cx - d, cy + d / 2 + 1, NULL);
            LineTo(mem, cx, cy - d / 2 - 1);
            LineTo(mem, cx + d + 1, cy + d / 2 + 2);
        } else {
            // Expand: chevron down.
            MoveToEx(mem, cx - d, cy - d / 2 - 1, NULL);
            LineTo(mem, cx, cy + d / 2 + 1);
            LineTo(mem, cx + d + 1, cy - d / 2 - 2);
        }

        SelectObject(mem, oldPen);
        DeleteObject(pen);
    }
}

static void StripPaint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);

    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;

    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    HBRUSH bg = CreateSolidBrush(g_cfg.colBg);
    FillRect(mem, &rc, bg);
    DeleteObject(bg);

    COLORREF accent = AccentColor();

    // Accent bar down the left edge.
    RECT bar = { 0, 0, Sc(4), h };
    HBRUSH ab = CreateSolidBrush(accent);
    FillRect(mem, &bar, ab);
    DeleteObject(ab);

    // Status dot.
    int dotR = Sc(5);
    int dotCx = Sc(23);
    int dotCy = Sc(18);
    HBRUSH db = CreateSolidBrush(accent);
    HGDIOBJ oldBr = SelectObject(mem, db);
    HGDIOBJ oldPen = SelectObject(mem, GetStockObject(NULL_PEN));
    Ellipse(mem, dotCx - dotR, dotCy - dotR, dotCx + dotR + 1, dotCy + dotR + 1);
    SelectObject(mem, oldPen);
    SelectObject(mem, oldBr);
    DeleteObject(db);

    StripLayout L = ComputeLayout(w);

    SetBkMode(mem, TRANSPARENT);

    // Header. Collapsed carries the origin too and may ellipsize it; expanded
    // keeps it short because the origin gets its own full-width line below.
    HGDIOBJ oldFont = SelectObject(mem, g_fontHead);
    SetTextColor(mem, g_cfg.colFg);
    RECT r1 = { L.textLeft, Sc(7), L.headRight, Sc(26) };
    std::wstring l1 = g_cfg.expanded ? Line1Core() : Line1();
    DrawTextW(mem, l1.c_str(), -1, &r1, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS | DT_VCENTER);

    SelectObject(mem, g_fontBody);
    std::wstring age = (!g_paused && g_snap.kind != CK_EMPTY) ? AgeText(g_snap.tick) : std::wstring();

    if (!g_cfg.expanded) {
        // One flattened line, with the age parked on the right.
        RECT r2 = { L.textLeft, Sc(26), L.textRight, Sc(46) };
        if (!age.empty()) {
            RECT calc = { 0, 0, 0, 0 };
            DrawTextW(mem, age.c_str(), -1, &calc, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
            SetTextColor(mem, g_cfg.colDim);
            DrawTextW(mem, age.c_str(), -1, &r2, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER);
            r2.right -= (calc.right + Sc(10));
        }
        SetTextColor(mem, g_cfg.colDim);
        std::wstring l2 = CollapsedPreview();
        DrawTextW(mem, l2.c_str(), -1, &r2, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS | DT_VCENTER);
    } else {
        // Full origin, wrapped across as many lines as it needs.
        SetTextColor(mem, g_snap.external ? accent : g_cfg.colFg);
        RECT rm = { L.textLeft, L.metaY, L.textRight, L.metaY + L.metaH };
        std::wstring meta = MetaLine();
        DrawTextW(mem, meta.c_str(), -1, &rm,
                  DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);

        // Full image path of the owning process.
        if (L.pathH > 0) {
            SetTextColor(mem, g_cfg.colDim);
            RECT rp = { L.textLeft, L.pathY, L.textRight, L.pathY + L.pathH };
            DrawTextW(mem, g_snap.ownerPath, -1, &rp,
                      DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_PATH_ELLIPSIS | DT_VCENTER);
        }

        HPEN rule = CreatePen(PS_SOLID, 1, MixColor(g_cfg.colBg, g_cfg.colFg, 14));
        HGDIOBJ oldRule = SelectObject(mem, rule);
        MoveToEx(mem, L.textLeft, L.rule1Y, NULL);
        LineTo(mem, L.textRight, L.rule1Y);
        MoveToEx(mem, L.textLeft, L.rule2Y, NULL);
        LineTo(mem, L.textRight, L.rule2Y);
        SelectObject(mem, oldRule);
        DeleteObject(rule);

        // The whole preview: wrapped text, or every file on its own line.
        SetTextColor(mem, g_cfg.colFg);
        RECT rb = { L.textLeft, L.bodyY, L.textRight, L.bodyY + L.bodyH };
        std::wstring body = Line2();
        DrawTextW(mem, body.c_str(), -1, &rb,
                  DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);

        // Every format on the clipboard.
        if (L.fmtH > 0) {
            SetTextColor(mem, g_cfg.colDim);
            RECT rfm = { L.textLeft, L.fmtY, L.textRight, L.fmtY + L.fmtH };
            std::wstring fmts = FormatsLine();
            DrawTextW(mem, fmts.c_str(), -1, &rfm,
                      DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
        }

        RECT rf = { L.textLeft, L.footY, L.textRight, L.footY + Sc(18) };
        SetTextColor(mem, g_cfg.colDim);
        if (!age.empty()) {
            DrawTextW(mem, age.c_str(), -1, &rf, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER);
        }
        if (g_snap.sourceUrl[0]) {
            RECT ru = rf;
            ru.right -= Sc(48);
            DrawTextW(mem, g_snap.sourceUrl, -1, &ru,
                      DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS | DT_VCENTER);
        }
    }

    DrawButtons(mem, w);
    SelectObject(mem, oldFont);
    BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);

    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

static void ShowMenu(POINT pt, bool fromTray);

static LRESULT CALLBACK StripProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT:
        StripPaint(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    // The strip behaves as a title bar so it can be dragged from anywhere,
    // except over the two buttons, which need real client clicks.
    case WM_NCHITTEST: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        RECT rc;
        GetClientRect(hwnd, &rc);
        RECT bc = BtnClose(rc.right), be = BtnExpand(rc.right);
        if (PtInRect(&bc, pt) || PtInRect(&be, pt)) return HTCLIENT;
        return HTCAPTION;
    }

    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT rc;
        GetClientRect(hwnd, &rc);
        RECT bc = BtnClose(rc.right), be = BtnExpand(rc.right);
        int hv = PtInRect(&bc, pt) ? 2 : (PtInRect(&be, pt) ? 1 : 0);
        if (hv != g_hover) {
            g_hover = hv;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        TRACKMOUSEEVENT tme;
        tme.cbSize      = sizeof(tme);
        tme.dwFlags     = TME_LEAVE;
        tme.hwndTrack   = hwnd;
        tme.dwHoverTime = 0;
        TrackMouseEvent(&tme);
        return 0;
    }

    case WM_MOUSELEAVE:
        if (g_hover) {
            g_hover = 0;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && g_hover) {
            SetCursor(LoadCursor(NULL, IDC_HAND));
            return TRUE;
        }
        break;

    case WM_LBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT rc;
        GetClientRect(hwnd, &rc);
        RECT bc = BtnClose(rc.right), be = BtnExpand(rc.right);
        // Routed through the main window so the strip is never destroyed or
        // resized from inside its own message handler.
        if (PtInRect(&bc, pt))      PostMessageW(g_hwndMain, WM_COMMAND, IDM_STRIP, 0);
        else if (PtInRect(&be, pt)) PostMessageW(g_hwndMain, WM_COMMAND, IDM_EXPAND, 0);
        return 0;
    }

    // Double-click anywhere else on the strip toggles expansion too.
    case WM_NCLBUTTONDBLCLK:
        PostMessageW(g_hwndMain, WM_COMMAND, IDM_EXPAND, 0);
        return 0;

    case WM_NCRBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ShowMenu(pt, false);
        return 0;
    }

    case WM_EXITSIZEMOVE: {
        RECT r;
        GetWindowRect(hwnd, &r);
        g_cfg.stripX = r.left;
        g_cfg.stripY = r.top;
        ConfigSaveUi(g_cfg);
        return 0;
    }

    case WM_DPICHANGED: {
        g_dpi = (int)HIWORD(wp);
        MakeFonts();
        RECT* nr = (RECT*)lp;
        SetWindowPos(hwnd, NULL, nr->left, nr->top, nr->right - nr->left, nr->bottom - nr->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void CreateStrip()
{
    if (g_hwndStrip) return;

    int w = StripWidth();
    int h = ComputeLayout(w).height;

    int x = g_cfg.stripX, y = g_cfg.stripY;
    if (x == POS_AUTO || y == POS_AUTO) {
        RECT wa;
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
            x = wa.right - w - Sc(16);
            y = wa.top + Sc(16);
        } else {
            x = 100; y = 100;
        }
    }

    g_hwndStrip = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        L"WhoseClipStrip", L"WhoseClip", WS_POPUP,
        x, y, w, h, NULL, NULL, g_inst, NULL);
    if (!g_hwndStrip) return;

    SetLayeredWindowAttributes(g_hwndStrip, 0, 249, LWA_ALPHA);
    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, Sc(10), Sc(10));
    if (rgn) SetWindowRgn(g_hwndStrip, rgn, TRUE);
    ShowWindow(g_hwndStrip, SW_SHOWNOACTIVATE);
}

static void DestroyStrip()
{
    if (!g_hwndStrip) return;
    DestroyWindow(g_hwndStrip);
    g_hwndStrip = NULL;
    g_hover = 0;
}

// -------------------------------------------------------------------- menu

static void CopyTextToClipboard(const std::wstring& t)
{
    if (!OpenClipboard(g_hwndMain)) return;
    EmptyClipboard();
    size_t bytes = (t.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
        void* p = GlobalLock(h);
        if (p) {
            memcpy(p, t.c_str(), bytes);
            GlobalUnlock(h);
            if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
        } else {
            GlobalFree(h);
        }
    }
    CloseClipboard();
}

static std::wstring Diagnostics()
{
    std::wstring t = L"WhoseClip " WC_VERSION L" by " WC_AUTHOR L"\r\n" WC_URL L"\r\nMIT licensed\r\n\r\nMachine label: " + g_cfg.label + L"\r\n";
    wchar_t b[128];
    swprintf_s(b, L"Sequence: %lu\r\n", g_snap.seq);
    t += b;
    t += L"Kind: " + g_snap.kindLabel + L"\r\n";
    if (g_snap.bytesKnown) t += L"Size: " + SizeText(g_snap.bytes) + L"\r\n";
    t += L"Age: " + AgeText(g_snap.tick) + L"\r\n";
    if (g_snap.ownerKnown) {
        swprintf_s(b, L"Owner: %s (pid %lu)\r\n", g_snap.ownerName, g_snap.ownerPid);
        t += b;
        t += L"Owner path: ";
        t += g_snap.ownerPath;
        t += L"\r\n";
    } else {
        t += L"Owner: unknown\r\n";
    }
    t += L"Origin: " + g_snap.originTag + L"\r\n";
    if (g_snap.virtualFiles)        t += L"Virtual files: yes (bytes stream on paste)\r\n";
    if (!g_snap.dropEffect.empty()) t += L"Drop effect: " + g_snap.dropEffect + L"\r\n";
    if (g_snap.sourceUrl[0]) { t += L"SourceURL: "; t += g_snap.sourceUrl; t += L"\r\n"; }
    if (g_snap.sensitive)    t += L"Source asked not to be monitored: " + g_snap.sensitiveReason + L"\r\n";
    if (!g_snap.blocker.empty()) t += L"Blocked by: " + g_snap.blocker + L"\r\n";
    t += L"Formats:\r\n";
    for (size_t i = 0; i < g_snap.formats.size(); ++i) t += L"  " + g_snap.formats[i] + L"\r\n";
    return t;
}

static void ShowFormats()
{
    std::wstring t;
    if (g_snap.formats.empty()) {
        t = L"The clipboard is empty, or holds no readable formats.";
    } else {
        t = L"Formats currently on the clipboard:\r\n\r\n";
        for (size_t i = 0; i < g_snap.formats.size(); ++i) t += L"    " + g_snap.formats[i] + L"\r\n";
        t += L"\r\n" + OriginText(true);
    }
    MessageBoxW(NULL, t.c_str(), L"WhoseClip - clipboard formats",
                MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
}

static void ShowMenu(POINT pt, bool fromTray)
{
    HMENU m = CreatePopupMenu();
    if (!m) return;
    AppendMenuW(m, MF_STRING | (g_cfg.showStrip ? MF_CHECKED : 0), IDM_STRIP, L"Show strip");
    AppendMenuW(m, MF_STRING | (g_cfg.expanded ? MF_CHECKED : 0)
                             | (g_cfg.showStrip ? 0u : (MF_DISABLED | MF_GRAYED)),
                IDM_EXPAND, L"Expand strip");
    AppendMenuW(m, MF_STRING | (g_paused ? MF_CHECKED : 0), IDM_PAUSE, L"Pause monitoring");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_FORMATS, L"Clipboard formats...");
    AppendMenuW(m, MF_STRING, IDM_DIAG, L"Copy diagnostics (replaces clipboard)");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    std::wstring about = L"WhoseClip " WC_VERSION + (kDot + g_cfg.label);
    AppendMenuW(m, MF_STRING | MF_DISABLED | MF_GRAYED, 0, about.c_str());
    // Plain text, not a link. Opening a browser from here would mean shelling
    // out to a URL, and the point of this binary is that it does not reach out.
    std::wstring by = L"by " WC_AUTHOR + (kDot + std::wstring(WC_URL));
    AppendMenuW(m, MF_STRING | MF_DISABLED | MF_GRAYED, 0, by.c_str());
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Exit");

    SetForegroundWindow(g_hwndMain);
    UINT flags = TPM_RIGHTBUTTON | (fromTray ? TPM_BOTTOMALIGN : 0u);
    TrackPopupMenu(m, flags, pt.x, pt.y, 0, g_hwndMain, NULL);
    PostMessageW(g_hwndMain, WM_NULL, 0, 0);
    DestroyMenu(m);
}

// ------------------------------------------------------------------- main wnd

static void Recapture()
{
    SnapshotCapture(g_hwndMain, g_cfg, g_snap);
    UpdateTray();
    if (!g_hwndStrip) return;
    // The expanded strip is sized to its content, so new content resizes it.
    if (g_cfg.expanded) ApplyStripSize();
    else                InvalidateRect(g_hwndStrip, NULL, FALSE);
}

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CLIPBOARDUPDATE:
        if (!g_paused) SetTimer(hwnd, TIMER_CAPTURE, CAPTURE_DELAY_MS, NULL);
        return 0;

    case WM_TRAY:
        switch (LOWORD(lp)) {
        case WM_LBUTTONUP:
            g_cfg.showStrip = !g_cfg.showStrip;
            if (g_cfg.showStrip) CreateStrip(); else DestroyStrip();
            ConfigSaveUi(g_cfg);
            return 0;
        case WM_CONTEXTMENU:
        case WM_RBUTTONUP: {
            POINT pt = { GET_X_LPARAM(wp), GET_Y_LPARAM(wp) };
            if (pt.x == 0 && pt.y == 0) GetCursorPos(&pt);
            ShowMenu(pt, true);
            return 0;
        }
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_STRIP:
            g_cfg.showStrip = !g_cfg.showStrip;
            if (g_cfg.showStrip) CreateStrip(); else DestroyStrip();
            ConfigSaveUi(g_cfg);
            return 0;
        case IDM_EXPAND:
            g_cfg.expanded = !g_cfg.expanded;
            ApplyStripSize();
            ConfigSaveUi(g_cfg);
            return 0;
        case IDM_PAUSE:
            g_paused = !g_paused;
            if (!g_paused) Recapture();
            else { UpdateTray(); if (g_hwndStrip) InvalidateRect(g_hwndStrip, NULL, FALSE); }
            return 0;
        case IDM_FORMATS:
            ShowFormats();
            return 0;
        case IDM_DIAG:
            CopyTextToClipboard(Diagnostics());
            return 0;
        case IDM_EXIT:
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;

    case WM_TIMER:
        if (wp == TIMER_CAPTURE) {
            KillTimer(hwnd, TIMER_CAPTURE);
            if (!g_paused) Recapture();
        } else if (wp == TIMER_TICK) {
            ++g_ticks;
            if (g_hwndStrip) InvalidateRect(g_hwndStrip, NULL, FALSE);
            if (g_ticks % 3 == 0) UpdateTray();
        }
        return 0;

    // The shell recreates the tray after an Explorer restart.
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_TICK);
        KillTimer(hwnd, TIMER_CAPTURE);
        if (g_listening) { RemoveClipboardFormatListener(hwnd); g_listening = false; }
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }

    static UINT s_taskbarCreated = 0;
    if (!s_taskbarCreated) s_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (msg == s_taskbarCreated && s_taskbarCreated) {
        Shell_NotifyIconW(NIM_ADD, &g_nid);
        Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
        g_iconState = -1;
        UpdateTray();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmd, int show)
{
    UNREFERENCED_PARAMETER(hPrev);
    UNREFERENCED_PARAMETER(cmd);
    UNREFERENCED_PARAMETER(show);
    g_inst = hInst;

    InitDpiAwareness();
    ConfigLoad(g_cfg);
    ConfigApplyArgs(g_cfg);

    // One instance per machine label, so a stray second copy cannot confuse the
    // thing whose whole job is removing confusion.
    std::wstring mutexName = L"Local\\WhoseClip_" + g_cfg.label;
    HANDLE mtx = CreateMutexW(NULL, TRUE, mutexName.c_str());
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"WhoseClip is already running for this machine label.",
                    L"WhoseClip", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    ClipInit();
    SnapshotClear(g_snap);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = MainProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"WhoseClipMain";
    if (!RegisterClassExW(&wc)) return 1;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = StripProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"WhoseClipStrip";
    if (!RegisterClassExW(&wc)) return 1;

    // A real (never shown) window rather than a message-only one: message-only
    // windows are not reliable recipients of WM_CLIPBOARDUPDATE.
    g_hwndMain = CreateWindowExW(WS_EX_TOOLWINDOW, L"WhoseClipMain", L"WhoseClip",
                                 WS_POPUP, 0, 0, 0, 0, NULL, NULL, hInst, NULL);
    if (!g_hwndMain) return 1;

    g_dpi = DpiOf(g_hwndMain);
    MakeFonts();

    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = g_hwndMain;
    g_nid.uID              = ID_TRAY;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon            = LoadIcon(NULL, IDI_APPLICATION);
    lstrcpynW(g_nid.szTip, L"WhoseClip", 128);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);

    g_listening = AddClipboardFormatListener(g_hwndMain) != FALSE;

    if (g_cfg.showStrip) CreateStrip();
    Recapture();
    SetTimer(g_hwndMain, TIMER_TICK, 1000, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DestroyStrip();
    if (g_icon) DestroyIcon(g_icon);
    if (g_fontHead) DeleteObject(g_fontHead);
    if (g_fontBody) DeleteObject(g_fontBody);
    SnapshotClear(g_snap);
    if (mtx) { ReleaseMutex(mtx); CloseHandle(mtx); }
    return 0;
}
