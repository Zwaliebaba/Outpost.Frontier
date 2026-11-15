// Author: Giperion.

#pragma once

// baseline Event::Handler (RTTI .?AVHandler@Event@@, Client\Interface\KeyMap.cpp) -- the face a
// class inherits to receive KeyMap command ids through the the field registry
// (hud_command_dispatch.h). Base vftable 0x005cbed8 has a single slot (_purecall): the event
// callback, thiscall RET 4, which the routine calls as vt[0](MsgId) and stops at the first
// true. The destructor is not virtual: the routine is called directly by every derived dtor
// (e.g. TypeSelect 0x0056624f, UIPage::MasterWin, the main command handler atexit 005b2550).

// The face sits wherever the derived class puts it (TypeSelect +0x7c, MasterWin +0x80); the
// registry stores the face's address, so AsHudCmdHandler() is `this` of the face.

#include "hud_command_dispatch.h"

class EventHandler
{
public:
    // slot 0 -- handle MsgId; true consumes it.
    virtual bool OnEvent(unsigned MsgId) = 0;

    FHudCmdHandler* AsHudCmdHandler()
    {
        return this;
    }

    // (thiscall, RET 4) -- add (MsgId, this) to the registry.
    void RegisterEvent(unsigned MsgId)
    {
        HudRegisterCommandHandler(AsHudCmdHandler(), MsgId);
    }

    // (thiscall, RET 4) -- drop this handler's entries for MsgId.
    void UnregisterEvent(unsigned MsgId)
    {
        HudUnregisterCommand(AsHudCmdHandler(), MsgId);
    }

    // (thiscall) -- drop every entry of this handler.
    void UnregisterAllEvents()
    {
        HudUnregisterCommandHandler(AsHudCmdHandler());
    }

protected:
    EventHandler()
    {
    }

    // vftable back to 0x005cbed8, the routine(this), erase every registration
    // (freeing an emptied tree), the routine(this, 0).
    ~EventHandler()
    {
        HudDestroyEventHandler(AsHudCmdHandler());
    }

private:
    EventHandler(EventHandler const&) = delete;
    EventHandler& operator=(EventHandler const&) = delete;
};
