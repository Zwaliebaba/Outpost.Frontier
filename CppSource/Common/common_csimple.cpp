// Author: Giperion.

// CSimple (baseline CSimple.cpp) and CEquipmentObj.
#include "common_cobject.h"
#include "common_cobject_internal.h" // CmnCobject::CopyMatrix9
#include "x86math.h"

#include "common_cobjdb.h"
#include "common_physys.h"
#include "common_net.h"     // IsMPServer (CSimple::update_zones)
#include "common_server.h"  // CRemotePhysicsController (CSimple::update_zones)
#include "common_universe.h"

#include "NeuronRuntime.h"

using CmnCobject::CopyMatrix9;

namespace
{
    const char kCSimpleSource[] = "Common\\CSimple.cpp";
    const float kHitPtsUseMax = -1.0f;
    const float kHitPtsNegativeFallback = 0.5f;
}

CEquipmentObj::CEquipmentObj(Class ObjectClass)
    : CObject(ObjectClass)
{
    // CObject::CObject(this, ObjectClass), then the CEquipmentObj vptr.
}

CEquipmentObj::CEquipmentObj(CEquipmentObj const& Other)
    : CObject(Other)
{
    // CObject(CObject const&) inlined (06292d60-06292f09), then the
    // CEquipmentObj vptr (06292f0f). CEquipmentObj adds no fields.
}

CEquipmentObj::~CEquipmentObj()
{
}

CSimple::CSimple(Class ObjectClass)
    : CObject(ObjectClass)
{
    // Common (listing 0x55850). The empty VC6 set header baseline builds at
    // this+0xbc..+0xcc (head/nil sentinels, _Multi, _Size) is AttachedDamageObjs, which
    // default-constructs empty on its own -- see its declaration in common_cobject.h.
    // SelfWatchable (+0x9c) default-constructs to Head=nullptr on its own -- no explicit init needed.
    FlagsA0 = 1;
    ZoneAtA4 = nullptr;
    ScannerInterference = 0.0f;
    ObjectDb = nullptr;
    Id = 0;
    OwnerPlayer = 0;
    HitPts = 0.0f;
    CachedCenterOfMass = Vector{0.0f, 0.0f, 0.0f};
    // Common: this+0xdc (CachedRadius, see cache_physical_props) is explicitly
    // initialized to -1.0f (0xbf800000), not 0 -- zero-initializing it would leave 0.0f, which
    // is wrong (0.0f is a valid computed radius; -1.0f is baseline's "not yet cached" sentinel).
    CachedRadius = -1.0f;
    TypeBits = 0;
}

// Common (0x211b0), tracker id 10557. See common_cobject.h for the field
// layout archaeology; this delegates the CObject-owned portion to CObject::CObject(CObject
// const&) and copies CSimple's own fields below, in the same order the analysis copies them.
CSimple::CSimple(CSimple const& Other)
    : CObject(Other)
    // Common: baseline XORs the destination's (pre-zeroed) byte against the
    // source, masks to bit 0, then XORs back in -- net effect is "copy only bit 0 (the only
    // documented meaning, 'targetable'), leave the rest zero". Reproduced directly rather than
    // replicating the XOR/AND/XOR trick, which depends on the destination already being
    // zeroed (true of baseline's allocator, not guaranteed of this port's `operator new`).
    , FlagsA0((unsigned char)(Other.FlagsA0 & 0x1))
    , ZoneAtA4(Other.ZoneAtA4)
    , ScannerInterference(Other.ScannerInterference)
    , ObjectDb(Other.ObjectDb)
    , Id(Other.Id)
    , OwnerPlayer(Other.OwnerPlayer)
    , HitPts(Other.HitPts)
    , AttachedDamageObjs(Other.AttachedDamageObjs)
    , CachedCenterOfMass(Other.CachedCenterOfMass)
    , CachedRadius(Other.CachedRadius)
    , TypeBits(Other.TypeBits)
{
    // SelfWatchable (Watchable, sizeof 4): baseline copies only the raw Head pointer (a single
    // dword at +0x9c), i.e. a shallow copy that shares the source's watcher chain rather than
    // cloning it -- matches Watchable's own shape (just an intrusive list head).
    SelfWatchable.Head = Other.SelfWatchable.Head;

    // AttachedDamageObjs (+0xbc) is deep-copied in the init list above: baseline copies the
    // set node by node (items 673/2230/2108), which is std::set's own copy ctor.
}

CSimple::~CSimple()
{
    // Common (listing 0x55980). baseline also tears down the set at this+0xbc
    // (the routine/the routine/the routine/the routine); here that is AttachedDamageObjs'
    // own destructor, which runs after this body.
    PhySys::SetCollisionState(this, false);
    // SelfWatchable's own destructor (unlinks every watching BaseWatcher, common_watchable.cpp)
    // runs automatically as a member destructor after this body -- no explicit call needed.
}

void CSimple::connect(IObjDB* ObjDb)
{
    // Listing 0x55f80: store only. IObjDB is exe/server inspect DB, not Vibe::Alloc.
    ObjectDb = ObjDb;
}

void CSimple::disconnect(IObjDB* ObjDb)
{
    if (ObjectDb == ObjDb)
        ObjectDb = nullptr;
}

void CSimple::open(Archetype::Root* Arch)
{
    // , the whole body. CSimple::open does NOT create the engine
    // instance: every baseline creator calls Archetype::Root::create_instance itself (Common
    // CPhysAttachment::Connect 06293dc2, CArchGroup::AttachDamageObjs 062a0cbf,
    // CAsteroidCube::init 062a64f2; Server GameObject/Loot/Mine/Guided/Beam/CounterMeasure/
    // Solar create; OutpostFrontier.exe the routine / the branch).
    CObject::open(Arch);
    // TypeBits (+0xe0) seeded from Archetype (+0x88) +0x10 when
    // still zero; no null test on the archetype.
    if (TypeBits == 0)
        TypeBits = Archetype->ObjectType;
}

void CSimple::cache_physical_props()
{
    // Common 0x55ed0. CenterOfMass/Radius are the named CachedCenterOfMass (+0xd0) /
    // CachedRadius (+0xdc) members (see common_cobject.h) rather than raw pointer arithmetic.
    // x. With a body (+0x54): CachedRadius (+0xdc) =
    // float(PhySys::GetRadiusR(this, false, CachedCenterOfMass +0xd0)). Without one:
    // NeuronRuntime::Engine (no null test) +0x88 GetInstanceExtent(instance +0x4, 0, &CachedRadius,
    // &CachedCenterOfMass), then the cached 3DMathEngine TransformXform (+0x28) of that
    // centre by this +0x08 (Orientation + WorldPosition, one Transform) into a temporary that
    // is copied back.
    if (BodyPtr != nullptr)
    {
        CachedRadius = PhySys::GetRadiusR(this, false, CachedCenterOfMass);
        return;
    }

    NeuronRuntime::Engine->GetInstanceExtent((void*)EngineInstanceId, 0, &CachedRadius,
                                     (float*)&CachedCenterOfMass);
    Vector World;
    // this+0x08: Orientation followed by WorldPosition, one Transform.
    Acquire3DMathEngine()->TransformXform((FVector&)World, *(FTransform const*)&Orientation,
                                          (FVector const&)CachedCenterOfMass);
    CachedCenterOfMass = World;
}

float CSimple::get_max_hit_pts() const
{
    // FLD float [Archetype(+0x88)+0x1c]; no null test.
    return Archetype->HitPts;
}

float CSimple::get_hit_pts() const
{
    // Common 0x55fc0. Returns current hit points.
    return HitPts;
}

void CSimple::set_hit_pts(float NewHitPts)
{
    // Common 0x55fd0. Stores hit points verbatim (no clamping in baseline).
    HitPts = NewHitPts;
}

double CSimple::get_relative_health() const
{
    // FLD HitPts (+0xb8); FDIV archetype (+0x88, no null test)
    // HitPts (+0x1c); returned unrounded in ST0. No zero test (baseline divides by 0 when the
    // archetype has no hit points).
    return (double)HitPts / Archetype->HitPts;
}

void CSimple::unmake_physical()
{
    // `JMP CObject::unmake_physical` -- the virtual disable_controllers
    // (vt+0x5c; CShip / CEqObj override it and unregister every equip, steering, nudge and
    // strafe controller, clearing their bControllerEnabled bytes) and then UnMakePhysicalR.
    // Skipping the virtual left those bytes set while the core died, so the following
    // remake_physical -> enable_controllers found ControlObject already "enabled" and never
    // registered them on the new core (ship frozen after a system switch).
    CObject::unmake_physical();
}

float CSimple::get_scanner_interference() const
{
    // Common (listing 0x55e00). Re-queries the current zone (ZoneAtA4, set by
    // update_zones) through the ZONE OBJECT's own vtable slot +0x60 (not this class's +0x60,
    // which is cache_physical_props) -- a modifier in the range (0, +inf); if positive, scales
    // the cached ScannerInterference (set by update_zones), else falls back to 0. Clamped to
    // a max of 1.0.
    // ZoneAtA4 is the Universe::IZone update_zones found (ISystem
    // slot 11); its slot +0x60 (ZONE 0633ad90) is the zone's interference.
    float Result = 0.0f;
    if (ZoneAtA4 != nullptr)
    {
        float Modifier = ZoneAtA4->GetInterference();
        if (Modifier > 0.0f)
            Result = Modifier * ScannerInterference;
    }
    // FCOMP double 1.0 / TEST AH,0x5 / JP -> 1.0 when Result >= 1.0
    // AND when unordered (C0=C2=C3=1 has even parity), so a NaN also comes back as 1.0.
    if (!(Result < 1.0f))
        return 1.0f;
    return Result;
}

unsigned int const& CSimple::get_id() const
{
    return Id;
}

unsigned int CSimple::get_type() const
{
    return TypeBits;
}

IObjDB* CSimple::get_object_database() const
{
    return ObjectDb;
}

unsigned int CSimple::GetOwnerPlayer() const
{
    return OwnerPlayer;
}

void CSimple::SetOwnerPlayer(unsigned int PlayerId)
{
    // Common 0x78920
    OwnerPlayer = PlayerId;
}

unsigned CSimple::get_name() const
{
    // (shared with CShip via ICF). First introduced here, not on CObject (see
    // common_cobject.h: CObject's own named vtable has no get_name slot). MOV EAX,[this+0x88];
    // MOV EAX,[EAX+0x14] -- Archetype->IdsName, no null test.
    return (unsigned)Archetype->IdsName;
}

bool CSimple::is_targetable() const
{
    // MOV AL,[this+0xa0] / AND AL,1 / RET.
    return (FlagsA0 & 1) != 0;
}

void CSimple::init(CreateParms const& Parms)
{
    // (RET 4), traced instruction by instruction. No null tests:
    // baseline reads Archetype (+0x88) and NeuronRuntime::Engine unconditionally.
    // Id (+0xb0), connect(ObjDb) through vt+0x90, OwnerPlayer (+0xb4).
    Id = Parms.Id;
    connect(Parms.ObjDb);
    OwnerPlayer = Parms.OwnerPlayer;

    // CMP [Parms+0xc],0xbf800000 -- -1.0f means "archetype max".
    if (Parms.HitPts == kHitPtsUseMax)
    {
        // HitPts (+0xb8) = Archetype+0x1c.
        HitPts = Archetype->HitPts;
    }
    // FCOMP 0.0f, TEST AH,5 / JP -- anything not below zero
    // (a NaN included) goes on to the max check.
    else if (!(Parms.HitPts < 0.0f))
    {
        // FLD Archetype+0x1c, FCOMP Parms.HitPts, TEST AH,5 / JP --
        // the requested value is kept unless the max is strictly below it.
        float const ArchMax = Archetype->HitPts;
        if (!(ArchMax < Parms.HitPts))
        {
            HitPts = Parms.HitPts;
        }
        else
        {
            // FDUMP(0x100001, fmt, file, 0x45, arch nickname
            // (CObject::get_archetype, folded with CAsteroid::asteroidarch, then Root+0x08
            // ID_String::get_string), Id, (double)init, (double)max); HitPts = max.
            FDUMP(0x100001,
                  "%s(%d) : *** ERROR: ARCH(%s) ID(0x%X) CSimple::init(): "
                  "init_hit_pts (%f) greater than max (%f)",
                  kCSimpleSource, 0x45, Archetype->Nickname.get_string(), Id,
                  (double)Parms.HitPts, (double)ArchMax);
            HitPts = ArchMax;
        }
    }
    else
    {
        // FDUMP(0x100001, fmt, file, 0x3b, arch nickname, Id,
        // (double)init); HitPts = 0.5f (0x3f000000).
        FDUMP(0x100001,
              "%s(%d) : *** ERROR: ARCH(%s) ID(0x%X) CSimple::init(): "
              "Negative init_hit_pts (%f)",
              kCSimpleSource, 0x3b, Archetype->Nickname.get_string(), Id,
              (double)Parms.HitPts);
        HitPts = kHitPtsNegativeFallback;
    }

    // Parms+0x10 -> +0x2c (3 dwords), Parms+0x1c -> +0x08
    // (REP MOVSD, 9 dwords).
    WorldPosition = Parms.Pos;
    CopyMatrix9(Orientation, Parms.Orient);

    // NeuronRuntime::Engine vt+0x6c, IEngine::UpdateInstance(this+4, 0, 0).
    // The engine instance was made by whoever created this object (see CSimple::open).
    NeuronRuntime::Engine->UpdateInstance((void*)EngineInstanceId, 0, nullptr);

    // vt+0x9c init_physics(Parms+0x40, Parms+0x4c).
    init_physics(Parms.Velocity, Parms.AngularVelocity);
}

void CSimple::update_zones(float Dt, unsigned int Flags)
{
    // Listing module storage .. module storage (RET 8). Neither argument is read.
    (void)Dt;
    (void)Flags;

    // -> the branch: no object database clears both the
    // interference (+0xa8) and the zone (+0xa4).
    if (ObjectDb == nullptr)
    {
        ScannerInterference = 0.0f;
        ZoneAtA4 = nullptr;
        return;
    }

    // the sample point. A multiplayer server samples a player-owned
    // object (+0xb4 != 0) at the latest position its client reported, read off the
    // CRemotePhysicsController hanging at +0x60 (__RTDynamicCast IVP_Controller ->
    // CRemotePhysicsController, module storage). Every other case, a failed cast included
    // (module storage -> the branch), uses the local position at +0x2c.
    Vector Pos;
    if (IsMPServer() && OwnerPlayer != 0)
    {
        CRemotePhysicsController* const Remote =
            dynamic_cast<CRemotePhysicsController*>(GolemController);
        if (Remote != nullptr)
        {
            // -> the branch: no client sample yet clears only the
            // zone; the interference at +0xa8 keeps its previous value.
            if (Remote->GetSampleCount() == 0)
            {
                ZoneAtA4 = nullptr;
                return;
            }
            // the three dwords at controller+0xe0 (the
            // CRemotePhysicsSimulation base's GetLatestUpdate, inlined).
            Pos = Remote->GetLatestUpdate();
        }
        else
        {
            Pos = WorldPosition;
        }
    }
    else
    {
        Pos = WorldPosition;
    }

    // IObjDB vt+0x08 (the system's Universe::ISystem, used without
    // a null check) -> ISystem slot 11 (vt+0x2c, SYSTEM 0633b4b0) with &Pos; the result is
    // stored at +0xa4 unconditionally.
    Universe::ISystem const* const ZoneDb = ObjectDb->GetZoneDb();
    Universe::IZone const* const Zone = ZoneDb->GetSmallestPhysicalZoneAt(&Pos);
    ZoneAtA4 = const_cast<Universe::IZone*>(Zone);
    if (Zone == nullptr)
    {
        ScannerInterference = 0.0f;
        return;
    }
    // zone slot 0 with &Pos, FSTP float into +0xa8.
    ScannerInterference = Zone->GetImmersion(Pos);
}

float CSimple::get_physical_radius_r(Vector& OutCenter) const
{
    // Common (listing 0x55e60). Real bug fix: this was a stub always returning a
    // zero radius/center. baseline lazily (re)computes the cached center-of-mass/radius via
    // cache_physical_props() -- called through vtable+0x84, which this port's own vtable dump
    // (Common, addresses 0639b900/0639b908) resolves to the very same
    // code as CSimple::cache_physical_props (see CSimple::beam_object below) -- whenever
    // Radius (CachedRadius, +0xdc) is still the ctor's -1.0f "uncached" sentinel,
    // then returns the cached CachedCenterOfMass (+0xd0) / CachedRadius (+0xdc) verbatim.
    if (CachedRadius == -1.0f)
        const_cast<CSimple*>(this)->cache_physical_props();
    OutCenter = CachedCenterOfMass;
    return CachedRadius;
}

void CSimple::beam_object(Vector const& Pos, Matrix const& Orient, bool bAttach)
{
    // Common (listing 0x55ea0). CObject::beam_object then vtable+0x84, which
    // this port's own vtable dump (Common, address 0639b908) resolves to the
    // very same code address as CSimple::cache_physical_props (0x062b5ed0, confirmed by
    // reading that address directly) -- refresh the cached center-of-mass/radius after the
    // object is teleported/attached.
    CObject::beam_object(Pos, Orient, bAttach);
    cache_physical_props();
}
