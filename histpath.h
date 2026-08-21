#pragma once

#include <string>
#include <cstdlib>

// All history tools share this file (not bash's ~/.bash_history).
inline std::string myterm_history_path() {
    const char* home = getenv("HOME");
    if (!home || !home[0]) {
        return ".myterm_history";
    }
    return std::string(home) + "/.myterm_history";
}
