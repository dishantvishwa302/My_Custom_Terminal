// myshell — the shell that runs as soon as you start ./myterm.
// Interview file: tokenise, then fork / execvp / pipe / dup2.

#include "histpath.h"

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <cerrno>
#include <cstddef>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>

using std::string;
using std::vector;
using std::cout;

static const char* kPrompt = "user@myterm> ";
static pid_t g_foreground = -1;
static struct termios g_orig_termios;
static bool g_raw = false;
static vector<string> g_history;

static void on_sigint(int) {
    if (g_foreground > 0) {
        kill(-g_foreground, SIGINT);
    }
}

static void on_sigtstp(int) {
    if (g_foreground > 0) {
        kill(-g_foreground, SIGTSTP);
    }
}

static void disable_raw() {
    if (g_raw) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
        g_raw = false;
    }
}

static void enable_raw() {
    if (g_raw) {
        return;
    }
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) == -1) {
        return;
    }
    struct termios raw = g_orig_termios;
    // ISIG off: Ctrl+C / Ctrl+Z / Ctrl+R arrive as bytes at the prompt,
    // instead of the kernel turning them into signals.
    raw.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON | IEXTEN | ISIG));
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    g_raw = true;
}

static void load_history() {
    std::ifstream in(myterm_history_path());
    string line;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            g_history.push_back(line);
        }
    }
    const size_t cap = 10000;
    if (g_history.size() > cap) {
        g_history.erase(g_history.begin(), g_history.end() - static_cast<std::ptrdiff_t>(cap));
    }
}

static void save_history(const string& cmd) {
    if (cmd.empty()) {
        return;
    }
    g_history.push_back(cmd);
    std::ofstream out(myterm_history_path(), std::ios::app);
    if (out) {
        out << cmd << '\n';
    }
}

// Redraw the current input.
// Start with \r so the GUI replaces the line. Do NOT send a second \r
// afterwards — the GUI treats \r as "wipe this line", which made typing
// invisible even though the shell still had the command.
static void redraw_line(const string& buf, int cursor) {
    cout << '\r' << kPrompt << buf << "\033[K";
    int extra = static_cast<int>(buf.size()) - cursor;
    for (int i = 0; i < extra; ++i) {
        cout << '\b';
    }
    cout.flush();
}

// Read one command line in raw mode (Ctrl+A / Ctrl+E / arrows / backspace).
// Returns false on EOF (Ctrl+D on an empty line).
static bool read_line(string& out) {
    out.clear();
    string buf;
    int cursor = 0;
    int hist_pos = static_cast<int>(g_history.size());
    string saved;

    cout << kPrompt << std::flush;

    while (true) {
        char c = 0;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n < 0) {
            if (errno == EINTR) {
                cout << '\n' << kPrompt << std::flush;
                buf.clear();
                cursor = 0;
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }

        if (c == '\r' || c == '\n') {
            cout << '\n';
            out = buf;
            return true;
        }

        if (c == 0x03) { // Ctrl+C — cancel the line, keep the shell
            cout << "^C\n" << kPrompt << std::flush;
            buf.clear();
            cursor = 0;
            hist_pos = static_cast<int>(g_history.size());
            continue;
        }
        if (c == 0x1A) { // Ctrl+Z at the prompt: nothing is running
            continue;
        }
        if (c == 0x04) { // Ctrl+D
            if (buf.empty()) {
                cout << '\n';
                return false;
            }
            continue;
        }

        if (c == 0x01) { // Ctrl+A — start of line
            cursor = 0;
            redraw_line(buf, cursor);
            continue;
        }
        if (c == 0x05) { // Ctrl+E — end of line
            cursor = static_cast<int>(buf.size());
            redraw_line(buf, cursor);
            continue;
        }
        if (c == 0x0c) { // Ctrl+L — ask the GUI to clear (form-feed)
            cout << '\f' << std::flush;
            redraw_line(buf, cursor);
            continue;
        }
        if (c == 0x12) { // Ctrl+R — run hsearch
            cout << '\n';
            out = "hsearch";
            return true;
        }
        if (c == 0x7f || c == 0x08) { // Backspace
            if (cursor > 0) {
                buf.erase(static_cast<size_t>(cursor - 1), 1);
                --cursor;
                redraw_line(buf, cursor);
            }
            continue;
        }

        if (c == 0x1B) { // Arrow keys: ESC [ A/B/C/D
            char seq[2] = {0, 0};
            if (read(STDIN_FILENO, &seq[0], 1) != 1) {
                continue;
            }
            if (seq[0] != '[') {
                continue;
            }
            if (read(STDIN_FILENO, &seq[1], 1) != 1) {
                continue;
            }
            if (seq[1] == 'C' && cursor < static_cast<int>(buf.size())) {
                ++cursor;
            } else if (seq[1] == 'D' && cursor > 0) {
                --cursor;
            } else if (seq[1] == 'A' && !g_history.empty()) { // Up
                if (hist_pos == static_cast<int>(g_history.size())) {
                    saved = buf;
                }
                if (hist_pos > 0) {
                    --hist_pos;
                    buf = g_history[static_cast<size_t>(hist_pos)];
                    cursor = static_cast<int>(buf.size());
                }
            } else if (seq[1] == 'B') { // Down
                if (hist_pos < static_cast<int>(g_history.size())) {
                    ++hist_pos;
                    if (hist_pos == static_cast<int>(g_history.size())) {
                        buf = saved;
                    } else {
                        buf = g_history[static_cast<size_t>(hist_pos)];
                    }
                    cursor = static_cast<int>(buf.size());
                }
            }
            redraw_line(buf, cursor);
            continue;
        }

        if (static_cast<unsigned char>(c) >= 32) {
            buf.insert(static_cast<size_t>(cursor), 1, c);
            ++cursor;
            redraw_line(buf, cursor);
        }
    }
}

static vector<string> tokenize(const string& line) {
    vector<string> tokens;
    string cur;
    bool in_quote = false;

    for (char ch : line) {
        if (ch == '"') {
            in_quote = !in_quote;
            continue;
        }
        if (!in_quote && (ch == ' ' || ch == '\t')) {
            if (!cur.empty()) {
                tokens.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(ch);
        }
    }
    if (!cur.empty()) {
        tokens.push_back(cur);
    }
    return tokens;
}

struct Command {
    vector<string> argv;
    string infile;
    string outfile;
};

static vector<Command> parse_pipeline(vector<string> tokens, bool& background) {
    background = false;
    if (!tokens.empty() && tokens.back() == "&") {
        background = true;
        tokens.pop_back();
    }

    vector<Command> cmds;
    Command cur;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i] == "|") {
            cmds.push_back(cur);
            cur = Command{};
        } else if (tokens[i] == "<" && i + 1 < tokens.size()) {
            cur.infile = tokens[++i];
        } else if (tokens[i] == ">" && i + 1 < tokens.size()) {
            cur.outfile = tokens[++i];
        } else {
            cur.argv.push_back(tokens[i]);
        }
    }
    if (!cur.argv.empty() || !cur.infile.empty() || !cur.outfile.empty()) {
        cmds.push_back(cur);
    }
    return cmds;
}

static vector<char*> make_argv(const vector<string>& args) {
    vector<char*> argv;
    for (const string& s : args) {
        argv.push_back(const_cast<char*>(s.c_str()));
    }
    argv.push_back(nullptr);
    return argv;
}

static void apply_redirects(const Command& cmd) {
    if (!cmd.infile.empty()) {
        int fd = open(cmd.infile.c_str(), O_RDONLY);
        if (fd < 0) {
            perror("open <");
            _exit(1);
        }
        dup2(fd, STDIN_FILENO);
        close(fd);
    }
    if (!cmd.outfile.empty()) {
        int fd = open(cmd.outfile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            perror("open >");
            _exit(1);
        }
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }
}

static bool builtin(const vector<Command>& cmds, bool background) {
    if (cmds.size() != 1 || background) {
        return false;
    }
    const vector<string>& argv = cmds[0].argv;
    if (argv.empty()) {
        return true;
    }
    if (argv[0] == "exit") {
        disable_raw();
        exit(0);
    }
    if (argv[0] == "cd") {
        const char* dir = (argv.size() > 1) ? argv[1].c_str() : getenv("HOME");
        if (!dir) {
            dir = "/";
        }
        if (chdir(dir) != 0) {
            perror("cd");
        }
        return true;
    }
    return false;
}

static void run_pipeline(const vector<Command>& cmds, bool background) {
    const int n = static_cast<int>(cmds.size());
    if (n == 0) {
        return;
    }

    vector<int> pipes(2 * std::max(0, n - 1), -1);
    for (int i = 0; i < n - 1; ++i) {
        if (pipe(&pipes[static_cast<size_t>(i) * 2]) < 0) {
            perror("pipe");
            return;
        }
    }

    vector<pid_t> pids(static_cast<size_t>(n), -1);
    pid_t pgid = 0;

    for (int i = 0; i < n; ++i) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            break;
        }
        if (pid == 0) {
            signal(SIGINT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            setpgid(0, (i == 0) ? 0 : pgid);

            if (i > 0 && cmds[static_cast<size_t>(i)].infile.empty()) {
                dup2(pipes[static_cast<size_t>(i - 1) * 2], STDIN_FILENO);
            }
            if (i < n - 1 && cmds[static_cast<size_t>(i)].outfile.empty()) {
                dup2(pipes[static_cast<size_t>(i) * 2 + 1], STDOUT_FILENO);
            }
            apply_redirects(cmds[static_cast<size_t>(i)]);

            for (int fd : pipes) {
                if (fd >= 0) {
                    close(fd);
                }
            }

            if (cmds[static_cast<size_t>(i)].argv.empty()) {
                _exit(0);
            }
            vector<char*> argv = make_argv(cmds[static_cast<size_t>(i)].argv);
            execvp(argv[0], argv.data());
            perror("execvp");
            _exit(1);
        }

        pids[static_cast<size_t>(i)] = pid;
        if (i == 0) {
            pgid = pid;
        }
        setpgid(pid, pgid);
    }

    for (int fd : pipes) {
        if (fd >= 0) {
            close(fd);
        }
    }

    if (background) {
        cout << "[background PID " << pgid << "]\n";
        return;
    }

    // Give the terminal to the command so Ctrl+C / Ctrl+Z hit the child, not us.
    signal(SIGTTOU, SIG_IGN);
    tcsetpgrp(STDIN_FILENO, pgid);

    g_foreground = pgid;
    for (pid_t pid : pids) {
        if (pid <= 0) {
            continue;
        }
        int status = 0;
        if (waitpid(pid, &status, WUNTRACED) > 0 && WIFSTOPPED(status)) {
            cout << "\n[Stopped PID " << pid << "]\n";
        }
    }
    g_foreground = -1;

    tcsetpgrp(STDIN_FILENO, getpgrp());
}

int main() {
    signal(SIGINT, on_sigint);
    signal(SIGTSTP, on_sigtstp);
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    atexit(disable_raw);

    load_history();
    enable_raw();

    while (true) {
        while (waitpid(-1, nullptr, WNOHANG) > 0) {
        }

        string line;
        if (!read_line(line)) {
            break;
        }
        if (line.empty()) {
            continue;
        }

        if (line != "hsearch") {
            save_history(line);
        }

        vector<string> tokens = tokenize(line);
        bool background = false;
        vector<Command> cmds = parse_pipeline(std::move(tokens), background);
        if (cmds.empty()) {
            continue;
        }

        disable_raw();
        if (!builtin(cmds, background)) {
            run_pipeline(cmds, background);
        }
        enable_raw();
    }

    disable_raw();
    return 0;
}
