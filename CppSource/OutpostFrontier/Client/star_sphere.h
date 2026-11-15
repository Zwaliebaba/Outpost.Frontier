// Author: Giperion.

#pragma once

// baseline SunEffect / StarSphere — OutpostFrontier.exe the routine (7 slots).
// Source path (embedded): Client\space\StarSphere.h. Epic: star-sphere.
// Distinct from TexturePanels (the branch) and from skybox "star_sphere" perf option.

// Layout from the routine ctor + the branch Load stores. Child at +0x14 is 0x30c
// (the routine); glow descriptors at +0x10/+0x0c are 0x50 (star_glow / star_center).

#include <cstddef>

// — write identity 3×3 into Out9 (nine floats, no translation).
void Win3DIdentityRotation3x3(float* Out9);

// (cdecl) — GWin3DModelview = ObjectOrNull ? view(vt+0x40, __stdcall) * Matrix12
// : Matrix12; pipeline state 7 set -> SetModelview, else SetView(view or identity) +
// SetWorld(Matrix12). ObjectOrNull is a camera (TexturePanelsDrawPiece 0050b6ba) or null
// (DrawClouds 005518c6, the UI tables).
void Win3DBindModelview(void* ObjectOrNull, float const* Matrix12);

// Star-glow CacheString-ish record (0x50). Shape name: length@+0, chars@+4 (0x20).
struct FStarGlowDesc
{
    unsigned ShapeLen;       // +0x00  the routine return length
    char ShapeName[0x20];    // +0x04
    float Scale;             // +0x24
    float InnerColor[3];     // +0x28
    float OuterColor[3];     // +0x34
    float Alpha;             // +0x40
    unsigned Field44;        // +0x44
    unsigned Field48;        // +0x48
    unsigned char Field4C;   // +0x4c
    unsigned char Pad4D[3];
};
static_assert(sizeof(FStarGlowDesc) == 0x50, "star_glow desc 0x50");

// SunEffect body at StarSphere+0x14 (operator_new 0x30c, the routine).
struct FSunEffectBody
{
    unsigned char Raw[0x30c];
};
static_assert(sizeof(FSunEffectBody) == 0x30c, "SunEffect body 0x30c");

// Lens-flare / spines child shell (0x40). Load parsers the routine / 0050fba0 fill
// the element ranges; draw (the routine / 00510160) walks [Begin, End).
struct FStarFxChild40
{
    unsigned char Raw[0x40];
};

struct IObjInspect;
struct FStarSphere;

// StarEffects::Star — seven thiscall virtual methods.
struct FStarSphere
{
    explicit FStarSphere(IObjInspect* InOwner);
    virtual ~FStarSphere();                     // +0x00 the routine / the routine
    virtual int Load(char const* IniPath, char const* Nickname); // +0x04
    virtual void UpdateIntensity(float Dt);                     // +0x08
    virtual void DrawSimple(void* Camera);                      // +0x0c
    virtual void Draw(void* Camera, float Dt);                   // +0x10
    virtual void DrawFlare(void* Camera, float Dt);              // +0x14
    virtual void DrawBillboard(void* Camera, float A, float B);   // +0x18

    IObjInspect* Owner;              // +0x04 borrowed solar IObjRW primary base
    float Radius;                    // +0x08  default 1000
    FStarGlowDesc* StarCenter;       // +0x0c
    FStarGlowDesc* StarGlow;         // +0x10
    FSunEffectBody* SunBody;         // +0x14  lens_glow target
    FStarFxChild40* LensFlare;       // +0x18
    FStarFxChild40* Spines;          // +0x1c
    float IntensityFadeIn;           // +0x20  default 1
    float IntensityFadeOut;          // +0x24  default 1
    float ZoneOcclusionFadeIn;       // +0x28  default 2
    float ZoneOcclusionFadeOut;      // +0x2c  default 2
    float Field30;                   // +0x30  pi
    float ZoneOcclusion;             // +0x34  approaches the routine
    float Field38;                   // +0x38  1 − the routine(SunBody)
    unsigned char Pad3C[0x24];       // +0x3c .. +0x5f
    float Dt;                        // +0x60  UpdateIntensity stores Dt (RET 4)
    float Intensity;                 // +0x64  default 1
    float WorldPos[3];               // +0x68 .. +0x70  owner position in VIEW space (Draw 0x511dc0)
    float Pad74;                     // +0x74
    float ScreenDepth;               // +0x78  Project OutZ
    float ScreenX;                   // +0x7c  Project OutX
    float ScreenY;                   // +0x80  Project OutY
    float ScreenZFlat;               // +0x84  forced 0 for 2D corona
    float PixelScale;                // +0x88  znear·xscale / depth
};
static_assert(offsetof(FStarSphere, SunBody) == 0x14, "SunBody +0x14");
static_assert(offsetof(FStarSphere, Dt) == 0x60, "Dt +0x60");
static_assert(offsetof(FStarSphere, Intensity) == 0x64, "Intensity +0x64");
static_assert(offsetof(FStarSphere, WorldPos) == 0x68, "WorldPos +0x68");
static_assert(offsetof(FStarSphere, PixelScale) == 0x88, "PixelScale +0x88");

// — SunEffect perf quality (the branch). Draw/DrawFlare gate on > 0.
extern float G_SunEffectQuality;

// — parse [star_glow] nickname into Desc from IniPath. Returns 0 on ok.
int StarGlowLoadFromIni(FStarGlowDesc* Desc, char const* IniPath, char const* Nickname);
// — parse [lens_glow] into SunBody. Returns 0 on ok.
int LensGlowLoadFromIni(FSunEffectBody* Body, char const* IniPath, char const* Nickname);
// — parse [lens_flare] into child at StarSphere+0x18. Returns 0 on ok.
int LensFlareLoadFromIni(FStarFxChild40* Flare, char const* IniPath, char const* Nickname);
// — parse [spines] into child at StarSphere+0x1c. Returns 0 on ok.
int SpinesLoadFromIni(FStarFxChild40* Spines, char const* IniPath, char const* Nickname);
// — bake corona ring positions/UVs after lens_glow load.
int SunBodyBakeCorona(FSunEffectBody* Body);

FSunEffectBody* SunEffectBodyInit(FSunEffectBody* Body);
// — solar\\StarArch.ini path + Pipeline block + shapes load.
void LoadSolarStarArchIni();
// — clear the StarArch ShapeDB (the field); called by the space teardown
// (DestroySpaceState, 0x0054afb6).
void ClearSolarStarArchShapes();


// The live star list, a std::list<FStarSphere*> at the field: allocator byte, head (the
// sentinel, the field), size (the field). Its static initializer the routine allocates the
// self-linked sentinel and registers the branch, which frees every node and the sentinel.
struct FStarSphereListNode
{
    FStarSphereListNode* Next; // +0x00
    FStarSphereListNode* Prev; // +0x04
    FStarSphere* Sphere;       // +0x08
};
struct FStarSphereList
{
    unsigned char AllocatorByte;   // +0x00 the field
    unsigned char Pad01[3];
    FStarSphereListNode* Sentinel; // +0x04 the field
    int Count;                     // +0x08 the field
};

// &the field. Walked by the cockpit env-fx passes (the routine / 0051c9e0)
// and the field draw (space_scene_obj_draw.cpp), and by the SunEffect perf apply the branch.
FStarSphereList* StarSphereList();
// (cdecl) -- new FStarSphere (0x8c, the routine with Owner), appended to the list;
// returns the list's last element. Caller: the solar ini reader the routine ("star").
FStarSphere* StarSphereCreateListed(IObjInspect* Owner);
// slot 0 (deleting destructor, flag 1) on every non-null element, then every
// node erased. Caller: StarSys leave the routine at 0x00554c12.
void StarSphereDestroyAll();
// (thiscall on the sphere, RET 4) -- the owner's (+4) IObjInspect slot 0
// get_position copied into *Out; returns Out. No tests. Callers: the Ship draw the branch
// (0x00546206) and the Solar radius slot the routine (0x00548125).
class Vector;
Vector* StarSphereGetOwnerPosition(FStarSphere const* Sphere, Vector* Out);
// (thiscall on the sphere) -- max(glow scale, centre scale or 0) * Radius; the glow
// is read with no test. Caller: the Solar radius slot the routine (0x00548144).
float StarSphereMaxGlowScale(FStarSphere const* Sphere);
