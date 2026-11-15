// Author: Giperion.

#pragma once

// baseline Client/Interface UIPage — the object a PAGE_*.txt layout becomes once instantiated.

// RTTI (OutpostFrontier.exe COL/CHD):
//   UIPage::IUIPage            vftable 0x005e534c  (slot 0 = 0x005914d0, slots 1..4 _purecall)
//   UIPage::Page : IUIPage     vftable 0x005e5334  (0x1c bytes, operator_new in the routine)
//   UIPage::MasterWin : Win3D, IUIPage (+0x7c), Event::Handler (+0x80)
//                              vftables 0x005e5384 (main), 0x005e536c (IUIPage),
// (Event::Handler)

// MasterWin derives from Win3D (win3d.h); the UI* widgets from WinControl, TextWin or WinObject.

#include "event_handler.h"
#include "ui_page_desc.h"
#include "wincontrol_base.h"

#include "x86math.h"

#include <map>
#include <string>

// — the UI_PAGE widgets' shared lazy the routine (3DMathEngine) cache,
// used by every UI* slot-8 override (the routine, the routine, ...).
I3DMathEngine* UiWidgetMathEngine();

// Shared body of the UI* slot-8 overrides after their SetLayout (+0x1c) / SetPosition (+0x14)
// calls (the routine 00599b8c.., the routine 0059a54c..): with M = the 3x3 at +0x3c and
// Z = the new translation Z,
//   OutWidth  = |(M * (Width, 0, Z)).x - (M * (0, 0, Z)).x|
//   OutHeight = |(M * (0, Height, Z)).y - (M * (0, 0, Z)).y|
// via four I3DMathEngine::TransformMatrix (+0x2c) calls in that order.
void UiWidgetProjectExtent(float const Matrix[9], float Z, float Width, float Height,
                           float* OutWidth, float* OutHeight);

namespace UIPage
{
    // vftable 0x005e534c.
    class IUIPage
    {
    public:
        // slot 0 — the routine scalar deleting destructor.
        virtual ~IUIPage() {}
        // slot 1 (+0x04) — widget count.
        virtual unsigned GetCount() = 0;
        // slot 2 (+0x08) — the Index-th widget name in map order, or null.
        virtual char const* GetWidgetName(unsigned Index) = 0;
        // slot 3 (+0x0c) — the Index-th widget in map order, or null.
        virtual Win3D* GetWidget(unsigned Index) = 0;
        // slot 4 (+0x10) — widget by instance name, or null.
        virtual Win3D* FindWidget(char const* Name) = 0;
    };

    // vftable 0x005e5334. baseline layout: +0x04 std::map<std::string, Win3D*> (VC6 _Tree, size
    // at +0x14), +0x18 float Aspect (1.0f from the routine's construction, then the page
    // descriptor's value).
    class Page : public IUIPage
    {
    public:
        Page();
        // (via the routine): delete every mapped widget, then the map.
        ~Page() override;

        // — MOV EAX,[ECX+0x14] (map size).
        unsigned GetCount() override;

        char const* GetWidgetName(unsigned Index) override;

        Win3D* GetWidget(unsigned Index) override;

        Win3D* FindWidget(char const* Name) override;

        // — build every entry through the WINFACTORY registry, place it and
        // record it by name. Returns the result of the last child recursion (1 for an empty
        // array); a factory miss returns 0 at once.
        bool Instantiate(FUiWidgetEntry** Entries, Win3D* Parent, FTransform const* Matrix);

        float Aspect;   // +0x18

    private:
        std::map<std::string, Win3D*> Widgets;   // +0x04

        Page(Page const&) = delete;
        Page& operator=(Page const&) = delete;

        friend Page* CreatePage(FUiPageData* Data, FTransform const* Matrix, Win3D* Parent);
    };

    // (cdecl) — new Page, Aspect = Data->Aspect, Instantiate(Data->Entries, Parent,
    // Matrix); on failure the page is deleted and null returned.
    Page* CreatePage(FUiPageData* Data, FTransform const* Matrix, Win3D* Parent);

    // RTTI .?AVMasterWin@UIPage@@ : Win3D, IUIPage (+0x7c, vftable 0x005e534c), Event::Handler
    // (+0x80). vftable 0x005e5384 (28 slots). baseline +0x84 = FUiPageData*, +0x88 = Page*.
    // Event::Handler face (event_handler.h) at +0x80, vftable 0x005e5364: slot 0 = 0053f330
    // (XOR AL,AL / RET 4) -- MasterWin consumes no command; the PAGE screens that derive from it
    // override the slot.
    class MasterWin : public Win3D, public IUIPage, public EventHandler
    {
    public:
        // — Data is the owning screen's FUiPageDesc::GetPageData().
        explicit MasterWin(FUiPageData* Data);
        // (scalar deleting the routine, IUIPage thunk the branch).
        ~MasterWin() override;

        // slot 9 (+0x24) — the routine.
        bool Create(Win3D* NewParent, void* Payload) override;
        // slot 10 (+0x28) — the routine.
        void Destroy() override;

        // IUIPage face at +0x7c — the branch / 00591c70 / 00591c90 / 00591cb0 forward to
        // the Page at +0x88 (0 when there is none).
        unsigned GetCount() override;
        char const* GetWidgetName(unsigned Index) override;
        Win3D* GetWidget(unsigned Index) override;
        Win3D* FindWidget(char const* Name) override;

        // Event::Handler face at +0x80, slot 0 — the routine: false.
        bool OnEvent(unsigned MsgId) override;

        Page* GetPage() const { return PageObject; }

    protected:
        FUiPageData* PageData;   // +0x84
        Page* PageObject;        // +0x88

    private:
        MasterWin(MasterWin const&) = delete;
        MasterWin& operator=(MasterWin const&) = delete;
    };
}
