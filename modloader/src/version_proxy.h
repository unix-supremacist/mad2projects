#pragma once

// Loads the real system version.dll and resolves the function pointers our
// exported proxy stubs forward to. Must run before anything calls one of
// this DLL's exported version.dll functions (i.e. as early as possible in
// DllMain).
void InitVersionProxy();
