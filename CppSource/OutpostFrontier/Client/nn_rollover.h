// Author: Giperion.

#pragma once

// baseline Rollover (RTTI .?AURollover@@ : TextWin, vftable 0x005d5454, 29 slots, size 0x18c).
// Built by WINFACTORY<Rollover>::Create the routine (the factory is the field, made by the
// static initializer the branch -> the routine) and kept in the singleton the field.
// Pre-init the routine creates it through the routine("Rollover", "Rollover", 0, 0).

// vftable 0x005d5454 against TextWin (0x005d54cc): 0 the routine (dtor the routine),
// 4 the routine (Win3D's, not TextWin's HitTest), 9 the routine Create, 13 the branch Draw,
// 23 the branch. Everything else is TextWin's / Win3D's.

// Layout past TextWin (0xec):
//   +0x0ec the shown hover record (FToolTipHoverInfo, 0x64; its dtor is the routine)
//   +0x150 the RolloverTable map (VC6 std::map<int, int>, ctor the routine, filled by
// from interface\Rollover.ini, key = a hover's IdsAlt, value = the ids shown)
//   +0x164 hold time left, +0x16c anchor (3 floats, the SetPosition argument),
//   +0x178 "was visible" latch, +0x17c hidden grace time, +0x180 cursor-still time,
//   +0x184 anchor X outside the intro menu (-0.51f), +0x188 anchor X in the intro menu (0.2f).
// The port holds the map as std::map<int, int>, so the offsets past +0x150 are not baseline's.

#include "text_win.h"
#include "tooltip_hover.h"
#include "win_factory.h"

#include <map>

class NnRolloverControl : public TextWin
{
public:
    // 's inlined part: the TextWin part (TextWin()), the +0xec record
    // (+0xf4 / +0xf8 zeroed, the routine on +0xfc, the routine), the map (the routine),
    // then the routine.
    NnRolloverControl();
    // .
    ~NnRolloverControl() override;

    // slot 4 the routine -- XOR AL,AL / RET 4.
    bool HitTest(float const* Ray) override;
    // slot 9 the routine.
    bool Create(Win3D* NewParent, void* Data) override;
    // slot 13 the branch.
    void Draw() override;
    // slot 23 the branch -- message 0xef02 hides the rollover.
    bool Slot23(unsigned Msg, unsigned A, unsigned B) override;

    // the post-vtable part of the factory constructor.
    void InitRolloverDefaults();

    // interface\Rollover.ini [RolloverTable] map = key, value into +0x150.
    void LoadRolloverTable();

    // (thiscall, double dt).
    void TickRollover(double DeltaSeconds);

    // (RET 4) -- the ToolTip's hover mapped through the table.
    void FeedHover(FToolTipHoverInfo const* Info);

    // (RET 8) -- show a record for HoldSeconds at the environment's anchor.
    void ShowHover(FToolTipHoverInfo const* Info, float HoldSeconds);

    // (RET 0xc) -- the same with an explicit anchor X.
    void ShowHoverAt(FToolTipHoverInfo const* Info, float HoldSeconds, float AnchorX);

    // 's body on the singleton: +0x164 = 0, +0x6c &= 0xfd (hidden).
    void HideRollover();

private:
    FToolTipHoverInfo Entry;      // +0x0ec
    std::map<int, int> Table;     // +0x150
    float HoldSeconds;            // +0x164
    float Anchor[3];              // +0x16c
    unsigned char bWasVisible;    // +0x178
    float HiddenGrace;            // +0x17c
    float CursorStillSeconds;     // +0x180
    float AnchorXSpace;           // +0x184 = -0.51f
    float AnchorXIntro;           // +0x188 = 0.2f
};

// WINFACTORY<Rollover> (the field). The ctor is the routine (the WinFactory head, then
// vftable 0x005d55f4); its one slot is the routine.
class RolloverFactory : public WinFactory
{
public:
    explicit RolloverFactory(char const* Name)
        : WinFactory(Name)
    {
    }

    // .
    Win3D* Create(Win3D* Parent, void* Payload) override;
};

NnRolloverControl* GetNnRolloverSingleton();

// (cdecl, double) -- tick the singleton when there is one.
void NnRolloverTickGlobal(double DeltaSeconds);
// (cdecl) -- FeedHover on the singleton when there is one. Caller: the ToolTip's
// (0x004bdecd).
void NnRolloverFeedHoverGlobal(FToolTipHoverInfo const* Info);
// (cdecl) -- ShowHover on the singleton when there is one. Caller 0x0045a379.
void NnRolloverShowHoverGlobal(FToolTipHoverInfo const* Info, float HoldSeconds);
// (cdecl) -- with the singleton: a record {Ids, Ids, empty caption} through
// ShowHover. Callers: the NavBar 0x0044235d / 0x004423c4 / 0x0044242f.
void NnRolloverShowIdsGlobal(int Ids, float HoldSeconds);
// (cdecl) -- ShowHoverAt on the singleton when there is one. Callers: NN_Preferences
// (0x004b2254, 0x004b2396).
void NnRolloverShowHoverAtGlobal(FToolTipHoverInfo const* Info, float HoldSeconds,
                                 float AnchorX);
// (cdecl) -- with the singleton: a record {Ids, Ids, empty caption} through
// ShowHoverAt. Caller: NN_Preferences::SetProperty 0x004ac1b4 (0x68c, 0, 0.2f).
void NnRolloverShowIdsAtGlobal(int Ids, float HoldSeconds, float AnchorX);
// (cdecl) -- with the singleton up, its hold time (+0x164) = 0 and its visible bit
// cleared (+0x6c &= 0xfd). Caller: NN_Pause::Create 0x004a6bb8.
void NnRolloverHideGlobal();
// (cdecl) -- the singleton's slot 10 (Destroy), then, re-read, its deleting dtor.
// Caller: the client shutdown 0x005b2228.
void NnRolloverDestroyGlobal();
