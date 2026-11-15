// Author: Giperion.

#include "ui_win_object.h"

#include "ui_page.h"
#include "ui_page_desc.h"
#include "win_factory.h"

#include "x86math.h"

#include <string.h>

UiWinObject::UiWinObject()
    : Width_(0.0f)
    , Height_(0.0f)
    , Depth_(0.0f)
{
    // 0059c97e..0059c9df (inlined ctor): the Win3D and WinObject parts run in
    // their constructors; then +0xb8 the routine identity 3x3, +0xdc..+0xe4 translation = 0.
    // +0xac..+0xb4 / +0xe8..+0xfc are not written (zeros here are only C++ initialisation).
    PageBlock_[0] = 1.0f;
    PageBlock_[1] = 0.0f;
    PageBlock_[2] = 0.0f;
    PageBlock_[3] = 0.0f;
    PageBlock_[4] = 1.0f;
    PageBlock_[5] = 0.0f;
    PageBlock_[6] = 0.0f;
    PageBlock_[7] = 0.0f;
    PageBlock_[8] = 1.0f;
    PageBlock_[9] = 0.0f;
    PageBlock_[10] = 0.0f;
    PageBlock_[11] = 0.0f;

    for (int Index = 0; Index < 3; ++Index)
    {
        Origin_[Index] = 0.0f;
        Corner_[Index] = 0.0f;
    }
}

UiWinObject::~UiWinObject()
{
    // = the WinObject dtor the routine (runs as the base).
}

void UiWinObject::SetPosition(float const* NewPosition)
{
    // (RET 4): +0x60..+0x68 = NewPosition; the routine(holder, NewPosition) --
    // WinObject's slot 5 -- then the routine(holder).
    WinObject::SetPosition(NewPosition);
    ClientMeshHolderUpdate(&MeshHolder);
}

void UiWinObject::SetTransform(float const* Transform)
{
    // (RET 4).
    FTransform const& Block = *reinterpret_cast<FTransform const*>(Transform);

    // REP MOVSD 0xc — keep the page placement at +0xb8.
    memcpy(PageBlock_, Transform, sizeof(PageBlock_));

    FVector const Translation = Block.Translation;

    // (Width * 0.5, Height * -0.5, 0) rotated by the block (TransformMatrix +0x2c), plus the
    // block translation (the routine builds the sum).
    FVector HalfExtent;
    HalfExtent.X = Width_ * 0.5f;
    HalfExtent.Y = Height_ * -0.5f;
    HalfExtent.Z = 0.0f;
    FVector Rotated;
    UiWidgetMathEngine()->TransformMatrix(Rotated, Block.Rotation, HalfExtent);
    FVector Center;
    Center.X = Rotated.X + Translation.X;
    Center.Y = Rotated.Y + Translation.Y;
    Center.Z = Rotated.Z + Translation.Z;

    // FLD +0xb4 / FDIV translation.z — kept on the x87 stack for both products.
    double const Scale = static_cast<double>(Depth_) / static_cast<double>(Translation.Z);
    float Position[3];
    Position[0] = static_cast<float>(static_cast<double>(Center.X) * Scale);
    Position[1] = static_cast<float>(static_cast<double>(Center.Y) * Scale);
    Position[2] = Depth_;

    // +0xe8 = TransformXform (+0x28)(Block, (0, 0, 0)).
    FVector Zero;
    Zero.X = 0.0f;
    Zero.Y = 0.0f;
    Zero.Z = 0.0f;
    FVector OriginOut;
    UiWidgetMathEngine()->TransformXform(OriginOut, Block, Zero);
    Origin_[0] = OriginOut.X;
    Origin_[1] = OriginOut.Y;
    Origin_[2] = OriginOut.Z;

    // +0xf4 = TransformXform (+0x28)(Block, (Width, -Height, 0)).
    FVector CornerIn;
    CornerIn.X = Width_;
    CornerIn.Y = -Height_;
    CornerIn.Z = 0.0f;
    FVector CornerOut;
    UiWidgetMathEngine()->TransformXform(CornerOut, Block, CornerIn);
    Corner_[0] = CornerOut.X;
    Corner_[1] = CornerOut.Y;
    Corner_[2] = CornerOut.Z;

    // vftable+0x1c SetOrientation(identity 3x3), vftable+0x14 SetPosition(Position) — slot 5.
    float Identity[9];
    Identity[0] = 1.0f;
    Identity[1] = 0.0f;
    Identity[2] = 0.0f;
    Identity[3] = 0.0f;
    Identity[4] = 1.0f;
    Identity[5] = 0.0f;
    Identity[6] = 0.0f;
    Identity[7] = 0.0f;
    Identity[8] = 1.0f;
    SetOrientation(Identity);
    SetPosition(Position);
}

bool UiWinObject::Create(Win3D* NewParent, void* Payload)
{
    // (RET 8).
    if (Payload == nullptr)
        return false;

    FUiPayloadWinObject const* ObjPayload = static_cast<FUiPayloadWinObject const*>(Payload);
    Width_ = ObjPayload->Right;                                       // +0xac
    Height_ = ObjPayload->Bottom;                                     // +0xb0
    Depth_ = ObjPayload->ExtraFloat;                                  // +0xb4

    // std::string("") append(P[2]) → c_str() (empty buffer → "").
    // The loader always stores a non-null copy in P[2] (ui_page_desc.cpp, UIWinObject line).
    char const* MeshPath = ObjPayload->ExtraName;

    // (this+0x7c, path, 0, 0).
    if (!ClientMeshLoadFromPath(&MeshHolder, MeshPath, 0, false))
        return false;

    Win3DReparent(this, NewParent);
    Win3DRegister(this);
    Flags = static_cast<unsigned char>((Flags & 0xfb) | kWin3DActive | kWin3DVisible);
    return true;
}

// — WINFACTORY<UIWinObject> static (the branch → the routine; Create
// ); atexit dtor the routine.
WINFACTORY<UiWinObject> G_UIWinObjectFactory("UIWinObject");
