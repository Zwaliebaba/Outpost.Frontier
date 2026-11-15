// Author: Giperion.

#pragma once

// baseline Client\Space\CliAsteroid.cpp -- [DynamicAsteroids]: the manager
// record a field keeps per section (FCliDynamicAsteroid, cli_asteroid.h, 0xe8) and the
// DynamicAsteroid gameobjs it scatters, fades and recycles around the camera.

//   DynamicAsteroid : GameObject   ctor inlined in the routine (0x0052668a..0x005266d4)  0xc8
//                                  IObjRW 005dccdc (112 slots), IObjAffect 005dcc5c (GameObject's
//                                  entries), CBase 005dcc50 -> the routine (body the routine)
// RTTI .?AVDynamicAsteroid@CliAsteroid@@. Slots other than 34 (get_type), 90, 91, 101, 108 and
// the new 111 are GameObject's (vtable 005de7ec).

#include "cli_asteroid.h"
#include "client_gameobj.h"
#include "common_id.h"

struct FSpaceCamera;

class FCliDynamicAsteroidGameobj : public FClientGameobj
{
public:
    // the routine(9), the owner into +0xa8, the companion (+0xb8..
    // +0xc4) zeroed, DynamicAsteroid's vtables, both flags cleared, Object and +0xb4 zeroed.
    explicit FCliDynamicAsteroidGameobj(FCliDynamicAsteroid* Owner);
    // (this = the CBase sub-object): the companion's particle released
    // (the routine), sound released (vt+8), light released (the routine), +0xb8 zeroed, then
    // GameObject's the routine; deleting wrapper the routine.
    ~FCliDynamicAsteroidGameobj() override;

    // IObjInspect 34 +0x88 the branch (RET 4): *OutType = 0x20000000, returns 0.
    int get_type(uint& OutType) const override;

    // 90 +0x168 the routine (RET 4): the routine(0x4001, Arch), the object's vt+0x4c
    // open(Arch) (no narrowing, no null test), +0xb4 = the archetype's nickname (+0x08).
    void create(Archetype::Root* Arch) override;
    // 91 +0x16c the routine: XOR AL,AL / RET 4 (Beam's and Debris's body).
    bool find(unsigned const& Id) override;
    // 101 +0x194 the branch (RET 0x10): Other->Slot1a0(A, D, this, SUBOBJ_ID_NONE); then an
    // active asteroid hit by a munition ((class & 7) == 7) explodes at A and, when the shot is
    // the player's and the owner's zone is lootable, may drop loot (IServer::MineAsteroid).
    void Slot194(void const* A, void const* B, FClientGameobj* Other, void const* D) override;
    // 108 +0x1b0 the branch (RET 4): deactivate, collisions off, the companion's effect
    // dropped, then the explosion (process must be 2) at Pos or get_position().
    void Slot1b0(Vector const* Pos) override;
    // 111 +0x1bc the branch (RET 0x14): MedAsteroid's body (CliAsteroidSlot1bcBody).
    virtual unsigned short* Slot1bc(unsigned short* Out, unsigned A, unsigned B, unsigned C,
                                    unsigned D);

    // (thiscall, RET 0xc) -- beam the object to Pos with an identity orientation
    // and give it a random velocity (up to MaxVelocity per axis, square-biased) and a random
    // angular velocity (up to MaxAngularVelocity); the particle's intensity starts at 0.
    // Caller: the routine (0x00525fe7).
    void Place(Vector const* Pos, float MaxVelocity, float MaxAngularVelocity);

    // +0xa8 -- the owning manager. baseline stores the dword over GameObject's bWatchListed byte
    // and its three pad bytes (0x00526693); the port keeps that storage.
    FCliDynamicAsteroid* GetOwner() const;
    void SetOwner(FCliDynamicAsteroid* Owner);

    // ---- data ---------------------------------------------------------------------------
    unsigned char bActive;   // +0xac  the routine sets it when placed; slot 108 clears it
    unsigned char bPlaced;   // +0xad  inside the placement sphere since the last placement
    unsigned char Padae[2];
    float Alpha;             // +0xb0  the routine's fade, the draw's material alpha
    ID_String Nickname;      // +0xb4  slot 90: the archetype's nickname
    void* Companion[4];      // +0xb8  effect companion {arch, ISoundSource*, particle, light}
};
static_assert(offsetof(FCliDynamicAsteroidGameobj, bActive) == 0xac, "DynamicAsteroid +0xac");
static_assert(offsetof(FCliDynamicAsteroidGameobj, Alpha) == 0xb0, "DynamicAsteroid +0xb0");
static_assert(offsetof(FCliDynamicAsteroidGameobj, Nickname) == 0xb4, "DynamicAsteroid +0xb4");
static_assert(offsetof(FCliDynamicAsteroidGameobj, Companion) == 0xb8, "DynamicAsteroid +0xb8");
static_assert(sizeof(FCliDynamicAsteroidGameobj) == 0xc8,
              "DynamicAsteroid sizeof 0xc8 (operator new(0xc8))");

// (thiscall, RET 4) -- the manager constructor over raw operator new(0xe8)
// memory: only the fields it names are written. Caller: the routine (0x005210bf).
FCliDynamicAsteroid* CliDynamicAsteroidConstruct(FCliDynamicAsteroid* This,
                                                 CmnAsteroid::CAsteroidField* CmnField);
// (thiscall) -- release the archetype, the slots (slot 87 with true), the slot
// array, the material and the mesh copy. The record itself is freed by the caller. Callers:
// (0x005206ea) and the routine (0x005209a8).
void CliDynamicAsteroidDestroy(FCliDynamicAsteroid* This);
// (thiscall, RET 4) -- the [DynamicAsteroids] keys, the slot array rebuilt, the
// archetype's mesh copied (IMesh / IRenderableMesh), then the vertex source descriptor
// (the routine). Caller: the routine (0x0052112b).
void CliDynamicAsteroidParseIni(FCliDynamicAsteroid* This, INI_Reader& Reader);
// (thiscall) -- every live slot gets slot 87 (+0x15c) with true and is cleared.
// Callers: the routine (0x005259a0) and the routine.
void CliDynamicAsteroidReleaseSlots(FCliDynamicAsteroid* This);
// (thiscall, RET 0xc) -- the per-frame spawn / recycle / fade of the slots
// around a point ahead of the camera. Caller: the routine (0x005223a6).
void CliDynamicAsteroidUpdate(FCliDynamicAsteroid* This, FSpaceCamera* Cam, float Dt,
                              Exclusion::AsteroidExclusionZoneMgr* ExclusionMgr);
// (thiscall, RET 0xc) -- the draw: the mesh copy into a locked vertex buffer,
// then every visible slot with its alpha, the field's ambient pass run once (*bAmbientDone).
// Caller: the routine (0x005226e9).
void CliDynamicAsteroidDraw(FCliDynamicAsteroid* This, FSpaceCamera* Cam,
                            unsigned char* bAmbientDone, FCliAsteroid* Field);
// (thiscall) -- a new DynamicAsteroid gameobj for this manager: created from
// the archetype, init'ed in the active system, collisions off, slot 92, into the ObjDB.
// Caller: the routine (0x00525eec).
FCliDynamicAsteroidGameobj* CliDynamicAsteroidSpawn(FCliDynamicAsteroid* This);
