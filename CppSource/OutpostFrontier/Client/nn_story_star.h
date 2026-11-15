// Author: Giperion.

#pragma once

// baseline NN_StoryStar.cpp ("Client\nn\NN_StoryStar.cpp", 0x004b9890..
// ): the mission log and the mission-objective banner.

// The data model is two VC6 std::list<FmtStr> (0x20-byte nodes {next, prev, FmtStr}):
//   the HISTORY  (alloc byte the field, head the field, size the field) -- the log the
//                NeuroNetStoryStar window lists; entries whose text starts with '*' are log
//                lines, the others mission objectives (static init the branch, atexit
// );
//   the PENDING  (the field / the field / the field) -- objectives waiting to be shown
//                by the MissionObjectiveWindow banner (the branch / the branch).
// Server messages reach it through IClient slot 70 (the routine) and slot 97 (the routine);
// NN_MainButton's update ticks it (the routine).

// Four WinFactory classes live here:
//   NeuroNetStoryStar          WINFACTORY the field (the branch -> the routine), Create
// operator_new(0x390); vftable 0x005d5b74; instance
// . NN_MainButton's mission-log panel (0x67) makes it.
//   MissionObjectiveWindow     WINFACTORY the field (the branch -> the routine), Create
// operator_new(0x340); vftable 0x005d5a8c; instance
// . The fading objective banner.
//   MissionLogMoveToNNWindow   WINFACTORY the field (the branch -> the routine), Create
// operator_new(0x350); vftable 0x005d5dd4,
//                              ButtonMover face 0x005d5e8c. The log icon flying to the button.
//   MissionLogMoveFromNNWindow WINFACTORY the field (the branch -> the routine), Create
// operator_new(0x350); vftable 0x005d5eac,
//                              ButtonMover face 0x005d5f64. RTTI: derives from the ToNN class.

#include "button_mover.h"
#include "wincontrol_base.h"

#include "common_text.h"

#include <list>
#include <stddef.h>

class NnTextControl;
class RichTextWin;

// ---------------------------------------------------------------------------------------------
// MissionObjectiveWindow (RTTI .?AVMissionObjectiveWindow@@ : WinControl), 0x340 bytes.

// vftable 0x005d5a8c -- WinControl's 0x005e5e44 with four overrides:
//   0 the routine (scalar deleting dtor around the routine), 9 the routine Create,
//   11 the branch Update, 13 the routine Draw.
class MissionObjectiveWindow : public WinControl
{
public:
    // Inlined into the routine (0x004bd1a8..0x004bd1e6): WinControl ctor, the vptrs,
    // = this, +0x32c = 0, +0x331 = 1, +0x332 = 0xff, +0x334 = 2.5f, +0x330 = 0,
    // +0x338 = 0.
    MissionObjectiveWindow();

    ~MissionObjectiveWindow() override;

    // slot 9 the routine (RET 8)
    bool Create(Win3D* NewParent, void* Data) override;
    // slot 11 the branch
    void Update() override;
    // slot 13 the routine
    void Draw() override;

    // (thiscall) -- the first pending objective into the text (property 0x1d),
    // new lines dropped; one other than ids 0x762 also becomes the "last objective".
    void ShowFrontObjective();

    float Elapsed;                  // +0x32c -- time in the current phase
    unsigned char bHolding;         // +0x330 -- fully shown, counting HoldTime down
    unsigned char bFadingIn;        // +0x331 -- ctor 1
    unsigned char Alpha;            // +0x332 -- ctor 0xff; the last alpha sent to the text
    unsigned char Pad333;
    float HoldTime;                 // +0x334 -- ctor 2.5f
    // +0x338 -- set when another NeuroNet window cut the banner short; the pending objective
    // then stays queued instead of being popped when the banner goes.
    unsigned char bInterrupted;
    unsigned char Pad339[3];
    NnTextControl* Text;            // +0x33c -- "MissionObjectiveText"

private:
    MissionObjectiveWindow(MissionObjectiveWindow const&) = delete;
    MissionObjectiveWindow& operator=(MissionObjectiveWindow const&) = delete;
};

static_assert(sizeof(void*) != 4 || sizeof(MissionObjectiveWindow) == 0x340,
              "baseline MissionObjectiveWindow is 0x340 bytes (0x004bd189)");
static_assert(sizeof(void*) != 4 || offsetof(MissionObjectiveWindow, Elapsed) == 0x32c,
              "MissionObjectiveWindow +0x32c");
static_assert(sizeof(void*) != 4 || offsetof(MissionObjectiveWindow, HoldTime) == 0x334,
              "MissionObjectiveWindow +0x334");
static_assert(sizeof(void*) != 4 || offsetof(MissionObjectiveWindow, Text) == 0x33c,
              "MissionObjectiveWindow +0x33c");

// ---------------------------------------------------------------------------------------------
// NeuroNetStoryStar (RTTI .?AVNeuroNetStoryStar@@ : WinControl), 0x390 bytes.

// vftable 0x005d5b74 -- WinControl's with six overrides:
//   0 the routine (scalar deleting dtor around the routine), 9 the routine Create,
//   11 the routine Update, 13 the routine Draw, 23 the branch Slot23,
//   42 the branch SetProperty.

// Two layouts, chosen in Create by NN_MainButton's force-mission-log flag (+0x52f): normally a
// close button and, in single player, the Mission / Log toggles over the history text; when
// the flag is up (the player died) the load-game / main-menu buttons (single player) or respawn
// / new player / new server (multiplayer) instead, with the game paused.
class NeuroNetStoryStar : public WinControl
{
public:
    // Inlined into the routine (0x004bd068..0x004bd09b): WinControl ctor, the vptrs,
    // = this, +0x338 = 0, +0x37c = 1, +0x36c = 0, +0x374 = 0, +0x38c = 0.
    NeuroNetStoryStar();

    ~NeuroNetStoryStar() override;

    // slot 9 the routine (RET 8)
    bool Create(Win3D* NewParent, void* Data) override;
    // slot 11 the routine
    void Update() override;
    // slot 13 the routine
    void Draw() override;
    // slot 23 the branch (RET 0xc) -- 0xef01 show / 0xef02 tear down
    bool Slot23(unsigned Msg, unsigned A, unsigned B) override;
    // slot 42 the branch (RET 0xc) -- 0x26, a button was clicked
    intptr_t SetProperty(int Msg, uintptr_t A, uintptr_t B) override;

    // (RET 4) -- switch the history text between the objectives (true) and the log
    // (false), keeping each one's scroll position.
    void SetMissionMode(bool bMission);
    // rebuild the history text from the history list.
    void RebuildHistoryText();
    // "Load Autosave": reload the auto-save character.
    void LoadAutosave();

    int Field_0x32c;                // +0x32c -- TODO: never written or read by this class
    WinControl* CloseButton;        // +0x330 "StoryStarCloseButton"
    RichTextWin* HistoryText;       // +0x334 "StoryStarHistoryText"
    int HistoryShownCount;          // +0x338 -- the field when the text was built
    WinControl* LoadAutosaveButton; // +0x33c
    WinControl* LoadSavegameButton; // +0x340
    WinControl* MainMenuButton;     // +0x344
    WinControl* LoadAutosaveText;   // +0x348
    WinControl* LoadSavegameText;   // +0x34c
    WinControl* MainMenuText;       // +0x350
    WinControl* RespawnButton;      // +0x354
    WinControl* NewPlayerButton;    // +0x358
    WinControl* NewServerButton;    // +0x35c
    WinControl* RespawnText;        // +0x360
    WinControl* NewPlayerText;      // +0x364
    WinControl* NewServerText;      // +0x368
    WinControl* MissionButton;      // +0x36c
    WinControl* MissionText;        // +0x370
    WinControl* LogButton;          // +0x374
    WinControl* LogText;            // +0x378
    unsigned char bMissionMode;     // +0x37c -- ctor 1
    unsigned char SavedCursorDrawFlag; // +0x37d -- the routine's result
    unsigned char Pad37e[2];
    int MissionScrollY;             // +0x380
    int LogScrollY;                 // +0x384
    int PendingScrollY;             // +0x388 -- applied by the next Draw, then -1
    unsigned char bPausedGame;      // +0x38c -- Create paused the game (the routine)
    unsigned char Pad38d[3];

private:
    NeuroNetStoryStar(NeuroNetStoryStar const&) = delete;
    NeuroNetStoryStar& operator=(NeuroNetStoryStar const&) = delete;
};

static_assert(sizeof(void*) != 4 || sizeof(NeuroNetStoryStar) == 0x390,
              "baseline NeuroNetStoryStar is 0x390 bytes (0x004bd049)");
static_assert(sizeof(void*) != 4 || offsetof(NeuroNetStoryStar, HistoryText) == 0x334,
              "NeuroNetStoryStar +0x334");
static_assert(sizeof(void*) != 4 || offsetof(NeuroNetStoryStar, MissionButton) == 0x36c,
              "NeuroNetStoryStar +0x36c");
static_assert(sizeof(void*) != 4 || offsetof(NeuroNetStoryStar, bMissionMode) == 0x37c,
              "NeuroNetStoryStar +0x37c");
static_assert(sizeof(void*) != 4 || offsetof(NeuroNetStoryStar, PendingScrollY) == 0x388,
              "NeuroNetStoryStar +0x388");
static_assert(sizeof(void*) != 4 || offsetof(NeuroNetStoryStar, bPausedGame) == 0x38c,
              "NeuroNetStoryStar +0x38c");

// ---------------------------------------------------------------------------------------------
// MissionLogMoveToNNWindow (RTTI .?AVMissionLogMoveToNNWindow@@ : WinControl, ButtonMover at
// +0x32c), 0x350 bytes. The story-star icon ("DummyMissionLog") sliding from the middle of the
// screen to the mission-log button; it deletes itself once the slide ends.

// vftable 0x005d5dd4: 0 the routine (scalar deleting dtor around the routine),
// 9 the routine Create, 11 the routine Update. The ButtonMover face 0x005d5e8c keeps
// ButtonMover's two slots (the branch / the branch).
class MissionLogMoveToNNWindow : public WinControl, public ButtonMover
{
public:
    // Inlined into the routine (0x004bd2f9..0x004bd31f): WinControl ctor, ButtonMover ctor
    // at +0x32c, the vptrs.
    MissionLogMoveToNNWindow();
    // the mover's list freed, then the WinControl dtor.
    ~MissionLogMoveToNNWindow() override;

    // slot 9 the routine (RET 8)
    bool Create(Win3D* NewParent, void* Data) override;
    // slot 11 the routine -- shared with MissionLogMoveFromNNWindow
    void Update() override;

    WinControl* Icon;               // +0x34c "DummyMissionLog"

private:
    MissionLogMoveToNNWindow(MissionLogMoveToNNWindow const&) = delete;
    MissionLogMoveToNNWindow& operator=(MissionLogMoveToNNWindow const&) = delete;
};

// MissionLogMoveFromNNWindow (RTTI .?AVMissionLogMoveFromNNWindow@@ :
// MissionLogMoveToNNWindow), 0x350 bytes. The same icon sliding back out of the button.
// vftable 0x005d5eac: 0 the routine, 9 the routine Create, 11 the routine Update.
class MissionLogMoveFromNNWindow : public MissionLogMoveToNNWindow
{
public:
    // Inlined into the routine (0x004bd429..0x004bd44f).
    MissionLogMoveFromNNWindow();

    // slot 9 the routine (RET 8)
    bool Create(Win3D* NewParent, void* Data) override;
};

// ---------------------------------------------------------------------------------------------
// The data model.

// the history (front = newest).
extern std::list<FmtStr> G_StoryStarHistory;
// the objectives waiting for the banner (front = shown next).
extern std::list<FmtStr> G_StoryStarPending;
// the last objective the banner showed (or the deferred 0x549c one); static
// init the branch, atexit the branch (~FmtStr).
extern FmtStr G_StoryStarLastObjective;
// G_StoryStarLastObjective holds something the button can re-show.
extern unsigned char G_bStoryStarHasLastObjective;
// the MissionObjectiveWindow (ctor sets, dtor the routine clears).
extern MissionObjectiveWindow* G_MissionObjectiveWindow;
// the NeuroNetStoryStar (ctor sets, dtor the routine clears).
extern NeuroNetStoryStar* G_NeuroNetStoryStar;
// seconds with nothing to show (the routine); the banner's Create compares it
// with 0.5 to decide whether the log icon flies back out.
extern float G_StoryStarIdleTime;
// a rank reached in single player (IClient slot 117) not yet announced.
extern int G_StoryStarRankUp;

// (cdecl) -- G_StoryStarRankUp = Rank. Callers: IClient slot 117 in single player
// (0x0053e23f), and 0 from the connect-list resets (0x004c4baa, 0x004c4cab).
void StoryStarSetRankUp(int Rank);
// (cdecl) -- G_bStoryStarHasLastObjective = 0. Caller: the base load 0x00439b11.
void StoryStarForgetLastObjective();
// (cdecl) -- with a last objective, nothing pending and no banner: queue it again.
// Caller: NN_MainButton's event 0x6e (0x0045cb39).
void StoryStarRequeueLastObjective();
// (cdecl) -- unless the field, drop the pending objectives and the last
// objective flag; then tear the banner down. Callers: 0x0043d33c, 0x0043d3ee, 0x0043d40a,
// (MissionBriefing), 0x0045a189 (NN shutdown), 0x0049f71b (nav map abort),
// , 0x004c4ca4 (connect-list resets).
void StoryStarReset();
// (cdecl) -- a mission message of kind Type (IClient slot 70, 0x005a5ba9; the
// lost-ship path 0x00471efe).
void StoryStarOnMissionMessage(FmtStr const* Text, int Type);
// (cdecl) -- queue an objective for the banner.
void StoryStarPushPending(FmtStr const* Text);
// (cdecl) -- keep only the log lines of the history, re-send it to the server in
// single player, and drop the pending objectives.
void StoryStarPruneHistory();
// (cdecl) -- add an entry to the front of the history (single player: send it).
void StoryStarAddHistory(FmtStr const* Text);
// (cdecl) -- replace the history with the server's packed log (IClient slot 97,
// ): +0x04 the record count, the FmtStr records from +0x08, Size bytes in all.
void StoryStarLoadHistory(unsigned char* Data, int Size);
// (cdecl) -- turn the game ids in Text's values into ids-string ids in place.
void StoryStarResolveIds(FmtStr* Text);
// (cdecl) -- the per-frame driver: show the banner for the next pending objective
// (or the rank-up) when no other NeuroNet window is in the way. Caller: NN_MainButton::Update
// (0x0045c949).
void StoryStarTick();
