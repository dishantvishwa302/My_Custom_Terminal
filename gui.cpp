#include "gui.h"
#include "pty.h"
#include "ansi.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>

#include <poll.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include <deque>
#include <string>
#include <algorithm>

namespace {

const int kWinW = 800;
const int kWinH = 600;
const int kMaxLines = 10000;

struct Terminal {
    PtySession pty;
    std::deque<std::string> lines;
    std::string csi_pending;
    std::string title = "myshell";
    int col = 0; // cursor column on the current (last) line
};

std::string& current_line(Terminal& term) {
    if (term.lines.empty() || (!term.lines.back().empty() && term.lines.back().back() == '\n')) {
        term.lines.emplace_back();
        term.col = 0;
        if (static_cast<int>(term.lines.size()) > kMaxLines) {
            term.lines.pop_front();
        }
    }
    return term.lines.back();
}

void put_char(Terminal& term, char ch) {
    std::string& line = current_line(term);
    if (term.col < 0) {
        term.col = 0;
    }
    if (term.col > static_cast<int>(line.size())) {
        term.col = static_cast<int>(line.size());
    }
    if (term.col == static_cast<int>(line.size())) {
        line.push_back(ch);
    } else {
        line[static_cast<size_t>(term.col)] = ch;
    }
    ++term.col;
}

void apply_csi(Terminal& term, const std::string& params, char final) {
    int n = params.empty() ? -1 : atoi(params.c_str());
    if (final == 'K') {
        // Erase in line: 0/empty = cursor→end, 1 = start→cursor, 2 = whole line
        std::string& line = current_line(term);
        if (n == 2) {
            line.clear();
            term.col = 0;
        } else if (n == 1) {
            if (term.col > 0 && term.col <= static_cast<int>(line.size())) {
                line.erase(0, static_cast<size_t>(term.col));
            }
            term.col = 0;
        } else {
            if (term.col < static_cast<int>(line.size())) {
                line.erase(static_cast<size_t>(term.col));
            }
        }
    } else if (final == 'C') {
        term.col += (n < 1) ? 1 : n;
    } else if (final == 'D') {
        term.col -= (n < 1) ? 1 : n;
        if (term.col < 0) {
            term.col = 0;
        }
    } else if (final == 'G') {
        term.col = (n < 1) ? 0 : n - 1;
    } else if (final == 'J' && n == 2) {
        term.lines.clear();
        term.col = 0;
    }
    // final == 'm' (colors) and the rest: ignore
}

// Real-terminal rules: \r goes to column 0, \b moves left, letters overwrite.
// myshell redraws with:  \r  prompt+text  ESC[K  then \b to place the caret.
void append_output(Terminal& term, const std::string& text) {
    std::string work = term.csi_pending.empty() ? text : (term.csi_pending + text);
    term.csi_pending.clear();

    size_t i = 0;
    while (i < work.size()) {
        unsigned char ch = static_cast<unsigned char>(work[i]);

        if (ch == 0x1B) {
            if (i + 1 >= work.size()) {
                term.csi_pending = work.substr(i);
                break;
            }
            if (work[i + 1] == '[') {
                size_t j = i + 2;
                while (j < work.size()) {
                    unsigned char d = static_cast<unsigned char>(work[j]);
                    if (d >= 0x40 && d <= 0x7E) {
                        apply_csi(term, work.substr(i + 2, j - (i + 2)), static_cast<char>(d));
                        i = j + 1;
                        break;
                    }
                    ++j;
                }
                if (j >= work.size()) {
                    term.csi_pending = work.substr(i);
                    break;
                }
                continue;
            }
            i += 2; // skip other ESC x sequences
            continue;
        }

        if (ch == '\n') {
            current_line(term).push_back('\n');
            term.col = 0;
        } else if (ch == '\r') {
            term.col = 0;
        } else if (ch == '\f') {
            term.lines.clear();
            term.col = 0;
        } else if (ch == '\b' || ch == 0x7f) {
            if (term.col > 0) {
                --term.col;
            }
        } else if (ch == '\t') {
            put_char(term, ' ');
            put_char(term, ' ');
            put_char(term, ' ');
            put_char(term, ' ');
        } else if (ch >= 32) {
            put_char(term, static_cast<char>(ch));
        }
        ++i;
    }
}

int text_width(XFontSet fontset, XFontStruct* font, const std::string& s) {
    if (fontset) {
        return Xutf8TextEscapement(fontset, s.c_str(), static_cast<int>(s.size()));
    }
    if (font) {
        return XTextWidth(font, s.c_str(), static_cast<int>(s.size()));
    }
    return static_cast<int>(s.size()) * 8;
}

void draw_text(Display* dpy, Window win, GC gc, XFontSet fontset, XFontStruct* font,
               int x, int y, const std::string& s) {
    if (s.empty()) {
        return;
    }
    if (fontset) {
        Xutf8DrawString(dpy, win, fontset, gc, x, y, s.c_str(), static_cast<int>(s.size()));
    } else if (font) {
        XDrawString(dpy, win, gc, x, y, s.c_str(), static_cast<int>(s.size()));
    }
}

// Send one key from X11 to myshell through the PTY master.
void handle_key(Terminal& term, XKeyEvent* ev) {
    unsigned int mods = ev->state;
    char buf[32];
    KeySym ks = 0;
    int len = XLookupString(ev, buf, sizeof(buf), &ks, nullptr);
    if (ks == 0) {
        ks = XLookupKeysym(ev, 0);
    }

    if (ks == XK_Return || ks == XK_KP_Enter) {
        pty_write(term.pty, "\r", 1);
        return;
    }
    if (ks == XK_BackSpace) {
        const char del = 0x7f;
        pty_write(term.pty, &del, 1);
        return;
    }
    if (ks == XK_Tab) {
        pty_write(term.pty, "\t", 1);
        return;
    }
    if (ks == XK_Up)    { pty_write(term.pty, "\033[A", 3); return; }
    if (ks == XK_Down)  { pty_write(term.pty, "\033[B", 3); return; }
    if (ks == XK_Right) { pty_write(term.pty, "\033[C", 3); return; }
    if (ks == XK_Left)  { pty_write(term.pty, "\033[D", 3); return; }

    // Always consume Ctrl+letter here so XLookupString does not also send 'r'/'a'/...
    if (mods & ControlMask) {
        KeySym letter = ks;
        if (letter >= XK_A && letter <= XK_Z) {
            letter = letter - XK_A + XK_a;
        }
        char c = 0;
        if (letter == XK_a) { c = 0x01; }
        else if (letter == XK_e) { c = 0x05; }
        else if (letter == XK_c) { c = 0x03; }
        else if (letter == XK_d) { c = 0x04; }
        else if (letter == XK_l) { c = 0x0c; }
        else if (letter == XK_r) { c = 0x12; }
        else if (letter == XK_z) { c = 0x1a; }
        if (c != 0) {
            pty_write(term.pty, &c, 1);
            return;
        }
    }

    if (len > 0) {
        pty_write(term.pty, buf, static_cast<size_t>(len));
    }
}

} // namespace

int run_gui() {
    Terminal term;
    if (!pty_start(term.pty)) {
        fprintf(stderr, "Failed to start myshell on a PTY. Run 'make' first.\n");
        return 1;
    }

    XSetLocaleModifiers("");

    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        fprintf(stderr, "Cannot open X display. Is DISPLAY set?\n");
        pty_stop(term.pty);
        return 1;
    }

    int screen = DefaultScreen(dpy);
    Window win = XCreateSimpleWindow(
        dpy, RootWindow(dpy, screen),
        50, 50, kWinW, kWinH, 1,
        BlackPixel(dpy, screen), WhitePixel(dpy, screen));

    XStoreName(dpy, win, "MyTerm");
    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);
    XSelectInput(dpy, win,
                 ExposureMask | KeyPressMask | StructureNotifyMask |
                 ButtonPressMask | FocusChangeMask);
    XMapWindow(dpy, win);

    Cursor cursor = XCreateFontCursor(dpy, XC_xterm);
    XDefineCursor(dpy, win, cursor);

    GC gc = XCreateGC(dpy, win, 0, nullptr);
    XSetForeground(dpy, gc, BlackPixel(dpy, screen));

    char** missing = nullptr;
    int nmissing = 0;
    char* def_str = nullptr;
    XFontSet fontset = XCreateFontSet(
        dpy, "-*-*-medium-r-normal--14-*-*-*-*-*-*-*,fixed",
        &missing, &nmissing, &def_str);
    if (missing) {
        XFreeStringList(missing);
    }
    (void)nmissing;
    (void)def_str;

    XFontStruct* font = nullptr;
    int font_h = 16;
    int ascent = 12;
    if (fontset) {
        XFontSetExtents* ext = XExtentsOfFontSet(fontset);
        font_h = ext->max_logical_extent.height;
        ascent = -ext->max_logical_extent.y;
        if (font_h <= 0) {
            font_h = 16;
        }
        if (ascent <= 0) {
            ascent = 12;
        }
    } else {
        font = XLoadQueryFont(dpy, "fixed");
        if (font) {
            XSetFont(dpy, gc, font->fid);
            font_h = font->ascent + font->descent;
            ascent = font->ascent;
        }
    }

    int win_w = kWinW;
    int win_h = kWinH;
    bool has_focus = false;
    bool running = true;
    const int pad = 8;
    const int status_h = font_h + 8;

    while (running) {
        struct pollfd pfd{};
        pfd.fd = term.pty.master_fd;
        pfd.events = POLLIN;

        poll(&pfd, 1, XPending(dpy) ? 0 : 40);

        if (pfd.revents & (POLLIN | POLLHUP | POLLERR)) {
            char buf[512];
            ssize_t n = read(term.pty.master_fd, buf, sizeof(buf));
            if (n > 0) {
                std::string chunk(buf, static_cast<size_t>(n));
                std::string no_osc;
                std::string new_title;
                strip_osc(chunk, no_osc, &new_title);
                if (!new_title.empty()) {
                    term.title = new_title;
                }
                if (!no_osc.empty()) {
                    append_output(term, no_osc);
                }
            } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                running = false;
            }
        }

        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type == KeyPress) {
                handle_key(term, &ev.xkey);
            } else if (ev.type == ConfigureNotify) {
                win_w = ev.xconfigure.width;
                win_h = ev.xconfigure.height;
                int cols = std::max(20, (win_w - 2 * pad) / 8);
                int rows = std::max(5, (win_h - status_h - pad) / font_h);
                pty_set_size(term.pty, rows, cols);
            } else if (ev.type == ButtonPress) {
                XSetInputFocus(dpy, win, RevertToParent, CurrentTime);
            } else if (ev.type == FocusIn) {
                has_focus = true;
            } else if (ev.type == FocusOut) {
                has_focus = false;
            } else if (ev.type == ClientMessage) {
                if (static_cast<Atom>(ev.xclient.data.l[0]) == wm_delete) {
                    running = false;
                }
            }
        }

        XClearWindow(dpy, win);

        std::string cap = "MyTerm — " + term.title;
        XStoreName(dpy, win, cap.c_str());

        int max_lines = std::max(1, (win_h - status_h - pad) / font_h);
        int start = std::max(0, static_cast<int>(term.lines.size()) - max_lines);
        int y = pad + ascent;
        std::string last_drawn;
        int last_y = y;

        for (int i = start; i < static_cast<int>(term.lines.size()); ++i) {
            std::string draw = term.lines[static_cast<size_t>(i)];
            if (!draw.empty() && draw.back() == '\n') {
                draw.pop_back();
            }
            draw_text(dpy, win, gc, fontset, font, pad, y, draw);
            last_drawn = draw;
            last_y = y;
            y += font_h;
            if (y > win_h - status_h) {
                break;
            }
        }

        if (has_focus && !term.lines.empty()) {
            const std::string& last = term.lines.back();
            bool partial = last.empty() || last.back() != '\n';
            if (partial) {
                int caret_at = term.col;
                if (caret_at < 0) {
                    caret_at = 0;
                }
                if (caret_at > static_cast<int>(last_drawn.size())) {
                    caret_at = static_cast<int>(last_drawn.size());
                }
                int caret_x = pad + text_width(fontset, font, last_drawn.substr(0, static_cast<size_t>(caret_at)));
                XDrawLine(dpy, win, gc, caret_x, last_y - ascent, caret_x, last_y - ascent + font_h - 2);
            }
        }

        std::string status =
            "Ctrl+C interrupt | Ctrl+Z stop | Ctrl+A/E line nav | Ctrl+R hsearch | exit to quit";
        draw_text(dpy, win, gc, fontset, font, pad, win_h - 6, status);

        XFlush(dpy);
    }

    pty_stop(term.pty);
    if (fontset) {
        XFreeFontSet(dpy, fontset);
    }
    if (font) {
        XFreeFont(dpy, font);
    }
    XFreeGC(dpy, gc);
    XFreeCursor(dpy, cursor);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
