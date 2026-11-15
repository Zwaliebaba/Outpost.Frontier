// Author: Giperion.

#include "NeuronRuntime.h"
#include "neuroncomponents.h"
#include "particlefx.h"
#include "soundmanager.h"
#include "soundstreamer.h"
#include "system.h"

#include <io.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unknwn.h>

namespace
{
    // (the routine) -- cdecl debug print: a NULL format does nothing;
    // otherwise vsprintf into a 0x100-byte stack buffer (unbounded, as baseline) and
    // OutputDebugStringA. Only caller: NeuronRuntime::Startup's NEURONCOMPONENTS/INI failure path.
    void DebugPrint(const char* Format, ...)
    {
        if (Format == nullptr)
            return;
        char Buffer[0x100];
        va_list Args;
        va_start(Args, Format);
        vsprintf(Buffer, Format, Args);
        va_end(Args);
        OutputDebugStringA(Buffer);
    }

    // SystemContainer uses the extended descriptor; other factories consume its prefix.
    struct FNamedAggDesc
    {
        unsigned long Size;
        const char* Name;
        void* Outer;
        void* InnerOut;
        const char* Extra;
    };

    // Kept across calls for Shutdown's trailing-reference checks.
    unsigned long GLastReleaseCount = 0;
}

// NeuronRuntime's own module handle, stored by DllMain (module storage) and
// passed to GetModuleFileNameA by Startup (module storage).
static HMODULE GModule = nullptr;

// / 065ca0b4 -- both start NULL; the host installs its handlers before
// NeuronRuntime::Startup (OutpostFrontier.exe WinMain module storage / 005b30f4).
FatalErrorFnType FatalErrorFn = nullptr;
CriticalWarningFnType CriticalWarningFn = nullptr;

ICOManager* NeuronRuntime::NeuronComponents = nullptr;
IEngine* NeuronRuntime::Engine = nullptr;
IEngine2* NeuronRuntime::Engine2 = nullptr;
IRenderer2* NeuronRuntime::Renderer = nullptr;
IAnimation2* NeuronRuntime::Anim = nullptr;
IHardpoint* NeuronRuntime::Hardpoint = nullptr;
IDeformable* NeuronRuntime::Deform = nullptr;
IChannel* NeuronRuntime::Channel = nullptr;
IRenderPipeline* NeuronRuntime::Pipeline = nullptr;
IRPDraw* NeuronRuntime::RPDraw = nullptr;
IRPIndexBuffer* NeuronRuntime::RPIndexBuffer = nullptr;
IRPVertexBuffer* NeuronRuntime::RPVertexBuffer = nullptr;
IFileSystem* NeuronRuntime::Data = nullptr;
ISoundManager* NeuronRuntime::Sound = nullptr;
IStreamer2* NeuronRuntime::Streamer = nullptr;
ITextureLibrary2* NeuronRuntime::TextureLib = nullptr;
IMaterialLibrary* NeuronRuntime::MaterialLib = nullptr;
IMaterialBatcher2* NeuronRuntime::MaterialBatcher = nullptr;
IVertexBufferManager* NeuronRuntime::VertexBuffer = nullptr;
IParticleFx* NeuronRuntime::ParticleFx = nullptr;
IFxRuntime* NeuronRuntime::FxRuntime = nullptr;
IFxEffectLibrary* NeuronRuntime::FxEffectLibrary = nullptr;
IMatAnimLibrary* NeuronRuntime::MatAnimLibrary = nullptr;
IVMeshLibrary* NeuronRuntime::VMeshLibrary = nullptr;

namespace
{
    // One `if (p) { LastRelease = p->Release(); p = 0; }` block of NeuronRuntime's teardown
    // (module storage.., e.g. Channel module storage-1705). The count is stored even when 0.
    template <typename T>
    void ReleaseGlobal(T*& Global)
    {
        if (Global != nullptr)
        {
            GLastReleaseCount = ((INeuronComponent*)Global)->Release();
            Global = nullptr;
        }
    }
}

// NeuronRuntime::Startup(HWND, const char*).
bool __cdecl NeuronRuntime::Startup(HWND Hwnd, const char* IniPath)
{
    NeuronComponents = NEURONCOMPONENTS_Acquire();
    if (NeuronComponents == nullptr)
    {
        DebugPrint("ERROR: creating NEURONCOMPONENTS with INI file\n"); 
        return false;
    }

    // -1218: an explicit path goes straight to SetINIConfig; else "NEURONCOMPONENTS.INI" if
    // it is readable from the cwd; else NEURONCOMPONENTS.INI next to NeuronRuntime.dll itself (GModule).
    GENRESULT IniResult;
    if (IniPath != nullptr)
    {
        IniResult = NeuronComponents->SetINIConfig(IniPath, 0);
    }
    else if (_access("NEURONCOMPONENTS.INI", 4) == 0)
    {
        IniResult = NeuronComponents->SetINIConfig("NEURONCOMPONENTS.INI", 0);
    }
    else
    {
        char ModulePath[0x104];
        char Drive[0x44];
        char Dir[0x100];
        char IniFile[0x100];
        GetModuleFileNameA(GModule, ModulePath, 0x104);
        _splitpath(ModulePath, Drive, Dir, nullptr, nullptr);
        _makepath(IniFile, Drive, Dir, "NEURONCOMPONENTS.INI", nullptr);
        IniResult = NeuronComponents->SetINIConfig(IniFile, 0);
    }
    if (IniResult != GR_OK)
    {
        DebugPrint("ERROR: creating NEURONCOMPONENTS with INI file\n");
        return false;
    }

    // -12ae. The SystemContainer instance is released only once ISystemContainer
    // was obtained from it; baseline leaks it on every earlier failure.
    bool bResult = true; // byte [ESP+0x17] = 1
    ISystemContainer* Container = nullptr;
    {
        FNamedAggDesc Desc = {0x14, "SystemContainer", nullptr, nullptr, nullptr};
        INeuronComponent* System = nullptr;
        if (NeuronComponents->CreateInstance((AGGDESC*)(&Desc), (void**)(&System)) ==
            GR_OK)
        {
            IAggregateComponent* Aggregate = nullptr;
            if (System->QueryInterface(Iids::AggregateComponent, (void**)(&Aggregate)) ==
                GR_OK)
            {
                const GENRESULT InitResult = Aggregate->Initialize();
                Aggregate->Release();
                if (InitResult == GR_OK &&
                    System->QueryInterface(Iids::SystemContainer, (void**)(&Container)) ==
                        GR_OK)
                {
                    System->Release();
                }
            }
        }
    }

    if (Container != nullptr)
    {
        // -13a8: every result lands directly in the exported global.
        Container->LoadSystemComponents();
        Container->QueryInterface(Iids::SoundManager, (void**)(&Sound));
        Container->QueryInterface(Iids::Streamer2, (void**)(&Streamer));
        Container->QueryInterface(Iids::RenderPipeline8B, (void**)(&Pipeline));
        if (Pipeline != nullptr)
        {
            Pipeline->QueryInterface(Iids::RPDraw, (void**)(&RPDraw));
            Pipeline->QueryInterface(Iids::RPIndexBuffer, (void**)(&RPIndexBuffer));
            Pipeline->QueryInterface(Iids::RPVertexBuffer, (void**)(&RPVertexBuffer));
        }
        Container->QueryInterface(Iids::TextureLibrary2, (void**)(&TextureLib));
        Container->QueryInterface(Iids::MaterialLibrary, (void**)(&MaterialLib));
        Container->QueryInterface(Iids::MaterialBatcher2, (void**)(&MaterialBatcher));
        Container->QueryInterface(Iids::VertexBufferManager, (void**)(&VertexBuffer));
        Container->QueryInterface(Iids::ParticleFx, (void**)(&ParticleFx));
        Container->QueryInterface(Iids::FxRuntime, (void**)(&FxRuntime));
        Container->QueryInterface(Iids::MatAnimLibrary, (void**)(&MatAnimLibrary));
        Container->QueryInterface(Iids::VMeshLibrary, (void**)(&VMeshLibrary));

        // -13f5: a sound device that fails to start is shut down and dropped.
        bool bAudioFailed = false;
        if (Sound != nullptr && Sound->StartupDevice(Hwnd, 0x113) != GR_OK)
        {
            bAudioFailed = true;
            Sound->ShutdownDevice();
            ReleaseGlobal(Sound);
        }

        // -1489: the streamer gets the sound manager's DirectSound; a streamer
        // whose InitializeDevice returns 0 is dropped. The DirectSound reference is released
        // last, and its count too lands in GLastReleaseCount.
        if (Streamer != nullptr)
        {
            IDirectSound* DirectSound = nullptr;
            if (Sound != nullptr)
                Sound->GetDirectSound(&DirectSound);

            FStreamerInitDesc Desc = {};
            Desc.Size = 0x18;
            Desc.DirectSound = DirectSound;
            Desc.Window = Hwnd;
            if (Streamer->InitializeDevice(&Desc) == 0)
            {
                bAudioFailed = true;
                ReleaseGlobal(Streamer);
            }
            if (DirectSound != nullptr)
                GLastReleaseCount = DirectSound->Release();
        }

        // -14a0: the host's verdict becomes Startup's return value.
        if (bAudioFailed)
            bResult = CriticalWarningFn(0xd3, nullptr);
    }

    // -1553: CreateInstance writes NeuronRuntime::Engine itself; on success the engine
    // loads its components from the container (NULL when the container failed) and is queried.
    {
        FNamedAggDesc Desc = {8, "Engine", nullptr, nullptr, nullptr};
        if (NeuronComponents->CreateInstance((AGGDESC*)(&Desc), (void**)(&Engine)) ==
            GR_OK)
        {
            Engine->LoadEngineComponents(Container);
            Engine->QueryInterface(Iids::Animation2, (void**)(&Anim));
            Engine->QueryInterface(Iids::Renderer2, (void**)(&Renderer));
            Engine->QueryInterface(Iids::Hardpoint, (void**)(&Hardpoint));
            Engine->QueryInterface(Iids::Deformable, (void**)(&Deform));
            Engine->QueryInterface(Iids::Channel, (void**)(&Channel));
            Engine->QueryInterface(Iids::Engine2, (void**)(&Engine2));
        }
    }

    // -1590: no ParticleFx check -- a NULL ParticleFx is passed through.
    {
        FNamedAggDesc Desc = {8, "FxEffectLibrary", nullptr, nullptr, nullptr};
        if (NeuronComponents->CreateInstance((AGGDESC*)(&Desc),
                                  (void**)(&FxEffectLibrary)) == GR_OK)
        {
            FxEffectLibrary->BindParticleFx(ParticleFx, ParticleFx);
        }
    }

    if (Container != nullptr)
        Container->Release();

    return bResult;
}

// NeuronRuntime::OpenData -- 0x34-byte "FileSystem"/"DOS" descriptor, result
// written straight into NeuronRuntime::Data; neither NeuronComponents nor the result is checked.
void __cdecl NeuronRuntime::OpenData(const char* DataPath)
{
    FFileSystemAggDesc Desc = {};
    Desc.Size = 0x34;
    Desc.ComponentName = "FileSystem";
    Desc.BackendName = "DOS";
    Desc.DataPath = DataPath;
    Desc.Field10 = 0x80000000; // GENERIC_READ
    Desc.Field14 = 1;          // FILE_SHARE_READ
    Desc.Field1C = 3;          // OPEN_EXISTING
    Desc.Field20 = 0x08000080;
    Desc.Field30 = 0xFFFFFFFF;
    NeuronComponents->CreateInstance((AGGDESC*)(&Desc), (void**)(&Data));
}

// NeuronRuntime::CloseData.
void __cdecl NeuronRuntime::CloseData()
{
    ReleaseGlobal(Data);
}

// NeuronRuntime::Shutdown. NeuronRuntime::Data is not touched (CloseData is the host's
// job); MatAnimLibrary and VMeshLibrary are neither released nor cleared.
void __cdecl NeuronRuntime::Shutdown(HWND /*Hwnd*/)
{
    OutputDebugStringA("NeuronRuntime::Shutdown () ENTER\n");

    if (FxEffectLibrary != nullptr)
        FxEffectLibrary->ClearEffects(); // slot +0x24
    IRenderPipeline* const SavedPipeline = Pipeline; // EBX, module storage
    ReleaseGlobal(FxEffectLibrary);
    ReleaseGlobal(Channel);
    ReleaseGlobal(Anim);
    ReleaseGlobal(Renderer);
    ReleaseGlobal(Hardpoint);
    ReleaseGlobal(Deform);
    ReleaseGlobal(Engine2);
    ReleaseGlobal(Engine);

    if (GLastReleaseCount != 0)
    {
        FDUMP(0x100002, "%s(%d) : WARNING:General:%s", "NeuronRuntime\\NeuronRuntime.cpp",
              0x14e, "DA ENGINE: trailing references");
    }

    ReleaseGlobal(TextureLib);
    ReleaseGlobal(MaterialBatcher);
    ReleaseGlobal(MaterialLib);

    if (ParticleFx != nullptr)
    {
        ParticleFx->ClearNodeLibrary();    // slot +0x24
        ParticleFx->ReleaseVertexBuffer(); // slot +0x38
        ReleaseGlobal(ParticleFx);
    }

    ReleaseGlobal(FxRuntime);

    if (VertexBuffer != nullptr)
    {
        VertexBuffer->VbMgrReleaseAll(); // slot +0x10
        ReleaseGlobal(VertexBuffer);
    }

    ReleaseGlobal(Streamer);

    if (Sound != nullptr)
    {
        Sound->ShutdownDevice(); // slot +0xa8
        ReleaseGlobal(Sound);
    }

    ReleaseGlobal(RPDraw);
    ReleaseGlobal(RPIndexBuffer);
    ReleaseGlobal(RPVertexBuffer);

    if (Pipeline != nullptr)
    {
        Pipeline->ShutdownPipeline(); // slot +0x10
        ReleaseGlobal(Pipeline);
    }

    // -1991: drain the saved pipeline pointer until a Release returns 0.
    while (GLastReleaseCount != 0)
    {
        FDUMP(0x100002, "%s(%d) : WARNING:General:%s", "NeuronRuntime\\NeuronRuntime.cpp",
              0x178, "******* DA SYSTEM: trailing references *******");
        GLastReleaseCount = SavedPipeline->Release();
    }

    if (NeuronComponents != nullptr)
    {
        NeuronComponents->Shutdown(); // slot +0x24
        NeuronComponents = nullptr;
    }

    OutputDebugStringA("NeuronRuntime::Shutdown () EXIT\n");
}

// DllMain: on DLL_PROCESS_ATTACH (reason 1) stash the module handle in
// and opt out of thread notifications; every reason returns 1.
BOOL WINAPI DllMain(HMODULE Module, DWORD Reason, LPVOID)
{
    if (Reason == DLL_PROCESS_ATTACH)
    {
        GModule = Module;
        DisableThreadLibraryCalls(Module);
    }
    return TRUE;
}
