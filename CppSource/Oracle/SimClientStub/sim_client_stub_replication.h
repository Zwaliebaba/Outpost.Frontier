// Oracle instrumentation (work package P3.3, decision record 0007 point 11).

#pragma once

// replication.jsonl, the replication record of Oracle.exe sim: one line for each packet the
// server sends a client that the replication comparison reads (the creates, destroys,
// updates and damage of the observers, the fan-outs and the system switches), with the fields
// that matter for replication. The generated stub (sim_client_stub_iclient.cpp, from
// make_sim_client_stub.py's REPLICATED table) calls these after it has written the call to
// iclient.jsonl, with that call's seq. Nothing is written until the harness names the folder
// (SimClientStubRecordReplication, the scenario's `dump replication`). Record layout:
// design/sim-records.md, `replication.jsonl`; the Rust side is
// crates/game-logic/tests/replication_oracle.rs.

#include "rpc_iclient.h"   // the argument types, declared

// Channel 1: the fan-outs and the object updates.
void SimStubReplFireWeapon(unsigned Seq, unsigned int ClientId, XFireWeaponInfo const& Info);
void SimStubReplActivateEquip(unsigned Seq, unsigned int ClientId, XActivateEquip const& Info);
void SimStubReplActivateCruise(unsigned Seq, unsigned int ClientId, XActivateCruise const& Info);
void SimStubReplActivateThrusters(unsigned Seq, unsigned int ClientId, XActivateThrusters const& Info);
void SimStubReplSetTarget(unsigned Seq, unsigned int ClientId, XSetTarget const& Info);
void SimStubReplGoTradeLane(unsigned Seq, unsigned int ClientId, XGoTradelane const& Info);
void SimStubReplStopTradeLane(unsigned Seq, unsigned int ClientId, unsigned int ShipId, unsigned int TradelaneRing1,
                              unsigned int TradelaneRing2);
void SimStubReplJettisonCargo(unsigned Seq, unsigned int ClientId, XJettisonCargo const& Info);
void SimStubReplUpdateObject(unsigned Seq, unsigned int ClientId, FCommonUpdateObjectInfo const& Info);

// Channel 2: the creates, the destroys, the damage and the rest.
void SimStubReplCreateSolar(unsigned Seq, unsigned int ClientId, CSolarCreateInfo const& Info);
void SimStubReplCreateShip(unsigned Seq, unsigned int ClientId, CShipCreateInfo const& Info);
void SimStubReplCreateLoot(unsigned Seq, unsigned int ClientId, FServerCreateLootRequest const& Info);
void SimStubReplCreateMine(unsigned Seq, unsigned int ClientId, FServerCreateProjectileInfo const& Info);
void SimStubReplCreateGuided(unsigned Seq, unsigned int ClientId, FServerCreateGuidedInfo const& Info);
void SimStubReplCreateCounter(unsigned Seq, unsigned int ClientId, FServerCreateProjectileInfo const& Info);
void SimStubReplSetGuidedTarget(unsigned Seq, unsigned int ClientId, unsigned int GuidedId, unsigned int TargetId,
                                unsigned short TargetSubObjId);
void SimStubReplDestroyObject(unsigned Seq, unsigned int ClientId, FServerDestroyObjectInfo const& Info);
void SimStubReplActivateObject(unsigned Seq, unsigned int ClientId, FPayloadServerActivateObject const& Info);
void SimStubReplSystemSwitchOut(unsigned Seq, unsigned int ClientId, FServerSystemSwitchOutInfo const& Info);
void SimStubReplSystemSwitchIn(unsigned Seq, unsigned int ClientId, FPayloadServerSystemSwitchIn const& Info);
void SimStubReplLand(unsigned Seq, unsigned int ClientId, FServerLandInfo const& Info);
void SimStubReplLaunch(unsigned Seq, unsigned int ClientId, FServerLaunchInfo const& Info);
void SimStubReplDamageObject(unsigned Seq, unsigned int ClientId, unsigned int ObjectId, DamageList const& Damage);
void SimStubReplSetReputation(unsigned Seq, unsigned int ClientId, FServerSetReputationInfo const& Info);
void SimStubReplBurnFuse(unsigned Seq, unsigned int ClientId, FServerBurnFuseInfo const& Info);
