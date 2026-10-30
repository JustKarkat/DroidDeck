/* xprobe.c — minimal X11 client for the no-gamescope checkpoint.
 *
 * Proves the two things handoff §30 needs before Steam is involved:
 *   1. an X window maps and is composited (the compositor logs "window opened",
 *      and the screen shows the colour blocks),
 *   2. pointer/keyboard events reach the X client through Xwayland's seat.
 *
 * Writes /tmp/xprobe.log; runs for N seconds (default 20) or until killed.
 * Usage: xprobe [display] [seconds]
 */
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static FILE *logf(void) {
    static FILE *f;
    if (!f) f = fopen("/tmp/xprobe.log", "a");
    return f ? f : stderr;
}

static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    FILE *f = logf();
    fprintf(f, "[xprobe] ");
    vfprintf(f, fmt, ap);
    fprintf(f, "\n");
    fflush(f);
    va_end(ap);
}

int main(int argc, char **argv) {
    const char *dpy_name = argc > 1 ? argv[1] : NULL;
    int seconds = argc > 2 ? atoi(argv[2]) : 20;

    Display *d = XOpenDisplay(dpy_name);
    if (!d) {
        say("XOpenDisplay(%s) FAILED", dpy_name ? dpy_name : "$DISPLAY");
        return 1;
    }
    int scr = DefaultScreen(d);
    int W = 480, H = 270;
    Window root = RootWindow(d, scr);
    Window w = XCreateSimpleWindow(d, root, 40, 40, W, H, 0,
                                   BlackPixel(d, scr), WhitePixel(d, scr));
    XSelectInput(d, w, ExposureMask | KeyPressMask | KeyReleaseMask |
                          ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                          StructureNotifyMask | FocusChangeMask);
    XStoreName(d, w, "xprobe");
    XMapWindow(d, w);
    XFlush(d);

    say("opened window %#lx on %s (%dx%d), screen %dx%d", w,
        DisplayString(d), W, H, DisplayWidth(d, scr), DisplayHeight(d, scr));

    GC gc = XCreateGC(d, w, 0, NULL);
    XSetForeground(d, gc, 0x00c040);

    time_t start = time(NULL);
    while (time(NULL) - start < seconds) {
        while (XPending(d)) {
            XEvent e;
            XNextEvent(d, &e);
            switch (e.type) {
            case Expose:
                XFillRectangle(d, w, gc, 0, 0, W, H);
                XSetForeground(d, gc, 0xffffff);
                XDrawString(d, w, gc, 20, 130, "xprobe: Xwayland OK", 19);
                XSetForeground(d, gc, 0x00c040);
                XFlush(d);
                say("Expose");
                break;
            case ButtonPress:
                say("ButtonPress %u at %d,%d", e.xbutton.button, e.xbutton.x, e.xbutton.y);
                break;
            case ButtonRelease:
                say("ButtonRelease %u", e.xbutton.button);
                break;
            case MotionNotify:
                say("Motion %d,%d", e.xmotion.x, e.xmotion.y);
                break;
            case KeyPress: {
                KeySym ks = XLookupKeysym(&e.xkey, 0);
                say("KeyPress keycode=%u keysym=%s", e.xkey.keycode,
                    ks ? XKeysymToString(ks) : "?");
                if (ks == XK_Escape) { say("Escape: exiting"); seconds = 0; }
                break;
            }
            case MapNotify: say("MapNotify"); break;
            case UnmapNotify: say("UnmapNotify"); break;
            case FocusIn: say("FocusIn"); break;
            case FocusOut: say("FocusOut"); break;
            case ConfigureNotify:
                say("Configure %dx%d+%d+%d", e.xconfigure.width,
                    e.xconfigure.height, e.xconfigure.x, e.xconfigure.y);
                break;
            default: break;
            }
        }
        usleep(20000);
    }

    say("exiting after %d s", seconds);
    XFreeGC(d, gc);
    XDestroyWindow(d, w);
    XCloseDisplay(d);
    return 0;
}
