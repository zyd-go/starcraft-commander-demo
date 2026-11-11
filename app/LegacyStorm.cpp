// Legacy BWAPI asks Storm for its install path. Resolve that one setting
// inside the Skynet match directory, then forward every other registry read.
#include <windows.h>
#include <cstring>

extern "C" __declspec(dllimport) void StormAnchor();
static FARPROC exports[916]{};

#define ENTRY(ordinal) extern "C" __declspec(naked) void Export##ordinal() { \
    __asm { jmp dword ptr [exports + ordinal * 4] } \
}
#include "LegacyStormEntries.inc"
#undef ENTRY

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(module);
    HMODULE library = GetModuleHandleW(L"StormBase.dll");
    if (!library) return FALSE;
    exports[422] = reinterpret_cast<FARPROC>(&StormAnchor);
    #define ENTRY(ordinal) exports[ordinal] = GetProcAddress(library, MAKEINTRESOURCEA(ordinal)); \
                           if (!exports[ordinal]) return FALSE;
    #include "LegacyStormEntries.inc"
    #undef ENTRY
    return TRUE;
}

extern "C" BOOL WINAPI LocalInstallPath(const char* key, const char* value,
                                        BYTE flags, char* output, size_t capacity) {
    if (key && value && _stricmp(key, "starcraft") == 0 && _stricmp(value, "InstallPath") == 0) {
        DWORD length = GetCurrentDirectoryA(static_cast<DWORD>(capacity), output);
        return length > 0 && length < capacity;
    }
    using LoadString = BOOL (WINAPI *)(const char*, const char*, BYTE, char*, size_t);
    return reinterpret_cast<LoadString>(exports[422])(key, value, flags, output, capacity);
}
