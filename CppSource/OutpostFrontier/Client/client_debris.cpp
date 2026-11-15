// Author: Giperion.

#include "client_debris.h"

#include "client_explosion.h"   // Action_Explode (the routine)
#include "client_objdb.h"       // ClientObjDb_RegisterGameobj (the routine)
#include "common_csys.h"        // LookMatrix, Acquire3DMathEngine
#include "common_ini.h"         // INI_Reader
#include "common_physys.h"
#include "common_universe.h"    // Archetype::GetExplosion
#include "neuroncomponents.h"              // FDUMP
#include "gf_object_ship.h"     // ShipTransposeRotate (the routine)
#include "particle_effect.h"    // CliCreateEffectOneShot (the routine)
#include "star_sys.h"           // StarSysGetActiveSystemId / StarSysFindNebula
#include "vis_effect_arch.h"    // FindEffectArch (the routine)
#include "x86math.h"            // I3DMathEngine, FTransform

#include <cfloat>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <utility>

namespace
{
    char const kDebrisSourceFile[] = "Client\\space\\Debris.cpp";

    // 1/32768, the rand() scale.
    constexpr float kRandScale = 3.0517578125e-05f;

    // the [Debris] table, keyed by the nickname id (the static initialiser
    // builds it, the branch tears it down at exit).
    std::map<unsigned, FDebrisArch> G_DebrisArchDb;

    // the record the routine hands out when a name is unknown (the branch).
    FDebrisArch G_DefaultDebrisArch;

    // Debris.cpp's lazily acquired 3DMathEngine (0x00530372..0x00530381).
    I3DMathEngine* G_DebrisMathEngine = nullptr;

    // The object narrowed by (ObjectClass & Mask) == Mask, null otherwise -- the `this`,
    // `this + 0xc` and object tests 0x0053077c..0x00530799 inlines.
    CObject* NarrowDebrisObject(FClientGameobj* Gameobj, unsigned Mask)
    {
        if (Gameobj == nullptr)
            return nullptr;
        CObject* const Obj = Gameobj->Object;
        if (Obj == nullptr || (static_cast<unsigned>(Obj->ObjectClass) & Mask) != Mask)
            return nullptr;
        return Obj;
    }
}

// ===========================================================================
// The [Debris] table
// ===========================================================================

FDebrisArch::FDebrisArch()
{
    LifetimeMin = 0.0f;
    LifetimeMax = FLT_MAX;
    DeathMethod = 1;
    Explosion.clear();
    Trail.clear();
    LinearDrag = 0.0f;
    AngularDrag.x = 0.0f;
    AngularDrag.y = 0.0f;
    AngularDrag.z = 0.0f;
}

bool ReadDebrisArch(INI_Reader* Reader, bool bQuiet)
{
    // (cdecl).
    FDebrisArch Arch;
    ID_String Nickname;
    Nickname.clear();

    while (Reader->read_value())
    {
        if (Reader->is_value("nickname"))
        {
            Nickname.Id = CreateID(Reader->get_value_string());
        }
        else if (Reader->is_value("trail"))
        {
            Arch.Trail.Id = CreateID(Reader->get_value_string());
        }
        else if (Reader->is_value("lifetime"))
        {
            // one value -> min = max; with a second, max is that value
            // unless it is below min (TEST AH,0x5 / JP), then max = min. The second value is
            // read again for the store.
            Arch.LifetimeMin = Reader->get_value_float(0);
            if (Reader->is_value_empty(1))
                Arch.LifetimeMax = Arch.LifetimeMin;
            else if (Reader->get_value_float(1) < Arch.LifetimeMin)
                Arch.LifetimeMax = Arch.LifetimeMin;
            else
                Arch.LifetimeMax = Reader->get_value_float(1);
        }
        else if (Reader->is_value("explosion"))
        {
            Arch.Explosion.Id = CreateID(Reader->get_value_string());
        }
        else if (Reader->is_value("death_method"))
        {
            // the value copied into a 16-byte buffer (at most 15
            // characters; none when the string is null) and compared without case. The cut
            // cannot turn a longer value into either name, so comparing the value itself
            // decides the same; any other value leaves the method as it was.
            char const* const Method = Reader->get_value_string();
            if (Method != nullptr)
            {
                if (_stricmp(Method, "exploding") == 0)
                    Arch.DeathMethod = 0;
                else if (_stricmp(Method, "vanishing") == 0)
                    Arch.DeathMethod = 1;
            }
        }
        else if (Reader->is_value("linear_drag"))
        {
            Arch.LinearDrag = Reader->get_value_float(0);
        }
        else if (Reader->is_value("angular_drag"))
        {
            // read as values 2, 1, 0.
            float const Z = Reader->get_value_float(2);
            float const Y = Reader->get_value_float(1);
            float const X = Reader->get_value_float(0);
            Arch.AngularDrag.x = X;
            Arch.AngularDrag.y = Y;
            Arch.AngularDrag.z = Z;
        }
    }

    // a section without a nickname is dropped; an existing nickname
    // keeps its first record (the routine does not replace) and warns unless bQuiet.
    if (Nickname.IsEmpty())
        return false;
    std::pair<std::map<unsigned, FDebrisArch>::iterator, bool> const Inserted =
        G_DebrisArchDb.insert(std::make_pair(Nickname.Id, Arch));
    if (Inserted.second)
        return true;
    if (!bQuiet)
    {
        FDUMP(0x100002,
              "%s(%d) : *** WARNING: Duplicate [Debris] name(%s).  First overwritten with second!",
              kDebrisSourceFile, 0xf8, Nickname.get_string());
    }
    return false;
}

int LoadDebrisArchFile(char const* Path, bool bQuiet)
{
    // (cdecl).
    size_t const SizeBefore = G_DebrisArchDb.size();
    INI_Reader Reader;
    if (Reader.open(Path, false))
    {
        while (Reader.read_header())
        {
            if (Reader.is_header("Debris"))
                ReadDebrisArch(&Reader, bQuiet);
        }
    }
    else
    {
        FDUMP(0x100002, "%s(%d) : *** WARNING: Failed to open debris file[%s].",
              kDebrisSourceFile, 0x147, Path);
    }
    return static_cast<int>(G_DebrisArchDb.size() - SizeBefore);
}

void ShutdownDebrisArchDb()
{
    // the table's erase(begin, end).
    G_DebrisArchDb.clear();
}

FDebrisArch* FindDebrisArch(ID_String const* Nickname)
{
    // lower_bound (the routine) and the key test; the record is node + 0x10.
    std::map<unsigned, FDebrisArch>::iterator const It = G_DebrisArchDb.find(Nickname->Id);
    if (It == G_DebrisArchDb.end())
        return nullptr;
    return &It->second;
}

FDebrisArch* GetDefaultDebrisArch()
{
    // .
    return &G_DefaultDebrisArch;
}

// ===========================================================================
// Debris
// ===========================================================================

FClientDebrisGameobj::FClientDebrisGameobj()
    : FClientGameobj(EClientGameobjKind::Debris)
{
    // +0xb4 = 0, +0xb8 = 0, +0xbc = 1. The death method and the two
    // lifetimes are left for the routine.
    Explosion = nullptr;
    Remaining = 0.0f;
    Field_0xbc = 1;
}

FClientDebrisGameobj::~FClientDebrisGameobj()
{
    // the explosion's references dropped (the routine), then GameObject's
    // destructor the routine.
    if (Explosion != nullptr)
        ExplosionArchRelease(Explosion);
}

int FClientDebrisGameobj::get_type(uint& OutType) const
{
    // .
    OutType = 0x80000000u;
    return 0;
}

void FClientDebrisGameobj::create(Archetype::Root* Arch)
{
    // (RET 4): no narrowing, no null test on the new object.
    GameobjCreateOfClass(this, CObject::ClassObject, Arch);
    Object->open(Arch);
}

bool FClientDebrisGameobj::find(unsigned const& Id)
{
    // XOR AL,AL / RET 4.
    (void)Id;
    return false;
}

void FClientDebrisGameobj::update(float Dt)
{
    // (RET 4).
    // the object's update with CObject::UPDATE_DEFAULT (vt+0x50), then
    // GameObject's update the routine.
    Object->update(Dt, CObject::UPDATE_DEFAULT);
    FClientGameobj::update(Dt);

    // TEST AH,0x41 / JNZ -- nothing once the countdown is not above 0.
    if (!(Remaining > 0.0f))
        return;
    Remaining = Remaining - Dt;

    // a vanishing piece (method 1) with Field_0xbc clear goes at once
    // when it has lived longer than lifetime min (max - remaining > min, TEST AH,0x41 / JNZ).
    int const Method = DeathMethod;
    if (Method == 1 && Field_0xbc == 0)
    {
        if (LifetimeMax - Remaining > LifetimeMin)
            Remaining = 0.0f;
    }

    // TEST AH,0x41 / JP -- at 0 or below (ordered): exploding (0) goes
    // through slot 108 with no position, vanishing (1) finishes the Gameobj (slot 87).
    if (Remaining <= 0.0f)
    {
        if (Method == 0)
        {
            Slot1b0(nullptr);
            return;
        }
        if (Method == 1)
            Slot15c(true);
    }
}

unsigned* FClientDebrisGameobj::Slot17c(unsigned* OutLod, void* Camera,
                                        unsigned const* CachedLod)
{
    // (RET 0xc): GameObject's pick the routine into a local (baseline reuses the
    // third argument's stack slot), its return value ignored.
    unsigned Lod = 0;
    FClientGameobj::Slot17c(&Lod, Camera, CachedLod);
    // CMP CX,3 / JC (unsigned low word < 3), CMP word,2 / JL (the
    // signed high word below 2 keeps the flag).
    unsigned short const LowWord = static_cast<unsigned short>(Lod & 0xffffu);
    short const HighWord = static_cast<short>(static_cast<unsigned short>(Lod >> 16));
    if (LowWord < 3u || HighWord >= 2)
        Field_0xbc = 0;
    *OutLod = Lod;
    return OutLod;
}

void FClientDebrisGameobj::Slot1b0(Vector const* Pos)
{
    // (RET 4).
    if (Explosion == nullptr)
    {
        // GameObject's slot 108 the routine.
        FClientGameobj::Slot1b0(Pos);
        return;
    }
    // Pos, or get_position() (vt+0) when it is null, copied to the
    // stack; the routine(Explosion, this, &Copy) into the action list. Unlike GameObject's
    // slot 108 the effect list is not cleared.
    Vector const* const Where = (Pos != nullptr) ? Pos : &get_position();
    Vector const Copy = *Where;
    Actions.add(Action_Explode::Create(Explosion, this, &Copy));
}

void FClientDebrisGameobj::Init(FDebrisArch const* Arch)
{
    // (RET 4).
    // GameObject's slot 92 the routine, called directly. It reads bWatchListed
    // only after get_type, and Debris's get_type (0x80000000) has none of the bits it tests.
    FClientGameobj::Slot170();

    // +0xa8 = the death method (the baseline dword store overwrites the
    // bWatchListed byte too), +0xac = lifetime min, +0xb0 = lifetime max.
    DeathMethod = Arch->DeathMethod;
    bWatchListed = static_cast<unsigned char>(DeathMethod & 0xff);
    LifetimeMin = Arch->LifetimeMin;
    LifetimeMax = Arch->LifetimeMax;

    // the explosion, with its references taken (the routine).
    if (!Arch->Explosion.IsEmpty())
    {
        Explosion = Archetype::GetExplosion(Arch->Explosion);
        if (Explosion != nullptr)
            ExplosionArchAddRef(Explosion);
    }
    else
    {
        Explosion = nullptr;
    }

    // the countdown, lifetime min + rand()/32768 * (max - min).
    float const Min = LifetimeMin;
    float const Max = LifetimeMax;
    Remaining = static_cast<float>(rand()) * kRandScale * (Max - Min) + Min;

    // the trail -- a one-shot on this Gameobj, pointing along the
    // velocity (LookMatrix) at the centre of mass (the object's vt+0x68 when it has a body,
    // else get_position()), expressed in the object's frame.
    if (!Arch->Trail.IsEmpty())
    {
        Vector Where;
        if (Object->BodyPtr != nullptr)
            Where = Object->get_center_of_mass();
        else
            Where = get_position();
        Vector const Velocity = get_velocity();
        Matrix const Look = LookMatrix(Velocity);
        FTransform World;
        World.Rotation = reinterpret_cast<FMatrix const&>(Look);
        World.Translation.X = Where.x;
        World.Translation.Y = Where.y;
        World.Translation.Z = Where.z;

        // the inverse of the object's frame -- get_orientation (vt+0x10)
        // and the position that follows it: rotation transposed (the routine), translation
        // -(M^T * P) (the routine, then the routine on the negated floats; the routine
        // packs the two).
        Transform const& ObjectFrame = reinterpret_cast<Transform const&>(get_orientation());
        FMatrix const& Rotation = reinterpret_cast<FMatrix const&>(ObjectFrame.Rotation);
        FVector ObjectPos;
        ObjectPos.X = ObjectFrame.Translation.x;
        ObjectPos.Y = ObjectFrame.Translation.y;
        ObjectPos.Z = ObjectFrame.Translation.z;
        FVector Rotated;
        ShipTransposeRotate(Rotated, Rotation, ObjectPos); // (0x00530302)
        FTransform Inverse;
        int Row = 0;
        for (Row = 0; Row < 3; ++Row)
        {
            int Col = 0;
            for (Col = 0; Col < 3; ++Col)
                Inverse.Rotation.D[Row][Col] = Rotation.D[Col][Row];
        }
        Inverse.Translation.X = -Rotated.X;
        Inverse.Translation.Y = -Rotated.Y;
        Inverse.Translation.Z = -Rotated.Z;

        // Local = Inverse * World through the field's MulTransform
        // (vt+0x1c).
        if (G_DebrisMathEngine == nullptr)
            G_DebrisMathEngine = Acquire3DMathEngine();
        FTransform Local;
        G_DebrisMathEngine->MulTransform(Local, Inverse, World);

        // get_index (vt+0x1c), then -- the trail id tested again --
        // and the routine on the field (EffectArch, this, index, &Local, no
        // companion).
        int const Index = static_cast<int>(get_index());
        if (!Arch->Trail.IsEmpty())
            CliCreateEffectOneShot(FindEffectArch(&Arch->Trail.Id), this, Index, &Local, nullptr);
    }

    // with a physics body, the drag divided by the mass (vt+0x6c) and
    // by the moment of inertia (zero when the body is gone by the second test).
    if (Object->BodyPtr != nullptr)
    {
        float const Linear = Arch->LinearDrag / Object->get_mass();
        Vector Inertia;
        if (Object->BodyPtr != nullptr)
        {
            Inertia = PhySys::GetMomentOfInertia(Object);
        }
        else
        {
            Inertia.x = 0.0f;
            Inertia.y = 0.0f;
            Inertia.z = 0.0f;
        }
        Vector Angular;
        Angular.x = Arch->AngularDrag.x / Inertia.x;
        Angular.y = Arch->AngularDrag.y / Inertia.y;
        Angular.z = Arch->AngularDrag.z / Inertia.z;
        PhySys::SetDamping(Object, Linear, Angular);
    }

    // the nebula at get_position() into +0x38, its fog factor into
    // +0x3c (the routine, no hint).
    FNebulaEffect* const Nebula =
        StarSysFindNebula(&get_position(), reinterpret_cast<float*>(&DrawStateB), nullptr);
    DrawStateA = static_cast<unsigned>(reinterpret_cast<uintptr_t>(Nebula));
}

FClientDebrisGameobj* CreateDebrisGameobjForObject(CObject* Object, FDebrisArch const* Arch)
{
    // (cdecl).
    FClientDebrisGameobj* const Debris = new FClientDebrisGameobj();
    Debris->Object = Object;                                               // +0x10
    Debris->advise(true);                                                  // CBase::advise
    Debris->Init(Arch);
    ClientObjDb_RegisterGameobj(Debris, false);                            // (, 0)
    return Debris;
}

FClientDebrisGameobj* CreateDebrisGameobjFromArch(Archetype::Root* Arch,
                                                  FDebrisArch const* DebrisArch,
                                                  Vector const* Pos)
{
    // (cdecl).
    FClientDebrisGameobj* const Debris = new FClientDebrisGameobj();
    // Arch->load (vt+0xc), its companion's Load(1) (vt+0x20), the
    // Gameobj's create (vt+0x168), Arch->Release.
    Arch->load();
    Arch->Companion->Load(1);
    Debris->create(Arch);
    Arch->Release();

    // the object (class & 1, no null test on the result) placed at Pos
    // (+0x2c).
    CObject* const Simple = NarrowDebrisObject(Debris, 1u);
    Simple->WorldPosition = *Pos;

    // a body -- mass 1, the default damping, type 2 with collisions
    // on, no inertia, no name, the current system (the routine, read last).
    PhySys::CreateParms Parms;
    std::memset(&Parms, 0, sizeof(Parms));
    Parms.Mass = 1.0f;
    Parms.LinearDamping = PhySys::DEFAULT_LINEAR_DAMPING;
    Parms.AngularDamping[0] = PhySys::DEFAULT_ANGULAR_DAMPING.x;
    Parms.AngularDamping[1] = PhySys::DEFAULT_ANGULAR_DAMPING.y;
    Parms.AngularDamping[2] = PhySys::DEFAULT_ANGULAR_DAMPING.z;
    Parms.CollisionGroupId = 0;
    Parms.Type = 2;
    Parms.bFlag1c = 0;
    Parms.bEnableCollisions = 1;
    Parms.bHasInertia = 0;
    Parms.Inertia.x = 0.0f;
    Parms.Inertia.y = 0.0f;
    Parms.Inertia.z = 0.0f;
    Parms.Name[0] = '\0';
    Parms.CollisionGroupId = StarSysGetActiveSystemId();
    PhySys::BuildIVP_Real(Simple, Parms);

    // the routine(DebrisArch), the routine(Debris, 0).
    Debris->Init(DebrisArch);
    ClientObjDb_RegisterGameobj(Debris, false);
    return Debris;
}
