#pragma once

// Hooks LoadLibraryA/W and LoadLibraryExA/W in every currently loaded
// module so that whenever one of them loads a new plugin DLL (many of
// this engine's DLLs -- ActorInfoLib, ScriptInfoLib, the
// commonplugins/rtplugins folders, etc. -- aren't static imports of
// Mad2.exe and only get LoadLibrary'd on demand), we immediately
// re-install the save/registry redirect hooks before that DLL's own
// init code runs. Without this, a newly loaded module's file/registry
// I/O can race ahead of a periodic rehook and be missed entirely.
void InstallLoadLibraryHooks();
