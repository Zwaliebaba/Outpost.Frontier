// Oracle instrumentation (design/migration-plan.md, Phase 0; work package P3.6e, decision record
// 0008 points 7 and 10): the Tier B base session trace, what BaseSupport and GameSupport's base
// cases do while a player visits he01_01_Base, for the Rust comparison
// (crates/game-logic/tests/base_session.rs). A new export, not part of the reference DLL's
// interface: only Oracle.exe sim calls it, for the session file CppSource/Oracle/base_session.txt
// (`dump base_session`), right after the multiplayer start-up, so a game run never reaches it.
//
// The synthetic player 7 is registered with ReplaceContentPlayerState in the base's system, with
// no ship and no server slot. The export makes a BaseSupport of its own (CreateInstance on a
// controller id no controller has, so the bus delivers it nothing; the generic scripts are
// emptied first, so its constructor's load leaves them read once), loads the base's rooms in
// Server.dll's base database (OracleBaseDbLoadBase, the BaseDB::load_base that
// IServerImpl::BaseEnter makes before the bus hears of the entry) and sets Server.dll's base
// database hook (OracleSetBaseDbTrace). It then runs the steps below, each from
// std::srand(OracleBaseSeed + step). A message is handed to GameSupport's case for it, which the
// export runs as GameSupport.cpp:556-614 runs it (the bus's GameSupport is not reachable from
// here), then to the BaseSupport through IDLL::HandleMessage:
//   0 base_enter        BaseEnter {base, 7};
//   1 vendor_offers     RefreshMissionVendorOffers, in place of the offers BaseEnter builds only
//                       for a player with a ship (pub::Player::GetShipID fails for player 7);
//   2.. room            per [mRoom] of the base, LocationEnter then LocationExit;
//   bar                 LocationEnter of the room holding the player's bartender;
//   select_bartender    GFObjSelect {the bartender's CharacterId, 7};
//   accept_vendor       MissionResponse {the first vendor offer's id, 2, accepted, 7};
//   base_exit           LocationExit of the bar, then BaseExit {base, 7}.
// At the end the hook is cleared, the BaseSupport destroyed (which empties the generic scripts),
// the scripts read again for the bus's BaseSupport, and player 7 removed. The DestroyMission
// controller the acceptance makes stays on the bus until the run ends.
//
// OracleDumpBaseSession(Dir) writes <Dir>\base.jsonl, one JSON object per line, in the format of
// game_logic::bases::DbEvent::to_json and the Rust test's records:
//   {"rec":"step","step":N,"name":"X"}
//   {"rec":"message","step":N,"msg":M}
//   {"rec":"create","step":N,"kind":"K","handle":H,"filter":F,"data":"<hex>"}
//   {"rec":"destroy","step":N,"kind":"K","handle":H,"found":0|1}
//   {"rec":"set_behavior","step":N,"character":C,"behavior":B,"found":0|1}
//   {"rec":"acceptance","step":N,"handle":H,"accepted":0|1,"session":S,"data":"<hex>"}
//   {"rec":"why_empty","step":N,"client":C,"reason":R}
//   {"rec":"state","step":N,"draws":D,"location":L,"visit":null|{"news":[..],"vendor":[[h,o],..]},
//    "rooms":[{"room":R,"locks":[[player,placement,count],..],
//              "characters":[[id,behavior,uses,placement,"role",player,talks,offer kind],..],
//              "refs":[character id,..]},..]}
// and returns the number of records, -1 when the file cannot be opened, the base is unknown or
// Server.dll lacks the hooks.

#include "content.h"
#include "BaseDB.h"             // FindContentBaseInfo, FContentBase, FBaseRoomState
#include "BaseSupport.h"        // GBaseVisits, RefreshMissionVendorOffers
#include "ContentPlayerState.h" // ReplaceContentPlayerState, RemoveContentPlayerState
#include "GenericScripts.h"     // LoadGenericScripts, UnloadGenericScripts
#include "SystemInfo.h"         // FindSystemInfo
#include "common_server.h"
#include "common_server_controller.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <map>
#include <string>
#include <vector>

namespace
{
    using pub::Controller::EMessage;

    unsigned int const OracleBaseSeed = 0x4f524143u;
    unsigned int const OracleBasePlayerId = 7u;
    unsigned int const OracleBaseSupportController = 0x00ff0002u;
    char const* const OracleBaseName = "he01_01_Base";

    // Server.dll's hook (server_basedb.h).
    typedef void(__cdecl* FOracleBaseDbTraceFn)(void* Context, int Event, int Kind, unsigned int A,
                                                 unsigned int B, unsigned int C, void const* Data,
                                                 unsigned int Size);
    typedef void(__cdecl* FOracleSetBaseDbTrace)(FOracleBaseDbTraceFn Fn, void* Context);
    typedef void(__cdecl* FOracleBaseDbLoadBase)(unsigned int BaseId);

    // The message parameter blocks (GameSupport.cpp:42-64, BaseSupport.h:50-56).
    struct FOracleBasePlayer
    {
        unsigned int Id;
        unsigned int PlayerId;
    };

    struct FOracleSession
    {
        FILE* File = nullptr;
        unsigned int Step = 0;
        int Records = 0;
        FContentBase* Base = nullptr;
        unsigned int BaseId = 0;
        IDLL* Support = nullptr;
    };

    FOracleSession GOracleBase;

    char const* OracleBaseKind(int Kind)
    {
        switch (Kind)
        {
        case 1:
            return "character";
        case 2:
            return "behavior";
        case 3:
            return "ambient";
        case 4:
            return "vendor";
        case 5:
            return "news";
        default:
            return "?";
        }
    }

    void OracleBaseHex(FILE* Out, void const* Data, unsigned int Size)
    {
        unsigned char const* const Bytes = static_cast<unsigned char const*>(Data);
        for (unsigned int Index = 0; Index < Size; ++Index)
            std::fprintf(Out, "%02x", Bytes[Index]);
    }

    // A JSON string as the Rust test writes it: \\, \" and the bytes outside printable ASCII as
    // \u00XX.
    void OracleBaseString(FILE* Out, char const* Text)
    {
        std::fputc('"', Out);
        for (unsigned char const* It = reinterpret_cast<unsigned char const*>(Text); *It != 0; ++It)
        {
            if (*It == '"')
                std::fputs("\\\"", Out);
            else if (*It == '\\')
                std::fputs("\\\\", Out);
            else if (*It >= 0x20 && *It <= 0x7e)
                std::fputc(*It, Out);
            else
                std::fprintf(Out, "\\u%04x", static_cast<unsigned int>(*It));
        }
        std::fputc('"', Out);
    }

    void __cdecl OracleBaseDbHook(void* Context, int Event, int Kind, unsigned int A, unsigned int B,
                                  unsigned int C, void const* Data, unsigned int Size)
    {
        FOracleSession* const Session = static_cast<FOracleSession*>(Context);
        FILE* const Out = Session->File;
        if (Out == nullptr)
            return;
        unsigned int const Step = Session->Step;
        switch (Event)
        {
        case 1:
            std::fprintf(Out, "{\"rec\":\"create\",\"step\":%u,\"kind\":\"%s\",\"handle\":%u,\"filter\":%u,\"data\":\"",
                         Step, OracleBaseKind(Kind), A, B);
            OracleBaseHex(Out, Data, Size);
            std::fprintf(Out, "\"}\n");
            break;
        case 2:
            std::fprintf(Out, "{\"rec\":\"destroy\",\"step\":%u,\"kind\":\"%s\",\"handle\":%u,\"found\":%u}\n", Step,
                         OracleBaseKind(Kind), A, B);
            break;
        case 3:
            std::fprintf(Out, "{\"rec\":\"set_behavior\",\"step\":%u,\"character\":%u,\"behavior\":%u,\"found\":%u}\n",
                         Step, A, B, C);
            break;
        case 4:
            std::fprintf(Out, "{\"rec\":\"acceptance\",\"step\":%u,\"handle\":%u,\"accepted\":%u,\"session\":%u,\"data\":\"",
                         Step, A, B, C);
            OracleBaseHex(Out, Data, Size);
            std::fprintf(Out, "\"}\n");
            break;
        case 5:
            std::fprintf(Out, "{\"rec\":\"why_empty\",\"step\":%u,\"client\":%u,\"reason\":%u}\n", Step, A, B);
            break;
        default:
            return;
        }
        ++Session->Records;
    }

    // The draws since std::srand(Seed), as OracleDumpRandomMissionSaves counts them: the next
    // four values matched against the generator's sequence from the seed (-1 beyond 0x10000).
    int OracleBaseCountDraws(unsigned int Seed)
    {
        int Probe[4];
        for (int Index = 0; Index < 4; ++Index)
            Probe[Index] = std::rand();
        std::srand(Seed);
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

    void OracleBaseBegin(unsigned int Step, char const* Name)
    {
        GOracleBase.Step = Step;
        std::fprintf(GOracleBase.File, "{\"rec\":\"step\",\"step\":%u,\"name\":\"%s\"}\n", Step, Name);
        ++GOracleBase.Records;
        std::srand(OracleBaseSeed + Step);
    }

    // The `message` record, GameSupport's case (GameSupport.cpp:556-614), then BaseSupport's.
    void OracleBaseSend(EMessage Message, void const* Parms)
    {
        std::fprintf(GOracleBase.File, "{\"rec\":\"message\",\"step\":%u,\"msg\":%u}\n", GOracleBase.Step,
                     static_cast<unsigned int>(Message));
        ++GOracleBase.Records;
        FOracleBasePlayer const* const Msg = static_cast<FOracleBasePlayer const*>(Parms);
        switch (Message)
        {
        case EMessage::BaseEnter:
        {
            FindContentPlayerState(Msg->PlayerId)->OnBaseChange(Msg->Id);
            FContentBase* const Base = FindContentBaseInfo(&Msg->Id);
            if (Base != nullptr)
            {
                Base->Load(nullptr);
                Base->OnPlayerEnter(Msg->PlayerId);
            }
            break;
        }
        case EMessage::BaseExit:
        {
            FContentBase* const Base = FindContentBaseInfo(&Msg->Id);
            FContentPlayerState* const State = FindContentPlayerState(Msg->PlayerId);
            if (State != nullptr)
            {
                if (Base != nullptr)
                    Base->OnPlayerExit(Msg->PlayerId);
                State->OnBaseChange(0);
            }
            break;
        }
        case EMessage::LocationEnter:
            FindContentPlayerState(Msg->PlayerId)->SetLocation(Msg->Id);
            break;
        case EMessage::LocationExit:
            FindContentPlayerState(Msg->PlayerId)->SetLocation(0);
            break;
        default:
            break;
        }
        GOracleBase.Support->HandleMessage(Message, Parms);
    }

    // The step's `state` record.
    void OracleBaseEnd()
    {
        FILE* const Out = GOracleBase.File;
        int const Draws = OracleBaseCountDraws(OracleBaseSeed + GOracleBase.Step);
        FContentPlayerState const* const State = FindContentPlayerState(OracleBasePlayerId);
        std::fprintf(Out, "{\"rec\":\"state\",\"step\":%u,\"draws\":%d,\"location\":%u,\"visit\":", GOracleBase.Step,
                     Draws, State != nullptr ? State->LocationId : 0u);
        std::map<unsigned int, FBaseVisit>::const_iterator Visit = GBaseVisits.find(OracleBasePlayerId);
        if (Visit == GBaseVisits.end())
        {
            std::fprintf(Out, "null");
        }
        else
        {
            std::fprintf(Out, "{\"news\":[");
            for (size_t Index = 0; Index < Visit->second.NewsHandles.size(); ++Index)
                std::fprintf(Out, "%s%lu", Index == 0 ? "" : ",", Visit->second.NewsHandles[Index]);
            std::fprintf(Out, "],\"vendor\":[");
            bool bFirst = true;
            for (std::list<FBaseVendorOffer>::const_iterator It = Visit->second.VendorOffers.begin();
                 It != Visit->second.VendorOffers.end(); ++It, bFirst = false)
                std::fprintf(Out, "%s[%lu,%u]", bFirst ? "" : ",", It->VendorHandle, It->OfferId);
            std::fprintf(Out, "]}");
        }
        std::fprintf(Out, ",\"rooms\":[");
        bool bFirstRoom = true;
        for (std::map<unsigned int, FBaseRoomState>::const_iterator Room = GOracleBase.Base->RoomStates.begin();
             Room != GOracleBase.Base->RoomStates.end(); ++Room, bFirstRoom = false)
        {
            std::fprintf(Out, "%s{\"room\":%u,\"locks\":[", bFirstRoom ? "" : ",", Room->first);
            bool bFirst = true;
            for (std::list<FBarPlacementLock>::const_iterator Lock = Room->second.Locks.begin();
                 Lock != Room->second.Locks.end(); ++Lock, bFirst = false)
                std::fprintf(Out, "%s[%u,%d,%d]", bFirst ? "" : ",", Lock->PlayerId, Lock->Placement, Lock->Count);
            std::fprintf(Out, "],\"characters\":[");
            bFirst = true;
            for (FBarCharacterList::const_iterator It = Room->second.Characters.begin();
                 It != Room->second.Characters.end(); ++It, bFirst = false)
            {
                std::fprintf(Out, "%s[%u,%lu,%d,%d,", bFirst ? "" : ",", It->CharacterId, It->Behavior, It->UseCount,
                             It->Placement);
                OracleBaseString(Out, It->Role.Data);
                std::fprintf(Out, ",%u,%d,%d]", It->PlayerId, It->TalkCount, It->Offer.Kind);
            }
            std::fprintf(Out, "],\"refs\":[");
            bFirst = true;
            for (std::list<FBarCharacterList::iterator>::const_iterator Ref = Room->second.CharacterRefs.begin();
                 Ref != Room->second.CharacterRefs.end(); ++Ref, bFirst = false)
                std::fprintf(Out, "%s%u", bFirst ? "" : ",", (*Ref)->CharacterId);
            std::fprintf(Out, "]}");
        }
        std::fprintf(Out, "]}\n");
        ++GOracleBase.Records;
    }

    // The room and CharacterId of the player's bartender: the first room (by id) with a
    // character of the player whose role is "bartender" (_stricmp).
    void OracleBaseFindBartender(unsigned int* OutRoom, unsigned int* OutCharacter)
    {
        *OutRoom = 0;
        *OutCharacter = 0;
        for (std::map<unsigned int, FBaseRoomState>::const_iterator Room = GOracleBase.Base->RoomStates.begin();
             Room != GOracleBase.Base->RoomStates.end(); ++Room)
        {
            for (FBarCharacterList::const_iterator It = Room->second.Characters.begin();
                 It != Room->second.Characters.end(); ++It)
            {
                if (It->PlayerId == OracleBasePlayerId && _stricmp(It->Role.Data, "bartender") == 0)
                {
                    *OutRoom = Room->first;
                    *OutCharacter = It->CharacterId;
                    return;
                }
            }
        }
    }
}

extern "C" __declspec(dllexport) int __cdecl OracleDumpBaseSession(char const* Dir)
{
    if (Dir == nullptr)
        return -1;
    HMODULE const Server = GetModuleHandleA("server.dll");
    FOracleSetBaseDbTrace const SetTrace =
        Server != nullptr ? (FOracleSetBaseDbTrace)GetProcAddress(Server, "OracleSetBaseDbTrace") : nullptr;
    FOracleBaseDbLoadBase const LoadBase =
        Server != nullptr ? (FOracleBaseDbLoadBase)GetProcAddress(Server, "OracleBaseDbLoadBase") : nullptr;
    if (SetTrace == nullptr || LoadBase == nullptr)
        return -1;
    unsigned int BaseId = 0;
    pub::GetBaseID(BaseId, OracleBaseName);
    FContentBase* const Base = FindContentBaseInfo(&BaseId);
    if (Base == nullptr)
        return -1;
    std::string const Path = std::string(Dir) + "\\base.jsonl";
    FILE* const File = std::fopen(Path.c_str(), "wb");
    if (File == nullptr)
        return -1;

    // RegisterOracleBasePlayer: player 7 in the base's system, no ship.
    FContentPlayerState* const State = ReplaceContentPlayerState(OracleBasePlayerId);
    unsigned int SystemId = 0;
    if (pub::GetSystem(SystemId, BaseId) == 0)
        State->SystemRecord = FindSystemInfo(&SystemId);

    // The session's own BaseSupport, whose constructor reads the generic scripts once.
    UnloadGenericScripts();
    IDLL* const Support = CreateInstance(OracleBaseSupportController, "BaseSupport", nullptr);
    GOracleBase = FOracleSession();
    GOracleBase.File = File;
    GOracleBase.Base = Base;
    GOracleBase.BaseId = BaseId;
    GOracleBase.Support = Support;
    if (Support == nullptr)
    {
        std::fclose(File);
        LoadGenericScripts("scripts\\gcs\\genericScripts.ini");
        RemoveContentPlayerState(OracleBasePlayerId);
        return -1;
    }
    LoadBase(BaseId);
    SetTrace(&OracleBaseDbHook, &GOracleBase);

    unsigned int Step = 0;
    FOracleBasePlayer Parms = {BaseId, OracleBasePlayerId};

    OracleBaseBegin(Step, "base_enter");
    OracleBaseSend(EMessage::BaseEnter, &Parms);
    OracleBaseEnd();

    OracleBaseBegin(++Step, "vendor_offers");
    RefreshMissionVendorOffers(State);
    OracleBaseEnd();

    std::vector<unsigned int> Rooms;
    for (std::vector<FBaseRoom>::const_iterator It = Base->Rooms.begin(); It != Base->Rooms.end(); ++It)
        Rooms.push_back(It->RoomId);
    for (size_t Index = 0; Index < Rooms.size(); ++Index)
    {
        OracleBaseBegin(++Step, "room");
        FOracleBasePlayer Location = {Rooms[Index], OracleBasePlayerId};
        OracleBaseSend(EMessage::LocationEnter, &Location);
        OracleBaseSend(EMessage::LocationExit, &Location);
        OracleBaseEnd();
    }

    unsigned int BarRoom = 0;
    unsigned int Bartender = 0;
    OracleBaseFindBartender(&BarRoom, &Bartender);
    FOracleBasePlayer Bar = {BarRoom, OracleBasePlayerId};
    OracleBaseBegin(++Step, "bar");
    OracleBaseSend(EMessage::LocationEnter, &Bar);
    OracleBaseEnd();

    OracleBaseBegin(++Step, "select_bartender");
    FOracleBasePlayer Select = {Bartender, OracleBasePlayerId};
    OracleBaseSend(EMessage::GFObjSelect, &Select);
    OracleBaseEnd();

    FMissionOfferAcceptMsg Accept;
    std::memset(&Accept, 0, sizeof(Accept));
    std::map<unsigned int, FBaseVisit>::const_iterator Visit = GBaseVisits.find(OracleBasePlayerId);
    Accept.OfferId =
        Visit != GBaseVisits.end() && !Visit->second.VendorOffers.empty() ? Visit->second.VendorOffers.front().OfferId : 0u;
    Accept.Kind = 2;
    Accept.bAccepted = 1;
    Accept.PlayerId = OracleBasePlayerId;
    OracleBaseBegin(++Step, "accept_vendor");
    OracleBaseSend(EMessage::MissionResponse, &Accept);
    OracleBaseEnd();

    OracleBaseBegin(++Step, "base_exit");
    OracleBaseSend(EMessage::LocationExit, &Bar);
    OracleBaseSend(EMessage::BaseExit, &Parms);
    OracleBaseEnd();

    SetTrace(nullptr, nullptr);
    int const Records = GOracleBase.Records;
    GOracleBase = FOracleSession();
    std::fclose(File);
    DestroyInstance(Support);
    LoadGenericScripts("scripts\\gcs\\genericScripts.ini");
    RemoveContentPlayerState(OracleBasePlayerId);
    return Records;
}
