# DESIGNDOC – MyTerm Custom Shell

## Overview
MyTerm is a custom terminal emulator built in C++ with Xlib GUI and PTY backend, supporting modern shell features.

---

## 1. Basic Shell Execution
- Implemented using `fork()`, `execvp()`, and `wait()`.
- Supports basic command parsing and built-ins (`cd`, `exit`).

---

## 2. I/O Redirection and Pipes
- Handles `<`, `>`, and `|` using `dup2()`.
- Supports single and multiple pipes dynamically.

---

## 3. Signal Handling & Background Jobs
- Handles Ctrl+C via `SIGINT`.
- Runs background jobs using `&`.
- Tracks active processes.

---

## 4. Command History
- Saves history persistently to a file.
- Allows navigation with arrow keys.

---

## 5. History Search (`hsearch.cpp`)
- Implements interactive fuzzy search through command history.

---

## 6. MultiWatch (`multiWatch.cpp`)
- Executes multiple commands in parallel.
- Uses `poll()` to capture real-time outputs.

---

## 7. GUI (Xlib)
- Provides graphical shell window using Xlib.
- Captures keyboard input and renders terminal text.

---

## 8. PTY Integration
- Connects GUI and shell I/O via pseudo-terminal.
- Enables interactive and isolated shell sessions.

---


