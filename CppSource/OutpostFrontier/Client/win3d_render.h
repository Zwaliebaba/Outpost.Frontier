// Author: Giperion.

#pragma once

// baseline Win3D / OpenGL layer helpers: the glClear translation and the camera apply.
// glEnable / glDisable (the routine / the routine) are Win3DEnable / Win3DDisable in
// win3d_immediate.h.

// the glClear translation. Its argument is a GL-style bit set, but baseline only
// ever looks at AH, so exactly two bits mean anything and every other bit (including anything
// above bit 15) is ignored:

//     0x4000 -> clear the colour buffer      0x0100 -> clear the depth buffer

// Those map to D3DCLEAR_TARGET (1) and D3DCLEAR_ZBUFFER (2) for IRenderPipeline::ClearBuffers.
// The call sites pass 0x100 (depth only, e.g. camera slot 0x78), 0x4000 (colour only) and
// 0x4100 (both).
enum EWin3DClearBits
{
    kWin3DClearColour = 0x4000,
    kWin3DClearDepth  = 0x0100
};

void ClearWin3DBuffers(unsigned int Bits);

// (cdecl, one pointer to four 0..255 bytes in R,G,B,A order). Sets the pipeline's
// clear colour (each byte times 1/255) and clears both the colour and depth buffers -- baseline
// tail-calls the routine(0x4100) after setting the colour, which is exactly ClearWin3DBuffers
// with both bits set.
void ClearWin3DBuffersToColor(unsigned char const* Rgba);

// (cdecl, one argument): the camera's perspective through the routine (fovY,
// aspect, znear, zfar from slots 0x1c / 0x28 / 0x20 / 0x24), then its packed view (slot 0x40)
// copied into the field; pipeline state 7 is READ -- non-zero sends the copy through
// SetModelview, zero sets an identity view and the slot-0x40 view as the world.
struct ICamera;
void Win3DApplyCamera(ICamera* Camera);
