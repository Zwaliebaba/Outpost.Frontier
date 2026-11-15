// Author: Giperion.

#pragma once

// baseline Client\space\HUD.cpp (FDUMP path at .rdata 0x005d6dcc) -- the HUD element registry, the
// HUDItem base class every HUD panel derives from, the HUDFACTORY<T> self-registering factories,
// and HUD_Steer (the mouse-flight steering arrow).

// Globals (.data):
// / the field -- factory chain head / tail   (the branch zeroes them)
// / the field -- active HUDItem list head / tail (the branch)
// / the field -- the stored hud.ini path, length + char[0x104] (the branch)
// the HUD ShapeDB (G_HudShapeDB, texture_shapes.h)
// the [HUD] static_txm texture reference (static ctor the branch)
// the [HUD] static_blend int (initial value 2)
// HUD_Steer::Render's lazily acquired 3DMathEngine (the routine)

#include "client_mesh_load.h"
#include "common_goods.h"  // BaseWatcher
#include "common_tstring.h"

class INI_Reader;
struct IObjRW;
struct FSpaceCamera;
struct FTextureRef;

// -------------------------------------------------------------------------------------------
// HUDItem (RTTI .?AVHUDItem@@, vftable 0x005d6eac, 11 slots, 0x38 bytes).

// No out-of-line constructor exists: every derived constructor inlines it (e.g. 0x004c84d8 in
// HUD_Steer's factory, 0x004c8de3 in HUD_Button's). The inlined sequence is: +0x04 name length
// = 0, +0x08 name[0] = 0, the watcher at +0x28 built (BaseWatcher() + set(0)), the vptr, +0x34
// = 0, then an inlined SetTarget(0) (`if (watched) set_pointer(0)`), then +0x30 = 0.
// Destructor the routine (vptr back, watcher cleared); scalar deleting the routine.

// Slots (baseline byte offset; implementation in the HUDItem vftable):
//   0 +0x00 the routine  scalar deleting dtor
//   1 +0x04 the routine  XOR AL,AL / RET -- HUD_Steer answers its +0x38 byte (the branch)
//   2 +0x08 the routine  RET 4 -- broadcast by the routine from the HUD camera's slot 0x5c
//                          (0x0051cb9c), whose argument is the ViewRect it just applied
//   3 +0x0c the routine  MOV AL,1 / RET -- the routine keeps the element only when true
//   4 +0x10 the routine  RET -- the routine's first pass
//   5 +0x14 the routine  watch an IObjRW (broadcast by the routine from 0x0051cbea)
//   6 +0x18 the routine  RET 8 -- the routine (camera, frame time) from 0x0051dae6
//   7 +0x1c the routine  XOR AL,AL / RET 4 -- polled by the routine (camera slot 0x70)
//   8 +0x20 the routine  -- polled by the routine (camera slot 0x74)
//   9 +0x24 the routine
//  10 +0x28 the routine  RET
// Slots 7 and 8 receive the command record the routine hands the active camera (a 0x28-byte
// block whose first dword is the command id -- HUD_Button::MouseEvent 0x004c9430 builds one).
// -------------------------------------------------------------------------------------------

class HUDItem
{
public:
    HUDItem();

    virtual ~HUDItem();

    // slot 1 (+0x04) the routine
    virtual bool Slot1();
    // slot 2 (+0x08) the routine
    virtual void SetViewport(int const* ViewRect4);
    // slot 3 (+0x0c) the routine
    virtual bool Init();
    // slot 4 (+0x10) the routine
    virtual void Shutdown();
    // slot 5 (+0x14) the routine -- watch Target's Watchable (IObjRW + 8), or nothing
    virtual void SetTarget(IObjRW* Target);
    // slot 6 (+0x18) the routine
    virtual void Render(FSpaceCamera* Camera, float DeltaSeconds);
    // slots 7 / 8 / 9 (+0x1c / +0x20 / +0x24) the routine
    virtual bool Slot7(void* Command);
    virtual bool Slot8(void* Command);
    virtual bool Slot9(void* Command);
    // slot 10 (+0x28) the routine
    virtual void Slot10();

    // (thiscall) -- with a tool-tip id at +0x30, feed the ToolTip window
    // (the routine) with a rich-text copy of that string. HUD_Button::Update 0x004c941c calls it
    // while the button is hovered.
    void ShowToolTip();

    TString<32> Name;          // +0x04
    BaseWatcher Target;        // +0x28
    int ToolTipIds;            // +0x30 ("tool_tip_id" in the HUD_Button ini)
    HUDItem* Next;             // +0x34 active-list link

private:
    HUDItem(HUDItem const&) = delete;
    HUDItem& operator=(HUDItem const&) = delete;
};

static_assert(sizeof(HUDItem) == 0x38, "baseline HUDItem is 0x38 bytes");

// Transitional name kept while the HUD-A lane (hud_targetable_objects / hud_target) moves over.
using IHudElement = HUDItem;

// -------------------------------------------------------------------------------------------
// HUDItem_Factory (RTTI .?AVHUDItem_Factory@@, vftable 0x005d6e30: two slots, both
// `XOR EAX,EAX / RET 4`) and HUDFACTORY<T>.

// Layout: +0x00 vptr, +0x04 next in the factory chain, +0x08 TString<32> section name. The
// constructor is inlined into every HUDFACTORY<T> constructor (0x004cd270, 0x004d9990, ...):
// base vptr, +0x08 = 0, +0x0c = 0, +0x04 = 0, the name copied (<= 31 chars), the routine to
// append to the chain, then the derived vptr. The destructor is not virtual: the atexit thunks
// (the branch, the branch) call the branch directly, which puts the base vptr back and
// unlinks the factory (same body as the routine, inlined).
// -------------------------------------------------------------------------------------------

class HUDItem_Factory
{
public:
    explicit HUDItem_Factory(char const* SectionName);

    ~HUDItem_Factory();

    // slot 0 (+0x00) -- build the element for the [SectionName] section the reader sits on.
    // passes its INI_Reader by address.
    virtual HUDItem* Create(INI_Reader& Reader);
    // slot 1 (+0x04) the routine -- never overridden, never called through a traced site
    virtual HUDItem* Slot1(void* Arg);

    HUDItem_Factory* Next;     // +0x04
    TString<32> SectionName;   // +0x08

private:
    HUDItem_Factory(HUDItem_Factory const&) = delete;
    HUDItem_Factory& operator=(HUDItem_Factory const&) = delete;
};

// baseline HUDFACTORY<T>::Create (e.g. the routine for HUD_Maneuvers, the routine for
// HUD_Receiver): operator new(sizeof(T)), T(Reader), and the HUDItem face of the result -- which
// is not the object's address when HUDItem is not T's first base (HUD_Receiver: +0x7c).
template <class T>
class HUDFACTORY : public HUDItem_Factory
{
public:
    explicit HUDFACTORY(char const* SectionName) : HUDItem_Factory(SectionName) {}

    HUDItem* Create(INI_Reader& Reader) override
    {
        T* const Element = new T(Reader);
        return Element;
    }
};

// Transitional name kept while the HUD-A lane moves over (see IHudElement above).
using HudElementFactoryBase = HUDItem_Factory;

// -------------------------------------------------------------------------------------------
// HUD_Steer (RTTI .?AUHUD_Steer@@ : HUDItem, vftable 0x005d6e7c, 0x74 bytes).
// Built by UHUD_Steer::HUDFACTORY::Create the routine (the constructor is inlined there),
// destroyed by the routine (scalar deleting the routine). Its factory is the static object
// (ctor the routine from the XC entry the branch, "Steer").
// Overrides: 0 the routine, 1 the branch (the +0x38 byte), 6 the branch Render.
// -------------------------------------------------------------------------------------------

struct HUD_Steer : public HUDItem
{
    // 's inlined constructor
    explicit HUD_Steer(INI_Reader& Reader);

    ~HUD_Steer() override;

    // MOV AL,[ECX+0x38]
    bool Slot1() override;

    void Render(FSpaceCamera* Camera, float DeltaSeconds) override;

    unsigned char Byte38;          // +0x38 -- the ctor clears it; nothing in the PE sets it
    unsigned char Pad39[3];
    float Radius;                  // +0x3c "radius", 40.0f
    float Range;                   // +0x40 "range", 200.0f
    float Field44;                 // +0x44 0.2f, never read
    unsigned char Byte48;          // +0x48 cleared, never read
    unsigned char Pad49[3];
    FClientMeshHolder MeshHolder;  // +0x4c interface\hud\HUD_steeringarrow.3db
};

static_assert(sizeof(HUD_Steer) == 0x74, "baseline HUD_Steer is 0x74 bytes");

// UHUD_Steer::HUDFACTORY: the factory whose Create builds HUD_Steer. baseline
// keeps a separate out-of-line copy of the base constructor for it (with the chain append
// inlined) instead of the shared inlined form; the effect is the same.
class HUD_SteerFactory : public HUDItem_Factory
{
public:
    explicit HUD_SteerFactory(char const* SectionName) : HUDItem_Factory(SectionName) {}

    HUDItem* Create(INI_Reader& Reader) override;
};

// -------------------------------------------------------------------------------------------
// Registry functions.
// -------------------------------------------------------------------------------------------

// zero the factory-chain head/tail.
void HudElementFactoryListInit();
// append to the factory chain's tail.
void HudElementFactoryListAdd(HUDItem_Factory* Factory);
// unlink a factory from the chain (no-op if not present).
void HudElementFactoryListRemove(HUDItem_Factory* Factory);

// zero the active-element list head/tail.
void HudElementListInit();
// (cdecl) -- HUDItem::SetTarget(Target) on every active element.
void HudElementListSetTarget(IObjRW* Target);
// (cdecl) -- HUDItem::SetViewport(ViewRect4) on every active element.
void HudElementListSetViewport(int const* ViewRect4);
// (cdecl) -- IMaterialBatcher slot 8 (bin 1, -1), HUDItem::Render(Camera,
// DeltaSeconds) on every active element, slot 8 (1, 1). Its one caller, 0x0051dae6 in
// (the HUD camera's draw), pushes the frame time (the routine) then the camera.
void HudElementListRender(FSpaceCamera* Camera, float DeltaSeconds);
// (cdecl) -- true at the first active element whose Slot7(Command) is true.
bool HudElementListAnySlot7(void* Command);
// (cdecl) -- the same over Slot8.
bool HudElementListAnySlot8(void* Command);
// release the static_txm texture (the field, TextureLib slot +0x38),
// Shutdown() on every element, delete every element, empty the list, then tail-jump into
// on the HUD ShapeDB. Caller: the space teardown 0x0054af1d.
void HudElementListShutdown();

// ProgressCB's `HUD = ...` key stores the resolved path (cap 0x103).
void SetHudIniPath(char const* Path);
// The path last stored by SetHudIniPath (the field).
char const* GetHudIniPath();

// open the stored hud.ini; [HUD] loads shapes / static_txm / static_blend,
// every other section is offered to each factory in chain order until one builds an element;
// an element whose Init() is false is deleted, a section nobody builds is FDUMPed. Then the
// mouse-flight state is reset (0x004c75c5 .. 0x004c75ee). Caller: the space init 0x0054ad62.
void LoadHudElementsFromIni();

// / the field -- read by the comm window's static-noise draw (the routine).
FTextureRef* GetHudStaticTexture();
int GetHudStaticBlend();

// (cdecl Out, In, Length) -- Out = In * (1 / Length); callers besides HUD_Steer at
// , 0x005809cc, 0x00580c7b.
void HudVec3ScaleByInverse(float* Out, float const* In, float Length);

// The three rotation helpers on a row-major 3x3 matrix (baseline thiscall on the matrix, RET 4);
// callers besides HUD_Steer: 0x00583fa7 .. 0x005848xx, 0x0059b196, 0x004f4249.
// rotation about Y: [ cos 0 sin ; 0 1 0 ; -sin 0 cos ].
void HudMatrixRotationY(float* M, float Angle);
// rows 1 and 2 turned column by column:
//   row1 = row1 * cos - sin * row2;  row2 = row1 * sin + cos * row2.
void HudMatrixRotateRows12(float* M, float Angle);
// rows 0 and 1 turned the same way.
void HudMatrixRotateRows01(float* M, float Angle);

// (cdecl X, Y) -- the angle of (X, Y) measured from the +Y axis: 0 for (0, 0);
// with |X| > |Y| it is atan(Y / |X|) -/+ pi/2, otherwise atan(-X / |Y|), shifted by +/- pi
// when Y < 0 (sign chosen by the sign of that arctangent).
float HudVectorAngle(float X, float Y);
