// Author: Giperion.

#pragma once

// The client state classes -- baseline OutpostFrontier.exe's IInputEvent / IEnvironment hierarchy.

// Every client state object derives from IEnvironment, which derives from IInputEvent; the
// RTTI Class Hierarchy Descriptors at each vftable's [-4] COL list exactly these two bases:
//   GF               vftable 0x005cae24, COL 0x005e8734 (.?AUGF@@)              gameflow.cpp
//   SpaceApp         vftable 0x005e0554, COL 0x005ef678 (.?AVSpaceApp@@)        space_app.h
//                    (plus Event::Handler at +4, vftable 0x005e0548, COL 0x005ef6e0)
//   IntroFocus       vftable 0x005e3034, COL 0x005f04a0 (.?AUIntroFocus@@)      client_state.cpp
//   MissionBriefing  vftable 0x005cb094, COL 0x005e88d4 (.?AUMissionBriefing@@) mission_briefing.cpp
// A scan of every COL in the image finds no other class with either base, and neither base has
// a vftable of its own in the exe (their constructors are elided: IntroFocus's ctor the routine
// stamps only 0x005e3034). Each derived vftable has the same 15 slots, all thiscall; the
// arities below are the RET n of every body in the four tables. No slot is a destructor.

// Where IInputEvent ends and IEnvironment begins is not in the binary. The port puts the five
// window-message handlers (slots 0..4: the message-shaped arguments, RET 0x10 / 0xc, a bool
// answer) in IInputEvent and the rest in IEnvironment; the vtable order -- the only thing a
// caller sees -- is the baseline order either way.

// Who calls what (raw listing, every holder of a state pointer read: the field XREF[14] and
// the four singleton getters the routine / the routine / the routine / the routine):
//   +0x1c slot 7   the routine (IsClientIntroEnvironment), the routine case 3 (0x005b2b93)
//   +0x20 slot 8   InitializeEngine 0x005b1e0d / 0x005b1ea5 / 0x005b1f50 / 0x005b1f5c
//   +0x24 slot 9   the atexit teardown the branch (0x005b21db..0x005b21ff)
//   +0x28 slot 10  the routine cases 4 / 5 (0x005b2c05 / 0x005b2bf2)
//   +0x2c slot 11  the render surface reset 0x00424c23 (Width, Height)
//   +0x30 slot 12  the routine (0x005b2af4, 0x005b2b50, 0x005b2bb7, 0x005b2bdf)
//   +0x34 slot 13  the routine (every case), the branch (0x005b21ac), SpaceApp slot 9
//   +0x38 slot 14  the frame the routine (0x005b2a0b), NN_Prompt's modal loops (0x004740b6,
// , 0x0047439b), ServerFilterDialog's loop (0x00563872)
// Slots 0..6 are called by nothing in the exe (tracker the branch has the full survey); the
// window procedure feeds input through the Win3D manager and Event::Handler, not these slots.

// Names: slots 7..9 and 11..14 are named after what every one of the four bodies and every
// caller does; the baseline names are not recovered. Slots with no caller and no shared meaning
// keep SlotNN.

struct IInputEvent
{
    // +0x00, RET 0x10 -- a key message (A, Message, B, C). GF the branch, SpaceApp
    // , IntroFocus 0x0043d8f0, MissionBriefing the branch.
    virtual bool Slot00(unsigned A, unsigned Message, unsigned B, unsigned C) = 0;
    // +0x04, RET 0xc. GF the branch, SpaceApp / IntroFocus 0x004b50e0, MissionBriefing
    // .
    virtual bool Slot01(unsigned A, unsigned B, unsigned C) = 0;
    // +0x08, RET 0xc -- a mouse move (A, B, packed lParam). GF the branch, SpaceApp
    // , IntroFocus 0x004b50e0, MissionBriefing the branch.
    virtual bool Slot02(unsigned A, unsigned B, unsigned long Packed) = 0;
    // +0x0c, RET 0x10 -- a mouse button (A, Message, B, packed lParam). GF the branch,
    // SpaceApp the branch, IntroFocus 0x0043d8f0, MissionBriefing the branch.
    virtual bool Slot03(unsigned A, unsigned Message, unsigned B, unsigned long Packed) = 0;
    // +0x10, RET 0xc. GF / MissionBriefing the branch, SpaceApp the branch, IntroFocus
    // .
    virtual bool Slot04(unsigned A, unsigned B, unsigned C) = 0;
};

struct IEnvironment : public IInputEvent
{
    // +0x14, RET 0. GF the branch (JMP the routine); the others 0x00401e90 (a bare RET).
    virtual void Slot05() = 0;
    // +0x18, RET 4. 0x0053e4a0 (a bare RET 4) in all four tables.
    virtual void Slot06(unsigned A) = 0;
    // +0x1c -- the environment type: SpaceApp 0 (0x004b5010), GF 1 (the branch), IntroFocus 2
    // (the branch), MissionBriefing 3 (the branch).
    virtual int GetEnvironmentType() = 0;
    // +0x20 -- once, from InitializeEngine. GF the branch, MissionBriefing the branch;
    // SpaceApp / IntroFocus 0x00401e90 (a bare RET).
    virtual void PreInit() = 0;
    // +0x24 -- once, from the atexit teardown. GF the branch, SpaceApp the routine,
    // MissionBriefing the branch; IntroFocus 0x00401e90.
    virtual void Shutdown() = 0;
    // +0x28, RET 0. 0x00401e90 (a bare RET) in all four tables.
    virtual void Slot10() = 0;
    // +0x2c, RET 8 -- the render surface size. GF / IntroFocus the branch, SpaceApp
    // , MissionBriefing 0x0053aaa0 (a bare RET 8).
    virtual void Resize(int Width, int Height) = 0;
    // +0x30, RET 4 -- enter the state; false refuses it (the routine then returns null).
    virtual bool Init(int Param) = 0;
    // +0x34 -- leave the state.
    virtual void Destroy() = 0;
    // +0x38, RET 4 -- one frame; the answer is the next state id (0 = stay).
    virtual int Update(float DeltaSeconds) = 0;
};
