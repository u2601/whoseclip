// Clipboard inspection. Opens the clipboard, reads a bounded preview and some
// metadata, closes it immediately. Nothing is retained beyond the single
// ClipSnapshot the caller owns.
#include "whoseclip.h"
#include <shellapi.h>
#include <stdio.h>

// Formats applications register to opt their content out of monitoring.
// ExcludeClipboardContentFromMonitorProcessing: presence alone means "exclude".
// The other two carry a DWORD; a value of 0 means "not allowed".
static UINT g_fmtExclude = 0, g_fmtHistory = 0, g_fmtCloud = 0, g_fmtHtml = 0;

// Files copied from another machine do not exist here yet, so the shell cannot
// offer CF_HDROP (a list of local paths). It uses the virtual-file formats
// instead: a descriptor carrying names and sizes, with the bytes streamed only
// when you paste. This is the normal shape of a VM-to-host file copy.
static UINT g_fmtFgdW = 0, g_fmtFgdA = 0, g_fmtDropEffect = 0;
static UINT g_fmtShellIdList = 0;

// FILEDESCRIPTORW / FILEGROUPDESCRIPTORW, laid out by hand so this file does not
// have to pull in the shell headers. Sizes are checked against the real payload
// before anything is read.
#pragma pack(push, 1)
struct FileDescW {
    DWORD    dwFlags;
    GUID     clsid;
    LONG     sizelCx, sizelCy;
    LONG     pointlX, pointlY;
    DWORD    dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD    nFileSizeHigh;
    DWORD    nFileSizeLow;
    wchar_t  cFileName[MAX_PATH];
};
#pragma pack(pop)
#define FD_FILESIZE_FLAG 0x00000040

// Processes that move a clipboard across a machine boundary. If one of these
// owns the clipboard, the content did not originate on this machine.
struct ExternalProc { const wchar_t* exe; const wchar_t* tag; };
static const ExternalProc kExternals[] = {
    { L"vmtoolsd.exe",     L"from host / another VM" },
    { L"vmware-vmx.exe",   L"from a VM" },
    { L"vmware.exe",       L"from a VM" },
    { L"vmware-view.exe",  L"from VMware Horizon" },
    { L"vmusr.exe",        L"from host / another VM" },
    { L"rdpclip.exe",      L"from an RDP session" },
    { L"mstsc.exe",        L"from an RDP session" },
    { L"VBoxTray.exe",     L"from the VirtualBox host" },
    { L"VBoxClient.exe",   L"from the VirtualBox host" },
};

void ClipInit()
{
    g_fmtExclude = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    g_fmtHistory = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    g_fmtCloud   = RegisterClipboardFormatW(L"CanUploadToCloudClipboard");
    g_fmtHtml    = RegisterClipboardFormatW(L"HTML Format");

    g_fmtFgdW        = RegisterClipboardFormatW(L"FileGroupDescriptorW");
    g_fmtFgdA        = RegisterClipboardFormatW(L"FileGroupDescriptor");
    g_fmtDropEffect  = RegisterClipboardFormatW(L"Preferred DropEffect");
    g_fmtShellIdList = RegisterClipboardFormatW(L"Shell IDList Array");
}

std::wstring AgeText(ULONGLONG tick)
{
    ULONGLONG d = (GetTickCount64() - tick) / 1000;
    wchar_t b[32];
    if (d < 60)        swprintf_s(b, L"%llus", d);
    else if (d < 3600) swprintf_s(b, L"%llum", d / 60);
    else               swprintf_s(b, L"%lluh", d / 3600);
    return b;
}

std::wstring SizeText(ULONGLONG n)
{
    wchar_t b[32];
    if (n < 1024)                       swprintf_s(b, L"%llu B", n);
    else if (n < 1024ULL * 1024)        swprintf_s(b, L"%.1f KB", (double)n / 1024.0);
    else if (n < 1024ULL * 1024 * 1024) swprintf_s(b, L"%.1f MB", (double)n / (1024.0 * 1024.0));
    else                                swprintf_s(b, L"%.2f GB", (double)n / (1024.0 * 1024.0 * 1024.0));
    return b;
}

static const wchar_t* BaseName(const wchar_t* path)
{
    const wchar_t* s = wcsrchr(path, L'\\');
    return s ? s + 1 : path;
}

static bool ProcPathFromPid(DWORD pid, wchar_t* out, DWORD cch)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) return false;
    DWORD n = cch;
    BOOL ok = QueryFullProcessImageNameW(h, 0, out, &n);
    CloseHandle(h);
    return ok != FALSE;
}

// Tidies clipboard text for display: control characters become spaces, runs of
// whitespace collapse, and blank lines collapse - but real line breaks are kept,
// because the expanded strip shows the copied text with its structure intact.
// The collapsed strip flattens them again at draw time. Always NUL terminates.
static void SanitizeInto(wchar_t* dst, size_t dstMax, const wchar_t* src, size_t srcLen, int maxChars)
{
    size_t limit = dstMax - 4;
    if ((size_t)maxChars < limit) limit = (size_t)maxChars;
    size_t o = 0;
    size_t i = 0;
    bool lastSpace = true;   // also trims leading whitespace
    bool lastNl    = true;   // also trims leading blank lines
    for (; i < srcLen && o < limit; ++i) {
        wchar_t c = src[i];
        if (c == 0) break;

        if (c == L'\r') {
            if (i + 1 < srcLen && src[i + 1] == L'\n') continue;   // CRLF is one break
            c = L'\n';
        }
        if (c == L'\n') {
            if (lastNl) continue;
            while (o > 0 && dst[o - 1] == L' ') o--;               // no trailing spaces
            dst[o++] = L'\n';
            lastNl = true;
            lastSpace = true;
            continue;
        }

        if (c < 32) c = L' ';
        if (c == L' ') {
            if (lastSpace) continue;
            lastSpace = true;
        } else {
            lastSpace = false;
            lastNl = false;
        }
        dst[o++] = c;
    }
    while (o > 0 && (dst[o - 1] == L' ' || dst[o - 1] == L'\n')) o--;
    bool truncated = (i < srcLen && src[i] != 0);
    if (truncated && o + 3 < dstMax) { dst[o++] = L'.'; dst[o++] = L'.'; dst[o++] = L'.'; }
    dst[o] = 0;
}

// CF_HTML is UTF-8 with a small plain-text header. Browsers record the page a
// selection came from in a "SourceURL:" line, which is the only real provenance
// Windows ever carries.
static void ExtractSourceUrl(const char* html, size_t len, wchar_t* out, size_t outMax)
{
    static const char kKey[] = "SourceURL:";
    const size_t klen = sizeof(kKey) - 1;
    size_t scan = len < 1024 ? len : 1024;
    if (scan <= klen) return;
    for (size_t i = 0; i + klen < scan; ++i) {
        if (_strnicmp(html + i, kKey, klen) != 0) continue;
        size_t s = i + klen;
        while (s < len && (html[s] == ' ' || html[s] == '\t')) s++;
        size_t e = s;
        while (e < len && html[e] != '\r' && html[e] != '\n') e++;
        if (e <= s) return;
        size_t n = e - s;
        if (n > 900) n = 900;
        char tmp[904];
        memcpy(tmp, html + s, n);
        tmp[n] = 0;
        MultiByteToWideChar(CP_UTF8, 0, tmp, -1, out, (int)outMax);
        out[outMax - 1] = 0;
        return;
    }
}

void SnapshotClear(ClipSnapshot& s)
{
    SecureZeroMemory(s.preview, sizeof(s.preview));
    SecureZeroMemory(s.sourceUrl, sizeof(s.sourceUrl));
    SecureZeroMemory(s.ownerName, sizeof(s.ownerName));
    SecureZeroMemory(s.ownerPath, sizeof(s.ownerPath));
    s.seq = 0;
    s.tick = GetTickCount64();
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

// Reads a DWORD payload from a registered format. Returns false if absent or
// not shaped the way we expect, in which case the caller treats it as unset.
static bool ReadDwordFormat(UINT fmt, DWORD& value)
{
    HANDLE h = GetClipboardData(fmt);
    if (!h) return false;
    SIZE_T sz = GlobalSize(h);
    if (sz < sizeof(DWORD)) return false;
    void* p = GlobalLock(h);
    if (!p) return false;
    value = *(const DWORD*)p;
    GlobalUnlock(h);
    return true;
}

void SnapshotCapture(HWND hwnd, const Config& cfg, ClipSnapshot& s)
{
    SnapshotClear(s);
    s.seq = GetClipboardSequenceNumber();

    // Owner first: this needs no clipboard lock, so we still get a useful
    // answer even when someone else is holding it.
    HWND owner = GetClipboardOwner();
    if (owner) {
        DWORD pid = 0;
        GetWindowThreadProcessId(owner, &pid);
        if (pid && ProcPathFromPid(pid, s.ownerPath, OWNER_MAX)) {
            s.ownerPid = pid;
            s.ownerKnown = true;
            lstrcpynW(s.ownerName, BaseName(s.ownerPath), OWNER_MAX);
            for (const ExternalProc& e : kExternals) {
                if (_wcsicmp(e.exe, s.ownerName) == 0) {
                    s.external = true;
                    s.originTag = e.tag;
                    break;
                }
            }
            if (!s.external) {
                for (const std::wstring& e : cfg.extraExternals) {
                    if (_wcsicmp(e.c_str(), s.ownerName) == 0) {
                        s.external = true;
                        s.originTag = L"from another machine";
                        break;
                    }
                }
            }
            if (!s.external) s.originTag = L"copied here";
        }
    }
    if (!s.ownerKnown) s.originTag = L"owner unknown";

    // Hold the clipboard as briefly as possible; other applications block on it
    // while we are inside.
    bool opened = false;
    for (int i = 0; i < 12; ++i) {
        if (OpenClipboard(hwnd)) { opened = true; break; }
        Sleep(15);
    }
    if (!opened) {
        s.kind = CK_LOCKED;
        s.kindLabel = L"LOCKED";
        HWND b = GetOpenClipboardWindow();
        DWORD pid = 0;
        wchar_t path[OWNER_MAX];
        if (b) GetWindowThreadProcessId(b, &pid);
        if (pid && ProcPathFromPid(pid, path, OWNER_MAX)) s.blocker = BaseName(path);
        else s.blocker = L"another process";
        return;
    }

    bool hasText = false, hasWText = false, hasDib = false, hasBmp = false;
    bool hasDrop = false, hasHtml = false, hasExclude = false;
    bool hasHist = false, hasCloud = false;
    bool hasFgdW = false, hasFgdA = false, hasIdList = false, hasDropEff = false;

    UINT f = 0;
    while ((f = EnumClipboardFormats(f)) != 0) {
        if (f == CF_UNICODETEXT)               hasWText = true;
        else if (f == CF_TEXT)                 hasText = true;
        else if (f == CF_DIB || f == CF_DIBV5) hasDib = true;
        else if (f == CF_BITMAP)               hasBmp = true;
        else if (f == CF_HDROP)                hasDrop = true;
        if (g_fmtHtml && f == g_fmtHtml)       hasHtml = true;
        if (g_fmtExclude && f == g_fmtExclude) hasExclude = true;
        if (g_fmtHistory && f == g_fmtHistory) hasHist = true;
        if (g_fmtCloud && f == g_fmtCloud)     hasCloud = true;
        if (g_fmtFgdW && f == g_fmtFgdW)       hasFgdW = true;
        if (g_fmtFgdA && f == g_fmtFgdA)       hasFgdA = true;
        if (g_fmtShellIdList && f == g_fmtShellIdList) hasIdList = true;
        if (g_fmtDropEffect && f == g_fmtDropEffect)   hasDropEff = true;

        if (f >= 0xC000) {
            wchar_t name[128];
            if (GetClipboardFormatNameW(f, name, 128) > 0) s.formats.push_back(name);
            else s.formats.push_back(L"(registered)");
        } else {
            const wchar_t* known =
                f == CF_TEXT        ? L"CF_TEXT" :
                f == CF_UNICODETEXT ? L"CF_UNICODETEXT" :
                f == CF_BITMAP      ? L"CF_BITMAP" :
                f == CF_DIB         ? L"CF_DIB" :
                f == CF_DIBV5       ? L"CF_DIBV5" :
                f == CF_HDROP       ? L"CF_HDROP" :
                f == CF_LOCALE      ? L"CF_LOCALE" :
                f == CF_OEMTEXT     ? L"CF_OEMTEXT" : NULL;
            if (known) {
                s.formats.push_back(known);
            } else {
                wchar_t name[32];
                swprintf_s(name, L"#%u", f);
                s.formats.push_back(name);
            }
        }
    }

    // Whether the copying application asked not to be monitored. Recorded as
    // metadata only: WhoseClip exists to show exactly what is on the clipboard,
    // so the content is displayed either way. Reported in the diagnostics dump.
    if (hasExclude) {
        s.sensitive = true;
        s.sensitiveReason = L"marked exclude-from-monitoring";
    }
    DWORD v = 0;
    if (!s.sensitive && hasHist && ReadDwordFormat(g_fmtHistory, v) && v == 0) {
        s.sensitive = true;
        s.sensitiveReason = L"marked no-clipboard-history";
    }
    v = 0;
    if (!s.sensitive && hasCloud && ReadDwordFormat(g_fmtCloud, v) && v == 0) {
        s.sensitive = true;
        s.sensitiveReason = L"marked no-cloud-clipboard";
    }

    if (hasDrop) {
        s.kind = CK_FILES;
        HDROP hd = (HDROP)GetClipboardData(CF_HDROP);
        UINT n = hd ? DragQueryFileW(hd, 0xFFFFFFFF, NULL, 0) : 0;
        s.fileCount = n;
        wchar_t b[64];
        swprintf_s(b, L"FILES (%u)", n);
        s.kindLabel = b;
        if (hd) { s.bytes = GlobalSize(hd); s.bytesKnown = true; }
        if (hd && n > 0) {
            // Every path, one per line, so the expanded strip can list them all.
            // The collapsed strip shows the first and a count.
            std::wstring t;
            const size_t budget = (size_t)cfg.previewChars + 64;
            for (UINT i = 0; i < n && t.size() < budget; ++i) {
                wchar_t path[MAX_PATH] = { 0 };
                if (DragQueryFileW(hd, i, path, MAX_PATH) == 0) continue;
                if (!t.empty()) t += L"\n";
                t += path;
            }
            SanitizeInto(s.preview, PREVIEW_MAX, t.c_str(), t.size(), cfg.previewChars);
        }
    } else if (hasFgdW || hasFgdA || hasIdList) {
        // Virtual files: named and sized here, but the bytes live on the other
        // machine until you paste. This is what a VM-to-host file copy looks
        // like, and why there is no CF_HDROP to read paths from.
        s.kind = CK_FILES;
        s.virtualFiles = true;

        UINT n = 0;
        std::wstring listing;
        ULONGLONG total = 0;
        bool sizeKnown = false;

        UINT descFmt = hasFgdW ? g_fmtFgdW : (hasFgdA ? g_fmtFgdA : g_fmtShellIdList);
        HANDLE h = GetClipboardData(descFmt);
        if (h) {
            SIZE_T sz = GlobalSize(h);
            const BYTE* p = (const BYTE*)GlobalLock(h);
            if (p) {
                if (sz >= sizeof(UINT)) n = *(const UINT*)p;
                // Names and sizes are only laid out this way in the wide
                // descriptor; trust the payload's own length over its count.
                if (hasFgdW && sz > sizeof(UINT)) {
                    SIZE_T avail = (sz - sizeof(UINT)) / sizeof(FileDescW);
                    UINT usable = ((SIZE_T)n <= avail) ? n : (UINT)avail;
                    const FileDescW* fd = (const FileDescW*)(p + sizeof(UINT));
                    const size_t budget = (size_t)cfg.previewChars + 64;
                    for (UINT i = 0; i < usable; ++i) {
                        bool hasSize = (fd[i].dwFlags & FD_FILESIZE_FLAG) != 0;
                        ULONGLONG one = 0;
                        if (hasSize) {
                            one = ((ULONGLONG)fd[i].nFileSizeHigh << 32) | fd[i].nFileSizeLow;
                            total += one;
                            sizeKnown = true;
                        }
                        // Every name, one per line, each with its own size.
                        if (listing.size() < budget) {
                            if (!listing.empty()) listing += L"\n";
                            listing.append(fd[i].cFileName, wcsnlen(fd[i].cFileName, MAX_PATH));
                            if (hasSize) listing += L"   " + SizeText(one);
                        }
                    }
                }
                GlobalUnlock(h);
            }
        }

        s.fileCount = n;
        wchar_t b[64];
        swprintf_s(b, L"FILES (%u) virtual", n);
        s.kindLabel = b;
        if (sizeKnown) { s.bytes = total; s.bytesKnown = true; }

        if (!listing.empty()) {
            SanitizeInto(s.preview, PREVIEW_MAX, listing.c_str(), listing.size(), cfg.previewChars);
        }
    } else if (hasWText || hasText) {
        s.kind = CK_TEXT;
        s.kindLabel = L"TEXT";
        HANDLE h = GetClipboardData(hasWText ? CF_UNICODETEXT : CF_TEXT);
        if (h) {
            // Keep the in-memory length as SIZE_T: s.bytes is 64-bit for large
            // virtual file sets, but a single clipboard block cannot exceed it.
            const SIZE_T gsz = GlobalSize(h);
            s.bytes = gsz;
            s.bytesKnown = true;
            void* p = GlobalLock(h);
            if (p) {
                if (hasWText) {
                    const wchar_t* w = (const wchar_t*)p;
                    size_t maxChars = gsz / sizeof(wchar_t);
                    SanitizeInto(s.preview, PREVIEW_MAX, w, wcsnlen(w, maxChars), cfg.previewChars);
                } else {
                    const char* a = (const char*)p;
                    size_t alen = strnlen(a, gsz);
                    int need = MultiByteToWideChar(CP_ACP, 0, a, (int)alen, NULL, 0);
                    if (need > 0) {
                        std::vector<wchar_t> tmp((size_t)need + 1, 0);
                        MultiByteToWideChar(CP_ACP, 0, a, (int)alen, tmp.data(), need);
                        SanitizeInto(s.preview, PREVIEW_MAX, tmp.data(), (size_t)need, cfg.previewChars);
                        SecureZeroMemory(tmp.data(), tmp.size() * sizeof(wchar_t));
                    }
                }
                GlobalUnlock(h);
            }
        }
    } else if (hasDib || hasBmp) {
        s.kind = CK_IMAGE;
        s.kindLabel = L"IMAGE";
        if (hasDib) {
            HANDLE h = GetClipboardData(CF_DIB);
            if (h) {
                s.bytes = GlobalSize(h);
                s.bytesKnown = true;
                const BYTE* p = (const BYTE*)GlobalLock(h);
                if (p) {
                    if (s.bytes >= 12) {
                        LONG w  = *(const LONG*)(p + 4);
                        LONG hh = *(const LONG*)(p + 8);
                        if (hh < 0) hh = -hh;
                        wchar_t b[64];
                        swprintf_s(b, L"IMAGE %ldx%ld", w, hh);
                        s.kindLabel = b;
                    }
                    GlobalUnlock(h);
                }
            }
        }
    } else if (!s.formats.empty()) {
        s.kind = CK_OTHER;
        s.kindLabel = L"OTHER";
        std::wstring t = s.formats[0];
        if (s.formats.size() > 1) {
            wchar_t more[48];
            swprintf_s(more, L"   (+%zu formats)", s.formats.size() - 1);
            t += more;
        }
        SanitizeInto(s.preview, PREVIEW_MAX, t.c_str(), t.size(), cfg.previewChars);
    } else {
        s.kind = CK_EMPTY;
        s.kindLabel = L"EMPTY";
    }

    // The originating URL, when a browser supplied one.
    if (hasHtml && g_fmtHtml) {
        HANDLE h = GetClipboardData(g_fmtHtml);
        if (h) {
            SIZE_T sz = GlobalSize(h);
            const char* p = (const char*)GlobalLock(h);
            if (p) {
                ExtractSourceUrl(p, sz, s.sourceUrl, SOURCEURL_MAX);
                GlobalUnlock(h);
            }
        }
    }

    // Whether the source offered this as a copy or a move.
    if (hasDropEff) {
        DWORD de = 0;
        if (ReadDwordFormat(g_fmtDropEffect, de)) {
            if (de & 2)      s.dropEffect = L"move";
            else if (de & 1) s.dropEffect = L"copy";
            else if (de & 4) s.dropEffect = L"link";
        }
    }

    CloseClipboard();

    s.tick = GetTickCount64();
}
