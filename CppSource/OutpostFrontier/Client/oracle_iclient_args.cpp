// Oracle instrumentation (work package P0.5, design/reference-build.md section 8).

// Bodies of the argument recorders oracle_iclient_args.h declares. The includes follow
// client_iclient_space.cpp, which needs the same complete types.

#include "oracle_iclient_args.h"

#include "OutpostFrontierClient.h"
#include "common_archetype.h"     // CollisionGroupDesc
#include "common_cobject.h"
#include "common_goods.h"         // EquipDesc, EquipDescVector, BaseHint
#include "common_misc.h"          // DamageList
#include "common_reputation.h"    // Costume, Reputation::Relation
#include "common_server_types.h"  // X* records, CHAT_ID
#include "common_text.h"          // FmtStr
#include "rpc_iclient_args.h"     // the F* argument structs
#include "server_createinfo.h"    // CSolarCreateInfo, CShipCreateInfo, F*Create*
#include "server_sysdb.h"         // FMineExplosion

#include "oracle_json.h"
#include "oracle_mode.h"

#include <list>
#include <string>
#include <vector>

namespace
{
    template <class T>
    void HexBytes(FOracleJson& J, char const* Name, T const& Value)
    {
        J.Bytes(Name, &Value, sizeof(Value));
    }

    void VectorValue(FOracleJson& J, char const* Name, Vector const& Value)
    {
        J.BeginArray(Name);
        J.Float(nullptr, Value.x);
        J.Float(nullptr, Value.y);
        J.Float(nullptr, Value.z);
        J.EndArray();
    }

    void Present(FOracleJson& J, char const* Name)
    {
        J.Str(Name, "present");
    }

    void Relations(FOracleJson& J, char const* Name, ::Reputation::Relation const* Values,
                   unsigned Count)
    {
        if (Values == nullptr)
        {
            J.Null(Name);
            return;
        }
        J.BeginArray(Name);
        for (unsigned Index = 0; Index < Count; ++Index)
        {
            J.BeginArray(nullptr);
            J.Uint(nullptr, Values[Index].GroupId);
            J.Float(nullptr, Values[Index].Feeling);
            J.EndArray();
        }
        J.EndArray();
    }
}

#define ORACLE_HEX_ARG(Type)                                                                    \
    void OracleArg(FOracleJson& J, char const* Name, Type const& Value)                        \
    {                                                                                           \
        HexBytes(J, Name, Value);                                                               \
    }

ORACLE_HEX_ARG(XActivateEquip)
ORACLE_HEX_ARG(XActivateCruise)
ORACLE_HEX_ARG(XActivateThrusters)
ORACLE_HEX_ARG(XSetTarget)
ORACLE_HEX_ARG(XGoTradelane)
ORACLE_HEX_ARG(XJettisonCargo)
ORACLE_HEX_ARG(FServerCharSelectVerifiedInfo)
ORACLE_HEX_ARG(FServerCreateProjectileInfo)
ORACLE_HEX_ARG(FServerCreateGuidedInfo)
ORACLE_HEX_ARG(FMineExplosion)
ORACLE_HEX_ARG(FCommonUpdateObjectInfo)
ORACLE_HEX_ARG(FServerDestroyObjectInfo)
ORACLE_HEX_ARG(FPayloadServerActivateObject)
ORACLE_HEX_ARG(FServerSystemSwitchOutInfo)
ORACLE_HEX_ARG(FPayloadServerSystemSwitchIn)
ORACLE_HEX_ARG(FServerLandInfo)
ORACLE_HEX_ARG(FServerLaunchInfo)
ORACLE_HEX_ARG(FServerRequestSpaceScriptInfo)
ORACLE_HEX_ARG(FPayloadServerItemTractored)
ORACLE_HEX_ARG(FPayloadServerUseItem)
ORACLE_HEX_ARG(FPayloadServerAdjustAttitude)
ORACLE_HEX_ARG(FServerMissionObjectiveStateInfo)
ORACLE_HEX_ARG(FPayloadServerCfgInterfaceNotification)
ORACLE_HEX_ARG(FServerRepairObjectInfo)
ORACLE_HEX_ARG(FServerBurnFuseInfo)
ORACLE_HEX_ARG(FPayloadServerSetCamera)
ORACLE_HEX_ARG(FPayloadServerSetDirective)
ORACLE_HEX_ARG(Costume)

#undef ORACLE_HEX_ARG

void OracleArg(FOracleJson& J, char const* Name, Vector const& Value)
{
    VectorValue(J, Name, Value);
}

void OracleArg(FOracleJson& J, char const* Name, CHAT_ID const& Value)
{
    J.Uint(Name, Value.Id);
}

void OracleArg(FOracleJson& J, char const* Name, XFireWeaponInfo const& Value)
{
    J.BeginObject(Name);
    J.Uint("ObjectId", Value.ObjectId);
    VectorValue(J, "TargetPos", Value.TargetPos);
    J.BeginArray("Hardpoints");
    for (size_t Index = 0; Index < Value.Hardpoints.size(); ++Index)
        J.Uint(nullptr, Value.Hardpoints[Index]);
    J.EndArray();
    J.EndObject();
}

void OracleArg(FOracleJson& J, char const* Name, XTractorObjects const& Value)
{
    J.BeginObject(Name);
    J.Uint("ObjectId", Value.ObjectId);
    J.Uint("TractorEquipId", Value.TractorEquipId);
    J.BeginArray("TargetIds");
    for (size_t Index = 0; Index < Value.TargetIds.size(); ++Index)
        J.Uint(nullptr, Value.TargetIds[Index]);
    J.EndArray();
    J.EndObject();
}

void OracleArg(FOracleJson& J, char const* Name, std::list<CollisionGroupDesc> const& Value)
{
    J.BeginArray(Name);
    for (std::list<CollisionGroupDesc>::const_iterator It = Value.begin(); It != Value.end(); ++It)
    {
        J.BeginArray(nullptr);
        J.Uint(nullptr, It->Id);
        J.Float(nullptr, It->Health);
        J.EndArray();
    }
    J.EndArray();
}

void OracleArg(FOracleJson& J, char const* Name, EquipDesc const& Value)
{
    J.BeginObject(Name);
    J.Uint("Cached", Value.Cached);
    J.Uint("Id", Value.Id);
    J.Uint("ArchId", Value.ArchId);
    J.Str("Hardpoint", Value.Hardpoint);
    J.Uint("bEquipped", Value.bEquipped);
    J.Float("Health", Value.Health);
    J.Int("Count", Value.Count);
    J.Bool("bMission", Value.bMission);
    J.Uint("Owner", Value.Owner);
    J.EndObject();
}

void OracleArg(FOracleJson& J, char const* Name, EquipDescVector const& Value)
{
    J.BeginArray(Name);
    if (Value.Begin != nullptr)
    {
        for (EquipDesc const* It = Value.Begin; It != Value.End; ++It)
            OracleArg(J, nullptr, *It);
    }
    J.EndArray();
}

void OracleArg(FOracleJson& J, char const* Name, CSolarCreateInfo const& Value)
{
    J.BeginObject(Name);
    J.Uint("ObjId", Value.ObjId);
    J.Uint("ArchetypeNickname", Value.ArchetypeNickname);
    J.EndObject();
}

void OracleArg(FOracleJson& J, char const* Name, CShipCreateInfo const& Value)
{
    J.BeginObject(Name);
    J.Uint("ObjId", Value.ObjId);
    J.Uint("ArchetypeNickname", Value.ArchetypeNickname);
    J.EndObject();
}

void OracleArg(FOracleJson& J, char const* Name, FServerCreateLootRequest const& Value)
{
    J.BeginObject(Name);
    J.Uint("OwnerId", Value.OwnerId);
    J.Uint("Kind", Value.Kind);
    J.Uint("bMarkedBy", Value.bMarkedBy);
    J.Uint("bTemporary", Value.bTemporary);
    J.Uint("Count", Value.Count);
    if (Value.Items == nullptr)
    {
        J.Null("Items");
    }
    else
    {
        J.BeginArray("Items");
        for (unsigned Index = 0; Index < Value.Count; ++Index)
            HexBytes(J, nullptr, Value.Items[Index]);
        J.EndArray();
    }
    J.EndObject();
}

void OracleArg(FOracleJson& J, char const* Name, DamageList const& Value)
{
    J.BeginObject(Name);
    J.Int("Count", Value.Count);
    J.Bool("bDestroyed", Value.bDestroyed);
    J.Int("Cause", static_cast<int>(Value.Cause));
    J.Uint("InflictorId", Value.InflictorId);
    J.Uint("InflictorOwner", Value.InflictorOwner);
    J.BeginArray("Entries");
    for (std::list<DamageEntry>::const_iterator It = Value.Entries.begin();
         It != Value.Entries.end(); ++It)
    {
        J.BeginArray(nullptr);
        J.Uint(nullptr, It->SubObjId);
        J.Float(nullptr, It->HitPts);
        J.Int(nullptr, static_cast<int>(It->Fate));
        J.EndArray();
    }
    J.EndArray();
    J.EndObject();
}

void OracleArg(FOracleJson& J, char const* Name, FServerSetReputationInfo const& Value)
{
    J.BeginObject(Name);
    J.Int("VibeHandle", Value.VibeHandle);
    J.Float("Attitude", Value.Attitude);
    J.Uint("Field_0x08", Value.Field_0x08);
    J.Uint("Field_0x0c", Value.Field_0x0c);
    J.Uint("RelationCount", Value.RelationCount);
    Relations(J, "Relations", Value.Relations, Value.RelationCount);
    Present(J, "NameA");
    Present(J, "NameB");
    J.WStrN("DisplayName", Value.DisplayName, sizeof(Value.DisplayName) / sizeof(wchar_t));
    J.EndObject();
}

void OracleArg(FOracleJson& J, char const* Name, CSetMissionMessage const& Value)
{
    J.BeginObject(Name);
    Present(J, "Text");
    J.Uint("Type", Value.Type);
    J.EndObject();
}

void OracleArg(FOracleJson& J, char const* Name, FServerReplaceMissionObjectiveInfo const& Value)
{
    J.BeginObject(Name);
    J.Uint("Field_0x00", Value.Field_0x00);
    Present(J, "Text");
    J.Uint("ObjectiveType", Value.ObjectiveType);
    J.Uint("State", Value.State);
    J.EndObject();
}

void OracleArg(FOracleJson& J, char const* Name, FServerSetMissionObjectivesInfo const& Value)
{
    J.BeginObject(Name);
    J.Uint("Field_0x00", Value.Field_0x00);
    J.Uint("Count", Value.Count);
    if (Value.Entries == nullptr)
    {
        J.Null("States");
    }
    else
    {
        J.BeginArray("States");
        for (unsigned Index = 0; Index < Value.Count; ++Index)
            J.Uint(nullptr, Value.Entries[Index].State);
        J.EndArray();
    }
    Present(J, "TextA");
    Present(J, "TextB");
    J.Uint("Field_0x3c", Value.Field_0x3c);
    J.EndObject();
}

void OracleArg(FOracleJson& J, char const* Name, FServerSetGroupFeelingsInfo const& Value)
{
    J.BeginObject(Name);
    J.Bytes("Name", Value.Name, sizeof(Value.Name));
    J.Uint("RelationCount", Value.RelationCount);
    Relations(J, "Relations", Value.Relations, Value.RelationCount);
    J.EndObject();
}

void OracleArg(FOracleJson& J, char const* Name, BaseHint const* Value)
{
    if (Value == nullptr)
    {
        J.Null(Name);
        return;
    }
    J.BeginObject(Name);
    J.Uint("Kind", Value->Kind);
    J.EndObject();
}

void OracleArg(FOracleJson& J, char const* Name, std::wstring const& Value)
{
    J.WStrN(Name, Value.c_str(), Value.size());
}

void OracleArg(FOracleJson& J, char const* Name, std::vector<unsigned int> const& Value)
{
    J.BeginArray(Name);
    for (size_t Index = 0; Index < Value.size(); ++Index)
        J.Uint(nullptr, Value[Index]);
    J.EndArray();
}

void OracleArgPresence(FOracleJson& J, char const* Name, bool bPresent)
{
    if (bPresent)
        Present(J, Name);
    else
        J.Null(Name);
}

void OracleArgUintArray(FOracleJson& J, char const* Name, unsigned int const* Values, int Count)
{
    if (Values == nullptr)
    {
        J.Null(Name);
        return;
    }
    J.BeginArray(Name);
    for (int Index = 0; Index < Count; ++Index)
        J.Uint(nullptr, Values[Index]);
    J.EndArray();
}

unsigned OracleIClientBegin(FOracleJson& J, int Slot, char const* Method)
{
    unsigned const Seq = OracleNextIClientSeq();
    J.Uint("frame", OracleFrameNumber());
    J.Uint("seq", Seq);
    J.Int("slot", Slot);
    J.Str("m", Method);
    J.BeginObject("a");
    return Seq;
}

void OracleIClientEnd(FOracleJson& J)
{
    J.EndObject();
    OracleWriteRecord(kOracleIClient, J.Finish());
}

void OracleIClientReturnBegin(FOracleJson& J, int Slot, char const* Method, unsigned Seq)
{
    J.Uint("frame", OracleFrameNumber());
    J.Uint("seq", Seq);
    J.Int("slot", Slot);
    J.Str("m", Method);
}

void OracleIClientReturnEnd(FOracleJson& J)
{
    OracleWriteRecord(kOracleIClient, J.Finish());
}
