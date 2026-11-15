// Author: Giperion.

// baseline Client\Gf\RoomControlHotspot.cpp -- the room buttons. See room_control_hotspot.h.

#include "room_control_hotspot.h"

#include "fade_to_black.h"      // SetFadeTarget / SetFadeImmediate / GetFadeDuration
#include "gf_object_ship.h"     // GfObjectShip (the routine), PlayLaunchScript (the routine)
#include "input_manager.h"      // SetClientMouseCursorDrawFlag (the routine)
#include "menu_universe_base.h" // ApplyClientLocationAudio / QueueClientLocationChange
#include "nav_bar.h"            // NavBarCloseService (the routine)
#include "nn_mission_vendor.h"  // NnMissionVendor::CloseWindow (the routine)
#include "win3d.h"              // Win3DFindByName (the routine), Win3DDeleteTree (the routine)
#include "win_factory.h"        // WinFactoryCreate (the routine)

RoomButton::RoomButton()
    : Location(0)
    , Behavior(0)
{
    // vftable 0x005cc084, +0x04 = +0x08 = 0.
}

RoomButton::~RoomButton()
{
    // MOV [ECX],0x005cc084 -- nothing else.
}

void RoomButton::Activate()
{
    // RET.
}

void RoomButton::Update(float /*Delta*/)
{
    // RET 4.
}

void RoomButton::Set(unsigned InLocation, int InBehavior)
{
    // +0x04 = param_1, +0x08 = param_2.
    Location = InLocation;
    Behavior = InBehavior;
}

ExitDoorButton::ExitDoorButton()
    : State(0)
    , Mode(0)
{
    // vftable 0x005cc068, +0x10 = 0, +0x0c = 0. +0x14 is left alone
    // (Activate zeroes it before Update reads it).
}

ExitDoorButton::~ExitDoorButton()
{
    // thunk_FUN_0044d300 (the RoomButton vftable back) -- nothing of its own.
}

void ExitDoorButton::Activate()
{
    // . 0x0044d333..0x0044d338: only from idle.
    if (State != 0)
        return;
    // +0x0c = 1, +0x14 = 0.
    State = 1;
    Elapsed = 0.0f;
    // the routine(1.0f, (float)(1.0 / the field)) -- FLD the fade
    // duration, FDIVR the double 1.0 at 0x005c89b8, FSTP float.
    SetFadeTarget(1.0f, static_cast<float>(1.0 / static_cast<double>(GetFadeDuration())));
    // the routine(0) -- the software cursor off.
    SetClientMouseCursorDrawFlag(0);
    // the routine(+0x04) on the field -- the location's audio.
    ApplyClientLocationAudio(Location);
}

void ExitDoorButton::Update(float Delta)
{
    // (RET 4).
    if (State == 0)
        return;
    if (State == 1)
    {
        // FLD Delta, FADD float [+0x14], FST float [+0x14], FCOMP
        // float [the field], TEST AH,0x1 (C0) + JNZ -> return. C0 is set when the sum is
        // below the duration or unordered; the compare is on the unrounded sum still on the
        // FPU stack, not on the float just stored.
        double const Sum = static_cast<double>(Delta) + static_cast<double>(Elapsed);
        Elapsed = static_cast<float>(Sum);
        if (!(Sum >= static_cast<double>(GetFadeDuration())))
            return;
        // +0x0c = 2, the routine(1), the routine(1.0f).
        State = 2;
        SetClientMouseCursorDrawFlag(1);
        SetFadeImmediate(1.0f);
        return;
    }
    if (State != 2)
        return;
    // by +0x10.
    if (Mode == 0)
    {
        // the routine(+0x04, 0, 0) on the field -- the location
        // change queued with no delay and no audio; +0x0c = 0.
        QueueClientLocationChange(Location, 0.0f, false);
        State = 0;
        return;
    }
    if (Mode == 1)
    {
        // the routine on the routine()'s result, not null-tested.
        GfObjectShip()->PlayLaunchScript();
    }
    // .
    State = 0;
}

LaunchButton::LaunchButton()
{
    // +0x0c = 0, vftable 0x005cc058, +0x10 = 1.
    Mode = 1;
}

NewsVendorButton::NewsVendorButton()
{
    // +0x04 = +0x08 = 0, vftable 0x005cc094.
}

NewsVendorButton::~NewsVendorButton()
{
    // . 0x0044d474..0x0044d48d: the routine("NN_News"); none -> nothing.
    Win3D* const Window = Win3DFindByName("NN_News");
    if (Window != nullptr)
    {
        // the routine(0) -- the NavBar's open service tab closed.
        NavBarCloseService(false);
        // the routine on every child (the next sibling read first),
        // slot 10 (+0x28) and slot 0 with 1 on the window -- the routine's body inlined.
        Win3DDeleteTree(Window);
    }
    // vftable 0x005cc084 (~RoomButton).
}

void NewsVendorButton::Activate()
{
    // . 0x0044d4e1..0x0044d526: the destructor's close, inlined.
    Win3D* const Window = Win3DFindByName("NN_News");
    if (Window != nullptr)
    {
        NavBarCloseService(false);
        Win3DDeleteTree(Window);
    }
    // the routine("NN_News", "NN_News", 0, 0); the result is dropped.
    WinFactoryCreate("NN_News", "NN_News", nullptr, nullptr);
}

MissionVendorButton::MissionVendorButton()
{
    // +0x04 = +0x08 = 0, vftable 0x005cc0a4.
}

MissionVendorButton::~MissionVendorButton()
{
    // . 0x0044d5a3..0x0044d5be: the routine("NN_MissionVendor") -- the string is
    // + 1 -- and the routine on the window found.
    Win3D* const Window = Win3DFindByName("NN_MissionVendor");
    if (Window != nullptr)
        static_cast<NnMissionVendor*>(Window)->CloseWindow();
    // vftable 0x005cc084 (~RoomButton).
}

void MissionVendorButton::Activate()
{
    // the routine("NN_MissionVendor", "NN_MissionVendor", 0, 0) (the result is
    // dropped), then the routine(1) -- the software cursor on.
    WinFactoryCreate("NN_MissionVendor", "NN_MissionVendor", nullptr, nullptr);
    SetClientMouseCursorDrawFlag(1);
}
