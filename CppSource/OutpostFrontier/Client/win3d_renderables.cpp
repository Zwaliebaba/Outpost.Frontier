// Author: Giperion.

#include "win3d_renderables.h"
#ifdef OUTPOST_FRONTIER_WALK_MODE
#include "base_walk.h"
#endif
#include "win3d.h"            // Win3DGetCamera (the routine, &the field)
#include "win3d_render.h"
#include "win3d_immediate.h"
#include "neuroncomponents.h"
#include "font_atlas.h"
#include "nn_montage_draw.h"
#include "screen_quad_state.h"
#include "space_camera.h"
#include "NeuronRuntime.h"
#include "render/IRenderPipeline.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

int G_DrawCallCount = 0;

static_assert(sizeof(FImmedVertex) == 0x10, "FImmedVertex is 0x10 bytes");
static_assert(offsetof(FImmedBatcher, VertBase) == 0x0c, "immed VertBase");
static_assert(offsetof(FImmedBatcher, VertCount) == 0x1c, "immed VertCount");
static_assert(offsetof(FImmedBatcher, Dirty) == 0x24, "immed Dirty");
static_assert(sizeof(FImmedBatcher) == 0x28, "FImmedBatcher is 0x28 bytes");

// Static initializers the branch / the branch: +0x00..+0x18 = 0, Dirty (+0x24) = 1,
// +0x25 = 0; +0x1c / +0x20 are not written (zero .bss).
FImmedBatcher G_ImmedBatchA = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                               nullptr, 0, 0, 1, 0, {0, 0}};
FImmedBatcher G_ImmedBatchB = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                               nullptr, 0, 0, 1, 0, {0, 0}};

namespace
{
    // — XC default 1; forces TSS COLOROP/ALPHAOP on in the routine.
    unsigned char G_UiOverlayForceTss = 1;

    IRenderPipeline* GetPipe()
    {
        if (NeuronRuntime::Pipeline == nullptr)
            return nullptr;
        return NeuronRuntime::Pipeline;
    }

    void ResetImmedBatchDirty()
    {
        // — listing 005a29b0..005a2a10.
        if (G_ImmedBatchA.Dirty != 0)
        {
            G_ImmedBatchA.Dirty = 0;
            G_ImmedBatchA.VertCount = 0;
            G_ImmedBatchA.IndexCount = 0;
            G_ImmedBatchA.VertCursor = G_ImmedBatchA.VertBase;
            G_ImmedBatchA.IndexCursor = G_ImmedBatchA.IndexBase;
        }
        if (G_ImmedBatchB.Dirty != 0)
        {
            G_ImmedBatchB.Dirty = 0;
            G_ImmedBatchB.VertCount = 0;
            G_ImmedBatchB.IndexCount = 0;
            G_ImmedBatchB.VertCursor = G_ImmedBatchB.VertBase;
            G_ImmedBatchB.IndexCursor = G_ImmedBatchB.IndexBase;
        }
    }

    // {state, value} tables the flush walks until a zero state: the field (render states
    // on), the field (render states off), the field (texture stage states, applied to
    // stage 0 and then stage 1). Values read from the PE .rdata.
    struct FImmedStatePair
    {
        int State;
        int Value;
    };
    FImmedStatePair const kImmedRsOn[] = {{0x8d, 1}, {0x91, 1}, {0, 0}};
    FImmedStatePair const kImmedRsOff[] = {{0x8d, 0}, {0, 0}};
    FImmedStatePair const kImmedTss[] = {{1, 1}, {2, 2}, {3, 1}, {4, 1}, {5, 2}, {6, 1}, {0, 0}};

    // The handle the flush passes for "the pipeline's current buffer".
    void* const kImmedCurrentBuffer = reinterpret_cast<void*>(static_cast<intptr_t>(-1));

    // and 0x005a31ce..0x005a31ea: a stack object {pipeline, state,
    // saved value} (EH state 0, unwind the branch) -- the state is read into it and set to 0,
    // and written back when it goes out of scope.
    struct FImmedStateSaver
    {
        IRenderPipeline* Pipe;
        int State;
        int Saved;

        explicit FImmedStateSaver(int InState) : Pipe(NeuronRuntime::Pipeline), State(InState), Saved(0)
        {
            Pipe->GetRenderState((D3DRENDERSTATETYPE)State, &Saved); // +0xf4
            Pipe->SetRenderState((D3DRENDERSTATETYPE)State, 0);      // +0xf0
        }

        ~FImmedStateSaver()
        {
            Pipe->SetRenderState((D3DRENDERSTATETYPE)State, Saved);
        }
    };

    // (module storage-005a3205), thiscall on a batcher.
    void FlushImmedBatch(FImmedBatcher* Batch)
    {
        // the dirty byte is raised before the empty test.
        Batch->Dirty = 1;
        if (Batch->VertCount == 0)
            return;

        FSpaceCamera* const WinCamera = Win3DGetCamera(); // &the field
        if (Batch->bOrtho == 0)
        {
            // the camera's perspective (the routine), then lighting,
            // depth, culling and texturing off, blending on with SRCALPHA / INVSRCALPHA.
            Win3DApplyCamera(AsCamera(WinCamera));
            Win3DDisable(kWin3DLightingCap); // 0xb50
            Win3DDisable(kWin3DDepthTest);   // 0xb71
            Win3DDisable(kWin3DCullFace);    // 0xb44
            Win3DDisable(kWin3DTexture2D);   // 0xde1
            Win3DEnable(kWin3DBlend);        // 0xbe2
            Win3DSetBlendFunc(kWin3DBlendSrcAlpha, kWin3DBlendInvSrcAlpha);
        }
        else
        {
            // lighting, texturing, culling, depth and fog off, RS 0x16 =
            // 1, the camera's 2D box (the routine), RS 0x8b = -1, blending on.
            Win3DDisable(kWin3DLightingCap); // 0xb50
            Win3DDisable(kWin3DTexture2D);   // 0xde1
            Win3DDisable(kWin3DCullFace);    // 0xb44
            Win3DDisable(kWin3DDepthTest);   // 0xb71
            Win3DDisable(kWin3DFog);         // 0xb60
            NeuronRuntime::Pipeline->SetRenderState(D3DRS_CULLMODE, 1);  // +0xf0
            Win3DSetOrtho2DForCamera(WinCamera);
            NeuronRuntime::Pipeline->SetRenderState(D3DRS_AMBIENT, -1); // +0xf0
            Win3DEnable(kWin3DBlend);                  // 0xbe2
            Win3DSetBlendFunc(kWin3DBlendSrcAlpha, kWin3DBlendInvSrcAlpha);
        }

        // the source descriptor -- every dword zeroed, Flags 2, the
        // position and diffuse streams of the 0x10-byte vertices, FVF 0x42 and the count.
        FRpVertexSrcDesc Desc;
        memset(&Desc, 0, sizeof(Desc));
        Desc.Flags = 2;
        Desc.Position.Base = reinterpret_cast<unsigned char const*>(Batch->VertBase);
        Desc.Position.ByteSize = 0xc;
        Desc.Position.Stride = 0x10;
        Desc.Diffuse.Base = reinterpret_cast<unsigned char const*>(Batch->VertBase) + 0xc;
        Desc.Diffuse.ByteSize = 4;
        Desc.Diffuse.Stride = 0x10;
        Desc.SrcFvf = 0x42;
        Desc.SrcCount = static_cast<unsigned int>(Batch->VertCount);

        // .
        for (FImmedStatePair const* Pair = kImmedRsOn; Pair->State != 0; ++Pair)
            NeuronRuntime::Pipeline->SetRenderState((D3DRENDERSTATETYPE)Pair->State, Pair->Value);
        {
            FImmedStateSaver LightingSaver(0x89);
            for (FImmedStatePair const* Pair = kImmedTss; Pair->State != 0; ++Pair)
                NeuronRuntime::Pipeline->SetTextureStageState(0, Pair->State, Pair->Value); // +0xf8
            for (FImmedStatePair const* Pair = kImmedTss; Pair->State != 0; ++Pair)
                NeuronRuntime::Pipeline->SetTextureStageState(1, Pair->State, Pair->Value);

            // vertices and indices into the pipeline's current
            // buffers (handle -1), both selected, one indexed triangle-list draw. The first
            // non-zero result skips the rest; the draw's own result is not read.
            unsigned int VbOffset = 0;
            unsigned int IbOffset = 0;
            if (Batch->Vb->CopyVertices(kImmedCurrentBuffer, &VbOffset, &Desc, 0,
                                        (unsigned int)Batch->VertCount) == 0 &&           // +0x18
                Batch->Ib->CopyIndices(kImmedCurrentBuffer, &IbOffset, Batch->IndexBase,
                                       static_cast<unsigned int>(Batch->IndexCount)) == 0 && // +0x18
                Batch->Vb->SelectVb(kImmedCurrentBuffer) == 0 &&                  // +0x28
                Batch->Ib->SelectIb(kImmedCurrentBuffer, VbOffset, 0, 0) == 0)    // +0x24
            {
                Batch->Draw->DrawIndexedPrimitive(4, 0,
                                                  static_cast<unsigned int>(Batch->VertCount),
                                                  IbOffset,
                                                  static_cast<unsigned int>(Batch->IndexCount)); // +0xc
            }

            // render states off, then the perspective again after an
            // ortho flush (the routine); the saver restores RS 0x89 last.
            for (FImmedStatePair const* Pair = kImmedRsOff; Pair->State != 0; ++Pair)
                NeuronRuntime::Pipeline->SetRenderState((D3DRENDERSTATETYPE)Pair->State, Pair->Value);
            if (Batch->bOrtho != 0)
                Win3DApplyCamera(AsCamera(WinCamera));
        }
    }

    // (module storage-005a2e71), thiscall: both arrays allocated and both counts
    // zeroed (the cursors are left to the routine), then each pipeline interface that is still
    // null is queried from NeuronRuntime::Pipeline (slot 0). The query results are not read.
    void CreateImmedBatch(FImmedBatcher* Batch)
    {
        Batch->VertBase = static_cast<FImmedVertex*>(operator new(0xc000));
        Batch->IndexBase = static_cast<unsigned short*>(operator new(0x3000));
        Batch->VertCount = 0;
        Batch->IndexCount = 0;
        if (Batch->Vb == nullptr)
            NeuronRuntime::Pipeline->QueryInterface(Iids::RPVertexBuffer,
                                            reinterpret_cast<void**>(&Batch->Vb));
        if (Batch->Ib == nullptr)
            NeuronRuntime::Pipeline->QueryInterface(Iids::RPIndexBuffer,
                                            reinterpret_cast<void**>(&Batch->Ib));
        if (Batch->Draw == nullptr)
            NeuronRuntime::Pipeline->QueryInterface(Iids::RPDraw, reinterpret_cast<void**>(&Batch->Draw));
    }

    // (module storage-005a2edc), thiscall: each array freed (the routine) and
    // each interface released (slot 2); every non-null field is nulled after it goes.
    void DestroyImmedBatch(FImmedBatcher* Batch)
    {
        if (Batch->VertBase != nullptr)
        {
            operator delete(Batch->VertBase);
            Batch->VertBase = nullptr;
        }
        if (Batch->IndexBase != nullptr)
        {
            operator delete(Batch->IndexBase);
            Batch->IndexBase = nullptr;
        }
        if (Batch->Vb != nullptr)
        {
            Batch->Vb->Release();
            Batch->Vb = nullptr;
        }
        if (Batch->Ib != nullptr)
        {
            Batch->Ib->Release();
            Batch->Ib = nullptr;
        }
        if (Batch->Draw != nullptr)
        {
            Batch->Draw->Release();
            Batch->Draw = nullptr;
        }
    }

    void FlushImmedBatchers()
    {
        // ECX=the field; CALL; ECX=the field; JMP.
        FlushImmedBatch(&G_ImmedBatchA);
        FlushImmedBatch(&G_ImmedBatchB);
    }

    void EnsureButtonMontage()
    {
        // — if not loaded (the field), the routine.
        if (!G_bButtonMontageLoaded)
            LoadButtonMontage();
    }

    void FlushMontageButtons()
    {
        // the whole body (both savers and the "buttons" flush) lives in
        // NnMontageFlushButtons (nn_montage_draw.cpp).
        NnMontageFlushButtons();
    }

    void PrepUiOverlayPipeline()
    {
        // — listing 00417f10..004180ba.
        // PUSH 0x1701; CALL empty OnDisconnect — no-op (00401e90).
        Win3DPushProjection();
        Win3DSetOrtho2D();

        IRenderPipeline* Pipe = GetPipe();
        if (Pipe != nullptr)
        {
            int SavedTss10 = 0;
            int SavedTss11 = 0;
            Pipe->GetTextureStageState(0, 0x10, &SavedTss10);
            Pipe->GetTextureStageState(0, 0x11, &SavedTss11);
            if (G_UiOverlayForceTss != 0)
            {
                Pipe->SetTextureStageState(0, 0x10, 1);
                Pipe->SetTextureStageState(0, 0x11, 1);
            }

            int Saved17 = 0;
            int Saved0E = 0;
            Pipe->GetRenderState(D3DRS_ZFUNC, &Saved17);
            Pipe->SetRenderState(D3DRS_ZFUNC, 8);
            Pipe->GetRenderState(D3DRS_ZWRITEENABLE, &Saved0E);
            Pipe->SetRenderState(D3DRS_ZWRITEENABLE, 0);

            // the routine(Pipeline, the field) -- the font atlas list, found in
            // the container map by its key (no material-key call before it).
            ScreenQuadFlush(Pipe, FontAtlasMaterialKey());

            Pipe->SetTextureStageState(0, 0x10, SavedTss10);
            Pipe->SetTextureStageState(0, 0x11, SavedTss11);
            Win3DPopProjection();
            Pipe->SetRenderState(D3DRS_ZWRITEENABLE, Saved0E);
            Pipe->SetRenderState(D3DRS_ZFUNC, Saved17);
        }
        else
        {
            Win3DPopProjection();
        }
    }
}

void Win3DImmedBatchReset()
{
    ResetImmedBatchDirty();
}

void Win3DImmedBatchFlushAll()
{
    FlushImmedBatchers();
}

void ImmedBatchAddLine(FImmedBatcher* Batch, float const* A, float const* B,
                       unsigned long Color, unsigned long EdgeColor, float Width)
{
#ifdef OUTPOST_FRONTIER_WALK_MODE
    Color = BaseWalkFadeInterfaceColor(Color);
    EdgeColor = BaseWalkFadeInterfaceColor(EdgeColor);
#endif
    // (module storage-005a2de0). 0x005a2a48..0x005a2ad3: 12 more indices reaching
    // 0x1800 or 8 more vertices reaching 0xc00 (signed JGE / JL) flush A, then B, then run the
    // body inline.
    if (Batch->IndexCount + 12 >= 0x1800 || Batch->VertCount + 8 >= 0xc00)
    {
        FlushImmedBatch(&G_ImmedBatchA);
        FlushImmedBatch(&G_ImmedBatchB);
        ResetImmedBatchDirty();
    }

    // the index count first, then twelve WORD indices on the current
    // vertex count (16-bit adds), then the vertex count.
    Batch->IndexCount += 12;
    static unsigned short const kLineIndexOffsets[12] = {0, 1, 2, 2, 3, 1, 4, 5, 6, 6, 7, 5};
    for (int Index = 0; Index < 12; ++Index)
    {
        *Batch->IndexCursor = static_cast<unsigned short>(
            static_cast<unsigned short>(Batch->VertCount) + kLineIndexOffsets[Index]);
        ++Batch->IndexCursor;
    }
    Batch->VertCount += 8;

    // FPATAN with ST1 = B.x - A.x and ST0 = B.y - A.y, i.e.
    // atan2(dx, dy), plus the double the field (1.57079632675). The cosine is stored as a
    // float first and multiplied by Width from there; the sine * Width stays on the FPU stack.
    double const Dx = static_cast<double>(B[0]) - static_cast<double>(A[0]);
    double const Dy = static_cast<double>(B[1]) - static_cast<double>(A[1]);
    double const Angle = atan2(Dx, Dy) + 1.57079632675;
    float const CosAngle = static_cast<float>(cos(Angle));
    double const SinW = sin(Angle) * static_cast<double>(Width);
    double const CosW = static_cast<double>(CosAngle) * static_cast<double>(Width);

    // eight vertices -- A, A + off, B, B + off, A, A - off, B, B - off,
    // off = (SinW, CosW, 0); the ends in Color, the offset ones in EdgeColor.
    FImmedVertex Vertices[8];
    Vertices[0] = {A[0], A[1], A[2], Color};
    Vertices[1] = {static_cast<float>(static_cast<double>(A[0]) + SinW),
                   static_cast<float>(static_cast<double>(A[1]) + CosW), A[2], EdgeColor};
    Vertices[2] = {B[0], B[1], B[2], Color};
    Vertices[3] = {static_cast<float>(static_cast<double>(B[0]) + SinW),
                   static_cast<float>(static_cast<double>(B[1]) + CosW), B[2], EdgeColor};
    Vertices[4] = {A[0], A[1], A[2], Color};
    Vertices[5] = {static_cast<float>(static_cast<double>(A[0]) - SinW),
                   static_cast<float>(static_cast<double>(A[1]) - CosW), A[2], EdgeColor};
    Vertices[6] = {B[0], B[1], B[2], Color};
    Vertices[7] = {static_cast<float>(static_cast<double>(B[0]) - SinW),
                   static_cast<float>(static_cast<double>(B[1]) - CosW), B[2], EdgeColor};
    for (int Index = 0; Index < 8; ++Index)
    {
        *Batch->VertCursor = Vertices[Index];
        ++Batch->VertCursor;
    }
}

void Win3DImmedBatchCreateAll()
{
    // (module storage-005a298b).
    CreateImmedBatch(&G_ImmedBatchA);
    CreateImmedBatch(&G_ImmedBatchB);
    G_ImmedBatchB.bOrtho = 1;
}

void Win3DImmedBatchDestroyAll()
{
    // (module storage-005a29a4): A, then a tail jump for B.
    DestroyImmedBatch(&G_ImmedBatchA);
    DestroyImmedBatch(&G_ImmedBatchB);
}

void Win3DImmedDrawLine(float const* A, float const* B, unsigned long Color,
                        unsigned long EdgeColor, float Width)
{
    // (module storage-005a2963), cdecl: the five arguments passed on unchanged to
    // on the field.
    ImmedBatchAddLine(&G_ImmedBatchB, A, B, Color, EdgeColor, Width);
}

void ButtonMontageEnsure()
{
    EnsureButtonMontage();
}

void ButtonMontageFlushBatch()
{
    FlushMontageButtons();
}

void UiOverlayPrepPipeline()
{
    PrepUiOverlayPipeline();
}

void Win3DFlushBeforeOverlayDraw()
{
    // listing order: 005a2a20, 005a29b0, 00559c20, 00559c10, 00417f10.
    FlushImmedBatchers();
    ResetImmedBatchDirty();
    FlushMontageButtons();
    EnsureButtonMontage();
    PrepUiOverlayPipeline();
}
