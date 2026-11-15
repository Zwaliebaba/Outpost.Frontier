// Author: Giperion.

#include "ui_page.h"

#include "globals.h"
#include "space_camera.h"
#include "win3d_engine_submit.h"
#include "win_factory.h"

#include "common_csys.h"

#include <math.h>
#include <string.h>
#include <utility>

namespace
{
    // — the routine's own lazy the routine result.
    I3DMathEngine* G_UiPageMathEngine = nullptr;

    // .data 0x006155ec = 0xb22bcc77 (-1e-08f), added to the depth of the top-left unproject in
    // .
    unsigned const kMasterWinDepthBiasBits = 0xb22bcc77u;

    float FloatFromBits(unsigned Bits)
    {
        float Value;
        memcpy(&Value, &Bits, sizeof(Value));
        return Value;
    }
}

namespace
{
    // .
    I3DMathEngine* G_UiWidgetMathEngine = nullptr;
}

I3DMathEngine* UiWidgetMathEngine()
{
    if (G_UiWidgetMathEngine == nullptr)
        G_UiWidgetMathEngine = Acquire3DMathEngine();
    return G_UiWidgetMathEngine;
}

void UiWidgetProjectExtent(float const Matrix[9], float Z, float Width, float Height,
                           float* OutWidth, float* OutHeight)
{
    FMatrix const& M = *reinterpret_cast<FMatrix const*>(Matrix);

    FVector Source;
    FVector WidthPoint;
    FVector Origin;

    Source.X = Width;
    Source.Y = 0.0f;
    Source.Z = Z;
    UiWidgetMathEngine()->TransformMatrix(WidthPoint, M, Source);
    Source.X = 0.0f;
    Source.Y = 0.0f;
    Source.Z = Z;
    UiWidgetMathEngine()->TransformMatrix(Origin, M, Source);
    *OutWidth = fabsf(WidthPoint.X - Origin.X);

    FVector HeightPoint;
    Source.X = 0.0f;
    Source.Y = Height;
    Source.Z = Z;
    UiWidgetMathEngine()->TransformMatrix(HeightPoint, M, Source);
    Source.X = 0.0f;
    Source.Y = 0.0f;
    Source.Z = Z;
    UiWidgetMathEngine()->TransformMatrix(Origin, M, Source);
    *OutHeight = fabsf(HeightPoint.Y - Origin.Y);
}

namespace UIPage
{
    Page::Page()
        : Aspect(1.0f)
    {
        // inline ctor: vftable 0x005e534c then 0x005e5334, empty _Tree
        // (head + nil nodes), +0x18 = 0x3f800000.
    }

    Page::~Page()
    {
        // for every node, if (second) second->vftable[0](1); then erase all and
        // free the head/nil nodes (std::map destructor).
        for (std::map<std::string, Win3D*>::iterator It = Widgets.begin();
             It != Widgets.end(); ++It)
        {
            if (It->second != nullptr)
                delete It->second;
        }
        Widgets.clear();
    }

    unsigned Page::GetCount()
    {
        // MOV EAX,[ECX+0x14].
        return static_cast<unsigned>(Widgets.size());
    }

    char const* Page::GetWidgetName(unsigned Index)
    {
        // it = begin; while (it != end && Index-- != 0) ++it;
        // it == end → 0; else node string _Ptr, or the shared empty string when _Ptr is 0.
        std::map<std::string, Win3D*>::iterator It = Widgets.begin();
        while (It != Widgets.end())
        {
            unsigned const Remaining = Index;
            --Index;
            if (Remaining == 0)
                break;
            ++It;
        }
        if (It == Widgets.end())
            return nullptr;
        return It->first.c_str();
    }

    Win3D* Page::GetWidget(unsigned Index)
    {
        // same walk; it == end → 0; else node+0x1c (second).
        std::map<std::string, Win3D*>::iterator It = Widgets.begin();
        while (It != Widgets.end())
        {
            unsigned const Remaining = Index;
            --Index;
            if (Remaining == 0)
                break;
            ++It;
        }
        if (It == Widgets.end())
            return nullptr;
        return It->second;
    }

    Win3D* Page::FindWidget(char const* Name)
    {
        // std::string key(Name); lower_bound (the routine); it == end or
        // key < it->first → end; return it == end ? 0 : it->second.
        std::map<std::string, Win3D*>::iterator It = Widgets.find(std::string(Name));
        if (It == Widgets.end())
            return nullptr;
        return It->second;
    }

    bool Page::Instantiate(FUiWidgetEntry** Entries, Win3D* Parent,
                           FTransform const* Matrix)
    {
        // (thiscall, RET 0xc).
        bool bResult = true;
        if (Entries == nullptr || *Entries == nullptr)
            return bResult;

        for (FUiWidgetEntry** Cursor = Entries; *Cursor != nullptr; ++Cursor)
        {
            FUiWidgetEntry* Entry = *Cursor;

            // the routine(InstanceName, TypeName, Parent, Payload).
            Win3D* Widget =
                WinFactoryCreate(Entry->InstanceName, Entry->TypeName, Parent, Entry->Payload);
            if (Widget == nullptr)
                return false;

            // Local = identity rotation, translation (Left, Top, 0).
            FTransform Local;
            Local.Rotation.D[0][0] = 1.0f;
            Local.Rotation.D[0][1] = 0.0f;
            Local.Rotation.D[0][2] = 0.0f;
            Local.Rotation.D[1][0] = 0.0f;
            Local.Rotation.D[1][1] = 1.0f;
            Local.Rotation.D[1][2] = 0.0f;
            Local.Rotation.D[2][0] = 0.0f;
            Local.Rotation.D[2][1] = 0.0f;
            Local.Rotation.D[2][2] = 1.0f;
            Local.Translation.X = Entry->Left;
            Local.Translation.Y = Entry->Top;
            Local.Translation.Z = 0.0f;

            if (G_UiPageMathEngine == nullptr)
                G_UiPageMathEngine = Acquire3DMathEngine();

            // vftable+0x1c MulTransform(Out, *Matrix, Local).
            FTransform Placed;
            G_UiPageMathEngine->MulTransform(Placed, *Matrix, Local);

            // Widget->vftable+0x20 with a 12-dword copy of Out.
            FTransform PlacedCopy = Placed;
            Widget->SetTransform(reinterpret_cast<float const*>(&PlacedCopy));

            // insert(pair(string(InstanceName), 0)) then it->second = Widget.
            std::pair<std::map<std::string, Win3D*>::iterator, bool> Inserted =
                Widgets.insert(std::make_pair(std::string(Entry->InstanceName),
                                              static_cast<Win3D*>(nullptr)));
            Inserted.first->second = Widget;

            // — recursion result overwrites the running result every iteration.
            bResult = Instantiate(Entry->NestedChildren, Widget, Matrix);
        }
        return bResult;
    }

    Page* CreatePage(FUiPageData* Data, FTransform const* Matrix, Win3D* Parent)
    {
        // .
        Page* NewPage = new Page();

        // — delete any mapped widget and erase the whole map. On a
        // freshly constructed page the map is empty, so both loops fall through.
        for (std::map<std::string, Win3D*>::iterator It = NewPage->Widgets.begin();
             It != NewPage->Widgets.end(); ++It)
        {
            if (It->second != nullptr)
                delete It->second;
        }
        NewPage->Widgets.clear();

        NewPage->Aspect = Data->Aspect;
        if (!NewPage->Instantiate(Data->Entries, Parent, Matrix))
        {
            delete NewPage;
            return nullptr;
        }
        return NewPage;
    }

    MasterWin::MasterWin(FUiPageData* Data)
        : Win3D()
        , PageData(Data)
        , PageObject(nullptr)
    {
        // (RET 4): the inline Win3D ctor (Win3D(), the routine's stores and
        // ++the field), the IUIPage (+0x7c) and Event::Handler (+0x80) faces, then
        // +0x84 = Data, +0x88 = 0. Nothing is registered with the Event registry here.
    }

    MasterWin::~MasterWin()
    {
        // if (+0x88) { delete Page; +0x88 = 0; }
        if (PageObject != nullptr)
        {
            delete PageObject;
            PageObject = nullptr;
        }
        // Then the routine on the +0x80 face (~EventHandler) and the inline Win3D dtor
        // (--the field, flags &= 0xfc, children slot 10, the routine = ~Win3D) -- the base
        // destructors run in that order after this body.
    }

    bool MasterWin::OnEvent(unsigned /*MsgId*/)
    {
        // vftable 0x005e5364 slot 0 = the routine: XOR AL,AL / RET 4.
        return false;
    }

    bool MasterWin::Create(Win3D* NewParent, void* /*Payload*/)
    {
        // (RET 8).
        Win3DReparent(this, NewParent);
        Win3DRegister(this);
        Flags = static_cast<unsigned char>((Flags & 0xfb) | kWin3DActive | kWin3DVisible);

        if (PageData == nullptr)
            return false;

        // → &the field; vftable+0x38 Unproject.
        ICamera* Camera = AsCamera(GetWin3DSceneCamera());
        float TopLeft[3];
        Camera->Unproject(TopLeft, 0.0f, 0.0f);
        float BottomRight[3];
        // FILD the field / the field — the created surface size.
        Camera->Unproject(BottomRight,
                          static_cast<float>(static_cast<int>(G_RenderSurfaceWidth)),
                          static_cast<float>(static_cast<int>(G_RenderSurfaceHeight)));

        float const Height = TopLeft[1] - BottomRight[1];

        FTransform Frame;
        Frame.Rotation.D[0][0] = Height;
        Frame.Rotation.D[0][1] = 0.0f;
        Frame.Rotation.D[0][2] = 0.0f;
        Frame.Rotation.D[1][0] = 0.0f;
        Frame.Rotation.D[1][1] = Height;
        Frame.Rotation.D[1][2] = 0.0f;
        Frame.Rotation.D[2][0] = 0.0f;
        Frame.Rotation.D[2][1] = 0.0f;
        Frame.Rotation.D[2][2] = 1.0f;
        Frame.Translation.X = 0.0f;
        Frame.Translation.Y = 0.0f;
        Frame.Translation.Z = FloatFromBits(kMasterWinDepthBiasBits) + TopLeft[2];

        PageObject = CreatePage(PageData, &Frame, this);
        return PageObject != nullptr;
    }

    void MasterWin::Destroy()
    {
        // (slot 10).
        if (PageObject != nullptr)
        {
            delete PageObject;
            PageObject = nullptr;
        }
        Flags = static_cast<unsigned char>(Flags & 0xfc);
        for (Win3D* Child = ChildHead; Child != nullptr; Child = Child->NextSibling)
            Child->Destroy();
        Win3DUnregister(this);
    }

    unsigned MasterWin::GetCount()
    {

        if (PageObject == nullptr)
            return 0;
        return PageObject->GetCount();
    }

    char const* MasterWin::GetWidgetName(unsigned Index)
    {

        if (PageObject == nullptr)
            return nullptr;
        return PageObject->GetWidgetName(Index);
    }

    Win3D* MasterWin::GetWidget(unsigned Index)
    {

        if (PageObject == nullptr)
            return nullptr;
        return PageObject->GetWidget(Index);
    }

    Win3D* MasterWin::FindWidget(char const* Name)
    {

        if (PageObject == nullptr)
            return nullptr;
        return PageObject->FindWidget(Name);
    }
}
