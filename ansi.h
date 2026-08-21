#pragma once

#include <string>

// Strip OSC sequences (ESC ] ... BEL) and optionally copy the window title.
void strip_osc(const std::string& in, std::string& out, std::string* title);

// Strip CSI sequences (ESC [ ... final-byte).
// `pending` holds a sequence that was split across two PTY reads.
void strip_csi(std::string& pending, const std::string& in, std::string& out);
