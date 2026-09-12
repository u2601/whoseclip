// WhoseClip - see what is on this machine's clipboard, and where it came from.
// Single-machine, current-clipboard-only. Never stores clipboard content.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>

#define WC_VERSION       L"1.0.0"
#define WC_AUTHOR        L"u2601"
#define WC_URL           L"github.com/u2601/whoseclip"
// Large enough that the expanded strip has real content to show without
// needing a second, deeper read of the clipboard when it is opened.
#define PREVIEW_MAX      4096
#define SOURCEURL_MAX    512
#define OWNER_MAX        MAX_PATH

enum ClipKind { CK_EMPTY, CK_TEXT, CK_IMAGE, CK_FILES, CK_OTHER, CK_LOCKED };

// One point-in-time view of the clipboard. Replaced wholesale on every change;
// the two fixed buffers below are the only place content ever lands, and they
// are wiped with SecureZeroMemory before reuse and on exit.
struct ClipSnapshot {
    ULONG        seq;            // GetClipboardSequenceNumber at capture
    ULONGLONG    tick;           // GetTickCount64 at capture
    ClipKind     kind;
    std::wstring kindLabel;      // "TEXT", "IMAGE 1920x1080", "FILES (3)"
    ULONGLONG    bytes;          // widened: a virtual file set can exceed 4 GB
    bool         bytesKnown;

    DWORD        ownerPid;
    wchar_t      ownerName[OWNER_MAX];   // "vmtoolsd.exe"
    wchar_t      ownerPath[OWNER_MAX];   // full path
    bool         ownerKnown;
    bool         external;               // owner is a known cross-machine relay
    std::wstring originTag;              // "from host / another VM", "local", ...

    bool         sensitive;              // app asked not to be monitored;
    std::wstring sensitiveReason;        // recorded, but content is shown anyway

    bool         virtualFiles;           // files described but not yet on disk
    UINT         fileCount;              // items in a file drop, 0 otherwise
    std::wstring dropEffect;             // "copy" / "move", when the source said

    wchar_t      preview[PREVIEW_MAX];
    wchar_t      sourceUrl[SOURCEURL_MAX];   // from CF_HTML "SourceURL:"

    std::vector<std::wstring> formats;
    std::wstring blocker;        // process holding the clipboard, if open failed
};

struct Config {
    std::wstring label;          // machine label shown in the UI
    COLORREF colLocal, colForeign, colIdle, colBg, colFg, colDim;
    int      stripX, stripY;     // INT_MIN = auto (top-right of work area)
    bool     showStrip;
    bool     expanded;           // strip grown to show the whole preview
    int      previewChars;
    std::vector<std::wstring> extraExternals;   // extra exe names, lowercase
    std::wstring iniPath;
};

void ConfigLoad(Config& c);
void ConfigApplyArgs(Config& c);
void ConfigSaveUi(const Config& c);

void ClipInit();
void SnapshotClear(ClipSnapshot& s);
void SnapshotCapture(HWND hwnd, const Config& cfg, ClipSnapshot& s);

std::wstring AgeText(ULONGLONG tick);
std::wstring SizeText(ULONGLONG n);
