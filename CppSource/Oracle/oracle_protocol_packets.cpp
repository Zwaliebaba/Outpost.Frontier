// Oracle suites of work package P2.2 (protocol), packet part: every packet of
// RemoteCommon encoded through its own Encode, as RemoteClient.dll compiles the
// codecs (Oracle.vcxproj gives this file RemoteClient's definitions; the codec
// bodies are linked from oracle_net_packet_codecs.cpp and
// oracle_net_server_codecs.cpp). The packets are filled from the generator of
// oracle_protocol.h in the order of crates/game-sim/tests/support/fill.rs;
// each Make* function mirrors the Rust function of the same channel. No
// archetype is loaded, so every small archetype ID encodes as 0 and every
// maximum of hit points is 1. Each encoding is then decoded again by the real
// Decode and encoded once more (protocol_decode_*.jsonl, see DecodeLine).

#if !defined(REMOTECLIENT_EXPORTS)
#error "Oracle.vcxproj compiles this file with RemoteClient.dll's definitions"
#endif

#include "rpc_packet_codec.h"
#include "rpc_server_channel_codec.h"

#include "common_misc.h"
#include "common_net.h"

#include "oracle_support.h"

#include "oracle_protocol.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

using namespace oracle;
using oracle_protocol::Gen;
using oracle_protocol::Mode;
using oracle_protocol::ModeClient;
using oracle_protocol::ModeServer;
using oracle_protocol::ModeSingle;

namespace
{
    // --- Shared fills ----------------------------------------------------------

    unsigned char* RandomBytes(Gen& G, unsigned int Count)
    {
        if (Count == 0)
            return nullptr;
        unsigned char* Out = static_cast<unsigned char*>(::operator new(Count));
        for (unsigned int Index = 0; Index < Count; ++Index)
            Out[Index] = G.U8();
        return Out;
    }

    unsigned int* Dwords(Gen& G, unsigned int Count)
    {
        if (Count == 0)
            return nullptr;
        unsigned int* Out = static_cast<unsigned int*>(::operator new(Count * 4u));
        for (unsigned int Index = 0; Index < Count; ++Index)
            Out[Index] = G.U32();
        return Out;
    }

    void FillVector(Vector& Out, Gen& G)
    {
        Out.x = G.F32();
        Out.y = G.F32();
        Out.z = G.F32();
    }

    void FillQuaternion(Quaternion& Out, Gen& G)
    {
        Out.w = G.F32();
        Out.x = G.F32();
        Out.y = G.F32();
        Out.z = G.F32();
    }

    void FillUnitQuaternion(Quaternion& Out, Gen& G)
    {
        Out.w = G.Unit();
        Out.x = G.Unit();
        Out.y = G.Unit();
        Out.z = G.Unit();
    }

    // HARDPOINTS in fill.rs, by record index.
    const char* HardpointName(unsigned int Index)
    {
        static const char* const Names[4] = {nullptr, "HpWeapon01", "HpTurret_U1_03", "HpCustom7"};
        return Names[Index % 4u];
    }

    void SetRecordHardpoint(FRpcServerEquipRecord* Record, unsigned int Index)
    {
        const char* Name = HardpointName(Index);
        Record->HardpointSize = 0;
        Record->Hardpoint = nullptr;
        if (Name == nullptr)
            return;
        const unsigned int Size = (unsigned int)std::strlen(Name) + 1u;
        Record->HardpointSize = (unsigned short)Size;
        Record->Hardpoint = static_cast<char*>(::operator new(Size));
        std::memcpy(Record->Hardpoint, Name, Size);
    }

    // equip_record
    FRpcServerEquipRecord* EquipRecord(Gen& G, unsigned int Index)
    {
        FRpcServerEquipRecord* Record =
            static_cast<FRpcServerEquipRecord*>(::operator new(sizeof(FRpcServerEquipRecord)));
        Record->Count = G.I32();
        Record->Health = G.F32();
        Record->ArchId = G.U32();
        Record->Id = G.U16();
        Record->bEquipped = G.U8();
        Record->Field_0x0f = G.U8();
        SetRecordHardpoint(Record, Index);
        return Record;
    }

    // equip_records into a packet's handle; none leaves the handle null.
    void EquipRecords(FRpcServerEquipListPacketBase& Packet, Gen& G, unsigned int Count)
    {
        if (Count == 0)
            return;
        Packet.Equipment = new FRpcServerEquipHandle();
        for (unsigned int Index = 0; Index < Count; ++Index)
            Packet.Equipment->PushBack(EquipRecord(G, Index));
    }

    // collision_groups
    void CollisionGroups(std::list<CollisionGroupDesc>& Out, Gen& G, unsigned int Count)
    {
        for (unsigned int Index = 0; Index < Count; ++Index)
        {
            CollisionGroupDesc Group;
            Group.Id = G.U16();
            Group.Pad02 = 0;
            Group.Health = G.F32();
            Out.push_back(Group);
        }
    }

    void TextUnits(wchar_t* Out, Gen& G, unsigned int Count)
    {
        for (unsigned int Index = 0; Index < Count; ++Index)
            Out[Index] = (wchar_t)G.TextUnit();
        Out[Count] = L'\0';
    }

    void TextBytes(char* Out, Gen& G, unsigned int Count)
    {
        for (unsigned int Index = 0; Index < Count; ++Index)
            Out[Index] = (char)G.TextByte();
        Out[Count] = '\0';
    }

    // fire_value
    float FireValue(Gen& G, unsigned int Case)
    {
        if (Case == 0)
            return G.Unit() * 32.0f;
        if (Case == 1)
            return (float)((int)(G.U32() % 16001u) - 8000);
        return G.F32();
    }

    // The buffer messages: case 0 none, otherwise Length bytes.
    void FillBuffer(FRpcCommonBufferPacketBase& Packet, Gen& G, unsigned int Case, unsigned int Length)
    {
        if (Case == 0)
        {
            Packet.BorrowData(nullptr, 0);
            return;
        }
        Packet.AdoptData(RandomBytes(G, Length), Length);
    }

    template<typename T>
    T* Blob(Gen& G, unsigned int Case, unsigned int Length)
    {
        T* Packet = new T();
        if (Case != 0)
        {
            Packet->Data = RandomBytes(G, Length);
            Packet->Size = Length;
        }
        return Packet;
    }

    template<typename T>
    T* Pair(Gen& G)
    {
        T* Packet = new T();
        Packet->Field_0x00 = G.U32();
        Packet->Field_0x04 = G.U32();
        return Packet;
    }

    template<typename T>
    T* Triple(Gen& G)
    {
        T* Packet = new T();
        Packet->Field_0x00 = G.U32();
        Packet->Field_0x04 = G.U32();
        Packet->Field_0x08 = G.U32();
        return Packet;
    }

    template<typename T>
    T* Quad(Gen& G)
    {
        T* Packet = new T();
        Packet->Field_0x00 = G.U32();
        Packet->Field_0x04 = G.U32();
        Packet->Field_0x08 = G.U32();
        Packet->Field_0x0c = G.U32();
        return Packet;
    }

    template<typename T>
    T* ScalarU32(Gen& G)
    {
        T* Packet = new T();
        Packet->Value = G.U32();
        return Packet;
    }

    template<typename T>
    T* ScalarI32(Gen& G)
    {
        T* Packet = new T();
        Packet->Value = G.I32();
        return Packet;
    }

    template<typename T>
    T* ScalarU8(Gen& G)
    {
        T* Packet = new T();
        Packet->Value = G.U8();
        return Packet;
    }

    template<typename T>
    T* ScalarF32(Gen& G)
    {
        T* Packet = new T();
        Packet->Value = G.F32();
        return Packet;
    }

    // --- Channel 1 (fill::common) ----------------------------------------------

    const unsigned char TradeActions[9] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x03};

    FPacketBase* MakeCommon(unsigned int Id, unsigned int Case, Gen& G)
    {
        switch (Id)
        {
        case 0x10001u:
        {
            FRpcUpdateObjectPacket* P = new FRpcUpdateObjectPacket();
            P->Field_0x00 = G.U8();
            P->ObjectId = G.U32();
            FillVector(P->Position, G);
            FillUnitQuaternion(P->Orientation, G);
            P->Field_0x28 = G.Unit();
            P->UpdateTime = G.F32();
            return P;
        }
        case 0x10002u:
        {
            FRpcFireWeaponPacket* P = new FRpcFireWeaponPacket();
            P->ObjectId = G.U32();
            P->TargetPos.x = FireValue(G, Case);
            P->TargetPos.y = FireValue(G, Case);
            P->TargetPos.z = FireValue(G, Case);
            const unsigned int Counts[3] = {1u, 0u, 255u};
            for (unsigned int Index = 0; Index < Counts[Case]; ++Index)
                P->Hardpoints.push_back(G.U16());
            return P;
        }
        case 0x10004u:
        {
            FRpcSetTargetPacket* P = new FRpcSetTargetPacket();
            P->ObjectId = G.U32();
            P->TargetId = G.U32();
            P->SubObjId = G.U16();
            P->TargetSubObjId = G.U16();
            return P;
        }
        case 0x10005u:
        {
            FRpcChatMsgPacket* P = new FRpcChatMsgPacket();
            const unsigned int Size = Case == 0 ? 0u : 5u;
            P->Data = RandomBytes(G, Size);
            P->DataSize = Size;
            P->TargetId = G.U32();
            P->Field_0x04 = G.U32();
            return P;
        }
        case 0x10008u:
        {
            FRpcActivateEquipPacket* P = new FRpcActivateEquipPacket();
            P->ObjectId = G.U32();
            P->EquipId = G.U16();
            P->bActive = G.U8();
            return P;
        }
        case 0x1000eu:
        {
            FRpcActivateCruisePacket* P = new FRpcActivateCruisePacket();
            P->ObjectId = G.U32();
            P->bActive = G.U8();
            P->Field_0x05 = G.U8();
            return P;
        }
        case 0x1000fu:
        {
            FRpcGoTradeLanePacket* P = new FRpcGoTradeLanePacket();
            P->ObjectId = G.U32();
            P->TradelaneRing1 = G.U32();
            P->TradelaneRing2 = G.U32();
            P->Field_0x0c = G.U8();
            return P;
        }
        case 0x10010u:
        {
            FRpcStopTradeLanePacket* P = new FRpcStopTradeLanePacket();
            P->ShipId = G.U32();
            P->TradeLaneRing1 = G.U32();
            P->TradeLaneRing2 = G.U32();
            return P;
        }
        case 0x10011u:
        {
            FRpcSetWeaponGroupPacket* P = new FRpcSetWeaponGroupPacket();
            FillBuffer(*P, G, Case, 7);
            return P;
        }
        case 0x10013u:
        {
            FRpcSetVisitedStatePacket* P = new FRpcSetVisitedStatePacket();
            FillBuffer(*P, G, Case, 7);
            return P;
        }
        case 0x10016u:
        {
            FRpcRequestBestPathPacket* P = new FRpcRequestBestPathPacket();
            FillBuffer(*P, G, Case, 7);
            return P;
        }
        case 0x10017u:
        {
            FRpcRequestGroupPositionsPacket* P = new FRpcRequestGroupPositionsPacket();
            FillBuffer(*P, G, Case, 7);
            return P;
        }
        case 0x10018u:
        {
            FRpcRequestPlayerStatsPacket* P = new FRpcRequestPlayerStatsPacket();
            FillBuffer(*P, G, Case, 7);
            return P;
        }
        case 0x10019u:
        {
            FRpcSetMissionLogPacket* P = new FRpcSetMissionLogPacket();
            FillBuffer(*P, G, Case, 7);
            return P;
        }
        case 0x1001au:
        {
            FRpcRequestRankLevelPacket* P = new FRpcRequestRankLevelPacket();
            FillBuffer(*P, G, Case, 7);
            return P;
        }
        case 0x1001cu:
        {
            FRpcSetInterfaceStatePacket* P = new FRpcSetInterfaceStatePacket();
            FillBuffer(*P, G, Case, 7);
            return P;
        }
        case 0x10012u:
        {
            FRpcPlayerTradePacket* P = new FRpcPlayerTradePacket();
            const unsigned char Action = TradeActions[Case];
            P->Action = Action;
            switch (Action)
            {
            case 0x02u:
                P->Reason = (int)(signed char)G.U8();
                break;
            case 0x04u:
                P->Money = G.I32();
                break;
            case 0x08u:
                P->EquipArchId = G.U32();
                P->EquipCount = G.I32();
                P->EquipHealth = G.F32();
                P->EquipId = G.U16();
                break;
            case 0x10u:
                P->EquipArchId = G.U32();
                P->EquipCount = G.I32();
                P->EquipId = G.U16();
                break;
            case 0x40u:
                P->bAccepted = (unsigned char)(G.U8() & 1u);
                break;
            default:
                break;
            }
            if (Action == 0x01u || Action == 0x04u || Action == 0x08u || Action == 0x10u || Action == 0x20u ||
                Action == 0x40u || Action == 0x80u)
                P->TargetPlayer = G.U32();
            return P;
        }
        case 0x10014u:
        {
            FRpcJettisonCargoPacket* P = new FRpcJettisonCargoPacket();
            P->ObjectId = G.U32();
            P->ItemId = G.U16();
            P->Count = G.U32();
            return P;
        }
        case 0x10015u:
        {
            FRpcActivateThrustersPacket* P = new FRpcActivateThrustersPacket();
            P->ObjectId = G.U32();
            P->bActive = G.U8();
            return P;
        }
        case 0x1001bu:
        {
            FRpcPopUpDialogPacket* P = new FRpcPopUpDialogPacket();
            const unsigned int Size = Case == 0 ? 0u : 3u;
            P->DataA = RandomBytes(G, Size);
            P->SizeA = Size;
            P->DataB = RandomBytes(G, Size);
            P->SizeB = Size;
            P->Field_0x10 = G.U32();
            return P;
        }
        case 0x1001du:
        {
            FRpcTractorObjectsPacket* P = new FRpcTractorObjectsPacket();
            P->ObjectId = G.U32();
            P->TractorEquipId = G.U16();
            const unsigned int Count = Case == 0 ? 0u : 300u;
            for (unsigned int Index = 0; Index < Count; ++Index)
                P->TargetIds.push_back(G.U32());
            return P;
        }
        default:
            return nullptr;
        }
    }

    // --- Channel 3 (fill::client) ----------------------------------------------

    void FillItemChange(FRpcItemChangePacketBase& P, Gen& G, unsigned int Case, const char* Hardpoint)
    {
        P.Count = G.I32();
        P.Health = G.F32();
        P.bMounted = G.U8();
        if (Case != 0)
            P.CopyHardpoint(Hardpoint, (unsigned int)std::strlen(Hardpoint) + 1u);
    }

    FPacketBase* MakeClient(unsigned int Id, unsigned int Case, Gen& G)
    {
        const bool bLong = Case != 0;
        switch (Id)
        {
        case 0x30001u:
        {
            FRpcClientLoginPacket* P = new FRpcClientLoginPacket();
            std::memset(P->AccountName, 0, sizeof(P->AccountName));
            std::memset(P->Field_0x220, 0, sizeof(P->Field_0x220));
            P->CreateFlag = G.U8();
            if (bLong)
            {
                TextUnits(P->AccountName, G, 5);
                P->AccountName[5] = (wchar_t)0x00c0;
                P->AccountName[6] = L'\0';
                TextUnits(P->Field_0x220, G, 3);
            }
            return P;
        }
        case 0x30003u:
        {
            FRpcMunitionCollisionPacket* P = new FRpcMunitionCollisionPacket();
            P->Field_0x00 = G.U32();
            P->Field_0x04 = G.U32();
            P->ObjectId = G.U32();
            P->Field_0x0c = G.U16();
            FillVector(P->HitPosition, G);
            return P;
        }
        case 0x30004u: return ScalarU32<FRpcRequestLaunchPacket>(G);
        case 0x30007u: return ScalarU32<FRpcEnterBasePacket>(G);
        case 0x3000bu: return ScalarU32<FRpcSystemSwitchOutCompletePacket>(G);
        case 0x3000du: return ScalarU32<FRpcExitBasePacket>(G);
        case 0x3000eu: return ScalarU32<FRpcEnterLocationPacket>(G);
        case 0x3000fu: return ScalarU32<FRpcExitLocationPacket>(G);
        case 0x30013u: return ScalarU32<FRpcGfSelectObjectPacket>(G);
        case 0x30015u: return ScalarU32<FRpcRequestShipArchPacket>(G);
        case 0x3001bu: return ScalarI32<FRpcRequestSetCashPacket>(G);
        case 0x3001cu: return ScalarI32<FRpcRequestChangeCashPacket>(G);
        case 0x30024u: return ScalarU32<FRpcDockPacket>(G);
        case 0x30035u: return ScalarU32<FRpcInterfaceItemUsedPacket>(G);
        case 0x3003bu: return ScalarF32<FRpcRequestHullStatusPacket>(G);
        case 0x30041u: return ScalarU32<FRpcAbortMissionPacket>(G);
        case 0x30045u: return ScalarU32<FRpcMissionSaveBPacket>(G);
        case 0x30046u: return ScalarF32<FRpcRequestDifficultyScalePacket>(G);
        case 0x30047u: return ScalarU32<FRpcRtcDonePacket>(G);
        case 0x30005u: return new FRpcRequestCharInfoPacket();
        case 0x3000au: return new FRpcGfRequestShipInfoPacket();
        case 0x30010u: return new FRpcRequestCreateShipPacket();
        case 0x3002eu: return new FRpcDebugFreeSystemPacket();
        case 0x30038u: return new FRpcRequestNewCharInfoPacket();
        case 0x30042u: return new FRpcSkipAutosavePacket();
        case 0x30006u:
        {
            FRpcSelectCharacterPacket* P = new FRpcSelectCharacterPacket();
            TextBytes(P->CharacterId.Buffer, G, bLong ? 0x1ffu : 0u);
            return P;
        }
        case 0x3003au:
        {
            FRpcDestroyCharacterPacket* P = new FRpcDestroyCharacterPacket();
            TextBytes(P->CharacterId.Buffer, G, bLong ? 9u : 0u);
            return P;
        }
        case 0x30008u:
        {
            FRpcRequestBaseInfoPacket* P = new FRpcRequestBaseInfoPacket();
            P->Id = G.U32();
            P->bFull = G.U8();
            return P;
        }
        case 0x30009u:
        {
            FRpcRequestLocationInfoPacket* P = new FRpcRequestLocationInfoPacket();
            P->Id = G.U32();
            P->bFull = G.U8();
            return P;
        }
        case 0x3000cu:
        {
            FRpcObjectCollisionPacket* P = new FRpcObjectCollisionPacket();
            P->ObjectId = G.U32();
            P->SubObjId = G.U16();
            P->TargetObjectId = G.U32();
            P->TargetSubObjId = G.U16();
            P->Field_0x10 = G.F32();
            return P;
        }
        case 0x30011u: return Triple<FRpcGfGoodSellPacket>(G);
        case 0x30012u: return Quad<FRpcGfGoodBuyPacket>(G);
        case 0x30014u:
        {
            FRpcMissionResponsePacket* P = new FRpcMissionResponsePacket();
            P->Field_0x00 = G.U32();
            P->MissionId = G.U32();
            P->bAccepted = G.U8();
            return P;
        }
        case 0x30016u:
        {
            FRpcRequestEquipmentPacket* P = new FRpcRequestEquipmentPacket();
            EquipRecords(*P, G, bLong ? 4u : 0u);
            return P;
        }
        case 0x30017u:
        {
            FRpcRequestCargoPacket* P = new FRpcRequestCargoPacket();
            EquipRecords(*P, G, bLong ? 4u : 0u);
            return P;
        }
        case 0x30018u:
        {
            FRpcRequestAddItemPacket* P = new FRpcRequestAddItemPacket();
            P->ArchId = G.U32();
            FillItemChange(*P, G, Case, "HpTurret01");
            return P;
        }
        case 0x3001au:
        {
            FRpcRequestModifyItemPacket* P = new FRpcRequestModifyItemPacket();
            P->EquipId = G.U32();
            FillItemChange(*P, G, Case, "HpWeapon03");
            return P;
        }
        case 0x30019u:
        {
            FRpcRequestRemoveItemPacket* P = new FRpcRequestRemoveItemPacket();
            P->EquipId = G.U32();
            P->Count = G.I32();
            return P;
        }
        case 0x3001eu:
        {
            FRpcSaveGamePacket* P = new FRpcSaveGamePacket();
            P->Description = nullptr;
            P->DescriptionCount = 0;
            if (bLong)
            {
                wchar_t Units[9];
                for (int Index = 0; Index < 9; ++Index)
                    Units[Index] = (wchar_t)G.U16();
                P->CopyDescription(Units, 9);
            }
            return P;
        }
        case 0x30020u:
        {
            FRpcMineAsteroidPacket* P = new FRpcMineAsteroidPacket();
            P->SystemId = G.U32();
            FillVector(P->Position, G);
            P->LootId = G.U32();
            P->SecondaryLoot = G.U32();
            P->Quantity = G.U32();
            return P;
        }
        case 0x30022u:
        {
            FRpcDebugCreateShipPacket* P = new FRpcDebugCreateShipPacket();
            P->Field_0x00 = G.U32();
            P->Field_0x04 = G.U32();
            P->Field_0x08 = G.U32();
            P->Field_0x0c = G.U32();
            P->Field_0x10 = G.U32();
            FillVector(P->Field_0x14, G);
            FillVector(P->Field_0x20, G);
            FillQuaternion(P->Field_0x2c, G);
            P->Field_0x3c = G.U32();
            const unsigned int Wide = bLong ? 3u : 0u;
            if (Wide != 0)
            {
                P->WideData = static_cast<wchar_t*>(::operator new(Wide * 2u));
                for (unsigned int Index = 0; Index < Wide; ++Index)
                    P->WideData[Index] = (wchar_t)G.U16();
            }
            P->WideCount = Wide;
            P->DataSizeA = bLong ? 2u : 0u;
            P->DataA = RandomBytes(G, P->DataSizeA);
            P->DataSizeB = bLong ? 5u : 0u;
            P->DataB = RandomBytes(G, P->DataSizeB);
            P->Field_0x50 = G.U32();
            P->Field_0x54 = G.U32();
            P->Field_0x58 = G.U32();
            P->Field_0x5c = G.U32();
            P->ArrayCount = bLong ? 2u : 0u;
            P->ArrayData = Dwords(G, P->ArrayCount);
            return P;
        }
        case 0x30023u:
        {
            FRpcDebugLoadSystemPacket* P = new FRpcDebugLoadSystemPacket();
            P->DataSize = bLong ? 6u : 0u;
            P->Data = reinterpret_cast<char*>(RandomBytes(G, P->DataSize));
            return P;
        }
        case 0x30025u:
        {
            FRpcDebugDestroyObjectPacket* P = new FRpcDebugDestroyObjectPacket();
            P->Field_0x00 = G.U32();
            P->Field_0x04 = G.U16();
            return P;
        }
        case 0x3002bu: return Pair<FRpcCargoScanPacket>(G);
        case 0x3002du:
        {
            FRpcDebugConsolePacket* P = new FRpcDebugConsolePacket();
            P->Field_0x00 = G.U32();
            for (unsigned int Index = 0; Index < sizeof(P->Command); ++Index)
                P->Command[Index] = G.U8();
            return P;
        }
        case 0x3002fu:
        {
            FRpcSetManeuverPacket* P = new FRpcSetManeuverPacket();
            P->Field_0x00 = G.U32();
            P->ObjectId = G.U32();
            P->Field_0x08 = G.U32();
            return P;
        }
        case 0x30030u:
        {
            FRpcDebugRelocateShipPacket* P = new FRpcDebugRelocateShipPacket();
            P->Field_0x00 = G.U32();
            FillVector(P->Position, G);
            return P;
        }
        case 0x30031u:
        {
            FRpcRequestEventPacket* P = new FRpcRequestEventPacket();
            P->EventType = G.U8();
            P->Arg1 = G.U32();
            P->Arg2 = G.U32();
            P->Arg3 = G.U32();
            P->Field_0x10 = G.U8();
            return P;
        }
        case 0x30032u:
        {
            FRpcRequestCancelPacket* P = new FRpcRequestCancelPacket();
            P->EventType = G.U8();
            P->Arg1 = G.U32();
            P->Arg2 = G.U32();
            P->Field_0x0c = G.U8();
            return P;
        }
        case 0x30036u:
        {
            FRpcRequestCollisionGroupsPacket* P = new FRpcRequestCollisionGroupsPacket();
            CollisionGroups(P->Groups, G, bLong ? 3u : 0u);
            return P;
        }
        case 0x30037u: return Quad<FRpcCommCompletePacket>(G);
        case 0x30039u:
        {
            FRpcCreateNewCharacterPacket* P = new FRpcCreateNewCharacterPacket();
            std::memset(P->Name, 0, sizeof(P->Name));
            TextUnits(P->Name, G, bLong ? 23u : 0u);
            P->Field_0x80 = G.U32();
            P->Field_0x84 = G.U32();
            P->Field_0x88 = G.U32();
            P->Field_0x8c = G.U32();
            return P;
        }
        case 0x3003cu: return Triple<FRpcGfGoodVaporizedPacket>(G);
        case 0x3003du:
        {
            FRpcBadlandsObjectCollisionPacket* P = new FRpcBadlandsObjectCollisionPacket();
            P->ObjectId = G.U32();
            P->Field_0x08 = G.U16();
            P->Field_0x00 = G.U32();
            return P;
        }
        case 0x3003eu: return Pair<FRpcLaunchCompletePacket>(G);
        case 0x3003fu: return Triple<FRpcHailPacket>(G);
        case 0x30040u:
        {
            FRpcRequestUseItemPacket* P = new FRpcRequestUseItemPacket();
            P->ObjectId = G.U32();
            P->ItemId = G.U16();
            P->Count = G.U16();
            return P;
        }
        case 0x30043u: return Pair<FRpcJumpInCompletePacket>(G);
        case 0x30044u:
        {
            FRpcRequestInvincibilityPacket* P = new FRpcRequestInvincibilityPacket();
            P->ShipId = G.U32();
            P->Reason = G.U8();
            P->bInvincible = G.U8();
            return P;
        }
        default:
            return nullptr;
        }
    }

    // --- Channel 2 (fill::server) ----------------------------------------------

    // create_equip
    FRpcServerEquipRecord* CreateEquip(Gen& G, unsigned int Index)
    {
        FRpcServerEquipRecord* Record =
            static_cast<FRpcServerEquipRecord*>(::operator new(sizeof(FRpcServerEquipRecord)));
        Record->bEquipped = (unsigned char)(G.U8() & 1u);
        Record->Field_0x0f = (unsigned char)(G.U8() & 1u);
        if (Index % 3u == 0)
            Record->Count = 1;
        else if (Index % 3u == 1)
            Record->Count = (int)(G.U32() % 256u);
        else
            Record->Count = 256 + (int)(G.U32() % 0x1000u);
        Record->Health = Index % 2u == 0 ? 1.0f : G.Unit();
        Record->ArchId = G.U32();
        Record->Id = Index % 2u == 0 ? (unsigned short)(G.U16() & 0xffu) : (unsigned short)(G.U16() | 0x100u);
        SetRecordHardpoint(Record, Index);
        return Record;
    }

    // create_object
    void CreateObject(FRpcServerCreateObjectFields& F, Gen& G, unsigned int Count)
    {
        F.ObjId = G.U32();
        F.ArchetypeId = G.U32();
        F.DockingBaseId = G.U32();
        F.VibeGroupId = G.I32();
        F.Costume.Body = G.U32();
        F.Costume.Head = G.U32();
        F.Costume.Count = (int)Count;
        F.Costume.Ids = Dwords(G, Count);
        F.VoiceId = G.U32();
        FillVector(F.Position, G);
        FillUnitQuaternion(F.Orientation, G);
        F.HitPts = G.I32();
        F.Field_0x6c = G.U32();
        for (unsigned int Index = 0; Index < Count; ++Index)
            F.Equipment.PushBack(CreateEquip(G, Index));
        for (unsigned int Index = 0; Index < Count; ++Index)
        {
            CollisionGroupDesc Group;
            Group.Id = (unsigned short)G.U8();
            Group.Pad02 = 0;
            Group.Health = G.Unit();
            F.CollisionGroups.push_back(Group);
        }
        for (unsigned int Index = 0; Index < Count; ++Index)
        {
            DamageEntry Entry;
            Entry.SubObjId = G.U16();
            Entry.Fate = static_cast<DamageEntry::SubObjFate>(G.U32());
            Entry.HitPts = G.F32();
            F.Damage.Entries.push_back(Entry);
            ++F.Damage.Count;
        }
    }

    // costume(g, count) as a CHARACTERINFO block.
    FRpcServerCharacterIdBlock* IdBlock(Gen& G, unsigned int Count)
    {
        FRpcServerCharacterIdBlock* Block =
            static_cast<FRpcServerCharacterIdBlock*>(::operator new(sizeof(FRpcServerCharacterIdBlock)));
        Block->Field_0x00 = G.U32();
        Block->Field_0x04 = G.U32();
        Block->Field_0x08 = G.U32();
        Block->Field_0x0c = G.U32();
        Block->Count = (int)Count;
        Block->Ids = Dwords(G, Count);
        return Block;
    }

    // character_record
    FRpcServerCharacterRecord* CharacterRecord(Gen& G, unsigned int Index)
    {
        FRpcServerCharacterRecord* R = new FRpcServerCharacterRecord();
        TextBytes(R->Name.Buffer, G, 11);
        R->Field_0x200 = G.U8();
        R->Field_0x201 = G.U8();
        for (unsigned int Unit = 0; Unit < 5u + Index; ++Unit)
            R->TextA.push_back((wchar_t)G.TextUnit());
        for (unsigned int Unit = 0; Unit < Index * 2u; ++Unit)
            R->TextB.push_back((wchar_t)G.TextUnit());
        R->Field_0x224 = G.U32();
        R->Field_0x22c = G.U32();
        R->Field_0x228 = G.U32();
        R->Field_0x230 = G.U32();
        R->Field_0x234 = G.U32();
        R->Field_0x238 = G.U32();
        R->Field_0x23c = G.U32();
        R->Field_0x240 = G.U32();
        R->Field_0x244 = G.U32();
        R->Field_0x248 = G.U32();
        R->Field_0x24c = G.U32();
        const float Hull = G.F32();
        std::memcpy(&R->Field_0x250, &Hull, 4u);
        R->Field_0x254 = G.U32();
        R->Field_0x258 = G.U32();
        R->Field_0x25c = G.U32();
        if (Index == 0)
        {
            R->BlockA = IdBlock(G, 2);
            R->BlockB = IdBlock(G, 0);
        }
        const unsigned int Equipment = Index * 3u;
        for (unsigned int Item = 0; Item < Equipment; ++Item)
            R->Equipment.PushBack(EquipRecord(G, Item));
        R->Field_0x260 = (unsigned char)Equipment;
        CollisionGroups(R->CollisionGroups, G, Index + 1u);
        return R;
    }

    FRpcServerFlatTextBlock& Block(FRpcServerFlatTextBlock& Out, unsigned char* Data, unsigned int Size)
    {
        Out.Data = Data;
        Out.Size = Size;
        return Out;
    }

    unsigned char* ZeroBytes(unsigned int Count)
    {
        unsigned char* Out = static_cast<unsigned char*>(::operator new(Count));
        std::memset(Out, 0, Count);
        return Out;
    }

    ::Reputation::Relation* NewRelation(Gen& G)
    {
        ::Reputation::Relation* Relation =
            static_cast< ::Reputation::Relation*>(::operator new(sizeof(::Reputation::Relation)));
        Relation->Feeling = G.F32();
        Relation->GroupId = G.U32();
        return Relation;
    }

    FPacketBase* MakeServer(unsigned int Id, unsigned int Case, Gen& G)
    {
        const bool bLong = Case != 0;
        switch (Id)
        {
        case 0x20001u: return ScalarU32<FRpcConnectResponsePacket>(G);
        case 0x20002u: return ScalarU8<FRpcLoginResponsePacket>(G);
        case 0x2000eu: return ScalarU32<FRpcServerGfCompleteNewsBroadcastListPacket>(G);
        case 0x2000fu: return ScalarU32<FRpcServerGfCompleteCharListPacket>(G);
        case 0x20010u: return ScalarU32<FRpcServerGfCompleteMissionComputerListPacket>(G);
        case 0x20011u: return ScalarU32<FRpcServerGfCompleteScriptBehaviorListPacket>(G);
        case 0x20013u: return ScalarU32<FRpcServerGfCompleteAmbientScriptListPacket>(G);
        case 0x20023u: return ScalarU32<FRpcSetShipArchPacket>(G);
        case 0x20030u: return ScalarU32<FRpcServerSetCashPacket>(G);
        case 0x2003eu: return ScalarU32<FRpcServerSetNumKillsPacket>(G);
        case 0x2003fu: return ScalarU32<FRpcServerSetMissionSuccessesPacket>(G);
        case 0x20040u: return ScalarU32<FRpcServerSetMissionFailuresPacket>(G);
        case 0x20043u: return ScalarU32<FRpcServerSetStoryCuePacket>(G);
        case 0x20049u: return ScalarF32<FRpcSetHullStatusPacket>(G);
        case 0x20056u: return ScalarU32<FRpcServerSendNNMessagePacket>(G);
        case 0x20059u: return ScalarU32<FRpcServerPlaySoundEffectPacket>(G);
        case 0x2005au: return ScalarU8<FRpcServerGfMissionVendorWhyEmptyPacket>(G);
        case 0x2005bu: return ScalarU32<FRpcServerMissionSaveAPacket>(G);
        case 0x2004cu: return new FRpcServerRevertCameraPacket();
        case 0x20058u: return new FRpcServerCancelMusicPacket();
        case 0x20024u:
        {
            FRpcServerSetEquipmentPacket* P = new FRpcServerSetEquipmentPacket();
            EquipRecords(*P, G, bLong ? 4u : 0u);
            return P;
        }
        case 0x20025u:
        {
            FRpcServerSetCargoPacket* P = new FRpcServerSetCargoPacket();
            EquipRecords(*P, G, bLong ? 4u : 0u);
            return P;
        }
        case 0x2001au: return Blob<FRpcServerGfScriptBehaviorPacket>(G, Case, 9);
        case 0x2001du: return Blob<FRpcServerGfUpdateMissionComputerPacket>(G, Case, 9);
        case 0x2001eu: return Blob<FRpcServerGfUpdateNewsBroadcastPacket>(G, Case, 9);
        case 0x2001fu: return Blob<FRpcServerGfUpdateAmbientScriptPacket>(G, Case, 9);
        case 0x20020u: return Blob<FRpcServerGfMissionVendorAcceptancePacket>(G, Case, 9);
        case 0x20026u: return Blob<FRpcServerGfUpdateCharPacket>(G, Case, 9);
        case 0x2000du: return Pair<FRpcServerSetStartRoomPacket>(G);
        case 0x20014u: return Pair<FRpcServerGfDestroyNewsBroadcastPacket>(G);
        case 0x20015u: return Pair<FRpcServerGfDestroyCharacterPacket>(G);
        case 0x20016u: return Pair<FRpcServerGfDestroyMissionComputerPacket>(G);
        case 0x20017u: return Pair<FRpcServerGfDestroyScriptBehaviorPacket>(G);
        case 0x20019u: return Pair<FRpcServerGfDestroyAmbientScriptPacket>(G);
        case 0x20021u: return Pair<FRpcServerSystemSwitchOutPacket>(G);
        case 0x20039u: return Pair<FRpcServerScanNotifyPacket>(G);
        case 0x2000bu: return Triple<FRpcServerLandPacket>(G);
        case 0x2002fu: return Triple<FRpcServerSetRemoveItemPacket>(G);
        case 0x20003u:
        {
            FRpcServerCharacterInfoPacket* P = new FRpcServerCharacterInfoPacket();
            const unsigned int Counts[3] = {0u, 1u, 2u};
            for (unsigned int Index = 0; Index < Counts[Case]; ++Index)
                P->Records.push_back(CharacterRecord(G, Index));
            P->SaveFailureCount = G.U32();
            return P;
        }
        case 0x20004u:
        {
            FRpcServerCreateShipPacket* P = new FRpcServerCreateShipPacket();
            const unsigned int Counts[3] = {0u, 1u, 3u};
            CreateObject(P->Object, G, Counts[Case]);
            P->bUsingTradelane = (unsigned char)(G.U8() & 1u);
            P->bMarkedBy = (unsigned char)(G.U8() & 1u);
            FillVector(P->Velocity, G);
            P->Throttle = G.Unit();
            P->Field_0xc0 = G.U16();
            P->ReputationRank = G.U8();
            if (Case == 1)
            {
                P->OwnerPlayer = (unsigned int)(G.U8() | 1u);
                P->Field_0xc4 = G.U16();
                P->DisplayName = static_cast<wchar_t*>(::operator new(7u * 2u + 2u));
                TextUnits(P->DisplayName, G, 7);
                P->DisplayNameLength = 7;
            }
            P->NameASize = Case == 2 ? 6u : 0u;
            P->NameA = RandomBytes(G, P->NameASize);
            P->NameBSize = Case == 2 ? 3u : 0u;
            P->NameB = RandomBytes(G, P->NameBSize);
            P->Affiliation = G.U32();
            P->Attitude = G.Unit();
            return P;
        }
        case 0x20005u:
        {
            FRpcServerDamageObjectPacket* P = new FRpcServerDamageObjectPacket();
            P->ObjectId = G.U32();
            P->Damage.bDestroyed = (G.U8() & 1u) != 0;
            P->Damage.Cause = static_cast<DamageCause>(G.U32());
            P->Damage.InflictorId = G.U32();
            for (unsigned int Index = 0; Index < (bLong ? 3u : 0u); ++Index)
            {
                DamageEntry Entry;
                Entry.SubObjId = G.U16();
                Entry.Fate = static_cast<DamageEntry::SubObjFate>(G.U32());
                Entry.HitPts = G.F32();
                P->Damage.Entries.push_back(Entry);
                ++P->Damage.Count;
            }
            return P;
        }
        case 0x20006u:
        {
            FRpcServerDestroyObjectPacket* P = new FRpcServerDestroyObjectPacket();
            P->ObjectId = G.U32();
            P->Field_0x04 = G.U8();
            return P;
        }
        case 0x20007u:
        {
            FRpcServerLaunchPacket* P = new FRpcServerLaunchPacket();
            P->Field_0x00 = G.U32();
            P->Field_0x04 = G.U32();
            P->Field_0x08 = G.U32();
            FillVector(P->Position, G);
            FillQuaternion(P->Orientation, G);
            return P;
        }
        case 0x20008u:
        {
            FRpcServerCharSelectVerifiedPacket* P = new FRpcServerCharSelectVerifiedPacket();
            P->RepInstance = G.U32();
            const unsigned int Low = G.U32();
            const unsigned int High = G.U32() & 0x7fefffffu;
            const unsigned long long Bits = ((unsigned long long)High << 32) | Low;
            std::memcpy(&P->Time, &Bits, 8u);
            return P;
        }
        case 0x2000au:
        {
            FRpcServerActivateObjectPacket* P = new FRpcServerActivateObjectPacket();
            P->ObjectId = G.U32();
            P->bActivate = G.U8();
            P->Field_0x08 = G.U32();
            return P;
        }
        case 0x20022u:
        {
            FRpcServerSystemSwitchInPacket* P = new FRpcServerSystemSwitchInPacket();
            P->Field_0x00 = G.U32();
            P->Field_0x04 = G.U32();
            FillVector(P->Position, G);
            FillQuaternion(P->Orientation, G);
            return P;
        }
        case 0x20027u:
        {
            FRpcServerRequestCreateShipRespPacket* P = new FRpcServerRequestCreateShipRespPacket();
            P->Field_0x00 = G.U8();
            P->Field_0x04 = G.U32();
            return P;
        }
        case 0x20028u:
        {
            FRpcServerCreateLootPacket* P = new FRpcServerCreateLootPacket();
            P->OwnerId = G.U32();
            P->Kind = G.U8();
            for (unsigned int Index = 0; Index < (bLong ? 2u : 0u); ++Index)
            {
                FRpcServerLootRecord R;
                std::memset(&R, 0, sizeof(R));
                R.LootId = G.U32();
                R.ContentsNickname = G.U32();
                R.ContentsHitPts = G.F32();
                R.Units = G.U16();
                R.ContainerNickname = G.U32();
                R.HitPts = G.F32();
                R.NameOverride = G.U32();
                FillVector(R.Position, G);
                FillVector(R.Velocity, G);
                FillQuaternion(R.Orientation, G);
                R.bMarkedBy = G.U8();
                R.bTemporary = G.U8();
                P->AddLoot(R);
            }
            return P;
        }
        case 0x20029u:
        {
            FRpcServerSetReputationPacket* P = new FRpcServerSetReputationPacket();
            P->Target = G.U32();
            if (Case != 0)
                P->Affiliation = G.U32();
            if (Case == 1)
            {
                wchar_t Name[7];
                TextUnits(Name, G, 6);
                P->SetDisplayName(Name, 6);
            }
            if (Case == 1)
                Block(P->NameA, RandomBytes(G, 8), 8);
            else
                Block(P->NameA, ZeroBytes(4), 4);
            if (Case == 1)
                Block(P->NameB, RandomBytes(G, 5), 5);
            else
                Block(P->NameB, ZeroBytes(4), 4);
            const unsigned int Counts[3] = {0u, 3u, 300u};
            for (unsigned int Index = 0; Index < Counts[Case]; ++Index)
                P->Relations.push_back(NewRelation(G));
            P->Attitude = G.F32();
            return P;
        }
        case 0x2002au:
        {
            FRpcServerAdjustAttitudePacket* P = new FRpcServerAdjustAttitudePacket();
            P->Field_0x00 = G.U32();
            P->Field_0x04 = G.F32();
            return P;
        }
        case 0x2002bu:
        {
            FRpcServerSetGroupFeelingsPacket* P = new FRpcServerSetGroupFeelingsPacket();
            if (bLong)
                P->SetName("fc_x_grp", 9);
            for (unsigned int Index = 0; Index < (bLong ? 3u : 0u); ++Index)
                P->Relations.push_back(NewRelation(G));
            return P;
        }
        case 0x2002cu:
        {
            FRpcServerCreateMinePacket* P = new FRpcServerCreateMinePacket();
            P->ObjId = G.U32();
            P->ArchetypeNickname = G.U32();
            P->OwnerId = G.U32();
            P->SubObjId = G.U16();
            FillVector(P->Position, G);
            FillQuaternion(P->Orientation, G);
            FillVector(P->Velocity, G);
            FillVector(P->Field_0x38, G);
            return P;
        }
        case 0x2002du:
        {
            FRpcServerCreateCounterPacket* P = new FRpcServerCreateCounterPacket();
            P->ArchetypeNickname = G.U32();
            P->ObjId = G.U32();
            P->Field_0x08 = G.U32();
            P->OwnerId = G.U32();
            P->SubObjId = G.U16();
            FillQuaternion(P->Orientation, G);
            FillVector(P->Position, G);
            FillVector(P->Velocity, G);
            FillVector(P->Field_0x3c, G);
            return P;
        }
        case 0x2002eu:
        {
            FRpcServerSetAddItemPacket* P = new FRpcServerSetAddItemPacket();
            P->ArchId = G.U32();
            P->Id = G.U32();
            P->Count = G.I32();
            P->Health = G.F32();
            P->bEquipped = G.U8();
            P->bMission = G.U8();
            P->Field_0x0c = G.U32();
            if (bLong)
                P->SetHardpoint("HpShield01", 10);
            return P;
        }
        case 0x20031u:
        {
            FRpcServerExplodeAsteroidMinePacket* P = new FRpcServerExplodeAsteroidMinePacket();
            P->Field_0x00 = G.U32();
            FillVector(P->Position, G);
            return P;
        }
        case 0x20032u:
        {
            FRpcServerRequestSpaceScriptPacket* P = new FRpcServerRequestSpaceScriptPacket();
            P->NameSize = 0;
            P->Field_0x00 = G.U32();
            if (bLong)
                P->SetName("scripts/x.thn", 13);
            return P;
        }
        case 0x20033u:
        {
            FRpcServerSetMissionObjectiveStatePacket* P = new FRpcServerSetMissionObjectiveStatePacket();
            P->Field_0x00 = G.U32();
            P->Field_0x04 = G.U32();
            P->Field_0x08 = G.U16();
            return P;
        }
        case 0x20034u:
        {
            FRpcServerReplaceMissionObjectivePacket* P = new FRpcServerReplaceMissionObjectivePacket();
            P->Field_0x00 = G.U32();
            P->State = G.U32();
            P->TextSize = bLong ? 13u : 0u;
            P->TextData = RandomBytes(G, P->TextSize);
            return P;
        }
        case 0x20035u:
        {
            FRpcServerSetMissionObjectivesPacket* P = new FRpcServerSetMissionObjectivesPacket();
            P->Field_0x00 = G.U32();
            P->State = G.U32();
            Block(P->TextA, RandomBytes(G, bLong ? 7u : 0u), bLong ? 7u : 0u);
            Block(P->TextB, RandomBytes(G, bLong ? 2u : 0u), bLong ? 2u : 0u);
            for (unsigned int Index = 0; Index < (bLong ? 3u : 0u); ++Index)
            {
                FRpcServerMissionObjectiveRecord* R = static_cast<FRpcServerMissionObjectiveRecord*>(
                    ::operator new(sizeof(FRpcServerMissionObjectiveRecord)));
                R->State = G.U8();
                R->Size = Index * 3u;
                R->Data = RandomBytes(G, R->Size);
                P->PushObjective(R);
            }
            return P;
        }
        case 0x20037u:
        {
            FRpcServerCreateGuidedPacket* P = new FRpcServerCreateGuidedPacket();
            P->ArchetypeNickname = G.U32();
            P->OwnerId = G.U32();
            P->SubObjId = G.U16();
            P->TargetId = G.U32();
            P->TargetSubObjId = G.U16();
            P->ObjId = G.U32();
            FillVector(P->Position, G);
            FillUnitQuaternion(P->Orientation, G);
            return P;
        }
        case 0x20038u:
        {
            FRpcServerItemTractoredPacket* P = new FRpcServerItemTractoredPacket();
            P->Field_0x00 = G.U32();
            P->Field_0x04 = G.U32();
            P->Field_0x08 = G.U16();
            P->Field_0x0a = G.U16();
            return P;
        }
        case 0x2003cu:
        {
            FRpcServerRepairObjectPacket* P = new FRpcServerRepairObjectPacket();
            P->ObjectId = G.U32();
            P->EquipId = G.U32();
            P->Points = G.I32();
            return P;
        }
        case 0x2003du:
        {
            FRpcServerRemoteObjectCargoUpdatePacket* P = new FRpcServerRemoteObjectCargoUpdatePacket();
            P->ObjectId = G.U32();
            P->Flag = G.U8();
            EquipRecords(*P, G, bLong ? 4u : 0u);
            return P;
        }
        case 0x20041u:
        {
            FRpcServerBurnFusePacket* P = new FRpcServerBurnFusePacket();
            P->SourceId = G.U32();
            P->ObjectId = G.U32();
            P->FuseId = G.U32();
            P->SubObjId = G.U16();
            P->AtT = G.F32();
            P->Lifetime = G.F32();
            P->bLit = G.U8();
            return P;
        }
        case 0x20042u:
        {
            FRpcServerCreateSolarPacket* P = new FRpcServerCreateSolarPacket();
            CreateObject(P->Fields, G, bLong ? 2u : 0u);
            P->Field_0x9c = G.U8();
            P->bMarkedBy = G.U8();
            P->Affiliation = G.U32();
            P->Attitude = G.Unit();
            P->NameASize = bLong ? 6u : 0u;
            P->NameA = RandomBytes(G, P->NameASize);
            P->NameBSize = bLong ? 3u : 0u;
            P->NameB = RandomBytes(G, P->NameBSize);
            return P;
        }
        case 0x20044u:
        {
            FRpcServerRequestReturnedPacket* P = new FRpcServerRequestReturnedPacket();
            P->ObjectId = G.U32();
            P->RequestType = G.U8();
            P->Response = G.U8();
            P->Field_0x26 = G.U8();
            return P;
        }
        case 0x20045u:
        {
            FRpcServerSetMissionMessagePacket* P = new FRpcServerSetMissionMessagePacket();
            P->Type = G.U32();
            P->TextSize = bLong ? 11u : 0u;
            P->TextData = RandomBytes(G, P->TextSize);
            return P;
        }
        case 0x20046u:
        {
            FRpcServerMarkObjPacket* P = new FRpcServerMarkObjPacket();
            P->ObjectId = G.U32();
            P->MarkType = G.U8();
            return P;
        }
        case 0x20047u:
        {
            FRpcServerCfgInterfaceNotificationPacket* P = new FRpcServerCfgInterfaceNotificationPacket();
            P->NotifType = G.U32();
            P->bEnable = G.U8();
            P->Param = G.U32();
            return P;
        }
        case 0x20048u:
        {
            FRpcServerSetCollisionGroupsPacket* P = new FRpcServerSetCollisionGroupsPacket();
            CollisionGroups(P->Groups, G, bLong ? 3u : 0u);
            return P;
        }
        case 0x2004au:
        {
            FRpcServerSetGuidedTargetPacket* P = new FRpcServerSetGuidedTargetPacket();
            P->Field_0x20 = G.U32();
            P->Field_0x24 = G.U32();
            P->Field_0x28 = G.U16();
            return P;
        }
        case 0x2004bu:
        {
            FRpcServerSetCameraPacket* P = new FRpcServerSetCameraPacket();
            FillVector(P->Position, G);
            FillQuaternion(P->Orientation, G);
            P->Fov = G.F32();
            P->Aspect = G.F32();
            return P;
        }
        case 0x2004du:
        {
            FRpcServerLoadHintPacket* P = new FRpcServerLoadHintPacket();
            P->Kind = G.U32();
            P->Size = bLong ? 10u : 0u;
            P->Data = RandomBytes(G, P->Size);
            return P;
        }
        case 0x2004eu:
        {
            FRpcServerSetDirectivePacket* P = new FRpcServerSetDirectivePacket();
            FPayloadServerSetDirective& D = P->Directive;
            D.Field_0x00 = G.U32();
            D.Field_0x04 = G.U32();
            D.Field_0x08 = G.U8();
            D.Field_0x0c = G.F32();
            D.Field_0x10 = G.U32();
            D.Field_0x14 = G.I32();
            D.Field_0x18 = G.I32();
            D.Field_0x1c = G.U32();
            D.Field_0x20 = G.F32();
            D.Field_0x24 = G.F32();
            D.Field_0x28 = G.U32();
            D.Field_0x2c = G.F32();
            FillVector(D.Field_0x30, G);
            D.Field_0x3c = G.F32();
            D.Field_0x40 = G.I32();
            FillVector(D.Field_0x44, G);
            D.Field_0x50 = G.I32();
            FRpcDirectiveBlock* Blocks[4] = {&D.Block_0x54, &D.Block_0x60, &D.Block_0x6c, &D.Block_0x78};
            for (int Index = 0; Index < 4; ++Index)
            {
                Blocks[Index]->Field_0x00 = G.U32();
                Blocks[Index]->Field_0x04 = G.U32();
                Blocks[Index]->Field_0x08 = G.U32();
            }
            D.Field_0x84 = G.F32();
            D.Field_0x88 = G.F32();
            D.Field_0x8c = G.U8();
            D.Field_0x8d = G.U8();
            D.Field_0x8e = G.U8();
            D.Field_0x8f = G.U8();
            D.Field_0x90 = G.U8();
            return P;
        }
        case 0x2004fu:
        {
            FRpcServerSendCommPacket* P = new FRpcServerSendCommPacket();
            P->Field_0x20 = G.U32();
            P->Field_0x24 = G.U32();
            P->Field_0x28 = G.U32();
            P->Field_0x74 = Case == 0 ? 0u : 1u;
            if (Case != 0)
            {
                const unsigned int Accessories[3] = {0u, 3u, 8u};
                for (int Index = 0; Index < 4; ++Index)
                    P->Body.PartIds[Index] = G.U32();
                P->Body.AccessoryCount = Accessories[Case];
                for (unsigned int Index = 0; Index < Accessories[Case]; ++Index)
                    P->Body.AccessoryIds[Index] = G.U32();
            }
            P->Field_0x60 = G.U32();
            const unsigned int Texts[3] = {0u, 2u, 127u};
            P->Field_0x68 = (signed char)Texts[Case];
            P->Field_0x64 = Dwords(G, Texts[Case]);
            P->Field_0x6c = G.U32();
            P->Field_0x70 = G.Unit() * 8.0f + 2.0f;
            return P;
        }
        case 0x20051u:
        {
            FRpcServerUseItemPacket* P = new FRpcServerUseItemPacket();
            P->ObjectId = G.U32();
            P->EquipId = G.U16();
            P->Count = G.U16();
            return P;
        }
        case 0x20052u:
        {
            FRpcServerPlayerListPacket* P = new FRpcServerPlayerListPacket();
            const unsigned int Kind = G.U32();
            const unsigned int Player = G.U32();
            const unsigned char Byte = G.U8();
            wchar_t Name[6];
            if (bLong)
                TextUnits(Name, G, 5);
            P->Kind = Kind;
            P->SetEntry(bLong ? Name : nullptr, Player);
            P->Field_0x2c = Byte;
            return P;
        }
        case 0x20053u:
        {
            FRpcServerFormationUpdatePacket* P = new FRpcServerFormationUpdatePacket();
            P->LeaderId = G.U32();
            FillVector(P->Offset, G);
            return P;
        }
        case 0x20054u:
        {
            FRpcServerMiscObjUpdatePacket* P = new FRpcServerMiscObjUpdatePacket();
            const unsigned short Flags[3] = {0x0000u, 0x01ffu, 0x0110u};
            const unsigned short F = Flags[Case];
            P->Flags = F;
            if ((F & 0x08u) != 0)
                P->Field_0x28 = G.U32();
            if ((F & 0x04u) != 0)
                P->Field_0x24 = G.U32();
            if ((F & 0x01u) != 0)
                P->Field_0x34 = G.U32();
            if ((F & 0x02u) != 0)
            {
                P->Field_0x38 = G.U32();
                P->Field_0x3c = G.U32();
            }
            if ((F & 0x10u) != 0)
            {
                const unsigned int Count = Case == 1 ? 4u : 9u;
                for (unsigned int Index = 0; Index < Count; ++Index)
                    P->Text.push_back((wchar_t)G.TextUnit());
            }
            if ((F & 0x20u) != 0)
                P->Field_0x54 = G.U32();
            if ((F & 0x40u) != 0)
                P->Field_0x2c = G.U16();
            if ((F & 0x80u) != 0)
                P->Field_0x30 = G.U32();
            if ((F & 0x100u) != 0)
            {
                const unsigned int Count = Case == 1 ? 2u : 255u;
                for (unsigned int Index = 0; Index < Count; ++Index)
                    P->Ids.push_back(G.U32());
                P->IdCount = (unsigned char)P->Ids.size();
            }
            return P;
        }
        case 0x20055u:
        {
            FRpcServerObjectCargoUpdatePacket* P = new FRpcServerObjectCargoUpdatePacket();
            P->ObjectId = G.U32();
            P->ArchId = G.U32();
            P->EquipId = G.U16();
            P->Count = G.U16();
            P->Status = G.F32();
            P->bMission = G.U8();
            return P;
        }
        case 0x20057u:
        {
            FRpcServerSetMusicPacket* P = new FRpcServerSetMusicPacket();
            P->Field_0x20 = G.U32();
            P->Field_0x24 = G.U32();
            P->Field_0x28 = G.U32();
            P->Field_0x2c = G.U32();
            P->Field_0x30 = G.U8();
            P->Field_0x34 = G.F32();
            return P;
        }
        default:
            return nullptr;
        }
    }

    // --- Suites ----------------------------------------------------------------

    // One line of the entry tables in fill.rs.
    struct Entry
    {
        unsigned int Id;
        const char* Name;
        Mode Modes[2];
        unsigned int ModeCount;
        unsigned int Cases;
    };

    typedef FPacketBase* (*MakeFn)(unsigned int Id, unsigned int Case, Gen& G);

    // The encode buffers: room for the server's second attempt (0x19000 bytes)
    // and for what PLAYER_TRADE writes past a message (divergence D106).
    std::vector<unsigned char>& SentBuffer()
    {
        static std::vector<unsigned char> Buffer(0x19000u + 0x100u, 0);
        return Buffer;
    }

    std::vector<unsigned char>& BackBuffer()
    {
        static std::vector<unsigned char> Buffer(0x19000u + 0x100u, 0);
        return Buffer;
    }

    // Encodes Packet into Buffer, cleared first; its bytes, or "error".
    std::string Encoded(FPacketBase& Packet, std::vector<unsigned char>& Buffer, unsigned int& Length,
                        const char* Label)
    {
        std::fill(Buffer.begin(), Buffer.end(), (unsigned char)0);
        unsigned char* Holder = &Buffer[0];
        Length = 0;
        const unsigned int Result = Packet.Encode(&Holder, 0x19000u, &Length);
        if (Result != 0)
        {
            fprintf(stderr, "%s: Encode returned %#x\n", Label, Result);
            GFailed = true;
            Length = 0;
            return "error";
        }
        return Hex(&Buffer[0], Length);
    }

    // --- Decode (protocol_decode_*.jsonl) ---------------------------------------
    //
    // Each encoding is decoded again by the real decoder into an empty packet
    // of its type, made by the factory both DLLs register, and that packet is
    // encoded once more: oracle_protocol.rs decodes the same bytes with the
    // Rust decoder and must give the same second encoding (with what the C++
    // decoder's conversions change on the way; see
    // crates/game-sim/tests/support/cpp_decode.rs).

    // The decoding side of a mode: a server's message is decoded by a
    // multiplayer client, a client's by the server, single player by itself.
    Mode Receiver(Mode Sender)
    {
        switch (Sender)
        {
        case ModeServer:
            return ModeClient;
        case ModeClient:
            return ModeServer;
        default:
            return ModeSingle;
        }
    }

    // The packets whose C++ decoder is undefined on its own encoder's output
    // have no decode line.
    bool DecodeSkipped(unsigned int Id)
    {
        switch (Id)
        {
        case 0x30039u: // CREATENEWCHAR: after an empty name the fields are read two bytes early (D108)
        case 0x2002bu: // SETGROUPFEELINGS: reads a relation count that is not sent (D109)
            return true;
        default:
            return false;
        }
    }

    // SAVEGAME's constructor leaves Description unset, as the reference does;
    // the harness clears it as MakeClient does, so the packet can be deleted.
    void PrepareDecode(FPacketBase& Packet)
    {
        if (Packet.PacketId == 0x3001eu)
        {
            FRpcSaveGamePacket& P = static_cast<FRpcSaveGamePacket&>(Packet);
            P.Description = nullptr;
            P.DescriptionCount = 0;
        }
    }

    // SETREPUTATION's single-player encoder reads both name blocks without a
    // null test, and a decoded packet without names has none: they get the
    // four zero bytes MakeServer gives them, which send no names either.
    void PrepareReencode(FPacketBase& Packet)
    {
        if (Packet.PacketId == 0x20029u)
        {
            FRpcServerSetReputationPacket& P = static_cast<FRpcServerSetReputationPacket&>(Packet);
            if (P.NameA.Data == nullptr)
                Block(P.NameA, ZeroBytes(4), 4);
            if (P.NameB.Data == nullptr)
                Block(P.NameB, ZeroBytes(4), 4);
        }
    }

    // {"in":label,"rc":decoder result,"used":bytes it consumed,"back":encoding}.
    // The decoder reads the bytes after the two-byte header, as the transport
    // hands them over, in the receiver's mode; the second encoding is made in
    // the sender's mode again.
    std::string DecodeLine(FPacketSerializer& Registry, unsigned int Id, Mode Sender, const std::string& Label,
                           unsigned int Length)
    {
        FPacketBase* Fresh = nullptr;
        const int Allocated = Registry.Allocate(Id >> 16, Id & 0xffffu, &Fresh);
        if (Allocated != 0 || Fresh == nullptr)
        {
            fprintf(stderr, "%s: no factory (%d)\n", Label.c_str(), Allocated);
            GFailed = true;
            return "{\"in\":\"" + Label + "\",\"rc\":\"no factory\"}";
        }
        PrepareDecode(*Fresh);
        oracle_protocol::SetMode(Receiver(Sender));
        unsigned int Used = 0;
        const unsigned int Result = Fresh->Decode(&SentBuffer()[2], Length - 2u, &Used);
        oracle_protocol::SetMode(Sender);
        std::string Back;
        if (Result == 0)
        {
            PrepareReencode(*Fresh);
            unsigned int BackLength = 0;
            Back = Encoded(*Fresh, BackBuffer(), BackLength, Label.c_str());
            delete Fresh;
        }
        // A packet whose decode failed may be half built: it is left alone.
        return "{\"in\":\"" + Label + "\",\"rc\":\"" + Format("%x", Result) + "\",\"used\":\"" + Format("%u", Used) +
               "\",\"back\":\"" + Back + "\"}";
    }

    void PacketSuite(const char* File, const char* DecodeFile, const Entry* Entries, size_t Count, MakeFn Make,
                     FPacketSerializer& Registry)
    {
        Suite Out(File);
        Suite DecodeOut(DecodeFile);
        for (size_t Index = 0; Index < Count; ++Index)
        {
            const Entry& E = Entries[Index];
            for (unsigned int ModeIndex = 0; ModeIndex < E.ModeCount; ++ModeIndex)
            {
                const Mode Sender = E.Modes[ModeIndex];
                oracle_protocol::SetMode(Sender);
                for (unsigned int Case = 0; Case < E.Cases; ++Case)
                {
                    Gen G = Gen::ForCase(E.Id, Case);
                    const std::string Label =
                        std::string(E.Name) + " " + oracle_protocol::ModeName(Sender) + " " + Format("%u", Case);
                    FPacketBase* Packet = Make(E.Id, Case, G);
                    if (Packet == nullptr)
                    {
                        fprintf(stderr, "%s: no packet\n", Label.c_str());
                        GFailed = true;
                        continue;
                    }
                    unsigned int Length = 0;
                    const std::string Encoding = Encoded(*Packet, SentBuffer(), Length, Label.c_str());
                    Out.Line("{\"in\":\"" + Label + "\",\"out\":\"" + Encoding + "\"}");
                    if (Length >= 2u && !DecodeSkipped(E.Id))
                        DecodeOut.Line(DecodeLine(Registry, E.Id, Sender, Label, Length));
                    delete Packet;
                }
            }
        }
        oracle_protocol::RestoreMode();
    }

#define ORACLE_ENTRY1(Id, Name, M, Cases) {Id, Name, {M, M}, 1u, Cases}
#define ORACLE_ENTRY2(Id, Name, M1, M2, Cases) {Id, Name, {M1, M2}, 2u, Cases}

    // fill::COMMON
    const Entry CommonEntries[] = {
        ORACLE_ENTRY1(0x10001u, "FLPACKET_COMMON_UPDATEOBJECT", ModeClient, 2),
        ORACLE_ENTRY2(0x10002u, "FLPACKET_COMMON_FIREWEAPON", ModeClient, ModeSingle, 3),
        ORACLE_ENTRY1(0x10004u, "FLPACKET_COMMON_SETTARGET", ModeClient, 2),
        ORACLE_ENTRY1(0x10005u, "FLPACKET_COMMON_CHATMSG", ModeClient, 2),
        ORACLE_ENTRY1(0x10008u, "FLPACKET_COMMON_ACTIVATEEQUIP", ModeClient, 2),
        ORACLE_ENTRY1(0x1000eu, "FLPACKET_COMMON_ACTIVATECRUISE", ModeClient, 2),
        ORACLE_ENTRY1(0x1000fu, "FLPACKET_COMMON_GOTRADELANE", ModeClient, 2),
        ORACLE_ENTRY1(0x10010u, "FLPACKET_COMMON_STOPTRADELANE", ModeClient, 2),
        ORACLE_ENTRY1(0x10011u, "FLPACKET_COMMON_SET_WEAPON_GROUP", ModeClient, 2),
        ORACLE_ENTRY2(0x10012u, "FLPACKET_COMMON_PLAYER_TRADE", ModeServer, ModeClient, 9),
        ORACLE_ENTRY1(0x10013u, "FLPACKET_COMMON_SET_VISITED_STATE", ModeClient, 2),
        ORACLE_ENTRY1(0x10014u, "FLPACKET_COMMON_JETTISONCARGO", ModeClient, 2),
        ORACLE_ENTRY1(0x10015u, "FLPACKET_COMMON_ACTIVATETHRUSTERS", ModeClient, 2),
        ORACLE_ENTRY1(0x10016u, "FLPACKET_COMMON_REQUEST_BEST_PATH", ModeClient, 2),
        ORACLE_ENTRY1(0x10017u, "FLPACKET_COMMON_REQUEST_GROUP_POSITIONS", ModeClient, 2),
        ORACLE_ENTRY1(0x10018u, "FLPACKET_COMMON_REQUEST_PLAYER_STATS", ModeClient, 2),
        ORACLE_ENTRY1(0x10019u, "FLPACKET_COMMON_SET_MISSION_LOG", ModeClient, 2),
        ORACLE_ENTRY1(0x1001au, "FLPACKET_COMMON_REQUEST_RANK_LEVEL", ModeClient, 2),
        ORACLE_ENTRY1(0x1001bu, "FLPACKET_COMMON_POP_UP_DIALOG", ModeClient, 2),
        ORACLE_ENTRY1(0x1001cu, "FLPACKET_COMMON_SET_INTERFACE_STATE", ModeClient, 2),
        ORACLE_ENTRY1(0x1001du, "FLPACKET_COMMON_TRACTOROBJECTS", ModeClient, 2),
    };

    // fill::CLIENT; FLPACKET_CLIENT_TRADERESPONSE has no vector (divergence D107).
    const Entry ClientEntries[] = {
        ORACLE_ENTRY1(0x30001u, "FLPACKET_CLIENT_LOGIN", ModeClient, 2),
        ORACLE_ENTRY1(0x30003u, "FLPACKET_CLIENT_MUNCOLLISION", ModeClient, 2),
        ORACLE_ENTRY1(0x30004u, "FLPACKET_CLIENT_REQUESTLAUNCH", ModeClient, 2),
        ORACLE_ENTRY1(0x30005u, "FLPACKET_CLIENT_REQUESTCHARINFO", ModeClient, 1),
        ORACLE_ENTRY1(0x30006u, "FLPACKET_CLIENT_SELECTCHARACTER", ModeClient, 2),
        ORACLE_ENTRY1(0x30007u, "FLPACKET_CLIENT_ENTERBASE", ModeClient, 2),
        ORACLE_ENTRY1(0x30008u, "FLPACKET_CLIENT_REQUESTBASEINFO", ModeClient, 2),
        ORACLE_ENTRY1(0x30009u, "FLPACKET_CLIENT_REQUESTLOCATIONINFO", ModeClient, 2),
        ORACLE_ENTRY1(0x3000au, "FLPACKET_CLIENT_GFREQUESTSHIPINFO", ModeClient, 1),
        ORACLE_ENTRY1(0x3000bu, "FLPACKET_CLIENT_SYSTEM_SWITCH_OUT_COMPLETE", ModeClient, 2),
        ORACLE_ENTRY1(0x3000cu, "FLPACKET_CLIENT_OBJCOLLISION", ModeClient, 2),
        ORACLE_ENTRY1(0x3000du, "FLPACKET_CLIENT_EXITBASE", ModeClient, 2),
        ORACLE_ENTRY1(0x3000eu, "FLPACKET_CLIENT_ENTERLOCATION", ModeClient, 2),
        ORACLE_ENTRY1(0x3000fu, "FLPACKET_CLIENT_EXITLOCATION", ModeClient, 2),
        ORACLE_ENTRY1(0x30010u, "FLPACKET_CLIENT_REQUESTCREATESHIP", ModeClient, 1),
        ORACLE_ENTRY1(0x30011u, "FLPACKET_CLIENT_GFGOODSELL", ModeClient, 2),
        ORACLE_ENTRY1(0x30012u, "FLPACKET_CLIENT_GFGOODBUY", ModeClient, 2),
        ORACLE_ENTRY1(0x30013u, "FLPACKET_CLIENT_GFSELECTOBJECT", ModeClient, 2),
        ORACLE_ENTRY1(0x30014u, "FLPACKET_CLIENT_MISSIONRESPONSE", ModeClient, 2),
        ORACLE_ENTRY1(0x30015u, "FLPACKET_CLIENT_REQSHIPARCH", ModeClient, 2),
        ORACLE_ENTRY1(0x30016u, "FLPACKET_CLIENT_REQEQUIPMENT", ModeClient, 2),
        ORACLE_ENTRY1(0x30017u, "FLPACKET_CLIENT_REQCARGO", ModeClient, 2),
        ORACLE_ENTRY1(0x30018u, "FLPACKET_CLIENT_REQADDITEM", ModeClient, 2),
        ORACLE_ENTRY1(0x30019u, "FLPACKET_CLIENT_REQREMOVEITEM", ModeClient, 2),
        ORACLE_ENTRY1(0x3001au, "FLPACKET_CLIENT_REQMODIFYITEM", ModeClient, 2),
        ORACLE_ENTRY1(0x3001bu, "FLPACKET_CLIENT_REQSETCASH", ModeClient, 2),
        ORACLE_ENTRY1(0x3001cu, "FLPACKET_CLIENT_REQCHANGECASH", ModeClient, 2),
        ORACLE_ENTRY1(0x3001eu, "FLPACKET_CLIENT_SAVEGAME", ModeClient, 2),
        ORACLE_ENTRY1(0x30020u, "FLPACKET_CLIENT_MINEASTEROID", ModeClient, 2),
        ORACLE_ENTRY1(0x30022u, "FLPACKET_CLIENT_DBGCREATESHIP", ModeClient, 2),
        ORACLE_ENTRY1(0x30023u, "FLPACKET_CLIENT_DBGLOADSYSTEM", ModeClient, 2),
        ORACLE_ENTRY1(0x30024u, "FLPACKET_CLIENT_DOCK", ModeClient, 2),
        ORACLE_ENTRY1(0x30025u, "FLPACKET_CLIENT_DBGDESTROYOBJECT", ModeClient, 2),
        ORACLE_ENTRY1(0x3002bu, "FLPACKET_CLIENT_CARGOSCAN", ModeClient, 2),
        ORACLE_ENTRY1(0x3002du, "FLPACKET_CLIENT_DBGCONSOLE", ModeClient, 2),
        ORACLE_ENTRY1(0x3002eu, "FLPACKET_CLIENT_DBGFREESYSTEM", ModeClient, 1),
        ORACLE_ENTRY1(0x3002fu, "FLPACKET_CLIENT_SETMANEUVER", ModeClient, 2),
        ORACLE_ENTRY1(0x30030u, "FLPACKET_CLIENT_DBGRELOCATE_SHIP", ModeClient, 2),
        ORACLE_ENTRY1(0x30031u, "FLPACKET_CLIENT_REQUEST_EVENT", ModeClient, 2),
        ORACLE_ENTRY1(0x30032u, "FLPACKET_CLIENT_REQUEST_CANCEL", ModeClient, 2),
        ORACLE_ENTRY1(0x30035u, "FLPACKET_CLIENT_INTERFACEITEMUSED", ModeClient, 2),
        ORACLE_ENTRY1(0x30036u, "FLPACKET_CLIENT_REQCOLLISIONGROUPS", ModeClient, 2),
        ORACLE_ENTRY1(0x30037u, "FLPACKET_CLIENT_COMMCOMPLETE", ModeClient, 2),
        ORACLE_ENTRY1(0x30038u, "FLPACKET_CLIENT_REQUESTNEWCHARINFO", ModeClient, 1),
        ORACLE_ENTRY1(0x30039u, "FLPACKET_CLIENT_CREATENEWCHAR", ModeClient, 2),
        ORACLE_ENTRY1(0x3003au, "FLPACKET_CLIENT_DESTROYCHAR", ModeClient, 2),
        ORACLE_ENTRY1(0x3003bu, "FLPACKET_CLIENT_REQHULLSTATUS", ModeClient, 2),
        ORACLE_ENTRY1(0x3003cu, "FLPACKET_CLIENT_GFGOODVAPORIZED", ModeClient, 2),
        ORACLE_ENTRY1(0x3003du, "FLPACKET_CLIENT_BADLANDSOBJCOLLISION", ModeClient, 2),
        ORACLE_ENTRY1(0x3003eu, "FLPACKET_CLIENT_LAUNCHCOMPLETE", ModeClient, 2),
        ORACLE_ENTRY1(0x3003fu, "FLPACKET_CLIENT_HAIL", ModeClient, 2),
        ORACLE_ENTRY1(0x30040u, "FLPACKET_CLIENT_REQUEST_USE_ITEM", ModeClient, 2),
        ORACLE_ENTRY1(0x30041u, "FLPACKET_CLIENT_ABORT_MISSION", ModeClient, 2),
        ORACLE_ENTRY1(0x30042u, "FLPACKET_CLIENT_SKIP_AUTOSAVE", ModeClient, 1),
        ORACLE_ENTRY1(0x30043u, "FLPACKET_CLIENT_JUMPINCOMPLETE", ModeClient, 2),
        ORACLE_ENTRY1(0x30044u, "FLPACKET_CLIENT_REQINVINCIBILITY", ModeClient, 2),
        ORACLE_ENTRY1(0x30045u, "FLPACKET_CLIENT_MISSIONSAVEB", ModeClient, 2),
        ORACLE_ENTRY1(0x30046u, "FLPACKET_CLIENT_REQDIFFICULTYSCALE", ModeClient, 2),
        ORACLE_ENTRY1(0x30047u, "FLPACKET_CLIENT_RTCDONE", ModeClient, 2),
    };

    // fill::SERVER
    const Entry ServerEntries[] = {
        ORACLE_ENTRY1(0x20001u, "FLPACKET_SERVER_CONNECTRESPONSE", ModeServer, 2),
        ORACLE_ENTRY1(0x20002u, "FLPACKET_SERVER_LOGINRESPONSE", ModeServer, 2),
        ORACLE_ENTRY1(0x20003u, "FLPACKET_SERVER_CHARACTERINFO", ModeServer, 3),
        ORACLE_ENTRY2(0x20004u, "FLPACKET_SERVER_CREATESHIP", ModeServer, ModeSingle, 3),
        ORACLE_ENTRY1(0x20005u, "FLPACKET_SERVER_DAMAGEOBJECT", ModeServer, 2),
        ORACLE_ENTRY1(0x20006u, "FLPACKET_SERVER_DESTROYOBJECT", ModeServer, 2),
        ORACLE_ENTRY1(0x20007u, "FLPACKET_SERVER_LAUNCH", ModeServer, 2),
        ORACLE_ENTRY1(0x20008u, "FLPACKET_SERVER_CHARSELECTVERIFIED", ModeServer, 2),
        ORACLE_ENTRY1(0x2000au, "FLPACKET_SERVER_ACTIVATEOBJECT", ModeServer, 2),
        ORACLE_ENTRY1(0x2000bu, "FLPACKET_SERVER_LAND", ModeServer, 2),
        ORACLE_ENTRY1(0x2000du, "FLPACKET_SERVER_SETSTARTROOM", ModeServer, 2),
        ORACLE_ENTRY1(0x2000eu, "FLPACKET_SERVER_GFCOMPLETENEWSBROADCASTLIST", ModeServer, 2),
        ORACLE_ENTRY1(0x2000fu, "FLPACKET_SERVER_GFCOMPLETECHARLIST", ModeServer, 2),
        ORACLE_ENTRY1(0x20010u, "FLPACKET_SERVER_GFCOMPLETEMISSIONCOMPUTERLIST", ModeServer, 2),
        ORACLE_ENTRY1(0x20011u, "FLPACKET_SERVER_GFCOMPLETESCRIPTBEHAVIORLIST", ModeServer, 2),
        ORACLE_ENTRY1(0x20013u, "FLPACKET_SERVER_GFCOMPLETEAMBIENTSCRIPTLIST", ModeServer, 2),
        ORACLE_ENTRY1(0x20014u, "FLPACKET_SERVER_GFDESTROYNEWSBROADCAST", ModeServer, 2),
        ORACLE_ENTRY1(0x20015u, "FLPACKET_SERVER_GFDESTROYCHARACTER", ModeServer, 2),
        ORACLE_ENTRY1(0x20016u, "FLPACKET_SERVER_GFDESTROYMISSIONCOMPUTER", ModeServer, 2),
        ORACLE_ENTRY1(0x20017u, "FLPACKET_SERVER_GFDESTROYSCRIPTBEHAVIOR", ModeServer, 2),
        ORACLE_ENTRY1(0x20019u, "FLPACKET_SERVER_GFDESTROYAMBIENTSCRIPT", ModeServer, 2),
        ORACLE_ENTRY1(0x2001au, "FLPACKET_SERVER_GFSCRIPTBEHAVIOR", ModeServer, 2),
        ORACLE_ENTRY1(0x2001du, "FLPACKET_SERVER_GFUPDATEMISSIONCOMPUTER", ModeServer, 2),
        ORACLE_ENTRY1(0x2001eu, "FLPACKET_SERVER_GFUPDATENEWSBROADCAST", ModeServer, 2),
        ORACLE_ENTRY1(0x2001fu, "FLPACKET_SERVER_GFUPDATEAMBIENTSCRIPT", ModeServer, 2),
        ORACLE_ENTRY1(0x20020u, "FLPACKET_SERVER_GFMISSIONVENDORACCEPTANCE", ModeServer, 2),
        ORACLE_ENTRY1(0x20021u, "FLPACKET_SERVER_SYSTEM_SWITCH_OUT", ModeServer, 2),
        ORACLE_ENTRY1(0x20022u, "FLPACKET_SERVER_SYSTEM_SWITCH_IN", ModeServer, 2),
        ORACLE_ENTRY1(0x20023u, "FLPACKET_SERVER_SETSHIPARCH", ModeServer, 2),
        ORACLE_ENTRY1(0x20024u, "FLPACKET_SERVER_SETEQUIPMENT", ModeServer, 2),
        ORACLE_ENTRY1(0x20025u, "FLPACKET_SERVER_SETCARGO", ModeServer, 2),
        ORACLE_ENTRY1(0x20026u, "FLPACKET_SERVER_GFUPDATECHAR", ModeServer, 2),
        ORACLE_ENTRY1(0x20027u, "FLPACKET_SERVER_REQUESTCREATESHIPRESP", ModeServer, 2),
        ORACLE_ENTRY2(0x20028u, "FLPACKET_SERVER_CREATELOOT", ModeServer, ModeSingle, 2),
        ORACLE_ENTRY2(0x20029u, "FLPACKET_SERVER_SETREPUTATION", ModeServer, ModeSingle, 3),
        ORACLE_ENTRY1(0x2002au, "FLPACKET_SERVER_ADJUSTATTITUDE", ModeServer, 2),
        ORACLE_ENTRY1(0x2002bu, "FLPACKET_SERVER_SETGROUPFEELINGS", ModeServer, 2),
        ORACLE_ENTRY2(0x2002cu, "FLPACKET_SERVER_CREATEMINE", ModeServer, ModeSingle, 2),
        ORACLE_ENTRY1(0x2002du, "FLPACKET_SERVER_CREATECOUNTER", ModeServer, 2),
        ORACLE_ENTRY1(0x2002eu, "FLPACKET_SERVER_SETADDITEM", ModeServer, 2),
        ORACLE_ENTRY1(0x2002fu, "FLPACKET_SERVER_SETREMOVEITEM", ModeServer, 2),
        ORACLE_ENTRY1(0x20030u, "FLPACKET_SERVER_SETCASH", ModeServer, 2),
        ORACLE_ENTRY1(0x20031u, "FLPACKET_SERVER_EXPLODEASTEROIDMINE", ModeServer, 2),
        ORACLE_ENTRY1(0x20032u, "FLPACKET_SERVER_REQUESTSPACESCRIPT", ModeServer, 2),
        ORACLE_ENTRY1(0x20033u, "FLPACKET_SERVER_SETMISSIONOBJECTIVESTATE", ModeServer, 2),
        ORACLE_ENTRY1(0x20034u, "FLPACKET_SERVER_REPLACEMISSIONOBJECTIVE", ModeServer, 2),
        ORACLE_ENTRY1(0x20035u, "FLPACKET_SERVER_SETMISSIONOBJECTIVES", ModeServer, 2),
        ORACLE_ENTRY1(0x20037u, "FLPACKET_SERVER_CREATEGUIDED", ModeServer, 2),
        ORACLE_ENTRY1(0x20038u, "FLPACKET_SERVER_ITEMTRACTORED", ModeServer, 2),
        ORACLE_ENTRY1(0x20039u, "FLPACKET_SERVER_SCANNOTIFY", ModeServer, 2),
        ORACLE_ENTRY1(0x2003cu, "FLPACKET_SERVER_REPAIROBJECT", ModeServer, 2),
        ORACLE_ENTRY1(0x2003du, "FLPACKET_SERVER_REMOTEOBJECTCARGOUPDATE", ModeServer, 2),
        ORACLE_ENTRY1(0x2003eu, "FLPACKET_SERVER_SETNUMKILLS", ModeServer, 2),
        ORACLE_ENTRY1(0x2003fu, "FLPACKET_SERVER_SETMISSIONSUCCESSES", ModeServer, 2),
        ORACLE_ENTRY1(0x20040u, "FLPACKET_SERVER_SETMISSIONFAILURES", ModeServer, 2),
        ORACLE_ENTRY1(0x20041u, "FLPACKET_SERVER_BURNFUSE", ModeServer, 2),
        ORACLE_ENTRY2(0x20042u, "FLPACKET_SERVER_CREATESOLAR", ModeServer, ModeSingle, 2),
        ORACLE_ENTRY1(0x20043u, "FLPACKET_SERVER_SET_STORY_CUE", ModeServer, 2),
        ORACLE_ENTRY1(0x20044u, "FLPACKET_SERVER_REQUEST_RETURNED", ModeServer, 2),
        ORACLE_ENTRY1(0x20045u, "FLPACKET_SERVER_SET_MISSION_MESSAGE", ModeServer, 2),
        ORACLE_ENTRY1(0x20046u, "FLPACKET_SERVER_MARKOBJ", ModeServer, 2),
        ORACLE_ENTRY1(0x20047u, "FLPACKET_SERVER_CFGINTERFACENOTIFICATION", ModeServer, 2),
        ORACLE_ENTRY1(0x20048u, "FLPACKET_SERVER_SETCOLLISIONGROUPS", ModeServer, 2),
        ORACLE_ENTRY1(0x20049u, "FLPACKET_SERVER_SETHULLSTATUS", ModeServer, 2),
        ORACLE_ENTRY1(0x2004au, "FLPACKET_SERVER_SETGUIDEDTARGET", ModeServer, 2),
        ORACLE_ENTRY1(0x2004bu, "FLPACKET_SERVER_SET_CAMERA", ModeServer, 2),
        ORACLE_ENTRY1(0x2004cu, "FLPACKET_SERVER_REVERT_CAMERA", ModeServer, 1),
        ORACLE_ENTRY1(0x2004du, "FLPACKET_SERVER_LOADHINT", ModeServer, 2),
        ORACLE_ENTRY1(0x2004eu, "FLPACKET_SERVER_SETDIRECTIVE", ModeServer, 2),
        ORACLE_ENTRY1(0x2004fu, "FLPACKET_SERVER_SENDCOMM", ModeServer, 3),
        ORACLE_ENTRY1(0x20051u, "FLPACKET_SERVER_USE_ITEM", ModeServer, 2),
        ORACLE_ENTRY1(0x20052u, "FLPACKET_SERVER_PLAYERLIST", ModeServer, 2),
        ORACLE_ENTRY1(0x20053u, "FLPACKET_SERVER_FORMATION_UPDATE", ModeServer, 2),
        ORACLE_ENTRY1(0x20054u, "FLPACKET_SERVER_MISCOBJUPDATE", ModeServer, 3),
        ORACLE_ENTRY1(0x20055u, "FLPACKET_SERVER_OBJECTCARGOUPDATE", ModeServer, 2),
        ORACLE_ENTRY1(0x20056u, "FLPACKET_SERVER_SENDNNMESSAGE", ModeServer, 2),
        ORACLE_ENTRY1(0x20057u, "FLPACKET_SERVER_SET_MUSIC", ModeServer, 2),
        ORACLE_ENTRY1(0x20058u, "FLPACKET_SERVER_CANCEL_MUSIC", ModeServer, 1),
        ORACLE_ENTRY1(0x20059u, "FLPACKET_SERVER_PLAY_SOUND_EFFECT", ModeServer, 2),
        ORACLE_ENTRY1(0x2005au, "FLPACKET_SERVER_GFMISSIONVENDORWHYEMPTY", ModeServer, 2),
        ORACLE_ENTRY1(0x2005bu, "FLPACKET_SERVER_MISSIONSAVEA", ModeServer, 2),
    };

#undef ORACLE_ENTRY1
#undef ORACLE_ENTRY2
}

void RunProtocolPacketSuites()
{
    // The create-object codec's hardpoint dictionary, as the packet serializer's
    // constructor builds it in both DLLs.
    RpcServerBuildHardpointDictionary();
    // The packet factories RemoteClient.dll registers (RemoteClient_transport.cpp):
    // channels 1, 2 and 3. Its constructor builds the dictionary again.
    FPacketSerializer Registry;
    RpcRegisterCommonScalarPackets(&Registry);
    RpcRegisterRemoteClientServerPackets(&Registry);
    RpcRegisterClientScalarPackets(&Registry);
    PacketSuite("game-sim\\protocol_common.jsonl", "game-sim\\protocol_decode_common.jsonl", CommonEntries,
                sizeof(CommonEntries) / sizeof(CommonEntries[0]), MakeCommon, Registry);
    PacketSuite("game-sim\\protocol_client.jsonl", "game-sim\\protocol_decode_client.jsonl", ClientEntries,
                sizeof(ClientEntries) / sizeof(ClientEntries[0]), MakeClient, Registry);
    PacketSuite("game-sim\\protocol_server.jsonl", "game-sim\\protocol_decode_server.jsonl", ServerEntries,
                sizeof(ServerEntries) / sizeof(ServerEntries[0]), MakeServer, Registry);
}
