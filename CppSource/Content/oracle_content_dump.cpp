// Oracle instrumentation (P3.5a, decision record 0008 point 10 and its Verification table):
// Content.dll's mission scripts as the parser leaves them, for the Rust comparison
// (crates/game-logic/tests/mission_scripts.rs). A new export, not part of the reference DLL's
// interface: only Oracle.exe sim calls it, for the session file
// CppSource/Oracle/mission_scripts.txt (`dump mission_scripts`), right after the multiplayer
// start-up, so a game run never reaches it.
//
// For each of the fourteen story missions (Mission_01a, Mission_01b, Mission_02 to 13), in that
// order: the mission dialogs are emptied, the CRT generator is seeded with OracleMissionSeed,
// the mission is made through CreateInstance for a synthetic player 7 (registered with
// ReplaceContentPlayerState, in he01) on a controller id no controller has (so the
// constructors' SetHeartbeatInterval finds none and draws nothing), its draws are counted, its
// objects written, and it is destroyed (DestroyInstance). Player 7 is removed at the end.
//
// Every file is `<Dir>\mission_<table>.txt`, one record per line, fields `key=value` separated
// by one space: ids and unsigned values decimal, signed values decimal with their sign, flags
// as the byte's value, floats as the eight lower-case hex digits of their bit pattern, strings
// as the hex of their bytes; vectors, matrices (row by row), transforms (rotation, then
// translation) and quaternions (W, X, Y, Z) as their floats joined by `,`; a costume as
// `head,body,left,right/count/accessories`; lists joined by `,` (design/sim-records.md).
//
//   mission_scripts.txt          one line per mission: its draws, the script's and the mission
//                                object's values and the counts of its tables;
//   mission_triggers.txt         one line per trigger, in id order, with its condition;
//   mission_actions.txt          one line per action of each trigger;
//   mission_ship_groups.txt      one line per [MsnShip] / [MsnFormation] record;
//   mission_ships.txt            one line per ship slot of each record;
//   mission_solars.txt           one line per [MsnSolar];
//   mission_loot.txt             one line per [MsnLoot];
//   mission_rand_encs.txt        one line per [MsnRandEnc] with its parameters;
//   mission_npcs.txt             one line per [NPC] record, in key order;
//   mission_npc_ship_archs.txt   the archetypes of the [Mission] npc_ship_file;
//   mission_objectives.txt       the [NNObjective] objectives of each player record;
//   mission_dialogs.txt          one line per dialog in GMissionDialogs after the mission;
//   mission_dialog_lines.txt     one line per line of each dialog's speakers;
//   mission_costumes.txt         the script's dialog costumes.

#include "content.h"
#include "ContentPlayerState.h"   // ReplaceContentPlayerState, RemoveContentPlayerState
#include "MissionAct.h"
#include "MissionCnd.h"
#include "MissionDialogs.h"       // GMissionDialogs
#include "MissionObject.h"
#include "MissionScript.h"
#include "NpcShips.h"             // FNpcShipArch
#include "RandomEncounters.h"
#include "SystemInfo.h"           // FindSystemInfo
#include "common_goods.h"         // EquipDesc, EquipDescVector
#include "common_server.h"        // pub::GetSystemID

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <map>
#include <string>
#include <vector>

namespace
{
    // The seed of every mission's parse (the harness's default seed).
    unsigned int const OracleMissionSeed = 0x4f524143u;

    // The synthetic player and the first controller id (no controller has these ids).
    unsigned int const OraclePlayerId = 7u;
    unsigned int const OracleControllerBase = 0x00ff0001u;

    unsigned int OracleBits(float Value)
    {
        unsigned int Bits = 0;
        std::memcpy(&Bits, &Value, sizeof(Bits));
        return Bits;
    }

    void OracleHex(FILE* Out, char const* Data)
    {
        if (Data == nullptr)
            return;
        for (size_t Index = 0; Data[Index] != '\0'; ++Index)
            std::fprintf(Out, "%02x", static_cast<unsigned char>(Data[Index]));
    }

    void OU(FILE* Out, char const* Name, unsigned int Value)
    {
        std::fprintf(Out, " %s=%u", Name, Value);
    }

    void OI(FILE* Out, char const* Name, int Value)
    {
        std::fprintf(Out, " %s=%d", Name, Value);
    }

    void OF(FILE* Out, char const* Name, float Value)
    {
        std::fprintf(Out, " %s=%08x", Name, OracleBits(Value));
    }

    // A float kept in an unsigned field (pub::Audio::Tryptich::Field14).
    void OFB(FILE* Out, char const* Name, unsigned int Bits)
    {
        std::fprintf(Out, " %s=%08x", Name, Bits);
    }

    template <unsigned N>
    void OS(FILE* Out, char const* Name, TString<N> const& Value)
    {
        std::fprintf(Out, " %s=", Name);
        OracleHex(Out, Value.Data);
    }

    void OFloats(FILE* Out, float const* Values, int Count)
    {
        for (int Index = 0; Index < Count; ++Index)
            std::fprintf(Out, "%s%08x", Index == 0 ? "" : ",", OracleBits(Values[Index]));
    }

    void OV3(FILE* Out, char const* Name, float X, float Y, float Z)
    {
        float const V[3] = {X, Y, Z};
        std::fprintf(Out, " %s=", Name);
        OFloats(Out, V, 3);
    }

    void OV(FILE* Out, char const* Name, Vector const& V)
    {
        OV3(Out, Name, V.x, V.y, V.z);
    }

    void OME(FILE* Out, char const* Name, Matrix const& M)
    {
        std::fprintf(Out, " %s=", Name);
        OFloats(Out, &M.E[0][0], 9);
    }

    void OMD(FILE* Out, char const* Name, FMatrix const& M)
    {
        std::fprintf(Out, " %s=", Name);
        OFloats(Out, &M.D[0][0], 9);
    }

    void OFT(FILE* Out, char const* Name, FTransform const& T)
    {
        std::fprintf(Out, " %s=", Name);
        OFloats(Out, &T.Rotation.D[0][0], 9);
        float const V[3] = {T.Translation.X, T.Translation.Y, T.Translation.Z};
        std::fprintf(Out, ",");
        OFloats(Out, V, 3);
    }

    void OT(FILE* Out, char const* Name, Transform const& T)
    {
        std::fprintf(Out, " %s=", Name);
        OFloats(Out, &T.Rotation.E[0][0], 9);
        float const V[3] = {T.Translation.x, T.Translation.y, T.Translation.z};
        std::fprintf(Out, ",");
        OFloats(Out, V, 3);
    }

    void OQ(FILE* Out, char const* Name, FQuaternion const& Q)
    {
        float const V[4] = {Q.W, Q.X, Q.Y, Q.Z};
        std::fprintf(Out, " %s=", Name);
        OFloats(Out, V, 4);
    }

    void OCostume(FILE* Out, Costume const& C)
    {
        std::fprintf(Out, "%u,%u,%u,%u/%u/", C.PartIds[0], C.PartIds[1], C.PartIds[2], C.PartIds[3],
                     C.AccessoryCount);
        unsigned int const Count = C.AccessoryCount < 8u ? C.AccessoryCount : 8u;
        for (unsigned int Index = 0; Index < Count; ++Index)
            std::fprintf(Out, "%s%u", Index == 0 ? "" : ",", C.AccessoryIds[Index]);
    }

    void OC(FILE* Out, char const* Name, Costume const& C)
    {
        std::fprintf(Out, " %s=", Name);
        OCostume(Out, C);
    }

    template <class TList>
    void OUL(FILE* Out, char const* Name, TList const& List)
    {
        std::fprintf(Out, " %s=", Name);
        bool bFirst = true;
        for (typename TList::const_iterator It = List.begin(); It != List.end(); ++It)
        {
            std::fprintf(Out, "%s%u", bFirst ? "" : ",", static_cast<unsigned int>(*It));
            bFirst = false;
        }
    }

    template <class TList>
    void OSL(FILE* Out, char const* Name, TList const& List)
    {
        std::fprintf(Out, " %s=", Name);
        bool bFirst = true;
        for (typename TList::const_iterator It = List.begin(); It != List.end(); ++It)
        {
            std::fprintf(Out, "%s", bFirst ? "" : ",");
            OracleHex(Out, It->Data);
            bFirst = false;
        }
    }

    void OPL(FILE* Out, char const* Name, std::vector<FTlrException> const& List)
    {
        std::fprintf(Out, " %s=", Name);
        for (size_t Index = 0; Index < List.size(); ++Index)
            std::fprintf(Out, "%s%u:%u", Index == 0 ? "" : ",", List[Index].Values[0], List[Index].Values[1]);
    }

    void OLO(FILE* Out, char const* Name, EquipDescVector const& V)
    {
        std::fprintf(Out, " %s=", Name);
        for (EquipDesc const* It = V.Begin; It != nullptr && It != V.End; ++It)
        {
            std::fprintf(Out, "%s%u:%u:%d:%08x:%u:%u:", It == V.Begin ? "" : ",",
                         static_cast<unsigned int>(It->Id), It->ArchId, It->Count, OracleBits(It->Health),
                         static_cast<unsigned int>(It->bEquipped), It->bMission ? 1u : 0u);
            OracleHex(Out, It->Hardpoint);
        }
    }

    void ONE(FILE* Out, char const* Name, std::list<FNpcSystemEntry> const& List)
    {
        std::fprintf(Out, " %s=", Name);
        bool bFirst = true;
        for (std::list<FNpcSystemEntry>::const_iterator It = List.begin(); It != List.end(); ++It)
        {
            std::fprintf(Out, "%s", bFirst ? "" : ",");
            OracleHex(Out, It->Label.Data);
            std::fprintf(Out, ":%u", It->SystemId);
            bFirst = false;
        }
    }

    void OU4(FILE* Out, char const* Name, unsigned int A, unsigned int B, unsigned int C, unsigned int D)
    {
        std::fprintf(Out, " %s=%u,%u,%u,%u", Name, A, B, C, D);
    }

    // One line per class, in the factory's order: the factory name, then every field.
    bool DumpCondition(FILE* Out, Cnd const* Object)
    {
        if (CndBaseEnter const* C = dynamic_cast<CndBaseEnter const*>(Object))
        {
            fputs("Cnd_BaseEnter", Out);
            OU(Out, "base_id", static_cast<unsigned int>(C->BaseId));
            return true;
        }
        if (CndBaseExit const* C = dynamic_cast<CndBaseExit const*>(Object))
        {
            fputs("Cnd_BaseExit", Out);
            OU(Out, "base_id", static_cast<unsigned int>(C->BaseId));
            return true;
        }
        if (CndCargoScanned const* C = dynamic_cast<CndCargoScanned const*>(Object))
        {
            fputs("Cnd_CargoScanned", Out);
            OS(Out, "scanner_label", C->ScannerLabel);
            OS(Out, "scanned_label", C->ScannedLabel);
            OU(Out, "field_0x74", static_cast<unsigned int>(C->Field_0x74));
            OU(Out, "field_0x78", static_cast<unsigned int>(C->Field_0x78));
            return true;
        }
        if (CndCharSelect const* C = dynamic_cast<CndCharSelect const*>(Object))
        {
            fputs("Cnd_CharSelect", Out);
            OU(Out, "location_id", static_cast<unsigned int>(C->LocationId));
            OS(Out, "character", C->Character);
            OU(Out, "base_id", static_cast<unsigned int>(C->BaseId));
            return true;
        }
        if (CndCommComplete const* C = dynamic_cast<CndCommComplete const*>(Object))
        {
            fputs("Cnd_CommComplete", Out);
            OS(Out, "source_label", C->SourceLabel);
            OU(Out, "comm_id", static_cast<unsigned int>(C->CommId));
            OU(Out, "source_obj_id", static_cast<unsigned int>(C->SourceObjId));
            return true;
        }
        if (CndCmpToPlane const* C = dynamic_cast<CndCmpToPlane const*>(Object))
        {
            fputs("Cnd_CmpToPlane", Out);
            OS(Out, "object_label", C->ObjectLabel);
            OF(Out, "plane_a", C->PlaneA);
            OF(Out, "plane_b", C->PlaneB);
            OF(Out, "plane_c", C->PlaneC);
            OF(Out, "plane_d", C->PlaneD);
            OI(Out, "side", static_cast<int>(C->Side));
            OU(Out, "object_id", static_cast<unsigned int>(C->ObjectId));
            return true;
        }
        if (CndDestroyed const* C = dynamic_cast<CndDestroyed const*>(Object))
        {
            fputs("Cnd_Destroyed", Out);
            OI(Out, "count", static_cast<int>(C->Count));
            OI(Out, "needed", static_cast<int>(C->Needed));
            OU(Out, "label", static_cast<unsigned int>(C->Label));
            OS(Out, "killer_label", C->KillerLabel);
            OU(Out, "killer_obj_id", static_cast<unsigned int>(C->KillerObjId));
            OI(Out, "mode", static_cast<int>(C->Mode));
            return true;
        }
        if (CndDistCircle const* C = dynamic_cast<CndDistCircle const*>(Object))
        {
            fputs("Cnd_DistCircle", Out);
            OS(Out, "object_label", C->ObjectLabel);
            OS(Out, "circle_obj", C->CircleObj);
            OU(Out, "circle_obj_id", static_cast<unsigned int>(C->CircleObjId));
            OV(Out, "last_position", C->LastPosition);
            OV(Out, "circle_center", C->CircleCenter);
            OV(Out, "normal", C->Normal);
            OF(Out, "radius", C->Radius);
            OF(Out, "plane_offset", C->PlaneOffset);
            return true;
        }
        if (CndDistShip const* C = dynamic_cast<CndDistShip const*>(Object))
        {
            fputs("Cnd_DistShip", Out);
            OS(Out, "object_a", C->ObjectA);
            OS(Out, "object_b", C->ObjectB);
            OF(Out, "distance_sq", C->DistanceSq);
            OU(Out, "inside", static_cast<unsigned int>(C->bInside));
            OF(Out, "tick_timer", C->TickTimer);
            OF(Out, "tick_interval", C->TickInterval);
            OI(Out, "tick_mode", static_cast<int>(C->TickMode));
            OF(Out, "last_distance_sq", C->LastDistanceSq);
            OU(Out, "object_a_id", static_cast<unsigned int>(C->ObjectAId));
            OU(Out, "object_b_id", static_cast<unsigned int>(C->ObjectBId));
            return true;
        }
        if (CndDistVec const* C = dynamic_cast<CndDistVec const*>(Object))
        {
            fputs("Cnd_DistVec", Out);
            OV3(Out, "position", C->PositionX, C->PositionY, C->PositionZ);
            OS(Out, "object_label", C->ObjectLabel);
            OF(Out, "distance_sq", C->DistanceSq);
            OU(Out, "inside", static_cast<unsigned int>(C->bInside));
            OF(Out, "tick_timer", C->TickTimer);
            OF(Out, "tick_interval", C->TickInterval);
            OI(Out, "tick_mode", static_cast<int>(C->TickMode));
            OF(Out, "last_distance_sq", C->LastDistanceSq);
            OU(Out, "object_id", static_cast<unsigned int>(C->ObjectId));
            return true;
        }
        if (CndDistVecLbl const* C = dynamic_cast<CndDistVecLbl const*>(Object))
        {
            fputs("Cnd_DistVecLbl", Out);
            OV3(Out, "position", C->PositionX, C->PositionY, C->PositionZ);
            OU(Out, "label", static_cast<unsigned int>(C->Label));
            OF(Out, "distance_sq", C->DistanceSq);
            OU(Out, "inside", static_cast<unsigned int>(C->bInside));
            OU(Out, "any", static_cast<unsigned int>(C->bAny));
            OF(Out, "tick_timer", C->TickTimer);
            OF(Out, "tick_interval", C->TickInterval);
            return true;
        }
        if (CndEncLaunched const* C = dynamic_cast<CndEncLaunched const*>(Object))
        {
            fputs("Cnd_EncLaunched", Out);
            OU(Out, "encounter_id", static_cast<unsigned int>(C->EncounterId));
            return true;
        }
        if (CndHasMsn const* C = dynamic_cast<CndHasMsn const*>(Object))
        {
            fputs("Cnd_HasMsn", Out);
            OI(Out, "wanted", static_cast<int>(C->Wanted));
            return true;
        }
        if (CndHealthDec const* C = dynamic_cast<CndHealthDec const*>(Object))
        {
            fputs("Cnd_HealthDec", Out);
            OS(Out, "object_label", C->ObjectLabel);
            OS(Out, "attacker_label", C->AttackerLabel);
            OF(Out, "health", C->Health);
            OU(Out, "object_id", static_cast<unsigned int>(C->ObjectId));
            OU(Out, "attacker_id", static_cast<unsigned int>(C->AttackerId));
            return true;
        }
        if (CndInSpace const* C = dynamic_cast<CndInSpace const*>(Object))
        {
            fputs("Cnd_InSpace", Out);
            OU(Out, "in_space", static_cast<unsigned int>(C->bInSpace));
            return true;
        }
        if (CndInTradelane const* C = dynamic_cast<CndInTradelane const*>(Object))
        {
            fputs("Cnd_InTradelane", Out);
            OS(Out, "object_label", C->ObjectLabel);
            OU(Out, "in_lane", static_cast<unsigned int>(C->bInLane));
            return true;
        }
        if (CndInZone const* C = dynamic_cast<CndInZone const*>(Object))
        {
            fputs("Cnd_InZone", Out);
            OU(Out, "in_zone", static_cast<unsigned int>(C->bInZone));
            OU(Out, "zone_flags", static_cast<unsigned int>(C->ZoneFlags));
            OS(Out, "object_label", C->ObjectLabel);
            OU(Out, "object_id", static_cast<unsigned int>(C->ObjectId));
            return true;
        }
        if (CndJumpgateAct const* C = dynamic_cast<CndJumpgateAct const*>(Object))
        {
            fputs("Cnd_JumpgateAct", Out);
            OS(Out, "jumpgate", C->Jumpgate);
            OU(Out, "gate_id", static_cast<unsigned int>(C->GateId));
            return true;
        }
        if (CndJumpInComplete const* C = dynamic_cast<CndJumpInComplete const*>(Object))
        {
            fputs("Cnd_JumpInComplete", Out);
            OU(Out, "system_id", static_cast<unsigned int>(C->SystemId));
            return true;
        }
        if (CndLaunchComplete const* C = dynamic_cast<CndLaunchComplete const*>(Object))
        {
            fputs("Cnd_LaunchComplete", Out);
            OS(Out, "base_nickname", C->BaseNickname);
            OS(Out, "ship_label", C->ShipLabel);
            OU(Out, "base_id", static_cast<unsigned int>(C->BaseId));
            OU(Out, "ship_id", static_cast<unsigned int>(C->ShipId));
            return true;
        }
        if (CndLocEnter const* C = dynamic_cast<CndLocEnter const*>(Object))
        {
            fputs("Cnd_LocEnter", Out);
            OU(Out, "location_id", static_cast<unsigned int>(C->LocationId));
            OU(Out, "base_id", static_cast<unsigned int>(C->BaseId));
            return true;
        }
        if (CndLocExit const* C = dynamic_cast<CndLocExit const*>(Object))
        {
            fputs("Cnd_LocExit", Out);
            OU(Out, "location_id", static_cast<unsigned int>(C->LocationId));
            OU(Out, "base_id", static_cast<unsigned int>(C->BaseId));
            return true;
        }
        if (CndLootAcquired const* C = dynamic_cast<CndLootAcquired const*>(Object))
        {
            fputs("Cnd_LootAcquired", Out);
            OU(Out, "loot_label", static_cast<unsigned int>(C->LootLabel));
            OS(Out, "ship_label", C->ShipLabel);
            OI(Out, "count", static_cast<int>(C->Count));
            OI(Out, "needed", static_cast<int>(C->Needed));
            return true;
        }
        if (CndMsnResponse const* C = dynamic_cast<CndMsnResponse const*>(Object))
        {
            fputs("Cnd_MsnResponse", Out);
            OI(Out, "response", static_cast<int>(C->Response));
            return true;
        }
        if (CndNpcSystemEnter const* C = dynamic_cast<CndNpcSystemEnter const*>(Object))
        {
            fputs("Cnd_NPCSystemEnter", Out);
            ONE(Out, "ships", C->Ships);
            ONE(Out, "awaited", C->Awaited);
            ONE(Out, "pending", C->Pending);
            OU(Out, "system_id", static_cast<unsigned int>(C->SystemId));
            OU(Out, "any_system", static_cast<unsigned int>(C->bAnySystem));
            OU(Out, "delay_running", static_cast<unsigned int>(C->bDelayRunning));
            OF(Out, "delay_start", C->DelayStart);
            OF(Out, "delay_elapsed", C->DelayElapsed);
            OF(Out, "delay", C->Delay);
            OU(Out, "loaded_same_system", static_cast<unsigned int>(C->bLoadedSameSystem));
            return true;
        }
        if (CndNpcSystemExit const* C = dynamic_cast<CndNpcSystemExit const*>(Object))
        {
            fputs("Cnd_NPCSystemExit", Out);
            ONE(Out, "ships", C->Ships);
            ONE(Out, "awaited", C->Awaited);
            ONE(Out, "pending", C->Pending);
            OU(Out, "system_id", static_cast<unsigned int>(C->SystemId));
            OU(Out, "any_system", static_cast<unsigned int>(C->bAnySystem));
            return true;
        }
        if (CndPlayerLaunch const* C = dynamic_cast<CndPlayerLaunch const*>(Object))
        {
            fputs("Cnd_PlayerLaunch", Out);
            (void)C;
            return true;
        }
        if (CndPlayerManeuver const* C = dynamic_cast<CndPlayerManeuver const*>(Object))
        {
            fputs("Cnd_PlayerManeuver", Out);
            OI(Out, "behavior_id", static_cast<int>(C->BehaviorId));
            OS(Out, "target_label", C->TargetLabel);
            OU(Out, "inverse", static_cast<unsigned int>(C->bInverse));
            OU(Out, "target_id", static_cast<unsigned int>(C->TargetId));
            return true;
        }
        if (CndPopUpDialog const* C = dynamic_cast<CndPopUpDialog const*>(Object))
        {
            fputs("Cnd_PopUpDialog", Out);
            OU(Out, "button_mask", static_cast<unsigned int>(C->ButtonMask));
            return true;
        }
        if (CndProjHit const* C = dynamic_cast<CndProjHit const*>(Object))
        {
            fputs("Cnd_ProjHit", Out);
            OS(Out, "target_label", C->TargetLabel);
            OS(Out, "shooter_label", C->ShooterLabel);
            OI(Out, "needed", static_cast<int>(C->Needed));
            OU(Out, "target_id", static_cast<unsigned int>(C->TargetId));
            OU(Out, "shooter_id", static_cast<unsigned int>(C->ShooterId));
            OI(Out, "count", static_cast<int>(C->Count));
            return true;
        }
        if (CndProjHitShipToLbl const* C = dynamic_cast<CndProjHitShipToLbl const*>(Object))
        {
            fputs("Cnd_ProjHitShipToLbl", Out);
            OU(Out, "target_label", static_cast<unsigned int>(C->TargetLabel));
            OS(Out, "shooter_label", C->ShooterLabel);
            OI(Out, "needed", static_cast<int>(C->Needed));
            OU(Out, "shooter_id", static_cast<unsigned int>(C->ShooterId));
            OI(Out, "count", static_cast<int>(C->Count));
            return true;
        }
        if (CndRtcDone const* C = dynamic_cast<CndRtcDone const*>(Object))
        {
            fputs("Cnd_RTCDone", Out);
            OS(Out, "rtc_file", C->RtcFile);
            return true;
        }
        if (CndRumorHeard const* C = dynamic_cast<CndRumorHeard const*>(Object))
        {
            fputs("Cnd_RumorHeard", Out);
            OU(Out, "location_id", static_cast<unsigned int>(C->LocationId));
            OS(Out, "rumor", C->Rumor);
            OU(Out, "base_id", static_cast<unsigned int>(C->BaseId));
            return true;
        }
        if (CndSpaceEnter const* C = dynamic_cast<CndSpaceEnter const*>(Object))
        {
            fputs("Cnd_SpaceEnter", Out);
            OU(Out, "system_id", static_cast<unsigned int>(C->SystemId));
            return true;
        }
        if (CndSpaceExit const* C = dynamic_cast<CndSpaceExit const*>(Object))
        {
            fputs("Cnd_SpaceExit", Out);
            OU(Out, "system_id", static_cast<unsigned int>(C->SystemId));
            return true;
        }
        if (CndSystemEnter const* C = dynamic_cast<CndSystemEnter const*>(Object))
        {
            fputs("Cnd_SystemEnter", Out);
            OU(Out, "from_system_id", static_cast<unsigned int>(C->FromSystemId));
            OU(Out, "system_id", static_cast<unsigned int>(C->SystemId));
            return true;
        }
        if (CndSystemExit const* C = dynamic_cast<CndSystemExit const*>(Object))
        {
            fputs("Cnd_SystemExit", Out);
            OU(Out, "system_id", static_cast<unsigned int>(C->SystemId));
            OU(Out, "to_system_id", static_cast<unsigned int>(C->ToSystemId));
            return true;
        }
        if (CndTetherBroke const* C = dynamic_cast<CndTetherBroke const*>(Object))
        {
            fputs("Cnd_TetherBroke", Out);
            OS(Out, "ship_a", C->ShipA);
            OS(Out, "ship_b", C->ShipB);
            OU(Out, "ship_a_id", static_cast<unsigned int>(C->ShipAId));
            OU(Out, "ship_b_id", static_cast<unsigned int>(C->ShipBId));
            OU(Out, "field_0x7c", static_cast<unsigned int>(C->Field_0x7c));
            OF(Out, "distance_sq", C->DistanceSq);
            OF(Out, "field_0x84", C->Field_0x84);
            OF(Out, "field_0x88", C->Field_0x88);
            OF(Out, "field_0x8c", C->Field_0x8c);
            OF(Out, "field_0x90", C->Field_0x90);
            OF(Out, "field_0x94", C->Field_0x94);
            return true;
        }
        if (CndTimer const* C = dynamic_cast<CndTimer const*>(Object))
        {
            fputs("Cnd_Timer", Out);
            OF(Out, "elapsed", C->Elapsed);
            OF(Out, "duration", C->Duration);
            OU(Out, "run_on_base", static_cast<unsigned int>(C->bRunOnBase));
            return true;
        }
        if (CndTlEntered const* C = dynamic_cast<CndTlEntered const*>(Object))
        {
            fputs("Cnd_TLEntered", Out);
            OS(Out, "ship_label", C->ShipLabel);
            OS(Out, "ring_a", C->RingA);
            OS(Out, "ring_b", C->RingB);
            OU(Out, "ship_id", static_cast<unsigned int>(C->ShipId));
            OU(Out, "ring_a_id", static_cast<unsigned int>(C->RingAId));
            OU(Out, "ring_b_id", static_cast<unsigned int>(C->RingBId));
            return true;
        }
        if (CndTlExited const* C = dynamic_cast<CndTlExited const*>(Object))
        {
            fputs("Cnd_TLExited", Out);
            OS(Out, "ship_label", C->ShipLabel);
            OS(Out, "ring_a", C->RingA);
            OS(Out, "ring_b", C->RingB);
            OU(Out, "ship_id", static_cast<unsigned int>(C->ShipId));
            OU(Out, "ring_b_id", static_cast<unsigned int>(C->RingBId));
            OU(Out, "ring_a_id", static_cast<unsigned int>(C->RingAId));
            return true;
        }
        if (CndTrue const* C = dynamic_cast<CndTrue const*>(Object))
        {
            fputs("Cnd_True", Out);
            (void)C;
            return true;
        }
        if (CndWatchTrigger const* C = dynamic_cast<CndWatchTrigger const*>(Object))
        {
            fputs("Cnd_WatchTrigger", Out);
            OU(Out, "watched_trigger_id", static_cast<unsigned int>(C->WatchedTriggerId));
            OU(Out, "wanted_state", static_cast<unsigned int>(C->WantedState));
            return true;
        }
        if (CndWatchVibe const* C = dynamic_cast<CndWatchVibe const*>(Object))
        {
            fputs("Cnd_WatchVibe", Out);
            OI(Out, "compare", static_cast<int>(C->Compare));
            OS(Out, "object_a", C->ObjectA);
            OS(Out, "object_b", C->ObjectB);
            OU(Out, "object_a_id", static_cast<unsigned int>(C->ObjectAId));
            OU(Out, "object_b_id", static_cast<unsigned int>(C->ObjectBId));
            OF(Out, "value", C->Value);
            return true;
        }
        return false;
    }

    // One line per class, in the factory's order: the factory name, then every field.
    bool DumpAction(FILE* Out, Act const* Object)
    {
        if (ActActTrig const* C = dynamic_cast<ActActTrig const*>(Object))
        {
            fputs("Act_ActTrig", Out);
            OU(Out, "target_trigger_id", static_cast<unsigned int>(C->TargetTriggerId));
            return true;
        }
        if (ActAddAmbient const* C = dynamic_cast<ActAddAmbient const*>(Object))
        {
            fputs("Act_AddAmbient", Out);
            OS(Out, "path", C->Path);
            OI(Out, "handle", static_cast<int>(C->Handle));
            OU(Out, "room_id", static_cast<unsigned int>(C->RoomId));
            OU(Out, "base_id", static_cast<unsigned int>(C->BaseId));
            return true;
        }
        if (ActAddRtc const* C = dynamic_cast<ActAddRtc const*>(Object))
        {
            fputs("Act_AddRTC", Out);
            OS(Out, "path", C->Path);
            OU(Out, "repeatable", static_cast<unsigned int>(C->bRepeatable));
            return true;
        }
        if (ActAdjAcct const* C = dynamic_cast<ActAdjAcct const*>(Object))
        {
            fputs("Act_AdjAcct", Out);
            OI(Out, "amount", static_cast<int>(C->Amount));
            return true;
        }
        if (ActAdjHealth const* C = dynamic_cast<ActAdjHealth const*>(Object))
        {
            fputs("Act_AdjHealth", Out);
            OU(Out, "formation_id", static_cast<unsigned int>(C->FormationId));
            OF(Out, "delta", C->Delta);
            return true;
        }
        if (ActCallSceneScript const* C = dynamic_cast<ActCallSceneScript const*>(Object))
        {
            fputs("Act_CallSceneScript", Out);
            OS(Out, "target", C->Target);
            OS(Out, "script", C->Script);
            OV(Out, "position", C->Position);
            OI(Out, "mode", static_cast<int>(C->Mode));
            return true;
        }
        if (ActChangeState const* C = dynamic_cast<ActChangeState const*>(Object))
        {
            fputs("Act_ChangeState", Out);
            OI(Out, "state", static_cast<int>(C->State));
            OU(Out, "field_0x10", static_cast<unsigned int>(C->Field_0x10));
            return true;
        }
        if (ActCloak const* C = dynamic_cast<ActCloak const*>(Object))
        {
            fputs("Act_Cloak", Out);
            OS(Out, "formation", C->Formation);
            OU(Out, "cloak", static_cast<unsigned int>(C->bCloak));
            return true;
        }
        if (ActDeactTrig const* C = dynamic_cast<ActDeactTrig const*>(Object))
        {
            fputs("Act_DeactTrig", Out);
            OU(Out, "target_trigger_id", static_cast<unsigned int>(C->TargetTriggerId));
            return true;
        }
        if (ActDebugMsg const* C = dynamic_cast<ActDebugMsg const*>(Object))
        {
            fputs("Act_DebugMsg", Out);
            OS(Out, "text", C->Text);
            return true;
        }
        if (ActDestroy const* C = dynamic_cast<ActDestroy const*>(Object))
        {
            fputs("Act_Destroy", Out);
            OS(Out, "label", C->Label);
            OI(Out, "destroy_mode", static_cast<int>(C->DestroyMode));
            return true;
        }
        if (ActDisableEnc const* C = dynamic_cast<ActDisableEnc const*>(Object))
        {
            fputs("Act_DisableEnc", Out);
            OS(Out, "encounter", C->Encounter);
            return true;
        }
        if (ActDisableFriendlyFire const* C = dynamic_cast<ActDisableFriendlyFire const*>(Object))
        {
            fputs("Act_DisableFriendlyFire", Out);
            OSL(Out, "labels", C->Labels);
            return true;
        }
        if (ActDisableTradelane const* C = dynamic_cast<ActDisableTradelane const*>(Object))
        {
            fputs("Act_DisableTradelane", Out);
            OU(Out, "ring_id", static_cast<unsigned int>(C->RingId));
            return true;
        }
        if (ActDockRequest const* C = dynamic_cast<ActDockRequest const*>(Object))
        {
            fputs("Act_DockRequest", Out);
            OU(Out, "target_id", static_cast<unsigned int>(C->TargetId));
            OU(Out, "field_0x10", static_cast<unsigned int>(C->Field_0x10));
            return true;
        }
        if (ActEnableEnc const* C = dynamic_cast<ActEnableEnc const*>(Object))
        {
            fputs("Act_EnableEnc", Out);
            OS(Out, "encounter", C->Encounter);
            return true;
        }
        if (ActEnableManeuver const* C = dynamic_cast<ActEnableManeuver const*>(Object))
        {
            fputs("Act_EnableManeuver", Out);
            OI(Out, "maneuver_id", static_cast<int>(C->ManeuverId));
            OU(Out, "enable", static_cast<unsigned int>(C->bEnable));
            return true;
        }
        if (ActEtherComm const* C = dynamic_cast<ActEtherComm const*>(Object))
        {
            fputs("Act_EtherComm", Out);
            OS(Out, "receiver", C->Receiver);
            OC(Out, "speaker_costume", C->SpeakerCostume);
            OU(Out, "voice_id", static_cast<unsigned int>(C->VoiceId));
            OU(Out, "comm_type", static_cast<unsigned int>(C->CommType));
            OU(Out, "text_id", static_cast<unsigned int>(C->TextId));
            OI(Out, "duration", static_cast<int>(C->Duration));
            return true;
        }
        if (ActForceLand const* C = dynamic_cast<ActForceLand const*>(Object))
        {
            fputs("Act_ForceLand", Out);
            OU(Out, "base_id", static_cast<unsigned int>(C->BaseId));
            return true;
        }
        if (ActGcsClamp const* C = dynamic_cast<ActGcsClamp const*>(Object))
        {
            fputs("Act_GCSClamp", Out);
            OU(Out, "clamp", static_cast<unsigned int>(C->bClamp));
            return true;
        }
        if (ActGiveMb const* C = dynamic_cast<ActGiveMb const*>(Object))
        {
            fputs("Act_GiveMB", Out);
            OS(Out, "npc", C->Npc);
            OU(Out, "list_id", static_cast<unsigned int>(C->ListId));
            return true;
        }
        if (ActGiveNnObjs const* C = dynamic_cast<ActGiveNnObjs const*>(Object))
        {
            fputs("Act_GiveNNObjs", Out);
            (void)C;
            return true;
        }
        if (ActGiveObjList const* C = dynamic_cast<ActGiveObjList const*>(Object))
        {
            fputs("Act_GiveObjList", Out);
            OS(Out, "npc", C->Npc);
            OU(Out, "list_id", static_cast<unsigned int>(C->ListId));
            return true;
        }
        if (ActHostileClamp const* C = dynamic_cast<ActHostileClamp const*>(Object))
        {
            fputs("Act_HostileClamp", Out);
            OU(Out, "clamp", static_cast<unsigned int>(C->bClamp));
            return true;
        }
        if (ActInvulnerable const* C = dynamic_cast<ActInvulnerable const*>(Object))
        {
            fputs("Act_Invulnerable", Out);
            OS(Out, "label", C->Label);
            OU(Out, "invulnerable", static_cast<unsigned int>(C->bInvulnerable));
            OU(Out, "param3", static_cast<unsigned int>(C->bParam3));
            OF(Out, "duration", C->Duration);
            return true;
        }
        if (ActJumper const* C = dynamic_cast<ActJumper const*>(Object))
        {
            fputs("Act_Jumper", Out);
            OU(Out, "label_id", static_cast<unsigned int>(C->LabelId));
            OU(Out, "jumper", static_cast<unsigned int>(C->bJumper));
            return true;
        }
        if (ActLightFuse const* C = dynamic_cast<ActLightFuse const*>(Object))
        {
            fputs("Act_LightFuse", Out);
            OS(Out, "fuse", C->Fuse);
            OS(Out, "label", C->Label);
            OF(Out, "delay", C->Delay);
            return true;
        }
        if (ActLockDock const* C = dynamic_cast<ActLockDock const*>(Object))
        {
            fputs("Act_LockDock", Out);
            OS(Out, "target", C->Target);
            OS(Out, "gate", C->Gate);
            OI(Out, "mode", static_cast<int>(C->Mode));
            return true;
        }
        if (ActLockManeuvers const* C = dynamic_cast<ActLockManeuvers const*>(Object))
        {
            fputs("Act_LockManeuvers", Out);
            OU(Out, "lock", static_cast<unsigned int>(C->bLock));
            return true;
        }
        if (ActMarkObj const* C = dynamic_cast<ActMarkObj const*>(Object))
        {
            fputs("Act_MarkObj", Out);
            OS(Out, "label", C->Label);
            OI(Out, "mode", static_cast<int>(C->Mode));
            return true;
        }
        if (ActMovePlayer const* C = dynamic_cast<ActMovePlayer const*>(Object))
        {
            fputs("Act_MovePlayer", Out);
            OV(Out, "position", C->Position);
            OF(Out, "radius_sq", C->RadiusSq);
            return true;
        }
        if (ActNagClamp const* C = dynamic_cast<ActNagClamp const*>(Object))
        {
            fputs("Act_NagClamp", Out);
            OU(Out, "clamp", static_cast<unsigned int>(C->bClamp));
            OU(Out, "field_0x0d", static_cast<unsigned int>(C->Field_0x0d));
            OU(Out, "field_0x0e", static_cast<unsigned int>(C->Field_0x0e));
            return true;
        }
        if (ActNagDistLeaving const* C = dynamic_cast<ActNagDistLeaving const*>(Object))
        {
            fputs("Act_NagDistLeaving", Out);
            OU(Out, "nag_id", static_cast<unsigned int>(C->NagId));
            OS(Out, "speaker", C->Speaker);
            OS(Out, "target", C->Target);
            OV(Out, "position", C->Position);
            OU(Out, "has_position", static_cast<unsigned int>(C->bHasPosition));
            OF(Out, "distance", C->Distance);
            OU(Out, "fail_mad_lib_id", static_cast<unsigned int>(C->FailMadLibId));
            OU(Out, "nag_in_combat", static_cast<unsigned int>(C->bNagInCombat));
            OU(Out, "nag_out_of_combat", static_cast<unsigned int>(C->bNagOutOfCombat));
            return true;
        }
        if (ActNagDistTowards const* C = dynamic_cast<ActNagDistTowards const*>(Object))
        {
            fputs("Act_NagDistTowards", Out);
            OU(Out, "nag_id", static_cast<unsigned int>(C->NagId));
            OS(Out, "speaker", C->Speaker);
            OS(Out, "target", C->Target);
            OF(Out, "distance", C->Distance);
            OU(Out, "fail_mad_lib_id", static_cast<unsigned int>(C->FailMadLibId));
            OV(Out, "position", C->Position);
            OU(Out, "nag_in_combat", static_cast<unsigned int>(C->bNagInCombat));
            OU(Out, "nag_out_of_combat", static_cast<unsigned int>(C->bNagOutOfCombat));
            return true;
        }
        if (ActNagGreet const* C = dynamic_cast<ActNagGreet const*>(Object))
        {
            fputs("Act_NagGreet", Out);
            OU(Out, "nag_id", static_cast<unsigned int>(C->NagId));
            OS(Out, "speaker", C->Speaker);
            return true;
        }
        if (ActNagOff const* C = dynamic_cast<ActNagOff const*>(Object))
        {
            fputs("Act_NagOff", Out);
            OU(Out, "nag_id", static_cast<unsigned int>(C->NagId));
            return true;
        }
        if (ActNnPath const* C = dynamic_cast<ActNnPath const*>(Object))
        {
            fputs("Act_NNPath", Out);
            OV(Out, "position", C->Position);
            OS(Out, "label", C->Label);
            OU(Out, "system_id", static_cast<unsigned int>(C->SystemId));
            OI(Out, "travel_ids", static_cast<int>(C->TravelIds));
            OI(Out, "target_ids", static_cast<int>(C->TargetIds));
            return true;
        }
        if (ActNnIds const* C = dynamic_cast<ActNnIds const*>(Object))
        {
            fputs("Act_NNIds", Out);
            OI(Out, "text_id", static_cast<int>(C->TextId));
            OI(Out, "message_type", static_cast<int>(C->MessageType));
            return true;
        }
        if (ActPlayerCanDock const* C = dynamic_cast<ActPlayerCanDock const*>(Object))
        {
            fputs("Act_PlayerCanDock", Out);
            OU(Out, "can_dock", static_cast<unsigned int>(C->bCanDock));
            OUL(Out, "exceptions", C->Exceptions);
            return true;
        }
        if (ActPlayerCanTradelane const* C = dynamic_cast<ActPlayerCanTradelane const*>(Object))
        {
            fputs("Act_PlayerCanTradelane", Out);
            OU(Out, "can_tradelane", static_cast<unsigned int>(C->bCanTradelane));
            OPL(Out, "exceptions", C->Exceptions);
            return true;
        }
        if (ActPlayerEnemyClamp const* C = dynamic_cast<ActPlayerEnemyClamp const*>(Object))
        {
            fputs("Act_PlayerEnemyClamp", Out);
            OI(Out, "min_clamp", static_cast<int>(C->MinClamp));
            OI(Out, "max_clamp", static_cast<int>(C->MaxClamp));
            return true;
        }
        if (ActPlayerForm const* C = dynamic_cast<ActPlayerForm const*>(Object))
        {
            fputs("Act_PlayerForm", Out);
            OS(Out, "npc", C->Npc);
            OU(Out, "formation", C->Formation != nullptr ? 1u : 0u);
            return true;
        }
        if (ActPlayMusic const* C = dynamic_cast<ActPlayMusic const*>(Object))
        {
            fputs("Act_PlayMusic", Out);
            OU4(Out, "music", C->Music.Field0, C->Music.Field4, C->Music.Field8, C->Music.FieldC);
            OU(Out, "field_10", static_cast<unsigned int>(C->Music.Field10));
            OFB(Out, "fade", C->Music.Field14);
            OU(Out, "cancel", static_cast<unsigned int>(C->bCancel));
            return true;
        }
        if (ActPlayNn const* C = dynamic_cast<ActPlayNn const*>(Object))
        {
            fputs("Act_PlayNN", Out);
            OU(Out, "message_id", static_cast<unsigned int>(C->MessageId));
            return true;
        }
        if (ActPlaySoundEffect const* C = dynamic_cast<ActPlaySoundEffect const*>(Object))
        {
            fputs("Act_PlaySoundEffect", Out);
            OU(Out, "sound_id", static_cast<unsigned int>(C->SoundId));
            return true;
        }
        if (ActPilotParams const* C = dynamic_cast<ActPilotParams const*>(Object))
        {
            fputs("Act_PilotParams", Out);
            OI(Out, "flee_mode", static_cast<int>(C->FleeMode));
            OS(Out, "label", C->Label);
            return true;
        }
        if (ActPobjIdle const* C = dynamic_cast<ActPobjIdle const*>(Object))
        {
            fputs("Act_PobjIdle", Out);
            (void)C;
            return true;
        }
        if (ActPopUpDialog const* C = dynamic_cast<ActPopUpDialog const*>(Object))
        {
            fputs("Act_PopUpDialog", Out);
            OU(Out, "title_id", static_cast<unsigned int>(C->TitleId));
            OU(Out, "text_id", static_cast<unsigned int>(C->TextId));
            OU(Out, "buttons", static_cast<unsigned int>(C->Buttons));
            return true;
        }
        if (ActSetPriority const* C = dynamic_cast<ActSetPriority const*>(Object))
        {
            fputs("Act_SetPriority", Out);
            OU(Out, "formation_id", static_cast<unsigned int>(C->FormationId));
            OI(Out, "priority", static_cast<int>(C->Priority));
            return true;
        }
        if (ActRandomPop const* C = dynamic_cast<ActRandomPop const*>(Object))
        {
            fputs("Act_RandomPop", Out);
            OU(Out, "enabled", static_cast<unsigned int>(C->bEnabled));
            return true;
        }
        if (ActRandomPopSphere const* C = dynamic_cast<ActRandomPopSphere const*>(Object))
        {
            fputs("Act_RandomPopSphere", Out);
            OV3(Out, "center", C->CenterX, C->CenterY, C->CenterZ);
            OF(Out, "radius", C->Radius);
            OI(Out, "mode", static_cast<int>(C->Mode));
            return true;
        }
        if (ActRelocateForm const* C = dynamic_cast<ActRelocateForm const*>(Object))
        {
            fputs("Act_RelocateForm", Out);
            OS(Out, "label", C->Label);
            OFT(Out, "placement", C->Placement);
            return true;
        }
        if (ActRelocateShip const* C = dynamic_cast<ActRelocateShip const*>(Object))
        {
            fputs("Act_RelocateShip", Out);
            OS(Out, "label", C->Label);
            OFT(Out, "placement", C->Placement);
            return true;
        }
        if (ActRemoveAmbient const* C = dynamic_cast<ActRemoveAmbient const*>(Object))
        {
            fputs("Act_RemoveAmbient", Out);
            OS(Out, "path", C->Path);
            return true;
        }
        if (ActRemoveCargo const* C = dynamic_cast<ActRemoveCargo const*>(Object))
        {
            fputs("Act_RemoveCargo", Out);
            OS(Out, "loot", C->Loot);
            OI(Out, "count", static_cast<int>(C->Count));
            return true;
        }
        if (ActRemoveRtc const* C = dynamic_cast<ActRemoveRtc const*>(Object))
        {
            fputs("Act_RemoveRTC", Out);
            OS(Out, "rtc_file", C->RtcFile);
            return true;
        }
        if (ActRepChangeRequest const* C = dynamic_cast<ActRepChangeRequest const*>(Object))
        {
            fputs("Act_RepChangeRequest", Out);
            OU(Out, "shown", static_cast<unsigned int>(C->bShown));
            OU(Out, "armed", static_cast<unsigned int>(C->bArmed));
            OU(Out, "system_id", static_cast<unsigned int>(C->SystemId));
            OUL(Out, "groups", C->Groups);
            return true;
        }
        if (ActRevertCam const* C = dynamic_cast<ActRevertCam const*>(Object))
        {
            fputs("Act_RevertCam", Out);
            (void)C;
            return true;
        }
        if (ActRpopAttClamp const* C = dynamic_cast<ActRpopAttClamp const*>(Object))
        {
            fputs("Act_RpopAttClamp", Out);
            OU(Out, "clamp", static_cast<unsigned int>(C->bClamp));
            return true;
        }
        if (ActRpopTlAttacksEnabled const* C = dynamic_cast<ActRpopTlAttacksEnabled const*>(Object))
        {
            fputs("Act_RpopTLAttacksEnabled", Out);
            OU(Out, "enabled", static_cast<unsigned int>(C->bEnabled));
            return true;
        }
        if (ActSave const* C = dynamic_cast<ActSave const*>(Object))
        {
            fputs("Act_Save", Out);
            OU(Out, "save_trigger_id", static_cast<unsigned int>(C->SaveTriggerId));
            OU(Out, "save_type", static_cast<unsigned int>(C->SaveType));
            return true;
        }
        if (ActSendComm const* C = dynamic_cast<ActSendComm const*>(Object))
        {
            fputs("Act_SendComm", Out);
            OS(Out, "sender", C->Sender);
            OS(Out, "receiver", C->Receiver);
            OU(Out, "text_id", static_cast<unsigned int>(C->TextId));
            OC(Out, "speaker_costume", C->SpeakerCostume);
            OU(Out, "voice_id", static_cast<unsigned int>(C->VoiceId));
            OU(Out, "comm_type", static_cast<unsigned int>(C->CommType));
            OI(Out, "duration", static_cast<int>(C->Duration));
            return true;
        }
        if (ActSetFlee const* C = dynamic_cast<ActSetFlee const*>(Object))
        {
            fputs("Act_SetFlee", Out);
            OS(Out, "npc", C->Npc);
            OI(Out, "mode", static_cast<int>(C->Mode));
            OV(Out, "position", C->Position);
            OS(Out, "target", C->Target);
            OF(Out, "distance", C->Distance);
            OU(Out, "flag", static_cast<unsigned int>(C->bFlag));
            return true;
        }
        if (ActSetInitialPlayerPos const* C = dynamic_cast<ActSetInitialPlayerPos const*>(Object))
        {
            fputs("Act_SetInitialPlayerPos", Out);
            OME(Out, "orientation", C->Orientation);
            OV(Out, "position", C->Position);
            return true;
        }
        if (ActSetLifeTime const* C = dynamic_cast<ActSetLifeTime const*>(Object))
        {
            fputs("Act_SetLifeTime", Out);
            OS(Out, "label", C->Label);
            OF(Out, "life_time", C->LifeTime);
            return true;
        }
        if (ActSetNnHidden const* C = dynamic_cast<ActSetNnHidden const*>(Object))
        {
            fputs("Act_SetNNHidden", Out);
            OU(Out, "visible", static_cast<unsigned int>(C->bVisible));
            OU(Out, "objective_id", static_cast<unsigned int>(C->ObjectiveId));
            return true;
        }
        if (ActSetNnObj const* C = dynamic_cast<ActSetNnObj const*>(Object))
        {
            fputs("Act_SetNNObj", Out);
            OU(Out, "objective_id", static_cast<unsigned int>(C->ObjectiveId));
            OI(Out, "state", static_cast<int>(C->State));
            return true;
        }
        if (ActSetNnState const* C = dynamic_cast<ActSetNnState const*>(Object))
        {
            fputs("Act_SetNNState", Out);
            OU(Out, "objective_id", static_cast<unsigned int>(C->ObjectiveId));
            OI(Out, "state", static_cast<int>(C->State));
            return true;
        }
        if (ActSetOffer const* C = dynamic_cast<ActSetOffer const*>(Object))
        {
            fputs("Act_SetOffer", Out);
            OU(Out, "offer_id", static_cast<unsigned int>(C->OfferId));
            return true;
        }
        if (ActSetOrient const* C = dynamic_cast<ActSetOrient const*>(Object))
        {
            fputs("Act_SetOrient", Out);
            OS(Out, "label", C->Label);
            OQ(Out, "orientation", C->Orientation);
            return true;
        }
        if (ActSetRep const* C = dynamic_cast<ActSetRep const*>(Object))
        {
            fputs("Act_SetRep", Out);
            OS(Out, "label", C->Label);
            OS(Out, "faction", C->Faction);
            OU(Out, "group_id", static_cast<unsigned int>(C->GroupId));
            OF(Out, "value", C->Value);
            return true;
        }
        if (ActSetShipAndLoadout const* C = dynamic_cast<ActSetShipAndLoadout const*>(Object))
        {
            fputs("Act_SetShipAndLoadout", Out);
            OS(Out, "loadout_name", C->LoadoutName);
            OS(Out, "ship_name", C->ShipName);
            OLO(Out, "loadout", C->Loadout);
            OU(Out, "ship_arch_id", static_cast<unsigned int>(C->ShipArchId));
            return true;
        }
        if (ActSetTitle const* C = dynamic_cast<ActSetTitle const*>(Object))
        {
            fputs("Act_SetTitle", Out);
            OU(Out, "title_id", static_cast<unsigned int>(C->TitleId));
            return true;
        }
        if (ActSetVibe const* C = dynamic_cast<ActSetVibe const*>(Object))
        {
            fputs("Act_SetVibe", Out);
            OS(Out, "label_a", C->LabelA);
            OS(Out, "label_b", C->LabelB);
            OF(Out, "lo", C->Lo);
            OF(Out, "hi", C->Hi);
            return true;
        }
        if (ActSetVibeLbl const* C = dynamic_cast<ActSetVibeLbl const*>(Object))
        {
            fputs("Act_SetVibeLbl", Out);
            OU(Out, "label_a", static_cast<unsigned int>(C->LabelA));
            OU(Out, "label_b", static_cast<unsigned int>(C->LabelB));
            OF(Out, "lo", C->Lo);
            OF(Out, "hi", C->Hi);
            return true;
        }
        if (ActSetVibeLblToShip const* C = dynamic_cast<ActSetVibeLblToShip const*>(Object))
        {
            fputs("Act_SetVibeLblToShip", Out);
            OU(Out, "formation_id", static_cast<unsigned int>(C->FormationId));
            OS(Out, "ship", C->Ship);
            OF(Out, "lo", C->Lo);
            OF(Out, "hi", C->Hi);
            return true;
        }
        if (ActSetVibeShipToLbl const* C = dynamic_cast<ActSetVibeShipToLbl const*>(Object))
        {
            fputs("Act_SetVibeShipToLbl", Out);
            OS(Out, "ship", C->Ship);
            OU(Out, "formation_id", static_cast<unsigned int>(C->FormationId));
            OF(Out, "lo", C->Lo);
            OF(Out, "hi", C->Hi);
            return true;
        }
        if (ActSetVibeOfferBaseHack const* C = dynamic_cast<ActSetVibeOfferBaseHack const*>(Object))
        {
            fputs("Act_SetVibeOfferBaseHack", Out);
            OU(Out, "base_id", static_cast<unsigned int>(C->BaseId));
            return true;
        }
        if (ActSpawnFormation const* C = dynamic_cast<ActSpawnFormation const*>(Object))
        {
            fputs("Act_SpawnFormation", Out);
            OS(Out, "name", C->Name);
            OFT(Out, "placement", C->Placement);
            OU(Out, "position", static_cast<unsigned int>(C->bPosition));
            OU(Out, "orientation", static_cast<unsigned int>(C->bOrientation));
            return true;
        }
        if (ActSpawnLoot const* C = dynamic_cast<ActSpawnLoot const*>(Object))
        {
            fputs("Act_SpawnLoot", Out);
            OS(Out, "name", C->Name);
            OFT(Out, "placement", C->Placement);
            OU(Out, "position", static_cast<unsigned int>(C->bPosition));
            OU(Out, "orientation", static_cast<unsigned int>(C->bOrientation));
            return true;
        }
        if (ActSpawnShip const* C = dynamic_cast<ActSpawnShip const*>(Object))
        {
            fputs("Act_SpawnShip", Out);
            OS(Out, "name", C->Name);
            OS(Out, "new_nickname", C->NewNickname);
            OFT(Out, "placement", C->Placement);
            OU(Out, "position", static_cast<unsigned int>(C->bPosition));
            OU(Out, "orientation", static_cast<unsigned int>(C->bOrientation));
            OU(Out, "rename", static_cast<unsigned int>(C->bRename));
            OU(Out, "obj_list", static_cast<unsigned int>(C->ObjList));
            return true;
        }
        if (ActSpawnShipRel const* C = dynamic_cast<ActSpawnShipRel const*>(Object))
        {
            fputs("Act_SpawnShipRel", Out);
            OS(Out, "name", C->Name);
            OS(Out, "rel_obj", C->RelObj);
            OS(Out, "new_nickname", C->NewNickname);
            OFT(Out, "offset", C->Offset);
            OI(Out, "mode", static_cast<int>(C->Mode));
            OU(Out, "field_0xdc", static_cast<unsigned int>(C->Field_0xdc));
            OU(Out, "rename", static_cast<unsigned int>(C->bRename));
            OU(Out, "obj_list", static_cast<unsigned int>(C->ObjList));
            OF(Out, "heading", C->Heading);
            return true;
        }
        if (ActSpawnSolar const* C = dynamic_cast<ActSpawnSolar const*>(Object))
        {
            fputs("Act_SpawnSolar", Out);
            OS(Out, "name", C->Name);
            OFT(Out, "placement", C->Placement);
            OU(Out, "position", static_cast<unsigned int>(C->bPosition));
            OU(Out, "orientation", static_cast<unsigned int>(C->bOrientation));
            return true;
        }
        if (ActStartDialog const* C = dynamic_cast<ActStartDialog const*>(Object))
        {
            fputs("Act_StartDialog", Out);
            OU(Out, "dialog_id", static_cast<unsigned int>(C->DialogId));
            OI(Out, "delay_seconds", static_cast<int>(C->DelaySeconds));
            OU(Out, "delay_pending", static_cast<unsigned int>(C->bDelayPending));
            return true;
        }
        if (ActStaticCam const* C = dynamic_cast<ActStaticCam const*>(Object))
        {
            fputs("Act_StaticCam", Out);
            OT(Out, "camera", C->Camera);
            OF(Out, "fov", C->Fov);
            OF(Out, "aspect", C->Aspect);
            return true;
        }
        return false;
    }

    // The draws since the seeding: the next four values are matched against the generator's
    // sequence from the seed (at most 0x10000 draws; -1 beyond).
    int CountDraws()
    {
        int Probe[4];
        for (int Index = 0; Index < 4; ++Index)
            Probe[Index] = std::rand();
        std::srand(OracleMissionSeed);
        std::vector<int> Sequence;
        Sequence.reserve(0x10004);
        for (int Index = 0; Index < 0x10004; ++Index)
            Sequence.push_back(std::rand());
        for (int Draws = 0; Draws <= 0x10000; ++Draws)
        {
            if (Sequence[Draws] == Probe[0] && Sequence[Draws + 1] == Probe[1] &&
                Sequence[Draws + 2] == Probe[2] && Sequence[Draws + 3] == Probe[3])
                return Draws;
        }
        return -1;
    }

    struct FOracleFiles
    {
        FILE* Scripts;
        FILE* Triggers;
        FILE* Actions;
        FILE* ShipGroups;
        FILE* Ships;
        FILE* Solars;
        FILE* Loot;
        FILE* RandEncs;
        FILE* Npcs;
        FILE* NpcShipArchs;
        FILE* Objectives;
        FILE* Dialogs;
        FILE* DialogLines;
        FILE* Costumes;
    };

    int DumpTriggers(FOracleFiles const& F, char const* Name, FMissionScript const* S)
    {
        int Lines = 0;
        for (FTriggerMap::const_iterator It = S->TriggerMap.begin(); It != S->TriggerMap.end(); ++It)
        {
            FTrigger const& T = It->second;
            std::fprintf(F.Triggers, "mission=%s trigger=%u state=%d repeatable=%u", Name, It->first, T.State,
                         static_cast<unsigned int>(T.bRepeatable));
            OU(F.Triggers, "id", T.Id);
            OUL(F.Triggers, "events", T.EventIds);
            OU(F.Triggers, "actions", static_cast<unsigned int>(T.Actions.size()));
            if (T.Condition != nullptr)
            {
                std::fprintf(F.Triggers, " cnd_trigger=%u cnd=", T.Condition->TriggerId);
                if (!DumpCondition(F.Triggers, T.Condition))
                    std::fprintf(F.Triggers, "unknown");
            }
            else
            {
                std::fprintf(F.Triggers, " cnd=none");
            }
            std::fprintf(F.Triggers, "\n");
            ++Lines;
            unsigned int Index = 0;
            for (std::list<Act*>::const_iterator A = T.Actions.begin(); A != T.Actions.end(); ++A, ++Index)
            {
                std::fprintf(F.Actions, "mission=%s trigger=%u index=%u act_trigger=%u act=", Name, It->first,
                             Index, (*A)->TriggerId);
                if (!DumpAction(F.Actions, *A))
                    std::fprintf(F.Actions, "unknown");
                std::fprintf(F.Actions, "\n");
                ++Lines;
            }
        }
        return Lines;
    }

    int DumpShips(FOracleFiles const& F, char const* Name, FMissionScript const* S)
    {
        int Lines = 0;
        unsigned int GroupIndex = 0;
        for (std::list<FMissionShipGroup>::const_iterator G = S->MissionShips.begin(); G != S->MissionShips.end();
             ++G, ++GroupIndex)
        {
            std::fprintf(F.ShipGroups, "mission=%s group=%u", Name, GroupIndex);
            OS(F.ShipGroups, "nickname", G->Nickname);
            OU(F.ShipGroups, "system", G->System != nullptr ? G->System->SystemId : 0u);
            OMD(F.ShipGroups, "orientation", G->Orientation);
            OV(F.ShipGroups, "position", G->Position);
            OF(F.ShipGroups, "rel_pos_heading", G->RelPosHeading);
            OS(F.ShipGroups, "rel_pos_obj", G->RelPosObj);
            OF(F.ShipGroups, "rel_pos_range", G->RelPosRange);
            OU(F.ShipGroups, "rel_pos", G->bRelPos);
            OU(F.ShipGroups, "has_orientation", G->bOrientation);
            OS(F.ShipGroups, "arrival_obj", G->ArrivalObj);
            OU(F.ShipGroups, "arrival_obj_id", G->ArrivalObjId);
            OI(F.ShipGroups, "field_0xe0", G->Field_0xe0);
            OU(F.ShipGroups, "formation", G->Formation.Id);
            OU(F.ShipGroups, "ships", static_cast<unsigned int>(G->Ships.size()));
            std::fprintf(F.ShipGroups, "\n");
            ++Lines;
            for (size_t Index = 0; Index < G->Ships.size(); ++Index)
            {
                FMissionShip const& Ship = G->Ships[Index];
                std::fprintf(F.Ships, "mission=%s group=%u index=%u", Name, GroupIndex,
                             static_cast<unsigned int>(Index));
                OS(F.Ships, "nickname", Ship.Nickname);
                OU(F.Ships, "npc_id", Ship.NpcId.Id);
                OUL(F.Ships, "label_ids", Ship.LabelIds);
                OU(F.Ships, "jumper", Ship.bJumper);
                OU(F.Ships, "original_jump_flag", Ship.bOriginalJumpFlag);
                OI(F.Ships, "priority", Ship.Priority);
                OU(F.Ships, "random_name", Ship.bRandomName);
                OU(F.Ships, "init_objectives", Ship.InitObjectives.Id);
                OU(F.Ships, "init_meta_behavior", Ship.InitMetaBehavior.Id);
                std::fprintf(F.Ships, " cargo=");
                for (size_t Cargo = 0; Cargo < Ship.Cargo.size(); ++Cargo)
                    std::fprintf(F.Ships, "%s%u:%d", Cargo == 0 ? "" : ",", Ship.Cargo[Cargo].GoodId,
                                 Ship.Cargo[Cargo].Count);
                OI(F.Ships, "hit_points", Ship.HitPoints);
                OI(F.Ships, "field_0x80", Ship.Field_0x80);
                OF(F.Ships, "radius", Ship.Radius);
                std::fprintf(F.Ships, "\n");
                ++Lines;
            }
        }
        return Lines;
    }

    int DumpSolars(FOracleFiles const& F, char const* Name, FMissionScript const* S)
    {
        int Lines = 0;
        unsigned int Index = 0;
        for (std::list<FMissionSolar>::const_iterator It = S->MissionSolars.begin(); It != S->MissionSolars.end();
             ++It, ++Index)
        {
            std::fprintf(F.Solars, "mission=%s index=%u", Name, Index);
            OU(F.Solars, "space_obj_id", It->SpaceObjId);
            OU(F.Solars, "spawned", It->bSpawned);
            OU(F.Solars, "pilot", It->bPilot);
            OF(F.Solars, "radius", It->Radius);
            OS(F.Solars, "nickname", It->Nickname);
            OMD(F.Solars, "orientation", It->Orientation);
            OV(F.Solars, "position", It->Position);
            OU(F.Solars, "archetype_id", It->ArchetypeId);
            OU(F.Solars, "system_id", It->SystemId);
            OU(F.Solars, "loadout_id", It->LoadoutId);
            OI(F.Solars, "field_0x458", It->Field_0x458);
            OU(F.Solars, "faction", It->Faction);
            OU(F.Solars, "base_id", It->BaseId);
            OU(F.Solars, "voice_id", It->VoiceId);
            OU(F.Solars, "field_0x468", It->Field_0x468);
            OUL(F.Solars, "label_ids", It->LabelIds);
            OC(F.Solars, "space_costume", It->SpaceCostume);
            OU(F.Solars, "name", It->Name.MadLibId);
            OU(F.Solars, "visit", It->Visit);
            std::fprintf(F.Solars, "\n");
            ++Lines;
        }
        return Lines;
    }

    int DumpLoot(FOracleFiles const& F, char const* Name, FMissionScript const* S)
    {
        int Lines = 0;
        unsigned int Index = 0;
        for (std::list<FMissionLoot>::const_iterator It = S->MissionLoots.begin(); It != S->MissionLoots.end();
             ++It, ++Index)
        {
            std::fprintf(F.Loot, "mission=%s index=%u", Name, Index);
            OS(F.Loot, "nickname", It->Nickname);
            OU(F.Loot, "space_obj_id", It->SpaceObjId);
            OU(F.Loot, "spawned", It->bSpawned);
            OU(F.Loot, "system_id", It->Info.SystemID);
            OU(F.Loot, "equip_arch_id", It->Info.EquipArchID);
            OU(F.Loot, "name_id", It->Info.NameId);
            OU(F.Loot, "owner_obj_id", It->Info.OwnerObjId);
            OV(F.Loot, "velocity", It->Info.Velocity);
            OV(F.Loot, "position", It->Info.Position);
            OME(F.Loot, "orientation", It->Info.Orientation);
            OV(F.Loot, "angular_velocity", It->Info.AngularVelocity);
            OU(F.Loot, "units", It->Info.Units);
            OF(F.Loot, "hit_pts_scale", It->Info.HitPtsScale);
            OU(F.Loot, "can_ai_tractor", It->Info.bCanAiTractor);
            OU(F.Loot, "mission", It->Info.bMission);
            OU(F.Loot, "mark_for_player", It->Info.MarkForPlayer);
            OS(F.Loot, "rel_pos_obj", It->RelPosObj);
            OV(F.Loot, "rel_pos_offset", It->RelPosOffset);
            OU(F.Loot, "rel_pos", It->bRelPos);
            OU(F.Loot, "field_0xe8", It->Field_0xe8);
            OUL(F.Loot, "label_ids", It->LabelIds);
            OU(F.Loot, "sub_obj_id", It->SubObjId);
            OU(F.Loot, "cannot_jettison", It->bCannotJettison);
            std::fprintf(F.Loot, "\n");
            ++Lines;
        }
        return Lines;
    }

    // "formations=" entries joined by '+': found:nickname:field_0x08:min:max:count:weight:choices,
    // the choices joined by ';', each its classes joined by ',' then '/' and its weight.
    void DumpRandEncFormations(FILE* Out, std::list<FRandEncFormation> const& List)
    {
        std::fprintf(Out, " formations=");
        bool bFirst = true;
        for (std::list<FRandEncFormation>::const_iterator It = List.begin(); It != List.end(); ++It)
        {
            std::fprintf(Out, "%s%u:%u:%08x:%d:%d:%d:%08x:", bFirst ? "" : "+", It->Formation != nullptr ? 1u : 0u,
                         It->Nickname.Id, OracleBits(It->Field_0x08), It->MinCount, It->MaxCount, It->Count,
                         OracleBits(It->Weight));
            bool bFirstChoice = true;
            for (std::list<FRandEncShipChoice>::const_iterator C = It->ShipChoices.begin(); C != It->ShipChoices.end();
                 ++C)
            {
                std::fprintf(Out, "%s", bFirstChoice ? "" : ";");
                for (size_t Class = 0; Class < C->ShipClasses.size(); ++Class)
                    std::fprintf(Out, "%s%u", Class == 0 ? "" : ",", C->ShipClasses[Class]);
                std::fprintf(Out, "/%08x", OracleBits(C->Weight));
                bFirstChoice = false;
            }
            bFirst = false;
        }
    }

    int DumpRandEncs(FOracleFiles const& F, char const* Name, FMissionScript const* S)
    {
        int Lines = 0;
        unsigned int Index = 0;
        for (std::list<FMissionRandEnc>::const_iterator It = S->RandEncs.begin(); It != S->RandEncs.end();
             ++It, ++Index)
        {
            FILE* const Out = F.RandEncs;
            std::fprintf(Out, "mission=%s index=%u", Name, Index);
            OS(Out, "nickname", It->Nickname);
            OU(Out, "enabled", It->bEnabled);
            OU(Out, "nickname_id", It->NicknameId.Id);
            OU(Out, "formation_index", It->FormationIndex);
            OU(Out, "npc_count", It->NpcCount);
            FRandEncParams* const P = It->Params.Pointer;
            if (P == nullptr)
            {
                std::fprintf(Out, " params=0\n");
                ++Lines;
                continue;
            }
            OI(Out, "params", P->GetType());
            OU(Out, "system", P->System != nullptr ? P->System->SystemId : 0u);
            OU(Out, "controller", P->ControllerId);
            OU(Out, "player", P->Player != nullptr ? P->Player->PlayerId : 0u);
            OI(Out, "attacker_rep", P->AttackerRep);
            OU(Out, "pilot_id", P->PilotId.Id);
            OUL(Out, "labels", P->Labels);
            std::fprintf(Out, " positions=");
            bool bFirst = true;
            for (std::list<FRandEncPosition>::const_iterator Pos = P->Positions.begin(); Pos != P->Positions.end();
                 ++Pos)
            {
                std::fprintf(Out, "%s%08x,%08x,%08x,%08x", bFirst ? "" : ";", OracleBits(Pos->Position.x),
                             OracleBits(Pos->Position.y), OracleBits(Pos->Position.z), OracleBits(Pos->Weight));
                bFirst = false;
            }
            OI(Out, "ships_spawned", P->ShipsSpawned);
            OI(Out, "formations_to_spawn", P->FormationsToSpawn);
            OI(Out, "num_forms_min", P->NumFormsMin);
            OI(Out, "num_forms_max", P->NumFormsMax);
            DumpRandEncFormations(Out, P->Formations);
            OS(Out, "target_ship_name", P->TargetShipName);
            OU(Out, "goto_target_id", P->GotoTargetId);
            OU(Out, "target_object_id", P->TargetObjectId);
            OU(Out, "dock_target", P->DockTarget);
            OF(Out, "spawn_angle", P->SpawnAngle);
            OF(Out, "spawn_distance", P->SpawnDistance);
            OI(Out, "empty_pos_radius", P->EmptyPosRadius);
            OI(Out, "ship_name_counter", P->ShipNameCounter);
            if (FRandEncSPParams const* Space = dynamic_cast<FRandEncSPParams const*>(P))
            {
                OI(Out, "activation_type", Space->ActivationType);
                OV(Out, "activation_location", Space->ActivationLocation);
                OI(Out, "activation_range", Space->ActivationRange);
                OF(Out, "activation_time_0", Space->ActivationTime[0]);
                OF(Out, "activation_time_1", Space->ActivationTime[1]);
                OF(Out, "cumulative_time", Space->CumulativeTime);
            }
            else if (FRandEncTLParams const* Lane = dynamic_cast<FRandEncTLParams const*>(P))
            {
                OI(Out, "tl_attack_chance", Lane->TlAttackChance);
                OI(Out, "tl_attack_chance_read_in", Lane->TlAttackChanceReadIn);
                OI(Out, "direction", Lane->Direction);
                OU(Out, "entry_ring", Lane->EntryRing);
                OU(Out, "exit_ring", Lane->ExitRing);
                OU(Out, "target_tradelane", Lane->TargetTradelane);
            }
            std::fprintf(Out, "\n");
            ++Lines;
        }
        return Lines;
    }

    int DumpNpcs(FOracleFiles const& F, char const* Name, FMissionScript const* S)
    {
        int Lines = 0;
        for (std::map<unsigned int, FMsnNpc>::const_iterator It = S->MissionNpcs.begin(); It != S->MissionNpcs.end();
             ++It)
        {
            FMsnNpc const& N = It->second;
            std::fprintf(F.Npcs, "mission=%s key=%u", Name, It->first);
            OU(F.Npcs, "nickname", N.Nickname.Id);
            OU(F.Npcs, "affiliation", N.Affiliation);
            OI(F.Npcs, "individual_name", N.IndividualName);
            OU(F.Npcs, "npc_ship_arch", N.NpcShipArch.Id);
            OU(F.Npcs, "field_0x0c", N.Field_0x0C.Id);
            OC(F.Npcs, "appearance", N.Appearance);
            OU(F.Npcs, "voice", N.Voice);
            std::fprintf(F.Npcs, "\n");
            ++Lines;
        }
        for (FNpcShipArchMap::const_iterator It = S->NpcShipArchs.begin(); It != S->NpcShipArchs.end(); ++It)
        {
            FNpcShipArch const& A = It->second;
            std::fprintf(F.NpcShipArchs, "mission=%s key=%u", Name, It->first);
            OU(F.NpcShipArchs, "nickname", A.Nickname.Id);
            OU(F.NpcShipArchs, "ship_archetype", A.ShipArchetype);
            OU(F.NpcShipArchs, "loadout", A.Loadout);
            OU(F.NpcShipArchs, "pilot", A.Pilot.Id);
            OI(F.NpcShipArchs, "state_graph", A.StateGraph);
            OUL(F.NpcShipArchs, "npc_classes", A.NpcClasses);
            OI(F.NpcShipArchs, "level", A.Level);
            std::fprintf(F.NpcShipArchs, "\n");
            ++Lines;
        }
        return Lines;
    }

    void DumpMarker(FILE* Out, FmtStr::NavMarker const& Marker)
    {
        std::fprintf(Out, " marker=%u,%u,%u,%u,%u", Marker.Data[0], Marker.Data[1], Marker.Data[2], Marker.Data[3],
                     Marker.Data[4]);
    }

    // The base part, then the fields of the objective's own class (types 7 and 8 write none:
    // their parse reads nothing).
    int DumpObjectives(FOracleFiles const& F, char const* Name, FMissionScript const* S)
    {
        int Lines = 0;
        for (std::map<unsigned int, FMissionPlayer*>::const_iterator P = S->Players.begin(); P != S->Players.end(); ++P)
        {
            if (P->second == nullptr)
                continue;
            std::list<FMissionObjective*> const& List = P->second->Objectives.Objectives;
            unsigned int Index = 0;
            for (std::list<FMissionObjective*>::const_iterator It = List.begin(); It != List.end(); ++It, ++Index)
            {
                FMissionObjective* const O = *It;
                FILE* const Out = F.Objectives;
                std::fprintf(Out, "mission=%s player=%u n=%u", Name, P->first, Index);
                OI(Out, "type", O->GetType());
                OU(Out, "hidden", O->bHidden);
                OU(Out, "nickname", O->Nickname.Id);
                OI(Out, "ids", O->Ids);
                OI(Out, "index", O->Index);
                OU(Out, "state", static_cast<unsigned int>(O->Objective.Type));
                if (FNNObjectiveBase const* Base = dynamic_cast<FNNObjectiveBase const*>(O))
                    OU(Out, "base_id", Base->BaseId);
                else if (FNNObjectiveNavMarker const* Nav = dynamic_cast<FNNObjectiveNavMarker const*>(O))
                    DumpMarker(Out, Nav->Marker);
                else if (FNNObjectiveRepInst const* Rep = dynamic_cast<FNNObjectiveRepInst const*>(O))
                {
                    DumpMarker(Out, Rep->Marker);
                    OS(Out, "label", Rep->Label);
                }
                else if (FNNObjectiveType5 const* T5 = dynamic_cast<FNNObjectiveType5 const*>(O))
                {
                    OI(Out, "rep_instance", T5->RepInstance);
                    OU(Out, "system_id", T5->SystemId);
                    DumpMarker(Out, T5->Marker);
                }
                else if (FNNObjectiveType6 const* T6 = dynamic_cast<FNNObjectiveType6 const*>(O))
                {
                    OI(Out, "rep_instance", T6->RepInstance);
                    OU(Out, "system_id", T6->SystemId);
                    DumpMarker(Out, T6->Marker);
                }
                std::fprintf(Out, "\n");
                ++Lines;
            }
        }
        return Lines;
    }

    int DumpDialogs(FOracleFiles const& F, char const* Name)
    {
        int Lines = 0;
        for (std::map<unsigned int, FMissionDialog>::const_iterator It = GMissionDialogs.begin();
             It != GMissionDialogs.end(); ++It)
        {
            FMissionDialog const& D = It->second;
            unsigned int LineCount = 0;
            typedef std::map<TString<0x30>, std::vector<FMissionDialogLine>, FMissionDialogSpeakerLess> FSpeakers;
            for (FSpeakers::const_iterator S = D.Speakers.begin(); S != D.Speakers.end(); ++S)
            {
                for (size_t Index = 0; Index < S->second.size(); ++Index)
                {
                    FMissionDialogLine const& L = S->second[Index];
                    FILE* const Out = F.DialogLines;
                    std::fprintf(Out, "mission=%s dialog=%u speaker_key=", Name, It->first);
                    OracleHex(Out, S->first.Data);
                    std::fprintf(Out, " index=%u", static_cast<unsigned int>(Index));
                    OS(Out, "speaker", L.Speaker);
                    OS(Out, "target", L.Target);
                    OU(Out, "first", L.bFirst);
                    OU(Out, "field_0x69", L.Field_0x69);
                    OU(Out, "key", L.Comm.Key);
                    OU(Out, "text_id", L.Comm.TextId);
                    OU(Out, "target_id", L.Comm.TargetId);
                    OU(Out, "field_0x0c", L.Comm.Field_0x0c);
                    std::fprintf(Out, "\n");
                    ++Lines;
                    ++LineCount;
                }
            }
            std::fprintf(F.Dialogs, "mission=%s dialog=%u controller=%u system=%u speakers=%u lines=%u\n", Name, It->first,
                         D.ControllerId, D.System != nullptr ? D.System->SystemId : 0u,
                         static_cast<unsigned int>(D.Speakers.size()), LineCount);
            ++Lines;
        }
        return Lines;
    }

    int DumpScript(FOracleFiles const& F, char const* Name, FMissionScript const* S, int Draws)
    {
        unsigned int Objectives = 0;
        for (std::map<unsigned int, FMissionPlayer*>::const_iterator P = S->Players.begin(); P != S->Players.end(); ++P)
        {
            if (P->second != nullptr)
                Objectives += static_cast<unsigned int>(P->second->Objectives.Objectives.size());
        }
        unsigned int Actions = 0;
        for (FTriggerMap::const_iterator It = S->TriggerMap.begin(); It != S->TriggerMap.end(); ++It)
            Actions += static_cast<unsigned int>(It->second.Actions.size());
        FILE* const Out = F.Scripts;
        std::fprintf(Out, "mission=%s draws=%d", Name, Draws);
        OU(Out, "controller", S->ControllerId);
        OU(Out, "player", S->PlayerId);
        OU(Out, "player_state", S->PlayerState != nullptr ? 1u : 0u);
        OU(Out, "players", static_cast<unsigned int>(S->Players.size()));
        OF(Out, "heartbeat_interval", S->HeartbeatInterval);
        OU(Out, "reward", S->Reward);
        OU(Out, "title", S->Field_0x50.MadLibId);
        OU(Out, "offer", S->Field_0x68.MadLibId);
        OI(Out, "previous_state", S->PreviousMissionState);
        OI(Out, "mission_state", S->MissionState);
        OU(Out, "field_0x2b0", S->Field_0x2b0);
        OI(Out, "campaign_slot_current", S->CampaignSlotCurrent);
        OI(Out, "campaign_slot_succeed", S->CampaignSlotSucceed);
        OI(Out, "story_cue", S->StoryCue);
        OU(Out, "field_0x2c0", S->Field_0x2c0);
        OU(Out, "field_0x2c4", S->Field_0x2c4);
        OU(Out, "field_0x2e8", S->Field_0x2e8);
        OF(Out, "elapsed_time", S->ElapsedTime);
        OU(Out, "triggers", static_cast<unsigned int>(S->TriggerMap.size()));
        OU(Out, "actions", Actions);
        std::fprintf(Out, " initially_active=");
        for (std::map<unsigned int, unsigned int>::const_iterator It = S->InitiallyActive.begin();
             It != S->InitiallyActive.end(); ++It)
            std::fprintf(Out, "%s%u:%u", It == S->InitiallyActive.begin() ? "" : ",", It->first, It->second);
        OU(Out, "ship_groups", static_cast<unsigned int>(S->MissionShips.size()));
        OU(Out, "solars", static_cast<unsigned int>(S->MissionSolars.size()));
        OU(Out, "loot", static_cast<unsigned int>(S->MissionLoots.size()));
        OU(Out, "rand_encs", static_cast<unsigned int>(S->RandEncs.size()));
        OU(Out, "npcs", static_cast<unsigned int>(S->MissionNpcs.size()));
        OU(Out, "npc_ship_archs", static_cast<unsigned int>(S->NpcShipArchs.size()));
        OU(Out, "objectives", Objectives);
        OU(Out, "dialogs", static_cast<unsigned int>(GMissionDialogs.size()));
        OU(Out, "dialog_costumes", static_cast<unsigned int>(S->DialogCostumes.size()));
        std::fprintf(Out, "\n");
        for (size_t Index = 0; Index < S->DialogCostumes.size(); ++Index)
        {
            std::fprintf(F.Costumes, "mission=%s index=%u", Name, static_cast<unsigned int>(Index));
            OC(F.Costumes, "costume", S->DialogCostumes[Index]);
            std::fprintf(F.Costumes, "\n");
        }
        return 1 + static_cast<int>(S->DialogCostumes.size());
    }

    FILE* OracleOpen(std::string const& Dir, char const* Table)
    {
        std::string const Path = Dir + "\\mission_" + Table + ".txt";
        return std::fopen(Path.c_str(), "wb");
    }
}

// The fourteen missions in this order. Returns the number of lines written, -1 when a file
// cannot be opened or a mission is not made.
extern "C" __declspec(dllexport) int __cdecl OracleDumpMissionScripts(char const* Dir)
{
    if (Dir == nullptr)
        return -1;
    std::string const Root(Dir);
    char const* const Tables[] = {"scripts",     "triggers",       "actions",    "ship_groups", "ships",
                                  "solars",      "loot",           "rand_encs",  "npcs",        "npc_ship_archs",
                                  "objectives",  "dialogs",        "dialog_lines", "costumes"};
    int const Count = static_cast<int>(sizeof(Tables) / sizeof(Tables[0]));
    FILE* Files[sizeof(Tables) / sizeof(Tables[0])] = {};
    bool bOpen = true;
    for (int Index = 0; Index < Count; ++Index)
    {
        Files[Index] = OracleOpen(Root, Tables[Index]);
        bOpen = bOpen && Files[Index] != nullptr;
    }
    int Lines = -1;
    if (bOpen)
    {
        FOracleFiles const F = {Files[0], Files[1], Files[2],  Files[3],  Files[4],  Files[5],  Files[6],
                                Files[7], Files[8], Files[9], Files[10], Files[11], Files[12], Files[13]};
        char const* const Missions[] = {"Mission_01a", "Mission_01b", "Mission_02", "Mission_03", "Mission_04",
                                        "Mission_05",  "Mission_06",  "Mission_07", "Mission_08", "Mission_09",
                                        "Mission_10",  "Mission_11",  "Mission_12", "Mission_13"};
        // The synthetic player, in he01 (a multiplayer player needs a system for its mission
        // record, which the [NNObjective] sections fill).
        FContentPlayerState* const State = ReplaceContentPlayerState(OraclePlayerId);
        unsigned int He01 = 0;
        pub::GetSystemID(He01, "he01");
        State->SystemRecord = FindSystemInfo(&He01);
        Lines = 0;
        for (int Index = 0; Index < static_cast<int>(sizeof(Missions) / sizeof(Missions[0])); ++Index)
        {
            GMissionDialogs.clear();
            std::srand(OracleMissionSeed);
            FStoryMissionCreateParms Parms;
            Parms.PlayerId = OraclePlayerId;
            Parms.StoryEvents = nullptr;
            IDLL* const Instance =
                CreateInstance(OracleControllerBase + static_cast<unsigned int>(Index), Missions[Index], &Parms);
            int const Draws = CountDraws();
            FMissionScript const* const Script = dynamic_cast<FMissionScript const*>(Instance);
            if (Script == nullptr)
            {
                Lines = -1;
                break;
            }
            Lines += DumpScript(F, Missions[Index], Script, Draws);
            Lines += DumpTriggers(F, Missions[Index], Script);
            Lines += DumpShips(F, Missions[Index], Script);
            Lines += DumpSolars(F, Missions[Index], Script);
            Lines += DumpLoot(F, Missions[Index], Script);
            Lines += DumpRandEncs(F, Missions[Index], Script);
            Lines += DumpNpcs(F, Missions[Index], Script);
            Lines += DumpObjectives(F, Missions[Index], Script);
            Lines += DumpDialogs(F, Missions[Index]);
            DestroyInstance(Instance);
        }
        GMissionDialogs.clear();
        RemoveContentPlayerState(OraclePlayerId);
    }
    for (int Index = 0; Index < Count; ++Index)
    {
        if (Files[Index] != nullptr)
            std::fclose(Files[Index]);
    }
    return Lines;
}
