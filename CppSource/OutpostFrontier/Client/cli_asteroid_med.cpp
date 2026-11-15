// Author: Giperion.

#include "cli_asteroid_med.h"

#include "cli_asteroid.h"
#include "client_explosion.h"   // Action_Explode (the routine)
#include "client_objdb.h"       // ClientObjDb_RegisterGameobj (the routine)
#include "common_server.h"      // IServer::SPBadLandsObjCollision (slot 54)
#include "common_misc.h"        // SinglePlayer
#include "neuroncomponents.h"              // FDUMP
#include "player_spawn.h"       // GetPlayerConnectionId (the field)
#include "server_connect.h"     // GetLocalServerInterface (the field)

#include <cstdint>
#include <new>
#include <utility>

namespace
{
    // CliAsteroid.cpp's FDUMP file argument.
    char const kCliAsteroidSourceFile[] = "Client\\Space\\CliAsteroid.cpp";

    static_assert(sizeof(FCliAsteroidCubeMap) <= sizeof(FCliAsteroid::MapAt08),
                  "the cube map must fit the baseline 0x14 bytes at FCliAsteroid+0x08");
    static_assert(alignof(FCliAsteroidCubeMap) <= 8, "FCliAsteroid+0x08 is 8-aligned");
}

// ===========================================================================
// MedAsteroid
// ===========================================================================

FCliMedAsteroidGameobj::FCliMedAsteroidGameobj()
    : FClientGameobj(EClientGameobjKind::MedAsteroid)
{
    // the routine(8) and the three vtable stores -- nothing else.
    // bWatchListed (+0xa8) is not written here; the routine stores the instant-kill byte into
    // it after slot 92 (the routine) has already read it.
}

FCliMedAsteroidGameobj::~FCliMedAsteroidGameobj()
{
    // MedAsteroid adds no data -- the vtables go back and GameObject's
    // runs (the base destructor).
}

bool FCliMedAsteroidGameobj::find(unsigned const& Id)
{
    // (RET 4). The exe imports the reference overload of CObject::Find and pushes
    // the slot's own reference argument (0x00524ee0 / 0x00524eea).
    Object = CObject::Find(Id, CObject::ClassAsteroid);
    // advise only when the object was found.
    if (Object != nullptr)
        advise(true);
    return Object != nullptr;
}

void FCliMedAsteroidGameobj::Slot1a0(void const* Contact, void const* B, FClientGameobj* Other,
                                     void const* D)
{
    // (RET 0x10). 0x00524f20..0x00524f4d: bWatchListed, Other, Other's CBase
    // (+0xc, never null for a live Other) and its object, class & 0x503 == 0x503 (a CShip).
    if (bWatchListed != 0 && Other != nullptr && Other->GetObject() != nullptr &&
        (static_cast<unsigned>(Other->GetObject()->ObjectClass) & 0x503u) == 0x503u)
    {
        // own slot 108 with the contact point (the first argument).
        Slot1b0(static_cast<Vector const*>(Contact));
        return;
    }
    // the arguments rewritten in place unchanged and a tail JMP to
    // GameObject's the routine.
    FClientGameobj::Slot1a0(Contact, B, Other, D);
}

void FCliMedAsteroidGameobj::Slot1ac(void const* A, void const* B, FClientGameobj* Other,
                                     void const* D)
{
    // (RET 0x10).
    if (bWatchListed == 0)
    {
        // GameObject's the routine with the same four arguments.
        FClientGameobj::Slot1ac(A, B, Other, D);
        return;
    }

    // Other's vt+0x20 get_id; 0 -> nothing.
    if (Other->get_id() == 0)
        return;

    // in multiplayer only an Other the local player owns reports --
    // its class & 3 object's +0xb4 OwnerPlayer, or the routine when the narrow fails, against
    // .
    if (!SinglePlayer())
    {
        CObject* const OtherObj = Other->GetObject();
        unsigned Owner = 0;
        if (OtherObj != nullptr && (static_cast<unsigned>(OtherObj->ObjectClass) & 3u) == 3u)
            Owner = static_cast<CSimple const*>(OtherObj)->OwnerPlayer;
        else
            Owner = GetPlayerConnectionId();
        if (Owner != GetPlayerConnectionId())
            return;
    }

    // {this id, Other's id, D's low word} and the field into
    // slot 54 (+0xd8), no null test. The word at +0x0a is the stack's.
    SSPBadLandsObjCollisionInfo Info;
    Info.Field_0x00 = get_id();
    Info.ObjectId = Other->get_id();
    Info.Field_0x08 = static_cast<unsigned short>(reinterpret_cast<uintptr_t>(D));
    Info.Pad_0x0a = 0;
    static_cast<IServer*>(GetLocalServerInterface())
        ->SPBadLandsObjCollision(Info, GetPlayerConnectionId());
}

void FCliMedAsteroidGameobj::Slot1b0(Vector const* Pos)
{
    // (RET 4). 0x00525044..0x00525051: nothing while an action is pending.
    if (Actions.get_action_count() != 0)
        return;

    if (ExplosionArch == nullptr)
    {
        // .
        FDUMP(0x100002, "%s(%d) : *** WARNING: %s", kCliAsteroidSourceFile, 0x7a5,
              "asteroid_mine told to explode but has no explosion archetype");
        return;
    }
    if (ExplosionArch->Process != 2)
    {
        // .
        FDUMP(0x100002, "%s(%d) : *** WARNING: %s", kCliAsteroidSourceFile, 0x79c,
              "Explosion process for asteroid should be set to none");
        return;
    }
    // the routine(ExplosionArch, this, Pos or get_position()) -- the
    // pointer itself, no stack copy -- into the action list.
    Vector const* const Where = (Pos != nullptr) ? Pos : &get_position();
    Actions.add(Action_Explode::Create(ExplosionArch, this, Where));
}

unsigned short* FCliMedAsteroidGameobj::Slot1bc(unsigned short* Out, unsigned A, unsigned B,
                                               unsigned C, unsigned D)
{
    // .
    (void)A;
    (void)B;
    (void)C;
    (void)D;
    return CliAsteroidSlot1bcBody(Out);
}

unsigned short* CliAsteroidSlot1bcBody(unsigned short* Out)
{
    // MOV EAX,[ESP+4]; MOV word [EAX],0; MOV word [EAX+2],0xffff; RET 0x14.
    Out[0] = 0;
    Out[1] = 0xffff;
    return Out;
}

// ===========================================================================
// The cube value
// ===========================================================================

FCliAsteroidCube::FCliAsteroidCube()
    : CubeId(0)
    , CubeSize(0xfa)
{
    // (the routine's temporary): the routine builds the inner map
    // (the member's constructor here), +4 = 0, +0x38 = 0xfa. Cube, Orient and Pos are not
    // written.
}

FCliAsteroidCube::~FCliAsteroidCube()
{
    // (thiscall on the value). 0x00524b12..0x00524b66: slot 87 (vt+0x15c) with
    // true on every asteroid in map order -- the ObjDB reap deletes them later.
    for (std::map<unsigned long, FCliMedAsteroidGameobj*>::iterator It = Asteroids.begin();
         It != Asteroids.end(); ++It)
    {
        It->second->Slot15c(true);
    }
    // the map's erase(begin, end) and its destructor (the member's).
}

void FCliAsteroidCube::ApplyEvent(CmnAsteroid::asteroid_marker const* Event)
{
    // (RET 4). 0x00524cd2..0x00524ce0: find the event's asteroid id (+4).
    std::map<unsigned long, FCliMedAsteroidGameobj*>::iterator const It =
        Asteroids.find(Event->AsteroidId);

    if (Event->bCubeActive) // +0x0c
    {
        if (It != Asteroids.end())
        {
            // .
            FDUMP(0x100002, "%s(%d) : *** WARNING: %s", kCliAsteroidSourceFile, 0x72d,
                  "asteroid cube told to create asteroid that already exists");
            return;
        }
        // operator new(0xac) and the inlined ctor.
        FCliMedAsteroidGameobj* const Med = new FCliMedAsteroidGameobj();
        // the routine -- insert {AsteroidId, Med}.
        Asteroids.insert(std::pair<unsigned long const, FCliMedAsteroidGameobj*>(
            Event->AsteroidId, Med));
        // vt+0x16c find(&Event->ObjId), then vt+0x170 (the routine).
        Med->find(Event->ObjId);
        Med->Slot170();
        // bWatchListed = bInstantKill (+0x0e), then the routine(Med, 0).
        Med->bWatchListed = Event->bInstantKill ? 1 : 0;
        ClientObjDb_RegisterGameobj(Med, false);
        return;
    }

    if (It == Asteroids.end())
    {
        // .
        FDUMP(0x100002, "%s(%d) : *** WARNING: %s", kCliAsteroidSourceFile, 0x739,
              "asteroid cube told to delete asteroid that does not exist");
        return;
    }
    // slot 87 (vt+0x15c) with true on the gameobj, then erase
    // (the routine).
    It->second->Slot15c(true);
    Asteroids.erase(It);
}

// ===========================================================================
// The field's cube map (FCliAsteroid +0x08)
// ===========================================================================

FCliAsteroidCubeMap& CliAsteroidCubeMap(FCliAsteroid* Field)
{
    return *reinterpret_cast<FCliAsteroidCubeMap*>(Field->MapAt08);
}

void CliAsteroidCubeMapConstruct(FCliAsteroid* Field)
{
    // 0x00520201..0x0052021a: the allocator bytes (+0x08, +0x09), the byte at
    // +0x14 and the routine's head node -- one std::map here.
    new (Field->MapAt08) FCliAsteroidCubeMap();
}

void CliAsteroidCubeMapDestruct(FCliAsteroid* Field)
{
    // 0x005207f6..0x00520834: the routine (erase(begin, end); each pair's
    // destructor the routine runs the value's the routine), then the head node and the
    // allocator block freed inline (the routine's body; the routine itself is the EH unwind
    // copy, XREFs 0x005bff96 / 0x005bfff6).
    CliAsteroidCubeMap(Field).~FCliAsteroidCubeMap();
}
