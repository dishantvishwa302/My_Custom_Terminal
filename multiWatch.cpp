#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#include <chrono>
#include <iomanip>
#include <cstring>
#include <ctime>
#include <cerrno>

static std::vector<pid_t> g_children;

static void on_sigint(int) {
    std::cout << "\n[multiWatch] Ctrl+C — stopping children...\n";
    for (pid_t pid : g_children) {
        if (pid > 0) {
            kill(pid, SIGTERM);
        }
    }
}

static std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

static std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t");
    if (a == std::string::npos) {
        return "";
    }
    const auto b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}

// Parse:  [cmd1, cmd2, cmd3]
static std::vector<std::string> parse_commands(const std::string& arg) {
    std::vector<std::string> cmds;
    std::string s = trim(arg);
    if (s.size() < 2 || s.front() != '[' || s.back() != ']') {
        std::cerr << "Usage: multiWatch [cmd1, cmd2, ...]\n";
        return cmds;
    }
    s = s.substr(1, s.size() - 2);

    std::stringstream ss(s);
    std::string part;
    while (std::getline(ss, part, ',')) {
        part = trim(part);
        if (!part.empty() && part.front() == '"' && part.back() == '"' && part.size() >= 2) {
            part = part.substr(1, part.size() - 2);
        }
        if (!part.empty()) {
            cmds.push_back(part);
        }
    }
    return cmds;
}

static void print_chunk(const std::string& cmd, const char* buf) {
    std::cout << "\"" << cmd << "\", " << timestamp() << " :\n"
              << "----------------------------------------------------\n"
              << buf
              << "----------------------------------------------------\n"
              << std::flush;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: multiWatch [cmd1, cmd2, ...]\n";
        return 1;
    }

    std::string joined;
    for (int i = 1; i < argc; ++i) {
        if (i > 1) {
            joined += " ";
        }
        joined += argv[i];
    }

    std::vector<std::string> commands = parse_commands(joined);
    if (commands.empty()) {
        return 1;
    }

    signal(SIGINT, on_sigint);

    const int n = static_cast<int>(commands.size());
    std::vector<pollfd> pfds(n);

    for (int i = 0; i < n; ++i) {
        int pipefd[2];
        if (pipe(pipefd) < 0) {
            perror("pipe");
            return 1;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        }
        if (pid == 0) {
            signal(SIGINT, SIG_DFL);
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[1]);
            execlp("bash", "bash", "-c", commands[static_cast<size_t>(i)].c_str(),
                   static_cast<char*>(nullptr));
            perror("execlp");
            _exit(1);
        }

        close(pipefd[1]);
        pfds[static_cast<size_t>(i)].fd = pipefd[0];
        pfds[static_cast<size_t>(i)].events = POLLIN;
        g_children.push_back(pid);
    }

    int alive = n;
    while (alive > 0) {
        int ret = poll(pfds.data(), static_cast<nfds_t>(n), -1);
        if (ret < 0) {
            if (errno == EINTR) {
                break;
            }
            perror("poll");
            break;
        }

        for (int i = 0; i < n; ++i) {
            if (pfds[i].fd == -1) {
                continue;
            }
            if (pfds[i].revents & POLLIN) {
                char buf[4096];
                ssize_t bytes = read(pfds[i].fd, buf, sizeof(buf) - 1);
                if (bytes > 0) {
                    buf[bytes] = '\0';
                    print_chunk(commands[static_cast<size_t>(i)], buf);
                }
            }
            if (pfds[i].revents & (POLLHUP | POLLERR)) {
                close(pfds[i].fd);
                pfds[i].fd = -1;
                --alive;
            }
        }
    }

    for (pid_t pid : g_children) {
        if (pid > 0) {
            waitpid(pid, nullptr, 0);
        }
    }
    std::cout << "[multiWatch] All commands finished.\n";
    return 0;
}
