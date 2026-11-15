// Author: Giperion.

// baseline ShipViewControl -- see ship_view_control.h for the class, its vftable and layout.
// baseline source: Client\Interface\ShipViewControl.cpp (.rdata 0x005e4cd8).

#include "ship_view_control.h"

#include "engine_light.h"      // FEngineLight, EngineLight* (the routine .. the routine)
#include "gf_object_ship.h"    // GfShipApplyCollisionDamage (the routine)
#include "equip_visual_set.h"  // FEquipVisualSet (the routine .. the routine)
#include "globals.h"           // GetClientFrameTimeCapped (the routine)
#include "hud_element_list.h"  // HudMatrixRotationY / RotateRows12 / RotateRows01
#include "hud_target.h"        // FHudInstanceBounds, HudInverseTransformPoint
#include "nn_ui_pulse.h"       // G_HudChromeColorRgba (the field)
#include "win3d_immediate.h"
#include "win3d_render.h"      // Win3DApplyCamera (the routine)
#include "win_factory.h"

#include "client_cliobj.h"
#include "common_archetype.h"
#include "common_csys.h"       // Acquire3DMathEngine (the routine)
#include "common_goods.h"
#include "common_ini.h"
#include "neuroncomponents.h"
#include "NeuronRuntime.h"
#include "engine/sceneruntime.h"
#include "render/IRenderPipeline.h"
#include "render/rendcomp.h"
#include "x86math.h"

#include <math.h>
#include <string.h>

// baseline WINFACTORY<ShipViewControl> "ShipViewControl" (ctor the routine, vftable 0x005e4d60;
// Create the routine: operator_new(0x570), the routine, slot 9, scalar delete on failure).
WINFACTORY<ShipViewControl> G_ShipViewControlFactory("ShipViewControl");

namespace
{
    char const* const kShipViewControlCpp =
        "Client\\Interface\\ShipViewControl.cpp";

    // degrees to radians as a float.
    float const kDegToRad = 0.01745329238474369f;

    // this file's lazily acquired 3DMathEngine (the routine).
    I3DMathEngine* G_ShipViewMath = nullptr;

    I3DMathEngine* GetShipViewMath()
    {
        if (G_ShipViewMath == nullptr)
            G_ShipViewMath = Acquire3DMathEngine();
        return G_ShipViewMath;
    }

    // 0x3c rows {HpAttachmentType, caption IDS} (read by the routine and
    // ; no writer in the exe).
    struct FHpTypeCaption
    {
        int HpType;
        int Ids;
    };

    FHpTypeCaption const G_ShipViewHpTypeCaptions[0x3c] = {
        {15, 1525}, {16, 1526}, {17, 1527}, {18, 1528}, {19, 1529}, {20, 1530},
        {21, 1531}, {22, 1532}, {23, 1533}, {24, 1534}, {13, 1524}, {25, 1731},
        {26, 1732}, {27, 1733}, {28, 1734}, {29, 1735}, {30, 1736}, {31, 1737},
        {32, 1738}, {33, 1739}, {34, 1740}, {10, 1521}, {36, 1741}, {35, 1742},
        {11, 1522}, {12, 1523}, {37, 1700}, {38, 1701}, {39, 1702}, {40, 1704},
        {41, 1705}, {42, 1706}, {43, 1707}, {44, 1708}, {45, 1709}, {46, 1710},
        {6, 1517},  {47, 1711}, {48, 1712}, {49, 1713}, {50, 1714}, {51, 1715},
        {52, 1716}, {53, 1717}, {54, 1718}, {55, 1719}, {56, 1720}, {7, 1518},
        {57, 1721}, {58, 1722}, {59, 1723}, {60, 1724}, {61, 1725}, {62, 1726},
        {63, 1727}, {64, 1728}, {65, 1729}, {66, 1730}, {8, 1519},  {9, 1520}};

    // The grouping switch the routine (table 0x00586880) and the routine (table 0x00587008)
    // each inline: 0 for 4, 0xa..0xd and 0xf..0x24; 1 for 5..9 and 0x25..0x42; 2 for 0xe and for
    // anything outside 4..0x42 (the JA default is the 0xe case).
    int HpTypeGroup(int HpType)
    {
        if (HpType < 4 || HpType > 0x42)
            return 2;
        if (HpType == 0xe)
            return 2;
        if ((HpType >= 5 && HpType <= 9) || HpType >= 0x25)
            return 1;
        return 0;
    }

    void* HandleOf(long Instance)
    {
        return reinterpret_cast<void*>(static_cast<intptr_t>(Instance));
    }

    long InstanceOf(void* Handle)
    {
        return static_cast<long>(reinterpret_cast<intptr_t>(Handle));
    }

    IRenderPipeline* ShipViewPipeline()
    {
        return NeuronRuntime::Pipeline;
    }

    void SetIdentity9(float* M)
    {
        M[0] = 1.0f;
        M[1] = 0.0f;
        M[2] = 0.0f;
        M[3] = 0.0f;
        M[4] = 1.0f;
        M[5] = 0.0f;
        M[6] = 0.0f;
        M[7] = 0.0f;
        M[8] = 1.0f;
    }

    // The orientation every ypr writer builds (Create 0x00583f81, Init 0x0058430c inlined,
    // DragRotate 0x00584f72 inlined, SetObjectYpr 0x0058691e inlined): the routine(y0),
    // (y1), the routine(y2), each angle times the field.
    void BuildYprOrientation(float* M, float const* Ypr)
    {
        HudMatrixRotationY(M, Ypr[0] * kDegToRad);
        HudMatrixRotateRows12(M, Ypr[1] * kDegToRad);
        HudMatrixRotateRows01(M, Ypr[2] * kDegToRad);
    }

    // ftol(Value * 255.0) (the field) into a colour byte.
    unsigned char ColorByte(double Value)
    {
        return static_cast<unsigned char>(static_cast<int>(Value * 255.0));
    }

    // The mountpoint marker holders are released twice: the explicit the routine when an
    // instance is bound, then the holder's scalar deleting dtor the routine (the routine =
    // ~FClientMeshHolder, the same release again, which finds nothing) and operator delete.
    void DeleteMountHolder(FClientMeshHolder* Holder)
    {
        if (InstanceOf(Holder->EngineInstance) != -1)
            ClientUiMeshHolderRelease(Holder);
        delete Holder;
    }
}

int ShipViewEquipmentHpClass(unsigned ArchId, Archetype::Equipment const** OutWeapon,
                             Archetype::Equipment const** OutShield)
{
    // (cdecl).
    *OutWeapon = nullptr;
    *OutShield = nullptr;
    Archetype::Equipment* const Equip = Archetype::GetEquipment(ArchId);
    if (Equip == nullptr)
        return 0;
    int const HpType = static_cast<int>(Equip->get_hp_type()); // vt+0x18
    if (HpType == 0)
        return 1;
    if (HpType == 4 || HpType == 0xd || HpType == 0xa)
    {
        *OutWeapon = Equip;
        return 4;
    }
    if (HpType == 5 || HpType == 6 || HpType == 7 || HpType == 8)
    {
        // get_class_type (vt+0x10) == AClassShieldGenerator.
        if (Equip->get_class_type() == 0xe)
            *OutShield = Equip;
    }
    return HpType;
}

int ShipViewEquipDescHpClass(EquipDesc const* Desc, Archetype::Equipment const** OutWeapon,
                             Archetype::Equipment const** OutShield)
{
    // the first argument replaced by Desc+4 (ArchId), JMP the routine.
    return ShipViewEquipmentHpClass(Desc->ArchId, OutWeapon, OutShield);
}

int ShipViewHpTypeClass(int HpType)
{
    // switch on HpType - 4 through the byte table 0x00585b90.
    switch (HpType)
    {
    case 0xa:
    case 0x24:
        return 1;
    case 0x23:
        return 2;
    case 0xb:
        return 3;
    case 0xd:
    case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1d:
    case 0x1e: case 0x1f: case 0x20: case 0x21: case 0x22:
        return 4;
    case 0xc:
        return 5;
    case 5:
    case 6:
    case 0x25: case 0x26: case 0x27: case 0x28: case 0x29:
    case 0x2a: case 0x2b: case 0x2c: case 0x2d: case 0x2e:
        return 6;
    case 7:
    case 0x2f: case 0x30: case 0x31: case 0x32: case 0x33:
    case 0x34: case 0x35: case 0x36: case 0x37: case 0x38:
        return 7;
    case 8:
    case 0x39: case 0x3a: case 0x3b: case 0x3c: case 0x3d:
    case 0x3e: case 0x3f: case 0x40: case 0x41: case 0x42:
        return 8;
    case 9:
        return 9;
    default:
        return 0;
    }
}

int ShipViewHpTypeTableCount()
{
    // MOV EAX,0x3c / RET.
    return 0x3c;
}

int ShipViewHpTypeTableType(int Index)
{
    // .
    if (Index < 0 || Index >= 0x3c)
        return 0;
    return G_ShipViewHpTypeCaptions[Index].HpType;
}

ShipViewControl::ShipViewControl()
{
    // , in store order. ObjectYpr (+0x37c), ScreenRect (+0x494), SelRadius /
    // SelExtent (+0x50c / +0x510), Lights[3..7], Field514 and Field548 are not written, as in
    // baseline.
    ShipId.clear();                         // ID_String::clear on +0x32c
    ClientMeshHolderConstruct(&ShipHolder); // + the routine, anim -1
    SpaceCameraConstruct(&Camera);          // on +0x388
    Ambient[3] = 0;
    Ambient[2] = 0;
    Ambient[1] = 0;
    Ambient[0] = 0;
    WireColor[3] = 0;
    WireColor[2] = 0;
    WireColor[1] = 0;
    WireColor[0] = 0;
    RectColor[3] = 0;
    RectColor[2] = 0;
    RectColor[1] = 0;
    RectColor[0] = 0;
    Color4c4[3] = 0;
    Color4c4[2] = 0;
    Color4c4[1] = 0;
    Color4c4[0] = 0;
    // +0x4c8 std::list ctor (import 0x00203c6c) and the three vectors: the members' own ctors.
    Visuals = nullptr;
    Distance = 0.0f;
    Mode = 2;
    Reset();

    ViewOffset[0] = 0.0f;
    ViewOffset[1] = 0.0f;
    ViewOffset[2] = 0.0f;
    ViewSize[0] = 0.0f;
    ViewSize[1] = 0.0f;
    ViewSize[2] = 0.0f;
    AttachPos[0] = 0.0f;
    AttachPos[1] = 0.0f;
    AttachPos[2] = 0.0f;
    SetIdentity9(ObjectOrientation);

    memcpy(WireColor, G_HudChromeColorRgba, sizeof(WireColor));
    memcpy(RectColor, G_HudChromeColorRgba, sizeof(RectColor));
    Color4c4[0] = 0xc3;
    Color4c4[1] = 0xb8;
    Color4c4[2] = 0x20;
    Color4c4[3] = 0xff;
    RotSpeed[0] = 0.0f;
    RotSpeed[1] = 0.0f;
    RotSpeed[2] = 0.0f;
    RotAccum[0] = 0.0f;
    RotAccum[1] = 0.0f;
    Ambient[0] = 0;
    Ambient[1] = 0;
    Ambient[2] = 0;
    Ambient[3] = 0xff;
    FovY = 25.0f;
    RotAccum[2] = 0.0f;

    // three directional lights -- directions {1,1,-1},
    // {-0.965,0.259,0.965}, {-1,-1,1}; colours white and twice {0x32,0x46,0x64}.
    float const Directions[3][3] = {
        {1.0f, 1.0f, -1.0f}, {-0.965f, 0.259f, 0.965f}, {-1.0f, -1.0f, 1.0f}};
    unsigned char const Colors[3][4] = {
        {0xff, 0xff, 0xff, 0xff}, {0x32, 0x46, 0x64, 0xff}, {0x32, 0x46, 0x64, 0xff}};
    LightCount = 3;
    for (int Index = 0; Index < LightCount; ++Index)
    {
        // operator_new(0xc8) + the routine(-1).
        FEngineLight* const Light = static_cast<FEngineLight*>(operator new(sizeof(FEngineLight)));
        EngineLightInit(Light, -1);
        Lights[Index] = Light;
        EngineLightSetType(Light, 3, false); // (3, 0)

        // the direction negated and normalised through y*y + z*z + x*x.
        float const X = Directions[Index][0];
        float const Y = Directions[Index][1];
        float const Z = Directions[Index][2];
        float const Inverse = 1.0f / sqrtf(Y * Y + Z * Z + X * X);
        float Direction[3];
        Direction[0] = -(X * Inverse);
        Direction[1] = -(Y * Inverse);
        Direction[2] = -(Z * Inverse);
        EngineLightSetDirection(Light, Direction);
        EngineLightSetColorBytes(Light, Colors[Index]);
        EngineLightRegister(Light); // vt+4 the routine
    }
    Byte55c = 0;
}

ShipViewControl::~ShipViewControl()
{
    // .
    for (size_t Index = 0; Index < MountHolders.size(); ++Index)
        DeleteMountHolder(MountHolders[Index]);
    MountHolders.clear();

    for (int Index = 0; Index < LightCount; ++Index)
    {
        if (Lights[Index] != nullptr)
            EngineLightDelete(Lights[Index]); // vt slot 0 (1) = the routine
    }

    if (Visuals != nullptr)
    {
        delete Visuals; // + operator delete
        Visuals = nullptr;
    }
    ClientUiMeshHolderRelease(&ShipHolder); // on +0x330

    // Member teardown: +0x560 (element dtor the routine), +0x54c, +0x538, the +0x4c8 list, the
    // camera (vftable reset only) -- then the ship holder's own dtor the routine
    // (~FClientMeshHolder, the same release again, which finds nothing), and ~WinControl.
}

void ShipViewControl::Reset()
{
    // .
    SelHpClass = 0;
    SelWeapon = nullptr;
    SelShield = nullptr;
    Field4f4 = 0;
    Field4f8 = 0;
    ViewFlags = 0;
    Instance = -1;
    SelInstance = -1;
    Field4f0 = -1;
    DragLast[0] = 0.0f;
    DragLast[1] = 0.0f;
    DragLast[2] = 0.0f;
    DragYpr[0] = 0.0f;
    DragYpr[1] = 0.0f;
    DragYpr[2] = 0.0f;
}

bool ShipViewControl::Create(Win3D* NewParent, void* Data)
{
    // (RET 8): Win3D's Create inlined (reparent, register, flags), then the INI
    // keys. Always returns 1.
    Win3D::Create(NewParent, nullptr);
    INI_Reader* const Ini = static_cast<INI_Reader*>(Data);
    if (Ini == nullptr || !Ini->read_value())
        return true;

    do
    {
        // Every key is tested in turn; only "viewSize" skips the "hardpoint" test.
        if (Ini->is_value("rot_speed"))
        {
            float const Y = Ini->get_value_float(1);
            float const X = Ini->get_value_float(0);
            float const Z = Ini->get_value_float(2);
            RotSpeed[0] = X;
            RotSpeed[1] = Y;
            RotSpeed[2] = Z;
        }
        if (Ini->is_value("offset"))
        {
            float const Y = Ini->get_value_float(1);
            float const X = Ini->get_value_float(0);
            float const Z = Ini->get_value_float(2);
            ViewOffset[0] = X;
            ViewOffset[1] = Y;
            ViewOffset[2] = Z;
        }
        if (Ini->is_value("object_ypr"))
        {
            float const Y = Ini->get_value_float(1);
            float const X = Ini->get_value_float(0);
            float const Z = Ini->get_value_float(2);
            ObjectYpr[0] = X;
            ObjectYpr[1] = Y;
            ObjectYpr[2] = Z;
            BuildYprOrientation(ObjectOrientation, ObjectYpr);
        }
        if (Ini->is_value("viewSize"))
        {
            float const Y = Ini->get_value_float(1);
            float const X = Ini->get_value_float(0);
            float const Z = Ini->get_value_float(2);
            ViewSize[0] = X;
            ViewSize[1] = Y;
            ViewSize[2] = Z;
        }
        else if (Ini->is_value("hardpoint"))
        {
            // identity attach orientation (the routine), zero attach
            // position, the NEW parent's slot 36 (value, &+0x2f8), then +0x47c = +0x31c.
            SetIdentity9(AttachOrientation);
            AttachPosition[2] = 0.0f;
            AttachPosition[1] = 0.0f;
            AttachPosition[0] = 0.0f;
            if (NewParent != nullptr)
            {
                WinControl* const ParentControl = NewParent->AsWinControl();
                if (ParentControl != nullptr)
                {
                    ParentControl->GetHardpointTransform(Ini->get_value_string(),
                                                         AttachOrientation);
                    AttachPos[0] = AttachPosition[0];
                    AttachPos[1] = AttachPosition[1];
                    AttachPos[2] = AttachPosition[2];
                }
            }
        }
        if (Ini->is_value("show_rect"))
        {
            // FCOMP 0.0, TEST AH,0x44 / JNP -> bit 2 = (value != 0).
            unsigned char const Bit = (Ini->get_value_float(0) != 0.0f) ? 4 : 0;
            ViewFlags = static_cast<unsigned char>((ViewFlags & ~4) | Bit);
        }
        if (Ini->is_value("show_wireframe"))
        {
            unsigned char const Bit = (Ini->get_value_float(0) != 0.0f) ? 8 : 0;
            ViewFlags = static_cast<unsigned char>((ViewFlags & ~8) | Bit);
        }
        if (Ini->is_value("rect_color"))
        {
            // components 2, 1, 0, 3 read; each * 255.0, ftol.
            double const V2 = Ini->get_value_float(2);
            double const V1 = Ini->get_value_float(1);
            double const V0 = Ini->get_value_float(0);
            unsigned char const B3 = ColorByte(Ini->get_value_float(3));
            RectColor[0] = ColorByte(V0);
            RectColor[1] = ColorByte(V1);
            RectColor[2] = ColorByte(V2);
            RectColor[3] = B3;
        }
        if (Ini->is_value("wire_color"))
        {
            double const V2 = Ini->get_value_float(2);
            double const V1 = Ini->get_value_float(1);
            double const V0 = Ini->get_value_float(0);
            unsigned char const B3 = ColorByte(Ini->get_value_float(3));
            WireColor[0] = ColorByte(V0);
            WireColor[1] = ColorByte(V1);
            WireColor[2] = ColorByte(V2);
            WireColor[3] = B3;
        }
    } while (Ini->read_value());
    return true;
}

void ShipViewControl::Init(float const* NewRotSpeed, float const* NewOffset, float const* NewYpr,
                           float const* NewViewSize, char const* Hardpoint, bool bShowRect,
                           bool bShowWire, unsigned const* NewRectColor,
                           unsigned const* NewWireColor)
{
    // (RET 0x24).
    RotSpeed[0] = NewRotSpeed[0];
    RotSpeed[1] = NewRotSpeed[1];
    RotSpeed[2] = NewRotSpeed[2];
    ViewOffset[0] = NewOffset[0];
    ViewOffset[1] = NewOffset[1];
    ViewOffset[2] = NewOffset[2];
    ObjectYpr[0] = NewYpr[0];
    ObjectYpr[1] = NewYpr[1];
    ObjectYpr[2] = NewYpr[2];
    // the routine and the routine inlined, the routine called.
    BuildYprOrientation(ObjectOrientation, ObjectYpr);
    ViewSize[0] = NewViewSize[0];
    ViewSize[1] = NewViewSize[1];
    ViewSize[2] = NewViewSize[2];
    if (Hardpoint != nullptr)
    {
        // identity + zero attach, this->Parent's slot 36, +0x47c.
        SetIdentity9(AttachOrientation);
        AttachPosition[2] = 0.0f;
        AttachPosition[1] = 0.0f;
        AttachPosition[0] = 0.0f;
        if (Parent != nullptr)
        {
            WinControl* const ParentControl = Parent->AsWinControl();
            if (ParentControl != nullptr)
            {
                ParentControl->GetHardpointTransform(Hardpoint, AttachOrientation);
                AttachPos[0] = AttachPosition[0];
                AttachPos[1] = AttachPosition[1];
                AttachPos[2] = AttachPosition[2];
            }
        }
    }
    unsigned char Bits = static_cast<unsigned char>((ViewFlags & ~4) | (bShowRect ? 4 : 0));
    Bits = static_cast<unsigned char>((Bits & ~8) | (bShowWire ? 8 : 0));
    ViewFlags = Bits;
    memcpy(RectColor, NewRectColor, sizeof(RectColor)); // +0x4c0
    memcpy(WireColor, NewWireColor, sizeof(WireColor)); // +0x4bc
}

void ShipViewControl::Draw()
{
    // .
    Win3D::Draw(); // the children; no WinControl mesh
    if (Instance == -1)
        return;
    if ((ScreenRect[3] | ScreenRect[2] | ScreenRect[1] | ScreenRect[0]) == 0)
        return;

    // this control's light rig and ambient.
    EngineLightClearRegisteredList();
    unsigned char AmbientRgb[3];
    AmbientRgb[0] = Ambient[0];
    AmbientRgb[1] = Ambient[1];
    AmbientRgb[2] = Ambient[2];
    EngineLightSetAmbientRgbBytes(AmbientRgb);
    EngineLightApplyList(Lights, static_cast<unsigned>(LightCount), AsCamera(&Camera), nullptr);

    // the camera into the screen rect, the old viewport kept.
    SpaceCameraSetFovY(&Camera, FovY);
    SpaceCameraSetViewportEx(&Camera, ScreenRect, 1);
    int SavedViewport[4];
    Win3DGetViewport(0xba2, SavedViewport);
    ICamera* const ViewCamera = AsCamera(&Camera);
    Win3DSetViewport(ViewCamera, kShipViewControlCpp, 0x1d8);

    // clear the depth buffer over the camera's viewport.
    IRenderPipeline* const Pipe = ShipViewPipeline();
    int const* const View = ViewCamera->GetViewport(); // camera slot 5 (+0x14)
    int ClearRect[4];
    ClearRect[0] = 0;
    ClearRect[1] = 0;
    ClearRect[2] = View[2] - View[0] - 1;
    ClearRect[3] = View[3] - View[1] - 1;
    Pipe->ClearBuffers(2, ClearRect); // pipeline slot 15 (+0x3c)
    Win3DApplyCameraPerspective(ViewCamera);

    // z on, draw the framed instance, then back.
    Pipe->SetRenderState(D3DRS_ZENABLE, 1);
    Pipe->SetRenderState(D3DRS_ZFUNC, 4);
    Pipe->SetRenderState(D3DRS_ZWRITEENABLE, 1);
    // IRenderer2 +0x1c pushed as (camera, instance, 1.0f, 0x300, 0, 0): the fourth argument is
    // the flag word (rendcomp the branch).
    NeuronRuntime::Renderer->RenderInstance(ViewCamera, HandleOf(Instance), 1.0f, 0x300u, nullptr, nullptr);
    Win3DDisable(0xb71);
    Pipe->SetRenderState(D3DRS_ZENABLE, 0);
    Pipe->SetRenderState(D3DRS_ZFUNC, 8);
    Pipe->SetRenderState(D3DRS_ZWRITEENABLE, 0);
    Win3DDisable(0xb71);

    // the Win3D camera and the saved viewport back, white ambient.
    Win3DApplyCamera(AsCamera(Win3DGetCamera())); // (&the field)
    Win3DSetViewportRect(SavedViewport[0], SavedViewport[1], SavedViewport[2],
                         SavedViewport[3], kShipViewControlCpp, 0x1fc);
    unsigned char const White[3] = {0xff, 0xff, 0xff};
    EngineLightClearRegisteredList();
    EngineLightSetAmbientRgbBytes(White);
}

void ShipViewControl::SetPosition(float const* NewPosition)
{
    // Slot 5 is the branch, Win3D's own copy of three dwords -- not WinControl's the routine.
    Win3D::SetPosition(NewPosition);
}

bool ShipViewControl::Slot6(unsigned A)
{
    // (RET 4): A is a position; true when its view box lies strictly inside the
    // Win3D camera's viewport (the field..the field).
    ComputeScreenRect(reinterpret_cast<float const*>(static_cast<uintptr_t>(A)), false);
    FSpaceCamera const* const WinCamera = Win3DGetCamera();
    int const Width = WinCamera->Viewport[2] - WinCamera->Viewport[0] + 1;
    int const Height = WinCamera->Viewport[3] - WinCamera->Viewport[1] + 1;
    return ScreenRect[0] > 0 && ScreenRect[2] < Width && ScreenRect[1] > 0 &&
           ScreenRect[3] < Height;
}

void ShipViewControl::Update()
{
    // no base Update call.
    if (ClientMeshHolderHasInstance(&ShipHolder))
    {
        // rotate while any rot_speed component squared is non-zero
        // (TEST AH,0x41 / JP, the last one JNP to the still branch; NaN rotates).
        double const Square0 = static_cast<double>(RotSpeed[0]) * RotSpeed[0];
        double const Square1 = static_cast<double>(RotSpeed[1]) * RotSpeed[1];
        double const Square2 = static_cast<double>(RotSpeed[2]) * RotSpeed[2];
        if (Square0 != 0.0 || Square1 != 0.0 || Square2 != 0.0)
        {
            // the frame's step (the routine) added to +0x4d8.
            double const Dt = GetClientFrameTimeCapped();
            float const Step0 = static_cast<float>(Dt * RotSpeed[0]);
            float const Step1 = static_cast<float>(Dt * RotSpeed[1]);
            float const Step2 = static_cast<float>(Dt * RotSpeed[2]);
            RotAccum[0] = Step0 + RotAccum[0];
            RotAccum[1] = Step1 + RotAccum[1];
            RotAccum[2] = Step2 + RotAccum[2];

            // the turned orientation onto the ship instance.
            FTransform Turned;
            float* const M = &Turned.Rotation.D[0][0];
            HudMatrixRotationY(M, (ObjectYpr[0] + RotAccum[0]) * kDegToRad);
            HudMatrixRotateRows12(M, (RotAccum[1] + ObjectYpr[1]) * kDegToRad);
            HudMatrixRotateRows01(M, (RotAccum[2] + ObjectYpr[2]) * kDegToRad);
            NeuronRuntime::Engine->SetInstanceOrientation(ShipHolder.EngineInstance, M);
            ClientMeshHolderUpdate(&ShipHolder);

            // with no equipment shown, re-centre the hull on its
            // longest axis.
            float Axis[3];
            Axis[0] = 0.0f;
            Axis[1] = 0.0f;
            Axis[2] = 0.0f;
            if (InstanceOf(ShipHolder.EngineInstance) == -1 || Visuals != nullptr)
            {
                ComputeScreenRect(Position, true);
                return;
            }
            void* const Arch = NeuronRuntime::Engine->GetInstanceArchetype(ShipHolder.EngineInstance);
            if (InstanceOf(Arch) == -1)
            {
                ComputeScreenRect(Position, true);
                return;
            }
            float Box[6];
            Box[0] = 0.0f;
            Box[1] = 0.0f;
            Box[2] = 0.0f;
            Box[3] = 0.0f;
            Box[4] = 0.0f;
            Box[5] = 0.0f;
            NeuronRuntime::Renderer->GetBounds(Arch, 1.0f, Box); // IRenderer2 +0x10
            NeuronRuntime::Engine->ReleaseHandle(Arch);

            // the axis whose extent dominates, when the other two
            // together are less than half of the total.
            float const Dx = Box[0] - Box[1];
            float const Dy = Box[2] - Box[3];
            float const Dz = Box[4] - Box[5];
            if (Dx > Dy && Dx > Dz && (Dz + Dy) / (Dx + (Dz + Dy)) < 0.5f)
            {
                Axis[0] = 1.0f;
            }
            else if (Dy > Dx && Dy > Dz && (Dz + Dx) / ((Dz + Dy) + Dx) < 0.5f)
            {
                Axis[1] = 1.0f;
            }
            else if (Dz > Dx && Dz > Dy && (Dy + Dx) / ((Dz + Dy) + Dx) < 0.5f)
            {
                Axis[2] = 1.0f;
            }
            else
            {
                ComputeScreenRect(Position, true);
                return;
            }

            // the framed instance's sphere; the point Radius along the
            // axis through {turned, centre}; the ship moved by half its offset from the centre.
            float Radius;
            FVector Centre;
            NeuronRuntime::Engine->GetInstanceExtent(HandleOf(Instance), 0, &Radius, &Centre.X);
            Turned.Translation = Centre;
            FVector Along;
            Along.X = Radius * Axis[0];
            Along.Y = Axis[1] * Radius;
            Along.Z = Axis[2] * Radius;
            FVector Tip;
            GetShipViewMath()->TransformXform(Tip, Turned, Along); // math +0x28
            FTransform Moved;
            Moved.Rotation = Turned.Rotation;
            Moved.Translation.X = (Tip.X - Centre.X) * 0.5f;
            Moved.Translation.Y = (Tip.Y - Centre.Y) * 0.5f;
            Moved.Translation.Z = (Tip.Z - Centre.Z) * 0.5f;
            NeuronRuntime::Engine->SetInstanceMatrix(ShipHolder.EngineInstance, &Moved);
            NeuronRuntime::Engine->UpdateInstance(ShipHolder.EngineInstance, 0, nullptr);
        }
        else
        {
            // the fixed orientation.
            NeuronRuntime::Engine->SetInstanceOrientation(ShipHolder.EngineInstance, ObjectOrientation);
            ClientMeshHolderUpdate(&ShipHolder);
        }
    }
    ComputeScreenRect(Position, true); // (+0x60, 1)
}

bool ShipViewControl::HitTest(float const* Ray)
{
    // (RET 4): the ray's point at 1000 projected by the Win3D camera, strictly
    // inside the screen rect.
    float World[3];
    World[0] = Ray[0] * 1000.0f;
    World[1] = Ray[1] * 1000.0f;
    World[2] = Ray[2] * 1000.0f;
    float X;
    float Y;
    float Z;
    AsCamera(Win3DGetCamera())->Project(&X, &Y, &Z, World); // camera slot 13 (+0x34)
    return static_cast<double>(ScreenRect[0]) < X && static_cast<double>(ScreenRect[2]) > X &&
           static_cast<double>(ScreenRect[1]) < Y && static_cast<double>(ScreenRect[3]) > Y;
}

void ShipViewControl::DragRotate(bool bStart)
{
    // (RET 4).
    float Ray[3];
    Win3DGetMouseRay(Ray);
    float World[3];
    World[0] = Ray[0] * 1000.0f;
    World[1] = Ray[1] * 1000.0f;
    World[2] = Ray[2] * 1000.0f;
    float X;
    float Y;
    float Z;
    AsCamera(Win3DGetCamera())->Project(&X, &Y, &Z, World);

    // strictly inside the rect (left / top through a float store).
    float const Left = static_cast<float>(ScreenRect[0]);
    if (!(X > Left))
        return;
    if (!(static_cast<double>(ScreenRect[2]) > X))
        return;
    float const Top = static_cast<float>(ScreenRect[1]);
    if (!(Y > Top))
        return;
    if (!(static_cast<double>(ScreenRect[3]) > Y))
        return;

    // the two angles of the mouse inside the rect.
    float const HalfWidth = static_cast<float>(ScreenRect[2] - ScreenRect[0]) * 0.5f;
    float const HalfHeight = static_cast<float>(ScreenRect[3] - ScreenRect[1]) * 0.5f;
    float Angles[3];
    Angles[0] = 180.0f - (Left + HalfWidth - X) / fabsf(HalfWidth) * 90.0f;
    Angles[1] = -90.0f - (Top + HalfHeight - Y) / fabsf(HalfHeight) * 90.0f;
    Angles[2] = 0.0f;
    if (bStart)
    {
        DragLast[0] = Angles[0];
        DragLast[1] = Angles[1];
        DragLast[2] = Angles[2];
        return;
    }

    // the difference, yaw mirrored while the pitch is past +-180.
    float Delta0 = Angles[0] - DragLast[0];
    float const Delta1 = Angles[1] - DragLast[1];
    float const Delta2 = -DragLast[2];
    if (DragYpr[1] > 180.0f || DragYpr[1] < -180.0f)
        Delta0 = -Delta0;
    DragYpr[0] = Delta0 + DragYpr[0];
    DragYpr[1] = Delta1 + DragYpr[1];
    DragYpr[2] = Delta2 + DragYpr[2];
    DragLast[0] = Angles[0];
    DragLast[1] = Angles[1];
    DragLast[2] = Angles[2];

    // yaw and pitch wrapped into [-360, 360] (the field /
    // doubles, the field step).
    if (DragYpr[0] < -360.0)
    {
        float Value = DragYpr[0];
        do
            Value += 360.0f;
        while (Value < -360.0);
        DragYpr[0] = Value;
    }
    if (DragYpr[1] < -360.0)
    {
        float Value = DragYpr[1];
        do
            Value += 360.0f;
        while (Value < -360.0);
        DragYpr[1] = Value;
    }
    if (DragYpr[0] > 360.0)
    {
        float Value = DragYpr[0];
        do
            Value -= 360.0f;
        while (Value > 360.0);
        DragYpr[0] = Value;
    }
    if (DragYpr[1] > 360.0)
    {
        float Value = DragYpr[1];
        do
            Value -= 360.0f;
        while (Value > 360.0);
        DragYpr[1] = Value;
    }

    // object_ypr = the drag angles, orientation rebuilt inline.
    ObjectYpr[0] = DragYpr[0];
    ObjectYpr[1] = DragYpr[1];
    ObjectYpr[2] = DragYpr[2];
    BuildYprOrientation(ObjectOrientation, ObjectYpr);
}

void ShipViewControl::SetDragYpr(float const* Ypr)
{
    // (RET 4).
    DragYpr[0] = Ypr[0];
    DragYpr[1] = Ypr[1];
    DragYpr[2] = Ypr[2];
}

void ShipViewControl::LoadEquipment(EquipDescList const* List)
{
    // (RET 4).
    if (Visuals != nullptr)
    {
        Visuals->Clear();
        delete Visuals;   // + operator delete
        Visuals = nullptr;
    }
    if (List == nullptr)
        return;

    // erase the list, then std::list<EquipDesc>::operator= (import
    // ).
    Equipment.Items.clear();
    Equipment.Items = List->Items;

    // operator_new(0x2c), the routine, the routine(+0x4b8).
    Visuals = new FEquipVisualSet();
    Visuals->Populate(Instance);

    // drop internal items and items on a hardpoint the summary lacks.
    HardpointSummary* const Summary = Visuals->Summary;
    std::list<EquipDesc>::iterator It = Equipment.Items.begin();
    while (It != Equipment.Items.end())
    {
        EquipDesc const& Desc = *It;
        if (Desc.is_internal() || Summary->find_name(Desc.Hardpoint) == -1)
        {
            It = Equipment.Items.erase(It);
            continue;
        }
        ++It;
    }
    Visuals->AttachAll(&Equipment);

    // one mountpoint marker per hardpoint.
    for (size_t HolderIndex = 0; HolderIndex < MountHolders.size(); ++HolderIndex)
        DeleteMountHolder(MountHolders[HolderIndex]);
    MountHolders.clear();
    int const Count = Visuals->Count;
    if (MountHolders.capacity() < static_cast<size_t>(Count < 0 ? 0 : Count))
        MountHolders.reserve(static_cast<size_t>(Count < 0 ? 0 : Count));
    for (int Remaining = Count; Remaining > 0; --Remaining)
    {
        // operator_new(0x28), the routine + the routine, vftable 0x005d2efc, anim -1.
        FClientMeshHolder* const Holder = new FClientMeshHolder();
        ClientMeshHolderConstruct(Holder);
        ClientMeshLoadFromPath(Holder, "Interface\\NeuroNet\\Inventory\\mountpoint.3db", 0,
                               true); // (path, 0, 1)
        MountHolders.push_back(Holder);
    }
}

void ShipViewControl::SetInstance(long NewInstance, bool bFixedDistance)
{
    // (RET 8).
    if (NewInstance == -1)
    {
        Instance = -1;
        if (ClientMeshHolderHasInstance(&ShipHolder))
            ClientUiMeshHolderRelease(&ShipHolder);
        return;
    }
    Instance = NewInstance;
    void* const Handle = HandleOf(NewInstance);

    // the instance's own transform, its sphere centre in world space
    // (the routine) and back into that transform's frame (the routine).
    FTransform Xform;
    memcpy(&Xform, NeuronRuntime::Engine->GetInstanceTransform(Handle), sizeof(Xform));
    float Radius = 0.0f;
    FVector Centre;
    NeuronRuntime::Engine->GetInstanceExtent(Handle, 0, &Radius, &Centre.X);
    // the instance transform fetched again (Engine +0xa0) and handed to
    // as `this` -- HUD_Target_Base's the field, not this file's the field.
    FVector World;
    HudTransformPoint(
        reinterpret_cast<FTransform const*>(NeuronRuntime::Engine->GetInstanceTransform(Handle)), &World,
        &Centre);
    FVector Local;
    HudInverseTransformPoint(&Xform, &Local, &World);

    // the root sphere as a box (the routine / the routine; baseline
    // recomputes Local between the two), then every child folded in (the routine).
    FHudInstanceBounds Bounds;
    Bounds.Min.X = Local.X - Radius;
    Bounds.Min.Y = Local.Y - Radius;
    Bounds.Min.Z = Local.Z - Radius;
    HudInverseTransformPoint(&Xform, &Local, &World);
    Bounds.Max.X = Local.X + Radius;
    Bounds.Max.Y = Local.Y + Radius;
    Bounds.Max.Z = Local.Z + Radius;
    Bounds.ExpandChildren(Handle, &Xform, true);

    // the centre, then the half of the wider of x and z.
    Center[0] = (Bounds.Max.X + Bounds.Min.X) * 0.5f;
    Center[1] = (Bounds.Max.Y + Bounds.Min.Y) * 0.5f;
    Center[2] = (Bounds.Max.Z + Bounds.Min.Z) * 0.5f;
    float const HalfX = Bounds.Max.X - Center[0];
    float const HalfY = Bounds.Max.Y - Center[1];
    float const HalfZ = Bounds.Max.Z - Center[2];
    Distance = sqrtf(HalfX * HalfX + HalfY * HalfY + HalfZ * HalfZ); // , overwritten
    float const SpanX = Bounds.Max.X - Bounds.Min.X;
    float const SpanZ = Bounds.Max.Z - Bounds.Min.Z;
    Distance = static_cast<float>(static_cast<double>(SpanX > SpanZ ? SpanX : SpanZ) * 0.5);

    // plus the length of the extent centre; 14 when fixed.
    float ExtentRadius = Distance;
    FVector ExtentCentre;
    NeuronRuntime::Engine->GetInstanceExtent(HandleOf(Instance), 0, &ExtentRadius, &ExtentCentre.X);
    Distance = sqrtf(ExtentCentre.Y * ExtentCentre.Y + ExtentCentre.Z * ExtentCentre.Z +
                     ExtentCentre.X * ExtentCentre.X) +
               Distance;
    if (bFixedDistance)
        Distance = 14.0f;

    PlaceCamera(1);
    ScreenRect[3] = 0;
    ScreenRect[2] = 0;
    ScreenRect[1] = 0;
    ScreenRect[0] = 0;
}

void ShipViewControl::PlaceCamera(int PlaceMode)
{
    // (RET 4).
    if (Instance == -1)
        return;

    // the distance that fits the framed radius into the FOV --
    // cot(fov * pi / 180) * radius, at least 2; twice the radius below 1.
    double const Cot = 1.0 / tan(static_cast<double>(FovY) * static_cast<double>(3.14159265f) *
                                 (1.0 / 180.0));
    float Reach;
    if (Distance < 1.0f)
    {
        Reach = Distance + Distance;
    }
    else
    {
        double const Scaled = Cot * Distance;
        Reach = (Scaled < 2.0) ? 2.0f : static_cast<float>(Scaled);
    }

    FTransform Eye;
    FVector Focus;
    if (PlaceMode == 1)
    {
        // the instance transform turned -pi/2 (the field) about its
        // x axis, the focus through the untouched engine transform.
        memcpy(&Eye, NeuronRuntime::Engine->GetInstanceTransform(HandleOf(Instance)), sizeof(Eye));
        double const Cos = cos(-1.5707963705062866); // , a float's pi/2 as double
        double const Sin = sin(-1.5707963705062866);
        for (int Row = 0; Row < 3; ++Row)
        {
            double const M1 = Eye.Rotation.D[Row][1];
            double const M2 = Eye.Rotation.D[Row][2];
            Eye.Rotation.D[Row][1] = static_cast<float>(Sin * M2 + M1 * Cos);
            Eye.Rotation.D[Row][2] = static_cast<float>(Cos * M2 - M1 * Sin);
        }
        FVector CenterVec;
        CenterVec.X = Center[0];
        CenterVec.Y = Center[1];
        CenterVec.Z = Center[2];
        GetShipViewMath()->TransformXform(
            Focus,
            *reinterpret_cast<FTransform const*>(
                NeuronRuntime::Engine->GetInstanceTransform(HandleOf(Instance))),
            CenterVec);
    }
    else if (PlaceMode == 0)
    {
        // the instance transform as it is.
        memcpy(&Eye, NeuronRuntime::Engine->GetInstanceTransform(HandleOf(Instance)), sizeof(Eye));
        FVector CenterVec;
        CenterVec.X = Center[0];
        CenterVec.Y = Center[1];
        CenterVec.Z = Center[2];
        GetShipViewMath()->TransformXform(Focus, Eye, CenterVec);
    }
    else
    {
        return;
    }

    // eye = focus + Reach * column 2; the camera's world transform.
    float const BackX = Eye.Rotation.D[0][2] * Reach;
    float const BackY = Eye.Rotation.D[1][2] * Reach;
    float const BackZ = Eye.Rotation.D[2][2] * Reach;
    Eye.Translation.X = Focus.X + BackX;
    Eye.Translation.Y = Focus.Y + BackY;
    Eye.Translation.Z = Focus.Z + BackZ;
    Camera.bViewDirty = 1;                         // +0x3bc
    memcpy(Camera.World, &Eye, sizeof(Eye));       // +0x38c: World[9] and WorldPos[3]
}

bool ShipViewControl::LoadMesh(char const* Path, char const* MaterialPath, bool bFixedDistance,
                               bool bDpConnect)
{
    // (RET 0x10).
    if (Path != nullptr && Path[0] != '\0')
    {
        ClientUiMeshHolderRelease(&ShipHolder);
        // the routine(Path, MaterialPath, 0).
        ClientMeshLoadFromPath(&ShipHolder, Path, MaterialPath, false);
        // the routine(+0x334) -- the player ship's damaged parts.
        if (bDpConnect)
            GfShipApplyCollisionDamage(ShipHolder.EngineInstance);
        SetInstance(InstanceOf(ShipHolder.EngineInstance), bFixedDistance);

        // move the ship so that its sphere centre sits at the origin
        // of the framed transform: identity rotation, TransposeTransform(rotation, c - pos).
        float Radius;
        FVector ExtentCentre;
        NeuronRuntime::Engine->GetInstanceExtent(HandleOf(Instance), 0, &Radius, &ExtentCentre.X);
        FTransform Framed;
        memcpy(&Framed, NeuronRuntime::Engine->GetInstanceTransform(HandleOf(Instance)), sizeof(Framed));
        FVector Delta;
        Delta.X = ExtentCentre.X - Framed.Translation.X;
        Delta.Y = ExtentCentre.Y - Framed.Translation.Y;
        Delta.Z = ExtentCentre.Z - Framed.Translation.Z;
        FVector Local;
        GetShipViewMath()->TransposeTransform(Local, Framed.Rotation, Delta); // math +0x38
        FTransform Placed;
        SetIdentity9(&Placed.Rotation.D[0][0]);
        Placed.Translation = Local;
        NeuronRuntime::Engine->SetInstanceMatrix(ShipHolder.EngineInstance, &Placed);
        NeuronRuntime::Engine->UpdateInstance(ShipHolder.EngineInstance, 0, nullptr);
    }
    return Instance != -1;
}

bool ShipViewControl::LoadShip(ID_String const* NewShipId, bool bFixedDistance, bool bDpConnect)
{
    // (RET 0xc).
    LoadEquipment(nullptr);
    ShipId.Id = NewShipId->Id;
    Archetype::Ship* const Ship = Archetype::GetShip(ShipId.Id);

    // the routine(companion + 0x20, entries, 4) keeps the ship's
    // material libraries referenced while the mesh loads; the routine on each afterwards.
    Archetype::FCliMaterialEntry* Entries[4];
    int const EntryCount = Archetype::ResolveMaterialEntryList(
        Archetype::CliObjMaterialPaths(Ship->Companion), Entries, 4);
    LoadMesh(Ship->DaArchetype.Value, 0, bFixedDistance, bDpConnect);
    for (int Index = 0; Index < EntryCount; ++Index)
        Archetype::ReleaseMaterialEntrySlot(&Entries[Index]);
    return true;
}

void ShipViewControl::SelectInstance(long NewSelection, bool bFlag, int Unused)
{
    // (RET 0xc); the third argument's slot only serves as scratch.
    (void)Unused;
    if (NewSelection == -1)
        return;
    ViewFlags = static_cast<unsigned char>((ViewFlags & ~0x10) | (bFlag ? 0x10 : 0));
    SelInstance = NewSelection;
    float Radius;
    float Centre[3];
    NeuronRuntime::Engine->GetInstanceExtent(HandleOf(NewSelection), 0, &Radius, Centre);
    SelRadius = Radius;
    SelExtent = Radius;
    void* const Arch = NeuronRuntime::Engine->GetInstanceArchetype(HandleOf(NewSelection));
    if (InstanceOf(Arch) == -1)
        return;
    float Box[6];
    Box[0] = 0.0f;
    Box[1] = 0.0f;
    Box[2] = 0.0f;
    Box[3] = 0.0f;
    Box[4] = 0.0f;
    Box[5] = 0.0f;
    NeuronRuntime::Renderer->GetBounds(Arch, 1.0f, Box);
    // FCOMP, TEST AH,5 / JP -- the radius when it is below the x span.
    float const SpanX = Box[0] - Box[1];
    SelRadius = (Radius < SpanX) ? Radius : SpanX;
    NeuronRuntime::Engine->ReleaseHandle(Arch);
}

void ShipViewControl::SelectHpClass(int HpClass, Archetype::Equipment const* Weapon,
                                    Archetype::Equipment const* Shield, bool bFlag)
{
    // (RET 0x10): bits 4 and 5 cleared, bit 5 = bFlag.
    SelHpClass = HpClass;
    SelWeapon = Weapon;
    SelShield = Shield;
    ViewFlags = static_cast<unsigned char>((ViewFlags & 0xcf) | (bFlag ? 0x20 : 0));
    SelInstance = -1;
}

void ShipViewControl::SelectEquipDesc(EquipDesc const* Desc, bool bFlag)
{
    // (RET 8).
    Archetype::Equipment const* Weapon = nullptr;
    Archetype::Equipment const* Shield = nullptr;
    int const HpClass = ShipViewEquipmentHpClass(Desc->ArchId, &Weapon, &Shield);
    SelHpClass = HpClass;
    SelWeapon = Weapon;
    SelShield = Shield;
    ViewFlags = static_cast<unsigned char>((ViewFlags & 0xcf) | (bFlag ? 0x20 : 0));
    SelInstance = -1;
}

void ShipViewControl::SelectGood(unsigned GoodId, bool bFlag)
{
    // (RET 8).
    Archetype::Equipment const* Weapon = nullptr;
    Archetype::Equipment const* Shield = nullptr;
    int HpClass = 0;
    if (GoodId != 0)
    {
        GoodInfo const* const Good = GoodList::find_by_id(GoodId);
        if (Good != nullptr)
            HpClass = ShipViewEquipmentHpClass(Good->ArchId, &Weapon, &Shield);
    }
    SelShield = Shield;
    SelHpClass = HpClass;
    SelWeapon = Weapon;
    ViewFlags = static_cast<unsigned char>((ViewFlags & 0xcf) | (bFlag ? 0x20 : 0));
    SelInstance = -1;
}

char const* ShipViewControl::GetHardpointOfInstance(long ItemInstance) const
{
    // (RET 4).
    if (ItemInstance == -1 || Visuals == nullptr)
        return nullptr;
    for (int Index = 0; Index < Visuals->Count; ++Index)
    {
        if (Visuals->GetInstance(Index) == ItemInstance && Visuals->Summary != nullptr)
            return Visuals->Summary->get_name(Index);
    }
    return nullptr;
}

long ShipViewControl::GetInstanceOnHardpoint(char const* Name) const
{
    // (RET 4).
    if (Visuals == nullptr)
        return -1;
    HardpointSummary const* const Summary = Visuals->Summary;
    if (Summary == nullptr)
        return -1;
    for (int Index = 0; Index < Summary->get_count(); ++Index)
    {
        if (_stricmp(Summary->get_name(Index), Name) == 0) // NEURONCOMPONENTS stricmp
            return Visuals->GetInstance(Index);
    }
    return -1;
}

char const* ShipViewControl::FindHardpointForEquipDesc(EquipDesc const* Desc,
                                                       bool bFirstMatch) const
{
    // (RET 8).
    if (Desc == nullptr)
        return nullptr;
    return FindHardpointForGood(Arch2Good(Desc->ArchId), bFirstMatch);
}

char const* ShipViewControl::FindHardpointForGood(unsigned GoodId, bool bFirstMatch) const
{
    // (RET 8).
    if (Visuals == nullptr || GoodId == 0)
        return nullptr;

    Archetype::Equipment const* Weapon = nullptr;
    Archetype::Equipment const* Shield = nullptr;
    GoodInfo const* const Good = GoodList::find_by_id(GoodId);
    int const HpClass =
        (Good != nullptr) ? ShipViewEquipmentHpClass(Good->ArchId, &Weapon, &Shield) : 0;

    Archetype::Ship const* const Ship = Archetype::GetShipByName(ShipId);
    HardpointSummary const* const Summary = Visuals->Summary;
    std::vector<char const*> Names; // the free weapon hardpoints (the routine appends)

    int const EntryCount = static_cast<int>(Ship->LegalHps.size());
    for (int EntryIndex = 0; EntryIndex < EntryCount; ++EntryIndex)
    {
        Archetype::LegalHpTypes const& Entry = Ship->LegalHps[EntryIndex];
        int const Type = static_cast<int>(Entry.Type);

        // a weapon whose Gun hp types include this entry's type. baseline
        // calls Gun's accessors on whatever class-4 equipment it got (hp types 4, 0xa, 0xd).
        bool bWeaponMatch = false;
        if (HpClass == 4 && Weapon != nullptr)
        {
            Archetype::Gun const* const Gun = static_cast<Archetype::Gun const*>(Weapon);
            int const TypeCount = Gun->get_number_of_hp_types();
            for (int TypeIndex = 0; TypeIndex < TypeCount; ++TypeIndex)
            {
                if (Type == static_cast<int>(Gun->get_hp_type_by_index(TypeIndex)))
                {
                    bWeaponMatch = true;
                    break;
                }
            }
        }

        // the same for a shield generator of class 5..8.
        bool bShieldMatch = false;
        if (Shield != nullptr && (HpClass == 5 || HpClass == 6 || HpClass == 7 || HpClass == 8))
        {
            Archetype::ShieldGenerator const* const Generator =
                static_cast<Archetype::ShieldGenerator const*>(Shield);
            int const TypeCount = Generator->get_number_of_hp_types();
            for (int TypeIndex = 0; TypeIndex < TypeCount; ++TypeIndex)
            {
                if (Type == static_cast<int>(Generator->get_hp_type_by_index(TypeIndex)))
                {
                    bShieldMatch = true;
                    break;
                }
            }
        }

        if (Type != HpClass && !bWeaponMatch && !bShieldMatch)
            continue;

        // every legal hardpoint of the entry the summary knows.
        int const HardpointCount = static_cast<int>(Entry.Hardpoints.size());
        for (int HardpointIndex = 0; HardpointIndex < HardpointCount; ++HardpointIndex)
        {
            char const* const LegalName = Entry.Hardpoints[HardpointIndex].Value;
            if (Summary == nullptr)
                continue;
            for (int SummaryIndex = 0; SummaryIndex < Summary->get_count(); ++SummaryIndex)
            {
                char const* const HpName = Summary->get_name(SummaryIndex);
                if (HpName == nullptr || _stricmp(HpName, LegalName) != 0)
                    continue;
                if (bFirstMatch)
                    return HpName;
                if (Visuals->GetInstance(SummaryIndex) != -1)
                    continue;
                if (!bWeaponMatch)
                    return HpName;
                Names.push_back(HpName);
            }
        }

        // an exact hp class match ends the search.
        if (!bWeaponMatch && !bShieldMatch)
            break;
    }

    // one candidate is the answer; among several, the one listed first
    // in +0x560 (rank 9999 to start, index 0 by default).
    int const NameCount = static_cast<int>(Names.size());
    if (NameCount <= 0)
        return nullptr;
    if (NameCount == 1)
        return Names[0];
    int Best = 0;
    int BestRank = 9999;
    int const ListCount = static_cast<int>(HpList.size());
    for (int NameIndex = 0; NameIndex < NameCount; ++NameIndex)
    {
        for (int ListIndex = 0; ListIndex < ListCount; ++ListIndex)
        {
            if (_stricmp(HpList[static_cast<size_t>(ListIndex)].Name.c_str(), Names[NameIndex]) !=
                0)
                continue;
            if (ListIndex < BestRank)
            {
                BestRank = ListIndex;
                Best = NameIndex;
            }
            break;
        }
    }
    return Names[static_cast<size_t>(Best)];
}

void ShipViewControl::ComputeScreenRect(float const* Where, bool bSetCamera)
{
    // (RET 8): the corners (attach + offset + Where) -/+ viewSize projected by
    // the Win3D camera and truncated (ftol).
    float const BaseX = AttachPos[0] + ViewOffset[0];
    float const BaseY = AttachPos[1] + ViewOffset[1];
    float const BaseZ = AttachPos[2] + ViewOffset[2];
    float const PointX = BaseX + Where[0];
    float const PointY = BaseY + Where[1];
    float const PointZ = BaseZ + Where[2];
    ICamera* const WinCamera = AsCamera(Win3DGetCamera());

    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    float Corner[3];
    Corner[0] = PointX - ViewSize[0];
    Corner[1] = PointY - ViewSize[1];
    Corner[2] = PointZ - ViewSize[2];
    WinCamera->Project(&X, &Y, &Z, Corner);
    ScreenRect[0] = static_cast<int>(X);
    ScreenRect[1] = static_cast<int>(Y);
    Corner[0] = PointX + ViewSize[0];
    Corner[1] = PointY + ViewSize[1];
    Corner[2] = PointZ + ViewSize[2];
    WinCamera->Project(&X, &Y, &Z, Corner);
    ScreenRect[2] = static_cast<int>(X);
    ScreenRect[3] = static_cast<int>(Y);

    if (ScreenRect[0] < 0 || ScreenRect[1] < 0 || ScreenRect[2] < 0 || ScreenRect[3] < 0)
    {
        ScreenRect[3] = 0;
        ScreenRect[2] = 0;
        ScreenRect[1] = 0;
        ScreenRect[0] = 0;
        return;
    }
    if (bSetCamera)
    {
        SpaceCameraSetViewportEx(&Camera, ScreenRect, 1);
        // (0.1f, 10000.0f): the two stores at +0xa4 / +0xa8.
        Camera.ZNear = 0.1f;
        Camera.ZFar = 10000.0f;
    }
}

intptr_t ShipViewControl::SetProperty(int Msg, uintptr_t A, uintptr_t B)
{
    // (RET 0xc).
    if (Msg == 0x19)
    {
        // CMP WORD [A],0 -- bit 3 = (low word of A != 0).
        unsigned char const Bit = (static_cast<unsigned short>(A) != 0) ? 8 : 0;
        ViewFlags = static_cast<unsigned char>((ViewFlags & ~8) | Bit);
        return 0;
    }
    if (Msg == 0x4c)
    {
        Mode = static_cast<int>(A);
        return 0;
    }
    return WinControl::SetProperty(Msg, A, B); // JMP the routine
}

bool ShipViewControl::HasHpType(int HpType) const
{
    // (RET 4).
    for (size_t Index = 0; Index < HpTypes.size(); ++Index)
    {
        if (HpTypes[Index] == HpType)
            return true;
    }
    return false;
}

void ShipViewControl::CollectHpTypes(char const* HpName)
{
    // (RET 4).
    HpTypes.clear(); // + destroy range
    if (HpName == nullptr || ShipId.IsEmpty())
        return;
    Archetype::Ship const* const Ship = Archetype::GetShipByName(ShipId);
    int const EntryCount = static_cast<int>(Ship->LegalHps.size());
    for (int EntryIndex = 0; EntryIndex < EntryCount; ++EntryIndex)
    {
        Archetype::LegalHpTypes const& Entry = Ship->LegalHps[EntryIndex];
        int const HardpointCount = static_cast<int>(Entry.Hardpoints.size());
        for (int HardpointIndex = 0; HardpointIndex < HardpointCount; ++HardpointIndex)
        {
            if (_stricmp(HpName, Entry.Hardpoints[HardpointIndex].Value) == 0)
                HpTypes.push_back(static_cast<int>(Entry.Type)); // inline insert(end, 1, &type)
        }
    }
}

bool ShipViewControl::HasHpTypeGroup(int Group) const
{
    // (RET 4).
    for (size_t Index = 0; Index < HpTypes.size(); ++Index)
    {
        if (HpTypeGroup(HpTypes[Index]) == Group)
            return true;
    }
    return false;
}

void ShipViewControl::SetViewSize(float const* NewViewSize)
{
    // (RET 4).
    ViewSize[0] = NewViewSize[0];
    ViewSize[1] = NewViewSize[1];
    ViewSize[2] = NewViewSize[2];
}

void ShipViewControl::SetRotSpeed(float const* NewRotSpeed)
{
    // (RET 4).
    RotSpeed[0] = NewRotSpeed[0];
    RotSpeed[1] = NewRotSpeed[1];
    RotSpeed[2] = NewRotSpeed[2];
}

void ShipViewControl::SetObjectYpr(float const* NewYpr)
{
    // (RET 4): the three rotations inlined.
    ObjectYpr[0] = NewYpr[0];
    ObjectYpr[1] = NewYpr[1];
    ObjectYpr[2] = NewYpr[2];
    BuildYprOrientation(ObjectOrientation, ObjectYpr);
}

void ShipViewControl::SetAttachPosition(float const* NewPosition)
{
    // (RET 4).
    AttachPos[0] = NewPosition[0];
    AttachPos[1] = NewPosition[1];
    AttachPos[2] = NewPosition[2];
}

void ShipViewControl::SetShowRect(bool bShow)
{
    // (RET 4).
    ViewFlags = static_cast<unsigned char>((ViewFlags & ~4) | (bShow ? 4 : 0));
}

void ShipViewControl::SetShowWireframe(bool bShow)
{
    // (RET 4).
    ViewFlags = static_cast<unsigned char>((ViewFlags & ~8) | (bShow ? 8 : 0));
}

void ShipViewControl::SetRectColor(float const* Rgba)
{
    // (RET 4): components 3, 2, 1, 0 converted, stored 0..3.
    unsigned char const B3 = ColorByte(Rgba[3]);
    unsigned char const B2 = ColorByte(Rgba[2]);
    unsigned char const B1 = ColorByte(Rgba[1]);
    unsigned char const B0 = ColorByte(Rgba[0]);
    RectColor[0] = B0;
    RectColor[3] = B3;
    RectColor[1] = B1;
    RectColor[2] = B2;
}

void ShipViewControl::SetWireColor(float const* Rgba)
{
    // (RET 4).
    unsigned char const B3 = ColorByte(Rgba[3]);
    unsigned char const B2 = ColorByte(Rgba[2]);
    unsigned char const B1 = ColorByte(Rgba[1]);
    unsigned char const B0 = ColorByte(Rgba[0]);
    WireColor[0] = B0;
    WireColor[3] = B3;
    WireColor[1] = B1;
    WireColor[2] = B2;
}

void ShipViewControl::BuildHardpointList()
{
    // .
    HpList.clear();
    if (Visuals == nullptr)
        return;

    // per hardpoint the highest hp type (by value) the caption table
    // lists among its legal types.
    HardpointSummary const* const Summary = Visuals->Summary;
    std::vector<FShipViewHpEntry> Collected;
    if (Summary != nullptr)
    {
        for (int Index = 0; Index < Summary->get_count(); ++Index)
        {
            Summary->get_info(Index); // the result is not used
            char const* const Name = Summary->get_name(Index);
            CollectHpTypes(Name);
            if (HpTypes.empty())
                continue;
            int BestRow = -1;
            int BestType = 0;
            for (int Row = 0; Row < 0x3c; ++Row)
            {
                int const RowType = G_ShipViewHpTypeCaptions[Row].HpType;
                for (size_t TypeIndex = 0; TypeIndex < HpTypes.size(); ++TypeIndex)
                {
                    if (RowType == HpTypes[TypeIndex] && RowType > BestType)
                    {
                        BestType = RowType;
                        BestRow = Row;
                    }
                }
            }
            if (BestRow < 0)
                continue;
            FShipViewHpEntry Entry;
            Entry.Ids = G_ShipViewHpTypeCaptions[BestRow].Ids;
            Entry.Name = Name;
            Entry.SortKey = BestRow;
            Entry.Group = HpTypeGroup(BestType);
            Entry.Class = ShipViewHpTypeClass(BestType);
            Collected.push_back(Entry);
        }
    }

    // an index list bubble-sorted by row descending, then by name
    // descending (swap when the left one is smaller).
    std::vector<int> Order;
    int const CollectedCount = static_cast<int>(Collected.size());
    for (int Index = 0; Index < CollectedCount; ++Index)
        Order.push_back(Index);
    int const Last = CollectedCount - 1;
    bool bSorted = true;
    do
    {
        if (Last <= 0)
            break;
        bSorted = true;
        for (int Index = 0; Index < Last; ++Index)
        {
            FShipViewHpEntry const& Left = Collected[static_cast<size_t>(Order[Index])];
            FShipViewHpEntry const& Right = Collected[static_cast<size_t>(Order[Index + 1])];
            bool bSwap = false;
            if (Left.SortKey < Right.SortKey)
                bSwap = true;
            else if (Left.SortKey == Right.SortKey &&
                     _stricmp(Left.Name.c_str(), Right.Name.c_str()) < 0)
                bSwap = true;
            if (bSwap)
            {
                int const Swapped = Order[Index];
                Order[Index] = Order[Index + 1];
                Order[Index + 1] = Swapped;
                bSorted = false;
            }
        }
    } while (!bSorted);

    // stored from the back of the sorted order (ascending), each copy
    // with +0x14 = 0 (0x00586f04 stores EBX, zeroed at 0x00586ebc).
    for (int Index = Last; Index >= 0; --Index)
    {
        FShipViewHpEntry const& Source = Collected[static_cast<size_t>(Order[Index])];
        FShipViewHpEntry Copy;
        Copy.Ids = Source.Ids;
        Copy.Name = Source.Name;
        Copy.SortKey = 0;
        Copy.Group = Source.Group;
        Copy.Class = Source.Class;
        HpList.push_back(Copy);
    }
}

int ShipViewControl::GetHardpointListCount() const
{
    // .
    return static_cast<int>(HpList.size());
}

FShipViewHpEntry const* ShipViewControl::GetHardpointListEntry(int Index) const
{
    // (RET 4).
    if (Index < 0 || static_cast<size_t>(Index) >= HpList.size())
        return nullptr;
    return &HpList[static_cast<size_t>(Index)];
}

WinControl* WinControl::CreateShipViewControl(char const* Name, float const* RotSpeed,
                                              float const* NewOffset, float const* Ypr,
                                              float const* NewViewSize, char const* Hardpoint,
                                              bool bShowRect, bool bShowWire,
                                              unsigned const* RectColor,
                                              unsigned const* WireColor)
{
    // (RET 0x28): the routine(Name, "ShipViewControl", this, 0), then Init.
    Win3D* const Created = WinFactoryCreate(Name, "ShipViewControl", this, nullptr);
    if (Created != nullptr)
    {
        static_cast<ShipViewControl*>(Created)->Init(RotSpeed, NewOffset, Ypr, NewViewSize,
                                                     Hardpoint, bShowRect, bShowWire, RectColor,
                                                     WireColor);
    }
    return static_cast<WinControl*>(Created);
}
