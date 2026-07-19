#pragma once

// Loads every .dll found in the "mods" folder next to Mad2.exe. Each mod's
// own DllMain runs as it's loaded, which is its entry point for installing
// further hooks or patches.
void LoadMods();
