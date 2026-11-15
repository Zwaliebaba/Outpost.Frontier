// Author: Giperion.

#include "nn_montage_draw.h"

#include "screen_quad_state.h"

#include "NeuronRuntime.h"
#include "common_ini.h"
#include "common_misc.h"   // GetFileSysPath
#include "neuroncomponents.h"
#include "render/IRenderPipeline.h"
#include "render/shading.h"      // ITextureLibrary2 (NeuronRuntime::TextureLib slot 22)
#include "space_camera.h"        // AsCamera
#include "win3d.h"               // Win3DGetCamera (the field)

#include <cstring>

// Static initialisers in baseline XC order: the branch (0x0060fdb8), the branch (0x0060fdbc),
// (0x0060fdc0).
std::vector<FButtonMontageTile> G_ButtonMontageTiles;
FShapeDB G_ButtonShapeDB;
unsigned long G_ButtonsMaterialKey = NEURONCOMPONENTS_CRC::GetCRC32("buttons");
bool G_bButtonMontageLoaded = false;

namespace
{
    FTexShape* G_ButtonShapeMontage32 = nullptr;
    FTexShape* G_ButtonShapeMontage64 = nullptr;
    FTexShape* G_ButtonShapeTargetingElement = nullptr;
    FTexShape* G_ButtonShapeTalkElement = nullptr;

}

void LoadButtonMontage()
{
    // .
    if (G_bButtonMontageLoaded)
        return;
    G_bButtonMontageLoaded = true;

    // the routine on the field.
    G_ButtonShapeDB.Load("Interface\\ButtonTextures.ini", NeuronRuntime::Data);

    // four inlined ShapeDB finds (cmn\shapedb.h:0xf2 on a miss; the
    // last one through the routine, the same cached find).
    G_ButtonShapeMontage32 = G_ButtonShapeDB.Find("Montage32", kShapeDbHeaderLower);
    G_ButtonShapeMontage64 = G_ButtonShapeDB.Find("Montage64", kShapeDbHeaderLower);
    G_ButtonShapeTargetingElement =
        G_ButtonShapeDB.Find("HUD_targetingelement", kShapeDbHeaderLower);
    G_ButtonShapeTalkElement = G_ButtonShapeDB.Find("HUD_talkelement", kShapeDbHeaderLower);

    // .
    char Path[0x100];
    GetFileSysPath(Path, "interface\\ButtonMontage.ini", NeuronRuntime::Data);
    INI_Reader Reader;
    if (!Reader.open(Path, false) || !Reader.read_header())
        return;

    do
    {
        // per section.
        float Step = 0.0f;
        float U = 0.0f;
        float V = 0.0f;
        int TextureIndex = 0;
        int Size = 0;
        if (!Reader.is_header("Montage") || !Reader.read_value())
            continue;

        do
        {
            if (Reader.is_value("size"))
            {
                // 1 / (256 / size), 1 / 4 for a non-positive size.
                Size = Reader.get_value_int(0);
                int TilesPerRow = 4;
                if (Size > 0)
                    TilesPerRow = 0x100 / Size;
                Step = static_cast<float>(1.0 / static_cast<double>(TilesPerRow));
                U = 0.0f;
                V = 0.0f;
            }
            else if (Reader.is_value("texture"))
            {
                TextureIndex = Reader.get_value_int(0);
            }
            else if (Reader.is_value("num_pages"))
            {
                Reader.get_value_int(0);
            }
            else
            {
                // the shape the tile sits on.
                FTexShape* Shape = nullptr;
                if (TextureIndex == 2)
                {
                    Shape = G_ButtonShapeTargetingElement;
                }
                else if (TextureIndex == 3)
                {
                    Shape = G_ButtonShapeTalkElement;
                    Step = 0.5f;
                }
                else if (Size == 0x20)
                {
                    Shape = G_ButtonShapeMontage32;
                }
                else if (Size == 0x40)
                {
                    Shape = G_ButtonShapeMontage64;
                }

                // .
                float Scale = 0.0f;
                if (!Reader.is_value_empty(1))
                    Scale = Reader.get_value_float(1);

                // the routine -- insert one element at end().
                FButtonMontageTile Tile;
                Tile.Name = Reader.get_name_ptr();
                Tile.U = U;
                Tile.V = V;
                Tile.Step = Step;
                Tile.Shape = Shape;
                Tile.Scale = Scale;
                G_ButtonMontageTiles.push_back(Tile);

                // FST stores the rounded sum, FCOMP tests the unrounded
                // one against 0.99f (the field); only an ordered > moves to the next row.
                double const NextU = static_cast<double>(Step) + static_cast<double>(U);
                U = static_cast<float>(NextU);
                if (NextU > static_cast<double>(0.99f))
                {
                    U = 0.0f;
                    double const NextV = static_cast<double>(Step) + static_cast<double>(V);
                    V = static_cast<float>(NextV);
                    if (NextV > static_cast<double>(0.99f))
                        V = 0.0f;
                }
            }
        } while (Reader.read_value());
    } while (Reader.read_header());
}

FButtonMontageTile* FindButtonMontageTile(std::string Name)
{
    // .
    if (!G_bButtonMontageLoaded)
        LoadButtonMontage();

    int const Count = static_cast<int>(G_ButtonMontageTiles.size());
    for (int Index = 0; Index < Count; ++Index)
    {
        // NEURONCOMPONENTS stricmp(entry, Name); a null string buffer reads as "".
        if (_stricmp(G_ButtonMontageTiles[Index].Name.c_str(), Name.c_str()) == 0)
            return &G_ButtonMontageTiles[Index];
    }
    return nullptr;
}

void ClearButtonMontage()
{
    // erase(begin, end), then JMP the routine on the field.
    G_ButtonMontageTiles.clear();
    G_ButtonShapeDB.Clear();
}

bool NnMontageDrawTileAt(FButtonMontageTile const* Tile, float const* Position,
                         unsigned char const* Rgba, float Size, int Mode, float Angle)
{
    // . 0x00559904..0x0055991a: nothing without a tile or its shape.
    if (Tile == nullptr)
        return false;
    if (Tile->Shape == nullptr)
        return false;

    // frame 0 of the shape's texture record (handle, u0, v0, u1, v1).
    unsigned long Frame[5];
    NeuronRuntime::TextureLib->GetTextureFrame(Tile->Shape->Texture, 0, Frame);

    // the tile's square, inset by 1/256, mirrored per Mode.
    double const Inset = static_cast<double>(0.00390625f);
    float const Low0 = static_cast<float>(static_cast<double>(Tile->U) + Inset);
    float const Low1 = static_cast<float>(static_cast<double>(Tile->V) + Inset);
    float const High0 = static_cast<float>(
        static_cast<double>(Tile->Step) + static_cast<double>(Tile->U) - Inset);
    float const High1 = static_cast<float>(
        static_cast<double>(Tile->Step) + static_cast<double>(Tile->V) - Inset);
    float Rect[4];
    bool bRect = true;
    switch (Mode)
    {
    case 0:
        Rect[0] = Low0;
        Rect[1] = Low1;
        Rect[2] = High0;
        Rect[3] = High1;
        break;
    case 1:
        Rect[0] = High0;
        Rect[1] = Low1;
        Rect[2] = Low0;
        Rect[3] = High1;
        break;
    case 2:
        Rect[0] = Low0;
        Rect[1] = High1;
        Rect[2] = High0;
        Rect[3] = Low1;
        break;
    case 3:
        Rect[0] = High0;
        Rect[1] = High1;
        Rect[2] = Low0;
        Rect[3] = Low1;
        break;
    default:
        bRect = false;   // JA caseD_4: the frame's own rect stays
        break;
    }
    if (bRect)
        memcpy(&Frame[1], Rect, sizeof(Rect));

    // the "buttons" key, the batch node, the icon (the RGBA bytes as a
    // D3DCOLOR A:R:G:B).
    ScreenQuadSetMaterialKey(G_ButtonsMaterialKey);   // (the field)
    ScreenQuadBeginBatchFrame(Frame, 2, true);        // (&frame, 2, 1)
    unsigned long const Argb = (static_cast<unsigned long>(Rgba[3]) << 24) |
                               (static_cast<unsigned long>(Rgba[0]) << 16) |
                               (static_cast<unsigned long>(Rgba[1]) << 8) |
                               static_cast<unsigned long>(Rgba[2]);
    ScreenQuadEnqueueIcon(AsCamera(Win3DGetCamera()), Position, Argb, Size, Angle, 0);
    return true;
}

void NnMontageFlushButtons()
{
    // , listing-traced 0x00559c20..0x00559d13. Two stack savers on NeuronRuntime::Pipeline
    // (slot 61 read, slot 60 write; EH states 0 / 1): 0x17 -> 8, 0xe -> 0. the routine on the
    // "buttons" list the field with NeuronRuntime::Pipeline read again. Then 0xe restored, then 0x17
    // (0x00559cc7..0x00559cff). No null check anywhere; no material-key call -- the routine
    // finds the "buttons" container by its key.
    IRenderPipeline* const Pipe17 = NeuronRuntime::Pipeline;
    int Saved17 = 0;
    Pipe17->GetRenderState(D3DRS_ZFUNC, &Saved17);
    Pipe17->SetRenderState(D3DRS_ZFUNC, 8);
    IRenderPipeline* const Pipe0E = NeuronRuntime::Pipeline;
    int Saved0E = 0;
    Pipe0E->GetRenderState(D3DRS_ZWRITEENABLE, &Saved0E);
    Pipe0E->SetRenderState(D3DRS_ZWRITEENABLE, 0);
    ScreenQuadFlush(NeuronRuntime::Pipeline, G_ButtonsMaterialKey);
    Pipe0E->SetRenderState(D3DRS_ZWRITEENABLE, Saved0E);
    Pipe17->SetRenderState(D3DRS_ZFUNC, Saved17);
}
