// Author: Giperion.

#include "win3d.h"
#ifdef OUTPOST_FRONTIER_WALK_MODE
#include "base_walk.h"
#endif

#include "client_cliobj.h"   // ResolveMaterialEntryFromFs (the routine) / ReleaseMaterialEntrySlot
#include "client_mesh_load.h"
#include "engine_light.h"
#include "hud_command_dispatch.h"
#include "hud_widescreen.h"    // port-only (OUTPOST_FRONTIER_WIDESCREEN_SUPPORT)
#include "event_handler.h"
#include "input_manager.h"
#include "nn_rollover.h"
#include "nn_tooltip.h"
#include "nn_ui_pulse.h"
#include "space_camera.h"
#include "space_scene_obj_draw.h" // SpaceCameraSetNearFar (the routine)
#include "win3d_engine_submit.h"
#include "win3d_immediate.h"
#include "win3d_render.h"
#include "win3d_renderables.h"
#include "wincontrol_base.h"

#include "common_csys.h"
#include "NeuronRuntime.h"
#include "readfile.h"             // IFileSystem::CreateInstance (the "FileSystem" instance)
#include "render/IRenderPipeline.h"
#include "render/shading.h"
#include "x86math.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

// ---------------------------------------------------------------------------------------
// Manager state (Win3D.cpp .data / .bss)
// ---------------------------------------------------------------------------------------

namespace
{
    // 's list: a count and a doubly linked chain of 0xc-byte nodes
    // { prev +0, next +4, payload +8 }.
    struct FWin3DListNode
    {
        FWin3DListNode* Prev;
        FWin3DListNode* Next;
        void* Payload;
    };

    struct FWin3DList
    {
        int Count;            // +0
        FWin3DListNode* Head; // +4
        FWin3DListNode* Tail; // +8

        // The atexit bodies the branch (registry) and the branch (pending list), one code
        // shape: walk from Head freeing each node (the routine, the next link read first),
        // then Count = 0, Tail = 0, Head = 0 in that order. The payloads are not touched.
        ~FWin3DList()
        {
            FWin3DListNode* Node = Head;
            while (Node != nullptr)
            {
                FWin3DListNode* const Next = Node->Next;
                operator delete(Node);
                Node = Next;
            }
            Count = 0;
            Tail = nullptr;
            Head = nullptr;
        }
    };

    // / dbe8 / dbec -- the registry, payload = Win3D*. Static init the branch
    // zeroes it; atexit the branch frees the nodes (~FWin3DList).
    FWin3DList G_Registry = {0, nullptr, nullptr};
    // / dbdc / dbe0 -- nodes queued for removal, payload = registry node.
    // Static init the branch, atexit the branch (~FWin3DList).
    FWin3DList G_PendingRemove = {0, nullptr, nullptr};

    // keyboard focus, the field capture stack head (link at +0x38),
    // object under the mouse.
    Win3D* G_KeyFocus = nullptr;
    Win3D* G_CaptureHead = nullptr;
    Win3D* G_Hover = nullptr;

    // / d0 / d4 -- the mouse ray. the branch seeds (0, 0, -1); the producer is
    // the Win3D input handler the routine case 2.
    float G_MouseRay[3] = {0.0f, 0.0f, -1.0f};

    // how many registry nodes the last the routine walked.
    int G_UpdateNodeCount = 0;
    // read by the routine (JG skips the whole draw). No writer exists in
    // the binary (XREF[1]: 0059e30a R), so it stays zero.
    int G_DrawSuppressCount = 0;

    // (thiscall on the list, RET 4): operator_new(0xc); a null allocation still
    // runs the tail with node 0, as baseline does.
    bool ListPushBack(FWin3DList* List, void* Payload)
    {
        FWin3DListNode* Node = static_cast<FWin3DListNode*>(operator new(sizeof(FWin3DListNode)));
        if (Node != nullptr)
        {
            Node->Next = nullptr;
            Node->Prev = nullptr;
            Node->Payload = Payload;
        }
        if (List->Tail != nullptr)
            List->Tail->Next = Node;
        else
            List->Head = Node;
        Node->Prev = List->Tail;
        List->Tail = Node;
        Node->Next = nullptr;
        List->Count = List->Count + 1;
        return true;
    }

    // (thiscall on a LinkTree's child-head field, RET 4): unlink Child from the
    // sibling chain starting at *ChildHeadField. Silently returns when it is not there.
    void LinkTreeRemoveChild(Win3D** ChildHeadField, Win3D* Child)
    {
        Win3D* Walk = *ChildHeadField;
        Win3D* Prev = nullptr;
        if (Walk == nullptr)
            return;
        while (Walk != Child)
        {
            Prev = Walk;
            Walk = Walk->NextSibling;
            if (Walk == nullptr)
                return;
        }
        if (Prev != nullptr)
            Prev->NextSibling = Walk->NextSibling;
        else
            *ChildHeadField = Walk->NextSibling;
    }

    // The unlink-and-free of one registry node, repeated inline four times in baseline
    // (0059d6f0.., 0059d808.., 0059e1f0.., 0059e280..).
    void RegistryEraseNode(FWin3DListNode* Node)
    {
        FWin3DListNode* const Prev = Node->Prev;
        FWin3DListNode* const Next = Node->Next;
        Node->Payload = nullptr;
        operator delete(Node);
        if (Prev != nullptr)
            Prev->Next = Next;
        else
            G_Registry.Head = Next;
        if (Next != nullptr)
            Next->Prev = Prev;
        else
            G_Registry.Tail = Prev;
        G_Registry.Count = G_Registry.Count - 1;
    }

    // / 0059e272..0059e2ef: erase every registry node the pending list
    // names, free the pending nodes, then clear the pending list header.
    void DrainPendingRemove()
    {
        for (FWin3DListNode* Pending = G_PendingRemove.Head; Pending != nullptr;
             Pending = Pending->Next)
        {
            FWin3DListNode* const Node = static_cast<FWin3DListNode*>(Pending->Payload);
            if (Node != nullptr)
                RegistryEraseNode(Node);
        }
        FWin3DListNode* Pending = G_PendingRemove.Head;
        while (Pending != nullptr)
        {
            FWin3DListNode* const Next = Pending->Next;
            operator delete(Pending);
            Pending = Next;
        }
        G_PendingRemove.Count = 0;
        G_PendingRemove.Tail = nullptr;
        G_PendingRemove.Head = nullptr;
    }

    // The sorted draw used by the routine (roots) and the routine / the routine (children):
    // zero layer draws at once, the rest go into a 0x400-entry stack array ordered by the
    // unsigned layer, then by +0x68 with an ordered-less compare (TEST AH,5 / JNP), equal keys
    // keeping insertion order. baseline checks the capacity only on the append path
    // (0059e656 / 0059e479 / 0059e781); the insertion path writes past the array when it is
    // full, and so does the port (the extra bound it used to have was removed 2026-09-30).
    void InsertSorted(Win3D** Sorted, unsigned* Count, Win3D* Object)
    {
        unsigned const Layer = Object->DrawLayer;
        unsigned const Size = *Count;
        if (Size == 0 || Layer > Sorted[Size - 1]->DrawLayer)
        {
            if (Size >= 0x400)
                return;
            Sorted[Size] = Object;
            *Count = Size + 1;
            return;
        }

        unsigned InsertAt = 0;
        while (InsertAt < Size)
        {
            Win3D* const Other = Sorted[InsertAt];
            if (Layer < Other->DrawLayer)
                break;
            if (Layer == Other->DrawLayer && Object->Position[2] < Other->Position[2])
                break;
            InsertAt = InsertAt + 1;
        }
        // no capacity check on the insertion path (baseline bounds only
        // the append at 0x0059e656); more than 0x400 visible layered children of one parent
        // would overrun the stack array in baseline too.
        if (Size - InsertAt != 0)
            memmove(&Sorted[InsertAt + 1], &Sorted[InsertAt], (Size - InsertAt) * sizeof(Win3D*));
        Sorted[InsertAt] = Object;
        *Count = Size + 1;
    }

    // Body shared by the routine and the routine.
#ifdef OUTPOST_FRONTIER_WIDESCREEN_SUPPORT
    // Port-only (hud_widescreen.h): every draw, update, hit test and mouse call on an object
    // runs with the Win3D camera shifted for that object's HUD corner window (no-op for
    // everything else, and on a 4:3 surface).
    void DrawWidescreen(Win3D* Object)
    {
        FWidescreenHudShiftScope const Scope(WidescreenHudShiftPixelsFor(Object), true);
        Object->Draw();
    }

    void UpdateWidescreen(Win3D* Object)
    {
        FWidescreenHudShiftScope const Scope(WidescreenHudShiftPixelsFor(Object), false);
        if ((Object->Flags & kWin3DActive) != 0)
            Object->Update();
        else
            Object->UpdateInactive();
    }

    // Ray is the frame's mouse ray (the field); a shifted object is tested against the
    // cursor's ray through the shifted camera instead.
    bool HitTestWidescreen(Win3D* Object, float const* Ray)
    {
        float const Shift = WidescreenHudShiftPixelsFor(Object);
        if (Shift == WidescreenHudCurrentShift())
            return Object->HitTest(Ray);
        FWidescreenHudShiftScope const Scope(Shift, false);
        float Shifted[3];
        return Object->HitTest(Win3DGetMouseRay(Shifted));
    }

    enum class EMouseCall
    {
        Enter,
        Over,
        Leave
    };

    void MouseCallWidescreen(Win3D* Object, EMouseCall Call)
    {
        FWidescreenHudShiftScope const Scope(WidescreenHudShiftPixelsFor(Object), false);
        float Ray[3];
        Win3DGetMouseRay(Ray);
        if (Call == EMouseCall::Enter)
            Object->MouseEnter(Ray);
        else if (Call == EMouseCall::Over)
            Object->MouseOver(Ray);
        else
            Object->MouseLeave(Ray);
    }

    bool MouseEventWidescreen(Win3D* Object, FWin3DMouseEvent const* Event)
    {
        FWidescreenHudShiftScope const Scope(WidescreenHudShiftPixelsFor(Object), false);
        return Object->MouseEvent(Event);
    }

    bool WheelWidescreen(Win3D* Object, int Wheel)
    {
        FWidescreenHudShiftScope const Scope(WidescreenHudShiftPixelsFor(Object), false);
        return Object->Slot18(Wheel);
    }
#endif

    void DrawChildrenSorted(Win3D* Parent)
    {
        Win3D* Sorted[0x400];
        unsigned Count = 0;
        for (Win3D* Child = Parent->ChildHead; Child != nullptr; Child = Child->NextSibling)
        {
            if ((Child->Flags & kWin3DVisible) == 0)
                continue;
            if (Child->DrawLayer == 0)
            {
#ifdef OUTPOST_FRONTIER_WIDESCREEN_SUPPORT
                DrawWidescreen(Child);
#else
                Child->Draw();
#endif
                continue;
            }
            InsertSorted(Sorted, &Count, Child);
        }
        for (unsigned Index = 0; Index < Count; ++Index)
#ifdef OUTPOST_FRONTIER_WIDESCREEN_SUPPORT
            DrawWidescreen(Sorted[Index]);
#else
            Sorted[Index]->Draw();
#endif
    }

    char const* const kWin3DSourceFile = "Client\\Interface\\Win3D.cpp";
}

int G_Win3DLiveCount = 0;

// nesting count raised by the modal NN dialogs' ctors (004746db, 00474956,
// ) and lowered by their dtors (004721ea, 0047256a); while it is non-zero
// updates only the WinControl holding the mouse.
int G_ModalCaptureCount = 0;

// ---------------------------------------------------------------------------------------
// Win3D
// ---------------------------------------------------------------------------------------

Win3D::Win3D()
{
    // . Name[1..31], Pad6d and the upper flag bits are left as allocated.
    ChildHead = nullptr;
    NextSibling = nullptr;
    Parent = nullptr;
    NameLength = 0;
    Name[0] = '\0';
    Orientation[7] = 0.0f;
    Orientation[6] = 0.0f;
    Orientation[5] = 0.0f;
    Orientation[3] = 0.0f;
    Orientation[2] = 0.0f;
    Orientation[1] = 0.0f;
    Orientation[8] = 1.0f;
    Orientation[4] = 1.0f;
    Orientation[0] = 1.0f;
    Position[2] = 0.0f;
    Position[1] = 0.0f;
    Position[0] = 0.0f;
    ClassName = nullptr;
    DrawLayer = 0;
    CaptureNext = nullptr;
    HitPriority = 0;
    DrawSubLayer = 0;
    Flags = static_cast<unsigned char>((Flags & 0xf8) | kWin3DPickable);
    G_Win3DLiveCount = G_Win3DLiveCount + 1;
}

Win3D::~Win3D()
{
    // --the field, flags &= 0xfc, slot 10 on every child (children are not
    // deleted here), the routine(this).
    G_Win3DLiveCount = G_Win3DLiveCount - 1;
    Flags = static_cast<unsigned char>(Flags & 0xfc);
    for (Win3D* Child = ChildHead; Child != nullptr; Child = Child->NextSibling)
        Child->Destroy();
    Win3DUnregister(this);
}

bool Win3D::Slot1(unsigned /*A*/, unsigned /*B*/)
{
    // XOR AL,AL / RET 8.
    return false;
}

WinControl* Win3D::AsWinControl()
{
    // XOR EAX,EAX / RET.
    return nullptr;
}

bool Win3D::IsDescendantOf(Win3D* Ancestor)
{
    // parent == Ancestor → true; parent → tail-jump to parent's slot 3.
    Win3D* const Up = Parent;
    if (Up == Ancestor)
        return true;
    if (Up != nullptr)
        return Up->IsDescendantOf(Ancestor);
    return false;
}

bool Win3D::HitTest(float const* /*Ray*/)
{
    // XOR AL,AL / RET 4.
    return false;
}

void Win3D::SetPosition(float const* NewPosition)
{
    // three dword copies to +0x60.
    Position[0] = NewPosition[0];
    Position[1] = NewPosition[1];
    Position[2] = NewPosition[2];
}

bool Win3D::Slot6(unsigned /*A*/)
{
    // MOV AL,1 / RET 4.
    return true;
}

void Win3D::SetOrientation(float const* NewOrientation)
{
    // REP MOVSD, nine dwords to +0x3c.
    memmove(Orientation, NewOrientation, sizeof(Orientation));
}

void Win3D::SetTransform(float const* Transform)
{
    // virtual slot 7 (Transform), then virtual slot 5 (Transform + 9 floats).
    SetOrientation(Transform);
    SetPosition(Transform + 9);
}

bool Win3D::Create(Win3D* NewParent, void* /*Data*/)
{
    // (RET 8): the routine(this, NewParent), the routine(this),
    // flags = (flags & 0xfb) | 3, return 1.
    Win3DReparent(this, NewParent);
    Win3DRegister(this);
    Flags = static_cast<unsigned char>((Flags & 0xfb) | kWin3DActive | kWin3DVisible);
    return true;
}

void Win3D::Destroy()
{
    // .
    Flags = static_cast<unsigned char>(Flags & 0xfc);
    for (Win3D* Child = ChildHead; Child != nullptr; Child = Child->NextSibling)
        Child->Destroy();
    Win3DUnregister(this);
}

void Win3D::Update()
{
    // the next sibling is read before each call.
    Win3D* Child = ChildHead;
    while (Child != nullptr)
    {
        Win3D* const Next = Child->NextSibling;
#ifdef OUTPOST_FRONTIER_WIDESCREEN_SUPPORT
        UpdateWidescreen(Child);
#else
        if ((Child->Flags & kWin3DActive) != 0)
            Child->Update();
        else
            Child->UpdateInactive();
#endif
        Child = Next;
    }
}

void Win3D::UpdateInactive()
{
    // the next sibling is read after each call.
    for (Win3D* Child = ChildHead; Child != nullptr; Child = Child->NextSibling)
    {
        if ((Child->Flags & kWin3DActive) == 0)
        {
#ifdef OUTPOST_FRONTIER_WIDESCREEN_SUPPORT
            UpdateWidescreen(Child);
#else
            Child->UpdateInactive();
#endif
        }
    }
}

void Win3D::Draw()
{
    // .
    Win3DDrawChildren(this);
}

void Win3D::MouseOver(float const* /*Ray*/)
{
    // RET 4.
}

void Win3D::MouseEnter(float const* /*Ray*/)
{
    // RET 4.
}

void Win3D::MouseLeave(float const* /*Ray*/)
{
    // RET 4.
}

void Win3D::Slot17()
{
    // RET.
}

bool Win3D::Slot18(int /*Value*/)
{
    // XOR AL,AL / RET 4.
    return false;
}

bool Win3D::MouseEvent(FWin3DMouseEvent const* Event)
{
    // parent → tail-jump to its slot 19 with the same argument; else false.
    Win3D* const Up = Parent;
    if (Up != nullptr)
        return Up->MouseEvent(Event);
    return false;
}

bool Win3D::KeyEvent(unsigned /*Key*/, unsigned char* /*Modifiers*/)
{
    // XOR AL,AL / RET 8.
    return false;
}

bool Win3D::Slot21(unsigned /*A*/, unsigned /*B*/, unsigned /*C*/, unsigned /*D*/)
{
    // XOR AL,AL / RET 0x10.
    return false;
}

bool Win3D::Slot22(unsigned /*A*/, unsigned /*B*/)
{
    // XOR AL,AL / RET 8.
    return false;
}

bool Win3D::Slot23(unsigned /*Msg*/, unsigned /*A*/, unsigned /*B*/)
{
    // XOR AL,AL / RET 0xc.
    return false;
}

void Win3D::Slot24(unsigned A)
{
    // slot 24(A) on every child.
    for (Win3D* Child = ChildHead; Child != nullptr; Child = Child->NextSibling)
        Child->Slot24(A);
}

float Win3D::Slot25()
{
    // FLD the field (1.0f).
    return 1.0f;
}

float Win3D::Slot26()
{
    // (the same body).
    return 1.0f;
}

int Win3D::GetHitPriority()
{
    // .
    return HitPriority;
}

void Win3D::SetName(char const* NewName)
{
    // .
    if (NewName == nullptr)
    {
        NameLength = 0;
        Name[0] = '\0';
        return;
    }
    unsigned Length = static_cast<unsigned>(strlen(NewName));
    if (Length >= 0x20)
        Length = 0x1f;
    memcpy(Name, NewName, Length);
    Name[Length] = '\0';
    NameLength = Length;
}

// ---------------------------------------------------------------------------------------
// Manager
// ---------------------------------------------------------------------------------------

void Win3DSetKeyFocus(Win3D* Focus)
{
    // .
    G_KeyFocus = Focus;
}

void Win3DPushCapture(Win3D* Object)
{
    // . A non-zero +0x38 means the object may already be on the stack: find it
    // (keeping the previous entry) and move it to the head. A zero +0x38 means it is either
    // the tail or absent -- either way it is pushed without a search, as baseline does.
    Win3D* const OldNext = Object->CaptureNext;
    if (OldNext == nullptr)
    {
        Object->CaptureNext = G_CaptureHead;
        G_CaptureHead = Object;
        return;
    }

    Win3D* const Head = G_CaptureHead;
    Win3D* Walk = Head;
    Win3D* Prev = nullptr;
    while (Walk != nullptr)
    {
        if (Walk == Object)
        {
            if (Prev != nullptr)
            {
                Prev->CaptureNext = OldNext;
                Object->CaptureNext = nullptr;
                Object->CaptureNext = G_CaptureHead;
                G_CaptureHead = Object;
                return;
            }
            G_CaptureHead = OldNext;
            Object->CaptureNext = nullptr;
            Object->CaptureNext = G_CaptureHead;
            G_CaptureHead = Object;
            return;
        }
        Prev = Walk;
        Walk = Walk->CaptureNext;
    }
    Object->CaptureNext = Head;
    G_CaptureHead = Object;
}

void Win3DRemoveCapture(Win3D* Object)
{
    // .
    Win3D* Walk = G_CaptureHead;
    Win3D* Prev = nullptr;
    while (Walk != nullptr)
    {
        if (Walk == Object)
        {
            if (Prev != nullptr)
            {
                Prev->CaptureNext = Object->CaptureNext;
                Object->CaptureNext = nullptr;
                return;
            }
            G_CaptureHead = Object->CaptureNext;
            Object->CaptureNext = nullptr;
            return;
        }
        Prev = Walk;
        Walk = Walk->CaptureNext;
    }
}

Win3D* Win3DGetCapture()
{
    // .
    return G_CaptureHead;
}

Win3D* Win3DFindByClassName(char const* ClassName)
{
    // nodes with a null payload or a null +0x10 are skipped.
    for (FWin3DListNode* Node = G_Registry.Head; Node != nullptr; Node = Node->Next)
    {
        Win3D* const Object = static_cast<Win3D*>(Node->Payload);
        if (Object == nullptr || Object->ClassName == nullptr)
            continue;
        if (_stricmp(Object->ClassName, ClassName) == 0)   // NEURONCOMPONENTS.DLL::stricmp
            return Object;
    }
    return nullptr;
}

Win3D* Win3DFindByName(char const* Name)
{
    // .
    for (FWin3DListNode* Node = G_Registry.Head; Node != nullptr; Node = Node->Next)
    {
        Win3D* const Object = static_cast<Win3D*>(Node->Payload);
        if (Object == nullptr)
            continue;
        if (_stricmp(Object->Name, Name) == 0)             // NEURONCOMPONENTS.DLL::stricmp
            return Object;
    }
    return nullptr;
}

Win3D* Win3DGetFirstRegistered()
{
    // .
    if (G_Registry.Head != nullptr)
        return static_cast<Win3D*>(G_Registry.Head->Payload);
    return nullptr;
}

void Win3DForEachRegistered(void (*Visit)(Win3D* Object, void* Context), void* Context)
{
    // Oracle instrumentation (P0.5); no baseline counterpart.
    for (FWin3DListNode* Node = G_Registry.Head; Node != nullptr; Node = Node->Next)
    {
        Win3D* const Object = static_cast<Win3D*>(Node->Payload);
        if (Object != nullptr)
            Visit(Object, Context);
    }
}

float* Win3DGetMouseRay(float* Out)
{
    // .
#ifdef OUTPOST_FRONTIER_WIDESCREEN_SUPPORT
    // Port-only: inside a shifted HUD window the ray comes through the shifted camera.
    if (WidescreenHudCurrentShift() != 0.0f)
    {
        WidescreenHudCursorRay(Out);
        return Out;
    }
#endif
    Out[0] = G_MouseRay[0];
    Out[1] = G_MouseRay[1];
    Out[2] = G_MouseRay[2];
    return Out;
}

float* Win3DMouseRayStorage()
{
    return G_MouseRay;
}

FSpaceCamera* Win3DGetCamera()
{
    // MOV EAX, &the field. The port's the field is G_Win3DSceneCamera, built
    // at static init as the branch builds it (win3d_engine_submit.cpp).
    return GetWin3DSceneCamera();
}

void Win3DSetViewportSize(int Width, int Height)
{
    // (cdecl w, h): {0, 0, w - 1, h - 1} into the routine on the field with a
    // second argument of 1 (dead in the callee).
    int ViewRect[4];
    ViewRect[0] = 0;
    ViewRect[1] = 0;
    ViewRect[2] = Width - 1;
    ViewRect[3] = Height - 1;
    SpaceCameraSetViewportEx(Win3DGetCamera(), ViewRect, 1);
}

Win3D* Win3DGetHover()
{
    // .
    return G_Hover;
}

void Win3DRegister(Win3D* Object)
{
    // .
    ListPushBack(&G_Registry, Object);
}

bool Win3DUnregister(Win3D* Object)
{
    // .
    if ((Object->Flags & kWin3DUnregistered) != 0)
        return true;
    Object->Flags = static_cast<unsigned char>(Object->Flags | kWin3DUnregistered);
    if (G_KeyFocus == Object)
        G_KeyFocus = nullptr;
    if (G_CaptureHead == Object)
        G_CaptureHead = nullptr;
    if (G_Hover == Object)
        G_Hover = nullptr;

    FWin3DListNode* Node = G_Registry.Head;
    while (Node != nullptr && Node->Payload != Object)
        Node = Node->Next;
    if (Node == nullptr || Node->Payload == nullptr)
        return false;

    // the routine inlined on the parent's child list, then +4 = 0.
    Win3D* const Up = Object->Parent;
    if (Up != nullptr)
    {
        LinkTreeRemoveChild(&Up->ChildHead, Object);
        Object->Parent = nullptr;
    }
    ListPushBack(&G_PendingRemove, Node);
    Node->Payload = nullptr;
    return true;
}

void Win3DReparent(Win3D* Child, Win3D* NewParent)
{
    // (cdecl).
    if (Child == nullptr)
        return;
    Win3D* const OldParent = Child->Parent;
    if (OldParent == NewParent)
        return;
    if (OldParent != nullptr)
        LinkTreeRemoveChild(&OldParent->ChildHead, Child);
    Child->Parent = NewParent;
    if (NewParent == nullptr)
        return;
    Win3D* Tail = NewParent->ChildHead;
    if (Tail == nullptr)
    {
        Child->NextSibling = nullptr;
        NewParent->ChildHead = Child;
        return;
    }
    while (Tail->NextSibling != nullptr)
        Tail = Tail->NextSibling;
    Tail->NextSibling = Child;
    Child->NextSibling = nullptr;
}

bool Win3DDispatchKey(FHudCmdRecord const* Record)
{
    // (cdecl, one argument). The modifier byte is built in the argument's own stack
    // slot, so bits 5..7 are whatever the low byte of the Record pointer holds
    // (0059dfd3 MOV AL,[ESP+param_1] / AND AL,0xe0); only bits 0..4 are written:
    //   bit0 = (+0x18 == 0), bit1 = +0x18 bit0, bit2 = (+0x08 & 3) != 0,
    //   bit3 = (+0x08 & 0xc) != 0, bit4 = (+0x08 & 0x30) != 0.
    unsigned char Modifiers =
        static_cast<unsigned char>(reinterpret_cast<uintptr_t>(Record) & 0xe0);
    unsigned char const Byte18 = Record->Byte18;
    Modifiers = static_cast<unsigned char>(Modifiers | ((Byte18 & 1) << 1));
    if (Byte18 == 0)
        Modifiers = static_cast<unsigned char>(Modifiers | 0x01);
    unsigned const Shift = Record->Modifiers;
    if ((Shift & 0x03) != 0)
        Modifiers = static_cast<unsigned char>(Modifiers | 0x04);
    if ((Shift & 0x0c) != 0)
        Modifiers = static_cast<unsigned char>(Modifiers | 0x08);
    if ((Shift & 0x30) != 0)
        Modifiers = static_cast<unsigned char>(Modifiers | 0x10);
    unsigned const Key = Record->Key;

    // the focus object alone gets the key, and its answer is returned.
    if (G_KeyFocus != nullptr)
        return G_KeyFocus->KeyEvent(Key, &Modifiers);

    // otherwise every registered object in order until one takes it.
    bool bHandled = false;
    for (FWin3DListNode* Node = G_Registry.Head; Node != nullptr; Node = Node->Next)
    {
        Win3D* const Object = static_cast<Win3D*>(Node->Payload);
        if (Object == nullptr)
            continue;
        bHandled = Object->KeyEvent(Key, &Modifiers) || bHandled;
        if (bHandled)
            break;
    }
    return bHandled;
}

Win3D* Win3DPick(float const* Ray)
{
    // .
    Win3D* Best = nullptr;
    int BestDepth = -1;
    for (FWin3DListNode* Node = G_Registry.Head; Node != nullptr; Node = Node->Next)
    {
        Win3D* const Object = static_cast<Win3D*>(Node->Payload);
        if (Object == nullptr)
            continue;
        unsigned char const ObjectFlags = Object->Flags;
        if ((ObjectFlags & kWin3DActive) == 0 || (ObjectFlags & kWin3DVisible) == 0 ||
            (ObjectFlags & kWin3DPickable) == 0)
            continue;

        // Every ancestor (and the object itself) must be visible.
        bool bVisibleChain = true;
        for (Win3D* Walk = Object; Walk != nullptr; Walk = Walk->Parent)
        {
            if ((Walk->Flags & kWin3DVisible) == 0)
            {
                bVisibleChain = false;
                break;
            }
        }
        if (!bVisibleChain)
            continue;

        int Depth = Object->GetHitPriority();
        if (Depth == 0)
        {
            for (Win3D* Walk = Object; Walk != nullptr; Walk = Walk->Parent)
                Depth = Depth + 1;
        }
        if (Depth > BestDepth)
        {
#ifdef OUTPOST_FRONTIER_WIDESCREEN_SUPPORT
            if (HitTestWidescreen(Object, Ray))
#else
            if (Object->HitTest(Ray))
#endif
            {
                Best = Object;
                BestDepth = Depth;
            }
            continue;
        }
        if (Depth != BestDepth)
            continue;
        // FCOMP / TEST AH,0x41 / JNZ -- only a strictly greater +0x68 competes.
        if (!(Object->Position[2] > Best->Position[2]))
            continue;
#ifdef OUTPOST_FRONTIER_WIDESCREEN_SUPPORT
        if (HitTestWidescreen(Object, Ray))
#else
        if (Object->HitTest(Ray))
#endif
        {
            Best = Object;
            BestDepth = Depth;
        }
    }
    return Best;
}

Win3D* Win3DPickWithin(Win3D* Capture, float const* Ray)
{
    // (cdecl Capture, Ray). Unlike the routine the ancestor-visibility walk runs
    // only after the depth comparison, and only on the "deeper" branch.
    Win3D* Best = nullptr;
    int BestDepth = -1;
    for (FWin3DListNode* Node = G_Registry.Head; Node != nullptr; Node = Node->Next)
    {
        Win3D* const Object = static_cast<Win3D*>(Node->Payload);
        if (Object == nullptr)
            continue;
        unsigned char const ObjectFlags = Object->Flags;
        if ((ObjectFlags & kWin3DActive) == 0 || (ObjectFlags & kWin3DVisible) == 0 ||
            (ObjectFlags & kWin3DPickable) == 0)
            continue;
        if (!Object->IsDescendantOf(Capture) && Object != Capture)
            continue;

        int Depth = Object->GetHitPriority();
        if (Depth == 0)
        {
            for (Win3D* Walk = Object; Walk != nullptr; Walk = Walk->Parent)
                Depth = Depth + 1;
        }
        if (Depth > BestDepth)
        {
            bool bVisibleChain = true;
            for (Win3D* Walk = Object; Walk != nullptr; Walk = Walk->Parent)
            {
                if ((Walk->Flags & kWin3DVisible) == 0)
                {
                    bVisibleChain = false;
                    break;
                }
            }
            if (!bVisibleChain)
                continue;
#ifdef OUTPOST_FRONTIER_WIDESCREEN_SUPPORT
            if (HitTestWidescreen(Object, Ray))
#else
            if (Object->HitTest(Ray))
#endif
            {
                Best = Object;
                BestDepth = Depth;
            }
            continue;
        }
        if (Depth != BestDepth)
            continue;
        if (!(Object->Position[2] > Best->Position[2]))
            continue;
#ifdef OUTPOST_FRONTIER_WIDESCREEN_SUPPORT
        if (HitTestWidescreen(Object, Ray))
#else
        if (Object->HitTest(Ray))
#endif
        {
            Best = Object;
            BestDepth = Depth;
        }
    }
    return Best;
}

bool Win3DBroadcast(unsigned Msg, unsigned A, unsigned B)
{
    // .
    bool bHandled = false;
    for (FWin3DListNode* Node = G_Registry.Head; Node != nullptr; Node = Node->Next)
    {
        Win3D* const Object = static_cast<Win3D*>(Node->Payload);
        if (Object == nullptr)
            continue;
        bHandled = bHandled || Object->Slot23(Msg, A, B);
        if (bHandled)
            break;
    }
    return bHandled;
}

void Win3DSetHover(Win3D* Object)
{
    // . Leave/enter only on a change; slot 14 runs on every call with a
    // non-null Object.
#ifdef OUTPOST_FRONTIER_WIDESCREEN_SUPPORT
    if (G_Hover != Object)
    {
        if (G_Hover != nullptr)
            MouseCallWidescreen(G_Hover, EMouseCall::Leave);
        G_Hover = Object;
        if (Object != nullptr)
            MouseCallWidescreen(Object, EMouseCall::Enter);
    }
    if (Object != nullptr)
        MouseCallWidescreen(Object, EMouseCall::Over);
#else
    if (G_Hover != Object)
    {
        if (G_Hover != nullptr)
            G_Hover->MouseLeave(G_MouseRay);
        G_Hover = Object;
        if (Object != nullptr)
            Object->MouseEnter(G_MouseRay);
    }
    if (Object != nullptr)
        Object->MouseOver(G_MouseRay);
#endif
}

void Win3DUpdateAll(double DeltaSeconds)
{
    // (cdecl, double dt).
    NnUiPulseAdvance(DeltaSeconds);
    NnToolTipTickGlobal(DeltaSeconds);
    NnRolloverTickGlobal(DeltaSeconds);

    // the routine -- the software cursor is drawn.
    Win3D* Picked = nullptr;
    if (GetClientMouseCursorDrawFlag() != 0)
    {
        if (G_CaptureHead == nullptr)
        {
            Picked = Win3DPick(G_MouseRay);
        }
        else
        {
            Picked = Win3DPickWithin(G_CaptureHead, G_MouseRay);
            if (Picked == nullptr)
                Picked = G_CaptureHead;
        }
    }

    // the routine's body inlined, but slot 14 runs only on a change.
    if (G_Hover != Picked)
    {
#ifdef OUTPOST_FRONTIER_WIDESCREEN_SUPPORT
        if (G_Hover != nullptr)
            MouseCallWidescreen(G_Hover, EMouseCall::Leave);
        G_Hover = Picked;
        if (Picked != nullptr)
        {
            MouseCallWidescreen(Picked, EMouseCall::Enter);
            MouseCallWidescreen(Picked, EMouseCall::Over);
        }
#else
        if (G_Hover != nullptr)
            G_Hover->MouseLeave(G_MouseRay);
        G_Hover = Picked;
        if (Picked != nullptr)
        {
            Picked->MouseEnter(G_MouseRay);
            Picked->MouseOver(G_MouseRay);
        }
#endif
    }

    G_UpdateNodeCount = 0;
    if (G_ModalCaptureCount != 0)
    {
        // the first registered WinControl holding the mouse gets the only
        // update of the frame, then the pending list is drained.
        for (FWin3DListNode* Node = G_Registry.Head; Node != nullptr; Node = Node->Next)
        {
            Win3D* const Object = static_cast<Win3D*>(Node->Payload);
            if (Object == nullptr)
                continue;
            WinControl* const Control = Object->AsWinControl();
            if (Control == nullptr || Control->bCaptured == 0)
                continue;
#ifdef OUTPOST_FRONTIER_WIDESCREEN_SUPPORT
            UpdateWidescreen(Object);
#else
            if ((Object->Flags & kWin3DActive) != 0)
                Object->Update();
            else
                Object->UpdateInactive();
#endif
            DrainPendingRemove();
            return;
        }
    }

    // every registered root (+4 == 0), counting every node walked.
    for (FWin3DListNode* Node = G_Registry.Head; Node != nullptr; Node = Node->Next)
    {
        Win3D* const Object = static_cast<Win3D*>(Node->Payload);
        if (Object != nullptr && Object->Parent == nullptr)
        {
#ifdef OUTPOST_FRONTIER_WIDESCREEN_SUPPORT
            UpdateWidescreen(Object);
#else
            if ((Object->Flags & kWin3DActive) != 0)
                Object->Update();
            else
                Object->UpdateInactive();
#endif
        }
        G_UpdateNodeCount = G_UpdateNodeCount + 1;
    }
    DrainPendingRemove();
}

void Win3DBeginDraw()
{
    // .
    Win3DDisable(kWin3DLightingCap);   // 0xb50
    Win3DDisable(kWin3DTexture2D);     // 0xde1
    Win3DDisable(kWin3DCullFace);      // 0xb44
    Win3DDisable(kWin3DDepthTest);     // 0xb71
    Win3DDisable(kWin3DFog);           // 0xb60
    ICamera* const Camera = AsCamera(Win3DGetCamera());
    Win3DSetViewport(Camera, kWin3DSourceFile, 0x487);
    Win3DApplyCamera(Camera);                            // (&the field)
    NeuronRuntime::MaterialBatcher->SetBinOrder(1, -1);                // +0x20
    EngineLightClearRegisteredList();
    IRenderPipeline* const Pipe = NeuronRuntime::Pipeline;
    Pipe->SetRenderState(D3DRS_AMBIENT, -1);                      // +0xf0
    Pipe->SetRenderState(D3DRS_DITHERENABLE, 0);
}

void Win3DEndDraw()
{
    // .
    NeuronRuntime::MaterialBatcher->SetBinOrder(1, 1);
}

void Win3DDrawAll()
{
    // .
    if (G_DrawSuppressCount > 0)
        return;

    {
#ifdef OUTPOST_FRONTIER_WALK_MODE
        FBaseWalkInterfaceDraw InterfaceDraw;
#endif
        Win3DBeginDraw();          // , the same sequence as the routine
        Win3DImmedBatchReset();
        ButtonMontageEnsure();

        Win3D* Sorted[0x400];
        unsigned Count = 0;
        for (FWin3DListNode* Node = G_Registry.Head; Node != nullptr; Node = Node->Next)
        {
            Win3D* const Object = static_cast<Win3D*>(Node->Payload);
            if (Object == nullptr || Object->Parent != nullptr)
                continue;
            if ((Object->Flags & kWin3DVisible) == 0)
                continue;
            if (Object->DrawLayer == 0)
            {
#ifdef OUTPOST_FRONTIER_WIDESCREEN_SUPPORT
                DrawWidescreen(Object);
#else
                Object->Draw();
#endif
                continue;
            }
            InsertSorted(Sorted, &Count, Object);
        }

        for (int SubLayer = 0; SubLayer < 8; ++SubLayer)
        {
            for (unsigned Index = 0; Index < Count; ++Index)
            {
                if (Sorted[Index]->DrawSubLayer == SubLayer)
#ifdef OUTPOST_FRONTIER_WIDESCREEN_SUPPORT
                    DrawWidescreen(Sorted[Index]);
#else
                    Sorted[Index]->Draw();
#endif
            }
        }

        ButtonMontageFlushBatch();
        Win3DImmedBatchFlushAll();
        UiOverlayPrepPipeline();
        ImeFocusDrawPass();
    }
    MouseFrameUpdate();
    NeuronRuntime::MaterialBatcher->SetBinOrder(1, 1);
}

void Win3DDrawChildren(Win3D* Parent)
{
    // .
    DrawChildrenSorted(Parent);
}

void Win3DDrawChildrenOf(Win3D* Parent)
{
    // .
    DrawChildrenSorted(Parent);
}

void Win3DDeleteTree(Win3D* Root)
{
    // (cdecl). 0043d2a5 TEST EDI,EDI / JZ: a null root does nothing. Post-order:
    // the next sibling is read (0043d2b1) before the child's subtree goes, then slot 10
    // Destroy (0043d2c8) and the scalar deleting dtor with 1 (0043d2d1).
    if (Root == nullptr)
        return;
    Win3D* Child = Root->ChildHead;
    while (Child != nullptr)
    {
        Win3D* const Next = Child->NextSibling;
        Win3DDeleteTree(Child);
        Child = Next;
    }
    Root->Destroy();
    delete Root;
}

bool Win3DAnyCaptured()
{
    // .
    for (FWin3DListNode* Node = G_Registry.Head; Node != nullptr; Node = Node->Next)
    {
        Win3D* const Object = static_cast<Win3D*>(Node->Payload);
        if (Object == nullptr)
            continue;
        WinControl* const Control = Object->AsWinControl();
        if (Control != nullptr && Control->bCaptured != 0)
            return true;
    }
    return false;
}

// the material-cache entry of interface\interface.generic.vms (the routine
// fills it through the routine, the routine gives it back through the routine).
Archetype::FCliMaterialEntry* G_InterfaceGenericMaterial = nullptr;

void Win3DStartupManager(int Width, int Height)
{
    // (module storage-0059d6c1), cdecl(width, height); sole caller InitializeEngine
    // with the options object's size (the field / the field).
    FSpaceCamera* const WinCamera = Win3DGetCamera(); // &the field

    // the routine with the stack rect {0, 0, width - 1, height - 1}.
    int ViewRect[4];
    ViewRect[0] = 0;
    ViewRect[1] = 0;
    ViewRect[2] = Width - 1;
    ViewRect[3] = Height - 1;
    SpaceCameraSetViewport(WinCamera, ViewRect);
    // the routine(0.1f, 1000.0f) -- the field / the field.
    SpaceCameraSetNearFar(WinCamera, 0.1f, 1000.0f);
    // the routine with the float bits 0x41d9ba5e.
    {
        unsigned int const kHalfFovBits = 0x41d9ba5eu;
        float HalfFovX;
        memcpy(&HalfFovX, &kHalfFovBits, sizeof(HalfFovX));
        SpaceCameraSetHalfFovX(WinCamera, HalfFovX);
    }
    // the routine -- mouse.ini shapes and cursors.
    MouseInitCursors();
    // the routine -- both line batchers.
    Win3DImmedBatchCreateAll();

    // a "FileSystem" for interface\interface.generic.vms under
    // NeuronRuntime::Data (slot 3), the 0x34-byte description zeroed by REP STOSD and filled like every
    // other client call site.
    FFileSystemAggDesc Desc;
    memset(&Desc, 0, sizeof(Desc));
    Desc.Size = 0x34;
    Desc.ComponentName = "FileSystem";
    Desc.DataPath = "interface\\interface.generic.vms";
    Desc.Field10 = 0x80000000u;
    Desc.Field14 = 1;
    Desc.Field1C = 3;
    Desc.Field20 = 0x8000080u;
    Desc.Field30 = 0xffffffffu;
    void* FsOut = nullptr;
    if (static_cast<IFileSystem*>(NeuronRuntime::Data)->CreateInstance(reinterpret_cast<AGGDESC*>(&Desc),
                                                               &FsOut) >= 0)
    {
        // the routine(fs, path, 1, -1) -> the field.
        G_InterfaceGenericMaterial = Archetype::ResolveMaterialEntryFromFs(
            static_cast<IFileSystem*>(FsOut), Desc.DataPath, true, 0xffffffffu);
    }
    // the file system is released (slot 2) whenever the out pointer is
    // non-null, whatever the create returned.
    if (FsOut != nullptr)
        static_cast<IFileSystem*>(FsOut)->Release();
}

void Win3DShutdownManager()
{
    // , 0059d6d0..0059d8ae.
    G_KeyFocus = nullptr;
    G_CaptureHead = nullptr;

    // erase whatever is already pending.
    for (FWin3DListNode* Pending = G_PendingRemove.Head; Pending != nullptr;
         Pending = Pending->Next)
    {
        FWin3DListNode* const Node = static_cast<FWin3DListNode*>(Pending->Payload);
        if (Node != nullptr)
            RegistryEraseNode(Node);
    }
    FWin3DListNode* Pending = G_PendingRemove.Head;
    while (Pending != nullptr)
    {
        FWin3DListNode* const Next = Pending->Next;
        operator delete(Pending);
        Pending = Next;
    }
    G_PendingRemove.Count = 0;
    G_PendingRemove.Tail = nullptr;
    G_PendingRemove.Head = nullptr;

    // the routine inlined on every registered object (the next node is
    // read before the object is processed).
    FWin3DListNode* Node = G_Registry.Head;
    while (Node != nullptr)
    {
        FWin3DListNode* const Next = Node->Next;
        Win3D* const Object = static_cast<Win3D*>(Node->Payload);
        if (Object != nullptr && (Object->Flags & kWin3DUnregistered) == 0)
        {
            Object->Flags = static_cast<unsigned char>(Object->Flags | kWin3DUnregistered);
            if (G_KeyFocus == Object)
                G_KeyFocus = nullptr;
            if (G_CaptureHead == Object)
                G_CaptureHead = nullptr;
            if (G_Hover == Object)
                G_Hover = nullptr;
            FWin3DListNode* Found = G_Registry.Head;
            while (Found != nullptr && Found->Payload != Object)
                Found = Found->Next;
            if (Found != nullptr && Found->Payload != nullptr)
            {
                Win3D* const Up = Object->Parent;
                if (Up != nullptr)
                {
                    LinkTreeRemoveChild(&Up->ChildHead, Object);
                    Object->Parent = nullptr;
                }
                ListPushBack(&G_PendingRemove, Found);
                Found->Payload = nullptr;
            }
        }
        Node = Next;
    }

    // erase the pending registry nodes, then the routine frees the
    // pending list itself.
    for (FWin3DListNode* Drain = G_PendingRemove.Head; Drain != nullptr; Drain = Drain->Next)
    {
        FWin3DListNode* const Erase = static_cast<FWin3DListNode*>(Drain->Payload);
        if (Erase != nullptr)
            RegistryEraseNode(Erase);
    }
    FWin3DListNode* Free = G_PendingRemove.Head;
    while (Free != nullptr)
    {
        FWin3DListNode* const Next = Free->Next;
        operator delete(Free);
        Free = Next;
    }
    G_PendingRemove.Count = 0;
    G_PendingRemove.Tail = nullptr;
    G_PendingRemove.Head = nullptr;

    // free whatever registry nodes remain and clear the header.
    FWin3DListNode* Rest = G_Registry.Head;
    while (Rest != nullptr)
    {
        FWin3DListNode* const Next = Rest->Next;
        operator delete(Rest);
        Rest = Next;
    }
    G_Registry.Count = 0;
    G_Registry.Tail = nullptr;
    G_Registry.Head = nullptr;

    // the routine on the model cache the field.
    Win3DModelCacheClear();

    // the routine(&the field) gives back the interface material
    // entry, the routine releases the mouse device and the cursors, the routine destroys both
    // line batchers. The tail jump to the camera's the branch (a bare RET) does nothing.
    Archetype::ReleaseMaterialEntrySlot(&G_InterfaceGenericMaterial);
    MouseShutdown();
    Win3DImmedBatchDestroyAll();
}

// ---------------------------------------------------------------------------------------
// Input router the field
// ---------------------------------------------------------------------------------------

namespace
{
    // the routine's lazily acquired 3DMathEngine (the routine on first use).
    I3DMathEngine* G_TransformMathEngine = nullptr;

    // (cdecl Out, Matrix, Source): I3DMathEngine slot 0x2c into a local, copied out.
    void TransformByMatrix(FVector& Out, FMatrix const& Matrix, FVector const& Source)
    {
        if (G_TransformMathEngine == nullptr)
            G_TransformMathEngine = Acquire3DMathEngine();
        FVector Result;
        G_TransformMathEngine->TransformMatrix(Result, Matrix, Source);
        Out = Result;
    }

    // (thiscall on the camera; Out, ScreenX, ScreenY): the camera-space point on
    // the near plane under a screen pixel. Each product is rounded to float on its FSTP.
    void ScreenToNearPlane(FSpaceCamera const* Cam, FVector& Out, float ScreenX, float ScreenY)
    {
        double const ZNear = static_cast<double>(Cam->ZNear);
        Out.X = static_cast<float>((static_cast<double>(ScreenX) - Cam->ScreenCenterX) *
                                   (ZNear / Cam->XScale));
        Out.Y = static_cast<float>((static_cast<double>(ScreenY) - Cam->ScreenCenterY) *
                                   (ZNear / Cam->YScale));
        Out.Z = static_cast<float>(-ZNear);
    }

    // (thiscall on the vector): scale by 1 / |v|. There is no zero test: a zero
    // vector divides by zero exactly as baseline does.
    void NormalizeInPlace(FVector& V)
    {
        double const X = V.X;
        double const Y = V.Y;
        double const Z = V.Z;
        double const Inv = 1.0 / sqrt(X * X + Y * Y + Z * Z);
        V.X = static_cast<float>(Inv * V.X);
        V.Y = static_cast<float>(Inv * V.Y);
        V.Z = static_cast<float>(Inv * V.Z);
    }

    // The handler object at the field (vftable 0x005e5dcc, one slot). Its only virtual is
    // slot 0, so its vptr is exactly the FHudCmdHandler shape the routine calls through.
    class FWin3DInputHandler : public EventHandler
    {
    public:
        FWin3DInputHandler();
        ~FWin3DInputHandler();

        // (thiscall, RET 4).
        bool OnEvent(unsigned MsgId) override;

    };

    FWin3DInputHandler::FWin3DInputHandler()
    {
        // , run from the static initializer the branch: the twelve ids are laid
        // out on the stack in this order and registered one by one through the routine.
        static unsigned const kIds[12] = {5, 4, 6, 7, 8, 9, 10, 11, 12, 13, 1, 2};
        for (unsigned Index = 0; Index < 12; ++Index)
            RegisterEvent(kIds[Index]);
    }

    FWin3DInputHandler::~FWin3DInputHandler()
    {
        // , reached from the atexit thunk the branch: the routine erases every
        // registration of this handler; the Event::Handler base dtor the routine then repeats
        // the same erase (after the routine on the handler), which finds nothing left.
        UnregisterAllEvents();
        // follows through the EventHandler base destructor.
    }

    bool FWin3DInputHandler::OnEvent(unsigned MsgId)
    {
        // LEA EAX,[ECX-1] / CMP EAX,0xc / JA -> the false arm; the byte table at
        // sends 1, 2, 4, 5 to their own arms, 3 to the false arm and 6..13 to the
        // mouse-button arm.
        if (MsgId - 1 > 12 || MsgId == 3)
            return false;

        if (MsgId == 1)
        {
            // the key goes to the routine with the event record.
            return Win3DDispatchKey(HudCommandRecord());
        }

        if (MsgId == 2)
        {
            // slot 22 (record +0x10, record +0x14) to the focus object and its answer
            // returned; without a focus, to every registered object until one returns true.
            unsigned const B = HudCommandRecord()->Arg14;
            unsigned const A = HudCommandRecord()->Arg10;
            if (G_KeyFocus != nullptr)
                return G_KeyFocus->Slot22(A, B);
            bool bHandled = false;
            for (FWin3DListNode* Node = G_Registry.Head; Node != nullptr; Node = Node->Next)
            {
                Win3D* const Object = static_cast<Win3D*>(Node->Payload);
                if (Object == nullptr)
                    continue;
                bHandled = Object->Slot22(A, B) || bHandled;
                if (bHandled)
                    break;
            }
            return bHandled;
        }

        if (MsgId == 4)
        {
            // the wheel. The capture head alone gets slot 18 with the field;
            // otherwise the picked object does.
#ifdef OUTPOST_FRONTIER_WIDESCREEN_SUPPORT
            if (G_CaptureHead != nullptr)
                return WheelWidescreen(G_CaptureHead, GetClientMouseWheel());
            Win3D* const Picked = Win3DPick(G_MouseRay);
            if (Picked == nullptr)
                return false;
            return WheelWidescreen(Picked, GetClientMouseWheel());
#else
            if (G_CaptureHead != nullptr)
                return G_CaptureHead->Slot18(GetClientMouseWheel());
            Win3D* const Picked = Win3DPick(G_MouseRay);
            if (Picked == nullptr)
                return false;
            return Picked->Slot18(GetClientMouseWheel());
#endif
        }

        if (MsgId == 5)
        {
            // mouse move. The cursor pixel (the field / the field) goes to the
            // near plane of the field (the routine), into world space through a copy of
            // the camera's orientation the field (the routine), and is normalised
            // (the routine) into the stored ray the field.
            int CursorX = 0;
            int CursorY = 0;
            GetClientSoftwareCursor(&CursorX, &CursorY, nullptr);
            FSpaceCamera* const Cam = Win3DGetCamera();
            FVector Local;
            ScreenToNearPlane(Cam, Local, static_cast<float>(CursorX),
                              static_cast<float>(CursorY));
            FMatrix Orientation;
            memcpy(&Orientation, Cam->World, sizeof(Orientation));
            FVector World;
            TransformByMatrix(World, Orientation, Local);
            NormalizeInPlace(World);
            G_MouseRay[0] = World.X;
            G_MouseRay[1] = World.Y;
            G_MouseRay[2] = World.Z;

            // with a capture, the pick inside it, falling back to the capture head
            // itself; without one, the global pick. The result becomes the hover object and
            // the event counts as consumed when there is one.
            Win3D* Picked = nullptr;
            if (G_CaptureHead == nullptr)
            {
                Picked = Win3DPick(G_MouseRay);
            }
            else
            {
                Picked = Win3DPickWithin(G_CaptureHead, G_MouseRay);
                if (Picked == nullptr)
                    Picked = G_CaptureHead;
            }
            Win3DSetHover(Picked);
            return Picked != nullptr;
        }

        // mouse buttons 6..13. Field0 is -1 - (id - 6); the listing's "else 0" arm
        // (0059d177) is unreachable from these ids.
        FWin3DMouseEvent Event;
        unsigned const ButtonIndex = MsgId - 6;
        if (ButtonIndex < 8)
            Event.Field0 = -1 - static_cast<int>(ButtonIndex);
        else
            Event.Field0 = 0;
        unsigned char const Byte1A = HudCommandRecord()->Byte1A;
        if (HudCommandRecord()->Byte18 != 0)
            Event.Field1 = 2 + (Byte1A != 0 ? 1 : 0);
        else
            Event.Field1 = 1;

        // walk the capture chain. For each capture object the pick inside it
        // gets the event first; the capture object itself gets it when the pick missed or
        // declined, unless the pick WAS the capture object. The walk stops once handled.
        bool bHandled = false;
        Win3D* Capture = G_CaptureHead;
        if (Capture != nullptr)
        {
            do
            {
                if (bHandled)
                    return bHandled;
                Win3D* const Next = Capture->CaptureNext;
                Win3D* const Hit = Win3DPickWithin(Capture, G_MouseRay);
#ifdef OUTPOST_FRONTIER_WIDESCREEN_SUPPORT
                if (Hit != nullptr)
                    bHandled = MouseEventWidescreen(Hit, &Event);
                if (!bHandled && Hit != Capture)
                    bHandled = MouseEventWidescreen(Capture, &Event);
#else
                if (Hit != nullptr)
                    bHandled = Hit->MouseEvent(&Event);
                if (!bHandled && Hit != Capture)
                    bHandled = Capture->MouseEvent(&Event);
#endif
                Capture = Next;
            } while (Capture != nullptr);
            if (bHandled)
                return bHandled;
        }

        // nothing in the capture chain took it; the global pick gets it.
        Win3D* const Picked = Win3DPick(G_MouseRay);
        if (Picked == nullptr)
            return bHandled;
#ifdef OUTPOST_FRONTIER_WIDESCREEN_SUPPORT
        return MouseEventWidescreen(Picked, &Event);
#else
        return Picked->MouseEvent(&Event);
#endif
    }

    // . Constructed by the static initializer the branch, destroyed through the
    // atexit thunk the branch.
    FWin3DInputHandler G_Win3DInputHandler;
}

void Win3DTransformByMatrix(FVector& Out, FMatrix const& Matrix, FVector const& Source)
{
    // the same body the input router uses; one engine cache (the field).
    TransformByMatrix(Out, Matrix, Source);
}

I3DMathEngine* Win3DTransformMathEngine()
{
    // `MOV EAX,[the field]; TEST EAX,EAX; JNZ; CALL the routine; MOV [..],EAX`.
    if (G_TransformMathEngine == nullptr)
        G_TransformMathEngine = Acquire3DMathEngine();
    return G_TransformMathEngine;
}

void Win3DScreenToNearPlane(FSpaceCamera const* Cam, FVector& Out, float ScreenX, float ScreenY)
{
    // .
    ScreenToNearPlane(Cam, Out, ScreenX, ScreenY);
}
