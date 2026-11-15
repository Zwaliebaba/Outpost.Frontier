// Author: Giperion.

#pragma once

// baseline Client\Gf\RoomControlHotspot.cpp (the FDUMP path at .rdata 0x005cc01c) -- the room
// buttons RoomControl builds from its RoomData hotspots (the routine, room_control.cpp) and
// keeps at RoomControl +0x1134 (count +0x1334). NavBar tells them a tab was pressed
// (the routine: slot 1 on every button whose +0x04 or +0x08 equals the tab's type); the room
// update steps them every frame (the routine: slot 2 with the frame delta); the RoomControl
// destructor deletes them (the routine).

// RTTI (COL at vftable[-1]), every vftable three slots
//   0 scalar deleting dtor, 1 Activate (thiscall, no arguments), 2 Update (thiscall, float, RET 4):
//   RoomButton          vftable 0x005cc084 (COL 0x005e8df8): the branch, 0x00401e90, 0x0053e4a0
//   ExitDoorButton      vftable 0x005cc068 (COL 0x005e8d50) : RoomButton
// , the routine, the routine
//   LaunchButton        vftable 0x005cc058 (COL 0x005e8dcc) : ExitDoorButton -- the same three
//                       functions; only its constructor differs (+0x10 = 1)
//   NewsVendorButton    vftable 0x005cc094 (COL 0x005e8e40) : RoomButton
// , the branch, 0x0053e4a0
//   MissionVendorButton vftable 0x005cc0a4 (COL 0x005e8e88) : RoomButton
// , the branch, 0x0053e4a0
// (a bare RET) and 0x0053e4a0 (RET 4) are ICF-folded empty bodies shared with
// IDPMsgHandler; here they are RoomButton's own empty Activate / Update.

// The RoomButtonInfo "behavior=" value (Common.dll RoomData::read_Hotspot_block option table)
// picks the class in the routine: 1 ExitDoor -> ExitDoorButton, 2 NewsVendor ->
// NewsVendorButton, 3 MissionVendor -> MissionVendorButton, 4 Launch -> LaunchButton,
// 5 StartDealer / 6 FrontDesk -> a plain RoomButton, 7..12 -> no button, anything else ->
// FDUMP "Unknown button behavior type".

class RoomButton
{
public:
    // vftable 0x005cc084, +0x04 = +0x08 = 0.
    RoomButton();
    // (the vftable put back; thunk 0x0044d190), scalar deleting the branch.
    virtual ~RoomButton();
    // Slot 1 -- 0x00401e90: nothing.
    virtual void Activate();
    // Slot 2 -- 0x0053e4a0 (RET 4): nothing.
    virtual void Update(float Delta);

    // (thiscall, RET 8) -- +0x04 = InLocation, +0x08 = InBehavior.
    void Set(unsigned InLocation, int InBehavior);

    // +0x04 -- MakeLocationID(RoomControl +0x34 base, hotspot room_switch) through the routine,
    // 0 when the hotspot has no room_switch.
    unsigned Location;
    // +0x08 -- the hotspot's behavior value.
    int Behavior;

private:
    RoomButton(RoomButton const&) = delete;
    RoomButton& operator=(RoomButton const&) = delete;
};

// operator_new(0x18). An exit door: Activate fades to black and starts the location's audio;
// once the fade has run its duration Update snaps to black and changes location (Mode 0) or
// plays the player ship's launch script (Mode 1, LaunchButton).
class ExitDoorButton : public RoomButton
{
public:
    // Inlined at 0x0044cf62..0x0044cf74: the routine, vftable 0x005cc068, +0x10 = +0x0c = 0.
    ExitDoorButton();
    // thunk_FUN_0044d300, then the conditional delete.
    ~ExitDoorButton() override;
    // .
    void Activate() override;
    // (RET 4).
    void Update(float Delta) override;

    int State;     // +0x0c 0 idle, 1 fading, 2 faded
    int Mode;      // +0x10 0 exit door (location change), 1 launch
    float Elapsed; // +0x14 the fade time run so far (not written by the ctor)
};

class LaunchButton : public ExitDoorButton
{
public:
    // Inlined at 0x0044d029..0x0044d03d: the routine, +0x0c = 0, vftable 0x005cc058, +0x10 = 1.
    LaunchButton();
};

// operator_new(0xc). Opens NN_News (a fresh one each time); the destructor closes it.
class NewsVendorButton : public RoomButton
{
public:
    // +0x04 = +0x08 = 0, vftable 0x005cc094.
    NewsVendorButton();
    // (scalar deleting the routine).
    ~NewsVendorButton() override;
    // .
    void Activate() override;
};

// operator_new(0xc). Opens NN_MissionVendor; the destructor closes it.
class MissionVendorButton : public RoomButton
{
public:
    // +0x04 = +0x08 = 0, vftable 0x005cc0a4.
    MissionVendorButton();
    // (scalar deleting the routine).
    ~MissionVendorButton() override;
    // .
    void Activate() override;
};
