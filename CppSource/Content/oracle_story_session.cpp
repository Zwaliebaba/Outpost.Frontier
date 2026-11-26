// Oracle instrumentation (design/migration-plan.md, Phase 0; work package P3.5d, decision record
// 0008 point 10 and question 9): the Tier B story trace, Story and its missions through two
// chapter transitions, for the Rust comparison (crates/game-logic/tests/story_session.rs). A new
// export, not part of the reference DLL's interface: only Oracle.exe sim calls it, for the session
// file CppSource/Oracle/story_session.txt (`dump story_session`), right after the multiplayer
// start-up, so a game run never reaches it.
//
// The synthetic player 7 is registered with ReplaceContentPlayerState in he01_02_Base's system,
// with no ship and no server slot, and the base's rooms are loaded in Server.dll's base database,
// as OracleDumpRmSession does for its base. The export runs these steps, each from
// std::srand(OracleStorySeed + step), every message through the bus (_SendMessage to one
// controller):
//   0 launcher      pub::Controller::Create("Content.dll", "StoryLauncher", null, BELOW_NORMAL);
//   1 login         PlayerLogin {7} to the launcher, which creates Story;
//   2 load          PlayerLoad {7, the [StoryInfo] of newplayer.fl} to Story, which creates
//                   Mission_01a, loads and starts it;
//   3 accept        MissionResponse {Mission_01a, 0, accepted, 7} to the mission;
//   4 succeed       the mission's desired state SUCCEED and its ApplyDesiredMissionState: Story
//                   destroys it and creates and starts Mission_01b;
//   5 teardown      Server.dll's OracleBusTick(0): the queued controllers torn down;
//   6 base_enter    GameSupport's case of BaseEnter {he01_02_Base, 7} (run inline, as
//                   OracleDumpRmSession runs it), then BaseEnter to Mission_01b, whose override
//                   reports the acceptance;
//   7 succeed       Mission_01b's success: Story destroys it and sets chapter 5's worth;
//   8 teardown;
//   9 worth         PlayerWorthChanged {7} to Story: the worth is below chapter 5's;
//  10 load_chapter  PlayerLoad {7, [StoryInfo] at chapter 21 with the worth 0} to Story;
//  11 worth         PlayerWorthChanged {7} to Story: Mission_07 created and started;
//  12 ship_bought   PlayerShipArchSet {7} to Story: the standing with gd_bh_grp;
//  13 teardown      Mission_07, Story and the launcher destroyed (pub::Controller::Destroy), then
//                   OracleBusTick(0).
// At the end the hooks are off and player 7 is removed.
//
// OracleDumpStorySession(Dir) writes <Dir>\story.jsonl, one JSON object per line, ids unsigned
// decimal, a float as its bits in lower-case hexadecimal, in the format of
// game_logic::content::story::trace and the Rust test's records:
//   {"rec":"step","step":N,"name":"X"}
//   {"rec":"send","step":N,"msg":M}
//   {"rec":"message","step":N,"msg":M}
//   {"rec":"event","step":N,"kind":K,"chapter":C,"controller":I}
//   {"rec":"create","step":N,"name":"X","id":I}
//   {"rec":"destroy","step":N,"id":I}
//   {"rec":"state","step":N,"num":M,"worth":"%08x","bought":B,"mission":"X","controller":C}
//   {"rec":"end","step":N,"draws":D,"num":M,"live":L,"attack":A,"completed":[[t,c],..]}
// and returns the number of records, -1 when the file cannot be opened, the base is unknown, no
// Story or mission was made or Server.dll lacks its exports.

#include "content.h"
#include "BaseDB.h"             // FindContentBaseInfo, FContentBase
#include "ContentPlayerState.h" // ReplaceContentPlayerState, RemoveContentPlayerState
#include "MissionScript.h"      // FMissionScript
#include "Populators.h"         // GSpacePopAttackClamp
#include "SystemInfo.h"         // FindSystemInfo
#include "common_ini.h"
#include "common_server.h"
#include "common_server_controller.h"
#include "oracle_story_trace.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace
{
    using pub::Controller::EMessage;

    unsigned int const OracleStorySeed = 0x4f524143u;
    unsigned int const OracleStoryPlayerId = 7u;
    char const* const OracleStoryBaseName = "he01_02_Base";

    // newplayer.fl's [StoryInfo] (a new character's), and a save at chapter 21 whose worth is
    // reached at once.
    char const OracleStoryNewPlayer[] = "[StoryInfo]\nMission = Mission_01a\nMissionNum = 1\ndelta_worth = -1\n";
    char const OracleStoryChapter21[] =
        "[StoryInfo]\nship_bought = false\nMission = No_Mission\nMissionNum = 21\ndelta_worth = 0.000000\n";

    typedef void(__cdecl* FOracleBaseDbLoadBase)(unsigned int BaseId);
    typedef void(__cdecl* FOracleBusTick)(float Dt);

    // The message parameter blocks (Story.cpp:45-55, GameSupport.cpp, BaseSupport.h:50-56).
    struct FOracleStoryLoad
    {
        unsigned int PlayerId;
        INI_Reader* Reader;
    };

    struct FOracleStoryBasePlayer
    {
        unsigned int Id;
        unsigned int PlayerId;
    };

    struct FOracleStoryAccept
    {
        unsigned int OfferId;
        unsigned int Kind;
        unsigned char bAccepted;
        unsigned int PlayerId;
    };

    struct FOracleStorySession
    {
        FILE* File = nullptr;
        unsigned int Step = 0;
        int Records = 0;
    };

    FOracleStorySession GOracleStory;
    unsigned int GOracleStoryId = 0;
    FMissionScript* GOracleStoryMission = nullptr;
    unsigned int GOracleStoryMissionId = 0;

    unsigned int OracleStoryBits(float Value)
    {
        unsigned int Bits = 0;
        std::memcpy(&Bits, &Value, sizeof(Bits));
        return Bits;
    }

    // The draws since std::srand(Seed), as OracleDumpRmSession counts them.
    int OracleStoryCountDraws(unsigned int Seed)
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

    void OracleStoryBegin(unsigned int Step, char const* Name)
    {
        GOracleStory.Step = Step;
        std::fprintf(GOracleStory.File, "{\"rec\":\"step\",\"step\":%u,\"name\":\"%s\"}\n", Step, Name);
        ++GOracleStory.Records;
        std::srand(OracleStorySeed + Step);
    }

    // A message the session sends, to one controller through the bus.
    void OracleStorySend(unsigned int Controller, EMessage Message, void const* Parms)
    {
        std::fprintf(GOracleStory.File, "{\"rec\":\"send\",\"step\":%u,\"msg\":%u}\n", GOracleStory.Step,
                     static_cast<unsigned int>(Message));
        ++GOracleStory.Records;
        pub::Controller::_SendMessage(Controller, Message, Parms);
    }

    // The step's `end` record.
    void OracleStoryEnd()
    {
        FILE* const Out = GOracleStory.File;
        int const Draws = OracleStoryCountDraws(OracleStorySeed + GOracleStory.Step);
        FContentPlayerState const* const State = FindContentPlayerState(OracleStoryPlayerId);
        std::fprintf(Out, "{\"rec\":\"end\",\"step\":%u,\"draws\":%d,\"num\":%u,\"live\":%u,\"attack\":%u,\"completed\":[",
                     GOracleStory.Step, Draws, State != nullptr ? State->MissionNumber : 0u,
                     State != nullptr ? State->ValueD8 : 0u, static_cast<unsigned int>(GSpacePopAttackClamp));
        if (State != nullptr)
        {
            bool bFirst = true;
            for (std::map<unsigned char, int>::const_iterator It = State->Stats.RandomMissionsCompleted.begin();
                 It != State->Stats.RandomMissionsCompleted.end(); ++It, bFirst = false)
                std::fprintf(Out, "%s[%u,%d]", bFirst ? "" : ",", static_cast<unsigned int>(It->first), It->second);
        }
        std::fprintf(Out, "]}\n");
        ++GOracleStory.Records;
    }

    // PlayerLoad {7, a reader over Text} to Story.
    void OracleStoryLoad(char const* Text)
    {
        INI_Reader Reader;
        Reader.open_memory(Text, static_cast<unsigned int>(std::strlen(Text)));
        FOracleStoryLoad Load = {OracleStoryPlayerId, &Reader};
        OracleStorySend(GOracleStoryId, EMessage::PlayerLoad, &Load);
        Reader.close();
    }

    // The noted mission's success, as an action's SUCCEED and the handler's tail make it.
    void OracleStorySucceed()
    {
        GOracleStoryMission->MissionState = G_MissionStateSucceed;
        GOracleStoryMission->ApplyDesiredMissionState();
    }
}

void OracleStoryNoteStory(unsigned int ControllerId)
{
    GOracleStoryId = ControllerId;
}

void OracleStoryNoteMission(FMissionScript* Mission, unsigned int ControllerId)
{
    GOracleStoryMission = Mission;
    GOracleStoryMissionId = ControllerId;
}

void OracleStoryTraceMessage(unsigned int MessageId)
{
    if (GOracleStory.File == nullptr)
        return;
    std::fprintf(GOracleStory.File, "{\"rec\":\"message\",\"step\":%u,\"msg\":%u}\n", GOracleStory.Step, MessageId);
    ++GOracleStory.Records;
}

void OracleStoryTraceEvent(unsigned int EventKind, unsigned int Chapter, unsigned int Controller)
{
    if (GOracleStory.File == nullptr)
        return;
    std::fprintf(GOracleStory.File, "{\"rec\":\"event\",\"step\":%u,\"kind\":%u,\"chapter\":%u,\"controller\":%u}\n",
                 GOracleStory.Step, EventKind, Chapter, Controller);
    ++GOracleStory.Records;
}

void OracleStoryTraceCreate(char const* Name, unsigned int ControllerId)
{
    if (GOracleStory.File == nullptr)
        return;
    std::fprintf(GOracleStory.File, "{\"rec\":\"create\",\"step\":%u,\"name\":\"%s\",\"id\":%u}\n", GOracleStory.Step,
                 Name, ControllerId);
    ++GOracleStory.Records;
}

void OracleStoryTraceDestroy(unsigned int ControllerId)
{
    if (GOracleStory.File == nullptr)
        return;
    std::fprintf(GOracleStory.File, "{\"rec\":\"destroy\",\"step\":%u,\"id\":%u}\n", GOracleStory.Step, ControllerId);
    ++GOracleStory.Records;
}

void OracleStoryTraceState(int MissionNum, float DeltaWorth, bool bShipBought, char const* MissionName,
                           unsigned int MissionController)
{
    if (GOracleStory.File == nullptr)
        return;
    std::fprintf(GOracleStory.File,
                 "{\"rec\":\"state\",\"step\":%u,\"num\":%d,\"worth\":\"%08x\",\"bought\":%u,\"mission\":\"%s\","
                 "\"controller\":%u}\n",
                 GOracleStory.Step, MissionNum, OracleStoryBits(DeltaWorth), bShipBought ? 1u : 0u, MissionName,
                 MissionController);
    ++GOracleStory.Records;
}

extern "C" __declspec(dllexport) int __cdecl OracleDumpStorySession(char const* Dir)
{
    if (Dir == nullptr)
        return -1;
    HMODULE const Server = GetModuleHandleA("server.dll");
    FOracleBaseDbLoadBase const LoadBase =
        Server != nullptr ? (FOracleBaseDbLoadBase)GetProcAddress(Server, "OracleBaseDbLoadBase") : nullptr;
    FOracleBusTick const BusTick = Server != nullptr ? (FOracleBusTick)GetProcAddress(Server, "OracleBusTick") : nullptr;
    if (LoadBase == nullptr || BusTick == nullptr)
        return -1;
    unsigned int BaseId = 0;
    pub::GetBaseID(BaseId, OracleStoryBaseName);
    if (FindContentBaseInfo(&BaseId) == nullptr)
        return -1;
    std::string const Path = std::string(Dir) + "\\story.jsonl";
    FILE* const File = std::fopen(Path.c_str(), "wb");
    if (File == nullptr)
        return -1;

    // Player 7 in the base's system, no ship.
    FContentPlayerState* const State = ReplaceContentPlayerState(OracleStoryPlayerId);
    unsigned int SystemId = 0;
    if (pub::GetSystem(SystemId, BaseId) == 0)
        State->SystemRecord = FindSystemInfo(&SystemId);
    LoadBase(BaseId);

    GOracleStory = FOracleStorySession();
    GOracleStoryId = 0;
    GOracleStoryMission = nullptr;
    GOracleStoryMissionId = 0;
    // The hooks record from here on.
    GOracleStory.File = File;


    unsigned int const Player = OracleStoryPlayerId;
    unsigned int Step = 0;
    int Records = -1;

    OracleStoryBegin(Step, "launcher");
    unsigned int const Launcher =
        pub::Controller::Create("Content.dll", "StoryLauncher", nullptr, pub::Controller::BELOW_NORMAL);
    OracleStoryEnd();

    OracleStoryBegin(++Step, "login");
    OracleStorySend(Launcher, EMessage::PlayerLogin, &Player);
    OracleStoryEnd();

    // Each mission is the one the factory made last; a step that should make one stops the session
    // when the noted controller did not change, so no stale mission is used.
    unsigned int Previous = 0;
    bool bMade = GOracleStoryId != 0;
    if (bMade)
    {
        OracleStoryBegin(++Step, "load");
        OracleStoryLoad(OracleStoryNewPlayer);
        OracleStoryEnd();
        bMade = GOracleStoryMission != nullptr && GOracleStoryMissionId != Previous;
    }
    if (bMade)
    {
        FOracleStoryAccept Accept;
        std::memset(&Accept, 0, sizeof(Accept));
        Accept.OfferId = GOracleStoryMissionId;
        Accept.bAccepted = 1;
        Accept.PlayerId = OracleStoryPlayerId;
        OracleStoryBegin(++Step, "accept");
        OracleStorySend(GOracleStoryMissionId, EMessage::MissionResponse, &Accept);
        OracleStoryEnd();

        Previous = GOracleStoryMissionId;
        OracleStoryBegin(++Step, "succeed");
        OracleStorySucceed();
        OracleStoryEnd();
        bMade = GOracleStoryMissionId != Previous;

        OracleStoryBegin(++Step, "teardown");
        BusTick(0.0f);
        OracleStoryEnd();
    }
    if (bMade)
    {
        // GameSupport's case (GameSupport.cpp:556-567, run inline as OracleDumpRmSession runs it),
        // then the message to the mission.
        OracleStoryBegin(++Step, "base_enter");
        FOracleStoryBasePlayer const Enter = {BaseId, OracleStoryPlayerId};
        std::fprintf(GOracleStory.File, "{\"rec\":\"send\",\"step\":%u,\"msg\":%u}\n", GOracleStory.Step,
                     static_cast<unsigned int>(EMessage::BaseEnter));
        ++GOracleStory.Records;
        FindContentPlayerState(OracleStoryPlayerId)->OnBaseChange(BaseId);
        FContentBase* const Base = FindContentBaseInfo(&BaseId);
        if (Base != nullptr)
        {
            Base->Load(nullptr);
            Base->OnPlayerEnter(OracleStoryPlayerId);
        }
        pub::Controller::_SendMessage(GOracleStoryMissionId, EMessage::BaseEnter, &Enter);
        OracleStoryEnd();

        OracleStoryBegin(++Step, "succeed");
        OracleStorySucceed();
        OracleStoryEnd();

        OracleStoryBegin(++Step, "teardown");
        BusTick(0.0f);
        OracleStoryEnd();

        OracleStoryBegin(++Step, "worth");
        OracleStorySend(GOracleStoryId, EMessage::PlayerWorthChanged, &Player);
        OracleStoryEnd();

        OracleStoryBegin(++Step, "load_chapter");
        OracleStoryLoad(OracleStoryChapter21);
        OracleStoryEnd();

        Previous = GOracleStoryMissionId;
        OracleStoryBegin(++Step, "worth");
        OracleStorySend(GOracleStoryId, EMessage::PlayerWorthChanged, &Player);
        OracleStoryEnd();
        bMade = GOracleStoryMissionId != Previous;

        OracleStoryBegin(++Step, "ship_bought");
        OracleStorySend(GOracleStoryId, EMessage::PlayerShipArchSet, &Player);
        OracleStoryEnd();

        OracleStoryBegin(++Step, "teardown");
        pub::Controller::Destroy(GOracleStoryMissionId);
        pub::Controller::Destroy(GOracleStoryId);
        pub::Controller::Destroy(Launcher);
        BusTick(0.0f);
        OracleStoryEnd();
        if (bMade)
            Records = GOracleStory.Records;
    }

    GOracleStory.File = nullptr;
    GOracleStory = FOracleStorySession();
    GOracleStoryId = 0;
    GOracleStoryMission = nullptr;
    GOracleStoryMissionId = 0;
    std::fclose(File);
    RemoveContentPlayerState(OracleStoryPlayerId);
    return Records;
}
