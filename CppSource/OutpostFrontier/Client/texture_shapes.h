// Author: Giperion.

#pragma once

// Texture-shape library -- Client\cmn\ShapeDB.cpp and cmn\ShapeDB.h.

// A "shape" is a named sub-rectangle of a texture: the UV rect plus a centred scale/offset pair
// that the FX and UI code multiplies into its quads. baseline keeps shapes in ShapeDB objects --
// a std::map<TString<24>, shape> compared with _stricmp, plus a last-found cache -- and there
// are several of them, each filled by the routine from its own INI:

// mouse.ini                        (the routine; cleared 0041e727)
// [HUD] shapes =                   (the routine; cleared by the routine)
// every [Data] effect_shapes =     (the routine; cleared by the routine)
// solar\StarArch.ini               (the routine; cleared 00511010)
// the system's [TexturePanels]     (00553e49; cleared 00554c63)
// Interface\ButtonTextures.ini     (00559df6; cleared 00559d9c)
// Interface\UI\UITextures.ini      (the routine; cleared by the branch)
//   nebula +0x04, TexturePanels +0x04, asteroid field +0xe0, nav map +0x880 (per object)

// The INI understands two section types:

//   [Texture]                       [Shape]
//   file        = fx\efx.txm        name       = <shape>
//   name        = <texture>         shape_name = <shape>
//   texture_name= <texture>         x / y / w / h = <float>
//   shape_name  = <shape>           upper_left  = u, v
//   tex_shape   = <texture+shape>   lower_right = u, v   (each +1)
//   dim         = x,y,w,h[,ul,lr]   dim = x,y,w,h,ulu,ulv,lru,lrv

#include "common_tstring.h"

#include <map>
#include <stddef.h>

struct IFileSystem;

// baseline's texture reference, 8 bytes: the ITextureLibrary2 record Slot12 hands out plus the
// alpha-probe cache the routine fills. Its default constructor is the static initializer
// (`[+4] = -1`, `[+0] = 0`), its destructor the routine / the branch (release
// through slot +0x38), and its copy constructor the routine (copy the record, take a
// reference with slot +0x34, copy +4). baseline declares no assignment.
struct FTextureRef
{
    void* Texture; // +0x00 ITextureLibrary2 texture record* (Slot12 OutTex)
    int Field04;   // +0x04 the routine cache (-1 = unprobed; 0/1 = has-alpha)

    FTextureRef();
    FTextureRef(FTextureRef const& Other);
    ~FTextureRef();
    FTextureRef& operator=(FTextureRef const& Other) = delete;
};

// The shape record, 0x30 bytes: the texture reference at +0, then what the routine stores
// (`ESI + 0x08 ... 0x2c`). Its copy is FTextureRef's copy plus a member-wise copy of the rest
// (the MOV chain at 00426e76).
struct FTexShape : FTextureRef
{
    float U0;      // +0x08
    float V0;      // +0x0c
    float U1;      // +0x10
    float V1;      // +0x14
    int PixelW;    // +0x18 ftol(w)
    int PixelH;    // +0x1c ftol(h)
    float ScaleU;  // +0x20
    float ScaleV;  // +0x24
    float OffsetU; // +0x28
    float OffsetV; // +0x2c
};

static_assert(sizeof(FTextureRef) == 0x08, "baseline texture reference is 8 bytes");
static_assert(offsetof(FTexShape, U0) == 0x08, "shape U0 at +0x08");
static_assert(offsetof(FTexShape, PixelW) == 0x18, "shape pixel width at +0x18");
static_assert(offsetof(FTexShape, ScaleU) == 0x20, "shape scale at +0x20");
static_assert(sizeof(FTexShape) == 0x30, "baseline shape record is 0x30 bytes");

// release the current record, then look Name up through Slot12 with its CRC32;
// FDUMP an error when it is not defined. Returns whether a record is now held.
bool TextureRefLoad(FTextureRef* Ref, char const* Name);

// swap the record a reference holds: release the old one, store the new one and
// take a reference on it (slot +0x34).
void TexShapeSetTexture(FTextureRef* Ref, void* Texture);

// TextureLib Slot22(Ref->Texture, Frame, out5); returns out5[0], the GPU
// handle, or null when no record is held.
void* TexShapeGetGpuHandle(FTextureRef const* Ref, int Frame = 0);

// recompute the UV rect and the centred scale/offset from a pixel (or
// normalised) rect plus an optional explicit upper-left / lower-right pair. Negative
// upper-left / lower-right components mean "derive from the rect".
void TexShapeSetRect(FTexShape* Shape, float X, float Y, float W, float H, float UpperLeftU,
                     float UpperLeftV, float LowerRightU, float LowerRightV);

// The three spellings of the header path the inlined ShapeDB::Find FDUMPs with.
extern char const kShapeDbHeaderFull[];  // s_E:\FL\...\cmn\ShapeDB.h (005c95c8)
extern char const kShapeDbHeaderLower[]; // s_cmn\shapedb.h (005d3c00)
extern char const kShapeDbHeader[];      // s_cmn\ShapeDB.h (005da4a0)

// The map comparator: baseline's lower_bound (the routine) and key test (the routine) both call
// MSVCRT _stricmp on the key text.
struct FShapeKeyLess
{
    bool operator()(TString<24> const& A, TString<24> const& B) const;
};

class FShapeDB
{
public:
    typedef std::map<TString<24>, FTexShape, FShapeKeyLess> FShapeMap;

    FShapeDB();
    FShapeDB(FShapeDB const& Other) = delete;
    FShapeDB& operator=(FShapeDB const& Other) = delete;

    // __thiscall, RET 8. Loads one shapes INI (resolved against BaseDir when it
    // is given) and returns the number of shapes the DB now holds.
    int Load(char const* Path, IFileSystem* BaseDir);

    // release every shape's record, empty the map, reset the last-found cache.
    void Clear();

    // the shape under Key, inserted with a null record when it is missing.
    // Does not touch the last-found cache.
    FTexShape* FindOrCreate(TString<24> const& Key);

    // The ShapeDB.h:0xf2 find every caller inlines: TString<24> key (the routine), cache test
    // (the routine), else lower_bound (the routine) and store the cache; a miss FDUMPs a
    // warning with SourceFile and returns null.
    FTexShape* Find(char const* Name, char const* SourceFile = kShapeDbHeader);

    // baseline reads `[this + 0x10]`, the map size.
    unsigned Count() const;

private:
    FShapeMap Shapes;
    // baseline +0x14, an iterator whose end() means "no cache"; null plays end() here.
    FShapeMap::value_type* LastFound;
};

// Owners whose layout is still a raw byte image keep the DB in the bytes baseline gives it
// (nebula +0x04, TexturePanels +0x04, asteroid field +0xe0; baseline's ShapeDB is 0x18 bytes) and
// construct / destroy it in place, where baseline's own constructor and destructor do.
int const kShapeDbStorageBytes = 0x18;
FShapeDB* ShapeDBConstructAt(void* Storage);
void ShapeDBDestructAt(void* Storage);
FShapeDB* ShapeDBAt(void* Storage);

// The global ShapeDB instances (see the table at the top).
extern FShapeDB G_MouseShapeDB;
extern FShapeDB G_HudShapeDB;
extern FShapeDB G_EffectShapeDB;
extern FShapeDB G_StarSphereShapeDB;
extern FShapeDB G_UiShapeDB;

// queue a `[Data] effect_shapes =` path (Common StringAlloc'd) for the drain.
void AddEffectShapesFile(char const* Path);
// empty that queue (client teardown 0x005b2290), strings not freed.
void ClearEffectShapesFiles();
// load every queued file into G_EffectShapeDB, then create the FLFxMeshFactory
// (fl_fx_mesh.h, the field) and hand it to IFxRuntime::BindMeshFactory. Returns the last
// Load result.
int WarmUpEffectShapes();
