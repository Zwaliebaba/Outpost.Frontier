// Author: Giperion.

#pragma once

// baseline NN_MainButton (RTTI .?AVNN_MainButton@@ : WinControl, Event::Handler, ButtonMover,
// ButtonScaler; Client\nn\NN_MainButton.cpp) -- the in-game NeuroNet bar:
// the row of NeuroNet icons (nav map, inventory, mission log, player info, help and, in
// multiplayer, chat and group) that slide out and open their panel window. One instance at a
// time: the ctor stores itself in the field (G_NnMainButton, nn_modal.h), the dtor clears it.
// Made by the routine (NnSystemInit) through the WinFactory, destroyed by the routine
// (NnSystemShutdown).

// WINFACTORY<NN_MainButton> the field "NN_MainButton": static init the branch
// (the routine + atexit the branch -> the routine), Create the routine
// (operator_new(0x530), the routine, +0x10 = &factory name, slot 9).

// vftable 0x005cea44 (45 slots) -- WinControl's 0x005e5e44 with five overrides:
//   0 the routine (scalar deleting dtor around the routine), 9 the branch Create,
//   11 the branch Update, 13 the branch Draw, 42 the branch SetProperty.
// Faces: IDataViewOwner +0x7c (0x005cea38 = WinControl's), Event::Handler +0x32c (0x005cea30:
// ), ButtonMover +0x330 (0x005cea24: the branch, the branch), ButtonScaler
// +0x350 (0x005cea18: the branch, the branch).

// Panel ids are the HUD command ids the buttons post (the roots' Dword34c): 0x63 nav map,
// 0x66 inventory, 0x67 mission log (story star), 0x65 player info, 0x6a help, 0x68 chat,
// 0x69 group. Every per-panel table is indexed by the slot the baseline switch maps them to:
// 0x66 -> 1, 0x67 -> 2, 0x65 -> 3, 0x6a -> 4, 0x68 -> 5, 0x69 -> 6, anything else -> 0.

// baseline layout (the ButtonMover / ButtonScaler bases keep their entries in a std::vector here,
// so offsets past +0x330 are baseline's, not this class's):
//   +0x000 WinControl (0x32c)          +0x32c Event::Handler vptr
//   +0x330 ButtonMover (0x20)          +0x350 ButtonScaler (0x20)
//   +0x370..+0x388 the seven icon buttons, +0x38c..+0x3a4 their "Root" twins
//   +0x3a8 open button, +0x3ac its panel id, +0x3b0 the button's tooltip saved while open
//   +0x3b4 "NNBackground", +0x3b8 Buttons[7], +0x3d4 Roots[7]
//   +0x3f0 OpenTarget[7][3], +0x444 PanelOrigin[7][3], +0x498 PanelScale[7],
//   +0x4b4 BackgroundPosition[7][3]
//   +0x508 "ScaledBackground", +0x50c closing button, +0x510 its panel id, +0x514 its saved
//   tooltip, +0x518 its slide start[3], +0x524 queued button, +0x528 queued panel id,
//   +0x52c..+0x52f four bytes, size 0x530

#include "button_mover.h"
#include "event_handler.h"
#include "wincontrol_base.h"

class NN_MainButton : public WinControl, public EventHandler, public ButtonMover, public ButtonScaler
{
public:
    // the bases, the zeroed state (+0x3a8, +0x3ac, +0x508..+0x510, +0x518[3],
    // +0x524, the four bytes) and the field = this.
    NN_MainButton();
    // the field = 0 when it is this, then the bases.
    ~NN_MainButton() override;

    // --- WinControl / Win3D overrides ---
    // slot 9 the branch (RET 8)
    bool Create(Win3D* NewParent, void* Data) override;
    // slot 11 the branch
    void Update() override;
    // slot 13 the branch
    void Draw() override;
    // slot 42 the branch (RET 0xc) -- property 0x4f is swallowed (returns 1) while either
    // animation runs; everything else goes to WinControl's the routine.
    intptr_t SetProperty(int Msg, uintptr_t A, uintptr_t B) override;

    // --- Event::Handler ---
    // (RET 4)
    bool OnEvent(unsigned MsgId) override;

    // --- ButtonMover / ButtonScaler ---
    // a slide finished.
    void OnMoveFinished() override;
    // a scale finished.
    void OnScaleFinished() override;

    // (RET 8) -- PanelOrigin[slot(PanelId)] = Origin.
    void SetPanelOrigin(int PanelId, float const* Origin);
    // (RET 4) -- Buttons[slot(PanelId)].
    WinControl* GetPanelButton(int PanelId) const;
    // (RET 8) -- slide Button out and open panel PanelId.
    void OpenPanel(WinControl* Button, int PanelId);
    // drop the open button at once (no animation) and snap the mover.
    void ResetPanel();
    // close the open panel (its window gets 0xef02, the background shrinks).
    void ClosePanel();
    // (RET 4) -- rebuild "ScaledBackground" for the open panel at Scale.
    void MakeScaledBackground(float Scale);
    // [+0x3a8] != 0.
    bool HasActivePanel() const;
    // (RET 4) -- the group button pair's colour override (0x33, the field).
    void SetGroupButtonAlert(bool bAlert);
    // (RET 4) -- the group button pair's mesh: 0 request, 1 trade, 2 group.
    void SetGroupButtonMesh(int Mode);
    // delete "ScaledBackground".
    void FreeScaledBackground();
    // +0x52c = 1.
    void SetInfoCardLayout();
    // (RET 4) / the routine -- +0x52f.
    void SetForceMissionLog(bool bForce);
    bool GetForceMissionLog() const;

    WinControl* NavmapButton;        // +0x370
    WinControl* InventoryButton;     // +0x374
    WinControl* MissionLogButton;    // +0x378
    WinControl* PlayerInfoButton;    // +0x37c
    WinControl* ChatButton;          // +0x380 (multiplayer only)
    WinControl* GroupButton;         // +0x384 (multiplayer only)
    WinControl* HelpButton;          // +0x388
    WinControl* NavmapRoot;          // +0x38c
    WinControl* InventoryRoot;       // +0x390
    WinControl* MissionLogRoot;      // +0x394
    WinControl* PlayerInfoRoot;      // +0x398
    WinControl* ChatRoot;            // +0x39c (multiplayer only)
    WinControl* GroupRoot;           // +0x3a0 (multiplayer only)
    WinControl* HelpRoot;            // +0x3a4
    WinControl* OpenButton;          // +0x3a8
    int OpenPanelId;                 // +0x3ac
    int SavedToolTipIds;             // +0x3b0
    WinControl* Background;          // +0x3b4
    WinControl* Buttons[7];          // +0x3b8
    WinControl* Roots[7];            // +0x3d4
    float OpenTarget[7][3];          // +0x3f0
    float PanelOrigin[7][3];         // +0x444
    float PanelScale[7];             // +0x498
    float BackgroundPosition[7][3];  // +0x4b4
    WinControl* ScaledBackground;    // +0x508
    WinControl* ClosingButton;       // +0x50c
    int ClosingPanelId;              // +0x510
    int ClosingToolTipIds;           // +0x514
    float ClosingStart[3];           // +0x518
    WinControl* QueuedButton;        // +0x524
    int QueuedPanelId;               // +0x528
    // +0x52c -- the panels open onto the inventory info-card layout (the routine sets it,
    // NN_Inventory 0x00477b41; the scaler's open branch clears it).
    unsigned char bInfoCardLayout;
    // +0x52d -- set when the inventory button is clicked while it is open and "NN_Dealer"
    // exists; Update posts 0x66 once the dealer window is gone and nothing animates.
    unsigned char bInventoryAfterDealer;
    // +0x52e -- set when a button is clicked while "NN_Dealer" exists and an animation runs;
    // Update posts 0x66 once the animation settles while the dealer is still there.
    unsigned char bInventoryWithDealer;
    // +0x52f -- the routine (the story star, 0x004b9eb3 / 0x004b9f4c): in multiplayer Update
    // opens the mission log itself while no panel is open; Draw keeps drawing under the letterbox.
    unsigned char bForceMissionLog;

private:
    // The seven-root loop written out at 0x0045ce7d, 0x0045d15c, 0x0045d3a1, 0x0045d4c4 and
    // Roots[i]->SetProperty(0xc, Buttons[i] is the open or the closing button, 0)
    // for every non-null root.
    void UpdateRootHighlights();

    NN_MainButton(NN_MainButton const&) = delete;
    NN_MainButton& operator=(NN_MainButton const&) = delete;
};

// while set, NN_MainButton's Draw does not auto-close the open panel when the
// HUD stops picking (0x0045ca36) and NN_Exit's Create does not close it (0x00471810). Written 0
// at 0x004499f7 / 0x0044adaf / 0x0044bd30 / 0x0044c29e, from the base room state's +0x1338 at
// / 0x0044c498, 0 at 0x004bb7b3 and 1 at 0x004bb9df (the story star). None of those
// writers is ported yet, so the byte stays at its load-time 0.
extern unsigned char G_bNnKeepPanelOpen;
