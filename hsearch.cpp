#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstdlib>
#include <algorithm>
#include <unistd.h>
#include <termios.h>

// --- Terminal Control Functions ---
struct termios orig_termios;

void disableRawMode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void enableRawMode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disableRawMode); // Ensure raw mode is disabled on exit
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

// --- History Search Logic (mostly the same) ---
std::vector<std::string> read_history_file() {
    std::vector<std::string> history;
    const char* home_dir = getenv("HOME");
    if (!home_dir) return history;
    std::string history_file_path = std::string(home_dir) + "/.bash_history";
    std::ifstream history_file(history_file_path);
    std::string line;
    while (std::getline(history_file, line)) {
        history.push_back(line);
    }
    return history;
}

int longest_common_substring_length(const std::string& a, const std::string& b) {
    int max_len = 0;
    // (This function remains the same as before)
    for (size_t i = 0; i < a.length(); ++i) {
        for (size_t j = 0; j < b.length(); ++j) {
            int len = 0;
            while (i + len < a.length() && j + len < b.length() && a[i + len] == b[j + len]) {
                len++;
            }
            if (len > max_len) max_len = len;
        }
    }
    return max_len;
}

// --- Main Function with Manual Input Handling ---
int main() {
    enableRawMode(); // Take control of the terminal

    std::cout << "Enter search term: " << std::flush;
    std::string search_term;
    char c;
    while (read(STDIN_FILENO, &c, 1) == 1 && c != '\n') {
        if (c == 127 || c == 8) { // Handle backspace
            if (!search_term.empty()) {
                search_term.pop_back();
                std::cout << "\b \b" << std::flush; // Erase visually
            }
        } else {
            search_term.push_back(c);
            std::cout << c << std::flush; // Echo character back
        }
    }
    std::cout << "\r\n"; // Move to a new line after input

    disableRawMode(); // Give control back to the shell

    // --- Search logic remains the same ---
    if (search_term.empty()) return 0;

    std::vector<std::string> history = read_history_file();
    if (history.empty()) {
        std::cout << "No match for search term in history" << std::endl;
        return 0;
    }

    for (int i = history.size() - 1; i >= 0; --i) {
        if (history[i] == search_term) {
            std::cout << history[i] << std::endl;
            return 0;
        }
    }

    std::string best_match_command = "";
    int max_len = 0;
    for (int i = history.size() - 1; i >= 0; --i) {
        int len = longest_common_substring_length(history[i], search_term);
        if (len > max_len) {
            max_len = len;
            best_match_command = history[i];
        }
    }
    
    if (max_len > 2) {
        std::cout << best_match_command << std::endl;
    } else {
        std::cout << "No match for search term in history" << std::endl;
    }

    return 0;
}