#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <unistd.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#include <chrono>
#include <iomanip>
#include <cstring>
#include <algorithm>

// A global list of child process IDs to terminate on Ctrl+C
std::vector<pid_t> child_pids;

// Signal handler for Ctrl+C (SIGINT)
void handle_sigint(int signal_num) {
    (void)signal_num; // Unused parameter
    std::cout << "\n[multiWatch] Ctrl+C received. Terminating child processes..." << std::endl;
    for (pid_t pid : child_pids) {
        if (pid > 0) {
            kill(pid, SIGTERM); // Send termination signal to each child
        }
    }
}

// Function to get a formatted timestamp string
std::string get_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %X");
    return ss.str();
}

// Helper function to trim leading/trailing whitespace from a string
std::string trim(const std::string& str) {
    const std::string whitespace = " \t";
    const auto strBegin = str.find_first_not_of(whitespace);
    if (strBegin == std::string::npos) return ""; // No content
    const auto strEnd = str.find_last_not_of(whitespace);
    const auto strRange = strEnd - strBegin + 1;
    return str.substr(strBegin, strRange);
}

// Main function to parse the specific "[cmd1, cmd2, ...]" format
std::vector<std::string> parse_command_string(const std::string& full_arg) {
    std::vector<std::string> commands;
    std::string content = trim(full_arg);

    // 1. Strip the outer brackets
    if (content.front() == '[' && content.back() == ']') {
        content = content.substr(1, content.length() - 2);
    } else {
        std::cerr << "Error: Commands must be enclosed in brackets [ ]." << std::endl;
        return commands;
    }

    // 2. Split the string by commas
    std::stringstream ss(content);
    std::string segment;
    while(std::getline(ss, segment, ',')) {
        std::string trimmed_cmd = trim(segment);
        if (!trimmed_cmd.empty()) {
            commands.push_back(trimmed_cmd);
        }
    }
    return commands;
}


int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: multiWatch [cmd1, cmd2, ...]" << std::endl;
        return 1;
    }

    // 1. Reassemble the arguments into a single string
    std::string full_command_arg;
    for (int i = 1; i < argc; ++i) {
        full_command_arg += argv[i];
        if (i < argc - 1) {
            full_command_arg += " ";
        }
    }

    // 2. Parse the reassembled string to get individual commands
    std::vector<std::string> commands = parse_command_string(full_command_arg);
    if (commands.empty()) {
        return 1;
    }

    // Register the Ctrl+C signal handler
    signal(SIGINT, handle_sigint);

    int num_commands = commands.size();
    std::vector<struct pollfd> pfds(num_commands);
    child_pids.clear(); // Clear global PIDs

    // 3. The rest of the logic is the same: fork and execute each parsed command
    for (int i = 0; i < num_commands; ++i) {
        int pipefd[2];
        if (pipe(pipefd) == -1) { /* ... error handling ... */ }

        pid_t pid = fork();
        if (pid == 0) { // --- Child Process ---
            signal(SIGINT, SIG_DFL);
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[1]);
            execlp("bash", "bash", "-c", commands[i].c_str(), nullptr);
            perror("execlp");
            exit(1);
        } else { // --- Parent Process ---
            close(pipefd[1]);
            pfds[i].fd = pipefd[0];
            pfds[i].events = POLLIN;
            child_pids.push_back(pid);
        }
    }
    
    // --- The poll loop remains the same ---
    int active_children = num_commands;
    while (active_children > 0) {
        int ret = poll(pfds.data(), num_commands, -1);
        if (ret < 0) {
            if (errno == EINTR) break;
            perror("poll");
            break;
        }

        for (int i = 0; i < num_commands; ++i) {
            if (pfds[i].fd != -1 && pfds[i].revents & POLLIN) {
                char buffer[4096];
                ssize_t bytes_read = read(pfds[i].fd, buffer, sizeof(buffer) - 1);
                if (bytes_read > 0) {
                    buffer[bytes_read] = '\0';
                    std::cout << "\"" << commands[i] << "\", " << get_timestamp() << ":\n" << buffer << std::flush;
                }
            }
            if (pfds[i].fd != -1 && pfds[i].revents & (POLLHUP | POLLERR)) {
                close(pfds[i].fd);
                pfds[i].fd = -1;
                active_children--;
            }
        }
    }

    for (pid_t pid : child_pids) {
        if (pid > 0) waitpid(pid, nullptr, 0);
    }
    std::cout << "[multiWatch] All commands finished." << std::endl;
    return 0;
}