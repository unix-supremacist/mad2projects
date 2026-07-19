#include "config.h"

#include <fstream>
#include <string>

namespace {

std::string Trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

}  // namespace

Config LoadConfig() {
    Config cfg;

    std::ifstream in("config.cfg");
    if (!in.is_open()) return cfg;

    std::string section;
    std::string line;
    while (std::getline(in, line)) {
        std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == '@') continue;

        if (trimmed.front() == '[' && trimmed.back() == ']') {
            section = trimmed.substr(1, trimmed.size() - 2);
            continue;
        }

        size_t eq = trimmed.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(trimmed.substr(0, eq));
        std::string val = Trim(trimmed.substr(eq + 1));

        if (section == "Music") {
            if (key == "Enabled") cfg.musicEnabled = (val == "true");
            else if (key == "AllowCopyright") cfg.musicAllowCopyright = (val == "true");
            else if (key == "AllowLyrics") cfg.musicAllowLyrics = (val == "true");
            else if (key == "AllowCustom") cfg.musicAllowCustom = (val == "true");
        } else if (section == "Podcast") {
            if (key == "ControlPort") {
                try {
                    cfg.controlPort = std::stoi(val);
                } catch (...) {
                }
            } else if (key == "StatusPort") {
                try {
                    cfg.statusPort = std::stoi(val);
                } catch (...) {
                }
            }
        }
    }

    return cfg;
}
