// Oracle instrumentation (work package P3.3, decision record 0007 point 11).
//
// The bodies of sim_client_stub_replication.h: each writes one replication.jsonl line,
// {"tick","seq","client","m","id", ...}, when the harness has asked for the record. Floats are
// bit patterns (oracle_json.h); `q` is a Quaternion in its member order w, x, y, z. Only the
// fields the replication comparison reads are written; iclient.jsonl has every call.

#include "sim_client_stub_replication.h"
#include "sim_client_stub.h"

#include "common_misc.h"          // DamageList, DamageEntry
#include "common_server_types.h"  // the X* records
#include "rpc_iclient_args.h"     // the F* argument structs
#include "server_createinfo.h"    // CSolarCreateInfo, CShipCreateInfo, F*Create*, FServerLootDesc

#include <list>
#include <vector>

namespace
{
    void VectorJ(FOracleJson& J, char const* Name, Vector const& Value)
    {
        float const Values[3] = {Value.x, Value.y, Value.z};
        J.FloatArray(Name, Values, 3);
    }

    void QuaternionJ(FOracleJson& J, char const* Name, Quaternion const& Value)
    {
        float const Values[4] = {Value.w, Value.x, Value.y, Value.z};
        J.FloatArray(Name, Values, 4);
    }

    // The head of a line; false when no record is being written.
    bool Begin(FOracleJson& J, unsigned Seq, unsigned int ClientId, char const* Method, unsigned int Id)
    {
        if (!SimStubReplicationOn())
            return false;
        SimStubReplicationBegin(J, Seq, ClientId, Method);
        J.Uint("id", Id);
        return true;
    }
}

void SimStubReplFireWeapon(unsigned Seq, unsigned int ClientId, XFireWeaponInfo const& Info)
{
    FOracleJson J;
    if (!Begin(J, Seq, ClientId, "SendCommonFireWeapon", Info.ObjectId))
        return;
    VectorJ(J, "target", Info.TargetPos);
    J.BeginArray("guns");
    for (size_t Index = 0; Index < Info.Hardpoints.size(); ++Index)
        J.Uint(nullptr, Info.Hardpoints[Index]);
    J.EndArray();
    SimStubReplicationEnd(J);
}

void SimStubReplActivateEquip(unsigned Seq, unsigned int ClientId, XActivateEquip const& Info)
{
    FOracleJson J;
    if (!Begin(J, Seq, ClientId, "SendCommonActivateEquip", Info.ObjectId))
        return;
    J.Uint("equip", Info.EquipId);
    J.Bool("on", Info.bActivate);
    SimStubReplicationEnd(J);
}

void SimStubReplActivateCruise(unsigned Seq, unsigned int ClientId, XActivateCruise const& Info)
{
    FOracleJson J;
    if (!Begin(J, Seq, ClientId, "SendCommonActivateCruise", Info.ObjectId))
        return;
    J.Bool("on", Info.bActivate);
    J.Uint("arg", Info.Field_0x05);
    SimStubReplicationEnd(J);
}

void SimStubReplActivateThrusters(unsigned Seq, unsigned int ClientId, XActivateThrusters const& Info)
{
    FOracleJson J;
    if (!Begin(J, Seq, ClientId, "SendCommonActivateThrusters", Info.ObjectId))
        return;
    J.Bool("on", Info.bActivate);
    SimStubReplicationEnd(J);
}

void SimStubReplSetTarget(unsigned Seq, unsigned int ClientId, XSetTarget const& Info)
{
    FOracleJson J;
    if (!Begin(J, Seq, ClientId, "SendCommonSetTarget", Info.ObjectId))
        return;
    J.Uint("sub", Info.SubObjId);
    J.Uint("target", Info.TargetObjectId);
    J.Uint("target_sub", Info.TargetSubObjId);
    SimStubReplicationEnd(J);
}

void SimStubReplGoTradeLane(unsigned Seq, unsigned int ClientId, XGoTradelane const& Info)
{
    FOracleJson J;
    if (!Begin(J, Seq, ClientId, "SendCommonGoTradeLane", Info.ObjectId))
        return;
    J.Uint("ring1", Info.TradelaneRing1);
    J.Uint("ring2", Info.TradelaneRing2);
    J.Bool("flag", Info.Field_0x0c);
    SimStubReplicationEnd(J);
}

void SimStubReplStopTradeLane(unsigned Seq, unsigned int ClientId, unsigned int ShipId, unsigned int TradelaneRing1,
                              unsigned int TradelaneRing2)
{
    FOracleJson J;
    if (!Begin(J, Seq, ClientId, "SendCommonStopTradeLane", ShipId))
        return;
    J.Uint("ring1", TradelaneRing1);
    J.Uint("ring2", TradelaneRing2);
    SimStubReplicationEnd(J);
}

void SimStubReplJettisonCargo(unsigned Seq, unsigned int ClientId, XJettisonCargo const& Info)
{
    FOracleJson J;
    if (!Begin(J, Seq, ClientId, "SendCommonJettisonCargo", Info.ObjectId))
        return;
    J.Uint("item", Info.ItemId);
    J.Uint("count", Info.Count);
    SimStubReplicationEnd(J);
}

void SimStubReplUpdateObject(unsigned Seq, unsigned int ClientId, FCommonUpdateObjectInfo const& Info)
{
    FOracleJson J;
    if (!Begin(J, Seq, ClientId, "SendCommonUpdateObject", Info.ObjectId))
        return;
    VectorJ(J, "pos", Info.Position);
    QuaternionJ(J, "q", Info.Orientation);
    J.Float("time", Info.Field_0x20);
    J.Float("throttle", Info.Field_0x24);
    J.Uint("behavior", Info.Field_0x28);
    SimStubReplicationEnd(J);
}

void SimStubReplCreateSolar(unsigned Seq, unsigned int ClientId, CSolarCreateInfo const& Info)
{
    FOracleJson J;
    if (!Begin(J, Seq, ClientId, "SendServerCreateSolar", Info.ObjId))
        return;
    J.Uint("arch", Info.ArchetypeNickname);
    VectorJ(J, "pos", Info.Position);
    QuaternionJ(J, "q", Info.Orientation);
    SimStubReplicationEnd(J);
}

void SimStubReplCreateShip(unsigned Seq, unsigned int ClientId, CShipCreateInfo const& Info)
{
    FOracleJson J;
    if (!Begin(J, Seq, ClientId, "SendServerCreateShip", Info.ObjId))
        return;
    J.Uint("arch", Info.ArchetypeNickname);
    VectorJ(J, "pos", Info.Position);
    QuaternionJ(J, "q", Info.Orientation);
    VectorJ(J, "vel", Info.Velocity);
    VectorJ(J, "angvel", Info.AngularVelocity);
    J.Int("hp", Info.HitPts);
    J.Float("throttle", Info.Throttle);
    J.Uint("owner", Info.OwnerPlayer);
    J.Uint("lane", Info.bUsingTradelane);
    int const Equipment = Info.Equipment.Begin != nullptr
                              ? static_cast<int>(Info.Equipment.End - Info.Equipment.Begin)
                              : 0;
    J.Int("equip", Equipment);
    SimStubReplicationEnd(J);
}

void SimStubReplCreateLoot(unsigned Seq, unsigned int ClientId, FServerCreateLootRequest const& Info)
{
    // The observer's create is one container; its id is the first record's.
    unsigned int const First = Info.Items != nullptr && Info.Count > 0 ? Info.Items[0].LootId : 0u;
    FOracleJson J;
    if (!Begin(J, Seq, ClientId, "SendServerCreateLoot", First))
        return;
    J.Uint("owner", Info.OwnerId);
    J.Uint("kind", Info.Kind);
    J.BeginArray("items");
    for (unsigned Index = 0; Info.Items != nullptr && Index < Info.Count; ++Index)
    {
        FServerLootDesc const& Item = Info.Items[Index];
        J.BeginObject(nullptr);
        J.Uint("id", Item.LootId);
        J.Uint("contents", Item.ContentsNickname);
        J.Uint("units", Item.Units);
        J.Uint("container", Item.ContainerNickname);
        VectorJ(J, "pos", Item.Position);
        J.EndObject();
    }
    J.EndArray();
    SimStubReplicationEnd(J);
}

namespace
{
    void Projectile(unsigned Seq, unsigned int ClientId, char const* Method, FServerCreateProjectileInfo const& Info)
    {
        FOracleJson J;
        if (!Begin(J, Seq, ClientId, Method, Info.ObjId))
            return;
        J.Uint("arch", Info.ArchetypeNickname);
        J.Uint("owner", Info.OwnerId);
        J.Uint("sub", Info.SubObjId);
        VectorJ(J, "pos", Info.Position);
        QuaternionJ(J, "q", Info.Orientation);
        VectorJ(J, "vel", Info.Velocity);
        SimStubReplicationEnd(J);
    }
}

void SimStubReplCreateMine(unsigned Seq, unsigned int ClientId, FServerCreateProjectileInfo const& Info)
{
    Projectile(Seq, ClientId, "SendServerCreateMine", Info);
}

void SimStubReplCreateCounter(unsigned Seq, unsigned int ClientId, FServerCreateProjectileInfo const& Info)
{
    Projectile(Seq, ClientId, "SendServerCreateCounter", Info);
}

void SimStubReplCreateGuided(unsigned Seq, unsigned int ClientId, FServerCreateGuidedInfo const& Info)
{
    FOracleJson J;
    if (!Begin(J, Seq, ClientId, "SendServerCreateGuided", Info.ObjId))
        return;
    J.Uint("arch", Info.ArchetypeNickname);
    J.Uint("owner", Info.OwnerId);
    J.Uint("sub", Info.SubObjId);
    J.Uint("target", Info.TargetId);
    J.Uint("target_sub", Info.TargetSubObjId);
    VectorJ(J, "pos", Info.Position);
    QuaternionJ(J, "q", Info.Orientation);
    SimStubReplicationEnd(J);
}

void SimStubReplSetGuidedTarget(unsigned Seq, unsigned int ClientId, unsigned int GuidedId, unsigned int TargetId,
                                unsigned short TargetSubObjId)
{
    FOracleJson J;
    if (!Begin(J, Seq, ClientId, "SendServerSetGuidedTarget", GuidedId))
        return;
    J.Uint("target", TargetId);
    J.Uint("target_sub", TargetSubObjId);
    SimStubReplicationEnd(J);
}

void SimStubReplDestroyObject(unsigned Seq, unsigned int ClientId, FServerDestroyObjectInfo const& Info)
{
    FOracleJson J;
    if (!Begin(J, Seq, ClientId, "SendServerDestroyObject", Info.ObjectId))
        return;
    J.Uint("kind", Info.Field_0x04);
    SimStubReplicationEnd(J);
}

void SimStubReplActivateObject(unsigned Seq, unsigned int ClientId, FPayloadServerActivateObject const& Info)
{
    FOracleJson J;
    if (!Begin(J, Seq, ClientId, "SendServerActivateObject", Info.ObjectId))
        return;
    J.Uint("on", Info.bActivate);
    J.Uint("index", Info.Field_0x08);
    SimStubReplicationEnd(J);
}

void SimStubReplSystemSwitchOut(unsigned Seq, unsigned int ClientId, FServerSystemSwitchOutInfo const& Info)
{
    FOracleJson J;
    if (!Begin(J, Seq, ClientId, "SendServerSystemSwitchOut", Info.ShipId))
        return;
    J.Uint("gate", Info.GateId);
    SimStubReplicationEnd(J);
}

void SimStubReplSystemSwitchIn(unsigned Seq, unsigned int ClientId, FPayloadServerSystemSwitchIn const& Info)
{
    FOracleJson J;
    if (!Begin(J, Seq, ClientId, "SendServerSystemSwitchIn", Info.Field_0x00))
        return;
    J.Uint("bits", Info.Field_0x04);
    VectorJ(J, "pos", Info.Position);
    QuaternionJ(J, "q", Info.Orientation);
    SimStubReplicationEnd(J);
}

void SimStubReplLand(unsigned Seq, unsigned int ClientId, FServerLandInfo const& Info)
{
    FOracleJson J;
    if (!Begin(J, Seq, ClientId, "SendServerLand", Info.ShipId))
        return;
    J.Uint("dock", Info.DockId);
    J.Uint("base", Info.BaseId);
    SimStubReplicationEnd(J);
}

void SimStubReplLaunch(unsigned Seq, unsigned int ClientId, FServerLaunchInfo const& Info)
{
    FOracleJson J;
    if (!Begin(J, Seq, ClientId, "SendServerLaunch", Info.Field_0x00))
        return;
    J.Uint("dock", Info.Field_0x04);
    J.Uint("index", Info.Field_0x08);
    VectorJ(J, "pos", Info.Position);
    QuaternionJ(J, "q", Info.Orientation);
    SimStubReplicationEnd(J);
}

void SimStubReplDamageObject(unsigned Seq, unsigned int ClientId, unsigned int ObjectId, DamageList const& Damage)
{
    FOracleJson J;
    if (!Begin(J, Seq, ClientId, "SendServerDamageObject", ObjectId))
        return;
    J.Bool("destroyed", Damage.bDestroyed);
    J.Int("cause", static_cast<int>(Damage.Cause));
    J.Uint("inflictor", Damage.InflictorId);
    J.BeginArray("entries");
    for (std::list<DamageEntry>::const_iterator It = Damage.Entries.begin(); It != Damage.Entries.end(); ++It)
    {
        J.BeginArray(nullptr);
        J.Uint(nullptr, It->SubObjId);
        J.Float(nullptr, It->HitPts);
        J.Int(nullptr, static_cast<int>(It->Fate));
        J.EndArray();
    }
    J.EndArray();
    SimStubReplicationEnd(J);
}

void SimStubReplSetReputation(unsigned Seq, unsigned int ClientId, FServerSetReputationInfo const& Info)
{
    // No object id: the vibe is the record's subject.
    FOracleJson J;
    if (!Begin(J, Seq, ClientId, "SendServerSetReputation", 0u))
        return;
    J.Int("vibe", Info.VibeHandle);
    J.Uint("affiliation", Info.Field_0x08);
    SimStubReplicationEnd(J);
}

void SimStubReplBurnFuse(unsigned Seq, unsigned int ClientId, FServerBurnFuseInfo const& Info)
{
    FOracleJson J;
    if (!Begin(J, Seq, ClientId, "SendServerBurnFuse", Info.ObjectId))
        return;
    J.Uint("source", Info.SourceId);
    J.Uint("fuse", Info.FuseId);
    J.Uint("sub", Info.SubObjId);
    J.Float("at", Info.AtT);
    J.Float("lifetime", Info.Lifetime);
    J.Uint("lit", Info.bLit);
    SimStubReplicationEnd(J);
}
