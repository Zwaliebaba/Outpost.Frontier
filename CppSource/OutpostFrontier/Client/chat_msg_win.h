// Author: Giperion.

#pragma once

// baseline MsgWin (RTTI .?AVMsgWin@@ : RichTextWin) -- the chat message window, the field.
// (ChatCreateMsgWin) builds it through WINFACTORY<MsgWin> the field ("MsgWin",
// ctor the routine from the branch, create the routine, operator_new(0x4a0)).

// vftable 0x005cffc4 (48 slots) is RichTextWin's except:
//    0 the routine (scalar deleting dtor around the routine)
//    4 the routine HitTest (XOR AL,AL / RET 4) -- RichTextWin has the branch here
//   11 the branch Update
// The IDataViewOwner face at +0x7c keeps WinControl's entries.

// Layout: +0x000 RichTextWin (0x498), +0x498 FChatDisplaySnapshot* (the snapshot it shows --
// , the store's Recent), +0x49c the snapshot generation last shown. Size 0x4a0.
// The ctor (inlined in the routine) and the dtor the routine zero both.

#include "chat_init.h"
#include "rich_text_win.h"

class MsgWin : public RichTextWin
{
public:
    MsgWin();

    ~MsgWin() override;

    // slot 4 the routine -- always false: the window is never picked.
    bool HitTest(float const* Ray) override;

    // slot 11 the branch -- post the "lost / new affiliation" notices for the local client
    // from Common's changed-affiliation map (then empty it), let the Recent snapshot age, and
    // re-show it when its generation moved on.
    void Update() override;

    // ClearContent, one RDLRefNode on the snapshot's display list, remember the
    // generation.
    void RefreshFromSnapshot();

    FChatDisplaySnapshot* Snapshot;     // +0x498
    int SnapshotGeneration;             // +0x49c

private:
    MsgWin(MsgWin const&) = delete;
    MsgWin& operator=(MsgWin const&) = delete;
};

// (cdecl) -- with no MsgWin yet and a Recent snapshot (the field): build
// "MsgWin", place it at {-0.51, 0.25, -1}, 1.0 x 0.39, bind it to the snapshot, show it
// (Flags |= 3), no frame, align mode 3, no auto scrollbar. Caller: the multiplayer entry of the
// menu state machine (0x00574994).
void ChatCreateMsgWin();
