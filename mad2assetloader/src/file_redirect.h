#pragma once

#include <windows.h>

// Resolves the real (unhooked) CreateFileA/CreateFileW from kernel32.
void InitFileRedirect();

// Scans every currently loaded module and redirects its CreateFileA/W
// imports to our hooks. Safe to call again later (e.g. after loading mod
// DLLs) to pick up newly loaded modules -- though for that specific case,
// prefer PatchFileRedirectHooksInModule below, which is O(1) in the
// number of already-loaded modules instead of re-scanning everything.
void InstallFileRedirectHooks();

// Patches CreateFileA/W in a single module (typically one just returned by
// LoadLibrary -- see loadlibrary_hook.cpp). Scoped alternative to calling
// InstallFileRedirectHooks() again on every new module load: that
// re-enumerates and re-patches EVERY currently loaded module each time
// (an O(n) Toolhelp32Snapshot + full IAT walk), which compounds into
// real, measured boot-time cost once more than one mod does the same
// thing on every LoadLibrary call (see mad2levelredirectmod's identical
// helper, and the CLAUDE.md-adjacent history of this fix for the actual
// symptom: mod loading visibly stalling partway through mad2/mods/).
void PatchFileRedirectHooksInModule(HMODULE module);
