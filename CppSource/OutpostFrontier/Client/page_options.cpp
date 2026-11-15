// Author: Giperion.

#include "page_options.h"

#include "menu_ui_init.h"
#include "win_factory.h"
#include "wincontrol_base.h"

namespace
{
    // .
    PageOptions G_PageOptions;
}

PageOptions::PageOptions()
    : UIPage::MasterWin(nullptr)
    , Result(0)
    , Preferences(nullptr)
{
    // the routine(0), the routine on +0x8c (Desc), the three vftables, then
    // +0xb4 = 0. +0xb8 is not written; the static object starts zeroed, which is what the
    // initialiser above reproduces.
}

PageOptions::~PageOptions()
{
    // vftables back, the routine on +0x8c, the routine -- the member and base
    // destructors.
}

bool PageOptions::Create(Win3D* NewParent, void* /*Payload*/)
{
    // . MasterWin::Create (the routine) with a null payload; its result is not
    // tested (there is no page data, so it only reparents / registers / sets the flags).
    UIPage::MasterWin::Create(NewParent, nullptr);
    Result = 0;
    // the routine("NN_Preferences", "NN_Preferences", 0, 1), then
    // slot 2 (AsWinControl) on the result with no null test.
    Win3D* const Created =
        WinFactoryCreate("NN_Preferences", "NN_Preferences", nullptr, reinterpret_cast<void*>(1));
    Preferences = Created->AsWinControl();
    return true;
}

bool PageOptions::Slot23(unsigned Msg, unsigned A, unsigned /*B*/)
{
    // .
    if (Msg == 0x3004)
    {
        PlayOrRewindUiSoundSlot(0); // (0)
        if (A != 0x300c)
            return false;
        Result = 1;
        return true;
    }
    return Msg == 0xf002;
}

PageOptions& GetPageOptions()
{
    return G_PageOptions;
}
