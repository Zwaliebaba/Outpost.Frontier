// Author: Giperion.

// Oracle instrumentation (design/migration-plan.md, Phase 0; work package P3.6c): the NPC
// ships of `Oracle.exe sim`'s `content` scenario objects. Only the harness calls this export,
// so a game run never reaches it.
//
// OracleSimCreateNpcShip makes the ship the way Content's spawners do:
// FSystemInfo::CreateNpcShip with a parameter block of the ship's archetype, loadout, place,
// hit points, state graph and personality, owned by SpaceSupport, with no directive, no guide and no dock
// target. The block's reputation handle holds the vibe the harness made for the ship's
// `faction` (it frees nothing; the harness owns the vibe). FContentNpcShip's constructor then
// creates the space object and submits its state with the personality and the ship as both its
// directive and its content callback, and CreateNpcShip announces the ship (NpcShipCreated). With
// bFlee the ship gets a flee position (FContentNpcShip::SetFleePosition), which it answers
// request_flee_destination with.

#include "ContentInternal.h"      // GSpaceSupportControllerId
#include "ContentPlayerState.h"   // FContentPlayerStateBase
#include "NpcShip.h"              // FContentNpcShip, FContentNpcShipParams
#include "SystemInfo.h"           // FindSystemInfo, FSystemInfo::CreateNpcShip
#include "common_pub.h"           // pub::AI::Personality

namespace
{
    // A reputation handle around a vibe the harness made.
    struct FOracleNpcReputation : public FContentPlayerStateBase
    {
        explicit FOracleNpcReputation(int Vibe)
        {
            Reputation = Vibe;
        }
    };
}

// Returns the ship's space object id, 0 when the system has no record or the ship was not made.
extern "C" __declspec(dllexport) unsigned int __cdecl OracleSimCreateNpcShip(
    unsigned int SystemId, unsigned int ShipArchId, unsigned int LoadoutId, Vector const* Position,
    Matrix const* Orientation, int HitPoints, int StateGraphId,
    pub::AI::Personality const* Personality, int Vibe, bool bFlee, Vector const* FleePosition,
    float FleeDistance)
{
    FSystemInfo* System = FindSystemInfo(&SystemId);
    if (System == nullptr)
        return 0;

    FContentNpcShipParams Params;
    // The block holds the handle's one reference (released by the block's destructor).
    Params.ReputationHandle = new FOracleNpcReputation(Vibe);
    Params.ShipArchId = ShipArchId;
    Params.Loadout = LoadoutId;
    Params.Position = *Position;
    Params.Orientation = *Orientation;
    Params.HitPoints = HitPoints;
    Params.StateGraph = StateGraphId;
    Params.Personality = *Personality;
    Params.ControllerId = GSpaceSupportControllerId;

    FContentNpcShip* Ship = nullptr;
    System->CreateNpcShip(&Params, 0, &Ship);
    if (Ship == nullptr)
        return 0;
    if (bFlee)
        Ship->SetFleePosition(FleePosition, &FleeDistance);
    return Ship->SpaceObjId;
}
