// Author: Giperion.

#include "lights_camera.h"

#include "NeuronRuntime.h"
#include "particlefx.h"
#include "neuroncomponents.h"
#include "game_options.h"
#include "misc_util.h"
#include "vis_effect_arch.h"

namespace
{
    char const* const GLightsCameraFile = "Client\\supt\\LightsCamera.cpp";
}

// .
// NeuronRuntime::ParticleFx null -> return 0 (no field written).
// +0x4 = Vis; the routine on it (its answer is not read).
// node = 0; NeuronComponents slot 3 CreateInstance({8, "FxNode"}, &node).
// FxEffectLibrary slot 6 (node, Vis+0x4 effect_crc, this).
// ParticleFx slot 25 GetRootNode(&root), slot 20 AttachNode(root, node),
//                           root slot 2 Release; result 1.
// either failure: FDUMP(0x100001, ..., 0x215, *Vis (its nickname)),
// (Vis), +0x4 = 0.
// node non-null -> node slot 2 Release.
bool FLightsCameraParticles::Create(FVisEffectArch* InVis)
{
    if (NeuronRuntime::ParticleFx == nullptr)
        return false;

    bool bCreated = false;
    Vis = InVis;
    VisEffectArchAddRef(Vis);

    void* Node = nullptr;
    AGGDESC Desc = {8, "FxNode"};
    bool bFailed = true;
    if (NeuronRuntime::NeuronComponents->CreateInstance(&Desc, &Node) == GR_OK)
    {
        if (NeuronRuntime::FxEffectLibrary->CreateEffect(Node, reinterpret_cast<void*>(Vis->EffectCrc),
                                                 &Effect) == GR_OK)
        {
            void* Root = nullptr;
            NeuronRuntime::ParticleFx->GetRootNode(&Root);
            NeuronRuntime::ParticleFx->AttachNode(Root, Node);
            static_cast<INeuronComponent*>(Root)->Release();
            bCreated = true;
            bFailed = false;
        }
    }
    if (bFailed)
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: ParticleEffect could not create ParticleFx effect (%s)\n",
              GLightsCameraFile, 0x215, Vis->Nickname);
        VisEffectArchRelease(Vis);
        Vis = nullptr;
    }
    if (Node != nullptr)
        static_cast<INeuronComponent*>(Node)->Release();
    return bCreated;
}

// . 0x005574a9..0x005574c7: slot 13 (+0x34) with 7, slot 2 Release, +0 = 0,
// on +0x4 (no null test), +0x4 = 0.
void FLightsCameraParticles::Destroy()
{
    if (Effect == nullptr)
        return;
    Effect->Slot13(reinterpret_cast<void*>(7));
    static_cast<INeuronComponent*>(Effect)->Release();
    Effect = nullptr;
    VisEffectArchRelease(Vis);
    Vis = nullptr;
}

// . 0x005574d6 +0 null -> out; the routine(0x25) / (0x26) zero -> out; slot 4
// (+0x10) (0, &Scratch[0]); slot 15 (+0x3c) (&Scratch[1]) -- the center at Scratch[1..3] and
// the radius at Scratch[4] -- negative -> out; the routine(Camera, &center, radius) false ->
// out; slot 9 (+0x24) (Camera). The 0x14-byte stack block is shared by both slots, as baseline's
// local_14 is.
void FLightsCameraParticles::Draw(ICamera* Camera)
{
    if (Effect == nullptr)
        return;
    if (GetGameOption(kGameOptEffectsGroup) == 0)
        return;
    if (GetGameOption(kGameOptParticles) == 0)
        return;

    float Scratch[5];
    Effect->Slot4(nullptr, &Scratch[0]);
    if (Effect->Slot15(&Scratch[1]) < 0)
        return;
    if (!MiscUtilSphereInView(Camera, &Scratch[1], Scratch[4]))
        return;
    Effect->Slot9(Camera);
}
