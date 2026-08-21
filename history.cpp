#include "histpath.h"

#include <iostream>
#include <fstream>
#include <string>
#include <deque>

// history — print the most recent 1000 commands from ~/.myterm_history
int main() {
    const int limit = 1000;
    std::ifstream in(myterm_history_path());
    if (!in) {
        std::cerr << "No history yet. Run some commands first.\n";
        return 0;
    }

    std::deque<std::string> recent;
    std::string line;
    while (std::getline(in, line)) {
        recent.push_back(line);
        if (static_cast<int>(recent.size()) > limit) {
            recent.pop_front();
        }
    }

    int n = 1;
    for (const auto& cmd : recent) {
        std::cout << "  " << n++ << "  " << cmd << "\n";
    }
    return 0;
}
