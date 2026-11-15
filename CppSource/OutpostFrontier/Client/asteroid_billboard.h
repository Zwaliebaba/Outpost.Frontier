// Author: Giperion.

#pragma once

// baseline Client\Space\SpaceTerrainRenderComp.cpp, the asteroid-field billboard half: the rock
// sprites a CliAsteroid field draws beyond its real asteroid cubes (FCliAsteroid +0x198 flag,
// +0x19C block, see cli_asteroid.h), and the vertex-buffer batch they are drawn through.

// baseline call path: CliAsteroid tick the routine -> (Flag198, ASTEROID_EXT > 1, [EFFECTS],
// ) -> the routine ambient pass -> the routine(camera, field ExclusionMgr, 2).

// The batch is one static object, the field (static init the branch = the routine(0x142),
// atexit the branch -> the routine). It is an immediate-mode writer over
// IVertexBufferManager::LockVertexMemory: three {pointer, stride} cursors into the locked
// vertices, a current colour and texture coordinate, and a two-vertex save area for the
// loop-closing and quad-splitting modes (flags +0x58 bits 0..2).

#include "cli_asteroid.h"
#include "render/IRenderPipeline.h"

struct FSpaceCamera;
struct IVertexBufferManager;

namespace Exclusion
{
    class AsteroidExclusionZoneMgr;
}

// A {write pointer, stride} pair inside the batch (+0x6c / +0x74 / +0x7c). The baseline dtor
// zeroes both fields: the routine 0x0054dbbc..0x0054dbce clears them in reverse member order,
// which is what three member destructors produce.
struct FBillboardVertexCursor
{
    explicit FBillboardVertexCursor(int InStride);
    ~FBillboardVertexCursor();

    // (thiscall, RET 4): Ptr += Stride. The argument is never read.
    void Advance(int Unused);

    unsigned char* Ptr; // +0x00
    int Stride;         // +0x04
};

// One saved vertex, 0x18 bytes (batch +0x28 and +0x40).
struct FBillboardSavedVertex
{
    float Pos[3];       // +0x00
    unsigned Color;     // +0x0c
    float U;            // +0x10
    float V;            // +0x14
};

static_assert(sizeof(FBillboardSavedVertex) == 0x18, "saved vertex 0x18");

// , 0x84 bytes.
class FAsteroidBillboardBatch
{
public:
    // (thiscall, RET 4).
    explicit FAsteroidBillboardBatch(unsigned InFvf);
    // .
    ~FAsteroidBillboardBatch();

    // (thiscall, RET 8): release the held pipeline (+0x0c) and vertex-buffer
    // manager (+0x10), then hold and AddRef the new ones that are not null.
    void SetInterfaces(IRenderPipeline* InPipeline, IVertexBufferManager* InVbMgr);
    // flags bits 0..2, the counters, colour -1, UV 0, primitive 0x7fffffff.
    void Reset();
    // (thiscall, RET 8): the three cursors from the locked vertex pointer. The
    // strides come from the batch's own lock FVF (+0x18); the offsets from InFvf.
    void BindCursors(unsigned char* Base, unsigned InFvf);
    // (thiscall, RET 0xc): one vertex at the current colour and UV.
    void Vertex(float X, float Y, float Z);

    unsigned Prim;                  // +0x00 0x7fffffff when idle, 4 (triangle list) in use
    int Count;                      // +0x04 vertices written
    unsigned Fvf;                   // +0x08 ctor argument, 0x142
    IRenderPipeline* Pipeline;      // +0x0c AddRef'd
    IVertexBufferManager* VbMgr;    // +0x10 AddRef'd
    FVertexMemoryLock Lock;         // +0x14 borrowed listing and locked vertices
    int LockedCount;                // +0x24
    FBillboardSavedVertex Saved[2]; // +0x28
    unsigned char Flags;            // +0x58 bit 0 save-first, bit 1 close-loop, bit 2 quads
    unsigned char Pad59[3];
    unsigned QuadCounter;           // +0x5c
    float U;                        // +0x60
    float V;                        // +0x64
    unsigned Color;                 // +0x68 D3DCOLOR
    FBillboardVertexCursor PosCursor;   // +0x6c
    FBillboardVertexCursor ColorCursor; // +0x74
    FBillboardVertexCursor UvCursor;    // +0x7c

private:
    FAsteroidBillboardBatch(FAsteroidBillboardBatch const&) = delete;
    FAsteroidBillboardBatch& operator=(FAsteroidBillboardBatch const&) = delete;
};

// .
extern FAsteroidBillboardBatch G_AsteroidBillboardBatch;

// (cdecl): SetRenderState for each {state, value} pair up to a zero state.
void BillboardApplyRenderStates(IRenderPipeline* Pipeline, unsigned const* Table);

// (thiscall on the billboard block, RET 0x24): the ambient colour (D3DRS_AMBIENT *
// ambient_intensity / 255 * tinted ambient * color_shift) into all five outputs, then for up
// to MaxLights directional (type 3) lights within 100 of Pos, each light's ambient share and
// its tinted diffuse colour weighted by how far the light faces each of the five directions
// Forward + Right, Forward - Right, Forward + Up, Forward - Up and Forward; each output
// clamped to 1.0 per component.
void BillboardComputeLighting(FAsteroidBillboardBlock* Bb, float const* Pos,
                              float const* Forward, float const* Up, float const* Right,
                              float* OutCenter, float* OutCorner0, float* OutCorner1,
                              float* OutCorner2, float* OutCorner3);

// (thiscall on the billboard block, RET 0x14; the block is not read): one rock as
// a single triangle into G_AsteroidBillboardBatch -- base P -/+ A, apex P + B, A and B the
// rock-size-scaled Right / Up (swapped by bSwapAxes, B negated by bNegateApex), the base
// vertices in CornerColor with a corner-UV pair picked by the flip bits, the apex in
// CenterColor at UV (0.5, 0.5), all at the rock's alpha.
void BillboardEmitRock(FAsteroidBillboardBlock* Bb, FAsteroidBillboardRock* Rock,
                       float const* Right, float const* Up, float const* CornerColor,
                       float const* CenterColor);

// (thiscall on the billboard block, RET 0xc). Caller the routine passes the
// field's ExclusionMgr (+0xdc) and ExcludeType 2 (billboards).
void BillboardDraw(FAsteroidBillboardBlock* Bb, FSpaceCamera* Camera,
                   Exclusion::AsteroidExclusionZoneMgr* Excl, int ExcludeType);
