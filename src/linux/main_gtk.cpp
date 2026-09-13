// WhoseClip for Linux. The strip, the tray icon and the menu.
//
// A parallel of src/main.cpp: same layout, same wording, same geometry in
// logical pixels, with GDI swapped for Cairo and DrawText for Pango. Where the
// two platforms genuinely differ the comment says why.
#include "whoseclip_linux.h"

#include <gtk/gtk.h>
#include <glib-unix.h>
#include <cairo.h>
#include <pango/pangocairo.h>

#ifdef HAVE_APPINDICATOR
#include <libayatana-appindicator/app-indicator.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

// Applications often write the clipboard in several steps. Reading it the
// instant the owner changes puts us in the middle of that, and on X11 it also
// means asking an owner to convert before it is ready to. Restarting the timer
// coalesces a burst of changes into a single read.
#define CAPTURE_DELAY_MS 120

// A middle dot separator. Written as an escape so this file stays pure ASCII
// and cannot be mangled by a compiler reading it in the wrong encoding.
static const char kDot[] = " \xC2\xB7 ";

static Config       g_cfg;
static ClipSnapshot g_snap;
static GtkWidget*   g_strip     = NULL;
static bool         g_paused    = false;
static int          g_hover     = 0;    // 0 none, 1 expand button, 2 close button
static guint        g_captureTimer = 0;
static guint        g_saveTimer = 0;
static bool         g_syncing   = false;   // menu items are being set, not clicked
static PangoFontDescription* g_fontHead = NULL;
static PangoFontDescription* g_fontBody = NULL;

// A press that has not yet moved far enough to be a drag. GTK delivers a plain
// button press before it delivers the double-click, so handing the press
// straight to the window manager would swallow every double-click on the strip.
static bool   g_maybeDrag = false;
static double g_pressX = 0, g_pressY = 0;
static guint32 g_pressTime = 0;
static guint   g_pressButton = 0;

// Check items on the indicator menu, which lives for the life of the process
// and therefore has to be told when the state it shows changes elsewhere.
static GtkWidget* g_miStrip  = NULL;
static GtkWidget* g_miExpand = NULL;
static GtkWidget* g_miPause  = NULL;

#ifdef HAVE_APPINDICATOR
static AppIndicator* g_ind = NULL;
static std::string   g_iconDir;
static int           g_iconState = -1;
#endif

// GTK scales the drawing surface for integer HiDPI itself, and Pango picks up
// the desktop text scale from the font size in points, so the geometry below is
// in logical pixels exactly as the Win32 build writes it at 96 dpi.
static int Sc(int v) { return v; }

// ---------------------------------------------------------------- presentation

static WcColor AccentColor()
{
    if (g_paused) return g_cfg.colIdle;
    if (g_snap.kind == CK_EMPTY || g_snap.kind == CK_LOCKED) return g_cfg.colIdle;
    return g_snap.external ? g_cfg.colForeign : g_cfg.colLocal;
}

static std::string HostOf(const char* url)
{
    const char* p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char* e = p;
    while (*e && *e != '/' && *e != '?') ++e;
    return std::string(p, (size_t)(e - p));
}

// full = the whole story, for the expanded strip and the tray tooltip, where
// there is room and the reader asked for detail. Compact = what earns its place
// in a glance: the accent colour already says "this came from another machine",
// so repeating that in words only pushes the process name out of view.
static std::string OriginText(bool full)
{
    if (g_snap.kind == CK_LOCKED) return "locked by " + g_snap.blocker;

    if (g_snap.ownerKnown) {
        std::string t = g_snap.external ? "via " : "from ";
        t += g_snap.ownerName;
        if (full && g_snap.external && !g_snap.originTag.empty())
            t += " (" + g_snap.originTag + ")";
        return t;
    }

    // With no owning process, a page a browser recorded is the only origin
    // there is, so it is worth the space even when collapsed. On Wayland the
    // originTag carries the reason the owner is missing, which is worth more
    // than the bare words "owner unknown".
    std::string t = g_snap.originTag.empty() ? "owner unknown" : g_snap.originTag;
    if (g_snap.sourceUrl[0]) {
        std::string h = HostOf(g_snap.sourceUrl);
        if (!h.empty()) t += "  [" + h + "]";
    }
    return t;
}

static std::string Line1Core()
{
    std::string t = g_cfg.label;
    t += "     ";
    if (g_paused) { t += "PAUSED"; return t; }
    t += g_snap.kindLabel;
    if (g_snap.bytesKnown && g_snap.kind != CK_EMPTY) t += "  " + SizeText(g_snap.bytes);
    return t;
}

static std::string Line1()
{
    std::string t = Line1Core();
    if (g_paused) return t;
    t += "     " + OriginText(false);
    return t;
}

static std::string MetaLine()
{
    if (g_paused)                return "monitoring paused";
    if (g_snap.kind == CK_EMPTY) return "nothing on the clipboard";

    std::string t = OriginText(true);
    if (!g_snap.dropEffect.empty()) t += std::string(kDot) + g_snap.dropEffect;
    if (g_snap.virtualFiles)        t += std::string(kDot) + "bytes stream on paste";
    if (g_snap.sensitive)           t += std::string(kDot) + "source asked not to be monitored";
    return t;
}

static std::string FormatsLine()
{
    if (g_snap.formats.empty()) return std::string();
    std::string t;
    for (size_t i = 0; i < g_snap.formats.size(); ++i) {
        if (i) t += kDot;
        t += g_snap.formats[i];
    }
    return t;
}

static std::string Line2()
{
    if (g_paused)                 return "monitoring paused";
    if (g_snap.kind == CK_LOCKED) return "could not read - the owner did not answer";
    if (g_snap.kind == CK_EMPTY)  return "clipboard empty";
    if (g_snap.preview[0])        return g_snap.preview;
    return "(no preview available)";
}

static std::string OneLine(const std::string& s)
{
    std::string t;
    t.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n') t += kDot;
        else              t += s[i];
    }
    return t;
}

static std::string CollapsedPreview()
{
    std::string s = Line2();
    if (g_snap.kind == CK_FILES && g_snap.fileCount > 1) {
        // The count is already in the header as FILES (n) a few pixels away, so
        // spend this line on the first name rather than repeating the number.
        size_t nl = s.find('\n');
        return (nl == std::string::npos) ? s : s.substr(0, nl);
    }
    return OneLine(s);
}

// ---------------------------------------------------------------- geometry

static int StripWidth()    { return Sc(470); }
static int StripTextLeft() { return Sc(37); }

struct Rect { int x, y, w, h; };

static Rect BtnClose(int w)
{
    int s = Sc(15), m = Sc(9);
    Rect r = { w - m - s, m, s, s };
    return r;
}

static Rect BtnExpand(int w)
{
    Rect c = BtnClose(w);
    int s = Sc(15), g = Sc(6);
    Rect r = { c.x - g - s, c.y, s, s };
    return r;
}

static bool HitRect(const Rect& r, int x, int y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static WcColor MixColor(WcColor a, WcColor b, int pctB)
{
    int r  = (WcR(a) * (100 - pctB) + WcR(b) * pctB) / 100;
    int g  = (WcG(a) * (100 - pctB) + WcG(b) * pctB) / 100;
    int bl = (WcB(a) * (100 - pctB) + WcB(b) * pctB) / 100;
    return WcRGB(r, g, bl);
}

static void SetSource(cairo_t* cr, WcColor c, double alpha = 1.0)
{
    cairo_set_source_rgba(cr, WcR(c) / 255.0, WcG(c) / 255.0, WcB(c) / 255.0, alpha);
}

// ------------------------------------------------------------------- fonts

static void MakeFonts()
{
    if (g_fontHead) { pango_font_description_free(g_fontHead); g_fontHead = NULL; }
    if (g_fontBody) { pango_font_description_free(g_fontBody); g_fontBody = NULL; }

    // Start from whatever the desktop is set to, so the strip looks like the
    // rest of the session rather than importing a Windows font name that is
    // not installed. Only the size and weight are ours.
    gchar* fontName = NULL;
    GtkSettings* st = gtk_settings_get_default();
    if (st) g_object_get(st, "gtk-font-name", &fontName, NULL);

    g_fontBody = fontName ? pango_font_description_from_string(fontName)
                          : pango_font_description_from_string("Sans");
    if (fontName) g_free(fontName);

    // The Win32 build asks for 12 pixels at 96 dpi, which is 9 points. Points
    // let the desktop text-scaling setting apply the way every other app does.
    pango_font_description_set_size(g_fontBody, 9 * PANGO_SCALE);
    pango_font_description_set_weight(g_fontBody, PANGO_WEIGHT_NORMAL);

    g_fontHead = pango_font_description_copy(g_fontBody);
    pango_font_description_set_weight(g_fontHead, PANGO_WEIGHT_SEMIBOLD);
}

enum WrapMode { WRAP_NONE, WRAP_WORD };
enum ElideMode { ELIDE_NONE, ELIDE_END, ELIDE_MIDDLE };

static PangoLayout* MakeLayout(cairo_t* cr, PangoFontDescription* font, const std::string& s,
                               int width, WrapMode wrap, ElideMode elide)
{
    PangoLayout* l = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(l, font);
    // Pango refuses to lay out invalid UTF-8, which would blank the strip
    // rather than show one bad character. SanitizeInto already guarantees valid
    // text for anything from the clipboard; this covers everything else.
    if (g_utf8_validate(s.c_str(), (gssize)s.size(), NULL))
        pango_layout_set_text(l, s.c_str(), (int)s.size());
    else
        pango_layout_set_text(l, "(unprintable)", -1);

    if (width > 0) pango_layout_set_width(l, width * PANGO_SCALE);
    pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_single_paragraph_mode(l, wrap == WRAP_NONE);
    if (wrap == WRAP_NONE) {
        pango_layout_set_ellipsize(l, elide == ELIDE_MIDDLE ? PANGO_ELLIPSIZE_MIDDLE
                                    : elide == ELIDE_END    ? PANGO_ELLIPSIZE_END
                                                            : PANGO_ELLIPSIZE_NONE);
    } else {
        pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_NONE);
    }
    return l;
}

// Height of a block of wrapped text, measured the same way it will be drawn.
static int MeasureWrapped(PangoFontDescription* font, const std::string& s, int width)
{
    if (s.empty() || width < 20) return 0;
    cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t* cr = cairo_create(surf);
    PangoLayout* l = MakeLayout(cr, font, s, width, WRAP_WORD, ELIDE_NONE);
    int w = 0, h = 0;
    pango_layout_get_pixel_size(l, &w, &h);
    g_object_unref(l);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    return h;
}

// ------------------------------------------------------------------ layout

struct StripLayout {
    int textLeft, textRight, textWidth;
    int headRight;
    int metaY,  metaH;
    int pathY,  pathH;
    int rule1Y;
    int bodyY,  bodyH;
    int rule2Y;
    int fmtY,   fmtH;
    int footY;
    int height;
};

static int WorkAreaHeight()
{
    GdkDisplay* d = gdk_display_get_default();
    if (!d) return 1080;
    GdkMonitor* m = NULL;
    if (g_strip && gtk_widget_get_window(g_strip))
        m = gdk_display_get_monitor_at_window(d, gtk_widget_get_window(g_strip));
    if (!m) m = gdk_display_get_primary_monitor(d);
    if (!m && gdk_display_get_n_monitors(d) > 0) m = gdk_display_get_monitor(d, 0);
    if (!m) return 1080;
    GdkRectangle wa;
    gdk_monitor_get_workarea(m, &wa);
    return wa.height;
}

static StripLayout ComputeLayout(int w)
{
    StripLayout L;
    memset(&L, 0, sizeof L);
    L.textLeft  = StripTextLeft();
    L.textRight = w - Sc(12);
    L.textWidth = L.textRight - L.textLeft;
    L.headRight = BtnExpand(w).x - Sc(8);

    if (!g_cfg.expanded) {
        // The Win32 build can hardcode this because its font is a known size.
        // Here the desktop may be scaling text, so the fixed design height is a
        // floor rather than the answer.
        int lineH = MeasureWrapped(g_fontBody, "Ag", L.textWidth);
        if (lineH <= 0) lineH = Sc(16);
        int need = Sc(7) + lineH + Sc(3) + lineH + Sc(8);
        L.height = (need > Sc(54)) ? need : Sc(54);
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
    int cap  = WorkAreaHeight() * 6 / 10;
    if (cap > minH && cap < maxH) maxH = cap;

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

// ----------------------------------------------------------------- painting

static void DrawText(cairo_t* cr, PangoFontDescription* font, const std::string& s,
                     int x, int y, int width, int height, WcColor col,
                     WrapMode wrap, ElideMode elide, bool rightAlign = false,
                     bool vcenter = false)
{
    if (s.empty()) return;
    PangoLayout* l = MakeLayout(cr, font, s, width, wrap, elide);
    if (rightAlign) pango_layout_set_alignment(l, PANGO_ALIGN_RIGHT);

    int tw = 0, th = 0;
    pango_layout_get_pixel_size(l, &tw, &th);
    int ty = y;
    if (vcenter && height > 0) ty = y + (height - th) / 2;

    cairo_save(cr);
    if (height > 0) {
        // Pango happily draws a partial last line; the Win32 build had to fight
        // DT_EDITCONTROL for the opposite reason. Clipping keeps the body text
        // inside the box the layout reserved for it either way.
        cairo_rectangle(cr, x, y, width, height);
        cairo_clip(cr);
    }
    SetSource(cr, col);
    cairo_move_to(cr, x, ty);
    pango_cairo_show_layout(cr, l);
    cairo_restore(cr);
    g_object_unref(l);
}

static void RoundRect(cairo_t* cr, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r,     r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0,          G_PI / 2);
    cairo_arc(cr, x + r,     y + h - r, r, G_PI / 2,   G_PI);
    cairo_arc(cr, x + r,     y + r,     r, G_PI,       3 * G_PI / 2);
    cairo_close_path(cr);
}

static void DrawButtons(cairo_t* cr, int w)
{
    for (int which = 0; which < 2; ++which) {
        Rect r   = (which == 0) ? BtnExpand(w) : BtnClose(w);
        bool hot = (g_hover == (which == 0 ? 1 : 2));

        if (hot) {
            SetSource(cr, MixColor(g_cfg.colBg, g_cfg.colFg, 16));
            cairo_rectangle(cr, r.x - Sc(3), r.y - Sc(3), r.w + Sc(6), r.h + Sc(6));
            cairo_fill(cr);
        }

        SetSource(cr, hot ? g_cfg.colFg : g_cfg.colDim);
        cairo_set_line_width(cr, 1.4);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

        double cx = r.x + r.w / 2.0;
        double cy = r.y + r.h / 2.0;
        double d  = Sc(4);

        if (which == 1) {
            cairo_move_to(cr, cx - d, cy - d);
            cairo_line_to(cr, cx + d, cy + d);
            cairo_move_to(cr, cx + d, cy - d);
            cairo_line_to(cr, cx - d, cy + d);
        } else if (g_cfg.expanded) {
            cairo_move_to(cr, cx - d, cy + d / 2);
            cairo_line_to(cr, cx,     cy - d / 2);
            cairo_line_to(cr, cx + d, cy + d / 2);
        } else {
            cairo_move_to(cr, cx - d, cy - d / 2);
            cairo_line_to(cr, cx,     cy + d / 2);
            cairo_line_to(cr, cx + d, cy - d / 2);
        }
        cairo_stroke(cr);
    }
}

static gboolean OnDraw(GtkWidget* widget, cairo_t* cr, gpointer)
{
    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    int w = alloc.width, h = alloc.height;

    GdkScreen* scr = gtk_widget_get_screen(widget);
    bool composited = scr && gdk_screen_is_composited(scr);
    double radius = Sc(10);

    // The Win32 build clips the window to a rounded region. Wayland and X11
    // both do that with an alpha channel instead, which needs a compositor;
    // without one the corners stay square rather than turning black.
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    if (composited) {
        cairo_set_source_rgba(cr, 0, 0, 0, 0);
        cairo_paint(cr);
        RoundRect(cr, 0, 0, w, h, radius);
        cairo_clip(cr);
    }
    SetSource(cr, g_cfg.colBg);
    cairo_paint(cr);
    cairo_restore(cr);

    if (composited) {
        RoundRect(cr, 0, 0, w, h, radius);
        cairo_clip(cr);
    }

    WcColor accent = AccentColor();

    // Accent bar down the left edge.
    SetSource(cr, accent);
    cairo_rectangle(cr, 0, 0, Sc(4), h);
    cairo_fill(cr);

    // Status dot.
    SetSource(cr, accent);
    cairo_arc(cr, Sc(23), Sc(18), Sc(5), 0, 2 * G_PI);
    cairo_fill(cr);

    StripLayout L = ComputeLayout(w);

    std::string age = (!g_paused && g_snap.kind != CK_EMPTY) ? AgeText(g_snap.tick) : std::string();

    // Header. Collapsed carries the origin too and may ellipsize it; expanded
    // keeps it short because the origin gets its own full-width line below.
    std::string l1 = g_cfg.expanded ? Line1Core() : Line1();
    DrawText(cr, g_fontHead, l1, L.textLeft, Sc(7), L.headRight - L.textLeft, Sc(19),
             g_cfg.colFg, WRAP_NONE, ELIDE_END, false, true);

    if (!g_cfg.expanded) {
        int right = L.textRight;
        if (!age.empty()) {
            // Measured against the context we are already drawing into, so the
            // preview knows exactly how much room the age has taken on the right.
            PangoLayout* m = MakeLayout(cr, g_fontBody, age, 0, WRAP_NONE, ELIDE_NONE);
            int aw = 0, ah = 0;
            pango_layout_get_pixel_size(m, &aw, &ah);
            (void)ah;
            g_object_unref(m);

            DrawText(cr, g_fontBody, age, L.textLeft, Sc(26), L.textRight - L.textLeft, Sc(20),
                     g_cfg.colDim, WRAP_NONE, ELIDE_NONE, true, true);
            right -= (aw + Sc(10));
        }
        DrawText(cr, g_fontBody, CollapsedPreview(), L.textLeft, Sc(26), right - L.textLeft,
                 Sc(20), g_cfg.colDim, WRAP_NONE, ELIDE_END, false, true);
    } else {
        DrawText(cr, g_fontBody, MetaLine(), L.textLeft, L.metaY, L.textWidth, L.metaH,
                 g_snap.external ? accent : g_cfg.colFg, WRAP_WORD, ELIDE_NONE);

        if (L.pathH > 0) {
            DrawText(cr, g_fontBody, g_snap.ownerPath, L.textLeft, L.pathY, L.textWidth,
                     L.pathH, g_cfg.colDim, WRAP_NONE, ELIDE_MIDDLE, false, true);
        }

        SetSource(cr, MixColor(g_cfg.colBg, g_cfg.colFg, 14));
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, L.textLeft, L.rule1Y + 0.5);
        cairo_line_to(cr, L.textRight, L.rule1Y + 0.5);
        cairo_move_to(cr, L.textLeft, L.rule2Y + 0.5);
        cairo_line_to(cr, L.textRight, L.rule2Y + 0.5);
        cairo_stroke(cr);

        DrawText(cr, g_fontBody, Line2(), L.textLeft, L.bodyY, L.textWidth, L.bodyH,
                 g_cfg.colFg, WRAP_WORD, ELIDE_NONE);

        if (L.fmtH > 0) {
            DrawText(cr, g_fontBody, FormatsLine(), L.textLeft, L.fmtY, L.textWidth, L.fmtH,
                     g_cfg.colDim, WRAP_WORD, ELIDE_NONE);
        }

        if (!age.empty()) {
            DrawText(cr, g_fontBody, age, L.textLeft, L.footY, L.textWidth, Sc(18),
                     g_cfg.colDim, WRAP_NONE, ELIDE_NONE, true, true);
        }
        if (g_snap.sourceUrl[0]) {
            DrawText(cr, g_fontBody, g_snap.sourceUrl, L.textLeft, L.footY,
                     L.textWidth - Sc(48), Sc(18), g_cfg.colDim, WRAP_NONE, ELIDE_END, false, true);
        }
    }

    DrawButtons(cr, w);
    return TRUE;
}

// --------------------------------------------------------------------- tray

static std::string Diagnostics();

#ifdef HAVE_APPINDICATOR
// AppIndicator takes an icon by name from a theme directory, not a bitmap, so
// the coloured letter the Win32 tray draws in memory has to exist as a file.
// These go in the runtime directory, which is tmpfs and is cleared at logout.
// They are the only thing besides the ini that WhoseClip writes, and they
// contain nothing but a letter and a colour.
static bool WriteTrayIcon(const std::string& path, char letter, WcColor bg)
{
    const int S = 64;
    cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, S, S);
    cairo_t* cr = cairo_create(surf);

    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    SetSource(cr, bg);
    RoundRect(cr, 4, 4, S - 8, S - 8, 12);
    cairo_fill(cr);

    char txt[2] = { letter, 0 };
    PangoLayout* l = pango_cairo_create_layout(cr);
    PangoFontDescription* fd = pango_font_description_from_string("Sans Bold");
    pango_font_description_set_absolute_size(fd, 36 * PANGO_SCALE);
    pango_layout_set_font_description(l, fd);
    pango_layout_set_text(l, txt, -1);
    int tw = 0, th = 0;
    pango_layout_get_pixel_size(l, &tw, &th);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_move_to(cr, (S - tw) / 2.0, (S - th) / 2.0);
    pango_cairo_show_layout(cr, l);
    pango_font_description_free(fd);
    g_object_unref(l);

    cairo_destroy(cr);
    cairo_status_t st = cairo_surface_write_to_png(surf, path.c_str());
    cairo_surface_destroy(surf);
    return st == CAIRO_STATUS_SUCCESS;
}

static bool BuildTrayIcons()
{
    const char* rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || !*rt) rt = "/tmp";
    g_iconDir = std::string(rt) + "/whoseclip";
    if (mkdir(g_iconDir.c_str(), 0700) != 0 && access(g_iconDir.c_str(), W_OK) != 0)
        return false;

    char letter = g_cfg.label.empty() ? 'W' : g_cfg.label[0];
    if (letter >= 'a' && letter <= 'z') letter = (char)(letter - 32);

    return WriteTrayIcon(g_iconDir + "/wc-local.png",   letter, g_cfg.colLocal)
        && WriteTrayIcon(g_iconDir + "/wc-foreign.png", letter, g_cfg.colForeign)
        && WriteTrayIcon(g_iconDir + "/wc-idle.png",    letter, g_cfg.colIdle);
}
#endif

static void UpdateTray()
{
#ifdef HAVE_APPINDICATOR
    if (!g_ind) return;

    int state = g_paused || g_snap.kind == CK_EMPTY || g_snap.kind == CK_LOCKED ? 2
              : g_snap.external ? 1 : 0;

    if (state != g_iconState) {
        const char* name = state == 0 ? "wc-local" : state == 1 ? "wc-foreign" : "wc-idle";
        app_indicator_set_icon_full(g_ind, name, "WhoseClip");
        g_iconState = state;
    }

    std::string tip = Line1Core();
    if (!g_paused) tip += "\n" + OriginText(true);
    if (!g_paused && g_snap.kind != CK_EMPTY) tip += "\n" + OneLine(Line2());
    app_indicator_set_title(g_ind, tip.c_str());
#endif
}

// ------------------------------------------------------------------- strip

// A window manager sends a configure event for every step of a drag. Writing
// the ini on each one would mean hundreds of writes to move the strip once, so
// the position is written a moment after the movement stops. The Win32 build
// gets this for free from WM_EXITSIZEMOVE, which has no X11 equivalent.
static gboolean OnSaveTimer(gpointer)
{
    g_saveTimer = 0;
    ConfigSaveUi(g_cfg);
    return G_SOURCE_REMOVE;
}

static void SaveSoon()
{
    if (g_saveTimer) g_source_remove(g_saveTimer);
    g_saveTimer = g_timeout_add(500, OnSaveTimer, NULL);
}

// The indicator menu outlives every change made from the strip, so its check
// marks have to be pushed rather than read. The guard stops a programmatic set
// from looking like a click and toggling the setting straight back.
static void SyncMenu()
{
    g_syncing = true;
    if (g_miStrip)  gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(g_miStrip),  g_cfg.showStrip);
    if (g_miExpand) gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(g_miExpand), g_cfg.expanded);
    if (g_miPause)  gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(g_miPause),  g_paused);
    if (g_miExpand) gtk_widget_set_sensitive(g_miExpand, g_cfg.showStrip);
    g_syncing = false;
}

static void ApplyStripSize()
{
    if (!g_strip) return;
    int w = StripWidth();
    int h = ComputeLayout(w).height;
    gtk_widget_set_size_request(g_strip, w, h);
    gtk_window_resize(GTK_WINDOW(g_strip), w, h);
    gtk_widget_queue_draw(g_strip);
}

static void PlaceStrip()
{
    if (!g_strip) return;
    if (g_cfg.stripX != POS_AUTO && g_cfg.stripY != POS_AUTO) {
        gtk_window_move(GTK_WINDOW(g_strip), g_cfg.stripX, g_cfg.stripY);
        return;
    }
    GdkDisplay* d = gdk_display_get_default();
    GdkMonitor* m = d ? gdk_display_get_primary_monitor(d) : NULL;
    if (!m && d && gdk_display_get_n_monitors(d) > 0) m = gdk_display_get_monitor(d, 0);
    if (!m) return;
    GdkRectangle wa;
    gdk_monitor_get_workarea(m, &wa);
    int w = StripWidth();
    gtk_window_move(GTK_WINDOW(g_strip), wa.x + wa.width - w - Sc(16), wa.y + Sc(16));
}

static void ShowMenu(GdkEvent* ev);
static GtkWidget* BuildMenu(bool keep);

static gboolean OnButtonPress(GtkWidget* widget, GdkEventButton* e, gpointer)
{
    if (e->button == 3) { ShowMenu((GdkEvent*)e); return TRUE; }
    if (e->button != 1) return FALSE;

    GtkAllocation a;
    gtk_widget_get_allocation(widget, &a);
    int x = (int)e->x, y = (int)e->y;

    if (e->type == GDK_2BUTTON_PRESS) {
        g_maybeDrag = false;
        g_cfg.expanded = !g_cfg.expanded;
        ConfigSaveUi(g_cfg);
        SyncMenu();
        ApplyStripSize();
        return TRUE;
    }
    if (HitRect(BtnClose(a.width), x, y)) {
        g_cfg.showStrip = false;
        ConfigSaveUi(g_cfg);
        SyncMenu();
        gtk_widget_hide(g_strip);
        return TRUE;
    }
    if (HitRect(BtnExpand(a.width), x, y)) {
        g_cfg.expanded = !g_cfg.expanded;
        ConfigSaveUi(g_cfg);
        SyncMenu();
        ApplyStripSize();
        return TRUE;
    }

    // Anywhere else may become a drag, but only once the pointer has actually
    // moved. Starting it here would consume the press that the double-click is
    // built from, and expanding by double-click is a documented gesture.
    g_maybeDrag   = true;
    g_pressX      = e->x_root;
    g_pressY      = e->y_root;
    g_pressTime   = e->time;
    g_pressButton = e->button;
    return TRUE;
}

static gboolean OnButtonRelease(GtkWidget*, GdkEventButton*, gpointer)
{
    g_maybeDrag = false;
    return FALSE;
}

static gboolean OnMotion(GtkWidget* widget, GdkEventMotion* e, gpointer)
{
    if (g_maybeDrag && (e->state & GDK_BUTTON1_MASK)) {
        double dx = e->x_root - g_pressX, dy = e->y_root - g_pressY;
        if (dx * dx + dy * dy > 16.0) {          // four pixels in any direction
            g_maybeDrag = false;
            g_hover = 0;
            // Hand the drag to the window manager from the original press, so
            // it stays smooth and behaves the same way under a tiling WM.
            gtk_window_begin_move_drag(GTK_WINDOW(widget), (int)g_pressButton,
                                       (int)g_pressX, (int)g_pressY, g_pressTime);
            return TRUE;
        }
    }

    GtkAllocation a;
    gtk_widget_get_allocation(widget, &a);
    int was = g_hover;
    g_hover = HitRect(BtnExpand(a.width), (int)e->x, (int)e->y) ? 1
            : HitRect(BtnClose(a.width),  (int)e->x, (int)e->y) ? 2 : 0;
    if (g_hover != was) gtk_widget_queue_draw(widget);
    return FALSE;
}

static gboolean OnLeave(GtkWidget* widget, GdkEventCrossing*, gpointer)
{
    if (g_hover) { g_hover = 0; gtk_widget_queue_draw(widget); }
    return FALSE;
}

static gboolean OnConfigure(GtkWidget*, GdkEventConfigure* e, gpointer)
{
    // Remember where the user dragged it, the way the Win32 build does on
    // WM_EXITSIZEMOVE. Under Wayland these coordinates are meaningless and the
    // compositor ignores the move on the next run, which is a Wayland fact
    // rather than something worth working around.
    if (e->x != g_cfg.stripX || e->y != g_cfg.stripY) {
        g_cfg.stripX = e->x;
        g_cfg.stripY = e->y;
        SaveSoon();
    }
    return FALSE;
}

static void CreateStrip()
{
    g_strip = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g_strip), "WhoseClip");
    gtk_window_set_decorated(GTK_WINDOW(g_strip), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(g_strip), FALSE);
    gtk_window_set_keep_above(GTK_WINDOW(g_strip), TRUE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(g_strip), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(g_strip), TRUE);
    gtk_window_set_type_hint(GTK_WINDOW(g_strip), GDK_WINDOW_TYPE_HINT_UTILITY);
    gtk_widget_set_app_paintable(g_strip, TRUE);

    GdkScreen* scr = gtk_widget_get_screen(g_strip);
    GdkVisual* rgba = gdk_screen_get_rgba_visual(scr);
    if (rgba) gtk_widget_set_visual(g_strip, rgba);

    gtk_widget_add_events(g_strip, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                                   GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK);

    g_signal_connect(g_strip, "draw",                 G_CALLBACK(OnDraw), NULL);
    g_signal_connect(g_strip, "button-press-event",   G_CALLBACK(OnButtonPress), NULL);
    g_signal_connect(g_strip, "button-release-event", G_CALLBACK(OnButtonRelease), NULL);
    g_signal_connect(g_strip, "motion-notify-event",  G_CALLBACK(OnMotion), NULL);
    g_signal_connect(g_strip, "leave-notify-event",  G_CALLBACK(OnLeave), NULL);
    g_signal_connect(g_strip, "configure-event",     G_CALLBACK(OnConfigure), NULL);
    g_signal_connect(g_strip, "delete-event",        G_CALLBACK(gtk_widget_hide_on_delete), NULL);

    ApplyStripSize();
    gtk_widget_show_all(g_strip);
    PlaceStrip();
}

// -------------------------------------------------------------------- menu

static std::string Diagnostics()
{
    std::string t = "WhoseClip " WC_VERSION " by " WC_AUTHOR "\n" WC_URL "\nMIT licensed\n\n";
    t += "Machine label: " + g_cfg.label + "\n";

    const char* sess = ClipSession() == SESS_X11      ? "X11"
                     : ClipSession() == SESS_XWAYLAND ? "Wayland (via XWayland)"
                     : ClipSession() == SESS_WAYLAND  ? "Wayland (no X bridge)" : "none";
    t += std::string("Session: ") + sess + "\n";
    if (ClipSessionNote()[0]) t += std::string("Note: ") + ClipSessionNote() + "\n";

    char b[256];
    snprintf(b, sizeof b, "Sequence: %lu\n", g_snap.seq);
    t += b;
    t += "Kind: " + g_snap.kindLabel + "\n";
    if (g_snap.bytesKnown) t += "Size: " + SizeText(g_snap.bytes) + "\n";
    t += "Age: " + AgeText(g_snap.tick) + "\n";
    if (g_snap.ownerKnown) {
        // Built by concatenation rather than into a fixed buffer: an owner name
        // can be a full path component and would be truncated into b.
        snprintf(b, sizeof b, " (pid %d)\n", (int)g_snap.ownerPid);
        t += std::string("Owner: ") + g_snap.ownerName + b;
        t += std::string("Owner path: ") + g_snap.ownerPath + "\n";
    } else {
        t += "Owner: unknown\n";
    }
    t += "Origin: " + g_snap.originTag + "\n";
    if (g_snap.virtualFiles)        t += "Virtual files: yes (bytes stream on paste)\n";
    if (!g_snap.dropEffect.empty()) t += "Drop effect: " + g_snap.dropEffect + "\n";
    if (g_snap.sourceUrl[0])        t += std::string("SourceURL: ") + g_snap.sourceUrl + "\n";
    if (g_snap.sensitive)           t += "Source asked not to be monitored: " + g_snap.sensitiveReason + "\n";
    if (!g_snap.blocker.empty())    t += "Blocked by: " + g_snap.blocker + "\n";
    t += "Formats:\n";
    for (size_t i = 0; i < g_snap.formats.size(); ++i) t += "  " + g_snap.formats[i] + "\n";
    return t;
}

static void ShowInfo(const std::string& title, const std::string& body)
{
    GtkWidget* d = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO,
                                          GTK_BUTTONS_OK, "%s", title.c_str());
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", body.c_str());
    gtk_window_set_title(GTK_WINDOW(d), "WhoseClip");
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

static void Recapture();

static void OnMenuStrip(GtkCheckMenuItem* it, gpointer)
{
    if (g_syncing) return;
    g_cfg.showStrip = gtk_check_menu_item_get_active(it);
    ConfigSaveUi(g_cfg);
    SyncMenu();
    if (!g_strip) { if (g_cfg.showStrip) CreateStrip(); return; }
    if (g_cfg.showStrip) { gtk_widget_show(g_strip); PlaceStrip(); }
    else                 gtk_widget_hide(g_strip);
}

static void OnMenuExpand(GtkCheckMenuItem* it, gpointer)
{
    if (g_syncing) return;
    g_cfg.expanded = gtk_check_menu_item_get_active(it);
    ConfigSaveUi(g_cfg);
    SyncMenu();
    ApplyStripSize();
}

static void OnMenuPause(GtkCheckMenuItem* it, gpointer)
{
    if (g_syncing) return;
    g_paused = gtk_check_menu_item_get_active(it);
    SyncMenu();
    if (!g_paused) Recapture();
    else { UpdateTray(); if (g_strip) gtk_widget_queue_draw(g_strip); }
}

static void OnMenuFormats(GtkMenuItem*, gpointer)
{
    std::string t;
    if (g_snap.formats.empty()) {
        t = "The clipboard is empty, or holds no readable formats.";
    } else {
        for (size_t i = 0; i < g_snap.formats.size(); ++i) t += "    " + g_snap.formats[i] + "\n";
        t += "\n" + OriginText(true);
    }
    ShowInfo("Formats currently on the clipboard", t);
}

static void OnMenuDiag(GtkMenuItem*, gpointer)
{
    std::string d = Diagnostics();
    GtkClipboard* cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(cb, d.c_str(), (gint)d.size());
    gtk_clipboard_store(cb);
}

static void OnMenuExit(GtkMenuItem*, gpointer) { gtk_main_quit(); }

// keep = this menu lives for the life of the process (the indicator owns it),
// so its items are worth remembering and pushing state into later. A menu built
// for one right-click is destroyed on selection-done, and remembering its items
// would leave SyncMenu holding freed widgets.
static GtkWidget* BuildMenu(bool keep)
{
    GtkWidget* m = gtk_menu_new();

    GtkWidget* mi = gtk_check_menu_item_new_with_label("Show strip");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi), g_cfg.showStrip);
    g_signal_connect(mi, "toggled", G_CALLBACK(OnMenuStrip), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), mi);
    if (keep) g_miStrip = mi;

    mi = gtk_check_menu_item_new_with_label("Expand strip");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi), g_cfg.expanded);
    gtk_widget_set_sensitive(mi, g_cfg.showStrip);
    g_signal_connect(mi, "toggled", G_CALLBACK(OnMenuExpand), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), mi);
    if (keep) g_miExpand = mi;

    mi = gtk_check_menu_item_new_with_label("Pause monitoring");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi), g_paused);
    g_signal_connect(mi, "toggled", G_CALLBACK(OnMenuPause), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), mi);
    if (keep) g_miPause = mi;

    gtk_menu_shell_append(GTK_MENU_SHELL(m), gtk_separator_menu_item_new());

    mi = gtk_menu_item_new_with_label("Clipboard formats...");
    g_signal_connect(mi, "activate", G_CALLBACK(OnMenuFormats), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), mi);

    mi = gtk_menu_item_new_with_label("Copy diagnostics (replaces clipboard)");
    g_signal_connect(mi, "activate", G_CALLBACK(OnMenuDiag), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), mi);

    gtk_menu_shell_append(GTK_MENU_SHELL(m), gtk_separator_menu_item_new());

    std::string about = std::string("WhoseClip " WC_VERSION) + kDot + g_cfg.label;
    mi = gtk_menu_item_new_with_label(about.c_str());
    gtk_widget_set_sensitive(mi, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), mi);

    // Plain text, not a link. Opening a browser from here would mean shelling
    // out to a URL, and the point of this binary is that it does not reach out.
    std::string by = std::string("by " WC_AUTHOR) + kDot + WC_URL;
    mi = gtk_menu_item_new_with_label(by.c_str());
    gtk_widget_set_sensitive(mi, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), mi);

    mi = gtk_menu_item_new_with_label("Exit");
    g_signal_connect(mi, "activate", G_CALLBACK(OnMenuExit), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), mi);

    gtk_widget_show_all(m);
    return m;
}

static void ShowMenu(GdkEvent* ev)
{
    GtkWidget* m = BuildMenu(false);
    g_signal_connect(m, "selection-done", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_menu_popup_at_pointer(GTK_MENU(m), ev);
}

// -------------------------------------------------------------- capture loop

static void Recapture()
{
    if (g_paused) return;
    SnapshotCapture(g_cfg, g_snap);
    UpdateTray();
    if (!g_strip) return;
    if (g_cfg.expanded) ApplyStripSize();
    else                gtk_widget_queue_draw(g_strip);
}

static gboolean OnCaptureTimer(gpointer)
{
    g_captureTimer = 0;
    Recapture();
    return G_SOURCE_REMOVE;
}

static void ScheduleCapture()
{
    if (g_captureTimer) g_source_remove(g_captureTimer);
    g_captureTimer = g_timeout_add(CAPTURE_DELAY_MS, OnCaptureTimer, NULL);
}

// The X connection carries the XFixes owner-change events. Folding its fd into
// the GTK main loop means no polling thread and no missed change.
static gboolean OnXReadable(gint, GIOCondition, gpointer)
{
    ClipDrainEvents();
    if (ClipPending() && !g_paused) ScheduleCapture();
    return G_SOURCE_CONTINUE;
}

// The age text counts upward, so the strip needs a redraw even when nothing
// about the clipboard has changed.
static gboolean OnTick(gpointer)
{
    if (g_strip && gtk_widget_get_visible(g_strip) && !g_cfg.expanded)
        gtk_widget_queue_draw(g_strip);
    return G_SOURCE_CONTINUE;
}

// -------------------------------------------------------------------- main

int main(int argc, char** argv)
{
    bool probe = false;

    ConfigLoad(g_cfg);
    ConfigApplyArgs(g_cfg, argc, argv);

    // Answered before gtk_init, which fails outright with no display. Asking a
    // program what its options are should work over a plain ssh connection.
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("WhoseClip " WC_VERSION " - what is on this machine's clipboard, and where it came from.\n"
                   "  --label NAME            name shown in the strip and tray\n"
                   "  --preview N             characters of preview to keep\n"
                   "  --color-local HEX       accent for content copied here (RRGGBB)\n"
                   "  --color-foreign HEX     accent for content from another machine\n"
                   "  --strip|--no-strip      show or hide the strip at startup\n"
                   "  --expanded|--collapsed  which state the strip starts in\n"
                   "  --probe                 print one reading to stdout and exit\n"
                   "Config: %s\n", g_cfg.iniPath.c_str());
            return 0;
        }
        if (!strcmp(argv[i], "--version")) { printf("WhoseClip " WC_VERSION "\n"); return 0; }
        if (!strcmp(argv[i], "--probe")) probe = true;
    }

    // This is an X11 program whether or not the session is. The clipboard can
    // only be inspected over X, so the window has to live on the same X display
    // the readings come from. Left to itself GDK prefers its Wayland backend
    // whenever a compositor socket exists, which would put the strip on one
    // display server while the content it describes was read from another.
    gdk_set_allowed_backends("x11");

    if (!gtk_init_check(&argc, &argv)) {
        fprintf(stderr, "WhoseClip: no X display found. Set DISPLAY, or start this inside "
                        "a desktop session. On Wayland this needs XWayland, which a normal "
                        "GNOME or KDE session provides.\n");
        return 1;
    }

    MakeFonts();
    SnapshotClear(g_snap);

    if (!ClipInit()) {
        // Nothing to monitor. Say so plainly and leave, rather than sitting in
        // the tray reporting an empty clipboard that is really an unreadable one.
        fprintf(stderr, "WhoseClip: %s\n", ClipSessionNote());
        if (ClipSession() == SESS_WAYLAND) {
            ShowInfo("WhoseClip cannot read this session's clipboard", ClipSessionNote());
        }
        return 1;
    }

    if (ClipSessionNote()[0]) fprintf(stderr, "WhoseClip: %s\n", ClipSessionNote());

    // One reading, printed, no window and no tray. This is how you check that
    // the owning process is reported the way this program assumes it is - copy
    // something on the host, run this in the guest, and read the Owner line.
    if (probe) {
        SnapshotCapture(g_cfg, g_snap);
        std::string d = Diagnostics();
        // The preview is deliberately absent from the diagnostics dump, which
        // goes onto the clipboard and would otherwise quote the clipboard back
        // at itself. Printing it here is the point of probing, though: it is how
        // you confirm that content crossed from another machine at all.
        d += "Preview:\n";
        if (g_snap.preview[0]) {
            d += "  ";
            for (const char* p = g_snap.preview; *p; ++p) {
                d += *p;
                if (*p == '\n') d += "  ";
            }
            d += "\n";
        } else {
            d += "  (nothing)\n";
        }
        fputs(d.c_str(), stdout);
        SnapshotClear(g_snap);
        ClipShutdown();
        return 0;
    }

#ifdef HAVE_APPINDICATOR
    if (BuildTrayIcons()) {
        // Order matters. The icon theme path has to be known before the icon
        // name can resolve, and the menu has to exist before the status goes
        // active: on a panel with no StatusNotifier watcher - Xfce with the
        // older systray, among others - going active builds a legacy tray icon
        // there and then, and doing that without a menu trips an assertion
        // inside the library.
        g_ind = app_indicator_new_with_path("whoseclip", "wc-idle",
                                            APP_INDICATOR_CATEGORY_APPLICATION_STATUS,
                                            g_iconDir.c_str());
        if (g_ind) {
            app_indicator_set_menu(g_ind, GTK_MENU(BuildMenu(true)));
            app_indicator_set_title(g_ind, "WhoseClip");
            app_indicator_set_status(g_ind, APP_INDICATOR_STATUS_ACTIVE);
        }
    }
#endif

    if (g_cfg.showStrip) CreateStrip();

    int fd = ClipFd();
    if (fd >= 0) g_unix_fd_add(fd, G_IO_IN, OnXReadable, NULL);
    g_timeout_add_seconds(1, OnTick, NULL);

    Recapture();
    gtk_main();

    // The two content buffers are the only place clipboard data ever landed.
    SnapshotClear(g_snap);
    ClipShutdown();
    return 0;
}
