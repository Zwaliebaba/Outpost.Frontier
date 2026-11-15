// Author: Giperion.

#pragma once

// The helpers the routine (Win3DDrawAll, win3d.cpp) calls around its draw loop.

// The raw-offset renderable registry that used to live here (RegisterRenderable /
// DrawRenderables on void*) is gone: the registry is Win3D's own list the field, typed as
// Win3D* in win3d.cpp.

struct IRPVertexBuffer;
struct IRPIndexBuffer;
struct IRPDraw;

// One vertex of the immediate batchers: position and diffuse, FVF 0x42, 0x10 bytes.
struct FImmedVertex
{
    float X;             // +0x00
    float Y;             // +0x04
    float Z;             // +0x08
    unsigned long Color; // +0x0c
};

// The immediate line batchers the field (A, the wire controls' lines, drawn through the
// Win3D camera's perspective) and the field (B, the HUD's 2D lines -- the routine sets its
// +0x25 byte, so it draws through the camera's ortho box). 0x28 bytes. Static initializers
// / the branch zero +0x00..+0x18, set Dirty 1 and +0x25 0.
struct FImmedBatcher
{
    IRPVertexBuffer* Vb;         // +0x00  QI "1.11_IRPVertexBuffer" (the routine)
    IRPIndexBuffer* Ib;          // +0x04  QI "1.11_IRPIndexBuffer"
    IRPDraw* Draw;               // +0x08  QI "1.11_IRPDraw"
    FImmedVertex* VertBase;      // +0x0c  operator new(0xc000): 0xc00 vertices
    unsigned short* IndexBase;   // +0x10  operator new(0x3000): 0x1800 indices
    FImmedVertex* VertCursor;    // +0x14
    unsigned short* IndexCursor; // +0x18
    int VertCount;               // +0x1c
    int IndexCount;              // +0x20
    unsigned char Dirty;         // +0x24  set by every flush; the routine rewinds on it
    unsigned char bOrtho;        // +0x25  flush through the routine instead of the routine
    unsigned char Pad26[2];
};

extern FImmedBatcher G_ImmedBatchA;
extern FImmedBatcher G_ImmedBatchB;

// (thiscall on a batcher, RET 0x14) -- one thick line A -> B: both batchers are
// flushed and rewound first when 12 more indices or 8 more vertices would not fit, then eight
// vertices (A and B in Color, each also pushed Width to either side along the perpendicular in
// EdgeColor) and twelve indices.
void ImmedBatchAddLine(FImmedBatcher* Batch, float const* A, float const* B,
                       unsigned long Color, unsigned long EdgeColor, float Width);

// the routine on both batchers, then B's +0x25 = 1. Caller: the routine.
void Win3DImmedBatchCreateAll();
// the routine on both batchers. Caller: the routine.
void Win3DImmedBatchDestroyAll();

// reset both immediate batchers (the field / the field) whose dirty
// byte is set.
void Win3DImmedBatchReset();

// the routine on the field, then on the field.
void Win3DImmedBatchFlushAll();

// (cdecl) -- a 2D line from A to B (x, y, z each) into the second immediate batcher
// (the field) through the routine (RET 0x14): both batchers are flushed first when 12 more
// indices or 8 more vertices would not fit; then eight vertices -- A and B in Color, each also
// offset Width to either side along the perpendicular (atan2 of B - A plus the field) in
// EdgeColor -- and twelve indices. Callers: HUD_Targetable_Objects' bracket drawer
// (0x004eb06a..0x004ebc8a) and lead-marker cross (the routine).
void Win3DImmedDrawLine(float const* A, float const* B, unsigned long Color,
                        unsigned long EdgeColor, float Width);

// ButtonMontage.ini loaded once (the routine).
void ButtonMontageEnsure();

// flush the "buttons" material batch with RS 0x17 / 0xe saved around it.
void ButtonMontageFlushBatch();

// 2D overlay prep and the font-atlas screen-quad flush.
void UiOverlayPrepPipeline();

// IME focus overlay pass (defined in client_ime_focus.cpp): with a singleton that
// has input boxes, IMEFocus::Draw(0) under a 2D projection, point filtering, depth func ALWAYS /
// no depth write and no culling, then the "IMEbkground" and "IMEtext" screen-quad lists flushed.
void ImeFocusDrawPass();

// Tip Draw and other overlay callers: flush particle batch, reset
// camera dirty, end/begin sorted pass, then UI overlay pipeline prep.
void Win3DFlushBeforeOverlayDraw();

// Incremented by scene-object draw stubs (smoke verification).
extern int G_DrawCallCount;
