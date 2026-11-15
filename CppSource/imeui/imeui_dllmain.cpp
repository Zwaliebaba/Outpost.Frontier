// Author: Giperion.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// . The old CRT TLS flag is zero: imeui owns no TLS.
BOOL WINAPI DllMain(HINSTANCE Instance, DWORD Reason, LPVOID Reserved)
{
    if (Reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(Instance);
    return TRUE;
}
