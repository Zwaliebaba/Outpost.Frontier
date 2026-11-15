// Author: Giperion.

#pragma once

// IClientImpl -- OutpostFrontier.exe's receiver of the server's IClient calls.

// baseline class IClientImpl (RTTI module storage ".?AVIClientImpl@@", reached from the
// complete-object locator module storage that precedes its IClient vftable at
// , 126 entries). ONE object carries both interfaces: the static initializer
// stores the IClient vptr (module storage) at .data:Client+0, the IChatClient
// vptr (module storage, one slot) at +4 and zero at +8, and GetChatClientInterface returns
// Client+4 -- the IChatClient sub-object of the same instance, not a second object. The
// object (`Client`, ordinal 66) and the two getters live in client_exports.cpp.

// The slot bodies are split along baseline's translation units:
//   client_iclient_space.cpp -- IClientS.cpp (module storage..0053e4a0, the FDUMP path string
// ), the space side;
//   client_iclient.cpp       -- everything else: the GF mirror forwards (module storage..
// ), the player-state / chat / trade block (.text:
// ), the login forwards (module storage..
// ) and the three shared stubs (0052fba0, 00401e90,
// ).

// The argument COUNT of every slot is baseline's RET n / 4 (rpc_iclient.h); the first argument
// is the client (connection) id, which almost no receiver reads.

#include "rpc_iclient.h"

#include <string>
#include <vector>

class IClientImpl : public IClient, public IChatClient
{
public:
    IClientImpl();

    // IClient, in slot order (rpc_iclient.h names each slot and its baseline address).
    void SendCommonFireWeapon(unsigned int ClientId, const XFireWeaponInfo& Info) override;
    void SendCommonActivateEquip(unsigned int ClientId, const XActivateEquip& Info) override;
    void SendCommonActivateCruise(unsigned int ClientId, const XActivateCruise& Info) override;
    void SendCommonActivateThrusters(unsigned int ClientId, const XActivateThrusters& Info) override;
    void SendCommonSetTarget(unsigned int ClientId, const XSetTarget& Info) override;
    void SendCommonTractorObjects(unsigned int ClientId, const XTractorObjects& Info) override;
    void SendCommonGoTradeLane(unsigned int ClientId, const XGoTradelane& Info) override;
    void SendCommonStopTradeLane(unsigned int ClientId, unsigned int ShipId, unsigned int TradelaneRing1,
                                 unsigned int TradelaneRing2) override;
    void SendCommonJettisonCargo(unsigned int ClientId, const XJettisonCargo& Info) override;
    void PushToClient(unsigned int ClientId, CDAPacket* Packet) override;
    bool Startup(unsigned int MaxPlayers, unsigned int Port) override;
    void OnConnect(unsigned int ClientId) override;
    void SendServerLoginResponse(unsigned int ClientId, const unsigned char& Status) override;
    void SendServerCharacterInfo(unsigned int ClientId, const CCharacterSet& Characters) override;
    void SendServerCharSelectVerified(unsigned int ClientId, const FServerCharSelectVerifiedInfo& Info) override;
    void Shutdown() override;
    int Update() override;
    void DisconnectClient(unsigned int ClientId) override;
    unsigned int GetSendQSize(unsigned int ClientId) override;
    unsigned int GetSendQBytes(unsigned int ClientId) override;
    double GetLinkSaturation(unsigned int ClientId) override;
    void SendServerSetShipArch(unsigned int ClientId, unsigned int ShipArchId) override;
    void SendServerSetHullStatus(unsigned int ClientId, float HullStatus) override;
    void SendServerSetCollisionGroups(unsigned int ClientId, const std::list<CollisionGroupDesc>& Groups) override;
    void SendServerSetEquipment(unsigned int ClientId, const EquipDescVector& Equipment) override;
    void SendServerSetCargo(unsigned int ClientId, const EquipDescVector& Cargo) override;
    void SendServerSetAddItem(unsigned int ClientId, int AddResult, const EquipDesc& Item) override;
    void SendServerSetRemoveItem(unsigned int ClientId, int RemoveResult, unsigned short ItemId,
                                 int Count) override;
    void SendServerSetStartRoom(unsigned int ClientId, unsigned int BaseId, unsigned int RoomId) override;
    void SendServerGFDestroyCharacter(unsigned int ClientId, unsigned int RoomId, unsigned int Key) override;
    void SendServerGFUpdateChar(unsigned int ClientId, const unsigned char* Data, unsigned int Size) override;
    void SendServerGFCompleteCharList(unsigned int ClientId, unsigned int RoomId) override;
    void SendServerGFScriptBehavior(unsigned int ClientId, const unsigned char* Data, unsigned int Size) override;
    void SendServerGFDestroyScriptBehavior(unsigned int ClientId, unsigned int RoomId,
                                           unsigned int BehaviorId) override;
    void SendServerGFCompleteScriptBehaviorList(unsigned int ClientId, unsigned int RoomId) override;
    void SendServerGFUpdateAmbientScript(unsigned int ClientId, const unsigned char* Data,
                                         unsigned int Size) override;
    void SendServerGFDestroyAmbientScript(unsigned int ClientId, unsigned int RoomId, unsigned int Key) override;
    void SendServerGFCompleteAmbientScriptList(unsigned int ClientId, unsigned int RoomId) override;
    void SendServerGFDestroyMissionComputer(unsigned int ClientId, unsigned int BaseId, unsigned int Key) override;
    void SendServerGFUpdateMissionComputer(unsigned int ClientId, const unsigned char* Data,
                                           unsigned int Size) override;
    void SendServerGFCompleteMissionComputerList(unsigned int ClientId, unsigned int BaseId) override;
    void SendServerGFMissionVendorAcceptance(unsigned int ClientId, const unsigned char* Data,
                                             unsigned int Size) override;
    void SendServerGFMissionVendorWhyEmpty(unsigned int ClientId, unsigned int Reason) override;
    void SendServerGFDestroyNewsBroadcast(unsigned int ClientId, unsigned int BaseId, unsigned int Key) override;
    void SendServerGFUpdateNewsBroadcast(unsigned int ClientId, const unsigned char* Data,
                                         unsigned int Size) override;
    void SendServerGFCompleteNewsBroadcastList(unsigned int ClientId, unsigned int BaseId) override;
    void SendServerCreateSolar(unsigned int ClientId, const CSolarCreateInfo& Info) override;
    void SendServerCreateShip(unsigned int ClientId, const CShipCreateInfo& Info) override;
    void SendServerCreateLoot(unsigned int ClientId, const FServerCreateLootRequest& Info) override;
    void SendServerCreateMine(unsigned int ClientId, const FServerCreateProjectileInfo& Info) override;
    void SendServerCreateGuided(const unsigned int& ClientId, const FServerCreateGuidedInfo& Info) override;
    void SendServerCreateCounter(const unsigned int& ClientId, const FServerCreateProjectileInfo& Info) override;
    void SendServerExplodeAsteroidMine(unsigned int ClientId, const FMineExplosion& Info) override;
    void SendServerSetGuidedTarget(unsigned int ClientId, unsigned int GuidedId, unsigned int TargetId,
                                   unsigned short TargetSubObjId) override;
    void SendCommonUpdateObject(unsigned int ClientId, const FCommonUpdateObjectInfo& Info) override;
    void SendServerDestroyObject(unsigned int ClientId, const FServerDestroyObjectInfo& Info) override;
    void SendServerActivateObject(unsigned int ClientId, const FPayloadServerActivateObject& Info) override;
    void SendServerSystemSwitchOut(unsigned int ClientId, const FServerSystemSwitchOutInfo& Info) override;
    void SendServerSystemSwitchIn(unsigned int ClientId, const FPayloadServerSystemSwitchIn& Info) override;
    void SendServerLand(unsigned int ClientId, const FServerLandInfo& Info) override;
    void SendServerLaunch(unsigned int ClientId, const FServerLaunchInfo& Info) override;
    void SendServerRequestCreateShipResp(unsigned int ClientId, bool bSuccess, unsigned int ShipId) override;
    void SendServerRequestSpaceScript(unsigned int ClientId, const FServerRequestSpaceScriptInfo& Info) override;
    void SendServerDamageObject(unsigned int ClientId, unsigned int ObjectId, const DamageList& Damage) override;
    void SendServerItemTractored(unsigned int ClientId, const FPayloadServerItemTractored& Info) override;
    void SendServerUseItem(unsigned int ClientId, const FPayloadServerUseItem& Info) override;
    void SendServerSetReputation(unsigned int ClientId, const FServerSetReputationInfo& Info) override;
    void SendServerAdjustAttitude(unsigned int ClientId, const FPayloadServerAdjustAttitude& Info) override;
    void SendServerSendComm(unsigned int ClientId, unsigned int SenderId, unsigned int ReceiverId,
                            unsigned int VoiceId, Costume CommCostume, unsigned int CommType,
                            const unsigned int* TextIds, int TextCount, unsigned int Mood, float Duration,
                            bool bReceiverIsOwnShip) override;
    void SendServerSendNNMessage(unsigned int ClientId, unsigned int MessageId) override;
    void SendServerSetMissionMessage(const unsigned int& ClientId, const CSetMissionMessage& Message) override;
    void SendServerReplaceMissionObjective(const unsigned int& ClientId,
                                           const FServerReplaceMissionObjectiveInfo& Info) override;
    void SendServerSetMissionObjectives(const unsigned int& ClientId,
                                        const FServerSetMissionObjectivesInfo& Info) override;
    void SendServerSetMissionObjectiveState(const unsigned int& ClientId,
                                            const FServerMissionObjectiveStateInfo& Info) override;
    void SendServerMissionSaveA(unsigned int ClientId, unsigned long MissionId) override;
    void SendServerMarkObj(const unsigned int& ClientId, unsigned int ObjectId, int MarkType) override;
    void SendServerCfgInterfaceNotification(const unsigned int& ClientId,
                                            const FPayloadServerCfgInterfaceNotification& Info) override;
    void SendServerSetCash(const unsigned int& ClientId, int Cash) override;
    void SendServerSetNumKills(const unsigned int& ClientId, int NumKills) override;
    void SendServerSetMissionSuccesses(const unsigned int& ClientId, int NumSuccesses) override;
    void SendServerSetMissionFailures(const unsigned int& ClientId, int NumFailures) override;
    void SendServerLoadHint(const unsigned int& ClientId, BaseHint* Hint) override;
    void SendServerSetGroupFeelings(unsigned int ClientId, const FServerSetGroupFeelingsInfo& Info) override;
    void SendServerRequestReturned(const unsigned int& ClientId, unsigned int ShipId, int RequestType,
                                   int Response, int Extra) override;
    void SendServerRepairObject(unsigned int ClientId, const FServerRepairObjectInfo& Info) override;
    void SendServerRemoteObjectCargoUpdate(const unsigned int& ClientId, unsigned int ObjectId,
                                           const EquipDescVector& Cargo, unsigned char Flag) override;
    void SendServerObjectCargoUpdate(const unsigned int& ClientId, unsigned int ObjectId,
                                     const EquipDesc& Item) override;
    void SendServerBurnFuse(const unsigned int& ClientId, const FServerBurnFuseInfo& Info) override;
    void SendServerSetCamera(const unsigned int& ClientId, const FPayloadServerSetCamera& Info) override;
    void SendServerRevertCamera(const unsigned int& ClientId) override;
    void SendServerSetStoryCue(const unsigned int& ClientId, const unsigned int& CueId) override;
    void SendCommonSetWeaponGroup(unsigned int ClientId, const unsigned char* Data, unsigned int Size) override;
    void SendCommonSetVisitedState(unsigned int ClientId, unsigned char* Data, unsigned int Size) override;
    void SendCommonRequestBestPath(unsigned int ClientId, const unsigned char* Data, unsigned int Size) override;
    void SendCommonRequestPlayerStats(unsigned int ClientId, const unsigned char* Data, unsigned int Size) override;
    void SendCommonPopUpDialog(unsigned int ClientId, const FmtStr& Title, const FmtStr& Text,
                               unsigned int Buttons) override;
    void SendCommonRequestGroupPositions(unsigned int ClientId, unsigned char* Data, unsigned int Size) override;
    void SendCommonSetMissionLog(unsigned int ClientId, unsigned char* Data, unsigned int Size) override;
    void SendCommonSetInterfaceState(unsigned int ClientId, unsigned char* Data, unsigned int Size) override;
    void SendCommonRequestRankLevel(unsigned int ClientId, unsigned char* Data, unsigned int Size) override;
    void SendServerSetDirective(const unsigned int& ClientId, const FPayloadServerSetDirective& Info) override;
    void SendCommonPlayerTradeInitiateTrade(unsigned int ClientId, unsigned int ShipId) override;
    void SendCommonPlayerTradeTerminateTrade(unsigned int ClientId, int Reason) override;
    void SendCommonPlayerTradeAcceptTrade(unsigned int ClientId, unsigned int ShipId, bool bAccepted) override;
    void SendCommonPlayerTradeSetTradeMoney(unsigned int ClientId, unsigned int ShipId, unsigned long Money) override;
    void SendCommonPlayerTradeAddTradeEquip(unsigned int ClientId, unsigned int ShipId, const EquipDesc& Item) override;
    void SendCommonPlayerTradeDelTradeEquip(unsigned int ClientId, unsigned int ShipId, const EquipDesc& Item) override;
    void SendCommonPlayerTradeRequestTrade(unsigned int ClientId, unsigned int ShipId) override;
    void SendCommonPlayerTradeStopTradeRequest(unsigned int ClientId, unsigned int ShipId) override;
    void SendServerScanNotify(unsigned int ClientId, unsigned int ScannerId, unsigned int GoodId) override;
    void SendServerPlayerListAdd(unsigned int ClientId, const wchar_t* Name, unsigned int PlayerId,
                                 bool bAlreadyPresent) override;
    void SendServerPlayerListRemove(unsigned int ClientId, unsigned int PlayerId) override;
    void SendServerPlayerListClear(unsigned int ClientId) override;
    void SendServerMiscObjUpdateObjectGroup(unsigned int ClientId, unsigned int ObjectId,
                                            unsigned int GroupId) override;
    void SendServerMiscObjUpdateMission(unsigned int ClientId, unsigned int MissionId,
                                        unsigned int MissionSetBy) override;
    void SendServerMiscObjUpdateMotd(unsigned int ClientId, const std::wstring& Motd) override;
    void SendServerMiscObjUpdateAffiliation(unsigned int ClientId, unsigned int ObjectId,
                                            unsigned int Affiliation) override;
    void SendServerMiscObjUpdateRank(unsigned int ClientId, unsigned int PlayerId, int Rank) override;
    void SendServerMiscObjUpdatePlayerGroup(unsigned int ClientId, unsigned int PlayerId,
                                            unsigned int GroupId) override;
    void SendServerMiscObjUpdateSystem(unsigned int ClientId, unsigned int PlayerId, unsigned int SystemId) override;
    void SendServerMiscObjUpdateFormation(unsigned int ClientId, const std::vector<unsigned int>& MemberIds) override;
    void SendServerFormationUpdate(unsigned int ClientId, unsigned int LeaderId, const Vector& Offset) override;
    void SendServerSetMusic(unsigned int ClientId, unsigned int SpaceMusic, unsigned int DangerMusic,
                            unsigned int BattleMusic, unsigned int OverrideMusic, bool bFlag, float Fade) override;
    void SendServerCancelMusic(unsigned int ClientId) override;
    void SendServerPlaySoundEffect(unsigned int ClientId, unsigned int SoundId) override;
    void DumpPacketStats(const char* Name) override;

    // IChatClient slot 0 (baseline module storage, ch1 CHATMSG).
    void SendCommonChatMsg(CHAT_ID TargetId, CHAT_ID SenderId, unsigned long Size, const void* Data) override;

    // +0x08, zeroed by module storage. TODO: no other reference to module storage in the
    // baseline listing -- meaning unrecovered.
    unsigned int Field_0x08;
};
static_assert(sizeof(IClientImpl) == 0x0c, "baseline Client spans module storage..0067ea6f");

// Only the Client data object crosses this boundary; the class has no export attribute.
#if defined(OUTPOST_FRONTIER_EXPORTS)
#define OUTPOST_FRONTIER_CLIENT_DATA_API __declspec(dllexport)
#else
#define OUTPOST_FRONTIER_CLIENT_DATA_API __declspec(dllimport)
#endif

extern "C" OUTPOST_FRONTIER_CLIENT_DATA_API IClientImpl Client;
