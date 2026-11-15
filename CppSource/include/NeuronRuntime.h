// Author: Giperion.

#pragma once

#include "neuroncomponents.h"

// Keep IEngine2 a distinct incomplete type so NeuronRuntime::Engine2 mangles as PAUIEngine2
// (sceneruntime.h would otherwise alias it to IEngine).
struct IEngine2;
#define OUTPOST_FRONTIER_IENGINE2_DEFINED
#include "engine/sceneruntime.h"
#include "render/IRenderPipeline.h"

// NEURONRUNTIME_API, FFileSystemAggDesc, FatalErrorFn / CriticalWarningFn and Startup / OpenData /
// CloseData / Shutdown (the part MFC translation units can include).
#include "neuronruntime_core.h"

struct IRPDraw;
struct IRPIndexBuffer;
struct IRPVertexBuffer;
struct IRenderer2;
struct IFileSystem;
struct ISoundManager;
struct IStreamer2;
struct ITextureLibrary2;
struct IMaterialLibrary;
struct IMaterialBatcher2;
struct IVertexBufferManager;
struct IParticleFx;
struct IFxRuntime;
struct IFxEffectLibrary;
struct IMatAnimLibrary;
struct IVMeshLibrary;

namespace NeuronRuntime
{
    // The DirectPlay8 network classes (CDPClient, CDPServer, CDPMsgList, CGunWrapper, ...) are
    // published in neuronruntime_net.h; OutpostFrontier.exe calls CDPServer::CrashCleanup() and
    // CDPClient::SetSourcePort() directly, as baseline does.

    NEURONRUNTIME_API extern ICOManager* NeuronComponents;
    NEURONRUNTIME_API extern IEngine* Engine;
    NEURONRUNTIME_API extern IEngine2* Engine2;
    NEURONRUNTIME_API extern IRenderer2* Renderer;
    NEURONRUNTIME_API extern IAnimation2* Anim;
    NEURONRUNTIME_API extern IHardpoint* Hardpoint;
    NEURONRUNTIME_API extern IDeformable* Deform;
    NEURONRUNTIME_API extern IChannel* Channel;
    NEURONRUNTIME_API extern IRenderPipeline* Pipeline;
    NEURONRUNTIME_API extern IRPDraw* RPDraw;
    NEURONRUNTIME_API extern IRPIndexBuffer* RPIndexBuffer;
    NEURONRUNTIME_API extern IRPVertexBuffer* RPVertexBuffer;
    NEURONRUNTIME_API extern IFileSystem* Data;
    NEURONRUNTIME_API extern ISoundManager* Sound;
    NEURONRUNTIME_API extern IStreamer2* Streamer;
    NEURONRUNTIME_API extern ITextureLibrary2* TextureLib;
    NEURONRUNTIME_API extern IMaterialLibrary* MaterialLib;
    NEURONRUNTIME_API extern IMaterialBatcher2* MaterialBatcher;
    NEURONRUNTIME_API extern IVertexBufferManager* VertexBuffer;
    NEURONRUNTIME_API extern IParticleFx* ParticleFx;
    NEURONRUNTIME_API extern IFxRuntime* FxRuntime;
    NEURONRUNTIME_API extern IFxEffectLibrary* FxEffectLibrary;
    NEURONRUNTIME_API extern IMatAnimLibrary* MatAnimLibrary;
    NEURONRUNTIME_API extern IVMeshLibrary* VMeshLibrary;
}
