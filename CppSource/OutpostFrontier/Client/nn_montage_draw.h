// Author: Giperion.

#pragma once

// baseline ButtonMontage -- the tile atlas behind every 2D HUD / NeuroNet icon ("montage" tiles):
// interface\ButtonMontage.ini names square tiles on the ButtonTextures.ini shapes, and
// draws one tile through the screen-quad batcher under the "buttons" material key.

// Globals (.data):
// GetCRC32("buttons"), static initialiser the branch
// the ButtonTextures ShapeDB (ctor the routine from the branch, atexit
// -> the routine)
// std::vector<tile> (allocator byte, then begin / end / capacity at
// / the field / the field); the branch zeroes it,
// (atexit) destroys every element and frees the buffer
// loaded flag (the routine sets it; nothing clears it)
// / the field / the field / the field -- the "Montage32", "Montage64",
//                   "HUD_targetingelement" and "HUD_talkelement" shapes

#include "texture_shapes.h"

#include <string>
#include <vector>

// The 0x24-byte vector element the routine appends (the VC6 std::string is its first 0x10 bytes;
// is that string's destructor).
struct FButtonMontageTile
{
    std::string Name;   // +0x00 the ini key
    float U;            // +0x10 upper-left u on the shape
    float V;            // +0x14 upper-left v
    float Step;         // +0x18 tile size in uv (1 / (256 / size), or 0.5 for texture = 3)
    FTexShape* Shape;   // +0x1c the ButtonTextures shape the tile sits on (may be null)
    float Scale;        // +0x20 the key's optional second value (the routine reads it), else 0
};

extern unsigned long G_ButtonsMaterialKey;
extern FShapeDB G_ButtonShapeDB;
extern std::vector<FButtonMontageTile> G_ButtonMontageTiles;
extern bool G_bButtonMontageLoaded;

// (cdecl) -- once (the field): load Interface\ButtonTextures.ini into
// , find the four shapes, then read every [Montage] section of
// interface\ButtonMontage.ini ("size", "texture", "num_pages" and one key per tile).
void LoadButtonMontage();

// (cdecl, the string by value) -- load on first use, then the first tile whose name
// _stricmp-equals Name, or null.
FButtonMontageTile* FindButtonMontageTile(std::string Name);

// (cdecl) -- empty the tile vector, then the ShapeDB (the routine). The loaded
// flag stays set. Caller: the client teardown 0x005b2237.
void ClearButtonMontage();

// (cdecl) in its baseline form -- false without a tile or without its shape; else
// the shape's texture frame (NeuronRuntime::TextureLib slot 22, frame 0), its UV rect replaced by the
// tile's square inset by 1/256 (the field) and mirrored per Mode (0 plain, 1 horizontal,
// 2 vertical, 3 both; other values keep the frame's rect), the "buttons" material key
// (the routine), the batch node for the frame (the routine, blend 2, find), and one icon of
// Size at Position through the Win3D camera (the routine(&the field, ..., Angle, 0)) in
// the colour of the RGBA bytes. True then. Callers: the 21 HUD_Targetable_Objects sites the
// XREF header lists (the routine, the routine, the routine) and ButtonControl's body draw
// (0x00559b86, missing from that header; Mode 0, Angle 0).
bool NnMontageDrawTileAt(FButtonMontageTile const* Tile, float const* Position,
                         unsigned char const* Rgba, float Size, int Mode, float Angle);

// — flush the "buttons" material batch (ScreenQuadFlush).
void NnMontageFlushButtons();
