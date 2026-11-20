// Author: Giperion.

#include "../include/common_server.h"
#include "../include/server_iserverimpl.h"   // IServerImpl, Server
#include "../include/server_cobject.h"
#include "../include/server_gameobject.h"
#include "../include/common_dataconfig.h" // DataConfig::startup / shutdown
#include "../include/common_timing.h" // Timing::Delta
#include "../include/common_tradeoffer.h" // CPlayerTradeOffer
#include "../include/common_id.h"
#include "../include/common_ini.h"
#include "../include/common_goods.h"
#include "../include/common_universe.h"
#include "../include/common_server_playerdb.h"
#include "../include/server_savegame.h"
#include "../include/server_characterset.h" // CCharacterSet (CharacterInfoReq)
#include "../include/common_misc.h"      // safe_strcpy (CharacterSelect key)
#include "../include/common_server_controller.h"
#include "../include/content.h"   // IDLL, CreateInstance / DestroyInstance (controller DLL entry points)
#include "../include/server_sysdb.h"
#include "../include/server_basedb.h"
#include "../include/server_fuse.h"     // ServerFuseDB (the routine / the routine)
#include "../include/rpc_iclient.h"
#include "../include/rpc_iclient_args.h" // FServerSetReputationInfo / FServerCharSelectVerifiedInfo
#include "../include/NeuronRuntime.h"           // NeuronRuntime::Data (the routine)
#include "../include/readfile.h"        // IFileSystem::GetFileSysPath (+0x84)
#include "../include/common_physys.h"
#include "../include/common_casteroid.h"   // CmnAsteroid::Update (module storage)
#include "../include/common_animdb.h"     // AnimDB::Update (the routine)
#include "../include/server_createinfo.h" // FShipCreateRequest, Server_Acquire3DMathEngine (PlayerLaunch)
#include "../include/x86math.h"           // I3DMathEngine (PlayerLaunch)
#include "../include/oracle_sim_hooks.h"  // Oracle instrumentation (P2.3 sim)
#include <windows.h>
#include <mmsystem.h>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>
#include <list>
#include <stdexcept>
#include <cstring>
#include <cstddef>

#pragma comment(lib, "winmm.lib")

// Server_ControllerDispatch (common_server_controller.h), pub::Controller::_SendMessage's
// dispatch core, is defined further down this file. baseline's the routine (exported by the
// port from Common.dll as Server_SendMessage) is only `_SendMessage(0 /* broadcast */,
// MsgId, MsgData)`, so every call site that used to go through the empty Common.dll
// dispatcher now calls it directly.
extern "C" int __cdecl Server_ControllerSetHeartbeat(unsigned int Flid, float Interval);
extern "C" int __cdecl Server_ControllerSetTimer(unsigned int Flid, unsigned int Param1,
                                                 unsigned int Param2, float Timeout);
// IServerImpl::SubmitChat's body (baseline FLChatServer.cpp), src/Server/server_chat.cpp.
void Server_SubmitChat(CHAT_ID Sender, unsigned long Size, const void* Data, CHAT_ID Target, int SystemId);

// IClient is the real 126-slot RPC client interface (src/include/rpc_iclient.h).
// A fabricated 125-slot copy used to live here, with invented method names and a
// virtual destructor occupying slot 0 (baseline slot 0 is the FLPACKET_COMMON_FIREWEAPON
// sender). That was an ODR duplicate of the shared header; it is gone.

// baseline exports IClient's default ctor, copy ctor and operator= (rpclocal ships
// ??0IClient@@QAE@XZ / ??4IClient@@QAEAAV0@ABV0@@Z beside ??_7IClient@@6B@, and our
// Server.dll re-exports all three from src/Server/server_classes.cpp). They are
// declared in rpc_iclient.h and defined here; the class is abstract and has no data,
// so every one of them is trivial.
IClient::IClient() {}
IClient::IClient(const IClient& Other) { (void)Other; }
IClient& IClient::operator=(const IClient& Other) { (void)Other; return *this; }

// baseline keeps TWO pairs of client-interface pointers:
// HMODULE of the client DLL          (the routine / the routine)
// IClient*     as loaded             (W 06d43371, R 06d43380 / 06d433d0,
//                                                       zeroed 06d433aa)
// IChatClient* as loaded             (W 06d4337a, R 06d433c0,
//                                                       zeroed 06d433b4)
// IClient*     live, every caller    (W 06d44beb Startup,
//                                                       zeroed 06d45203 Shutdown)
// IChatClient* live, every caller    (W 06d44bf5 Startup, never zeroed)
// Startup copies the loaded pair into the live pair through the routine /
// ; FreeClientDll clears only the loaded pair.
static HMODULE g_hClientDll = nullptr;
static IClient* GLoadedClientInterface = nullptr;
static IChatClient* GLoadedChatClientInterface = nullptr;
// accessed by SendGroupID/SendSystemID in server_classes.cpp
IClient* g_pClientInterface = nullptr;
// baseline .data:the field (set from the routine at 0x06d44bf0). It is the
// IChatClient half of rpclocal's Client object; CPlayerGroup's group/chat
// senders call its single slot (see server_classes.cpp). Nothing in baseline
// ever writes it again, so it outlives Shutdown with its stale value.
IChatClient* g_pChatClientInterface = nullptr;
// baseline's server clock is TWO doubles plus the Delta at module storage:
// returns the field + the field (module storage FLD / FADD).
// is the running sum ElapseTime adds read_dt() into (module storage)
// and nothing ever resets; the field is the BASE a loaded game or world file
// sets (module storage the routine, module storage the routine) and Startup
// zeroes (module storage). g_ElapsedTimeAccum is the field, GServerTimeBase
// is the field (split 2026-09-24, claude:srv-save-2; they used to be one).
static Timing::Delta g_TimingDelta;
double g_ElapsedTimeAccum = 0.0;
double GServerTimeBase = 0.0;

// ---------------------------------------------------------------------------
// Server_LoadSinglePlayerState -- baseline the routine (module storage ..
// ), cdecl (INI_Reader*), plain RET. Called only by the
// CharacterSelect worker the routine in single player (module storage), on the
// reader it opened over the selected character file.
// get_state(&saved)
// time = 0.0 (both dwords)
// read_header loop until is_header("Time") (module storage);
//             on a hit: read_value (result ignored), time = get_value_float(0)
// set_state(&saved)
// the field = time ; 06d448a1 Delta(06d8f970).start()
// Reputation::FreeFeelings() ; 06d448ae LoadFeelings(reader)
// the routine(0x40, reader) -- the broadcast, data = the reader*
// ---------------------------------------------------------------------------
void Server_LoadSinglePlayerState(INI_Reader& Reader) {
    INI_Reader::State Saved;
    Reader.get_state(Saved);
    double Time = 0.0;
    if (Reader.read_header()) {
        do {
            if (Reader.is_header("Time")) {
                Reader.read_value();
                Time = Reader.get_value_float(0);
                break;
            }
        } while (Reader.read_header());
    }
    Reader.set_state(Saved);
    GServerTimeBase = Time;
    g_TimingDelta.start();
    ::Reputation::FreeFeelings();
    ::Reputation::LoadFeelings(Reader);
    Server_ControllerDispatch(0, pub::Controller::EMessage::LoadGame, &Reader);
}

// , the 11-dword counter block GetServerStats copies out
// (module storage). Startup zero-fills it (module storage); Login bumps +0x04 /
// +0x0c, PlayerDB::logout +0x08 (server_classes.cpp), AbortMission +0x1c. It used
// to be three unrelated statics plus a never-written copy inside GetServerStats.
ServerStats GServerStats = {};
// .data:the field -- IServerS.cpp's own lazily acquired I3DMathEngine
// (the routine), used only by PlayerLaunch (module storage / 06cefceb /
// ), each time in the `MOV EAX,[DAT] / CMP EAX,EBP / JNZ / CALL
// / MOV [DAT],EAX` idiom.
static I3DMathEngine* GIServerSMathEngine = nullptr;
// .data:the field -- [OutpostFrontier] initial_world. Read also by
// PlayerDB::create_restart_file (module storage), hence external linkage.
char g_InitialWorldPath[260] = {0};

static IClient* Server_LoadClientDll(const char* module_path);
static void Server_FreeClientDll();
static IChatClient* Server_GetLoadedChatClientInterface();
static IClient* Server_GetLoadedClientInterface();
static void Server_ReadWorldIni(const char* config_path);
static void Server_LoadWorldState();
unsigned int Server_ControllerCreate(const char* dll_path, const char* instance_name, void* parms, unsigned int priority);
static void Server_LoadInitialDlls(const char* IniPath);
static void Server_PlayerSubsystemInit(unsigned int max_players, unsigned int allow_pvp);
static void Server_CleanupPhysics();
static void Server_ControllerCleanup();
static void Server_PhysicsUpdate(float dt);

// one system's update; defined next to Server_PhysicsUpdate.
static unsigned int StarSystem_Update(StarSystem* Sys, float Dt);
static void Server_TimerTick(float dt);
static void Server_TimerFiring(float dt);

// Oracle instrumentation (P2.3 sim): Oracle.exe sim loads its scenario system as a player's
// arrival would (StarSystem::load), since it has no player. No game module calls it.
extern "C" SERVER_API bool __cdecl OracleSimLoadSystem(unsigned int SystemId) {
    FStarSystem* const System = SysDB_LookupSystem(SystemId);
    if (System == nullptr) return false;
    System->load();
    return System->bLoaded == 1;
}

static PlayerSlot* GetPlayerBySession(unsigned int session_id) {
    return PlayerDB_GetPlayer(session_id);
}

// ---------------------------------------------------------------------------
// PlayerData member the routine (module storage .. module storage, thiscall on
// the slot, plain RET). Ported 2026-09-24 (claude-code:w3-baseenter):
// CALL [->COMMON.DLL::SinglePlayer] / TEST AL,AL / JNZ out
// ECX = [.data:the field]          ; the IClient
// EDX = [this + 0x348]                ; client_id
// PUSH 0 / PUSH EDX / CALL [EAX + 0x24] ; IClient slot 9
// i.e. outside single player, IClient::PushToClient(client_id, NULL packet). No null
// test on the interface. Callers: BaseExit's two refusals (module storage,
// ), the inlined LocationExit refusal inside BaseExit
// (module storage) and LocationExit's own (module storage). What the MP
// transport does with a NULL packet on slot 9 is not recovered, so the name
// only says what the bytes do.
// ---------------------------------------------------------------------------
static void PlayerData_SendClientNullPacket(PlayerData& Slot) {
    if (SinglePlayer()) return;
    ::g_pClientInterface->PushToClient(Slot.client_id, nullptr);
}

// IServerImpl and the exported object `Server` (ordinal 524) are declared in
// src/include/server_iserverimpl.h; `Server` is defined at the end of this file. The
// member bodies below were written in-class until 2026-10-15 and moved out of the class
// verbatim (no logic change) when the declaration was published for OutpostServer.exe.

    // .. module storage (RET 0x4). Traced term by term
    // 2026-09-24 (claude-code:w5-server-06d4); the port's boot diagnostics
    // (ServerStartupLog to runtime_boot.log / OutputDebugString) were removed --
    // baseline writes nothing here.
bool IServerImpl::Startup(const SStartupInfo& info) {
        GServerTimeBase = 0.0;             // /module storage -- the BASE; the field is not reset
        g_TimingDelta.start();             // , this = the field
        // .. module storage. Oracle instrumentation (P2.3 sim, the reproducibility work, owner
        // 2026-11-19): under the harness's fixed clock the seed is GOracleSimHooks.StartupSeed,
        // not the wall clock; outside Oracle.exe sim the hooks are zero and this is
        // srand(timeGetTime()) as before.
        srand(GOracleSimHooks.bFixedClock ? GOracleSimHooks.StartupSeed : timeGetTime());
        RefreshLoadingProgress();
        if (CreateID("OutpostFrontier") == 0)   // .. module storage
            return false;                  // XOR AL,AL
        // MOV ECX,.data:Players / CALL PlayerDB::init --
        // the exported global, not a member of this object.
        Players.init(info.MaxPlayers, info.bAllowNewPlayers != 0);
        // Oracle instrumentation (as above): InitServerAccount's first start on a machine (no
        // accounts.cfg yet) seeds from the wall clock again and draws six values; the harness
        // seeds once more, so a machine's first run and its later runs load alike.
        if (GOracleSimHooks.bFixedClock)
            srand(GOracleSimHooks.StartupSeed);
        RefreshLoadingProgress();
        // baseline 0x64b8f: the routine((LPCSTR)param_1) — the MODULE path, i.e. the
        // first 0x104 bytes of SStartupInfo. This is what LoadLibrary+GetClientInterface
        // must use; the INI path at +0x104 is a different string entirely.
        Server_LoadClientDll(info.ModulePath);
        // CALL the routine / 06d44beb MOV [the field],EAX
        g_pClientInterface = Server_GetLoadedClientInterface();
        // CALL the routine / 06d44bf5 MOV [the field],EAX
        g_pChatClientInterface = Server_GetLoadedChatClientInterface();
        RefreshLoadingProgress();
        memset(&GServerStats, 0, sizeof(GServerStats));   // REP STOSD x11
        // MOV EBX,EAX / NEG EBX / SBB BL,BL / INC BL / JZ.
        // That idiom yields 1 only when DataConfig::startup returned ZERO, and the JZ
        // then SKIPS the whole init block. Success is a 0 return, and the same byte is
        // what Startup gives back. src/Common/common_dataconfig.cpp keeps that convention.
        // baseline 0x64bdd / 0x64bf6: both DataConfig::startup and the routine take
        // param_1 + 0x104 — the INI path, not the module path.
        const bool bConfigOk = (DataConfig::startup(info.IniPath, nullptr, nullptr) == 0);
        if (bConfigOk) {
            Server_ReadWorldIni(info.IniPath);
            Server_LoadInitialDlls(info.IniPath);   // PUSH EDI (param_1+0x104)
            RefreshLoadingProgress();
            // .. module storage -- MP only: the routine.
            if (!SinglePlayer())
                Server_LoadWorldState();
            RefreshLoadingProgress();
            Server_SubsystemInit();
            RefreshLoadingProgress();
            Server_PlayerSubsystemInit(info.MaxPlayers, info.bAllowPvp);
            RefreshLoadingProgress();
        }
        // baseline 0x64c5f: (**(code **)(*the field + 0x28))(param_1+0x208, param_1+0x20c)
        // — IClient slot 10, the session-init slot that returns bool. Reached on
        // BOTH paths (the branch is the JZ target at module storage) and called
        // through the field with no null test (module storage .. module storage).
        g_pClientInterface->Startup(info.MaxPlayers, info.Port);
        RefreshLoadingProgress();
        return bConfigOk;                  // MOV AL,BL
    }

void IServerImpl::Shutdown() {
        Players.logout_all();
        Server_CleanupPhysics();
        CObject::ReleaseAll();
        Server_SubsystemShutdown();
        Server_ControllerCleanup();
        DataConfig::shutdown();
        if (g_pClientInterface) {
            // IClient slot 15 (+0x3c): tears the real client interface down.
            g_pClientInterface->Shutdown();
            Server_FreeClientDll();
            g_pClientInterface = nullptr;
        }
        Players.free();
    }

int IServerImpl::Update() {
        // IClient slot 16 (+0x40): the client-side pump.
        if (g_pClientInterface) g_pClientInterface->Update();
        return 1;
    }

void IServerImpl::ElapseTime(float dt) {
        double real_dt = g_TimingDelta.read_dt();
        OracleSimFixElapsed(real_dt);   // Oracle instrumentation (P2.3 sim): fixed clock only
        if (real_dt < 0.0) real_dt = 0.0;
        g_ElapsedTimeAccum += real_dt;
        Server_PhysicsUpdate(dt);
        Server_SubsystemUpdate(dt);                // , module storage PUSH ESI (the dt argument)
        Server_TimerTick(dt);
        Server_TimerFiring(dt);
    }

    // ---------------------------------------------------------------------------
    // Slots 0..8 -- the object-command handlers (ported 2026-09-24,
    // claude-code:w5-server-06cf). Every one of them opens with the routine
    // (&Id, &Obj, &System), the object lookup by id: AL = found, Obj = the
    // IObjRW, System = its owning StarSystem. the routine is Server_FindObject
    // (server_gameobject.h); the handlers that only keep the object call its
    // object-only wrapper Server_LookupGameObject, whose null is exactly baseline's
    // AL == 0 (a false return always leaves the object out null). The ones that
    // test the SYSTEM out call Server_FindObject itself: for an in-transit object
    // that out is 0, not the object's OwnerSystem. The object's IObjAffect sub-object is
    // baseline's `LEA ECX,[obj + 4]`; its slots are declared in common_iface.h.
    // None of these bodies sends a pub::Controller message.
    // ---------------------------------------------------------------------------

    // Slot 0: FireWeapon -- baseline module storage .. 06cf0e3c, RET 8.
    // the routine(&info.ObjectId, &obj, &sys); AL == 0 -> out
    // obj == 0 -> out
    // obj->vt[+0x160] (is_destroyed) true -> out
    // the routine on Players' raw slot (terminate_trade)
    // count = Hardpoints.Begin ? (End - Begin) >> 1 : 0
    // IObjAffect +0x34 (Hardpoints.Begin, count, &info.TargetPos, player_id)
void IServerImpl::FireWeapon(unsigned int player_id, const XFireWeaponInfo& info) {
        GameObject* const Obj = Server_LookupGameObject(info.ObjectId);
        if (Obj == nullptr)
            return;
        if (Obj->is_destroyed())
            return;
        PlayerData_TerminateTrade(Players[player_id]);
        // data() is the vector's _First (+0x14) as baseline pushes it, size() its
        // `_First ? (_Last - _First) >> 1 : 0`.
        static_cast<IObjAffect*>(Obj)->slot_34(info.Hardpoints.data(),
                                               static_cast<unsigned>(info.Hardpoints.size()),
                                               &info.TargetPos, static_cast<int>(player_id));
    }
    // Slot 1: ActivateEquip -- baseline module storage .. 06cf09cf, RET 8.
    // the routine(&info.ObjectId, ...); miss (06cf099c), obj == 0
    //             (06cf09a4), is_destroyed (06cf09b2) -> out
    // IObjAffect +0x4c (&info.EquipId, byte info+6, player_id)
void IServerImpl::ActivateEquip(unsigned int player_id, const XActivateEquip& info) {
        GameObject* const Obj = Server_LookupGameObject(info.ObjectId);
        if (Obj == nullptr)
            return;
        if (Obj->is_destroyed())
            return;
        static_cast<IObjAffect*>(Obj)->slot_4c(&info.EquipId, info.bActivate,
                                               static_cast<int>(player_id));
    }
    // Slot 2: ActivateCruise -- baseline module storage .. 06cf0a31, RET 8.
    // lookup; miss (06cf09fc), obj == 0 (06cf0a04), is_destroyed
    //             (06cf0a12) -> out
    // IObjAffect +0x50 (byte info+4, byte info+5, player_id)
void IServerImpl::ActivateCruise(unsigned int player_id, const XActivateCruise& info) {
        GameObject* const Obj = Server_LookupGameObject(info.ObjectId);
        if (Obj == nullptr)
            return;
        if (Obj->is_destroyed())
            return;
        static_cast<IObjAffect*>(Obj)->slot_50(info.bActivate, info.Field_0x05 != 0,
                                               static_cast<int>(player_id));
    }
    // Slot 3: ActivateThrusters -- baseline module storage .. 06cf0a8b, RET 8.
    // lookup; miss (06cf0a5c), obj == 0 (06cf0a64), is_destroyed
    //             (06cf0a72) -> out
    // IObjAffect +0x54 (byte info+4, player_id)
void IServerImpl::ActivateThrusters(unsigned int player_id, const XActivateThrusters& info) {
        GameObject* const Obj = Server_LookupGameObject(info.ObjectId);
        if (Obj == nullptr)
            return;
        if (Obj->is_destroyed())
            return;
        static_cast<IObjAffect*>(Obj)->slot_54(info.bActivate, static_cast<int>(player_id));
    }
    // Slot 4: SetTarget -- baseline module storage .. 06cf0d4d, RET 8.
    // lookup of info.ObjectId; miss (06cf0c8f), obj == 0 (06cf0c9b),
    //             is_destroyed (06cf0cad) -> out
    // info.SubObjId == SUBOBJ_ID_NONE:
    // the routine(&info.TargetObjectId, &target, &sys); a miss
    //               stores 0 into target (06cf0cdf) -- no other test on it
    // IObjAffect +0x48 set_target(target, info.TargetSubObjId, player_id)
    //   else:
    // the same lookup of the target (06cf0d24 zeroes it on a miss)
    // IObjAffect +0x44 (&info.SubObjId, target, info.TargetSubObjId,
    //               player_id)
void IServerImpl::SetTarget(unsigned int player_id, const XSetTarget& info) {
        GameObject* const Obj = Server_LookupGameObject(info.ObjectId);
        if (Obj == nullptr)
            return;
        if (Obj->is_destroyed())
            return;
        IObjAffect* const Affect = static_cast<IObjAffect*>(Obj);
        if (info.SubObjId == SUBOBJ_ID_NONE)
        {
            IObjRW* const Target = Server_LookupGameObject(info.TargetObjectId);
            Affect->set_target(Target, info.TargetSubObjId, static_cast<int>(player_id));
            return;
        }
        IObjRW* const Target = Server_LookupGameObject(info.TargetObjectId);
        Affect->slot_44(&info.SubObjId, Target, info.TargetSubObjId,
                        static_cast<int>(player_id));
    }
    // Slot 5: TractorObjects -- baseline module storage .. 06cf0c68, RET 8, with an
    // SEH frame for the local std::vector<IObjRW*>.
    // lookup of info.ObjectId; miss (06cf0b84), obj == 0 (06cf0b92),
    //             is_destroyed (06cf0ba5) -> out
    // the vector: begin / end / capacity zeroed
    // n = TargetIds.Begin ? (End - Begin) >> 2 : 0; the loop runs while
    //             i < n, signed (JLE / JL)
    // per id: the routine(&ids[i], &target, &sys); a miss zeroes
    //             target (06cf0bfd) and skips; a null target skips (06cf0c0d)
    // vector<IObjRW*>::_Insert_n(end, 1, &target) -- a push_back
    // IObjAffect +0x3c (&info.TractorEquipId, vector, player_id)
    // operator delete on the vector's storage
void IServerImpl::TractorObjects(unsigned int player_id, const XTractorObjects& info) {
        GameObject* const Obj = Server_LookupGameObject(info.ObjectId);
        if (Obj == nullptr)
            return;
        if (Obj->is_destroyed())
            return;
        std::vector<IObjRW*> Targets;
        int const Count = static_cast<int>(info.TargetIds.size());
        for (int Index = 0; Index < Count; ++Index)
        {
            IObjRW* const Target = Server_LookupGameObject(info.TargetIds[Index]);
            if (Target == nullptr)
                continue;
            Targets.push_back(Target);
        }
        static_cast<IObjAffect*>(Obj)->slot_3c(&info.TractorEquipId, Targets,
                                               static_cast<int>(player_id));
    }
    // Slot 6: GoTradelane -- baseline module storage .. 06cf0ae5, RET 8.
    // info.ObjectId != the raw slot's ship id ([sid*0x418 + Players
    //             - 0x34], no PlayerDB::is_valid) -> out
    // the routine(&info.ObjectId, &obj, &sys); AL == 0 -> out
    // sys == 0 -> out (the SYSTEM out is tested here, not the object)
    // sys->the routine(info.ObjectId, info+4, info+8, byte info+0xc,
    //             PUSH 0 -- a float 0.0) = StarSystem::notify_go_tradelane
    // The port's controller event 0x16 and its IClient SendCommonGoTradeLane call are gone: both
    // are the routine's own work (server_classes.cpp), not this handler's.
void IServerImpl::GoTradelane(unsigned int player_id, const XGoTradelane& info) {
        if (info.ObjectId != Players[player_id].ship_object_id) return;
        GameObject* Obj = nullptr;
        StarSystem* Sys = nullptr;
        if (!Server_FindObject(info.ObjectId, Obj, Sys))
            return;
        if (Sys == nullptr)
            return;
        Sys->notify_go_tradelane(info.ObjectId, info.TradelaneRing1, info.TradelaneRing2,
                                 info.Field_0x0c, 0.0f);
    }
    // Slot 7: StopTradelane -- baseline module storage .. 06cf0b43, RET 0x10.
    // arg1 != the raw slot's ship id -> out
    // the routine(&arg1, &obj, &sys); AL == 0 -> out
    // sys == 0 -> out
    // sys->the routine(arg1, arg2, arg3) = StarSystem::
    //             notify_stop_tradelane(ShipId, EntranceId, ExitId)
void IServerImpl::StopTradelane(unsigned int session_id, unsigned int arg1, unsigned int arg2, unsigned int arg3) {
        if (arg1 != Players[session_id].ship_object_id) return;
        GameObject* Obj = nullptr;
        StarSystem* Sys = nullptr;
        if (!Server_FindObject(arg1, Obj, Sys))
            return;
        if (Sys == nullptr)
            return;
        Sys->notify_stop_tradelane(arg1, arg2, arg3);
    }
    // Slot 8: JettisonCargo -- baseline module storage .. 06cf0dbc, RET 8.
    // lookup; miss (06cf0d6c), obj == 0 (06cf0d74), is_destroyed
    //             (06cf0d82) -> out
    // the routine on Players' raw slot (terminate_trade)
    // IObjAffect +0x38 (info.ItemId, info.Count, player_id)
void IServerImpl::JettisonCargo(unsigned int player_id, const XJettisonCargo& info) {
        GameObject* const Obj = Server_LookupGameObject(info.ObjectId);
        if (Obj == nullptr)
            return;
        if (Obj->is_destroyed())
            return;
        PlayerData_TerminateTrade(Players[player_id]);
        static_cast<IObjAffect*>(Obj)->slot_38(info.ItemId, static_cast<int>(info.Count),
                                               static_cast<int>(player_id));
    }
    // Slot 13: baseline body is the shared bare RET at Server 0x06ce1050 / rpclocal
    // — a genuine no-op in both modules.
void IServerImpl::SetActiveConnection(EFLConnection /*conn*/) {}
    // Slot 14: rpclocal 0x06d8f3f0 is MOV AL,1 / RET 8. Server.dll's body (0x06d519e0)
    // is likewise a RET n stub.
bool IServerImpl::SwapConnections(EFLConnection /*conn_a*/, EFLConnection /*conn_b*/) { return true; }
void IServerImpl::Connect(const char* server_name, unsigned short* port) {}

    // IServerImpl::DisConnect -- baseline module storage .. module storage, in full:
    // PlayerDB::is_valid inlined (sid != 0, sid-1 < max,
    //                       client_id != 0); nothing else -- no port `active` test
    // [slot-0x20] & 0x3fffffff -> LocationInfoRequest(room, sid, false),
    //             then LocationExit([slot-0x20] RE-READ, sid)
    // [slot-0x2c] & 0x3fffffff -> BaseInfoRequest(base, sid, false),
    //             then BaseExit([slot-0x2c] RE-READ, sid)
    // PlayerDB::logout(sid)
    // The port used to gate both halves on its own location_active/base_active
    // flags as well; baseline only tests the masked ids, and those flags are gone.
    // `type` is accepted and ignored, as in baseline.
void IServerImpl::DisConnect(unsigned int session_id, EFLConnection type) {
        (void)type;
        if (!Players.is_valid(session_id)) return;
        // All four calls are `MOV ECX,.data:Server` + a direct CALL (06d4501d,
        // , 06d45051, 06d45067) -- the global object, not `this`.
        if ((Players[session_id].current_location & 0x3fffffff) != 0) {
            Server.LocationInfoRequest(Players[session_id].current_location, session_id, false);
            Server.LocationExit(Players[session_id].current_location, session_id);
        }
        if ((Players[session_id].current_base & 0x3fffffff) != 0) {
            Server.BaseInfoRequest(Players[session_id].current_base, session_id, false);
            Server.BaseExit(Players[session_id].current_base, session_id);
        }
        Players.logout(session_id);
    }

    // IServerImpl::OnConnect -- baseline module storage .. module storage, in full:
    // MOV ECX,[the field]          ; the live IClient, no null test
    // PUSH .data:the field         ; &Players + 0x20 == &Players.m_MOTD
    // PUSH EDX                        ; session_id
    // CALL [EAX + 0x1cc]              ; IClient slot 115
    // RET 4
    // Players is module storage (MOV ECX,.data:Players at 06d44bd4), and PlayerDB
    // +0x20 is the MOTD std::wstring -- the same (session, &message) pair
    // PlayerDB::SetMOTD sends through this slot to every connected session.
    // m_MOTD is protected; GetMOTD() returns a reference to that very member, so
    // its address is baseline's &the field.
void IServerImpl::OnConnect(unsigned int session_id) {
        g_pClientInterface->SendServerMiscObjUpdateMotd(session_id, Players.GetMOTD());
    }

void IServerImpl::Login(const SLoginInfo& info, unsigned int session_id) {
        // proceed when SinglePlayer(), or when the slot has
        // no client yet (player[-0xd0] == 0). A slot that already has a client is a
        // duplicate login and is dropped. player[-0xd0] is +0x348, a client id dword,
        // not a pointer -- see PlayerSlot::client_id.
        bool is_sp = SinglePlayer();
        if (!is_sp) {
            PlayerSlot* player = GetPlayerBySession(session_id);
            if (player && player->client_id != 0) return;
        }
        // -adjacent: MOV ECX,.data:Players / CALL
        // PlayerDB::login(SLoginInfo const&, unsigned int). The port used to
        // route this through Common.dll's PlayerDB_login, which wrote the other
        // table and never reached PlayerDB::load_user_data at all.
        unsigned char result = Players.login(info, session_id);
        if (result == 3) Server_ControllerDispatch(0, pub::Controller::EMessage::PlayerLogin, &session_id);
        // IClient slot 12 (+0x30) SendServerLoginResponse — ch2 op 0x02
        // FLPACKET_SERVER_LOGINRESPONSE. baseline 0x06d883a0: MOV DL,byte ptr [Status] — the
        // status byte travels BY REFERENCE, not by value.
        if (g_pClientInterface)
            g_pClientInterface->SendServerLoginResponse(session_id, result);
        // CMP byte ptr [ESP+0x3],0x3 reads back the stored LOGIN RESULT
        // byte, not the session id. (analysis renders the spill slot as the high byte of
        // param_2, which is what the previous port code copied.)
        if (result == 3) {
            GServerStats.LoginSuccessCount++;
        } else {
            GServerStats.LoginFailCount++;
        }
    }

void IServerImpl::CharacterInfoReq(unsigned int session_id, bool full_info) {
        // baseline IServerImpl::CharacterInfoReq, module storage .. module storage
        // (RET 8).
        // .. module storage -- sid != 0, sid-1 < MaxPlayers,
        // [slot+0x348] != 0: PlayerDB::is_valid.
        if (!Players.is_valid(session_id)) return;

        // .. module storage -- the CCharacterSet local at
        // ESP+0xc: OwnerList<CCharacterSet::CharacterInfo> vptr
        // (module storage), Head / Tail / bUseHeap / FreeList zeroed.
        CCharacterSet Set;

        // .. module storage -- only when the bool is false does the
        // slot fill it (the routine). With the bool set, the list goes out empty.
        if (!full_info)
            PlayerData_BuildCharacterSet(Players[session_id], Set);

        // .. module storage -- IClient +0x34 (slot 13, ch2 0x03
        // FLPACKET_SERVER_CHARACTERINFO) with (sid, &set); no null test on the
        // interface.
        g_pClientInterface->SendServerCharacterInfo(session_id, Set);

        // .. module storage -- the set's destructor: OwnerList::free
        // (each record destroyed and freed, then the nodes), then ~MetaList.
    }

void IServerImpl::CharacterSelect(const CHARACTER_ID& char_id, unsigned int session_id) {
        // baseline IServerImpl::CharacterSelect, module storage .. module storage
        // (RET 8). Frame: S = ESP after the EBX/EBP/EDI pushes; S' = S - 4 once
        // ESI is pushed at module storage.

        // .. module storage -- sid != 0, sid-1 < MaxPlayers,
        // [slot+0x348] != 0: PlayerDB::is_valid.
        if (!Players.is_valid(session_id)) return;
        PlayerData& Slot = Players[session_id];

        // .. module storage -- the worker the routine on the slot
        // (server_classes.cpp); a miss or a failed load ends here.
        if (!PlayerData_SelectCharacter(Slot, char_id, session_id)) return;

        // .. module storage -- the SETREPUTATION argument at S+0x44:
        // FmtStr ctors (the routine), +0x10 = 0, byte +0x14 = 0, word +0x48 = 0,
        // +0x00 = [slot+0x318], +0x10 = operator new(group_count() * 8), then
        // Reputation::Vibe::Get and the wide name wcscpy'd in (or +0x48 = 0).
        // +0x04 is never written by baseline (stack residue); zeroed here.
        FServerSetReputationInfo Info;
        Info.Attitude = 0.0f;
        Info.Field_0x08 = 0;
        Info.Field_0x0c = 0;
        Info.Relations = nullptr;
        Info.RelationCount = 0;
        Info.Pad_0x15[0] = 0;
        Info.Pad_0x15[1] = 0;
        Info.Pad_0x15[2] = 0;
        Info.DisplayName[0] = L'\0';
        Info.VibeHandle = Slot.rep_vibe;
        Info.Relations = static_cast< ::Reputation::Relation*>(
            ::operator new(::Reputation::group_count() * 8));
        wchar_t const* VibeName = nullptr;                               // S+0xc, module storage
        ::Reputation::Vibe::Get(Info.VibeHandle, Info.Field_0x08, Info.Field_0x0c,
                                Info.RelationCount, Info.Relations, Info.NameA,
                                Info.NameB, VibeName);
        if (VibeName == nullptr) {
            Info.DisplayName[0] = L'\0';
        } else {
            wcscpy(Info.DisplayName, VibeName);                          // , unbounded
        }

        // .. module storage -- PlayerDB::traverse_active over every
        // active slot: IClient +0x108 (SendServerSetReputation)([slot_i+0x348], &Info).
        for (PlayerData* Active = Players.traverse_active(nullptr); Active != nullptr;
             Active = Players.traverse_active(Active)) {
            g_pClientInterface->SendServerSetReputation(Active->client_id, Info);
        }

        // .. module storage -- the routine(0x3b, S'+0x1c): seven
        // dwords, {[slot+0x3e0], 0, sid, 0, 0, 0, 0}.
        unsigned int EnterEvent[7];
        EnterEvent[0] = Slot.system_id;
        EnterEvent[1] = 0;
        EnterEvent[2] = session_id;
        EnterEvent[3] = 0;
        EnterEvent[4] = 0;
        EnterEvent[5] = 0;
        EnterEvent[6] = 0;
        Server_ControllerDispatch(0, pub::Controller::EMessage::SystemEnter, EnterEvent);

        // .. module storage -- IClient +0x38 (SendServerCharSelectVerified)(sid, S'+0x38):
        // {Info.VibeHandle, (unwritten), the routine = the field + the field}.
        FServerCharSelectVerifiedInfo Verified;
        Verified.VibeHandle = Info.VibeHandle;
        Verified.Unused_0x04 = 0;
        Verified.ServerTime = g_ElapsedTimeAccum + GServerTimeBase;
        g_pClientInterface->SendServerCharSelectVerified(session_id, Verified);

        // .. module storage -- GetAffiliation(copy of [slot+0x318],
        // out = 0xffffffff, false); the result code is not tested.
        int AffiliationVibe = Slot.rep_vibe;
        unsigned int Affiliation = 0xffffffffu;
        ::Reputation::Vibe::GetAffiliation(AffiliationVibe, Affiliation, false);

        // .. module storage -- IClient +0x1d0
        // (SendServerMiscObjUpdateAffiliation)(sid, [slot+0x3e4] ship, affiliation).
        g_pClientInterface->SendServerMiscObjUpdateAffiliation(session_id, Slot.ship_object_id, Affiliation);

        // .. module storage -- operator delete of the relation
        // array (the routine); the two FmtStr destructors run with Info.
        ::operator delete(Info.Relations);
    }

    // Slot 21: baseline body is the shared RET stub (Server 0x06ce1050); rpclocal
    // sends ch3 op 0x38 FLPACKET_CLIENT_REQUESTNEWCHARINFO.
void IServerImpl::NewCharacterInfoReq(unsigned int /*session_id*/) {}

void IServerImpl::CreateNewCharacter(const SCreateCharacterInfo& info, unsigned int session_id) {
        // .. module storage (RET 8), no guards:
        // PlayerDB::create_new_character(info, sid), then this->vtable +0x4c --
        // CharacterInfoReq -- (sid, false) on success (module storage) and
        // (sid, true) on failure (module storage).
        if (Players.create_new_character(info, session_id)) {
            CharacterInfoReq(session_id, false);
        } else {
            CharacterInfoReq(session_id, true);
        }
    }

void IServerImpl::DestroyCharacter(const CHARACTER_ID& char_id, unsigned int session_id) {
        // baseline 0x656b0: validate, the routine delete char, vtable[19](session_id, 0)
        // .. module storage -- sid != 0, sid-1 < MaxPlayers,
        // [slot+0x348] != 0: exactly PlayerDB::is_valid.
        if (!Players.is_valid(session_id)) return;
        PlayerSlot* player = &Players[session_id];

        // .. module storage -- slot->the routine(char_id, sid),
        // result ignored: deselect if selected, find the record, refuse
        // Restart.fl, drop it from the account maps, unlink the file, erase the
        // record (PlayerData_DestroyCharacter, server_classes.cpp). REPLACED
        // 2026-09-24 (claude:srv-save-2): this used to delete by a path built
        // from the character NAME under the accounts root and then wipe the
        // live slot (active = false, session_id = 0, cash, ship...), none of
        // which baseline does.
        PlayerData_DestroyCharacter(*player, char_id, session_id);

        // .. module storage -- this->vtable +0x4c(sid, 0),
        // i.e. CharacterInfoReq(sid, false).
        CharacterInfoReq(session_id, false);
    }

void IServerImpl::CharacterSkipAutosave(unsigned int session_id) {
        // PlayerDB::is_valid inlined (06d45386 sid
        // != 0, 06d45393 sid-1 < max, 06d453ac client_id != 0) -- that is `!p` --
        // then the routine(slot). No `active` test (w4-server-guards).
        PlayerSlot* p = GetPlayerBySession(session_id);
        if (!p) return;
        PlayerData_SkipNextAutosave(*p);   // , module storage
    }
    // ---------------------------------------------------------------------------
    // The in-base request family, module storage .. module storage. Rewritten
    // 2026-09-24 (claude-code:w3-baseenter). Every member but ReqHullStatus is the
    // same eighteen-instruction shape:
    //   CALL [->COMMON.DLL::SinglePlayer] / TEST AL,AL / JNZ work
    //   MOV  reg,[sid*0x418 + Players - 0x24] / TEST reg,reg / JZ out   ; +0x3F4
    //   work: LEA ECX,[sid*0x418 + Players - 0x418] / CALL <PlayerData member>
    // i.e. `SinglePlayer() || entered_base_id != 0` (an UNMASKED dword test), on
    // a slot indexed RAW -- no PlayerDB::is_valid, no client-id test, no port
    // `active` test. The +0x3F4 dword is the base BaseEnter wrote at
    // ; the port used to read it as `bool bAuthorized`, which
    // nothing ever set, so in MP all of these returned at once.
    // ---------------------------------------------------------------------------

    // ReqShipArch -- baseline module storage .. module storage (RET 0x8): gate at
    // , then module storage CALL the routine(arch) on the slot --
    // PlayerData_SetShipArch (GetShip validation, collision-group refill,
    // IClient slot 21, message 0x35 with &client_id). The port used to store the
    // archetype itself and send 0x35 with an invented {session, arch} payload.
void IServerImpl::ReqShipArch(unsigned int arch_id, unsigned int session_id) {
        if (!SinglePlayer() && Players[session_id].entered_base_id == 0) return;
        PlayerData_SetShipArch(Players[session_id], arch_id);
    }
    // ReqHullStatus -- baseline module storage .. module storage (RET 0x8), the one
    // member of the family that validates the session first:
    // PlayerDB::is_valid inlined (sid != 0, sid-1 < max,
    //                       [slot+0x348] != 0)
    // SinglePlayer() || [slot-0x24] (+0x3F4) != 0
    // [slot+0x3ec] & 0x3fffffff != 0 (current_base)
    // FLD hull / FCOMP double [module storage] (= 0.0, read
    //                       from the PE) / FNSTSW / TEST AH,0x5 / JP keep /
    //                       hull := 0 -- i.e. `if (hull < 0.0) hull = 0` (C0 set
    //                       alone gives odd parity; equal, greater and unordered
    //                       all give even parity and keep the value, NaN included)
    // [ESP+8] := 1, tail JMP the routine(hull, true)
    // is PlayerData_SetRelativeHealth (+0x268, IClient slot 22).
    // The port used to write `hull_hp` and skip both the base test and the
    // notification.
void IServerImpl::ReqHullStatus(float hull_status, unsigned int session_id) {
        if (!Players.is_valid(session_id)) return;
        if (!SinglePlayer() && Players[session_id].entered_base_id == 0) return;
        if ((Players[session_id].current_base & 0x3fffffff) == 0) return;
        if (hull_status < 0.0f) hull_status = 0.0f;
        PlayerData_SetRelativeHealth(Players[session_id], hull_status, true);
    }
    // ReqCollisionGroups -- baseline module storage .. module storage (RET 0x8):
    // gate at module storage, then module storage CALL the routine(groups) on the
    // slot -- PlayerData_SetCollisionGroups (+0x26C, IClient slot 23).
void IServerImpl::ReqCollisionGroups(const std::list<CollisionGroupDesc>& groups, unsigned int session_id) {
        if (!SinglePlayer() && Players[session_id].entered_base_id == 0) return;
        PlayerData_SetCollisionGroups(Players[session_id], groups);
    }
    // ReqEquipment -- baseline module storage .. module storage (RET 0x8): gate at
    // , then module storage CALL the routine(equipment) on the
    // slot -- PlayerData_LoadEquipment (list replace, EquipIdMaker ids for
    // SUBOBJ_ID_NONE entries, IClient slot 24, message 0 with &client_id). The
    // port used to drop the list and send message 0 with &session_id.
void IServerImpl::ReqEquipment(const EquipDescList& equipment, unsigned int session_id) {
        if (!SinglePlayer() && Players[session_id].entered_base_id == 0) return;
        PlayerData_LoadEquipment(Players[session_id], equipment);
    }

    // Slot 29 — the real slot 29, not the "Connect2" duplicate that used to sit here.

    // THERE IS NO MERGE RULE. Read off the listing 2026-09-17 (wave 11): baseline's
    // ?ReqCargo@IServerImpl@@UAEXABVEquipDescList@@I@Z is exported at RVA 0x48880
    // as ordinal 366, and that address holds exactly one instruction:
    // c20800  RET  0x8
    // The linker ICF-folded it with ?Connect@IServerImpl@@UAEXPBDPAG@Z (ordinal
    // 83) -- the listing prints both mangled names over the same byte -- because
    // both take eight bytes of stack arguments and both do nothing. The IServer
    // vtable confirms the slot: ??_7IServerImpl@@6BIServer@@@ is at RVA 0x85bcc
    // (module storage) and module storage, i.e. base + 0x74 == slot 29, is the
    // one that points here.

    // So this handler must NOT touch the slot's EquipDescList, and must NOT
    // broadcast: the previous port's Server_ControllerDispatch(0, 0, &session_id)
    // was a message baseline never sends. The whole packet is dead on the server
    // side; the client's cargo state arrives through ReqAddItem / ReqRemoveItem
    // and ReqEquipment instead.
void IServerImpl::ReqCargo(const EquipDescList& cargo, unsigned int session_id) {
        (void)cargo;
        (void)session_id;
    }
    // ReqAddItem -- baseline module storage .. module storage (RET 0x18).
    // MOV  EDX,[sid*0x418 + Players - 0x24] ; +0x3F4 entered_base_id
    //                   TEST EDX,EDX / JZ out                 ; unless SinglePlayer()
    // StringAlloc(hardpoint, false), result kept in a 1-dword
    //                   stack slot that is then passed BY ADDRESS -- i.e. a
    //                   CacheString built on the stack
    // the routine(slot, arch, &hpCacheString, count, health,
    //                                b_mounted, /*bNoCombine*/0, /*bNotify*/1)
    // Slot indexed raw; hardpoint handed to StringAlloc unchecked (the port's
    // StringAlloc maps null to its empty string itself).
void IServerImpl::ReqAddItem(unsigned int arch_id, const char* hardpoint, int count, float health,
                    unsigned char b_mounted, unsigned int session_id) {
        if (!SinglePlayer() && Players[session_id].entered_base_id == 0) return;
        CacheString Hp;
        Hp.Value = StringAlloc(hardpoint, false);
        PlayerData_AddEquipment(Players[session_id], arch_id, Hp, count, health, b_mounted,
                                /*bNoCombine*/false, /*bNotify*/true);
    }
    // ReqRemoveItem -- baseline module storage .. module storage (RET 0xc).
    // +0x3F4 gate, as above
    // MOV dword ptr [ESP + param_3],0x0   ; bNotify := 0
    // JMP the routine                    ; tail call
    // The zeroed third stack slot is the notify flag, so this path deliberately
    // does NOT send IClient slot 27 -- the client asked for the removal and
    // already knows.
void IServerImpl::ReqRemoveItem(unsigned short equip_id, int count, unsigned int session_id) {
        if (!SinglePlayer() && Players[session_id].entered_base_id == 0) return;
        PlayerData_RemoveEquipment(Players[session_id], equip_id, count, /*bNotify*/false);
    }
    // ReqModifyItem -- baseline module storage .. module storage (RET 0x18).
    // +0x3F4 gate, as above
    // the routine(slot, equip_id, &hpCacheString, count,
    //                                health, b_mounted, 0)
    // RETs 0x18 (six stack dwords) but only reads five; the sixth,
    // the literal 0 pushed at module storage, is dead in baseline too, so the port
    // signature drops it.
void IServerImpl::ReqModifyItem(unsigned short equip_id, const char* hardpoint, int count, float health,
                       unsigned char b_mounted, unsigned int session_id) {
        if (!SinglePlayer() && Players[session_id].entered_base_id == 0) return;
        CacheString Hp;
        Hp.Value = StringAlloc(hardpoint, false);
        PlayerData_ModifyEquipment(Players[session_id], equip_id, Hp, count, health, b_mounted);
    }

void IServerImpl::ReqSetCash(int cash, unsigned int session_id) {
        // .. module storage, in full: outside single player
        // the session's [slot - 0x24] (+0x3F4, entered_base_id) must be non-zero,
        // then the routine(cash) on the raw-indexed slot. No other check.
        // is PlayerData_SetCash: MP selected-character gate, clamp to
        // [0, 999999999], store, IClient SETCASH with &client_id,
        // (0, &client_id).
        // Cash is an INT in baseline, not a float -- the mangled name is ?ReqSetCash@...@@UAEXHI@Z.
        if (!SinglePlayer() && Players[session_id].entered_base_id == 0) return;
        PlayerData_SetCash(Players[session_id], cash);
    }

void IServerImpl::ReqChangeCash(int delta, unsigned int session_id) {
        // .. module storage: the same +0x3F4 test as
        // ReqSetCash, then `MOV [ESP + param_2],0` and a tail JMP to
        // (delta, false) on the slot -- PlayerData_AdjustCash without
        // the +0x320 total.
        if (!SinglePlayer() && Players[session_id].entered_base_id == 0) return;
        PlayerData_AdjustCash(Players[session_id], delta, false);
    }

    // IServerImpl::BaseEnter -- baseline module storage .. module storage (RET 0x8),
    // re-ported line by line 2026-09-24 (claude-code:w3-baseenter). EBX = base id
    // (param_1), EBP = session id (param_2), EDI = slot END (sid*0x418 + Players).
    // PlayerDB::is_valid inlined; no port `active` test
    // [EDI-0x24] (+0x3F4 entered_base_id) & 0x3fffffff != 0
    //                       -> return (already inside a base)
    // EAX = [EDI-0x8] (+0x410 exited_base_id), parked in
    //                       param_2's stack slot
    // Universe::get_base(base) == 0 -> return
    // exited == 0 || base == 0 || exited == base -> skip the
    //                       system change
    // old = [slot-0x38] (+0x3E0 system_id); new = the base's
    //                       vtable +0x10 (the port's IBase::SystemId);
    // (0x3a, {old, new, sid})  (ESP+0x1c block)
    // [slot-0x38] = new
    // the routine(0x3b, {new, old, sid, 0, 0, 0, 0})
    //                       (the 0x1c-byte block at ESP+0x28; the same seven-dword
    //                       shape CharacterSelect and RequestCreateShip send)
    // [EDI-0x2c] = [EDI-0x28] = [EDI-0x24] = base
    //                       (current_base, last_base_id, entered_base_id)
    // the routine(BaseDB, sid, base) -- BaseDB::load_base
    // the routine(1, {base, sid})
    // !SinglePlayer(): the routine then the routine
    // The port used to test current_base instead of +0x3F4, compare the old system
    // against the new one instead of testing +0x410, send 0x3a/0x3b with one shared
    // invented {sid, old, new} block, and skip last_base_id and +0x3F4 entirely.
void IServerImpl::BaseEnter(unsigned int base_id, unsigned int session_id) {
        if (!Players.is_valid(session_id)) return;
        PlayerData& Slot = Players[session_id];
        if ((Slot.entered_base_id & 0x3fffffff) != 0) return;

        unsigned int const ExitedBase = Slot.exited_base_id;
        Universe::IBase const* Base = Universe::get_base(base_id);
        if (Base == nullptr) return;

        if (ExitedBase != 0 && base_id != 0 && ExitedBase != base_id) {
            unsigned int const OldSystem = Slot.system_id;
            unsigned int const NewSystem = Base->SystemId;   // IBase vtable +0x10
            unsigned int LeaveEvent[3] = { OldSystem, NewSystem, session_id };
            Server_ControllerDispatch(0, pub::Controller::EMessage::SystemExit, LeaveEvent);
            Slot.system_id = NewSystem;
            unsigned int EnterEvent[7] = { NewSystem, OldSystem, session_id, 0, 0, 0, 0 };
            Server_ControllerDispatch(0, pub::Controller::EMessage::SystemEnter, EnterEvent);
        }

        Slot.current_base = base_id;      // [EDI-0x2c]
        Slot.last_base_id = base_id;      // [EDI-0x28]
        Slot.entered_base_id = base_id;   // [EDI-0x24]

        GetBaseDB()->load_base(session_id, base_id);

        unsigned int BaseEvent[2] = { base_id, session_id };
        Server_ControllerDispatch(0, pub::Controller::EMessage::BaseEnter, BaseEvent);

        if (!SinglePlayer()) {
            PlayerData_ResetTotalEarned(Players[session_id]);
            PlayerData_SnapshotEquipment(Players[session_id]);
        }
    }

    // IServerImpl::BaseExit -- baseline module storage .. module storage (RET 0x8),
    // re-ported line by line 2026-09-24 (claude-code:w3-baseenter). param_1 = base
    // id, EBP = session id, ESI = slot base, EDI = sid*0x418.
    // PlayerDB::is_valid inlined; no port `active` test
    // ([slot-0x24] & 0x3fffffff) == 0 -> the routine(slot),
    //                       return
    // param_1 != [slot-0x24] (raw compare) -> the routine,
    //                       return
    // room = [slot+0x3f8]; (room & 0x3fffffff) == 0 -> skip
    // LocationInfoRequest(room, sid, false) INLINED: the
    //                       is_valid test, the routine() != 0, then
    // (BaseDB, sid, room, 0)
    // LocationExit([slot+0x3f8] re-read, sid) INLINED: the
    //                       is_valid test, current_base / room masks and the
    //                       room == param test, the routine(0x2a, {room, sid}),
    // (BaseDB, sid, room), [slot-0x20] = 0 --
    //                       else the routine(slot)
    // the routine(2, {param_1, sid})
    // the routine(BaseDB, sid, param_1) -- BaseDB::unload_base
    // the routine(slot), the autosave
    // [slot+0x410] = [slot-0x2c]; [slot+0x3ec] = 0;
    //                       [slot+0x3f4] = 0
    // The two inlined bodies are exactly this class's own LocationInfoRequest /
    // LocationExit (same order, same arguments), so the port calls them. The port
    // used to test base_active and a masked current_base, skip the room exit
    // event and the routine, send param_1 from current_base, and never write
    // +0x410 or +0x3F4.
void IServerImpl::BaseExit(unsigned int base_id, unsigned int session_id) {
        if (!Players.is_valid(session_id)) return;
        if ((Players[session_id].entered_base_id & 0x3fffffff) == 0) {
            PlayerData_SendClientNullPacket(Players[session_id]);
            return;
        }
        if (base_id != Players[session_id].entered_base_id) {
            PlayerData_SendClientNullPacket(Players[session_id]);
            return;
        }
        if ((Players[session_id].current_location & 0x3fffffff) != 0) {
            LocationInfoRequest(Players[session_id].current_location, session_id, false);
            LocationExit(Players[session_id].current_location, session_id);
        }

        unsigned int ExitEvent[2] = { base_id, session_id };
        Server_ControllerDispatch(0, pub::Controller::EMessage::BaseExit, ExitEvent);
        GetBaseDB()->unload_base(session_id, base_id);
        PlayerData_AutoSave(Players[session_id]);

        Players[session_id].exited_base_id = Players[session_id].current_base;
        Players[session_id].current_base = 0;
        Players[session_id].entered_base_id = 0;
    }

    // IServerImpl::LocationEnter -- baseline module storage .. module storage (RET 0x8),
    // re-ported line by line 2026-09-24 (claude-code:w5-server-06d1):
    // PlayerDB::is_valid inlined -> return
    // (current_location & 0x3fffffff) != 0 -> return
    // (current_base & 0x3fffffff) == 0 -> return
    // the routine(BaseDB, param_1) == 0 -> return (the
    //                       miss FDUMPs inside find_location)
    // [slot-0x20] = param_1
    // the routine(BaseDB, sid, param_1)
    // the routine(0x29, {param_1, sid})
void IServerImpl::LocationEnter(unsigned int location_id, unsigned int session_id) {
        if (!Players.is_valid(session_id)) return;
        if ((Players[session_id].current_location & 0x3fffffff) != 0) return;
        if ((Players[session_id].current_base & 0x3fffffff) == 0) return;
        if (GetBaseDB()->find_location(location_id) == nullptr) return;
        Players[session_id].current_location = location_id;
        GetBaseDB()->enter_location(session_id, location_id);
        unsigned int RoomEvent[2] = { location_id, session_id };
        Server_ControllerDispatch(0, pub::Controller::EMessage::LocationEnter, RoomEvent);
    }

    // IServerImpl::LocationExit -- baseline module storage .. module storage (RET 0x8),
    // re-ported line by line 2026-09-24 (claude-code:w3-baseenter):
    // PlayerDB::is_valid inlined -> plain return on failure
    // (current_base & 0x3fffffff) == 0, or
    //                       (current_location & 0x3fffffff) == 0, or
    //                       current_location != param_1 (raw compare)
    //                       -> the routine(slot) and return (06d17ab5)
    // the routine(0x2a, {param_1, sid})
    // the routine(BaseDB, sid, param_1) -- BaseDB::exit_location
    //                       (the NPC-character teardown + handle release; NOT the
    //                       content-callback unsubscribe the port used to do here,
    //                       which is LocationInfoRequest(.., false)'s job)
    // [slot-0x20] = 0
    // BaseExit inlines this whole body at module storage .. module storage.
void IServerImpl::LocationExit(unsigned int location_id, unsigned int session_id) {
        if (!Players.is_valid(session_id)) return;
        if ((Players[session_id].current_base & 0x3fffffff) == 0 ||
            (Players[session_id].current_location & 0x3fffffff) == 0 ||
            Players[session_id].current_location != location_id) {
            PlayerData_SendClientNullPacket(Players[session_id]);
            return;
        }
        unsigned int RoomEvent[2] = { location_id, session_id };
        Server_ControllerDispatch(0, pub::Controller::EMessage::LocationExit, RoomEvent);
        GetBaseDB()->exit_location(session_id, location_id);
        Players[session_id].current_location = 0;
    }

    // IServerImpl::BaseInfoRequest -- baseline module storage..06d17b34, which then
    // calls the routine(BaseDB, session_id, base_id, b_full). Both halves were
    // rewritten from the listing 2026-09-24 (claude-code:server-triage-s2): the
    // port ignored b_full, so a b_full == false call SUBSCRIBED the session
    // instead of unsubscribing it, and it skipped the wrapper's "only the base
    // you are in" test.
void IServerImpl::BaseInfoRequest(unsigned int base_id, unsigned int session_id, bool b_full) {
        // PlayerDB::is_valid inlined -- and nothing more;
        // the port's extra `active` test was removed 2026-09-24
        // (claude-code:w3-baseenter).
        if (!Players.is_valid(session_id)) return;
        PlayerSlot* player = &Players[session_id];
        // a subscribe is honoured only for the base the
        // player is in (raw compare against player[-0x2c], no 0x3fffffff mask).
        if (b_full && base_id != player->current_base) return;

        // the accessor's result is tested (the routine
        // returns the static's address, so this never fails), then the routine.
        if (GetBaseDB() == nullptr) return;
        GetBaseDB()->base_info(session_id, base_id, b_full);
    }
    // IServerImpl::LocationInfoRequest -- baseline module storage..06d17ba4, then
    // (BaseDB, session_id, location_id, b_full). Rewritten from the
    // listing 2026-09-24 (claude-code:server-triage-s2): b_full was ignored (see
    // BaseInfoRequest), the wrapper's current-location test was missing, and a
    // room miss fell into an invented "system location" path that dispatched
    // controller event 0x31 -- baseline has no such path, it FDUMPs and returns.
void IServerImpl::LocationInfoRequest(unsigned int location_id, unsigned int session_id, bool b_full) {
        // PlayerDB::is_valid inlined -- and nothing more;
        // the port's extra `active` test was removed 2026-09-24
        // (claude-code:w3-baseenter).
        if (!Players.is_valid(session_id)) return;
        PlayerSlot* player = &Players[session_id];
        // compare against player[+0x3f8], unmasked.
        if (b_full && location_id != player->current_location) return;

        // the same accessor test, then the routine.
        if (GetBaseDB() == nullptr) return;
        GetBaseDB()->location_info(session_id, location_id, b_full);
    }
    // IServerImpl::GFObjSelect -- baseline module storage .. module storage (RET 0x8),
    // re-read 2026-09-24 (claude-code:w3-baseenter):
    // EAX = [sid*0x418 + Players - 0x2c]   ; current_base, slot indexed RAW
    // TEST EAX,0x3fffffff / JZ out
    // Universe::get_base(current_base) == 0 -> out
    // the routine(0x1b, {param_1, sid})
    // No session validation at all. The port used to test its own `base_id`
    // mirror (really +0x3F4's twin) plus `active`, and skipped get_base.
void IServerImpl::GFObjSelect(unsigned int object_id, unsigned int session_id) {
        unsigned int const CurrentBase = Players[session_id].current_base;
        if ((CurrentBase & 0x3fffffff) == 0) return;
        if (Universe::get_base(CurrentBase) == nullptr) return;
        unsigned int SelectEvent[2] = { object_id, session_id };
        Server_ControllerDispatch(0, pub::Controller::EMessage::GFObjSelect, SelectEvent);
    }
    // GFGoodVaporized — baseline 0x37c30: packs info struct + session_id, sends message 0x1e
    // Interface limitation: baseline takes (SGFGoodVaporizedInfo const&, session_id)
void IServerImpl::GFGoodVaporized(const SGFGoodVaporizedInfo& info, unsigned int session_id) {
        // has no branch at all -- no session test; the
        // port's `!p || !p->active` early-out is gone (w4-server-guards).
        // copies the struct's three dwords onto the stack and calls
        // (0x1e, &frame). The field semantics are still unidentified.
        unsigned int msg_data[4] = { info.Field_0x00, info.Field_0x04, info.Field_0x08, session_id };
        Server_ControllerDispatch(0, pub::Controller::EMessage::GFGoodVaporized, msg_data);
    }
// writes a BYTE at +8; bytes +9..+b stay unwritten.
void IServerImpl::MissionResponse(unsigned int Arg0, unsigned long MissionId,
                                 unsigned char bAccepted, unsigned int SessionId) {
    struct FMissionResponseMessage {
        unsigned int Field_0x00; // TODO: forwarded first argument's meaning.
        unsigned long MissionId;
        unsigned char bAccepted;
        unsigned char Padding[3];
        unsigned int SessionId;
    };
    static_assert(sizeof(FMissionResponseMessage) == 0x10, "MissionResponse message size");
    FMissionResponseMessage Message;
    Message.MissionId = MissionId;
    Message.Field_0x00 = Arg0;
    Message.bAccepted = bAccepted;
    Message.SessionId = SessionId;
    Server_ControllerDispatch(0, pub::Controller::EMessage::MissionResponse, &Message);
}
    // TradeResponse -- baseline module storage .. module storage, RET 0xc. No branch
    // at all (no session test, no size test). Frame, off ESP after the prologue:
    // MarshalBuffer::MarshalBuffer() on S+0x70
    // MarshalBuffer::use_read_buffer(data, size)
    // TradeResponseInfo::TradeResponseInfo() on S+0x3c
    // TradeResponseInfo::unmarshal(&buffer)
    // / 06d458c9  MetaList<GoodDesc>(false) + the GoodDescList vptr on
    //             S+0x10 and S+0x24 (= GoodDescList() inlined)
    // S+0x04 = info+0x00, S+0x08 = info+0x04, byte S+0x0c =
    //             info+0x08 (the pad bytes S+0x0d..0x0f are never written)
    // / 06d4590f  GoodDescList::operator= (appending) from info+0x0c and
    //             info+0x20 into S+0x10 / S+0x24
    // S+0x38 = session_id
    // the routine(0x3f, S+0x04) -- the broadcast controller message
    //   then ~OwnerList<GoodDesc> on S+0x24 and S+0x10, ~TradeResponseInfo.
    // So message 0x3f carries {Field00, Field04, bFlag08, Offered, Requested,
    // session_id}: a TradeResponseInfo's members followed by the session id.
void IServerImpl::TradeResponse(const unsigned char* data, int size, unsigned int session_id) {
        MarshalBuffer Buffer;
        Buffer.use_read_buffer(data, size);
        TradeResponseInfo Info;
        Info.unmarshal(&Buffer);

        struct FTradeResponseMessage {
            int Field00;                  // +0x00
            int Field04;                  // +0x04
            bool bFlag08;                 // +0x08
            GoodDescList OfferedGoods;    // +0x0c
            GoodDescList RequestedGoods;  // +0x20
            unsigned int SessionId;       // +0x34
        };
        FTradeResponseMessage Message;                                // / 06d458c9
        Message.Field00 = Info.Field00;
        Message.bFlag08 = Info.bFlag08;
        Message.Field04 = Info.Field04;
        Message.OfferedGoods = Info.OfferedGoods;
        Message.RequestedGoods = Info.RequestedGoods;
        Message.SessionId = session_id;
        Server_ControllerDispatch(0, pub::Controller::EMessage::TradeResponse, &Message);
    }

    // GFGoodBuy — baseline packs struct fields + session_id, sends message 0x1c
void IServerImpl::GFGoodBuy(const SGFGoodBuyInfo& info, unsigned int session_id) {
        // has no branch -- no session test; the port's
        // `!p || !p->active` early-out is gone (w4-server-guards).
        // copies all four dwords onto the stack, then the routine(0x1c, &frame).
        unsigned int msg_data[5] = { info.Field_0x00, info.Field_0x04, info.Field_0x08,
                                     info.Field_0x0c, session_id };
        Server_ControllerDispatch(0, pub::Controller::EMessage::GFGoodBuy, msg_data);
    }
    // GFGoodSell — baseline sends message 0x1d
void IServerImpl::GFGoodSell(const SGFGoodSellInfo& info, unsigned int session_id) {
        // has no branch -- no session test; the port's
        // `!p || !p->active` early-out is gone (w4-server-guards).
        // copies three dwords onto the stack, then the routine(0x1d, &frame).
        unsigned int msg_data[4] = { info.Field_0x00, info.Field_0x04, info.Field_0x08, session_id };
        Server_ControllerDispatch(0, pub::Controller::EMessage::GFGoodSell, msg_data);
    }

    // SystemSwitchOutComplete -- baseline module storage .. 06cef9d6 (RET 8): the
    // player's ship has finished the jump-out animation at a gate/hole; move it
    // into the destination system at the arrival object.
void IServerImpl::SystemSwitchOutComplete(unsigned int ship_id, unsigned int session_id) {
        // PlayerDB::is_valid inlined: sid != 0 (JBE),
        // sid - 1 < max (JNC), client_id (+0x348) != 0 -- PlayerDB_GetPlayer's null.
        unsigned int max_players = PlayerDB_GetMaxPlayers();
        if (session_id == 0 || (session_id - 1) >= max_players) return;
        PlayerSlot* p = GetPlayerBySession(session_id);
        if (!p) return;
        // not in a base, in a ship.
        if ((p->current_base & 0x3fffffff) != 0) return;
        if ((p->ship_object_id & 0x3fffffff) == 0) return;

        // the routine(&ship_id, &Obj, &Sys); a miss
        // or a null system (in transit) ends it. Obj is not null-tested.
        GameObject* Obj = nullptr;
        StarSystem* Sys = nullptr;
        if (!Server_FindObject(ship_id, Obj, Sys)) return;
        if (Sys == nullptr) return;

        // the object's CSimple (the (& 3) accessor,
        // null otherwise) must be owned by this session. baseline reads +0xb4 off the
        // accessor's result without a null test.
        CObject* const ShipRaw = Obj->Object;
        CSimple* const ShipSimple = (ShipRaw != nullptr &&
                                     (static_cast<unsigned int>(ShipRaw->ObjectClass) & 3u) == 3u)
                                        ? static_cast<CSimple*>(ShipRaw) : nullptr;
        if (ShipSimple->OwnerPlayer != session_id) return;

        // the gate is the ship's +0x90 (read off the
        // wrapper with no class test), found in this system under type mask 0xc40
        // (the routine); its CObject must be a CSolar (0x303). Ship+0x90 is the
        // field the port calls DeathSequenceObjId; its writer (module storage,
        // right after controller event 0x11) stores the jump object's id, so the
        // name is a misnomer -- here it is the gate the ship jumped out through.
        unsigned int* const GateIdField = &static_cast<Ship*>(Obj)->DeathSequenceObjId;   // [ESP+0x48]
        GameObject* const Gate = Sys->find_object(*GateIdField, 0xc40);
        if (Gate == nullptr) return;
        CObject* const GateRaw = Gate->Object;
        if (GateRaw == nullptr) return;
        if ((static_cast<unsigned int>(GateRaw->ObjectClass) & 0x303u) != 0x303u) return;
        CSolar* const GateSolar = static_cast<CSolar*>(GateRaw);

        // get_type_bits (vt +0x154); the result is
        // dropped (EAX is overwritten at module storage).
        (void)Gate->get_type_bits();

        // destination system (+0x1b4) and the
        // arrival object's nickname (+0x1b8, kept at [ESP+0x14]).
        unsigned int const DestSystemId = GateSolar->DestSystem;
        ID_String DestGate = GateSolar->DestGate;

        // the routine; a miss FDUMPs (the gate's
        // nickname is pushed though the format has no slot for it) and returns.
        StarSystem* const DestSys = SysDB_LookupSystem(DestSystemId);
        if (DestSys == nullptr) {
            FDUMP(0x100001, "%s(%d) : *** ERROR: FATAL: Couldn't find gate destination system!",
                  "Server\\space\\IServerS.cpp", 0x6b,
                  GateSolar->Nickname.get_string());
            return;
        }

        // controller event 0x25 {ship, gate, the
        // system being left (the routine)}.
        unsigned int Msg25[3];
        Msg25[0] = ship_id;
        Msg25[1] = *GateIdField;
        Msg25[2] = Sys->SystemInfo->NicknameId;
        Server_ControllerDispatch(0, pub::Controller::EMessage::Msg0x25, Msg25);

        // the transit CreateParms at [ESP+0x7c]. The
        // bytes baseline never writes (+0x1f .. +0x25, +0x27) are stack contents
        // there; the port zero-initialises them.
        PhySys::CreateParms Parms = {};
        Parms.Mass = 1.0f;
        Parms.LinearDamping = PhySys::DEFAULT_LINEAR_DAMPING;
        Parms.AngularDamping[0] = PhySys::DEFAULT_ANGULAR_DAMPING.x;
        Parms.AngularDamping[1] = PhySys::DEFAULT_ANGULAR_DAMPING.y;
        Parms.AngularDamping[2] = PhySys::DEFAULT_ANGULAR_DAMPING.z;
        Parms.CollisionGroupId = 0;                                        // +0x14
        Parms.Type = 2;                                                    // +0x18
        Parms.bFlag1c = 0;
        Parms.bEnableCollisions = 1;
        Parms.Name[0] = '\0';                                              // +0x1e
        Parms.bHasInertia = 0;                                             // +0x26
        Parms.Inertia.x = 0.0f;
        Parms.Inertia.y = 0.0f;
        Parms.Inertia.z = 0.0f;

        // the routine: out of the old system.
        Sys->remove_ship(Obj, &Parms);
        // the routine(sid, DestSystemId).
        Sys->player_switched_out(session_id, DestSystemId);
        // the routine(sid) on the destination.
        DestSys->add_player(session_id);

        // the arrival object (the routine) picks
        // the spot (Solar +0x250); without one, an FDUMP, the origin and the
        // identity orientation.
        Vector Pos;
        Matrix Orient;
        GameObject* const Arrival = DestSys->find_solar(&DestGate.Id);
        if (Arrival != nullptr) {
            static_cast<Solar*>(Arrival)->ChooseSystemArrivalLocation(Obj, Pos, Orient);
        } else {
            FDUMP(0x100001, "%s(%d) : *** ERROR: Couldn't find arrival gate %s.",
                  "Server\\space\\IServerS.cpp", 0x8d,
                  DestGate.get_string());
            Pos.x = 0.0f;
            Pos.y = 0.0f;
            Pos.z = 0.0f;
            Orient = Matrix(1.0f, 0.0f, 0.0f,
                            0.0f, 1.0f, 0.0f,
                            0.0f, 0.0f, 1.0f);
        }

        // the routine (add_ship) and the routine
        // (arrive_ship, handed the possibly-null arrival object).
        DestSys->add_ship(Obj, &Parms, Pos, Orient);
        DestSys->arrive_ship(Obj, Arrival, Pos, Orient);

        // the slot's system is the destination.
        p->system_id = DestSystemId;

        // controller event 0x3b, 7 dwords: {dest
        // system, old system, sid, arrival object id, Pos}. baseline calls the
        // arrival object's get_id (vt +0x20) with no null test -- after the
        // "Couldn't find arrival gate" FDUMP it faults here, and so does the port.
        unsigned int Msg3b[7];
        Msg3b[0] = DestSys->SystemInfo->NicknameId;
        Msg3b[2] = session_id;
        Msg3b[1] = Sys->SystemInfo->NicknameId;
        Msg3b[3] = Arrival->get_id();
        std::memcpy(&Msg3b[4], &Pos.x, sizeof(float));
        std::memcpy(&Msg3b[5], &Pos.y, sizeof(float));
        std::memcpy(&Msg3b[6], &Pos.z, sizeof(float));
        Server_ControllerDispatch(0, pub::Controller::EMessage::SystemEnter, Msg3b);

        // the routine on the slot: the autosave.
        PlayerData_AutoSave(*p);
    }

    // PlayerLaunch — baseline 0xfb20: complex launch sequence (~160 lines inspected)
void IServerImpl::PlayerLaunch(unsigned int ship_id, unsigned int session_id) {
        unsigned int max_players = PlayerDB_GetMaxPlayers();
        if (session_id == 0 || (session_id - 1) >= max_players) return;
        // client_id (+0x348) != 0 -- PlayerDB_GetPlayer's null. No
        // `active` test in baseline (removed, w4-server-guards).
        PlayerSlot* p = GetPlayerBySession(session_id);
        if (!p) return;
        // baseline validation: not in ship ([-0x34] & 0x3fffffff == 0, module storage),
        // a ship reserved by RequestCreateShip ([slot+0x3e8] & 0x3fffffff != 0,
        // ) and param_1 == that same +0x3E8 (module storage), then
        // the system object via the routine(player[-0x38]). The port used to test
        // its `base_id` mirror (+0x3F4's twin) and compare against ship_object_id,
        // which the preceding test had just required to be zero.
        if ((p->ship_object_id & 0x3fffffff) != 0) return; // Already in ship
        if ((p->created_ship_id & 0x3fffffff) == 0) return; // No ship reserved
        if (ship_id != p->created_ship_id) return; // Ship ID mismatch

        // the routine on the slot's system (+0x3e0);
        // no system, no launch. Kept at [ESP+0x34].
        StarSystem* const Sys = SysDB_LookupSystem(p->system_id);
        if (Sys == nullptr) return;

        // a thiscall on the slot to module storage,
        // which is a bare RET (the ICF fold analysis labels
        // pub::AI::ContentCallback::combat_drift_event). Nothing to run.

        // the routine: the session joins the system.
        Sys->add_player(session_id);

        // controller event 6 {sid, [slot-0x8]}:
        // +0x410, exited_base_id (the base the session just left).
        unsigned int msg6[2] = { session_id, p->exited_base_id };
        Server_ControllerDispatch(0, pub::Controller::EMessage::PlayerLaunch, msg6);

        // the request at [ESP+0x88]: the routine (the
        // CShipCreateInfo constructor), the EquipIdMaker at +0xe0 (import
        // ), +0xe4 = 0, +0xe8 = 0.
        FShipCreateRequest Req;
        Req.EquipStateData = nullptr;
        Req.MarkPlayerId = 0;

        // the routine: `MOV EAX,[ECX+0x264]`.
        Req.ArchetypeNickname = p->ship_archetype;
        // PlayerDB::GetGroupID(sid) -> +0xd8.
        Req.Field_0xd8 = Players.GetGroupID(session_id);

        // Archetype::GetShip, then its load (vt +0xc).
        Archetype::Ship* const ShipArch = Archetype::GetShip(Req.ArchetypeNickname);
        ShipArch->load();

        // HardpointPositionA(Root+0x40 engine
        // index, "HpMount", &HpPos at [ESP+0x3c], NULL); the result is not
        // tested. baseline leaves HpPos as the stack held it when the call fails;
        // the port starts it at zero.
        Vector HpPos;
        HpPos.x = 0.0f;
        HpPos.y = 0.0f;
        HpPos.z = 0.0f;
        HardpointPositionA(ShipArch->EngineIndex, "HpMount", &HpPos, nullptr);

        // the routine with ObjId = 0 and Kind = -1
        // preset; position and orientation land in the request (+0x4c / +0x58).
        unsigned int LaunchObjId = 0;
        int LaunchKind = -1;
        Server_GetLaunchPosition(session_id, &LaunchObjId, &LaunchKind, &Req.Position,
                                 &Req.Orientation);

        // the file's math engine (the field):
        // QuaternionToMatrix (vt +0x54) into [ESP+0x48], then TransformMatrix
        // (vt +0x2c) of HpPos by it into [ESP+0x14].
        if (GIServerSMathEngine == nullptr)
            GIServerSMathEngine = Server_Acquire3DMathEngine();
        Matrix LaunchOrient;
        GIServerSMathEngine->QuaternionToMatrix(reinterpret_cast<FMatrix&>(LaunchOrient),
                                                reinterpret_cast<FQuaternion const&>(Req.Orientation));
        if (GIServerSMathEngine == nullptr)
            GIServerSMathEngine = Server_Acquire3DMathEngine();
        Vector HpOffset;
        GIServerSMathEngine->TransformMatrix(reinterpret_cast<FVector&>(HpOffset),
                                             reinterpret_cast<FMatrix const&>(LaunchOrient),
                                             reinterpret_cast<FVector const&>(HpPos));

        // the ship's origin sits HpMount behind the
        // launch point: FLD / FSUB / FSTP float per component.
        Req.Position.x = Req.Position.x - HpOffset.x;
        Req.Position.y = Req.Position.y - HpOffset.y;
        Req.Position.z = Req.Position.z - HpOffset.z;

        // IClient +0xf0 (SendServerLaunch) FLPACKET_SERVER_LAUNCH
        // with the record at [ESP+0x48].
        FServerLaunchInfo Launch;
        Launch.Field_0x00 = ship_id;
        Launch.Field_0x04 = LaunchObjId;
        Launch.Field_0x08 = static_cast<unsigned int>(LaunchKind);
        Launch.Orientation = Req.Orientation;
        Launch.Position = Req.Position;
        g_pClientInterface->SendServerLaunch(session_id, Launch);

        // outside single player: IClient +0x1c4
        // (SendServerMiscObjUpdateObjectGroup)(sid, ship, group id), then
        // GetAffiliation(copy of [slot+0x318], out = -1, false) and IClient +0x1d0
        // (SendServerMiscObjUpdateAffiliation)(sid, ship, affiliation).
        if (!SinglePlayer()) {
            g_pClientInterface->SendServerMiscObjUpdateObjectGroup(session_id, ship_id, Req.Field_0xd8);
            unsigned int Affiliation = 0xffffffffu;
            int AffiliationVibe = p->rep_vibe;
            ::Reputation::Vibe::GetAffiliation(AffiliationVibe, Affiliation, false);
            g_pClientInterface->SendServerMiscObjUpdateAffiliation(session_id, ship_id, Affiliation);
        }

        // velocity and angular velocity zero (the
        // request already holds zeros; baseline stores them again), ObjId, owner.
        Req.Velocity.x = 0.0f;
        Req.Velocity.y = 0.0f;
        Req.Velocity.z = 0.0f;
        Req.ObjId = ship_id;                                               // +0x04
        Req.OwnerPlayer = session_id;                                      // +0xac
        Req.AngularVelocity.x = 0.0f;
        Req.AngularVelocity.y = 0.0f;
        Req.AngularVelocity.z = 0.0f;

        // vibe (+0x318) and the com_ costume
        // (+0x28c): the four parts, the count, then `count` accessory dwords
        // (signed JLE / JL, no clamp).
        Req.VibeGroupId = p->rep_vibe;                                     // +0x10
        Req.CostumeHead = p->com_costume.PartIds[0];                       // +0x14
        Req.CostumeBody = p->com_costume.PartIds[1];                       // +0x18
        Req.CostumeLeftHand = p->com_costume.PartIds[2];                   // +0x1c
        Req.CostumeRightHand = p->com_costume.PartIds[3];                  // +0x20
        Req.CostumeAccessoryCount = static_cast<int>(p->com_costume.AccessoryCount);   // +0x44
        for (int Index = 0; Index < Req.CostumeAccessoryCount; ++Index)
            Req.CostumeAccessory[Index] = p->com_costume.AccessoryIds[Index];

        // CreateID(voice, +0x2c4) -> +0x48.
        Req.VoiceId = CreateID(p->voice);
        // the routine (FLD [slot+0x268]) FMUL the
        // archetype's hit_pts (+0x1c), _ftol -> +0x68.
        Req.HitPts = static_cast<int>(static_cast<double>(p->relative_health) *
                                      static_cast<double>(ShipArch->HitPts));

        // a temporary EquipDescVector built from the
        // slot's EquipDescList (+0x278, the routine), assigned to +0x70, then
        // destroyed.
        Req.Equipment = EquipDescVector(p->equipment);
        // the slot's collision groups (+0x26c,
        // ) assigned to +0x80 (skipped for self-assignment).
        Req.CollisionGroups = p->collision_groups;

        // GetRank(+0x10, &Rank). baseline's Rank
        // ([ESP+0x38]) is not initialised before the call; the port starts it at 0.
        unsigned int Rank = 0;
        ::Reputation::Vibe::GetRank(Req.VibeGroupId, Rank);
        Req.Throttle = 0.2f;                                               // = 0x3e4ccccd
        Req.ReputationRank = Rank;                                         // +0xb4
        Req.MarkPlayerId = 0;                                              // +0xe8 (module storage)

        // the routine.
        ::Ship* const NewShip = Sys->create_ship(Req);
        // Archetype::Root::Release.
        ShipArch->Release();

        // launched from an object: the routine on
        // the new wrapper (not null-tested).
        if (LaunchObjId != 0)
            NewShip->apply_launch_invincibility();

        // single player: the orientation matrix again
        // (vt +0x54, into [ESP+0x48]) and CObject::beam_object (vt +0x7c) of the
        // wrapper's raw object under the (& 1) test -- called whatever that test
        // yields, as in baseline.
        if (SinglePlayer()) {
            if (GIServerSMathEngine == nullptr)
                GIServerSMathEngine = Server_Acquire3DMathEngine();
            GIServerSMathEngine->QuaternionToMatrix(reinterpret_cast<FMatrix&>(LaunchOrient),
                                                    reinterpret_cast<FQuaternion const&>(Req.Orientation));
            CObject* Raw = nullptr;
            if (NewShip != nullptr) {
                CObject* const Obj = NewShip->Object;
                if (Obj != nullptr && (static_cast<unsigned int>(Obj->ObjectClass) & 1u) == 1u)
                    Raw = Obj;
            }
            Raw->beam_object(Req.Position, LaunchOrient, false);
        }

        // the slot's ship (+0x3e4) = the request's ObjId.
        p->ship_object_id = Req.ObjId;

        // controller event 0x36 {system (the routine),
        // sid, launch object, launch_pos index}.
        unsigned int msg36[4];
        msg36[0] = Sys->SystemInfo->NicknameId;
        msg36[1] = session_id;
        msg36[2] = LaunchObjId;
        msg36[3] = static_cast<unsigned int>(LaunchKind);
        Server_ControllerDispatch(0, pub::Controller::EMessage::SpaceEnter, msg36);

        // the request's destructor, inlined (base
        // vftable restamp, the +0x8c and +0x80 lists, the +0x70 vector): Req's
        // own destructor here.
    }

void IServerImpl::LaunchComplete(unsigned int arg0, unsigned int object_id) {
        // (RET 8). rpclocal serializes BOTH
        // arguments (ch3 op 0x3e), so neither is an implicit session id; arg0 is
        // passed through untouched (Ship::slot_70 sends it as the first dword of
        // controller event 0x28). No pub::Controller message of its own -- the old
        // id 0x47 was invented.
        // the routine(&object_id, &obj, &sys)
        // / 06cefac4   AL == 0 or obj == 0 -> out
        // (obj + 4)->vtable[+0x70](arg0) -- IObjAffect slot 28
        GameObject* Obj = nullptr;
        StarSystem* Sys = nullptr;
        if (!Server_FindObject(object_id, Obj, Sys))
            return;
        if (Obj == nullptr)
            return;
        static_cast<IObjAffect*>(Obj)->slot_70(arg0);
    }

void IServerImpl::JumpInComplete(unsigned int arg0, unsigned int object_id) {
        // (RET 8), the same shape as
        // LaunchComplete with IObjAffect slot 29 (+0x74, Ship::slot_74 sends arg0
        // in controller event 0x26). Both arguments are serialized (ch3 op 0x43).
        // The old message id 0x48 was invented.
        // the routine(&object_id, &obj, &sys)
        // / 06cefb04   AL == 0 or obj == 0 -> out
        // (obj + 4)->vtable[+0x74](arg0)
        GameObject* Obj = nullptr;
        StarSystem* Sys = nullptr;
        if (!Server_FindObject(object_id, Obj, Sys))
            return;
        if (Obj == nullptr)
            return;
        static_cast<IObjAffect*>(Obj)->slot_74(arg0);
    }

    // Hail -- baseline module storage .. 06cf0848, RET 0xc. Ported 2026-09-24
    // (claude-code:w5-server-06cf):
    // the routine(&object_id, &obj, &sys); AL == 0 -> out. Only the
    //             found flag is tested -- the object and system are discarded.
    // payload {object_id, arg1, arg2} on the stack
    // the routine(0x1f, &payload)
    // is Server_LookupGameObject in this port; its null is AL == 0.
    // All three arguments are serialized by rpclocal (ch3 op 0x3f) -- no session id.
void IServerImpl::Hail(unsigned int object_id, unsigned int arg1, unsigned int arg2) {
        if (Server_LookupGameObject(object_id) == nullptr)
            return;
        unsigned int msg_data[3] = { object_id, arg1, arg2 };
        Server_ControllerDispatch(0, pub::Controller::EMessage::Hail, msg_data);
    }
    // SPObjUpdate -- baseline module storage .. module storage (RET 8).
    // Ported 1:1 2026-09-24 (claude-code:w5-server-06cf):
    // SinglePlayer() -> return
    // the routine(&info.ObjectId, &obj, &sys) (the sys out is the
    //                         param_1 stack slot); false -> return
    // sys == 0 -> return
    // PlayerDB::is_valid inlined (sid != 0, sid-1 < max,
    //                         client_id +0x348 != 0); failing it jumps to 06cf06a1, i.e.
    //                         skips ONLY the bound check below
    // the routine (PlayerData_CheckPositionBound) on the slot with
    //                         (&info.Position, info+0x20, info+0x24); false -> return
    // sys->the routine(info, session_id) (StarSystem::sp_obj_update)
void IServerImpl::SPObjUpdate(const SSPObjUpdateInfo& info, unsigned int session_id) {
        if (SinglePlayer()) return;
        GameObject* Obj = nullptr;
        StarSystem* System = nullptr;
        if (!Server_FindObject(info.ObjectId, Obj, System)) return;
        if (System == nullptr) return;
        if (Players.is_valid(session_id)) {
            // reads only its first stack argument; the port's helper takes
            // just that one (common_server_playerdb.h).
            if (!PlayerData_CheckPositionBound(Players[session_id], &info.Position))
                return;
        }
        System->sp_obj_update(info, session_id);
    }

    // SPMunitionCollision -- baseline module storage .. 06cf0744, RET 8. Ported
    // 2026-09-24 (claude-code:w5-server-06cf):
    // PlayerDB::is_valid inlined: sid != 0, sid-1 < max,
    //                         client_id (+0x348) != 0 -- else out
    // [slot-0x34] & 0x3fffffff == 0 (not in a ship) -> out
    // the routine(slot) = FLD [slot+0x268], the relative
    //                         health; FCOMP double [06d65190] (0.0001), TEST AH,0x41
    //                         / JNP out -- i.e. out when health < or == 0.0001
    //                         (exactly one of C0/C3 set); unordered continues
    // the routine(&info.ObjectId [info+8], &obj, &sys); AL == 0 -> out
    // sys == 0 -> out
    // sys->the routine(info) = StarSystem::munition_collision
void IServerImpl::SPMunitionCollision(const SSPMunitionCollisionInfo& info, unsigned int session_id) {
        unsigned int max_players = PlayerDB_GetMaxPlayers();
        if (session_id == 0 || (session_id - 1) >= max_players) return;
        PlayerSlot* p = GetPlayerBySession(session_id);
        if (!p) return;
        if ((p->ship_object_id & 0x3fffffff) == 0) return;
        double const Health = static_cast<double>(p->relative_health);
        if (Health < 0.0001 || Health == 0.0001) return;
        GameObject* Obj = nullptr;
        StarSystem* Sys = nullptr;
        if (!Server_FindObject(info.ObjectId, Obj, Sys))
            return;
        if (Sys == nullptr)
            return;
        Sys->munition_collision(info);
    }

    // SPBadLandsObjCollision -- baseline module storage .. 06cf07c0, RET 8. Ported
    // 2026-09-24 (claude-code:w5-server-06cf). No session test at all:
    // the routine(&info.ObjectId [info+4], &obj, &sys); AL == 0 -> out
    // sys == 0 -> out
    // sys->the routine(info) = StarSystem::badlands_collision
    // session_id is not read.
void IServerImpl::SPBadLandsObjCollision(const SSPBadLandsObjCollisionInfo& info, unsigned int session_id) {
        (void)session_id;
        GameObject* Obj = nullptr;
        StarSystem* Sys = nullptr;
        if (!Server_FindObject(info.ObjectId, Obj, Sys))
            return;
        if (Sys == nullptr)
            return;
        Sys->badlands_collision(info);
    }

    // SPObjCollision -- baseline module storage .. 06cf0785, RET 8. Ported
    // 2026-09-24 (claude-code:w5-server-06cf). No session test at all:
    // the routine(&info.ObjectId [info+8], &obj, &sys); AL == 0 -> out
    // sys == 0 -> out
    // sys->the routine(info, obj) = StarSystem::collision_damage
    // baseline hands the 0x14-byte record straight on: SSPObjCollisionInfo IS the
    // FCollisionDamage record GameObject::on_collision builds (+0 source id,
    // +4 source sub-object, +8 target id -- the object looked up here -- +0xc the
    // target's sub-object, +0x10 the damage). The static_asserts below pin the
    // two declarations to the same offsets; the pointer is passed unchanged, as
    // baseline passes ESI. session_id is not read.
void IServerImpl::SPObjCollision(const SSPObjCollisionInfo& info, unsigned int session_id) {
        static_assert(sizeof(SSPObjCollisionInfo) == sizeof(FCollisionDamage),
                      "SSPObjCollisionInfo is the FCollisionDamage record");
        static_assert(offsetof(SSPObjCollisionInfo, ObjectId) == offsetof(FCollisionDamage, TargetId),
                      "+8 is the target module state resolves");
        static_assert(offsetof(SSPObjCollisionInfo, SubObjId) == offsetof(FCollisionDamage, TargetSubObjId),
                      "+0xc is the target's sub-object");
        static_assert(offsetof(SSPObjCollisionInfo, Field_0x10) == offsetof(FCollisionDamage, Damage),
                      "+0x10 is the damage");
        (void)session_id;
        GameObject* Obj = nullptr;
        StarSystem* Sys = nullptr;
        if (!Server_FindObject(info.ObjectId, Obj, Sys))
            return;
        if (Sys == nullptr)
            return;
        Sys->collision_damage(reinterpret_cast<FCollisionDamage const&>(info), Obj);
    }

    // SPRequestUseItem -- baseline module storage .. 06cf137c, RET 8. Ported
    // 2026-09-24 (claude-code:w5-server-06cf). No session test at all:
    // the routine(&info.ObjectId, &obj, &sys); AL == 0 -> out
    // obj == 0 -> out
    // obj->vt[+0x160] (is_destroyed) true -> out
    // IObjAffect +0x58 (word info+4 ItemId, MOVZX word info+6 Count,
    //             session_id); a non-zero result -> out (06cf1360)
    // the routine on Players' raw slot (terminate_trade)
void IServerImpl::SPRequestUseItem(const SSPUseItem& info, unsigned int session_id) {
        GameObject* const Obj = Server_LookupGameObject(info.ObjectId);
        if (Obj == nullptr)
            return;
        if (Obj->is_destroyed())
            return;
        if (static_cast<IObjAffect*>(Obj)->slot_58(info.ItemId, info.Count,
                                                   static_cast<int>(session_id)) != 0)
            return;
        PlayerData_TerminateTrade(Players[session_id]);
    }
    // SPRequestInvincibility -- baseline module storage .. 06cf153c, RET 0x10, with an
    // SEH frame for the local list. Ported 2026-09-24 (claude-code:w5-server-06cf):
    // PlayerDB::is_valid inlined -> out
    // RAW compare [slot-0x34] == ship_id (no mask) -> else out
    // the FInvincibility record on the stack: +0 reason,
    //             +4 and +5 both b_invincible, +8 below
    // !b_invincible: +8 = 1.0f (06cf142d), straight to the apply
    //   else +8 = 0.0f (06cf1407), and by reason:
    //     2  (06cf143a): sys = the routine(slot system id [slot-0x38]); null
    //        -> out. A local std::list; sys->the routine(&list, 0) collects the
    //        dockable solars (06cf1484); the first whose the routine(ship_id)
    //        (the ship is in its dock queue) answers true sets the flag
    //        (06cf14b0); the list is freed (the routine per node, 06cf14d1,
    //        then the head); no hit -> out (06cf14ea)
    //     5  (06cf1414): straight to the apply
    //     any other: return (06cf141a)
    // the routine(&ship_id, &obj, &sys); AL == 0 / obj == 0 /
    //             is_destroyed -> out
    // obj->vt[+0x194] set_invincible(record)
    // The record's two pad bytes (+6/+7) are never written by baseline; the port
    // zeroes them (deviations.md, zeroed indeterminate stack bytes).
void IServerImpl::SPRequestInvincibility(unsigned int ship_id, unsigned char b_invincible,
                                InvincibilityReason reason, unsigned int session_id) {
        PlayerSlot* p = GetPlayerBySession(session_id);
        if (!p) return;
        if (p->ship_object_id != ship_id) return;

        FInvincibility Cmd = {};
        Cmd.Type = static_cast<unsigned int>(reason);
        Cmd.bFromPlayers = b_invincible;
        Cmd.bProtect = b_invincible;
        if (!b_invincible)
        {
            Cmd.MaxHpLoss = 1.0f;
        }
        else
        {
            Cmd.MaxHpLoss = 0.0f;
            int const Reason = static_cast<int>(reason);
            if (Reason == 2)
            {
                StarSystem* const System = SysDB_LookupSystem(p->system_id);
                if (System == nullptr)
                    return;
                bool bQueued = false;
                std::list<GameObject*> Solars;
                System->collect_dockable_solars(Solars, 0);
                for (std::list<GameObject*>::const_iterator It = Solars.begin(); It != Solars.end(); ++It)
                {
                    if (static_cast<EqObj*>(*It)->is_ship_queued(ship_id))
                    {
                        bQueued = true;
                        break;
                    }
                }
                if (!bQueued)
                    return;
            }
            else if (Reason != 5)
            {
                return;
            }
        }

        GameObject* const Obj = Server_LookupGameObject(ship_id);
        if (Obj == nullptr)
            return;
        if (Obj->is_destroyed())
            return;
        Obj->set_invincible(Cmd);
    }
void IServerImpl::SaveGame(const CHARACTER_ID& char_id, const wchar_t* description,
                  unsigned int session_id) {
        // baseline 0x659d0 (module storage .. module storage): SP only, a live
        // session, and NOT in a ship ([slot-0x34] & 0x3fffffff == 0 at
        // there is no in-base exception), then a tail JMP to
        // (slot, char_id, description, 0) with the third stack
        // slot overwritten to 0 (module storage).
        // REWIRED 2026-09-24 (claude:srv-save): this used to call the
        // serializer directly and accept a ship-borne save while docked.
        // is PlayerDB::is_valid inlined -- `!p`; there
        // is no `active` test (removed, w4-server-guards).
        if (!SinglePlayer()) return;
        PlayerSlot* p = GetPlayerBySession(session_id);
        if (!p) return;
        if ((p->ship_object_id & 0x3fffffff) != 0) return;
        PlayerData_SaveGame(*p, char_id.Buffer, description, 0);
    }
    // MissionSaveB -- baseline 0x65a30 (module storage .. module storage): SP
    // only, a live session, then the routine(slot, mission_id) -- the mission
    // id IS the save's descrip_strid.
void IServerImpl::MissionSaveB(unsigned int session_id, unsigned long mission_id) {
        if (!SinglePlayer()) return;
        unsigned int max_players = PlayerDB_GetMaxPlayers();
        if (session_id == 0 || (session_id - 1) >= max_players) return;
        // client_id (+0x348) != 0 -- PlayerDB_GetPlayer's null. No
        // `active` test in baseline (removed, w4-server-guards).
        PlayerSlot* p = GetPlayerBySession(session_id);
        if (!p) return;
        PlayerData_MissionSave(*p, static_cast<unsigned int>(mission_id));
    }
    // RequestEvent -- baseline module storage .. 06cf0927, RET 0x18 (six stack
    // arguments). Ported 2026-09-24 (claude-code:w5-server-06cf):
    // the routine(&p1, &obj, &sys); AL == 0 -> out
    // obj == 0 -> out
    // IObjAffect +0x5c (event_type, &p2, &p3, p4, PUSH 0 -- no
    //             RequestCB)
    // p5 is never read. No controller message: the port's id 0x21 was invented.
void IServerImpl::RequestEvent(int event_type, unsigned int p1, unsigned int p2, unsigned int p3, unsigned long p4, unsigned int p5) {
        (void)p5;
        GameObject* const Obj = Server_LookupGameObject(p1);
        if (Obj == nullptr)
            return;
        static_cast<IObjAffect*>(Obj)->slot_5c(event_type, &p2, &p3, static_cast<int>(p4),
                                               nullptr);
    }
    // RequestCancel -- baseline module storage .. 06cf0970, RET 0x14 (five stack
    // arguments). Ported 2026-09-24 (claude-code:w5-server-06cf):
    // the routine(&p1, &obj, &sys); AL == 0 -> out
    // obj == 0 -> out
    // IObjAffect +0x60 (event_type, &p2, p3)
    // p4 is never read. No controller message: the port's id 0x22 was invented.
void IServerImpl::RequestCancel(int event_type, unsigned int p1, unsigned int p2, unsigned long p3, unsigned int p4) {
        (void)p4;
        GameObject* const Obj = Server_LookupGameObject(p1);
        if (Obj == nullptr)
            return;
        static_cast<IObjAffect*>(Obj)->slot_60(event_type, &p2, static_cast<int>(p3));
    }
    // MineAsteroid -- baseline module storage .. 06cf07f8, RET 0x18. Ported
    // 2026-09-24 (claude-code:w5-server-06d0):
    // the routine(system_id), the SysDB lookup; the
    //                         result is not null-tested
    // the routine (StarSystem::spawn_mined_loot) with
    //                         (&position, arg3, arg4, arg5); the sixth argument
    //                         is never read. No controller message: the port's
    //                         id 0x23 was invented.
void IServerImpl::MineAsteroid(unsigned int system_id, const Vector& position, unsigned int loot_id,
                      unsigned int secondary_loot, unsigned int quantity, unsigned int session_id) {
        (void)session_id;
        StarSystem* const System = SysDB_LookupSystem(system_id);
        System->spawn_mined_loot(position, loot_id, secondary_loot, static_cast<int>(quantity));
    }
    // CommComplete -- baseline module storage .. 06cf08d1, RET 0x10. Ported
    // 2026-09-24 (claude-code:w5-server-06cf):
    // the routine(&arg1, &obj, &sys); AL != 0 and sys != 0 -> 06cf0899
    // otherwise the routine(&arg0, &obj, &sys); AL == 0 or sys == 0
    //             skips the conversion (06cf088f / 06cf0897)
    // the routine(sys) -- the system's nickname id -- into payload+4
    // payload {arg1, system id, arg0, arg2, result} (five dwords)
    // the routine(0xb, &payload)
    // When neither id resolves, baseline never writes payload+4 and sends whatever
    // the stack held; the port sends 0 there (deviations.md, zeroed indeterminate
    // stack bytes). the routine = Server_FindObject.
void IServerImpl::CommComplete(unsigned int arg0, unsigned int arg1, unsigned int arg2, CommResult result) {
        GameObject* Obj = nullptr;
        StarSystem* Sys = nullptr;
        unsigned int SystemRef = 0;
        if (Server_FindObject(arg1, Obj, Sys) && Sys != nullptr)
            SystemRef = Sys->SystemInfo->NicknameId;
        else if (Server_FindObject(arg0, Obj, Sys) && Sys != nullptr)
            SystemRef = Sys->SystemInfo->NicknameId;
        unsigned int msg_data[5] = { arg1, SystemRef, arg0, arg2, (unsigned int)result };
        Server_ControllerDispatch(0, pub::Controller::EMessage::CommComplete, msg_data);
    }
    // RequestCreateShip — baseline 0xf9e0: validates player, gets/creates FLID,
    // then IClient +0xf4 (SendServerRequestCreateShipResp) directly — not a
    // controller message. Listing: .text:06cfa3xx `CALL [the field+0xf4]`.

    // CORRECTED 2026-09-24 (claude-code:w3-baseenter): the field is [slot-0x30] =
    // +0x3E8 (created_ship_id), read at module storage and written at
    // NOT ship_object_id (+0x3E4 = -0x34), which the port used
    // to read and write here. PlayerLaunch checks its argument against the same
    // +0x3E8 (module storage). The `g_pClientInterface != nullptr` tests are gone
    // too: module storage / module storage load the field and call through it.
    // baseline's null-CFLIDMaker path stores `sid & 0xffffff00` (the argument slot
    // with its low byte cleared, module storage/06cefa6d) as both flid and flag;
    // for every session id below 0x100 that is the 0 / false the port passes.
void IServerImpl::RequestCreateShip(unsigned int session_id) {
        if (!Players.is_valid(session_id)) return;
        PlayerSlot* p = &Players[session_id];
        if ((p->created_ship_id & 0x3fffffff) != 0) {
            // SendServerRequestCreateShipResp(session, true, existing)
            g_pClientInterface->SendServerRequestCreateShipResp(session_id, true, p->created_ship_id);
            return;
        }
        // baseline 0xfa3d: CFLIDMaker::GetInstance(), then post-increment the dynamic-id
        // counter at maker+4 and mask the pre-increment value to 30 bits. A null maker
        // yields flid 0 with the success flag cleared.
        unsigned int flid = 0;
        unsigned int flag = 0;
        CFLIDMaker* Maker = CFLIDMaker::GetInstance();
        if (Maker != nullptr) {
            flid = Maker->AllocateObjectID();
            flag = (flid != 0) ? 1u : 0u;
        }
        p->created_ship_id = flid;   // [slot-0x30]
        // CALL [EAX+0xf4](session, flag, flid)
        g_pClientInterface->SendServerRequestCreateShipResp(session_id, flag != 0, flid);
    }
    // SPScanCargo — baseline 0x112e0: sends message 3
void IServerImpl::SPScanCargo(const unsigned int& scanner_id, const unsigned int& target_id,
                     unsigned int session_id) {
        // has no branch and never reads the session:
        // ECX = *scanner_id -> [ESP]    (local_8)
        // EAX = *target_id  -> [ESP+4]  (local_4, stored at 06cf12fa)
        // the routine(3, &local_8)
        // i.e. a TWO-dword payload. The port's `!p || !p->active` early-out and the
        // trailing session dword were additions and are gone (w4-server-guards).
        (void)session_id;
        // baseline 0x112e0 sends message 3; rpclocal serializes both ids (ch3 op 0x2b).
        unsigned int msg_data[2] = { scanner_id, target_id };
        Server_ControllerDispatch(0, pub::Controller::EMessage::CargoScanned, msg_data);
    }
    // SetManeuver -- baseline module storage .. module storage (RET 8).
    // Ported 1:1 2026-09-24 (claude-code:w5-server-06cf):
    // the routine(&info.ObjectId, &obj, &sys) (the obj out is
    //                         the param_2 stack slot); false -> return
    // obj == 0 -> return; its CBase::Object (+0x10) == 0 -> return
    // (ObjectClass & 0x103) != 0x103 -> return
    // CEqObj::get_behavior_interface(); non-null -> its slot 0
    //                         (Common the branch) with the event {2, &info}
    // SinglePlayer() -> the routine(0x2f, {session_id,
    //                         info.ObjectId (06cf15be), info.Field_0x08 (06cf15c9)})
    // Every early out skips the single-player frame as well.
void IServerImpl::SetManeuver(unsigned int session_id, const XSetManeuver& info) {
        GameObject* Obj = nullptr;
        StarSystem* System = nullptr;
        if (!Server_FindObject(info.ObjectId, Obj, System)) return;
        if (Obj == nullptr) return;
        CObject* const Raw = Obj->Object;
        if (Raw == nullptr) return;
        if ((static_cast<unsigned int>(Raw->ObjectClass) & 0x103u) != 0x103u) return;
        IBehaviorManager* const Manager =
            static_cast<CEqObj*>(Raw)->get_behavior_interface();
        if (Manager != nullptr) {
            FBehaviorManagerEvent Event;
            Event.Type = 2;
            Event.Data = const_cast<XSetManeuver*>(&info);
            Manager->Slot00(&Event);
        }
        if (SinglePlayer()) {
            unsigned int msg_data[3];
            msg_data[0] = session_id;
            msg_data[1] = info.ObjectId;
            msg_data[2] = info.Field_0x08;
            Server_ControllerDispatch(0, pub::Controller::EMessage::PlayerManeuver, msg_data);
        }
    }
    // InterfaceItemUsed — baseline 0x657b0: sends message 0x21
void IServerImpl::InterfaceItemUsed(unsigned int session_id, unsigned int item_id) {
        // has no branch: {param_1, param_2} ->
        // (0x21, &frame). The port's `!p || !p->active` early-out was
        // an addition and is gone (w4-server-guards).
        unsigned int msg_data[2] = { session_id, item_id };
        Server_ControllerDispatch(0, pub::Controller::EMessage::InterfaceItemUsed, msg_data);
    }
void IServerImpl::AbortMission(unsigned int session_id, unsigned int mission_id) {
        // baseline 0x65810: send message 0x24, increment abort counter
        unsigned int msg_data[2] = { session_id, mission_id };
        Server_ControllerDispatch(0, pub::Controller::EMessage::AbortMission, msg_data);
        // INC [module storage] == GServerStats + 0x1c.
        GServerStats.AbortMissionCount++;
    }
    // RTCDone — baseline: sends message 0x22 via pub::Controller::_SendMessage
void IServerImpl::RTCDone(unsigned int session_id, unsigned int rtc_id) {
        unsigned int msg_data[2] = { session_id, rtc_id };
        Server_ControllerDispatch(0, pub::Controller::EMessage::RtcDone, msg_data);
    }
    // SetWeaponGroup — baseline 0x10e40: validates player connected, calls the routine(player, data)
    // copies null-terminated string into player+0x380 (weapon group name)
void IServerImpl::SetWeaponGroup(unsigned int session_id, unsigned char* data, int size) {
        unsigned int max_players = PlayerDB_GetMaxPlayers();
        if (session_id == 0 || (session_id - 1) >= max_players) return;
        // client_id (+0x348) != 0 -- PlayerDB_GetPlayer's null. No
        // `active` test in baseline (removed, w4-server-guards).
        PlayerSlot* p = GetPlayerBySession(session_id);
        if (!p) return;
        // (0x06d504f0..0x06d5059a) is the VC6 inline
        // basic_string::assign(const char*) on player+0x380: strlen of the whole
        // NUL-terminated buffer (06d50505 REPNE SCASB), no bound from `size` and no
        // null check on `data` -- the caller at 06cf0e40 has neither guard either.
        (void)size;
        p->weapon_group_name = reinterpret_cast<const char*>(data);
    }
    // SetVisitedState — baseline 0x10e90: iterates buffer entries, calls the routine per entry
    // finds or inserts {system_id, visit_flags} in player+0x3A0 sorted list,
    //               ORs new flags with existing if entry already present
    // .. module storage (RET 0xc), re-checked 2026-09-24
    // (claude-code:w3-baseenter): PlayerDB::is_valid inlined (06cf0e94..06cf0ebc),
    // then count = *(int*)data (06cf0ec2, signed JLE at 06cf0ec9) and the 5-byte
    // record loop into the routine. There is no `data` null test, no data_len
    // test (param_3 is never read) and no port `active` test -- all three were
    // port additions and are gone.
void IServerImpl::SetVisitedState(unsigned int session_id, unsigned char* data, int data_len) {
        (void)data_len;
        if (!Players.is_valid(session_id)) return;
        PlayerSlot* p = &Players[session_id];

        int count = *(int*)data;
        unsigned char* ptr = data + 4;
        for (int i = 0; i < count; i++) {
            unsigned int system_id = *(unsigned int*)ptr;
            unsigned char visit_flags = ptr[4];
            ptr += 5;
            // (0x06d508a0): lower_bound on the player's visited
            // MAP at +0x3a0, OR the incoming flags into an existing entry, otherwise
            // insert a fresh {system_id, flags} node at the hint position.

            // That is PlayerData::visit_map, NOT the pending queue at +0x3b8 this
            // used to write. The two are different containers (see the field note
            // in common_server_playerdb.h): +0x3b8 is appended by the routine and
            // drained into the map by the routine when the player leaves a system.
            std::map<unsigned int, unsigned char>::iterator Existing =
                p->visit_map.find(system_id);
            if (Existing != p->visit_map.end()) {
                Existing->second = (unsigned char)(Existing->second | visit_flags);
            } else {
                p->visit_map[system_id] = visit_flags;
            }
        }
    }
    // RequestBestPath — baseline 0x10f00: packs 9 fields from buffer, sends message 0x31
void IServerImpl::RequestBestPath(unsigned int session_id, unsigned char* data, int data_len) {
        unsigned int max_players = PlayerDB_GetMaxPlayers();
        if (session_id == 0 || (session_id - 1) >= max_players) return;
        // client_id (+0x348) != 0 -- PlayerDB_GetPlayer's null.
        // baseline has no `active` test and no test on `data` / `data_len` (param_3
        // is never read); both were port additions and are gone (w4-server-guards).
        (void)data_len;
        PlayerSlot* p = GetPlayerBySession(session_id);
        if (!p) return;

        // baseline stack layout (0x10f00), ascending from the message base:
        // session, +0x00, +0x1c, +0x0c, +0x10, +0x14, +0x20, +0x24, +0x28, +0x30.
        // (local_4 holds data+0x30 and sits LAST, at message offset +0x24.)
        unsigned int msg_data[10];
        msg_data[0] = session_id;
        msg_data[1] = *(unsigned int*)(data + 0x00);
        msg_data[2] = *(unsigned int*)(data + 0x1c);
        msg_data[3] = *(unsigned int*)(data + 0x0c);
        msg_data[4] = *(unsigned int*)(data + 0x10);
        msg_data[5] = *(unsigned int*)(data + 0x14);
        msg_data[6] = *(unsigned int*)(data + 0x20);
        msg_data[7] = *(unsigned int*)(data + 0x24);
        msg_data[8] = *(unsigned int*)(data + 0x28);
        msg_data[9] = *(unsigned int*)(data + 0x30);
        Server_ControllerDispatch(0, pub::Controller::EMessage::RequestBestPath, msg_data);
    }
    // RequestPlayerStats — baseline 0x10fa0: validates player, sends message 0x33
void IServerImpl::RequestPlayerStats(unsigned int session_id, unsigned char* data, int data_len) {
        unsigned int max_players = PlayerDB_GetMaxPlayers();
        if (session_id == 0 || (session_id - 1) >= max_players) return;
        // client_id (+0x348) != 0 -- PlayerDB_GetPlayer's null. No
        // `active` test in baseline (removed, w4-server-guards).
        (void)data; (void)data_len;
        PlayerSlot* p = GetPlayerBySession(session_id);
        if (!p) return;
        Server_ControllerDispatch(0, pub::Controller::EMessage::RequestPlayerStats, &session_id);
    }
    // PopUpDialog — baseline 0x10ff0: sends message 0x30
void IServerImpl::PopUpDialog(unsigned int session_id, unsigned int arg1) {
        // baseline 0x10ff0 is unconditional: it just sends {session_id, arg1} as message 0x30.
        unsigned int msg_data[2] = { session_id, arg1 };
        Server_ControllerDispatch(0, pub::Controller::EMessage::PopUpDialog, msg_data);
    }
    // ---------------------------------------------------------------------
    // RequestGroupPositions — baseline 0x11020 (0x06cf1020), ordinal 380
    // Guard: session in range and Players[session][+0x348] (the connection id)
    // non-zero (0x06cf104d). Then group = Players[session][-0x14]; a null group
    // takes the short arm at 0x06cf1194 -- operator new(4), store 0, and send it
    // with size 4.

    // Populated arm:
    //   Count  = group->GetMemberCount()                     (0x06cf1077)
    //   Others = Count > 0 ? Count - 1 : Count                (0x06cf1082/0x06cf1084)
    //   Buf    = operator new(Others * 0x18 + 4)              (0x06cf108a/0x06cf1096)
    //   *(uint*)Buf = Others                                  (0x06cf10a4)
    // then one 0x18-byte entry per member EXCEPT the requester himself
    // (0x06cf10bd CMP against param_1):
    //   +0x00 float Pos[3]
    //   +0x0c uint  BaseId   -- only on the in-base arm (0x06cf115e), from [-0x2c]
    //   +0x10 uint  SystemId -- [-0x38]  (0x06cf10d7)
    //   +0x14 uint  Vibe     -- [-0x100] (0x06cf10e7)
    // When the member has a ship ([-0x34] non-zero) BaseId is zeroed (0x06cf10fc)
    // and the position comes from the routine(SystemId) -> the routine(ship,
    // 0x7f0000) -> obj->vtable[0](); when that lookup fails baseline writes three
    // zeros (0x06cf1132-0x06cf113b). Finally IClient vtable +0x180 == slot 96
    // (FLPACKET_COMMON_REQUEST_GROUP_POSITIONS) with (session, Buf, BufSize)
    // at 0x06cf1184. baseline never frees Buf on either arm -- the RPC layer owns it.

    // The incoming request payload (data/size) is not parsed -- baseline does not
    // read it either.
    // ---------------------------------------------------------------------
void IServerImpl::RequestGroupPositions(unsigned int session_id, unsigned char* data, int size) {
        (void)data; (void)size;
        unsigned int max_players = PlayerDB_GetMaxPlayers();
        if (session_id == 0 || (session_id - 1) >= max_players) return;   // / 06cf1039
        // Players[session]+0x348 (client_id) != 0 -- PlayerDB_GetPlayer's null.
        PlayerSlot* p = GetPlayerBySession(session_id);
        if (!p) return;

        CPlayerGroup* Group = (CPlayerGroup*)p->group_ptr;
        if (!Group) {
            // a bare operator new(4) holding 0. The block is
            // handed over: rpclocal's sender (0x06d8b7a0) sets the packet's
            // owns-buffer flag and the packet destructor frees it.
            unsigned int* Empty = static_cast<unsigned int*>(::operator new(4));
            *Empty = 0;
            g_pClientInterface->SendCommonRequestGroupPositions(
                session_id, reinterpret_cast<unsigned char*>(Empty), 4);
            return;
        }

        unsigned int Count = Group->GetMemberCount();
        // TEST EBP,EBP / JLE -- a signed test.
        unsigned int Others = (static_cast<int>(Count) > 0) ? (Count - 1) : Count;
        unsigned int BufSize = Others * 0x18 + 4;
        // bare operator new, no initialisation -- handed over as above.
        unsigned char* Buf = static_cast<unsigned char*>(::operator new(BufSize));
        *(unsigned int*)Buf = Others;

        unsigned char* Entry = Buf + 4;
        for (int i = 0; i < static_cast<int>(Count); ++i) {             // JL, signed
            unsigned int MemberId = Group->GetMember(i);
            if (MemberId == session_id) continue;                          // no advance
            // baseline indexes Players[member] directly, no logged-in test (06cf10c7).
            PlayerSlot* Member = &Players[MemberId];
            *(unsigned int*)(Entry + 0x10) = Member->system_id;
            *(unsigned int*)(Entry + 0x14) = (unsigned int)Member->rep_vibe;
            if (Member->ship_object_id != 0) {
                *(unsigned int*)(Entry + 0x0c) = 0;
                // the routine(SystemId) (no null test on
                // the result) -> the routine(sys, ship, 0x7f0000).
                StarSystem* const MemberSystem =
                    SysDB_LookupSystem(*(unsigned int*)(Entry + 0x10));
                GameObject* const MemberShip =
                    MemberSystem->find_object(Member->ship_object_id, 0x7f0000u);
                if (MemberShip == nullptr) {
                    // three zero dwords.
                    *(unsigned int*)(Entry + 0x00) = 0;
                    *(unsigned int*)(Entry + 0x04) = 0;
                    *(unsigned int*)(Entry + 0x08) = 0;
                } else {
                    // IObjInspect slot 0 (get_position),
                    // its three dwords copied.
                    Vector const& Pos = MemberShip->get_position();
                    std::memcpy(Entry, &Pos, 0x0c);
                }
            } else {
                // only BaseId is written; Pos stays as operator new left it.
                *(unsigned int*)(Entry + 0x0c) = Member->current_base;
            }
            Entry += 0x18;
        }

        g_pClientInterface->SendCommonRequestGroupPositions(session_id, Buf, BufSize);
    }
    // SetMissionLog — baseline 0x111c0: validates player, calls the routine(player, data, size)
    // case 0 = clear log (free all nodes), case 1 = add entries from buffer
void IServerImpl::SetMissionLog(unsigned int session_id, unsigned char* data, int data_len) {
        unsigned int max_players = PlayerDB_GetMaxPlayers();
        if (session_id == 0 || (session_id - 1) >= max_players) return;
        // client_id (+0x348) != 0 -- PlayerDB_GetPlayer's null. No
        // `active` test in baseline (removed, w4-server-guards).
        PlayerSlot* p = GetPlayerBySession(session_id);
        if (!p) return;

        // is ONLY the player validation plus
        // (player_record, data, data_len)      (0x06cf1205)
        // — no pub::Controller message of any id, and no check on data/data_len.

        // (PlayerData method, RET 0x8) on the std::list<FmtStr> at +0x390
        // (head +0x394, size +0x398):
        //   op = data[0]                                      06d505c0
        //   op 0: erase every node, --size per node           06d505ca .. 06d5062d
        //   op 1: data_len -= 8; count = data[1]; cursor = data + 8   06d5063d .. 06d5064a
        //         while count-- > 0 (signed, JLE at 06d5064e):
        //           FmtStr tmp (inline zeroing of 0x18 bytes) 06d50658 .. 06d50694
        //           ExtractMissionLogData(&tmp, cursor, data_len)      06d506ab
        //           insert a copy BEFORE begin() -- the new node's _Next is
        //           [_Head] (06d506b7), so this is push_front, not push_back

        //   any other op: nothing.
        int* const Ops = reinterpret_cast<int*>(data);
        if (Ops[0] == 0) {
            p->mission_log.clear();
        } else if (Ops[0] == 1) {
            data_len -= 8;
            int Count = Ops[1];
            unsigned char* Cursor = data + 8;
            for (; Count > 0; --Count) {
                FmtStr Entry;
                ExtractMissionLogData(&Entry, Cursor, data_len);
                p->mission_log.push_front(Entry);
            }
        }
    }
    // SetInterfaceState — baseline 0x11210: validates player, calls the routine(player, data)
    // trivially stores *data into player+0x39C (interface state flag)
    // .. module storage (RET 0xc), re-checked 2026-09-24
    // (claude-code:w3-baseenter): PlayerDB::is_valid inlined, then
    // (slot, data, data_len), whose whole body is
    // `MOV EAX,[ESP+4] / MOV EDX,[EAX] / MOV [this+0x39c],EDX / RET 8` -- data_len
    // is pushed but never read, and nothing tests `data`. The port's
    // `!data || data_len < 4` and `active` tests were additions and are gone.
void IServerImpl::SetInterfaceState(unsigned int session_id, unsigned char* data, int data_len) {
        (void)data_len;
        if (!Players.is_valid(session_id)) return;
        Players[session_id].interface_state = *(unsigned int*)data;
    }
    // RequestRankLevel — baseline 0x11260: SP check, reads player+0x284, sends via controller vtable+0x18c
    // returns *(uint*)(player + 0x284) — the rank level
void IServerImpl::RequestRankLevel(unsigned int session_id, unsigned char* data, int data_len) {
        unsigned int max_players = PlayerDB_GetMaxPlayers();
        if (session_id == 0 || (session_id - 1) >= max_players) return;
        // Players[session]+0x348 (client_id) != 0 -- PlayerDB_GetPlayer's null.
        PlayerSlot* p = GetPlayerBySession(session_id);
        if (!p) return;
        // 0x18c is a vtable BYTE offset, not a message id: 0x18c / 4 = IClient slot 99
        // (FLPACKET_COMMON_REQUEST_RANK_LEVEL). baseline allocates the 4-byte block FIRST
        // (06cf128e operator new(4)), then fills it: SinglePlayer() ? the routine(player)
        // (= [player+0x284], rank_level) : 0xffffffff (06cf129a .. 06cf12c1), and sends
        // SendCommonRequestRankLevel(session_id, block, 4) at 06cf12d3. The block is handed over and never
        // freed here: rpclocal's sender (0x06d8b890) sets the packet's owns-buffer flag
        // (06d8b8c3) and the packet destructor frees it.
        unsigned int* const RankBlock = static_cast<unsigned int*>(::operator new(4));
        if (SinglePlayer()) {
            *RankBlock = p->rank_level;
        } else {
            *RankBlock = 0xFFFFFFFF;
        }
        g_pClientInterface->SendCommonRequestRankLevel(
            session_id, reinterpret_cast<unsigned char*>(RankBlock), 4);
    }
    // Trade system. The offers are Common.dll's own CPlayerTradeOffer (common_tradeoffer.h,
    // baseline sizeof 0x24): {vfptr, +4 SourcePlayerId, +8 TargetPlayerId, +0xc TargetOffer,
    // +0x10 EquipDescList, +0x1c MoneyOffered, +0x20 Accepted}. Server.dll imports its
    // ctor/dtor, SetSourcePlayerID, SetTargetPlayerID, SetAccepted, SetMoneyOffered
    // (IAT module storage .. 06d64410) and reads +8 / +0xc / +0x10 / +0x1c / +0x20
    // inline; the port reaches the same fields through the exported accessors. The
    // port's own FTradeOffer struct that stood in for it until 2026-09-24 is gone.

    // Slot 79: InitiateTrade -- baseline module storage .. 06cf1963, RET 8, with an SEH
    // frame around the two allocations. Ported 1:1 2026-09-24 (claude-code:w5-server-06cf):
    // PlayerDB::is_valid inlined on the TARGET only; failure is
    //                         the self-call TerminateTrade(source, 2) (06cf17d5)
    // Players[target].trade_request_partner [slot-0x48] != source ->
    //             TerminateTrade(source, 2) (06cf17fb)
    // source == target -> TerminateTrade(source, 2) (06cf180f)
    // the SOURCE slot, raw: a live offer -> TerminateTrade(
    //             source, 4) and FALL THROUGH (Players reloaded at 06cf1839)
    // the target has a live offer -> IClient +0x198
    //             (SendCommonPlayerTradeTerminateTrade)(source, 6), return
    // two operator new(0x24) + CPlayerTradeOffer::
    //             CPlayerTradeOffer (IAT 06d643e8); the vfptr is then set to Server's
    //             own copy of the vtable (module storage, sole slot = the deleting dtor
    // = ~CPlayerTradeOffer + operator delete) -- `new` here
    // A = source's: SetSourcePlayerID(source), SetTargetPlayerID(
    //             target), A+0xc = B; B = target's: SetSourcePlayerID(target),
    //             SetTargetPlayerID(source), B+0xc = A
    // Players[source].trade_offer = A, Players[target].trade_offer = B
    // self-call IServer +0x158 StopTradeRequest(target)
    // SendCommonPlayerTradeInitiateTrade(source, Players[target].ship_object_id [slot-0x34])
    // SendCommonPlayerTradeInitiateTrade(target, Players[source].ship_object_id)
    // baseline writes neither side's trade_request_partner here; the port's two
    // `= 0` stores (which also stopped StopTradeRequest from ever sending
    // SendCommonPlayerTradeStopTradeRequest) are
    // gone. No null test on the field (the IClient pointer) anywhere in the family.
void IServerImpl::InitiateTrade(unsigned int session_id, unsigned int target_id) {
        PlayerSlot* const target = GetPlayerBySession(target_id);
        if (!target) {
            TerminateTrade(session_id, 2);
            return;
        }
        if (target->trade_request_partner != session_id) {
            TerminateTrade(session_id, 2);
            return;
        }
        if (session_id == target_id) {
            TerminateTrade(session_id, 2);
            return;
        }

        PlayerSlot* const p = &Players[session_id];            // , raw
        if (p->trade_offer != nullptr)
            TerminateTrade(session_id, 4);                     // , falls through

        if (target->trade_offer != nullptr) {
            g_pClientInterface->SendCommonPlayerTradeTerminateTrade(session_id, 6);
            return;
        }

        CPlayerTradeOffer* const SourceOffer = new CPlayerTradeOffer();
        CPlayerTradeOffer* const TargetOffer = new CPlayerTradeOffer();

        SourceOffer->SetSourcePlayerID(session_id);
        SourceOffer->SetTargetPlayerID(target_id);
        SourceOffer->SetTargetOffer(TargetOffer);
        TargetOffer->SetSourcePlayerID(target_id);
        TargetOffer->SetTargetPlayerID(session_id);
        TargetOffer->SetTargetOffer(SourceOffer);

        Players[session_id].trade_offer = SourceOffer;
        Players[target_id].trade_offer = TargetOffer;

        StopTradeRequest(target_id);

        g_pClientInterface->SendCommonPlayerTradeInitiateTrade(session_id, Players[target_id].ship_object_id);
        g_pClientInterface->SendCommonPlayerTradeInitiateTrade(target_id, Players[session_id].ship_object_id);
    }

    // Slot 80: TerminateTrade -- baseline module storage .. 06cf1a39, RET 8. Ported 1:1:
    // PlayerDB::is_valid inlined -> out
    // offer = [slot-0x4c]; null -> straight to the last
    //                         SendCommonPlayerTradeTerminateTrade
    // partner id = offer+8, then the slot's offer is cleared
    // / 06cf19f0   partner id == 0 or offer+0xc == 0 -> skip the partner arm
    // Players[partner].trade_offer = 0 on a RAW slot (no is_valid)
    // (offer+0xc)->vtable[0](1) -- delete the partner's offer
    // SendCommonPlayerTradeTerminateTrade(partner, reason)
    // offer->vtable[0](1) -- delete the own offer
    // SendCommonPlayerTradeTerminateTrade(session, reason), outside the offer branch
void IServerImpl::TerminateTrade(unsigned int session_id, int reason) {
        PlayerSlot* const p = GetPlayerBySession(session_id);
        if (!p) return;

        CPlayerTradeOffer* const Offer = p->trade_offer;
        if (Offer != nullptr) {
            unsigned int const PartnerId = Offer->GetTargetPlayerID();
            p->trade_offer = nullptr;
            CPlayerTradeOffer* const PartnerOffer = Offer->GetTargetOffer();
            if (PartnerId != 0 && PartnerOffer != nullptr) {
                Players[PartnerId].trade_offer = nullptr;
                delete PartnerOffer;
                g_pClientInterface->SendCommonPlayerTradeTerminateTrade(PartnerId, reason);
            }
            delete Offer;
        }

        g_pClientInterface->SendCommonPlayerTradeTerminateTrade(session_id, reason);
    }

    // Slot 81: AcceptTrade -- baseline module storage .. 06cf2003, RET 8, SEH frame for the
    // two local cargo lists. Ported 1:1 2026-09-24 (claude-code:w5-server-06cf):
    // PlayerDB::is_valid inlined -> out
    // own offer [slot-0x4c] == 0 -> out
    // own->SetAccepted(b_accepted)
    // IClient +0x19c (SendCommonPlayerTradeAcceptTrade)(offer+8 = partner id,
    //             own ship_object_id [+0x3e4], b_accepted). The partner record is
    //             indexed RAW from offer+8 (06cf1ada).
    // partner record's own offer [+0x3cc], NO null test:
    //             its Accepted (+0x20) != 1 -> out; own Accepted != 1 -> out
    // own offer's money > the routine(own record) (unsigned
    //             JA) -> TerminateTrade(session, 3) (06cf1feb); the same for the partner
    // the routine on each ship id (+0x3e4); a miss zeroes
    //             that object pointer
    // each object's CBase::Object (+0x10), narrowed to CShip by
    //             (ObjectClass & 0x503) == 0x503, else 0
    // a copy of the own record's cargo list (the routine =
    //             &record+0x278, std::list copy ctor); for every item of the own offer:
    //             find_matching_cargo(ArchId, Count, Health) on the COPY, miss ->
    //             TerminateTrade(session, 3) (06cf1cc2) and out, hit ->
    //             remove_equipment_item(hit->Id, Count) on the copy
    // the same for the partner's offer against a copy of the
    //             partner's list; miss -> TerminateTrade(session, 3) (06cf1cda)
    // free hold, per side, all through ftol:
    //             (int)((float)(int)Archetype::GetShip(record+0x264)->hold_size [+0x100]
    //                   - record's cargo get_cargo_space_occupied())
    //             - (int)incoming offer volume + (int)outgoing offer volume;
    //             either < 0 (signed) -> TerminateTrade(session, 5) (06cf1fb5)
    // the routine(own, partner money - own money, 1), then
    // (partner, own money - partner money, 1) -- unconditional
    // own offer's items: the routine(own, Arch, Count, Health,
    //             1) remove, the routine(partner, Arch, Count, Health, 0, 1) add
    // partner offer's items: the mirror image
    // CEqObj::sync_cargo(record cargo) on both narrowed ships,
    //             no null test
    // the routine(&own ship->Id [+0xb0]) for the SYSTEM (a
    //             miss makes it 0, used unguarded), then the routine(system, own
    //             object) and the routine(system, partner object)
    // TerminateTrade(session, 1), then the two local lists are freed
    // = PlayerData_GetCash, the routine = PlayerData_AdjustCash,
    // = PlayerData_RemoveCargo, the routine = PlayerData_AddEquipment with
    // the cargo-bay hardpoint (see server_classes.cpp's add-cargo site), the routine =
    // StarSystem_SendRemoteCargoUpdate.
void IServerImpl::AcceptTrade(unsigned int session_id, bool b_accepted) {
        PlayerSlot* const p = GetPlayerBySession(session_id);
        if (!p) return;
        CPlayerTradeOffer* const Offer = p->trade_offer;
        if (Offer == nullptr) return;

        Offer->SetAccepted(b_accepted);

        unsigned int const PartnerId = Offer->GetTargetPlayerID();
        PlayerSlot& Own = Players[session_id];
        PlayerSlot& Partner = Players[PartnerId];                                 // raw, module storage
        g_pClientInterface->SendCommonPlayerTradeAcceptTrade(PartnerId, Own.ship_object_id, b_accepted);

        CPlayerTradeOffer* const PartnerOffer = Partner.trade_offer;
        if (!PartnerOffer->GetAccepted()) return;
        if (!Offer->GetAccepted()) return;

        if (Offer->GetMoneyOffered() > static_cast<unsigned long>(PlayerData_GetCash(Own))) {
            TerminateTrade(session_id, 3);
            return;
        }
        if (PartnerOffer->GetMoneyOffered() > static_cast<unsigned long>(PlayerData_GetCash(Partner))) {
            TerminateTrade(session_id, 3);
            return;
        }

        GameObject* OwnObj = nullptr;
        GameObject* PartnerObj = nullptr;
        StarSystem* LookupSystem = nullptr;
        if (!Server_FindObject(Own.ship_object_id, OwnObj, LookupSystem))
            OwnObj = nullptr;
        if (!Server_FindObject(Partner.ship_object_id, PartnerObj, LookupSystem))
            PartnerObj = nullptr;

        CShip* OwnShip = nullptr;
        CBase* const OwnBase = OwnObj;
        if (OwnBase != nullptr) {
            CObject* const Cobj = OwnBase->Object;
            if (Cobj != nullptr && (static_cast<unsigned int>(Cobj->ObjectClass) & 0x503u) == 0x503u)
                OwnShip = static_cast<CShip*>(Cobj);
        }
        CShip* PartnerShip = nullptr;
        CBase* const PartnerBase = PartnerObj;
        if (PartnerBase != nullptr) {
            CObject* const Cobj = PartnerBase->Object;
            if (Cobj != nullptr && (static_cast<unsigned int>(Cobj->ObjectClass) & 0x503u) == 0x503u)
                PartnerShip = static_cast<CShip*>(Cobj);
        }

        std::list<EquipDesc> const& OwnItems = Offer->GetList().Items;
        std::list<EquipDesc> const& PartnerItems = PartnerOffer->GetList().Items;
        std::list<EquipDesc>::const_iterator It;

        EquipDescList OwnCargo(Own.equipment);
        for (It = OwnItems.begin(); It != OwnItems.end(); ++It) {
            EquipDesc const* const Hit = OwnCargo.find_matching_cargo(It->ArchId, It->Count, It->Health);
            if (Hit == nullptr) {
                TerminateTrade(session_id, 3);
                return;
            }
            OwnCargo.remove_equipment_item(Hit->Id, It->Count);
        }

        EquipDescList PartnerCargo(Partner.equipment);
        for (It = PartnerItems.begin(); It != PartnerItems.end(); ++It) {
            EquipDesc const* const Hit = PartnerCargo.find_matching_cargo(It->ArchId, It->Count, It->Health);
            if (Hit == nullptr) {
                TerminateTrade(session_id, 3);
                return;
            }
            PartnerCargo.remove_equipment_item(Hit->Id, It->Count);
        }

        // . The x87 difference is taken in double: both
        // operands are floats (the hold via FILD/FSTP float, the occupied volume as
        // returned), so the subtraction is exact before ftol truncates it.
        int const OwnOfferVolume = static_cast<int>(Offer->GetList().get_cargo_space_occupied());
        int const PartnerOfferVolume = static_cast<int>(PartnerOffer->GetList().get_cargo_space_occupied());
        float const OwnHold =
            static_cast<float>(static_cast<int>(Archetype::GetShip(Own.ship_archetype)->HoldSize));
        int const OwnFree = static_cast<int>(static_cast<double>(OwnHold) -
                                             static_cast<double>(Own.equipment.get_cargo_space_occupied()))
                            - PartnerOfferVolume + OwnOfferVolume;
        float const PartnerHold =
            static_cast<float>(static_cast<int>(Archetype::GetShip(Partner.ship_archetype)->HoldSize));
        int const PartnerFree = static_cast<int>(static_cast<double>(PartnerHold) -
                                                 static_cast<double>(Partner.equipment.get_cargo_space_occupied()))
                                - OwnOfferVolume + PartnerOfferVolume;
        if (OwnFree < 0 || PartnerFree < 0) {
            TerminateTrade(session_id, 5);
            return;
        }

        PlayerData_AdjustCash(Own, static_cast<int>(PartnerOffer->GetMoneyOffered() - Offer->GetMoneyOffered()),
                              true);
        PlayerData_AdjustCash(Partner, static_cast<int>(Offer->GetMoneyOffered() - PartnerOffer->GetMoneyOffered()),
                              true);

        for (It = OwnItems.begin(); It != OwnItems.end(); ++It) {
            PlayerData_RemoveCargo(Own, It->ArchId, It->Count, It->Health, true);
            PlayerData_AddEquipment(Partner, It->ArchId, EquipDesc::CARGO_BAY_HP_NAME, It->Count,
                                    It->Health, /*bEquipped*/false, /*bNoCombine*/false,
                                    /*bNotify*/true);
        }
        for (It = PartnerItems.begin(); It != PartnerItems.end(); ++It) {
            PlayerData_RemoveCargo(Partner, It->ArchId, It->Count, It->Health, true);
            PlayerData_AddEquipment(Own, It->ArchId, EquipDesc::CARGO_BAY_HP_NAME, It->Count,
                                    It->Health, /*bEquipped*/false, /*bNoCombine*/false,
                                    /*bNotify*/true);
        }

        OwnShip->sync_cargo(Own.equipment);
        PartnerShip->sync_cargo(Partner.equipment);

        GameObject* ScratchObj = nullptr;
        StarSystem* ShipSystem = nullptr;
        if (!Server_FindObject(OwnShip->Id, ScratchObj, ShipSystem))
            ShipSystem = nullptr;
        StarSystem_SendRemoteCargoUpdate(*ShipSystem, OwnObj);
        StarSystem_SendRemoteCargoUpdate(*ShipSystem, PartnerObj);

        TerminateTrade(session_id, 1);
    }

    // Slot 82: SetTradeMoney -- baseline module storage .. 06cf20d0, RET 8. Ported 1:1:
    // PlayerDB::is_valid inlined -> out
    // offer [slot-0x4c] == 0 -> TerminateTrade(session, 2)
    // offer+0xc != 0 -> (offer+0xc)->SetAccepted(false)
    // SetMoneyOffered(money); 06cf207c SetAccepted(false)
    // IClient +0x1a0 (SendCommonPlayerTradeSetTradeMoney)(session, own ship_object_id, money)
    // SendCommonPlayerTradeSetTradeMoney(offer+8, own ship_object_id, money)
void IServerImpl::SetTradeMoney(unsigned int session_id, unsigned long money) {
        PlayerSlot* const p = GetPlayerBySession(session_id);
        if (!p) return;
        CPlayerTradeOffer* const Offer = p->trade_offer;
        if (Offer == nullptr) {
            TerminateTrade(session_id, 2);
            return;
        }
        CPlayerTradeOffer* const PartnerOffer = Offer->GetTargetOffer();
        if (PartnerOffer != nullptr)
            PartnerOffer->SetAccepted(false);
        Offer->SetMoneyOffered(money);
        Offer->SetAccepted(false);
        g_pClientInterface->SendCommonPlayerTradeSetTradeMoney(
            session_id, Players[session_id].ship_object_id, money);
        g_pClientInterface->SendCommonPlayerTradeSetTradeMoney(
            Offer->GetTargetPlayerID(), Players[session_id].ship_object_id, money);
    }

    // Slot 83: AddTradeEquip -- baseline module storage .. 06cf21b4, RET 8. Ported 1:1:
    // PlayerDB::is_valid inlined -> out
    // offer == 0 -> tail JMP TerminateTrade(session, 2) (06cf21b4)
    // offer+0xc != 0 -> (offer+0xc)->SetAccepted(false)
    // (offer+0x10).add_equipment_item(equip,
    //             arch_is_combinable(equip.ArchId)); the result is dropped
    // SetAccepted(false)
    // IClient +0x1a4 (SendCommonPlayerTradeAddTradeEquip)(session, own ship_object_id, &equip)
    // SendCommonPlayerTradeAddTradeEquip(offer+8, own ship_object_id, &equip)
void IServerImpl::AddTradeEquip(unsigned int session_id, const EquipDesc& equip) {
        PlayerSlot* const p = GetPlayerBySession(session_id);
        if (!p) return;
        CPlayerTradeOffer* const Offer = p->trade_offer;
        if (Offer == nullptr) {
            TerminateTrade(session_id, 2);
            return;
        }
        CPlayerTradeOffer* const PartnerOffer = Offer->GetTargetOffer();
        if (PartnerOffer != nullptr)
            PartnerOffer->SetAccepted(false);
        Offer->GetList().add_equipment_item(equip, arch_is_combinable(equip.ArchId));
        Offer->SetAccepted(false);
        g_pClientInterface->SendCommonPlayerTradeAddTradeEquip(
            session_id, Players[session_id].ship_object_id, equip);
        g_pClientInterface->SendCommonPlayerTradeAddTradeEquip(
            Offer->GetTargetPlayerID(), Players[session_id].ship_object_id, equip);
    }

    // Slot 84: DelTradeEquip -- baseline module storage .. 06cf22b8, RET 8. Ported 1:1:
    // PlayerDB::is_valid inlined -> out
    // offer == 0 -> tail JMP TerminateTrade(session, 2) (06cf22b8)
    // offer+0xc != 0 -> (offer+0xc)->SetAccepted(false)
    // (offer+0x10).find_matching_cargo(equip.ArchId, equip.Count,
    //             equip.Health)
    // a hit -> remove_equipment_item(hit->Id, equip.Count)
    // SetAccepted(false)
    // IClient +0x1a8 (SendCommonPlayerTradeDelTradeEquip)(session, own ship_object_id, &equip)
    // SendCommonPlayerTradeDelTradeEquip(offer+8, own ship_object_id, &equip)
void IServerImpl::DelTradeEquip(unsigned int session_id, const EquipDesc& equip) {
        PlayerSlot* const p = GetPlayerBySession(session_id);
        if (!p) return;
        CPlayerTradeOffer* const Offer = p->trade_offer;
        if (Offer == nullptr) {
            TerminateTrade(session_id, 2);
            return;
        }
        CPlayerTradeOffer* const PartnerOffer = Offer->GetTargetOffer();
        if (PartnerOffer != nullptr)
            PartnerOffer->SetAccepted(false);
        EquipDescList& List = Offer->GetList();
        EquipDesc const* const Hit = List.find_matching_cargo(equip.ArchId, equip.Count, equip.Health);
        if (Hit != nullptr)
            List.remove_equipment_item(Hit->Id, equip.Count);
        Offer->SetAccepted(false);
        g_pClientInterface->SendCommonPlayerTradeDelTradeEquip(
            session_id, Players[session_id].ship_object_id, equip);
        g_pClientInterface->SendCommonPlayerTradeDelTradeEquip(
            Offer->GetTargetPlayerID(), Players[session_id].ship_object_id, equip);
    }

    // Slot 85: RequestTrade -- baseline module storage .. 06cf234d, RET 8. Ported 1:1:
    // PlayerDB::is_valid inlined on the source -> out
    // the same on the target -> out
    // Players[source].trade_offer != 0 -> out
    // Players[target].trade_request_partner == source ->
    //             self-call IServer +0x13c InitiateTrade(source, target), return
    // otherwise the routine(source record, target) --
    //             PlayerData_SetTradeRequest
    // baseline's own body makes no IClient call.
void IServerImpl::RequestTrade(unsigned int session_id, unsigned int target_id) {
        PlayerSlot* const p = GetPlayerBySession(session_id);
        if (!p) return;
        PlayerSlot* const target = GetPlayerBySession(target_id);
        if (!target) return;
        if (Players[session_id].trade_offer != nullptr) return;
        if (Players[target_id].trade_request_partner == session_id) {
            InitiateTrade(session_id, target_id);
            return;
        }
        PlayerData_SetTradeRequest(Players[session_id], target_id);
    }

    // Slot 86: StopTradeRequest -- baseline module storage .. 06cf23a8, RET 4. Ported 1:1:
    // PlayerDB::is_valid inlined -> out
    // IClient +0x198 (SendCommonPlayerTradeTerminateTrade)(session, 0)
    // the routine(record, 1) -- PlayerData_CancelTradeRequest with the
    //             partner-list unlink on (the port used to inline half of it)
void IServerImpl::StopTradeRequest(unsigned int session_id) {
        PlayerSlot* const p = GetPlayerBySession(session_id);
        if (!p) return;
        g_pClientInterface->SendCommonPlayerTradeTerminateTrade(session_id, 0);
        PlayerData_CancelTradeRequest(Players[session_id], true);
    }
    // ReqDifficultyScale — baseline 0x65a90: SP only, validates player, calls the routine
    // clamps param_1 to [0.0, 2.0], stores at player+0x3C4
void IServerImpl::ReqDifficultyScale(float scale, unsigned int session_id) {
        if (!SinglePlayer()) return;
        unsigned int max_players = PlayerDB_GetMaxPlayers();
        if (session_id == 0 || (session_id - 1) >= max_players) return;
        PlayerSlot* p = GetPlayerBySession(session_id);
        if (!p) return;   // client_id != 0; baseline has no `active` test (w4-server-guards)
        // clamps to [0.0, 2.0] and stores at player+0x3C4.
        // The x87 tests are written as negations so an unordered (NaN) input goes
        // the same way as in baseline: module storage TEST AH,0x41 / JNZ takes the
        // 0.0f (module storage) arm for below, equal AND unordered; module storage
        // TEST AH,0x5 / JP takes the 2.0f (module storage = 0x40000000) arm for
        // not-below and unordered.
        if (!(scale > 0.0f)) scale = 0.0f;
        if (!(scale < 2.0f)) scale = 2.0f;
        p->difficulty_scale = scale;
    }
    // is two instructions and ignores `this` entirely:
    // a16802d906  MOV EAX,[.data:the field]
    // c3          RET
    // is &Players + 0x8, i.e. PlayerDB::m_ServerID. The sibling
    // IServerImpl::GetServerSig at 0x06d453e0 has the same shape:
    // b97002d906  MOV ECX,.data:the field       ; &Players+0x10
    // ff257046d6  JMP [COMMON.DLL::CPlayerAccount::GetAccountNameSigChar]
    // An immediate global address, never `LEA ECX,[this + N]` -- which is the
    // proof that baseline's IServerImpl embeds no PlayerDB.
unsigned int IServerImpl::GetServerID() { return Players.GetServerID(); }
    // Same story, 0x06d453e0 above: baseline loads &Players + 0x10 and tail-jumps
    // into CPlayerAccount::GetAccountNameSigChar. The literal "OutpostFrontier" the
    // port returned here was invented.
const char* IServerImpl::GetServerSig() { return Players.GetServerSig(); }
    // DumpPacketStats — baseline 0x123b0: debug function, calls controller vtable+500
void IServerImpl::DumpPacketStats(const char* name) {
        // baseline 0x123b0: if the IClient global is set, forwards to its slot 125 (+0x1f4)
        // -- the one IClient slot whose signature baseline spells out, and literally the same
        // code as ?DumpPacketStats@IServerImpl@@UAEXPBD@Z in rpclocal.
        if (g_pClientInterface) g_pClientInterface->DumpPacketStats(name);
    }
    // Slot 91: Dock -- baseline module storage .. module storage,
    // ?Dock@IServerImpl@@UAEXABI0@Z, RET 8. Both arguments are references;
    // rpclocal serializes *BaseId only (ch3 op 0x24), so SessionId is the second
    // one (module storage reads param_2 as the session, module storage param_1 as
    // the base). pub::Player::ForceLand (module storage) is the other caller.

    // REWRITTEN 2026-09-24 from the listing. The previous body was a stand-in:
    // it checked session/ship/base guards baseline does not have, sent invented
    // controller events 0x39 and 1, and set the slot's base fields itself --
    // baseline does none of that here; the base is entered later, by the client's
    // own BaseEnter after FLPACKET_SERVER_LAND.
void IServerImpl::Dock(const unsigned int& base_id, const unsigned int& session_id) {
        // .. module storage -- the routine (Server_FindObject) on
        // the slot's ship_object_id (`Players + sid*0x418 - 0x34`, no validation of
        // sid), out-parameters the object (ESP+0x10) and its StarSystem (ESP+0xc);
        // return when it fails. The system is that third out-parameter, used below
        // without a null test (an in-transit ship leaves it 0, as in baseline).
        PlayerData& Slot = Players[session_id];
        GameObject* ShipObj = nullptr;
        StarSystem* System = nullptr;
        if (!Server_FindObject(Slot.ship_object_id, ShipObj, System)) return;

        // .. module storage -- Universe::get_base(*BaseId).
        Universe::IBase const* const Base = Universe::get_base(base_id);
        if (Base == nullptr) return;

        // .. module storage -- the routine on that system:
        // (Ship, no target, &*BaseId, &land = 1). The byte is the caller's own
        // param_2 slot, overwritten with 1 at module storage.
        bool const bLand = true;
        System->queue_dock(ShipObj, nullptr, &base_id, &bLand);

        // .. module storage -- when the base is in the system the
        // player is already in, that is all: the next regular system update
        // lands the ship. IBase slot +0x10 is the base's system id.
        if (Base->SystemId == Slot.system_id) return;

        // .. module storage -- otherwise run the routine on the
        // ship's system right now, with 0.2f (PUSH 0x3e4ccccd): the dock queue
        // is processed, the LAND goes out and the player leaves that system.
        StarSystem_Update(System, 0.2f);

        // .. module storage -- the routine on the slot.
        PlayerData_FlushPendingVisits(Players[session_id]);

        // .. module storage -- the routine(0x3a, {system id
        // (the routine: SystemInfo vtable slot 0), base's system, sid}).
        unsigned int LeaveEvent[3];
        LeaveEvent[0] = System->SystemInfo->NicknameId;
        LeaveEvent[1] = Base->SystemId;
        LeaveEvent[2] = session_id;
        Server_ControllerDispatch(0, pub::Controller::EMessage::SystemExit, LeaveEvent);

        // .. module storage -- the slot's system_id (+0x3e0) is
        // the base's system from here on.
        Players[session_id].system_id = LeaveEvent[1];

        // .. module storage -- the routine(0x3b, seven dwords
        // {base's system, the old system id (the routine again), sid, 0, 0, 0,
        // 0}), the same event CharacterSelect sends with a zero second dword.
        unsigned int EnterEvent[7];
        EnterEvent[0] = LeaveEvent[1];
        EnterEvent[1] = System->SystemInfo->NicknameId;
        EnterEvent[2] = session_id;
        EnterEvent[3] = 0;
        EnterEvent[4] = 0;
        EnterEvent[5] = 0;
        EnterEvent[6] = 0;
        Server_ControllerDispatch(0, pub::Controller::EMessage::SystemEnter, EnterEvent);
    }

    // Slot 92 (+0x170, module storage): PushToServer — Server.dll's body is the shared
    // RET 4 stub at 0x06ce1050. IServerImpl's OWN virtual, not an IServer override:
    // IServer's baseline table (module storage) stops at slot 91, so this, the only virtual
    // IServerImpl adds, lands right after it. The packet queue lives on the rpclocal side
    // (IServerImpl::PushToServer 0x06d8f460 appends to the CDAConnection), not here.
void IServerImpl::PushToServer(CDAPacket* /*packet*/) {}

    // ?GetServerStats@IServerImpl@@QAEXAAUServerStats@@@Z is NOT virtual — see
    // GetServerStats below, which is the real (non-virtual) implementation.
    // The old vtable entries "GetServerStats", "GetServerStats2", "TradeAbort" and
    // "UnknownSlot93" were fabrications and are gone.

    // =================================================================
    // IChatServer::SubmitChat -- baseline 0x06d191b0 (RVA 0x391B0), `RET 0x14`.

    // The whole of baseline's FLChatServer.cpp -- command table, group verbs,
    // "<name>: " header, the private / universe / system / local / group
    // routing -- lives in src/Server/server_chat.cpp. baseline's body is a
    // member only because it is reached through the IChatServer vtable; it
    // touches no member of IServerImpl, so the forward loses nothing.
    // =================================================================
void IServerImpl::SubmitChat(CHAT_ID Sender, unsigned long Size, const void* Data, CHAT_ID Target, int SystemId) {
        Server_SubmitChat(Sender, Size, Data, Target, SystemId);
    }

    // =================================================================
    // GetServerStats(ServerStats&) -- non-virtual export
    // ?GetServerStats@IServerImpl@@QAEXAAUServerStats@@@Z, module storage..06d45b36.
    // =================================================================
void IServerImpl::GetServerStats(ServerStats& OutStats) {
        OutStats = GServerStats;                           // MOV ECX,0xb / REP MOVSD
        unsigned int PlayerCount = 0;
        PlayerData* Slot = Players.traverse_active(nullptr);
        while (Slot != nullptr) {
            ++PlayerCount;
            Slot = Players.traverse_active(Slot);
        }
        OutStats.Reserved3 = 0;                            // [EBX+0x28]
        OutStats.Reserved2 = 0;                            // [EBX+0x24]
        OutStats.Reserved1 = 0;                            // [EBX+0x20]
        OutStats.PlayerCount = PlayerCount;                // [EBX]
    }

    // Exported constructors and operator= for Server.dll baseline ABI.

    // baseline IServerImpl is TWELVE BYTES and holds no player storage at all.
    // The default ctor, 0x06cef560, is six instructions end to end:
    // 8bc1          MOV   EAX,ECX
    // c74004445...  MOV   dword ptr [EAX + 0x4],06d5fff0
    // c74008000...  MOV   dword ptr [EAX + 0x8],0x0
    // c700cc5bd606  MOV   dword ptr [EAX],IServerImpl::`vftable'
    // c74004c45...  MOV   dword ptr [EAX + 0x4],06d191b0
    // c3            RET
    // -- IServer vptr at +0, IChatServer vptr at +4, one dword at +8. The copy
    // ctor (0x06cef580) and operator= (0x06cef5b0) copy exactly that one dword.
    // The global instance confirms the size: `Server`, ordinal 524, sits at
    // and the next symbol is at 06d8fad8, twelve bytes later.

    // The port's `char m_Players[4096]` was therefore an invention twice over:
    // baseline has no such member, and 4096 is not derived from anything. It was
    // memset by the ctor and then `reinterpret_cast<PlayerDB*>`-ed and called
    // through, running container code over zeroed std::wstring/std::map state.
    // The real table is the exported global `Players` (see common_server.h).

// Field_0x8 (baseline +0x8, the single data member) is declared in server_iserverimpl.h.

// (module storage .. 06d43385). baseline keeps the module in
// even when LoadLibraryA fails, and on success calls BOTH
// GetProcAddress results straight away -- `CALL ESI` / `CALL EAX` at
// /06d43363 and module storage/06d43378 -- with no null test on
// either export. The results land in the field (IClient) and the field
// (IChatClient); Startup copies them on through the routine/the routine into
// the live pair the field/the field. The function returns the field
// (module storage, reached on both paths); Startup ignores it.
static IClient* Server_LoadClientDll(const char* module_path) {
    g_hClientDll = LoadLibraryA(module_path);
    if (g_hClientDll) {
        typedef IClient* (__cdecl *GetClientInterfaceFn)();
        typedef IChatClient* (__cdecl *GetChatClientInterfaceFn)();
        GetClientInterfaceFn pfn = (GetClientInterfaceFn)GetProcAddress(g_hClientDll, "GetClientInterface");
        GLoadedClientInterface = pfn();
        GetChatClientInterfaceFn pfn2 = (GetChatClientInterfaceFn)GetProcAddress(g_hClientDll, "GetChatClientInterface");
        GLoadedChatClientInterface = pfn2();
    }
    return GLoadedClientInterface;
}

// (module storage .. 06d433be): `if (the field) { FreeLibrary;
// = 0; the field = 0; the field = 0; }` -- all three stores
// sit inside the null test, and only the LOADED pair is cleared. The live IClient
// copy the field is zeroed by the caller (Shutdown, module storage); the live
// IChatClient copy the field is never zeroed in baseline. Shutdown pushes
// before the call (module storage) and pops it after (06d45200),
// but the body never reads a stack argument, so the port takes none.
static void Server_FreeClientDll() {
    if (g_hClientDll) {
        FreeLibrary(g_hClientDll);
        g_hClientDll = nullptr;
        GLoadedClientInterface = nullptr;
        GLoadedChatClientInterface = nullptr;
    }
}

// (module storage): MOV EAX,[the field] / RET. Sole caller
// Startup module storage, which stores the result into the field.
static IChatClient* Server_GetLoadedChatClientInterface() {
    return GLoadedChatClientInterface;
}

// (module storage): MOV EAX,[the field] / RET. Sole caller
// Startup module storage, which stores the result into the field.
static IClient* Server_GetLoadedClientInterface() {
    return GLoadedClientInterface;
}

// — Read [OutpostFrontier] initial_world from OutpostFrontier.ini
static void Server_ReadWorldIni(const char* config_path) {
    INI_Reader reader;
    if (!reader.open(config_path, false)) return;
    while (reader.read_header()) {
        if (reader.is_header("OutpostFrontier")) {
            while (reader.read_value()) {
                if (reader.is_value("initial_world")) {
                    // get_value_string, then a byte copy
                    // into the field up to and including the NUL -- no null test
                    // on the returned pointer (the port's guard was removed
                    // 2026-09-24, s2). baseline stops reading once found (BL flag),
                    // which the early return mirrors.
                    const char* val = reader.get_value_string();
                    strncpy_s(g_InitialWorldPath, sizeof(g_InitialWorldPath), val, _TRUNCATE);
                    return;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Server_LoadWorldState -- baseline the routine (module storage .. module storage),
// cdecl, no arguments; called only from Startup in multiplayer (module storage).
// The MP twin of Server_LoadSinglePlayerState: the same [Time] / feelings / event
// 0x40 sequence, over the initial_world file instead of a character file.
// (*NeuronRuntime::Data)->GetFileSysPath(buf, 0x104)  (IFileSystem +0x84)
// _makepath(buf, NULL, buf, the field, NULL)
//                         (the field = initial_world, the routine)
// INI_Reader ctor ; open(buf, false) -- result not tested
// get_state ; time = 0.0 ; read_header loop until
//                         is_header("Time") -> read_value, time = get_value_float(0)
//                         ; set_state
// the field = time ; Delta(06d8f970).start()
// Reputation::FreeFeelings() ; LoadFeelings(reader)
// the routine(0x40, &reader)
// ---------------------------------------------------------------------------
static void Server_LoadWorldState() {
    // baseline builds the path in place (same buffer as dir and output); the
    // modern CRT's _makepath may pre-fill its output in debug builds, so the
    // dir half gets its own buffer. The resulting string is the same.
    char DataPath[0x104];
    NeuronRuntime::Data->GetFileSysPath(DataPath, 0x104);
    char WorldPath[0x104];
    _makepath(WorldPath, nullptr, DataPath, g_InitialWorldPath, nullptr);

    INI_Reader Reader;
    Reader.open(WorldPath, false);

    INI_Reader::State Saved;
    Reader.get_state(Saved);
    double Time = 0.0;
    if (Reader.read_header()) {
        do {
            if (Reader.is_header("Time")) {
                Reader.read_value();
                Time = Reader.get_value_float(0);
                break;
            }
        } while (Reader.read_header());
    }
    Reader.set_state(Saved);
    GServerTimeBase = Time;
    g_TimingDelta.start();
    ::Reputation::FreeFeelings();
    ::Reputation::LoadFeelings(Reader);
    Server_ControllerDispatch(0, pub::Controller::EMessage::LoadGame, &Reader);
}

// / module storage -- a TString<0x104> {length, chars}: the
// `path` value of the [Initial SP/MP DLLs] section. The static initializer
// seeds it empty (XOR EAX,EAX / MOV [06d8f650],EAX / MOV [06d8f654],AL);
// writes it; pub::DLL_LoadLibrary (0x06d43c40, server_pub.cpp) reads
// the chars as the directory for every relative controller DLL name, so it keeps
// its value for the rest of the session, not just for the ini walk.
unsigned int GDllPathLength = 0;
char GDllPath[0x104] = {0};

// (module storage .. module storage), cdecl (const char* IniPath).
// Startup passes the same ini path it gave DataConfig::startup and the routine
// (module storage / module storage, both PUSH EDI = param_1+0x104).
// INI_Reader::open(IniPath, false); read_header; on failure skip to cleanup
// per header: RefreshLoadingProgress()
//             (is_header("Initial SP DLLs") && SinglePlayer()) ||
//             (is_header("Initial MP DLLs") && !SinglePlayer()), then the read_value loop
// "path": get_value_string(); null -> {0, ""} (06d4393a), else
//             len = the routine(GDllPath, 0x104, s) (06d43956)
// "DLL":  get_value_string(0/1/2) each copied the same way into a
//             TString<0x104> name, TString<0x104> interface, TString<0x20> priority
// the priority word is looked up (_stricmp) in the 5-row table at
// (0x14-byte rows: char[16] name, int value):
//             HIGHEST 0, ABOVE_NORMAL 1, NORMAL 2, BELOW_NORMAL 3, LOWEST 4
// pub::Controller::Create(name, interface, NULL, priority)
// No match leaves the priority slot ([ESP+0x10]) holding whatever it held before:
// the previous row's priority, or uninitialised stack for the first row. The port
// keeps one function-scope variable for the same carry-over and starts it at 0
// (HIGHEST) where baseline's first value is indeterminate.
static void Server_LoadInitialDlls(const char* IniPath) {
    struct FPriorityRow { const char* Name; unsigned int Value; };
    static const FPriorityRow PriorityTable[5] = {
        { "HIGHEST", 0 }, { "ABOVE_NORMAL", 1 }, { "NORMAL", 2 },
        { "BELOW_NORMAL", 3 }, { "LOWEST", 4 },
    };
    // Src null -> 0 and Dst untouched; else copy min(strlen, Size-1)
    // chars, terminate, return the copied length. The null arm is inlined by every
    // caller here as {len = 0, chars[0] = 0}.
    struct FBounded {
        static unsigned int Copy(char* Dst, unsigned int Size, const char* Src) {
            unsigned int Len = static_cast<unsigned int>(strlen(Src));
            if (!(Len < Size)) Len = Size - 1;
            memcpy(Dst, Src, Len);
            Dst[Len] = '\0';
            return Len;
        }
    };

    INI_Reader Reader;
    unsigned int Priority = 0;
    if (Reader.open(IniPath, false) && Reader.read_header()) {
        do {
            RefreshLoadingProgress();
            if (((Reader.is_header("Initial SP DLLs") && SinglePlayer()) ||
                 (Reader.is_header("Initial MP DLLs") && !SinglePlayer())) &&
                Reader.read_value()) {
                do {
                    if (Reader.is_value("path")) {
                        const char* PathValue = Reader.get_value_string();
                        if (PathValue == nullptr) {
                            GDllPathLength = 0;
                            GDllPath[0] = '\0';
                        } else {
                            GDllPathLength = FBounded::Copy(GDllPath, sizeof(GDllPath), PathValue);
                        }
                    } else if (Reader.is_value("DLL")) {
                        char DllName[0x104];
                        char InterfaceName[0x104];
                        char PriorityName[0x20];
                        const char* Value = Reader.get_value_string(0);
                        DllName[0] = '\0';
                        if (Value != nullptr) FBounded::Copy(DllName, sizeof(DllName), Value);
                        Value = Reader.get_value_string(1);
                        InterfaceName[0] = '\0';
                        if (Value != nullptr) FBounded::Copy(InterfaceName, sizeof(InterfaceName), Value);
                        Value = Reader.get_value_string(2);
                        PriorityName[0] = '\0';
                        if (Value != nullptr) FBounded::Copy(PriorityName, sizeof(PriorityName), Value);
                        for (unsigned int Row = 0; Row < 5; ++Row) {
                            if (_stricmp(PriorityTable[Row].Name, PriorityName) == 0) {
                                Priority = PriorityTable[Row].Value;
                                break;
                            }
                        }
                        Server_ControllerCreate(DllName, InterfaceName, nullptr, Priority);
                    }
                } while (Reader.read_value());
            }
        } while (Reader.read_header());
    }
}

// ===========================================================================
// Controller / Timer / Physics Infrastructure
// Matches baseline Server.dll internal data structures
// ===========================================================================

// Controller node — 32 bytes (0x20), matches baseline layout
struct FControllerNode {
    FControllerNode* Prev;        // +0x00
    FControllerNode* Next;        // +0x04
    IDLL* InterfacePtr;           // +0x08  CreateInstance result (Content.dll's IDLL)
    HMODULE ModuleHandle;         // +0x0C
    float HeartbeatAccum;         // +0x10  accumulator (stored as float bits in int)
    float HeartbeatTimer;         // +0x14  period (stored as float bits in int)
    unsigned int FLID;            // +0x18  unique controller ID
    unsigned int Priority;        // +0x1C  sort order
};

// Timer node for the routine timer firing system
struct FTimerNode {
    FTimerNode* Prev;             // +0x00
    FTimerNode* Next;             // +0x04
    unsigned int TargetController; // +0x08  target controller FLID
    // +0x0c/+0x10 are pub::Controller::TimerExpired's two dwords, copied verbatim by
    // pub::SetTimer (baseline 0x06d5ee78..0x06d5ee84) and handed back to the controller
    // as the message payload when the timer fires (baseline 0x06d5e00a-0x06d5e014).
    unsigned int Param1;          // +0x0C
    unsigned int Param2;          // +0x10
    float RemainingTime;          // +0x14  countdown
};

// Global controller lists (match baseline addresses)
static FControllerNode g_ControllerListSentinel = { &g_ControllerListSentinel, &g_ControllerListSentinel, nullptr, nullptr, 0, 0, 0, 0 };
static FControllerNode g_DeferredListSentinel = { &g_DeferredListSentinel, &g_DeferredListSentinel, nullptr, nullptr, 0, 0, 0, 0 };
// /62c/630: a std::list of POINTERS to active-list nodes (0xc-byte
// list nodes, ctor 0x06d43560 / dtor 0x06d435a0). pub::Controller::Destroy only queues
// here; the heartbeat walk (the routine) does the real teardown next tick.
static std::list<FControllerNode*> g_DestroyPending;
static unsigned int g_ControllerCount = 0;
static unsigned int g_DeferredCount = 0;
static unsigned int g_ReentrancyGuard = 0;

// Controller DLL entry points as the baseline call sites use them:
// CreateInstance(Flid, Name, Parms) at module storage (three pushes, cdecl) and
// DestroyInstance(Interface) at module storage / 06d43df1 / 06d43f21 (one push).
// baseline resolves both with GetProcAddress, never through the import table; the only
// controller DLL [Initial SP/MP DLLs] and Content's own Controller::Create calls name is
// Content.dll, which exports both plus IDLL (Content.def @1..@7). The pointer types are
// therefore taken from content.h's declarations rather than restated here.
typedef decltype(&::CreateInstance) FControllerCreateInstanceFn;
typedef decltype(&::DestroyInstance) FControllerDestroyInstanceFn;

// Timer list for the routine
static FTimerNode g_TimerListSentinel = { &g_TimerListSentinel, &g_TimerListSentinel, 0, 0, 0, 0.0f };
static unsigned int g_TimerCount = 0;

// the timer list's destructor, registered through _onexit by the
// list's static initializer 0x06d5dec0. Walks from the first node, and for each one
// unlinks it, frees it (0x06d5df24) and decrements the field; then frees the head node
// and zeroes the head pointer and the count (0x06d5df43..0x06d5df53). The port's head is
// the static sentinel above rather than a 0x18-byte heap node, so instead of being freed
// and nulled it is relinked to itself; nodes are freed with the allocator
// Server_ControllerSetTimer uses.
struct FTimerListTeardown {
    ~FTimerListTeardown() {
        FTimerNode* Cur = g_TimerListSentinel.Next;
        while (Cur != &g_TimerListSentinel) {
            FTimerNode* Node = Cur;
            Cur = Cur->Next;
            Node->Prev->Next = Node->Next;
            Node->Next->Prev = Node->Prev;
            free(Node);
            g_TimerCount--;
        }
        g_TimerListSentinel.Prev = &g_TimerListSentinel;
        g_TimerListSentinel.Next = &g_TimerListSentinel;
        g_TimerCount = 0;
    }
};
static FTimerListTeardown g_TimerListTeardown;


// Linked list helpers
static void ListInsertBefore(FControllerNode* sentinel, FControllerNode* node) {
    node->Next = sentinel;
    node->Prev = sentinel->Prev;
    sentinel->Prev->Next = node;
    sentinel->Prev = node;
}

static void ListRemove(FControllerNode* node) {
    node->Prev->Next = node->Next;
    node->Next->Prev = node->Prev;
    node->Prev = node;
    node->Next = node;
}

static void TimerListInsertBefore(FTimerNode* sentinel, FTimerNode* node) {
    node->Next = sentinel;
    node->Prev = sentinel->Prev;
    sentinel->Prev->Next = node;
    sentinel->Prev = node;
}

static void TimerListRemove(FTimerNode* node) {
    node->Prev->Next = node->Next;
    node->Next->Prev = node->Prev;
    node->Prev = node;
    node->Next = node;
}

// ---------------------------------------------------------------------------
// pub::Controller::_SendMessage's dispatch core — baseline 0x06d44080.

// This is what makes every `Server_SendMessage(id, data)` call in Server.dll mean
// something. baseline's the routine (which the port exported from Common.dll as
// Server_SendMessage) is only a two-line forwarder:
// (id, data) { uint zero = 0; pub::Controller::_SendMessage(zero, id, data); }
// and _SendMessage itself (0x06d44080) does:
//     if ((Flid & 0x3fffffff) == 0)   // broadcast
//         for (node : controller list)  node->InterfacePtr->vtable[0](MsgId, MsgData);
//     else { node = lookup(Flid); if (!node) return -2;
//            node->InterfacePtr->vtable[0](MsgId, MsgData); }
// The broadcast arm bumps the re-entrancy guard (the field) around the walk and,
// when it drops back to zero, drains the deferred-create queue — exactly what
// Server_ControllerHeartbeat already does here.

// The record's interface is what the controller DLL's CreateInstance returned:
// Content.dll's IDLL (src/include/content.h; Content exports ??_7IDLL@@6B@). Slot 0,
// IDLL::HandleMessage (thiscall, int-sized pub::Controller::EMessage MsgId, const void*
// MsgData, RET 8), is the only
// entry point Server.dll calls on it; teardown goes through DestroyInstance, never
// through slot 1 (the destructor).

static void Server_ControllerDrainDeferred();

// (module storage .. module storage), cdecl (const uint* Flid): walk the
// ACTIVE list (the field) for a node whose +0x18 equals *Flid and return &node+8
// (the record); if the active list has none, walk the DEFERRED-create list
// (the field) the same way; NULL when neither has it. A controller created while
// a broadcast is in flight is therefore already addressable before the drain moves
// it over. Callers: _SendMessage's targeted arm (module storage),
// SetHeartbeatInterval (module storage) and the routine (module storage).
// The port's node pointer stands in for baseline's &node+8.
static FControllerNode* Server_ControllerFind(unsigned int Flid) {
    FControllerNode* Node = g_ControllerListSentinel.Next;
    while (Node != &g_ControllerListSentinel) {
        if (Node->FLID == Flid) return Node;
        Node = Node->Next;
    }
    Node = g_DeferredListSentinel.Next;
    while (Node != &g_DeferredListSentinel) {
        if (Node->FLID == Flid) return Node;
        Node = Node->Next;
    }
    return nullptr;
}

// (module storage .. module storage), cdecl (const uint* Flid):
// `return the routine(Flid) != NULL;` -- is this Flid a live or pending controller.
// Sole caller: pub::SpaceObj::Create (module storage), passing &ShipInfo, whose
// first dword is therefore the controller Flid.
extern "C" bool __cdecl Server_ControllerExists(const unsigned int& Flid) {
    return Server_ControllerFind(Flid) != nullptr;
}

// RefreshLoadingProgress() first (module storage), on both arms.
// Neither arm tests the record's interface pointer before `CALL [EDX]`
// (module storage..06d440bf broadcast, module storage..06d44163 targeted).
extern "C" int __cdecl Server_ControllerDispatch(unsigned int Flid, pub::Controller::EMessage MsgId,
                                                 const void* MsgData) {
    RefreshLoadingProgress();
    if ((Flid & 0x3fffffff) == 0) {
        // Broadcast arm (baseline 0x06d4409c-0x06d440cd).
        g_ReentrancyGuard++;
        FControllerNode* cur = g_ControllerListSentinel.Next;
        while (cur != &g_ControllerListSentinel) {
            cur->InterfacePtr->HandleMessage(MsgId, MsgData);
            cur = cur->Next;
        }
        g_ReentrancyGuard--;
        if (g_ReentrancyGuard == 0)
            Server_ControllerDrainDeferred();
        return 0;
    }
    // Targeted arm (baseline 0x06d44148-0x06d4416c).
    FControllerNode* node = Server_ControllerFind(Flid);
    if (node == nullptr) return -2;
    node->InterfacePtr->HandleMessage(MsgId, MsgData);
    return 0;
}

// pub::Controller::SetHeartbeatInterval's body — baseline 0x06d44030. the routine(&Flid)
// finds the node (payload pointer = node+8) and the whole body is then
//     payload[+0x8] = Interval;                       // node+0x10 -- the period/gate
//     payload[+0xc] = rand() * f * Interval; // node+0x14 -- initial accumulator
//     return 0;                                        // -2 when not found
// f is 1/32768; the product is a random fraction of one period, so freshly
// configured controllers do not all fire on the same frame. The heartbeat walk
// (the routine) gates on node+0x10 >= 0.0 (FLD [ESI+0x10] / FCOMP 0.0 at 0x06d43735).
// NOTE: this port's FControllerNode names the two the other way round -- HeartbeatTimer
// (+0x14) is what Server_ControllerHeartbeat uses as the period/gate and HeartbeatAccum
// (+0x10) as the accumulator. The node is private to Server.dll, so the swap is
// harmless, but the assignment below has to follow the PORT's convention, not baseline's
// offsets, or the existing tick would never fire.
extern "C" int __cdecl Server_ControllerSetHeartbeat(unsigned int Flid, float Interval) {
    FControllerNode* node = Server_ControllerFind(Flid);   // the routine
    if (node == nullptr) return -2;
    node->HeartbeatTimer = Interval;
    node->HeartbeatAccum = (float)rand() * (1.0f / 32768.0f) * Interval;
    return 0;
}

// Oracle instrumentation (P2.3 sim, the reproducibility work, owner 2026-11-19): every
// controller's heartbeat as it stands, in list order (the active list, then the deferred
// one), for session.jsonl's `heartbeats` record. A new export; no game module calls it.
typedef void(__cdecl* FOracleSimHeartbeatFn)(void* Context, unsigned int Flid, unsigned int Priority,
                                              char const* Module, float Interval, float Phase,
                                              bool bDeferred);
extern "C" SERVER_API void __cdecl OracleSimControllerHeartbeats(FOracleSimHeartbeatFn Fn, void* Context) {
    if (Fn == nullptr) return;
    FControllerNode* const Sentinels[2] = { &g_ControllerListSentinel, &g_DeferredListSentinel };
    for (int List = 0; List < 2; ++List) {
        for (FControllerNode* Node = Sentinels[List]->Next; Node != Sentinels[List]; Node = Node->Next) {
            char Path[MAX_PATH] = {};
            if (Node->ModuleHandle != nullptr)
                GetModuleFileNameA(Node->ModuleHandle, Path, MAX_PATH);
            char const* const Slash = strrchr(Path, '\\');
            Fn(Context, Node->FLID, Node->Priority, Slash != nullptr ? Slash + 1 : Path, Node->HeartbeatTimer,
               Node->HeartbeatAccum, List == 1);
        }
    }
}

// Oracle instrumentation (P3.1, the controller_bus suite, decision record 0006 point 10):
// the two steps of ElapseTime that drive the registry, the heartbeat walk with its teardown
// and then timer firing, and the registry's part of Shutdown, for Oracle.exe neuron-server,
// which runs the registry without a started server. New exports; no game module calls them.
extern "C" SERVER_API void __cdecl OracleBusTick(float Dt) {
    Server_TimerTick(Dt);
    Server_TimerFiring(Dt);
}

extern "C" SERVER_API void __cdecl OracleBusShutdown() {
    Server_ControllerCleanup();
}

// Oracle instrumentation (P3.2, the accounts kind of Oracle.exe dump): Startup's read of
// [OutpostFrontier] initial_world (g_InitialWorldPath) from a server INI, for a dump that
// runs the account flow without a started server. A new export; no game module calls it.
extern "C" SERVER_API void __cdecl OracleReadWorldIni(char const* IniPath) {
    Server_ReadWorldIni(IniPath);
}

// pub::SetTimer's body — baseline 0x06d5ee00. Allocates a 0x18-byte node, links it at the
// END of the timer list (list::push_back: node->next = head, node->prev = head->prev,
// ) and bumps the count. baseline checks ONLY Timeout < 0 (-4); it
// does not validate the Flid, and it does not check the allocation either (operator new
// at 0x06d5ee4f is followed straight by the store [EAX],EBX at 0x06d5ee59).
extern "C" int __cdecl Server_ControllerSetTimer(unsigned int Flid, unsigned int Param1,
                                                 unsigned int Param2, float Timeout) {
    if (Timeout < 0.0f) return -4;
    FTimerNode* node = (FTimerNode*)malloc(sizeof(FTimerNode));
    node->TargetController = Flid;
    node->Param1 = Param1;
    node->Param2 = Param2;
    node->RemainingTime = Timeout;
    TimerListInsertBefore(&g_TimerListSentinel, node);
    g_TimerCount++;
    return 0;
}

// pub::Controller::Create -- baseline 0x06d43ce0 (ordinal 85), cdecl
// (DllPath, InstanceName, Parms, Priority).
// Flid = CFLIDMaker::GetInstance()->next id (inlined: [EAX+4] & 0x3fffffff,
//             [EAX+4]++); Flid == 0 -> return 0
//   record    {Interface, Module, +0x10 = -1.0f, +0x14 = 0, Flid, Priority}
// / 06d43e41  Module = pub::DLL_LoadLibrary(DllPath); NULL -> FDUMP
//             0x100001 "failed create DLL instance" (dlls.cpp, 0xa7) and carry on
//   guard the field != 0: append the record to the DEFERRED list (06d43d85);
//   guard == 0: insert it into the ACTIVE list before the first node whose priority
//             is >= Priority (signed JGE, 06d43e89) (06d43eb7)
//   then RefreshLoadingProgress, GetModuleFileNameA (result unused),
//   Interface = GetProcAddress(Module,"CreateInstance")(Flid, Name, Parms);
//   non-NULL -> return Flid.  NULL -> GetProcAddress(Module,"DestroyInstance")(Interface),
//   GetModuleFileNameA, FreeLibrary(Module), erase the record from the list it went
//   into (06d43e24 / 06d43f4f), return 0.
// Neither GetProcAddress result is tested before the call.
unsigned int Server_ControllerCreate(const char* dll_path, const char* instance_name,
                                             void* parms, unsigned int priority) {
    unsigned int flid = CFLIDMaker::GetInstance()->AllocateObjectID();
    if (flid == 0) return 0;

    HMODULE hMod = pub::DLL_LoadLibrary(dll_path);
    if (hMod == nullptr) {
        FDUMP(0x100001, "%s(%d) : *** ERROR: failed create DLL instance '%s' (make sure file exists)\n",
              "Server\\dlls.cpp", 0xa7, dll_path);
    }

    FControllerNode* node = (FControllerNode*)malloc(sizeof(FControllerNode));
    node->InterfacePtr = nullptr;
    node->ModuleHandle = hMod;
    node->HeartbeatAccum = 0.0f;
    node->HeartbeatTimer = -1.0f;
    node->FLID = flid;
    node->Priority = priority;

    const bool bDeferred = (g_ReentrancyGuard != 0);
    if (bDeferred) {
        ListInsertBefore(&g_DeferredListSentinel, node);
        g_DeferredCount++;
    } else {
        FControllerNode* cur = g_ControllerListSentinel.Next;
        while (cur != &g_ControllerListSentinel) {
            if (static_cast<int>(cur->Priority) >= static_cast<int>(priority)) break;
            cur = cur->Next;
        }
        ListInsertBefore(cur, node);
        g_ControllerCount++;
    }

    RefreshLoadingProgress();
    char ModuleName[0x104];
    GetModuleFileNameA(node->ModuleHandle, ModuleName, 0x104);
    FControllerCreateInstanceFn CreateInstance =
        (FControllerCreateInstanceFn)GetProcAddress(node->ModuleHandle, "CreateInstance");
    node->InterfacePtr = CreateInstance(flid, instance_name, parms);
    if (node->InterfacePtr != nullptr) return flid;

    FControllerDestroyInstanceFn DestroyInstance =
        (FControllerDestroyInstanceFn)GetProcAddress(node->ModuleHandle, "DestroyInstance");
    DestroyInstance(node->InterfacePtr);
    GetModuleFileNameA(node->ModuleHandle, ModuleName, 0x104);
    FreeLibrary(node->ModuleHandle);
    ListRemove(node);
    if (bDeferred) g_DeferredCount--;
    else g_ControllerCount--;
    free(node);
    return 0;
}

// pub::Controller::Destroy -- baseline 0x06d43f70 (ordinal 103), cdecl (Flid).
// if the Flid is already queued (the field walk, entry->node+0x18), return
// walk the ACTIVE list; for every node with that Flid: append the node
//             pointer to the destroy queue (the routine / the routine), bump
// , then _SendMessage(0 = broadcast, 0x0c, &Flid copy)
// Nothing is unlinked or unloaded here -- that happens at the top of the next
// heartbeat walk (the routine), so a broadcast in progress never sees a freed node.
// The deferred-create list is not searched.
void Server_ControllerDestroy(unsigned int flid) {
    for (std::list<FControllerNode*>::iterator It = g_DestroyPending.begin();
         It != g_DestroyPending.end(); ++It) {
        if ((*It)->FLID == flid) return;
    }
    FControllerNode* cur = g_ControllerListSentinel.Next;
    while (cur != &g_ControllerListSentinel) {
        if (cur->FLID == flid) {
            g_DestroyPending.push_back(cur);
            unsigned int DestroyedFlid = cur->FLID;
            Server_ControllerDispatch(0, pub::Controller::EMessage::ControllerDestroyed, &DestroyedFlid);
        }
        cur = cur->Next;
    }
}

// SysDB — per-system state storage
// baseline: std::map<unsigned int, StarSystem> SysDB::SysMap (red-black tree)
// iterates Universe systems, constructs FStarSystem for each
// looks up by system_id via map::lower_bound
// The table itself is the exported SysDB::SysMap (server_sysdb.h / server_classes.cpp).
static_assert(sizeof(FStarSystem) == 0x100, "FStarSystem must stay 0x100 — SysDB memset/map");
// No "already initialised" latch: baseline the routine has none, and its only
// caller the routine (Server_PlayerSubsystemInit) runs once per server startup.

// — lookup system in SysMap by ID
FStarSystem* SysDB_LookupSystem(unsigned int system_id) {
    if ((system_id & 0x3fffffff) == 0) return nullptr;
    SysDB::SysMapType::iterator it = SysDB::SysMap.find(system_id);
    if (it != SysDB::SysMap.end()) return &it->second;
    return nullptr;
}

// (module storage) -- SysDB initialization. For every Universe
// system (Universe::GetFirstSystem / GetNextSystem):
//   id = sys->vtable[0]()                         (NicknameId in this port)
//   StarSystem Temp;                              the routine, module storage
//   pair<unsigned const, StarSystem> P(id, Temp); the routine, module storage
//   It = SysMap.insert(P).first;                  the routine (node: the routine)
//   ~P, ~Temp
//   It->second.init(sys->vtable[0]())             the routine, module storage
// The map holds the COPY, so every entry's ObjectLists have bUseHeap = 0
// (server_sysdb.h, ObjectList).
void SysDB_Initialize() {

    Universe::ISystem* sys = Universe::GetFirstSystem();
    while (sys != nullptr) {
        unsigned int const SysId = sys->NicknameId;
        StarSystem Temp;
        std::pair<SysDB::SysMapType::iterator, bool> const Inserted =
            SysDB::SysMap.insert(SysDB::SysMapType::value_type(SysId, Temp));
        Inserted.first->second.init(sys->NicknameId);

        sys = Universe::GetNextSystem();
    }
}

// (module storage .. 06d10318) -- SysDB teardown, called only by
// (module storage, this port's Server_CleanupPhysics). (the routine,
// the name this used to carry, is SysMap's atexit destructor, not this.)
// in-order walk of SysMap: every StarSystem whose +0xfc (bLoaded) is set
//             gets StarSystem::shutdown (the routine, CALL at module storage)
// then SysMap.clear() -- ~StarSystem (the routine) per node.
// ~StarSystem is traced against the routine (server_classes.cpp).
// is std::map::erase(begin(), end()) inlined: the whole-tree
// fast path frees every node through the routine / the routine (the pair's
// scalar deleting dtor, flags 0 -> ~pair -> ~StarSystem) and resets the head;
// the other path erases node by node (the routine). SysMap.clear() is that.
// No logging in baseline (the port's OutputDebugString was removed 2026-09-24).
void SysDB_Shutdown() {
    for (SysDB::SysMapType::iterator It = SysDB::SysMap.begin();
         It != SysDB::SysMap.end(); ++It) {
        if (It->second.bLoaded != 0)
            It->second.shutdown();
    }
    SysDB::SysMap.clear();
}

// Mark system as loaded/active
void SysDB_SetSystemLoaded(unsigned int system_id, bool loaded) {
    FStarSystem* sys = SysDB_LookupSystem(system_id);
    if (sys) sys->bLoaded = loaded ? 1 : 0;
}

// see server_sysdb.h. Initial value 1 (module storage).
unsigned char GbAllowPvp = 1;

// 's one-entry lookup cache -- see server_sysdb.h.
// / the routine are this watcher's CRT constructor and atexit
// destructor: the routine(0) zeroes both dwords (= BaseWatcher()), and
// is `if (Watched) set_pointer(0)` (= ~BaseWatcher, whose
// set(nullptr) is a no-op while nothing is watched).
BaseWatcher GLookupCacheObject;
StarSystem* GLookupCacheSystem = nullptr;
unsigned int GLookupCacheHits = 0;
unsigned int GLookupCacheMisses = 0;

// (module storage, cdecl, called only from IServerImpl::Startup at
// with (info +0x208, (byte)info +0x210)):
// the field = (byte)param_2
// CALL the routine(param_1)  -- SysDB init; the pushed
//                   argument is never read by the callee (SysDB_Initialize)
// JMP the routine            -- Server fuse DB load
static void Server_PlayerSubsystemInit(unsigned int max_players, unsigned int allow_pvp) {
    (void)max_players;
    GbAllowPvp = static_cast<unsigned char>(allow_pvp);
    SysDB_Initialize();
    ServerFuseDB::LoadAll();
}

// (module storage, called only by IServerImpl::Shutdown). baseline:
// CALL the routine   -- Server fuse DB teardown
// CALL the routine   -- SysDB teardown (SysDB_Shutdown)
// the field = 0    -- the StarSystem half of the routine's
//                   one-entry object-lookup cache (GLookupCacheSystem; the
//                   watcher half, the field, is left as it is)
// Corrected 2026-09-24: the port also cleared two flags baseline does not touch
// here (a vestigial init flag and the field, the phantom-update latch); both
// were otherwise unused and are gone. The OutputDebugString the port used to
// emit here has no baseline counterpart and is gone too.
static void Server_CleanupPhysics() {
    ServerFuseDB::Clear();
    SysDB_Shutdown();
    GLookupCacheSystem = nullptr;
}

// 's payload for controller event 0x27, built on the stack at
// .. module storage (land) and module storage .. module storage
// (dock without landing): the object's get_id(), the entry's base id
// (`[EBX+0x18]`, node+0x18 == FSystemDockEntry::BaseId), the target id computed
// at module storage (or 0) and one byte -- 1 for land, 0 otherwise. The three
// bytes after the flag are never written.
struct FDockEventPayload {
    unsigned int ObjectId;    // +0x00
    unsigned int BaseId;      // +0x04
    unsigned int TargetId;    // +0x08
    bool bLand;               // +0x0C
};

// 's tail, module storage .. module storage (after the per-player
// view update): consume Common's changed-affiliation map. baseline:
//   Map = Reputation::GetChangedAffiliationClientMap();   // null while empty
//   if (!Map) return;
//   for (Client, OldAffiliation) in *Map:       // OldAffiliation is never read
//     Ship = Players[Client].ship_object_id               (module storage, slot+0x3e4)
//     Aff = 0xffffffff;
//     Reputation::Vibe::GetAffiliation(Players[Client].rep_vibe, Aff, false)
//                                                          (module storage, slot+0x318)
//     if (Ship != 0):
//       Obj = the routine(&Ship) ? object : 0             (module storage .. 06d0c57c)
//       for each player view of THIS system:              (module storage .. 06d0c5c2)
//         if (the routine(view, Obj))                     // SystemPlayerView_IsObjectVisible
//           IClient +0x1d0 (SendServerMiscObjUpdateAffiliation)(view->PlayerId, Ship, Aff)
//     else:
//       IClient +0x1d0 (SendServerMiscObjUpdateAffiliation)(Client, 0, Aff)
//                                                          (module storage .. 06d0c5da)
//   Map->erase(begin, end)                                 (module storage .. 06d0c68a)
// The map is Common's single global, so the first system updated in a frame
// consumes it and the others see null, exactly as in baseline.
static void StarSystem_SendChangedAffiliations(StarSystem* Sys)
{
    std::map<unsigned int, unsigned int>* const Changed =
        ::Reputation::GetChangedAffiliationClientMap();
    if (Changed == nullptr)
        return;

    std::map<unsigned int, unsigned int>::const_iterator It = Changed->begin();
    for (; It != Changed->end(); ++It)
    {
        unsigned int const Client = It->first;
        PlayerData& Slot = Players[Client];
        unsigned int const Ship = Slot.ship_object_id;
        int const Vibe = Slot.rep_vibe;
        unsigned int Affiliation = 0xffffffffu;
        ::Reputation::Vibe::GetAffiliation(Vibe, Affiliation, false);

        if (Ship != 0)
        {
            GameObject* const Obj = Server_LookupGameObject(Ship);
            SystemPlayerNode* const Head = static_cast<SystemPlayerNode*>(Sys->PlayerListHead);
            for (SystemPlayerNode* Node = Head->Next; Node != Head; Node = Node->Next)
            {
                if (SystemPlayerView_IsObjectVisible(&Node->View, Obj))
                    g_pClientInterface->SendServerMiscObjUpdateAffiliation(Node->View.PlayerId, Ship, Affiliation);
            }
        }
        else
        {
            g_pClientInterface->SendServerMiscObjUpdateAffiliation(Client, 0, Affiliation);
        }
    }

    Changed->clear();
}

// 's ship-proximity pass, module storage .. module storage, run
// once per step between the +0x70 tick and the +0x48 tick:
// Bucket = this->vtable[3](0xfdff)       -- StarSystem::GetBucket
// null -> skip the whole pass (module storage)
// a stack scan list {Capacity 0x800 at +0x94, Count at +0x98,
//             Items[0x800] at +0x9c}
// for every player view (the end test re-reads
//             [this+0xc] each pass):
//     ship = Players[view.PlayerId].ship_object_id    (node+0x40, slot+0x3e4)
// (&ship, &obj, &x); a failed lookup or a null obj -> next
//     sphere = {obj->vtable[0]() (get_position), the field = 10000.0f}
//     Bucket->vtable[+0x1c](&sphere, &list)          -- the BSP fill, thiscall
// then every non-null Data of the block at +0xd4
//             (head [this+0xd8]) is appended while Count < Capacity
// Count > 1 (unsigned): de-duplicate in place -- for
//             each non-null item, the FIRST later duplicate is zeroed, and the
//             item is compacted down; zeros are dropped; Count = kept
// for each kept item: __RTDynamicCast(item, IObjRW ->
//             GameObject); non-null -> vtable +0x16c (update) with the step;
//             the result is ignored
// is Server_LookupGameObject in this port (the same stand-in
// server_observer.cpp uses for the same slot read).
static void StarSystem_UpdateNearShips(StarSystem* Sys, float Step)
{
    void* const Bucket = Sys->GetBucket(0xfdffu);
    if (Bucket == nullptr)
        return;

    struct FScanList {
        unsigned int Capacity;          // ESP+0x94
        unsigned int Count;             // ESP+0x98
        void* Items[0x800];             // ESP+0x9c
    };
    struct FScanSphere {
        Vector Center;                  // ESP+0x68
        float Radius;                   // ESP+0x74
    };
    FScanList Scan;                     // baseline's own 0x20a0-byte frame holds it
    Scan.Capacity = 0x800;
    Scan.Count = 0;

    // ISpatialPartition +0x1c (sphere query); FScanSphere / FScanList have the layouts of
    // Geometry::Sphere and FSpatialQueryResult.
    ISpatialPartition* const Partition = static_cast<ISpatialPartition*>(Bucket);

    for (SystemPlayerNode* Node = static_cast<SystemPlayerNode*>(Sys->PlayerListHead)->Next;
         Node != static_cast<SystemPlayerNode*>(Sys->PlayerListHead);
         Node = Node->Next)
    {
        unsigned int const ShipId = Players[Node->View.PlayerId].ship_object_id;
        GameObject* const Ship = Server_LookupGameObject(ShipId);
        if (Ship == nullptr)
            continue;
        FScanSphere Sphere;
        Sphere.Radius = 10000.0f;       // = 0x461c4000
        Sphere.Center = Ship->get_position();
        Partition->QuerySphere(reinterpret_cast<Geometry::Sphere const*>(&Sphere),
                               reinterpret_cast<FSpatialQueryResult*>(&Scan));
    }

    FSystemListNode* It = nullptr;
    while (Sys->ListD4.traverse(It, true))
    {
        if (It->Data == nullptr)
            continue;
        if (Scan.Count < Scan.Capacity)
        {
            Scan.Items[Scan.Count] = static_cast<IObjRW*>(It->Data);
            ++Scan.Count;
        }
    }

    if (Scan.Count > 1)
    {
        int const Count = static_cast<int>(Scan.Count);
        int Kept = 0;
        for (int Index = 0; Index < Count; ++Index)
        {
            void* const Item = Scan.Items[Index];
            if (Item == nullptr)
                continue;
            for (int Later = Index + 1; Later < Count; ++Later)
            {
                if (Scan.Items[Later] == Item)
                {
                    Scan.Items[Later] = nullptr;
                    break;
                }
            }
            Scan.Items[Kept] = Scan.Items[Index];
            ++Kept;
        }
        Scan.Count = static_cast<unsigned int>(Kept);
    }

    for (unsigned int Index = 0; Index < Scan.Count; ++Index)
    {
        GameObject* const Obj =
            dynamic_cast<GameObject*>(static_cast<IObjRW*>(Scan.Items[Index]));
        if (Obj != nullptr)
            Obj->update(Step);
    }
}

// ---------------------------------------------------------------------------
// (module storage, thiscall on the StarSystem, `RET 0x10`; the
// name is the port's). One side of a PhySys collision -> the GameObject wrapper
// and the sub-object it was hit on. Only caller: StarSystem_ProcessCollisions
// below (module storage side A, module storage side B).
// ---------------------------------------------------------------------------
static void StarSystem_ResolveCollisionSide(StarSystem* Sys, CObject* Part, unsigned int PartId,
                                            GameObject** OutObj, unsigned short* OutSubObjId)
{
    *OutObj = nullptr;
    *OutSubObjId = SUBOBJ_ID_NONE;

    // the wrapper of the part's root object.
    GameObject* Found = nullptr;
    CObject* const Root = GetRoot(Part);
    if (Root != nullptr)
    {
        unsigned int const Class = static_cast<unsigned int>(Root->ObjectClass);
        if ((Class & 3u) == 3u)
        {
            if ((Class & 0x67u) != 0x67u)
            {
                // the routine on the CSimple id.
                Found = Sys->find_object(static_cast<CSimple*>(Root)->Id);
            }
            else
            {
                // a beam: the block at +0x84 (head
                // +0x88), node->Data read without a null test, the first whose
                // cobject() (vt +0x150) is the collided part itself (not the root).
                FSystemListNode* Node = nullptr;
                while (Sys->Beams.traverse(Node, true))
                {
                    GameObject* const Data = Node->Data;
                    if (Data->cobject() == Part)
                    {
                        Found = Data;
                        break;
                    }
                }
            }
        }
    }

    *OutObj = Found;
    if (Found == nullptr)
        return;

    // the wrapper's CObject (CBase +0x10) under the
    // CEqObj mask; anything else was hit on its root.
    CObject* const Obj = Found->Object;
    if (Obj == nullptr || (static_cast<unsigned int>(Obj->ObjectClass) & 0x103u) != 0x103u)
    {
        *OutSubObjId = SUBOBJ_ID_ROOT;
        return;
    }

    // CObject::part_to_inst inlined on the PART,
    // with -1 (no map entry, or an entry of -1) falling back to its own engine
    // instance (+0x04).
    long Inst = Part->part_to_inst(PartId);
    if (Inst == -1)
        Inst = Part->EngineInstanceId;

    // CEqObj::inst_to_subobj_id (vt +0xf4).
    CEqObj* const Eq = static_cast<CEqObj*>(Obj);
    unsigned short const SubObjId = Eq->inst_to_subobj_id(Inst);
    *OutSubObjId = SubObjId;

    // already a shield: done.
    if (SubObjectID::IsShieldEquipID(SubObjId))
        return;

    // a functioning shield (IsFunctioning, CEquip
    // vt +0xc) takes the hit instead: its id (+0x08).
    CEShield* const Shield = CEShield::cast(Eq->EquipManager.FindFirst(0x100));
    if (Shield == nullptr)
        return;
    if (!Shield->IsFunctioning())
        return;
    *OutSubObjId = Shield->Id;
}

// ---------------------------------------------------------------------------
// (module storage, thiscall on the StarSystem, plain RET; the name
// is the port's). The system's collision pass: PhySys::GenerateCollisions once
// per node of the block at +0x84, then every event PhySys::GetCollisions
// returns for this system is resolved on both sides and delivered to both
// wrappers' on_collision (vt +0x1a4). Only caller: the multiplayer tail of
// StarSystem_Update (module storage).
// ---------------------------------------------------------------------------
static void StarSystem_ProcessCollisions(StarSystem* Sys)
{
    // the beam block; a node whose Data, CObject
    // or class (0x67) does not make a CBeam passes null.
    FSystemListNode* Node = nullptr;
    while (Sys->Beams.traverse(Node, true))
    {
        CBeam* Beam = nullptr;
        GameObject* const Data = Node->Data;
        if (Data != nullptr)
        {
            CObject* const BeamObject = Data->Object;
            if (BeamObject != nullptr &&
                (static_cast<unsigned int>(BeamObject->ObjectClass) & 0x67u) == 0x67u)
                Beam = static_cast<CBeam*>(BeamObject);
        }
        PhySys::GenerateCollisions(Beam);
    }

    // the routine is the system's nickname id.
    PhySys::CollisionEvent const* Begin = nullptr;
    PhySys::CollisionEvent const* End = nullptr;
    PhySys::GetCollisions(Sys->SystemInfo->NicknameId, Begin, End);

    // the next element is taken (+0x34) before
    // the dispatch, the end compared after it.
    for (PhySys::CollisionEvent const* Event = Begin; Event != End; ++Event)
    {
        GameObject* ObjA = nullptr;
        unsigned short SubA = 0;
        StarSystem_ResolveCollisionSide(Sys, Event->ObjectA, Event->PartIdA, &ObjA, &SubA);
        if (ObjA == nullptr)
            continue;

        GameObject* ObjB = nullptr;
        unsigned short SubB = 0;
        StarSystem_ResolveCollisionSide(Sys, Event->ObjectB, Event->PartIdB, &ObjB, &SubB);
        if (ObjB == nullptr)
            continue;

        // side A's record at [ESP+0x40]. The two
        // pad words are never written by baseline (stack contents); the port
        // stores 0.
        FCollisionEvent HitA;
        HitA.SubObjId = SubA;
        HitA.Pad02 = 0;
        HitA.Other = ObjB;
        HitA.OtherSubObjId = SubB;
        HitA.Pad0a = 0;
        HitA.Point = Event->Point;                                   // event +0x10
        HitA.Field_0x18 = Event->SurfaceNormal;                         // event +0x1c
        HitA.Field_0x24 = Event->RelativeVelocity;                         // event +0x28
        ObjA->on_collision(HitA);

        // side B's record at [ESP+0x70]: the
        // partners swapped, the point shared, both vectors negated (FLD / FCHS
        // / FSTP dword per component).
        FCollisionEvent HitB;
        HitB.SubObjId = SubB;
        HitB.Pad02 = 0;
        HitB.Other = ObjA;
        HitB.OtherSubObjId = SubA;
        HitB.Pad0a = 0;
        HitB.Point = Event->Point;
        HitB.Field_0x18.x = -Event->SurfaceNormal.x;
        HitB.Field_0x18.y = -Event->SurfaceNormal.y;
        HitB.Field_0x18.z = -Event->SurfaceNormal.z;
        HitB.Field_0x24.x = -Event->RelativeVelocity.x;
        HitB.Field_0x24.y = -Event->RelativeVelocity.y;
        HitB.Field_0x24.z = -Event->RelativeVelocity.z;
        ObjB->on_collision(HitB);
    }
}

// ---------------------------------------------------------------------------
// StarSystem's update, thiscall with one float (RET 4). baseline
// has no name for it; the port calls it StarSystem_Update and keeps it a
// file-scope function because everything it calls lives here.

// Outer loop (module storage .. module storage):
//   Target = (double)dt (module storage / 06d0be77, before the clamp);
//   Step = dt; MP only: if ((double)dt > 0.2 (the field, double
// 9999999a; FCOMP / TEST AH,0x41)) Step = 0.2f (0x3e4ccccd, written
//   back into the argument slot);
//   Accum = 0; if (!(0.0 < Target)) skip (the field = 0.0, TEST AH,5 / JP --
//   NaN skips too);
//   do {
//     Accum = (double)Step + Accum;     // FLD float, 06d0bee0
//                                       // FADD double ptr, 06d0beef FSTP double
//     …work with Step…
//     if (Target - Accum < Step) Step = (float)(Target - Accum);   // ..b1
//   } while (Accum < Target);                                     // ..c2

// Why baseline needs no iter_cap / zero_step guard (listing + client clamp):
//   - OutpostFrontier.exe the field = 1.0f caps ElapseTime dt at 1s (never rewritten).
//   - SP: Step == dt == Target → exactly one body iteration when Target > 0.
//   - MP: Step ≤ 0.2 → at most ceil(1.0/0.2) = 5 full steps. When Accum is just
//     short of Target, Target - Accum is at least one ulp of Accum, and its float
//     rounding is still more than half an ulp, so the next double add always
//     moves Accum forward -- the loop cannot stall.
// A port that sees iters>>5 or a hang has an upstream bad dt/Step, not a missing cap.

// Callers: the routine (the per-frame walk, Server_PhysicsUpdate below) and
// IServerImpl::Dock (module storage, with 0.2f).

// Returns the number of step iterations. That is the port's own diagnostic
// for the first-frame log line; baseline returns nothing.

// (module storage, the SrvAsteroid field update) is called, and
// the server asteroid objects it spawns (SrvAsteroid::MedAsteroid/MedMine,
// server_classes.cpp) exist. The ship-proximity pass (module storage ..
// , StarSystem_UpdateNearShips), CmnAsteroid::Update
// (module storage), the multiplayer beam-move pass (module storage ..
// ) and the collision pass the routine (module storage,
// StarSystem_ProcessCollisions) are ported.
// The Reputation walk after the loop (module storage on) is
// StarSystem_SendChangedAffiliations.
// ---------------------------------------------------------------------------
static unsigned int StarSystem_Update(StarSystem* Sys, float Dt)
{

    float Step = Dt;
    if (!SinglePlayer() && static_cast<double>(Step) > 0.2)
        Step = 0.2f;

    // baseline: param_9 = (double)original_dt; loop gated by 0.0 < param_9.
    const double Target = static_cast<double>(Dt);
    unsigned int Iterations = 0;

    if (Target > 0.0)
    {
        double Accum = 0.0;
        float CurStep = Step;
        do
        {
            // FLD float [step] / module storage FADD double
            // ptr [Accum] / module storage FSTP double -- a double add.
            Accum = static_cast<double>(CurStep) + Accum;
            ++Iterations;

            // step body, module storage .. module storage.
            // Every the routine call pushes the block at +0xe8 as the deferred
            // list (ESI, module storage / module storage) and the step (EDI).
            // SP never calls PhySys::Update here (module storage gate).
            ObjectList* const Deferred = &Sys->Deferred;
            StarSystem_TickObjectList(&Sys->Ships, CurStep, Deferred);
            StarSystem_TickObjectList(&Sys->List70, CurStep, Deferred);

            // .. module storage -- the ship-proximity pass.
            StarSystem_UpdateNearShips(Sys, CurStep);

            StarSystem_TickObjectList(&Sys->Loot, CurStep, Deferred);
            StarSystem_TickObjectList(&Sys->Beams, CurStep, Deferred);
            StarSystem_TickObjectList(&Sys->List98, CurStep, Deferred);
            StarSystem_TickObjectList(&Sys->ListAC, CurStep, Deferred);

            // .. module storage -- COMMON.DLL::CmnAsteroid::Update
            // (import module storage), cdecl (this, step): StarSystem is the
            // IObjDB it walks.
            CmnAsteroid::Update(Sys, CurStep);

            // .. module storage the routine(this) -- looks this
            // system's id up (IObjDB slot 2, then the ISystem's vtable[0]) in
            // the SrvAsteroid::SrvAsteroidSystem map at the field and runs
            // on each of its fields (src/Server/server_classes.cpp).
            SrvAsteroid::SrvAsteroid_UpdateSystem(Sys);

            StarSystem_TickObjectList(&Sys->ListC0, CurStep, Deferred);

            // (a) module storage .. module storage -- drain the block at +0xe8,
            // which the passes above fill with the objects whose update()
            // answered false. baseline:
            //   it = 0;
            //   while (the routine(block, &it, true)) {    // MetaList::traverse
            //     if (it->Data) it->Data's CBase sub-object, vtable slot 0, flag 1;
            //     it->Data = 0;                             //
            // (block, &it);                 // MetaList::remove
            //   }
            // The node removed is always the head, so `it` comes back null and
            // the next traverse starts from the head again.
            {
                FSystemListNode* It = nullptr;
                while (Deferred->traverse(It, true))
                {
                    GameObject* const Data = It->Data;
                    if (Data != nullptr)
                    {
                        // `MOV EDX,[EAX+0xc] / LEA ECX,[EAX+0xc] / PUSH 1 /
                        // CALL [EDX]` -- the deleting destructor through the
                        // CBase sub-object.
                        delete static_cast<CBase*>(Data);
                    }
                    It->Data = nullptr;
                    Deferred->remove(It);
                }
            }

            // (b) module storage .. module storage -- the dock queue at +0x14
            // (server_sysdb.h, SystemDockNode). Skipped outright while it is
            // empty (module storage `MOV EAX,[EBX+0x1c]` / `JZ the branch`).
            if (Sys->ObjectListSize != 0)
            {
                // .. module storage -- a local std::list of
                // player ids (the routine builds its sentinel). The players
                // whose ships land this step are collected into it.
                std::list<unsigned int> Landed;

                // .. module storage -- walk from the sentinel's
                // Next; the end test re-reads `[this+0x18]` every pass
                // (module storage .. module storage).
                SystemDockNode* Node = static_cast<SystemDockNode*>(Sys->ObjectListHead)->Next;
                while (Node != static_cast<SystemDockNode*>(Sys->ObjectListHead))
                {
                    // .. module storage -- both watchers back to
                    // their wrappers.
                    GameObject* const Obj = FSystemDockEntry::WatchedObject(Node->Entry.Object);
                    GameObject* const DockTarget = FSystemDockEntry::WatchedObject(Node->Entry.Target);

                    // .. module storage -- the target's id, but
                    // only when its CEqObj has no DockingBaseId (+0x160). The
                    // CEqObj comes from CBase::Object under the 0x103 class
                    // mask; baseline reads +0x160 through it without a null test
                    // (module storage XOR EAX,EAX falls straight into
                    // `MOV EDX,[EAX+0x160]`). Every producer
                    // passes a CEqObj-backed wrapper or no target at all.
                    unsigned int TargetId = 0;
                    if (DockTarget != nullptr)
                    {
                        CObject* const TargetObject = DockTarget->Object;
                        CEqObj* TargetEq = nullptr;
                        if (TargetObject != nullptr &&
                            (static_cast<unsigned int>(TargetObject->ObjectClass) & 0x103u) == 0x103u)
                            TargetEq = static_cast<CEqObj*>(TargetObject);
                        if (TargetEq->DockingBaseId == 0)
                            TargetId = DockTarget->get_id();
                    }

                    // MOV AL,[EBX+0x1c] -- the land byte.
                    if (Node->Entry.bLand)
                    {
                        // .. module storage -- every player in
                        // the system is told (FLPACKET_SERVER_LAND, and the
                        // multiplayer autosave of the owner's slot). ObjId is
                        // re-read through vtable +0x20 for each view
                        // (module storage); Arg3 is `LEA EBP,[EBX+0x18]`, the
                        // entry's BaseId, by address.
                        SystemPlayerNode* View = static_cast<SystemPlayerNode*>(Sys->PlayerListHead)->Next;
                        while (View != static_cast<SystemPlayerNode*>(Sys->PlayerListHead))
                        {
                            unsigned int ObjId = Obj->get_id();
                            SystemPlayerView_OnObjectLanded(&View->View, &ObjId, &TargetId,
                                                            &Node->Entry.BaseId);
                            View = View->Next;
                        }

                        // .. module storage -- event 0x27, land = 1.
                        FDockEventPayload Event;
                        Event.ObjectId = Obj->get_id();
                        Event.BaseId = Node->Entry.BaseId;
                        Event.TargetId = TargetId;
                        Event.bLand = true;
                        Server_ControllerDispatch(0, pub::Controller::EMessage::Msg0x27, &Event);

                        // .. module storage -- CSimple::OwnerPlayer
                        // (+0xb4) under the (ObjectClass & 3) == 3 guard, read
                        // without a null test.
                        unsigned int const Owner = Obj->csimple()->OwnerPlayer;
                        if (Owner == 0)
                        {
                            // .. module storage -- vtable +0x158
                            // with (0, 0): nobody owns it, it just goes.
                            Obj->destroy(0, 0);
                        }
                        else
                        {
                            // .. module storage -- the routine,
                            // std::list<unsigned int>::insert at the sentinel.
                            Landed.push_back(Owner);
                        }
                    }
                    else
                    {
                        // .. module storage -- event 0x27, land = 0.
                        FDockEventPayload Event;
                        Event.ObjectId = Obj->get_id();
                        Event.BaseId = Node->Entry.BaseId;
                        Event.TargetId = TargetId;
                        Event.bLand = false;
                        Server_ControllerDispatch(0, pub::Controller::EMessage::Msg0x27, &Event);
                    }

                    Node = Node->Next;
                }

                // .. module storage -- every collected player
                // leaves the system: StarSystem::remove_player (the routine),
                // the pending-visit flush on Players[id] (the routine, ECX =
                // `Players + id*0x418 - 0x418`) and, in single player only,
                // StarSystem::shutdown (the routine).
                for (std::list<unsigned int>::iterator It = Landed.begin(); It != Landed.end(); ++It)
                {
                    unsigned int const PlayerId = *It;
                    Sys->remove_player(PlayerId);
                    PlayerData_FlushPendingVisits(Players[PlayerId]);
                    if (SinglePlayer())
                        Sys->shutdown();
                }

                // .. module storage -- the routine empties the
                // local list; module storage .. module storage -- the routine
                // empties the dock queue, [begin, end).
                Landed.clear();
                SystemDockNode* const DockHead = static_cast<SystemDockNode*>(Sys->ObjectListHead);
                Sys->erase_docks(DockHead->Next, DockHead);
                // / module storage -- the second (empty) erase
                // and the sentinel's operator delete: Landed's destructor.
            }

            // .. module storage -- multiplayer only, and only
            // while the system has players (06d0c417 `CMP [EBX+0x10],ESI`,
            // ESI = 0, the player list's size).
            // SinglePlayer() is called again here (module storage), not reused
            // from the clamp.
            // Oracle instrumentation (P2.3 sim): or when Oracle.exe sim steps this system,
            // which has no player (StepSystemId is 0 outside the harness).
            if (!SinglePlayer() && (Sys->PlayerListSize != 0 || GOracleSimHooks.StepSystemId != 0u))
            {
                // .. module storage -- every node of the block at
                // +0x84 (Beams), walked with the routine(block, &it, 1):
                // it = it ? it->Next : block head (+0x88). A null Data
                // (06d0c43f), a null CObject (06d0c44f) or a class outside the
                // 0x67 mask (06d0c457) all zero ECX, and CBeam::move is STILL
                // called (06d0c45e / 06d0c45f) -- baseline has no test on the
                // result, so every object on this block is a beam there.
                FSystemListNode* BeamNode = nullptr;
                while (Sys->Beams.traverse(BeamNode, true))
                {
                    GameObject* const BeamWrapper = BeamNode->Data;
                    CObject* const BeamObject =
                        (BeamWrapper != nullptr) ? BeamWrapper->Object : nullptr;
                    CBeam* Beam = nullptr;
                    if (BeamObject != nullptr &&
                        (static_cast<unsigned int>(BeamObject->ObjectClass) & 0x67u) == 0x67u)
                        Beam = static_cast<CBeam*>(BeamObject);
                    Beam->move(CurStep);
                }

                // .. module storage
                PhySys::Update(Sys->SystemInfo->NicknameId, CurStep);

                // the routine(this), the
                // collision pass.
                StarSystem_ProcessCollisions(Sys);
            }

            if (Target - Accum < static_cast<double>(CurStep))
                CurStep = static_cast<float>(Target - Accum);
        } while (Accum < Target);
    }

    // after the step loop (or straight from the
    // skip): when the player list is non-empty ([this+0x10]), every
    // view gets the routine with the original dt as a double (EDI:EBP =
    // [ESP+0x48]:[ESP+0x44]); the end test re-reads the head [this+0xc].
    if (Sys->PlayerListSize != 0)
    {
        for (SystemPlayerNode* Node = static_cast<SystemPlayerNode*>(Sys->PlayerListHead)->Next;
             Node != static_cast<SystemPlayerNode*>(Sys->PlayerListHead); Node = Node->Next)
        {
            SystemPlayerView_Update(&Node->View, Target);
        }
    }

    StarSystem_SendChangedAffiliations(Sys);

    return Iterations;
}

// (module storage, __cdecl, one float -- `PUSH EAX` of the routine's
// own argument at module storage, `ADD ESP,0x4` after) -- the per-frame walk.
// Traced term by term 2026-09-24 (claude-code:server-triage-s2):
// outside single player, AnimDB::Update(dt) first;
// a function-local static std::vector<unsigned int>
//                               of the systems already stepped this frame
//                               (the field: guard bit 0 of the field, ctor
// , atexit dtor the routine);
// for every active player (PlayerDB::traverse_active):
//     - its system id (PlayerData+0x3e0) already in the vector -> skip
//       (the inline find at module storage .. 06d103a4);
//     - SysDB::SysMap lower_bound + key compare (module storage .. 06d103e6),
//       i.e. find(); when present, the routine(dt) on the mapped StarSystem
//       (node+0x10, module storage .. 06d103ec). No loaded test here -- baseline
//       steps whatever the map holds;
//     - find again and, when still absent, insert at end (the routine with
//       the vector's _Last, module storage .. 06d1041a). The third find at
// has no effect on the result;
// / module storage  _Last = _First -- the vector is emptied,
//                               its storage kept, on both exits.
// No cap on the number of systems and no loaded guard: baseline has neither.
static void Server_StepSystems(float dt) {
    if (!SinglePlayer())
        AnimDB::Update(dt);

    // Oracle instrumentation (P2.3 sim): Oracle.exe sim steps its one scenario system, which
    // has no player, and nothing else. StepSystemId is 0 outside the harness.
    if (GOracleSimHooks.StepSystemId != 0u) {
        SysDB::SysMapType::iterator const OracleIt = SysDB::SysMap.find(GOracleSimHooks.StepSystemId);
        if (OracleIt != SysDB::SysMap.end())
            StarSystem_Update(&OracleIt->second, dt);
        return;
    }

    static std::vector<unsigned int> SteppedSystems;

    PlayerData* Player = Players.traverse_active(nullptr);
    while (Player != nullptr)
    {
        const unsigned int SysId = Player->system_id;

        bool bAlready = false;
        for (size_t i = 0; i < SteppedSystems.size(); ++i)
        {
            if (SteppedSystems[i] == SysId)
            {
                bAlready = true;
                break;
            }
        }

        if (!bAlready)
        {
            SysDB::SysMapType::iterator It = SysDB::SysMap.find(SysId);
            if (It != SysDB::SysMap.end())
            {
                StarSystem_Update(&It->second, dt);
            }

            bool bPresent = false;
            for (size_t i = 0; i < SteppedSystems.size(); ++i)
            {
                if (SteppedSystems[i] == SysId)
                {
                    bPresent = true;
                    break;
                }
            }
            if (!bPresent)
                SteppedSystems.push_back(SysId);
        }

        Player = Players.traverse_active(Player);
    }

    SteppedSystems.clear();
}

// (module storage .. 06d00272, cdecl, one float; only caller
// ). Every instruction:
// / 06d001da  the routine(dt)            -- Server_StepSystems
// the routine()              -- the phantom event pump
// for every node of the dying-ship list (head re-read
//                        from .data:the field each lap):
//                          [ESP+0x10] = the routine(ship->OwnerSystem)
//                                       (SystemInfo vtable slot 0)
//                          [ESP+0x08] = ship->get_id()     (vtable +0x20)
//                          [ESP+0x0c] = ship->DeathSequenceObjId (+0x90)
// (0x25, &[ESP+0x08])
// clear the list: unlink, free, --size, per node.
// The system id is read BEFORE get_id (06d001f8 vs 06d00206).
static void Server_PhysicsUpdate(float dt) {
    Server_StepSystems(dt);

    Server_PumpPhantomEvents();

    for (std::list<Ship*>::iterator It = GDyingShips.begin(); It != GDyingShips.end(); ++It) {
        Ship* const Dying = *It;
        unsigned int Event[3];
        Event[2] = Dying->OwnerSystem->SystemInfo->NicknameId;   // / 06d00200
        Event[0] = Dying->get_id();                              // / 06d00209
        Event[1] = Dying->DeathSequenceObjId;                    // / 06d0021d
        Server_ControllerDispatch(0, pub::Controller::EMessage::Msg0x25, Event);
    }
    GDyingShips.clear();
}

// (module storage .. module storage), called by Shutdown (module storage).
// (&active)   -- std::list::sort, ascending by record+0x10 = node+0x18
//                              (the Flid), unsigned compare, stable merge sort
//   walk from the LAST node back to the first: pub::Controller::Destroy(node->Flid)
//                              (re-reading the list head's first node each time)
// (0.0)        -- the heartbeat walk, whose first phase tears down
//                              everything Destroy just queued
// The deferred-create list is left alone.
static void Server_ControllerCleanup() {
    // stable ascending sort by Flid. A stable insertion into a fresh
    // chain gives the same order as the baseline merge sort.
    if (g_ControllerListSentinel.Next != &g_ControllerListSentinel) {
        FControllerNode* Pending = g_ControllerListSentinel.Next;
        g_ControllerListSentinel.Prev->Next = nullptr;
        g_ControllerListSentinel.Next = &g_ControllerListSentinel;
        g_ControllerListSentinel.Prev = &g_ControllerListSentinel;
        while (Pending != nullptr) {
            FControllerNode* NextPending = Pending->Next;
            FControllerNode* Before = g_ControllerListSentinel.Prev;
            while (Before != &g_ControllerListSentinel && Pending->FLID < Before->FLID)
                Before = Before->Prev;
            ListInsertBefore(Before->Next, Pending);
            Pending = NextPending;
        }
    }

    FControllerNode* cur = &g_ControllerListSentinel;
    if (cur != cur->Next) {
        do {
            Server_ControllerDestroy(cur->Prev->FLID);
            cur = cur->Prev;
        } while (cur != g_ControllerListSentinel.Next);
    }
    Server_TimerTick(0.0f);
}

// (module storage .. module storage), cdecl (float dt): the controller
// heartbeat walk, called every ElapseTime (module storage) and once by the cleanup.
//   1. for each queued Destroy (the field): GetProcAddress(Module,"DestroyInstance")
//      (Interface); GetModuleFileNameA (result unused); FreeLibrary(Module); unlink the
//      node from the active list, free it, the field--. Then empty the queue.
//   2. ++the field
//   3. for each active node with period (+0x10) >= 0: accum (+0x14) += dt; if
//      accum >= period: WrapTime = (float)(floor(accum / period) * period);
//      Interface->vtable[0](0x20, &WrapTime) (thiscall, module storage);
//      accum = accum (re-read) - WrapTime
//   4. --the field; at zero drain the deferred-create list (as _SendMessage does)
// Port field names: HeartbeatTimer is baseline +0x10 (period), HeartbeatAccum +0x14.
static void Server_TimerTick(float dt) {
    for (std::list<FControllerNode*>::iterator It = g_DestroyPending.begin();
         It != g_DestroyPending.end(); ++It) {
        FControllerNode* Node = *It;
        FControllerDestroyInstanceFn DestroyInstance =
            (FControllerDestroyInstanceFn)GetProcAddress(Node->ModuleHandle, "DestroyInstance");
        DestroyInstance(Node->InterfacePtr);
        char ModuleName[0x104];
        GetModuleFileNameA(Node->ModuleHandle, ModuleName, 0x104);
        FreeLibrary(Node->ModuleHandle);
        ListRemove(Node);
        free(Node);
        g_ControllerCount--;
    }
    g_DestroyPending.clear();

    g_ReentrancyGuard++;
    FControllerNode* cur = g_ControllerListSentinel.Next;
    while (cur != &g_ControllerListSentinel) {
        if (cur->HeartbeatTimer >= 0.0f) {
            // FLD dt / FADD [+0x14] / FCOM [+0x10] / FST [+0x14]: the compare and the
            // FDIV use the unrounded x87 sum still in ST0, only the store is rounded.
            const double Sum = static_cast<double>(dt) + static_cast<double>(cur->HeartbeatAccum);
            cur->HeartbeatAccum = static_cast<float>(Sum);
            const double Period = static_cast<double>(cur->HeartbeatTimer);
            if (Sum >= Period) {
                float WrapTime = static_cast<float>(floor(Sum / Period) * Period);
                cur->InterfacePtr->HandleMessage(pub::Controller::EMessage::Heartbeat, &WrapTime);
                cur->HeartbeatAccum = cur->HeartbeatAccum - WrapTime;
            }
        }
        cur = cur->Next;
    }
    g_ReentrancyGuard--;
    if (g_ReentrancyGuard == 0)
        Server_ControllerDrainDeferred();
}

// Deferred-create drain, shared by the heartbeat walk and by the broadcast arm of
// Server_ControllerDispatch (baseline 0x06d440d5-0x06d4413d does it in both places).
static void Server_ControllerDrainDeferred() {
    FControllerNode* cur = g_DeferredListSentinel.Next;
    while (cur != &g_DeferredListSentinel) {
        FControllerNode* next = cur->Next;
        ListRemove(cur);
        g_DeferredCount--;
        // Insert into active list sorted by priority
        FControllerNode* insert_cur = g_ControllerListSentinel.Next;
        while (insert_cur != &g_ControllerListSentinel) {
            if (cur->Priority <= insert_cur->Priority) break;
            insert_cur = insert_cur->Next;
        }
        cur->Next = insert_cur;
        cur->Prev = insert_cur->Prev;
        insert_cur->Prev->Next = cur;
        insert_cur->Prev = cur;
        g_ControllerCount++;
        cur = next;
    }
}

// ---------------------------------------------------------------------------
// Server_FormatTString256 -- baseline the routine (module storage .. module storage),
// cdecl varargs (TString<256>* Out, char const* Fmt, ...), plain RET. Only caller:
// the timer tick below (module storage).
// vsprintf(char Local[0x100], Fmt, &varargs) -- unbounded
// the inlined TString<256>::operator=(Local): strlen, a
//                       length >= 0x100 becomes 0xff, MOVSD/MOVSB copy into
//                       Out+4, Out->Data[len] = 0, Out->Length = len. No null test
//                       (the source is always the local buffer).
// ---------------------------------------------------------------------------
static void Server_FormatTString256(TString<256>* Out, char const* Fmt, ...) {
    char Local[0x100];
    va_list Args;
    va_start(Args, Fmt);
    vsprintf(Local, Fmt, Args);
    va_end(Args);
    unsigned int Len = static_cast<unsigned int>(strlen(Local));
    if (Len >= 0x100)
        Len = 0xff;
    memcpy(Out->Data, Local, Len);
    Out->Data[Len] = '\0';
    Out->Length = Len;
}

// — Timer firing system
static void Server_TimerFiring(float dt) {
    FTimerNode* expired[128];
    unsigned int expired_count = 0;

    // Phase 1 (module storage .. module storage): every timer loses dt; while
    // fewer than 0x80 are collected (CMP EBP,0x80 / JNC after the FSTP), one whose
    // remaining time is below 0.0 (FCOMP double 0.0, TEST AH,5 / JP) is collected.
    FTimerNode* cur = g_TimerListSentinel.Next;
    while (cur != &g_TimerListSentinel) {
        cur->RemainingTime -= dt;
        if (expired_count < 128 && cur->RemainingTime < 0.0f) {
            expired[expired_count] = cur;
            expired_count++;
            // .. module storage -- the collection that fills the
            // 128th entry warns once: the routine formats the pubMisc.cpp message
            // with the timer count the field into a TString<256> (Length and
            // Data[0] zeroed first, module storage/06d5dfc9) and FDUMP(0x100002,
            // "%s(%d) : *** WARNING: %s", pubMisc.cpp, 0x3c, Text.Data).
            if (expired_count == 128) {
                TString<256> Text;
                Text.Length = 0;
                Text.Data[0] = '\0';
                Server_FormatTString256(&Text,
                    "%x timers are trying to fire on the same frame.  Only 64 will fire now.  "
                    "The next ones will fire on the next frame.",
                    g_TimerCount);
                FDUMP(0x100002, "%s(%d) : *** WARNING: %s",
                      "Server\\pubMisc.cpp", 0x3c, Text.Data);
            }
        }
        cur = cur->Next;
    }

    // Phase 2: Fire all expired timers
    for (unsigned int i = 0; i < expired_count; i++) {
        FTimerNode* timer = expired[i];

        //   _SendMessage(node+0x08 /* the timer's OWN Flid, by reference */, 0x3e,
        //                node+0x0c /* the two TimerExpired dwords */)
        // i.e. the expiry is delivered to that one controller, not broadcast. The port
        // used to route it through the empty Common.dll dispatcher instead.
        Server_ControllerDispatch(timer->TargetController, pub::Controller::EMessage::TimerExpired,
                                  &timer->Param1);
        // Unlink and free
        TimerListRemove(timer);
        g_TimerCount--;
        free(timer);
    }
}

// `Server` -- baseline export ordinal 524, RVA 0xAFACC (module storage), UNDECORATED
// (dumpbin -exports EXE\server.dll: `524 20B 000AFACC Server`), so it is an
// extern "C" data symbol and cannot carry the G prefix. It is the one IServerImpl
// object, twelve bytes: the dynamic initializer the branch (module storage in
// the __xc table) is the inlined default ctor --
// MOV [06d8fad4],0                        ; Field_0x8
// MOV [06d8facc],??_7IServerImpl@@6BIServer@@@      (06cf0dc0 table)
// MOV [06d8fad0],??_7IServerImpl@@6BIChatServer@@@  (06d191b0 table)
// OutpostServer.exe and RemoteClient.dll import it by that name and call IServerImpl
// members on it directly. Inside Server.dll every user is `MOV ECX,.data:Server`
// plus a direct CALL (DisConnect 06d4501d/32/51/67, TerminateTrade at 06d49a6d,
// , 06d4a6f9, 06d50b03, 06d543a6, 06d54465, ForceLand's Dock at
// ) or `MOV ECX,.data:the field` for SubmitChat (06d4f8e1). The other
// translation units of this port reach the same object through
// GetServerInterface()/GetChatServerInterface(); the dynamic type is IServerImpl, so
// the call lands on the same member baseline calls. The class and this object are
// declared in src/include/server_iserverimpl.h.
extern "C" SERVER_API IServerImpl Server;
IServerImpl Server;

extern "C" {
    // MOV EAX,.data:Server / RET.
    SERVER_API IServer* __cdecl GetServerInterface() { return &Server; }
    // MOV EAX,.data:the field / RET -- Server + 4, the IChatServer
    // subobject (second base).
    SERVER_API void* __cdecl GetChatServerInterface() {
        return static_cast<IChatServer*>(&Server);
    }
}

static HMODULE g_hInstance = nullptr;

BOOL WINAPI DllMain(HMODULE hModule, DWORD fdwReason, LPVOID lpReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) { g_hInstance = hModule; DisableThreadLibraryCalls(hModule); }
    return TRUE;
}
