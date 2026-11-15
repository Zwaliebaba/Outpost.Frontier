// Author: Giperion.

#pragma once

// baseline WinControl (RTTI .?AUWinControl@@ : Win3D, IDataViewOwner) -- the mesh-backed Win3D
// every NeuroNet control, UIScroll, BaseRichTextWin / RichTextWin and most HUD panels derive
// from. Constructed by the routine, destroyed by the routine (scalar deleting the routine),
// built by WINFACTORY<WinControl>::Create the routine (operator_new(0x32c)).

// vftable 0x005e5e44 (45 slots): slots 0..27 are Win3D's (win3d.h) with these overrides --
//   0 the routine dtor, 2 the routine (returns this), 4 the routine HitTest,
//   5 the routine SetPosition, 7 the routine SetOrientation, 10 the routine Destroy,
//   13 the routine Draw, 19 the routine MouseEvent, 23 the routine Slot23
// -- and slots 28..44 are WinControl's own (declared below).
// The IDataViewOwner face sits at +0x7c (vftable 0x005e5e38).

// Layout (the routine):
//   +0x000 Win3D (0x7c)
//   +0x07c IDataViewOwner vptr
//   +0x080 FClientMeshHolder (0x28): holder ctor the routine, +0x90 anim = -1,
//          +0x94..+0xa4 = 1.0f; the holder's own vftable 0x005d2efc is not modelled
//   +0x0a8 offset[3]            ("offset" in the BaseFrame ini; added to +0x60 for the mesh)
//   +0x0b4 half extents[3]      ("mouse_size"; the box the routine tests the ray against)
//   +0x0c0 hit offset[3]        (added to the hit box position by the routine)
//   +0x0cc tooltip id           ("tooltip"; slot 34 / the routine)
//   +0x0d0 char[0x100] mesh path (slot 38)
//   +0x1d0 char[0x100] hardpoint name (slot 35 attaches to the parent's hardpoint of this name)
//   +0x2d0 bCaptured, +0x2d1 no_z_enable, +0x2d2 (property 9), +0x2d3 identity hit orientation
//   +0x2e0 {0, 0, 10000.0f}, +0x2ec 0, +0x2f0 0 -- written by the ctor, read by subclasses
//   +0x2f4 flag byte: bit0 = add +0xa8 to the instance position (ctor sets it)
//   +0x2f8 attach orientation[9], +0x31c attach position[3], +0x328 bAttached
//   size 0x32c

#include "client_mesh_load.h"
#include "win3d.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

class RichTextWin;
class RichTextEditWin;
class UiScroll;

// RTTI .?AUIDataViewOwner@@. The base vftable 0x005e5efc holds two _purecall entries; every
// derived face seen so far (WinControl 0x005e5e38) fills them with the branch (RET 8) and
// (RET 4). Names unknown.
struct IDataViewOwner
{
    virtual void DataViewSlot0(unsigned A, unsigned B) = 0; // +0x00
    virtual void DataViewSlot1(unsigned A) = 0;             // +0x04
};

class WinControl : public Win3D, public IDataViewOwner
{
public:

    WinControl();

    ~WinControl() override;

    // --- Win3D overrides ---
    // slot 2 the routine -- MOV EAX,ECX / RET
    WinControl* AsWinControl() override;
    // slot 4 the routine
    bool HitTest(float const* Ray) override;
    // slot 5 the routine
    void SetPosition(float const* NewPosition) override;
    // slot 7 the routine
    void SetOrientation(float const* NewOrientation) override;
    // slot 10 the routine
    void Destroy() override;
    // slot 13 the routine
    void Draw() override;
    // slot 19 the routine
    bool MouseEvent(FWin3DMouseEvent const* Event) override;
    // slot 23 the routine -- 0xef01 show / 0xef02 hide
    bool Slot23(unsigned Msg, unsigned A, unsigned B) override;

    // --- IDataViewOwner (vftable 0x005e5e38) ---
    // RET 8
    void DataViewSlot0(unsigned A, unsigned B) override;
    // RET 4
    void DataViewSlot1(unsigned A) override;

    // --- WinControl slots 28..44 ---
    // slot 28 (+0x70) the branch -- read the [BaseFrame] ini at Path, then build every other
    // section's class_name through the factory with this as the parent
    virtual void LoadIni(char const* Path);
    // slots 29 / 30 (+0x74 / +0x78) the routine -- RET 4. Slot 29's argument is a FILE*: the
    // one override seen, NavBar's the routine, fprintf's its [NavBar] section into it (the
    // counterpart of slot 28 LoadIni). Name not recovered.
    virtual void Slot29(FILE* File);
    virtual void Slot30(unsigned A);
    // slot 31 (+0x7c) the routine
    virtual void SetOffset(float X, float Y, float Z);
    // slot 32 (+0x80) the routine
    virtual void CaptureMouse();
    // slot 33 (+0x84) the routine
    virtual void ReleaseMouse();
    // slot 34 (+0x88) the routine -- feed the ToolTip with this control's tooltip id
    virtual void ShowToolTip();
    // slot 35 (+0x8c) the routine
    virtual void RefreshAttachment();
    // slot 36 (+0x90) the routine
    virtual bool GetHardpointTransform(char const* Name, float* OutTransform12);
    // slot 37 (+0x94) the routine
    virtual WinControl* FindChild(char const* Name);
    // slot 38 (+0x98) the routine -- MaterialPath is the routine's second argument.
    virtual void SetMesh(char const* Path, char const* MaterialPath);
    // slot 39 (+0x9c) the routine -- RET; Draw's tail call
    virtual void Slot39();
    // slot 40 (+0xa0) the routine
    virtual void GetAttachOrientation(float* OutOrientation9);
    // slot 41 (+0xa4) the routine
    virtual void GetAttachPosition(float* OutPosition3);
    // slot 42 (+0xa8) the routine -- property switch 1..11 (see wincontrol_base.cpp). A and B
    // are dwords that are an int, a flag or a pointer depending on Msg.
    virtual intptr_t SetProperty(int Msg, uintptr_t A, uintptr_t B);
    // slot 43 (+0xac) the routine
    virtual void AddAttachOffset(float const* Delta);
    // slot 44 (+0xb0) the routine
    virtual bool NameEquals(char const* OtherName);

    // (thiscall) -- with "DRAW STATIC CONTROLS" on and an instance bound, submit
    // the +0x80 instance with depth test off, tinted by the field. The head of Draw
    // (the routine inlines it); BaseRichTextWin::Draw (0x0057ae1f) and two HUD draws
    // (0x00492a2c, 0x004d573e) call it directly.
    void DrawStaticMesh();

    // (thiscall, RET 4) -- every child that answers AsWinControl (slot 2) gets
    // SetProperty(1, bHide, 0); then this control's own Flags: bHide clears bits 0..1, otherwise
    // sets them. Callers: HUD_Target 0x004e2062 / 0x004e3621.
    void SetHiddenWithChildren(bool bHide);

    // --- Child construction (wincontrol_children.cpp) ---
    // Each is thiscall on the parent: the routine(Name, <class>, this, 0), then -- when the
    // factory returned an object -- that control's Init with the remaining arguments in order.
    // The return value is the routine's result (null when the factory failed).

    // (RET 0x3c) -- "ButtonControl" + ButtonControl::Init the routine. The Init
    // arguments land at +0xd0 (mesh), +0x350 (string), +0x360 (sound, slot at +0x374), the
    // key lookup (+0x34c / +0xcc), +0xa8 (position), +0x1d0 (hardpoint), +0xb4
    // (half extents), +0x331 bit 0, +0x330 bits 3 and 5, +0x2d1, +0xcc, +0x32c and +0x34c.
    WinControl* CreateButtonControl(char const* Name, char const* MeshPath, char const* String350,
                                    char const* SoundName, char const* KeyCommandName,
                                    float const* Position, char const* Hardpoint,
                                    float const* HalfExtents, unsigned char bFlag331,
                                    unsigned char bFlag330Bit3, unsigned char bFlag330Bit5,
                                    unsigned char bNoZEnable, int ToolTipIds, int Dword32c,
                                    int Dword34c);
    // (RET 0x24) -- "WireButtonControl" + WireButtonControl::Init the routine.
    WinControl* CreateWireButtonControl(char const* Name, char const* SoundName,
                                        float const* Position, float const* HalfExtents,
                                        unsigned char bFlag3, unsigned char bNoZEnable,
                                        int ToolTipIds, int CommandId, int WireFlags);
    // (RET 0x1c) -- "WireStaticControl" + WireStaticControl::Init the routine.
    WinControl* CreateWireStaticControl(char const* Name, float const* Position,
                                        float const* HalfExtents, unsigned char bFlag3,
                                        unsigned char bNoZEnable, int ToolTipIds, int WireFlags);
    // (RET 0x14) -- "RichTextWin", then SetNormSize (the routine), SetAttachOffset
    // (the routine) and SetAutoScrollbar (the routine). Unlike the others it does not test
    // the factory's result before configuring it.
    // (RET 0x20) -- "UIScroll" with the FUiPayloadScrollCycle {Width, Height,
    // float(Min), float(Max), float(Page), 1.0f, bHorizontal}; on a non-null result
    // SetScrollOffset(Position) the routine, SetValue(0) the routine and SetHorizontal
    // . Caller: HUD_Status::Create 0x004daf10 ("WeaponListScroll").
    UiScroll* CreateUIScroll(char const* Name, float const* Position, float Width, float Height,
                             int Min, int Max, int Page, unsigned char bHorizontal);
    // (RET 0x20) -- byte for byte the routine with the class name "UICycle"
    // (0x005e53f4). Caller: NN_Preferences::Create 0x004aa7ab ("video_mode_cycle").
    UiScroll* CreateUICycle(char const* Name, float const* Position, float Width, float Height,
                            int Min, int Max, int Page, unsigned char bHorizontal);
    RichTextWin* CreateRichTextWin(char const* Name, float Width, float Height,
                                   float const* AttachOffset, bool bAutoScrollbar);
    // (RET 0x14) -- "UITextEdit" with the FUiPayloadText {Width, Height, Caption};
    // on a non-null result SetAttachOffset (the routine). Caller: NN_LoadSave::Create
    // ("LoadSave_Edit2").
    RichTextEditWin* CreateUiTextEdit(char const* Name, float const* AttachOffset, float Width,
                                      float Height, wchar_t* Caption);
    // (RET 0x28) -- "RichTextWireButtonControl" + the routine.
    WinControl* CreateRichTextWireButtonControl(char const* Name, char const* SoundName,
                                                float const* Position, float const* HalfExtents,
                                                unsigned char bFlag3, unsigned char bNoZEnable,
                                                int ToolTipIds, unsigned CaptionIds,
                                                int CommandId, int WireFlags);
    // (RET 0x40) -- "StaticControl" + StaticControl::Init the routine.
    WinControl* CreateStaticControl(char const* Name, char const* MeshPath, float const* Position,
                                    char const* Hardpoint, float const* HalfExtents,
                                    unsigned char bNoZEnable, unsigned char Byte34c,
                                    unsigned char Byte344, unsigned char Byte345,
                                    unsigned char Byte346, unsigned char Byte34b,
                                    unsigned char Byte34a, int Int418, int Int41c,
                                    unsigned char Byte349, float const* Vector338);
    // (RET 0x2c) -- "TextControl" + TextControl::Init the routine.
    WinControl* CreateTextControl(char const* Name, char const* StyleName, float const* Position,
                                  char const* Hardpoint, float Width, float Height, float Arg6,
                                  unsigned Ids, int Dword358, int Dword344,
                                  unsigned char Byte378);
    // (RET 0x24) -- "CargoBarControl" + CargoBarControl::Init the routine
    // (cargo_bar_control.cpp). Color / HighlightColor are RGBA byte quads in a dword.
    WinControl* CreateCargoBarControl(char const* Name, char const* MeshPath,
                                      float const* NewOffset, char const* Hardpoint,
                                      float const* NewHalfExtents, unsigned char bHorizontal,
                                      int ToolTip2Ids, unsigned Color, unsigned HighlightColor);
    // (RET 0x28) -- "ShipViewControl" + ShipViewControl::Init the routine
    // (ship_view_control.cpp). RectColor / WireColor point at RGBA byte quads.
    WinControl* CreateShipViewControl(char const* Name, float const* RotSpeed,
                                      float const* NewOffset, float const* Ypr,
                                      float const* NewViewSize, char const* Hardpoint,
                                      bool bShowRect, bool bShowWire, unsigned const* RectColor,
                                      unsigned const* WireColor);
    // (RET 0x28) -- "EditControl" + the routine (TextControl::Init) with the
    // helper's arguments 2..10 and a trailing 0 for Init's last (byte +0x378) argument.
    WinControl* CreateEditControl(char const* Name, char const* StyleName, float const* Position,
                                  char const* Hardpoint, float Width, float Height, float Arg6,
                                  unsigned Ids, int Dword358, int Dword344);
    // (RET 0x38) -- "ToggleButtonControl" + ToggleButtonControl::Init the routine
    // with arguments 2..14. That Init sets byte +0x335 = 1, runs ButtonControl::Init
    // (MeshPath, AltMeshPath, SoundName, KeyCommandName, Position, Hardpoint,
    // HalfExtents, bFlag331, bFlag330Bit3, bFlag330Bit5, bNoZEnable, ToolTipIds, Dword32c, 0)
    // and loads AltMeshPath into a second mesh holder at +0x378.
    WinControl* CreateToggleButtonControl(char const* Name, char const* MeshPath,
                                          char const* AltMeshPath, char const* SoundName,
                                          char const* KeyCommandName, float const* Position,
                                          char const* Hardpoint, float const* HalfExtents,
                                          unsigned char bFlag331, unsigned char bFlag330Bit3,
                                          unsigned char bFlag330Bit5, unsigned char bNoZEnable,
                                          int ToolTipIds, int Dword32c);
    // (RET 0x70) -- "ScrollControl" + ScrollControl::Init the routine
    // (scroll_control.cpp) with the other 27 arguments. Callers: 0x004bf4ec, 0x004c1093.
    WinControl* CreateScrollControl(
        char const* Name, char const* BackMesh, float const* BackOffset, char const* BackHp,
        float const* BackMouse, bool bBackNoMeshRender, char const* NextMesh,
        float const* NextOffset, char const* NextHp, float const* NextMouse,
        bool bNextNoMeshRender, char const* PrevMesh, float const* PrevOffset,
        char const* PrevHp, float const* PrevMouse, bool bPrevNoMeshRender,
        char const* ThumbMesh, float const* ThumbOffset0, float const* ThumbOffset1,
        char const* ThumbHp0, char const* ThumbHp1, float const* ThumbMouse,
        bool bThumbNoMeshRender, int MinRange, int MaxRange, int PageSize,
        bool bSliderBehavior, bool bNoZEnableArg);

    FClientMeshHolder MeshHolder;   // +0x080
    float Offset[3];                // +0x0a8
    float HalfExtents[3];           // +0x0b4
    float HitOffset[3];             // +0x0c0
    int ToolTipIds;                 // +0x0cc
    char MeshPath[0x100];           // +0x0d0
    char HardpointName[0x100];      // +0x1d0
    unsigned char bCaptured;        // +0x2d0
    unsigned char bNoZEnable;       // +0x2d1
    unsigned char Field2d2;         // +0x2d2 -- property 9
    unsigned char bIdentityHitOrientation; // +0x2d3
    // +0x2d4 -- not written by the ctor and never read by WinControl itself: the HUD panels park
    // a child's rest Offset here (HUD_Target 0x004e0ee0.., HUD_ContactList 0x004d38e7) and hand
    // it to ButtonMover as the slide start (0x0045ce55, 0x004e4188).
    float HomeOffset[3];
    float Field2e0[3];              // +0x2e0 -- ctor {0, 0, 10000.0f}
    int Field2ec;                   // +0x2ec -- ctor 0
    int Field2f0;                   // +0x2f0 -- ctor 0
    unsigned char PositionFlags;    // +0x2f4 -- bit0: instance position includes +0xa8
    unsigned char Pad2f5[3];        // +0x2f5
    float AttachOrientation[9];     // +0x2f8
    float AttachPosition[3];        // +0x31c
    unsigned char bAttached;        // +0x328
    unsigned char Pad329[3];        // +0x329

private:
    WinControl(WinControl const&) = delete;
    WinControl& operator=(WinControl const&) = delete;
};

static_assert(sizeof(WinControl) == 0x32c, "baseline WinControl is 0x32c bytes");
static_assert(offsetof(WinControl, MeshHolder) == 0x80, "WinControl +0x80 mesh holder");
static_assert(offsetof(WinControl, Offset) == 0xa8, "WinControl +0xa8 offset");
static_assert(offsetof(WinControl, HalfExtents) == 0xb4, "WinControl +0xb4 half extents");
static_assert(offsetof(WinControl, HitOffset) == 0xc0, "WinControl +0xc0 hit offset");
static_assert(offsetof(WinControl, ToolTipIds) == 0xcc, "WinControl +0xcc tooltip");
static_assert(offsetof(WinControl, MeshPath) == 0xd0, "WinControl +0xd0 mesh path");
static_assert(offsetof(WinControl, HardpointName) == 0x1d0, "WinControl +0x1d0 hardpoint");
static_assert(offsetof(WinControl, bCaptured) == 0x2d0, "WinControl +0x2d0 captured");
static_assert(offsetof(WinControl, Field2e0) == 0x2e0, "WinControl +0x2e0");
static_assert(offsetof(WinControl, PositionFlags) == 0x2f4, "WinControl +0x2f4 flags");
static_assert(offsetof(WinControl, AttachOrientation) == 0x2f8, "WinControl +0x2f8 attach");
static_assert(offsetof(WinControl, AttachPosition) == 0x31c, "WinControl +0x31c attach pos");
static_assert(offsetof(WinControl, bAttached) == 0x328, "WinControl +0x328 attached");

// (thiscall on HalfExtents, RET 4; wincontrol_children.cpp) -- the mesh's box via
// ; on success HalfExtents = {box[0], box[2], 0}. Callers pass a control's +0xb4
// and its +0x80 holder (NN_Inventory 0x004782b1 and nine other windows).
bool WinControlHalfExtentsFromMesh(float* HalfExtents, FClientMeshHolder* Holder);
