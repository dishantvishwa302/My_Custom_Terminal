#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstdlib> // for getenv()
#include <deque>

// This function reads the bash history file and prints the last N commands.
void show_history(int num_commands_to_show) {
    // Find the user's home directory to locate the history file.
    const char* home_dir = getenv("HOME");
    if (!home_dir) {
        std::cerr << "Error: Could not find home directory." << std::endl;
        return;
    }

    std::string history_file_path = std::string(home_dir) + "/.bash_history";

    std::ifstream history_file(history_file_path);
    if (!history_file.is_open()) {
        std::cerr << "Error: Could not open history file at " << history_file_path << std::endl;
        return;
    }

    // Use a deque to efficiently store the last N lines.
    std::deque<std::string> recent_lines;
    std::string line;

    while (std::getline(history_file, line)) {
        recent_lines.push_back(line);
        if (recent_lines.size() > num_commands_to_show) {
            recent_lines.pop_front();
        }
    }

    // Print the stored commands with line numbers.
    int line_num = 1;
    for (const auto& cmd : recent_lines) {
        std::cout << "  " << line_num++ << "  " << cmd << std::endl;
    }
}

int main() {
    // The project requires showing the most recent 1000 commands.
    const int COMMAND_LIMIT = 1000;
    show_history(COMMAND_LIMIT);
    return 0;
}

