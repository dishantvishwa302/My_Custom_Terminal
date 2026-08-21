#include "histpath.h"

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>
#include <termios.h>
#include <stdlib.h>

struct termios orig_termios;

static void disable_raw() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

static void enable_raw() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static std::vector<std::string> read_history() {
    std::vector<std::string> history;
    std::ifstream in(myterm_history_path());
    std::string line;
    while (std::getline(in, line)) {
        history.push_back(line);
    }
    return history;
}

// Longest common substring length (consecutive characters, not subsequence).
static int lcs_len(const std::string& a, const std::string& b) {
    int best = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        for (size_t j = 0; j < b.size(); ++j) {
            int len = 0;
            while (i + len < a.size() && j + len < b.size() && a[i + len] == b[j + len]) {
                ++len;
            }
            if (len > best) {
                best = len;
            }
        }
    }
    return best;
}

int main() {
    enable_raw();
    std::cout << "Enter search term: " << std::flush;

    std::string term;
    char c;
    while (read(STDIN_FILENO, &c, 1) == 1 && c != '\n' && c != '\r') {
        if (c == 127 || c == 8) {
            if (!term.empty()) {
                term.pop_back();
                std::cout << "\b \b" << std::flush;
            }
        } else if (c >= 32) {
            term.push_back(c);
            std::cout << c << std::flush;
        }
    }
    std::cout << "\n";
    disable_raw();

    if (term.empty()) {
        return 0;
    }

    std::vector<std::string> history = read_history();
    if (history.empty()) {
        std::cout << "No match for search term in history\n";
        return 0;
    }

    // 1) most recent exact match
    for (int i = static_cast<int>(history.size()) - 1; i >= 0; --i) {
        if (history[static_cast<size_t>(i)] == term) {
            std::cout << history[static_cast<size_t>(i)] << "\n";
            return 0;
        }
    }

    // 2) largest longest-common-substring, length > 2
    std::string best;
    int best_len = 0;
    for (int i = static_cast<int>(history.size()) - 1; i >= 0; --i) {
        int len = lcs_len(history[static_cast<size_t>(i)], term);
        if (len > best_len) {
            best_len = len;
            best = history[static_cast<size_t>(i)];
        }
    }

    if (best_len > 2) {
        std::cout << best << "\n";
    } else {
        std::cout << "No match for search term in history\n";
    }
    return 0;
}
