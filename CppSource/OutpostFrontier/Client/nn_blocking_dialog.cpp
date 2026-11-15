// Author: Giperion.

#include "nn_blocking_dialog.h"

#include "globals.h"            // PushClientTimingPause / PopClientTimingPause
#include "input_manager.h"      // MouseSetCursor (the routine)
#include "nn_modal.h"
#include "space_letterbox.h"    // IsLetterboxVisible (the routine)
#include "win3d.h"
#include "win3d_renderables.h"  // Win3DFlushBeforeOverlayDraw (the routine)
#include "win_factory.h"

// WINFACTORY<BlockingDialog> static (the branch -> the routine; Create
// ; atexit the branch -> the routine).
WINFACTORY<BlockingDialog> G_BlockingDialogFactory("BlockingDialog");

BlockingDialog::BlockingDialog()
    : bCursorDrawFlag(1)
{
    // 004747fb..0047481e: the routine, vptrs, byte +0x32c = 1, the routine,
    // INC the field.
    PushClientTimingPause();
    G_ModalCaptureCount = G_ModalCaptureCount + 1;
}

BlockingDialog::~BlockingDialog()
{
    // DEC the field, the routine(0), the routine; then the routine as the
    // base-class destructor.
    G_ModalCaptureCount = G_ModalCaptureCount - 1;
    Win3DSetKeyFocus(nullptr);
    PopClientTimingPause();
}

bool BlockingDialog::Create(Win3D* NewParent, void* Data)
{
    // (RET 8).
    // the routine("Arrow", 0) -- the Mouse.cpp cursor shape.
    MouseSetCursor("Arrow", false);

    // / 004744a9
    DrawLayer = 0x64;
    DrawSubLayer = 6;

    bCursorDrawFlag = NnModalPauseGame();
    Win3DSetKeyFocus(this);
    // +0xb4 = {2.0f, 2.0f, 0.0f}.
    HalfExtents[0] = 2.0f;
    HalfExtents[1] = 2.0f;
    HalfExtents[2] = 0.0f;

    // the routine, SETNZ.
    return Win3D::Create(NewParent, Data);
}

void BlockingDialog::Draw()
{
    // .
    Win3DFlushBeforeOverlayDraw();
    WinControl::Draw();            // JMP the routine
}

void BlockingDialogOpen()
{
    // .
    if (Win3DFindByClassName("BlockingDialog") != nullptr)
        return;
    Win3D* const Created =
        WinFactoryCreate("BlockingDialog", "BlockingDialog", nullptr, nullptr);
    if (Created == nullptr)
        return;
    BlockingDialog* const Dialog = static_cast<BlockingDialog*>(Created);
    float const Zero[3] = {0.0f, 0.0f, 0.0f};
    Dialog->SetProperty(0x42, reinterpret_cast<uintptr_t>(Zero), 0);
    Dialog->CaptureMouse();
}

void BlockingDialogClose()
{
    // .
    Win3D* const Found = Win3DFindByClassName("BlockingDialog");
    if (Found == nullptr)
        return;
    BlockingDialog* const Dialog = static_cast<BlockingDialog*>(Found);

    if (IsLetterboxVisible())
        Dialog->bCursorDrawFlag = 0;
    NnModalResumeGame(Dialog->bCursorDrawFlag);
    Dialog->ReleaseMouse();
    Win3D* Child = Dialog->ChildHead;
    while (Child != nullptr)
    {
        Win3D* const Next = Child->NextSibling;
        Win3DDeleteTree(Child);
        Child = Next;
    }
    Dialog->Destroy();
    delete Dialog;
}
