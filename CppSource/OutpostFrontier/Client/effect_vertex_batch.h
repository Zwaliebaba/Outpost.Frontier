// Author: Giperion.

#pragma once

// Growable XYZ|DIFFUSE|TEX1 (FVF 0x142, stride 0x18) vertex + 16-bit index batch.
// baseline: the routine / the routine / the routine / the routine / the routine /
// , object layout at the field (Effect.cpp static).

// Distinct from screen_quad_state (the field FlashLight/HUD path) and from
// win3d_immediate (fixed 0x3000 OpenGL emulator). Do not merge.

struct IRenderPipeline;

// One CPU vertex — matches baseline stride 0x18 / FVF 0x142.
struct FEffectVertex
{
    float X;              // +0x00
    float Y;              // +0x04
    float Z;              // +0x08
    unsigned char B;      // +0x0C  D3DCOLOR byte order B,G,R,A
    unsigned char G;      // +0x0D
    unsigned char R;      // +0x0E
    unsigned char A;      // +0x0F
    float U;              // +0x10
    float V;              // +0x14
};

static_assert(sizeof(FEffectVertex) == 0x18, "Effect vertex stride 0x18");

// baseline object starting at the field. Methods take this; Effect.cpp uses the
// global G_EffectVertexBatch.
struct FEffectVertexBatch
{
    unsigned Type;                // +0x00  the branch writes 1
    FEffectVertex* Vertices;      // +0x04  the field
    int VertCount;                // +0x08  the field
    unsigned Fvf;                 // +0x0C  the field = 0x142
    unsigned short* Indices;      // +0x10  the field
    int IndexCount;               // +0x14  the field
    IRenderPipeline* Pipeline;  // +0x18  the field (AddRef'd during flush)
    int Growth;                   // +0x1C  the field = 0x80
    unsigned VertCapacity;        // +0x20  the field
    unsigned IndexCapacity;       // +0x24  the field
    unsigned char Flag28;         // +0x28  the field
    unsigned char bStripMode;     // +0x29  the field
    unsigned char Pad2A;
    unsigned char Pad2B;
    unsigned StripCounter;        // +0x2C  the field
    unsigned char ColorR;         // +0x30  the field  (written to vert +0xE)
    unsigned char ColorG;         // +0x31  the field  (vert +0xD)
    unsigned char ColorB;         // +0x32  the field  (vert +0xC)
    unsigned char ColorA;         // +0x33  the field
    float U;                      // +0x34  the field
    float V;                      // +0x38  the field
};

static_assert(sizeof(FEffectVertexBatch) == 0x3C, "batch object 0x3C");

// — Effect.cpp frame batch for beams / cross-billboards.
extern FEffectVertexBatch G_EffectVertexBatch;

// — once-flag for atexit dtor the branch.
extern unsigned char G_EffectVertexBatchDtorOnce;

// — lazy I3DMathEngine* cache for Effect/Beam TransformXform (+0x28).
struct I3DMathEngine;
extern I3DMathEngine* G_EffectMathEngine;

// + atexit the branch.
void EffectVertexBatchStaticInit();
void EffectVertexBatchStaticShutdown();

// — the batch's destructor: release Pipeline (+0x18) and free both buffers.
// baseline callers: the texture panels' atexit the branch (0x0050a039, the field -- the port's
// FPanelVertexBatcher in texture_panels.cpp, whose +0x04/+0x10/+0x18 are asserted to line up;
// G_PanelBatcherAtExit calls this on it) and
// the unwind funclet of the chat background draw the routine (0x005ba383; chat_text_input.cpp
// FChatBgBatch's destructor). The effect batch's own atexit the branch inlines the same body.
void EffectVertexBatchDestroy(FEffectVertexBatch* Batch);

// — grow Vertices when VertCapacity <= VertCount (step Growth, stride 0x18).
void EffectVertexBatchGrowVertices(FEffectVertexBatch* Batch);

// — zero VertCount/IndexCount/strip state; ensure index capacity >= 1.
void EffectVertexBatchReset(FEffectVertexBatch* Batch);

// — append one vertex from current Color/UV; returns new index (count-1).
int EffectVertexBatchAppend(FEffectVertexBatch* Batch, float X, float Y, float Z);

// — ensure IndexCapacity >= IndexCount + Needed (growth Growth*3).
void EffectVertexBatchEnsureIndices(FEffectVertexBatch* Batch, int Needed);

// — push one triangle (3 indices); grows as needed.
void EffectVertexBatchPushTriangle(FEffectVertexBatch* Batch, unsigned short I0,
                                   unsigned short I1, unsigned short I2);

// — push quad as two triangles (I0,I1,I2) + (I0,I2,I3).
void EffectVertexBatchPushQuad(FEffectVertexBatch* Batch, unsigned short I0,
                               unsigned short I1, unsigned short I2, unsigned short I3);

// — colour bytes = low byte of ftol(component * 255.0), alpha 0xff.
void EffectVertexBatchSetColor(FEffectVertexBatch* Batch, float R, float G, float B);

// (cdecl: camera) — RS/TSS tables (the field/9a3c/99dc/99f4), the routine
// texture, the camera's view (vt+0x40) as the modelview (+0x7c), DrawIndexedPrimitiveUp(4, FVF,
// …) of G_EffectVertexBatch, restore.
struct ICamera;
void EffectVertexBatchFlush(ICamera* Cam);
