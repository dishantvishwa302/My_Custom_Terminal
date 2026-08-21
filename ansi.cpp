#include "ansi.h"

void strip_osc(const std::string& in, std::string& out, std::string* title) {
    out.clear();
    const size_t n = in.size();
    size_t i = 0;

    while (i < n) {
        unsigned char c = static_cast<unsigned char>(in[i]);
        // OSC: ESC ] ... BEL, or ESC ] ... ESC backslash
        if (c == 0x1B && i + 1 < n && in[i + 1] == ']') {
            size_t j = i + 2;
            while (j < n && in[j] != 0x07) {
                if (in[j] == 0x1B && j + 1 < n && in[j + 1] == '\\') {
                    break;
                }
                ++j;
            }
            if (title) {
                std::string payload = in.substr(i + 2, j - (i + 2));
                auto semi = payload.find(';');
                std::string t = (semi == std::string::npos) ? payload : payload.substr(semi + 1);
                while (!t.empty() && (t.back() == '\r' || t.back() == '\n')) {
                    t.pop_back();
                }
                if (!t.empty()) {
                    *title = t;
                }
            }
            if (j < n && in[j] == 0x07) {
                i = j + 1;
            } else if (j + 1 < n && in[j] == 0x1B && in[j + 1] == '\\') {
                i = j + 2;
            } else {
                i = j;
            }
            continue;
        }
        out.push_back(static_cast<char>(c));
        ++i;
    }
}

void strip_csi(std::string& pending, const std::string& in, std::string& out) {
    out.clear();
    std::string work = pending.empty() ? in : (pending + in);
    pending.clear();

    size_t i = 0;
    while (i < work.size()) {
        unsigned char c = static_cast<unsigned char>(work[i]);
        if (c == 0x1B && i + 1 < work.size() && work[i + 1] == '[') {
            size_t j = i + 2;
            while (j < work.size()) {
                unsigned char d = static_cast<unsigned char>(work[j]);
                if (d >= 0x40 && d <= 0x7E) {
                    i = j + 1; // skip the whole CSI
                    break;
                }
                ++j;
            }
            if (j >= work.size()) {
                pending = work.substr(i); // incomplete sequence; wait for next read
                break;
            }
            continue;
        }
        // Other ESC-sequences (2 bytes): skip them so they are not drawn.
        if (c == 0x1B && i + 1 < work.size()) {
            i += 2;
            continue;
        }
        out.push_back(static_cast<char>(c));
        ++i;
    }
}
