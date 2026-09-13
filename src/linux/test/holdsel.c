/* holdsel - a test fixture that owns the X CLIPBOARD selection and serves one
 * payload, without forking.
 *
 * xclip and xsel both daemonise after opening their X connection, which leaves
 * the server holding the pid of a process that has already exited. That makes
 * them useless for testing whether WhoseClip resolves an owner correctly: the
 * lookup succeeds and then the pid does not exist. This holds the selection in
 * the foreground under its own pid, so the answer is checkable.
 *
 *   cc -O2 -o holdsel holdsel.c -lX11
 *   ./holdsel payload.txt &                 # served as text
 *   ./holdsel shot.png image/png &          # served as that type, and only that
 *   whoseclip --probe                       # Owner should be holdsel, this pid
 *
 * With no type it offers the usual text targets. With a type it offers exactly
 * that one, so a payload meant to be seen as an image is not also advertised as
 * text and classified as text.
 */
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: holdsel FILE [MIME-TYPE]\n");
        return 2;
    }
    const char *file = argv[1];
    int nmime = argc - 2;                     /* every argument after the file */

    FILE *f = fopen(file, "rb");
    if (!f) { fprintf(stderr, "holdsel: cannot open %s\n", file); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return 1; }
    unsigned char *payload = (unsigned char *)malloc((size_t)sz + 1);
    if (!payload) { fclose(f); return 1; }
    size_t plen = fread(payload, 1, (size_t)sz, f);
    fclose(f);
    payload[plen] = 0;

    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "holdsel: no display\n"); return 1; }

    Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), -10, -10, 1, 1, 0, 0, 0);

    Atom CLIPBOARD = XInternAtom(d, "CLIPBOARD", False);
    Atom TARGETS   = XInternAtom(d, "TARGETS", False);
    Atom TIMESTAMP = XInternAtom(d, "TIMESTAMP", False);
    Atom UTF8      = XInternAtom(d, "UTF8_STRING", False);
    Atom TEXTA     = XInternAtom(d, "TEXT", False);
    Atom PLAIN     = XInternAtom(d, "text/plain", False);

    /* What this owner claims to hold. Named types replace the text set rather
     * than joining it, so a payload meant to be seen as an image is not also
     * advertised as text and then classified as text. Naming several types is
     * how a real application behaves: a password manager offers its text and
     * the KDE hint together. */
    Atom offer[16];
    int noffer = 0;
    offer[noffer++] = TARGETS;
    offer[noffer++] = TIMESTAMP;
    if (nmime > 0) {
        for (int i = 0; i < nmime && noffer < 16; i++)
            offer[noffer++] = XInternAtom(d, argv[2 + i], False);
    } else {
        offer[noffer++] = UTF8;
        offer[noffer++] = XA_STRING;
        offer[noffer++] = TEXTA;
        offer[noffer++] = PLAIN;
    }

    XSetSelectionOwner(d, CLIPBOARD, w, CurrentTime);
    if (XGetSelectionOwner(d, CLIPBOARD) != w) {
        fprintf(stderr, "holdsel: could not take the selection\n");
        return 1;
    }
    printf("holdsel: pid %d owns CLIPBOARD as %s, %lu bytes\n",
           (int)getpid(), nmime > 0 ? argv[2] : "text", (unsigned long)plen);
    fflush(stdout);

    for (;;) {
        XEvent e;
        XNextEvent(d, &e);

        if (e.type == SelectionClear) break;          /* someone else took it */
        if (e.type != SelectionRequest) continue;

        XSelectionRequestEvent *r = &e.xselectionrequest;
        XSelectionEvent n;
        memset(&n, 0, sizeof n);
        n.type      = SelectionNotify;
        n.display   = r->display;
        n.requestor = r->requestor;
        n.selection = r->selection;
        n.target    = r->target;
        n.time      = r->time;
        n.property  = None;

        if (r->target == TARGETS) {
            XChangeProperty(d, r->requestor, r->property, XA_ATOM, 32,
                            PropModeReplace, (unsigned char *)offer, noffer);
            n.property = r->property;
        } else if (r->target == TIMESTAMP) {
            long t = CurrentTime;
            XChangeProperty(d, r->requestor, r->property, XA_INTEGER, 32,
                            PropModeReplace, (unsigned char *)&t, 1);
            n.property = r->property;
        } else {
            int serve = 0;
            for (int i = 0; i < noffer; i++)
                if (r->target == offer[i] && offer[i] != TARGETS && offer[i] != TIMESTAMP)
                    serve = 1;
            if (serve) {
                XChangeProperty(d, r->requestor, r->property, r->target, 8,
                                PropModeReplace, payload, (int)plen);
                n.property = r->property;
            }
        }

        XSendEvent(d, r->requestor, False, 0, (XEvent *)&n);
        XFlush(d);
    }

    free(payload);
    XCloseDisplay(d);
    return 0;
}
