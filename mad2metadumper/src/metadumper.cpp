// Diagnostic runtime reflection-metadata dumper -- NOT a gameplay mod, same
// "not meant to stay loaded permanently" status as mad2rhythmlogger/
// mad2climbprobe (see their own file headers for the precedent).
//
// Why this exists: docs/SCRIPT_FORMAT.md's own "Honest status" section
// says Mad2's hand-built TFBScriptInfo field schema (mad2iga/
// tfbscript_pc_field_schema.json) captures each class's *own* newly
// registered fields only, extracted one arkRegisterInitialize@<Class>
// disassembly at a time -- it does not merge in *inherited* base-class
// fields at all (confirmed empirically: OpCode.internalFlags, a real
// PC-verified field, never appears in a script-dump of any OpCode
// subclass, because ClassSchema() looks up a class's bare name only, with
// no base-class walk). Rather than keep hand-deriving parent-class field
// lists one registrar function at a time, this mod goes straight to the
// engine's own live reflection data: the Alchemy engine (igCore.dll)
// keeps a global class registry (igArkCore, reachable via the exported
// Gap::Core::ArkCore_function()) whose entries (igMetaObject) each expose
// a real parent-class pointer and a *complete* field list (own+inherited,
// each tagged with which class actually declared it) -- this is exactly
// how the engine's own save/load and editor tooling introspects objects
// at runtime, so it is authoritative, not a heuristic.
//
// Layout below was derived by decompiling igCore.dll's own exported,
// demangled accessor methods in Ghidra (igArkCore::getObjectMeta(int),
// igMetaObject::getIndexedMetaField(int)/getMetaFieldCount(),
// igMetaField::getOffset()/getParentMetaObject(), igMetaObject::isOfType())
// -- every numeric offset below is copied directly from one of those
// decompiles, not guessed, and cross-checked against the reference
// project github.com/NefariousTechSupport/AlchemyMetadataDumper (a
// runtime metadata dumper for the Skylanders titles, a later build of the
// same "Ark"/Alchemy reflection system) for the overall shape -- the
// concrete byte offsets differ from that project's own (expected: a
// different, earlier engine build), but the mechanism (a global class
// registry, igMetaObject::_parent, an igMetaField list per class) is
// identical, and every offset used here is independently PC-verified
// against Mad2's own igCore.dll rather than borrowed from that project.
//
// Name resolution (class name / field name) is the one offset NOT found
// via an exported accessor -- igCore.dll exports no separate
// igMetaObject::getName()/igMetaField::getName() (both classes inherit
// igNamedObject, which has no separately-compiled methods of its own,
// consistent with docs/SCRIPT_FORMAT.md's existing established finding
// that igNamedObject::getName() is *(this+8) as a plain char* -- see
// "ScriptObject inherits a real, clean, PC-native igNamedObject::getName()"
// in that doc). Reused verbatim here for both igMetaObject and igMetaField.
#include <windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../mad2settings/include/mad2settings_api.h"
#include "../../mad2sharedlog/mad2sharedlog.h"

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Mad2Log_Write("[mad2metadumper]", fmt, args);
    va_end(args);
}

// ---------------------------------------------------------------------
// igArkCore / igMetaObject / igMetaField layout, PC-verified against
// igCore.dll (see file header). All classes below are read via raw
// pointer arithmetic, not real C++ types, since the real vtable/RTTI
// layout of these engine classes isn't reconstructed here -- only the
// specific byte offsets actually needed.
// ---------------------------------------------------------------------

// igArkCore
static const int kArkCore_ClassListContainer = 0x18;

// A generic "__internalNonRefCountedObjectList"-shaped container, reused
// for both igArkCore's own class list (at +0x18) and each igMetaObject's
// field list (at +0x10): data array pointer at +0xC, element count at
// +0x10. (Confirmed twice independently: igArkCore::getObjectMeta(int)
// decompiles to `*(*(this+0x18)+0xC) + index*4`, and
// igMetaObject::getMetaFieldCount() decompiles to `*(*(this+0x10)+0x10)`
// -- same container shape, same +0xC data offset, both in real PC
// disassembly.)
static const int kContainer_Data = 0x0C;
static const int kContainer_Count = 0x10;

// igMetaObject
// PC-confirmed live via ScanForNameOffset (see below): class[0..4] read
// back exactly "igObject"/"igMetaObject"/"igMetaField"/"igRefMetaField"/
// "igObjectRefMetaField" at this offset -- not the igNamedObject +0x08
// guess this repo established elsewhere for a different class hierarchy.
static const int kMetaObject_Name = 0x0C;
static const int kMetaObject_FieldsContainer = 0x10;     // container, see above
static const int kMetaObject_Parent = 0x24;              // igMetaObject* (confirmed via isOfType's walk loop)

// igMetaField
// Same offset as kMetaObject_Name -- also matches the pre-SkyTT igMetaField
// layout in github.com/NefariousTechSupport/AlchemyMetadataDumper's own
// igMetaField.hpp ("char* _fieldName; //0x0C"), independent corroboration.
static const int kMetaField_Name = 0x0C;
static const int kMetaField_Offset = 0x18;                // int16 (confirmed via igMetaField::getOffset())
static const int kMetaField_ParentMetaObjectIndex = 0x1A;  // int16 (confirmed via igMetaField::getParentMetaObject())

static bool IsReadablePtr(const void* p, size_t len) {
    if (!p) return false;
    uintptr_t addr = reinterpret_cast<uintptr_t>(p);
    if (addr < 0x10000 || addr > 0x7FFFFFFF) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD)) return false;
    uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return addr + len <= regionEnd;
}

template <typename T>
static bool ReadAt(const void* base, int offset, T* out) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(base) + offset;
    if (!IsReadablePtr(p, sizeof(T))) return false;
    memcpy(out, p, sizeof(T));
    return true;
}

// Reads a char* field and sanity-checks it looks like a real short
// identifier string (printable ASCII, reasonable length) before trusting
// it -- guards against the +8 name-offset guess being wrong for some
// class without crashing or emitting garbage silently.
static bool ReadNamePlausible(const void* obj, int nameFieldOffset, char* out, size_t outSize) {
    char* namePtr = nullptr;
    if (!ReadAt(obj, nameFieldOffset, &namePtr)) return false;
    if (!IsReadablePtr(namePtr, 1)) return false;
    size_t n = 0;
    while (n + 1 < outSize) {
        char c;
        if (!IsReadablePtr(namePtr + n, 1)) return false;
        c = namePtr[n];
        if (c == '\0') break;
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) return false;
        out[n] = c;
        ++n;
        if (n > 200) return false;  // real class/field identifiers are short
    }
    out[n] = '\0';
    return n > 0;
}

static FILE* OpenDumpFile() {
    char exeDir[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exeDir, MAX_PATH);
    char* slash = strrchr(exeDir, '\\');
    if (slash) {
        *(slash + 1) = '\0';
    } else {
        exeDir[0] = '\0';
    }
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%slogs\\mad2metadump.txt", exeDir);
    CreateDirectoryA((std::string(exeDir) + "logs").c_str(), nullptr);
    return fopen(path, "w");
}

// One-shot diagnostic: for the first few resolved igMetaObject pointers,
// scans every 4-byte-aligned offset in [0, scanRange) for a value that
// looks like a valid char* to a short printable identifier string, and
// logs every hit (offset + string). Used to empirically pin down
// kMetaObject_Name/kMetaField_Name if the +8 guess (borrowed from the
// already-established igNamedObject::getName() offset for a *different*
// class elsewhere in this codebase) turns out wrong for this class --
// also hex-dumps the raw bytes so a match that isn't actually the class's
// *own* name (e.g. finds a field's name string instead) can be told apart
// by a human reading the log.
static void ScanForNameOffset(const char* label, void* obj, int scanRange) {
    if (!IsReadablePtr(obj, scanRange)) {
        Log("  ScanForNameOffset(%s): object 0x%p not readable for %d bytes\n", label, obj, scanRange);
        return;
    }
    char hex[256];
    int hp = 0;
    for (int off = 0; off < scanRange && hp < (int)sizeof(hex) - 4; off += 4) {
        uint8_t bytes[4];
        memcpy(bytes, reinterpret_cast<uint8_t*>(obj) + off, 4);
        hp += snprintf(hex + hp, sizeof(hex) - hp, "%02X%02X%02X%02X ", bytes[0], bytes[1], bytes[2], bytes[3]);
    }
    Log("  ScanForNameOffset(%s): obj=0x%p raw[0..0x%X)= %s\n", label, obj, scanRange, hex);
    for (int off = 0; off < scanRange; off += 4) {
        void* candidate = nullptr;
        memcpy(&candidate, reinterpret_cast<uint8_t*>(obj) + off, 4);
        char name[256];
        if (ReadNamePlausible(&candidate, 0, name, sizeof(name))) {
            Log("  ScanForNameOffset(%s): +0x%02X -> \"%s\"\n", label, off, name);
        }
    }
}

typedef void* (__cdecl* ArkCoreFunction_t)();

static void DumpAllMetadata() {
    HMODULE igCore = GetModuleHandleA("igCore.dll");
    if (!igCore) {
        Log("igCore.dll not loaded -- cannot dump reflection metadata\n");
        return;
    }
    // ?ArkCore_function@Core@Gap@@YAPAVigArkCore@12@XZ
    auto arkCoreFn = reinterpret_cast<ArkCoreFunction_t>(reinterpret_cast<void*>(
        GetProcAddress(igCore, "?ArkCore_function@Core@Gap@@YAPAVigArkCore@12@XZ")));
    if (!arkCoreFn) {
        Log("ArkCore_function export not found in igCore.dll\n");
        return;
    }
    void* arkCore = arkCoreFn();
    if (!IsReadablePtr(arkCore, 0x28)) {
        Log("ArkCore_function() returned an unreadable/null pointer (0x%p)\n", arkCore);
        return;
    }

    void* classListContainer = nullptr;
    if (!ReadAt(arkCore, kArkCore_ClassListContainer, &classListContainer) ||
        !IsReadablePtr(classListContainer, kContainer_Count + 4)) {
        Log("ArkCore class-list container unreadable\n");
        return;
    }
    void** classData = nullptr;
    int32_t classCount = 0;
    if (!ReadAt(classListContainer, kContainer_Data, &classData) ||
        !ReadAt(classListContainer, kContainer_Count, &classCount)) {
        Log("ArkCore class-list container fields unreadable\n");
        return;
    }
    if (classCount < 0 || classCount > 20000) {
        Log("ArkCore class count looks implausible (%d) -- aborting, offsets may be wrong for this build\n",
            classCount);
        return;
    }

    FILE* f = OpenDumpFile();
    if (!f) {
        Log("Could not open logs\\mad2metadump.txt for writing\n");
        return;
    }

    Log("Dumping reflection metadata: %d registered classes -> logs\\mad2metadump.txt ...\n", classCount);

    int classesWritten = 0, fieldsWritten = 0, classesSkipped = 0;
    int diagnosedSoFar = 0;
    for (int32_t i = 0; i < classCount; ++i) {
        void* classMeta = nullptr;
        if (!ReadAt(classData, i * 4, &classMeta) || !IsReadablePtr(classMeta, 0x40)) {
            ++classesSkipped;
            continue;
        }
        if (diagnosedSoFar < 5) {
            char label[32];
            snprintf(label, sizeof(label), "class[%d]", i);
            ScanForNameOffset(label, classMeta, 0x40);
            ++diagnosedSoFar;
        }
        char className[256];
        if (!ReadNamePlausible(classMeta, kMetaObject_Name, className, sizeof(className))) {
            ++classesSkipped;
            continue;
        }

        void* parentMeta = nullptr;
        char parentName[256] = "-";
        if (ReadAt(classMeta, kMetaObject_Parent, &parentMeta) && IsReadablePtr(parentMeta, kMetaObject_Name + 4)) {
            ReadNamePlausible(parentMeta, kMetaObject_Name, parentName, sizeof(parentName));
        }

        fprintf(f, "CLASS\t%d\t%s\t%s\n", i, className, parentName[0] ? parentName : "-");
        ++classesWritten;

        void* fieldsContainer = nullptr;
        if (!ReadAt(classMeta, kMetaObject_FieldsContainer, &fieldsContainer) ||
            !IsReadablePtr(fieldsContainer, kContainer_Count + 4)) {
            continue;
        }
        void** fieldData = nullptr;
        int32_t fieldCount = 0;
        if (!ReadAt(fieldsContainer, kContainer_Data, &fieldData) ||
            !ReadAt(fieldsContainer, kContainer_Count, &fieldCount)) {
            continue;
        }
        if (fieldCount < 0 || fieldCount > 2000) continue;

        for (int32_t j = 0; j < fieldCount; ++j) {
            void* field = nullptr;
            if (!ReadAt(fieldData, j * 4, &field) || !IsReadablePtr(field, kMetaField_ParentMetaObjectIndex + 2)) {
                continue;
            }
            char fieldName[256];
            if (!ReadNamePlausible(field, kMetaField_Name, fieldName, sizeof(fieldName))) {
                continue;
            }
            int16_t offset = 0;
            int16_t ownerIdx = -1;
            ReadAt(field, kMetaField_Offset, &offset);
            ReadAt(field, kMetaField_ParentMetaObjectIndex, &ownerIdx);

            char ownerName[256] = "?";
            if (ownerIdx >= 0 && ownerIdx < classCount) {
                void* ownerMeta = nullptr;
                if (ReadAt(classData, ownerIdx * 4, &ownerMeta) && IsReadablePtr(ownerMeta, kMetaObject_Name + 4)) {
                    ReadNamePlausible(ownerMeta, kMetaObject_Name, ownerName, sizeof(ownerName));
                }
            }

            fprintf(f, "FIELD\t%s\t%d\t%s\t%d\t%s\n", className, j, fieldName, offset, ownerName);
            ++fieldsWritten;
        }
    }

    fclose(f);
    Log("Dump complete: %d classes written, %d classes skipped (unreadable/no name), %d fields written\n",
        classesWritten, classesSkipped, fieldsWritten);
}

static void WINAPI DumpAllMetadataAction(void*) { DumpAllMetadata(); }

static void RegisterSettings() {
    const auto& settings = Mad2Settings_Resolve();
    if (!settings.Register) {
        Log("[Dependency] mad2settings.dll not available -- this hotkey won't appear in the debug menu\n");
        return;
    }
    Mad2SettingDesc desc{};
    desc.category = "MetaDumper (diagnostic)";
    desc.label = "Dump All Metadata";
    desc.type = MAD2SETTING_ACTION;
    desc.onChanged = DumpAllMetadataAction;
    settings.Register(&desc);
    Log("Registered settings with mad2settings.dll\n");
}

static DWORD WINAPI PollThreadFunc(LPVOID) {
    Log("Ready -- F4 dumps the full live reflection metadata (all registered classes, "
        "parent chain, full own+inherited field list with offsets) to logs\\mad2metadump.txt\n");
    bool wasF4Pressed = false;
    bool settingsRegistered = false;
    for (;;) {
        Sleep(50);

        if (!settingsRegistered && Mad2Settings_Resolve().Register) {
            RegisterSettings();
            settingsRegistered = true;
        }
        bool isF4Pressed = (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
        if (isF4Pressed && !wasF4Pressed) DumpAllMetadata();
        wasF4Pressed = isF4Pressed;
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(hinstDLL);
            HANDLE h = CreateThread(nullptr, 0, PollThreadFunc, nullptr, 0, nullptr);
            if (h) {
                CloseHandle(h);
            } else {
                Log("CreateThread for poll thread failed: %lu\n", GetLastError());
            }
            break;
        }
        case DLL_PROCESS_DETACH:
            break;
        default:
            break;
    }
    return TRUE;
}
