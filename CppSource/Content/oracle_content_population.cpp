// Author: Giperion.

// Oracle instrumentation (design/migration-plan.md, Phase 0; work package P3.6d): the
// populated scenario of `Oracle.exe sim` (`population <ship>`). Only the harness calls this
// export, so a game run never reaches it.
//
// OracleSimPopulate makes the scenario ship a Content player's ship the way GameSupport's
// launch does (FGameSupport::OnPlayerLaunch, GameSupport.cpp:408-446): a fresh player state
// (ReplaceContentPlayerState) in the system's record (OnSystemChange, FSystemInfo::AddPlayer),
// and a player ship of the ship's archetype with its mission properties and its fighter or
// freighter class, whose constructor submits the PLAYER state graph. Then it creates SpacePop
// (pub::Controller::Create, its 3 s heartbeat draws one rand()) and sends it the launch
// (SpaceEnter {system, player, 0, -1}), whose arrival population is the first of the run. The
// server's ElapseTime then runs SpacePop's heartbeat every 3 s.
//
// OracleSimPopulationShips (reporting only) walks the system record's NPC ships in list order
// (FSystemInfo::SpaceObjs, which CreateNpcShip appends to), so that the harness can name the
// ships the population spawns by their spawn order and record them beside the scenario's.

#include "ContentInternal.h"
#include "ContentPlayerState.h"   // ReplaceContentPlayerState, FContentPlayerShip
#include "ContentStaticIds.h"     // GIdFighter, GIdFreighter
#include "NpcShip.h"              // FContentNpcShip::Loadout
#include "RandomMissionDB.h"      // GetShipMissionProperties
#include "SystemInfo.h"           // FindSystemInfo, FSystemInfo::AddPlayer
#include "common_server.h"
#include "common_server_controller.h"

#include <algorithm>

// Returns SpacePop's controller id, 0 when the system has no record.
extern "C" __declspec(dllexport) unsigned int __cdecl OracleSimPopulate(unsigned int PlayerId,
                                                                        unsigned int SystemId,
                                                                        unsigned int ShipId,
                                                                        unsigned int ShipArchId)
{
    FSystemInfo* System = FindSystemInfo(&SystemId);
    if (System == nullptr)
        return 0;

    FContentPlayerState* State = ReplaceContentPlayerState(PlayerId);
    State->OnSystemChange(SystemId);
    System->AddPlayer(State);

    FContentPlayerShipParams Params;
    Params.ShipId = ShipId;
    Params.ShipArchId = ShipArchId;
    GetShipMissionProperties(ShipArchId, &Params.ClassIds);
    if (Params.ReputationHandle != State)
    {
        if (Params.ReputationHandle != nullptr)
            Params.ReputationHandle->Release();
        Params.ReputationHandle = State;
        State->AddRef();
    }
    Params.System = State->GetSystemRecord();
    unsigned int Type = 0;
    pub::SpaceObj::GetType(ShipId, Type);
    unsigned int const* ClassId = nullptr;
    if ((Type & 0x10000) != 0)
        ClassId = &GIdFighter;
    else if ((Type & 0x20000) != 0)
        ClassId = &GIdFreighter;
    if (ClassId != nullptr &&
        std::find(Params.ClassIds.begin(), Params.ClassIds.end(), *ClassId) == Params.ClassIds.end())
    {
        Params.ClassIds.insert(Params.ClassIds.end(), *ClassId);
    }
    State->SetShipObject(new FContentPlayerShip(&Params));

    unsigned int const SpacePop = pub::Controller::Create(
        "Content.dll", "SpacePop", nullptr, pub::Controller::NORMAL);
    unsigned int Launch[4] = {SystemId, PlayerId, 0u, 0xffffffffu};
    pub::Controller::_SendMessage(SpacePop, pub::Controller::EMessage::SpaceEnter, Launch);
    return SpacePop;
}

// One NPC ship of the record: its space object id and its loadout (FContentNpcShip +0x1c4).
typedef void(__cdecl* FOracleSimPopulationShipFn)(void* Context, unsigned int SpaceObjId,
                                                   unsigned int Loadout);

// Calls Fn for every object of the system record's SpaceObjs, in list order, taken as an NPC
// ship as FSystemInfo's own walks take them (SetNpcShipsField1b1). Returns the count, -1 when
// the system has no record.
extern "C" __declspec(dllexport) int __cdecl OracleSimPopulationShips(unsigned int SystemId,
                                                                      FOracleSimPopulationShipFn Fn,
                                                                      void* Context)
{
    FSystemInfo* System = FindSystemInfo(&SystemId);
    if (System == nullptr || Fn == nullptr)
        return -1;
    int Count = 0;
    for (std::list<FContentSpaceObj*>::iterator It = System->SpaceObjs.begin(); It != System->SpaceObjs.end();
         ++It)
    {
        FContentNpcShip const* const Ship = static_cast<FContentNpcShip const*>(*It);
        Fn(Context, Ship->SpaceObjId, Ship->Loadout);
        ++Count;
    }
    return Count;
}
