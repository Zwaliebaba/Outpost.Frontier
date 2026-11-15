// Author: Giperion.

#pragma once

// OutpostFrontier.exe explosion.cpp -- baseline Client\space\explosion.cpp,
// the Action_Explode IAction a Gameobj's action list runs while its
// [Explosion] archetype plays out (effects on their delays, the innards debris, the death, the
// shatter into Debris gameobjs), plus the impulse and camera flash it applies when it starts.

//   Action_Explode : IAction   sizeof 0x28 (IAction::operator new(0x28) at 0x005361fc)
//   ??_7Action_Explode@@6B@ module storage = {the branch (IAction::get_id thunk),
// update, the routine dtor}

// ExplosionArchAddRef / ExplosionArchRelease (the routine / the routine, the same file) live
// in client_gameobj.cpp.

#include "common_archetype.h"
#include "common_cequip.h" // IAction
#include "common_csys.h"   // Vector

#include <vector>

class FClientGameobj;
struct CObject;
struct FDebrisArch;

class Action_Explode : public IAction
{
public:
    // (cdecl): IAction::operator new(0x28), the fields, the lifetime roll, the
    // impulse on the player and on every Gameobj of ObjDB kinds 10 and 9, the damage flash on
    // the player's HUD, then update(0). Callers: GameObject slot 108 (0x0053a8c3), Debris
    // slot 108 (0x005305dd), EquippedObj the routine (0x00533dbb).
    static Action_Explode* Create(Archetype::Explosion* Arch, FClientGameobj* Gameobj,
                                  Vector const* Pos);

    // (RET 4). False once the explosion is over (the action list drops it).
    bool update(float Dt) override;
    // IAction's destructor, then IAction::operator delete when bit 0 is set.
    ~Action_Explode() override;

private:
    // , the constructor part of the routine.
    Action_Explode(Archetype::Explosion* InArch, FClientGameobj* InGameobj, Vector const& InPos);

    // The two tick tests the routine spells out inline (see the .cpp): the lifetime ran out
    // during this tick; a delay (>= 0: from the start; < 0 or NaN: the lifetime running out)
    // was reached during this tick.
    bool LifetimeEndedThisTick() const;
    bool DelayReachedThisTick(float Delay) const;

    // (RET 4): push Target's CSimple away from Pos when its centre of mass is
    // inside the explosion radius.
    void ApplyImpulse(FClientGameobj* Target);
    // split the Gameobj's engine instance tree into pieces and fly each one off
    // as a Debris gameobj (process = shatter).
    void Shatter();
    // (RET 0xc): collect the pieces of Instance's subtree, at most MaxPieces.
    void CollectChildPieces(long Instance, int MaxPieces, std::vector<CObject*>& Pieces);
    // throw innards_debris_num Debris gameobjs out of the Gameobj.
    void SpawnInnardsDebris();

    Archetype::Explosion* Arch;   // +0x08
    FClientGameobj* Gameobj;      // +0x0c
    Vector Pos;                   // +0x10
    float Lifetime;               // +0x1c  lerp(Arch->Lifetime, Arch->LifetimeMax, rand)
    float Elapsed;                // +0x20  -1.0f until the first update
    float PrevElapsed;            // +0x24  -1.0f until the second update
};

// (cdecl) -- a debris_type drawn by weight (rand()/32768 against the running sum
// of the counts), looked up in the [Debris] table; the default record (the routine) when the
// draw falls past the end or the name is not in the table (FDUMP warning). Callers: the
// shatter (0x0053688f) and the innards (0x005370bc).
FDebrisArch* PickExplosionDebrisArch(std::vector<Archetype::FExplosionDebrisType> const* Types);
