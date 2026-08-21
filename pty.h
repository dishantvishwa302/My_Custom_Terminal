#pragma once

#include <sys/types.h>
#include <string>

// One shell session connected through a pseudo-terminal.
// The GUI holds the master fd; myshell runs on the slave side.
struct PtySession {
    int master_fd = -1;
    pid_t child_pid = -1;
};

// Directory that contains the myterm binary (so helpers are on PATH).
std::string executable_dir();

// Fork myshell on a PTY. Returns false on failure.
bool pty_start(PtySession& session);

void pty_stop(PtySession& session);
void pty_set_size(PtySession& session, int rows, int cols);
ssize_t pty_write(PtySession& session, const void* buf, size_t n);
