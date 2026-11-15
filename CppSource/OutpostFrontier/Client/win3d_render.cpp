// Author: Giperion.

#include "win3d_render.h"
#include "win3d_immediate.h"

#include "NeuronRuntime.h"
#include "globals.h"
#include "render/IRenderPipeline.h"
#include "space_state.h"
#include "common_misc.h"

#include <string.h>

#include <windows.h>

// The depth cache and the glEnable/glDisable translations live in win3d_immediate.cpp (the
// port of OpenGL.cpp). This file used to carry a second, null-guarded copy of glDisable
// (ApplyWin3DRenderLayer) plus a port-only first-frame bootstrap (PrepareWin3DRenderPass,
// ApplyWin3DViewport, ApplyWin3DSceneDepthState, BootstrapWin3DFirstFrame) that no baseline
// function corresponds to and nothing called any more. All of it is gone; the glDisable call
// sites use Win3DDisable directly.

void ClearWin3DBuffers(unsigned int Bits)
{
    // , 0x004215e0..0x0042161f. No null test: 0x004215f9 and 0x0042160d load
    // NeuronRuntime::Pipeline and call through its vtable directly.
    IRenderPipeline* const Pipe = NeuronRuntime::Pipeline;

    // / 0x004215f1: baseline tests AH, not the whole word -- `TEST AH,0x40` is bit
    // 0x4000 and `TEST AH,0x01` is bit 0x0100. Nothing outside those two bits can affect the
    // result, so the masks below are the whole of the translation.
    unsigned long ClearFlags = (Bits & kWin3DClearColour) != 0 ? 1u : 0u;
    if ((Bits & kWin3DClearDepth) != 0)
        ClearFlags |= 2u;

    // depth range is reset to the full [0, 1] on every clear, before the clear
    // itself -- baseline pushes 0.0f and 1.0f as raw dwords 0x00000000 / 0x3f800000.
    Pipe->SetDepthRange(0.0f, 1.0f);
    // the pipeline is re-read from NeuronRuntime::Pipeline for the second call.
    NeuronRuntime::Pipeline->ClearBuffers(ClearFlags, nullptr);
}

void ClearWin3DBuffersToColor(unsigned char const* Rgba)
{
    // (cdecl, one pointer to four bytes R, G, B, A): each byte FILD, FMUL double
    // 1/255 (the field), FSTP float, then the routine(R, G, B, A), tail-jumping into
    // (0x4100) -- set clear colour, clear colour+depth.
    double const Scale = 1.0 / 255.0;
    Win3DSetPipelineClearColor(static_cast<float>(Rgba[0] * Scale), static_cast<float>(Rgba[1] * Scale),
                               static_cast<float>(Rgba[2] * Scale), static_cast<float>(Rgba[3] * Scale));
    ClearWin3DBuffers(kWin3DClearColour | kWin3DClearDepth);
}

void Win3DApplyCamera(ICamera* Camera)
{
    // , 00422950..00422a65. No null checks: baseline dereferences both the camera and
    // NeuronRuntime::Pipeline unconditionally. Identity locals at [ESP+0xc..0x38]: 1.0 at +0xc/+0x1c/+0x2c
    // = elements 0/4/8, everything else 0; SetWorld gets EBX, the slot-0x40 pointer itself.
    float const ZFar = Camera->GetZFar();         // slot 0x24
    float const ZNear = Camera->GetZNear();       // slot 0x20
    float const Aspect = Camera->GetAspect();     // slot 0x28
    float const FovY = Camera->GetFovYDeg();      // slot 0x1c
    Win3DSetPerspective(FovY, Aspect, ZNear, ZFar);

    float const* const View12 = Camera->GetViewTransform(); // slot 0x40
    memcpy(GWin3DModelview, View12, sizeof(float) * 12);    // REP MOVSD 0xc -> the field

    IRenderPipeline* const Pipe = NeuronRuntime::Pipeline;
    int bViewspace = 1;                           // the out value is seeded with 1
    Pipe->GetPipelineState(7, &bViewspace);
    if (bViewspace != 0)
    {
        Pipe->SetModelview(GWin3DModelview);      // +0x7c
        return;
    }

    // identity view (+0x74), then the camera's view as the world (+0x6c).
    float Identity[12] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                          0.0f, 0.0f, 0.0f};
    Pipe->SetView(Identity);
    Pipe->SetWorld(View12);
}
