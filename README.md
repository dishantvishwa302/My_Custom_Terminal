# MyTerm — Custom Linux Shell + GUI Terminal  
### CS69201 — Computing Lab Project  

---

## 🧩 Project Structure

| File | Description |
|------|--------------|
| `myterm.cpp` | Main shell logic + GUI integration using Xlib and PTYs |
| `history.cpp` | Maintains shell command history (load, save, append) |
| `hsearch.cpp` | Implements interactive searchable history (Ctrl+R style) |
| `multiWatch.cpp` | Executes multiple commands in parallel (like `watch`) |

---

## 🏗️ Compilation Instructions

Ensure you have all required development libraries installed:
sudo apt update
sudo apt install g++ libx11-dev libutil-dev


Then compile using:
g++ -std=c++17 -Wall -Wextra -lX11 -lutil -o myterm myterm.cpp history.cpp hsearch.cpp multiWatch.cpp

---

## ▶️ Running the Shell

To start the shell:
./myterm


To exit:

exit
or
CTRL + SHIFT + W



## How to Clean & Rebuild

To clean build artifacts:
rm myterm


Then rebuild:
g++ -std=c++17 -Wall -Wextra -lX11 -lutil -o myterm myterm.cpp history.cpp hsearch.cpp multiWatch.cpp

---

