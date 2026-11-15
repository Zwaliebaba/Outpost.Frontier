// Author: Giperion.

#include "ui_win_deform.h"

#include "globals.h"          // GetClientFrameTimeCapped (the routine)
#include "hud_element_list.h" // HudMatrixRotationY / RotateRows12 / RotateRows01 (the routine..)
#include "space_scene_obj_draw.h" // SpaceCameraSetNearFar (the routine)
#include "ui_page.h"          // UiWidgetMathEngine (the field)
#include "ui_page_desc.h"     // FUiPayloadBounds
#include "win3d_immediate.h"  // Win3DSetViewport (the routine)
#include "win3d_render.h"     // Win3DApplyCamera (the routine)
#include "win_factory.h"

#include "NeuronRuntime.h"
#include "common_reputation.h" // GetCostumeDescriptions, CostumeDescriptions, Costume
#include "neuroncomponents.h"             // FDUMP
#include "engine/sceneruntime.h"    // IDeformable
#include "x86math.h"

#include <stdio.h>
#include <string.h>

namespace
{
// .rdata 0x005e5874 -- the FDUMP / Win3DSetViewport source name of every UIWin.cpp call here.
char const kUiWinCpp[] = "Client\\Interface\\UIWin.cpp";
} // namespace

UiWinDeform::UiWinDeform()
    : Width(0.0f)
    , Height(0.0f)
    , PoseZ(-5.0f)
    , PoseHeight(-1.0f)
{
    // . Win3D's ctor is inlined (0x0059acbf..0x0059ad04); the string at +0x7c
    // (0x0059ad0e..0x0059ad18), the body (0x0059ad1e..0x0059ad9c: FEngineCharacterBody's ctor
    // inlined) and the camera base ctor the routine (0x0059adb3) follow in member order.
    // +0x3e8 / +0x3e4 = 0, +0x3ec = -5.0f (0xc0a00000),
    // +0x3f0 = -1.0f (0xbf800000) -- the initializer list above.
    SpaceCameraConstruct(&Camera);

    // the preview camera takes the Win3D scene camera's (the field)
    // viewport (+0xac = the field, the routine), near / far (+0xa4 the field, +0xa8
    // , the routine) and half horizontal fov (+0x98 the field, the routine).
    FSpaceCamera* const Scene = Win3DGetCamera();
    SpaceCameraSetViewport(&Camera, Scene->Viewport);
    SpaceCameraSetNearFar(&Camera, Scene->ZNear, Scene->ZFar);
    SpaceCameraSetHalfFovX(&Camera, Scene->HalfFovXDeg);
}

UiWinDeform::~UiWinDeform()
{
    // . 0x0059ae88..0x0059aed1: with a deformable, the routine's body inlined
    // (StopAnimations, the four holders released, IDeformable slot 6, +0x8c = 0).
    if (Body.Deformable != nullptr)
        Body.Reset();
    // the camera's base dtor (vftable 0x005c8944, an empty body);
    // FEngineCharacterBody's dtor (the routine inlined);
    // the MSVCP60 string at +0x7c released; 0x0059afc5..0x0059b004:
    // Win3D's dtor inlined -- all of them the members' and the base's own destructors.
}

void UiWinDeform::SetPosition(float const* NewPosition)
{
    // (RET 4). 0x0059bb46..0x0059bb5a: three dwords to +0x60 (Win3D's slot 5).
    Position[0] = NewPosition[0];
    Position[1] = NewPosition[1];
    Position[2] = NewPosition[2];

    // only with a deformable.
    if (Body.Deformable == nullptr)
        return;
    PoseBody();
}

void UiWinDeform::SetTransform(float const* Transform)
{
    // (RET 4).
    // REP MOVSD of the 12 incoming floats into a local transform.
    float Local[12];
    memcpy(Local, Transform, sizeof(Local));

    // the widget's top-right corner offset {Width * 0.5f (the field),
    // Height * -0.5f (the field), 0}.
    FVector HalfExtent;
    HalfExtent.X = Width * 0.5f;
    HalfExtent.Y = Height * -0.5f;
    HalfExtent.Z = 0.0f;

    // I3DMathEngine slot 11 TransformMatrix(Out, the incoming 3x3,
    // HalfExtent) through the shared cache the field.
    FVector Rotated;
    UiWidgetMathEngine()->TransformMatrix(Rotated,
                                          *reinterpret_cast<FMatrix const*>(Transform),
                                          HalfExtent);

    // the local translation = Rotated + the incoming translation.
    Local[9] = Rotated.X + Transform[9];
    Local[10] = Rotated.Y + Transform[10];
    Local[11] = Rotated.Z + Transform[11];

    // slot 7 SetOrientation(Local), slot 5 SetPosition(Local + 9) --
    // the virtual calls, so this class's SetPosition poses the body.
    SetOrientation(Local);
    SetPosition(Local + 9);

    // the top-left corner {Width * -0.5f, Height * 0.5f, 0} through
    // I3DMathEngine slot 10 TransformXform(Out, this + 0x3c (orientation + position), corner).
    FTransform const& Xform = *reinterpret_cast<FTransform const*>(Orientation);
    FVector Corner;
    Corner.X = Width * -0.5f;
    Corner.Y = Height * 0.5f;
    Corner.Z = 0.0f;
    FVector TopLeft;
    UiWidgetMathEngine()->TransformXform(TopLeft, Xform, Corner);

    // the bottom-right corner {Width * 0.5f, Height * -0.5f, 0}.
    Corner.X = Width * 0.5f;
    Corner.Y = Height * -0.5f;
    Corner.Z = 0.0f;
    FVector BottomRight;
    UiWidgetMathEngine()->TransformXform(BottomRight, Xform, Corner);

    // both corners projected by the Win3D scene camera (the field,
    // ICamera slot 13); the results are not tested.
    ICamera* const Scene = AsCamera(Win3DGetCamera());
    float X1;
    float Y1;
    float Z1;
    Scene->Project(&X1, &Y1, &Z1, &TopLeft.X);
    float X2;
    float Y2;
    float Z2;
    Scene->Project(&X2, &Y2, &Z2, &BottomRight.X);

    // ftol (truncation) of each coordinate into {left, top, right,
    // bottom}, then the routine(&rect, 1) on the preview camera.
    int Rect[4];
    Rect[0] = static_cast<int>(X1);
    Rect[1] = static_cast<int>(Y1);
    Rect[2] = static_cast<int>(X2);
    Rect[3] = static_cast<int>(Y2);
    SpaceCameraSetViewportEx(&Camera, Rect, 1);
}

bool UiWinDeform::Create(Win3D* NewParent, void* Payload)
{
    // (RET 8). 0x0059b8a0..0x0059b8c6: Win3D's slot 9 body inlined (reparent,
    // register, flags = (flags & 0xfb) | 3).
    Win3D::Create(NewParent, Payload);

    // a payload carries {width, height} into +0x3e4 / +0x3e8.
    if (Payload != nullptr)
    {
        FUiPayloadBounds const* const Bounds = static_cast<FUiPayloadBounds const*>(Payload);
        Width = Bounds->Right;
        Height = Bounds->Bottom;
    }
    // MOV AL,1.
    return true;
}

void UiWinDeform::Update()
{
    // . 0x0059bc37..0x0059bc5a: Win3D's slot 11 body inlined (per child: bit0 ?
    // slot 11 : slot 12).
    Win3D::Update();

    // nothing more without a deformable.
    if (Body.Deformable == nullptr)
        return;

    // the frame time (the routine) stored as a float (FST);
    // FCOMP double 0.0 (the field), TEST AH,0x41 / JNZ -> out when <= 0 or unordered, the
    // fall-through is > 0.
    double const FrameTime = GetClientFrameTimeCapped();
    float Remaining = static_cast<float>(FrameTime);
    if (!(FrameTime > 0.0))
        return;

    for (;;)
    {
        // FCOMP 0.2f (the field), TEST AH,5 / JP -> the step is
        // 0.2f (0x3e4ccccd) when Remaining >= 0.2f or unordered; the fall-through (below) takes
        // Remaining itself.
        float const Step = (Remaining < 0.2f) ? Remaining : 0.2f;

        // the routine's body inlined, with its own deformable test
        // (0x0059bcaf) -- MixerWalk / UpdateInstance with Step * AnimRate, the playback slots
        // emptied when IDeformable slot 26 reports none running.
        Body.Advance(Step);

        // Remaining -= Step (FST to the float local), FCOMP double 0.0,
        // TEST AH,0x41 / JZ -> loop while the (unrounded) difference is > 0.
        double const Left = static_cast<double>(Remaining) - static_cast<double>(Step);
        Remaining = static_cast<float>(Left);
        if (!(Left > 0.0))
            break;
    }
}

void UiWinDeform::Draw()
{
    // . 0x0059bda0..0x0059be7a: a debug outline gated on the field (the routine,
    // x4, the routine, the routine(7), four the routine, the routine). The
    // byte has four readers (0x00599ce0, 0x0059a310, 0x0059ab6c, here) and no writer in the PE,
    // so the branch never runs; it is left out, as in ui_static_text / ui_list_box.

    // the body only with a deformable.
    if (Body.Deformable != nullptr)
    {
        // four zero LOD ids.
        int Zeros[4] = {0, 0, 0, 0};
        // [ESP+0x14]: Draw's out-parameter; baseline does not initialize it.
        int OutLod = 0;

        // the preview camera applied (the routine) and its viewport set
        // (the routine, UIWin.cpp line 0x29f).
        Win3DApplyCamera(AsCamera(&Camera));
        Win3DSetViewport(AsCamera(&Camera), kUiWinCpp, 0x29f);

        // IRenderPipeline slot 60 (+0xf0) SetRenderState(0x16, 2) and
        // SetRenderState(0x17, 4).
        NeuronRuntime::Pipeline->SetRenderState(D3DRS_CULLMODE, 2);
        NeuronRuntime::Pipeline->SetRenderState(D3DRS_ZFUNC, 4);

        // IDeformable slot 15 SetLod(inst, 0.0f, Zeros), slot 16
        // Draw(inst, &Camera, Zeros, 0, &OutLod, 0).
        NeuronRuntime::Deform->SetLod(Body.Deformable, 0.0f, Zeros);
        NeuronRuntime::Deform->Draw(Body.Deformable, &Camera, Zeros, nullptr, &OutLod, 0);

        // the scene camera back (the routine, the routine line 0x2ad).
        Win3DApplyCamera(AsCamera(Win3DGetCamera()));
        Win3DSetViewport(AsCamera(Win3DGetCamera()), kUiWinCpp, 0x2ad);
    }

    // the routine -- Win3D's slot 13 (the sorted children).
    Win3D::Draw();
}

void UiWinDeform::SetCostume(char const* Name)
{
    // (thiscall, RET 4).
    // with a deformable, the routine's body inlined.
    if (Body.Deformable != nullptr)
        Body.Reset();

    // the costume table, the named costume and a nonzero id, else out.
    CostumeDescriptions const* const Descs = GetCostumeDescriptions();
    if (Descs == nullptr)
        return;
    costume const* const Found = Descs->find_costume(Name);
    if (Found == nullptr || Found->Id == 0)
        return;

    // a local Costume with the four part ids and the accessory count
    // zeroed, filled by CostumeDescriptions::get_costume(int id, Costume&).
    Costume Local;
    Local.PartIds[0] = 0;
    Local.PartIds[1] = 0;
    Local.PartIds[2] = 0;
    Local.PartIds[3] = 0;
    Local.AccessoryCount = 0;
    Descs->get_costume(static_cast<int>(Found->Id), Local);

    // the routine; on failure the routine (_vsnprintf into the
    // 0x2000-byte stack buffer) and FDUMP(0x100003) with UIWin.cpp line 0x210.
    if (!Body.Build(Local))
    {
        char Message[0x2000];
        _snprintf(Message, sizeof(Message), "Failed to create costume \"%s\".\n", Name);
        Message[sizeof(Message) - 1] = '\0';
        FDUMP(0x100003, "%s(%d) : NOTICE:General:%s", kUiWinCpp, 0x210, Message);
        return;
    }

    // only with a deformable (Build's result already says so).
    if (Body.Deformable == nullptr)
        return;
    PoseBody();
}

void UiWinDeform::PoseBody()
{
    // The pose SetCostume (0x0059b12d..0x0059b20e) and SetPosition (0x0059bb6b..0x0059bc1c)
    // spell out inline: the 3x3 identity (the routine / the explicit 1s and 0s), then
    // (pi = 0x40490fdb), the routine(0), the routine(0).
    float Rotation[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    HudMatrixRotationY(Rotation, 3.14159274f);
    HudMatrixRotateRows12(Rotation, 0.0f);
    HudMatrixRotateRows01(Rotation, 0.0f);

    // / 0x0059bbb0..0x0059bbd0: IDeformable slot 24 with +0x3f0, only
    // with a deformable.
    if (Body.Deformable != nullptr)
        NeuronRuntime::Deform->SetHeight(Body.Deformable, PoseHeight);

    // / 0x0059bbd3..0x0059bc03: slot 19 with {0, 0, +0x3ec}.
    float Pose[3] = {0.0f, 0.0f, PoseZ};
    NeuronRuntime::Deform->SetPosePos(Body.Deformable, Pose);

    // / 0x0059bc06..0x0059bc1c: slot 20 with the rotation.
    NeuronRuntime::Deform->SetPoseOri(Body.Deformable, Rotation);
}

// WINFACTORY<UIWinDeform> (static init the branch -> the routine
// ("UIWinDeform"), atexit the branch -> the routine; Create the routine: operator_new(0x3f4),
// , +0x10 = the factory name, slot 9, the scalar deleting dtor on failure).
WINFACTORY<UiWinDeform> G_UIWinDeformFactory("UIWinDeform");
