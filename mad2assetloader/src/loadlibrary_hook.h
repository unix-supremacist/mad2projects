#pragma once

// Hooks LoadLibraryA/W and LoadLibraryExA/W in every currently loaded
// module so that whenever one of them loads a new plugin DLL (many of
// this engine's DLLs -- ActorInfoLib, ScriptInfoLib, TfbHavokLibrary, and
// whatever module actually opens level archives under Content/Streams/win
// -- aren't static imports of Mad2.exe and only get LoadLibrary'd on
// demand), we immediately re-install the file redirect hooks in it too.
// Without this, InstallFileRedirectHooks() only ever sees the initial
// static-import module set from its own DllMain-time snapshot, and
// silently never catches file opens made by anything loaded afterward --
// which turned out to be exactly what was happening for level archives
// (confirmed via mad2levelredirectmod's identical bug: zero CreateFileA/W
// hits ever logged for a Content/Streams/win/*.arc open across a full
// session that demonstrably loaded a level). See
// mad2saveloader/src/loadlibrary_hook.h for the sibling mod that already
// had this fix; this is the same technique applied here.
void InstallFileRedirectLoadLibraryHooks();
