# MyTerm — Custom Linux Terminal

X11 window + your own shell. Run `./myterm` and you are already in `myshell` (`user@myterm>`).

## What you get

| Binary | What it is |
|--------|------------|
| `myterm` | X11 window. Keys go to a PTY; the child is **myshell**, not bash |
| `myshell` | Your shell: `fork` / `execvp` / `pipe` / `dup2` |
| `history` | Last 1000 commands from `~/.myterm_history` |
| `hsearch` | Ctrl+R history search |
| `multiWatch` | Run several commands in parallel |

## Build

```bash
sudo apt update
sudo apt install g++ libx11-dev libutil-dev

make
```

Needs a graphical session (`DISPLAY` set). On Wayland, Xwayland is enough.

Build **both** `myterm` and `myshell`. The window starts `myshell` from `PATH` (the folder that contains `./myterm` is prepended).

## Run

```bash
./myterm
```

| Try this | What it shows |
|----------|----------------|
| `ls`, `cd`, `ls \| wc -l` | your shell: fork, exec, pipe |
| `echo hi > /tmp/a.txt` then `cat < /tmp/a.txt` | redirection (`dup2`) |
| `sleep 5 &` | background job |
| `history` | last 1000 commands |
| Ctrl+R | `hsearch` |
| `multiWatch [ls, date]` | parallel commands + timestamps |
| `exit` or close the window | quit |

Do **not** compile every `.cpp` into one binary. Each helper has its own `main()`.

## Project files

```
myterm.cpp      main() — locale, then start the GUI
gui.cpp         X11 window, keys, drawing
pty.cpp         openpty + fork + myshell
ansi.cpp        strip OSC / CSI so color codes are not drawn as junk
myshell.cpp     the shell (this is the interview file)
history.cpp     history command
hsearch.cpp     history search
multiWatch.cpp  parallel watch
histpath.h      shared path: ~/.myterm_history
```

See `GUIDE.md` for what changed, and `DESIGNDOC.md` for how each feature works.
