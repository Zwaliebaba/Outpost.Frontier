// Author: Giperion.

#pragma once

// Screen-quad / 2D batcher — baseline the routine / the routine / the routine / the routine /
// / the routine / the routine / the routine..the routine cluster.

// = material-list key (the routine); change nulls the field.
// = lazy 3DMathEngine (the routine) for TransformXform; clip rect / scales
// come from the camera (vtbl +0x14 / +0x20 / +0x2c / +0x30), not from that singleton.
// = std::map of batch containers keyed by the material-list key; the field =
// the container of the current key (free@+0 / live@+0xc / current@+0x18), found or inserted by
// ; nodes 0x28 pooled by the routine. Flush recycles live→free
// (the routine+the routine) in the one container it was asked for.
// Flush VB via IVertexBufferManager::LockVertexMemory (FVF pool) — never CreateVb mid-scene;
// IB via LockIb(-1) scratch. CPU record growth = count+0x21 (the routine / the routine).

struct IRenderPipeline;
struct FSpaceCamera;

// 11 floats / 0x2C — the routine / the routine / the routine record layout.
struct FScreenQuadRecord
{
    float X0;       // +0x00
    float Y0;       // +0x04
    float X1;       // +0x08
    float Y1;       // +0x0c
    float DepthA;   // +0x10  (1 - scale*near); HUD bars use 1.0
    float DepthB;   // +0x14
    float U0;       // +0x18
    float V0;       // +0x1c
    float U1;       // +0x20
    float V1;       // +0x24
    // +0x28 packed ARGB, a raw dword: baseline only ever moves it with MOV (the vertex expand
    // reads it at 0x00404387, 0x004043d6, 0x0040440c, 0x0040443d). Not a float:
    // in an x87 build (/arch:IA32) a float passed by value goes through FLD/FSTP, which
    // quiets a signalling NaN, and an opaque colour (A = 0xFF) with R in 0x80..0xBF is one --
    // its R would gain 0x40 (grey 0xFF808080 -> 0xFFC08080).
    unsigned ColorBits;
};

// The pipeline render-state saver baseline keeps on the stack (the routine, the routine,
// , ...): {pipeline, state, saved}; the constructor reads the state (slot 61, +0xf4)
// into Saved and sets the new value (slot 60, +0xf0); the destructor the routine (inlined at
// most sites, and run by the EH unwind funclets) writes Saved back.
struct FScreenQuadRenderStateSaver
{
    IRenderPipeline* Pipe;
    int State;
    int Saved;

    FScreenQuadRenderStateSaver(IRenderPipeline* InPipe, int InState, int Value);
    ~FScreenQuadRenderStateSaver();

    FScreenQuadRenderStateSaver(FScreenQuadRenderStateSaver const&) = delete;
    FScreenQuadRenderStateSaver& operator=(FScreenQuadRenderStateSaver const&) = delete;
};

// — 1D interval clip against [Lo, Hi), interpolating a paired attr.
int ScreenQuadClip1D(float* Edge0, float* Edge1, float* Attr0, float* Attr1, float Lo, float Hi,
                     float Span);

// Cached SetRenderState / SetPipelineState / SetTextureStageState — skip when unchanged.
void ScreenQuadSetRenderState(IRenderPipeline* Pipe, int State, int Value);
void ScreenQuadSetPipelineState(IRenderPipeline* Pipe, int Id, int Value);
void ScreenQuadSetTextureStageState(IRenderPipeline* Pipe, int Stage, int Type, int Value);
void ScreenQuadInvalidateStateCache();

// MenuSetup2D path (different RS preset from the batcher the field table).
void ApplyScreenQuadRenderStates(IRenderPipeline* Pipe);

// — the field material-list key. On change only the field is nulled; the
// previous key's container stays in the map with its queued nodes.
void ScreenQuadSetMaterialKey(unsigned long Key);

// — clear the whole container map (every node destroyed) + restore the default
// key, nulling the field only when the key changes.
void ScreenQuadResetDrawBatchCache();

// the default texture frame {no texture, u0 0, v0 0, u1 1.0f, v1 1.0f} (.data,
// never written): a new container's key (the routine / the routine) and the frame the core
// bars (0x004d54a2) and the IME background (0x00563f91) hand to the routine.
extern unsigned long const G_ScreenQuadDefaultFrame[5];

// (cdecl) -- a light's flare at Position through Cam: 0 when it is not in front of
// the near plane (depth z * 0.999) or both horizontal radii (size * near * x-scale-over-near /
// -depth) are below 1, otherwise 1 after up to two viewport-clipped squares (bulb, then glow;
// each only above one pixel and with a colour that is not black) in the current container's
// current node with its UV rect. Colours are RGB in 0..1. Sole caller: the routine.
struct ICamera;
unsigned char ScreenQuadEnqueueFlare(ICamera* Cam, float const* Position, float const* BulbColor,
                                     float const* GlowColor, float BulbSize, float GlowSize);

// (cdecl, returns the current node) -- the 5-dword texture frame (handle, then the
// u0 / v0 / u1 / v1 rect, as ITextureLibrary2 slot 22 fills it) copied into the container's key
// (+0x1c..+0x2c); the current node kept when its texture (+0x20) matches, otherwise with bFind
// the first live node that matches, otherwise a new node (the routine) appended to the live
// list with the texture, BlendMode and empty buffers. BlendMode as the routine reads it:
// 0 opaque, 1 SRC/DEST 2/2 (ONE/ONE), 2 SRC/DEST 5/6 (SRCALPHA/INVSRCALPHA). The records go in
// through ScreenQuadAppendRecord (baseline's inline append into the returned node).
void ScreenQuadBeginBatchFrame(unsigned long const* Frame, int BlendMode, bool bFind);

// The record append baseline inlines into the callers of the routine (NumberString paint
// 00413bb7..00413c6a / 00413cfe..00413dc1, the routine 0x004035e6..0x004036a8):
// count + 1; past the capacity a new block of count + 1 + 0x20 records (operator new), the old
// records copied and the old block freed; the 0x2c record written at the old count -- into the
// node the routine returned, i.e. the container's current node.
void ScreenQuadAppendRecord(FScreenQuadRecord const& Record);

// (cdecl) -- one icon square of Size (world units) around Position through Cam
// (the routine), in the current node with the container's UV rect: with |Angle| <= 1e-5 an
// axis-aligned record clipped to the viewport (the routine on both axes), otherwise four
// rotated vertices, dropped whole when any falls outside the viewport. Argb is a D3DCOLOR. The
// sixth argument is never read. Sole caller: the routine (Cam = &the field).
struct ICamera;
void ScreenQuadEnqueueIcon(ICamera* Cam, float const* Position, unsigned long Argb, float Size,
                           float Angle, int Unused);

// (cdecl) -- Position through the camera's view transform (slot 16, the
// TransformXform of the field); culled (0) unless it lies in front of the near plane.
// OutInvDepth = -1 / z; the half extents are Size * near * x-/y-scale-over-near * InvDepth / 2
// (the vertical one made positive), culled again when the horizontal one is below 1; the centre
// is the viewport's (slot 5) plus the scaled camera-space x / y. Sole caller: the routine.
unsigned char ScreenQuadProjectObjectExtent(ICamera* Cam, float const* Position, float Size,
                                            float* OutScreenX, float* OutScreenY,
                                            float* OutHalfW, float* OutHalfH,
                                            float* OutInvDepth);

// the "current list" key: -1, seeded by the static initializer the branch
// (the PE image holds 0). the routine compares its key argument against it; the routine and
// callers whose own key global is -1 (the field core bars, the field effects) end up on
// the list of the current material key.
extern unsigned long G_ScreenQuadCurrentListKey;

// (cdecl: pipeline, list key). Render states forced for 2D screen quads (three
// saved-and-restored states, then the the field / 005c7540 / 005c7568 / 005c75a0 tables),
// then the nodes of ONE container drawn through the routine and moved to its free list: the
// current container (created on demand) when Key is G_ScreenQuadCurrentListKey, otherwise the
// map entry for Key when there is one. Then the material key goes back to the default.
void ScreenQuadFlush(IRenderPipeline* Pipe, unsigned long Key);

// .data byte 1, never written: the routine forces point filtering when set.
extern unsigned char G_BigImageTilePointFilter;

// (cdecl) -- the FBigImageDrawTile callback the exe hands to ximage's BigImage
// (FBigImageDesc::DrawTile, render/IBigImage2.h): projection pushed and made 2D, the 2D screen-
// quad render states (five saved-and-restored, stage 0 filters saved and forced to point while
// is set), the tile queued on the "image" material list (blend 2) with the frame
// {TextureHandle, UvQuad[0], UvQuad[7], UvQuad[6], UvQuad[1]}, clipped to the surface, and when
// anything is left flushed at once through the routine. Nothing is null-checked.
void __cdecl ScreenQuadDrawBigImageTile(void* TextureHandle, unsigned long PackedColor,
                                        float* DestXy, float* UvQuad);
