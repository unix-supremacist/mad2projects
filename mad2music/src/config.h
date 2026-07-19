#pragma once

// Read-only config.cfg reader. mad2music does NOT own config.cfg or seed
// defaults into it -- mad2config.dll (loaded inside the Wine process) is
// the sole owner of that "read or seed default" contract for every mod,
// mad2podcastmod's [Podcast] section included (see its own LoadConfig()).
// mad2music just needs to agree on the same key names and fall back to
// hardcoded defaults if the file or keys are missing (e.g. running
// standalone before the game has ever been launched once).
struct Config {
    bool musicEnabled = true;
    bool musicAllowCopyright = true;
    bool musicAllowLyrics = true;
    bool musicAllowCustom = true;
    int controlPort = 47070;  // MAD2PODCAST_DEFAULT_CONTROL_PORT, kept in sync by hand
    int statusPort = 47071;   // MAD2PODCAST_DEFAULT_STATUS_PORT, kept in sync by hand
};

// Reads ./config.cfg (relative to cwd, same as every other mod -- see
// log.h). Missing file/section/keys silently keep the struct's defaults.
Config LoadConfig();
