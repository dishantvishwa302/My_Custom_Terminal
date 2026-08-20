#include <bits/stdc++.h>
#include <unistd.h>         // for fork(), execvp(), pipe(), dup2(), chdir(), getcwd(), read()
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>          // for open()
#include <termios.h>        // We’ll use termios to read raw input for arrow keys.
#include <poll.h>           // for poll()
#include <pty.h>            // for openpty()
#include <utmp.h>           // for struct termios defaults
#include <sys/ioctl.h>      // for ioctl()
#include <signal.h>         // for signal handling
#include <locale.h>
#include <dirent.h>         // Directory listing ke liye
#include <sys/stat.h>       // File/dir check karne ke liye


using namespace std;
int startShellPTY(int tabIndex); // forward declaration

// Close file descriptor if it appears valid (> 0) and set to -1 to avoid reuse.
static inline void close_if_valid(int &fd) {
    if (fd > 0) {
        close(fd);
        fd = -1;
    }
}

// Robust write that retries on EINTR and handles short writes.
static ssize_t write_all(int fd, const void *buf, size_t count) {
    const char *p = static_cast<const char*>(buf);
    size_t total = 0;
    while (total < count) {
        ssize_t w = write(fd, p + total, count - total);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += (size_t)w;
    }
    return (ssize_t)total;
}

// Small helper returning the GUI prompt text used when reprinting prompt in PTY interactions
static inline std::string guiPrompt() {
    return std::string("user@myterm> ");
}

// ====================================================================================
//                          Simple X11 GUI module
// ====================================================================================
#include <X11/Xlib.h>
#include <X11/keysym.h>   // XK_* constants (Ctrl+Tab, etc.)
#include <X11/Xutil.h>
#include <X11/cursorfont.h> // for XC_xterm
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>

// ====================================================================================
//                              GUI globals
// ====================================================================================

static const int GUI_WIDTH = 800;
static const int GUI_HEIGHT = 600;
static const int GUI_MAX_LINES = 10000; // buffer size per tab
bool guiHasFocus = false;  // window focused? caret dikhana hai tabhi
bool selection_active = false;      // Is the user currently dragging the mouse?
int selection_start_x, selection_start_y; // Pixel coordinates where selection began
int selection_end_x, selection_end_y;     // Pixel coordinates where selection ends
static std :: string selectedText; // Store the selected text here


struct GuiTab {
    deque  < string > lines;   // text buffer lines
    std :: mutex mtx;        // protect this tab buffer
    int ptyMaster = -1;         // file descriptor to read shell output
    int scrollOffset = 0;  // vertical scroll offset
    pid_t childPid = -1;   // shell PID (for later close/cleanup)
    std :: string title = "bash"; // tab/window title parsed from OSC
    bool clearLinePending = false;   // ESC[K] aaya → current line clear karo
    std :: string csiPending;   // split CSI sequence ka leftover (stateful)


};

struct PTYTab {
    int masterFd;          // master side of PTY
    pid_t childPid;        // shell process id
    std::string buffer;    // stores text output for display
};

int guiActiveTab = 0;
int guiNumTabs = 4;                // default 4 tabs (changeable)
vector< unique_ptr < GuiTab > > guiTabs;  // use pointers because GuiTab is not movable
std :: mutex guiRenderMtx;
std :: condition_variable guiCv;
bool guiShouldExit = false;



//========================= Append output to GUI buffer (thread-safe) ========================

// Finds all files/dirs in the current directory that start with a given prefix.
std :: vector < std :: string > getAutoCompleteMatches( const std :: string& prefix) {
    std::vector<std::string> matches;
    DIR* dir = opendir("."); // Note: This uses the CWD of your MyTerm program
    if (!dir) return matches;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        // Ignore '.' and '..' directories
        if (name == "." || name == "..") continue;

        if (name.rfind(prefix, 0) == 0) { // Check if name starts with prefix
            matches.push_back(name);
        }
    }
    closedir(dir);
    std::sort(matches.begin(), matches.end());
    return matches;
}

// Finds the longest common prefix among a vector of strings.
std::string find_lcp(const std::vector<std::string>& strings) {
    if (strings.empty()) return "";
    std::string lcp = strings[0];
    for (size_t i = 1; i < strings.size(); ++i) {
        while (strings[i].rfind(lcp, 0) != 0) {
            lcp = lcp.substr(0, lcp.length() - 1);
        }
    }
    return lcp;
}

// ---------------- Append output to GUI buffer (thread-safe)
//===============================================================================
//                          FIXED guiAppendOutput   
void guiAppendOutput(int tabIndex, const string &text) {
    if (tabIndex < 0 || tabIndex >= (int)guiTabs.size()) return;
    auto &tab = *guiTabs[tabIndex];
    std::lock_guard<std::mutex> lock(tab.mtx);
    auto &dq = tab.lines;
    auto get_current_line = [&]() -> string& {
        if (dq.empty() || (!dq.back().empty() && dq.back().back() == '\n')) {
            dq.emplace_back("");
            // Trim the buffer if it gets too long.
            if (dq.size() > GUI_MAX_LINES) {
                dq.pop_front();
            }
        }
        return dq.back();
    };

    for (char ch : text) {
        // We get a fresh reference to the current line for each character.
        string& current_line = get_current_line();

        switch (ch) {
            case '\n':
                // Finalize the current line by adding the newline character.
                current_line.push_back('\n');
                break;

            case '\r':
                // A carriage return means "go to column 0". Bash will usually
                // send the full line again right after this, so we just clear
                // the current line to prepare for the rewrite.
                current_line.clear();
                break;

            case '\b':
            case 0x7f: // This is the DEL character
                // Backspace: remove the last character.
                if (!current_line.empty()) {
                    current_line.pop_back();
                }
                break;

            default:
                // Normal character: append it.
                current_line.push_back(ch);
                break;
        }
    }

    guiCv.notify_one();
}

//===============================================================================
//                      Naya tab kholna (Ctrl+Shift+T) 
//===============================================================================

void guiOpenNewTab() {
    auto tab = std::make_unique<GuiTab>();
    guiTabs.push_back(std::move(tab));
    guiNumTabs = (int)guiTabs.size();

    int newIdx = guiNumTabs - 1;
    startShellPTY(newIdx);        // PTY start
    guiActiveTab = newIdx;        // naya tab active
    guiCv.notify_one();
}

//===============================================================================
//                   Active tab band karna (Ctrl+Shift+W)                  
//===============================================================================

void guiCloseActiveTab() {
    if (guiTabs.empty()) return;
    int idx = guiActiveTab;

    // Graceful close: shell ko SIGHUP
    if (guiTabs[idx]->childPid > 0) {
        kill(guiTabs[idx]->childPid, SIGHUP);
    }
    if (guiTabs[idx]->ptyMaster > 0) {
        close_if_valid(guiTabs[idx]->ptyMaster);
    }

    guiTabs.erase(guiTabs.begin() + idx);
    guiNumTabs = (int)guiTabs.size();

    if (guiNumTabs == 0) {
        guiOpenNewTab();
        // No tabs left, so we should exit the entire application.
        guiShouldExit = true;
        guiCv.notify_all(); // Wake up the main thread
    } else {
        guiActiveTab = std::min(idx, guiNumTabs - 1);
    }
    guiCv.notify_one();
}

//===============================================================================
// OSC title parser: "\x1b]0;TITLE\x07" or "\x1b]0;TITLE\x1b\\"
// in  -> raw PTY chunk
// out -> cleaned (no OSC bytes), and optional extracted title
//===============================================================================

static void stripOSCAndExtractTitle(const std::string& in,
                                    std::string& cleaned,
                                    std::string* titleOut) {
    cleaned.clear();
    size_t i = 0, n = in.size();
    while (i < n) {
        unsigned char c = in[i];
        if (c == 0x1B && i + 1 < n && in[i+1] == ']') {
            // start of OSC
            size_t j = i + 2;
            // scan till BEL (0x07) or ST (ESC \)
            while (j < n && in[j] != 0x07) {
                if (in[j] == 0x1B && j + 1 < n && in[j+1] == '\\') break;
                ++j;
            }
            // payload typically "0;TITLE"
            if (titleOut) {
                std::string payload = in.substr(i + 2, j - (i + 2));
                auto sc = payload.find(';');
                std::string t = (sc != std::string::npos) ? payload.substr(sc + 1) : payload;
                while (!t.empty() && (t.back() == '\r' || t.back() == '\n')) t.pop_back();
                if (!t.empty()) *titleOut = t;
            }
            // skip terminator
            if (j < n && in[j] == 0x07) i = j + 1;
            else if (j + 1 < n && in[j] == 0x1B && in[j+1] == '\\') i = j + 2;
            else i = j;
            continue;
        }
        cleaned.push_back((char)c);
        ++i;
    }
}

//===============================================================================
// CSI consumer: "\x1b[" se lekar final 0x40..0x7E tak padho.
// EL (K / 0K / 2K) ya 'G' ko detect karke flags set karo.
//===============================================================================

static size_t consumeCSI(const std::string& s, size_t i,
                         bool &sawEL, bool &sawEL2K) {
    size_t j = i + 2;               // after ESC[
    std::string param;

    while (j < s.size()) {
        unsigned char c = s[j];
        if (c >= '0' && c <= '9') { param.push_back(c); j++; continue; }
        if (c == ';') { param.push_back(c); j++; continue; }
        if (c >= 0x40 && c <= 0x7E) {         // final
            if (c == 'K') {
                // K, 0K, 2K -> erase in line (we’ll clear fully)
                if (!param.size() || param == "0") { sawEL = true; }
                if (param == "2") { sawEL2K = true; }
            }
            return j + 1;
        }
        j++;
    }

    return std::string::npos;       
}


//===============================================================================
// Stateful CSI stripper: tabIndex-wise pending buffer use karo.
//===============================================================================

static void stripCSI_stateful(int tabIndex, const std::string& in, std::string& out) {
    out.clear();
    std::string work;

    if (!guiTabs[tabIndex]->csiPending.empty()) {
        work = guiTabs[tabIndex]->csiPending + in;
        guiTabs[tabIndex]->csiPending.clear();
    } else {
        work = in;
    }

    size_t i = 0;
    while (i < work.size()) {
        unsigned char c = work[i];
        if (c == 0x1B && i + 1 < work.size() && work[i+1] == '[') {
            bool sawEL=false, sawEL2K=false;
            size_t nxt = consumeCSI(work, i, sawEL, sawEL2K);
            if (nxt == std::string::npos) {         // split CSI -> save tail
                guiTabs[tabIndex]->csiPending = work.substr(i);
                break;
            }
            if (sawEL || sawEL2K) guiTabs[tabIndex]->clearLinePending = true;
            i = nxt;                                // skip CSI completely
            continue;
        }
        out.push_back((char)c);
        ++i;
    }
}

//===============================================================================
//               Simple non-blocking poll to check if GUI wants a key
// Returns -1 if no key. (We'll implement minimal keyboard->integer mapping)
//===============================================================================
int guiPollKey() {
    return -1;
}

//===============================================================================
//               GUI thread function: draw and handle basic keys
//===============================================================================
void guiThreadFunc() {
    // init tabs
    guiTabs.clear();
    for (int i = 0; i < guiNumTabs; ++i)
        guiTabs.push_back(std::make_unique<GuiTab>());

    // --- Launch one PTY shell per tab ---
    for (int i = 0; i < guiNumTabs; ++i) {
        startShellPTY(i);
    }

    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        cerr << "GUI: XOpenDisplay failed\n";
        return;
    }
    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);

    // Create window
    Window win = XCreateSimpleWindow(display, root, 50, 50, GUI_WIDTH, GUI_HEIGHT, 1,
                                     BlackPixel(display, screen), WhitePixel(display, screen));
    XStoreName(display, win, "MyTerm - GUI (Stage 7: PTY Interactive)");
    XSelectInput(display, win, ExposureMask | KeyPressMask | StructureNotifyMask | ButtonPressMask | FocusChangeMask | Button1MotionMask | PropertyChangeMask);
    XMapWindow(display, win);
    Cursor text_cursor = XCreateFontCursor(display, XC_xterm); // XC_xterm is the standard I-beam cursor
    XDefineCursor(display, win, text_cursor);
    XFlush(display);

    // create GC and font info
    GC gc = XCreateGC(display, win, 0, nullptr);


    GC highlight_gc = XCreateGC(display, win, 0, nullptr);
    // Invert colors for highlighting   
    XSetForeground(display, highlight_gc, WhitePixel(display, screen));
    XSetBackground(display, highlight_gc, BlackPixel(display, screen));

    XSetForeground(display, gc, BlackPixel(display, screen));
    XFontStruct* font = XLoadQueryFont(display, "fixed");
    int fontHeight = 14;
    if (font) {
        XSetFont(display, gc, font->fid);
        fontHeight = font->ascent + font->descent;
    }
    // --- Tab bar geometry (simple) ---
    const int tabPadX = 12;
    const int tabPadY = 6;
    const int tabBarHeight = fontHeight + tabPadY;
    (void)tabPadX;

    // scrolling state
    int scrollOffset = 0;
    (void)scrollOffset;

    XEvent ev;
    // redraw loop
    while (!guiShouldExit) {

        // --- Non-blocking read from all PTYs using poll() ---
        struct pollfd pfds[guiNumTabs];
        for (int i = 0; i < guiNumTabs; ++i) {
            pfds[i].fd = guiTabs[i]->ptyMaster;
            pfds[i].events = POLLIN;
        }

        int ret = poll(pfds, guiNumTabs, 50); // short timeout
        if (ret > 0) {
            for (int i = 0; i < guiNumTabs; ++i) {
                if (pfds[i].fd != -1 && pfds[i].revents & (POLLIN | POLLHUP)) {
                    char buf[512];
                    ssize_t bytes = read(pfds[i].fd, buf, sizeof(buf) - 1);

                    if (bytes > 0) {
                        // This is your existing logic for handling output, which is correct.
                        buf[bytes] = '\0';
                        std::string chunk(buf);

                        std::string noOSC, newTitle;
                        stripOSCAndExtractTitle(chunk, noOSC, &newTitle);
                        if (!newTitle.empty()) guiTabs[i]->title = newTitle;

                        std::string noCSI;
                        stripCSI_stateful(i, noOSC, noCSI);

                        if (!noCSI.empty()) guiAppendOutput(i, noCSI);

                    } else {
                        // We will now shut down the entire application.
                        guiShouldExit = true;
                        guiCv.notify_all(); // Wake up the main thread and tell it to exit.
                        break; // Exit the for-loop since we are shutting down.
                    }
                }
            }
        }
        if (guiShouldExit) {
            continue;
        }

        // wait for event or timeout to refresh
        if (XPending(display) == 0) {
            unique_lock<std::mutex> lock(guiRenderMtx);
            guiCv.wait_for(lock, std::chrono::milliseconds(120));
        }

        // handle events
        while (XPending(display)) {
            XNextEvent(display, &ev);
            if (ev.type == Expose) {
                    // window ab viewable hai — ab focus le lo (sirf ek baar)
                    static bool focusedOnce = false;
                        if (!focusedOnce) {
                            XSetInputFocus(display, win, RevertToParent, CurrentTime);
                            int grab = XGrabKeyboard(display, win, True, GrabModeAsync, GrabModeAsync, CurrentTime);
                            if (grab != GrabSuccess) {
                                fprintf(stderr, "[myterm] XGrabKeyboard failed: %d\n", grab);
                            }
                            focusedOnce = true;
                            XFlush(display);
                        }
                // will redraw below
            } else if (ev.type == ConfigureNotify) {
                // window resize -> shells ko naya rows/cols do
                int w = ev.xconfigure.width;
                int h = ev.xconfigure.height;

                // char width estimate: fixed font me 'M' ki width
                int charW = 8;
                if (font) {
                    charW = XTextWidth(font, "M", 1);
                    if (charW <= 0) charW = 8;
                }

                int cols = std::max(20, w / charW);
                int rows = std::max(5, (h - tabBarHeight) / fontHeight);

                struct winsize ws{};
                ws.ws_col = cols;
                ws.ws_row = rows;

                for (auto &t : guiTabs) {
                    if (t && t->ptyMaster >= 0) {
                        ioctl(t->ptyMaster, TIOCSWINSZ, &ws);
                    }
                }
            } else if (ev.type == KeyPress) {

                        unsigned int mods = ev.xkey.state;
                        KeySym ks_no_mods = XLookupKeysym(&ev.xkey, 0);

                        // --- 1. Handle window-level shortcuts first (they don't go to the shell) ---
                        if ((mods & ControlMask) && (mods & ShiftMask)) {
                            KeySym ks_with_shift = XLookupKeysym(&ev.xkey, 1);
                            if (ks_with_shift == XK_T) { guiOpenNewTab(); continue; }
                            if (ks_with_shift == XK_W) { guiCloseActiveTab(); continue; }
                        }

                        if ((mods & ControlMask) && (ks_no_mods == XK_Tab)) {
                            if (mods & ShiftMask) { // Ctrl+Shift+Tab
                                guiActiveTab = (guiActiveTab - 1 + guiNumTabs) % guiNumTabs;
                            } else { // Ctrl+Tab
                                guiActiveTab = (guiActiveTab + 1) % guiNumTabs;
                            }
                            guiCv.notify_one(); // Redraw immediately for tab switch
                            continue;
                        }

                        if (ks_no_mods >= XK_F1 && ks_no_mods <= XK_F4) {
                            if (ks_no_mods == XK_F1) guiActiveTab = 0;
                            else if (ks_no_mods == XK_F2) guiActiveTab = 1 % guiNumTabs;
                            else if (ks_no_mods == XK_F3) guiActiveTab = 2 % guiNumTabs;
                            else if (ks_no_mods == XK_F4) guiActiveTab = 3 % guiNumTabs;
                            guiCv.notify_one(); // Redraw immediately for tab switch
                            continue;
                        }

                        // --- 2. All remaining keys are forwarded to the active shell's PTY ---
                        if (guiActiveTab < 0 || guiActiveTab >= static_cast<int>(guiTabs.size())) {
                            continue; // No active tab to write to
                        }
                        int masterFd = guiTabs[guiActiveTab]->ptyMaster;
                        if (masterFd <= 0) {
                            continue; // PTY is not valid
                        }

                        // A. Handle special single keys
                        if (ks_no_mods == XK_Return || ks_no_mods == XK_KP_Enter) {
                            write_all(masterFd, "\r", 1);
                        } else if (ks_no_mods == XK_BackSpace) {
                            const char del = 0x7f;
                            write_all(masterFd, &del, 1);
                        } else if (ks_no_mods == XK_Tab) { // This is a literal Tab, not Ctrl+Tab
                                // --- Custom Auto-Completion Logic ---
                                std::string current_input;
                                // 1. Get the current line the user is typing on.
                                {
                                    lock_guard<std::mutex> lock(guiTabs[guiActiveTab]->mtx);
                                    auto& dq = guiTabs[guiActiveTab]->lines;
                                    if (!dq.empty() && (dq.back().empty() || dq.back().back() != '\n')) {
                                        current_input = dq.back();
                                    }
                                }

                                // 2. Find the last word in the input string to use as the prefix.
                                size_t last_space = current_input.find_last_of(" \t");
                                std::string prefix = (last_space == std::string::npos) ? current_input : current_input.substr(last_space + 1);

                                    if (prefix.empty()) { // If there's no prefix, send a normal tab
                                    write_all(masterFd, "\t", 1);
                                    continue;
                                }

                                // 3. Find all matching files.
                                std::vector<std::string> matches = getAutoCompleteMatches(prefix);

                                if (matches.size() == 1) {
                                    // Case i: Exactly one match. Complete the rest of the filename.
                                    std::string completion = matches[0].substr(prefix.length()) + " ";
                                    write_all(masterFd, completion.c_str(), completion.length());
                                } else if (matches.size() > 1) {
                                    // Case ii: Multiple matches.
                                    std::string lcp = find_lcp(matches);
                                    if (lcp.length() > prefix.length()) {
                                        // First Tab press: complete up to the longest common prefix.
                                        std::string completion = lcp.substr(prefix.length());
                                        write_all(masterFd, completion.c_str(), completion.length());
                                    } else {
                                        // Second Tab press: Show the list of options.
                                        std::string options_display = "\n";
                                        for (const auto& match : matches) {
                                            options_display += match + "\t";
                                        }
                                        options_display += "\n";
                                        // We must also re-print the prompt and current input to make it look right.
                                        std::string prompt_and_input = guiPrompt() + current_input;
                                        options_display += prompt_and_input;
                                        write_all(masterFd, options_display.c_str(), options_display.length());
                                    }
                                }
                                // Case iii: No matches. Do nothing.
                                continue; // We've handled the Tab, so skip the default handlers.
                        } else {
                            // B. Handle all other characters and Ctrl combinations
                            char buf[32];
                            KeySym ks_lookup;
                            int len = XLookupString(&ev.xkey, buf, sizeof(buf), &ks_lookup, nullptr);

                            if (len > 0) {
                                // This handles all normal printable characters ('a', 'B', '!', etc.)
                                write(masterFd, buf, len);
                            } else if (mods & ControlMask) {
                                // This is the correct, single location for all Ctrl-key combos
                                if (ks_no_mods == XK_a || ks_no_mods == XK_A) { char ch = 0x01; write(masterFd, &ch, 1); } // ^A
                                else if (ks_no_mods == XK_c || ks_no_mods == XK_C) { char ch = 0x03; write(masterFd, &ch, 1); } // ^C
                                else if (ks_no_mods == XK_d || ks_no_mods == XK_D) { char ch = 0x04; write(masterFd, &ch, 1); } // ^D
                                else if (ks_no_mods == XK_e || ks_no_mods == XK_E) { char ch = 0x05; write(masterFd, &ch, 1); } // ^E
                                else if (ks_no_mods == XK_l || ks_no_mods == XK_L) { char ch = 0x0c; write(masterFd, &ch, 1); } // ^L
                                else if (ks_no_mods == XK_r || ks_no_mods == XK_R) { const char* cmd = "hsearch\n"; write(masterFd, cmd, strlen(cmd)); }// Tell bash to run our hsearch program } // ^R
                                else if (ks_no_mods == XK_z || ks_no_mods == XK_Z) { char ch = 0x1a; write(masterFd, &ch, 1); } // ^Z
                                guiCv.notify_one(); // Force a redraw for immediate feedback
                            }
                        }
                        

            } else if (ev.type == ButtonPress) {

                // --- Handle Mouse Wheel Scrolling ---
                if (ev.xbutton.button == 4) { // Scroll Up
                    auto& tab = *guiTabs[guiActiveTab];
                    // Decrease offset, but don't go below zero
                    tab.scrollOffset = std::max(0, tab.scrollOffset + 3);
                } else if (ev.xbutton.button == 5) { // Scroll Down
                    auto& tab = *guiTabs[guiActiveTab];
                    tab.scrollOffset -= 3; // Increase offset
                }
                    // Left mouse button starts a selection
                if (ev.xbutton.button == Button1) {
                    selection_active = true;
                    selection_start_x = ev.xbutton.x;
                    selection_start_y = ev.xbutton.y;
                    selection_end_x = selection_start_x;
                    selection_end_y = selection_start_y;
                }


                // click par focus le lo (warna KeyPress nahi aata)
                XSetInputFocus(display, win, RevertToParent, CurrentTime);
                 int grab = XGrabKeyboard(display, win, True, GrabModeAsync, GrabModeAsync, CurrentTime);
                (void)grab; // optional: print if you want
                XFlush(display);

                // Mouse: tab switch by clicking tab-bar
                int x = ev.xbutton.x;
                int y = ev.xbutton.y;
                if (y <= tabBarHeight) {
                    int approxTabWidth = 260; // simple fixed width
                    int idx = x / approxTabWidth;
                    if (idx >= 0 && idx < (int)guiTabs.size()) {
                        guiActiveTab = idx;
                    }
                }
            }
            else if (ev.type == FocusIn)  { guiHasFocus = true;  }
            else if (ev.type == FocusOut) { guiHasFocus = false; }
            else if (ev.type == MotionNotify) {
                if (selection_active) {
                    selection_end_x = ev.xmotion.x;
                    selection_end_y = ev.xmotion.y;
                    guiCv.notify_one(); // redraw to show selection
                }

            } else if (ev.type == ButtonRelease) {
                if (ev.xbutton.button == Button1) {
                    selection_active = false;
                    guiCv.notify_one(); // redraw to remove selection box
                    // --- Self-Contained Text Extraction Logic ---
                    selectedText.clear();

                    // 1. Safely get the text data for the active tab.
                    if (guiActiveTab < 0 || guiActiveTab >= static_cast<int>(guiTabs.size())) return;
                    vector<string> local_snapshot;
                    auto& tab = *guiTabs[guiActiveTab];
                    {
                        lock_guard<std::mutex> lock(tab.mtx);
                        local_snapshot.assign(tab.lines.begin(), tab.lines.end());
                    }

                    if (local_snapshot.empty()) return;

                    // 2. We must recalculate the geometry values here, just like in the render loop.
                    int local_contentTopY = tabBarHeight + 4;
                    int maxLinesOnWindow = (GUI_HEIGHT - local_contentTopY) / fontHeight;
                    int max_scroll_up = std::max(0, (int)local_snapshot.size() - maxLinesOnWindow);
                    int clamped_scrollOffset = std::max(0, std::min(tab.scrollOffset, max_scroll_up));
                    int local_startLine = (int)local_snapshot.size() - maxLinesOnWindow - clamped_scrollOffset;
                    if (local_startLine < 0) local_startLine = 0;

                    int selection_box_top = std::min(selection_start_y, selection_end_y);
                    int selection_box_bottom = std::max(selection_start_y, selection_end_y);

                    // 3. Iterate through the visible lines and extract the text.
                    int current_y = local_contentTopY + fontHeight;
                    bool first_line = true;

                    for (size_t i = local_startLine; i < local_snapshot.size(); ++i) {
                        int line_y_top = current_y - font->ascent;
                        int line_y_bottom = line_y_top + fontHeight;

                        if (line_y_bottom > selection_box_top && line_y_top < selection_box_bottom) {
                            if (!first_line) {
                                selectedText += '\n';
                            }
                            std::string line_content = local_snapshot[i];
                            // Remove the trailing newline before adding to the selection
                            if (!line_content.empty() && line_content.back() == '\n') {
                                line_content.pop_back();
                            }
                            selectedText += line_content;
                            first_line = false;
                        }
                        current_y += fontHeight;
                    }

                    // 4. After extracting, claim ownership of the PRIMARY selection (clipboard).
                    if (!selectedText.empty()) {
                        Atom primary_selection = XInternAtom(display, "PRIMARY", False);
                        XSetSelectionOwner(display, primary_selection, win, CurrentTime);
                    }
                    
                }
            }else if (ev.type == SelectionRequest) {
                XSelectionRequestEvent* req = &ev.xselectionrequest;
                XSelectionEvent sev = {};
                sev.type = SelectionNotify;
                sev.requestor = req->requestor;
                sev.selection = req->selection;
                sev.target = req->target;
                sev.property = req->property;
                sev.time = req->time;

                Atom utf8_string_atom = XInternAtom(display, "UTF8_STRING", False);

                if (req->target == utf8_string_atom) {
                    XChangeProperty(display, req->requestor, req->property, utf8_string_atom, 8,
                                    PropModeReplace, (unsigned char*)selectedText.c_str(), selectedText.length());
                } else {
                    // We don't support other formats, so we deny the request.
                    sev.property = None;
                }

                // Send the notification event back to the requesting application
                XSendEvent(display, req->requestor, True, NoEventMask, (XEvent*)&sev);

            } else if (ev.type == SelectionClear) {
                // Another application has taken ownership of the clipboard.
                // We should clear our stored selection.
                selectedText.clear();
            }

        } // <-- CLOSE while (XPending(display))

        // ---------- REDRAW ----------
        XClearWindow(display, win);

        // (A) Window caption update (active tab title)
        {
            std::string cap = "MyTerm — " + guiTabs[guiActiveTab]->title +
                              "  (Tab " + std::to_string(guiActiveTab+1) + "/" + std::to_string(guiNumTabs) + ")";
            XStoreName(display, win, cap.c_str());
        }

        // (B) Tab Bar
        const int tabPadX = 12;
        const int tabPadY = 6;
        const int tabBarHeight = fontHeight + tabPadY;

        // helper: truncate text so it fits in maxPx (adds "..." if needed)
        auto ellipsize = [&](const std::string& s, int maxPx) -> std::string {
            if (maxPx <= 0) return "";
            auto textWidth = [&](const std::string& t) {
                return font ? XTextWidth(font, t.c_str(), (int)t.size())
                            : (int)t.size() * 8; // crude fallback
            };
            if (textWidth(s) <= maxPx) return s;
            std::string dots = "...";
            int dotsW = textWidth(dots);
            std::string out;
            for (char c : s) {
                std::string tmp = out; tmp.push_back(c);
                if (textWidth(tmp) + dotsW > maxPx) break;
                out.push_back(c);
            }
            return out + dots;
        };

        int x = 6;                                       // running x position
        int yBase = fontHeight;                          // baseline for text
        int maxTabW = std::max(120, GUI_WIDTH / std::max(2, guiNumTabs)); // cap

        for (int i = 0; i < guiNumTabs; ++i) {
            std::string label = "[" + std::to_string(i+1) + "] " + guiTabs[i]->title;

            // compute width of label, add padding, cap to maxTabW, and elide if needed
            int labelPx = font ? XTextWidth(font, label.c_str(), (int)label.size())
                            : (int)label.size() * 8;
            int desiredW = labelPx + 2 * tabPadX;
            int tabW = std::min(desiredW, maxTabW);
            int textMaxPx = tabW - 2 * tabPadX;
            std::string drawText = ellipsize(label, textMaxPx);

            // underline active tab
            if (i == guiActiveTab) {
                XDrawLine(display, win, gc, x, tabBarHeight - 2,
                        x + tabW - 10, tabBarHeight - 2);
            }

            // draw text with left padding inside the tab slot
            XDrawString(display, win, gc, x + tabPadX, yBase,
                        drawText.c_str(), (int)drawText.size());

            x += tabW;                                   // advance to next tab slot
            if (x >= GUI_WIDTH - 10) break;              // stop if out of space
        }


        // (C) Active tab content (below tab-bar)
        int contentTopY = tabBarHeight + 4;

        vector<string> snapshot;
        {
            lock_guard<std::mutex> lock(guiTabs[guiActiveTab]->mtx);
            snapshot.assign(guiTabs[guiActiveTab]->lines.begin(), guiTabs[guiActiveTab]->lines.end());
        }

        int startLine = 0;
        if (!snapshot.empty()) {
            auto& tab = *guiTabs[guiActiveTab];
            int maxLinesOnWindow = (GUI_HEIGHT - contentTopY) / fontHeight;

            // The maximum number of lines we can scroll up from the bottom
            int max_scroll_up = std::max(0, (int)snapshot.size() - maxLinesOnWindow);

            // Clamp the offset: it cannot be negative or more than we can scroll up
            tab.scrollOffset = std::max(0, std::min(tab.scrollOffset, max_scroll_up));

            // Calculate the start line.
            startLine = (int)snapshot.size() - maxLinesOnWindow - tab.scrollOffset;
            if (startLine < 0) startLine = 0;
        }
        int y = contentTopY + fontHeight;
        
        // --- caret ke liye tracking ---
        int lastDrawnIndex = 8;
        int lastBaselineY  = y;
        std::string lastDrawnText;

            if (!snapshot.empty()) {
            // This is now a single, unified loop to draw all visible lines.
                    for (size_t i = startLine; i < snapshot.size(); ++i) {
                        std::string drawLine = snapshot[i];
                        // Remove trailing newline for correct rendering
                        if (!drawLine.empty() && drawLine.back() == '\n') {
                            drawLine.pop_back();
                        }

                        // --- Highlighting Logic ---
                        // Calculate the vertical boundaries for the current line.
                        int line_y_top = y - font->ascent;
                        int line_y_bottom = line_y_top + fontHeight;

                        // Determine the top and bottom of the user's selection area.
                        int selection_box_top = std::min(selection_start_y, selection_end_y);
                        int selection_box_bottom = std::max(selection_start_y, selection_end_y);

                        // Check if this line overlaps with the selection area.
                        if (selection_active && line_y_bottom > selection_box_top && line_y_top < selection_box_bottom) {
                            // This line is selected. Draw it with inverted colors.
                            // 1. Draw the highlight background across the whole window width.
                            XFillRectangle(display, win, highlight_gc, 0, line_y_top, GUI_WIDTH, fontHeight);
                            // 2. Draw the text on top using the inverted highlight_gc.
                            XDrawString(display, win, highlight_gc, lastDrawnIndex, y, drawLine.c_str(), (int)drawLine.size());
                        } else {
                            // This line is not selected. Draw it normally.
                            XDrawString(display, win, gc, lastDrawnIndex, y, drawLine.c_str(), (int)drawLine.size());
                        }

                        y += fontHeight; // Move to the next line position
                    }

                    // --- Caret Logic (handled after all lines are drawn) ---
                    const std::string &last = snapshot.back();
                    // Only draw the caret if the window has focus and the last line is a partial line (the prompt).
                    if (guiHasFocus && (last.empty() || last.back() != '\n')) {
                        auto text_px = [&](const std::string& s) -> int {
                            return font ? XTextWidth(font, s.c_str(), (int)s.size()) : (int)s.size() * 8;
                        };

                        // The last Y position is where the loop left off, minus one line height.
                        int last_line_y = y - fontHeight;
                        
                        int caretX  = lastDrawnIndex + text_px(last);
                        int caretTop = last_line_y - font->ascent;
                        int caretBot = caretTop + fontHeight - 2;
                        XDrawLine(display, win, gc, caretX, caretTop, caretX, caretBot);
                    }
        }



        // --- Fake caret draw: agar last line partial ho (no trailing '\n') ---
        if (guiHasFocus && !snapshot.empty()) {
            bool lastIsPartial = true;
            // If the actual last buffered line ends with '\n', then no caret
            const string& rawLast = snapshot.back();
            if (!rawLast.empty() && rawLast.back() == '\n') lastIsPartial = false;

            if (lastIsPartial && lastDrawnIndex >= 0) {
                // X width nikaalo (fixed font), approx fallback 8px/char
                int textPx = (font)
                    ? XTextWidth(font, lastDrawnText.c_str(), (int)lastDrawnText.size())
                    : (int)lastDrawnText.size() * 8;
                int caretX = 8 + textPx;
                int caretYTop = lastBaselineY - (font ? font->ascent : (fontHeight - 2));
                unsigned int caretW = 2;                    // patla vertical bar
                unsigned int caretH = (unsigned int)(fontHeight - 2);

                // chhota rectangle as caret
                XFillRectangle(display, win, gc, caretX, caretYTop, caretW, caretH);
            }
        }

        // (D) Bottom status line
        string status = "Tabs: " + to_string(guiNumTabs) +
                        " | Active: " + to_string(guiActiveTab+1) +
                        " | Ctrl+Tab / F1–F4 switch | New: Ctrl+Shift+T | Close: Ctrl+Shift+W";
        XDrawString(display, win, gc, 8, GUI_HEIGHT - 6, status.c_str(), (int)status.size());

        XFlush(display);
    }

    // close all PTYs + signal children
    for (auto &tab : guiTabs) {
        if (!tab) continue;
        if (tab->ptyMaster > 0) close(tab->ptyMaster);
        if (tab->childPid  > 0) kill(tab->childPid, SIGHUP);
    }

    // cleanup
    if (font) XFreeFont(display, font);
    XFreeGC(display, gc);
    XDestroyWindow(display, win);
    XCloseDisplay(display);
}
//===============================================================================
//                           End of GUI module                          
//===============================================================================


//===============================================================================
//                           History Management                         
//===============================================================================
vector<string> commandHistory;   // stores all commands entered
int historyIndex = -1;           // current position when navigating

// Track the currently running foreground process
pid_t foregroundPid = -1;


// ================================================================
//  Utility: convert vector<string> → vector<char*> for execvp()
// ================================================================

vector<char*> makeArgv(const vector<string>& args) {
    vector<char*> argv;
    for (const string& s : args)
        argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);
    return argv;
}


// ================================================================
//  Handle input redirection (<)
// ================================================================

void handleInputRedirection(const vector<string>& tokens) {
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i] == "<" && i + 1 < tokens.size()) {
            int fd = open(tokens[i + 1].c_str(), O_RDONLY);
            if (fd < 0) { perror("open (input redirection)"); _exit(1); }
            dup2(fd, STDIN_FILENO);
            close(fd);
            break;
        }
    }
}

// ================================================================
//  Handle output redirection (>)
// ================================================================

void handleOutputRedirection(const vector<string>& tokens) {
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i] == ">" && i + 1 < tokens.size()) {
            int fd = open(tokens[i + 1].c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) { perror("open (output redirection)"); _exit(1); }
            dup2(fd, STDOUT_FILENO);
            close(fd);
            break;
        }
    }
}

// =======================================================================================
//                          Handles Ctrl + C (SIGINT) signal
//=========================================================================================

void handleSigint(int signalNumber) {
    (void)signalNumber; // unused parameter
    if (foregroundPid > 0) {
        // Send SIGINT to the foreground process group
        kill(-foregroundPid, SIGINT); // The minus sign (-foregroundPid) means “send to this process group”.
    } else {
        cout << "\n";  // neat prompt spacing
    }
}


//=========================================================================================
//                                 Ctrl+Z handler 
//          Ctrl+Z presses send SIGTSTP, move foreground process to background
//=========================================================================================
void handleCtrlZ(int signalNumber) {
    (void)signalNumber; // unused parameter
    if (foregroundPid > 0) {
        // Send SIGTSTP to the foreground process group
        kill(-foregroundPid, SIGTSTP); // minus → send to process group
        cout << "\n[Stopped PID: " << foregroundPid << "]" << endl;
        // Do not reset foregroundPid, so shell knows this job is stopped
    } else {
        cout << "\n"; // neat prompt spacing if no foreground
    }
}

//======================================================================================
//                  Detect if the command should run in background 
// =====================================================================================

bool isBackgroundCommand(vector<string>& tokens) {
    if (!tokens.empty() && tokens.back() == "&") {
        tokens.pop_back(); // remove '&' from actual command
        return true;
    }
    return false;
}

// ================================================================
//                  Add a command to the history
// ================================================================

void addToHistory(const string& cmd) {
    if (cmd.empty()) return;
    commandHistory.push_back(cmd);
    historyIndex = commandHistory.size(); // reset index to end
}

// ================================================================
//          Read a single character without waiting for Enter
// ================================================================

char getch() {
    char buf = 0;
    struct termios old = {};
    if (tcgetattr(STDIN_FILENO, &old) < 0) perror("tcgetattr");
    struct termios newt = old;
    newt.c_lflag &= ~(ICANON | ECHO); // disable canonical mode and echo
    if (tcsetattr(STDIN_FILENO, TCSANOW, &newt) < 0) perror("tcsetattr");
    if (read(STDIN_FILENO, &buf, 1) < 0) perror("read");
    tcsetattr(STDIN_FILENO, TCSANOW, &old);
    return buf;
}

// ================================================================
//              Get previous or next command from history
// ================================================================

string navigateHistory(char key) {
    if (commandHistory.empty()) return "";

    if (key == 'A') { // Up arrow
        if (historyIndex > 0) historyIndex--;
    } else if (key == 'B') { // Down arrow
        if (historyIndex < (int)commandHistory.size() - 1) historyIndex++;
        else return ""; // past last command
    }

    if (historyIndex >= 0 && historyIndex < (int)commandHistory.size())
        return commandHistory[historyIndex];
    return "";
}

// ================================================================
//  Execute a single command (handles <, >, and & for background)
// ================================================================

void executeSimpleCommand(vector<string> tokens) {
    
    bool runInBackground = isBackgroundCommand(tokens);

   
    vector<string> pureTokens;
    for (size_t i = 0; i < tokens.size(); ++i)
        if (tokens[i] != ">" && tokens[i] != "<" &&
            (i == 0 || (tokens[i - 1] != ">" && tokens[i - 1] != "<")))
            pureTokens.push_back(tokens[i]);

    vector<char*> argv = makeArgv(pureTokens);

    
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return;
    }

    if (pid == 0) {
        // --- CHILD PROCESS ---
        signal(SIGINT, SIG_DFL); // restore default Ctrl+C handling

        handleInputRedirection(tokens);
        handleOutputRedirection(tokens);

        execvp(argv[0], argv.data());
        perror("execvp");
        _exit(1);
    } 
    else {
        // --- PARENT PROCESS ---
        if (runInBackground) {
            // Background job → don't wait
            cout << "[Background PID: " << pid << "]" << endl;
        } else {
            // Foreground job → wait normally
            foregroundPid = pid;
            waitpid(pid, nullptr, 0);
            foregroundPid = -1;
        }
    }
}

// ================================================================
//  Execute a piped command: cmd1 | cmd2
// ================================================================

void executePipedCommand(const vector<string>& leftCmd, const vector<string>& rightCmd) {

    int pipeFd[2];
    if (pipe(pipeFd) == -1) { perror("pipe"); return; }

    pid_t pid1 = fork();
    if (pid1 < 0) { perror("fork (left)"); return; }

    if (pid1 == 0) {
        // Left side writes into pipe
        signal(SIGINT, SIG_DFL);  // normal Ctrl+C behavior
        setpgid(0, 0);            // create a new process group (PGID = own PID)

        close(pipeFd[0]);
        dup2(pipeFd[1], STDOUT_FILENO);
        close(pipeFd[1]);

        vector<char*> argv1 = makeArgv(leftCmd);
        execvp(argv1[0], argv1.data());
        perror("execvp (left)");
        _exit(1);
    }
    // --- PARENT continues ---
    setpgid(pid1, pid1);  // ensure first child owns the group
    pid_t pid2 = fork();
    if (pid2 < 0) { perror("fork (right)"); return; }

    if (pid2 == 0) {
        // Right side reads from pipe
        signal(SIGINT, SIG_DFL);  // normal Ctrl+C behavior
        setpgid(0, pid1);         // join the first child's process group

        close(pipeFd[1]);
        dup2(pipeFd[0], STDIN_FILENO);
        close(pipeFd[0]);

        vector<char*> argv2 = makeArgv(rightCmd);
        execvp(argv2[0], argv2.data());
        perror("execvp (right)");
        _exit(1);
    }

    close(pipeFd[0]);
    close(pipeFd[1]);

    // --- Parent waits for both ---
    foregroundPid = pid1;  // group leader PID

    waitpid(pid1, nullptr, 0);
    waitpid(pid2, nullptr, 0);
    foregroundPid = -1;
}

// ===============================================================
// Execute commands with multiple pipes: cmd1 | cmd2 | cmd3 ...
// ===============================================================

void executeMultiplePipes(const vector<vector<string>>& commands) {
    int numCommands = commands.size();
    int pipeFd[2 * (numCommands - 1)];
    pid_t pids[numCommands];

    // Create all required pipes
    for (int i = 0; i < numCommands - 1; ++i) {
        if (pipe(pipeFd + i * 2) == -1) {
            perror("pipe");
            return;
        }
    }

    pid_t pgid = -1; // process group id (for signal handling)

    for (int i = 0; i < numCommands; ++i) {
        pids[i] = fork();
        if (pids[i] < 0) {
            perror("fork");
            return;
        }

        if (pids[i] == 0) {
            // --- CHILD PROCESS ---
            signal(SIGINT, SIG_DFL); // restore default signal handling

            // Set up process group
            if (i == 0)
                setpgid(0, 0); // leader
            else
                setpgid(0, pgid); // join existing group

            // Redirect stdin from previous pipe if not first command
            if (i > 0)
                dup2(pipeFd[(i - 1) * 2], STDIN_FILENO);

            // Redirect stdout to next pipe if not last command
            if (i < numCommands - 1)
                dup2(pipeFd[i * 2 + 1], STDOUT_FILENO);

            // Close all pipe fds
            for (int j = 0; j < 2 * (numCommands - 1); ++j)
                close(pipeFd[j]);

            vector<char*> argv = makeArgv(commands[i]);
            execvp(argv[0], argv.data());
            perror("execvp");
            _exit(1);
        } else {
            if (i == 0)
                pgid = pids[0]; // store first child as group leader
            setpgid(pids[i], pgid);
        }
    }

    // --- PARENT PROCESS ---
    // Close all pipe fds
    for (int j = 0; j < 2 * (numCommands - 1); ++j)
        close(pipeFd[j]);

    foregroundPid = pgid; // group leader
    int status;
    for (int i = 0; i < numCommands; ++i)
        waitpid(pids[i], &status, 0);
    foregroundPid = -1;
}

// ================================================================
//  Execute multiple commands in parallel and monitor outputs
//                   multiWatch: parallel execution 
// ================================================================

void multiWatch(const vector<vector<string>>& commands) {
    int n = commands.size();
    vector<int> pipeFdRead(n), pipeFdWrite(n);
    vector<pid_t> pids(n);

    // step 1 Create pipes and fork processes
    for (int i = 0; i < n; ++i) {
        int fds[2];
        if (pipe(fds) < 0) { perror("pipe"); return; }
        pipeFdRead[i] = fds[0];  // read end
        pipeFdWrite[i] = fds[1]; // write end

        pids[i] = fork();
        if (pids[i] < 0) { perror("fork"); return; }

        if (pids[i] == 0) {
            // --- CHILD ---
            close(pipeFdRead[i]);        // close unused read end
            dup2(pipeFdWrite[i], STDOUT_FILENO); // stdout -> pipe
            dup2(pipeFdWrite[i], STDERR_FILENO); // stderr -> pipe
            close(pipeFdWrite[i]);

            vector<char*> argv = makeArgv(commands[i]);
            execvp(argv[0], argv.data());
            perror("execvp multiWatch"); 
            _exit(1);
        } 
        else {
            // --- PARENT ---
            close(pipeFdWrite[i]); // parent doesn't write
        }
    }

    // step 2 Setup poll structures
    vector<struct pollfd> pfds(n);
    for (int i = 0; i < n; ++i) {
        pfds[i].fd = pipeFdRead[i];
        pfds[i].events = POLLIN;
    }

    // step 3 Poll loop: read all outputs
    bool running = true;
    char buffer[256];
    while (running) {
        int ret = poll(pfds.data(), n, 100); // 100ms timeout
        if (ret < 0) { perror("poll"); break; }

        running = false;
        for (int i = 0; i < n; ++i) {
            if (pfds[i].revents & POLLIN) {
                int bytes = read(pfds[i].fd, buffer, sizeof(buffer)-1);
                if (bytes > 0) {
                    buffer[bytes] = '\0';
                    string tagged = "[cmd" + to_string(i+1) + "] " + buffer;
                    cout << tagged;
                    cout.flush();
                    guiAppendOutput(0, tagged);   // ✅ Mirror output to GUI tab 0
                    running = true;
                }
            }
        }
    }

    // step 4 Final read: ensure short-lived commands are captured
    for (int i = 0; i < n; ++i) {
        int bytes;
        while ((bytes = read(pipeFdRead[i], buffer, sizeof(buffer)-1)) > 0) {
            buffer[bytes] = '\0';
            string tagged = "[cmd" + to_string(i+1) + "] " + buffer;
            cout << tagged;
            guiAppendOutput(0, tagged);   // ✅ Mirror final chunks too
        }
        close(pipeFdRead[i]); // close read end
    }

    // step 5 Wait for all children
    for (int i = 0; i < n; ++i)
        waitpid(pids[i], nullptr, 0);
}

// ==========================================================
//  Start a shell session connected to a pseudo-terminal (PTY)
// ==========================================================

int startShellPTY(int tabIndex) {
    int masterFd, slaveFd;
    char slaveName[100];
    struct termios termp{};
    struct winsize winp = {24, 80, 0, 0};

    // start from current tty settings (or use cfmakeraw(&t) then re-enable what you want)
    if (tcgetattr(STDIN_FILENO, &termp) == -1) memset(&termp, 0, sizeof(termp));

    // make sure echo & canonical line editing are ON
    termp.c_lflag |= (ECHO | ICANON );
    // common input/output mappings so Bash behaves
    termp.c_iflag |= ICRNL;                 // map CR -> NL on input
    termp.c_oflag |= OPOST;              // keep post-processing
    termp.c_oflag &= ~ONLCR;             // do NOT expand NL to CRNL
    termp.c_cc[VERASE] = 0x7f;              // Backspace = DEL

    if (openpty(&masterFd, &slaveFd, slaveName, &termp, &winp) < 0) {
        perror("openpty");
        return -1;
    }
    
    int flags = fcntl(masterFd, F_GETFL, 0);
    fcntl(masterFd, F_SETFL, flags | O_NONBLOCK);


    pid_t pid = fork();
    if (pid == 0) {
        // --- Child process: attach to slave PTY ---
        close(masterFd);
        setsid();
        ioctl(slaveFd, TIOCSCTTY, 0);
        dup2(slaveFd, STDIN_FILENO);
        dup2(slaveFd, STDOUT_FILENO);
        dup2(slaveFd, STDERR_FILENO);
        close(slaveFd);
        setenv("TERM", "xterm-256color", 1);     // helps programs like ls, vim, etc.
        setenv("LANG",   "en_US.UTF-8",    1);
        setenv("LC_ALL", "en_US.UTF-8",    1);
        setenv("PS1", "user@myterm> ", 1);       // <-- the prompt you want


        const char* standard_path = "/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin";
        setenv("PATH", standard_path, 1); // The '1' forces an overwrite of any existing PATH.
        // launch an interactive bash WITHOUT reading /etc/profile or ~/.bashrc,
        // so PS1 above is not overridden
        execlp("bash", "bash", "--noprofile", "--norc", "-i", (char*)nullptr);

        // if execlp fails:
        perror("execlp bash");
        _exit(127);
    }

    // --- Parent process: store masterFd for reading ---
    close(slaveFd);
    guiTabs[tabIndex]->ptyMaster = masterFd;
    guiTabs[tabIndex]->childPid  = pid;     // NEW: save child pid
    guiTabs[tabIndex]->title     = "bash";  // default; will be updated by OSC
    return masterFd;
}

// ==============================================================
//                      Raw mode utilities 
// ==============================================================
//              Save original terminal settings

struct termios origTermios;

// Enable raw mode (no buffering, capture keys immediately)
void enableRawMode() {
    tcgetattr(STDIN_FILENO, &origTermios); // save current terminal
    struct termios raw = origTermios;
    raw.c_lflag &= ~(ECHO | ICANON);      // disable echo & canonical mode
    raw.c_cc[VMIN] = 1;                   // read() returns after 1 char
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

// Disable raw mode (restore original terminal)
void disableRawMode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &origTermios);
}

// ==============================================================
//                  Search command history
// ==============================================================
string searchHistory() {
    cout << "\nEnter search term: ";
    cout.flush();
    string term;
    char c;
    while (true) {
        c = getch();
        if (c == '\n') break;            // Enter pressed
        else if (c == 127 || c == 8) {   // Backspace
            if (!term.empty()) {
                term.pop_back();
                cout << "\b \b";
                cout.flush();
            }
        } else {
            term.push_back(c);
            cout << c;
            cout.flush();
        }
    }

    // Search for exact match (most recent first)
    for (int i = commandHistory.size() - 1; i >= 0; --i) {
        if (commandHistory[i] == term)
            return commandHistory[i];
    }

    // Search for longest substring match (>2 chars)
    size_t bestLen = 0;
    string bestCmd;
    for (int i = commandHistory.size() - 1; i >= 0; --i) {
        const string& cmd = commandHistory[i];
        for (size_t j = 0; j + term.size() <= cmd.size(); ++j) {
            size_t k = 0;
            while (k < term.size() && cmd[j+k] == term[k]) k++;
            if (k > bestLen && k > 2) {
                bestLen = k;
                bestCmd = cmd;
            }
        }
    }

    if (!bestCmd.empty()) return bestCmd;
    return "No match for search term in history";
}

//==============================================================================

static inline std::string promptText() {
    char cwd[1024];
    getcwd(cwd, sizeof(cwd));
    return std::string(cwd) + " $ ";
}

// Repaint: clear line, print prompt+buffer, then place cursor at prompt+cursorPos
static inline void repaintLine(const std::string& prompt,
                               const std::string& buf,
                               int cursorPos) {
    // guard
    if (cursorPos < 0) cursorPos = 0;
    if (cursorPos > (int)buf.size()) cursorPos = (int)buf.size();

    const int target = (int)prompt.size() + cursorPos;

    std::cout << "\r\033[2K"            // go to col 1, clear entire line
              << prompt << buf          // print full line
              << "\r\033[" << target    // return to col 1, move right to target
              << "C";
    std::cout.flush();
}

//=============================================================
// ==============================================================
//                          Main shell loop
// ==============================================================
//First part me jitna bhi code hai wo shell ke liye hai, jo user se command lega aur usko execute karega

int main() {

    setlocale(LC_ALL, "");                 // use system UTF-8 locale
    XSetLocaleModifiers("");               // enable XIM defaults

    // hard-set UTF-8 for the child shell too (extra safety on minimal systems)
    setenv("LANG",   "en_US.UTF-8", 1);
    setenv("LC_ALL", "en_US.UTF-8", 1);

    string userInput;

    // Register custom signal handler for Ctrl + C
    signal(SIGINT, handleSigint);

    // Register custom signal handler for Ctrl + Z
    signal(SIGTSTP, handleCtrlZ);

    // Automatically reap finished background processes (avoid zombies)
    signal(SIGCHLD, SIG_IGN);

    // // start GUI thread (non-blocking)
    // std::thread(guiThreadFunc).detach();


        // Launch the GUI and detach it
    std::thread guiThread(guiThreadFunc);
    guiThread.detach();

    // The main thread must now wait for the GUI to signal it should exit.
    // This prevents the program from closing immediately.
    unique_lock<std::mutex> lock(guiRenderMtx);
    while (!guiShouldExit) {
        guiCv.wait(lock);
    }
    
    // The GUI has closed. The program can now exit.
    cout << "MyTerm is closing." << endl;

    return 0;
}
