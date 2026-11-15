// Author: Giperion.

#include "hud_element_list.h"

#include "common_cequip.h"
#include "common_cobject.h"
#include "common_csys.h"
#include "common_iface.h"
#include "common_ini.h"
#include "font_string.h"
#include "game_options.h"
#include "globals.h"
#include "widescreen.h"   // port-only (OUTPOST_FRONTIER_WIDESCREEN_SUPPORT)
#include "hud_status_core.h"
#include "input_manager.h"
#include "nn_ui_pulse.h"
#include "player_behavior.h"
#include "space_letterbox.h"
#include "texture_shapes.h"
#include "tooltip_hover.h"
#include "win3d.h"

#include "NeuronRuntime.h"
#include "neuroncomponents.h"
#include "engine/sceneruntime.h"
#include "render/rendcomp.h"
#include "render/shading.h"
#include "x86math.h"

#include <math.h>
#include <string.h>

namespace
{
    HUDItem_Factory* G_HudFactoryHead = nullptr;
    HUDItem_Factory* G_HudFactoryTail = nullptr;

    HUDItem* G_HudItemHead = nullptr;
    HUDItem* G_HudItemTail = nullptr;

    // / the field -- the routine's copy of the hud.ini path (cap 0x103).
    unsigned G_HudIniPathLength = 0;
    char G_HudIniPath[0x104] = {};

    // the [HUD] static_txm texture (FTextureRef's ctor is the static
    // initializer the branch, its dtor the atexit thunk the branch).
    FTextureRef G_HudStaticTexture;

    // [HUD] static_blend, initialised data 2.
    int G_HudStaticBlend = 2;

    // HUD_Steer::Render's 3DMathEngine (the routine on first use).
    I3DMathEngine* G_HudSteerMath = nullptr;

    char const kHudCpp[] = "Client\\space\\HUD.cpp"; // .rdata 0x005d6dcc

    // (and the same lines in the routine): the player's IObjRW from
    // , `+ 0xc` tested against null, the CEqObj* at [+0x10], and the ship class
    // test ([CObject+0x4c] & 0x503) == 0x503.
    CEqObj* HudPlayerShip()
    {
        void* const ObjRW = GetPlayerObjRW();
        // `ADD EAX,0xc / TEST EAX,EAX`: an IObjRW* of -0xc is the other "no object" value.
        if (ObjRW == nullptr || ObjRW == reinterpret_cast<void*>(static_cast<intptr_t>(-0xc)))
            return nullptr;
        CEqObj* const EqObj =
            *reinterpret_cast<CEqObj**>(static_cast<unsigned char*>(ObjRW) + 0x10);
        if (EqObj == nullptr)
            return nullptr;
        if ((static_cast<unsigned>(EqObj->ObjectClass) & 0x503u) != 0x503u)
            return nullptr;
        return EqObj;
    }

    // The cursor Y block repeated at 0x004c8745 and 0x004c8885: with the mouse-flight lock on
    // (the routine clear and the field set) or the hold timer past 0.25s, "INVERSE MOUSE"
    // (option 0x17) flips the cursor against the laid-out surface height the field.
    int HudSteerCursorY(int CursorY)
    {
        bool const bFlightView = IsMouseFlightLocked() || IsMouseFlightHoldThresholdReached();
        if (bFlightView && GetGameOption(kGameOptInverseMouse) != 0)
            return static_cast<int>(G_RenderSurfaceHeight) - CursorY;
        return CursorY;
    }
}

void HudVec3ScaleByInverse(float* Out, float const* In, float Length)
{
    // FLD 1.0 / FDIV Length / FST float -- Z uses the unrounded reciprocal, Y and
    // X the float re-read.
    double const InvExact = 1.0 / static_cast<double>(Length);
    float const Inv = static_cast<float>(InvExact);
    float const Z = static_cast<float>(InvExact * In[2]);
    float const Y = static_cast<float>(static_cast<double>(Inv) * In[1]);
    float const X = static_cast<float>(static_cast<double>(Inv) * In[0]);
    Out[0] = X;
    Out[1] = Y;
    Out[2] = Z;
}

void HudMatrixRotationY(float* M, float Angle)
{
    // the zeros and the 1, then cos / sin (FCOS / FSIN of the same argument).
    double const C = cos(static_cast<double>(Angle));
    double const S = sin(static_cast<double>(Angle));
    M[1] = 0.0f;
    M[3] = 0.0f;
    M[4] = 1.0f;
    M[5] = 0.0f;
    M[7] = 0.0f;
    M[0] = static_cast<float>(C);
    M[2] = static_cast<float>(S);
    M[6] = static_cast<float>(-S);
    M[8] = static_cast<float>(C);
}

void HudMatrixRotateRows12(float* M, float Angle)
{
    // .
    double const C = cos(static_cast<double>(Angle));
    double const S = sin(static_cast<double>(Angle));
    for (int Col = 0; Col < 3; ++Col)
    {
        double const A = M[3 + Col];
        double const B = M[6 + Col];
        M[3 + Col] = static_cast<float>(A * C - S * B);
        M[6 + Col] = static_cast<float>(A * S + C * B);
    }
}

void HudMatrixRotateRows01(float* M, float Angle)
{
    // .
    double const C = cos(static_cast<double>(Angle));
    double const S = sin(static_cast<double>(Angle));
    for (int Col = 0; Col < 3; ++Col)
    {
        double const A = M[Col];
        double const B = M[3 + Col];
        M[Col] = static_cast<float>(A * C - S * B);
        M[3 + Col] = static_cast<float>(C * B + A * S);
    }
}

// ============================================================================================
// HUDItem
// ============================================================================================

HUDItem::HUDItem()
{
    // The inlined constructor (0x004c84de .. 0x004c8516, 0x004c8de3 .. 0x004c8e05).
    Name.Length = 0;
    Name.Data[0] = '\0';
    Target.set(nullptr);
    Next = nullptr;
    Target.set(nullptr);
    ToolTipIds = 0;
}

HUDItem::~HUDItem()
{
    // `if (+0x28) set_pointer(0)` -- BaseWatcher::set(0); the member's own
    // destructor then finds nothing to unlink.
    if (Target.target() != nullptr)
        Target.set(nullptr);
}

bool HUDItem::Slot1()
{
    // .
    return false;
}

void HUDItem::SetViewport(int const* ViewRect4)
{
    // .
    (void)ViewRect4;
}

bool HUDItem::Init()
{
    // .
    return true;
}

void HUDItem::Shutdown()
{
    // .
}

void HUDItem::SetTarget(IObjRW* NewTarget)
{
    // the Watchable is IObjRW + 8 (null stays null); set_pointer only when it
    // differs from the one watched, which is BaseWatcher::set.
    Watchable const* const Watched = (NewTarget == nullptr)
        ? nullptr
        : reinterpret_cast<Watchable const*>(reinterpret_cast<unsigned char const*>(NewTarget) + 8);
    Target.set(Watched);
}

void HUDItem::Render(FSpaceCamera* Camera, float DeltaSeconds)
{
    // .
    (void)Camera;
    (void)DeltaSeconds;
}

bool HUDItem::Slot7(void* Command)
{
    // .
    (void)Command;
    return false;
}

bool HUDItem::Slot8(void* Command)
{
    // .
    (void)Command;
    return false;
}

bool HUDItem::Slot9(void* Command)
{
    // .
    (void)Command;
    return false;
}

void HUDItem::Slot10()
{
    // .
}

void HUDItem::ShowToolTip()
{
    // . Nothing without a tool-tip id (0x004c81f3).
    if (ToolTipIds == 0)
        return;

    // the hover record on the stack -- caption null, the Font::String
    // built (the routine) -- the record's constructor -- then +0x04 = 0, +0x00 = -1, +0x44 = 0
    // and the (null) caption emptied, +0x60 = 0, then +0x04 = the id and +0x00 = this + id (the
    // ToolTip's "same source" key).
    FToolTipHoverInfo Info;
    Info.IdsAlt = 0;
    Info.Ids = -1;
    Info.AuxField = 0;
    Info.bLive = 0;
    Info.IdsAlt = ToolTipIds;
    Info.Ids = static_cast<int>(reinterpret_cast<intptr_t>(this) + ToolTipIds);
    NnToolTipFeedHover(&Info);

    // the record's teardown (~FToolTipHoverInfo) -- the routine,
    // the child releases (the routine per slot), the child vector freed, then the caption
    // buffer (null here).
}

// ============================================================================================
// HUDItem_Factory
// ============================================================================================

HUDItem_Factory::HUDItem_Factory(char const* Name)
{
    // The inlined base constructor (e.g. 0x004cd270 .. 0x004cd2cb): +0x08 = 0, +0x0c = 0,
    // +0x04 = 0, the name (a null name leaves it empty), then the routine.
    SectionName.Length = 0;
    SectionName.Data[0] = '\0';
    Next = nullptr;
    SectionName = Name;
    HudElementFactoryListAdd(this);
}

HUDItem_Factory::~HUDItem_Factory()
{
    // the base vptr back, then the routine's body inlined.
    HudElementFactoryListRemove(this);
}

HUDItem* HUDItem_Factory::Create(INI_Reader& Reader)
{
    // .
    (void)Reader;
    return nullptr;
}

HUDItem* HUDItem_Factory::Slot1(void* Arg)
{
    // .
    (void)Arg;
    return nullptr;
}

// ============================================================================================
// Registry
// ============================================================================================

void HudElementFactoryListInit()
{
    // .
    G_HudFactoryHead = nullptr;
    G_HudFactoryTail = nullptr;
}

void HudElementFactoryListAdd(HUDItem_Factory* Factory)
{
    // no null test on the argument.
    if (G_HudFactoryTail != nullptr)
    {
        G_HudFactoryTail->Next = Factory;
        G_HudFactoryTail = Factory;
        return;
    }
    G_HudFactoryHead = Factory;
    G_HudFactoryTail = Factory;
}

void HudElementFactoryListRemove(HUDItem_Factory* Factory)
{
    // .
    HUDItem_Factory* Prev = nullptr;
    HUDItem_Factory* Cur = G_HudFactoryHead;
    if (Cur == nullptr)
        return;
    while (Cur != Factory)
    {
        Prev = Cur;
        Cur = Cur->Next;
        if (Cur == nullptr)
            return;
    }
    if (Prev != nullptr)
        Prev->Next = Cur->Next;
    else
        G_HudFactoryHead = Cur->Next;
    if (Cur == G_HudFactoryTail)
        G_HudFactoryTail = Prev;
    Factory->Next = nullptr;
}

void HudElementListInit()
{
    // .
    G_HudItemHead = nullptr;
    G_HudItemTail = nullptr;
}

void HudElementListSetTarget(IObjRW* NewTarget)
{
    // .
    for (HUDItem* Item = G_HudItemHead; Item != nullptr; Item = Item->Next)
        Item->SetTarget(NewTarget);
}

void HudElementListSetViewport(int const* ViewRect4)
{
    // .
    for (HUDItem* Item = G_HudItemHead; Item != nullptr; Item = Item->Next)
        Item->SetViewport(ViewRect4);
}

void HudElementListRender(FSpaceCamera* Camera, float DeltaSeconds)
{
    // .
    NeuronRuntime::MaterialBatcher->SetBinOrder(1, -1);
    for (HUDItem* Item = G_HudItemHead; Item != nullptr; Item = Item->Next)
        Item->Render(Camera, DeltaSeconds);
    NeuronRuntime::MaterialBatcher->SetBinOrder(1, 1);
}

bool HudElementListAnySlot7(void* Command)
{
    // .
    for (HUDItem* Item = G_HudItemHead; Item != nullptr; Item = Item->Next)
    {
        if (Item->Slot7(Command))
            return true;
    }
    return false;
}

bool HudElementListAnySlot8(void* Command)
{
    // .
    for (HUDItem* Item = G_HudItemHead; Item != nullptr; Item = Item->Next)
    {
        if (Item->Slot8(Command))
            return true;
    }
    return false;
}

void HudElementListShutdown()
{
    // .
    if (G_HudStaticTexture.Texture != nullptr)
    {
        NeuronRuntime::TextureLib->ReleaseTextureRecord(G_HudStaticTexture.Texture);
        G_HudStaticTexture.Texture = nullptr;
    }
    for (HUDItem* Item = G_HudItemHead; Item != nullptr; Item = Item->Next)
        Item->Shutdown();
    HUDItem* Item = G_HudItemHead;
    while (Item != nullptr)
    {
        HUDItem* const NextItem = Item->Next;
        delete Item;
        Item = NextItem;
    }
    G_HudItemTail = nullptr;
    G_HudItemHead = nullptr;
    // JMP the routine on the field.
    G_HudShapeDB.Clear();
}

void SetHudIniPath(char const* Path)
{
    // .
    if (Path == nullptr)
    {
        G_HudIniPathLength = 0;
        G_HudIniPath[0] = '\0';
        return;
    }
    unsigned Len = static_cast<unsigned>(strlen(Path));
    if (Len >= 0x104)
        Len = 0x103;
    memcpy(G_HudIniPath, Path, Len);
    G_HudIniPath[Len] = '\0';
    G_HudIniPathLength = Len;
}

char const* GetHudIniPath()
{
    return G_HudIniPath;
}

FTextureRef* GetHudStaticTexture()
{
    return &G_HudStaticTexture;
}

int GetHudStaticBlend()
{
    return G_HudStaticBlend;
}

void LoadHudElementsFromIni()
{
    // .
    INI_Reader Reader;
    if (!Reader.open(G_HudIniPath, false))
    {
        // .
        FDUMP(0x100002, "%s(%d) : *** WARNING: hud file not found '%s'\n", kHudCpp, 0x13d,
              G_HudIniPath);
    }
    else if (Reader.read_header())
    {
        do
        {
            if (Reader.is_header("HUD"))
            {
                while (Reader.read_value())
                {
                    if (Reader.is_value("shapes"))
                    {
                        // the routine on the field against NeuronRuntime::Data.
                        G_HudShapeDB.Load(Reader.get_value_string(), NeuronRuntime::Data);
                    }
                    else if (Reader.is_value("static_txm"))
                    {
                        // the routine on the field.
                        TextureRefLoad(&G_HudStaticTexture, Reader.get_value_string());
                    }
                    else if (Reader.is_value("static_blend"))
                    {
                        // get_value_float(0), ftol.
                        G_HudStaticBlend = static_cast<int>(Reader.get_value_float(0));
                    }
                }
                continue;
            }

            // every factory whose name matches the header is asked in chain order;
            // the first one that returns an element wins.
            HUDItem* Item = nullptr;
            for (HUDItem_Factory* Factory = G_HudFactoryHead; Factory != nullptr;
                 Factory = Factory->Next)
            {
                if (!Reader.is_header(Factory->SectionName.Data))
                    continue;
                Item = Factory->Create(Reader);
                if (Item != nullptr)
                    break;
            }

            if (Item == nullptr)
            {
                // .
                FDUMP(0x100002, "%s(%d) : *** WARNING: HUD - unknown INI entry '%s'\n", kHudCpp,
                      0x136, Reader.get_header_ptr());
                continue;
            }

            if (Item->Init())
            {
                // append through +0x34.
                if (G_HudItemTail != nullptr)
                {
                    G_HudItemTail->Next = Item;
                    G_HudItemTail = Item;
                }
                else
                {
                    G_HudItemHead = Item;
                    G_HudItemTail = Item;
                }
            }
            else
            {
                // vt[0](1).
                delete Item;
            }
        } while (Reader.read_header());
    }

    // .
    ResetMouseFlightState();
}

float HudVectorAngle(float X, float Y)
{
    // .
    float const AbsX = fabsf(X);
    float const AbsY = fabsf(Y);
    if (X == 0.0f && Y == 0.0f)
        return 0.0f;

    double const HalfPi = 1.5707963705062866;
    double const Pi = 3.1415927410125732;
    double const MinusPi = -3.1415927410125732;

    if (AbsX > AbsY)
    {
        // FPATAN of (Y / |X|, 1).
        double const A = atan(static_cast<double>(Y) / AbsX);
        if (X > 0.0f)
            return static_cast<float>(A - HalfPi);
        return static_cast<float>(HalfPi - A);
    }

    // FPATAN of (-X / |Y|, 1).
    double const A = atan(-(static_cast<double>(X) / AbsY));
    if (!(Y < 0.0f))
        return static_cast<float>(A);
    if (A > 0.0)
        return static_cast<float>(Pi - A);
    return static_cast<float>(MinusPi - A);
}

// ============================================================================================
// HUD_Steer
// ============================================================================================

HUD_Steer::HUD_Steer(INI_Reader& Reader)
{
    // after operator new(0x74): the HUDItem part, +0x30 = 0, the holder at +0x4c
    // (the routine, five 1.0f, anim -1), the name "Steer" (the routine, cap 0x20), +0x38 = 0,
    // an inlined SetTarget(0), the three floats and +0x48, then the steering-arrow mesh
    // (the routine(path, 0, 1)) and the ini overrides.
    ClientMeshHolderConstruct(&MeshHolder);
    MeshHolder.UserFloats[4] = 1.0f;
    MeshHolder.UserFloats[3] = 1.0f;
    MeshHolder.UserFloats[2] = 1.0f;
    MeshHolder.UserFloats[1] = 1.0f;
    MeshHolder.UserFloats[0] = 1.0f;
    MeshHolder.AnimOrArchIndex = -1;
    Name = "Steer";
    Byte38 = 0;
    Target.set(nullptr);
    Field44 = 0.2f;
    Range = 200.0f;
    Radius = 40.0f;
    Byte48 = 0;
    ClientMeshLoadFromPath(&MeshHolder, "interface\\hud\\HUD_steeringarrow.3db", 0, true);

    while (Reader.read_value())
    {
        if (Reader.is_value("radius"))
            Radius = Reader.get_value_float(0);
        else if (Reader.is_value("range"))
            Range = Reader.get_value_float(0);
    }
}

HUD_Steer::~HUD_Steer()
{
    // with an instance bound, the routine; then the holder's destructor
    // (~FClientMeshHolder: the same release again, which finds nothing); then
    // ~HUDItem.
    if (MeshHolder.EngineInstance != reinterpret_cast<void*>(static_cast<intptr_t>(-1)))
        ClientUiMeshHolderRelease(&MeshHolder);
}

bool HUD_Steer::Slot1()
{
    // .
    return Byte38 != 0;
}

void HUD_Steer::Render(FSpaceCamera* Camera, float DeltaSeconds)
{
    // (RET 8). Neither argument is read.
    (void)Camera;
    (void)DeltaSeconds;

    // shown unless the player's ship says otherwise -- its behaviour
    // interface byte +0x168 when it has one, then a tradelane ride or the letterbox hides it.
    // Without a ship the arrow stays enabled.
    unsigned char bShow = 1;
    CEqObj* const Ship = HudPlayerShip();
    if (Ship != nullptr)
    {
        IBehaviorManager* const Behavior = Ship->get_behavior_interface();
        if (Behavior != nullptr)
            bShow = reinterpret_cast<unsigned char const*>(Behavior)[0x168];
        if (static_cast<CShip*>(Ship)->is_using_tradelane())
            bShow = 0;
        else if (IsLetterboxVisible())
            bShow = 0;
    }

    // under the 0.25s hold, only a locked mouse-flight draws
    // (the routine clear and the field set -- IsMouseFlightLocked).
    if (!IsMouseFlightHoldThresholdReached() && !IsMouseFlightLocked())
        return;
    // not in the turret or rear view (HUD_Status +0x5b9 / +0x5ba).
    if (HudStatusIsTurretView() || HudStatusIsRearView())
        return;
    // the routine.
    if (ShouldSkipLocalServerTick())
        return;
    if (bShow == 0)
        return;

    int CursorX = 0;
    int CursorY = 0;
    int CursorZ = 0;
    GetClientSoftwareCursor(&CursorX, &CursorY, &CursorZ);
    int const SurfaceW = static_cast<int>(G_RenderSurfaceWidth);
    int const SurfaceH = static_cast<int>(G_RenderSurfaceHeight);

    // the cursor's offset from the surface centre, divided by
    // -(range * width * 0.00125) (the routine), and radius / range as the dead zone.
    float const CursorYf = static_cast<float>(HudSteerCursorY(CursorY));
    float const HalfW = static_cast<float>(SurfaceW / 2);
    float const HalfH = static_cast<float>(SurfaceH / 2);
#ifdef OUTPOST_FRONTIER_WIDESCREEN_SUPPORT
    // Port-only Hor+: the steering circle is interface, scaled by height like the rest of it.
    float const RangePixels = static_cast<float>(
        static_cast<double>(Range) * WidescreenEquivalentSurfaceWidth() * 0.0012499999720603228);
#else
    float const RangePixels =
        static_cast<float>(static_cast<double>(Range) * SurfaceW * 0.0012499999720603228);
#endif
    int const CursorYi = static_cast<int>(CursorYf);
    float const Cursor[3] = {static_cast<float>(CursorX), static_cast<float>(CursorYi), 0.0f};
    float const Offset[3] = {Cursor[0] - HalfW, Cursor[1] - HalfH, 0.0f};
    float Dir[3];
    HudVec3ScaleByInverse(Dir, Offset, -RangePixels);
    float const DeadZone = static_cast<float>(static_cast<double>(Radius) / Range);

    // the routine's length clamped to 1, and zero inside the dead
    // zone.
    double const LengthExact = sqrt(static_cast<double>(Dir[0]) * Dir[0] +
                                    static_cast<double>(Dir[1]) * Dir[1] +
                                    static_cast<double>(Dir[2]) * Dir[2]);
    float Length = static_cast<float>(LengthExact);
    if (LengthExact > 1.0)
        Length = 1.0f;
    // TEST AH,0x41 / JP keeps the length when it is above the dead zone or unordered.
    if (Length <= DeadZone)
        Length = 0.0f;

    // the arrow's roll, -the routine(dx, dy) with dx = x - w/2 and
    // dy = h/2 - y.
    int const CursorYi2 = static_cast<int>(static_cast<float>(HudSteerCursorY(CursorY)));
    float const Dx = static_cast<float>(CursorX - SurfaceW / 2);
    float const Dy = static_cast<float>(SurfaceH / 2 - CursorYi2);
    float const Angle = -HudVectorAngle(Dx, Dy);

    // the routine.
    Win3DBeginDraw();

    // the HUD chrome colour into the holder's user floats.
    double const Inv255 = 0.003921568859368563;
    MeshHolder.UserFloats[0] = static_cast<float>(G_HudChromeColorRgba[0] * Inv255);
    MeshHolder.UserFloats[1] = static_cast<float>(G_HudChromeColorRgba[1] * Inv255);
    MeshHolder.UserFloats[2] = static_cast<float>(G_HudChromeColorRgba[2] * Inv255);
    MeshHolder.UserFloats[4] = static_cast<float>(G_HudChromeColorRgba[3] * Inv255);

    // the routine(0), the routine(0), then the routine with the
    // roll times -0.99999994f (the field).
    FMatrix Rotation;
    HudMatrixRotationY(&Rotation.D[0][0], 0.0f);
    HudMatrixRotateRows12(&Rotation.D[0][0], 0.0f);
    HudMatrixRotateRows01(&Rotation.D[0][0],
                          static_cast<float>(static_cast<double>(Angle) * -0.9999999403953552));

    // one arrow segment every 0.15 (the field) while below the
    // length: placed at Rotation * (0, t * 0.265, 0) with z = -1, scaled by t * 0.5 + 0.1, drawn
    // through the Win3D camera the field.
    float const Step = 0.15000000596046448f;
    float T = Step;
    if (Step < Length)
    {
        do
        {
            FVector Local;
            Local.X = 0.0f;
            Local.Y = static_cast<float>(static_cast<double>(T) * 0.26499998569488525);
            Local.Z = 0.0f;
            if (G_HudSteerMath == nullptr)
                G_HudSteerMath = Acquire3DMathEngine();
            FVector Placed;
            G_HudSteerMath->TransformMatrix(Placed, Rotation, Local);

            FTransform Xform;
            Xform.Rotation = Rotation;
            Xform.Translation.X = Placed.X;
            Xform.Translation.Y = Placed.Y;
            Xform.Translation.Z = -1.0f;
            if (G_HudSteerMath == nullptr)
                G_HudSteerMath = Acquire3DMathEngine();
            float const Scale = static_cast<float>(static_cast<double>(T) * 0.5 +
                                                   0.10000000149011612);
            G_HudSteerMath->Scale(Xform.Rotation, Xform.Rotation, Scale);

            NeuronRuntime::Engine->SetInstanceMatrix(MeshHolder.EngineInstance, &Xform);
            NeuronRuntime::Engine->UpdateInstance(MeshHolder.EngineInstance, 0, nullptr);
            NeuronRuntime::Renderer->RenderInstance(Win3DGetCamera(), MeshHolder.EngineInstance, 1.0f, 0, nullptr,
                                   nullptr);

            // FADD / FST into T, then FCOMP of the unrounded sum against the length.
            double const Sum = static_cast<double>(T) + Step;
            T = static_cast<float>(Sum);
            if (!(Sum < Length))
                break;
        } while (true);
    }

    // the routine.
    Win3DEndDraw();
}

HUDItem* HUD_SteerFactory::Create(INI_Reader& Reader)
{
    // operator new(0x74); a failed allocation returns 0.
    return new HUD_Steer(Reader);
}

namespace
{
    // the static factory (XC entry the branch: the routine(this, "Steer"),
    // atexit the branch -> the branch).
    HUD_SteerFactory G_HudSteerFactory("Steer");
}
