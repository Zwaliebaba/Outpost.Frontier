// Author: Giperion.

#include "common_casteroid.h"
#include "common_ini.h"
#include "common_id.h"
#include "common_archetype.h"
#include "neuroncomponents.h"
#include "common_iface.h" // IObjInspect
#include "common_cobjdb.h"   // IObjDB::GetNext / GetZoneDb
#include "common_universe.h" // Universe::ISystem (CmnAsteroid::Update)
#include "common_csys.h"     // Acquire3DMathEngine (CAsteroidCube::init)
#include "x86math.h"         // I3DMathEngine::TransformMatrix / MulMatrix
#include "x87_trig.h"        // baseline's inline FSIN / FCOS (compute_cube_orientation)

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <tuple>
#include <utility>

namespace CmnAsteroid
{

// ============================================================================
// Free functions
// ============================================================================

int __cdecl closest_cube_coord(int Coord, int CubeSize, int HalfSize)
{
    // Common 0x42ff0.
    if (Coord > 0)
        return (Coord / CubeSize) * CubeSize + HalfSize;
    return (Coord / CubeSize) * CubeSize - HalfSize;
}

namespace
{
    // The per-axis "nudge then ftol" idiom of compute_cube_id, compute_asteroid_id and
    // CAsteroidField::create_cube_id / closest_cube_pos: FCOMP 0.0f; TEST AH,0x41; JNZ ->
    // FSUB 0.1f (the field = 0x3dcccccd) for less, equal and unordered, FADD 0.1f for
    // greater. The sum stays on the x87 stack and goes straight to ftol, which truncates to
    // 64 bits and keeps the low 32.
    inline int NudgeTruncate(float V)
    {
        double const Nudged = (V > 0.0f) ? (double)V + (double)0.1f : (double)V - (double)0.1f;
        return (int)(long long)Nudged;
    }
}

unsigned long __cdecl compute_cube_id(Vector const& Pos)
{
    // each axis nudged and truncated (062a6017-062a608c), then
    // abs >> 7 per axis (CDQ/XOR/SUB, SAR 7), range checks X <= 0x3ff, Y <= 0x1ff,
    // Z <= 0x3ff (unsigned JA -> 0), sign bits from the truncated values.
    long X = NudgeTruncate(Pos.x);
    long Y = NudgeTruncate(Pos.y);
    long Z = NudgeTruncate(Pos.z);

    unsigned GridX = (unsigned)std::abs(X) >> 7;
    unsigned GridY = (unsigned)std::abs(Y) >> 7;
    unsigned GridZ = (unsigned)std::abs(Z) >> 7;

    if (GridX >= 0x400 || GridY >= 0x200 || GridZ >= 0x400)
        return 0;

    if (X < 0)
        GridX |= 0x400;
    if (Z < 0)
        GridZ |= 0x400;
    if (Y < 0)
        GridY |= 0x200;

    return (GridZ << 10) | GridY | (GridX << 0x15);
}

namespace
{
    // Common.dll's inlined CRT-rand()-style LCG (see the rand() precedent already used
    // across this codebase, e.g. common_aisteer.cpp/common_misc.cpp). compute_cube_orientation
    // reseeds this straight from a caller-supplied Seed (baseline: the global holdrand at
    // , reseeded in place every call) rather than perturbing the process-wide
    // rand() stream, so it's replicated here against a local seed instead of calling rand().
    inline unsigned NextLcgRaw(unsigned& Seed)
    {
        Seed = Seed * 214013u + 2531011u;
        return (Seed >> 16) & 0x7fff;
    }

    // .data 063eb7f0 / 063eb7fc / 063eb7fd: byte flags initialised to 1 and never written by
    // any instruction of the module. 063eb7f0 gates CAsteroidSystem::update's observer scan
    // (read at 062a44ed and again per candidate at 062a45c4); 063eb7fc / 063eb7fd gate
    // CAsteroidField::populate_asteroids' grid and look-ahead candidates (062a5801 / 062a5922).
    bool GAsteroidObserverScan = true;
    // / 063eb7fd (GAsteroidGridSearch / GAsteroidAheadSearch) live in common_casteroidfield.cpp.

    // the file-scope system registry (std::map<unsigned, CAsteroidSystem>),
    // built by the CRT dynamic initialiser 063ea194 -> the branch (map ctor the routine,
    // atexit(the branch) = the map dtor, which runs ~CAsteroidSystem on every node).
    // is its header node, read as end() by Load / Unload / Update /
    // CreateAsteroidField / Find.
    std::map<unsigned, CAsteroidSystem> GAsteroidSystems;
}

bool __cdecl compute_cube_orientation(unsigned long Seed, Matrix* InOutOrient, AxisRotations const& Rot, float Freq)
{
    // Freq is the probability of an empty cube.
    unsigned RandState = Seed;
    unsigned WarmCount = (NextLcgRaw(RandState) >> 13) & 3;
    for (; WarmCount != 0; --WarmCount)
        RandState = RandState * 214013u + 2531011u;

    if (Freq >= 0.0f)
    {
        // FCOMP Freq; TEST AH,5 / JP continues for >= or unordered.
        // Only an ordered-less result falls through to XOR AL,AL / RET.
        double const Prob = (double)NextLcgRaw(RandState) * (1.0f / 32768.0f);
        if (Prob < Freq)
            return false;
    }

    int AxisIndices[3];
    for (int Axis = 0; Axis < 3; ++Axis)
    {
        int Idx = (int)((double)NextLcgRaw(RandState) * (4.0f / 32768.0f));
        if (Idx < 0) Idx = 0;
        if (Idx > 3) Idx = 3;
        AxisIndices[Axis] = Idx;
    }

    float const Angle0 = Rot.Angles[0][AxisIndices[0]];
    float const Angle1 = Rot.Angles[1][AxisIndices[1]];
    float const Angle2 = Rot.Angles[2][AxisIndices[2]];

    // Row-mixing composition applied in place onto the caller-supplied matrix (expected
    // identity -- see header note). Each step mixes a pair of rows across all 3 columns;
    // the sign pattern (sub-then-add vs add-then-sub) alternates per axis exactly like
    // baseline's asm (loop1/loop3 share one sign order, loop2 the other).
    // FCOS is stored to a float; FSIN remains on the x87 stack until the row loop ends.
    double C = X87CosF((double)Angle0);
    double S = X87Sin((double)Angle0);
    for (int Col = 0; Col < 3; ++Col)
    {
        float const A = InOutOrient->E[1][Col];
        float const B = InOutOrient->E[2][Col];
        InOutOrient->E[1][Col] = (float)(A * C - B * S);
        InOutOrient->E[2][Col] = (float)(B * C + A * S);
    }

    C = X87CosF((double)Angle1);
    S = X87Sin((double)Angle1);
    for (int Col = 0; Col < 3; ++Col)
    {
        float const A = InOutOrient->E[0][Col];
        float const B = InOutOrient->E[2][Col];
        InOutOrient->E[0][Col] = (float)(A * C + B * S);
        InOutOrient->E[2][Col] = (float)(B * C - A * S);
    }

    C = X87CosF((double)Angle2);
    S = X87Sin((double)Angle2);
    for (int Col = 0; Col < 3; ++Col)
    {
        float const A = InOutOrient->E[0][Col];
        float const B = InOutOrient->E[1][Col];
        InOutOrient->E[0][Col] = (float)(A * C - B * S);
        InOutOrient->E[1][Col] = (float)(B * C + A * S);
    }

    return true;
}

// ============================================================================
// CAsteroidCube implementation
// ============================================================================

CAsteroidCube::CAsteroidCube(CAsteroidField* OwnerField)
    : CubeId(0)
    , Owner(OwnerField)
    , Size(0)
    , State(CubeStateUnknown)
    , StateTimer(0.0f)
    , SystemId(0)
    , bEmpty(false)
{
    // Common 0x46150. baseline leaves CubeId/Orient/Pos/SystemId untouched here --
    // they're filled in by the following init() call -- and writes indeterminate data to
    // the never-read +0x44 byte (not modeled, see header note); this port zero-initializes
    // everything for determinism instead of reproducing that leftover-register artifact.
    Pos.x = 0.0f;
    Pos.y = 0.0f;
    Pos.z = 0.0f;
    for (int Row = 0; Row < 3; ++Row)
        for (int Col = 0; Col < 3; ++Col)
            Orient.E[Row][Col] = (Row == Col) ? 1.0f : 0.0f;
}

CAsteroidCube::~CAsteroidCube()
{
    // CObject::Release on every asteroid in list order (no null
    // test, 062a61c0-062a61cc), then every node erased and the head freed.
    for (std::list<CObject*>::iterator It = Asteroids.begin(); It != Asteroids.end(); ++It)
        (*It)->Release();
    Asteroids.clear();
}

Vector CAsteroidCube::get_pos()
{
    // Common 0x43020.
    return Pos;
}

Matrix CAsteroidCube::get_orient()
{
    // Common 0x43040.
    return Orient;
}

unsigned long CAsteroidCube::get_id()
{
    // MOV EAX,[this+0x8]; RET.
    return CubeId;
}

CubeState CAsteroidCube::get_state()
{
    // MOV EAX,[this+0x40]; RET.
    return State;
}

void CAsteroidCube::set_size(int SizeValue)
{
    // MOV [this+0x3c],arg; RET 4.
    Size = SizeValue;
}

void CAsteroidCube::set_system(unsigned SystemIdValue)
{
    // Common 0x43060.
    SystemId = SystemIdValue;
}

void CAsteroidCube::set_state(CubeState StateValue)
{
    // Common 0x433e0.
    State = StateValue;
}

void CAsteroidCube::init(Vector const& PosIn, unsigned long CubeIdValue,
                          AsteroidCubeArch* CubeArch,
                          AxisRotations const& Rot, float Freq)
{
    // Common/.txt 0x46260 (item 70318). Portable prefix: stamp CubeId/Pos, reset the
    // state timer, and roll this cube's snap-rotation.
    CubeId = CubeIdValue;
    Pos = PosIn;
    StateTimer = 0.0f;
    for (int Row = 0; Row < 3; ++Row)
        for (int Col = 0; Col < 3; ++Col)
            Orient.E[Row][Col] = (Row == Col) ? 1.0f : 0.0f;

    // bEmpty (+0x54) = !compute_cube_orientation(...); set -> no spawn.
    bEmpty = !compute_cube_orientation(CubeId, &Orient, Rot, Freq);
    if (bEmpty)
        return;

    // Common (raw analysis, no analysis .c existed for this tail --
    // hand-traced). Per-archetype spawn loop, now ported for real:
    CFLIDMaker* Maker = CFLIDMaker::GetInstance();

    // param_3+4 is the list head; an empty list skips the loop.
    std::list<AsteroidCubeArchEntry> const& ArchList = CubeArch->ArchList;
    for (std::list<AsteroidCubeArchEntry>::const_iterator It = ArchList.begin(); It != ArchList.end(); ++It)
    {
        AsteroidCubeArchEntry const& Entry = *It;

        // (item 977). baseline resets a fresh CreateParms-shaped local record
        // every iteration; field-by-field, its writes (Common) are
        // byte-identical to CSimple::CreateParms's own default member initializers (Id=0,
        // ObjDb=null, OwnerPlayer=0, HitPts=-1.0f, Pos=(0,0,0), Orient=identity,
        // Velocity=(0,0,0), AngularVelocity=(0,0,0)) -- i.e. it is just "construct a fresh
        // default CreateParms", already exactly what this local declaration does. The
        // CAsteroid::CreateParms fields it adds (Pad58/AsteroidId/CubeId/bMine/
        // bInstantKill/OwnerField) carry no default member initializers of their own and
        // are set explicitly below, matching every write this loop body actually performs.
        CAsteroid::CreateParms Parms;
        Parms.Pad58 = 0;
        // AsteroidId (+0x5c) = 0, bMine / bInstantKill = 0,
        // OwnerField (+0x68) = 0 (all overwritten below) and +0x60 = this cube's CubeId
        // (this+0x08), copied by CAsteroid::init to CAsteroid+0xec.
        Parms.AsteroidId = 0;
        Parms.CubeId = CubeId;

        // I3DMathEngine +0x2c TransformMatrix(Out, Orient (+0x0c),
        // Entry.Extent) -> the rock's offset from the cube centre, before Pos is added.
        Vector LocalOffset;
        Acquire3DMathEngine()->TransformMatrix((FVector&)LocalOffset, (FMatrix const&)Orient,
                                               (FVector const&)Entry.Extent);

        // compute_asteroid_id runs on the pre-add LocalOffset, not the final world Pos
        // (Common: compute_asteroid_id(this, &LocalOffset, &AsteroidLocalId)
        // happens before the routine's add).
        unsigned long AsteroidLocalId = 0;
        if (!compute_asteroid_id(LocalOffset, AsteroidLocalId))
            continue;

        // Common: sprintf("%i%i", CubeId, AsteroidLocalId) then
        // CFLIDMaker::CreateStaticID -- the spawned rock's FLID nickname.
        char NameBuf[64];
        std::sprintf(NameBuf, "%i%i", (int)CubeId, (int)AsteroidLocalId);
        Parms.Id = Maker->CreateStaticID(NameBuf); // , no null test on Maker

        Parms.OwnerField = Owner;
        // == plain Vector add (confirmed: Common).
        Parms.Pos.x = Pos.x + LocalOffset.x;
        Parms.Pos.y = Pos.y + LocalOffset.y;
        Parms.Pos.z = Pos.z + LocalOffset.z;
        // I3DMathEngine +0x20 MulMatrix(Out, Orient, Entry.Orient)
        // into a local, copied into Parms.Orient (REP MOVSD, 9 dwords).
        Matrix RockOrient;
        Acquire3DMathEngine()->MulMatrix((FMatrix&)RockOrient, (FMatrix const&)Orient,
                                         (FMatrix const&)Entry.Orient);
        Parms.Orient = RockOrient;
        Parms.AsteroidId = AsteroidLocalId;
        Parms.bMine = Entry.bMine;
        Parms.bInstantKill = Entry.bInstantKill;

        // CObject::Alloc(0x1003=ClassAsteroid) (no null test),
        // stamp CollisionGroupId directly (new+0xf4 = this+0x50, NOT via CreateParms --
        // matches CAsteroid::CollisionGroupId's own header comment), vtbl+0x4c open(Arch),
        // then Archetype::Root::create_instance(new object) (module storage, return value
        // discarded -- the engine writes the id through IEngineInstance::initialize_instance),
        // then CAsteroid::init(Parms) called directly.
        CObject* NewObj = CObject::Alloc(CObject::ClassAsteroid);
        CAsteroid* NewAsteroid = (CAsteroid*)NewObj;
        NewAsteroid->CollisionGroupId = SystemId;
        NewAsteroid->open(Entry.Arch);
        Entry.Arch->create_instance(NewObj);
        NewAsteroid->init(Parms); // non-virtual CAsteroid::init, module storage

        // Common: link the new asteroid into this cube's Asteroids
        // list (baseline's intrusive list at this+0x48/0x4c -- this port's std::list).
        Asteroids.push_back(NewObj);
    }
}

asteroid_marker* CAsteroidCube::get_changes()
{
    // . An empty cube (+0x54) has none. Only State 1 (Active) or 3
    // (Far) report (DEC EAX; JZ / SUB EAX,2; JNZ out). Otherwise one marker per entry of the
    // asteroid list (+0x48, node+8 = CObject*), in list order: operator_new(0x14) zeroed,
    // then CubeId <- this+0x08, AsteroidId <- obj+0xe8, ObjId <- obj+0xb0,
    // bCubeActive <- (State == 1), bMine <- obj+0xf0, bInstantKill <- obj+0xf1; the first
    // marker is the returned head, each later one is linked from the previous one's Next.
    if (bEmpty)
        return nullptr;

    asteroid_marker* Head = nullptr;
    if (State != CubeStateActive && State != CubeStateFar)
        return Head;

    asteroid_marker* Prev = nullptr;
    for (std::list<CObject*>::iterator It = Asteroids.begin(); It != Asteroids.end(); ++It)
    {
        CAsteroid* const Rock = (CAsteroid*)*It;
        asteroid_marker* Marker = new asteroid_marker();
        Marker->CubeId = CubeId;
        Marker->AsteroidId = Rock->AsteroidId;
        Marker->ObjId = Rock->Id;
        Marker->bCubeActive = (State == CubeStateActive);
        Marker->bMine = Rock->bMine;
        Marker->bInstantKill = Rock->bInstantKill;
        if (Head == nullptr)
            Head = Marker;
        else
            Prev->Next = Marker;
        Prev = Marker;
    }
    return Head;
}

bool CAsteroidCube::compute_asteroid_id(Vector const& PosIn, unsigned long& OutId)
{
    // . Pure function of PosIn; does not touch *this despite being a
    // member (baseline never dereferences its own "this" here). Packs abs(nudged coord) >> 1
    // per axis (10 bits, sign folded into bit 9); OutId is written only on success.
    int const RoundedX = NudgeTruncate(PosIn.x);
    int const RoundedY = NudgeTruncate(PosIn.y);
    int const RoundedZ = NudgeTruncate(PosIn.z);

    unsigned IdX = (unsigned)std::abs(RoundedX) >> 1;
    unsigned IdY = (unsigned)std::abs(RoundedY) >> 1;
    unsigned IdZ = (unsigned)std::abs(RoundedZ) >> 1;

    if (IdX >= 0x200 || IdY >= 0x200 || IdZ >= 0x200)
        return false;

    if (RoundedX < 0)
        IdX |= 0x200;
    if (RoundedY < 0)
        IdY |= 0x200;
    if (RoundedZ < 0)
        IdZ |= 0x200;

    OutId = IdZ | (IdY << 10) | (IdX << 0x14);
    return true;
}

// ============================================================================
// CAsteroidSystem implementation
// ============================================================================

CAsteroidSystem::CAsteroidSystem()
    : RefCount(0)
    , SystemId(0)
{
    // Fields (+0x08..+0x10) built empty (the clear loop that follows
    // runs over an empty list), SystemId = 0, RefCount = 0. baseline leaves FindIter (+0x14)
    // uninitialised; the port starts it at end() so FindNext before any FindFirst is
    // deterministic.
    FindIter = Fields.end();
}

CAsteroidSystem::~CAsteroidSystem()
{
    // . SystemId = 0, then every field is released: the inlined
    // CAsteroidField::Release (062a42f4-062a4394) decrements +0x10 and destroys and frees the
    // field when the count reaches 0. Then the list nodes and head are freed.
    SystemId = 0;
    for (std::list<CAsteroidField*>::iterator It = Fields.begin(); It != Fields.end(); ++It)
        (*It)->Release();
}

CAsteroidField* CAsteroidSystem::create_field()
{
    // . new CAsteroidField (operator new(0x80) + ctor, 0 when the
    // allocation fails), push_back, then on the back element: RefCount +1 and
    // SystemId (+0x40) = this->SystemId; returns that element.
    Fields.push_back(new CAsteroidField());
    CAsteroidField* Field = Fields.back();
    ++Field->RefCount;
    Field->SystemId = SystemId;
    return Field;
}

void CAsteroidSystem::update(IObjDB* ObjDb, float Dt)
{
    // .
    // flush_changes on every field.
    for (std::list<CAsteroidField*>::iterator It = Fields.begin(); It != Fields.end(); ++It)
        (*It)->flush_changes();

    // observer scan, re-tested against the field before every candidate.
    {
        // the IObjDB::GetNext cursor {0, 0, mask 0x7f0000}, built once.
        struct FIterState
        {
            unsigned Unused0;
            unsigned Unused1;
            unsigned Mask;
        };
        FIterState Cursor = { 0, 0, 0x7f0000u };

        while (GAsteroidObserverScan)
        {
            IObjRW* Candidate = ObjDb->GetNext(&Cursor);
            if (Candidate == nullptr)
                break;

            // IObjInspect +0x134 is_using_tradelane into a stack byte;
            // a candidate on a trade lane is skipped.
            bool bUsingTradelane = false;
            (void)ObjInspectOf(Candidate)->is_using_tradelane(&bUsingTradelane);
            if (bUsingTradelane)
                continue;

            // get_position (slot 0) copied into a local Vector.
            Vector Pos = ObjInspectOf(Candidate)->get_position();
            // get_transform (slot 3); the negated third column
            // (+0x08 / +0x14 / +0x20) is Dir.
            Matrix const& Rotation = ObjInspectOf(Candidate)->get_transform().Rotation;
            Vector Dir;
            Dir.x = -Rotation.E[0][2];
            Dir.y = -Rotation.E[1][2];
            Dir.z = -Rotation.E[2][2];

            // near_field / populate_asteroids on the fields in list order;
            // the counter is incremented before the call and the walk stops at 2.
            int PopulatedCount = 0;
            for (std::list<CAsteroidField*>::iterator It = Fields.begin(); It != Fields.end(); ++It)
            {
                if (!(*It)->near_field(Pos))
                    continue;
                ++PopulatedCount;
                (*It)->populate_asteroids(Pos, Dir);
                if (PopulatedCount >= 2)
                    break;
            }
        }
    }

    // update(Dt) on every field.
    for (std::list<CAsteroidField*>::iterator It = Fields.begin(); It != Fields.end(); ++It)
        (*It)->update(Dt);
}

int CAsteroidSystem::AddRef()
{
    // Common 0x44600.
    return ++RefCount;
}

int CAsteroidSystem::Release()
{
    // Common 0x44610. baseline does not self-delete at zero here (that's driven by
    // CmnAsteroid::Unload's own refcount check instead).
    return --RefCount;
}

CAsteroidField* CAsteroidSystem::FindFirst()
{
    // Common 0x44620.
    FindIter = Fields.begin();
    if (FindIter == Fields.end())
        return nullptr;
    return *FindIter;
}

CAsteroidField* CAsteroidSystem::FindNext()
{
    // Common 0x44640.
    if (FindIter == Fields.end())
        return nullptr;
    ++FindIter;
    if (FindIter == Fields.end())
        return nullptr;
    return *FindIter;
}

// ============================================================================
// Global registry (CmnAsteroid::Load/Unload/Find/Update/CreateAsteroidField)
// ============================================================================

int __cdecl Load(unsigned SystemId)
{
    // . Found -> RefCount + 1, return 2 (062a3be8). Otherwise a
    // default CAsteroidSystem (062a3c09-062a3c2e, the inlined ctor) is inserted under SystemId
    // (the routine, 062a3c7d; the pair copy and the temporary are destroyed afterwards), the
    // new entry's RefCount + 1 and SystemId = SystemId (062a3cd0-062a3cdb), return 1.
    std::map<unsigned, CAsteroidSystem>::iterator It = GAsteroidSystems.find(SystemId);
    if (It != GAsteroidSystems.end())
    {
        ++It->second.RefCount;
        return 2;
    }
    It = GAsteroidSystems.emplace(std::piecewise_construct, std::forward_as_tuple(SystemId),
                                  std::forward_as_tuple()).first;
    ++It->second.RefCount;
    It->second.SystemId = SystemId;
    return 1;
}

int __cdecl Unload(unsigned SystemId)
{
    // . Not found -> 0; RefCount - 1 != 0 -> 2; otherwise the entry
    // is erased (the routine, ~CAsteroidSystem) -> 1.
    std::map<unsigned, CAsteroidSystem>::iterator It = GAsteroidSystems.find(SystemId);
    if (It == GAsteroidSystems.end())
        return 0;
    if (--It->second.RefCount != 0)
        return 2;
    GAsteroidSystems.erase(It);
    return 1;
}

void __cdecl Update(IObjDB* ObjDb, float Dt)
{
    // . Only the system the object database belongs to is updated:
    // ObjDb->GetZoneDb() (vtable +0x08) is the Universe::ISystem, its slot 0 the system id
    // (062a3efc-062a3f03); that entry, if registered, gets update(ObjDb, Dt).
    Universe::ISystem const* const System = ObjDb->GetZoneDb();
    unsigned const CurrentSystemId = System->GetId();
    std::map<unsigned, CAsteroidSystem>::iterator It = GAsteroidSystems.find(CurrentSystemId);
    if (It != GAsteroidSystems.end())
        It->second.update(ObjDb, Dt);
}

CAsteroidField* __cdecl CreateAsteroidField(unsigned SystemId)
{
    // . Registered -> create_field(); otherwise Load(SystemId) and
    // return 0 (the caller gets no field on this call).
    std::map<unsigned, CAsteroidSystem>::iterator It = GAsteroidSystems.find(SystemId);
    if (It == GAsteroidSystems.end())
    {
        Load(SystemId);
        return nullptr;
    }
    return It->second.create_field();
}

CAsteroidSystem* __cdecl Find(unsigned SystemId)
{
    // the entry's value (node + 0x10) or 0.
    std::map<unsigned, CAsteroidSystem>::iterator It = GAsteroidSystems.find(SystemId);
    if (It == GAsteroidSystems.end())
        return nullptr;
    return &It->second;
}

}
