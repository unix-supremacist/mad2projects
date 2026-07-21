#pragma once

#include <windows.h>

// Resolves the real (unhooked) CreateFileA/CreateFileW/NtCreateFile from
// kernel32/ntdll.
void InitFileRedirect();

// Scans every currently loaded module and redirects its CreateFileA/W and
// NtCreateFile imports to our hooks (see file_redirect.cpp's NtCreateFile
// section for why both are needed -- CreateFileA/W alone misses level
// archive opens, which route through NtCreateFile directly). Safe to call
// again later (e.g. after loading mod DLLs) to pick up newly loaded
// modules -- though for that specific case, prefer
// PatchFileRedirectHooksInModule below, which is O(1) in the number of
// already-loaded modules instead of re-scanning everything.
void InstallFileRedirectHooks();

// Patches CreateFileA/W and NtCreateFile in a single module (typically one
// just returned by LoadLibrary -- see loadlibrary_hook.cpp). Scoped
// alternative to calling InstallFileRedirectHooks() again on every new
// module load: that re-enumerates and re-patches EVERY currently loaded
// module each time (an O(n) Toolhelp32Snapshot + full IAT walk), which
// compounds into real, measured boot-time cost once more than one mod does
// the same thing on every LoadLibrary call (see mad2levelredirectmod's
// identical helper, and the CLAUDE.md-adjacent history of this fix for the
// actual symptom: mod loading visibly stalling partway through
// mad2/mods/).
void PatchFileRedirectHooksInModule(HMODULE module);
