// Author: Giperion.

#include "ui_ship_view_control.h"

#include "ui_page.h"      // UiWidgetMathEngine (the field)
#include "ui_page_desc.h" // FUiPayloadBounds
#include "win_factory.h"

#include "x86math.h"

namespace
{
    // / module storage -- the RGBA the Create of UIShipViewControl hands to
    // (rect colour) and the routine (wire colour); both tables hold the same four
    // floats (checked in PE bytes) and nothing writes them.
    float const kUiShipViewRectColor[4] = {0.529411972f, 0.764706016f, 0.874509990f, 0.486274987f};
    float const kUiShipViewWireColor[4] = {0.529411972f, 0.764706016f, 0.874509990f, 0.486274987f};
}

UiShipViewControl::UiShipViewControl()
{
    // 0x0059cd2b..0x0059cd36: the ShipViewControl ctor, then the two vftables.
    // +0x570..+0x578 are not written until Create (baseline leaves the heap bytes); the port
    // starts them at 0.
    HalfExtent[0] = 0.0f;
    HalfExtent[1] = 0.0f;
    HalfExtent[2] = 0.0f;
}

UiShipViewControl::~UiShipViewControl()
{
    // the vftables back to UIShipViewControl's, then ShipViewControl's
    // destructor (the routine) -- nothing of its own.
}

bool UiShipViewControl::Create(Win3D* NewParent, void* Payload)
{
    // (RET 8): ShipViewControl's slot 9 (the routine) with no INI reader.
    bool const bCreated = ShipViewControl::Create(NewParent, nullptr);
    if (!bCreated)
        return bCreated;

    // with a payload, the half extent {Width * 0.5 (the field),
    // Height * -0.5 (the field), 0}.
    if (Payload != nullptr)
    {
        FUiPayloadBounds const* const Bounds = static_cast<FUiPayloadBounds const*>(Payload);
        HalfExtent[2] = 0.0f;
        HalfExtent[0] = Bounds->Right * 0.5f;
        HalfExtent[1] = Bounds->Bottom * -0.5f;
    }

    // the view set up through ShipViewControl's setters.
    float Vector[3];
    Vector[0] = 0.0f;
    Vector[1] = 0.0f;
    Vector[2] = 30.0f;
    SetRotSpeed(Vector);
    Vector[0] = 0.0f;
    Vector[1] = 270.0f;
    Vector[2] = 0.0f;
    SetObjectYpr(Vector);
    Vector[0] = 0.0f;
    Vector[1] = 0.0f;
    Vector[2] = 0.0f;
    SetAttachPosition(Vector);
    SetShowRect(false);                      // (0)
    SetShowWireframe(true);                  // (1)
    SetRectColor(kUiShipViewRectColor);      // (&the field)
    SetWireColor(kUiShipViewWireColor);      // (&the field)
    return bCreated;
}

void UiShipViewControl::SetTransform(float const* Transform)
{
    // (RET 4). The 12-float block (rotation, then the translation at +0x24) is
    // copied to a local; the half extent turned by the rotation (the field slot 11 (+0x2c),
    // the engine made on first use by the routine) is the view size, and the box's corner --
    // that turned extent plus the translation, each sum rounded to float (FADD / FSTP) -- the
    // position.
    float Block[12];
    for (int Index = 0; Index < 12; ++Index)
        Block[Index] = Transform[Index];
    float const Translation[3] = {Transform[9], Transform[10], Transform[11]};

    FVector Turned;
    FVector Source;
    Source.X = HalfExtent[0];
    Source.Y = HalfExtent[1];
    Source.Z = HalfExtent[2];
    UiWidgetMathEngine()->TransformMatrix(Turned, *reinterpret_cast<FMatrix const*>(Transform),
                                          Source);

    float Corner[3];
    Corner[0] = Turned.X + Translation[0];
    Corner[1] = Turned.Y + Translation[1];
    Corner[2] = Turned.Z + Translation[2];
    float const ViewExtent[3] = {Turned.X, Turned.Y, Turned.Z};

    SetOrientation(Block); // slot 7 (+0x1c) with the local copy
    SetPosition(Corner);   // slot 5 (+0x14)
    SetViewSize(ViewExtent);
}

// WINFACTORY<UIShipViewControl> (static init the branch -> the routine
// ("UIShipViewControl"), atexit the branch -> the routine; Create the routine).
WINFACTORY<UiShipViewControl> G_UIShipViewControlFactory("UIShipViewControl");
