# DESIGNDOC — MyTerm

Two layers, both yours:

1. **GUI** (`gui.cpp`) — X11 window that looks like a terminal.
2. **Shell** (`myshell.cpp`) — parses the line and calls `fork` / `execvp` / `pipe` / `dup2`.

`./myterm` starts the GUI, which starts **myshell** on a PTY. There is no bash in the middle.

---

## 1. GUI (X11)

`gui.cpp` uses `XOpenDisplay` / `XCreateSimpleWindow` / `XMapWindow`.

- A `deque<string>` is the screen buffer.
- `poll()` waits on the PTY master. `XPending()` handles keys.
- Keys are `write()`n to the PTY. Output is drawn with `Xutf8DrawString`.
- Click the window to focus. Close the window, or type `exit`, to quit.

One session, no tab bar.

---

## 2. PTY + myshell

`pty.cpp`:

1. `openpty()` — master fd for the GUI, slave fd for the shell.
2. `fork()` — child: `setsid()`, `ioctl(TIOCSCTTY)`, `dup2` slave onto stdin/stdout/stderr.
3. `execlp("myshell", "myshell", NULL)`.

The GUI is a dumb terminal. It does not parse `|` or `<`. **myshell** does.

The directory of `./myterm` is prepended to `PATH` so `history`, `hsearch`, and `multiWatch` run without `./`.

---

## 3. Cursor (why Ctrl+A / Ctrl+E work)

The GUI keeps a column index on the current line, like a real terminal:

- `\r` — column 0 (does **not** wipe the line)
- `\b` — move left
- `ESC[K` — erase from cursor to end of line
- letters — overwrite at the cursor

myshell redraws with: `\r` + prompt + text + `ESC[K` + `\b` until the caret is in the right place.

---

## 4. Custom shell (`myshell.cpp`)

This is the file to open in an interview.

| Feature | How |
|---------|-----|
| Line editing | raw mode (`termios`, **ISIG off** at the prompt): Ctrl+A start, Ctrl+E end, arrows, backspace |
| External command | `fork` + `execvp` + `waitpid` |
| `cd` / `exit` | builtins, no `fork` |
| `<` / `>` | `open` + `dup2` onto fd 0 or 1 |
| `cmd1 \| cmd2 \| cmd3` | N−1 `pipe()`s, N children, `dup2` |
| `cmd &` | skip `waitpid` |
| Ctrl+C | at prompt: cancel the line. while a command runs: `tcsetpgrp` so SIGINT goes to the child |
| Ctrl+Z | `waitpid(..., WUNTRACED)` + SIGTSTP — stop the command, return to the prompt |
| Ctrl+R | run `hsearch` |
| History | append to `~/.myterm_history` (cap 10000 in memory) |

Raw mode is **off** while a command runs, so `ls` and `hsearch` get a normal tty. `tcsetpgrp` gives that tty to the child so Ctrl+C/Z do not stop the shell itself.

---

## 5. `history`

Reads `~/.myterm_history`, keeps the last 1000 lines in a `deque`, prints them numbered.

---

## 6. `hsearch` (Ctrl+R)

myshell sees Ctrl+R (ASCII `0x12`) and runs the `hsearch` binary.

1. Exact match, newest first.
2. Else longest common **substring** with length > 2.
3. Else `No match for search term in history`.

---

## 7. `multiWatch`

```text
multiWatch [ls, date]
```

For each command: `pipe` + `fork` + `execlp("bash", "-c", cmd)` (bash here is only used to parse that one command string). Parent `poll()`s the read ends.

Ctrl+C sends `SIGTERM` to every child.

---

## Syscalls to name

`fork`, `execvp` / `execlp`, `waitpid`, `pipe`, `dup2`, `open` / `read` / `write`, `poll`, `openpty`, `setsid`, `ioctl(TIOCSCTTY)`, `tcsetattr`, `signal` / `kill`, `chdir`.
