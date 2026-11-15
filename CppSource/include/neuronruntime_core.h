// Author: Giperion.

// neuronruntime_core.h -- the part of NeuronRuntime.dll's public ABI that needs neither the engine nor the
// render-pipeline headers: the NEURONRUNTIME_API macro, the FileSystem AGGDESC, the two error hooks
// and Startup / OpenData / CloseData / Shutdown. NeuronRuntime.h includes it and adds the subsystem
// globals; neuronruntime_net.h needs only this part.

// Split out because render/IRenderPipeline.h pulls the DirectX 8 headers (d3d8types.h), and
// every MFC translation unit already has d3d9types.h through afxwin.h -> afxrendertarget.h:
// the two define the same enums, so OutpostServer.exe's MFC code can include this header and
// neuronruntime_net.h, but never NeuronRuntime.h.
#pragma once

#include <windows.h>

#ifdef NEURONRUNTIME_EXPORTS
#define NEURONRUNTIME_API __declspec(dllexport)
#else
#define NEURONRUNTIME_API __declspec(dllimport)
#endif

// baseline NeuronRuntime::OpenData AGGDESC size 0x34 @ 0x1610 — ReadFile DOS FileSystem factory.
struct FFileSystemAggDesc
{
    unsigned long Size;
    const char* ComponentName;
    const char* BackendName;
    const char* DataPath;
    unsigned long Field10;
    unsigned long Field14;
    unsigned long Field18;
    unsigned long Field1C;
    unsigned long Field20;
    unsigned long Field24;
    unsigned long Field28;
    unsigned long Field2C;
    unsigned long Field30;
};

using FatalErrorFnType = void(__cdecl*)(unsigned int, const char*);
using CriticalWarningFnType = bool(__cdecl*)(unsigned int, const char*);

NEURONRUNTIME_API extern FatalErrorFnType FatalErrorFn;
NEURONRUNTIME_API extern CriticalWarningFnType CriticalWarningFn;

namespace NeuronRuntime
{
    // Startup phases (baseline NeuronRuntime @ 0x1160): NEURONCOMPONENTS INI → SystemContainer QIs →
    // Sound +0xA4(hwnd,0x113) / +0xAC / Streamer +0x0C desc 0x18 → Engine → FxEffectLibrary.
    NEURONRUNTIME_API bool __cdecl Startup(HWND hwnd, const char* ini_path);
    NEURONRUNTIME_API void __cdecl OpenData(const char* data_path);
    NEURONRUNTIME_API void __cdecl CloseData();
    NEURONRUNTIME_API void __cdecl Shutdown(HWND hwnd);
}
