#include "pty.h"

#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <pty.h>

std::string executable_dir() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n < 0) {
        return ".";
    }
    buf[n] = '\0';
    std::string path(buf);
    auto slash = path.rfind('/');
    return (slash == std::string::npos) ? std::string(".") : path.substr(0, slash);
}

static void close_fd(int& fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

bool pty_start(PtySession& session) {
    int master = -1;
    int slave = -1;
    struct termios term{};
    struct winsize win = {24, 80, 0, 0};

    // Cooked settings for the slave. myshell switches to raw mode at the prompt
    // and restores this so Ctrl+C / Ctrl+Z become SIGINT / SIGTSTP while a
    // command is running.
    memset(&term, 0, sizeof(term));
    term.c_iflag = ICRNL;
    term.c_oflag = OPOST;
    term.c_lflag = ECHO | ICANON | ISIG | IEXTEN;
    term.c_cc[VMIN] = 1;
    term.c_cc[VTIME] = 0;
    term.c_cc[VINTR] = 0x03;  // Ctrl+C
    term.c_cc[VQUIT] = 0x1C;  // Ctrl+backslash
    term.c_cc[VERASE] = 0x7f; // Backspace
    term.c_cc[VKILL] = 0x15;
    term.c_cc[VEOF] = 0x04;   // Ctrl+D
    term.c_cc[VSUSP] = 0x1A;  // Ctrl+Z

    if (openpty(&master, &slave, nullptr, &term, &win) < 0) {
        perror("openpty");
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(master);
        close(slave);
        return false;
    }

    if (pid == 0) {
        // Child: become the session leader and attach to the slave PTY.
        close(master);
        setsid();
        ioctl(slave, TIOCSCTTY, 0);
        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        if (slave > 2) {
            close(slave);
        }

        setenv("TERM", "xterm-256color", 1);
        setenv("LANG", "en_US.UTF-8", 1);
        setenv("LC_ALL", "en_US.UTF-8", 1);

        // Put this project's binaries first so history / hsearch / multiWatch resolve.
        std::string bin = executable_dir();
        const char* old_path = getenv("PATH");
        std::string path = bin + ":" + (old_path ? old_path : "/usr/local/bin:/usr/bin:/bin");
        setenv("PATH", path.c_str(), 1);

        execlp("myshell", "myshell", static_cast<char*>(nullptr));
        perror("execlp myshell (did you run make?)");
        _exit(127);
    }

    close(slave);
    int flags = fcntl(master, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(master, F_SETFL, flags | O_NONBLOCK);
    }

    session.master_fd = master;
    session.child_pid = pid;
    return true;
}

void pty_stop(PtySession& session) {
    if (session.child_pid > 0) {
        kill(session.child_pid, SIGHUP);
        session.child_pid = -1;
    }
    close_fd(session.master_fd);
}

void pty_set_size(PtySession& session, int rows, int cols) {
    if (session.master_fd < 0) {
        return;
    }
    struct winsize ws{};
    ws.ws_row = static_cast<unsigned short>(rows);
    ws.ws_col = static_cast<unsigned short>(cols);
    ioctl(session.master_fd, TIOCSWINSZ, &ws);
}

ssize_t pty_write(PtySession& session, const void* buf, size_t n) {
    if (session.master_fd < 0) {
        return -1;
    }
    const char* p = static_cast<const char*>(buf);
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = write(session.master_fd, p + sent, n - sent);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        sent += static_cast<size_t>(w);
    }
    return static_cast<ssize_t>(sent);
}
