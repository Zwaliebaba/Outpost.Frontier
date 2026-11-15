// Author: Giperion.

#include "hud_target.h"

#include "character_select_equip.h" // GetPlayerBaseIdbaseline (the field)
#include "client_gameobj.h"         // FClientGameobj : IObjInspectImpl
#include "client_objdb.h"           // ClientObjDb_FindObject (the routine)
#include "font_string.h"
#include "globals.h"                // IsClientServerTickPaused (the routine)
#include "ids_strings.h"            // GetIdsStringWWithRawFallback (the routine)
#include "input_manager.h"          // GetClientMouseCursorDrawFlag, GetClientSoftwareCursor
#include "menu_universe_base.h"     // IsClientLocationInTransition (the field)
#include "misc_util.h"              // FmtStrSprintf (the routine)
#include "nn_modal.h"               // G_bNnScreenUp (the field)
#include "nn_text_scratch.h"        // G_NnTextScratchA/B (the field / the field)
#include "nn_ui_pulse.h"            // G_HudChromeColorRgba (the field)
#include "engine_light.h"           // ScaleVec3 (the routine)
#include "player_behavior.h"        // GetPlayerObjRW (the routine)
#include "player_spawn.h"           // IsPlayerShipId (the routine)
#include "space_camera.h"           // AsCamera
#include "star_sphere.h"            // Win3DIdentityRotation3x3 (the routine)
#include "win3d_immediate.h"        // Win3DSetViewport (the routine), the routine
#include "space_letterbox.h"        // IsLetterboxVisible (the routine)
#include "tooltip_hover.h"          // FToolTipHoverInfo, NnToolTipFeedHover (the routine)
#include "win3d.h"                  // Win3DGetCamera (the field)

#include "common_cequip.h"
#include "common_cobject.h"
#include "common_csys.h"            // Acquire3DMathEngine (the routine)
#include "common_iface.h"           // IObjRW, IObjInspectImpl, ObjRwFromWatchable
#include "common_misc.h"            // SUBOBJ_ID_NONE, ICamera
#include "common_reputation.h"      // Reputation::Vibe::GetAffiliation / GetName, get_short_name
#include "common_universe.h"        // Universe::get_system

#include "NeuronRuntime.h"
#include "engine/sceneruntime.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

namespace
{
    // the 3DMathEngine HUD_Target_Base.cpp acquires lazily (the routine) in
    // / the routine / the routine / the routine / the routine.
    I3DMathEngine* G_HudTargetMath = nullptr;

    // the routine's own lazily acquired 3DMathEngine.
    I3DMathEngine* G_HudPickMath = nullptr;

    I3DMathEngine* HudTargetMath()
    {
        if (G_HudTargetMath == nullptr)
        {
            G_HudTargetMath = Acquire3DMathEngine();
        }
        return G_HudTargetMath;
    }

    // Engine instance handles travel as void*; a GetRootIndex / get_index long converts 1:1.
    void* InstanceFromIndex(long Index)
    {
        return reinterpret_cast<void*>(static_cast<intptr_t>(Index));
    }

    void* NoInstance()
    {
        return reinterpret_cast<void*>(static_cast<intptr_t>(-1));
    }

    FTransform const* InstanceTransform(void* Instance)
    {
        // IEngine slot 40 (+0xa0) returns the instance's 12-float transform.
        return reinterpret_cast<FTransform const*>(NeuronRuntime::Engine->GetInstanceTransform(Instance));
    }

    // the slack PickSubTarget hands FHudSpherePick::Test (0x3ecccccd).
    float const kSubTargetPickSlack = 0.4f;

    // .rdata 0x3f333333 -- the chrome dimming factor of SetViewObject's wireframe colour.
    float const kViewWireDim = 0.7f;

    // the routine's 0x15 {type, ids_name} pairs.
    struct FCollisionTypeIds
    {
        unsigned Type;
        unsigned Ids;
    };

    FCollisionTypeIds const kCollisionTypeIds[0x15] = {
        {0x00000000u, 0x708u}, {0x00000001u, 0x708u}, {0x00000002u, 0x709u},
        {0x00000004u, 0x70au}, {0x00000008u, 0x70bu}, {0x00000010u, 0x70cu},
        {0x00000020u, 0x70du}, {0x00000040u, 0x70eu}, {0x00000080u, 0x70fu},
        {0x00000100u, 0x710u}, {0x00000200u, 0x711u}, {0x00000400u, 0x712u},
        {0x00000800u, 0x713u}, {0x00001000u, 0x714u}, {0x00002000u, 0x715u},
        {0x00004000u, 0x716u}, {0x00008000u, 0x717u}, {0x00010000u, 0x718u},
        {0x00020000u, 0x719u}, {0x00040000u, 0x71au}, {0x00080000u, 0x71bu},
    };

    // The inlined chain baseline spells out at every use (0x004e6166.., 0x004e7a20.., 0x004e7e2e..):
    // watched -> -8 (IObjRW) -> +0xc (IObjInspectImpl's CBase) -> +4 (CBase::Object), null
    // anywhere along it -> null.
    CObject* CObjectOfWatcher(BaseWatcher const& Watcher)
    {
        Watchable const* const Watched = Watcher.target();
        if (Watched == nullptr)
        {
            return nullptr;
        }
        IObjRW* const Rw = ObjRwFromWatchable(const_cast<Watchable*>(Watched));
        if (Rw == nullptr)
        {
            return nullptr;
        }
        IObjInspectImpl* const Impl = static_cast<IObjInspectImpl*>(Rw);
        CBase const* const Base = Impl;
        return Base->cobject();
    }

    // (cdecl) -- clamp to 0..255.
    int ClampByte(int Value)
    {
        if (Value < 0)
        {
            return 0;
        }
        if (Value > 0xff)
        {
            return 0xff;
        }
        return Value;
    }

    // FILD byte / FMUL 0.7f / ftol: the product of an 8-bit integer and a float is exact in
    // double, and ftol truncates.
    int DimChromeByte(unsigned char Byte)
    {
        return static_cast<int>(static_cast<double>(Byte) * static_cast<double>(kViewWireDim));
    }
}

// =============================================================================================
// FHudInstanceBounds
// =============================================================================================

FVector* HudInverseTransformPoint(FTransform const* Xform, FVector* Out, FVector const* In)
{
    // InverseTransform into a local, then copied to Out.
    FVector Local;
    HudTargetMath()->InverseTransform(Local, *Xform, *In);
    *Out = Local;
    return Out;
}

FHudInstanceBounds::FHudInstanceBounds(void* Instance, FTransform const* Xform,
                                       bool bIncludeShieldParts)
{
    // the root part's sphere (extent flag 0, radius pre-zeroed).
    float Radius = 0.0f;
    FVector Center;
    NeuronRuntime::Engine->GetInstanceExtent(Instance, 0, &Radius, &Center.X);
    FTransform const* const Transform = InstanceTransform(Instance);
    FVector World;
    HudTargetMath()->TransformXform(World, *Transform, Center);
    Center = World;

    // Two separate InverseTransform calls, one for Min and one for Max (0x004e6676 / 0x004e66d5).
    FVector Local;
    HudTargetMath()->InverseTransform(Local, *Xform, Center);
    Min.X = Local.X - Radius;
    Min.Y = Local.Y - Radius;
    Min.Z = Local.Z - Radius;

    HudTargetMath()->InverseTransform(Local, *Xform, Center);
    Max.X = Local.X + Radius;
    Max.Y = Local.Y + Radius;
    Max.Z = Local.Z + Radius;

    // the child loop -- the same body as the routine.
    ExpandChildren(Instance, Xform, bIncludeShieldParts);
}

FVector* FHudInstanceBounds::GetCenter(FVector* Out) const
{
    // (the field = 0.5f).
    Out->X = (Min.X + Max.X) * 0.5f;
    Out->Y = (Min.Y + Max.Y) * 0.5f;
    Out->Z = (Min.Z + Max.Z) * 0.5f;
    return Out;
}

void FHudInstanceBounds::ExpandChildren(void* Instance, FTransform const* Xform,
                                        bool bIncludeShieldParts)
{
    // GetRelatedInstance(Instance, 1, cursor) from -1 until it returns -1.
    void* Child = NeuronRuntime::Engine->GetRelatedInstance(Instance, 1, NoInstance());
    while (Child != NoInstance())
    {
        if (bIncludeShieldParts || !IsShieldPart(Child))
        {
            FVector Center;
            float const Radius = GetPartSphere(Child, &Center);

            // Min: v - (r, r, r), component-wise minimum (FCOMP / TEST AH,5 / JP: taken only
            // when strictly below).
            FVector Local;
            HudInverseTransformPoint(Xform, &Local, &Center);
            FVector Low;
            Low.X = Local.X - Radius;
            Low.Y = Local.Y - Radius;
            Low.Z = Local.Z - Radius;
            if (Low.X < Min.X)
            {
                Min.X = Low.X;
            }
            if (Low.Y < Min.Y)
            {
                Min.Y = Low.Y;
            }
            if (Low.Z < Min.Z)
            {
                Min.Z = Low.Z;
            }

            // Max: the transform again, v + (r, r, r), component-wise maximum (TEST AH,0x41 /
            // JNZ: taken only when strictly above).
            HudInverseTransformPoint(Xform, &Local, &Center);
            FVector High;
            High.X = Local.X + Radius;
            High.Y = Local.Y + Radius;
            High.Z = Local.Z + Radius;
            if (High.X > Max.X)
            {
                Max.X = High.X;
            }
            if (High.Y > Max.Y)
            {
                Max.Y = High.Y;
            }
            if (High.Z > Max.Z)
            {
                Max.Z = High.Z;
            }

            ExpandChildren(Child, Xform, bIncludeShieldParts);
        }
        Child = NeuronRuntime::Engine->GetRelatedInstance(Instance, 1, Child);
    }
}

float FHudInstanceBounds::GetPartSphere(void* Instance, FVector* OutCenter) const
{
    // radius pre-zeroed, extent flag 0 straight into OutCenter, then OutCenter is
    // moved through the instance transform.
    float Radius = 0.0f;
    NeuronRuntime::Engine->GetInstanceExtent(Instance, 0, &Radius, &OutCenter->X);
    FTransform const* const Transform = InstanceTransform(Instance);
    FVector World;
    HudTargetMath()->TransformXform(World, *Transform, *OutCenter);
    *OutCenter = World;
    return Radius;
}

bool FHudInstanceBounds::IsShieldPart(void* Instance) const
{
    // the instance's user data is its CObject; baseline inlines inst_to_part and
    // is_shield_part over the +0x68..+0x6c part map (no null test on the user data).
    CObject const* const Object = static_cast<CObject const*>(
        NeuronRuntime::Engine->GetInstanceUserData(Instance));
    long const EngineInst = static_cast<long>(reinterpret_cast<intptr_t>(Instance));
    return Object->is_shield_part(Object->inst_to_part(EngineInst));
}

// =============================================================================================
// FHudSpherePick
// =============================================================================================

FHudSpherePick::FHudSpherePick(unsigned char Flag)
    : HitCount(0)
{
    // .
    BestKey = 0.0f;
    Field50 = -1;
    Field4c = Flag;
}

void FHudSpherePick::Setup(FVector const* From, FVector const* To)
{
    // FDIVR 1.0 by the FSQRT of the squared length, then each component scaled.
    Origin = *From;
    Dir.X = To->X - Origin.X;
    Dir.Y = To->Y - Origin.Y;
    Dir.Z = To->Z - Origin.Z;
    double const Inverse = 1.0 / sqrt(static_cast<double>(Dir.X) * Dir.X +
                                      static_cast<double>(Dir.Y) * Dir.Y +
                                      static_cast<double>(Dir.Z) * Dir.Z);
    Dir.X = static_cast<float>(Inverse * Dir.X);
    Dir.Y = static_cast<float>(Inverse * Dir.Y);
    Dir.Z = static_cast<float>(Inverse * Dir.Z);
    Field18 = -1.0f;
    HitCount = 0;
    Field50 = -1;
    BestKey = 0.0f;
}

float HudRaySphereClosest(FVector const* Origin, FVector const* Dir, FVector const* Center,
                          float Radius, FVector* OutRay, FVector* OutSurface,
                          FTransform const* Xform)
{
    // .
    if (G_HudPickMath == nullptr)
    {
        G_HudPickMath = Acquire3DMathEngine();
    }
    FVector World;
    G_HudPickMath->TransformXform(World, *Xform, *Center);

    FVector ToCenter;
    ToCenter.X = World.X - Origin->X;
    ToCenter.Y = World.Y - Origin->Y;
    ToCenter.Z = World.Z - Origin->Z;

    // t = V.D summed x, z, y; the branch is taken on t < 0 (FCOM 0.0 / TEST AH,5 / JP).
    float const Along = ToCenter.X * Dir->X + ToCenter.Z * Dir->Z + ToCenter.Y * Dir->Y;
    float Cosine = 1.0f;
    if (Along < 0.0f)
    {
        *OutRay = *Origin;
    }
    else
    {
        OutRay->X = Along * Dir->X + Origin->X;
        OutRay->Y = Along * Dir->Y + Origin->Y;
        OutRay->Z = Along * Dir->Z + Origin->Z;
        Cosine = Along / sqrtf(ToCenter.Z * ToCenter.Z + ToCenter.Y * ToCenter.Y +
                               ToCenter.X * ToCenter.X);
    }

    FVector Gap;
    Gap.X = OutRay->X - World.X;
    Gap.Y = OutRay->Y - World.Y;
    Gap.Z = OutRay->Z - World.Z;
    float const Distance = sqrtf(Gap.Z * Gap.Z + Gap.Y * Gap.Y + Gap.X * Gap.X);
    float const Scale = Radius / Distance;
    OutSurface->X = World.X + Gap.X * Scale;
    OutSurface->Y = World.Y + Gap.Y * Scale;
    OutSurface->Z = World.Z + Gap.Z * Scale;

    // FCOMP Radius / TEST AH,5 / JP: the ray point snaps to the surface only when inside.
    if (Distance < Radius)
    {
        *OutRay = *OutSurface;
    }
    return Cosine;
}

bool FHudSpherePick::Test(void* Instance, float Slack)
{
    // every child first.
    bool bHit = false;
    void* Child = NeuronRuntime::Engine->GetRelatedInstance(Instance, 1, NoInstance());
    while (Child != NoInstance())
    {
        if (Test(Child, Slack))
        {
            bHit = true;
        }
        Child = NeuronRuntime::Engine->GetRelatedInstance(Instance, 1, Child);
    }

    // Then this instance's extent-flag-1 sphere.
    float Radius;
    FVector Center;
    NeuronRuntime::Engine->GetInstanceExtent(Instance, 1, &Radius, &Center.X);
    FTransform const* const Transform = InstanceTransform(Instance);
    FVector RayPoint;
    FVector SurfacePoint;
    float const Cosine = HudRaySphereClosest(&Origin, &Dir, &Center, Radius, &RayPoint,
                                             &SurfacePoint, Transform);

    // FCOM double 0.5 / TEST AH,0x41 / JNZ: only a cosine strictly above 0.5 goes on.
    if (!(Cosine > 0.5))
    {
        return bHit;
    }
    float const Key = -Cosine;
    float const Dx = RayPoint.X - SurfacePoint.X;
    float const Dy = RayPoint.Y - SurfacePoint.Y;
    float const Dz = RayPoint.Z - SurfacePoint.Z;
    float const Gap = sqrtf(Dx * Dx + Dz * Dz + Dy * Dy);
    // FCOMP Slack / TEST AH,0x41 / JP: on when the gap is below or equal to the slack.
    if (!(Gap <= Slack))
    {
        return bHit;
    }
    // FCOMP BestKey / TEST AH,5 / JP: a later hit must be strictly better.
    if (HitCount != 0 && !(Key < BestKey))
    {
        return bHit;
    }
    BestKey = Key;
    HitPoint = RayPoint;
    HitSurface = SurfacePoint;
    ++HitCount;
    return true;
}

// =============================================================================================
// Free helpers
// =============================================================================================

bool HudIsMousePickBlocked()
{
    // .
    if (G_bNnScreenUp)
    {
        return true;
    }
    if (GetClientMouseCursorDrawFlag() == 0)
    {
        return true;
    }
    if (IsClientServerTickPaused())
    {
        return true;
    }
    if (IsClientLocationInTransition()) // (0x0045a75b)
    {
        return true;
    }
    // docked: XOR AL,AL / RET.
    if ((GetPlayerBaseIdbaseline() & 0x3fffffffu) != 0)
    {
        return false;
    }
    // NEG / SBB / INC over the routine.
    return GetPlayerObjRW() == nullptr;
}

unsigned HudCollisionGroupTypeIds(unsigned CollisionGroupType)
{
    // .
    for (int Index = 0; Index < 0x15; ++Index)
    {
        if (kCollisionTypeIds[Index].Type == CollisionGroupType)
        {
            return kCollisionTypeIds[Index].Ids;
        }
    }
    return 0;
}

unsigned long* HudDamageColorArgb(unsigned long* OutArgb, float Damage)
{
    // . The first two bands: FCOMP / TEST AH,5 / JP leaves a band on "not below"
    // (unordered included), so each is taken only for Damage strictly below its bound.
    if (Damage < 0.25f)
    {
        memcpy(OutArgb, G_HudChromeColorRgba, sizeof(G_HudChromeColorRgba));
        return OutArgb;
    }
    if (Damage < 0.5f)
    {
        *OutArgb = 0xff4080ffu; // bytes ff, 80, 40, ff
        return OutArgb;
    }
    // TEST AH,0x41 / JNZ: "below, equal or unordered" takes the third band.
    if (!(Damage > 0.75f))
    {
        *OutArgb = 0xff4040ffu; // bytes ff, 40, 40, ff
        return OutArgb;
    }

    // the pulse phase stored as a float over the argument, flipped by the
    // direction byte; FLD 255.0 / FMUL ST1 / ftol, then FILD of that byte / FMUL ST1 / ftol.
    float const Phase = NnUiPulsePhaseNorm();
    double const Level = (NnUiPulseDirection() != 0) ? 1.0 - static_cast<double>(Phase)
                                                      : static_cast<double>(Phase);
    unsigned char const Red = static_cast<unsigned char>(static_cast<int>(255.0 * Level));
    unsigned char const Green =
        static_cast<unsigned char>(static_cast<int>(static_cast<double>(Red) * Level));
    *OutArgb = 0xff000000u | (static_cast<unsigned long>(Green) << 16) |
               (static_cast<unsigned long>(Green) << 8) | static_cast<unsigned long>(Red);
    return OutArgb;
}

int* HudProjectScreenRect(int* OutRect4, FVector const* A, FVector const* B)
{
    // . baseline's Project outputs x / y / z and the rectangle's fallback values share
    // the four stack dwords [ESP+0xc..0x18]. Project writes nothing when it fails, and the pair
    // of a failed projection takes the raw bits left there: left / top from the x / y slots,
    // right / bottom from the z slot and a fourth dword nothing writes. The port keeps the same
    // overlay; what it holds on a failure is as undefined as in baseline.
    union
    {
        float F[4];
        int I[4];
    } Scratch;
    ICamera* const Camera = AsCamera(Win3DGetCamera());

    int Left = 0;
    int Top = 0;
    if (static_cast<unsigned char>(Camera->Project(&Scratch.F[0], &Scratch.F[1], &Scratch.F[2],
                                                   &A->X)) != 0)
    {
        Left = static_cast<int>(Scratch.F[0]);
        Top = static_cast<int>(Scratch.F[1]);
    }
    else
    {
        Left = Scratch.I[0];
        Top = Scratch.I[1];
    }

    int Right = 0;
    int Bottom = 0;
    if (static_cast<unsigned char>(Camera->Project(&Scratch.F[0], &Scratch.F[1], &Scratch.F[2],
                                                   &B->X)) != 0)
    {
        Right = static_cast<int>(Scratch.F[0]);
        Bottom = static_cast<int>(Scratch.F[1]);
    }
    else
    {
        Right = Scratch.I[2];
        Bottom = Scratch.I[3];
    }

    OutRect4[0] = Left;
    OutRect4[1] = Top;
    OutRect4[2] = Right;
    OutRect4[3] = Bottom;
    return OutRect4;
}

FVector* HudTransformPoint(FTransform const* Xform, FVector* Out, FVector const* In)
{
    // TransformXform into a stack vector through the field, copied to Out.
    FVector Result;
    HudTargetMath()->TransformXform(Result, *Xform, *In);
    *Out = Result;
    return Out;
}

CShip* HudShipOfObjRw(IObjRW* ObjRw)
{
    // ECX null -> 0; LEA ECX+0xc (the IObjInspectImpl CBase) null -> 0; its +4
    // Object null -> 0; (Object+0x4c & 0x503) == 0x503 -> Object, else 0.
    if (ObjRw == nullptr)
    {
        return nullptr;
    }
    IObjInspectImpl* const Impl = static_cast<IObjInspectImpl*>(ObjRw);
    CBase* const Base = Impl;
    if (Base == nullptr)
    {
        return nullptr;
    }
    CObject* const Object = Base->cobject();
    if (Object == nullptr)
    {
        return nullptr;
    }
    if ((static_cast<unsigned>(Object->ObjectClass) & 0x503u) != 0x503u)
    {
        return nullptr;
    }
    return static_cast<CShip*>(Object);
}

namespace
{
    // The inlined `IObjRW + 0xc (CBase) -> +4 (Object)` chain of the routine, null anywhere
    // -> null.
    CObject* HudCObjectOf(IObjInspectImpl* Impl)
    {
        if (Impl == nullptr)
        {
            return nullptr;
        }
        CBase* const Base = Impl;
        if (Base == nullptr)
        {
            return nullptr;
        }
        return Base->cobject();
    }

    // CShip +0x1b4 -- the multiplayer group id (common_basefollowerbehavior.cpp module storage compares the
    // same field between two player ships) -- CShip::GroupId, read by offset from a CObject.
    unsigned HudShipGroupId(CObject const* Ship)
    {
        unsigned Value = 0;
        memcpy(&Value, reinterpret_cast<unsigned char const*>(Ship) + 0x1b4, sizeof(Value));
        return Value;
    }
}

bool HudIsUnvisitedSolar(CSimple const* Object)
{
    // .
    // a watch-listed client object of a matching type is known.
    FClientGameobj* const Gameobj = ClientObjDb_FindObject(Object->Id, 0);
    if (Gameobj != nullptr)
    {
        unsigned Type = 0;
        Gameobj->get_type(Type); // vt+0x88; the return value is not looked at
        if ((Type & 0x107ffdffu) != 0 && Gameobj->bWatchListed != 0)
            return false;
    }
    // only a solar can be unvisited.
    if ((static_cast<unsigned>(Object->ObjectClass) & 0x303u) != 0x303u)
        return false;
    // the solar's own visit byte OR the visit-map flags (zero when the
    // id has no entry: the routine leaves the pre-cleared byte alone); bit 0 means visited.
    unsigned char const OwnVisit = static_cast<CSolar const*>(Object)->GetVisitValue();
    unsigned char MapFlags = 0;
    GetPlayerVisitFlagsbaseline(Object->Id, &MapFlags, 0, 0);
    return ((MapFlags | OwnVisit) & 1u) == 0;
}

unsigned char HudObjectVisitFlags(CObject const* Object)
{
    // .
    if (Object == nullptr)
        return 1;
    if ((static_cast<unsigned>(Object->ObjectClass) & 0x303u) != 0x303u)
        return 1;
    CSolar const* const Solar = static_cast<CSolar const*>(Object);
    unsigned char const OwnVisit = Solar->GetVisitValue();
    unsigned char MapFlags = 0;
    GetPlayerVisitFlagsbaseline(Solar->Id, &MapFlags, 0, 0);
    return static_cast<unsigned char>(MapFlags | OwnVisit);
}

unsigned HudObjectTypeIds(CSimple const* Object)
{
    // .
    unsigned const Bits = Object->TypeBits;
    if ((Bits & 0x1u) != 0)
        return 0x4a0;
    if ((Bits & 0x2u) != 0)
        return 0x4a1;
    if ((Bits & 0x4u) != 0)
        return 0x4a2;
    if ((Bits & 0x8010u) != 0)
        return 0x4a3;
    if ((Bits & 0x40u) != 0)
        return 0x4a4;
    if ((Bits & 0x80u) != 0)
        return 0x4a5;
    if ((Bits & 0x100u) != 0)
        return 0x4a6;
    return 0x4a7;
}

I3DMathEngine* G_HudObjectsMath = nullptr;

namespace
{
    // / the field (.data, zero at load; only the routine touches them): the
    // last docking-ring owner base the routine resolved and the ids_name it found for it.
    unsigned G_CaptionOwnerBaseId = 0;
    unsigned G_CaptionOwnerIdsName = 0;

    // The copy every the routine / the routine exit ends with: reserve Length + 1, wcsncpy
    // Length chars, terminate. The reserve is the routine's body, which baseline inlines at
    // , 0x004e8657, 0x004e88be and 0x005472e5 -- WideTextReserve (font_string.cpp).
    void HudWideTextAssign(FHudWideText* Out, wchar_t const* Source, size_t Length)
    {
        WideTextReserve(Out, static_cast<unsigned>(Length + 1));
        wcsncpy(Out->Text, Source, Length);
        Out->Text[Length] = 0;
    }

    // 's exits: the caption built in G_NnTextScratchB goes to Out, true.
    bool HudCaptionFinish(FHudWideText* Out)
    {
        HudWideTextAssign(Out, G_NnTextScratchB, wcslen(G_NnTextScratchB));
        return true;
    }

    // (and 0x004e8916..0x004e89a1): the system's strid_name (ISystem
    // slot 3) and ids 0x561 when the player's system resolves, then the nav-map sector of the
    // object. baseline passes the capacity to the routine as a third argument it never reads.
    void HudCaptionAppendLocation(CSimple const* Object)
    {
        Universe::ISystem const* const System =
            Universe::get_system(GetPlayerStarSystemIdbaseline());
        if (System != nullptr)
        {
            GetIdsStringWWithRawFallback((unsigned)System->GetStridName(),
                                         G_NnTextScratchA, G_NnTextScratchCapacity);
            wcscat(G_NnTextScratchB, G_NnTextScratchA);
            GetIdsStringWWithRawFallback(0x561, G_NnTextScratchA, G_NnTextScratchCapacity);
            wcscat(G_NnTextScratchB, G_NnTextScratchA);
        }
        NavMapSectorName(&Object->WorldPosition, G_NnTextScratchA);
        wcscat(G_NnTextScratchB, G_NnTextScratchA);
    }

    // (also 0x004e87d1.., 0x004e8b92..): " (%d)" (u" (%d)" 0x005cfb88)
    // for a loot object ((class & 0x803) == 0x803) of more than one unit (CLoot +0xf0).
    void HudCaptionAppendLootCount(CSimple const* Object)
    {
        if ((static_cast<unsigned>(Object->ObjectClass) & 0x803u) != 0x803u)
        {
            return;
        }
        unsigned const Units = static_cast<CLoot const*>(Object)->Units;
        if (Units <= 1)
        {
            return;
        }
        wchar_t Count[0x80];
        swprintf(Count, 0x80, L" (%d)", static_cast<int>(Units));
        wcscat(G_NnTextScratchB, Count);
    }

    // Mode 2 puts "\n" (0x005d0254) and the location after the name
    // line, Mode 1 the location alone, any other mode nothing; then the exit copy.
    bool HudCaptionLocationTail(CSimple const* Object, FHudWideText* Out, int Mode)
    {
        if (Mode == 2)
        {
            wcscat(G_NnTextScratchB, L"\n");
            HudCaptionAppendLocation(Object);
        }
        else if (Mode == 1)
        {
            HudCaptionAppendLocation(Object);
        }
        return HudCaptionFinish(Out);
    }

    // for an object that is not a CEqObj, and its copy 0x004e8b4a..
    // for a CEqObj without a faction: the ids_name as the name line; a loot-type
    // object (TypeBits == 0x10000000) answers only in Mode 0 / 2 and takes the unit count
    // instead of a location.
    bool HudCaptionPlain(CSimple const* Object, unsigned Name, unsigned TypeBits,
                         FHudWideText* Out, int Mode)
    {
        if (Mode == 0 || Mode == 2)
        {
            GetIdsStringWWithRawFallback(Name, G_NnTextScratchB, G_NnTextScratchCapacity);
        }
        if (TypeBits != 0x10000000u)
        {
            return HudCaptionLocationTail(Object, Out, Mode);
        }
        if (Mode != 0 && Mode != 2)
        {
            return false;
        }
        HudCaptionAppendLootCount(Object);
        return HudCaptionFinish(Out);
    }
}

unsigned GetObjectIdsName(CObject const* Object)
{
    // (cdecl).
    if (Object == nullptr)
    {
        return 0;
    }
    if ((static_cast<unsigned>(Object->ObjectClass) & 0x503u) == 0x503u)
    {
        // Archetype (+0x88) -> ids_name (+0x14), no null test.
        return static_cast<unsigned>(Object->Archetype->IdsName);
    }
    // JMP [vtable + 0x88] -- CSimple::get_name.
    return static_cast<CSimple const*>(Object)->get_name();
}

unsigned GetShipPilotName(CObject const* Object, FHudWideText* Out)
{
    // (cdecl).
    // Out = "" (the field).
    wchar_t const* const Empty = L"";
    HudWideTextAssign(Out, Empty, wcslen(Empty));
    if (Object != nullptr && (static_cast<unsigned>(Object->ObjectClass) & 0x503u) == 0x503u)
    {
        wchar_t const* const Pilot = static_cast<CShip const*>(Object)->get_pilot_name();
        if (Pilot != nullptr)
        {
            HudWideTextAssign(Out, Pilot, wcslen(Pilot));
        }
        else
        {

            if (Out->Text == nullptr)
            {
                return 0;
            }
            if (Out->Text[0] != 0)
            {
                Out->Text[0] = 0;
            }
        }
    }

    if (Out->Text == nullptr)
    {
        return 0;
    }
    return static_cast<unsigned>(wcslen(Out->Text));
}

void NavMapSectorName(Vector const* Pos, wchar_t* Out)
{
    // (cdecl).
    // the system's NavMapScale (slot 20, +0x50), 1.0 without a system.
    float Scale = 1.0f;
    Universe::ISystem const* const System =
        Universe::get_system(GetPlayerStarSystemIdbaseline());
    if (System != nullptr)
    {
        Scale = System->GetNavMapScale();
    }
    // the row coordinate z * scale + 2700 (the field), the column
    // coordinate x * scale, and the format ids 0x54d into a 0x40-char buffer.
    float const RowCoord = Scale * Pos->z + 2700.0f;
    float const ColumnCoord = Scale * Pos->x;
    wchar_t Format[0x40];
    GetIdsStringWWithRawFallback(0x54d, Format, 0x40);

    // the first band (from -99999, the field, step 33333,
    // ) the coordinate lies below, capped at 8. TEST AH,5 / JNP leaves on "below"
    // only; equal and unordered step on.
    int Row = 0;
    float Bound = -99999.0f;
    for (Row = 0; Row < 8; ++Row)
    {
        if (RowCoord < Bound)
        {
            break;
        }
        Bound += 33333.0f;
    }
    int Column = 0;
    Bound = -99999.0f;
    for (Column = 0; Column < 8; ++Column)
    {
        if (ColumnCoord < Bound)
        {
            break;
        }
        Bound += 33333.0f;
    }

    // the row and column captions (0x10 chars each), then the legacy
    // two-argument swprintf of msvcrt (no bound on Out) -- Format(RowText, ColumnText).
    wchar_t RowText[0x10];
    wchar_t ColumnText[0x10];
    GetIdsStringWWithRawFallback(0x515 + Row, RowText, 0x10);
    GetIdsStringWWithRawFallback(0x506 + Column, ColumnText, 0x10);
    _swprintf(Out, Format, RowText, ColumnText);
}

unsigned HudObjectKindIds(unsigned TypeBits)
{
    // (cdecl).
    if ((TypeBits & 0x1u) != 0)
        return 0x563;
    if ((TypeBits & 0x2u) != 0)
        return 0x44b;
    if ((TypeBits & 0x4u) != 0)
        return 0x564;
    if ((TypeBits & 0x8u) != 0)
        return 0x565;
    if ((TypeBits & 0x8010u) != 0)
        return 0x566;
    if ((TypeBits & 0x20u) != 0)
        return 0x567;
    if ((TypeBits & 0x40u) != 0)
        return 0x568;
    if ((TypeBits & 0x80u) != 0)
        return 0x4fa;
    if ((TypeBits & 0x100u) != 0)
        return 0x44c;
    if ((TypeBits & 0x200u) != 0)
        return 0x442;
    if ((TypeBits & 0x400u) != 0)
        return 0x56a;
    if ((TypeBits & 0x800u) != 0)
        return 0x56b;
    if ((TypeBits & 0x1000u) != 0)
        return 0x56c;
    // AND 0x2000 / NEG / SBB / AND 0x56d.
    return (TypeBits & 0x2000u) != 0 ? 0x56du : 0u;
}

bool HudTradelaneCaption(CSimple const* Object, wchar_t* Out, int Capacity)
{
    // (cdecl).
    CSolar const* Ring = nullptr;
    if (Object != nullptr && (static_cast<unsigned>(Object->ObjectClass) & 0x303u) == 0x303u)
    {
        Ring = static_cast<CSolar const*>(Object);
    }
    IObjRW* const Player = static_cast<IObjRW*>(GetPlayerObjRW());
    if (Player == nullptr || Ring == nullptr)
    {
        return false;
    }

    // the ring's -z axis (its orientation, +0x08, copied with REP
    // MOVSD, times (0, 0, -1)), normalised, and the plane offset -(n . ring position).
    FMatrix const Orientation = reinterpret_cast<FMatrix const&>(Object->Orientation);
    FVector const Back = {0.0f, 0.0f, -1.0f};
    if (G_HudObjectsMath == nullptr)
    {
        G_HudObjectsMath = Acquire3DMathEngine();
    }
    FVector Axis;
    G_HudObjectsMath->TransformMatrix(Axis, Orientation, Back); // vt+0x2c
    // (x*x + y*y) + z*z, FSQRT, FDIVR 1.0f -- inline, not the routine;
    // the reciprocal stays on the FPU (double here). X and Y are stored as float; Z is FST'd
    // to float but the unrounded product feeds the plane offset straight away (0x004e8e22).
    double const Inverse = 1.0 / sqrt(static_cast<double>(Axis.X) * Axis.X +
                                      static_cast<double>(Axis.Y) * Axis.Y +
                                      static_cast<double>(Axis.Z) * Axis.Z);
    float const NormalX = static_cast<float>(Axis.X * Inverse);
    float const NormalY = static_cast<float>(Axis.Y * Inverse);
    double const NormalZExact = Inverse * Axis.Z;
    float const NormalZ = static_cast<float>(NormalZExact);
    Vector const& RingPos = Object->WorldPosition;
    float const PlaneOffset = static_cast<float>(
        -(NormalZExact * RingPos.z + static_cast<double>(NormalY) * RingPos.y +
          static_cast<double>(NormalX) * RingPos.x));

    // the player's side of that plane (IObjInspect slot 0,
    // get_position); TEST AH,0x41 takes only "greater than 0".
    Vector const PlayerPos = Player->get_position();
    bool const bSwap =
        (PlayerPos.z * NormalZ + PlayerPos.y * NormalY + PlayerPos.x * NormalX) + PlaneOffset >
        0.0f;

    // follow PrevRing (+0x1d8) to the first ring of the lane.
    CSolar const* Head = Ring;
    while (Head->PrevRing != 0)
    {
        CObject* const Next = HudCObjectOf(ClientObjDb_FindObject(Head->PrevRing, 0));
        if (Next == nullptr || (static_cast<unsigned>(Next->ObjectClass) & 0x303u) != 0x303u)
        {
            return false;
        }
        Head = static_cast<CSolar const*>(Next);
    }
    unsigned First = Head->get_tradelane_space_name();

    // and NextRing (+0x1dc) to the last.
    CSolar const* Tail = Ring;
    while (Tail->NextRing != 0)
    {
        CObject* const Next = HudCObjectOf(ClientObjDb_FindObject(Tail->NextRing, 0));
        if (Next == nullptr || (static_cast<unsigned>(Next->ObjectClass) & 0x303u) != 0x303u)
        {
            return false;
        }
        Tail = static_cast<CSolar const*>(Next);
    }
    unsigned Second = Tail->get_tradelane_space_name();
    if (First == 0 || Second == 0)
    {
        return false;
    }
    if (bSwap)
    {
        unsigned const Swap = First;
        First = Second;
        Second = Swap;
    }

    // First "->" Second, each through a 0x100-char buffer (0xff read),
    // refused when the running length (+2 for the arrow) reaches Capacity (signed JGE).
    wchar_t Text[0x100];
    GetIdsStringWWithRawFallback(First, Text, 0xff);
    int const FirstLength = static_cast<int>(wcslen(Text)) + 2;
    if (FirstLength >= Capacity)
    {
        return false;
    }
    wcscpy(Out, Text);
    wcscat(Out, L"->");
    GetIdsStringWWithRawFallback(Second, Text, 0xff);
    if (static_cast<int>(wcslen(Text)) + FirstLength >= Capacity)
    {
        return false;
    }
    wcscat(Out, Text);
    return true;
}

bool HudBuildObjectCaption(CSimple const* Object, FHudWideText* Out, int Mode,
                           bool bFactionShortName)
{
    // (cdecl).
    wchar_t* const Line = G_NnTextScratchB;
    wchar_t* const Scratch = G_NnTextScratchA;

    // the type bits, the ids_name, an empty line, and the watch-list
    // byte (+0xa8) of the object's client gameobj when TypeBits & 0xfdff.
    unsigned const TypeBits = Object->TypeBits;
    unsigned const Name = GetObjectIdsName(Object);
    Line[0] = 0;
    unsigned char bWatched = 0;
    FClientGameobj* const Gameobj = ClientObjDb_FindObject(Object->Id, 0);
    if (Gameobj != nullptr && (TypeBits & 0xfdffu) != 0)
    {
        bWatched = Gameobj->bWatchListed;
    }

    // TypeBits bit 15 or a watched object, with a name: the name line
    // (Mode 0 / 2) and the location tail.
    if (((TypeBits & 0x8000u) != 0 || bWatched != 0) && Name != 0)
    {
        if (Mode == 0 || Mode == 2)
        {
            GetIdsStringWWithRawFallback(Name, Line, G_NnTextScratchCapacity);
        }
        return HudCaptionLocationTail(Object, Out, Mode);
    }

    // anything but a CEqObj.
    if ((static_cast<unsigned>(Object->ObjectClass) & 0x103u) != 0x103u)
    {
        if (Name == 0)
        {
            return false;
        }
        return HudCaptionPlain(Object, Name, TypeBits, Out, Mode);
    }

    // the faction of the object's reputation instance (+0x104).
    CEqObj const* const EqObj = static_cast<CEqObj const*>(Object);
    int const VibeId = static_cast<int>(EqObj->VibeId); // local_118
    unsigned Affiliation = 0;                          // local_114, written by GetAffiliation
    if (Reputation::Vibe::GetAffiliation(VibeId, Affiliation, false) != 0 ||
        Affiliation == 0xffffffffu)
    {

        if (Name == 0)
        {
            return false;
        }
        if ((TypeBits & 0x7f0000u) == 0)
        {
            return HudCaptionPlain(Object, Name, TypeBits, Out, Mode);
        }
        if (Mode != 1 && Mode != 2)
        {
            return false;
        }
        // A player's display name, when the instance has one, is the whole caption. baseline
        // points GetName's out argument at the Name slot (+0x4c); GetName always writes it on
        // success, so the initial value is never read.
        FmtStr Name1;
        FmtStr Name2;
        wchar_t const* DisplayName = nullptr;
        if (Reputation::Vibe::GetName(VibeId, Name1, Name2, DisplayName) == 0 &&
            EqObj->OwnerPlayer != 0 && DisplayName != nullptr)
        {
            HudWideTextAssign(Out, DisplayName, wcslen(DisplayName));
            return true;
        }
        return false;
    }


    unsigned const ShortName = Reputation::get_short_name(Affiliation); // [ESP+0x14]
    if ((TypeBits & 0x7f0000u) != 0)
    {
        // the instance's two FmtStr names (ctors inlined, trylevel 2/3).
        FmtStr Name1;
        FmtStr Name2;
        wchar_t const* DisplayName = nullptr; // [ESP+0x48]; GetName writes it on success
        if (Reputation::Vibe::GetName(VibeId, Name1, Name2, DisplayName) == 0)
        {
            bool const bNoName1 = Name1.MadLibId == 0;
            if (Mode == 0 || Mode == 2)
            {
                if (!bNoName1)
                {
                    FmtStrSprintf(&Name1, Line, G_NnTextScratchCapacity);
                }
                if (Mode == 2 && !bNoName1)
                {
                    wcscat(Line, L"\n");
                }
            }
            if (Mode == 1 || Mode == 2)
            {

                if (EqObj->OwnerPlayer != 0 && DisplayName != nullptr)
                {
                    wcscat(Line, DisplayName);
                }
                else
                {
                    if (bNoName1 && ShortName != 0 && ShortName != 0xffffffffu &&
                        bFactionShortName)
                    {
                        wchar_t Short[0x80];
                        if (GetIdsStringWWithRawFallback(ShortName, Short, 0x80) == 0)
                        {
                            Short[0] = 0;
                        }
                        // JA on length > 1, else a first char other than ' '.
                        if (wcslen(Short) > 1 || Short[0] != L' ')
                        {
                            wcscat(Line, Short);
                            wcscat(Line, L" - ");
                        }
                    }
                    FmtStrSprintf(&Name2, Scratch, G_NnTextScratchCapacity);
                    wcscat(Line, Scratch);
                }
            }
            // the exit copy, then both FmtStr dtors (Name2 first).
            return HudCaptionFinish(Out);
        }
        // both dtors, then on to the base captions.
    }

    // docked at a base (+0x160) with a faction short name: the short
    // name + ids 0x562 as the name line, the ids_name as the location line.
    if (EqObj->DockingBaseId != 0 && ShortName != 0)
    {
        if (Mode == 0 || Mode == 2)
        {
            GetIdsStringWWithRawFallback(ShortName, Line, G_NnTextScratchCapacity);
            GetIdsStringWWithRawFallback(0x562, Scratch, G_NnTextScratchCapacity);
            wcscat(Line, Scratch);
            if (Mode == 2)
            {
                wcscat(Line, L"\n");
            }
        }
        if (Mode == 1 || Mode == 2)
        {
            GetIdsStringWWithRawFallback(Name, Scratch, G_NnTextScratchCapacity);
            wcscat(Line, Scratch);
        }
        return HudCaptionFinish(Out);
    }

    // the object-kind ids, else the ids_name, as the second word.
    unsigned const KindIds = HudObjectKindIds(TypeBits);
    unsigned const SecondIds = KindIds != 0 ? KindIds : Name;
    if (Mode == 0 || Mode == 2)
    {
        // the first word is the faction short name, or for TypeBits
        // 0x20 the ids_name of the solar whose client gameobj owns the base at +0x164 (bucket 2
        // walked through get_base, slot 35; the owner through cobject, slot 84), cached.
        unsigned FirstIds = ShortName;
        if ((TypeBits & 0x20u) != 0)
        {
            unsigned const OwnerBase = EqObj->DockingPointBaseId;
            if (OwnerBase != 0)
            {
                if (OwnerBase == G_CaptionOwnerBaseId)
                {
                    FirstIds = G_CaptionOwnerIdsName;
                }
                else
                {
                    FClientGameobj* Scan = nullptr;
                    for (;;)
                    {
                        int Kind = 2; // [ESP+0x58], re-stored every pass
                        Scan = Scan != nullptr ? Scan->ListNext : ClientObjDb_GetKindHead(&Kind);
                        if (Scan == nullptr)
                        {
                            break;
                        }
                        // baseline uses the short-name slot [ESP+0x14] for this; the short
                        // name is already in EBP and the slot is not read again.
                        unsigned Base = 0;
                        Scan->get_base(Base);
                        if (Base != OwnerBase)
                        {
                            continue;
                        }
                        CObject const* const Owner = Scan->cobject();
                        if (Owner != nullptr &&
                            (static_cast<unsigned>(Owner->ObjectClass) & 3u) == 3u)
                        {
                            FirstIds = GetObjectIdsName(Owner);
                            G_CaptionOwnerBaseId = OwnerBase;
                            G_CaptionOwnerIdsName = FirstIds;
                        }
                        break;
                    }
                }
            }
        }

        GetIdsStringWWithRawFallback(FirstIds, Line, G_NnTextScratchCapacity);
        GetIdsStringWWithRawFallback(SecondIds, Scratch, G_NnTextScratchCapacity);
        wcscat(Line, L" ");
        wcscat(Line, Scratch);
    }

    // a loot-type object answers only in Mode 0 / 2, with its count.
    if (TypeBits == 0x10000000u)
    {
        if (Mode != 0 && Mode != 2)
        {
            return false;
        }
        HudCaptionAppendLootCount(Object);
        return HudCaptionFinish(Out);
    }


    if (Mode == 2)
    {
        wcscat(Line, L"\n");
    }
    else if (Mode != 1)
    {
        return HudCaptionFinish(Out);
    }

    // a trade-lane ring (TypeBits bit 7) with an "A->B" caption.
    if ((TypeBits & 0x80u) != 0 &&
        HudTradelaneCaption(Object, Scratch, G_NnTextScratchCapacity))
    {
        wcscat(Line, Scratch);
        return HudCaptionFinish(Out);
    }
    // TypeBits & 0x8c0 -- the ids_name.
    if ((TypeBits & 0x8c0u) != 0)
    {
        GetIdsStringWWithRawFallback(Name, Scratch, G_NnTextScratchCapacity);
        wcscat(Line, Scratch);
        return HudCaptionFinish(Out);
    }
    // system name and sector.
    HudCaptionAppendLocation(Object);
    return HudCaptionFinish(Out);
}

void HudReputationColor(unsigned long* OutArgb, IObjRW* Player, IObjRW* Other)
{
    // (cdecl).
    if (Other == nullptr || Player == nullptr)
    {
        *OutArgb = G_NnPaletteArgb_679b84;
        return;
    }

    FClientGameobj* const Gameobj = ClientObjDb_FindObject(Other->get_id(), 1);
    if (Gameobj != nullptr)
    {
        // Ship gameobj byte +0x18c bit 2 (0x0045a517; bucket 1 holds ships only). No instruction
        // in OutpostFrontier.exe sets bit 2 (writers: ctor 0x005434a5 zeroes the dword, 0x00543b71
        // sets bit 3, 0x00545f07 bits 0/1, 0x005447c6 / 0x0051db60 / 0x0051db8d clear 0/1).
        if ((static_cast<FClientShipGameobj*>(Gameobj)->Field_0x18c & 4) != 0)
        {
            *OutArgb = G_NnPaletteArgb_679b9c;
            return;
        }
        // IObjInspect slot 47 (+0xbc) is_player, then a ship with a non-zero group id that the
        // player's ship shares (0x0045a530..0x0045a59b).
        if (Gameobj->is_player())
        {
            CObject* const Object = HudCObjectOf(Gameobj);
            if (Object != nullptr && (static_cast<unsigned>(Object->ObjectClass) & 0x503u) == 0x503u &&
                HudShipGroupId(Object) != 0)
            {
                IObjRW* const PlayerRw = static_cast<IObjRW*>(GetPlayerObjRW());
                if (PlayerRw != nullptr)
                {
                    // a player object that is not a ship leaves EAX = 0
                    // and baseline reads [0 + 0x1b4] -- the player in space is always a ship, and
                    // the port keeps that read unguarded, as baseline does.
                    CObject* PlayerShip = HudCObjectOf(static_cast<IObjInspectImpl*>(PlayerRw));
                    if (PlayerShip != nullptr &&
                        (static_cast<unsigned>(PlayerShip->ObjectClass) & 0x503u) != 0x503u)
                    {
                        PlayerShip = nullptr;
                    }
                    if (HudShipGroupId(PlayerShip) == HudShipGroupId(Object))
                    {
                        *OutArgb = G_NnPaletteArgb_679b88;
                        return;
                    }
                }
            }
        }
    }

    // Other's attitude towards the player (vt+0x68); a failed query is white.
    float Attitude = 0.0f;
    if (Other->get_attitude_towards(Attitude, Player) != 0)
    {
        *OutArgb = G_NnPaletteArgb_679b84;
        return;
    }
    // FCOMP -0.6f / TEST AH,0x41 / JP: hostile when at or below -0.6.
    if (Attitude <= -0.6f)
    {
        *OutArgb = G_NnPaletteArgb_679ba8;
        return;
    }
    // FCOMP -0.2f (the field = 0xbe4ccccd) / TEST AH,0x41 / JNP: white when at or below
    // -0.2; then FCOMP 0.6f / TEST AH,1 / JNZ: white below 0.6 (or unordered), green at or
    // above it. Both white exits are kept as baseline spells them.
    if (Attitude <= -0.2f)
    {
        *OutArgb = G_NnPaletteArgb_679b84;
        return;
    }
    if (!(Attitude >= 0.6f))
    {
        *OutArgb = G_NnPaletteArgb_679b84;
        return;
    }
    *OutArgb = G_NnPaletteArgb_679bac;
}

int HudReputationBand(float Attitude)
{
    // . FCOMP the field (-0.6f), TEST AH,0x41 + JP: below or equal returns 0,
    // greater (and unordered) goes on.
    if (Attitude <= -0.6f)
        return 0;
    // the same against the field (-0.2f) -- 1.
    if (Attitude <= -0.2f)
        return 1;
    // FCOMP the field (0.6f), TEST AH,0x01 + JNZ jumps when less (or
    // unordered); not less returns 4.
    if (Attitude >= 0.6f)
        return 4;
    // FCOMP the field (0.2f), TEST AH,0x41, EAX = 3 and JZ keeps it
    // when greater; less, equal or unordered falls to 2.
    if (Attitude > 0.2f)
        return 3;
    return 2;
}

// =============================================================================================
// HudTargetBase
// =============================================================================================

HudTargetBase::HudTargetBase()
    : ViewObject()
    , PartCache()
    , bSubTargetPickEnabled(0)
    , ViewPanel(nullptr)
    , bExpanded(0)
{
    // bases (HUDItem inlined with an empty name, WinControl the routine,
    // ButtonMover the routine, the handler vptr), +0x398 zeroed, the routine (the part cache
    // zeroed), then the GameCamera ctor the routine at +0x3b8.
    GameCameraConstruct(&ViewCamera);
    SubTargetId = SUBOBJ_ID_NONE;
}

HudTargetBase::~HudTargetBase()
{
    // , in baseline's order: the camera's watcher (+0x3b8 + 0xc0) cleared (the
    // camera vptr put back and its empty base dtor run), PartCacheClear the routine on +0x3a0;
    // the member dtors then clear the +0x398 watcher, and the base dtors run Event::Handler
    // , free the mover's list, ~WinControl the routine and HUDItem's watcher.

    // `if (watched != 0) BaseWatcher::set_pointer(0)`. set_pointer is protected in
    // Common (?set_pointer@BaseWatcher@@IAE...), so baseline's inlined test-and-call comes from a
    // header-only BaseWatcher subclass that left no symbol, vtable or RTTI in either PE; its body
    // is exactly Common's public BaseWatcher::set (`if (Target != Watched) set_pointer(Target)`,
    // .text import 0x005c645c), which the port calls.
    ViewCamera.Watched.set(nullptr);
    PartCacheClear(&PartCache);
}

void HudTargetBase::SetViewObject(IObjRW* Target)
{
    // . The client object (the routine, kind 0) of Target's id, kept only when its
    // CObject has class bits 3 (& 3 == 3).
    IObjRW* Found = nullptr;
    if (Target != nullptr)
    {
        FClientGameobj* const Gameobj = ClientObjDb_FindObject(Target->get_id(), 0);
        Found = Gameobj;
        if (Gameobj != nullptr)
        {
            CObject const* const Object = Gameobj->cobject();
            if (Object == nullptr || (static_cast<unsigned>(Object->ObjectClass) & 3u) != 3u)
            {
                Found = nullptr;
            }
        }
    }

    IObjRW* const Current = ObjRwFromWatchable(const_cast<Watchable*>(ViewObject.target()));
    if (Current == Found)
    {
        return;
    }
    // `if (Found+8 != watched) set_pointer(Found+8)` -- BaseWatcher::set's body (see
    // the destructor for why the port calls set).
    Watchable const* const NewWatched = (Found != nullptr) ? static_cast<Watchable*>(Found) : nullptr;
    ViewObject.set(NewWatched);
    Slot16();

    IObjRW* const Viewed = ObjRwFromWatchable(const_cast<Watchable*>(ViewObject.target()));
    if (Viewed == nullptr)
    {

        PartCacheClear(&PartCache);
        return;
    }

    // slot 4 (+0x10) get_orientation, whose baseline body is ICF-folded with slot 3
    // get_transform -- the pointer is the object's whole Transform, and the routine uses it as
    // one. The port asks for the transform by its own name.
    FTransform const* const Xform = reinterpret_cast<FTransform const*>(&Viewed->get_transform());
    void* const Instance = InstanceFromIndex(Viewed->get_index());
    FHudInstanceBounds Bounds(Instance, Xform, false);

    FVector Center;
    ViewCenter = *Bounds.GetCenter(&Center);

    // the routine (Max - Center into a stack vector), the routine on
    // it (the length left on the FPU), FSTP float to +0x388 -- stored, then overwritten below.
    float Reach[3];
    Reach[0] = Bounds.Max.X - ViewCenter.X;
    Reach[1] = Bounds.Max.Y - ViewCenter.Y;
    Reach[2] = Bounds.Max.Z - ViewCenter.Z;
    ViewRadius = static_cast<float>(Vec3Length(Reach));

    // FCOM / TEST AH,0x41 / JZ: the X span is kept only when strictly larger than the Z span,
    // then * 0.5 (the field, double).
    float const SpanX = Bounds.Max.X - Bounds.Min.X;
    float const SpanZ = Bounds.Max.Z - Bounds.Min.Z;
    float const Span = (SpanX > SpanZ) ? SpanX : SpanZ;
    ViewRadius = static_cast<float>(static_cast<double>(Span) * 0.5);

    // the chrome colour the field dimmed to 70 % per byte (byte 0 clamped inline,
    // bytes 1..3 through the routine), packed as byte3 << 24 | byte0 << 16 | byte1 << 8 | byte2.
    unsigned char const* const Chrome = G_HudChromeColorRgba;
    unsigned const Byte0 = static_cast<unsigned>(ClampByte(DimChromeByte(Chrome[0])));
    unsigned const Byte1 = static_cast<unsigned>(ClampByte(DimChromeByte(Chrome[1])));
    unsigned const Byte2 = static_cast<unsigned>(ClampByte(DimChromeByte(Chrome[2])));
    unsigned const Byte3 = static_cast<unsigned>(ClampByte(DimChromeByte(Chrome[3])));
    unsigned const Tag = (Byte3 << 24) | (Byte0 << 16) | (Byte1 << 8) | Byte2;

    // get_index again; the renderer LODs pinned to 3 while the cache is built, then
    // put back (the routine / the routine / the routine), then the parts hidden.
    void* const BuildInstance = InstanceFromIndex(Viewed->get_index());
    FRendererLodMap SavedLods;
    BindRendererLods(BuildInstance, 3, &SavedLods);
    PartCacheBuild(&PartCache, BuildInstance, Tag);
    ApplyRendererDetailQueue(BuildInstance, &SavedLods);
    HideGroupAndEquipParts();
}

void HudTargetBase::Slot12()
{
    // RET.
}

void HudTargetBase::Slot13(bool /*bHide*/)
{
    // RET 4.
}

void HudTargetBase::Slot14()
{
    // RET.
}

void HudTargetBase::Slot15()
{
    // RET.
}

void HudTargetBase::Slot16()
{
    // RET.
}

CObject* HudTargetBase::GetViewCObject() const
{
    return CObjectOfWatcher(ViewObject);
}

void HudTargetBase::HideGroupAndEquipParts()
{
    // only for a CEqObj (class & 0x103 == 0x103).
    CObject* const Object = GetViewCObject();
    if (Object == nullptr)
    {
        return;
    }
    if ((static_cast<unsigned>(Object->ObjectClass) & 0x103u) != 0x103u)
    {
        return;
    }
    CEqObj* const EqObj = static_cast<CEqObj*>(Object);

    // Every collision group root, without recursion; the root index is not tested against -1.
    CArchGrpTraverser GroupWalk;
    CArchGroup* Group = EqObj->ArchGroups.Traverse(GroupWalk);
    while (Group != nullptr)
    {
        PartCacheMarkHidden(&PartCache, InstanceFromIndex(Group->GetRootIndex()), false);
        Group = EqObj->ArchGroups.Traverse(GroupWalk);
    }

    // Every attached equipment (mask 0x1fe0) root other than -1, with recursion.
    CEquipTraverser EquipWalk(0x1fe0);
    CEquip* Equip = EqObj->EquipManager.Traverse(EquipWalk);
    while (Equip != nullptr)
    {
        // vt+0x84 CAttachedEquip::GetRootIndex, called on every traversed item.
        long const Root = static_cast<CAttachedEquip*>(Equip)->GetRootIndex();
        if (Root != -1)
        {
            PartCacheMarkHidden(&PartCache, InstanceFromIndex(Root), true);
        }
        Equip = EqObj->EquipManager.Traverse(EquipWalk);
    }
}

namespace
{
    // the view camera's half fov x (0x420c0000, read-only in the PE).
    float const kViewHalfFovX = 35.0f;

    // The part tag both colouring passes build from a colour's four memory bytes (0x004e6e60,
    // ): byte3 << 24 | byte0 << 16 | byte1 << 8 | byte2.
    unsigned PackPartTag(unsigned long Colour)
    {
        unsigned const Byte0 = static_cast<unsigned>(Colour & 0xffu);
        unsigned const Byte1 = static_cast<unsigned>((Colour >> 8) & 0xffu);
        unsigned const Byte2 = static_cast<unsigned>((Colour >> 16) & 0xffu);
        unsigned const Byte3 = static_cast<unsigned>((Colour >> 24) & 0xffu);
        return (Byte3 << 24) | (Byte0 << 16) | (Byte1 << 8) | Byte2;
    }

    unsigned long ChromeColourArgb()
    {
        unsigned long Colour = 0;
        memcpy(&Colour, G_HudChromeColorRgba, sizeof(G_HudChromeColorRgba));
        return Colour;
    }
}

void HudTargetBase::SetupViewCamera(bool bFixedOrbit, float ScreenShift)
{
    // . The hit box's top-left and bottom-right corners through the Win3D camera.
    FVector Corner0;
    Corner0.X = HitCenter.X - HitHalfWidth;
    Corner0.Y = HitCenter.Y + HitHalfHeight;
    Corner0.Z = HitCenter.Z;
    FVector Corner1;
    Corner1.X = HitCenter.X + HitHalfWidth;
    Corner1.Y = HitCenter.Y - HitHalfHeight;
    Corner1.Z = HitCenter.Z;
    int Box[4];
    int const* const Projected = HudProjectScreenRect(Box, &Corner0, &Corner1);
    int const Left = Projected[0];
    int const Top = Projected[1];
    int const Right = Projected[2];
    int const Bottom = Projected[3];

    // moved ScreenShift Win3D-camera widths sideways (the field / the field,
    // the left and right of that camera's viewport), then inset by an eighth of the box: its
    // width on the left and the right, its height at the top -- and its WIDTH again at the
    // bottom, because 0x004e7228 subtracts the width term still on the FPU stack. The products
    // and sums are exact in double, as they are on the x87 stack.
    int const* const Screen = Win3DGetCamera()->Viewport;
    double const ScreenWidth = static_cast<double>(Screen[2] - Screen[0] + 1);
    double const WidthInset = static_cast<double>(Right - Left) * 0.125;
    double const HeightInset = static_cast<double>(Bottom - Top) * 0.125;
    int const ShiftedLeft =
        static_cast<int>(ScreenWidth * static_cast<double>(ScreenShift) + static_cast<double>(Left));
    int const ShiftedRight =
        static_cast<int>(ScreenWidth * static_cast<double>(ScreenShift) + static_cast<double>(Right));
    int Viewport[4];
    Viewport[0] = static_cast<int>(static_cast<double>(ShiftedLeft) + WidthInset);
    Viewport[1] = static_cast<int>(HeightInset + static_cast<double>(Top));
    Viewport[2] = static_cast<int>(static_cast<double>(ShiftedRight) - WidthInset);
    Viewport[3] = static_cast<int>(static_cast<double>(Bottom) - WidthInset);
    SpaceCameraSetViewportEx(&ViewCamera, Viewport, 1);

    // 1 / tan(35 degrees), computed once (bit 0 of the field): FLD 35.0f,
    // FMUL 0.0174533f (the field), FPTAN, FDIVR 1.0.
    static float const InvTanHalfFov = static_cast<float>(
        1.0 / tan(static_cast<double>(kViewHalfFovX) * static_cast<double>(0.0174533f)));
    SpaceCameraSetHalfFovX(&ViewCamera, kViewHalfFovX);

    IObjRW* const Viewed = ObjRwFromWatchable(const_cast<Watchable*>(ViewObject.target()));
    FTransform Xform;
    if (!bFixedOrbit)
    {
        // ViewRadius * 1.1 (the field) / tan(35) away from the object's centre.
        float const Distance = static_cast<float>(static_cast<double>(InvTanHalfFov) *
                                                  static_cast<double>(ViewRadius) *
                                                  static_cast<double>(1.1f));
        Win3DIdentityRotation3x3(&Xform.Rotation.D[0][0]);
        Xform.Translation.X = 0.0f;
        Xform.Translation.Y = 0.0f;
        Xform.Translation.Z = 0.0f;

        // The HUDItem target (+0x28) when it is a ship, else null -- which baseline does not test:
        // GetDirectionMatrix is handed null + 8 (EngineObject::get_transform inlined).
        CObject* Ship = CObjectOfWatcher(HUDItem::Target);
        if (Ship != nullptr && (static_cast<unsigned>(Ship->ObjectClass) & 0x503u) != 0x503u)
        {
            Ship = nullptr;
        }
        Matrix const Facing = GetDirectionMatrix(Ship->get_transform(), Viewed->get_position());
        memcpy(&Xform.Rotation, &Facing, sizeof(FMatrix));

        // Slot 4 (+0x10), ICF-folded with get_transform -- see SetViewObject.
        FTransform const* const ViewXform =
            reinterpret_cast<FTransform const*>(&Viewed->get_transform());
        FVector Center;
        HudTransformPoint(ViewXform, &Center, &ViewCenter);
        float const Axis[3] = {Xform.Rotation.D[0][2], Xform.Rotation.D[1][2],
                               Xform.Rotation.D[2][2]};
        float Offset[3];
        ScaleVec3(Offset, Axis, Distance);
        // Loot (class & 0x803) stands four times as far (the field).
        CObject* const ViewedObject = GetViewCObject();
        if (ViewedObject != nullptr &&
            (static_cast<unsigned>(ViewedObject->ObjectClass) & 0x803u) == 0x803u)
        {
            Offset[0] = Offset[0] * 4.0f;
            Offset[1] = Offset[1] * 4.0f;
            Offset[2] = Offset[2] * 4.0f;
        }
        // .
        Xform.Translation.X = Center.X + Offset[0];
        Xform.Translation.Y = Center.Y + Offset[1];
        Xform.Translation.Z = Center.Z + Offset[2];
    }
    else
    {
        // the object's own orientation (slot 3, +0xc) with its y / z columns turned
        // by -90 degrees (the double the field = (double)-1.57079637f): per row,
        // m1' = m1 cos + sin m2 and m2' = cos m2 - m1 sin.
        FTransform const* const ObjectXform =
            reinterpret_cast<FTransform const*>(&Viewed->get_transform());
        Xform.Rotation = ObjectXform->Rotation;
        Xform.Translation.X = 0.0f;
        Xform.Translation.Y = 0.0f;
        Xform.Translation.Z = 0.0f;
        double const Angle = static_cast<double>(-1.57079637f);
        double const Cos = cos(Angle);
        double const Sin = sin(Angle);
        for (int Row = 0; Row < 3; ++Row)
        {
            double const M1 = static_cast<double>(Xform.Rotation.D[Row][1]);
            double const M2 = static_cast<double>(Xform.Rotation.D[Row][2]);
            Xform.Rotation.D[Row][1] = static_cast<float>(M1 * Cos + Sin * M2);
            Xform.Rotation.D[Row][2] = static_cast<float>(Cos * M2 - M1 * Sin);
        }

        float const Distance = static_cast<float>(static_cast<double>(InvTanHalfFov) *
                                                  static_cast<double>(ViewRadius) *
                                                  static_cast<double>(1.1f));
        // Slot 4 (+0x10) through the field directly (the routine's body, inlined).
        FTransform const* const ViewXform =
            reinterpret_cast<FTransform const*>(&Viewed->get_transform());
        FVector Center;
        HudTargetMath()->TransformXform(Center, *ViewXform, ViewCenter);
        float const OffsetX = Xform.Rotation.D[0][2] * Distance;
        float const OffsetY = Xform.Rotation.D[1][2] * Distance;
        float const OffsetZ = Xform.Rotation.D[2][2] * Distance;
        Xform.Translation.X = Center.X + OffsetX;
        Xform.Translation.Y = Center.Y + OffsetY;
        Xform.Translation.Z = Center.Z + OffsetZ;
    }

    // the twelve floats into the camera's world orientation and position, the view
    // marked dirty, then the viewport and the perspective of this camera.
    ViewCamera.bViewDirty = 1;
    memcpy(ViewCamera.World, &Xform.Rotation, sizeof(ViewCamera.World));
    memcpy(ViewCamera.WorldPos, &Xform.Translation, sizeof(ViewCamera.WorldPos));
    Win3DSetViewport(AsCamera(&ViewCamera),
                     "Client\\Space\\HUD_Target_Base.cpp", 0x16b);
    Win3DApplyCameraPerspective(AsCamera(&ViewCamera));
    bSubTargetPickEnabled = 1;
}

void HudTargetBase::ColourViewParts()
{
    // .
    PartCacheClearHiddenActive(&PartCache);
    ColourGroupParts();
    ColourEquipParts();
}

void HudTargetBase::ColourGroupParts()
{
    // only for a CEqObj (class & 0x103 == 0x103).
    CObject* const Object = GetViewCObject();
    if (Object == nullptr)
    {
        return;
    }
    if ((static_cast<unsigned>(Object->ObjectClass) & 0x103u) != 0x103u)
    {
        return;
    }
    CEqObj* const EqObj = static_cast<CEqObj*>(Object);

    CArchGrpTraverser GroupWalk;
    CArchGroup* Group = EqObj->ArchGroups.Traverse(GroupWalk);
    while (Group != nullptr)
    {
        // The chrome colour, or the damage colour while 1 - relative health > 0 (TEST AH,0x41).
        unsigned long Colour = ChromeColourArgb();
        float const Damage = 1.0f - Group->GetRelativeHealth();
        if (Damage > 0.0f)
        {
            unsigned long Damaged = 0;
            Colour = *HudDamageColorArgb(&Damaged, Damage);
        }

        // each byte * 0.7f, ftol, clamped to 0..255 inline.
        unsigned long const Dim0 = static_cast<unsigned long>(
            ClampByte(DimChromeByte(static_cast<unsigned char>(Colour))));
        unsigned long const Dim1 = static_cast<unsigned long>(
            ClampByte(DimChromeByte(static_cast<unsigned char>(Colour >> 8))));
        unsigned long const Dim2 = static_cast<unsigned long>(
            ClampByte(DimChromeByte(static_cast<unsigned char>(Colour >> 16))));
        unsigned long const Dim3 = static_cast<unsigned long>(
            ClampByte(DimChromeByte(static_cast<unsigned char>(Colour >> 24))));
        Colour = Dim0 | (Dim1 << 8) | (Dim2 << 16) | (Dim3 << 24);

        // The sub-target pulses (the routine round the dimmed colour) unless the mouse may not
        // pick (the routine).
        if (SubTargetId == Group->GetID() && !HudIsMousePickBlocked())
        {
            unsigned long Pulsed = 0;
            NnUiPulseBlendFromArgb(&Pulsed, Colour);
            Colour = Pulsed;
        }

        // Re-activated without recursion; the root index is not tested against -1.
        PartCacheActivatePart(&PartCache, InstanceFromIndex(Group->GetRootIndex()), false,
                              PackPartTag(Colour));
        Group = EqObj->ArchGroups.Traverse(GroupWalk);
    }
}

void HudTargetBase::ColourEquipParts()
{
    // only for a CEqObj (class & 0x103 == 0x103).
    CObject* const Object = GetViewCObject();
    if (Object == nullptr)
    {
        return;
    }
    if ((static_cast<unsigned>(Object->ObjectClass) & 0x103u) != 0x103u)
    {
        return;
    }
    CEqObj* const EqObj = static_cast<CEqObj*>(Object);

    // the routine on the object's id; its result is not used.
    IsPlayerShipId(EqObj->Id);

    CEquipTraverser EquipWalk(0x1fe0);
    CEquip* Equip = EqObj->EquipManager.Traverse(EquipWalk);
    while (Equip != nullptr)
    {
        unsigned long Colour = G_NnPaletteArgb_679b98;
        float const Damage = 1.0f - Equip->GetRelativeHealth();
        if (Damage > 0.0f)
        {
            unsigned long Damaged = 0;
            Colour = *HudDamageColorArgb(&Damaged, Damage);
        }

        // A launcher that is a gun without auto_turret (Gun +0xba): amber while active, else the
        // chrome colour -- in place of the damage colour.
        if (CELauncher::cast(Equip) != nullptr)
        {
            CEGun* const Gun = CEGun::cast(Equip);
            if (Gun != nullptr)
            {
                Archetype::Gun const* const Arch = Gun->GunArch();
                if (Arch != nullptr && !Arch->bAutoTurret)
                {
                    Colour = Equip->IsActive() ? G_NnPaletteArgb_679ba4 : ChromeColourArgb();
                }
            }
        }

        if (SubTargetId == Equip->Id && !HudIsMousePickBlocked())
        {
            unsigned long Pulsed = 0;
            NnUiPulseBlendFromArgb(&Pulsed, Colour);
            Colour = Pulsed;
        }

        // vt+0x84 CAttachedEquip::GetRootIndex; re-activated with recursion.
        long const Root = static_cast<CAttachedEquip*>(Equip)->GetRootIndex();
        if (Root != -1)
        {
            PartCacheActivatePart(&PartCache, InstanceFromIndex(Root), true, PackPartTag(Colour));
        }
        Equip = EqObj->EquipManager.Traverse(EquipWalk);
    }
}

void HudTargetBase::UpdateSubTargetToolTip()
{
    // .
    if (HudIsMousePickBlocked())
    {
        SubTargetId = SUBOBJ_ID_NONE;
        return;
    }

    int CursorX = 0;
    int CursorY = 0;
    int CursorZ = 0;
    GetClientSoftwareCursor(&CursorX, &CursorY, &CursorZ); // / the field
    unsigned short PickedSub = 0;
    int PickedCount = 0;
    unsigned const Ids = PickSubTarget(CursorX, CursorY, &PickedSub, &PickedCount);
    if (Ids == 0)
    {
        SubTargetId = SUBOBJ_ID_NONE;
        return;
    }
    SubTargetId = PickedSub;

    if (PickedCount > 1)
    {
        // "(%d) %s" with the part's name (the routine, 0x40 chars; empty when it fails).
        wchar_t Name[0x40];
        if (GetIdsStringWWithRawFallback(Ids, Name, 0x40) == 0)
        {
            Name[0] = 0;
        }
        wchar_t Text[0x80];
        swprintf(Text, 0x80, L"(%d) %s", PickedCount, Name);

        // The hover record: key = SubTargetId, no tooltip id, caption = Text (the routine
        // grows the buffer to len + 1, wcsncpy, terminator). baseline leaves +0x48..+0x5f as
        // stack garbage; the port zeroes them (same as WinControl::ShowToolTip). The caption
        // zeroing and the routine are the record's constructor.
        FToolTipHoverInfo Info;
        Info.IdsAlt = 0;
        Info.Ids = -1;
        Info.AuxField = 0;
        // clear the (still null) caption.
        if (Info.CaptionW != nullptr && Info.CaptionW[0] != 0)
        {
            Info.CaptionW[0] = 0;
        }
        Info.bLive = 0;
        Info.Ids = static_cast<int>(SubTargetId);

        size_t const Length = wcslen(Text);
        unsigned const Need = static_cast<unsigned>(Length + 1);
        if (Need > Info.CaptionCapacity)
        {

            if (Info.CaptionW != nullptr)
            {
                Info.CaptionW = static_cast<wchar_t*>(realloc(Info.CaptionW, Need * sizeof(wchar_t)));
            }
            else
            {
                Info.CaptionW = static_cast<wchar_t*>(malloc(Need * sizeof(wchar_t)));
                Info.CaptionW[0] = 0;
            }
            Info.CaptionCapacity = Need;
        }
        wcsncpy(Info.CaptionW, Text, Length);
        Info.CaptionW[Length] = 0;
        NnToolTipFeedHover(&Info);

        // Teardown (~FToolTipHoverInfo): the routine, the children (the routine /
        // ), the caption.
        return;
    }

    // One part: key = SubTargetId, tooltip id = the part's ids_name, no caption
    // (the routine(0, -1) on the null caption does nothing).
    FToolTipHoverInfo Info;
    Info.IdsAlt = 0;
    Info.Ids = -1;
    Info.AuxField = 0;
    Info.bLive = 0;
    Info.Ids = static_cast<int>(SubTargetId);
    Info.IdsAlt = static_cast<int>(Ids);
    NnToolTipFeedHover(&Info);
    // Teardown: ~FToolTipHoverInfo.
}

unsigned HudTargetBase::PickSubTarget(int X, int Y, unsigned short* OutSubId, int* OutCount)
{
    // .
    *OutSubId = 0;
    *OutCount = 0;
    if (bSubTargetPickEnabled == 0)
    {
        return 0;
    }
    CObject* const Object = GetViewCObject();
    if (Object == nullptr)
    {
        return 0;
    }
    if ((static_cast<unsigned>(Object->ObjectClass) & 0x103u) != 0x103u)
    {
        return 0;
    }
    CEqObj* const EqObj = static_cast<CEqObj*>(Object);

    // The view camera's unprojection of (X, Y) (slot 0x38), offset by its position (slot 0x0c).
    float const ScreenX = static_cast<float>(X);
    float const ScreenY = static_cast<float>(Y);
    ICamera* const Camera = AsCamera(&ViewCamera);
    FVector Unprojected;
    Camera->Unproject(&Unprojected.X, ScreenX, ScreenY);
    float const* const CameraPos = Camera->GetPosition();
    FVector RayEnd;
    RayEnd.X = Unprojected.X + CameraPos[0];
    RayEnd.Y = Unprojected.Y + CameraPos[1];
    RayEnd.Z = Unprojected.Z + CameraPos[2];

    // The same screen point on the Win3D camera's near plane (the field: +0x68 / +0x6c
    // centre, +0x78 / +0x7c scales, +0xa4 near), rotated by its orientation (+0x04) -- the
    // direction the WinControl face's HitTest (slot 4) takes.
    FSpaceCamera const* const UiCamera = Win3DGetCamera();
    FVector Near;
    Near.X = static_cast<float>((static_cast<double>(ScreenX) - UiCamera->ScreenCenterX) *
                                (static_cast<double>(UiCamera->ZNear) / UiCamera->XScale));
    Near.Y = static_cast<float>((static_cast<double>(ScreenY) - UiCamera->ScreenCenterY) *
                                (static_cast<double>(UiCamera->ZNear) / UiCamera->YScale));
    Near.Z = -UiCamera->ZNear;
    FMatrix Orientation;
    memcpy(&Orientation, UiCamera->World, sizeof(Orientation));
    FVector Direction;
    HudTargetMath()->TransformMatrix(Direction, Orientation, Near);
    if (!HitTest(&Direction.X))
    {
        return 0;
    }

    FHudSpherePick Pick(1);
    Pick.Setup(reinterpret_cast<FVector const*>(Camera->GetPosition()), &RayEnd);

    unsigned Result = 0;

    // Attached equipment (mask 0x1ee0): IsConnected (vt+0x58), then the root (vt+0x84).
    CEquipTraverser EquipWalk(0x1ee0);
    CEquip* Equip = EqObj->EquipManager.Traverse(EquipWalk);
    while (Equip != nullptr)
    {
        CAttachedEquip* const Attached = static_cast<CAttachedEquip*>(Equip);
        if (Attached->IsConnected() &&
            Pick.Test(InstanceFromIndex(Attached->GetRootIndex()), kSubTargetPickSlack))
        {
            *OutSubId = Equip->Id;
            Result = static_cast<unsigned>(Equip->Arch->IdsName);
            CELauncher* const Launcher = CELauncher::cast(Equip);
            *OutCount = (Launcher != nullptr) ? Launcher->GetAmmoCount() : 1;
        }
        Equip = EqObj->EquipManager.Traverse(EquipWalk);
    }

    // Collision groups: root != -1 (GetRootIndex asked twice), then the group's type name.
    CArchGrpTraverser GroupWalk;
    CArchGroup* Group = EqObj->ArchGroups.Traverse(GroupWalk);
    while (Group != nullptr)
    {
        if (Group->GetRootIndex() != -1 &&
            Pick.Test(InstanceFromIndex(Group->GetRootIndex()), kSubTargetPickSlack))
        {
            *OutSubId = Group->GetID();
            Result = HudCollisionGroupTypeIds(Group->GetCollisionGroupType());
            *OutCount = 1;
        }
        Group = EqObj->ArchGroups.Traverse(GroupWalk);
    }
    return Result;
}

bool HudTargetBase::HitTest(float const* Ray)
{
    // .
    if (GetClientMouseCursorDrawFlag() == 0)
    {
        return false;
    }
    if (GetPlayerObjRW() == nullptr)
    {
        return false;
    }
    if (IsLetterboxVisible())
    {
        return false;
    }

    if (bExpanded != 0)
    {
        // The ray scaled to the hit plane's depth, tested against the box around HitCenter.
        float const Scale = HitCenter.Z / Ray[2];
        float const PlaneX = Scale * Ray[0];
        float const PlaneY = Scale * Ray[1];
        float const Dx = PlaneX - (HitCenter.X - HitHalfWidth);
        float const Dy = (HitCenter.Y + HitHalfHeight) - PlaneY;
        // = 0.0f. Each test falls through to the base on false or unordered.
        if (Dx > 0.0f && Dx < HitHalfWidth + HitHalfWidth && Dy > 0.0f &&
            HitHalfHeight + HitHalfHeight > Dy)
        {
            return true;
        }
    }

    return WinControl::HitTest(Ray);
}

void HudTargetBase::Update()
{
    // Win3D::Update the routine, the slide the routine, then (expanded) the
    // sub-target tool tip (tail jump to the routine).
    Win3D::Update();
    ButtonMover::Update();
    if (bExpanded != 0)
    {
        UpdateSubTargetToolTip();
    }
}

void HudTargetBase::Draw()
{
    // .
    if (GetClientMouseCursorDrawFlag() == 0)
    {
        return;
    }
    if (GetPlayerObjRW() == nullptr)
    {
        return;
    }
    if (IsLetterboxVisible())
    {
        return;
    }

    if (bExpanded != 0 && !ButtonMover::IsActive())
    {
        // Visible children (flag bit 1): AsWinControl (slot 2), Draw (slot 13); right after the
        // view panel, HUDItem slot 14.
        for (Win3D* Child = ChildHead; Child != nullptr; Child = Child->NextSibling)
        {
            if ((Child->Flags & 2) == 0)
            {
                continue;
            }
            WinControl* const AsControl = Child->AsWinControl();
            Child->Draw();
            if (AsControl == ViewPanel)
            {
                Slot14();
            }
        }
    }
    else
    {
        for (Win3D* Child = ChildHead; Child != nullptr; Child = Child->NextSibling)
        {
            if ((Child->Flags & 2) == 0)
            {
                continue;
            }
            Child->AsWinControl();
            Child->Draw();
        }
    }
    // Tail jump through the WinControl face's slot 39 (+0x9c).
    Slot39();
}

intptr_t HudTargetBase::SetProperty(int Msg, uintptr_t A, uintptr_t /*B*/)
{
    // message 0x26 from the toggle button -> HUDItem slot 12; always 0, the base
    // is not called.
    if (Msg == 0x26 && A == reinterpret_cast<uintptr_t>(ToggleButton))
    {
        Slot12();
    }
    return 0;
}

void HudTargetBase::OnMoveFinished()
{
    // still collapsed (the opening slide just ended) -> HUDItem slot 13(0), i.e.
    // un-hide the panel controls; then the flag re-read and inverted.
    if (bExpanded == 0)
    {
        Slot13(false);
    }
    bExpanded = (bExpanded == 0) ? 1 : 0;
}

CEquip* HudTargetBase::FindTargetEquip(unsigned short SubId)
{
    // on the HUDItem target (+0x28), not the view object.
    if (SubId == SUBOBJ_ID_NONE)
    {
        return nullptr;
    }
    CObject* const Object = CObjectOfWatcher(HUDItem::Target);
    if (Object == nullptr)
    {
        return nullptr;
    }
    if ((static_cast<unsigned>(Object->ObjectClass) & 0x503u) != 0x503u)
    {
        return nullptr;
    }
    return static_cast<CEqObj*>(Object)->EquipManager.FindByID(SubId);
}
