// Author: Giperion.

#include "../include/common_server.h"
#include "../include/common_pathfind.h" // PathfindManager (pub::AI::SubmitState)
#include "../include/common_id.h"
#include "../include/common_goods.h"
#include "../include/common_universe.h"
#include "../include/common_archetype.h"
#include "../include/common_reputation.h"
#include "../include/common_iface.h"
#include "../include/common_cobjdb.h"
#include "../include/common_csys.h"
#include "../include/common_cobject.h"
#include "../include/common_server_playerdb.h"
#include "../include/server_exports.h"
#include "../include/server_basedb.h"
#include "../include/server_cobject.h"
#include "../include/common_physys.h"
#include "../include/server_gameobject.h"
#include "../include/server_cship.h"
#include "../include/server_sysdb.h"
#include "../include/common_misc.h"
#include "../include/common_net.h"      // IsMPServer (AdjustCash's gate)
#include "../include/common_newchar.h"   // RtcSlider::get_slider (pub::GF::GetRtcPerformanceSlider)
#include "../include/NeuronRuntime.h"            // NeuronRuntime::Engine (pub::FindHardpoint releases the archetype handle)
#include "../include/readfile.h"         // IFileSystem::GetFileSysPath (+0x84, pub::BuildBaseReader)
#include "../include/x86math.h"      // I3DMathEngine::MatrixToQuaternion (pub::Player::SetCamera)
#include "../include/server_createinfo.h" // Server_Acquire3DMathEngine (the routine)
#include "../include/rpc_iclient.h"
#include "../include/rpc_iclient_args.h" // FPayloadServerSetDirective (pub::AI::SubmitDirective)
#include "../include/server_savefile.h"   // SaveGame_Writer / ISave / Server_FLS1Transform
#include "../include/server_savegame.h"   // pub::Save -> PlayerData_AutoSave / PlayerData_MissionSave
#include "../include/common_casteroid.h"
#include <windows.h>
#include <cstdint>
#include <new>
#include <cmath>
#include <cstdio>
#include <cstring>     // std::memcpy (SpaceObj::Create EquipIdMaker seed)
#include <direct.h>
#include <io.h>        // _unlink (the closeout at module storage / module storage)
#include <map>
#include <set>
#include <stdexcept>   // std::out_of_range (Server_VibeID_Free, the routine)
#include <vector>
#include <list>
#include <string>      // Server_AccountDirName's lowercased wide copy
#include <cwchar>      // wcslen / wcstombs / swprintf
#include <cwctype>     // towlower (baseline _wcslwr)
#include <cstdlib>     // rand, wcstombs
#include <ctime>       // _strdate_s / _strtime_s (baseline _strdate / _strtime)

namespace GlobalReputation = ::Reputation;

// The IClient the server talks back to. Defined in IServerImpl.cpp and set by
// IServerImpl::Startup; baseline keeps it in .data:the field. Several pub:: exports
// dispatch straight into its vtable, so it has to be reachable from this file too.
extern IClient* g_pClientInterface;

// ---------------------------------------------------------------------------
// The object lookup. baseline the routine (Server_FindObject, server_classes.cpp)
// searches where the objects really live: its one-entry cache, the cached
// system's blocks, the in-transit list, then every system in SysDB::SysMap. The
// port's former FLID -> wrapper std::map (g_ObjectRegistry, filled by the
// create paths and read here) is gone (2026-09-24, claude-code:w5-server-06d0):
// an object is found only once it is in a StarSystem block, as in baseline.
// The lookup hands back the GameObject wrapper: pub::SpaceObj::GetCenterOfMass
// at module storage does `ADD EAX,0xc / MOV ECX,[EAX+4]` on the result, i.e.
// it walks into the CBase sub-object at +0x0c and reads CBase::Object.
// ---------------------------------------------------------------------------
// See server_gameobject.h: the callers that keep only the object.
GameObject* Server_LookupGameObject(unsigned int ObjId) {
    GameObject* Obj = nullptr;
    StarSystem* Sys = nullptr;
    Server_FindObject(ObjId, Obj, Sys);
    return Obj;
}

void* Server_LookupObject(unsigned int obj_id) {
    return Server_LookupGameObject(obj_id);
}

// A second, separate table for the handles that are NOT space objects: pub::GF
// character handles and pub::Phantom zones. baseline keeps neither in the object map
// (characters live in BaseDB's per-room list, phantoms in the phantom subsystem);
// the previous pass put them in the same map as ships and solars, which was already
// wrong and is now also a type error, since that map holds IObjRW wrappers.
// TODO: replace this table with the two real subsystems.
static std::map<unsigned int, void*> g_PlaceholderHandles;

static void Server_RegisterPlaceholder(unsigned int id, void* obj) {
    if (id != 0 && obj != nullptr)
        g_PlaceholderHandles[id] = obj;
}

static void Server_UnregisterPlaceholder(unsigned int id) {
    g_PlaceholderHandles.erase(id);
}

// (Server_LookupPlaceholder was removed 2026-09-17: pub::GF::CharacterDestroy
// was its only reader, and characters now live in BaseDB's real per-room NPC
// list. What is left in this table is pub::Phantom's zone handles.)

// Player validation helper -- the guard every pub::Player export opens with, e.g.
// SetMsnID module storage .. module storage:
//   TEST ESI,ESI / JBE fail                        ; id == 0
//   LEA EAX,[ESI-1] / CMP EAX,[the field] / JNC ; id-1 >= Players.MaxPlayers
//   IMUL EAX,EAX,0x418 / MOV EDX,[EAX+ECX+0x348]   ; Players[id-1].client_id
//   TEST EDX,EDX / JZ fail
// baseline has no "active" flag in PlayerData: occupancy is the client id at +0x348
// (2026-09-24, claude-code:w5-server-06d4 -- the port's invented `active` bool is
// gone from PlayerData).
static bool IsValidPlayer(unsigned int player_id) {
    if (player_id == 0 || (player_id - 1) >= PlayerDB_GetMaxPlayers()) return false;
    return Players[player_id].client_id != 0;
}

// Forward declarations for functions defined in IServerImpl.cpp
unsigned int Server_ControllerCreate(const char* dll_path, const char* instance_name, void* parms, unsigned int priority);
void Server_ControllerDestroy(unsigned int flid);

// Game time accumulator defined in IServerImpl.cpp (updated by ElapseTime)
extern double g_ElapsedTimeAccum;   // , IServerImpl.cpp
extern double GServerTimeBase;      // , IServerImpl.cpp
extern char GDllPath[0x104];        // , IServerImpl.cpp (pub::DLL_LoadLibrary)

// ===========================================================================
// pub global functions -- module storage..06d5f033 (0x7e440-0x7f033); per-function
// listing traces are in each function's comment and in its implementation tracker item
// ===========================================================================
namespace pub {

// pub::IsCommodity -- baseline 0x7e440 (ordinal 311), module storage..06d5e471.
// GoodList::find_by_id(*GoodId); a miss returns -4 WITHOUT touching OutCommodity.
// Found: OutCommodity = (GoodInfo+0x4c == 0), i.e. Category == GoodCategoryCommodity.
SERVER_API int __cdecl IsCommodity(const unsigned int& GoodId, bool& OutCommodity) {
    GoodInfo const* Info = GoodList::find_by_id(GoodId);
    if (Info == nullptr)
        return -4;
    OutCommodity = (Info->Category == GoodCategoryCommodity);
    return 0;
}

// pub::GetCargoHoldSize -- baseline 0x7e480 (ordinal 161), module storage..06d5e4c5.
// OutSize = 0 first (06d5e489), then the usual id checks: masked id 0 -> -4,
// Archetype::GetShip miss -> -2; else OutSize = ftol(Ship+0x100 hold_size).
SERVER_API int __cdecl GetCargoHoldSize(const unsigned int& ShipArchId, unsigned int& OutSize) {
    OutSize = 0;
    if ((ShipArchId & 0x3fffffff) == 0)
        return -4;
    Archetype::Ship* ShipArch = Archetype::GetShip(ShipArchId);
    if (ShipArch == nullptr)
        return -2;
    OutSize = static_cast<unsigned int>(static_cast<long long>(ShipArch->HoldSize));
    return 0;
}

// pub::GetLoadoutName -- baseline 0x7e4d0 (ordinal 203), module storage..06d5e518.
// baseline tests the OUTPUT buffer (null, or first byte already 0 -> -2), then
// Loadout::Get(*LoadoutId) (miss -> -2), then ID_String::get_string on the Map
// itself (its first dword is the nickname id; null -> -2), and finally a plain
// strncpy(OutBuffer, Str, MaxLen) -- no forced terminator -- returning 0.
SERVER_API int __cdecl GetLoadoutName(const unsigned int& LoadoutId, char* OutBuffer, int MaxLen) {
    if (OutBuffer == nullptr || OutBuffer[0] == '\0')
        return -2;
    Loadout::Map const* LoadoutMap = Loadout::Get(LoadoutId);
    if (LoadoutMap == nullptr)
        return -2;
    ID_String Name;
    Name.Id = LoadoutMap->NicknameId;
    char const* Str = Name.get_string();
    if (Str == nullptr)
        return -2;
    strncpy(OutBuffer, Str, static_cast<size_t>(MaxLen));
    return 0;
}

// pub::GetLoadoutID -- baseline 0x7e520 (ordinal 202), module storage..06d5e551.
// Null/empty name -> -4; Loadout::GetID(name) == 0 -> -2 with OutId untouched;
// else OutId = id, 0.
SERVER_API int __cdecl GetLoadoutID(unsigned int& OutId, const char* Nickname) {
    if (Nickname == nullptr || Nickname[0] == '\0')
        return -4;
    unsigned int Id = Loadout::GetID(Nickname);
    if (Id == 0)
        return -2;
    OutId = Id;
    return 0;
}

// pub::GetEquipmentID -- baseline 0x7e740 (ordinal 181), module storage..06d5e781.
// Null/empty name -> -4; CreateID into an ID_String, Archetype::GetEquipmentID(it)
// -> OutId unconditionally; return (id & 0x3fffffff) ? 0 : -2.
SERVER_API int __cdecl GetEquipmentID(unsigned int& OutId, const char* Nickname) {
    if (Nickname == nullptr || Nickname[0] == '\0')
        return -4;
    ID_String Name;
    Name.Id = CreateID(Nickname);
    unsigned int Id = Archetype::GetEquipmentID(Name);
    OutId = Id;
    return ((Id & 0x3fffffff) != 0) ? 0 : -2;
}

// pub::GetGoodID -- baseline 0x7e790 (ordinal 185), module storage..06d5e7cf.
// Null/empty name -> -4; GoodList::find_by_nickname miss -> -2 (OutId untouched);
// else OutId = GoodInfo+0x48 (Id) and return (id & 0x3fffffff) ? 0 : -2.
SERVER_API int __cdecl GetGoodID(unsigned int& OutId, const char* Nickname) {
    if (Nickname == nullptr || Nickname[0] == '\0')
        return -4;
    GoodInfo const* Info = GoodList::find_by_nickname(Nickname);
    if (Info == nullptr)
        return -2;
    unsigned int Id = Info->Id;
    OutId = Id;
    return ((Id & 0x3fffffff) != 0) ? 0 : -2;
}

// pub::TranslateArchToGood -- baseline 0x7e7d0 (ordinal 465), module storage..06d5e7ff.
// *ArchId == 0 (unmasked) -> -4; OutGoodId = Arch2Good(*ArchId) unconditionally;
// return (good & 0x3fffffff) ? 0 : -2.
SERVER_API int __cdecl TranslateArchToGood(const unsigned int& ArchId, unsigned int& OutGoodId) {
    if (ArchId == 0)
        return -4;
    unsigned int GoodId = Arch2Good(ArchId);
    OutGoodId = GoodId;
    return ((GoodId & 0x3fffffff) != 0) ? 0 : -2;
}

// pub::GetVoiceID -- baseline 0x7e800 (ordinal 296), module storage..06d5e829.
// Null/empty name -> -4; else OutId = CreateID(name), 0.
SERVER_API int __cdecl GetVoiceID(unsigned int& OutId, const char* Nickname) {
    if (Nickname == nullptr || Nickname[0] == '\0')
        return -4;
    OutId = CreateID(Nickname);
    return 0;
}

// pub::GetCostumeID -- baseline 0x7e830 (ordinal 175), module storage..06d5e870.
// Null/empty name -> -4; GetCostumeDescriptions()->find_costume(name); OutId =
// costume ? costume->Id (+0x00) : 0, written in both cases; return
// ((OutId != 0) - 1) & ~1, i.e. 0 when found and -2 otherwise.
SERVER_API int __cdecl GetCostumeID(int& OutId, const char* Nickname) {
    if (Nickname == nullptr || Nickname[0] == '\0')
        return -4;
    costume const* Found = GetCostumeDescriptions()->find_costume(Nickname);
    int Id = 0;
    if (Found != nullptr)
        Id = static_cast<int>(Found->Id);
    OutId = Id;
    return (Id != 0) ? 0 : -2;
}

// pub::GetDataPath -- baseline 0x7e880 (ordinal 177): MOV EAX,[NeuronRuntime::Data] / MOV EAX,[EAX].
SERVER_API IFileSystem* __cdecl GetDataPath() {
    return NeuronRuntime::Data;
}

SERVER_API int __cdecl GetTime(double& OutTime) {
    // baseline 0x7e890: raw_time = the routine() (_DAT_06d8fb28 + _DAT_06d8fb20)
    // OutTime = raw_time * DaysPerSecond
    OutTime = (g_ElapsedTimeAccum + GServerTimeBase) * DaysPerSecond;
    return 0;
}

SERVER_API int __cdecl GetRwTime(double& OutTime) {
    // baseline 0x7e8b0: OutTime = the routine() (raw seconds, no DaysPerSecond multiply)
    OutTime = g_ElapsedTimeAccum + GServerTimeBase;
    return 0;
}

// pub::NextSystemID -- baseline 0x7e8c0 (ordinal 346), module storage..06d5e90c.
// baseline does not resume Common's iterator: it restarts at GetFirstSystem and, when
// InOutId is non-zero, walks forward until it has stepped PAST the system whose id
// (ISystem vslot 0) equals InOutId. The system it lands on (or the first one, for
// InOutId == 0) is reported; running off the end returns false with InOutId intact.
SERVER_API bool __cdecl NextSystemID(unsigned int& InOutId) {
    Universe::ISystem const* Sys = Universe::GetFirstSystem();
    if (InOutId != 0) {
        while (Sys != nullptr) {
            bool const bMatch = (Sys->NicknameId == InOutId);
            Sys = Universe::GetNextSystem();
            if (bMatch)
                break;
        }
    }
    if (Sys == nullptr)
        return false;
    InOutId = Sys->NicknameId;
    return true;
}

// pub::NextBaseID -- baseline 0x7e910 (ordinal 345), module storage..06d5e95c. The
// NextSystemID walk over GetFirstBase / GetNextBase and IBase vslot 0.
SERVER_API bool __cdecl NextBaseID(unsigned int& InOutId) {
    Universe::IBase const* Base = Universe::GetFirstBase();
    if (InOutId != 0) {
        while (Base != nullptr) {
            bool const bMatch = (Base->NicknameId == InOutId);
            Base = Universe::GetNextBase();
            if (bMatch)
                break;
        }
    }
    if (Base == nullptr)
        return false;
    InOutId = Base->NicknameId;
    return true;
}

// pub::GetSystemID -- baseline 0x7e960 (ordinal 287), module storage..06d5e99a.
// Null/empty name -> -4; Universe::get_system_id(name) with a zero low-30-bit id
// -> -2 and OutId untouched; else OutId = id, 0.
SERVER_API int __cdecl GetSystemID(unsigned int& OutId, const char* Nickname) {
    if (Nickname == nullptr || Nickname[0] == '\0')
        return -4;
    unsigned int Id = Universe::get_system_id(Nickname);
    if ((Id & 0x3fffffff) == 0)
        return -2;
    OutId = Id;
    return 0;
}

SERVER_API int __cdecl GetSystemGateConnection(const unsigned int& GateId, unsigned int& OutConnectedSystemId) {
    // baseline 0x7e9a0: Universe::get_gate_system(GateId), check low 30 bits
    unsigned int SysId = Universe::get_gate_system(GateId);
    OutConnectedSystemId = SysId;
    if ((SysId & 0x3fffffff) != 0) {
        return 0;
    }
    return -2;
}

// pub::GetBaseID -- baseline 0x7e9d0 (ordinal 153), module storage..06d5ea0d.
// OutId = 0 first (06d5e9e1); null/empty name and an unknown base BOTH return -4
// (EDI = 0xfffffffc is the only failure code); else OutId = id, 0.
SERVER_API int __cdecl GetBaseID(unsigned int& OutId, const char* Nickname) {
    OutId = 0;
    if (Nickname == nullptr || Nickname[0] == '\0')
        return -4;
    unsigned int Id = Universe::get_base_id(Nickname);
    if ((Id & 0x3fffffff) == 0)
        return -4;
    OutId = Id;
    return 0;
}

// pub::BuildSystemReader -- baseline 0x7ea10 (ordinal 68), module storage..06d5eaa5.
// Reader.close(); get_system miss -> -2; Universe::get_filename(path, ISystem vslot 5
// = the system's "file"); Reader.open(path, false) -> 0, else FDUMP the missing file
// (pubMisc.cpp line 0x2b3) and -2.
SERVER_API int __cdecl BuildSystemReader(INI_Reader& Reader, const unsigned int& SystemId) {
    Reader.close();
    Universe::ISystem const* Sys = Universe::get_system(SystemId);
    if (Sys == nullptr)
        return -2;
    char Path[0x104];
    Universe::get_filename(Path, Sys->File);
    if (Reader.open(Path, false))
        return 0;
    FDUMP(0x100001, "%s(%d) : *** ERROR: missing System file '%s'",
          "Server\\pubMisc.cpp", 0x2b3, Path);
    return -2;
}

// pub::BuildBaseReader -- baseline 0x7eab0 (ordinal 66), module storage..06d5eb50.
// Reader.close(); get_base miss -> -2; (*NeuronRuntime::Data)->GetFileSysPath(dir, 0x104)
// (IFileSystem +0x84); _makepath(path, NULL, dir, IBase vslot 3 = the base's "file",
// NULL); Reader.open(path, false) ? 0 : -2 (no FDUMP on this one).
SERVER_API int __cdecl BuildBaseReader(INI_Reader& Reader, const unsigned int& BaseId) {
    Reader.close();
    Universe::IBase const* Base = Universe::get_base(BaseId);
    if (Base == nullptr)
        return -2;
    char DataDir[0x104];
    NeuronRuntime::Data->GetFileSysPath(DataDir, 0x104);
    char Path[0x104];
    _makepath(Path, nullptr, DataDir, Base->File, nullptr);
    if (Reader.open(Path, false))
        return 0;
    return -2;
}

// pub::GetSystemNickname -- baseline 0x7eb60 (ordinal 288), module storage..06d5ebb0.
// get_system miss -> 0. Needed = strlen(nickname (ISystem vslot 2)) + 1; MaxLen <
// Needed -> return Needed; else strcpy and 0.
SERVER_API unsigned int __cdecl GetSystemNickname(char* OutBuffer, unsigned int MaxLen, const unsigned int& SystemId) {
    Universe::ISystem const* Sys = Universe::get_system(SystemId);
    if (Sys == nullptr)
        return 0;
    unsigned int Needed = static_cast<unsigned int>(strlen(Sys->Nickname)) + 1;
    if (MaxLen < Needed)
        return Needed;
    memcpy(OutBuffer, Sys->Nickname, Needed);
    return 0;
}

// pub::GetBaseNickname -- baseline 0x7ebc0 (ordinal 154), module storage..06d5ec13.
// The same over get_base and IBase vslot 1 (called twice by baseline, same string).
SERVER_API unsigned int __cdecl GetBaseNickname(char* OutBuffer, unsigned int MaxLen, const unsigned int& BaseId) {
    Universe::IBase const* Base = Universe::get_base(BaseId);
    if (Base == nullptr)
        return 0;
    unsigned int Needed = static_cast<unsigned int>(strlen(Base->Nickname)) + 1;
    if (Needed > MaxLen)
        return Needed;
    memcpy(OutBuffer, Base->Nickname, Needed);
    return 0;
}

// pub::GetBaseStridName -- baseline 0x7ec20 (ordinal 156): get_base miss -> -2 (OutStrid
// untouched); else OutStrid = IBase vslot 2 (strid_name), 0.
SERVER_API int __cdecl GetBaseStridName(unsigned int& OutStrid, const unsigned int& BaseId) {
    Universe::IBase const* base = Universe::get_base(BaseId);
    if (base) {
        OutStrid = base->StridName;
        return 0;
    }
    return -2;
}

// pub::GetLocations -- baseline 0x7ec50 (ordinal 207), module storage..06d5ed08.
// BaseDataList_get()->get_base_data(*BaseId) miss -> OutCount = 0, return 0. Else
// all seven fixed room names are tried (the loop never stops early): MakeLocationID,
// and if BaseDataList_get()->get_room_data(room) exists AND there is still room in the
// output, append it. OutCount = appended, return 0.
SERVER_API int __cdecl GetLocations(const unsigned int& BaseId, unsigned int* const OutLocations, unsigned int MaxCount, unsigned int& OutCount) {
    static char const* const RoomNames[7] = {
        "Bar", "Deck", "Trader", "Equipment", "Cityscape", "ShipDealer", "Planetside"
    };
    unsigned int Count = 0;
    if (BaseDataList_get()->get_base_data(BaseId) == nullptr) {
        OutCount = Count;
        return 0;
    }
    for (unsigned int Index = 0; Index < 7; ++Index) {
        unsigned int RoomId = MakeLocationID(BaseId, RoomNames[Index]);
        if (BaseDataList_get()->get_room_data(RoomId) != nullptr && Count < MaxCount) {
            OutLocations[Count] = RoomId;
            ++Count;
        }
    }
    OutCount = Count;
    return 0;
}

// pub::GetLocationNickname -- baseline 0x7ed10 (ordinal 206), module storage..06d5ed5f.
// get_room_data(*RoomId) is dereferenced WITHOUT a null test (06d5ed29 reads the
// nickname at RoomData+0xc straight away). Needed = strlen + 1; Needed > MaxLen ->
// return Needed; else strcpy and 0.
SERVER_API unsigned int __cdecl GetLocationNickname(char* OutBuffer, unsigned int MaxLen, const unsigned int& RoomId) {
    RoomData* Room = BaseDataList_get()->get_room_data(RoomId);
    unsigned int Needed = static_cast<unsigned int>(strlen(Room->Nickname)) + 1;
    if (Needed > MaxLen)
        return Needed;
    memcpy(OutBuffer, Room->Nickname, Needed);
    return 0;
}

// pub::GetBases -- baseline 0x7ed60 (ordinal 157), module storage..06d5edbe.
// Walks EVERY base (GetFirstBase / GetNextBase to the end, leaving Common's iterator
// exhausted); a base whose system (IBase vslot 4) matches is appended while Count <
// MaxCount. OutCount = Count, return 0.
SERVER_API int __cdecl GetBases(const unsigned int& SystemId, unsigned int* const OutBases, unsigned int MaxCount, unsigned int& OutCount) {
    unsigned int Count = 0;
    Universe::IBase const* Base = Universe::GetFirstBase();
    while (Base != nullptr) {
        if (Base->SystemId == SystemId && Count < MaxCount) {
            OutBases[Count] = Base->NicknameId;
            ++Count;
        }
        Base = Universe::GetNextBase();
    }
    OutCount = Count;
    return 0;
}

SERVER_API int __cdecl GetSystem(unsigned int& OutSystemId, const unsigned int& BaseId) {
    OutSystemId = 0;
    Universe::IBase const* base = Universe::get_base(BaseId);
    if (base) {
        OutSystemId = base->SystemId;
        return 0;
    }
    return -2;
}

SERVER_API unsigned int __cdecl GetNicknameId(const char* Nickname) {
    return CreateID(Nickname);
}

SERVER_API unsigned short __cdecl MakeId(const char* Nickname) {
    return ::MakeId(Nickname);
}

// pub::DebugPrint -- baseline 0x7ef40 (ordinal 94), module storage..06d5ef95.
// Level 1 -> FDUMP warning (pubMisc.cpp line 0x3c5), level 2 -> FDUMP error (line
// 0x3c6), any other level prints nothing.
SERVER_API void __cdecl DebugPrint(const char* Msg, int Level) {
    if (Level == 1) {
        FDUMP(0x100002, "%s(%d) : *** WARNING: pub: %s",
              "Server\\pubMisc.cpp", 0x3c5, Msg);
    } else if (Level == 2) {
        FDUMP(0x100001, "%s(%d) : *** ERROR: pub: %s",
              "Server\\pubMisc.cpp", 0x3c6, Msg);
    }
}

// pub::GetNavMapScale -- baseline 0x7f000 (ordinal 226), module storage..06d5f033.
// OutScale = 0 first (06d5f00e: MOV dword [ESI],0); get_system miss -> -1; else
// OutScale = ISystem vslot 20 (+0x50, the navmapscale float), 0.
SERVER_API int __cdecl GetNavMapScale(unsigned int SystemId, float& OutScale) {
    OutScale = 0.0f;
    Universe::ISystem const* Sys = Universe::get_system(SystemId);
    if (Sys == nullptr)
        return -1;
    OutScale = Sys->NavMapScale;
    return 0;
}

// (.data, zero-initialised) -- the free-terminal mailbox.
// (the launch-position helper) writes -1 here (module storage),
// dispatches controller event 0x32 through the routine (module storage) so a
// mission script can answer synchronously with pub::ReportFreeTerminal, and
// reads the answer back at module storage (`CMP EAX,-1`). the routine is
// Server_GetLaunchPosition (server_classes.cpp; extern in server_gameobject.h).
// Defined at global scope (not pub::) to match that extern declaration.
} // namespace pub
int GReportedFreeTerminal = 0;
namespace pub {

// pub::ReportFreeTerminal -- baseline module storage (RVA 0x1fd10, ordinal 363),
// four instructions: store the SECOND argument in the field, return 0. The
// first argument is never read. (Corrected 2026-09-24: the port dropped the
// store.)
SERVER_API int __cdecl ReportFreeTerminal(unsigned int P1, int P2) {
    GReportedFreeTerminal = P2;
    return 0;
}

// pub::DLL_LoadLibrary -- baseline 0x06d43c40 (ordinal 93), cdecl, 0x208 bytes of locals.
// if (DllPath[0] == '\\' || DllPath[1] == ':')  byte-copy DllPath as is
// else _makepath(tmp, NULL, &the field, DllPath, NULL);
//                  _fullpath(full, tmp, 0x104)
//             LoadLibraryA(full); on NULL FDUMP(0x100002, "...failed to load mission
//             DLL '%s'", dlls.cpp, 0x1de, full); return the module.
// is the [Initial * DLLs] `path` (GDllPath, IServerImpl.cpp).

SERVER_API HINSTANCE __cdecl DLL_LoadLibrary(const char* DllPath) {
    char FullPath[0x104];
    if (DllPath[0] == '\\' || DllPath[1] == ':') {
        strcpy(FullPath, DllPath);
    } else {
        char Joined[0x104];
        _makepath(Joined, nullptr, GDllPath, DllPath, nullptr);
        _fullpath(FullPath, Joined, 0x104);
    }
    HMODULE Module = LoadLibraryA(FullPath);
    if (Module == nullptr) {
        FDUMP(0x100002, "%s(%d) : *** WARNING: failed to load mission DLL '%s'\n",
              "Server\\dlls.cpp", 0x1de, FullPath);
    }
    return Module;
}

// ===========================================================================
// pub:: utility functions — line-by-line verified against Server
// ===========================================================================

// pub::GetFullHealth — baseline 0x7e1f0 (ordinal 182)
// Assembly: Archetype::GetShip → FLD [EAX+0x1c] (Root::HitPts) → ftol → store
// Returns: 0 success, -2 ship archetype not found, -4 invalid obj_id
SERVER_API int __cdecl GetFullHealth(const unsigned int& ObjId, unsigned int& OutHealth) {
    int Result = -4;
    if ((ObjId & 0x3fffffff) != 0) {
        Archetype::Ship* ShipArch = Archetype::GetShip(ObjId);
        if (ShipArch != nullptr) {
            // FLD [EAX+0x1c] / CALL ftol / MOV [EDX],EAX -- ftol truncates
            // to 64 bits and the low dword is stored.
            OutHealth = static_cast<unsigned int>(static_cast<long long>(ShipArch->HitPts));
            return 0;
        }
        Result = -2;
    }
    return Result;
}

// pub::GetShipArchSTRID — baseline 0x7e230 (ordinal 269)
// Assembly: Archetype::GetShip → MOV EAX,[EAX+0x14] (Root::IdsName) → conditional return
// Return formula: (-(uint)((strid & 0x3fffffff) != 0) & 2) - 2
//   strid valid → 0, strid zero/invalid → -2, obj_id bad → -4
SERVER_API int __cdecl GetShipArchSTRID(const unsigned int& ObjId, unsigned int& OutStrid) {
    if ((ObjId & 0x3fffffff) != 0) {
        Archetype::Ship* ShipArch = Archetype::GetShip(ObjId);
        unsigned int StridValue = (unsigned int)ShipArch->IdsName;
        OutStrid = StridValue;
        // baseline branchless: NEG/SBB/AND/ADD sequence
        return (-(int)((StridValue & 0x3fffffff) != 0) & 2) - 2;
    }
    return -4;
}

// pub::GetType — baseline 0x7e330 (ordinal 294), module storage..06d5e364, all 18 insns:
//   MOV ECX,[ESP+4] / MOV ECX,[ECX] / TEST ECX,0x3fffffff / MOV EAX,-4 / JZ ret
//   PUSH ECX / CALL [COMMON.DLL::Archetype::GetShip] / ADD ESP,4
//   TEST EAX,EAX / MOV ECX,-2 / JZ ret_ecx
//   MOV EDX,[EAX+0x10] (Root::ObjectType) / MOV EAX,[ESP+8] / MOV [EAX],EDX / XOR EAX,EAX / RET
// GetSolarType below is the same body over Archetype::GetSolar.
SERVER_API int __cdecl GetType(const unsigned int& ObjId, unsigned int& OutType) {
    int Result = -4;
    if ((ObjId & 0x3fffffff) != 0) {
        Archetype::Ship* ShipArch = Archetype::GetShip(ObjId);
        if (ShipArch != nullptr) {
            OutType = ShipArch->ObjectType;
            return 0;
        }
        Result = -2;
    }
    return Result;
}

// pub::GetSolarType — baseline 0x7e370 (ordinal 279)
// Assembly: Archetype::GetSolar → MOV EDX,[EAX+0x10] (Root::ObjectType) → store
// Returns: 0 success, -2 solar archetype not found, -4 invalid obj_id
SERVER_API int __cdecl GetSolarType(const unsigned int& ObjId, unsigned int& OutType) {
    int Result = -4;
    if ((ObjId & 0x3fffffff) != 0) {
        Archetype::Solar* SolarArch = Archetype::GetSolar(ObjId);
        if (SolarArch != nullptr) {
            OutType = SolarArch->ObjectType;
            return 0;
        }
        Result = -2;
    }
    return Result;
}

// pub::GetMaxHitPoints — baseline 0x7e3b0 (ordinal 211)
// Assembly: Archetype::GetSolar → FLD [EAX+0x1c] (Root::HitPts) → ftol → store
// Returns: 0 success, -2 solar archetype not found, -4 invalid obj_id
SERVER_API int __cdecl GetMaxHitPoints(const unsigned int& ObjId, int& OutHP) {
    int Result = -4;
    if ((ObjId & 0x3fffffff) != 0) {
        Archetype::Solar* SolarArch = Archetype::GetSolar(ObjId);
        if (SolarArch != nullptr) {
            // FLD [EAX+0x1c] / CALL ftol -- 64-bit truncation, low dword stored.
            OutHP = static_cast<int>(static_cast<long long>(SolarArch->HitPts));
            return 0;
        }
        Result = -2;
    }
    return Result;
}

// pub::GetGoodProperties — baseline 0x7e3f0 (ordinal 187)
// Assembly: Good2Arch → Archetype::GetEquipment →
//   MOV EDX,[EAX+0x5c] (Equipment::Volume -> first float&)
//   MOV EDX,[EAX+0x20] (Root::Mass -> second float&)
// Returns: 0 success, -2 equipment archetype not found, -4 invalid good_id
SERVER_API int __cdecl GetGoodProperties(const unsigned int& GoodId, float& OutVolume, float& OutMass) {
    int Result = -4;
    if ((GoodId & 0x3fffffff) != 0) {
        unsigned int ArchId = Good2Arch(GoodId);
        Archetype::Equipment* Equip = Archetype::GetEquipment(ArchId);
        if (Equip != nullptr) {
            OutVolume = Equip->Volume;
            OutMass = Equip->Mass;
            return 0;
        }
        Result = -2;
    }
    return Result;
}

// pub::TranslateSystemToMsgIdPrefix — baseline RVA 0x7E160, ordinal 468
// result -4; Universe::get_system; null -> -4. Then ISystem slot 1
// (GetMsgIdPrefix, SYSTEM's CommReferrable half) and TString<64>::operator=(char const*)
// inlined on its Data (module storage-06d5e1d4), as the Good and Ship siblings below.
// Returns: 0 = success (including empty prefix), -4 = system not found.
SERVER_API int __cdecl TranslateSystemToMsgIdPrefix(unsigned int SystemId, TString<64>& OutPrefix) {
    Universe::ISystem const* Sys = Universe::get_system(SystemId);
    if (Sys == nullptr) {
        return -4;
    }
    OutPrefix = Sys->GetMsgIdPrefix().Data;
    return 0;
}

// pub::SinglePlayer — baseline 0x7e1e0 (ordinal 451)
// baseline: delegates to global ::SinglePlayer() (Common.dll export, reads GbSinglePlayer)
SERVER_API bool __cdecl SinglePlayer() {
    return ::SinglePlayer();
}

// pub::GetLoadout — baseline 0x7e560 (ordinal 201)
// baseline: Loadout::Get → vector<EquipDesc> copy with capacity reuse / realloc
// EquipDescVector is VC6-style (AllocPad, Begin, End, Cap); sizeof(EquipDesc)=0x20
// Three branches: (1) srcCount<=dstSize: copy+shrink, (2) srcCount<=dstCap: copy+extend,
// (3) reallocate. baseline also has a self-assignment test (module storage: param_1 ==
// &Map->Items -> return 0); in the port Map::Items is a std::vector<EquipDesc> owned by
// Common's loadout table, never an EquipDescVector, so the two cannot alias.
// Returns: 0 success, -2 loadout not found, -4 invalid loadout_id
SERVER_API int __cdecl GetLoadout(EquipDescVector& OutLoadout, const unsigned int& LoadoutId) {
    if (LoadoutId == 0) {
        return -4;
    }
    Loadout::Map const* LoadoutMap = Loadout::Get(LoadoutId);
    if (LoadoutMap == nullptr) {
        return -2;
    }
    // Source items from Loadout::Map's std::vector<EquipDesc>
    EquipDesc* SrcBegin = const_cast<EquipDesc*>(LoadoutMap->Items.data());
    unsigned int SrcCount = (unsigned int)LoadoutMap->Items.size();
    // Destination EquipDescVector state
    EquipDesc* DstBegin = OutLoadout.Begin;
    unsigned int DstSize = 0;
    unsigned int DstCap = 0;
    if (DstBegin != nullptr) {
        DstSize = (unsigned int)(OutLoadout.End - DstBegin);
        DstCap = (unsigned int)(OutLoadout.Cap - DstBegin);
    }
    if (SrcCount <= DstSize) {
        // Branch 1: source fits within current destination size — copy and shrink
        for (unsigned int I = 0; I < SrcCount; I++) {
            DstBegin[I] = SrcBegin[I];
        }
        if (SrcCount > 0) {
            OutLoadout.End = DstBegin + SrcCount;
        } else {
            OutLoadout.End = DstBegin;
        }
        return 0;
    }
    if (SrcCount <= DstCap) {
        // Branch 2: source fits within destination capacity — copy existing, construct new
        for (unsigned int I = 0; I < DstSize; I++) {
            DstBegin[I] = SrcBegin[I];
        }
        // Placement-new construct the additional items beyond old size
        for (unsigned int I = DstSize; I < SrcCount; I++) {
            new (&DstBegin[I]) EquipDesc(SrcBegin[I]);
        }
        OutLoadout.End = DstBegin + SrcCount;
        return 0;
    }
    // Branch 3 (module storage..06d5e71a): need more capacity. the routine (operator
    // delete) on the old Begin with no null test, operator new(SrcCount * 0x20), Begin =
    // the new block, then vector<EquipDesc>::_Ucopy of the source into it and End = Cap =
    // its result. SrcCount > DstCap >= 0 here, so the block is never empty. ::operator
    // new / ::operator delete: the same pair every other EquipDescVector buffer uses,
    // since ~EquipDescVector (Common 0x06269110) releases Begin with ::operator delete.
    ::operator delete(DstBegin);
    EquipDesc* NewBuf = static_cast<EquipDesc*>(::operator new(SrcCount * sizeof(EquipDesc)));
    OutLoadout.Begin = NewBuf;
    for (unsigned int I = 0; I < SrcCount; I++) {
        new (&NewBuf[I]) EquipDesc(SrcBegin[I]);
    }
    OutLoadout.End = NewBuf + SrcCount;
    OutLoadout.Cap = NewBuf + SrcCount;
    return 0;
}

} // namespace pub -- the save-file family below is not in pub::

// ===========================================================================
// The save-file writer family: baseline the routine, the routine, the routine
// and the routine, all read off Server (there is no analysis
// analysis of the routine at all -- 2123 instructions, complexity "huge").

// REWRITTEN 2026-09-17 (wave 17). The previous port of the routine was built
// from a `.c`-era reading and named four keys baseline does not write while
// missing eight that it does. Every key name, every format string and every
// source field below is quoted from the listing at the address in its comment.
// ===========================================================================

// ---------------------------------------------------------------------------
// Server_GetAccountsRoot -- baseline the routine (module storage .. module storage).

// CALL [->COMMON.DLL::GetUserDataPath]     ; into a 0x104 local
// CALL [->COMMON.DLL::IsMPServer]
// PUSH .data:s_Accts\MultiPlayer_06d8d308  ; = "Accts\\MultiPlayer"
// PUSH .data:s_Accts_06d8d200              ; = "Accts"
// CALL [->MSVCRT.DLL::_makepath]
// MOV  EAX,.data:the field              ; returns the GLOBAL buffer

// baseline really does hand back a pointer to one static 0x104 buffer, which is
// why the serializer can use the result as _makepath's `dir` and then overwrite
// the local it built the user-data path in. Kept as-is.
// ---------------------------------------------------------------------------
static char GAccountsRoot[260];

// , see server_savefile.h.
char GServerLastSavePath[0x104];

// De-staticed 2026-09-24 (claude:srv-save): the save wrappers in
// server_savegame.cpp use it too (declared in server_savefile.h).
char const* Server_GetAccountsRoot() {
    char UserDataPath[260];
    GetUserDataPath(UserDataPath);
    _makepath(GAccountsRoot, nullptr, UserDataPath,
              IsMPServer() ? "Accts\\MultiPlayer" : "Accts", nullptr);
    return GAccountsRoot;
}

// ---------------------------------------------------------------------------
// Server_AccountDirName -- baseline the routine (module storage .. module storage),
// the MP account-directory name: `sprintf(out, "%02x-%08x", wcslen(name), crc)`
// (.rdata:s_%02x-%08x_06d6aec0, pushed at module storage).

// The CRC is the same Rocksoft-model engine Common.dll's CFLIDMaker uses, but a
// SECOND instance with its own parameters, configured by the branch:
// MOV dword [.data:the field],0x20      ; register width  = 32
// MOV dword [.data:the field],ESI       ; ESI = 0x1f, crc width = 31
// MOV dword [.data:the field],0x8005    ; polynomial
// MOV dword [.data:the field],EBP       ; EBP = 0, XorIn  = 0
// MOV dword [.data:the field],EBP       ;          XorOut = 0
// MOV byte  [.data:the field],0x1       ; reflect-in
// MOV byte  [.data:the field],0x1       ; table path
// The table build at module storage .. module storage is the textbook reflected
// MSB-first table for that width/poly, and the per-byte step the hash loop takes
// with both flags set is the single branch at module storage:
//   ECX = (crc & 0xff) ^ byte ; ECX = Table[ECX] ; crc = (crc >> 8) ^ ECX

// The FINALIZE at module storage is where this differs from CreateID and must not
// be copied from it. With reflect-in set (module storage JNZ) baseline runs
// EBX = 0x20 (register width)
// EAX = 1 << (0x1f - 1)          ; 0x40000000
// ESI = (EAX + EAX - 1) | 1      ; 0x7fffffff, the width mask
// ESI &= (XorOut ^ crc)
// loop: EDI |= (ESI & 0xff) << (EBX - EDX); EDX += 8; ESI >>= 8
// and then prints EDI **unchanged** -- no `>> (32 - width)` fold-back and no
// top bit, both of which CFLIDMaker::FinalizeStaticId does apply. So
// the account hash is byteswap32(crc & 0x7fffffff), not a CreateID value.

// The input is the LOWERCASED WIDE string, hashed two bytes per code unit:
// _wcsdup, module storage _wcslwr, module storage
// `LEA ECX,[EAX + EAX*0x1]` = 2 * wcslen as the byte count.
// ---------------------------------------------------------------------------
namespace {
    const unsigned int AccountCrcWidth = 31;
    const unsigned int AccountCrcPoly = 0x8005;
    const unsigned int AccountCrcRegisterWidth = 32;

    unsigned int GAccountCrcTable[256];
    bool bAccountCrcTableReady = false;

    // and module storage -- the same bit-overlay reflect
    // CFLIDMaker uses, spelled out again here because this engine is a separate
    // instance in Server.dll and must not reach into Common.dll's private copy.
    unsigned int AccountCrcReflect(unsigned int Value, unsigned int BitCount) {
        unsigned int Result = Value;
        unsigned int Source = Value;
        for (unsigned int BitIndex = 0; BitIndex < BitCount; ++BitIndex) {
            unsigned int Mask = 1u << ((BitCount - BitIndex) - 1u);
            if ((Source & 1u) == 0)
                Result &= ~Mask;
            else
                Result |= Mask;
            Source >>= 1;
        }
        return Result;
    }

    void AccountCrcBuildTable() {
        if (bAccountCrcTableReady)
            return;
        unsigned int const TopBit = 1u << (AccountCrcWidth - 1u);
        unsigned int const Mask = ((TopBit - 1u) * 2u + 1u) | 1u;
        for (unsigned int Index = 0; Index < 256; ++Index) {
            // reflect-in reflects the table index byte first.
            unsigned int Work = AccountCrcReflect(Index, 8) & 0xffu;
            // SHL EAX,CL with CL = width - 8.
            Work <<= (AccountCrcWidth - 8u);
            for (int Bit = 0; Bit < 8; ++Bit) {
                if ((Work & TopBit) != 0)
                    Work = (Work << 1) ^ AccountCrcPoly;
                else
                    Work <<= 1;
            }
            // reflect-out folds the entry back.
            Work = AccountCrcReflect(Work, AccountCrcWidth);
            GAccountCrcTable[Index] = Work & Mask;
        }
        bAccountCrcTableReady = true;
    }
}

// Server_AccountCrc -- the reset/update/finalize sequence every user of the
// engine runs: `the field = the field` inline (XorIn = 0),
// (update over a byte block; the routine is the same step for one
// byte) and the routine (finalize). Only the reflect-in + table branch of those
// methods is live: the field / the field are written solely by the static
// initializer at module storage / module storage (both 1), so the other three
// per-byte branches and the non-reflected finalize are unreachable in baseline.
// Declared in server_savefile.h.
unsigned int Server_AccountCrc(unsigned char const* Bytes, unsigned int ByteCount) {
    AccountCrcBuildTable();

    unsigned int Crc = 0; // XorIn, module storage
    for (unsigned int I = 0; I < ByteCount; ++I)
        Crc = GAccountCrcTable[(Crc & 0xffu) ^ Bytes[I]] ^ (Crc >> 8);

    // reflect path (= module storage / module storage inlined in
    // ): mask with XorOut (0), then the byte-gather loop.
    unsigned int const TopBit = 1u << (AccountCrcWidth - 1u);
    unsigned int const Mask = ((TopBit - 1u) * 2u + 1u) | 1u;
    unsigned int Remain = Crc & Mask;
    unsigned int Folded = 0;
    for (unsigned int Shift = 8; Shift <= AccountCrcRegisterWidth; Shift += 8) {
        Folded |= (Remain & 0xffu) << (AccountCrcRegisterWidth - Shift);
        Remain >>= 8;
    }
    return Folded;
}

// De-staticed 2026-09-24 (claude:srv-save), declared in server_savefile.h.
void Server_AccountDirName(char* Out, wchar_t const* AccountName) {
    // no null check in baseline; _wcsdup/wcslen take the name as is.
    unsigned int const Length = static_cast<unsigned int>(wcslen(AccountName));

    // / module storage -- _wcsdup then _wcslwr. towlower on a
    // local copy is the same transform without the CRT allocation.
    std::wstring Lowered;
    Lowered.reserve(Length);
    for (unsigned int I = 0; I < Length; ++I)
        Lowered.push_back(static_cast<wchar_t>(towlower(AccountName[I])));

    // 2 * wcslen bytes of the lowercased copy.
    unsigned int const Folded = Server_AccountCrc(
        reinterpret_cast<unsigned char const*>(Lowered.c_str()), Length * 2u);

    sprintf(Out, "%02x-%08x", Length, Folded);
}

// ---------------------------------------------------------------------------
// Server_WriteCostume -- baseline the routine (module storage .. module storage).

// Four identical blocks, one per body part, each `find_bodypart(TypeId, PartId)`
// then `bodypart::get_name()` then a "<prefix><part>" key:
//   part ids   module storage  MOV ESI,[EAX + 0x4]   -> PartIds[1], TypeId 0, "%sbody"
// MOV EDI,[EAX]         -> PartIds[0], TypeId 1, "%shead"
// MOV EDI,[EAX + 0x8]   -> PartIds[2], TypeId 2, "%slefthand"
// MOV EDI,[EAX + 0xc]   -> PartIds[3], TypeId 3, "%srighthand"
//   formats    .rdata:s_%sbody_06d6aef0, s_%shead_06d6aee8,
//              s_%slefthand_06d6aedc, s_%srighthand_06d6aed0
// A zero part id, a missed find_bodypart or a null name skips the line entirely
// (module storage / module storage / module storage and the three copies of it).

// ARGUMENTS. `RET 0xc` is baseline's exit, so three dword stack arguments and no
// thiscall `this` -- module storage `MOV EAX,dword ptr [ESP + 0xc]` takes the
// THIRD one as the Costume. The first is not a FILE*: module storage
// `LEA EAX,[ESP + 0x40]` resolves to the serializer's own writer object, and
// `MOV ECX,[ESP + 0x120]` / `MOV EAX,[ECX]` / module storage
// `ff5014 CALL dword ptr [EAX + 0x14]` dispatches slot +0x14 on it, which is
// WriteString. FIXED 2026-09-17 (wave 18): that object is CIniFileSave now
// (renamed SaveGame_Writer 2026-09-25 after its RTTI, .?AUSaveGame_Writer@@).

// THE PREFIX. There is no "com_body" string anywhere in Server.dll -- searching
// the mapped image for it returns nothing. The keys are built at run time from
// .rdata:s_%sbody_06d6aef0 and its three siblings with a prefix argument, which
// is "" (.rdata:the field, four zero bytes) for the worn costume and "com_"
// (module storage) for the comm costume.
// ---------------------------------------------------------------------------
static void Server_WriteCostume(ISave& Save, char const* Prefix, Costume const& Cost) {
    CostumeDescriptions const* Descriptions = GetCostumeDescriptions();
    if (Descriptions == nullptr)
        return;

    struct FCostumePart {
        int TypeId;
        int PartSlot;
        char const* Format;
    };
    static const FCostumePart Parts[4] = {
        { 0, 1, "%sbody" },
        { 1, 0, "%shead" },
        { 2, 2, "%slefthand" },
        { 3, 3, "%srighthand" },
    };

    for (int I = 0; I < 4; ++I) {
        unsigned int const PartId = Cost.PartIds[Parts[I].PartSlot];
        if (PartId == 0)
            continue;
        char const* PartName = Descriptions->get_part_name(Parts[I].TypeId, PartId);
        if (PartName == nullptr)
            continue;
        char Key[128];
        sprintf_s(Key, Parts[I].Format, Prefix);
        Save.WriteString(Key, PartName);
    }
}

// ---------------------------------------------------------------------------
// Server_FormatMissionLogEntry -- baseline the routine
// (module storage .. module storage).

// ARGUMENTS. `RET 0x8` at module storage -- two dword stack arguments, plus an
// ECX that is saved at module storage and only ever handed back to the recursive
// call at module storage. It is never dereferenced, so this port drops it.
//   param_1 [ESP+0x14]  module storage -- the destination char buffer
//   param_2 [ESP+0x18]  module storage -- the record

// WHAT IT IS. An earlier wave filed this function as the cheat-report gate. It
// is nothing of the kind: it touches no PlayerData slot at all, and it is a
// recursive list formatter for one FmtStr.

// MOV  EAX,dword ptr [ESI]        ; FmtStr::Head
// MOV  EAX,dword ptr [EAX]        ; Val::Next -- count the chain
// MOV  EAX,dword ptr [ESI + 0x4]  ; FmtStr::MadLibId
// PUSH .rdata:s_%u,_%u_06d6b628   ; "%u, %u"      <- id, count
// MOVZX ECX,byte ptr [ESI + 0xa]  ; Val::Flags
// MOVZX EDX,word ptr [ESI + 0x8]  ; Val::TypeTag
// MOV  EAX,dword ptr [ESI + 0x4]  ; Val::Data
// PUSH .rdata:s_,_%u,_%u,_%u_06d6b618  ; ", %u, %u, %u"
// CMP  word ptr [ESI + 0x8],0x21  ; nested FmtStr?
// MOV  byte ptr [ECX],0x2c        ; a BARE ',' -- no space
// CALL the routine               ; recurse on Val::Data
// MOV  ESI,dword ptr [ESI]        ; Val::Next

// The strlen before each append is the inlined `EDI = buf; SCASB.REPNE; NOT ECX;
// DEC ECX; ADD ECX,EBX` at module storage .. module storage and again at
// .. module storage.

// VERIFIED ON DATA, not just on the listing. The fifty saved character files on
// this machine hold 3234 `log =` lines. 3231 of them match
// `id, count, {data, tag, flags} x count` exactly. The other three match only if
// the 0x21 rule is applied -- and in those three the separator before the nested
// record really is a bare comma with no space, exactly as module storage writes
// it (`..., 33, 0,327682, 2, ...`).

// THE BUFFER. The caller's destination is the 0x1000-byte stack buffer of
// at [ESP + 0x6b8] (module storage `LEA EDX,[ESP + 0x6bc]` after one
// push); the next local the function addresses is [ESP + 0x16b8]. baseline writes
// it with plain sprintf at the current end and checks nothing -- neither the
// buffer size, nor a null Val::Data on a 0x21 node, nor the chain against the
// count it took first. The port does the same.
// ---------------------------------------------------------------------------
static void Server_FormatMissionLogEntry(char* Out, FmtStr const& Entry) {
    // .. module storage -- count the chain into EDI.
    int Count = 0;
    for (FmtStr::Val const* Node = Entry.Head; Node != nullptr; Node = Node->Next)
        ++Count;

    // .. module storage -- sprintf(Out, "%u, %u", MadLibId, Count),
    // written at Out itself.
    sprintf(Out, "%u, %u", Entry.MadLibId, Count);

    // ESI = Head; module storage TEST EDI,EDI / JLE -- the loop runs
    // Count times (06d4ccb4 DEC / 06d4ccb9 JNZ), following Val::Next unchecked.
    FmtStr::Val const* Node = Entry.Head;
    for (int Remaining = Count; Remaining > 0; --Remaining) {
        // .. module storage -- appended at strlen(Out).
        sprintf(Out + strlen(Out), ", %u, %u, %u",
                Node->Value,   // the raw payload dword, whatever the tag
                (unsigned int)Node->TypeTag,
                (unsigned int)Node->Flags);

        // 0x21 is the tag common_text.h already documents as
        // "nested FmtStr", so Val::Subtree is live and the record recurses:
        // writes a bare ',' at the end, module storage recurses on
        // the byte after it.
        if (Node->TypeTag == 0x21) {
            char* const Tail = Out + strlen(Out);
            *Tail = ',';
            Server_FormatMissionLogEntry(Tail + 1, *Node->Subtree);
        }
        Node = Node->Next;
    }
}

// ---------------------------------------------------------------------------
// Server_SaveSinglePlayerState -- baseline the routine (module storage ..
// ), cdecl (ISave*), plain RET. The writer half of the routine
// (Server_LoadSinglePlayerState, IServerImpl.cpp); its only caller is the
// serializer below, in single player (module storage).
// Writer->vtbl+0x04 (WriteSection)("Time")        module storage
// FLD the field / FADD the field / FSTP dword -- the clock
//             (the routine's sum) narrowed to float
// Writer->vtbl+0x08 (WriteFloat)("seconds", that) module storage
// Reputation::Save(Writer)
// Writer->vtbl+0x00 (GetFile)() ; the routine(0x41, FILE*)
// ---------------------------------------------------------------------------
static void Server_SaveSinglePlayerState(ISave* Writer) {
    Writer->WriteSection("Time");
    Writer->WriteFloat("seconds", static_cast<float>(g_ElapsedTimeAccum + GServerTimeBase));
    ::Reputation::Save(Writer);
    Server_ControllerDispatch(0, pub::Controller::EMessage::SaveGame, Writer->GetFile());
}

// Server_CopyCostumeCounted -- the Costume copy the MP record refresh in
// inlines twice (module storage .. 06d4e68e and module storage ..
// ): the four part ids, the accessory count, then only `count`
// accessory ids under a SIGNED loop (TEST/JLE, INC/CMP/JL). Slots past the
// count keep what the destination held. Added 2026-09-24
// (claude-code:w5-server-06d4).
static void Server_CopyCostumeCounted(Costume& Dst, Costume const& Src) {
    Dst.PartIds[0] = Src.PartIds[0];
    Dst.PartIds[1] = Src.PartIds[1];
    Dst.PartIds[2] = Src.PartIds[2];
    Dst.PartIds[3] = Src.PartIds[3];
    Dst.AccessoryCount = Src.AccessoryCount;
    int const Count = static_cast<int>(Src.AccessoryCount);
    for (int I = 0; I < Count; ++I)
        Dst.AccessoryIds[I] = Src.AccessoryIds[I];
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// PlayerData_SerializeToFile -- baseline the routine
// (module storage .. module storage, listing lines 190145-192429).

// SIGNATURE. analysis's 15-parameter prototype is fiction. Every exit is `RET 0xc`
// (module storage, module storage, module storage), so there are exactly THREE
// dword stack arguments plus the thiscall `this`, and the return is AL. The
// 0x2044 at module storage is __chkstk's frame size, not an argument. The three
// arguments are pinned by their uses:
//   param_1 [EBP+0x08] module storage -- _makepath's `fname`, so a char* BASE
//                                        FILE NAME, not a path and not a FILE*
//   param_2 [EBP+0x0c] module storage -- WideToHex'd into the `description` key,
//                                        so a wchar_t*
//   param_3 [EBP+0x10] module storage -- printed with "%u" as `descrip_strid`
// and the autosave call site agrees: module storage pushes 0 (the strid), then
// the wide "<date> <time>" swprintf'd at module storage (the description), then
// a char buffer at EBP+0x20c (the file name).

// WHAT IT IS. A save that REFUSES TO RUN when the cheat check fires -- the gate
// and the writer are one function in baseline, and the port had them as two that
// never called each other.

// STILL DEVIATING, every one an open TODO and not a decision:
//  (1) CLOSED 2026-09-17 (wave 18). The ini-writer object is SaveGame_Writer in
//      server_savefile.cpp now, the error latch at module storage is
//      SaveGame_Writer::HadWriteError, and the container below is Close().
//  (2) CLOSED 2026-09-17 (wave 18), AND THE OLD TEXT WAS WRONG ON ONE POINT.
//      The "FLS1" container is not multiplayer-only. baseline encodes on every
//      path: module storage .. module storage runs the routine / the routine,
//      fopen "wb" (module storage = `77 62 00`), a bare fprintf of the four
//      bytes at module storage (`46 4c 53 31`), fwrite and fclose, with no
//      SinglePlayer test between the latch and it. What IS multiplayer-only is
//      the destination: outside single player the text goes to `<accts>\temp.fl`
//      (.rdata:s_%s\temp.fl_06d6b6f4, sprintf'd at module storage, chosen at
// ) and module storage .. module storage unlinks the real
//      path and renames the temp onto it. The cipher is
//      out[i] ^= (("Gene"[i & 3] + (unsigned char)i) | 0x80), keyed on the first
//      four bytes of module storage; it decodes forty-nine of the fifty saved
//      character files on this machine, all of them single-player.
//  (3) CLOSED 2026-09-24 (claude:srv-save-2). The MP base-record copy at
// .. module storage and the base_hull_status /
//      base_collision_group / base_cargo / base_equip keys at module storage /
// / module storage / module storage are ported below. The
//      "+0x468 / +0x46c / +0x478" offsets are NODE offsets: the record starts at
//      node + 0x20c, so they are FSavedCharacter's BaseHullStatus (+0x25c),
//      BaseEquipment (+0x260) and BaseCollisionGroups (+0x26c), which the
//      parser already filled. The key is +0x64 alone (the routine pairs it
//      with a default record from the routine for the map's unique insert).
//  (4) CLOSED 2026-09-17 (wave 18). The chain is a FmtStr's Val list and
//      PlayerData::mission_log is a std::list<FmtStr> now, so the key round
//      trips. See Server_FormatMissionLogEntry above.
//  (5) CLOSED 2026-09-24 (claude:srv-save-2): the routine(4, ...) at
// and the routine at module storage are called below.
//  (6) CLOSED 2026-09-24 (claude:srv-charinfo): the slots baseline dispatches
//      (ISystem vtable+0x8 at module storage, IBase vtable+0x4 at module storage /
// ) are plain nickname getters, and the port reads that field.
//  (7) CLOSED 2026-09-24 (claude-code:w6-rpc): the MP position source is the
//      CRemotePhysicsController __RTDynamicCast at module storage, ported below
//      against the class Common now really builds (src/include/common_server.h).
// ---------------------------------------------------------------------------
bool __cdecl PlayerData_SerializeToFile(PlayerSlot& Player, char const* FileName,
                                       wchar_t const* Description,
                                       unsigned int DescripStrId) {
    // No null test on FileName: the prologue (module storage .. 06d4cd01) goes
    // straight to the cheat gate, and the name only reaches _makepath
    // (module storage). Guard removed 2026-09-24 (claude-code:w5-server-06d4).

    // -----------------------------------------------------------------------
    // .. module storage -- THE CHEAT GATE. baseline runs it first
    // and returns AL = 0 (save refused) from inside it. PlayerData_CheckForCheats
    // in server_classes.cpp is the 1:1 port of this block, including the
    // already-reported latch at +0x34C; it was never called from here until now.
    // -----------------------------------------------------------------------
    if (PlayerData_CheckForCheats(Player))
        return false;

    // -----------------------------------------------------------------------
    // .. module storage -- a dead MP ship is put back to the
    // last-base loadout before it is written. SinglePlayer() skips it
    // (module storage); otherwise FLD the hull (+0x268), FCOMP double
    // .rdata:the field (PE bytes: eight zeros = 0.0), TEST AH,0x41 / JP skips
    // when C0 and C3 agree -- both clear (hull > 0) or both set (unordered) --
    // so the call runs for hull < 0 or hull == 0 only. Added 2026-09-24
    // (claude-code:w5-server-06d4).
    // -----------------------------------------------------------------------
    if (!SinglePlayer() && Player.relative_health <= 0.0f)
        PlayerData_ReloadShipData(Player);

    // -----------------------------------------------------------------------
    // .. module storage -- the pending-visit flush. baseline walks
    // the queue at +0x3B8 (count from the routine, entries from [EBX + 0x3b8],
    // stride 8) and feeds each one to the routine with its flags ORed with 8,
    // then empties the queue with the routine. PlayerData_FlushPendingVisits is
    // that function; the `visit` key below reads the COMMITTED map at +0x3A0, so
    // the flush has to happen before it or the newest systems are lost.
    // -----------------------------------------------------------------------
    PlayerData_FlushPendingVisits(Player);

    // -----------------------------------------------------------------------
    // .. module storage -- the destination, which baseline builds
    // itself. This used to be the caller's invention.
    //   name    IsMPServer() ? the routine(buf, slot)      module storage
    //                        : wcstombs(buf, slot, 0x104)   module storage
    //           -- the wide account name really does live at PlayerData+0x00.
    //   dir     _makepath(tmp, 0, the routine(), name, 0)  module storage
    //   final   _makepath(tmp, 0, tmp, param_1,             module storage
    //                     IsMPServer() ? 0 : "fl")
    //           -- the extension operand at module storage is `AND EAX,imm32`
    //           (opcode 0x25), i.e. the ADDRESS module storage, which holds the
    //           two bytes "fl". In MP param_1 already carries its own extension.
    //   target  SinglePlayer() ? that path : "<accts>\temp.fl"  module storage
    // -----------------------------------------------------------------------
    char AccountDirName[260];                                               // Stack[0x1f4], no pre-init
    if (IsMPServer()) {
        Server_AccountDirName(AccountDirName, Player.AccountName);
    } else {
        // .. module storage -- wcstombs(buf, slot, 0x104) and
        // nothing else: no return-value test and no explicit terminator. The
        // account name is a short NUL-terminated wide string, so wcstombs copies
        // its terminator itself. KNOWN baseline UB: a 260+ character name, or one
        // that does not convert (return -1), leaves the buffer unterminated.
        wcstombs(AccountDirName, Player.AccountName, 0x104);
    }

    char AccountDir[260];
    _makepath(AccountDir, nullptr, Server_GetAccountsRoot(), AccountDirName, nullptr);

    char SavePath[260];
    _makepath(SavePath, nullptr, AccountDir, FileName, IsMPServer() ? nullptr : "fl");

    char TempPath[260];
    sprintf_s(TempPath, "%s\\temp.fl", Server_GetAccountsRoot());

    char const* const WritePath = SinglePlayer() ? SavePath : TempPath;

    // -----------------------------------------------------------------------
    // .. module storage -- THE WRITER OBJECT. baseline builds it on
    // its own stack at ESP+0x24: the routine zero-initialises the {FILE*, buffer}
    // pair, module storage stamps the abstract base's vtable (06d6b5c0, seven
    // purecall slots) and module storage overwrites it with the concrete one
    // (06d6b5a0), then the path string is assigned and fopen runs.

    // CORRECTED 2026-09-17 (wave 18): the mode is "w+t", not "w". The previous
    // pass read .rdata:the field off the listing, which prints `;= 77h  w`
    // because it shows only the first byte of the operand. The real bytes in
    // EXE\Server.dll at that address are `77 2b 74 00`. The "+" is what lets
    // Close() read the file back for encoding, and the "t" is why every real
    // save this machine holds is LF-only.
    // -----------------------------------------------------------------------
    SaveGame_Writer Writer;
    if (!Writer.Open(WritePath)) {
        // .. module storage -- FDUMP(0x100002, module storage,
        // = PlayerDB.cpp, 0xad4, path), then AL = 0. (Was an
        // OutputDebugString with invented text until 2026-09-24.)
        FDUMP(0x100002, "%s(%d) : *** WARNING: Failed to open %s for writing",
              "Server\\PlayerDB.cpp", 0xad4, WritePath);
        return false;
    }

    // safe_strcpy(the field, 0x104, target): baseline keeps
    // the path it is writing in a global; the routine copies that file to
    // AutoSave.fl afterwards (module storage, PlayerData_MissionSave).
    safe_strcpy(GServerLastSavePath, 0x104, WritePath);

    // baseline writes most keys straight to the FILE* (`MOV EDX,[ESP + 0x30]`
    // reads the writer's +0x04 and hands it to fprintf) and only the costume
    // blocks and the single-player hook go through the vtable. This port keeps
    // that split rather than routing everything through the object, because
    // routing everything would silently reorder nothing but would stop the line
    // numbers here matching the listing.
    FILE* const Out = Writer.File;                                  // [writer+4], e.g. module storage

    // fprintf(f, "\n[%s]\n", "Player") (module storage).
    fprintf(Out, "\n[%s]\n", "Player");

    // .. module storage -- `description`, only when param_2 is
    // non-null, hex-encoded with COMMON.DLL::WideToHex.
    if (Description != nullptr) {
        char HexBuf[2048];
        WideToHex(Description, HexBuf);
        fprintf(Out, "%s = %s\n", "description", HexBuf);
    }

    // .. module storage -- `descrip_strid`, only when param_3 is
    // non-zero, "%s = %u\n" (module storage).
    if (DescripStrId != 0)
        fprintf(Out, "%s = %u\n", "descrip_strid", DescripStrId);

    // a bare newline (.rdata:the field is the single byte
    // 0x0a, passed as fprintf's whole format string).
    fprintf(Out, "\n");

    // -----------------------------------------------------------------------
    // .. module storage -- `tstamp`.
    //   GetLocalTime(&st) / SystemTimeToFileTime(&st, &ft)
    //   sprintf(buf, "%u,%u", ft.dwHighDateTime, ft.dwLowDateTime)
    // CORRECTED: the port had the two halves the other way round. The pushes are
    // MOV EAX,dword ptr [ESP + 0xcc]   ; ft.dwLowDateTime
    // MOV ECX,dword ptr [ESP + 0xd0]   ; ft.dwHighDateTime
    // PUSH EAX                          ; pushed FIRST  -> LAST arg
    // PUSH ECX                          ; pushed SECOND -> first %u
    // so cdecl's reverse order makes the HIGH half the first conversion.
    // No space after the comma (.rdata:s_%u,%u_06d6b6b8).
    // -----------------------------------------------------------------------
    {
        SYSTEMTIME SysTime;
        FILETIME FileTime;
        GetLocalTime(&SysTime);
        SystemTimeToFileTime(&SysTime, &FileTime);
        char TstampBuf[64];
        sprintf_s(TstampBuf, "%u,%u", FileTime.dwHighDateTime, FileTime.dwLowDateTime);
        fprintf(Out, "%s = %s\n", "tstamp", TstampBuf);
    }

    // -----------------------------------------------------------------------
    // .. module storage -- the reputation block: ONE call to
    // COMMON.DLL::Reputation::Vibe::Get and four keys off its out-parameters.

    // CALL [->COMMON.DLL::Reputation::group_count]
    // SHL EAX,0x3 / operator new          ; Relation[count], stride 8
    // CALL [->COMMON.DLL::Reputation::Vibe::Get]
    // with the eight arguments recovered from the LEA/PUSH pairs at
    // .. module storage (base P = ESP at module storage):
    //   GroupId     &[EBX + 0x318]   = PlayerData::rep_vibe
    //   Affiliation  P+0xe8          Rank        P+0x9c
    //   Count        P+0x43 (byte)   Relations   the new'd block
    //   Name1        P+0xb4          Name2       P+0xcc
    //   DisplayName  P+0xec

    // FOUR CORRECTIONS against the old port, all of them fabrication:
    //   * `name` is WideToHex of Vibe::Get's DISPLAY NAME out-parameter
    //     (module storage reads P+0xec, module storage WideToHex,
    // key module storage = "name"). The port wrote the
    //     character name there instead.
    //   * there is no `vibe_name` key anywhere in this function.
    //   * there is no `affiliation` key and no `affiliation_name` key. What
    // writes is module storage = "rank", as "%s = %u\n",
    //     from Vibe::Get's Rank -- overridden in SINGLE PLAYER from
    //     PlayerData+0x284 (module storage `MOV EDX,[EBX + 0x284]`), which is
    //     PlayerData::rank_level. (This wave's brief put that override at +0x142
    //     and called it an affiliation; the bytes say +0x284 and `rank`.)
    //   * the PER-GROUP loop key is `house` (module storage, module storage),
    //     not `rep_group`. `rep_group` (module storage, module storage) is
    //     written exactly ONCE, and only when Affiliation != 0xffffffff
    //     (module storage `CMP dword ptr [ESP + 0xc4],-0x1`), carrying the
    //     affiliation group's own nickname.
    // -----------------------------------------------------------------------
    // / module storage -- operator new(group_count() * 8), kept
    // in [ESP+0x8c] until module storage frees it: the MP record refresh at the
    // end of this function reads the array, its byte count ([ESP+0x1f]) and
    // the rank ([ESP+0x78]) again, so all three live at function scope.
    // (claude-code:w5-server-06d4, 2026-09-24: the port used to delete the array
    // at the end of this block and guard Vibe::Get on a non-null array and a
    // non-negative vibe; baseline calls it unconditionally at module storage.)
    unsigned int const GroupCount = GlobalReputation::group_count();
    GlobalReputation::Relation* const Relations = static_cast<GlobalReputation::Relation*>(
        operator new(GroupCount * sizeof(GlobalReputation::Relation)));
    unsigned int Affiliation = 0xffffffffu;
    unsigned int Rank = 0;
    unsigned char RelationCount = 0;
    FmtStr Name1;                                                          // ..
    FmtStr Name2;                                                          // ..
    {
        wchar_t const* DisplayName = nullptr;

        GlobalReputation::Vibe::Get(Player.rep_vibe, Affiliation, Rank, RelationCount,
                                    Relations, Name1, Name2, DisplayName);

        // .. module storage -- `name`, unconditionally:
        // MOV  EDX,[ESP + 0x10c]   ; the P+0xec out-parameter
        // CALL WideToHex(EDX, buf)
        // fprintf(f, "%s = %s\n", "name", buf)
        // There is no test anywhere between the call and the fprintf.
        // KNOWN baseline UB: nothing in this function writes P+0xec before the call
        // (module storage .. 06d4d1f0 initialise only the two FmtStr locals at
        // P+0xb4 and P+0xcc), and Reputation::Vibe::Get writes its out-parameters
        // only on a hit -- its miss path (module storage JZ -> 06320c9e) returns
        // -2 with all of them untouched. On a miss baseline hands an uninitialised
        // stack word to WideToHex; the port hands it the nullptr it starts with,
        // and WideToHex faults on it the same way. Not guarded: the path is
        // baseline's. (The same miss also leaves Affiliation, Rank and the count
        // byte uninitialised in baseline; the port starts them at 0xffffffff, 0, 0.)
        {
            char HexBuf[2048];
            WideToHex(DisplayName, HexBuf);
            fprintf(Out, "%s = %s\n", "name", HexBuf);
        }

        // .. module storage -- `rank`, with the SP override.
        if (SinglePlayer())
            Rank = Player.rank_level;
        fprintf(Out, "%s = %u\n", "rank", Rank);

        // .. module storage -- one `house` line per Relation.
        // CALL [->COMMON.DLL::Reputation::get_nickname]
        // FLD  float ptr [ESI + 0x4]              ; Relation::Feeling
        // sprintf(buf, "%g, %s", feeling, nick)   (module storage)
        // fprintf(f, "%s = %s\n", "house", buf)
        // The loop bound is the BYTE counter Vibe::Get filled, re-read every
        // iteration at module storage.
        for (unsigned int I = 0; I < static_cast<unsigned int>(RelationCount); ++I) {
            TString<16> GroupNick;
            GroupNick.Length = 0;
            GroupNick.Data[0] = '\0';
            GlobalReputation::get_nickname(GroupNick, Relations[I].GroupId);
            char HouseBuf[256];
            sprintf_s(HouseBuf, "%g, %s", static_cast<double>(Relations[I].Feeling),
                      GroupNick.Data);
            fprintf(Out, "%s = %s\n", "house", HouseBuf);
        }

        // .. module storage -- the single `rep_group` line.
        if (Affiliation != 0xffffffffu) {
            TString<16> AffilNick;
            AffilNick.Length = 0;
            AffilNick.Data[0] = '\0';
            GlobalReputation::get_nickname(AffilNick, Affiliation);
            fprintf(Out, "%s = %s\n", "rep_group", AffilNick.Data);
        }
    }

    // -----------------------------------------------------------------------
    // .. module storage -- four integer keys, in BASELINE's order
    // (`money` first; the port had it last).
    //   money              [EBX + 0x31c]  module storage
    //   num_kills          [EBX + 0x334]  module storage
    //   num_misn_successes [EBX + 0x338]  module storage
    //   num_misn_failures  [EBX + 0x33c]  module storage
    // all through "%s = %d\n" (module storage).
    // -----------------------------------------------------------------------
    fprintf(Out, "%s = %d\n", "money", Player.cash);
    fprintf(Out, "%s = %d\n", "num_kills", Player.num_kills);
    fprintf(Out, "%s = %d\n", "num_misn_successes", Player.num_mission_successes);
    fprintf(Out, "%s = %d\n", "num_misn_failures", Player.num_mission_failures);

    // bare newline.
    fprintf(Out, "\n");

    // `voice`, the ADDRESS of the inline buffer at +0x2C4.
    fprintf(Out, "%s = %s\n", "voice", Player.voice);

    // / module storage -- the two costume blocks. The prefixes are
    // .rdata:the field = "com_" and .rdata:the field = "" (four zero bytes).
    Server_WriteCostume(Writer, "com_", Player.com_costume);
    Server_WriteCostume(Writer, "", Player.costume);

    // bare newline.
    fprintf(Out, "\n");

    // -----------------------------------------------------------------------
    // .. module storage -- `system`, from Universe::get_system of
    // PlayerData+0x3E0. baseline does NOT null-check the result: module storage
    // dereferences it straight into the vtable, and neither does this port.
    // [EDX + 0x8] is Common's SYSTEM vtable (module storage, COL the field)
    // slot 2 = module storage `MOV EAX,[ECX + 0x50]; RET`, and +0x50 is the
    // StringAlloc'd `nickname` (module storage) -- ISystem::Nickname, handed to
    // fprintf unguarded as baseline does.
    // -----------------------------------------------------------------------
    {
        Universe::ISystem const* Sys = Universe::get_system(Player.system_id);
        fprintf(Out, "%s = %s\n", "system", Sys->Nickname);
    }

    // -----------------------------------------------------------------------
    // .. module storage -- the base / position branch.

    // MOV  EAX,[EBX + 0x3e4]                ; ship object id
    // TEST dword ptr [EBX + 0x3ec],0x3fffffff
    // MOV  ECX,[EBX + 0x3f0] / MOV [EBX + 0x3ec],ECX
    //   -- when there is no live ship object AND current_base holds no object
    //      bits, current_base is refilled from last_base_id first. The port used
    //      Player.base_id raw and never did this.
    // CALL [->COMMON.DLL::Universe::get_base]
    // CALL [EDX + 0x4]                      ; the IBase nickname
    // key .rdata:the field = "base"
    // -----------------------------------------------------------------------
    // [ESP+0x58..0x60] and [ESP+0xdc..0xff]: written only by the `pos` /
    // `rotate` arm below, read again by the MP record refresh at the end
    // (module storage .. 06d4e561) whether or not that arm ran.
    //   position     baseline ZEROES it unconditionally before `system`:
    // / 06d4d432 / 06d4d43d store 0 to
    //                [ESP+0x94/0x98/0x9c] with 0x3c bytes of pushes outstanding,
    //                i.e. frame+0x58/0x5c/0x60. So {0,0,0} is baseline, not a guard.
    //   orientation  nothing writes the nine dwords outside the `rotate` arm.
    //                KNOWN baseline UB: with a base, or with no live ship, the MP
    //                refresh copies nine uninitialised stack dwords into the
    //                record; the port copies zeros instead.
    float SavedPos[3] = { 0.0f, 0.0f, 0.0f };
    float SavedOrient[9] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    {
        if (Player.ship_object_id == 0 && (Player.current_base & 0x3fffffffu) == 0)
            Player.current_base = Player.last_base_id;

        Universe::IBase* Base = Universe::get_base(Player.current_base);
        if (Base != nullptr) {
            // [EDX + 0x4] is Common's BASE vtable (module storage) slot 1 =
            // `MOV EAX,[ECX + 0x8]; RET`; +0x8 is the StringAlloc'd
            // `nickname` (module storage). baseline does not test it for null.
            fprintf(Out, "%s = %s\n", "base", Base->Nickname);
            fprintf(Out, "\n");
            // single player jumps straight to `location`.
            // .. module storage -- outside it, the docked state
            // goes into the selected character's record at +0x50: the routine
            // pairs +0x64 with a fresh record (the routine), the routine is
            // the map's unique insert (an existing record is kept), and the node
            // + 0x20c record gets relative_health (+0x268 -> record+0x25c,
            // ), the collision-group list (+0x26c -> +0x26c,
            // inlined list::operator= module storage..06d4d5fa) and the
            // equipment list (+0x278 -> +0x260, list::operator= module storage).
            if (!SinglePlayer()) {
                // insert(pair(key, default)).first == operator[] (find or
                // default-insert, never overwrite).
                PlayerData::FSavedCharacter& BaseRecord =
                    Player.saved_characters[Player.selected_character];
                BaseRecord.BaseHullStatus = Player.relative_health;
                BaseRecord.BaseCollisionGroups = Player.collision_groups;
                BaseRecord.BaseEquipment.Items = Player.equipment.Items;
            }
        } else {
            // -----------------------------------------------------------------
            // .. module storage -- no base: `pos` and `rotate`.
            // Re-traced 2026-09-24 (claude-code:w5-server-06d4).
            // the routine(+0x3e4) = Server_LookupLiveObject
            //                   (the lookup plus the is_destroyed reject); null
            //                   skips both keys and the newline (module storage).
            //                   The port used Server_LookupGameObject plus a
            //                   cobj() test baseline does not have.
            //   position        SinglePlayer(): IObjInspect slot 0 (module storage).
            //                   Otherwise module storage .. 06d4d6aa: cobject()
            //                   (vt+0x150), zero unless (class & 0x503) == 0x503,
            //                   then __RTDynamicCast(that+0x60, IVP_Controller ->
            //                   CRemotePhysicsController) (module storage); on a hit
            //                   with controller+0x388 set the position is
            //                   controller+0xe0, with it clear slot 0; on a miss the
            //                   locals are printed as they are.
            //   orientation     IObjInspect slot 3 (+0xc) -- get_transform, whose
            //                   Rotation is its first 0x24 bytes -- copied nine
            //                   dwords to [ESP+0xdc] (module storage .. 06d4d714).
            //   angles          the routine / ea80 / eaa0 are column extractors
            //                   (flat M[0,3,6] / M[1,4,7] / M[2,5,8]). H =
            //                   sqrt(M0*M0 + M3*M3), FST to a float, compared with
            //                   .rdata:the field (PE bytes 00 00 00 36 =
            //                   1.9073486e-06f) by TEST AH,0x41:
            //                     H > eps : A = atan2(M7, M8), B = atan2(-M6, H),
            //                               C = atan2(M3, M0) (stored as a float)
            //                     else    : A = atan2(-M5, M4), B = atan2(-M6, H),
            //                               C = 0
            //                   each multiplied by .rdata:the field (PE bytes
            //                   e0 2e 65 42 = 57.2957763671875f) and printed
            //                   "%g,%g,%g" as A, B, C (module storage .. 06d4d7fa).
            // FIXED: the port wrote RADIANS and in the order (B, C, A). baseline
            // writes DEGREES in the order (A, B, C) -- and the loader feeds the
            // three values to EulerMatrix, which takes degrees.
            // -----------------------------------------------------------------
            GameObject* const LiveObj = Server_LookupLiveObject(Player.ship_object_id);
            if (LiveObj != nullptr) {
                // The position source, module storage .. module storage (deviations.md
                // "Server.dll wave 17" item 7, closed 2026-09-24 by claude-code:w6-rpc):
                //   single player  -> IObjInspect slot 0, get_position (module storage).
                //   otherwise      -> cobject() (vt+0x150, module storage), kept only when
                //                     (ObjectClass & 0x503) == 0x503 (a CShip), else 0
                //                     (module storage .. 06d4d65e); then
                //                     __RTDynamicCast(ship+0x60, 0, IVP_Controller,
                //                     CRemotePhysicsController, 0) (module storage).
                //                     Hit: sample count (+0x388) non-zero -> the latest
                //                     sample at +0xe0 (module storage); zero -> slot 0 as in
                //                     single player (module storage). Miss: the locals are
                //                     NOT written (module storage JZ 06d4d6ae) and `pos` prints
                //                     the zeros stored at module storage .. 06d4d43d.
                // KNOWN baseline UB, reproduced as a path, not guarded: when cobject() is null
                // or not a CShip, EAX is 0 at module storage and `MOV EDX,[EAX + 0x60]` reads
                // address 0x60 -- baseline faults there, and so does this port.
                if (SinglePlayer()) {
                    Vector const& LivePos = LiveObj->get_position();
                    SavedPos[0] = LivePos.x;
                    SavedPos[1] = LivePos.y;
                    SavedPos[2] = LivePos.z;
                } else {
                    CObject const* const Cobj = LiveObj->cobject();
                    CObject const* const Ship =
                        (Cobj != nullptr &&
                         (static_cast<unsigned int>(Cobj->ObjectClass) & 0x503u) == 0x503u)
                            ? Cobj
                            : nullptr;
                    IVP_Controller* const Rc =
                        static_cast<IVP_Controller*>(Ship->GolemController);
                    CRemotePhysicsController* const Remote =
                        dynamic_cast<CRemotePhysicsController*>(Rc);
                    if (Remote != nullptr) {
                        if (Remote->GetSampleCount() != 0) {
                            Vector const Latest = Remote->GetLatestUpdate();
                            SavedPos[0] = Latest.x;
                            SavedPos[1] = Latest.y;
                            SavedPos[2] = Latest.z;
                        } else {
                            Vector const& LivePos = LiveObj->get_position();
                            SavedPos[0] = LivePos.x;
                            SavedPos[1] = LivePos.y;
                            SavedPos[2] = LivePos.z;
                        }
                    }
                }
                char PosBuf[128];
                sprintf(PosBuf, "%g,%g,%g", static_cast<double>(SavedPos[0]),
                        static_cast<double>(SavedPos[1]),
                        static_cast<double>(SavedPos[2]));
                fprintf(Out, "%s = %s\n", "pos", PosBuf);

                ::Matrix const& LiveOrient = LiveObj->get_transform().Rotation;
                for (int I = 0; I < 9; ++I)
                    SavedOrient[I] = LiveOrient.E[I / 3][I % 3];

                float const* const M = SavedOrient;
                double const DegPerRad = static_cast<double>(57.2957763671875f);
                double const HypotExact = sqrt(static_cast<double>(M[0]) * M[0] +
                                               static_cast<double>(M[3]) * M[3]);
                float const Hypot = static_cast<float>(HypotExact);      // FST
                double AngleA = 0.0;
                double AngleB = 0.0;
                float AngleC = 0.0f;
                if (HypotExact > static_cast<double>(1.9073486328125e-06f)) {
                    AngleA = atan2(static_cast<double>(M[7]), static_cast<double>(M[8]));
                    AngleB = atan2(-static_cast<double>(M[6]), static_cast<double>(Hypot));
                    AngleC = static_cast<float>(atan2(static_cast<double>(M[3]),
                                                      static_cast<double>(M[0])));
                } else {
                    AngleA = atan2(-static_cast<double>(M[5]), static_cast<double>(M[4]));
                    AngleB = atan2(-static_cast<double>(M[6]), static_cast<double>(Hypot));
                    AngleC = 0.0f;
                }
                float const DegA = static_cast<float>(AngleA * DegPerRad); // FSTP float
                double const DegB = AngleB * DegPerRad;
                double const DegC = static_cast<double>(AngleC) * DegPerRad;

                char RotBuf[128];
                sprintf(RotBuf, "%g,%g,%g", static_cast<double>(DegA), DegB, DegC);
                fprintf(Out, "%s = %s\n", "rotate", RotBuf);
                fprintf(Out, "\n");
            }
        }
    }

    // .. module storage -- `location`, only when +0x3F8 is non-zero.
    // This key was missing from the port entirely.
    if (Player.current_location != 0)
        fprintf(Out, "%s = %u\n", "location", Player.current_location);

    // -----------------------------------------------------------------------
    // .. module storage -- ship_archetype / hull_status /
    // collision_group, all three gated on the archetype resolving.
    // (Unchanged from the previous wave, which read these off the listing.)
    // -----------------------------------------------------------------------
    if (Player.ship_archetype != 0) {
        Archetype::Ship* ShipArch = Archetype::GetShip(Player.ship_archetype);
        if (ShipArch != nullptr) {
            // `MOV EAX,[EAX + 0x8]` -- the RESOLVED archetype's
            // Root::Nickname id, through sprintf "%u" (module storage).
            char ArchBuf[32];
            sprintf_s(ArchBuf, "%u", ShipArch->get_id());
            fprintf(Out, "%s = %s\n", "ship_archetype", ArchBuf);

            // FLD [EBX + 0x268] / FCOMP 1.0f -- strictly below 1.
            if (Player.relative_health < 1.0f) {
                fprintf(Out, "%s = %g\n", "hull_status",
                        static_cast<double>(Player.relative_health));
            }

            // .. module storage -- the PLAYER's own list at +0x26C.
            for (std::list<CollisionGroupDesc>::const_iterator It =
                     Player.collision_groups.begin();
                 It != Player.collision_groups.end(); ++It) {
                if (!(It->Health < 1.0f))
                    continue;
                Archetype::CollisionGroup const* Group = ShipArch->get_group_by_id(It->Id);
                if (Group == nullptr)
                    continue;
                char GroupBuf[256];
                sprintf_s(GroupBuf, "%s, %g", Group->ObjName, static_cast<double>(It->Health));
                fprintf(Out, "%s = %s\n", "collision_group", GroupBuf);
            }
        }
    }

    // bare newline on the join point of both archetype skips.
    fprintf(Out, "\n");

    // -----------------------------------------------------------------------
    // .. module storage -- the equipment list, two keys.
    // (Unchanged from the previous wave, which read these off the listing.)
    // -----------------------------------------------------------------------
    for (std::list<EquipDesc>::const_iterator CI = Player.equipment.Items.begin();
         CI != Player.equipment.Items.end(); ++CI) {
        EquipDesc const& Item = *CI;

        // outside single player, mission items are skipped.
        if (!::SinglePlayer() && Item.bMission)
            continue;

        Archetype::Equipment* Equip = Archetype::GetEquipment(Item.ArchId);
        if (Equip == nullptr) continue;
        unsigned int const NicknameId = Equip->Nickname.Id;

        char Line[512];
        if (!Item.bEquipped) {
            if (Item.Health == 1.0f) {
                sprintf(Line, "%u, %d, , , %d", NicknameId, Item.Count, Item.bMission ? 1 : 0);
            } else {
                sprintf(Line, "%u, %d, , %g, %d", NicknameId, Item.Count,
                        static_cast<double>(Item.Health), Item.bMission ? 1 : 0);
            }
            fprintf(Out, "%s = %s\n", "cargo", Line);
        } else {
            if (Item.is_internal()) {
                sprintf(Line, "%u, , %g", NicknameId, static_cast<double>(Item.Health));
            } else {
                sprintf(Line, "%u, %s, %g", NicknameId, Item.Hardpoint,
                        static_cast<double>(Item.Health));
            }
            fprintf(Out, "%s = %s\n", "equip", Line);
        }
    }

    // bare newline after the equipment list.
    fprintf(Out, "\n");

    // -----------------------------------------------------------------------
    // .. module storage -- `last_base` and the base_* keys.

    // SETTLED: `last_base` is NOT the base written above. baseline resolves a
    // SECOND base object from PlayerData+0x3F0 --
    // MOV  EAX,dword ptr [EBX + 0x3f0]
    // CALL [->COMMON.DLL::Universe::get_base]
    // MOV  dword ptr [ESP + 0xc],EAX       ; stashed
    // MOV  ECX,dword ptr [ESP + 0xc]       ; read back
    // CALL dword ptr [EDX + 0x4]           ; its nickname
    // PUSH .rdata:s_last_base_06d6b1cc
    // and the whole block is gated twice: module storage skips it in single
    // player, module storage skips it when that second base is null.

    // The four base_* keys that follow it read the selected character's record
    // in the container at PlayerData+0x50, fetched at module storage ..
    // exactly like the copy at module storage (the routine key,
    // = the map's unique insert, the routine copies the returned
    // pair; the node + 0x20c record is ESI). baseline does that lookup BEFORE it
    // writes `last_base`.
    // -----------------------------------------------------------------------
    if (!SinglePlayer()) {
        Universe::IBase* LastBase = Universe::get_base(Player.last_base_id);
        if (LastBase != nullptr) {
            PlayerData::FSavedCharacter const& BaseRecord =
                Player.saved_characters[Player.selected_character];

            // the same BASE slot 1 nickname getter, unguarded.
            fprintf(Out, "%s = %s\n", "last_base", LastBase->Nickname);
            fprintf(Out, "\n");

            // .. module storage -- record+0x25c, "%s = %g\n",
            // unconditionally (no `< 1` test, unlike hull_status).
            fprintf(Out, "%s = %g\n", "base_hull_status",
                    static_cast<double>(BaseRecord.BaseHullStatus));

            // .. module storage -- GetShip(+0x264) with NO null
            // test, then every node of record+0x26c (no health filter):
            // get_group_by_id(Id), and on a hit "%s, %g" (group+0x08, Health)
            // under `base_collision_group`.
            Archetype::Ship* BaseShip = Archetype::GetShip(Player.ship_archetype);
            for (std::list<CollisionGroupDesc>::const_iterator It =
                     BaseRecord.BaseCollisionGroups.begin();
                 It != BaseRecord.BaseCollisionGroups.end(); ++It) {
                Archetype::CollisionGroup const* Group = BaseShip->get_group_by_id(It->Id);
                if (Group == nullptr)
                    continue;
                char GroupBuf[256];
                sprintf(GroupBuf, "%s, %g", Group->ObjName, static_cast<double>(It->Health));
                fprintf(Out, "%s = %s\n", "base_collision_group", GroupBuf);
            }
            fprintf(Out, "\n");

            // .. module storage -- record+0x260. A mission item
            // (+0x18) is skipped outright; an unresolved archetype is skipped.
            // Not mounted (+0x0c == 0): `base_cargo`, "%u, %d, , , %d" when
            // Health == 1.0f (module storage), else "%u, %d, , %g, %d".
            // Mounted: `base_equip`, "%u, , %g" for is_internal, else
            // "%u, %s, %g" with the hardpoint. %u is Equipment+0x08.
            for (std::list<EquipDesc>::const_iterator It =
                     BaseRecord.BaseEquipment.Items.begin();
                 It != BaseRecord.BaseEquipment.Items.end(); ++It) {
                EquipDesc const& Item = *It;
                if (Item.bMission)
                    continue;
                Archetype::Equipment* Equip = Archetype::GetEquipment(Item.ArchId);
                if (Equip == nullptr)
                    continue;
                unsigned int const NicknameId = Equip->Nickname.Id;

                char Line[512];
                if (!Item.bEquipped) {
                    if (Item.Health == 1.0f) {
                        sprintf(Line, "%u, %d, , , %d", NicknameId, Item.Count,
                                Item.bMission ? 1 : 0);
                    } else {
                        sprintf(Line, "%u, %d, , %g, %d", NicknameId, Item.Count,
                                static_cast<double>(Item.Health), Item.bMission ? 1 : 0);
                    }
                    fprintf(Out, "%s = %s\n", "base_cargo", Line);
                } else {
                    if (Item.is_internal()) {
                        sprintf(Line, "%u, , %g", NicknameId, static_cast<double>(Item.Health));
                    } else {
                        sprintf(Line, "%u, %s, %g", NicknameId, Item.Hardpoint,
                                static_cast<double>(Item.Health));
                    }
                    fprintf(Out, "%s = %s\n", "base_equip", Line);
                }
            }
            fprintf(Out, "\n");
        }
    }

    // -----------------------------------------------------------------------
    // .. module storage -- the weapon-group block, dumped
    // VERBATIM: baseline passes the std::string's own data pointer at +0x384 as
    // fprintf's whole FORMAT string, falling back to MSVCP60's static empty
    // string when it is null. So it is a pre-rendered block of ini lines, not a
    // single value. This block was missing from the port entirely.
    // MOV  EAX,[EBX + 0x384]    ; _Ptr, or _Nullstr when null
    // PUSH EAX / PUSH FILE* / CALL fprintf   -- two arguments
    // An empty string reaches fprintf as an empty format and prints nothing,
    // which is what c_str() of an empty std::string does here as well.
    // KNOWN baseline UB: the stored text is used as the FORMAT, so a '%' in it
    // is interpreted by fprintf with no arguments behind it. Reproduced as a
    // path, not escaped (was `fprintf(Out, "%s", ...)` until 2026-09-24).
    // -----------------------------------------------------------------------
#pragma warning(push)
#pragma warning(disable : 4774 6387)  // non-literal format string: baseline's own call shape
    fprintf(Out, Player.weapon_group_name.c_str());
#pragma warning(pop)

    // bare newline.
    fprintf(Out, "\n");

    // -----------------------------------------------------------------------
    // .. module storage -- `visit`, one line per entry of the
    // COMMITTED map at +0x3A0 (node+0xc key, node+0x10 byte), through
    // sprintf "%u, %u" (module storage) and then "%s = %s\n".
    // CORRECTED: the port wrote the key as `visited`. The string at
    // , pushed at module storage, is "visit".
    // -----------------------------------------------------------------------
    for (std::map<unsigned int, unsigned char>::const_iterator VI = Player.visit_map.begin();
         VI != Player.visit_map.end(); ++VI) {
        char VisitBuf[64];
        sprintf_s(VisitBuf, "%u, %u", VI->first, static_cast<unsigned int>(VI->second));
        fprintf(Out, "%s = %s\n", "visit", VisitBuf);
    }

    // bare newline.
    fprintf(Out, "\n");

    // -----------------------------------------------------------------------
    // .. module storage -- the `log` loop. CLOSED (wave 18).

    //   MOV  EAX,dword ptr [EBX + 0x394]   ; the list's _Head
    //   MOV  ESI,dword ptr [EAX]           ; first node
    //   CMP  ESI,EAX                       ; circular sentinel -> empty
    //   LEA  EAX,[ESI + 0x8]               ; the FmtStr payload, past _Next/_Prev
    //   CALL the routine
    //   PUSH .rdata:the field           ; "log"
    //   PUSH .rdata:s_%s_=_%s_06d6b578     ; "%s = %s\n"
    //   MOV  ESI,dword ptr [ESI]           ; advance
    // -----------------------------------------------------------------------
    for (std::list<FmtStr>::const_iterator LI = Player.mission_log.begin();
         LI != Player.mission_log.end(); ++LI) {
        // [ESP + 0x6b8] .. [ESP + 0x16b8): the routine's 0x1000-byte scratch,
        // formatted by the routine (module storage) and printed at module storage.
        char LogText[0x1000];
        Server_FormatMissionLogEntry(LogText, *LI);
        fprintf(Out, "%s = %s\n", "log", LogText);
    }

    // bare newline.
    fprintf(Out, "\n");

    // .. module storage -- `interface`, "%s = %u\n" from +0x39C.
    // This key was missing from the port entirely.
    fprintf(Out, "%s = %u\n", "interface", Player.interface_state);

    // bare newline.
    fprintf(Out, "\n");

    // .. module storage -- the routine(4, &{+0x348, FILE*}).
    // Two cdecl arguments of the preceding fprintf are still on the stack, so
    // [ESP + 0x48] is the writer's FILE* at frame+0x28, not frame+0x30: the
    // record is {client_id, the open save file} at frame+0xd4.
    struct FSaveWrittenEvent {
        unsigned int ClientId;                  // PlayerData+0x348
        FILE* File;                             // the writer's +0x04
    };
    FSaveWrittenEvent SaveEvent;
    SaveEvent.ClientId = Player.client_id;
    SaveEvent.File = Out;
    Server_ControllerDispatch(0, pub::Controller::EMessage::PlayerSave, &SaveEvent);

    // .. module storage -- single player: the routine(&writer),
    // the [Time] section, the feelings and event 0x41, still inside [Player]'s
    // file before the error latch and the encode.
    if (SinglePlayer())
        Server_SaveSinglePlayerState(&Writer);

    // -----------------------------------------------------------------------
    // .. module storage -- THE WRITE-ERROR LATCH.
    //   MOV  EAX,dword ptr [ESP + 0x28]    ; the writer's FILE*
    //   TEST byte ptr [EAX + 0xc],0x20     ; _flag & _IOERR -- an inlined ferror
    //   JZ   the branch
    // -----------------------------------------------------------------------
    bool const WriteFailed = Writer.HadWriteError();
    if (WriteFailed) {
        // .. module storage -- FDUMP(0x100002, module storage,
        // PlayerDB.cpp (module storage), 0xc6d, target path [ESP + 0x18]).
        FDUMP(0x100002, "%s(%d) : *** WARNING: Error writing %s\n",
              "Server\\PlayerDB.cpp", 0xc6d, WritePath);
    }

    Writer.Close();

    if (WriteFailed) {
        // .. module storage -- _unlink(the path just written).
        _unlink(WritePath);
        // .. module storage tears down the two FmtStrs and the
        // writer and returns AL = 0. It does NOT free the relation array
        // allocated at module storage -- only the success path reaches the free
        // at module storage -- so neither does the port.
        return false;
    }

    // -----------------------------------------------------------------------
    // .. module storage -- THE MULTIPLAYER SWAP, and the only
    // part of the closeout that really is mode-dependent.
    //   CALL SinglePlayer / TEST AL,AL / JNZ the branch   <- single player: skip
    //   _unlink(<the real path>)
    //   rename(<accts>\temp.fl, <the real path>)
    // Outside single player everything above was written to temp.fl (chosen at
    // ), so the finished, already-encoded temp file replaces the
    // real one here. That makes the save atomic: a crash mid-write leaves the
    // previous character file intact.
    // -----------------------------------------------------------------------
    if (!SinglePlayer()) {
        _unlink(SavePath);
        rename(TempPath, SavePath);
    }

    // -----------------------------------------------------------------------
    // .. module storage -- THE MP RECORD REFRESH. Added
    // 2026-09-24 (claude-code:w5-server-06d4). Outside single player
    // (module storage JNZ the branch) the selected character's record in
    // PlayerData+0x50 is brought up to what was just written, so the in-memory
    // record reflects the last SAVE and not the last load. The port used to
    // return right after the rename.
    // record = insert(pair(+0x64, the routine
    //                   default)).first (the routine / the routine) --
    //                   map operator[]; node offsets = record + 0x20c
    //   node+0x44c  HullStatus      = +0x268                 module storage
    //   node+0x450  Equipment       = +0x278 (list operator=) module storage..06d4e2cc
    //   node+0x56c  EquipIdMaker    = +0x3c8 (a WORD)        module storage
    //   node+0x45c  CollisionGroups = +0x26c (list operator=) module storage..06d4e382
    //   node+0x43c..0x448  Money / NumKills / NumMissionSuccesses /
    //               NumMissionFailures = +0x31c/+0x334/+0x338/+0x33c
    //   node+0x4c8  Rank            = Vibe::Get's rank ([ESP+0x78])  module storage
    //   node+0x590  Visits          = +0x3a0 (map operator=, the routine /
    // )          module storage..06d4e43f
    //   node+0x570  WeaponGroups    = +0x380 (string assign) module storage..06d4e513
    //   node+0x484 / 0x488  BaseId / LastBaseId = +0x3ec / +0x3f0
    //   node+0x494  Position        = the `pos` locals       module storage..06d4e54c
    //   node+0x4a0  Orientation     = the nine matrix dwords module storage..06d4e561
    //   node+0x438 / 0x48c / 0x490  ShipArchetype / LocationId / SystemId =
    //               +0x264 / +0x3f8 / +0x3e0
    //   node+0x580  MissionLog      = +0x390 (list operator= with FmtStr::
    //               operator=, the routine erase, the routine insert)
    //   node+0x4c4  StartingRing    = +0x414                 module storage
    //   node+0x4e0  ComCostume      = +0x28c: four part ids, the count, then
    //               only `count` accessory ids (signed loop)  module storage..06d4e68e
    //   node+0x514  Voice           = +0x2c4 (strlen, capped at 0x1f, NUL,
    //               length)                                  module storage..06d4e6ee
    //   node+0x538  BaseCostume     = +0x2e4, same partial copy  module storage..06d4e73e
    //   node+0x58c  Interface       = +0x39c                 module storage
    //   node+0x4cc  Relations: erase(begin, end) (module storage..06d4e77c), then
    //               one push_back per Vibe::Get relation, bounded by the BYTE
    //               count (module storage..06d4e91c)
    // then the relation array is freed (module storage) on both the SP and the
    // MP path.
    // -----------------------------------------------------------------------
    if (!SinglePlayer()) {
        PlayerData::FSavedCharacter& Record =
            Player.saved_characters[Player.selected_character];
        Record.HullStatus = Player.relative_health;
        Record.Equipment.Items = Player.equipment.Items;
        Record.EquipIdMaker = Player.equip_id_maker;
        Record.CollisionGroups = Player.collision_groups;
        Record.Money = Player.cash;
        Record.NumKills = Player.num_kills;
        Record.NumMissionSuccesses = Player.num_mission_successes;
        Record.NumMissionFailures = Player.num_mission_failures;
        Record.Rank = Rank;
        Record.Visits = Player.visit_map;
        Record.WeaponGroups = Player.weapon_group_name;
        Record.BaseId = Player.current_base;
        Record.LastBaseId = Player.last_base_id;
        Record.Position[0] = SavedPos[0];
        Record.Position[1] = SavedPos[1];
        Record.Position[2] = SavedPos[2];
        for (int I = 0; I < 9; ++I)
            Record.Orientation[I / 3][I % 3] = SavedOrient[I];
        Record.ShipArchetype = Player.ship_archetype;
        Record.LocationId = Player.current_location;
        Record.SystemId = Player.system_id;
        Record.MissionLog = Player.mission_log;
        Record.StartingRing = Player.starting_ring;

        Server_CopyCostumeCounted(Record.ComCostume, Player.com_costume);
        {
            // TString<32>::operator=(char const*) inlined. The null arm at
            // cannot fire: the source is the inline buffer's
            // own address.
            size_t VoiceLen = strlen(Player.voice);
            if (VoiceLen >= 0x20)
                VoiceLen = 0x1f;
            memcpy(Record.Voice.Data, Player.voice, VoiceLen);
            Record.Voice.Data[VoiceLen] = '\0';
            Record.Voice.Length = static_cast<unsigned>(VoiceLen);
        }
        Server_CopyCostumeCounted(Record.BaseCostume, Player.costume);
        Record.Interface = Player.interface_state;

        Record.Relations.clear();
        for (unsigned int I = 0; I < static_cast<unsigned int>(RelationCount); ++I)
            Record.Relations.push_back(Relations[I]);
    }

    operator delete(Relations);
    return true;
}

namespace pub {

// pub::Save -- baseline 0x7efa0 (ordinal 397), module storage .. module storage.
// Single player only. SaveType == 1 is the autosave the routine; anything
// else goes to the routine with SaveType itself as the descrip_strid
// (module storage PUSH EAX). The slot is `Players + id*0x418 - 0x418` with no
// validation, i.e. PlayerDB::operator[]. Always returns 0.

// REWRITTEN 2026-09-24 (claude:srv-save): this used to carry its own
// approximations of both wrappers (a "SaveXXXX" search with an unsigned-short
// format and an SP-only path, an autosave with no skip latch). Both wrappers
// are ported now in server_savegame.cpp and this is the baseline dispatch.
SERVER_API int __cdecl Save(unsigned int PlayerId, unsigned int SaveType) {
    if (::SinglePlayer()) {
        PlayerData& Slot = ::Players[PlayerId];
        if (SaveType == 1)
            PlayerData_AutoSave(Slot);
        else
            PlayerData_MissionSave(Slot, SaveType);
    }
    return 0;
}

} // namespace pub
// Server_ControllerDispatch, pub::Controller::_SendMessage's dispatch core, is declared in
// common_server_controller.h (defined in IServerImpl.cpp).
extern "C" int __cdecl Server_ControllerSetHeartbeat(unsigned int Flid, float Interval);
extern "C" int __cdecl Server_ControllerSetTimer(unsigned int Flid, unsigned int Param1,
                                                 unsigned int Param2, float Timeout);
// (IServerImpl.cpp): is this FLID a live or pending controller.
extern "C" bool __cdecl Server_ControllerExists(const unsigned int& Flid);

// (module storage), __cdecl, one BY-VALUE argument `unsigned int Flid`
// (the listing hands the ADDRESS of its own stack slot to the routine; every call site,
// e.g. DockRequest at module storage, does `MOV ECX,[EAX]` / `PUSH ECX` — it pushes the
// dereferenced FLID, not the pointer):

//   LEA EAX,[ESP+8] / LEA ECX,[ESP+8] / LEA EDX,[ESP+0x18] / CALL the routine
//   TEST AL,AL / JNZ .. -> ESI = out object, else ESI = 0
//   TEST ESI,ESI / JZ  -> return ESI (null)
//   MOV EAX,[ESI] / CALL [EAX+0x160] / TEST AL,AL
//   JZ  -> return ESI        (slot +0x160 == 0 -> object still alive)
//   XOR EAX,EAX / RET        (slot +0x160 != 0 -> object is dying, return null)

// (const unsigned int* pFlid, IObjRW** ppObj, int* pOwner) is the object
// registry lookup itself: it compares *pFlid against the cached object's FLID field at
// +0xb0, falls back to the routine and then to a walk of the global object list.

// So the routine is "look an object up BY FLID and reject it if it is dying" — it has
// nothing to do with the local player. The previous port bound it to a fabricated
// `Server_LookupCurrentPlayer()` that read a `g_CurrentPlayerFLID` global nobody ever
// set, which made every caller (Dock, InstantDock, DockRequest, SendComm, GetGoodID,
// GetJumpTarget, GetTerminalInfo, FormationResponse, LaneResponse) operate on the wrong
// object or bail out with -2.

// The liveness test is slot +0x160 of the wrapper's OWN vtable, not an IObjInspect slot:
// IObjInspect ends at +0x150, and +0x160 is module storage `MOV AL,[ECX+0x40]; RET` in all
// thirteen Server.dll vtables -- GameObject::is_destroyed (server_gameobject.h). Corrected
// 2026-09-24 (claude:srv-ai, full 20-instruction trace of module storage..06d006ba): the
// port used to test IObjInspect slot 14 (+0x38) is_dying, and additionally returned null
// when CBase::Object failed the (Class & 3) == 3 guard. baseline has no such test here;
// callers that need the CObject apply the guard themselves.
// Declared in server_gameobject.h (2026-09-24, claude-code:w5-server-06d4) so the
// other translation units that call the routine (PlayerData_CheckPositionBound,
// ) use this one body instead of a copy.
GameObject* Server_LookupLiveObject(unsigned int Flid) {
    GameObject* Obj = Server_LookupGameObject(Flid);
    if (Obj == nullptr) return nullptr;
    if (Obj->is_destroyed()) return nullptr;
    return Obj;
}

// NOTE: baseline's Dock / InstantDock / Launch / FormationResponse / LaneResponse all
// follow the lookup with __RTDynamicCast to Ship; the wrapper hierarchy makes that a
// plain `dynamic_cast< ::Ship*>` at each of those call sites.

// The two file-static trees of SpaceObj.cpp, written by pub::SpaceObj::Create and
// erased by pub::SpaceObj::Destroy, and referenced nowhere else in Server.dll (the
// complete xref lists of .data:the field and the field are those two functions
// plus their own static constructor / atexit pairs):
// std::set<unsigned int> of the ship ids Create made. Constructed by
//     the initializer module storage (the routine, the VC6 _Tree ctor) and destroyed
//     through the atexit thunk module storage (JMP the routine, the _Tree dtor).
//     Create inserts *out_id (module storage, the routine).
// std::map<unsigned int, unsigned int>, ship id -> the ShipInfo's
//     ControllerId. Initializer module storage (the routine ctor: head and nil nodes
//     of 0x18 bytes, size 0), atexit thunk module storage -> module storage (the
//     tree's clear + head/nil release). Create inserts {id, 0} (module storage,
// ) and then stores info.ControllerId into the node's mapped dword
//     (module storage, node +0x10).
// Node layout 0x18 bytes (left/parent/right, key +0xc, mapped +0x10, color +0x14,
// isnil +0x15) is the VC6 _Tree's; the port uses the current std containers (no
// baseline ABI), whose static construction and destruction replace the four thunks.
static std::set<unsigned int> GSpaceObjCreatedShips;                    // .data:the field
static std::map<unsigned int, unsigned int> GSpaceObjShipControllers;   // .data:the field

// ===========================================================================
// pub::SpaceObj exports
// ===========================================================================

// ===========================================================================
// pub::Phantom exports and the Server-side phantom record.
// Ported 1:1 2026-09-24 (claude-code:w5-server-06cf).

// baseline allocates one 0x28-byte record per phantom (operator new(0x28) at module storage
// / 06cfc07d) whose vtable module storage has exactly two slots, the branch and
// it is a PhySys::CObjectCB, handed to PhySys::CreatePhantom as the
// PhantomParms+0x40 listener. The pub::Phantom handle IS that record. Layout:
//   +0x00 vtable          +0x04 std::list<unsigned> of ids that entered (alloc byte,
//   +0x08 head, +0x0c size)  +0x10 the same for ids that left (+0x14 head, +0x18 size)
//   +0x1c TypeMask (tested against CSimple::TypeBits)  +0x20 the PhySys phantom handle
//   +0x24 the system id.
// Every live record sits on the global std::list at .data:the field (size
// ), built by the static initializer the branch and torn down by the atexit
// here a plain static std::list. .data:the field is set while
// (Server_PumpPhantomEvents) dispatches; every pub::Phantom export returns
// -5 while it is set.
// ===========================================================================
namespace {

class ServerPhantom : public PhySys::CObjectCB
{
public:
    // (vtable +0x00, RET 4): a CSimple (class & 3) whose TypeBits (+0xe0)
    // share a bit with TypeMask has its id (+0xb0) appended to Entered.
    void added(CObject* Obj) override;
    // (vtable +0x04, RET 4, left uninspected by analysis -- its bytes are
    // 's with +0x14 / +0x18 in place of +0x08 / +0x0c): the same test, onto Left.
    void deleted(CObject* Obj) override;

    std::list<unsigned int> Entered;   // +0x04 .. +0x0f
    std::list<unsigned int> Left;      // +0x10 .. +0x1b
    unsigned int TypeMask = 0;         // +0x1c
    void* PhyHandle = nullptr;         // +0x20
    unsigned int SystemId = 0;         // +0x24
    // is the destructor: it empties Left (the routine per node) and frees
    // its head, then does the same for Entered -- the reverse-declaration member
    // destruction order of the two lists above.
};

void ServerPhantom::added(CObject* Obj)
{
    if (Obj == nullptr) return;
    if ((static_cast<unsigned int>(Obj->ObjectClass) & 3u) != 3u) return;
    if ((static_cast<CSimple*>(Obj)->TypeBits & TypeMask) == 0) return;
    Entered.push_back(static_cast<CSimple*>(Obj)->Id);
}

void ServerPhantom::deleted(CObject* Obj)
{
    if (Obj == nullptr) return;
    if ((static_cast<unsigned int>(Obj->ObjectClass) & 3u) != 3u) return;
    if ((static_cast<CSimple*>(Obj)->TypeBits & TypeMask) == 0) return;
    Left.push_back(static_cast<CSimple*>(Obj)->Id);
}

std::list<ServerPhantom*> GPhantoms;   // .data:the field / the field
bool GbPhantomPumpBusy = false;        // .data:the field

// The linear search every export opens with (e.g. module storage .. 06cfbd03): the first
// node whose value equals the handle, or end().
std::list<ServerPhantom*>::iterator FindPhantom(void* Handle)
{
    std::list<ServerPhantom*>::iterator It = GPhantoms.begin();
    while (It != GPhantoms.end() && *It != Handle)
        ++It;
    return It;
}

} // namespace

// (module storage .. 06cffec3), called once per server physics tick from
// (module storage). With the field latched, for every phantom: each
// entered id -> the routine(0x2d, {id, phantom, phantom->SystemId}), then
// (list::erase(begin, end)) on Entered; each left id -> the same with 0x2e,
// then Left is emptied node by node. The latch is cleared at the end.
void Server_PumpPhantomEvents()
{
    GbPhantomPumpBusy = true;
    for (std::list<ServerPhantom*>::iterator It = GPhantoms.begin(); It != GPhantoms.end(); ++It) {
        ServerPhantom* const Ph = *It;
        for (std::list<unsigned int>::iterator Id = Ph->Entered.begin();
             Id != Ph->Entered.end(); ++Id) {
            unsigned int Event[3];
            Event[0] = *Id;
            Event[1] = static_cast<unsigned int>(reinterpret_cast<uintptr_t>(Ph));
            Event[2] = Ph->SystemId;
            Server_ControllerDispatch(0, pub::Controller::EMessage::PhantomEnter, Event);
        }
        Ph->Entered.clear();
        for (std::list<unsigned int>::iterator Id = Ph->Left.begin();
             Id != Ph->Left.end(); ++Id) {
            unsigned int Event[3];
            Event[0] = *Id;
            Event[1] = static_cast<unsigned int>(reinterpret_cast<uintptr_t>(Ph));
            Event[2] = Ph->SystemId;
            Server_ControllerDispatch(0, pub::Controller::EMessage::PhantomExit, Event);
        }
        Ph->Left.clear();
    }
    GbPhantomPumpBusy = false;
}

namespace pub {
namespace SpaceObj {

// baseline SpaceObj::Dock calls SpaceObj::ActivateEquipment, which is defined further
// down this file; forward-declare it so the call order matches baseline.
SERVER_API EQUIPMENT_RTYPE __cdecl ActivateEquipment(const unsigned int& obj_id_ref, EQInfo* eq_info);

// .data:the field -- the SpaceObj translation unit's own lazily filled
// I3DMathEngine (the routine, Server_Acquire3DMathEngine). Three users in
// baseline: pub::SpaceObj::Create (module storage test, module storage store),
// CreateSolar (module storage / module storage) and GetEmptyPos
// (module storage / module storage). Create and CreateSolar call slot +0x50,
// MatrixToQuaternion, on the caller's matrix (module storage, module storage);
// no null test on the engine. GetEmptyPos calls slot +0x28, TransformXform
// (module storage).
static I3DMathEngine* GSpaceObjMathEngine = nullptr;   // .data:the field

static void SpaceObj_MatrixToQuat(Quaternion& Out, Matrix const& In)
{
    if (GSpaceObjMathEngine == nullptr)
        GSpaceObjMathEngine = Server_Acquire3DMathEngine();
    GSpaceObjMathEngine->MatrixToQuaternion(reinterpret_cast<FQuaternion&>(Out),
                                            reinterpret_cast<FMatrix const&>(In));
}

// ---------------------------------------------------------------------------
// SpaceObj_BuildTransportLoadout -- baseline the routine (module storage ..
// , cdecl, plain RET), sole caller pub::SpaceObj::Create
// with (ship archetype, &EquipIdMaker, &ShipInfo+0x5c, &request
// +0x70). Ported 1:1 2026-09-24 (claude-code:w5-server-06d4); the name comes
// from its own FDUMP text ("Transport loadout aborted").
// reserve(size + cargo node count): when the
//                   capacity (unsigned JNC) is short, operator new(max(need,0)
//                   * 0x20), _Ucopy, _Destroy, the routine on the old buffer,
//                   Cap = new + need, End = new + old size, Begin = new
// hps = Arch->get_legal_hps(0xe = HpTypeCargoPod); null,
//                   no buffer or size 0 -> the plain branch at module storage
//   pod branch, at most the FIRST TWO cargo nodes (module storage JG 1):
//     good = GoodList::find_by_id(GoodId); arch = good+0x50;
//     eq = Archetype::GetEquipment(arch); pod = eq vslot 8 (+0x20)
//     pod null -> FDUMP(0x100002, module storage, SrvMisc.cpp, 0x3a,
//                 eq->Nickname.get_string()) and return (no further nodes)
//     per = ftol((unsigned)Count / (int)hp count)
//     per hp: append {CreateEquipID, pod id (+8), count 1, status 1.0, hp,
//             equipped}; append {CreateEquipID, arch, status 1.0, hp, not
//             equipped, count per}; left -= per
//     the last element's count = left + per
//   plain branch, every node: append {CreateEquipID, Good2Arch(GoodId),
//     Count, status 1.0, make_internal(), not equipped}
// Each append is vector<EquipDesc>::insert(end, 1, EquipDesc()) followed by
// the setters on the new last element; here a default EquipDesc is appended
// through add_equipment_item(.., false), which never folds, and the setters
// then run on End[-1] in baseline's order.
// ---------------------------------------------------------------------------
static void SpaceObj_AppendDefaultEquip(EquipDescVector& Out) {
    EquipDesc const Blank;                                          // EquipDesc::EquipDesc
    Out.add_equipment_item(Blank, false);                           // insert(end, 1, Blank)
}

static void SpaceObj_BuildTransportLoadout(Archetype::Ship const* Arch,
                                           SubObjectID::EquipIdMaker* IdMaker,
                                           OwnerList<CargoDesc> const* Cargo,
                                           EquipDescVector* Out) {
    // .. module storage -- size, node count, capacity.
    unsigned int Need = (Out->Begin == nullptr) ? 0u
        : static_cast<unsigned int>(Out->End - Out->Begin);
    for (MetaNode<CargoDesc> const* Node = Cargo->get_head(); Node != nullptr; Node = Node->Next)
        ++Need;
    unsigned int const Capacity = (Out->Begin == nullptr) ? 0u
        : static_cast<unsigned int>(Out->Cap - Out->Begin);
    if (Capacity < Need) {                                          // JNC
        int const AllocCount = static_cast<int>(Need) >= 0 ? static_cast<int>(Need) : 0; // JGE
        EquipDesc* const NewBuf = static_cast<EquipDesc*>(
            ::operator new(static_cast<size_t>(AllocCount) * sizeof(EquipDesc)));
        EquipDesc* Dst = NewBuf;
        for (EquipDesc* Src = Out->Begin; Src != Out->End; ++Src, ++Dst)
            new (Dst) EquipDesc(*Src);                              // _Ucopy module storage
        // _Destroy (module storage): EquipDesc's destructor is trivial.
        unsigned int const OldSize = (Out->Begin == nullptr) ? 0u
            : static_cast<unsigned int>(Out->End - Out->Begin);     // size() module storage
        ::operator delete(Out->Begin);                              // module storage
        Out->Cap = NewBuf + Need;
        Out->End = NewBuf + OldSize;
        Out->Begin = NewBuf;
    }

    std::vector<CacheString> const* const Hps = Arch->get_legal_hps(HpTypeCargoPod);
    if (Hps != nullptr && !Hps->empty()) {
        int const HpCount = static_cast<int>(Hps->size());          // [ESP+0x18]
        int Index = 0;                                              // [ESP+0x14]
        for (MetaNode<CargoDesc> const* Node = Cargo->get_head(); Node != nullptr;
             Node = Node->Next) {
            CargoDesc const* const Desc = Node->Data;
            GoodInfo const* const Good = GoodList::find_by_id(Desc->GoodId);
            unsigned int const ArchId = Good->ArchId;               // +0x50
            Archetype::Equipment* const Eq = Archetype::GetEquipment(ArchId);
            Archetype::CargoPod* const Pod = Eq->get_pod_appearance(); // vslot 8
            if (Pod == nullptr) {
                FDUMP(0x100002,
                      "%s(%d) : *** WARNING: Equipment(%s) has no pod_appearance.  Transport loadout aborted.",
                      "Server\\SrvMisc.cpp", 0x3a,
                      Eq->Nickname.get_string());
                return;
            }
            // FILD of the count zero-extended to a
            // qword, FIDIV by the hp count, ftol. Both operands fit a double
            // exactly and ftol truncates, so this is the same integer.
            int const PerHp = static_cast<int>(
                static_cast<double>(Desc->Count) / static_cast<double>(HpCount));
            int Left = static_cast<int>(Desc->Count);               // [ESP+0x7c]
            for (std::vector<CacheString>::const_iterator Hp = Hps->begin();
                 Hp != Hps->end(); ++Hp) {
                SpaceObj_AppendDefaultEquip(*Out);
                EquipDesc* Last = Out->End - 1;
                Last->set_id(IdMaker->CreateEquipID());             // / 06d5fb32
                Last->set_arch_id(Pod->Nickname.Id);                // pod+8
                Last->set_count(1);
                Last->set_status(1.0f);
                Last->set_hardpoint(*Hp);
                Last->set_equipped(true);

                SpaceObj_AppendDefaultEquip(*Out);
                Last = Out->End - 1;
                Last->set_id(IdMaker->CreateEquipID());             // / 06d5fb9e
                Last->set_arch_id(ArchId);
                Last->set_status(1.0f);
                Last->set_hardpoint(*Hp);
                Last->set_equipped(false);
                Last->set_count(PerHp);
                Left -= PerHp;
            }
            (Out->End - 1)->set_count(Left + PerHp);
            if (++Index > 1)
                return;
        }
        return;
    }

    // .. module storage -- no cargo-pod hardpoint: every node.
    for (MetaNode<CargoDesc> const* Node = Cargo->get_head(); Node != nullptr; Node = Node->Next) {
        CargoDesc const* const Desc = Node->Data;
        SpaceObj_AppendDefaultEquip(*Out);
        EquipDesc* const Last = Out->End - 1;
        Last->set_id(IdMaker->CreateEquipID());                     // / 06d5fc94
        Last->set_arch_id(Good2Arch(Desc->GoodId));                 // / 06d5fcac
        Last->set_count(static_cast<int>(Desc->Count));
        Last->set_status(1.0f);
        Last->make_internal();
        Last->set_equipped(false);
    }
}

// SpaceObj::Create -- baseline module storage .. 06cf80ca (ordinal 0x17c90, cdecl).
// Ported 1:1 2026-09-24 (claude-code:w5-server-06cf). B = the ESP after the
// system lookup's argument pop; the result lives at [B+0xc] (seeded -1 at
// ), the FShipCreateRequest at [B+0x34], a local EquipIdMaker at
// [B+0x18], the CFLIDMaker pointer at [B+0x1c].
// CFLIDMaker::GetInstance null -> FDUMP(0x100001,
//                         "...pubSpace.cpp", 0x3f, "Failed to grab instance of global
//                         ID maker.\n"), return -1
// the routine(info.SystemID) null -> return -1
// the request: the routine, then CShipCreateInfo's inline
//                         part (+0xac/+0xb0/+0xb4/+0xd0/+0xd8 = 0, +0xd4 = -1,
//                         +0xdc = 0, vftable 06cf0410), EquipIdMaker ctor on +0xe0,
//                         +0xe4 = 0, +0xe8 = 0
// the costume: +0x14 = info+0x70, +0x18 = +0x74, +0x1c =
//                         +0x78, +0x20 = +0x7c, +0x44 = count (info+0xa0, stored
//                         before the JLE), then `count` dwords info+0x80 -> +0x24
// +0xb4 = info+0xbc, +0x48 = info+0xa8, +0xb0 = info+0x54,
//                         +0x10 = info+0xa4, +0x08 = info+0x08, +0x4c = info+0x0c
// the field (lazily the routine) +0x50
//                         MatrixToQuaternion(&q, info+0x30); +0x58..+0x64 = q
// +0xb8 = info+0x24 (velocity), +0xc4 = info+0x18 (angular),
//                         +0x0c = info+0xac, +0x6c = info+0x58, +0xd0 = 0, +0xac = 0,
//                         +0x68 = info+0xb0, +0xe8 = info+0xb8
// Archetype::GetShip(info+0x08)->
//                         get_undamaged_collision_group_list(&request+0x80); no null
//                         test on the archetype
// local EquipIdMaker ctor; info+0x58 != 0 -> Loadout::Get's
//                         +0x14 word into it (no null test on the loadout)
// the routine(arch, &maker, &info+0x5c, &request+0x70);
//                         request+0xe0 = the maker's word
// the routine(&info) false -> result -4 (06cf7fec)
// the maker's +4 counter, masked and post-incremented
//                         (CFLIDMaker::AllocateObjectID inlined) -> request+0x04;
// (StarSystem::create_ship)(&request), its
//                         return value unused
// request+0x04 & 0x3fffffff == 0 -> the result stays -1
// *out_id = id; the routine set<uint>::insert(*out_id) on

// result = 0; the routine map<uint,uint>::insert({id, 0})
//                         on the field, then the returned node's value = info+0x00
// ~FShipCreateRequest inlined; 06cf80af return the result
SERVER_API int __cdecl Create(unsigned int& out_id, const SpaceObj::ShipInfo& info) {
    CFLIDMaker* const IdMaker = CFLIDMaker::GetInstance();
    if (IdMaker == nullptr) {
        FDUMP(0x100001, "%s(%d) : *** ERROR: %s",
              "Server\\space\\pubSpace.cpp", 0x3f,
              "Failed to grab instance of global ID maker.\n");
        return -1;
    }
    int Result = -1;
    FStarSystem* const System = SysDB_LookupSystem(info.SystemID);
    if (System == nullptr) return Result;

    FShipCreateRequest Request;
    Request.EquipStateData = nullptr;
    Request.MarkPlayerId = 0u;
    Request.CostumeBody = info.CostumeBody;
    Request.CostumeHead = info.CostumeHead;
    Request.CostumeRightHand = info.CostumeRightHand;
    Request.CostumeLeftHand = info.CostumeLeftHand;
    Request.CostumeAccessoryCount = info.CostumeAccessoryCount;
    for (int Index = 0; Index < Request.CostumeAccessoryCount; ++Index)  // / 06cf7ddf
        Request.CostumeAccessory[Index] = info.CostumeAccessory[Index];
    Request.ReputationRank = info.ReputationRank;
    Request.VoiceId = info.VoiceId;
    Request.VibeGroupId = info.VibeGroupId;
    Request.Field_0xb0 = info.Field_0x54;
    Request.ArchetypeNickname = info.ShipArchID;
    Request.Position = info.Position;
    Quaternion ShipOrient;
    SpaceObj_MatrixToQuat(ShipOrient, info.Orientation);
    Request.Orientation = ShipOrient;
    Request.Velocity = info.Velocity;
    Request.AngularVelocity = info.AngularVelocity;
    Request.DockingBaseId = info.DockingBaseId;
    Request.Field_0x6c = info.LoadoutID;
    Request.Throttle = 0.0f;
    Request.OwnerPlayer = 0u;
    Request.HitPts = info.HitPts;
    Request.MarkPlayerId = info.Field_0xb8;
    Archetype::Ship* const ShipArch = Archetype::GetShip(info.ShipArchID);
    ShipArch->get_undamaged_collision_group_list(Request.CollisionGroups);

    SubObjectID::EquipIdMaker EquipIds;
    if (info.LoadoutID != 0u) {
        Loadout::Map const* const LoadoutMap = Loadout::Get(info.LoadoutID);
        EquipIds = LoadoutMap->IdMaker;                                  // / 06cf7f3f word store
    }
    SpaceObj_BuildTransportLoadout(ShipArch, &EquipIds, &info.Cargo, &Request.Equipment);
    Request.EquipIds = EquipIds;                                         // / 06cf7f61

    if (!Server_ControllerExists(info.ControllerId)) {                   // / 06cf7f73
        Result = -4;
    } else {
        Request.ObjId = IdMaker->AllocateObjectID();
        System->create_ship(Request);
        unsigned int const Id = Request.ObjId;
        if ((Id & 0x3fffffffu) != 0) {                                   // / 06cf7fa2
            out_id = Id;
            GSpaceObjCreatedShips.insert(out_id);
            Result = 0;
            std::pair<std::map<unsigned int, unsigned int>::iterator, bool> const Inserted =
                GSpaceObjShipControllers.insert(
                    std::pair<unsigned int const, unsigned int>(Id, 0u));
            Inserted.first->second = info.ControllerId;
        }
    }
    return Result;
}

// SpaceObj::CreateLoot -- baseline module storage (cdecl, RET 0). One 0x6c-byte FLootRecord
// built on the stack from the LootInfo, handed to StarSystem::create_loot (the routine) in
// mode 4 with LootInfo+0x0c as the source object.
SERVER_API int __cdecl CreateLoot(unsigned int& out_id, const SpaceObj::LootInfo& loot_info) {
    // the routine (the system map lookup); none -> -4.
    StarSystem* const Sys = SysDB_LookupSystem(loot_info.SystemID);
    if (Sys == nullptr) return -4;

    // the record's first eight dwords zero (so no player to mark
    // and no name id until they are written below), +0x68 = 1, +0x69 = 0.
    FLootRecord Record = {};
    Record.Field_0x68 = 1;
    Record.bMission = 0;

    // the id maker's dynamic counter, post-incremented and masked
    // to 30 bits; the caller's out_id gets it before anything else is read. No null test on
    // the maker.
    unsigned int const LootId = CFLIDMaker::GetInstance()->AllocateObjectID();
    out_id = LootId;
    Record.Id = LootId;

    // the contents (Archetype::GetEquipment, dereferenced without a
    // null test), their hit points (Root+0x1c) and the unit count (LootInfo+0x58).
    Archetype::Equipment* const Contents = Archetype::GetEquipment(loot_info.EquipArchID);
    Record.ContentsArch = Contents;
    Record.ContentsHitPts = Contents->HitPts;
    Record.Count = static_cast<int>(loot_info.Units);

    // the crate is the contents' loot appearance (Equipment
    // vt+0x1c); its hit points (Root+0x1c) times LootInfo+0x5c, FMUL then FSTP float.
    Archetype::Equipment* const Appearance = Contents->get_loot_appearance();
    Record.Arch = Appearance;
    Record.HitPts = static_cast<float>(static_cast<double>(Appearance->HitPts) *
                                       static_cast<double>(loot_info.HitPtsScale));

    // the remaining fields, slot for slot.
    Record.Field_0x1c = loot_info.NameId;            // record +0x1c
    Record.Velocity = loot_info.Velocity;            // record +0x20
    Record.Position = loot_info.Position;            // record +0x2c
    Record.Orientation = loot_info.Orientation;      // REP MOVSD 9, record +0x38
    Record.AngularVelocity = loot_info.AngularVelocity; // record +0x5c
    Record.Field_0x68 = loot_info.bCanAiTractor;     // record +0x68
    Record.bMission = loot_info.bMission;            // record +0x69
    Record.Field_0x18 = loot_info.MarkForPlayer;     // record +0x18

    // create_loot(LootInfo+0x0c, 4, &Record, 1); always 0.
    Sys->create_loot(loot_info.OwnerObjId, 4, &Record, 1);
    return 0;
}

// SpaceObj::Destroy -- baseline module storage .. module storage (ordinal 0x185f0, cdecl).
// Ported 1:1 2026-09-24 (claude-code:w5-server-06cf):
// (obj_id & 0x3fffffff) == 0 -> -4
// ESI = -2; the routine(obj_id, &obj, &sys) false -> -2
// sys != 0: obj vtable +0x158 destroy(destroy_type, 0); ESI = 0
// sys == 0 (an in-transit object): the routine
//                         (Server_DestroyInTransit(obj)); ESI stays -2
// the field lower_bound(obj_id) (the routine), and when
//                         it is not end() and !(obj_id < key) erase it (the routine)
// the same on the field (the routine / the routine)
// return ESI
// Both tree erases run on both paths, keyed by obj_id, as std::set/map::erase(key).
SERVER_API int __cdecl Destroy(unsigned int obj_id, DestroyType destroy_type) {
    if ((obj_id & 0x3fffffffu) == 0) return -4;
    int Result = -2;
    GameObject* Obj = nullptr;
    StarSystem* System = nullptr;
    if (!Server_FindLiveObject(obj_id, Obj, System))
        return Result;

    if (System != nullptr) {
        Obj->destroy(static_cast<int>(destroy_type), 0);
        Result = 0;
    } else {
        Server_DestroyInTransit(Obj);
    }

    GSpaceObjCreatedShips.erase(obj_id);                                 // / 06cf869f
    GSpaceObjShipControllers.erase(obj_id);                              // / 06cf86e9
    return Result;
}

// SpaceObj::DrainShields — drains shields on an object
// baseline (0x18930, Server): mask check, the routine(obj_id),
// type & 0x103 == 0x103 (else -1), build a stack DamageList whose +0x10 dword (the
// DamageCause) is 0x17, then call EqObj slot +0x1d8 (module storage: for every shield,
// CEquipTraverser(0x100), add a -1.0f shield entry via slot +0x214) followed by slot
// +0x188 (module storage EqObj / module storage Ship: apply the damage list -- the
// FDUMP at module storage names EqObj.cpp "Tried to apply damage ... within another
// damage callback") on the object itself, then destroy the list. 0x17 is a damage
// cause, not a message id — an older port passed it to an invented message 0x25.
// Ported 1:1 2026-09-24 (claude-code:w5-server-06cf), module storage .. 06cf8a73:
// (obj_id & 0x3fffffff) == 0 -> -4
// the routine (Server_LookupLiveObject); null -> -2
// CBase::Object null or (ObjectClass & 0x103) != 0x103 -> -1
// the stack DamageList, its ctor inlined (list head, count 0,
//                         bDestroyed 0, +0x14/+0x18 0) with +0x10 Cause = 0x17
// wrapper vtable +0x1d8 (EqObj::drain_shields) on the object
//                         the lookup returned -- not re-narrowed after the class test
// wrapper vtable +0x188 (apply_damage_list)
// ~DamageList inlined; return 0
// 0x17 has no DamageCause enumerator in common_misc.h, so it is cast from the value.
SERVER_API int __cdecl DrainShields(unsigned int obj_id) {
    if ((obj_id & 0x3fffffffu) == 0) return -4;
    GameObject* const Obj = Server_LookupLiveObject(obj_id);
    if (Obj == nullptr) return -2;
    CObject* const Cobj = Obj->Object;
    if (Cobj == nullptr ||
        (static_cast<unsigned int>(Cobj->ObjectClass) & 0x103u) != 0x103u) return -1;
    DamageList Damage;
    Damage.Cause = static_cast<DamageCause>(0x17);
    static_cast< ::EqObj*>(Obj)->drain_shields(Damage);
    Obj->apply_damage_list(Damage);
    return 0;
}

// SpaceObj::EnumerateCargo — iterates cargo on an object
// baseline: CEquipTraverser with type 0x10000, Arch2Good conversion, callback per item
SERVER_API int __cdecl EnumerateCargo(const unsigned int& obj_id, CargoEnumerator& enumerator) {
    if ((obj_id & 0x3fffffff) == 0) return -4;
    GameObject* obj = Server_LookupGameObject(obj_id);
    if (!obj) return -2;
    ::EqObj* EqRw = dynamic_cast< ::EqObj*>(obj);
    CEqObj* Ceq = (EqRw != nullptr) ? EqRw->ceqobj() : nullptr;
    if (Ceq == nullptr) return 0; // nothing that can hold cargo

    // baseline (module storage .. module storage): CEquipTraverser(0x10000) over the
    // object's CEquipManager, Arch2Good on each item's archetype, and
    // `enumerator.notify_cargo(goodId, count, subObjId)` through the enumerator's
    // OWN vtable slot 0 -- not a function pointer out of the struct. baseline skips
    // an item outright when Arch2Good returns 0 (module storage) and NEVER breaks
    // the walk on the callback's answer: the slot returns void and the loop runs
    // to the end of the traverser.
    CEquipTraverser Walk(EQUIP_TYPE_CARGO);
    CEquip* Item = Ceq->EquipManager.Traverse(Walk);
    while (Item != nullptr) {
        EquipDesc Desc;
        if (Item->GetEquipDesc(Desc)) {
            unsigned int const good_id = Arch2Good(Desc.ArchId);
            if (good_id != 0) {
                enumerator.notify_cargo(good_id, Desc.Count, Desc.Id);
            }
        }
        Item = Ceq->EquipManager.Traverse(Walk);
    }
    return 0;
}

// SpaceObj::FormationResponse -- baseline module storage .. module storage (cdecl).
// Ported 1:1 2026-09-24 (claude-code:w5-server-06cf):
// (*obj_id & 0x3fffffff) == 0 -> -4
// the routine (Server_LookupLiveObject); null -> -2
// __RTDynamicCast to Ship, not tested
// the routine = Ship::request_returned(1, response_type, 0)
// return 0
SERVER_API int __cdecl FormationResponse(const unsigned int& obj_id, FORMATION_RTYPE response_type) {
    if ((obj_id & 0x3fffffffu) == 0) return -4;
    GameObject* const Obj = Server_LookupLiveObject(obj_id);
    if (Obj == nullptr) return -2;
    ::Ship* const ShipRw = dynamic_cast< ::Ship*>(Obj);
    ShipRw->request_returned(1, static_cast<int>(response_type), 0);
    return 0;
}

// SpaceObj::LaneResponse -- baseline module storage .. module storage (cdecl), the
// same body as FormationResponse with request type 2:
// mask -> -4; 06cfaa6a .. 06cfaa74  the routine null -> -2
// __RTDynamicCast to Ship, not tested
// Ship::request_returned(2, lane_param, 0); return 0
SERVER_API int __cdecl LaneResponse(const unsigned int& obj_id, int lane_param) {
    if ((obj_id & 0x3fffffffu) == 0) return -4;
    GameObject* const Obj = Server_LookupLiveObject(obj_id);
    if (Obj == nullptr) return -2;
    ::Ship* const ShipRw = dynamic_cast< ::Ship*>(Obj);
    ShipRw->request_returned(2, lane_param, 0);
    return 0;
}

// SpaceObj::DockRequest — initiates dock request with a target
// baseline (0x1a6b0) is four instructions plus a tail call — it does NOT mask-check
// obj_id and it does NOT return -4/-2:
//   piVar1 = the routine(&obj_id);
//   if (piVar1 == 0) return 0;
//   return (**(code **)(piVar1[1] + 0x5c))(0, &target_id, &the field, 1, 0);
// piVar1[1] is the object's SECOND vtable (IObjAffect at +0x04), slot +0x5c.
SERVER_API int __cdecl DockRequest(const unsigned int& obj_id, const unsigned int& target_id) {
    GameObject* Obj = Server_LookupLiveObject(obj_id);
    if (!Obj) return 0;
    // IObjAffect slot 23 (common_iface.h), tail-called with
    // (0, &target_id, &<zero dword>, 1, nullptr). The third argument is the address of
    // , four zero bytes.
    static unsigned const ZeroSecondId = 0;
    return Obj->slot_5c(0, &target_id, &ZeroSecondId, 1, nullptr);
}

// SpaceObj::Dock -- baseline module storage .. module storage (ordinal 0x1a6e0, cdecl).
// Ported 1:1 2026-09-24 (claude-code:w5-server-06cf):
// (*ship_id & 0x3fffffff) == 0 -> -4
// result = -2; obj = the routine(*ship_id)
//                         (Server_LookupLiveObject); null -> straight to 06cfa854
// __RTDynamicCast to Ship, not tested; its byte +0x98 set ->
// (Ship::request_returned(0, response, dock_port)),
//                         result = 0
// EQInfo {Type 3, byte +4 = (response == 4 || response == 3)};
//                         ActivateEquipment(ship_id, &info), its result ignored
// response == 3 -> return result
// the object's CBase::Object (+0x10) under (& 0x503) == 0x503
//                         (a CShip), else to 06cfa854
// each 8-byte entry of CShip [+0x1f8, +0x1fc) (the end re-read
//                         every pass): entry non-null and entry-8 non-null ->
//                         GetBehaviorManager(entry-8) non-null -> target =
// (*target_id) non-null -> block {*target_id,
//                         target get_type (+0x88), *ship_id, byte (dock_port != -1)},
//                         event {5, &block} -> the manager's slot 0 (the branch)
// response == 3 -> return result (reached from the null-obj
//                         and non-CShip exits too)
// the routine(*target_id) null -> -3
// __RTDynamicCast to EqObj, not tested
// dock_port >= 0 -> the routine (EqObj::queue_dock_request(
//                         dock_port, *ship_id)), else the routine
//                         (EqObj::dequeue_dock_request(dock_port, *ship_id), its bool
//                         ignored); return 0 either way
// EQInfo: baseline writes only the dword at +0 and the byte at +4; the port zeroes the
// rest (deviations.md, zeroed indeterminate stack bytes).
SERVER_API int __cdecl Dock(const unsigned int& ship_id, const unsigned int& target_id,
                             int dock_port, DOCK_HOST_RESPONSE dock_host_response) {
    if ((ship_id & 0x3fffffffu) == 0) return -4;
    int Result = -2;
    GameObject* const Obj = Server_LookupLiveObject(ship_id);
    if (Obj != nullptr) {
        ::Ship* const ShipRw = dynamic_cast< ::Ship*>(Obj);
        if (ShipRw->Field_0x98 != 0) {
            ShipRw->request_returned(0, static_cast<int>(dock_host_response), dock_port);
            Result = 0;
        }

        EQInfo DockEquip;
        memset(&DockEquip, 0, sizeof(DockEquip));
        DockEquip.Type = 3;
        DockEquip.EquipId = (dock_host_response == 4 || dock_host_response == 3) ? 1 : 0; // / 06cfa770 (byte)
        ActivateEquipment(ship_id, &DockEquip);

        if (dock_host_response == 3) return Result;

        CObject* const Raw = Obj->Object;
        if (Raw != nullptr &&
            (static_cast<unsigned int>(Raw->ObjectClass) & 0x503u) == 0x503u) {
            CShip* const Cship = static_cast<CShip*>(Raw);
            for (std::vector<FObjRWWatcher>::const_iterator Entry = Cship->FollowFollowers.begin();
                 Entry != Cship->FollowFollowers.end(); ++Entry) {      // / 06cfa848
                IObjRW* const Follower = Entry->get();
                if (Follower == nullptr) continue;
                IBehaviorManager* const Manager = GetBehaviorManager(Follower);
                if (Manager == nullptr) continue;
                GameObject* const Target = Server_LookupLiveObject(target_id);
                if (Target == nullptr) continue;

                // The block at [ESP+0x28] and the event at [ESP+0x14]; the branch's
                // case 5 reads [+0] / [+4] & 0x120 / [+8] / byte [+0xc].
                struct {
                    unsigned int TargetId;   // +0x00  module storage
                    unsigned int TargetType; // +0x04  module storage (get_type out)
                    unsigned int ShipId;     // +0x08  module storage
                    unsigned char bHasPort;  // +0x0C  module storage
                    unsigned char Pad0d[3];
                } Block;
                memset(&Block, 0, sizeof(Block));
                Block.ShipId = ship_id;
                Block.TargetId = target_id;
                Target->get_type(Block.TargetType);
                Block.bHasPort = (dock_port != -1) ? 1 : 0;
                FBehaviorManagerEvent Event;
                Event.Type = 5;
                Event.Data = &Block;
                Manager->Slot00(&Event);
            }
        }
    }
    if (dock_host_response == 3) return Result;

    GameObject* const Host = Server_LookupLiveObject(target_id);
    if (Host == nullptr) return -3;
    ::EqObj* const HostEq = dynamic_cast< ::EqObj*>(Host);
    if (dock_port >= 0)
        HostEq->queue_dock_request(dock_port, ship_id);
    else
        HostEq->dequeue_dock_request(dock_port, ship_id);
    return 0;                                                            // / 06cfa8aa
}

// SpaceObj::InstantDock -- baseline module storage .. module storage (ordinal 0x1a8d0, cdecl).
// Ported 1:1 2026-09-24 (claude-code:w5-server-06cf):
// (*ship_id & 0x3fffffff) == 0 or (*target_id & 0x3fffffff) == 0
//                         -> -4; from here EBP = -2 is the only result
// the routine(*ship_id) (Server_LookupLiveObject); null -> -2
// __RTDynamicCast to Ship (EDI), not tested
// host = 0, owner = 0, the routine(*target_id, &host, &owner)
//                         (Server_FindLiveObject); its bool result is ignored
// __RTDynamicCast host -> EqObj (ESI); null -> -2
// flags = its vtable +0x154 (get_type_bits);
//                         flags == (flags & 0xc40) ->
// owner->complete_dock(ship, eqobj) (the routine), -2
// the routine = EqObj::ceqobj (CBase::Object under 0x103);
//                         its archetype (+0x88) docking sphere [dock_port] (vector at
//                         +0x88, 0x10-byte spheres, type +0xc, unchecked index):
//                         bLand = !(type == 3 || 4 || 5) (moor_small/medium/large)
// owner->queue_dock(ship, eqobj, &ceq->DockingPointBaseId
//                         (+0x164), &bLand) (the routine)
// return -2 (always)
// The owner is not null-tested on either call; the routine leaves it null for an
// in-transit host, which baseline would dereference the same way.
SERVER_API int __cdecl InstantDock(const unsigned int& ship_id, const unsigned int& target_id,
                                    int dock_port) {
    if ((ship_id & 0x3fffffffu) == 0 || (target_id & 0x3fffffffu) == 0) return -4;
    GameObject* const Obj = Server_LookupLiveObject(ship_id);
    if (Obj == nullptr) return -2;

    ::Ship* const ShipRw = dynamic_cast< ::Ship*>(Obj);
    GameObject* Host = nullptr;
    StarSystem* Owner = nullptr;
    Server_FindLiveObject(target_id, Host, Owner);
    ::EqObj* const HostEq = dynamic_cast< ::EqObj*>(Host);
    if (HostEq == nullptr) return -2;

    unsigned int const Flags = HostEq->get_type_bits();
    if (Flags == (Flags & 0xc40u)) {
        Owner->complete_dock(ShipRw, HostEq);
        return -2;
    }

    CEqObj* const HostCeq = HostEq->ceqobj();
    Archetype::EqObj const* const Arch =
        static_cast<Archetype::EqObj const*>(HostCeq->Archetype);
    unsigned int const Kind = Arch->DockingSpheres[dock_port].Type;
    bool const bLand = !(Kind == 3 || Kind == 4 || Kind == 5);
    Owner->queue_dock(ShipRw, HostEq, &HostCeq->DockingPointBaseId, &bLand);
    return -2;
}

// The PhySys::CreateParms that pub::SpaceObj::JumpIn (module storage .. 06cfa2d7),
// Relocate (module storage .. 06cfa428) and Launch (module storage .. 06cfa61a) each
// build inline on their stack before a system transfer: Mass 1.0f, the two COMMON
// damping defaults, +0x14 = 0, Type 2, +0x1c = 0, bEnableCollisions 1, Name[0] = 0,
// bHasInertia 0, Inertia = 0. baseline leaves Name[1..7] and Pad27 unwritten; the port
// zeroes the whole block first (deviations.md, zeroed indeterminate stack bytes).
static void SpaceObj_InitTransferParms(PhySys::CreateParms& Parms)
{
    memset(&Parms, 0, sizeof(Parms));
    Parms.Mass = 1.0f;
    Parms.LinearDamping = PhySys::DEFAULT_LINEAR_DAMPING;
    Parms.AngularDamping[0] = PhySys::DEFAULT_ANGULAR_DAMPING.x;
    Parms.AngularDamping[1] = PhySys::DEFAULT_ANGULAR_DAMPING.y;
    Parms.AngularDamping[2] = PhySys::DEFAULT_ANGULAR_DAMPING.z;
    Parms.CollisionGroupId = 0;
    Parms.Type = 2;
    Parms.bFlag1c = 0;
    Parms.bEnableCollisions = 1;
    Parms.Name[0] = 0;
    Parms.bHasInertia = 0;
    Parms.Inertia.x = 0.0f;
    Parms.Inertia.y = 0.0f;
    Parms.Inertia.z = 0.0f;
}

// SpaceObj::Launch -- baseline module storage .. module storage (ordinal 0x1a520, cdecl).
// Ported 1:1 2026-09-24 (claude-code:w5-server-06cf):
// (*launch_obj_id & 0x3fffffff) == 0 -> -4
// the routine(*launch_obj_id, &obj, &sys) false -> -2
// the routine(*dock_obj_id, &dock, &docksys) false -> -3
//                         (baseline reuses the two parameter slots as the outs)
// __RTDynamicCast(dock -> EqObj) into EDI and
//                         __RTDynamicCast(obj -> Ship) into ESI; neither is tested
// docksys == sys -> straight to the launch
// the default transfer parms; sys->remove_ship(ship, &parms)
//                         (the routine)
// pos = 0, orient = identity;
//                         docksys->add_ship(ship, &parms, pos, orient) (the routine)
// docksys->launch(ship, eqobj, launch_type) (the routine)
// return 0
SERVER_API int __cdecl Launch(const unsigned int& launch_obj_id, const unsigned int& dock_obj_id,
                               int launch_type) {
    if ((launch_obj_id & 0x3fffffffu) == 0) return -4;
    GameObject* LaunchObj = nullptr;
    StarSystem* LaunchSystem = nullptr;
    if (!Server_FindLiveObject(launch_obj_id, LaunchObj, LaunchSystem))
        return -2;
    GameObject* DockObj = nullptr;
    StarSystem* DockSystem = nullptr;
    if (!Server_FindLiveObject(dock_obj_id, DockObj, DockSystem))
        return -3;

    ::EqObj* const DockEq = dynamic_cast< ::EqObj*>(DockObj);
    ::Ship* const ShipRw = dynamic_cast< ::Ship*>(LaunchObj);

    if (DockSystem != LaunchSystem) {
        PhySys::CreateParms Parms;
        SpaceObj_InitTransferParms(Parms);
        LaunchSystem->remove_ship(ShipRw, &Parms);
        Vector Pos;
        Pos.x = 0.0f;
        Pos.y = 0.0f;
        Pos.z = 0.0f;
        Matrix const Orient(1.0f, 0.0f, 0.0f,                            // ..
                            0.0f, 1.0f, 0.0f,
                            0.0f, 0.0f, 1.0f);
        DockSystem->add_ship(ShipRw, &Parms, Pos, Orient);
    }
    DockSystem->launch(ShipRw, DockEq, launch_type);
    return 0;
}

// SpaceObj::JumpIn -- baseline module storage .. module storage (ordinal 0x1a220, cdecl).
// Ported 1:1 2026-09-24 (claude-code:w5-server-06cf):
// (*ship_id & 0x3fffffff) == 0 -> -4
// the routine(*ship_id, &ship, &sys) false -> -2
// the routine(*gate_id, &gate, &gatesys) false -> -3
// the default transfer parms
// sys == 0 -> the routine (Server_TakeInTransit(ship, parms));
//                         else gatesys != sys -> sys->remove_ship(ship, &parms)
// gate vtable +0x250 (Solar::ChooseSystemArrivalLocation)
//                         (ship, &pos, &orient) -- called on whatever the gate is
// gatesys != sys -> gatesys->add_ship(ship, &parms, pos, orient)
// gatesys->arrive_ship(ship, gate, pos, orient) (the routine)
// return 0
SERVER_API int __cdecl JumpIn(const unsigned int& ship_id, const unsigned int& gate_id) {
    if ((ship_id & 0x3fffffffu) == 0) return -4;
    GameObject* ShipObj = nullptr;
    StarSystem* ShipSystem = nullptr;
    if (!Server_FindLiveObject(ship_id, ShipObj, ShipSystem))
        return -2;
    GameObject* GateObj = nullptr;
    StarSystem* GateSystem = nullptr;
    if (!Server_FindLiveObject(gate_id, GateObj, GateSystem))
        return -3;

    PhySys::CreateParms Parms;
    SpaceObj_InitTransferParms(Parms);
    if (ShipSystem == nullptr)
        Server_TakeInTransit(ShipObj, Parms);
    else if (GateSystem != ShipSystem)
        ShipSystem->remove_ship(ShipObj, &Parms);

    Vector Pos;
    Matrix Orient;
    static_cast<Solar*>(GateObj)->ChooseSystemArrivalLocation(ShipObj, Pos, Orient);
    if (GateSystem != ShipSystem)
        GateSystem->add_ship(ShipObj, &Parms, Pos, Orient);
    GateSystem->arrive_ship(ShipObj, GateObj, Pos, Orient);
    return 0;
}

// SpaceObj::Relocate -- baseline module storage .. module storage (ordinal 0x1a370, cdecl).
// Ported 1:1 2026-09-24 (claude-code:w5-server-06cf):
// (*obj_id & 0x3fffffff) == 0 -> -4
// the routine(*obj_id, &obj, &sys) false -> -2
// target = (*system_id & 0x3fffffff) ? the routine(*system_id)
//                         (SysDB_LookupSystem) : 0
// the default transfer parms
// sys == 0 -> the routine (Server_TakeInTransit(obj, parms));
//                         else target != sys -> sys->remove_ship(obj, &parms)
// / 06cfa4f0   target == 0 -> the routine (Server_PushInTransit(obj,
//                         parms)), return 0
// / 06cfa4b6   target == sys -> the object's raw CObject (CBase::Object
//                         under the (& 1) test, else null, NOT null-tested after)
//                         vtable +0x7c beam_object(pos, orient, false), return 0
// else target->add_ship(obj, &parms, pos, orient)
// the 0x503-narrowed CShip, when there is one:
//                         CEqObj::set_control_exclusion(0); return 0
SERVER_API int __cdecl Relocate(const unsigned int& obj_id, const unsigned int& system_id,
                                 const Vector& pos, const Matrix& orient) {
    if ((obj_id & 0x3fffffffu) == 0) return -4;
    GameObject* Obj = nullptr;
    StarSystem* OwnSystem = nullptr;
    if (!Server_FindLiveObject(obj_id, Obj, OwnSystem))
        return -2;

    StarSystem* TargetSystem = nullptr;
    if ((system_id & 0x3fffffffu) != 0)
        TargetSystem = SysDB_LookupSystem(system_id);

    PhySys::CreateParms Parms;
    SpaceObj_InitTransferParms(Parms);
    if (OwnSystem == nullptr)
        Server_TakeInTransit(Obj, Parms);
    else if (TargetSystem != OwnSystem)
        OwnSystem->remove_ship(Obj, &Parms);

    if (TargetSystem == nullptr) {
        Server_PushInTransit(Obj, Parms);
        return 0;
    }
    if (TargetSystem == OwnSystem) {
        CObject* Raw = Obj->Object;
        if (Raw != nullptr && (static_cast<unsigned int>(Raw->ObjectClass) & 1u) != 1u)
            Raw = nullptr;
        Raw->beam_object(pos, orient, false);
        return 0;
    }
    TargetSystem->add_ship(Obj, &Parms, pos, orient);
    CObject* const ShipRaw = Obj->Object;
    if (ShipRaw != nullptr &&
        (static_cast<unsigned int>(ShipRaw->ObjectClass) & 0x503u) == 0x503u)
        static_cast<CShip*>(ShipRaw)->set_control_exclusion(0);
    return 0;
}

// SpaceObj::SendComm -- baseline module storage .. module storage (ordinal 401, cdecl).
// Ported 1:1 2026-09-24 (claude-code:w5-server-06cf):
// text_ids == 0 or text_count <= 0 (signed JLE) -> -4
// (target_id & 0x3fffffff) != 0: the routine(&target_id,
//                         &obj, &sys); a miss zeroes sys. It is the SYSTEM that is
//                         kept and tested, not the object.
// no system yet: (sender_id & 0x3fffffff) == 0 -> -4, else
//                         the same lookup on sender_id; no system -> -4
// (sender_id & 0x3fffffff) != 0: the routine(sender_id)
//                         (Server_LookupLiveObject); its CBase::Object (+0x10) when
//                         (ObjectClass & 0x103) == 0x103 (a CEqObj): voice_id == 0 ->
//                         CEqObj+0x13c, costume == 0 -> &CEqObj+0x108 (13 dwords), and
//                         comm_type is ZEROED (06cf9320 XOR EDI,EDI) whenever that
//                         branch is taken
// no costume -> -2
// sys->the routine(sender_id, target_id, voice, *costume (13
//                         dwords by value), comm_type, text_ids, text_count, mood,
//                         duration, interrupt) -- StarSystem::send_comm
// return 0
SERVER_API int __cdecl SendComm(unsigned int sender_id, unsigned int target_id,
                                 unsigned int voice_id, const Costume* costume,
                                 unsigned int comm_type, unsigned int* text_ids,
                                 int text_count, unsigned int mood,
                                 float duration, bool interrupt) {
    if (text_ids == nullptr) return -4;
    if (text_count <= 0) return -4;

    GameObject* LookupObj = nullptr;
    StarSystem* System = nullptr;
    if ((target_id & 0x3fffffffu) != 0) {
        if (!Server_FindObject(target_id, LookupObj, System))
            System = nullptr;
    }
    if (System == nullptr) {
        if ((sender_id & 0x3fffffffu) == 0) return -4;
        if (!Server_FindObject(sender_id, LookupObj, System))
            System = nullptr;
        if (System == nullptr) return -4;
    }

    unsigned int Voice = voice_id;                                        // EBX
    const Costume* Cost = costume;                                        // ESI
    unsigned int CommType = comm_type;                                    // EDI
    if ((sender_id & 0x3fffffffu) != 0) {
        CBase* const SenderBase = Server_LookupLiveObject(sender_id);
        if (SenderBase != nullptr) {
            CObject* const Cobj = SenderBase->Object;
            if (Cobj != nullptr &&
                (static_cast<unsigned int>(Cobj->ObjectClass) & 0x103u) == 0x103u) {
                CEqObj* const Eq = static_cast<CEqObj*>(Cobj);
                if (Voice == 0)
                    Voice = Eq->VoiceId;
                if (costume == nullptr)
                    Cost = &Eq->SpaceCostume;                             // (+0x108)
                CommType = 0;
            }
        }
    }
    if (Cost == nullptr) return -2;

    System->send_comm(sender_id, target_id, Voice, *Cost, CommType, text_ids, text_count,
                      mood, duration, interrupt);
    return 0;
}

// SpaceObj::JettisonEquipment — jettisons equipment/cargo from ship
// baseline (0x19390): the routine resolve object, vtable[1]+0x38 dispatch
//   Return value mapping: (result != -2) - 1 & 0xFFFFFFFD
SERVER_API int __cdecl JettisonEquipment(const unsigned int& obj_id,
                                          const unsigned short& equip_id, const int& quantity) {
    // baseline has NO `obj_id == 0` guard here: it goes straight to
    // (&obj_id, &obj, &owner) and returns -2 when that fails.
    GameObject* obj = Server_LookupGameObject(obj_id);
    if (!obj) return -2;
    // IObjAffect slot 14 (common_iface.h) with
    // (MOVZX *equip_id, *quantity, 0), then `(result != -2) - 1 & 0xfffffffd` -- 0 on
    // success, -3 when the handler reported -2. The previous port invented message id 0x1E.
    int const Result = obj->slot_38(equip_id, quantity, 0);
    return (Result != -2) ? 0 : -3;
}

// SpaceObj::GetArchetypeID — gets archetype ID from good ID
// baseline: simply calls Good2Arch on the input value
SERVER_API int __cdecl GetArchetypeID(const unsigned int& good_id, unsigned int& out_arch_id) {
    // baseline is three statements with no entry guard, and it writes the out-param on
    // the failure path too (Server):
    //   uVar1 = Good2Arch(*param_1); *param_2 = uVar1;
    //   return (uVar1 != 0) - 1 & 0xfffffffe;      // 0 on success, -2 on failure
    unsigned int arch = Good2Arch(good_id);
    out_arch_id = arch;
    return (arch != 0) ? 0 : -2;
}

// SpaceObj::GetAtmosphereRange — gets atmosphere range for planets/suns
// baseline analysis at 0x19800: checks type flags & 0x303, reads +0x1ac and +0x38
SERVER_API int __cdecl GetAtmosphereRange(const unsigned int& obj_id, float& out_range) {
    if ((obj_id & 0x3fffffff) == 0) return -4;
    GameObject* obj = Server_LookupGameObject(obj_id);
    if (!obj) return -2;
    ::Solar* SolarRw = dynamic_cast< ::Solar*>(obj);
    CSolar* Cs = (SolarRw != nullptr) ? SolarRw->csolar() : nullptr;
    // baseline check: ObjectClass & 0x303 must equal 0x303 (planet or sun).
    if (Cs == nullptr) {
        out_range = 0.0f;
        return 0;
    }
    float atmo = Cs->AtmosphereRange;   // CSolar+0x1ac
    out_range = atmo;
    // baseline: if atmosphere > 0, subtract the object's radius (EngineObject+0x38).
    if (atmo > 0.0f) {
        out_range = atmo - Cs->Radius;
    }
    return 0;
}

// SpaceObj::GetScannerRange — gets scanner ranges from equipped scanner
// baseline: finds scanner equip (type 0x80000), refuses a destroyed one, reads radar/cargo scan ranges
SERVER_API int __cdecl GetScannerRange(unsigned int obj_id, int& radar_range, int& cargo_scan_range) {
    if ((obj_id & 0x3fffffff) == 0) return -4;
    GameObject* obj = Server_LookupGameObject(obj_id);
    if (!obj) return -2;
    // baseline: (ObjectClass & 0x503) == 0x503, i.e. ships only.
    ::Ship* ShipRw = dynamic_cast< ::Ship*>(obj);
    CShip* Cs = (ShipRw != nullptr) ? ShipRw->cship() : nullptr;
    if (Cs == nullptr) return -1;
    // baseline: CEquipManager::FindFirst(cobj + 0xe4, 0x80000). No scanner shares the -2
    // path with the failed lookup (JZ module storage -> the branch, out params untouched).
    CEquip const* Found = Cs->EquipManager.FindFirst(EQUIP_TYPE_SCANNER);
    CEScanner const* Scanner = dynamic_cast<CEScanner const*>(Found);
    if (Scanner == nullptr) return -2;
    // -3 when the scanner's vtable +0x08 returns TRUE, out
    // params untouched. In ??_7CEScanner@@6BCInternalEquip@@@ (Common.dll 0x0639cacc) +0x00
    // is the destructor, +0x04 CEquip::IsActive, +0x08 CEquip::IsDestroyed (0x06286d10) and
    // +0x0c CEquip::IsFunctioning (0x06286d20): only a destroyed scanner refuses.
    if (Scanner->IsDestroyed()) return -3;
    radar_range = (int)(long)Scanner->GetRadarRange();
    cargo_scan_range = (int)(long)Scanner->GetCargoScanRange();
    return 0;
}

// SpaceObj::GetType — gets object type flags
// baseline analysis at 0x18da0: checks flags & 3 == 3, reads +0xe0. out_type = 0 comes
// first (module storage, before the id test), so every failure path returns it zeroed.
SERVER_API int __cdecl GetType(unsigned int obj_id, unsigned int& out_type) {
    out_type = 0;
    if ((obj_id & 0x3fffffff) == 0) return -4;
    GameObject* obj = Server_LookupGameObject(obj_id);
    if (!obj) return -2;
    // baseline: (ObjectClass & 3) == 3, which is exactly GameObject::cobj()'s guard,
    // then read CSimple::TypeBits at +0xe0.
    CSimple* Cs = obj->csimple();
    if (Cs == nullptr) return 0;
    out_type = Cs->get_type();
    return 0;
}

// SpaceObj::Activate -- baseline module storage .. module storage (cdecl).
// Ported 1:1 2026-09-24 (claude-code:w5-server-06cf):
// (*obj_id & 0x3fffffff) == 0 -> -4
// the routine(*obj_id, &obj, &owner) (Server_FindLiveObject;
//                         obj in the reused param_1 slot) false -> -2
// IObjInspect +0x88 get_type(&flags) on the object
// activate clear and (flags & 0x400) -> 0
// owner->activate_object(obj, activate, param) (the routine)
// NEG/SBB/AND 3/ADD -3: true -> 0, false -> -3
// The owner system is used without a null test, as baseline does.
SERVER_API int __cdecl Activate(const unsigned int& obj_id, bool activate, int param) {
    if ((obj_id & 0x3fffffffu) == 0) return -4;
    GameObject* Obj = nullptr;
    StarSystem* Owner = nullptr;
    if (!Server_FindLiveObject(obj_id, Obj, Owner)) return -2;
    unsigned int Flags;
    Obj->get_type(Flags);
    if (!activate && (Flags & 0x400u) != 0) return 0;                    // / 06cf9b0d
    return Owner->activate_object(Obj, activate, static_cast<unsigned int>(param)) ? 0 : -3;
}

// SpaceObj::ActivateEquipment -- baseline module storage .. module storage (ordinal 0x193f0,
// cdecl); switch table at module storage.
// ?ActivateEquipment@SpaceObj@pub@@YA?AW4EQUIPMENT_RTYPE@@ABIPAUEQInfo@12@@Z
// Ported 1:1 2026-09-24 (claude-code:w5-server-06cf). EBP carries the result:
// EBP = -5; (*obj_id & 0x3fffffff) == 0 -> -5
// EBP = -1; the routine(obj_id, &obj, &sys) false or obj == 0
//                         -> -1 (the system out is not used)
// info->Type > 3 (unsigned) -> -1
//   case 0  06cf944e      Count != 1 -> -1; else IObjAffect +0x50 (slot_50)(1, 1, 0);
//                         NEG/SBB/AND 0xfffffffb: non-zero -> -5, zero -> 0
//   case 1  06cf9476      Field_0x0c == 0 -> -1; the routine(Field_0x0c) null -> -5;
//                         else the object's IObjInspect +0x120 (scan_cargo)(other,
//                         &CargoOut); non-zero -> -5, zero -> 0
//   case 2  06cf94b9      CBase::Object null or (class & 0x103) != 0x103 -> -1;
//                         CEquipManager::FindFirst(0x1000) null -> -2; else
//                         IObjAffect +0x4c (slot_4c)(&its SubObjId, byte +4, 0) -> 0
//   case 3  06cf9535      the same CEqObj test -> -1; EBP = 0; CEquipTraverser(1); for
//                         each CEquipManager::Traverse hit whose CELightEquip::LightArch()
//                         bDockingLight (+0x71) is set: slot_4c(&SubObjId, byte +4, 0);
//                         return 0
// The EQUIPMENT_RTYPE names are the port's; the values (-5/-2/-1/0) are baseline's.
SERVER_API EQUIPMENT_RTYPE __cdecl ActivateEquipment(const unsigned int& obj_id_ref, EQInfo* eq_info) {
    if ((obj_id_ref & 0x3fffffffu) == 0) return EQUIPMENT_RTYPE_BAD_TYPE;
    GameObject* Obj = nullptr;
    StarSystem* System = nullptr;
    if (!Server_FindObject(obj_id_ref, Obj, System))
        return EQUIPMENT_RTYPE_INVALID;
    if (Obj == nullptr) return EQUIPMENT_RTYPE_INVALID;

    switch (eq_info->Type) {                                             // (JA -> -1)
    case 0:
        if (eq_info->Count != 1) return EQUIPMENT_RTYPE_INVALID;
        return Obj->slot_50(true, true, 0) != 0 ? EQUIPMENT_RTYPE_BAD_TYPE
                                                 : EQUIPMENT_RTYPE_OK;
    case 1: {
        if (eq_info->Field_0x0c == 0) return EQUIPMENT_RTYPE_INVALID;
        GameObject* const Other = Server_LookupLiveObject(eq_info->Field_0x0c);
        if (Other == nullptr) return EQUIPMENT_RTYPE_BAD_TYPE;   // / 06cf94af
        EquipDescVector& Cargo = *reinterpret_cast<EquipDescVector*>(eq_info->CargoOut);
        return Obj->scan_cargo(Other, Cargo) != 0 ? EQUIPMENT_RTYPE_BAD_TYPE
                                                   : EQUIPMENT_RTYPE_OK;
    }
    case 2: {
        CObject* const Raw = Obj->Object;
        if (Raw == nullptr ||
            (static_cast<unsigned int>(Raw->ObjectClass) & 0x103u) != 0x103u)
            return EQUIPMENT_RTYPE_INVALID;
        CEqObj* const Ceq = static_cast<CEqObj*>(Raw);
        CEquip const* const Launcher = Ceq->EquipManager.FindFirst(0x1000u);
        if (Launcher == nullptr) return EQUIPMENT_RTYPE_NOT_FOUND;
        unsigned short const SubObjId = Launcher->Id;
        Obj->slot_4c(&SubObjId, static_cast<unsigned char>(eq_info->EquipId) != 0, 0);
        return EQUIPMENT_RTYPE_OK;
    }
    case 3: {
        CObject* const Raw = Obj->Object;
        if (Raw == nullptr ||
            (static_cast<unsigned int>(Raw->ObjectClass) & 0x103u) != 0x103u)
            return EQUIPMENT_RTYPE_INVALID;
        CEqObj* const Ceq = static_cast<CEqObj*>(Raw);
        CEquipTraverser Walk(1);
        CEquip* Light = Ceq->EquipManager.Traverse(Walk);
        while (Light != nullptr) {                                       // / 06cf95c9
            if (static_cast<CELightEquip*>(Light)->LightArch()->bDockingLight) {
                unsigned short const SubObjId = Light->Id;
                Obj->slot_4c(&SubObjId, static_cast<unsigned char>(eq_info->EquipId) != 0, 0);
            }
            Light = Ceq->EquipManager.Traverse(Walk);
        }
        return EQUIPMENT_RTYPE_OK;                          // (EBP = 0)
    }
    default:
        return EQUIPMENT_RTYPE_INVALID;
    }
}

// SpaceObj::SetInvincible -- baseline module storage .. module storage (ordinal 419, cdecl).
// Ported 1:1 2026-09-24 (claude-code:w5-server-06cf). ESI = -4 up front; both branches
// test obj_id against 0 as a whole dword (not the 0x3fffffff mask), set ESI = -2,
// (Server_FindLiveObject) into the reused parameter slots, and return -2
// when the lookup fails or the object's StarSystem is null. Then the object's vtable
// +0x194 (GameObject::set_invincible) gets a 0x0c-byte FInvincibility on the stack:
//   invincible (06cf8749 .. 06cf8776): { Type 3, bProtect 1, bFromPlayers = !param3
//                                        (TEST/SETZ), MaxHpLoss = duration }
//   otherwise  (06cf87ad .. 06cf87d2): { Type 3, bProtect 0, bFromPlayers 0,
//                                        MaxHpLoss = 1.0f }
// and the result is 0. The two pad bytes (+6/+7) are never written in baseline; the port
// zeroes them (deviations.md, zeroed indeterminate stack bytes).
SERVER_API int __cdecl SetInvincible(unsigned int obj_id, bool invincible, bool param3, float duration) {
    if (obj_id == 0) return -4;                                          // / 06cf8787
    GameObject* Obj = nullptr;
    StarSystem* System = nullptr;
    if (!Server_FindLiveObject(obj_id, Obj, System)) return -2;          // / 06cf8799
    if (System == nullptr) return -2;                                    // / 06cf87ab

    FInvincibility Cmd;
    Cmd.Type = 3;                                                        // / 06cf87b5
    Cmd.Pad06[0] = 0;
    Cmd.Pad06[1] = 0;
    if (invincible) {
        Cmd.bProtect = true;
        Cmd.bFromPlayers = !param3;                                      // / 06cf8756
        Cmd.MaxHpLoss = duration;
    } else {
        Cmd.bProtect = false;
        Cmd.bFromPlayers = false;
        Cmd.MaxHpLoss = 1.0f;
    }
    Obj->set_invincible(Cmd);                                            // / 06cf87d2
    return 0;
}

// SpaceObj::AddImpulse — adds physics impulse to an object
// baseline (0x190b0, Server): mask check, the routine(*obj_id),
// type & 3 == 3, and then — only when the physics body at CObject+0x54 is non-null —
// PhySys::AddImpulseAtPoint(cobj, impulse, point). Returns 0 either way.
// The previous port invented message id 0x28 for this.
SERVER_API int __cdecl AddImpulse(const unsigned int& obj_id_ref, const Vector& impulse, const Vector& point) {
    if ((obj_id_ref & 0x3fffffff) == 0) return -4;
    GameObject* obj = Server_LookupLiveObject(obj_id_ref);
    if (!obj) return -2;
    // Listing module storage: ADD EAX,0xc / MOV EAX,[EAX+4] reaches CBase::Object,
    // [cobj+0x4c] & 3 == 3 zeroes it otherwise, then MOV ECX,[cobj+0x54] -- the physics
    // body -- gates the PhySys call.
    CObject* Cobj = obj->cobj();
    if (Cobj == nullptr) return 0;
    if (Cobj->BodyPtr != nullptr) {
        PhySys::AddImpulseAtPoint(Cobj, impulse, point);
    }
    return 0;
}

// SpaceObj::GetLocation — gets position and orientation of an object
// baseline (0x18732): the routine lookup, vtable[0] get_position, vtable[3] get_orientation
SERVER_API int __cdecl GetLocation(unsigned int obj_id, Vector& out_pos, Matrix& out_orient) {
    out_pos.x = 0.0f; out_pos.y = 0.0f; out_pos.z = 0.0f;
    if ((obj_id & 0x3fffffff) == 0) return -4;
    GameObject* obj = Server_LookupGameObject(obj_id);
    if (!obj) return -2;
    // baseline: IObjInspect slot 0 (+0x00) get_position for the three dwords, then
    // slot 3 (+0x0c) get_transform and a nine-dword REP MOVSD off the result -- i.e.
    // the Transform's leading Matrix (Transform is Matrix Rotation + Vector
    // Translation, see common_csys.h). Slots 3 and 4 are ICF-folded in baseline, so
    // that is the orientation.
    out_pos = obj->get_position();
    out_orient = obj->get_transform().Rotation;
    return 0;
}

// SpaceObj::GetSystem -- baseline module storage .. module storage (cdecl).
// Ported 1:1 2026-09-24 (claude-code:w5-server-06cf):
// *out = 0 first
// (obj_id & 0x3fffffff) == 0 -> -4
// obj = sys = 0; the routine(obj_id, &obj, &sys)
//                         (Server_FindLiveObject) false -> -2
// sys: *out = the routine(sys) (SystemInfo vtable slot 0, the
//                         system's id -- ISystem::NicknameId in this port), return 0
// no sys (an in-transit object): *out = 0, return 0
SERVER_API int __cdecl GetSystem(unsigned int obj_id, unsigned int& out_system_id) {
    out_system_id = 0;
    if ((obj_id & 0x3fffffffu) == 0) return -4;
    GameObject* Obj = nullptr;
    StarSystem* System = nullptr;
    if (!Server_FindLiveObject(obj_id, Obj, System)) return -2;
    if (System == nullptr) {
        out_system_id = 0;
        return 0;
    }
    out_system_id = System->SystemInfo->NicknameId;                      // the routine
    return 0;
}

// SpaceObj::GetHealth — gets current and max health of an object
// baseline (0x20596): the routine lookup, vtable[15] (+0x3C) get_health(current, max)
SERVER_API int __cdecl GetHealth(const unsigned int& obj_id_ref, float& out_current, float& out_max) {
    out_current = 0.0f; out_max = 0.0f;
    if ((obj_id_ref & 0x3fffffff) == 0) return -4;
    GameObject* obj = Server_LookupGameObject(obj_id_ref);
    if (!obj) return -2;
    // baseline: ONE call to IObjInspect slot 15 (+0x3c), get_status(HitPts, MaxHitPts),
    // which fills both out-params.
    obj->get_status(out_current, out_max);
    return 0;
}

// SpaceObj::GetRelativeHealth — gets normalized health ratio (0.0-1.0)
// baseline (0x20551): the routine lookup, vtable[16] (+0x40) get_relative_health
// NOTE: baseline only checks != 0, NOT the 0x3fffffff mask (unique among SpaceObj functions)
SERVER_API int __cdecl GetRelativeHealth(const unsigned int& obj_id_ref, float& out_ratio) {
    out_ratio = 0.0f;
    if (obj_id_ref == 0) return -4;
    GameObject* obj = Server_LookupGameObject(obj_id_ref);
    if (!obj) return -2;
    // baseline: IObjInspect slot 16 (+0x40), get_status(Health) -- the single-argument
    // overload, which is the relative one.
    obj->get_status(out_ratio);
    return 0;
}

// SpaceObj::ExistsAndAlive — checks if object exists and is alive
// baseline (0x18612): the routine lookup (includes alive check), returns 0 or -2
SERVER_API int __cdecl ExistsAndAlive(unsigned int obj_id) {
    if ((obj_id & 0x3fffffff) == 0) return -2;
    // The whole point of this export is the liveness half of the routine — a plain
    // registry hit is not enough.
    GameObject* obj = Server_LookupLiveObject(obj_id);
    if (!obj) return -2;
    return 0;
}

// SpaceObj::GetTarget — gets the target ID of a ship object
// baseline (0x19194): the routine lookup, type check (& 0x503 == 0x503), CShip::get_target
SERVER_API int __cdecl GetTarget(const unsigned int& obj_id_ref, unsigned int& out_target_id) {
    // baseline zeroes the out-param first and never inspects its incoming value; the
    // previous `if (!out_target_id) return -4;` read an uninitialised reference.
    out_target_id = 0;
    if ((obj_id_ref & 0x3fffffff) == 0) return -4;
    GameObject* obj = Server_LookupGameObject(obj_id_ref);
    if (!obj) return -2;
    // baseline: (ObjectClass & 0x503) == 0x503 then CShip::get_target. IObjInspect slot
    // 29 (+0x74) get_target hands back the target's IObjRW, whose id is slot 8.
    ::Ship* ShipRw = dynamic_cast< ::Ship*>(obj);
    if (ShipRw == nullptr || ShipRw->cship() == nullptr) return -1;
    IObjRW* Target = nullptr;
    obj->get_target(Target);
    if (Target != nullptr) {
        out_target_id = Target->get_id();
    }
    return 0;
}

// SpaceObj::CreateSolar -- baseline module storage .. 06cf83bb (ordinal 0x180d0, cdecl).
// Ported 1:1 2026-09-24 (claude-code:w5-server-06cf). The request is a
// CSolarCreateInfo at [ESP+0x20]; the routine (StarSystem::create_solar) builds the
// Solar from it.
// the routine(info.SystemID) (SysDB_LookupSystem) null -> -4
//                         (06cf839f, before the request exists)
// the request: CEqObjCreateInfo ctor the routine, then the
//                         inlined CSolarCreateInfo part (+0xac = 0, +0xad = 0,
//                         +0xb0 = -1, vftable module storage)
// / 06cf8153   ID_String::clear on +0xb8; +0xbc = 0
// info.Nickname[0] == 0 -> result -4 (06cf82cf)
// id = CreateID(info.Nickname); *out_id = id
// +0xb8 = id, +0x04 = id, +0x68 = HitPts, +0x0c = DockingBaseId,
//                         +0x08 = SolarArchID, +0x14/+0x18/+0x1c/+0x20 = the costume head
// +0x44 = accessory count (stored before the JLE); then `count`
//                         dwords into +0x24, the count re-read from +0x44 each pass
// +0x48 = VoiceId, +0x10 = VibeGroupId, +0x4c = Position
// the field (lazily the routine) +0x50 MatrixToQuaternion(
//                         &q, &info.Orientation); +0x58..+0x64 = q
// / 06cf826e   +0x6c = LoadoutID
// Archetype::GetSolar(SolarArchID)->
//                         get_undamaged_collision_group_list(&request+0x80) -- no null
//                         test on the archetype
// +0xbc = info+0xcc, +0xb4 = info+0x80 (byte), +0xac = 1,
//                         +0xad = 1
// the routine(system, &request): result 0 when it returns a
//                         Solar, -1 when it returns null (NEG/SBB/NEG/DEC)
// ~CSolarCreateInfo inlined; return the result
SERVER_API int __cdecl CreateSolar(unsigned int& out_id, const SpaceObj::SolarInfo& solar_info) {
    FStarSystem* const System = SysDB_LookupSystem(solar_info.SystemID);
    if (System == nullptr) return -4;

    CSolarCreateInfo Request;
    Request.Field_0xb8.clear();
    Request.Field_0xbc = 0u;
    int Result;
    if (solar_info.Nickname[0] == '\0') {
        Result = -4;
    } else {
        unsigned int const Id = CreateID(solar_info.Nickname);
        out_id = Id;
        Request.Field_0xb8.Id = Id;
        Request.ObjId = Id;
        Request.HitPts = solar_info.HitPts;
        Request.DockingBaseId = solar_info.DockingBaseId;
        Request.ArchetypeNickname = solar_info.SolarArchID;
        Request.CostumeBody = solar_info.CostumeBody;
        Request.CostumeHead = solar_info.CostumeHead;
        Request.CostumeRightHand = solar_info.CostumeRightHand;
        Request.CostumeLeftHand = solar_info.CostumeLeftHand;
        Request.CostumeAccessoryCount = solar_info.CostumeAccessoryCount;
        for (int Index = 0; Index < Request.CostumeAccessoryCount; ++Index) // / 06cf81f7
            Request.CostumeAccessory[Index] = solar_info.CostumeAccessory[Index];
        Request.VoiceId = solar_info.VoiceId;
        Request.VibeGroupId = solar_info.VibeGroupId;
        Request.Position = solar_info.Position;
        Quaternion SolarOrient;
        SpaceObj_MatrixToQuat(SolarOrient, solar_info.Orientation);
        Request.Orientation = SolarOrient;
        Request.Field_0x6c = solar_info.LoadoutID;
        Archetype::Solar* const SolarArch = Archetype::GetSolar(solar_info.SolarArchID);
        SolarArch->get_undamaged_collision_group_list(Request.CollisionGroups);
        Request.Field_0xbc = solar_info.Field_0xcc;
        Request.Field_0xb4 = solar_info.Field_0x80;
        Request.Field_0xac = 1u;
        Request.Field_0xad = 1u;
        ::Solar* const Created = System->create_solar(Request);
        Result = (Created != nullptr) ? 0 : -1;
    }
    return Result;
}

// SpaceObj::GetMotion — gets linear and angular velocity of an object
// baseline (0x18b20): the routine lookup, vtable[1] (+0x04) get_linear_velocity, vtable[2] (+0x08) get_angular_velocity
SERVER_API int __cdecl GetMotion(unsigned int obj_id, Vector& out_linear, Vector& out_angular) {

    out_linear.x = 0; out_linear.y = 0; out_linear.z = 0;
    out_angular.x = 0; out_angular.y = 0; out_angular.z = 0;
    if ((obj_id & 0x3fffffff) == 0) return -4;
    GameObject* obj = Server_LookupGameObject(obj_id);
    if (!obj) return -2;
    // baseline: IObjInspect slot 1 (+0x04) get_velocity and slot 2 (+0x08)
    // get_angular_velocity.
    out_linear = obj->get_velocity();
    out_angular = obj->get_angular_velocity();
    return 0;
}

// SpaceObj::GetRep — gets reputation vibe handle for a game object
// baseline (0x18ba0): the routine lookup, type check & 0x103 == 0x103, read +0x104.
// out_rep is written only on success (module storage); every failure path (-4 06cf8bb0,
// -2 06cf8bf8, -1 06cf8bf3) leaves it untouched -- Content callers preset it (often -1)
// and rely on that.
SERVER_API int __cdecl GetRep(unsigned int obj_id, int& out_rep) {
    if ((obj_id & 0x3fffffff) == 0) return -4;
    GameObject* obj = Server_LookupGameObject(obj_id);
    if (!obj) return -2;
    // baseline: (ObjectClass & 0x103) == 0x103 then read CEqObj+0x104.
    ::EqObj* EqRw = dynamic_cast< ::EqObj*>(obj);
    CEqObj* Ceq = (EqRw != nullptr) ? EqRw->ceqobj() : nullptr;
    if (Ceq == nullptr) return -1;
    out_rep = (int)Ceq->VibeId;
    return 0;
}

// SpaceObj::GetSolarRep — gets solar reputation by verifying vibe handle
// baseline (0x18c00): Reputation::Vibe::Verify(param_1), if valid sets *param_2 = param_1
SERVER_API int __cdecl GetSolarRep(unsigned int vibe_id, int& out_rep) {
    bool valid = GlobalReputation::Vibe::Verify(vibe_id);
    if (valid) {
        out_rep = (int)vibe_id;
        return 0;
    }
    return -2;
}

// SpaceObj::GetSolarArchetypeID — gets archetype hash ID from a solar object
// baseline (0x18c30): the routine lookup, type check & 0x103, read *(*(obj+0x88) + 8)
SERVER_API int __cdecl GetSolarArchetypeID(unsigned int obj_id, unsigned int& out_arch_id) {
    out_arch_id = 0;
    if ((obj_id & 0x3fffffff) == 0) return -4;
    GameObject* obj = Server_LookupGameObject(obj_id);
    if (!obj) return -2;
    ::EqObj* EqRw = dynamic_cast< ::EqObj*>(obj);
    CEqObj* Ceq = (EqRw != nullptr) ? EqRw->ceqobj() : nullptr;
    if (Ceq == nullptr) return -1;
    // baseline reads the archetype hash at *(CObject+0x88) + 8, i.e. Root::Nickname.Id.
    if (Ceq->Archetype != nullptr) {
        out_arch_id = Ceq->Archetype->get_id();
    }
    return 0;
}

// SpaceObj::GetSolarArchetypeNickname — gets nickname string for a solar archetype
// baseline (0x18ca0): Archetype::GetSolar(arch_id), ID_String::get_string at solar+8, safe_strcpy
SERVER_API int __cdecl GetSolarArchetypeNickname(char* out_name, int max_len, unsigned int arch_id) {
    if (!out_name || max_len <= 0) return -4;
    // Look up solar archetype via Common.dll
    Archetype::Solar* solar = Archetype::GetSolar(arch_id);
    if (!solar) return -2;
    // Get nickname string from ID_String at solar+8 (Root::Nickname)
    const char* nick = solar->Nickname.get_string();
    if (!nick) return -2;
    // Safe string copy with length limit
    int len = 0;
    while (nick[len] != '\0') len++;
    if (len < max_len) {
        for (int i = 0; i <= len; i++) out_name[i] = nick[i];
        return 0;
    }
    return -1;
}

// SpaceObj::GetSolarParent — gets parent solar's FLID for a solar object
// baseline (0x18d00): the routine lookup, vtable+0x150 get_solar, check & 0x303,
//   CSolar::GetParentNickname, second lookup to resolve parent FLID
SERVER_API int __cdecl GetSolarParent(const unsigned int& obj_id_ref, unsigned int& out_parent_id) {
    // baseline zeroes the out-param first (Server:20 `*param_2 = 0;`) and never
    // inspects its incoming value.
    out_parent_id = 0;
    if (obj_id_ref == 0) return -4;
    GameObject* obj = Server_LookupGameObject(obj_id_ref);
    if (!obj) return -2;
    // baseline: IObjInspect slot 84 (+0x150) cobject() to reach the CObject, check
    // (ObjectClass & 0x303) == 0x303, then CSolar::GetParentNickname.
    ::Solar* SolarRw = dynamic_cast< ::Solar*>(obj);
    CSolar* Cs = (SolarRw != nullptr) ? SolarRw->csolar() : nullptr;
    if (Cs == nullptr) return -1;
    ID_String const& Parent = Cs->GetParentNickname();
    if (Parent.IsEmpty()) return -3;
    // baseline then re-resolves the parent through a second the routine and takes its
    // IObjInspect slot 8 (+0x20) get_id. The nickname id IS the registry key here, so
    // the lookup doubles as the existence check.
    GameObject* ParentObj = Server_LookupGameObject(Parent.Id);
    if (ParentObj == nullptr) return -3;
    out_parent_id = ParentObj->get_id();
    return 0;
}

// SpaceObj::GetHardpoint — gets hardpoint position/orientation on an object
// baseline (0x18e70): the routine lookup, vtable+0x1c get_inner_object, HardpointPosition()
SERVER_API int __cdecl GetHardpoint(const unsigned int& obj_id_ref, const char* hp_name,
                                     Vector* out_pos, Matrix* out_orient) {
    if ((obj_id_ref & 0x3fffffff) == 0) return -4;
    GameObject* obj = Server_LookupGameObject(obj_id_ref);
    if (!obj) return -2;
    // Listing module storage: CALL [vtable + 0x1c] -- IObjInspect slot 7, get_index,
    // which is the engine instance handle -- then COMMON.DLL::HardpointPosition with
    // it, and `return (-(bool)result & 3) - 3`, i.e. 0 on success and -3 on failure.
    bool bOk = HardpointPosition(obj->get_index(), hp_name, out_pos, out_orient);
    return bOk ? 0 : -3;
}

// SpaceObj::GetRadius — gets bounding radius and center of an object
// baseline (0x18ed0): the routine lookup, vtable+0x2c (+0xB0) get_bounding_sphere(radius, center)
SERVER_API int __cdecl GetRadius(const unsigned int& obj_id_ref, float& out_radius, Vector& out_center) {
    out_radius = 0.0f;
    out_center.x = 0; out_center.y = 0; out_center.z = 0;
    if ((obj_id_ref & 0x3fffffff) == 0) return -4;
    GameObject* obj = Server_LookupGameObject(obj_id_ref);
    if (!obj) return -2;
    // baseline: ONE call to IObjInspect slot 11 (+0x2c), get_physical_radius, which
    // fills the radius AND the sphere centre.
    obj->get_physical_radius(out_radius, out_center);
    return 0;
}

// SpaceObj::GetBurnRadius — gets engine burn radius and position
// baseline (0x18f60): the routine lookup, vtable[0] get_position, vtable+0x140 get_burn_radius
SERVER_API int __cdecl GetBurnRadius(const unsigned int& obj_id_ref, float& out_radius, Vector& out_center) {
    out_radius = 0.0f;
    out_center.x = 0; out_center.y = 0; out_center.z = 0;
    if ((obj_id_ref & 0x3fffffff) == 0) return -4;
    GameObject* obj = Server_LookupGameObject(obj_id_ref);
    if (!obj) return -2;
    // Listing module storage / 06cf8fe5: IObjInspect slot 0 (get_position) for the
    // centre, then slot 80 (+0x140) with the out float& -- which is
    // get_atmosphere_range. "Burn radius" and "atmosphere range" are the same slot.
    out_center = obj->get_position();
    obj->get_atmosphere_range(out_radius);
    return 0;
}

// SpaceObj::GetMass — gets mass of an object
// baseline (0x19000): the routine lookup, vtable+0x30 (+0xC0) get_mass
SERVER_API int __cdecl GetMass(const unsigned int& obj_id_ref, float& out_mass) {
    if ((obj_id_ref & 0x3fffffff) == 0) return -4;
    GameObject* obj = Server_LookupGameObject(obj_id_ref);
    if (!obj) return -2;
    // baseline: *param_2 = (float)vtable+0x30(), IObjInspect slot 12 get_mass.
    out_mass = obj->get_mass();
    return 0;
}

// SpaceObj::GetCenterOfMass — gets center of mass in world space
// baseline (0x19040): the routine lookup, type check & 3 == 3, vtable+0x68 get_center_of_mass
SERVER_API int __cdecl GetCenterOfMass(const unsigned int& obj_id_ref, Vector& out_center) {

    out_center.x = 0; out_center.y = 0; out_center.z = 0;
    if ((obj_id_ref & 0x3fffffff) == 0) return -4;
    GameObject* obj = Server_LookupGameObject(obj_id_ref);
    if (!obj) return -2;
    // CORRECTION (listing module storage-06cf90a2): the +0x68 slot baseline dispatches
    // here is on the CObject's OWN vtable, not IObjInspect's -- the code does
    // ADD EAX,0xc / MOV ECX,[EAX+4] to reach CBase::Object first, checks
    // [cobj+0x4c] & 3 == 3, and only then CALL [cobj_vtable + 0x68], which is
    // CObject::get_center_of_mass. (IObjInspect's own +0x68 is get_attitude_towards,
    // which takes two arguments and would make no sense here.) baseline then calls
    // through the pointer even when the type check zeroed it -- a genuine baseline
    // null-deref path the port declines to reproduce.
    CObject* Cobj = obj->cobj();
    if (Cobj == nullptr) return -2;
    out_center = Cobj->get_center_of_mass();
    return 0;
}

// SpaceObj::GetVoiceID — gets voice ID for comm messages
// baseline (0x191a0): the routine lookup (4-tier), the routine alive check,
//   type check & 0x103 == 0x103, read +0x13c
SERVER_API int __cdecl GetVoiceID(const unsigned int& obj_id_ref, unsigned int& out_voice_id) {
    // baseline's only entry guard is `if (*param_1 == 0) return -4;` — it never reads the
    // out-param, and it does NOT mask with 0x3fffffff here.
    if (obj_id_ref == 0) return -4;
    GameObject* obj = Server_LookupGameObject(obj_id_ref);
    if (!obj) return -2;
    ::EqObj* EqRw = dynamic_cast< ::EqObj*>(obj);
    CEqObj* Ceq = (EqRw != nullptr) ? EqRw->ceqobj() : nullptr;
    if (Ceq == nullptr) return -3;
    out_voice_id = Ceq->VoiceId;    // CEqObj+0x13c
    return 0;
}

// SpaceObj::GetInvincible — gets invincibility state of an object
// baseline06cf88a0..8922: live lookup and three sequential raw output stores.
SERVER_API int __cdecl GetInvincible(unsigned int ObjId, unsigned char& OutFromPlayers,
                                     unsigned char& OutProtect, float& OutMaxHpLoss) {
    if (ObjId == 0) return -4;
    GameObject* Obj = nullptr;
    StarSystem* System = nullptr;
    if (!Server_FindLiveObject(ObjId, Obj, System)) return -2;
    if (System == nullptr) return -2;
    OutFromPlayers = Obj->get_invincible_flag();  // +0x198 -> raw byte +0x39
    OutProtect = Obj->is_invincible();            // +0x19c -> raw byte +0x38
    OutMaxHpLoss = Obj->get_invincible_timer();   // +0x1a0 -> float +0x3c
    return 0;
}

// SpaceObj::GetCargoSpaceOccupied — gets cargo hold usage for a ship
// (0x196d0, 39 instructions):
// *out = 0, before anything else
// id & 0x3fffffff == 0 -> -4
// the routine (Server_LookupLiveObject); null -> -2
// [obj+0x10] narrowed on (ObjectClass & 0x503) == 0x503, else 0
// MOV ESI,[ECX+0x88]  -- CObject::Archetype, NO null test on the narrowing
// CALL CShip::get_cargo_hold_remaining
// FSUBR float [ESI+0x100]  -- Archetype::Ship::HoldSize - remaining, unrounded
// ftol -> *out; return 0
// The inspect hides the FSUBR; occupied = capacity - remaining, as the name says.
SERVER_API int __cdecl GetCargoSpaceOccupied(const unsigned int& obj_id_ref, unsigned int& out_occupied) {
    out_occupied = 0;
    if ((obj_id_ref & 0x3fffffff) == 0) return -4;
    GameObject* const Obj = Server_LookupLiveObject(obj_id_ref);
    if (Obj == nullptr) return -2;
    CObject* const Raw = Obj->cobj();
    CShip* const ShipObj =
        (Raw != nullptr && ((unsigned int)Raw->ObjectClass & 0x503u) == 0x503u) ? (CShip*)Raw : nullptr;
    Archetype::Ship const* const Arch = (Archetype::Ship const*)ShipObj->Archetype;
    double const Remaining = ShipObj->get_cargo_hold_remaining();
    out_occupied = (unsigned int)(long long)(Arch->HoldSize - Remaining);
    return 0;
}

// SpaceObj::GetDockingTarget — gets the docking target FLID for an object
// baseline (0x19880): the routine lookup, type check & 0x103 == 0x103,
//   read +0x164 first, fallback to +0x160 if secondary is invalid FLID
SERVER_API int __cdecl GetDockingTarget(const unsigned int& obj_id_ref, unsigned int& out_target) {
    // baseline zeroes the out-param first and never inspects its incoming value.
    out_target = 0;
    if ((obj_id_ref & 0x3fffffff) == 0) return -4;
    void* obj_raw = Server_LookupObject(obj_id_ref);
    if (!obj_raw) return -2;
    ::EqObj* EqRw = dynamic_cast< ::EqObj*>(Server_LookupGameObject(obj_id_ref));
    CEqObj* Ceq = (EqRw != nullptr) ? EqRw->ceqobj() : nullptr;
    if (Ceq == nullptr) return 0;
    // baseline prefers CEqObj+0x164 (DockingPointBaseId), falls back to +0x160
    // (DockingBaseId).
    unsigned int secondary = Ceq->DockingPointBaseId;
    if ((secondary & 0x3fffffff) != 0) {
        out_target = secondary;
    } else {
        out_target = Ceq->DockingBaseId;
    }
    return 0;
}

// SpaceObj::SetRelativeHealth -- baseline module storage .. module storage (cdecl).
// Ported 1:1 2026-09-24 (claude-code:w5-server-06cf):
// FCOMP ratio against module storage (0.0f), TEST AH,0x1 / JNZ:
//                         -4 when ratio < 0 or unordered (C0 is set for NaN too)
// FCOMP against module storage (1.0f), TEST AH,0x41 / JP: -4
//                         when C0 == C3, i.e. ratio > 1 or unordered
// *obj_id == 0 as a whole dword (no 0x3fffffff mask) -> -4
// the routine (Server_LookupLiveObject); null -> -2
// the wrapper's vtable +0x1ac set_relative_health(ratio)
// return 0
SERVER_API int __cdecl SetRelativeHealth(const unsigned int& obj_id_ref, float ratio) {
    if (!(ratio >= 0.0f)) return -4;
    if (!(ratio <= 1.0f)) return -4;
    if (obj_id_ref == 0) return -4;
    GameObject* const Obj = Server_LookupLiveObject(obj_id_ref);
    if (Obj == nullptr) return -2;
    Obj->set_relative_health(ratio);
    return 0;
}

// SpaceObj::GetShieldHealth — gets shield current/max health and active state
// baseline (0x1abb0): the routine lookup, vtable+0x44 (+0x110) get_shield_health(cur, max, active)
SERVER_API int __cdecl GetShieldHealth(const unsigned int& obj_id_ref, float& out_current,
                                        float& out_max, bool& out_active) {
    if ((obj_id_ref & 0x3fffffff) == 0) return -4;
    GameObject* obj = Server_LookupGameObject(obj_id_ref);
    if (!obj) return -2;
    // baseline dispatches ONE slot, vtable +0x44 (IObjInspect slot 17,
    // get_shield_status(cur, max, active)), which fills all three out-params.
    obj->get_shield_status(out_current, out_max, out_active);
    return 0;
}

// SpaceObj::GetTradelaneNextAndPrev — gets next/prev tradelane ring IDs for a tradelane object
// baseline (0x1ac00): the routine lookup, type check & 0x303 == 0x303, read +0x1dc/+0x1d8
SERVER_API int __cdecl GetTradelaneNextAndPrev(const unsigned int& obj_id_ref,
                                                unsigned int& out_next, unsigned int& out_prev) {

    if ((obj_id_ref & 0x3fffffff) == 0) return -4;
    void* obj_raw = Server_LookupObject(obj_id_ref);
    if (!obj_raw) return -2;
    ::Solar* SolarRw = dynamic_cast< ::Solar*>(Server_LookupGameObject(obj_id_ref));
    CSolar* Cs = (SolarRw != nullptr) ? SolarRw->csolar() : nullptr;
    if (Cs == nullptr) return -1;
    out_next = Cs->NextRing;   // CSolar+0x1dc
    out_prev = Cs->PrevRing;   // CSolar+0x1d8
    return 0;
}

// SpaceObj::LightFuse -- baseline module storage .. module storage (cdecl).
// Ported 1:1 2026-09-24 (claude-code:w5-server-06cf):
// *obj_id == 0 as a whole dword (no mask) -> -4
// the routine (Server_LookupLiveObject); null -> -2
// ID_String { CreateID(fuse_name) } in the reused param_1 slot
// wrapper vtable +0x1e4 light_fuse(0, id, SUBOBJ_ID_NONE,
//                         delay, -1.0f) on the looked-up object (EqObj's slot; the
//                         wrapper is not re-narrowed)
// NEG/SBB/AND 4/ADD -4: found 0, else -4
SERVER_API int __cdecl LightFuse(const unsigned int& obj_id_ref, const char* fuse_name, float delay) {
    if (obj_id_ref == 0) return -4;
    GameObject* const Obj = Server_LookupLiveObject(obj_id_ref);
    if (Obj == nullptr) return -2;
    ID_String FuseId;
    FuseId.Id = CreateID(fuse_name);                                     // / 06cfacb5
    bool const bLit = static_cast< ::EqObj*>(Obj)->light_fuse(0, FuseId, SUBOBJ_ID_NONE,
                                                              delay, -1.0f);
    return bLit ? 0 : -4;
}

// SpaceObj::GetTerminalInfo -- baseline module storage .. module storage (cdecl).
// Ported 1:1 2026-09-24 (claude-code:w5-server-06cf):
// (*obj_id & 0x3fffffff) == 0 -> -2 (ESI = -2 is the only
//                         failure value before the archetype)
// the routine (Server_LookupLiveObject); null -> -2
// CBase::Object null or (ObjectClass & 0x103) != 0x103 -> -2
// its archetype (+0x88, not null-tested) is an Archetype::EqObj;
//                         the docking-sphere vector (+0x88 begin / +0x8c end, 0x10-byte
//                         elements): null begin -> -3, index >= size (unsigned) -> -3.
//                         A null begin only occurs for an empty vector, so both tests
//                         are the port's single size test.
// safe_strcpy(out.Name, 0x20, sphere+4) -- the sphere's anim
//                         name (Common EqObj::read value(1), module storage); out.Value
//                         (+0x20) = sphere+0xc, its type; return 0
SERVER_API int __cdecl GetTerminalInfo(const unsigned int& obj_id_ref, int index, TerminalInfo& out_info) {
    if ((obj_id_ref & 0x3fffffffu) == 0) return -2;
    GameObject* const Obj = Server_LookupLiveObject(obj_id_ref);
    if (Obj == nullptr) return -2;
    CObject* const Cobj = Obj->Object;
    if (Cobj == nullptr ||
        (static_cast<unsigned int>(Cobj->ObjectClass) & 0x103u) != 0x103u) return -2;
    Archetype::EqObj const* const Arch =
        static_cast<Archetype::EqObj const*>(Cobj->Archetype);
    if (static_cast<unsigned int>(index) >= Arch->DockingSpheres.size()) return -3; // / 06cf9964
    Archetype::EqObj::DockingSphere const& Sphere = Arch->DockingSpheres[index];
    safe_strcpy(out_info.Name, 0x20, Sphere.HardpointName.c_str());     // (record +0x04)
    out_info.Value = Sphere.Type;
    return 0;
}

// SpaceObj::GetGoodID — gets good ID from a loot container's cargo archetype
// baseline (0x199a0): the routine lookup, type & 0x803 == 0x803 (loot),
//   reads *(obj+0xec) → inner ptr → *(ptr+8) archetype hash → Arch2Good
SERVER_API int __cdecl GetGoodID(const unsigned int& obj_id_ref, unsigned int& out_good_id) {
    if ((obj_id_ref & 0x3fffffff) == 0) return -2;
    GameObject* obj = Server_LookupLiveObject(obj_id_ref);
    if (!obj) return -2;
    // baseline: (ObjectClass & 0x803) == 0x803, then CObject+0xec -- which is
    // CLoot::ContentsArch, the pre-resolved Archetype::Equipment pointer -- and the
    // archetype hash at +8 on it (Root::Nickname.Id).
    ::Loot* LootRw = dynamic_cast< ::Loot*>(obj);
    CLoot* Cl = (LootRw != nullptr) ? LootRw->cloot() : nullptr;
    if (Cl == nullptr) return -1;
    if (Cl->ContentsArch == nullptr) return -1;
    out_good_id = Arch2Good(Cl->ContentsArch->get_id());
    return 0;
}

// SpaceObj::GetJumpTarget — gets jump target system/gate IDs from a planet/sun
// baseline (0x19a40): the routine lookup, type & 0x303 == 0x303 (planet/sun),
//   ObjectType check (& 0xc40), read +0x1b4/+0x1b8
SERVER_API int __cdecl GetJumpTarget(const unsigned int& obj_id_ref, unsigned int& out_target1, unsigned int& out_target2) {
    if ((obj_id_ref & 0x3fffffff) == 0) return -4;
    GameObject* obj = Server_LookupLiveObject(obj_id_ref);
    if (!obj) return -2;
    ::Solar* SolarRw = dynamic_cast< ::Solar*>(obj);
    CSolar* Cs = (SolarRw != nullptr) ? SolarRw->csolar() : nullptr;
    if (Cs == nullptr) return -1;
    // Check the object's type bits (CSimple::TypeBits, CObject+0xe0): only bits inside
    // 0xc40 may be set.
    unsigned int otype = Cs->get_type();
    if ((otype & 0xc40) != otype) return -1;
    out_target1 = Cs->DestSystem;    // CSolar+0x1b4
    out_target2 = Cs->DestGate.Id;   // CSolar+0x1b8 (ID_String)
    return 0;
}

// SpaceObj::SetInvincible2 -- baseline module storage .. module storage (ordinal 418, cdecl).
// Ported 1:1 2026-09-24 (claude-code:w5-server-06cf):
// obj_id == 0 (whole dword) -> -4
// the routine(obj_id, &obj, &sys) false, or sys == 0 -> -2
// FInvincibility { Type 3, bProtect = invincible, bFromPlayers =
//                         param3, MaxHpLoss = duration } -> obj vtable +0x194
//                         (GameObject::set_invincible); return 0
// Unlike SetInvincible there is no branch and param3 is not inverted. Pad bytes zeroed
// by the port (never written in baseline).
SERVER_API int __cdecl SetInvincible2(unsigned int obj_id, unsigned char invincible, unsigned char param3, float duration) {
    if (obj_id == 0) return -4;
    GameObject* Obj = nullptr;
    StarSystem* System = nullptr;
    if (!Server_FindLiveObject(obj_id, Obj, System)) return -2;
    if (System == nullptr) return -2;

    FInvincibility Cmd;
    Cmd.Type = 3;
    Cmd.bProtect = invincible;
    Cmd.bFromPlayers = param3;
    Cmd.Pad06[0] = 0;
    Cmd.Pad06[1] = 0;
    Cmd.MaxHpLoss = duration;
    Obj->set_invincible(Cmd);
    return 0;
}

// SpaceObj::RequestSpaceScript — requests a space script execution at a position
// baseline (0x19b40): copies Vector + int + string to stack, dispatches via the field vtable+0xf8
SERVER_API int __cdecl RequestSpaceScript(const unsigned int& obj_id_ref, const Vector& pos,
                                          const int& priority, unsigned int client_id, const char* script_name) {
    if ((obj_id_ref & 0x3fffffff) == 0) return -4;
    // baseline packs one 0x118-byte record on the stack and hands it to
    // IClient::SendServerRequestSpaceScript (+0xf8, FLPACKET_SERVER_REQUESTSPACESCRIPT) as
    // (client_id, &record) -- the fourth argument is the ClientId the packet goes to (it was
    // misnamed `script_type` until 2026-10-16; Content's Act_CallSceneScript passes the mission's
    // player id there). The layout is
    // FServerRequestSpaceScriptInfo (rpc_iclient_args.h): +0x000 id (06cf9b62), +0x108..
    // +0x110 the Vector (06cf9b6c / 06cf9b89 / 06cf9b7a), +0x114 = priority (06cf9b9c),
    // +0x004 safe_strcpy(.., 0x104, script_name) (06cf9ba3). No test of the client
    // interface or of the name (06cf9ba9 MOV ECX,[the field] / 06cf9bc1 CALL [EDX+0xf8]).
    // (Until 2026-09-26 the port used {id, Vector, int, name} here, which put the name where
    // rpclocal's sender 0x06d8aa40 reads the position.)
    FServerRequestSpaceScriptInfo Payload;
    Payload.Field_0x000 = obj_id_ref;
    memcpy(&Payload.Field_0x108, &pos.x, sizeof(float));
    memcpy(&Payload.Field_0x10c, &pos.y, sizeof(float));
    memcpy(&Payload.Field_0x110, &pos.z, sizeof(float));
    Payload.Field_0x114 = static_cast<unsigned int>(priority);
    safe_strcpy(Payload.Name, static_cast<int>(kRequestSpaceScriptNameSize), script_name);
    ::g_pClientInterface->SendServerRequestSpaceScript(client_id, Payload);
    return 0;
}

// SpaceObj::GetEmptyPos -- baseline module storage .. module storage (ordinal 0x19bd0, cdecl).
// Ported 1:1 2026-09-24 (claude-code:w5-server-06cf). Frame 0x109c bytes, 8-aligned; the
// 0x400-dword id buffer sits at [ESP1+0xa8].
// *start_radius < 0 or *clearance < 0 (FCOMP 0.0f, TEST AH,1 --
//                         NaN counts as below) -> -4
// R = *start_radius
// OUTER: Count = sys->scan_objects(buf, 0x400, &xform.Translation,
//                         R, 0xffffffff) when SysDB_LookupSystem(*system_id) is non-null;
//                         Count == 0 (or no system) -> a second lookup and, when it
//                         succeeds, Count = scan_objects(buf, 0x400, &xform.Translation,
//                         -1.0f, 2); otherwise Count = 0
// Attempts = 0
// INNER: *selection_type > 3 (unsigned) -> return result
//                         (-4 on the first pass)
//     case 0  06cf9cba    a = rand() * 2^-15 * 6.2831854820251465; offset (R cos a,
//                         R sin a, 0)
//     case 1  06cf9d03    (0, R cos a, R sin a)
//     case 2  06cf9d4c    (R cos a, 0, R sin a)
//     case 3  06cf9d92    u1 = rand()*2^-15, u2 = rand()*2^-15 (doubles), then
//                         (rand()*2^-15*R, R*u2, R*u1)
// the field (lazily the routine) ->TransformXform (+0x28)
//                         (&cand, &xform, &offset); *out_pos = cand
// bEmpty = 1, result = 0; Count == 0 -> the tail
// each id (unsigned index < Count): Bits = the live object's
//                         CSimple::TypeBits (+0xe0) when its CBase::Object is a CSimple
//                         (& 3), else 0; R_obj = 0, V = 0, then (only for a live id)
//                         Bits & 2 -> V = get_position(), get_atmosphere_range(R_obj);
//                         else get_physical_radius(R_obj, V) (+0x2c).
//                         (sqrt((dz*dz + dy*dy) + dx*dx) - R_obj) - *clearance
//                         compared with 0.0f: < or == -> collision
// collision: Attempts++, bEmpty = 0, result = -1;
//                         Attempts < 10 -> INNER
// R = R + R; !bEmpty -> OUTER
// return result
// x87 arithmetic is reproduced in double (the process's 53-bit precision control).
SERVER_API int __cdecl GetEmptyPos(const unsigned int& system_id_ref, const Transform& base_transform,
                                   const float& start_radius, const float& clearance,
                                   const PosSelectionType& selection_type, Vector& out_pos) {
    int Result = -4;
    if (!(start_radius >= 0.0f)) return Result;
    if (!(clearance >= 0.0f)) return Result;
    float Radius = start_radius;
    unsigned int Ids[0x400];                                             // [ESP1+0xa8]
    bool bEmpty = false;
    do {
        // OUTER (module storage).
        unsigned int Count = 0;
        StarSystem* System = SysDB_LookupSystem(system_id_ref);
        if (System != nullptr)
            Count = System->scan_objects(Ids, 0x400, &base_transform.Translation, Radius,
                                         0xffffffffu);
        if (Count == 0) {
            float const NoRadius = -1.0f;                             // (module storage)
            Count = 0;
            System = SysDB_LookupSystem(system_id_ref);
            if (System != nullptr)
                Count = System->scan_objects(Ids, 0x400, &base_transform.Translation,
                                             NoRadius, 2);
        }
        int Attempts = 0;
        for (;;) {
            // INNER (module storage).
            unsigned int const Selection = static_cast<unsigned int>(selection_type);
            if (Selection > 3u) return Result;
            Vector Offset;
            double const Step = 3.0517578125e-05;                        // (float 2^-15)
            double const TwoPi = 6.2831854820251465;
            if (Selection == 3u) {
                double const U1 = static_cast<double>(rand()) * Step;
                double const U2 = static_cast<double>(rand()) * Step;
                Offset.x = static_cast<float>(static_cast<double>(rand()) * Step *
                                              static_cast<double>(Radius));
                Offset.y = static_cast<float>(static_cast<double>(Radius) * U2);
                Offset.z = static_cast<float>(static_cast<double>(Radius) * U1);
            } else {
                double const Angle = static_cast<double>(rand()) * Step * TwoPi; // / 06cf9cda
                float const C = static_cast<float>(std::cos(Angle) * static_cast<double>(Radius)); // FCOS
                float const S = static_cast<float>(std::sin(Angle) * static_cast<double>(Radius)); // FSIN
                if (Selection == 0u) {
                    Offset.x = C;
                    Offset.y = S;
                    Offset.z = 0.0f;
                } else if (Selection == 1u) {
                    Offset.x = 0.0f;
                    Offset.y = C;
                    Offset.z = S;
                } else {
                    Offset.x = C;
                    Offset.y = 0.0f;
                    Offset.z = S;
                }
            }

            if (GSpaceObjMathEngine == nullptr)
                GSpaceObjMathEngine = Server_Acquire3DMathEngine();
            Vector Candidate;
            GSpaceObjMathEngine->TransformXform(reinterpret_cast<FVector&>(Candidate),
                                                reinterpret_cast<FTransform const&>(base_transform),
                                                reinterpret_cast<FVector const&>(Offset));
            out_pos = Candidate;

            bEmpty = true;
            Result = 0;
            bool bCollided = false;
            for (unsigned int Index = 0; Index < Count; ++Index) {       // / 06cf9f9a
                unsigned int const Id = Ids[Index];
                unsigned int Bits = 0;
                if ((Id & 0x3fffffffu) != 0) {
                    GameObject* const Probe = Server_LookupLiveObject(Id);
                    if (Probe != nullptr) {
                        CObject* const Raw = Probe->Object;
                        if (Raw != nullptr &&
                            (static_cast<unsigned int>(Raw->ObjectClass) & 3u) == 3u)
                            Bits = static_cast<CSimple*>(Raw)->TypeBits;
                    }
                }
                float OtherRadius = 0.0f;
                Vector Other;
                Other.x = 0.0f;
                Other.y = 0.0f;
                Other.z = 0.0f;
                if ((Id & 0x3fffffffu) != 0) {                           // / 06cf9fe1
                    GameObject* const Obj = Server_LookupLiveObject(Id); // / 06cf9fe8
                    if (Obj != nullptr) {
                        if ((Bits & 2u) != 0) {
                            Other = Obj->get_position();
                            Obj->get_atmosphere_range(OtherRadius);
                        } else {
                            Obj->get_physical_radius(OtherRadius, Other);
                        }
                    }
                }
                double const Dx = static_cast<double>(out_pos.x) - Other.x;
                double const Dy = static_cast<double>(out_pos.y) - Other.y;
                double const Dz = static_cast<double>(out_pos.z) - Other.z;
                double const Dist = std::sqrt((Dz * Dz + Dy * Dy) + Dx * Dx);
                double const Gap = (Dist - static_cast<double>(OtherRadius))
                                   - static_cast<double>(clearance);      // / 06cf9f82
                if (Gap <= 0.0) {
                    bCollided = true;
                    break;
                }
            }
            if (!bCollided)
                break;                                                   // -> 06cfa02d
            ++Attempts;
            bEmpty = false;
            Result = -1;
            if (Attempts >= 10)
                break;
        }
        Radius = static_cast<float>(static_cast<double>(Radius) + static_cast<double>(Radius));
    } while (!bEmpty);
    return Result;
}

// SpaceObj::IsPosEmpty -- baseline module storage .. module storage (ordinal 0x1a060, cdecl).
// Ported 1:1 2026-09-24 (claude-code:w5-server-06cf). Frame: 0x1024 bytes (__chkstk
// ), a 0x400-dword id buffer at [ESP0+0x24].
// *out_empty = true
// R = *radius; result = -1; the routine(*system_id)
//                         (SysDB_LookupSystem) null -> *out_empty = true, return -1
// Count1 = sys->scan_objects(buf, 0x400, &pos, R, 0xffffffff)
//                         (the routine); non-zero -> not empty
// R = -1.0f (module storage); the SAME system is looked up
//                         again; null -> the exit with Count2 = 0 (empty)
// Count2 = sys->scan_objects(buf, 0x400, &pos, -1.0f, 2)
// for each id (unsigned index < Count2): V = (0,0,0), R = 0 each
//                         pass; a live object (id & 0x3fffffff, the routine) gives
//                         V = get_position() (slot 0) and R via get_atmosphere_range
//                         (+0x140, return ignored); sqrt((dz*dz + dy*dy) + dx*dx) - R
//                         compared with 0.0f (module storage): < or == -> hit, stop
// Count1 != 0 or a hit -> *out_empty = false; otherwise
//                         *out_empty = true; return 0
// x87 arithmetic: baseline runs at the process's 53-bit precision control, which the
// double arithmetic below reproduces step for step (the same treatment as AcceptTrade /
// send_comm).
SERVER_API int __cdecl IsPosEmpty(const unsigned int& system_id_ref, const Vector& pos,
                                  const float& radius, bool& out_empty) {
    out_empty = true;
    float SearchRadius = radius;
    int Result = -1;
    bool bHit = false;
    unsigned int Count2 = 0;
    StarSystem* System = SysDB_LookupSystem(system_id_ref);
    if (System == nullptr) {
        out_empty = true;
        return Result;
    }
    unsigned int Ids[0x400];                                             // [ESP0+0x24]
    unsigned int const Count1 =
        System->scan_objects(Ids, 0x400, &pos, SearchRadius, 0xffffffffu);
    if (Count1 == 0) {
        SearchRadius = -1.0f;
        System = SysDB_LookupSystem(system_id_ref);
        if (System != nullptr) {
            Count2 = System->scan_objects(Ids, 0x400, &pos, SearchRadius, 2);
            for (unsigned int Index = 0; Index < Count2; ++Index) {      // / 06cfa1ce
                Vector Other;
                Other.x = 0.0f;                                          // (from 06cfa116)
                Other.y = 0.0f;
                Other.z = 0.0f;
                float OtherRadius = 0.0f;
                unsigned int const Id = Ids[Index];
                if ((Id & 0x3fffffffu) != 0) {
                    GameObject* const Obj = Server_LookupLiveObject(Id);
                    if (Obj != nullptr) {
                        Other = Obj->get_position();
                        Obj->get_atmosphere_range(OtherRadius);
                    }
                }
                double const Dx = static_cast<double>(pos.x) - Other.x;
                double const Dy = static_cast<double>(pos.y) - Other.y;
                double const Dz = static_cast<double>(pos.z) - Other.z;
                double const Dist = std::sqrt((Dz * Dz + Dy * Dy) + Dx * Dx);
                double const Gap = Dist - static_cast<double>(OtherRadius);
                if (Gap <= 0.0) {
                    bHit = true;
                    break;
                }
            }
        }
    }
    Result = 0;
    if (Count1 != 0 || (Count2 != 0 && bHit)) {
        out_empty = false;
        return Result;
    }
    out_empty = true;
    return Result;
}

} // namespace SpaceObj

// ===========================================================================
// pub::Player exports — all line-by-line verified against Server
// baseline pattern: validate player_id != 0 && (id-1) < max && active → field access
// ===========================================================================
namespace Player {

// Player::MarkObj — marks an object for a player (map marker / HUD highlight)
// baseline (0x74970): validate player, the routine lookup on obj_id, the routine mark, vtable+300 notify
SERVER_API int __cdecl MarkObj(unsigned int player_id, unsigned int obj_id, int mark_type) {
    if (!IsValidPlayer(player_id)) return -2;
    GameObject* obj = Server_LookupGameObject(obj_id);
    if (!obj) return -3;
    // baseline (module storage) calls the routine(obj, player_id, mark_type) to record the
    // mark on the object, then (module storage) dispatches
    //   CALL dword ptr [EDX + 0x12c]      with (&player_id, obj_id, mark_type)
    // on the field — that is IClient::SendServerMarkObj (+0x12c), ch2 op 0x46,
    // FLPACKET_SERVER_MARKOBJ. The previous port invented message id 0x48 instead.

    // is GameObject::mark_for_player now (wave 13): baseline's list
    // of marking player ids lives at GameObject+0x14..+0x1F and the CREATELOOT /
    // CREATESHIP / CREATESOLAR payloads all carry the answer as a byte.
    obj->mark_for_player(player_id, mark_type);
    // LEA EAX,[ESP+0x18] -- the ADDRESS of param_1, and rpclocal's
    // sender (0x06d8ace0, SendPacketClientRef) dereferences it. No null test on
    // (module storage).
    ::g_pClientInterface->SendServerMarkObj(player_id, obj_id, mark_type);
    return 0;
}

// Player::CfgInterfaceNotification — configures interface notifications
// baseline (0x74a10): validate player, server interface vtable+0x130 dispatch
SERVER_API int __cdecl CfgInterfaceNotification(unsigned int player_id, unsigned int notif_type,
                                      bool enable, int param) {
    if (!IsValidPlayer(player_id)) return -2;
    // baseline (module storage) dispatches CALL dword ptr [EAX + 0x130] on the field
    // with (&player_id, &payload), where payload is the 12-byte stack block
    // { uint notif_type; int param; bool enable; } built at module storage..06d54a66.
    // That is IClient::SendServerCfgInterfaceNotification (+0x130), ch2 op 0x47,
    // FLPACKET_SERVER_CFGINTERFACENOTIFICATION. The previous port dispatched nothing.
    // The block is rpc_iclient_args.h's FPayloadServerCfgInterfaceNotification (+0x00
    // notif_type, +0x04 param, byte +0x08 enable); baseline leaves the three pad bytes
    // unwritten, and so does this.
    FPayloadServerCfgInterfaceNotification Payload;
    Payload.Field_0x00 = notif_type;
    Payload.Field_0x04 = static_cast<unsigned int>(param);
    Payload.Field_0x08 = enable ? 1u : 0u;
    // LEA EDX,[ESP+0x18] -- &param_1 (rpclocal's sender 0x06d8ad30
    // dereferences it); the field is not null-tested.
    ::g_pClientInterface->SendServerCfgInterfaceNotification(player_id, Payload);
    return 0;
}

// Player::GetSystem — gets player's current system ID
// baseline (0x74a80): _Players[id * 0x418 - 0x38]
SERVER_API int __cdecl GetSystem(const unsigned int& player_id, unsigned int& out_system_id) {
    if (!IsValidPlayer(player_id)) return -2;
    PlayerSlot* p = PlayerDB_GetPlayer(player_id);
    if (!p) return -2;
    out_system_id = p->system_id;
    return 0;
}

// Player::GetShip — gets player's current ship FLID
// baseline (0x74ad0): _Players[id * 0x418 - 0x34]
SERVER_API int __cdecl GetShip(const unsigned int& player_id, unsigned int& out_ship_id) {
    if (!IsValidPlayer(player_id)) return -2;
    PlayerSlot* p = PlayerDB_GetPlayer(player_id);
    if (!p) return -2;
    out_ship_id = p->ship_object_id;
    return 0;
}

// Player::GetShipID — gets player's ship archetype ID
// baseline (0x74b20): _Players[id * 0x418 - 0x1b4]
SERVER_API int __cdecl GetShipID(const unsigned int& player_id, unsigned int& out_ship_arch) {
    if (!IsValidPlayer(player_id)) return -2;
    PlayerSlot* p = PlayerDB_GetPlayer(player_id);
    if (!p) return -2;
    out_ship_arch = p->ship_archetype;
    return 0;
}

// Player::GetRep — gets player reputation vibe handle
// baseline (0x74b70): _Players[id * 0x418 - 0x100], default -1
SERVER_API int __cdecl GetRep(const unsigned int& player_id, int& out_rep) {
    out_rep = -1;
    if (!IsValidPlayer(player_id)) return -2;
    PlayerSlot* p = PlayerDB_GetPlayer(player_id);
    if (!p) return -2;
    out_rep = p->rep_vibe;
    return 0;
}

// Player::GetBase — gets player's current base ID
// baseline (0x74bd0): _Players[id * 0x418 - 0x2c]
SERVER_API int __cdecl GetBase(const unsigned int& player_id, unsigned int& out_base_id) {
    if (!IsValidPlayer(player_id)) return -2;
    PlayerSlot* p = PlayerDB_GetPlayer(player_id);
    if (!p) return -2;
    out_base_id = p->current_base;
    return 0;
}

// Player::GetLocation — gets player's current location ID
// baseline (0x74c20): _Players[id * 0x418 - 0x20]
SERVER_API int __cdecl GetLocation(const unsigned int& player_id, unsigned int& out_location_id) {
    if (!IsValidPlayer(player_id)) return -2;
    PlayerSlot* p = PlayerDB_GetPlayer(player_id);
    if (!p) return -2;
    out_location_id = p->current_location;
    return 0;
}

// Player::GetCharacter -- baseline module storage: the standard id guard (-2),
// then *out = [Players + id*0x418 - 0x1c] = slot +0x3FC, PlayerData::Field_0x3FC
// (the port used to read a separate `character_flid` copy that nothing wrote).
SERVER_API int __cdecl GetCharacter(const unsigned int& player_id, unsigned int& out_char_id) {
    if (!IsValidPlayer(player_id)) return -2;
    out_char_id = Players[player_id].Field_0x3FC;
    return 0;
}

// Player::GetRelativeHealth — gets player's normalized hull health
// baseline (0x74cc0): _Players[id * 0x418 - 0x1b0], default -1.0
SERVER_API int __cdecl GetRelativeHealth(const unsigned int& player_id, float& out_health) {
    out_health = -1.0f;
    if (!IsValidPlayer(player_id)) return -2;
    PlayerSlot* p = PlayerDB_GetPlayer(player_id);
    if (!p) return -2;
    out_health = p->relative_health;
    return 0;
}

// Player::EnumerateCargo — iterates the player's cargo-bay contents.
// baseline 0x74d20 (module storage), ported 1:1.
// LEA ECX,[ECX + ESI*0x1 + 0xfffffe60]   ; &slot->equipment (+0x278)
// MOV EBX,[ECX + 0x4] / MOV ESI,[EBX]    ; walk the list
// MOV AL,[ESI + 0x14]  ; EquipDesc::bEquipped -- SKIP when set
// MOV EAX,[ESI + 0xc]  ; EquipDesc::ArchId  -> Arch2Good
// MOV ECX,[ESI + 0x1c] ; EquipDesc::Count
// MOV AX,[ESI + 0xa]   ; EquipDesc::Id
// CALL [enumerator vtable + 0]
// The old port body did nothing at all. Note the equipped filter: this is the
// CARGO enumerator, so mounted equipment is deliberately invisible to it.
// baseline does NOT test Arch2Good's answer here (unlike SpaceObj's version).
SERVER_API int __cdecl EnumerateCargo(const unsigned int& player_id, CargoEnumerator& enumerator) {
    if (!IsValidPlayer(player_id)) return -2;
    PlayerSlot* p = PlayerDB_GetPlayer(player_id);
    if (!p) return -2;

    for (std::list<EquipDesc>::const_iterator It = p->equipment.Items.begin();
         It != p->equipment.Items.end(); ++It) {
        EquipDesc const& Item = *It;
        if (Item.bEquipped)
            continue;
        unsigned int const GoodId = Arch2Good(Item.ArchId);
        enumerator.notify_cargo(GoodId, Item.Count, Item.Id);
    }
    return 0;
}

// Player::GetRemainingHoldSize — gets remaining cargo hold capacity
// baseline (0x74db0): get ship archetype from -0x1b4, GetCargoHoldSize, subtract occupied from -0x1a0
SERVER_API int __cdecl GetRemainingHoldSize(const unsigned int& player_id, float& out_remaining) {
    if (!IsValidPlayer(player_id)) return -2;
    PlayerSlot* p = PlayerDB_GetPlayer(player_id);
    if (!p) return -2;

    // MOV EAX,[EAX + EDX*0x1 + 0xfffffe4c]   ; -0x1b4 == +0x264
    // CALL pub::GetCargoHoldSize(&shipArch, &holdSize)
    // LEA  ECX,[EAX + EDX*0x1 + 0xfffffe60]   ; -0x1a0 == +0x278
    // CALL [->COMMON.DLL::EquipDescList::get_cargo_space_occupied]
    // FILD qword / FSUB ST0,ST1 / FSTP [out]
    // baseline goes through pub::GetCargoHoldSize -- whose out-parameter is an
    // UNSIGNED dword, zero-extended by the FILD qword -- not straight at
    // Archetype::Ship::HoldSize, and subtracts the EquipDescList own occupied
    // volume rather than a bare item count.
    unsigned int HoldSize = 0;
    pub::GetCargoHoldSize(p->ship_archetype, HoldSize);
    out_remaining = (float)HoldSize - p->equipment.get_cargo_space_occupied();
    return 0;
}

// Player::AddCargo -- adds cargo to the player ship, or to the cargo bay when
// the player has no ship object (i.e. is docked).
// baseline 0x74e90 (module storage).
// FLD health / FCOMP module storage (0x00000000 = 0.0f)
//                   TEST AH,0x1 / JNZ -> return -4          ; health < 0
// FLD health / FCOMP module storage (0x3f800000 = 1.0f)
//                   TEST AH,0x41 / JP -> return -4          ; health > 1
// MOV EDX,[EAX + -0x34]                   ; +0x3e4 ship object id
// the routine(&shipObjId, &param_1, &obj) ; false -> return -2
// the routine(obj, &playerId, Good2Arch(good), qty, health, flag)
// LEA ESI,[EAX + 0xfffffbe8]              ; back to the slot base
// the routine(slot, arch, CARGO_BAY_HP_NAME, qty, health,
//                                /*bEquipped*/0, flag, /*bNotify*/1)

// CORRECTION on the last argument, itself CORRECTED 2026-09-17 (wave 11).
// On THIS path -- the docked one -- it really is the routine's +0x18 byte, the
// "do not combine with an existing stack" flag, and nothing else. The earlier
// note went one step too far and concluded the bool never reaches
// EquipDesc::bMission anywhere "because Server.dll imports no EquipDesc setter
// for that field": true about the import table, false about the field. The
// IN-SPACE path writes it with a direct byte store instead --
// 8a4c247c  MOV  CL,byte ptr [ESP + 0x7c]   ; the bool
// 884c2440  MOV  byte ptr [ESP + 0x40],CL
// and [ESP+0x40] is EquipDesc+0x18 (the descriptor lives at [ESP+0x28] in that
// frame), i.e. EquipDesc::bMission. So in space the flag both suppresses the
// merge and marks the item as mission cargo; docked, only the former.
SERVER_API int __cdecl AddCargo(const unsigned int& player_id, const unsigned int& good_id,
                     unsigned int quantity, float health, bool mission_flag) {
    if (!IsValidPlayer(player_id)) return -2;
    // FCOMP 0.0f / TEST AH,0x1 / JNZ: C0 is set for "below" AND for
    // unordered, so a NaN health is rejected too -- hence the negated compares.
    if (!(health >= 0.0f)) return -4;
    // FCOMP 1.0f / TEST AH,0x41 / JP: jumps for "above" and unordered.
    if (!(health <= 1.0f)) return -4;
    PlayerSlot* p = &Players[player_id];

    if (p->ship_object_id != 0) {
        // .. module storage -- the routine(&ship_object_id,
        // &objOut, &ownerOut). It writes the resolved object back over the
        // stack slot that held param_1 (`LEA EAX,[ESP+0x10]`) and the owning
        // CONTAINER into the separate local at [ESP+0x4]; that container is
        // then the `this` of the the routine call at module storage
        // (module storage `MOV ECX,[ESP+0x14]` reloads it). A false return is
        // -2 (module storage).

        // The system is the routine's OutSys (Server_FindObject, 2026-09-24
        // claude-code:w5-server-06d4; the GameObject::OwnerSystem stand-in is
        // gone). The lookup failing is the only -2 here (module storage JZ ->
        // ); the system it hands back is used untested, null for an
        // in-transit ship exactly as in baseline.
        GameObject* ShipObj = nullptr;
        StarSystem* ShipSys = nullptr;
        if (!Server_FindObject(p->ship_object_id, ShipObj, ShipSys))
            return -2;
        // Good2Arch only after the lookup succeeded.
        ShipSys->add_cargo(ShipObj, Good2Arch(good_id), quantity, health,
                           mission_flag);
        return 0;
    }

    // .. module storage -- the docked path, ported 1:1.
    unsigned int const ArchId = Good2Arch(good_id);
    PlayerData_AddEquipment(*p, ArchId, EquipDesc::CARGO_BAY_HP_NAME,
                            (int)quantity, health, /*bEquipped*/false,
                            /*bNoCombine*/mission_flag, /*bNotify*/true);
    return 0;
}

// Player::RemoveCargo -- removes cargo from the player.
// baseline 0x74fb0 (module storage).
// MOV ECX,[ESI + -0x34]                   ; +0x3e4 ship object id
// the routine(...)                       ; false -> return -2
// the routine(obj, &playerId, id, count)
// the no-ship path: the MP gate on [ESI-0x3b4] (= +0x64),
//                   EquipDescList::remove_equipment_item at [ESI-0x1a0]
//                   (= +0x278), IClient slot 27 (+0x6c) with
//                   (client_id, result, id, count), then
// (0, &client_id).
// That tail is byte-for-byte PlayerData::remove_equipment (the routine)
// inlined, so the port calls the shared helper.
SERVER_API int __cdecl RemoveCargo(const unsigned int& player_id, unsigned short cargo_slot, unsigned int quantity) {
    if (!IsValidPlayer(player_id)) return -2;
    PlayerSlot* p = &Players[player_id];

    if (p->ship_object_id != 0) {
        // the same the routine resolve, then
        // the routine(owner, obj, cargo_slot, quantity).
        // The third argument is real: the routine ends in `RET 0xc` and reads
        // it at module storage (`MOV EBX,[ESP+0x40]`), which is entry+0x0C.
        // `this` is the routine's OutSys (module storage MOV ECX,[local_4]), as in
        // AddCargo; the lookup failing is the only -2 (module storage JZ ->
        // ), the system is used untested.
        GameObject* ShipObj = nullptr;
        StarSystem* ShipSys = nullptr;
        if (!Server_FindObject(p->ship_object_id, ShipObj, ShipSys))
            return -2;
        ShipSys->remove_cargo(ShipObj, cargo_slot, (int)quantity);
        return 0;
    }

    PlayerData_RemoveEquipment(*p, cargo_slot, (int)quantity, /*bNotify*/true);
    return 0;
}

// Player::SetShipAndLoadout -- sets the player hull and replaces the whole
// equipment list.
// baseline 0x750a0 (module storage).
// the routine(slot, shipArch)              ; PlayerData::set_ship_arch
// a local std::list<EquipDesc> is built, one node per element
//                   of the incoming EquipDescVector (stride 0x20, walked from
//                   [param_3+0x4] to [param_3+0x8]), and EVERY copy has its id
//                   overwritten with SUBOBJ_ID_NONE
//                   (module storage, set_id(*SUBOBJ_ID_NONE))
// the routine(slot, &thatList)             ; PlayerData::load_equipment
// the local list is erased and freed
// The id wipe is what makes load_equipment mint a fresh EquipIdMaker id for
// every item, which is the whole point of the round trip. The previous port
// assigned ship_archetype directly and dropped the loadout on the floor.
SERVER_API int __cdecl SetShipAndLoadout(const unsigned int& player_id, unsigned int ship_arch,
                                          const EquipDescVector& equip_desc_vector) {
    if (!IsValidPlayer(player_id)) return -2;
    PlayerSlot* p = PlayerDB_GetPlayer(player_id);
    if (!p) return -2;

    PlayerData_SetShipArch(*p, ship_arch);

    EquipDescList Fresh;
    for (EquipDesc const* It = equip_desc_vector.Begin; It != equip_desc_vector.End; ++It) {
        EquipDesc Copy(*It);
        Copy.set_id(SUBOBJ_ID_NONE);
        Fresh.add_equipment_item(Copy, false);
    }
    PlayerData_LoadEquipment(*p, Fresh);
    return 0;
}

// Player::GetRank — gets player rank level
// baseline (0x75220): _Players[id * 0x418 - 0x194]
SERVER_API int __cdecl GetRank(const unsigned int& player_id, int& out_rank) {
    out_rank = 0;
    if (!IsValidPlayer(player_id)) return -2;
    PlayerSlot* p = PlayerDB_GetPlayer(player_id);
    if (!p) return -2;
    out_rank = (int)p->rank_level;
    return 0;
}

// Player::GetMoneyNeededToNextRank — gets cash needed for next rank promotion
// baseline (0x75280): _Players[id * 0x418 - 0x190]
SERVER_API int __cdecl GetMoneyNeededToNextRank(const unsigned int& player_id, int& out_money) {
    if (!IsValidPlayer(player_id)) return -2;
    PlayerSlot* p = PlayerDB_GetPlayer(player_id);
    if (!p) return -2;
    out_money = p->money_needed_to_next_rank;
    return 0;
}

// Player::GetAssetValue — calculates total player asset value
// baseline (0x752d0): GoodList::find_by_ship_arch for ship value * 0.8,
//   iterate EquipDescList at -0x1a0, commodity qty * price * 0.3 or equip full price
SERVER_API int __cdecl GetAssetValue(const unsigned int& player_id, float& out_value) {
    out_value = 0.0f;
    if (!IsValidPlayer(player_id)) return -2;
    PlayerSlot* p = PlayerDB_GetPlayer(player_id);
    if (!p) return -2;

    // MOV ESI,[EAX + ECX*0x1 + 0xfffffe4c]  ; -0x1b4 == +0x264
    unsigned int const ship_arch = p->ship_archetype;
    if ((ship_arch & 0x3fffffffu) == 0) return -2;

    // GoodList_get()
    // GoodInfoList::find_by_ship_arch(ship_arch)  <- NOT
    //                 find_by_archetype, which is what the port used to call.
    GoodInfoList* good_list = GoodList_get();
    if (!good_list) return -2;

    const GoodInfo* ship_good = good_list->find_by_ship_arch(ship_arch);
    if (!ship_good) return -2;

    // FLD module storage / FMUL [EAX + 0x58] / FSTP [EDI]
    // is cd cc 4c 3f == 0x3f4ccccd == 0.8f.
    out_value = 0.8f * ship_good->Price;

    // LEA EBX,[EDX + EAX*0x1 + 0xfffffe60]   ; -0x1a0 == +0x278
    // .. module storage -- one pass over the EquipDescList.
    //   node+0x0c == EquipDesc::ArchId, node+0x18 == Health, node+0x1c == Count.
    // GoodList::find_by_archetype(ArchId)
    // MOV CL,[good + 0x70] / TEST CL,CL
    //     set   -> FILD Count / FMUL [good + 0x58]                (full price)
    //     clear -> FLD Health / FMUL [good + 0x58] / ftol
    //              IMUL by Count / FILD / FMUL module storage
    //              (9a 99 99 3e == 0x3e99999a == 0.3f)
    // GoodInfo+0x70 is the COMBINABLE flag, not a category: the routine splits
    // on exactly the same two branches using COMMON.DLL::arch_is_combinable
    // (module storage) and applies the identical 0.3f to the non-combinable
    // side. The previous port tested Category == GoodCategoryCommodity, had the
    // two branches the wrong way round, and multiplied by a hard 1.0f instead of
    // the item Count.
    for (std::list<EquipDesc>::const_iterator It = p->equipment.Items.begin();
         It != p->equipment.Items.end(); ++It) {
        EquipDesc const& Item = *It;
        if ((Item.ArchId & 0x3fffffffu) == 0) continue;
        const GoodInfo* item_good = GoodList::find_by_archetype(Item.ArchId);
        if (!item_good) continue;

        if (item_good->bCombinable) {
            out_value += (float)Item.Count * item_good->Price;
        } else {
            int const Unit = (int)(Item.Health * item_good->Price);
            out_value += (float)(Unit * Item.Count) * 0.3f;
        }
    }
    return 0;
}

// Player::AdjustCash (0x06d553c0) -- PlayerData::AdjustCash (0x06d4f4e0)
// inlined with its bool argument folded to true, so +0x320 always accumulates.

// The previous port kept only the clamp. Four things were missing, and three of
// them could not even be expressed until PlayerSlot's two layouts were unified:
//   * the running total at +0x320 (`ADD [EDI+0x320],EAX`, 0x06d5540e);
//   * the slot-occupied gate `MOV EDI,[EDX+ESI+0x348] / TEST EDI,EDI / JZ`
//     (0x06d553f2) -- baseline returns -2 when the client id is zero, which is a
//     stricter test than the index-range check alone;
//   * the `IsMPServer() && player+0x64 == 0` gate (0x06d5541c), which skips the
//     clamp, the store AND the notification, yet still returns 0;
//   * the two notifications: IClient vtable+0x134 == slot 77, taking the client
//     id BY ADDRESS (`ADD EDI,0x348 / PUSH EDI`, 0x06d55456), then
//     Server_SendMessage(0, &client_id) (0x06d55466).
SERVER_API int __cdecl AdjustCash(const unsigned int& player_id, int amount) {
    if (!IsValidPlayer(player_id)) return -2;
    PlayerSlot* p = PlayerDB_GetPlayer(player_id);
    if (!p) return -2;
    // the client id doubles as baseline's "slot occupied" flag.
    if (p->client_id == 0) return -2;

    p->total_earned += amount;
    int new_cash = p->cash + amount;

    // jumps into the body when IsMPServer() is false; 0x06d55430
    // jumps PAST it when [player+0x64] is zero. +0x64 is the session's SELECTED
    // CHARACTER_ID (see common_server_playerdb.h -- proved on the raw bytes at
    // ), so the body runs on
    // `!IsMPServer() || selected_character[0] != 0`: an MP server only adjusts
    // the cash of a session that has actually picked a character. Still returns
    // 0, not an error.
    if (IsMPServer() && !p->selected_character.is_valid()) return 0;

    if (new_cash < 0) new_cash = 0;
    else if (new_cash > 999999999) new_cash = 999999999;
    p->cash = new_cash;

    // the field is loaded and called through with no
    // null test (the port's `if (g_pClientInterface)` was an addition, removed
    // 2026-09-24 by claude-code:w3-baseenter).
    ::g_pClientInterface->SendServerSetCash(p->client_id, new_cash);
    // the routine(0, &client_id) == _SendMessage(&0u, 0, &client_id),
    // a broadcast of message 0 to every controller.
    Server_ControllerDispatch(0, pub::Controller::EMessage::PlayerWorthChanged, &p->client_id);
    return 0;
}

// Player::InspectCash (0x06d55480) -- reads +0x31c and clamps it on the way out
// without writing it back. Same slot-occupied gate as AdjustCash: 0x06d554b5
// loads [+0x348] and 0x06d554bf returns -2 on zero. out_cash is zeroed first
// (0x06d5548a), so the -2 paths still leave a defined value behind.
SERVER_API int __cdecl InspectCash(const unsigned int& player_id, int& out_cash) {
    out_cash = 0;
    if (!IsValidPlayer(player_id)) return -2;
    PlayerSlot* p = PlayerDB_GetPlayer(player_id);
    if (!p) return -2;
    if (p->client_id == 0) return -2;
    int cash = p->cash;
    if (cash < 0) cash = 0;
    if (cash > 999999999) cash = 999999999;
    out_cash = cash;
    return 0;
}

// Player::SetInitialPos — sets initial spawn position for player
// baseline (0x75500): copy Vector to _Players[id * 0x418 - 0xc8] (3 dwords)
SERVER_API int __cdecl SetInitialPos(const unsigned int& player_id, const Vector& pos) {
    if (!IsValidPlayer(player_id)) return -2;
    PlayerSlot* p = PlayerDB_GetPlayer(player_id);
    if (!p) return -2;
    p->initial_pos[0] = pos.x;
    p->initial_pos[1] = pos.y;
    p->initial_pos[2] = pos.z;
    return 0;
}

// Player::SetInitialOrnt — sets initial spawn orientation for player
// baseline (0x75560): copy Matrix to _Players[id * 0x418 - 0xbc] (9 dwords loop)
SERVER_API int __cdecl SetInitialOrnt(const unsigned int& player_id, const Matrix& orient) {
    if (!IsValidPlayer(player_id)) return -2;
    PlayerSlot* p = PlayerDB_GetPlayer(player_id);
    if (!p) return -2;
    for (int i = 0; i < 9; i++) {
        ((float*)p->initial_ornt)[i] = ((const float*)&orient)[i];
    }
    return 0;
}

// Player::GetNumKills — gets player kill count
// baseline (0x755c0): _Players[id * 0x418 - 0xe4], default -1 on error
SERVER_API int __cdecl GetNumKills(const unsigned int& player_id, int& out_kills) {
    if (!IsValidPlayer(player_id)) { out_kills = -1; return -2; }
    PlayerSlot* p = PlayerDB_GetPlayer(player_id);
    if (!p) { out_kills = -1; return -2; }
    out_kills = p->num_kills;
    return 0;
}

// Player::SetNumKills — baseline 0x06d55620, 30 instructions.
// MOV  [EAX + 0x334],EDX        ; the store
// PUSH EDX                      ; kills
// ADD  EAX,0x348 / PUSH EAX     ; &slot->client_id, BY ADDRESS
// CALL [IClient + 0x138]        ; FLPACKET_SERVER_SETNUMKILLS
// The notification was missing; without it the client never learns the new
// count. IClient +0x138 is SendServerSetNumKills and rpc_iclient.h's own packet-registry
// derivation names it SETNUMKILLS, which is the independent confirmation.
// The by-address first argument is the convention rpclocal already established
// for this packet family (RPC_CH2_DELIVER_DWORD_BYREF in rpc_payload_ch2.cpp).
SERVER_API int __cdecl SetNumKills(const unsigned int& player_id, int kills) {
    if (!IsValidPlayer(player_id)) return -2;
    PlayerSlot* p = PlayerDB_GetPlayer(player_id);
    if (!p) return -2;
    p->num_kills = kills;
    if (::g_pClientInterface) {
        ::g_pClientInterface->SendServerSetNumKills(p->client_id, kills);
    }
    return 0;
}

// Player::GetNumMissionSuccesses — gets mission success count
// baseline (0x75690): _Players[id * 0x418 - 0xe0], default -1 on error
SERVER_API int __cdecl GetNumMissionSuccesses(const unsigned int& player_id, int& out_count) {
    if (!IsValidPlayer(player_id)) { out_count = -1; return -2; }
    PlayerSlot* p = PlayerDB_GetPlayer(player_id);
    if (!p) { out_count = -1; return -2; }
    out_count = p->num_mission_successes;
    return 0;
}

// Player::SetNumMissionSuccesses — baseline 0x06d556f0, the same shape as
// SetNumKills above: `MOV [EAX+0x338],EDX` at module storage, then
// `CALL [IClient + 0x13c]` at module storage == SendServerSetMissionSuccesses,
// FLPACKET_SERVER_SETMISSIONSUCCESSES. The notification was missing.
SERVER_API int __cdecl SetNumMissionSuccesses(const unsigned int& player_id, int count) {
    if (!IsValidPlayer(player_id)) return -2;
    PlayerSlot* p = PlayerDB_GetPlayer(player_id);
    if (!p) return -2;
    p->num_mission_successes = count;
    if (::g_pClientInterface) {
        ::g_pClientInterface->SendServerSetMissionSuccesses(p->client_id, count);
    }
    return 0;
}

// Player::GetNumMissionFailures — gets mission failure count
// baseline (0x75760): _Players[id * 0x418 - 0xdc], default -1 on error
SERVER_API int __cdecl GetNumMissionFailures(const unsigned int& player_id, int& out_count) {
    if (!IsValidPlayer(player_id)) { out_count = -1; return -2; }
    PlayerSlot* p = PlayerDB_GetPlayer(player_id);
    if (!p) { out_count = -1; return -2; }
    out_count = p->num_mission_failures;
    return 0;
}

// Player::SetNumMissionFailures — baseline 0x06d557c0, same shape again:
// `MOV [EAX+0x33c],EDX` at module storage, then `CALL [IClient + 0x140]` at
// == SendServerSetMissionFailures, FLPACKET_SERVER_SETMISSIONFAILURES.
SERVER_API int __cdecl SetNumMissionFailures(const unsigned int& player_id, int count) {
    if (!IsValidPlayer(player_id)) return -2;
    PlayerSlot* p = PlayerDB_GetPlayer(player_id);
    if (!p) return -2;
    p->num_mission_failures = count;
    if (::g_pClientInterface) {
        ::g_pClientInterface->SendServerSetMissionFailures(p->client_id, count);
    }
    return 0;
}

// Player::DisplayMissionMessage -- baseline 0x75830 (module storage..06d5595d), ordinal
// 109. Traced 2026-09-24 (claude-code:w5-server-06d4):
//   guard (id / MaxPlayers / Players[id].+0x348) -> -2 (EAX preset at module storage)
// CSetMissionMessage record on the stack, FmtStr zeroed inline
// record.Text = fmt_str (FmtStr::operator=)
// record.Type = msg_type
// msg_type == 0 -> Players[id].+0x340 = 1 (bSkipNextAutosave)
// broadcast && Players[id].+0x404 (group) != 0
//                               -> CPlayerGroup::SetMissionMessage(&record)
// otherwise IClient slot 70 (+0x118)(&player_id, &record) --
//                             the default path, not a fallback; the field untested
// ~FmtStr; return 0
SERVER_API int __cdecl DisplayMissionMessage(const unsigned int& player_id, const FmtStr& fmt_str,
                                               MissionMessageType msg_type, bool broadcast) {
    if (!IsValidPlayer(player_id)) return -2;
    CSetMissionMessage Record;
    Record.Text = fmt_str;
    Record.Type = static_cast<unsigned int>(msg_type);
    if (msg_type == 0) {
        Players[player_id].bSkipNextAutosave = 1;
    }
    CPlayerGroup* const Group = static_cast<CPlayerGroup*>(Players[player_id].group_ptr);
    if (broadcast && Group != nullptr) {
        Group->SetMissionMessage(Record);
    } else {
        ::g_pClientInterface->SendServerSetMissionMessage(player_id, Record);
    }
    return 0;
}

// Player::SetStoryCue — sets story cue ID for player
// baseline (0x75aa0): validate player, store cue_id, notify server interface
// baseline 0x75d90 (module storage). Validates the slot, copies cue_id into a
// stack local, and fires IClient slot 90 (+0x168), ch2 op 0x43
// FLPACKET_SERVER_SET_STORY_CUE, with BOTH arguments by address:
// LEA  ESI,[ESP + 0x8]   ; &cueCopy
// PUSH ESI
// PUSH EDX               ; param_1, already a uint const*
// CALL dword ptr [EAX + 0x168]
// Nothing is stored on the player slot at all -- the old port comment said
// "store cue_id", and no such store exists.
SERVER_API int __cdecl SetStoryCue(const unsigned int& player_id, unsigned int cue_id) {
    if (!IsValidPlayer(player_id)) return -2;
    if (::g_pClientInterface) {
        ::g_pClientInterface->SendServerSetStoryCue(player_id, cue_id);
    }
    return 0;
}

// Player::PopUpDialog — baseline 0x06d55df0, THIRTEEN instructions and not one of
// them looks at Players:
// MOV  ECX,[.data:the field] ; the IClient global
// PUSH param_4 / param_3 / param_2 / param_1
// CALL [EAX + 0x17c]            ; SendCommonPopUpDialog, FLPACKET_COMMON_POP_UP_DIALOG
// XOR  EAX,EAX / RET            ; ALWAYS 0
// The port's `if (!IsValidPlayer(...)) return -2;` was invented -- baseline has no
// such guard here -- and the actual call was missing, so this was a no-op that
// could also refuse for a reason baseline does not have.
SERVER_API int __cdecl PopUpDialog(unsigned int player_id, const FmtStr& title, const FmtStr& text, unsigned int param) {
    if (::g_pClientInterface) {
        ::g_pClientInterface->SendCommonPopUpDialog(player_id, title, text, param);
    }
    return 0;
}

// Player::ReturnBestPath — baseline 0x06d55e20, eleven instructions:
// `CALL [IClient + 0x174]` (SendCommonRequestBestPath, FLPACKET_COMMON_REQUEST_BEST_PATH) with
// (param_1, param_2, param_3), then XOR EAX,EAX. No validation, always 0.
SERVER_API int __cdecl ReturnBestPath(unsigned int player_id, unsigned char* data, int data_len) {
    if (::g_pClientInterface) {
        ::g_pClientInterface->SendCommonRequestBestPath(player_id, data, (unsigned int)data_len);
    }
    return 0;
}

// Player::ReturnPlayerStats — baseline 0x06d55e40, the same eleven instructions
// through `CALL [IClient + 0x178]` (SendCommonRequestPlayerStats,
// FLPACKET_COMMON_REQUEST_PLAYER_STATS). No validation, always 0.
SERVER_API int __cdecl ReturnPlayerStats(unsigned int player_id, unsigned char* data, int data_len) {
    if (::g_pClientInterface) {
        ::g_pClientInterface->SendCommonRequestPlayerStats(player_id, data, (unsigned int)data_len);
    }
    return 0;
}

// Player::SetCamera -- baseline 0x75e60 (module storage). Validates the slot,
// assembles a 0x24-byte block and fires IClient slot 88 (+0x160), ch2 op 0x4b
// FLPACKET_SERVER_SET_CAMERA.

// THE SINGLETON IS NOT UNIDENTIFIED. The previous note called the field /
// "an unidentified Server.dll singleton" and left the dispatch
// out. the routine is four instructions and names itself:
// MOV  dword ptr [ESP + 0x8],.data:s_3DMathEngine
// MOV  dword ptr [ESP + 0x4],0xc
// CALL dword ptr [->NEURONCOMPONENTS.DLL::NEURONCOMPONENTS_Acquire]
// CALL dword ptr [ECX + 0xc]
// -- a NEURONCOMPONENTS_Acquire of "3DMathEngine" with a 0xC-byte descriptor, the same
// body Common.dll compiled into its own the routine. Server.dll has its own
// copy (21 call sites, XREF header of module storage), and the field is this
// function's file-static cache of the result, filled lazily at module storage.

// So slot +0x50 is I3DMathEngine::MatrixToQuaternion(FQuaternion&, FMatrix const&)
// (src/include/x86math.h), and the block baseline builds is:
// LEA EAX,[transform + 0x24]  -- Transform::Translation
// /ebd/eae  the three dwords -> [E-0x24], [E-0x20], [E-0x1c]
// PUSH ESI (&transform, which IS &transform.Rotation)
// LEA EDX,[ESP+0x8] (the 4-dword output at [E-0x34])
// CALL [ECX + 0x50]
// ..f14  those four dwords -> [E-0x18] .. [E-0x0c],
//                        then fov -> [E-0x08] and aspect -> [E-0x04]
// PUSH EDX (= E-0x24)      ; the block, second argument
// PUSH EDX (= E+0x04)      ; &player_id, FIRST argument
// CALL [EAX + 0x160]
// i.e. one contiguous 0x24-byte { Vector Pos; Quaternion Ornt; float A, B; },
// passed by address, with the player id also by address.

// The engine is this function's own lazily filled file-static, .data:the field
// (module storage test, module storage CALL the routine, module storage store),
// fetched through Server.dll's the routine (Server_Acquire3DMathEngine,
// server_createinfo.h) -- not through Common.dll's cache.
static I3DMathEngine* GSetCameraMathEngine = nullptr;   // .data:the field

SERVER_API int __cdecl SetCamera(unsigned int player_id, const Transform& transform, float fov, float aspect) {
    if (!IsValidPlayer(player_id)) return -2;

    // The exact block baseline puts on the wire: rpc_iclient_args.h's
    // FPayloadServerSetCamera {Vector Position +0x00, Quaternion Orient +0x0c (W,X,Y,Z),
    // float Fov +0x1c, float Aspect +0x20} -- the local FSetCameraBlock it replaced on
    // 2026-10-16 had this very layout (and gave the shared struct its field names).
    FPayloadServerSetCamera Block;
    Block.Position = transform.Translation;

    // .. module storage -- no null test on the engine and no
    // seed for the quaternion: the call always writes all four dwords.
    if (GSetCameraMathEngine == nullptr)
        GSetCameraMathEngine = Server_Acquire3DMathEngine();
    // baseline pushes &transform where a `FMatrix const&` is expected, which
    // works because Transform::Rotation is at +0x00. Spelled out here. Quaternion
    // and FQuaternion are the same four floats in the same (W,X,Y,Z) order.
    GSetCameraMathEngine->MatrixToQuaternion(reinterpret_cast<FQuaternion&>(Block.Orient),
                                             reinterpret_cast<FMatrix const&>(transform.Rotation));

    Block.Fov = fov;
    Block.Aspect = aspect;

    // / module storage -- the field is called through with
    // no test.
    ::g_pClientInterface->SendServerSetCamera(player_id, Block);
    return 0;
}

// Player::RevertCamera — reverts camera to default
// baseline (0x75b80): validate player, dispatch revert via server interface
// baseline 0x75f30 (module storage). Validates the slot, then:
// LEA  EDX,[ESP + 0x4]   ; &player_id -- BY ADDRESS
// PUSH EDX
// CALL dword ptr [EAX + 0x164]
// IClient::SendServerRevertCamera, ch2 op 0x4c FLPACKET_SERVER_REVERT_CAMERA.
SERVER_API int __cdecl RevertCamera(unsigned int player_id) {
    if (!IsValidPlayer(player_id)) return -2;
    if (::g_pClientInterface) {
        ::g_pClientInterface->SendServerRevertCamera(player_id);
    }
    return 0;
}

// Player::LoadHint — baseline 0x06d55f80, NINE instructions:
// MOV  EDX,[ESP + 0x8]          ; param_2, the hint
// PUSH EDX
// LEA  EDX,[ESP + 0x8] / PUSH   ; &param_1 -- the id BY ADDRESS
// CALL [IClient + 0x144]        ; SendServerLoadHint, FLPACKET_SERVER_LOADHINT
// XOR  EAX,EAX / RET            ; ALWAYS 0
// No validation here either: neither the interface global (06d55f84..06d55f8a load
// and its vtable with no test) nor the hint is checked. The by-address
// first argument matches the same BYREF convention rpclocal uses for this packet.
SERVER_API int __cdecl LoadHint(unsigned int player_id, BaseHint* hint) {
    // The hint goes on untested (module storage); the sender calls through it.
    ::g_pClientInterface->SendServerLoadHint(player_id, hint);
    return 0;
}

// Player::GetGroupSize — baseline 0x75fa0 (0x06d55fa0), ordinal 192
// ?GetGroupSize@Player@pub@@YAHIAAI@Z. Eleven instructions, and NOT one of
// them validates the player: it reads Players[id][-0x14], and
//   group != 0  -> *out = group->GetMemberCount()   (0x06d55fb8)
//   group == 0  -> *out = 1                         (0x06d55fca)
// always returning 0. The port's -2 guard and its `out_size = 1` placeholder
// were both invented; the 1 belongs on the NO-group branch, where it means
// "a party of one".
SERVER_API int __cdecl GetGroupSize(unsigned int player_id, unsigned int& out_size) {
    PlayerSlot* p = PlayerDB_GetPlayer(player_id);
    CPlayerGroup* grp = p ? (CPlayerGroup*)p->group_ptr : nullptr;
    if (grp) {
        out_size = grp->GetMemberCount();
    } else {
        out_size = 1;
    }
    return 0;
}

// Player::GetName — baseline 0x75fe0 (ordinal 222), module storage..06d56166.
// ?GetName@Player@pub@@YAHIAAV?$basic_string@GU?$char_traits@G@std@@V?$allocator@G@std@@@std@@@Z
// -- VC6's basic_string<unsigned short> (its wchar_t); the modern equivalent is std::wstring.
// EVERY path returns 0 (XOR EAX,EAX at 06d560b1 / 06d56102 / 06d56116 / 06d56164):
// id == 0, id-1 >= the field, slot+0x348 == 0 -> return 0,
//                       out-string untouched
// the inlined PlayerDB::GetActiveCharacterName (same guard again,
//                       the IsMPServer()/slot+0x64 gate, the routine)
// non-null name -> assign it (wcslen + _Grow + copy + terminate)
// null name     -> assign the literal u"<bad name>" (module storage)
SERVER_API int __cdecl GetName(unsigned int player_id, std::wstring& out_name) {
    if (!IsValidPlayer(player_id)) return 0;
    const wchar_t* Name = Players.GetActiveCharacterName(player_id);
    if (Name != nullptr) {
        out_name.assign(Name);
    } else {
        out_name.assign(L"<bad name>");
    }
    return 0;
}

// Player::IsGroupMember — baseline 0x76170 (0x06d56170), ordinal 314
// Same shape, no validation: equal ids answer true outright (0x06d56180),
// otherwise it asks PLAYER1's group whether player2 is in it
// (0x06d5619d CALL CPlayerGroup::IsMember), and answers false when player1
// has no group (0x06d561af). Comparing the two group_ptrs, as the port did,
// gives the same answer only because membership is symmetric -- but it also
// answered true when both players were grouped-with-nobody (two null
// pointers), which baseline's `TEST EAX,EAX / JZ` rules out.
SERVER_API int __cdecl IsGroupMember(unsigned int player1_id, unsigned int player2_id, bool& out_result) {
    if (player1_id == player2_id) { out_result = true; return 0; }
    PlayerSlot* p1 = PlayerDB_GetPlayer(player1_id);
    CPlayerGroup* grp = p1 ? (CPlayerGroup*)p1->group_ptr : nullptr;
    out_result = grp ? grp->IsMember(player2_id) : false;
    return 0;
}

// Player::RemoveFromGroup — baseline 0x761c0 (0x06d561c0), ordinal 361
// group = Players[id][-0x14]; when it is null the function returns
// == -3 (0x06d561e3), NOT -2, and when it is not it calls
// CPlayerGroup::DelMember (0x06d561db) -- which is what actually clears the
// player's group_ptr, unregisters him and notifies the rest. The port wrote
// group_ptr = nullptr by hand and left the group holding a stale member.
SERVER_API int __cdecl RemoveFromGroup(unsigned int player_id) {
    PlayerSlot* p = PlayerDB_GetPlayer(player_id);
    CPlayerGroup* grp = p ? (CPlayerGroup*)p->group_ptr : nullptr;
    if (!grp) return -3;
    grp->DelMember(player_id);
    return 0;
}

// Player::RewardGroup — baseline 0x763a0 (0x06d563a0), ordinal 388
// group != 0 -> CPlayerGroup::RewardMembers(amount) (0x06d563c3), return 0.
// group == 0 -> the solo path at 0x06d563cd: bounds-check the id against
// (the player count), require a live object at pd+0x348, then
// PlayerData::AdjustCash(amount, true) INLINED -- pd+0x320 accumulates the
// award (0x06d563f5) and pd+0x31c is the clamped cash (0x06d563fb).
// Either way the return value is 0; the port's -2 was invented.

// Re-traced 2026-09-24 (claude-code:w5-server-06d4), module storage..06d5645c:
// EDI = Players + id*0x418; group = [EDI-0x14] -- the slot
//                             is indexed RAW, before any bounds or occupancy test
// group -> CPlayerGroup::RewardMembers(amount); return 0
// else the usual guard (id / MaxPlayers / +0x348)
// PlayerData::AdjustCash(amount, true) inlined:
//                             +0x320 += amount; new = +0x31c + amount; the
//                             `!IsMPServer() || +0x64` gate; clamp [0, 999999999];
//                             +0x31c = new; IClient slot 77(&client_id, new);
// (0, &client_id)
// return 0 on every path
// PlayerData_AdjustCash (the routine) is that same body term for term.
SERVER_API int __cdecl RewardGroup(unsigned int player_id, int amount) {
    CPlayerGroup* const Group = static_cast<CPlayerGroup*>(Players[player_id].group_ptr);
    if (Group != nullptr) {
        Group->RewardMembers(amount);
        return 0;
    }
    if (IsValidPlayer(player_id)) {
        PlayerData_AdjustCash(Players[player_id], amount, true);
    }
    return 0;
}

// Player::SetMsnID -- baseline 0x76460 (module storage..06d564f2), ordinal 431.
// Ported 2026-09-24 (claude-code:w5-server-06d4). EVERY path returns 0 -- the guard
// failures jump straight to module storage XOR EAX,EAX:
// Players[id].+0x408 = msn_id   (group_mission_id)
// Players[id].+0x40c = sub_id   (group_mission_set_by)
// flag -> IClient slot 74 (+0x128)(Players[id].+0x348 BY VALUE, param)
//                   (module storage MOV EAX,[EAX+EBP-0xd0]; ch2 0x5b MISSIONSAVEA)
// IClient slot 114 (+0x1c8)(id, msn_id, sub_id), unconditional
// is not null-tested. The port's invented `active_mission` member is gone.
SERVER_API int __cdecl SetMsnID(unsigned int player_id, unsigned int msn_id, unsigned int sub_id,
                                  bool flag, unsigned int param) {
    if (!IsValidPlayer(player_id)) return 0;
    PlayerData& Slot = Players[player_id];
    Slot.group_mission_id = msn_id;
    Slot.group_mission_set_by = sub_id;
    if (flag) {
        ::g_pClientInterface->SendServerMissionSaveA(Slot.client_id, param);
    }
    ::g_pClientInterface->SendServerMiscObjUpdateMission(player_id, msn_id, sub_id);
    return 0;
}

// Player::GetMsnID -- baseline 0x76500 (module storage..06d56540), ordinal 221.
// Ported 2026-09-24 (claude-code:w5-server-06d4): on a valid id, out = Players[id].
// +0x408 (module storage MOV EAX,[ECX+EDX-0x10]); out is untouched otherwise, and the
// return is 0 on every path (module storage).
SERVER_API int __cdecl GetMsnID(unsigned int player_id, unsigned int& out_msn_id) {
    if (IsValidPlayer(player_id)) {
        out_msn_id = Players[player_id].group_mission_id;
    }
    return 0;
}

// Player::SetRank — sets player rank level
// baseline (0x75df0): validate player, store at +0x330 equivalent, notify vtable
// baseline 0x76550 (module storage): stores at slot[-0x194] (+0x284, rank_level)
// and then fires IClient slot 117 (+0x1d4):
// PUSH EAX   ; rank
// PUSH EDX   ; player id
// PUSH EDX   ; player id AGAIN
// CALL dword ptr [ESI + 0x1d4]
// Pushes are right-to-left, so the slot takes (id, id, rank) -- baseline really
// does pass the same id twice, by value both times. The port used to do the
// store and drop the notification.
SERVER_API int __cdecl SetRank(unsigned int player_id, int rank) {
    if (!IsValidPlayer(player_id)) return -2;
    PlayerSlot* p = PlayerDB_GetPlayer(player_id);
    if (!p) return -2;
    p->rank_level = (unsigned int)rank;
    if (::g_pClientInterface) {
        ::g_pClientInterface->SendServerMiscObjUpdateRank(player_id, player_id, rank);
    }
    return 0;
}

// Player::SetMoneyNeededToNextRank — sets cash threshold for next rank
// baseline (0x75e20): validate player, store at player data
SERVER_API int __cdecl SetMoneyNeededToNextRank(unsigned int player_id, int money) {
    if (!IsValidPlayer(player_id)) return -2;
    PlayerSlot* p = PlayerDB_GetPlayer(player_id);
    if (!p) return -2;
    p->money_needed_to_next_rank = money;
    return 0;
}

// Player::SetMonkey / SetRobot / SetRellan -- baseline 0x76600 / 0x76650 / 0x766c0.
// All three write the session Costume at PlayerData+0x2E4 and send NOTHING.
// The old comments ("set flag, notify vtable+0x144/+0x148/+0x14c", and the
// addresses 0x75e50/0x75e80/0x75eb0) were wrong on both counts: there is no
// flag, there is no notification, and those three addresses are inside
// SetCamera/RevertCamera.

//   SetMonkey  module storage  MOV dword ptr [slot + 0x2e4],0x88db0044
//   SetRobot   module storage  MOV dword ptr [slot + 0x2e4],0x0
// MOV dword ptr [slot + 0x2e8],0x8dffc147
// MOV dword ptr [slot + 0x2ec],0x9dc4c883
// MOV dword ptr [slot + 0x2f0],0x93582d4f
//   SetRellan   module storage  ADD EAX,0x2e4 / PUSH EAX / PUSH 0xb8249f07
// CALL [->COMMON.DLL::GetCostumeDescriptions]
// CALL [->COMMON.DLL::CostumeDescriptions::get_costume]
// The four literals are nickname ids (CreateID hashes); SetMonkey touching
// only PartIds[0] and leaving the rest alone is what baseline does.
SERVER_API int __cdecl SetMonkey(unsigned int player_id) {
    if (!IsValidPlayer(player_id)) return -2;
    PlayerSlot* p = PlayerDB_GetPlayer(player_id);
    if (!p) return -2;
    p->costume.PartIds[0] = 0x88db0044u;
    return 0;
}

SERVER_API int __cdecl SetRobot(unsigned int player_id) {
    if (!IsValidPlayer(player_id)) return -2;
    PlayerSlot* p = PlayerDB_GetPlayer(player_id);
    if (!p) return -2;
    p->costume.PartIds[0] = 0u;
    p->costume.PartIds[1] = 0x8dffc147u;
    p->costume.PartIds[2] = 0x9dc4c883u;
    p->costume.PartIds[3] = 0x93582d4fu;
    return 0;
}

SERVER_API int __cdecl SetRellan(unsigned int player_id) {
    if (!IsValidPlayer(player_id)) return -2;
    PlayerSlot* p = PlayerDB_GetPlayer(player_id);
    if (!p) return -2;
    CostumeDescriptions const* Costumes = GetCostumeDescriptions();
    if (Costumes != nullptr) {
        Costumes->get_costume((int)0xb8249f07u, p->costume);
    }
    return 0;
}

// Player::SendNNMessage -- baseline 0x76720 (module storage).
// TEST ESI,ESI / JZ -> return -4     ; msg_id == 0 is -4
// the usual index / slot-occupied validation -> -4 as well
//                   (EAX is seeded with 0xfffffffc, not 0xfffffffe, at
// , so EVERY failure here returns -4)
// PUSH ESI (msg_id) / PUSH EDX (player id)
// CALL dword ptr [EAX + 0x114]       ; IClient slot 69
// Note the -4: the port returned -2 for an invalid player, which no path in
// this function can produce.
SERVER_API int __cdecl SendNNMessage(unsigned int player_id, unsigned int msg_id) {
    if (msg_id == 0) return -4;
    if (!IsValidPlayer(player_id)) return -4;
    if (::g_pClientInterface) {
        ::g_pClientInterface->SendServerSendNNMessage(player_id, msg_id);
    }
    return 0;
}

// Player::ForceLand — forces player to dock at a base
// baseline 0x76780 (?ForceLand@Player@pub@@YAHII@Z) is a two-line function:
//     IServerImpl::Dock((IServerImpl *)&Server, &param_2, &param_1);
//     return 0;
// i.e. Dock(BaseId, PlayerId) — base first, session second, both by reference.
// This confirms the IServer slot 91 signature recovered from the IServer card in implementation tracker.
// baseline does NOT validate the player here; the validation lives inside Dock.
// `Server` is a static object (MOV ECX,.data:Server at 06d5678a), so there is
// nothing to null-test; GetServerInterface() returns that same object.
SERVER_API int __cdecl ForceLand(unsigned int player_id, unsigned int base_id) {
    ::GetServerInterface()->Dock(base_id, player_id);
    return 0;
}

// Player::SetCostume — sets player costume
// baseline: EMPTY function body (just returns 0)
// baseline 0x767c0 is literally two instructions -- `XOR EAX,EAX / RET`
// (module storage). It is a shipped no-op, not a port stub: nothing is stored,
// nothing is sent, and the player id is never even loaded. Do not "finish" it.
SERVER_API int __cdecl SetCostume(const unsigned int& player_id, int costume_id) {
    (void)player_id; (void)costume_id;
    return 0;
}

// Player::GetGender — baseline 0x767a0 (ordinal 183) is two instructions:
//   *param_2 = 1; return 0;
// There is no player validation at all — it always succeeds.
SERVER_API int __cdecl GetGender(const unsigned int& player_id, int& out_gender) {
    out_gender = 1;
    return 0;
}

// Player::GetBody — baseline 0x767b0 (ordinal 158) is likewise two instructions:
//   *param_2 = 0; return 0;
// No validation, no player-slot access. It was missing from the port entirely.
SERVER_API int __cdecl GetBody(const unsigned int& player_id, unsigned int& out_body) {
    out_body = 0;
    return 0;
}

// Player::GetGroupMembers — retrieves all member IDs in player's group
// baseline (0x761f0): delegates to CPlayerGroup::StoreMemberList if in group
SERVER_API int __cdecl GetGroupMembers(unsigned int player_id, std::vector<unsigned int>& out_members) {
    out_members.clear();
    if (!IsValidPlayer(player_id)) return -2;
    PlayerSlot* p = PlayerDB_GetPlayer(player_id);
    if (p && p->group_ptr) {
        CPlayerGroup* grp = (CPlayerGroup*)p->group_ptr;
        grp->StoreMemberList(out_members);
    } else {
        out_members.push_back(player_id);
    }
    return 0;
}

// Player::SetMissionObjectives -- baseline module storage .. module storage,
// ?SetMissionObjectives@Player@pub@@YAHABI0PBUMissionObjective@12@IABUFmtStr@@E2@Z.
// Rewritten 2026-09-24 (claude-code:w5-server-06d4) from the listing. baseline touches
// NO server-side mission log: it builds one FServerSetMissionObjectivesInfo on the
// stack and hands it to IClient slot 72 (+0x120, ch2 op 0x35).
// *player_id: 0 or > MaxPlayers or Players[id].client_id == 0
//                       -> return -2
// two FmtStr() (the inlined zeroing) at +0x0c / +0x24
// +0x00 = *mission_id
// +0x08 = count
// +0x0c = title        (FmtStr::operator=)
// +0x3c = type
// +0x24 = desc         (FmtStr::operator=)
// count != 0: operator new(count*0x1c + 4); on success the
//                       count goes into the first dword and the routine (eh vector
//                       ctor, the routine = FmtStr(), ~FmtStr) builds the elements
//                       from +4; a failed new leaves the array pointer 0
// +0x04 = that array (0 when count == 0, 06d55aea)
// for i < count: e[i].Text = objs[i].Description (operator=),
//                       e[i].State = objs[i].Type -- no test of the array pointer
// IClient slot 72 (&player_id, &record) -- the reference is
//                       passed as is, rpclocal dereferences it (module storage); no
//                       interface null test
// array != 0: eh vector dtor over the cookie count
//                       (the routine) and operator delete of array-4 (the routine)
// return 0
SERVER_API int __cdecl SetMissionObjectives(const unsigned int& player_id, const unsigned int& mission_id, const MissionObjective* objs, unsigned int count, const FmtStr& title, unsigned char type, const FmtStr& desc) {
    if (!IsValidPlayer(player_id)) return -2;

    FServerSetMissionObjectivesInfo Payload;
    Payload.Field_0x00 = mission_id;
    Payload.Count = count;
    Payload.TextA = title;
    Payload.Field_0x3c = type;
    Payload.TextB = desc;

    FServerMissionObjectiveEntry* Entries = nullptr;
    if (count != 0) {
        unsigned char* const Block = static_cast<unsigned char*>(
            operator new(count * sizeof(FServerMissionObjectiveEntry) + 4));
        if (Block != nullptr) {
            *reinterpret_cast<unsigned int*>(Block) = count;
            Entries = reinterpret_cast<FServerMissionObjectiveEntry*>(Block + 4);
            for (unsigned int I = 0; I < count; ++I)
                new (&Entries[I]) FServerMissionObjectiveEntry;
        }
        for (unsigned int I = 0; I < count; ++I) {
            Entries[I].Text = objs[I].Description;
            Entries[I].State = objs[I].Type;
        }
    }
    Payload.Entries = Entries;

    ::g_pClientInterface->SendServerSetMissionObjectives(player_id, Payload);

    if (Entries != nullptr) {
        unsigned char* const Block = reinterpret_cast<unsigned char*>(Entries) - 4;
        unsigned int const Cookie = *reinterpret_cast<unsigned int*>(Block);
        for (unsigned int I = Cookie; I != 0; --I)
            Entries[I - 1].~FServerMissionObjectiveEntry();
        operator delete(Block);
    }
    return 0;
}

// Player::ReplaceMissionObjective — updates specific objective in list
// baseline (0x75c10): validates player, replaces objective at index
// CHANGED 2026-09-17 (wave 18): this used to do
// `p->mission_log[index].timestamp = obj.Type`, writing a MissionObjective's type
// into a mission-log entry. That was a fabrication twice over -- baseline's 0x75c10
// never touches PlayerData+0x390 at all, and the `timestamp` field it wrote into
// did not exist in baseline either (the list holds FmtStr; see
// PlayerData::mission_log in common_server_playerdb.h). The bogus write is gone.

// Ported 2026-09-24 (claude-code:w5-server-06d4) off module storage..06d55d0f:
//   guard -> -2 (EAX preset at module storage)
// FmtStr of the record zeroed inline
// record+0x00 = *mission_id
// record+0x20 = (byte)index
// record+0x04 = obj.Description (FmtStr::operator=)
// record+0x1c = obj.Type
// IClient slot 71 (+0x11c)(&player_id, &record), untested
// ~FmtStr; return 0
// Nothing is written to the player slot.
SERVER_API int __cdecl ReplaceMissionObjective(const unsigned int& player_id, const unsigned int& mission_id, unsigned int index, const MissionObjective& obj) {
    if (!IsValidPlayer(player_id)) return -2;
    FServerReplaceMissionObjectiveInfo Record;
    Record.Field_0x00 = mission_id;
    Record.State = static_cast<unsigned char>(index);
    Record.Text = obj.Description;
    Record.ObjectiveType = obj.Type;
    ::g_pClientInterface->SendServerReplaceMissionObjective(player_id, Record);
    return 0;
}

// Player::SetMissionObjectiveState -- baseline 0x75d20 (module storage..06d55d8b),
// ordinal 426. Ported 2026-09-24 (claude-code:w5-server-06d4):
//   guard -> -2; record {+0x00 = *mission_id (module storage), byte +0x04 = (byte)index
//   (module storage), +0x08 = state (module storage)}; IClient slot 73 (+0x124)
//   (&player_id, &record) with the field untested (module storage); return 0.
SERVER_API int __cdecl SetMissionObjectiveState(const unsigned int& player_id, const unsigned int& mission_id, int index, unsigned int state) {
    if (!IsValidPlayer(player_id)) return -2;
    FServerMissionObjectiveStateInfo Record;
    Record.Field_0x00 = mission_id;
    Record.Field_0x04 = static_cast<unsigned char>(index);
    Record.Field_0x08 = state;
    ::g_pClientInterface->SendServerSetMissionObjectiveState(player_id, Record);
    return 0;
}

} // namespace Player

// ===========================================================================
// pub::GF exports (Game Framework) -- gf/pubGF.cpp. Every object the family
// creates lives in the BaseDB (the routine = GetBaseDB(), src/Server/server_basedb.cpp).
// ===========================================================================
namespace GF {

// ---- FindBase / FindLocation / GetSpaceflightLocation ----

// pub::GF::FindBase — baseline 0x37ca0: CreateID(name), returns hash
// simply calls CreateID
SERVER_API unsigned int __cdecl FindBase(const char* name) {
    // No null test on name in baseline (module storage..06d17cb1 -> the routine,
    // whose body is a bare CreateID call); the port's guard was removed 2026-09-24.
    return CreateID(name);
}

// pub::GF::FindLocation — baseline 0x37cc0: MakeLocationID(system_id, name)
// simply calls MakeLocationID
SERVER_API unsigned int __cdecl FindLocation(unsigned int base_id, const char* name) {
    // /0x06d17cd1 -> the routine, whose entire body (0x06d14210-
    // ) is `PUSH param_2; PUSH param_1; CALL [COMMON.DLL::MakeLocationID]`.
    // The port used to call CreateID(name) and DISCARD the first argument entirely, so
    // every base produced the same location id for a given room name. Fixed.
    // The `!name` guard (absent from both baseline bodies) was removed 2026-09-24.
    return MakeLocationID(base_id, name);
}

// pub::GF::GetSpaceflightLocation — baseline 0x37ce0
SERVER_API unsigned int __cdecl GetSpaceflightLocation() {
    // is two instructions: load the IAT slot
    // PTR_SpaceflightLocationID_06d647b0 (COMMON.DLL data export
    // ?SpaceflightLocationID@@3UID_String@@A) and dereference it. The port had a private
    // `static unsigned int g_SpaceflightLocationID = 0;` that nothing ever wrote, so this
    // always returned 0. The real global is already ported: common_misc.cpp's
    // SpaceflightLocationID = CreateID("Spaceflight"). Fixed.
    return ::SpaceflightLocationID.Id;
}

/// ---- GetBasePosition ----

// pub::GF::GetBasePosition -- baseline module storage .. module storage. Single-entry memo cache
// (base id) + the field/580/584 (the Vector), reset by the branch.
static unsigned int GCachedBasePosId = 0;
static float GCachedBasePos[3] = { 0, 0, 0 };

// baseline's exported name is ?GetBasePosition@GF@pub@@YAHABI0AAVVector@@@Z: "ABI0" means
// BOTH id parameters are `unsigned int const&` (the `0` back-references arg 1's type).
// Ported 1:1 2026-09-24 (claude-code:w6-sys):
// base_id & 0x3fffffff == 0 -> -2, then system_id & 0x3fffffff == 0
//                         -> -2 (base first, system second)
// base_id == cached id -> *out_pos = cached Vector, return 0
// Universe::get_system(system_id) -- no null test after it
// clear the four TString fields of the stack object record
// count = ISystem slot 15 (vt+0x3c); unsigned JBE 0 -> -2
// for index < count (unsigned): slot 16 (vt+0x40)(index, &record);
//                         true and record +0x80 (dock_with) == base_id -> match
// cache = {base_id, record +0x44 pos}; *out_pos = pos; return 0
// no match -> -2
SERVER_API int __cdecl GetBasePosition(const unsigned int& system_id, const unsigned int& base_id, Vector& out_pos) {
    if ((base_id & 0x3fffffff) == 0) return -2;
    if ((system_id & 0x3fffffff) == 0) return -2;

    if (base_id == GCachedBasePosId) {
        out_pos.x = GCachedBasePos[0];
        out_pos.y = GCachedBasePos[1];
        out_pos.z = GCachedBasePos[2];
        return 0;
    }

    Universe::ISystem const* Sys = Universe::get_system(system_id);
    Universe::FSystemObject Record;
    Record.Nickname.Length = 0;
    Record.Nickname.Data[0] = '\0';
    Record.Reputation.Length = 0;
    Record.Reputation.Data[0] = '\0';
    Record.NextRing.Length = 0;
    Record.NextRing.Data[0] = '\0';
    Record.PrevRing.Length = 0;
    Record.PrevRing.Data[0] = '\0';

    unsigned int const Count = (unsigned int)Sys->GetObjectCount();
    for (unsigned int Index = 0; Index < Count; ++Index) {
        if (!Sys->GetSystemObject(Index, &Record)) continue;
        if (Record.DockWith != base_id) continue;
        GCachedBasePosId = base_id;
        GCachedBasePos[0] = Record.Pos.x;
        GCachedBasePos[1] = Record.Pos.y;
        GCachedBasePos[2] = Record.Pos.z;
        out_pos.x = Record.Pos.x;
        out_pos.y = Record.Pos.y;
        out_pos.z = Record.Pos.z;
        return 0;
    }
    return -2;
}

// ---- Costume / BodyPart / Accessory ----

// pub::GF::GetCostumeByID -- baseline module storage .. module storage:
// GetCostumeDescriptions()->get_costume(param_1, param_2), no null test.
SERVER_API void __cdecl GetCostumeByID(int id, Costume& out_costume) {
    GetCostumeDescriptions()->get_costume(id, out_costume);
}

// pub::GF::GetBodyPart -- baseline module storage .. module storage:
// GetCostumeDescriptions()->find_bodypart(param_2, param_1); a hit returns the
// record's first dword, a miss FDUMPs "Cannot find bodypart %s" (pubGF.cpp line
// 0x78) with the name and returns 0. No null tests.
SERVER_API int __cdecl GetBodyPart(const char* name, int type_id) {
    bodypart const* bp = GetCostumeDescriptions()->find_bodypart(type_id, name);
    if (bp) return *(int const*)bp;
    FDUMP(0x100001, "%s(%d) : *** ERROR: Cannot find bodypart %s\n",
          "Server\\gf\\pubGF.cpp", 0x78, name);
    return 0;
}

// pub::GF::GetAccessory -- baseline module storage .. module storage: the same
// with find_accessory(param_1), "Cannot find accessory %s", line 0x87.
SERVER_API int __cdecl GetAccessory(const char* name) {
    accessory const* acc = GetCostumeDescriptions()->find_accessory(name);
    if (acc) return *(int const*)acc;
    FDUMP(0x100001, "%s(%d) : *** ERROR: Cannot find accessory %s\n",
          "Server\\gf\\pubGF.cpp", 0x87, name);
    return 0;
}

// pub::GF::GetCostumeSkeletonGender -- baseline module storage .. module storage:
// *param_2 = GetCostumeDescriptions()->get_costume_gender(param_1); return 0.
// No null test.
SERVER_API int __cdecl GetCostumeSkeletonGender(Costume const& desc, int& out_gender) {
    out_gender = GetCostumeDescriptions()->get_costume_gender(desc);
    return 0;
}

// ---- Character Create/Destroy/SetBehavior ----

// pub::GF::CharacterCreate -- baseline module storage .. module storage: a local
// CharacterProperties, init(description), the routine(BaseDB, &props,
// description+0x50 = the session filter), the destructor, the handle.
SERVER_API unsigned int __cdecl CharacterCreate(const CharacterDescription& description) {
    CharacterProperties Props;
    Props.init(description);
    return (unsigned int)GetBaseDB()->create_character(Props, description.Filter);
}

// pub::GF::CharacterDestroy -- baseline module storage .. module storage:
// `TEST [param_1],0x3fffffff / JZ out`, the routine(BaseDB, *param_1),
// *param_1 = 0. No null test of the pointer.
SERVER_API void __cdecl CharacterDestroy(unsigned int* handle) {
    if ((*handle & 0x3fffffff) != 0) {
        GetBaseDB()->destroy_character(*handle);
        *handle = 0;
    }
}

// pub::GF::CharacterSetBehavior -- baseline module storage .. module storage: an
// unconditional the routine(BaseDB, param_1, param_2).
SERVER_API void __cdecl CharacterSetBehavior(unsigned int character_id, unsigned long behavior_handle) {
    GetBaseDB()->set_character_behavior(character_id, behavior_handle);
}

// ---- CharacterBehavior Create/Destroy ----

// pub::GF::CharacterBehaviorCreate -- baseline module storage .. module storage: a
// local ScriptBehavior, init(description), the routine(BaseDB, &sb,
// description+0x25c = the session filter), the destructor, the handle.
SERVER_API unsigned long __cdecl CharacterBehaviorCreate(const CharacterBehaviorDescription& description) {
    ScriptBehavior Behavior;
    Behavior.init(description);
    return GetBaseDB()->create_behavior(Behavior, description.Filter);
}

// pub::GF::CharacterBehaviorDestroy -- baseline module storage .. module storage:
// `CMP [param_1],-1 / JZ out`, the routine(BaseDB, *param_1), *param_1 = -1.
SERVER_API void __cdecl CharacterBehaviorDestroy(unsigned long* handle) {
    if (*handle != 0xFFFFFFFF) {
        GetBaseDB()->destroy_behavior(*handle);
        *handle = 0xFFFFFFFF;
    }
}

// ---- Character Placement functions ----
// All use the routine(BaseDB, location_id) → RoomData*, then FUN_06d127XX on RoomData

// pub::GF::GetNumCharacterPlacements — baseline 0x380e0
// reads RoomData+0x178/0x17C (placement array begin/end), returns count
SERVER_API int __cdecl GetNumCharacterPlacements(unsigned int location_id) {
    // (find_location; a miss FDUMPs 'Location %u is invalid'),
    // null -> return; the FUN_06d127xx helper then reads [room+0x8] unchecked.
    FRoomEntry* Entry = GetBaseDB()->find_location(location_id);
    if (!Entry) return 0;
    RoomData* room = Entry->Data;
    return (int)room->CharacterPlacements.size();
}

// pub::GF::GetCharacterPlacementByName — baseline 0x38100
// iterates placements comparing names via stricmp, returns index or -1
SERVER_API int __cdecl GetCharacterPlacementByName(unsigned int location_id, const char* name, int& out_index) {
    // has no guard on param_3. The port opened with
    // `if (!name || !out_index) return -2;` -- out_index being an `int&`, `!out_index`
    // tested the caller's current VALUE, so the ordinary `int idx = 0; ...(loc, n, idx);`
    // call shape always bailed out with -2 before searching anything. Removed.
    // 2026-09-24 (claude-code:server-triage-s2): the `!name` early -2 and the
    // `cp &&` element test went too. the routine (module storage..06d127a8)
    // reads each record's first dword through the element pointer with no null
    // test and hands `name` straight to stricmp; only the record's name pointer
    // is tested for null.
    // (find_location; a miss FDUMPs 'Location %u is invalid'),
    // null -> return; the FUN_06d127xx helper then reads [room+0x8] unchecked.
    FRoomEntry* Entry = GetBaseDB()->find_location(location_id);
    if (!Entry) return -2;
    RoomData* room = Entry->Data;
    for (unsigned int i = 0; i < room->CharacterPlacements.size(); i++) {
        CharPlaceInfo* cp = room->CharacterPlacements[i];
        if (cp->Name && _stricmp(cp->Name, name) == 0) {
            out_index = (int)i;
            return 0;
        }
    }
    out_index = -1;
    return -3;
}

// pub::GF::GetCharacterPlacementName — baseline 0x38140
// returns placement[index]->name (first dword of placement struct)
SERVER_API const char* __cdecl GetCharacterPlacementName(unsigned int location_id, int index) {
    // (module storage..06d127c0) is a bare `placements[index]`: no
    // bounds test, no element null test; module storage then returns the record's
    // first dword as-is, a null name included. The bounds/null guards the port had
    // here were removed 2026-09-24 (claude-code:server-triage-s2).
    // (find_location; a miss FDUMPs 'Location %u is invalid'),
    // null -> return; the FUN_06d127xx helper then reads [room+0x8] unchecked.
    FRoomEntry* Entry = GetBaseDB()->find_location(location_id);
    if (!Entry) return "";
    RoomData* room = Entry->Data;
    return room->CharacterPlacements[index]->Name;
}

// pub::GF::GetCharacterPlacementOccupancy — baseline 0x38170
// + offset 0x34: reads occupancy from placement struct
// CharPlaceInfo+0x34 is CharType (-1=none, 0=npc, 1=pc, 2=rpc; see common_goods.h and
// RoomData::read_set_script's Setpoints->CharacterPlacements derivation in common_roomdata.cpp).
SERVER_API unsigned long __cdecl GetCharacterPlacementOccupancy(unsigned int location_id, int index) {
    // the routine (unchecked `placements[index]`) then
    // `MOV EAX,[EAX+0x34]`. Bounds/null guards removed 2026-09-24.
    // (find_location; a miss FDUMPs 'Location %u is invalid'),
    // null -> return; the FUN_06d127xx helper then reads [room+0x8] unchecked.
    FRoomEntry* Entry = GetBaseDB()->find_location(location_id);
    if (!Entry) return 0;
    RoomData* room = Entry->Data;
    return (unsigned long)room->CharacterPlacements[index]->CharType;
}

// pub::GF::GetCharacterPlacementPosture — baseline 0x381a0
// + offset 0x38: reads posture from placement struct
// CharPlaceInfo+0x38 is Posture (0=stand, 1=sitlow, 2=sithigh, 3=special, -1=unset).
SERVER_API int __cdecl GetCharacterPlacementPosture(unsigned int location_id, int index, unsigned long& out_posture) {
    // has no guard on param_3; the port's
    // `if (!out_posture) return -2;` tested the REFERENCE'S VALUE (same defect as
    // GetCharacterPlacementByName above) and short-circuited every zero-initialised
    // caller. Removed.
    // the routine (unchecked `placements[index]`) then
    // `MOV EAX,[EAX+0x38]`. Bounds/null guards removed 2026-09-24.
    // (find_location; a miss FDUMPs 'Location %u is invalid'),
    // null -> return; the FUN_06d127xx helper then reads [room+0x8] unchecked.
    FRoomEntry* Entry = GetBaseDB()->find_location(location_id);
    if (!Entry) return -2;
    RoomData* room = Entry->Data;
    out_posture = (unsigned long)room->CharacterPlacements[index]->Posture;
    return 0;
}

// pub::GF::IsCharacterPlacementNormal — baseline 0x381e0
// + offset 0x3C: checks flag == 0
// CharPlaceInfo+0x3C is bZoneScript (0="zg/" zone-group prefix = normal, 1="zs/" zone-script prefix = special).
SERVER_API bool __cdecl IsCharacterPlacementNormal(unsigned int location_id, int index) {
    // the routine (unchecked) then module storage
    // `MOV BL,[EAX+0x3c]`. Bounds/null guards removed 2026-09-24.
    // (find_location; a miss FDUMPs 'Location %u is invalid'),
    // null -> return; the FUN_06d127xx helper then reads [room+0x8] unchecked.
    FRoomEntry* Entry = GetBaseDB()->find_location(location_id);
    if (!Entry) return true;
    RoomData* room = Entry->Data;
    return room->CharacterPlacements[index]->bZoneScript == 0;
}

// pub::GF::IsCharacterPlacementSpecial — baseline 0x38210
// + offset 0x3C: checks flag == 1
SERVER_API bool __cdecl IsCharacterPlacementSpecial(unsigned int location_id, int index) {
    // the routine (unchecked) then `MOV BL,[EAX+0x3c]` /
    // `CMP BL,0x1`. Bounds/null guards removed 2026-09-24.
    // (find_location; a miss FDUMPs 'Location %u is invalid'),
    // null -> return; the FUN_06d127xx helper then reads [room+0x8] unchecked.
    FRoomEntry* Entry = GetBaseDB()->find_location(location_id);
    if (!Entry) return false;
    RoomData* room = Entry->Data;
    return room->CharacterPlacements[index]->bZoneScript == 1;
}

// pub::GF::EnumerateCharacterPlacementIni — baseline 0x38250
// delegates to RoomData::enumerate_CharacterPlacement_ini
// ?EnumerateCharacterPlacementIni@GF@pub@@YAXIP6AXHPAVINI_Reader@@PAX@Z1@Z -- the
// trailing `1` repeats PAX, so the context is a void*. baseline (module storage..06d18276)
// has no callback null test: GetBaseDB -> the routine(id), null room -> RET, else
// (room, callback, context).
SERVER_API void __cdecl EnumerateCharacterPlacementIni(unsigned int location_id,
    void (__cdecl *callback)(int, INI_Reader*, void*), void* context) {
    // (find_location; a miss FDUMPs 'Location %u is invalid'),
    // null -> return; the FUN_06d127xx helper then reads [room+0x8] unchecked.
    FRoomEntry* Entry = GetBaseDB()->find_location(location_id);
    if (!Entry) return;
    RoomData* room = Entry->Data;
    room->enumerate_CharacterPlacement_ini(callback, context);
}

// pub::GF::GetCharacterOnPlacement -- baseline module storage .. module storage:
// (BaseDB, param_1); 0 -> return 0; else the routine(room,
// param_2 = the session filter, param_3 = the placement index).
SERVER_API unsigned int __cdecl GetCharacterOnPlacement(unsigned int location_id, unsigned int filter_id, int placement_index) {
    FRoomEntry* room = GetBaseDB()->find_location(location_id);
    if (!room) return 0;
    return room->get_character_on_placement(filter_id, placement_index);
}

// pub::GF::GetCharacterPlacementProperties — baseline 0x382b0
// Gets placement name, then SetpointProperties::extract_from_name
SERVER_API bool __cdecl GetCharacterPlacementProperties(unsigned int location_id, int index, SetpointProperties* out_props) {
    // no test on param_3; the routine (unchecked
    // `placements[index]`), `MOV EAX,[EAX]`, and only the NAME is null-tested
    // (module storage). The out_props / bounds / element guards were removed
    // 2026-09-24 (claude-code:server-triage-s2).
    // (find_location; a miss FDUMPs 'Location %u is invalid'),
    // null -> return; the FUN_06d127xx helper then reads [room+0x8] unchecked.
    FRoomEntry* Entry = GetBaseDB()->find_location(location_id);
    if (!Entry) return false;
    RoomData* room = Entry->Data;
    CharPlaceInfo* cp = room->CharacterPlacements[index];
    if (!cp->Name) return false;
    out_props->extract_from_name(cp->Name);
    return out_props->bValid != 0;
}

// pub::GF::FormCharacterPlacementName -- baseline module storage .. module storage:
// `MOV ECX,[ESP+4] / JMP SetpointProperties::combine_to_name`, no null test.
SERVER_API const char* __cdecl FormCharacterPlacementName(SetpointProperties const* props) {
    return props->combine_to_name();
}

// ---- MissionVendor ----

// pub::GF::MissionVendorOfferCreate -- baseline module storage .. module storage:
// local MissionVendorProperties, init(description), the routine(BaseDB, &props,
// description+0x5c = the session filter), the destructor, the handle.
SERVER_API unsigned long __cdecl MissionVendorOfferCreate(const MissionVendorOfferDescription& description) {
    MissionVendorProperties Props;
    Props.init(description);
    return GetBaseDB()->create_mission_vendor(Props, description.Filter);
}

// pub::GF::MissionVendorOfferDestroy -- baseline module storage: `CMP [p],-1`,
// (BaseDB, *p), *p = -1.
SERVER_API void __cdecl MissionVendorOfferDestroy(unsigned long* handle) {
    if (*handle != 0xFFFFFFFF) {
        GetBaseDB()->destroy_mission_vendor(*handle);
        *handle = 0xFFFFFFFF;
    }
}

// pub::GF::GetMissionVendorOfferCount -- baseline module storage .. module storage:
// (BaseDB, param_1, param_2) returned as-is.
SERVER_API int __cdecl GetMissionVendorOfferCount(unsigned int base_id, unsigned int vendor_type) {
    return GetBaseDB()->count_mission_vendors(base_id, vendor_type);
}

// pub::GF::MissionVendorAcceptance -- baseline module storage .. module storage: an
// unconditional the routine(BaseDB, param_1, param_2, param_3, param_4).
SERVER_API void __cdecl MissionVendorAcceptance(unsigned long handle, bool accepted, const FmtStr& reason_fmtstr, unsigned int session_id) {
    GetBaseDB()->mission_vendor_acceptance(handle, accepted, reason_fmtstr, session_id);
}

// pub::GF::ReportWhyMissionVendorEmpty — baseline 0x38410
// ?ReportWhyMissionVendorEmpty@GF@pub@@YAHIW4MVEmptyReason@@@Z -- a global enum.
SERVER_API int __cdecl ReportWhyMissionVendorEmpty(unsigned int client_id, MVEmptyReason reason) {
    // , all nine instructions: load the field, load its
    // vtable, PUSH param_2 then PUSH param_1, CALL [vtable+0xa8], XOR EAX,EAX.
    // is the IClient pointer (implementation notes), and +0xa8 is
    // IClient::SendServerGFMissionVendorWhyEmpty = FLPACKET_SERVER_GFMISSIONVENDORWHYEMPTY,
    // channel 2 opcode 0x5a, arity 2 (implementation notes). So this is a real packet
    // send, not the "debug/diagnostic, stub is acceptable" the port had assumed --
    // another vtable offset mistaken for something else. Push order gives (client_id,
    // reason): the first argument is the ClientId the packet goes to (it was misnamed
    // `base_id` until 2026-10-16; Content's callers pass the mission state's PlayerId).
    // is called through with no null test (module storage .. 06d18422).
    ::g_pClientInterface->SendServerGFMissionVendorWhyEmpty(client_id, (unsigned int)reason);
    return 0;
}

// ---- NewsBroadcast ----

// pub::GF::NewsBroadcastCreate -- baseline module storage: local
// NewsBroadcastProperties, init(description), the routine(BaseDB, &props,
// description+0x5c = the session filter), the destructor, the handle.
SERVER_API unsigned long __cdecl NewsBroadcastCreate(const NewsBroadcastDescription& description) {
    NewsBroadcastProperties Props;
    Props.init(description);
    return GetBaseDB()->create_news_broadcast(Props, description.Filter);
}

// pub::GF::NewsBroadcastDestroy -- baseline module storage: `CMP [p],-1`,
// (BaseDB, *p), *p = -1.
SERVER_API void __cdecl NewsBroadcastDestroy(unsigned long* handle) {
    if (*handle != 0xFFFFFFFF) {
        GetBaseDB()->destroy_news_broadcast(*handle);
        *handle = 0xFFFFFFFF;
    }
}

// ---- AmbientScript ----

// pub::GF::AmbientScriptCreate -- baseline module storage .. module storage: local
// AmbientScriptProperties, init(description), the routine(BaseDB, &props,
// description+0x10 = the session filter), the destructor, the handle.
SERVER_API unsigned long __cdecl AmbientScriptCreate(const AmbientScriptDescription& description) {
    AmbientScriptProperties Props;
    Props.init(description);
    return GetBaseDB()->create_ambient_script(Props, description.Filter);
}

// pub::GF::AmbientScriptDestroy -- baseline module storage .. module storage:
// `CMP [p],-1`, the routine(BaseDB, *p), *p = -1.
SERVER_API void __cdecl AmbientScriptDestroy(unsigned long* handle) {
    if (*handle != 0xFFFFFFFF) {
        GetBaseDB()->destroy_ambient_script(*handle);
        *handle = 0xFFFFFFFF;
    }
}

// ---- GetRtcPerformanceSlider ----

// pub::GF::GetRtcPerformanceSlider — baseline 0x38590: if !IsMPServer(), RtcSlider::get_slider(); else 1.0
SERVER_API float __cdecl GetRtcPerformanceSlider() {
    // stage 1.0f in a stack local, CALL
    // [COMMON.DLL::IsMPServer]; if it returns false, tail-JMP
    // [COMMON.DLL::RtcSlider::get_slider]; otherwise FLD that 1.0f.
    // The port used SinglePlayer() (a different predicate) and returned a hard-coded
    // 1.0f on BOTH branches, so the real slider was never read. RtcSlider::get_slider is
    // already ported (src/include/common_newchar.h, src/Common/common_rtcslider.cpp).
    if (!::IsMPServer()) {
        return ::RtcSlider::get_slider();
    }
    return 1.0f;
}

} // namespace GF

// ===========================================================================
// pub::Reputation exports
// ===========================================================================

} // namespace pub -- the vibe-id bitmap is file-level Server state, not pub::

// Internal: vibe ID bitmap, baseline module storage (0x1000 bits, a
// std::bitset<0x1000> -- the routine is its out_of_range throw) and the last
// allocated index module storage. Both are .bss, so both start at 0.
static unsigned int g_VibeBitmap[128] = {};
static unsigned int g_VibeBitmapHint = 0;

// ---------------------------------------------------------------------------
// Server_VibeID_Alloc -- baseline the routine (module storage .. module storage),
// cdecl (unsigned int* Out), plain RET. Callers: the routine's record copy
// (module storage, PlayerData_ApplySavedCharacter, declared in
// server_savegame.h) and pub::Reputation below. CORRECTED 2026-09-24
// (claude:srv-save-2): it used to be defined inside `namespace pub`, so the
// global declaration in server_savegame.h never linked (LNK2019), and its loop
// also tried the hint slot itself last, which baseline never does.
// i = hint + 1; BL = 1 (first lap)
// i >= 0x1000 -> i = 0 (the bitset range checks at 06d45b76 /
// cannot fire)
// bit i free: BL ? take it : fail (06d45bc5)
//             bit i set:  BL ? next : fail (06d45bac)
// next: ++i; i >= 0x1000 -> i = 0 WITHOUT the hint test (06d45bb7
//             -> 06d45b74); else i == hint -> BL = 0 (06d45bc1), so arriving
//             back at the hint fails whatever its bit says
// take: set bit i, hint = i, *Out = i (the `JL` at 06d45c01 on a
//             value below 0x1000 cannot fire)
// fail: *Out = 0xffffffff
// ---------------------------------------------------------------------------
void Server_VibeID_Alloc(unsigned int* OutId)
{
    unsigned int Index = g_VibeBitmapHint + 1;
    bool bFirstLap = true;
    if (Index >= 0x1000)
        Index = 0;
    for (;;) {
        unsigned int const Mask = 1u << (Index & 0x1f);
        bool const bTaken = (g_VibeBitmap[Index >> 5] & Mask) != 0;
        if (!bFirstLap)
            break;
        if (!bTaken) {
            g_VibeBitmap[Index >> 5] |= Mask;
            g_VibeBitmapHint = Index;
            *OutId = Index;
            return;
        }
        ++Index;
        if (Index >= 0x1000)
            Index = 0;
        else if (Index == g_VibeBitmapHint)
            bFirstLap = false;
    }
    *OutId = 0xFFFFFFFFu;
}

// ---------------------------------------------------------------------------
// Server_VibeID_Free -- baseline the routine (module storage .. module storage),
// cdecl (unsigned int const* Id), plain RET. Only caller pub::Reputation::Free
// (module storage), which pushes the `int const&` it received -- the address,
// not the value (module storage PUSH ESI, ESI = [ESP+param_1]).
// i = *Id
// i >= 0x1000 -> the routine, std::bitset's _Xran: throws
//             std::out_of_range("invalid bitset<N> position") and never returns
// bit i cleared (reset(i))
// The earlier port took the id by value and silently skipped an out-of-range
// id; baseline throws.
// ---------------------------------------------------------------------------
static void Server_VibeID_Free(unsigned int const* Id)
{
    unsigned int const Index = *Id;
    if (Index >= 0x1000)
        throw std::out_of_range("invalid bitset<N> position");
    g_VibeBitmap[Index >> 5] &= ~(1u << (Index & 0x1f));
}

namespace pub {

// ---------------------------------------------------------------------------
// Server_NotifyRepChange -- baseline the routine (module storage .. module storage),
// cdecl (int const* Vibe, unsigned int SessionId), plain RET. Callers
// pub::Reputation::Alloc (module storage), SetAffiliation (module storage),
// SetReputation (module storage), SetAttitude (module storage) and the
// group-pair SetReputation (module storage). Both branches send ONE IClient
// call, +0x108 (SendServerSetReputation, ch2 op 0x29 FLPACKET_SERVER_SETREPUTATION), with an
// FServerSetReputationInfo (rpc_iclient_args.h) built on the stack. The old
// port posted an invented controller message 0x40 instead.

// SinglePlayer() (module storage):
// /06d5f168  the routine (FmtStr ctor) on +0x18 and +0x30
// +0x10 = 0, byte +0x14 = 0, word +0x48 = 0
// +0x00 = *Vibe
// +0x10 = operator new(Reputation::group_count() * 8)
// Reputation::Vibe::Get(+0x00, +0x08, +0x0c, +0x14,
//                            +0x10, +0x18, +0x30, &Name) with Name = 0
// Name ? wcscpy(+0x48, Name) : word +0x48 = 0
// PlayerDB::traverse_active: SendServerSetReputation([p+0x348], &Info)
// operator delete(+0x10) (the routine)
//   then ~FmtStr on +0x30 and +0x18. +0x04 is never written.
// MP (module storage):
//   SessionId != 0, SessionId-1 < the field, Players[SessionId].+0x348 != 0,
//   else return (module storage / 06d5f2c1 / 06d5f2da -- no other check);
// +0x18 zeroed inline (the FmtStr ctor's stores),
//                            FmtStr ctor on +0x30, +0x10 = 0, byte +0x14 = 0,
//                            word +0x48 = 0
// /06d5f363  +0x00 = *Vibe, a temp = Players[SessionId].+0x318
// Reputation::Vibe::AttitudeTowards(+0x00, &temp, +0x04)
// SendServerSetReputation(SessionId, &Info) -- the session id itself,
//                            not the slot's +0x348
//   then ~FmtStr on +0x30 and +0x18.
//   +0x08 / +0x0c are left as stack residue in the MP branch (rpclocal's sender
//   serializes +0x08); the port zeroes them -- no baseline value exists to copy.
// ---------------------------------------------------------------------------
static void Server_NotifyRepChange(int const* vibe_handle, unsigned int session_id)
{
    if (SinglePlayer()) {
        FServerSetReputationInfo Info;
        Info.Attitude = 0.0f;               // never written by baseline (stack residue)
        Info.Field_0x08 = 0;
        Info.Field_0x0c = 0;
        Info.Relations = nullptr;
        Info.RelationCount = 0;
        Info.Pad_0x15[0] = 0;
        Info.Pad_0x15[1] = 0;
        Info.Pad_0x15[2] = 0;
        Info.DisplayName[0] = L'\0';
        Info.VibeHandle = *vibe_handle;
        Info.Relations = static_cast< ::Reputation::Relation*>(
            ::operator new(GlobalReputation::group_count() * 8));
        wchar_t const* VibeName = nullptr;
        GlobalReputation::Vibe::Get(Info.VibeHandle, Info.Field_0x08, Info.Field_0x0c,
                                    Info.RelationCount, Info.Relations, Info.NameA,
                                    Info.NameB, VibeName);
        if (VibeName != nullptr) {
            wcscpy(Info.DisplayName, VibeName);                         // , unbounded
        } else {
            Info.DisplayName[0] = L'\0';
        }
        for (PlayerData* Active = Players.traverse_active(nullptr); Active != nullptr;
             Active = Players.traverse_active(Active)) {
            g_pClientInterface->SendServerSetReputation(Active->client_id, Info);
        }
        ::operator delete(Info.Relations);
        return;
    }

    if (session_id == 0 || (session_id - 1) >= PlayerDB_GetMaxPlayers()) return;
    if (Players[session_id].client_id == 0) return;

    FServerSetReputationInfo Info;
    Info.Field_0x08 = 0;                    // stack residue in baseline
    Info.Field_0x0c = 0;
    Info.Relations = nullptr;
    Info.RelationCount = 0;
    Info.Pad_0x15[0] = 0;
    Info.Pad_0x15[1] = 0;
    Info.Pad_0x15[2] = 0;
    Info.DisplayName[0] = L'\0';
    Info.VibeHandle = *vibe_handle;
    int const PlayerVibe = Players[session_id].rep_vibe;
    Info.Attitude = 0.0f;
    GlobalReputation::Vibe::AttitudeTowards(Info.VibeHandle, PlayerVibe,
                                            Info.Attitude);
    g_pClientInterface->SendServerSetReputation(session_id, Info);
}

namespace Reputation {

// pubGlobalReputation::Alloc — baseline 0x7f0f0
// → Vibe::Alloc → Vibe::SetName → the routine
SERVER_API int __cdecl Alloc(int& vibe_handle, const FmtStr& name_fmtstr, const FmtStr& nickname_fmtstr) {
    // Line 89886: the routine(param_1) � allocate vibe ID from bitmap
    unsigned int vibe_id = (unsigned int)vibe_handle;
    Server_VibeID_Alloc(&vibe_id);
    vibe_handle = (int)vibe_id;
    // Vibe::Alloc(param_1); a non-zero result skips SetName and the notify.
    int result = GlobalReputation::Vibe::Alloc(vibe_handle);
    if (result == 0) {
        // Line 89889: Vibe::SetName(param_1, param_2, param_3, nullptr)
        GlobalReputation::Vibe::SetName(vibe_handle, name_fmtstr, nickname_fmtstr, nullptr);
        // Line 89890: the routine(param_1, 0) � notify SP players
        int vibe_tmp = vibe_handle;
        Server_NotifyRepChange(&vibe_tmp, 0);
    }
    return result;
}

// pubGlobalReputation::Free — baseline 0x7f3e0
// Vibe::Free → the routine
SERVER_API int __cdecl Free(const int& vibe_handle) {
    // is thirteen instructions with NO guard at all: it calls
    // Vibe::Free, keeps the result in EDI across the routine and returns it.
    // The port's `if (vibe_handle == 0) return -1;` was invented.
    // Line 90036: Vibe::Free(param_1)
    int result = GlobalReputation::Vibe::Free(vibe_handle);
    // PUSH ESI / module storage the routine: the same reference,
    // passed on as a pointer -- clear the bitmap bit (throws when >= 0x1000).
    Server_VibeID_Free(reinterpret_cast<unsigned int const*>(&vibe_handle));
    return result;
}

// pubGlobalReputation::GetAffiliation — baseline 0x7f400
// Vibe::GetAffiliation(handle, out, true)
SERVER_API int __cdecl GetAffiliation(const int& vibe_handle, unsigned int& out_affiliation) {
    // Line 90051: Vibe::GetAffiliation(param_1, param_2, true)
    int result = GlobalReputation::Vibe::GetAffiliation(vibe_handle, out_affiliation, true);
    if (result == 0) return 0;
    // Lines 90055-90056: on error, set 0xFFFFFFFF, return -2
    out_affiliation = 0xFFFFFFFF;
    return -2;
}

// pubGlobalReputation::SetAffiliation — baseline 0x7f430
// Vibe::SetAffiliation → the routine
SERVER_API int __cdecl SetAffiliation(const int& vibe_handle, const unsigned int& affiliation) {
    // has no guard: it loads *param_2 at 0x06d5f434 and goes
    // straight into Vibe::SetAffiliation. The two-way zero check was invented.
    // Line 90069: Vibe::SetAffiliation(param_1, *param_2, false)
    int result = GlobalReputation::Vibe::SetAffiliation(vibe_handle, affiliation, false);
    if (result == 0) {
        // Line 90071: the routine(param_1, 0) — notify
        Server_NotifyRepChange(&vibe_handle, 0);
        return 0;
    }
    return -2;
}

// pubGlobalReputation::SetReputation (vibe overload) — baseline 0x7f470
// Vibe::SetGroupFeelingsTowards → the routine
SERVER_API int __cdecl SetReputation(const int& vibe_handle, const unsigned int& group_id, float value) {
    // Line 90087: Vibe::SetGroupFeelingsTowards(param_1, param_2, param_3)
    int result = GlobalReputation::Vibe::SetGroupFeelingsTowards(vibe_handle, group_id, value);
    if (result == 0) {
        // Line 90089: the routine(param_1, 0)
        int vibe_tmp = vibe_handle;
        Server_NotifyRepChange(&vibe_tmp, 0);
        return 0;
    }
    return -2;
}

// pubGlobalReputation::GetReputation (vibe overload) — baseline 0x7f4b0
// Vibe::GetGroupFeelingsTowards
SERVER_API int __cdecl GetReputation(const int& vibe_handle, const unsigned int& group_id, float& out_value) {
    // Line 90105: *param_3 = 0.0
    out_value = 0.0f;
    // Line 90106: Vibe::GetGroupFeelingsTowards(param_1, param_2, param_3)
    int result = GlobalReputation::Vibe::GetGroupFeelingsTowards(vibe_handle, group_id, out_value);
    if (result == 0) return 0;
    return -2;
}

// pubGlobalReputation::GetAttitude — baseline 0x7f4e0
// Vibe::AttitudeTowards with special return code handling
SERVER_API int __cdecl GetAttitude(const int& vibe1, const int& vibe2, float& out_attitude) {
    // Line 90124: *param_3 = 0.0
    out_attitude = 0.0f;
    // Line 90125: Vibe::AttitudeTowards(param_1, param_2, param_3)
    int result = GlobalReputation::Vibe::AttitudeTowards(vibe1, vibe2, out_attitude);
    // Lines 90126-90136: complex return mapping
    if (result == 0) return 0;
    if (result == -1) return -2;
    // result == -2 → return pointer-as-int (baseline quirk: returns param_3 ptr cast)
    // For our port, just return the attitude value as int or -3
    if (result == -2) return -3;
    return result;
}

// pubGlobalReputation::SetAttitude — baseline 0x7f520
// Vibe::AdjustAttitude → SP check → GetClientID → the routine
SERVER_API int __cdecl SetAttitude(const int& vibe1, const int& vibe2, float attitude) {
    // Line 90152: Vibe::AdjustAttitude(param_1, param_2, param_3)
    int result = GlobalReputation::Vibe::AdjustAttitude(vibe1, vibe2, attitude);
    if (result == 0) {
        // Lines 90154-90159: if !SP, get target's ClientID, notify that session
        if (!SinglePlayer()) {
            unsigned int client_id = GlobalReputation::Vibe::GetClientID(vibe2);
            if (client_id != 0) {
                int vibe_tmp = vibe1;
                Server_NotifyRepChange(&vibe_tmp, client_id);
            }
        }
        return 0;
    }
    if (result == -1) return -2;
    // result == -2 → return attitude as int (baseline quirk)
    if (result == -2) return -3;
    return result;
}

// pubGlobalReputation::GetRank — baseline 0x7f590
// Vibe::GetRank
SERVER_API int __cdecl GetRank(const int& vibe_handle, float& out_rank) {
    // has no guard. On success it does FILD on a QWORD whose
    // high dword is the zeroed EAX (0x06d5f5bc / 0x06d5f5c0), i.e. the rank is
    // converted as an UNSIGNED value, which is why the temp below is unsigned.
    // Line 90183: Vibe::GetRank(param_1, (uint*)&param_1) — reuses param_1 as temp
    unsigned rank_val = 0;
    int result = GlobalReputation::Vibe::GetRank(vibe_handle, rank_val);
    if (result == 0) {
        // Line 90185: *param_2 = (float)param_1 (the rank value)
        out_rank = (float)rank_val;
        return 0;
    }
    // Lines 90188-90189: *param_2 = 0.0, return -2
    out_rank = 0.0f;
    return -2;
}

// pubGlobalReputation::SetRank — baseline 0x7f5e0
// SP check → GetClientID → GetRank(old) → ftol → SetRank → notify if changed
SERVER_API int __cdecl SetRank(const int& vibe_handle, float rank) {
    // Lines 90208-90217: if !SP, get ClientID, save old rank
    unsigned int client_id = 0;
    // baseline seeds the old-rank slot with 0xffffffff, not 0 (0x06d5f5ec
    // MOV dword ptr [ESP+0x10],0xffffffff), so a rank of 0 still counts as a
    // change when GetRank below never runs.
    unsigned int old_rank = 0xffffffff;
    if (!SinglePlayer()) {
        client_id = GlobalReputation::Vibe::GetClientID(vibe_handle);
        if ((client_id & 0x3fffffff) != 0) {
            int cid = (int)client_id;
            GlobalReputation::Vibe::GetRank(cid, old_rank);
        }
    }
    // Line 90218: uVar3 = ftol(param_2) — convert float rank to unsigned
    unsigned new_rank = (unsigned)(int)rank;
    // Line 90219: Vibe::SetRank(param_1, uVar3)
    int result = GlobalReputation::Vibe::SetRank(vibe_handle, new_rank);
    if (result != 0) return -2;
    // Lines 90223-90227: if client_id valid && rank changed, notify all active players
    // the 0x3fffffff mask IS real here (TEST EDI,
    // at 0x06d5f60f and again at 0x06d5f648 -- unlike the two places
    // where it was copied in by hand), the rank must actually have changed
    // (0x06d5f650 CMP EBX,[ESP+0x10]), and then it walks PlayerDB::traverse_active
    // calling IClient(the field)->[+0x1d4] with (pd[+0x348], client_id,
    // new_rank). 0x1d4 is a vtable BYTE offset: 0x1d4/4 = slot 117
    // (IClient::SendServerMiscObjUpdateRank), three
    // arguments, matching implementation notes. The port used to fire an invented
    // controller message id 0x41 instead.
    // No interface null test (module storage loads the field and calls).
    if ((client_id & 0x3fffffff) != 0 && new_rank != old_rank) {
        for (PlayerData* Active = Players.traverse_active(nullptr); Active != nullptr;
             Active = Players.traverse_active(Active)) {
            // pd[+0x348], the slot's client id.
            g_pClientInterface->SendServerMiscObjUpdateRank(Active->client_id, client_id,
                                                            static_cast<int>(new_rank));
        }
    }
    return 0;
}

// pubGlobalReputation::GetReputationGroup — baseline 0x7f6a0
// Copy name to TString<16>, call get_id
SERVER_API int __cdecl GetReputationGroup(unsigned int& out_id, const char* name) {
    // Lines 90246-90276: copy name to local TString<16> buffer (max 15 chars + null)
    char local_name[16] = {};
    if (name != nullptr) {
        unsigned int len = 0;
        while (len < 15 && name[len] != '\0') {
            local_name[len] = name[len];
            len++;
        }
        local_name[len] = '\0';
    }
    // Line 90277: get_id(TString<16>). baseline builds the TString in place --
    // local_14 (the length dword at +0) and local_10 (the 16 chars at +4) are
    // adjacent and it passes &local_14 at 0x06d5f6f9. The length is written at
    // for a real name and zeroed at 0x06d5f6b9 for a null one; the
    // port used to memcpy only the characters and leave Length uninitialised.
    TString<16> tname;
    tname.Length = 0;
    while (tname.Length < 15 && local_name[tname.Length] != '\0') {
        ++tname.Length;
    }
    memcpy(tname.Data, local_name, 16);
    unsigned int id = GlobalReputation::get_id(tname);
    if (id != 0xFFFFFFFF) {
        out_id = id;
        return 0;
    }
    out_id = 0xFFFFFFFF;
    return -2;
}

// pubGlobalReputation::GetName — baseline 0x7f730
// Vibe::GetName
SERVER_API int __cdecl GetName(const int& vibe_handle, FmtStr& out_name_fmta, FmtStr& out_name_fmtb) {
    // Line 90296: Vibe::GetName(param_1, param_2, param_3, (ushort**)&param_3)
    wchar_t const* wide_name = nullptr;
    int result = GlobalReputation::Vibe::GetName(vibe_handle, out_name_fmta, out_name_fmtb, wide_name);
    if (result == 0) return 0;
    return -2;
}

// pubGlobalReputation::GetReputation (uint,uint overload) — baseline 0x7f760
// get_feelings_towards with -4 default for invalid IDs
// ?GetReputation@Reputation@pub@@YAHABI0AAM@Z -- an OVERLOAD of GetReputation; the
// port used to call it GetReputationByID, which exported an invented name.
SERVER_API int __cdecl GetReputation(const unsigned int& group_a, const unsigned int& group_b, float& out_value) {
    // Lines 90313-90321: check both != 0xFFFFFFFF
    if (group_a == 0xFFFFFFFF || group_b == 0xFFFFFFFF) return -4;
    int result = GlobalReputation::get_feelings_towards(group_a, group_b, out_value);
    if (result == 0) return 0;
    return -2;
}

// pubGlobalReputation::SetReputation (uint,uint overload) — baseline 0x7f7a0
// get_feelings_towards → affect_relation → the routine
// ?SetReputation@Reputation@pub@@YAHABI0M@Z -- overload, formerly SetReputationByID.
SERVER_API int __cdecl SetReputation(const unsigned int& group_a, const unsigned int& group_b, float value) {
    // Lines 90339-90351: check both != 0xFFFFFFFF
    if (group_a == 0xFFFFFFFF || group_b == 0xFFFFFFFF) return -4;
    float current = 0.0f;
    int result = GlobalReputation::get_feelings_towards(group_a, group_b, current);
    if (result == 0) {
        // Line 90343: affect_relation(group_a, group_b, value - current)
        float delta = value - current;
        GlobalReputation::affect_relation(group_a, group_b, delta);
        // Line 90345: the routine(&group_a_copy, 0)
        int tmp = (int)group_a;
        Server_NotifyRepChange(&tmp, 0);
        return 0;
    }
    return -2;
}

// pubGlobalReputation::GetGroupName — baseline 0x7f810
// get_name(group_id)
SERVER_API int __cdecl GetGroupName(const unsigned int& group_id, unsigned int& out_name_id) {
    // Line 90365: get_name(*param_1)
    unsigned int name_id = GlobalReputation::get_name(group_id);
    out_name_id = name_id;
    // Lines 90367-90371: return 0 if valid, -2 if 0xFFFFFFFF
    if (name_id != 0xFFFFFFFF) return 0;
    return -2;
}

// pubGlobalReputation::GetShortGroupName — baseline 0x7f840
// get_short_name(group_id)
SERVER_API int __cdecl GetShortGroupName(const unsigned int& group_id, unsigned int& out_name_id) {
    // Line 90385: get_short_name(*param_1)
    unsigned int name_id = GlobalReputation::get_short_name(group_id);
    out_name_id = name_id;
    // Lines 90387-90391: return 0 if valid, -2 if 0xFFFFFFFF
    if (name_id != 0xFFFFFFFF) return 0;
    return -2;
}

// pubGlobalReputation::GetGroupFeelingsTowards — baseline 0x7f870
// Vibe::GetGroupFeelingsTowards
SERVER_API int __cdecl GetGroupFeelingsTowards(const int& vibe_handle, const unsigned int& group_id, float& out_feeling) {
    // Line 90405: Vibe::GetGroupFeelingsTowards(param_1, param_2, param_3)
    int result = GlobalReputation::Vibe::GetGroupFeelingsTowards(vibe_handle, group_id, out_feeling);
    // Lines 90406-90410: return 0 unless result == -1 → -2
    if (result != -1) return 0;
    return -2;
}

// pubGlobalReputation::GetReputation (char* overload) — baseline 0x7f8a0
// CreateID → SolarReps::FindSolarRep
// The export is ?GetReputation@Reputation@pub@@YAHAAHPBD@Z -- AAH is `int&`,
// not `int*`, so the out parameter is a reference. baseline never null-checks it:
// it writes through it unconditionally at 0x06d5f8ca.
SERVER_API int __cdecl GetReputation(int& out_rep, const char* name) {
    // Line 90423: CreateID(param_2)
    // baseline keeps the id in a stack ID_String (the param_2 slot, 06d5f8b1) and passes
    // its address (06d5f8ac).
    ID_String Key;
    Key.Id = CreateID(name);
    // Line 90424: SolarReps::FindSolarRep
    int rep = ::SolarReps::FindSolarRep(Key);
    out_rep = rep;
    // Lines 90426-90429: return 0 if found, -2 if -1
    if (rep != -1) return 0;
    return -2;
}

// pubGlobalReputation::GetReputation (ID_String overload) — baseline 0x7f8e0
// SolarReps::FindSolarRep
// ?GetReputation@Reputation@pub@@YAHAAHABUID_String@@@Z -- `int&` plus
// `ID_String const&`, not (int*, unsigned int). baseline pushes param_2 straight
// through to SolarReps::FindSolarRep at 0x06d5f8eb, i.e. as an address, and
// writes the result through the reference at 0x06d5f8fb without a null check.
SERVER_API int __cdecl GetReputation(int& out_rep, const ID_String& id) {
    // Line 90442: SolarReps::FindSolarRep(param_2) -- ID_String const&, as Common exports it.
    int rep = ::SolarReps::FindSolarRep(id);
    out_rep = rep;
    // Lines 90444-90447: return 0 if found, -2 if -1
    if (rep != -1) return 0;
    return -2;
}

// pub::Reputation::EnumerateGroups — baseline 0x7f910 (0x06d5f910), ordinal 120
// Ten instructions: build an 8-byte stack adapter { vtable = 0x06d5f940, the
// Enumerator& } at 0x06d5f91c/0x06d5f924, hand it to COMMON.DLL's
// Reputation::enumerate at 0x06d5f928, then XOR EAX,EAX -- it ALWAYS returns 0.
// The adapter's one virtual, the thunk at 0x06d5f940, dereferences the
// `unsigned const*` Common.dll passes and tail-jumps into the Enumerator's own
// slot 0 with the group id by value.

// The old port walked g_VibeBitmap -- the VIBE allocation bitmap -- and called
// a fabricated `bool(*)(int, void*)` out of an equally fabricated
// {CallbackFn, Context} Enumerator. Reputation groups are not vibes, and the
// real Enumerator is an abstract struct; both are fixed now.
SERVER_API int __cdecl EnumerateGroups(Enumerator& enumerator) {
    struct FRepGroupAdapter : GlobalReputation::RepGroupCB {
        Enumerator* Sink;
        virtual char Execute(unsigned const* GroupId) { return Sink->Execute(*GroupId); }
    };
    FRepGroupAdapter Adapter;
    Adapter.Sink = &enumerator;
    GlobalReputation::enumerate(&Adapter);
    return 0;
}

} // namespace Reputation

// ===========================================================================
// pub::Controller exports
// ===========================================================================
namespace Controller {

SERVER_API unsigned int __cdecl Create(const char* dll_path, const char* instance_name,
                            const CreateParms* create_parms, PRIORITY priority) {
    return ::Server_ControllerCreate(dll_path, instance_name, (void*)create_parms, (unsigned int)priority);
}

SERVER_API void __cdecl Destroy(unsigned int flid) {
    ::Server_ControllerDestroy(flid);
}

// pub::Controller::SetHeartbeatInterval — baseline 0x64030 (0x06d44030). Read
// instruction by instruction: the routine(&Flid) finds the controller node, and the
// whole body is then
//     node[+0x10] = Interval;
//     node[+0x14] = (float)rand() * (1/32768) * Interval;   // FMUL the field = 0x38000000
//     return 0;            // -2 when the node is not found
// There is NO pub::Controller message: the 0x3d the port used to send here was invented
// (0x3d is a real id, but it is fired from the routine / the routine, not from here).
SERVER_API int __cdecl SetHeartbeatInterval(const unsigned int& Flid, float Interval) {
    return Server_ControllerSetHeartbeat(Flid, Interval);
}

// pub::Controller::_SendMessage — baseline 0x64080 (0x06d44080). The Flid==0 case is not
// an error: it is the BROADCAST arm, and it is the one the routine (every bare
// "send message N" in Server.dll) uses. The previous port returned -2 for it, which
// silently swallowed every one of those sends.
SERVER_API int __cdecl _SendMessage(const unsigned int& Flid, EMessage MsgId, const void* MsgData) {
    return Server_ControllerDispatch(Flid, MsgId, MsgData);
}

} // namespace Controller

// pub::SetTimer — baseline 0x7ee00 (0x06d5ee00). baseline checks ONLY `Timeout < 0` (-4);
// it never validates the Flid. The body then allocates a 0x18-byte node
// {next, prev, Flid, Timer.Param1, Timer.Param2, Timeout}, links it into the timer list
// at the field and bumps the field. No message is sent at insert time: message
// 0x3e is what the tick (0x06d5df60) sends when the timer EXPIRES, and it sends it to
// that one controller, not broadcast. The port's insert-time 0x3e broadcast was wrong
// on both counts.
SERVER_API int __cdecl SetTimer(const unsigned int& Flid, const Controller::TimerExpired& Timer, float Timeout) {
    return Server_ControllerSetTimer(Flid, Timer.Param1, Timer.Param2, Timeout);
}

// ===========================================================================
// pub::Zone exports — line-by-line verified against Server
// ===========================================================================
namespace Zone {

// pub::Zone::VerifyId — baseline 0x1b920
// Universe::get_zone(id) != nullptr
SERVER_API bool __cdecl VerifyId(unsigned int zone_id) {
    // Line 21372: get_zone(param_1)
    Universe::IZone const* zone = Universe::get_zone(zone_id);
    // Line 21373: return pIVar1 != 0
    return zone != nullptr;
}

// pub::Zone::GetId — baseline 0x1b940
// get_zone(sys_id, name) → vtable+4 → NicknameId
SERVER_API unsigned int __cdecl GetId(unsigned int system_id, const char* name) {
    // Line 21387: get_zone(param_1, param_2)
    Universe::IZone const* zone = Universe::get_zone(system_id, name);
    if (zone != nullptr) {
        // Line 21391: vtable+4 → returns NicknameId
        return zone->NicknameId;
    }
    // Line 21394: return 0
    return 0;
}

// pub::Zone::GetSystem — baseline 0x1b970
// get_zone(id) → vtable+8 → SystemId
SERVER_API unsigned int __cdecl GetSystem(unsigned int zone_id) {
    // Line 21408: get_zone(param_1)
    Universe::IZone const* zone = Universe::get_zone(zone_id);
    if (zone != nullptr) {
        // Line 21412: vtable+8 → returns SystemId
        return zone->SystemId;
    }
    // Line 21415: return 0
    return 0;
}

// pub::Zone::GetPopulation — baseline 0x1b990
// get_zone(id) → vtable+0x6c → weighted_vector ptr
// ?GetPopulation@Zone@pub@@YAHIAAPBV?$weighted_vector@I@@@Z -- a REFERENCE to a
// `weighted_vector<unsigned> const*` (AAPBV), not a pointer to one.
SERVER_API int __cdecl GetPopulation(unsigned int zone_id, const weighted_vector<unsigned int>*& out_population) {

    // Line 21429: get_zone(param_1)
    Universe::IZone const* zone = Universe::get_zone(zone_id);
    if (zone != nullptr) {
        // CALL [vtable+0x6c] then `MOV ECX,[ESP+0xc]; MOV [ECX],EAX`,
        // return 0. Slot +0x6c is Common the branch, `LEA EAX,[ECX+0x8c]`: the address
        // of the zone's faction population (Universe::IZone::Population).
        out_population = &zone->Population;
        return 0;
    }
    // Lines 21435-21436: *param_2 = nullptr, return -2
    out_population = nullptr;
    return -2;
}

// pub::Zone::GetPos — baseline 0x1b9d0
// get_zone(id) → vtable+0x10 → copy Vector (3 dwords)
SERVER_API Vector __cdecl GetPos(unsigned int zone_id) {
    // is exactly `get_zone(id)`, then either copy 3 dwords out of
    // vtable+0x10's Vector or zero the hidden return slot. There is NO id-mask guard --
    // an earlier port carried an invented `(zone_id & 0x3fffffff) == 0` early-out that
    // has no counterpart in the listing; removed.
    Vector result = {};
    Universe::IZone const* zone = Universe::get_zone(zone_id);
    if (!zone) return result;
    // baseline: vtable+0x10 returns Vector position
    result = zone->Pos;
    return result;
}

// pub::Zone::GetOrientation — baseline 0x1ba20
// get_zone(id) → vtable+0x14 → copy Matrix (9 dwords loop)
SERVER_API int __cdecl GetOrientation(const unsigned int& zone_id, Matrix& out_matrix) {
    // Line 21477: get_zone(param_1)
    Universe::IZone const* zone = Universe::get_zone(zone_id);
    if (zone != nullptr) {
        // vtable+0x14 (Common 062d1ae0, LEA +0x0c) → copy 9 dwords.
        out_matrix = zone->GetOrientation();
        return 0;
    }
    // Line 21487: return -2
    return -2;
}

// pub::Zone::GetRadius — baseline 0x1ba60 (module storage .. 06cfba7f)
// get_zone(id); null -> FLD [module storage] = 0.0f; else tail-JMP [vtable+0x30]
// (Common the branch = ZONE::GetRadius, the cached per-shape radius).
SERVER_API float __cdecl GetRadius(unsigned int zone_id) {
    Universe::IZone const* zone = Universe::get_zone(zone_id);
    if (zone != nullptr)
        return zone->GetRadius();
    return 0.0f;
}

// pub::Zone::GetSize — baseline 0x1ba80
// get_zone(id) → vtable+0x28 → copy Vector (3 dwords)
SERVER_API int __cdecl GetSize(unsigned int zone_id, Vector& out_size) {
    // Line 21523: get_zone(param_1)
    Universe::IZone const* zone = Universe::get_zone(zone_id);
    if (zone != nullptr) {
        // Lines 21525-21528: vtable+0x28 → copy 3 dwords to *param_2
        out_size = zone->Size;
        return 0;
    }
    // Line 21531: return -2
    return -2;
}

// pub::Zone::GetShape — baseline 0x1bad0
// get_zone(id) → vtable+0x18 → ZoneShape enum
SERVER_API int __cdecl GetShape(unsigned int zone_id, ZoneShape& out_shape) {
    // Line 21545: get_zone(param_1)
    Universe::IZone const* zone = Universe::get_zone(zone_id);
    if (zone != nullptr) {
        // Line 21547: vtable+0x18 → ZoneShape
        out_shape = (ZoneShape)zone->Shape;
        return 0;
    }
    // Lines 21551-21552: *param_2 = 0, return -2
    out_shape = (ZoneShape)0;
    return -2;
}

// pub::Zone::ClassifyPoint — baseline 0x1bb10
// get_zone(id) → vtable+0x48(point) → float
SERVER_API float __cdecl ClassifyPoint(unsigned int zone_id, const Vector& point) {
    // Line 21566: get_zone(param_1)
    Universe::IZone const* zone = Universe::get_zone(zone_id);
    if (zone != nullptr) {
        // PUSH point; CALL [vtable+0x48] (Common the branch,
        // ZONE::ClassifyPoint); the result is returned as it comes.
        return zone->ClassifyPoint(point);
    }
    // Line 21571: return 1.0 (outside)
    return 1.0f;
}

// pub::Zone::InZone — baseline 0x1bb40
// get_zone(id) → vtable+0x34(point, float) → bool
SERVER_API bool __cdecl InZone(unsigned int zone_id, const Vector& point, float margin) {
    // Line 21585: get_zone(param_1)
    Universe::IZone const* zone = Universe::get_zone(zone_id);
    if (zone != nullptr) {
        // PUSH margin; PUSH point; CALL [vtable+0x34] (Common
        // , ZONE::InZone).
        return zone->InZone(point, margin);
    }
    // Line 21590: return false
    return false;
}

// pub::Zone::GetDistance — baseline 0x1bb70
// get_zone(id) → vtable+0x3c(point) → float
SERVER_API float __cdecl GetDistance(unsigned int zone_id, const Vector& point) {
    // Line 21604: get_zone(param_1)
    Universe::IZone const* zone = Universe::get_zone(zone_id);
    if (zone != nullptr) {
        // PUSH point; CALL [vtable+0x3c] (Common the branch,
        // ZONE::GetDistance) -- a signed distance, negative inside; baseline does not clamp it.
        return zone->GetDistance(point);
    }
    // Line 21609: return 0.0
    return 0.0f;
}

// pub::Zone::Intersect — baseline 0x1bba0
// get_zone(id) → vtable+0x4c(start, end, hit_point) → bool
SERVER_API bool __cdecl Intersect(unsigned int zone_id, const Vector& start, const Vector& end, Vector& hit_point) {
    // Line 21624: get_zone(param_1)
    Universe::IZone const* zone = Universe::get_zone(zone_id);
    if (zone != nullptr) {
        // PUSH hit; PUSH end; PUSH start; CALL [vtable+0x4c]
        // (Common the branch, ZONE::Intersect), then TEST AL / JZ -> 0, else AL = 1.
        return zone->Intersect(start, end, hit_point) ? true : false;
    }
    // Line 21631: return false
    return false;
}

// pub::Zone::GetProperties — baseline 0x1bbe0
// get_zone(id) → vtable+0x1c → ulong flags
SERVER_API int __cdecl GetProperties(unsigned int zone_id, unsigned long& out_props) {
    // has no guard on the out-param at all; an earlier port carried
    // `if (!out_props) return -2;`, which -- out_props being a REFERENCE -- tested the
    // caller's current *value* and bailed out whenever it happened to be 0. Removed.
    // Line 21645: get_zone(param_1)
    Universe::IZone const* zone = Universe::get_zone(zone_id);
    if (zone != nullptr) {
        // Line 21647: vtable+0x1c → PropertyFlags
        out_props = (unsigned long)zone->PropertyFlags;
        return 0;
    }
    // Line 21651: return -2
    return -2;
}

// pub::Zone::GetLootableInfo — baseline 0x1bc10
// get_zone(id) → vtable+0x7c → loot info struct
SERVER_API int __cdecl GetLootableInfo(unsigned int zone_id, ID_String& out_archetype, ID_String& out_loot_table,
                                        int& out_min_level, int& out_max_level, int& field6) {
    // /0x06cfbc28 call COMMON.DLL::ID_String::clear on both string
    // out-params (not memset), then zero the three int out-params.
    out_archetype.clear();
    out_loot_table.clear();
    field6 = 0;
    out_max_level = 0;
    out_min_level = 0;
    // get_zone(param_1)
    Universe::IZone const* zone = Universe::get_zone(zone_id);
    if (zone != nullptr) {
        // CALL [vtable+0x7c] (ZONE::GetLootableInfo, Common module storage,
        // ICF-folded with File::tell, `MOV EAX,[this+0x84]; RET`): the owned loot record the
        // +0x8c setter (ZONE::Slot8C, the routine) installs -- IZone::OwnedLootEntry, a
        // CmnAsteroid::LootableZoneEntry (common_casteroidfield.cpp "[LootableZone]"). When
        // non-null (module storage): out_archetype <- [rec+0x08], out_loot_table <-
        // [rec+0x0c], out_min_level <- [rec+0x20], out_max_level <- [rec+0x10], field6 <-
        // [rec+0x14] (module storage .. 06cfbc82). A null record falls through to the same
        // `return 0`.
        CmnAsteroid::LootableZoneEntry const* const Loot = zone->GetLootableInfo();
        if (Loot != nullptr) {
            out_archetype = Loot->AsteroidLootCommodityId;   // +0x08
            out_loot_table = Loot->DynamicLootCommodityId;   // +0x0c
            out_min_level = Loot->AsteroidLootDifficulty;    // +0x20
            out_max_level = Loot->AsteroidLootCountMin;      // +0x10
            field6 = Loot->AsteroidLootCountMax;             // +0x14
        }
        return 0;
    }
    // Line 21686: return -2
    return -2;
}

// pub::Zone::GetName — baseline 0x1bca0
// get_zone(id) → vtable+0x84 → name id
SERVER_API int __cdecl GetName(unsigned int zone_id, unsigned int& out_name_id) {
    // Line 21700: get_zone(param_1)
    Universe::IZone const* zone = Universe::get_zone(zone_id);
    if (zone != nullptr) {
        // Line 21702: vtable+0x84 → IdsName
        out_name_id = (unsigned int)zone->IdsName;
        return 0;
    }
    // Line 21706: return -2
    return -2;
}

} // namespace Zone

// ===========================================================================
// pub::System exports — line-by-line verified against Server
// ===========================================================================
namespace System {

// ===========================================================================
// — System activation / object spawning
// baseline: StarSys.cpp — activates a star system, loads objects, notifies clients

// When param_1 == 0 (from LoadSystem): only initializes the system if bLoaded == 0
// When param_1 != 0 (from player connect): also sends all objects to that player
// ===========================================================================

// FSystemListNode moved to server_sysdb.h (2026-09-17, wave 8) so that
// StarSystem::remove_player and StarSystem::unload in server_classes.cpp can
// walk the same nodes baseline's module storage / module storage loops do.

// (FSystemObjectEntry and StarSystem_LoadObjects, the port's rewrite of the
// populate half of the routine, were removed 2026-09-24: StarSystem::load now
// calls the baseline functions, StarSystem::load_solars (the routine) and
// StarSystem::load_object (the routine), in src/Server/server_classes.cpp.)

// ---------------------------------------------------------------------------
// pub::System::LoadSystem — baseline module storage, RVA 0x1ace0, ordinal 327,
// ?LoadSystem@System@pub@@YAHABI@Z. Eight instructions, and all of them:
// 83ceff        OR   ESI,0xffffffff      ; the failure value
// CALL the routine                      ; SysDB lookup
// TEST EAX,EAX / JZ  -> module storage `MOV EAX,ESI` = -1
// 6a00          PUSH 0x0
// CALL the routine                      ; add_player(0)
// 33c0          XOR  EAX,EAX             ; 0

// CORRECTED 2026-09-17 (wave 12). The port had three things wrong here, all of
// them checklist class 6/7 (an invented condition, an invented constant, and a
// guard baseline does not have):
//   * there is NO `(system_id & 0x3fffffff)` test and therefore no -4 exit --
//     the mask guard lives inside add_player, not here;
//   * the miss returns -1, not -2;
//   * and, the one that mattered, it wrote `bLoaded = true` instead of calling
//     add_player. That marked the system loaded WITHOUT loading it, so the
//     StarSystem::load call this wave added to add_player would have been
//     skipped for every system LoadSystem had already touched.
// add_player(0) is exactly right: it loads the system and then falls out on its
// own `TEST EBP,0x3fffffff` because the session id is zero.
// ---------------------------------------------------------------------------
SERVER_API int __cdecl LoadSystem(const unsigned int& system_id) {
    FStarSystem* sys = SysDB_LookupSystem(system_id);
    if (!sys) {
        return -1;
    }
    sys->add_player(0);
    return 0;
}

// pub::System::Find -- baseline module storage (RVA 0x1b260, ordinal 124).
// -4 when name is null, name[0] == 0, or
//                             (system_id & 0x3fffffff) == 0
// the routine (SysDB lookup); miss -> -2
// key = CreateID(name)
// StarSystem::find_solar(&key); miss -> -3
// out_id = found->get_id() (IObjInspect +0x20), 0
// out_id is written only on success. (Corrected 2026-09-24: the port returned
// CreateID(name) without looking the solar up, never returned -3, accepted an
// empty name and zeroed out_id up front.)
SERVER_API int __cdecl Find(const unsigned int& system_id, const char* name, unsigned int& out_id) {
    if (name == nullptr || name[0] == '\0') return -4;
    if ((system_id & 0x3fffffff) == 0) return -4;
    FStarSystem* sys = SysDB_LookupSystem(system_id);
    if (!sys) return -2;
    unsigned int Key = CreateID(name);
    GameObject* Solar = sys->find_solar(&Key);
    if (Solar == nullptr) return -3;
    out_id = Solar->get_id();
    return 0;
}

// pub::System::GetName (0x1b8e0)
// baseline: Universe::get_system(id), vtable+0xc → StridName
SERVER_API int __cdecl GetName(unsigned int system_id, unsigned int& out_name_id) {
    out_name_id = 0;
    Universe::ISystem const* sys = Universe::get_system(system_id);
    if (!sys) return -2;
    out_name_id = sys->StridName;
    return 0;
}

// pub::System::GetNestedProperties -- baseline module storage .. module storage (RVA 0x1b880).
// Ported 1:1 2026-09-24 (claude-code:w6-sys):
// sys = Universe::get_system(system_id); null -> -2
// a 16-byte stack sphere {position, radius 0}
// out_props = ISystem slot 9 (vt+0x24)(&sphere); return 0
SERVER_API int __cdecl GetNestedProperties(const unsigned int& system_id, const Vector& position, unsigned long& out_props) {
    Universe::ISystem const* sys = Universe::get_system(system_id);
    if (!sys) return -2;
    Geometry::Sphere Query;
    Query.Center = position;
    Query.Radius = 0.0f;
    out_props = sys->GetNestedProperties(&Query);
    return 0;
}

// pub::System::EnumerateConnections -- baseline module storage .. module storage (RVA 0x1b170).
// Ported 1:1 2026-09-24 (claude-code:w6-sys):
// sys = Universe::get_system(system_id); null -> -2
// count = ISystem slot 13 (vt+0x34); signed JLE 0 -> 0
// per index: ID_String::clear on record +4, slot 14 (vt+0x38)
//                         (index, &record); its (always false) result is not read
// type 0 takes every record, 1 only GateType 1, 2 only GateType 2,
//                         any other type none
// append to the caller's struct at [Count], no bound check
// signed loop; return 0
SERVER_API int __cdecl EnumerateConnections(const unsigned int& system_id, ConnectionEnumerator& enumerator, ConnectionType connection_type) {
    Universe::ISystem const* sys = Universe::get_system(system_id);
    if (!sys) return -2;

    int const Count = sys->GetConnectionCount();
    for (int Index = 0; Index < Count; ++Index) {
        Universe::FSystemConnection Record;
        Record.Object.clear();
        sys->GetConnection(static_cast<unsigned int>(Index), &Record);
        if (connection_type == 1) {
            if (Record.GateType != 1) continue;
        } else if (connection_type == 2) {
            if (Record.GateType != 2) continue;
        } else if (connection_type != 0) {
            continue;
        }
        unsigned int const Slot = enumerator.Count;
        enumerator.GotoSystemId[Slot] = Record.GotoSystemId;
        enumerator.Object[Slot] = Record.Object;
        enumerator.ObjectId[Slot] = Record.ObjectId;
        enumerator.Pos[Slot] = Record.Pos;
        ++enumerator.Count;
    }
    return 0;
}

// pub::System::EnumerateObjects -- baseline module storage .. module storage (RVA 0x1b2e0).
// Ported 1:1 2026-09-24 (claude-code:w6-sys):
// result = -2; sys = Universe::get_system(system_id); an INI_Reader
//                         is constructed and destroyed but never opened; null -> -2
// result = 0; count = ISystem slot 15 (vt+0x3c); signed JLE 0
// per index: clear the four TString fields of the stack record, then
//                         slot 16 (vt+0x40)(index, &record); its result is not read
// build the 0x64-byte FSysObj (two strncpy of 0x20, no terminator
//                         forced), SystemId = *system_id
// enumerator slot 0 (&object); false -> stop
// signed loop; return result
SERVER_API int __cdecl EnumerateObjects(const unsigned int& system_id, SysObjEnumerator& enumerator) {
    int Result = -2;
    Universe::ISystem const* sys = Universe::get_system(system_id);
    INI_Reader Reader;
    if (!sys) return Result;

    Result = 0;
    int const Count = sys->GetObjectCount();
    for (int Index = 0; Index < Count; ++Index) {
        Universe::FSystemObject Record;
        Record.Nickname.Length = 0;
        Record.Nickname.Data[0] = '\0';
        Record.Reputation.Length = 0;
        Record.Reputation.Data[0] = '\0';
        Record.NextRing.Length = 0;
        Record.NextRing.Data[0] = '\0';
        Record.PrevRing.Length = 0;
        Record.PrevRing.Data[0] = '\0';
        sys->GetSystemObject((unsigned int)Index, &Record);

        FSysObj Object;
        strncpy(Object.Nickname, Record.Nickname.Data, 0x20);
        Object.Pos = Record.Pos;
        Object.ArchetypeId = Record.ArchetypeId;
        Object.IdsName = Record.IdsName;
        Object.IdsInfo = Record.IdsInfo;
        strncpy(Object.Reputation, Record.Reputation.Data, 0x20);
        Object.DockWith = Record.DockWith;
        Object.Goto = Record.Goto;
        Object.SystemId = system_id;
        if (!enumerator.Slot00(&Object)) break;
    }
    return Result;
}

// pub::System::EnumerateZones -- baseline module storage .. .text:06cfaf6x (RVA 0x1ad60).
// Ported 1:1 2026-09-24 (claude-code:w6-sys):
// result = -2; sys = Universe::get_system(system_id); INI_Reader
//                         constructed; null -> -2
// Universe::get_filename(path, ISystem slot 5 (vt+0x14) = file);
//                         reader.open(path, false); failure -> -2
// for every entry of ISystem slot 6 (vt+0x18, the zone list):
//                         reader.set_state(entry state); id = zone slot 1 (the zone id);
//                         enumerator slot 0 (&id, &reader)
// result = 0
SERVER_API int __cdecl EnumerateZones(const unsigned int& system_id, ZoneEnumerator& enumerator) {
    int Result = -2;
    Universe::ISystem const* sys = Universe::get_system(system_id);
    INI_Reader Reader;
    if (!sys) return Result;

    char Path[0x104];
    Universe::get_filename(Path, sys->GetFile());
    if (!Reader.open(Path, false)) return Result;

    std::list<Universe::FSystemZone> const* Zones = sys->GetZones();
    std::list<Universe::FSystemZone>::const_iterator It = Zones->begin();
    std::list<Universe::FSystemZone>::const_iterator End = Zones->end();
    for (; It != End; ++It) {
        Reader.set_state(It->State);
        unsigned int ZoneId = It->Zone->GetId();      // zone vt+0x04
        enumerator.Slot00(&ZoneId, &Reader);
    }
    Result = 0;
    return Result;
}

// pub::System::ScanObjects -- baseline module storage .. module storage (ordinal 0x1ad10, cdecl).
// ?ScanObjects@System@pub@@YAHABIQAIIABVVector@@MIAAI@Z -- QAI: the array parameter is
// `unsigned int* const`; MSVC mangles that top-level const.
// Ported 1:1 2026-09-24 (claude-code:w5-server-06cf):
// *out_count = 0
// the routine(*system_id) (SysDB_LookupSystem, no id mask test);
//                         null -> -2
// sys->the routine(buffer, param_3, &position, radius, param_6)
//                         (StarSystem::scan_objects). The third parameter is the buffer
//                         capacity and the sixth the type mask -- the port had the two
//                         names swapped.
// *out_count = the count; return 0
SERVER_API int __cdecl ScanObjects(const unsigned int& system_id, unsigned int* const buffer,
                                    unsigned int max_results, const Vector& position, float radius,
                                    unsigned int type_mask, unsigned int& out_count) {
    out_count = 0;
    StarSystem* const System = SysDB_LookupSystem(system_id);
    if (System == nullptr) return -2;
    out_count = System->scan_objects(buffer, max_results, &position, radius, type_mask);
    return 0;
}

// pub::System::InZones -- Vector overload, baseline module storage .. module storage (RVA 0x1b640).
// ?InZones@System@pub@@YAHIABVVector@@MQAIIAAI@Z -- array parameter `unsigned int* const` (QAI).
// Ported 1:1 2026-09-24 (claude-code:w6-sys):
// out_count = 0; radius < 0.0f (the field) -> -4
// sys = Universe::get_system(system_id); null -> -2
// tree = ISystem slot 7 (vt+0x1c); sphere {position, radius};
//                         tree vt+0x1c (&sphere, &{0x800, 0, items})
// for index < hit count while out_count < max_zones (tested first):
//                         !SphereCull(sphere, zone) -> out_zones[out_count++] = zone id
// return 0
SERVER_API int __cdecl InZones(unsigned int system_id, const Vector& position, float radius,
                                       unsigned int* const out_zones, unsigned int max_zones,
                                       unsigned int& out_count) {
    out_count = 0;
    if (radius < 0.0f) return -4;
    Universe::ISystem const* sys = Universe::get_system(system_id);
    if (!sys) return -2;

    ISpatialPartition* Tree = sys->GetZoneTree();
    Geometry::Sphere Query;
    Query.Center = position;
    Query.Radius = radius;
    FSpatialQueryResult Hits;
    Hits.Capacity = 0x800;
    Hits.Count = 0;
    SpatialFill(Tree, &Query, &Hits);

    for (unsigned int Index = 0; Index < Hits.Count; ++Index) {
        if (out_count >= max_zones) break;
        Universe::IZone const* Zone = static_cast<Universe::IZone const*>(Hits.Items[Index]);
        if (SphereCull(Query, Zone)) continue;
        out_zones[out_count] = Zone->GetId();       // zone vt+0x04
        ++out_count;
    }
    return 0;
}

// pub::System::InZones -- Transform overload, baseline module storage .. module storage
// (RVA 0x1b760). ?InZones@System@pub@@YAHIABVTransform@@MMMQAIIAAI@Z.
// The three floats are the frustum's AngleX / AngleY / Distance (module storage-06cfb7d7
// store them at frustum +0x30 / +0x34 / +0x38); the port used to call them radius_x/y/z.
// Ported 1:1 2026-09-24 (claude-code:w6-sys):
// out_count = 0; sys = Universe::get_system(system_id); null -> -2
//                         (no radius test in this overload)
// tree = ISystem slot 7; frustum {transform (0x30 bytes), angle_x,
//                         angle_y, distance}; tree vt+0x18 (&frustum, &{0x800, 0, items})
// as the Vector overload, with FrustumCull
// return 0
SERVER_API int __cdecl InZones(unsigned int system_id, const Transform& transform,
                                          float angle_x, float angle_y, float distance,
                                          unsigned int* const out_zones, unsigned int max_zones,
                                          unsigned int& out_count) {
    out_count = 0;
    Universe::ISystem const* sys = Universe::get_system(system_id);
    if (!sys) return -2;

    ISpatialPartition* Tree = sys->GetZoneTree();
    Geometry::Frustum Query;
    Query.Xform = transform;
    Query.AngleX = angle_x;
    Query.AngleY = angle_y;
    Query.Distance = distance;
    FSpatialQueryResult Hits;
    Hits.Capacity = 0x800;
    Hits.Count = 0;
    SpatialFillFrustum(Tree, &Query, &Hits);

    for (unsigned int Index = 0; Index < Hits.Count; ++Index) {
        if (out_count >= max_zones) break;
        Universe::IZone const* Zone = static_cast<Universe::IZone const*>(Hits.Items[Index]);
        if (FrustumCull(Query, Zone)) continue;
        out_zones[out_count] = Zone->GetId();       // zone vt+0x04
        ++out_count;
    }
    return 0;
}

} // namespace System

// The pub::pub_GetSystemID / pub_GetSystemGateConnection / pub_GetSystemNickname /
// pub_BuildSystemReader / pub_BuildBaseReader / pub_GetBaseID / pub_GetBaseNickname /
// pub_GetSystemFromBase block that stood here was a second, pointer-taking copy of the
// real exports (pub::GetSystemID etc., declared in common_server.h with baseline's
// reference signatures). Nothing called them and baseline exports none of those names.

// ===========================================================================
// pub::AI exports
// ===========================================================================
namespace AI {

// ---------------------------------------------------------------------------
// OP_RTYPE values. baseline exports no enumerator names for OP_RTYPE; the names in
// common_pub.h are the port's, derived from the return sites below (1 = accepted, which
// the header once called OpRTypeFailed). Every pub::AI function below returns the literal
// baseline value, cited at its instruction, instead of an enumerator:
//   0  SubmitState: personality applied / zone cleared
//   1  SubmitDirective: accepted (sent to the player's client, or set on the AI)
//   2  SubmitDirective: the AI already has a current directive
//   4  bad id (both), and every SubmitState zone / flag / unknown-op exit
//   5  op failed validate(), no object, not a CSimple, or no behavior for this op
// (IBehaviorManager::set_directive_priority in Common adds 1 and 3.)
// ---------------------------------------------------------------------------

// IDirectiveInfo is three dwords no module names (common_iface.h keeps it as bytes).
// SubmitDirective builds it at S+0x10: word 0 = kind (0 cancel, 1 behavior, 2 player
// maneuver), word 1 = BaseOp::Priority, word 2 = the behavior id (-1 until a branch sets
// it). Common's own builders use the same three words (e.g. common_launchaction.cpp).
static IDirectiveInfo Server_MakeDirectiveInfo(unsigned int Kind, unsigned int OpField08,
                                               unsigned int BehaviorId) {
    IDirectiveInfo Dir;
    unsigned int const Words[3] = { Kind, OpField08, BehaviorId };
    std::memcpy(Dir.Data, Words, sizeof(Words));
    return Dir;
}

// pub::AI::SubmitState — baseline 0x7c8d0, module storage..06d5cba5, all 207 instructions
// traced from the PE bytes (frame: SUB ESP,0xa0 + three pushes).

// (Flid & 0x3fffffff) == 0            -> return 4
// the routine(Flid) == null          -> return 4
// bm = GetBehaviorManager(obj)        (no null test: baseline would fault)
// switch (Op->OpType)                 other -> return 4
//     0 SetPersonalityParams  06d5cb57..06d5cba5:
//         bm->set_state_graph(Op+0x0c, false)                (PUSH 0 / PUSH [ESI+0xc])
//         Op+0x14 != -1 -> bm->set_directive_callback(Op+0x14)
//         Op+0x10 != -1 -> bm->set_content_callback(Op+0x10)
//         byte Op+0x18   -> bm->set_personality(Op+0x1c)
//         return 0
//     1 SetZoneBehaviorParams 06d5c957..06d5cb56: builds a PathfindManager::UserZone at
//         S+0x54 and hands it to the PathfindManager at [bm+0xc] (fields: common_pathfind.h).
//     2 SetFlagParams         06d5c916..06d5c956:
//         FlagId 0 -> byte [[bm+0xc] + 0x245] = bValue; FlagId 1 -> byte [[bm+8] + 0x11c0]
//         = bValue; anything else untouched. return 4 on all three.
SERVER_API OP_RTYPE __cdecl SubmitState(unsigned int Flid, BaseOp* Op) {
    if ((Flid & 0x3fffffff) == 0) return static_cast<OP_RTYPE>(4);
    GameObject* Obj = Server_LookupLiveObject(Flid);
    if (Obj == nullptr) return static_cast<OP_RTYPE>(4);
    IBehaviorManager* Behavior = GetBehaviorManager(Obj);

    if (Op->OpType == OpType_SetPersonalityParams) {
        SetPersonalityParams* Params = static_cast<SetPersonalityParams*>(Op);
        Behavior->set_state_graph(Params->StateGraphId, false);
        // / 06d5cb76: the two -1 sentinels ("leave the callback as it is").
        if (Params->DirectiveCallbackObject != (DirectiveCallback*)-1) {
            Behavior->set_directive_callback(Params->DirectiveCallbackObject);
        }
        if (Params->ContentCallbackObject != (ContentCallback*)-1) {
            Behavior->set_content_callback(Params->ContentCallbackObject);
        }
        if (Params->bSetPersonality != 0) {
            Behavior->set_personality(Params->Value);
        }
        return static_cast<OP_RTYPE>(0);
    }

    if (Op->OpType == OpType_SetZoneBehaviorParams) {
        SetZoneBehaviorParams* Params = static_cast<SetZoneBehaviorParams*>(Op);
        // MOV EBP,[EDI+0xc]: IBehaviorManager+0x0c is the PathfindManager*
        // (every call below is a PathfindManager export with ECX = EBP).
        PathfindManager* Pathfind = Behavior->Pathfind;

        // the routine(S+0x6c) + 06d5c969..06d5c97f: UserZone's inline ctor --
        // BoxRotation = identity, BoxCenter = 0 (common_pathfind.h).
        PathfindManager::UserZone Zone;
        // baseline writes only the fields each zone kind uses and submits the rest as stack
        // residue (+0x08/+0x14/+0x54 for a box, +0x08 for kind 2, +0x00 for an unknown
        // kind, +0x04 for an unknown mode). Residue is not reproducible; the port zeroes
        // those fields first.
        Zone.Mode = 0;
        Zone.Invert = 0;
        Zone.TargetPoint.x = 0.0f;
        Zone.TargetPoint.y = 0.0f;
        Zone.TargetPoint.z = 0.0f;
        Zone.Radius = 0.0f;
        Zone.BoxHalfExtent.x = 0.0f;
        Zone.BoxHalfExtent.y = 0.0f;
        Zone.BoxHalfExtent.z = 0.0f;
        Zone.TargetKey = 0;

        // mode.
        if (Params->InvertMode == 0) {
            Zone.Invert = 0;
        } else if (Params->InvertMode == 1) {
            Zone.Invert = 1;                                    // (ECX = 1)
        } else if (Params->InvertMode == 2) {
            Pathfind->clear_user_zone();
            return static_cast<OP_RTYPE>(0);
        }

        // BoxMin = Params+0x24..+0x2c, BoxMax = Params+0x30..+0x38.
        if (Params->Mode == 0) {

            Zone.Mode = 0;
            Zone.TargetPoint.x = Params->TargetPoint.x;
            Zone.TargetPoint.y = Params->TargetPoint.y;
            Zone.TargetPoint.z = Params->TargetPoint.z;
            Zone.Radius = Params->Radius;
        } else if (Params->Mode == 1) {
            // . Center = (max + min) * 0.5 (the field == 0.5f), each
            // component stored to a float before use; extents = |min - center| per axis. The
            // Matrix at +0x18 is re-copied from a second identity built at S+0x24 (REP MOVSD
            // of 12 dwords, 06d5cad4) -- the same values.
            Zone.Mode = 1;
            float const CenterX = (Params->BoxMax.x + Params->BoxMin.x) * 0.5f;
            float const CenterY = (Params->BoxMax.y + Params->BoxMin.y) * 0.5f;
            float const CenterZ = (Params->BoxMax.z + Params->BoxMin.z) * 0.5f;
            Zone.BoxCenter.x = CenterX;
            Zone.BoxCenter.y = CenterY;
            Zone.BoxCenter.z = CenterZ;
            Zone.BoxHalfExtent.x = std::fabs(Params->BoxMin.x - CenterX);
            Zone.BoxHalfExtent.y = std::fabs(Params->BoxMin.y - CenterY);
            Zone.BoxHalfExtent.z = std::fabs(Params->BoxMin.z - CenterZ);
        } else if (Params->Mode == 2) {

            Zone.Mode = 2;
            Zone.Radius = Params->Radius;
            Zone.TargetKey = Params->TargetId;
        }
        // / 06d5cb00 / 06d5cb42: every kind, known or not, is submitted.
        Pathfind->submit_user_zone(Zone);
        return static_cast<OP_RTYPE>(4);
    }

    if (Op->OpType == OpType_SetFlagParams) {
        SetFlagParams* Params = static_cast<SetFlagParams*>(Op);
        if (Params->FlagId == 0) {
            // byte PathfindManager+0x245 (common_pathfind.h Flag245).
            Behavior->Pathfind->Flag245 = Params->bValue;
        } else if (Params->FlagId == 1) {
            // byte SystemManager+0x11c0 == GunGroups (+0x12c) + 0x1094, the
            // "fire control active" byte common_aifirecontrol.cpp tests.
            Behavior->Sys->GunGroups.bEnabled = Params->bValue;
        }
        return static_cast<OP_RTYPE>(4);
    }

    return static_cast<OP_RTYPE>(4);
}

// OpType -> behavior id pairs of SubmitDirective's AI branch, in baseline's test order
// (module storage..06d5d6d2: thirteen independent `CMP [ESI+4],n / JNE` blocks, each
// setting IDirectiveInfo word 0 = 1 and word 2 = the id before set_current_directive).
struct FSubmitDirectiveBehavior {
    int OpType;
    unsigned int BehaviorId;
};
static const FSubmitDirectiveBehavior GSubmitDirectiveBehaviors[] = {
    { OpType_DirectiveGotoOp,              0x02 },
    { OpType_DirectiveFollowOp,            0x11 },
    { OpType_DirectiveTrailOp,             0x03 },   // (EBP == 3)
    { OpType_DirectiveDockOp,              0x07 },
    { OpType_DirectiveDelayOp,             0x14 },
    { OpType_DirectiveIdleOp,              0x06 },
    { OpType_DirectiveLaunchOp,            0x08 },
    { OpType_DirectiveInstantTradelaneOp,  0x09 },
    { OpType_DirectiveGuideOp,             0x0e },
    { OpType_DirectiveFaceOp,              0x0f },
    { OpType_DirectiveEvadeOp,             0x05 },
    { OpType_DirectiveDrasticEvadeOp,      0x12 },
    { OpType_DirectiveTractorBeamOp,       0x10 },
};

// pub::AI::SubmitDirective — baseline 0x7cbb0, module storage..06d5d764, all 702 instructions
// traced from the PE bytes, ESP tracked through every PUSH and every callee-cleaned
// (RET 0xc). Frame: SUB ESP,0xa8 + four pushes; the SETDIRECTIVE struct is
// S+0x24..S+0xb7, the owner player id S+0x20, the IDirectiveInfo S+0x10..S+0x1b, the result
// S+0x1c. Jump tables at module storage (7 dwords) and module storage (19 bytes, indexed by
// OpType - 3) read from the PE.

// (Flid & 0x3fffffff) == 0             -> return 4
// !Op->validate()                      -> return 5    (vtable slot 0)
// the routine(Flid) == null           -> return 5
// obj->cobject() (IObjInspect +0x150) null, not (Class & 3) == 3, or
//             CSimple::ObjectDb (+0xac) null       -> return 5
// owner = CSimple(obj+0x10)->OwnerPlayer (+0xb4)
// the inlined PlayerDB::is_valid_ship_owner(owner, Flid): a live player
//             slot whose ship (+0x3e4) is this object. Then the directive goes to that
//             player's client, not to an AI -- the player branch below.
// otherwise the AI branch.
SERVER_API OP_RTYPE __cdecl SubmitDirective(unsigned int Flid, BaseOp* Op) {
    if ((Flid & 0x3fffffff) == 0) return static_cast<OP_RTYPE>(4);
    if (!Op->validate()) return static_cast<OP_RTYPE>(5);

    GameObject* Obj = Server_LookupLiveObject(Flid);
    if (Obj == nullptr) return static_cast<OP_RTYPE>(5);

    CObject const* Inspected = Obj->cobject();
    if (Inspected == nullptr) return static_cast<OP_RTYPE>(5);
    if ((static_cast<unsigned int>(Inspected->ObjectClass) & 3u) != 3u) return static_cast<OP_RTYPE>(5);
    if (static_cast<CSimple const*>(Inspected)->ObjectDb == nullptr) return static_cast<OP_RTYPE>(5);

    // CBase::Object with the (Class & 3) == 3 guard, no null test on
    // the result (baseline would fault reading +0xb4 of null).
    unsigned int OwnerPlayer = Obj->csimple()->OwnerPlayer;

    if (::Players.is_valid_ship_owner(OwnerPlayer, Flid)) {
        // Player branch, 06d5cca6..06d5d43a. Each of the six supported kinds default-
        // constructs the struct inline (+0x18 = -1, +0x40 = 3, the rest 0; the three
        // (0,0,0) calls are the 3-dword blocks at +0x60/+0x6c/+0x78), fills its
        // own fields, stores the kind at +0x00 and calls IClient slot 100 (+0x190) with
        // (&OwnerPlayer, &Info); every such send returns 1. The kinds other than goto also
        // zero +0x84..+0x90 right before the send (06d5d1dc..06d5d20e, inline for cancel at
        // ) -- already zero here. Any other op returns 5 with no send.
        // Pad_0x09 / Pad_0x91 are left as stack residue by baseline; zero here (rpclocal's
        // marshal zeroes them again).
        FPayloadServerSetDirective Info = {};
        Info.Field_0x18 = -1;
        Info.Field_0x40 = 3;

        if (Op->OpType == OpType_DirectiveCancelOp) {
            // caseD_15, 06d5ccc3..06d5cdd1
            Info.Field_0x00 = 0;
        } else if (Op->OpType == OpType_DirectiveDelayOp) {
            // caseD_c, 06d5cdd6..06d5ceb6
            DirectiveDelayOp* Delay = static_cast<DirectiveDelayOp*>(Op);
            Info.Field_0x0c = Delay->DelaySeconds;
            Info.Field_0x00 = 1;
        } else if (Op->OpType == OpType_DirectiveDockOp) {
            // caseD_d, 06d5cebb..06d5cfc5
            DirectiveDockOp* Dock = static_cast<DirectiveDockOp*>(Op);
            Info.Field_0x10 = Dock->DockObjectId;
            Info.Field_0x14 = Dock->Field_10;
            Info.Field_0x08 = Dock->Field_14;
            Info.Field_0x18 = Dock->DockPortIndex;
            Info.Field_0x1c = static_cast<unsigned int>(Dock->Field_1C);
            Info.Field_0x20 = Dock->Field_20;
            Info.Field_0x24 = Dock->Field_24;
            Info.Field_0x00 = 2;
        } else if (Op->OpType == OpType_DirectiveFollowOp) {
            // caseD_4, 06d5cfca..06d5d0dd
            DirectiveFollowOp* Follow = static_cast<DirectiveFollowOp*>(Op);
            Info.Field_0x28 = Follow->TargetId;
            Info.Field_0x2c = Follow->FollowDistance;
            Info.Field_0x30.x = Follow->Field_14;
            Info.Field_0x30.y = Follow->Field_18;
            Info.Field_0x30.z = Follow->Field_1C;
            Info.Field_0x3c = Follow->Field_20;
            Info.Field_0x00 = 3;
        } else if (Op->OpType == OpType_DirectiveIdleOp) {
            // caseD_14, 06d5d0e2..06d5d1c5
            Info.Field_0x00 = 5;
        } else if (Op->OpType == OpType_DirectiveGotoOp) {
            // caseD_3, 06d5d22e..06d5d40c. The only kind that fills +0x84..+0x90, and the
            // only one that jumps straight to the send at 06d5d413.
            DirectiveGotoOp* Goto = static_cast<DirectiveGotoOp*>(Op);
            Info.Field_0x40 = Goto->Field_0C;
            Info.Field_0x44 = Goto->Destination;
            Info.Field_0x50 = Goto->Field_1C;
            Info.Block_0x54.Field_0x00 = Goto->Field_20;
            Info.Block_0x54.Field_0x04 = Goto->Field_24;
            Info.Block_0x54.Field_0x08 = Goto->Field_28;
            Info.Block_0x60.Field_0x00 = Goto->Field_2C;
            Info.Block_0x60.Field_0x04 = Goto->Field_30;
            Info.Block_0x60.Field_0x08 = Goto->Field_34;
            Info.Block_0x6c.Field_0x00 = Goto->Field_38;
            Info.Block_0x6c.Field_0x04 = Goto->Field_3C;
            Info.Block_0x6c.Field_0x08 = Goto->Field_40;
            Info.Block_0x78.Field_0x00 = Goto->Field_44;
            Info.Block_0x78.Field_0x04 = Goto->Field_48;
            Info.Block_0x78.Field_0x08 = Goto->Field_4C;
            Info.Field_0x84 = Goto->Field_50;
            Info.Field_0x88 = Goto->Field_54;
            Info.Field_0x8c = Goto->Field_58;
            Info.Field_0x8d = Goto->Field_59;
            Info.Field_0x8e = Goto->Field_5A;
            Info.Field_0x8f = Goto->Field_5B;
            Info.Field_0x1c = Goto->Field_5C;
            Info.Field_0x20 = Goto->Field_60;
            Info.Field_0x24 = Goto->Field_64;
            Info.Field_0x90 = Goto->Field_6C;
            Info.Field_0x00 = 4;
        } else {
            // caseD_5, 06d5d723: byte-table entry 6 (Trail, Strafe, Face, Ram, TractorBeam,
            // Evade, DrasticEvade, Launch, InstantTradelane, Formation, Guide, WaitForShip,
            // WaitForPlayerManeuver) and the Set*Params ops (OpType - 3 > 0x12, `JA`).
            return static_cast<OP_RTYPE>(5);
        }

        g_pClientInterface->SendServerSetDirective(OwnerPlayer, Info);
        return static_cast<OP_RTYPE>(1);
    }

    // AI branch, 06d5d43b..06d5d754. GetBehaviorManager is not null-tested (baseline would
    // fault on the first member call).
    IBehaviorManager* Behavior = GetBehaviorManager(Obj);

    if (Op->OpType == OpType_DirectiveWaitForPlayerManeuverOp) {
        // word 0 = 2, word 2 = BehaviorTypes::get_behavior_id(name); -1 -> 5.
        DirectiveWaitForPlayerManeuverOp* Wait = static_cast<DirectiveWaitForPlayerManeuverOp*>(Op);
        int const BehaviorId = BehaviorTypes::get_behavior_id(Wait->BehaviorName);
        if (BehaviorId == -1) return static_cast<OP_RTYPE>(5);
        Behavior->set_current_directive(
            Server_MakeDirectiveInfo(2u, Op->Priority, static_cast<unsigned int>(BehaviorId)), Op);
        return static_cast<OP_RTYPE>(1);
    }

    if (Op->OpType == OpType_DirectiveCancelOp) {
        // word 0 = EBX = 0, word 2 still -1; the callee's result is ignored.
        Behavior->set_current_directive(Server_MakeDirectiveInfo(0u, Op->Priority, 0xffffffffu), Op);
        return static_cast<OP_RTYPE>(1);
    }

    // a current directive whose word 0 is non-zero blocks every other op.
    unsigned int CurrentKind = 0;
    std::memcpy(&CurrentKind, Behavior->get_current_directive()->Data, sizeof(CurrentKind));
    if (CurrentKind != 0) return static_cast<OP_RTYPE>(2);

    unsigned int Result = 5;                                    // [S+0x1c] = 5
    size_t const PairCount = sizeof(GSubmitDirectiveBehaviors) / sizeof(GSubmitDirectiveBehaviors[0]);
    for (size_t Index = 0; Index < PairCount; ++Index) {
        if (Op->OpType != GSubmitDirectiveBehaviors[Index].OpType) continue;
        if (Behavior->set_current_directive(
                Server_MakeDirectiveInfo(1u, Op->Priority, GSubmitDirectiveBehaviors[Index].BehaviorId), Op)) {
            Result = 1;
        }
    }
    return static_cast<OP_RTYPE>(Result);
}

// pub::AI::set_directive_priority — baseline 0x7db70, module storage..06d5dba8, 20 insns.
// No object or no behavior -> 5; otherwise IBehaviorManager::set_directive_priority's own
// result (06d5dba2). No Flid mask test.
SERVER_API OP_RTYPE __cdecl set_directive_priority(unsigned int Flid, DirectivePriority Priority) {
    GameObject* Obj = Server_LookupLiveObject(Flid);
    if (Obj == nullptr) return static_cast<OP_RTYPE>(5);
    IBehaviorManager* Behavior = GetBehaviorManager(Obj);
    if (Behavior == nullptr) return static_cast<OP_RTYPE>(5);
    return Behavior->set_directive_priority(Priority);
}

// pub::AI::get_behavior_id — baseline 0x7dbb0, module storage..06d5dbdd, 17 insns.
// -1 (OR EAX,-1) on no object / no behavior, else IBehaviorManager+0xc0.
SERVER_API int __cdecl get_behavior_id(unsigned int Flid) {
    GameObject* Obj = Server_LookupLiveObject(Flid);
    if (Obj == nullptr) return -1;
    IBehaviorManager* Behavior = GetBehaviorManager(Obj);
    if (Behavior == nullptr) return -1;
    return Behavior->CurrentBehaviorIndex;
}

// pub::AI::get_state_graph_id — baseline 0x7dbe0, module storage..06d5dc14, 22 insns.
// -1 on no object / no behavior / no state graph (IBehaviorManager+0x04), else the graph's
// first dword, IStateGraph::Id.
SERVER_API int __cdecl get_state_graph_id(unsigned int Flid) {
    GameObject* Obj = Server_LookupLiveObject(Flid);
    if (Obj == nullptr) return -1;
    IBehaviorManager* Behavior = GetBehaviorManager(Obj);
    if (Behavior == nullptr) return -1;
    IStateGraph const* Graph = Behavior->StateGraph;
    if (Graph == nullptr) return -1;
    return Graph->Id;
}

// pub::AI::get_personality — baseline 0x7dc20, module storage..06d5dc51, all 19 insns:
//   PUSH [ESP+4] / CALL the routine (live-object lookup) / TEST EAX,EAX / JZ fail
//   PUSH EAX / CALL [COMMON.DLL::GetBehaviorManager] / TEST EAX,EAX / JZ fail
//   PUSH [ESP+8] / MOV ECX,EAX / CALL [COMMON.DLL::IBehaviorManager::get_personality]
//   MOV AL,1 / RET            fail: XOR AL,AL / RET
// No Flid mask test (the port's was invented).
// ?get_personality@AI@pub@@YA_NIAAVPersonality@12@@Z -- Personality is a class.
SERVER_API bool __cdecl get_personality(unsigned int Flid, Personality& OutPersonality) {
    GameObject* Obj = Server_LookupLiveObject(Flid);
    if (Obj == nullptr) return false;
    IBehaviorManager* Behavior = GetBehaviorManager(Obj);
    if (Behavior == nullptr) return false;
    Behavior->get_personality(OutPersonality);
    return true;
}

// pub::AI::enable_maneuver — baseline 0x7dc60, module storage..06d5dc96, 21 insns.
// IBehaviorManager::enable_maneuver's own bool is dropped (06d5dc91 MOV AL,1): true
// whenever the behavior exists, false otherwise.
SERVER_API bool __cdecl enable_maneuver(unsigned int Flid, int ManeuverId, bool bEnable) {
    GameObject* Obj = Server_LookupLiveObject(Flid);
    if (Obj == nullptr) return false;
    IBehaviorManager* Behavior = GetBehaviorManager(Obj);
    if (Behavior == nullptr) return false;
    Behavior->enable_maneuver(ManeuverId, bEnable);
    return true;
}

// pub::AI::enable_all_maneuvers — baseline 0x7dca0, module storage..06d5dcc9, 15 insns.
// Tail-jumps into IBehaviorManager::enable_all_maneuvers (06d5dcc1), so ITS result is
// returned; false when there is no object / behavior.
SERVER_API bool __cdecl enable_all_maneuvers(unsigned int Flid) {
    GameObject* Obj = Server_LookupLiveObject(Flid);
    if (Obj == nullptr) return false;
    IBehaviorManager* Behavior = GetBehaviorManager(Obj);
    if (Behavior == nullptr) return false;
    return Behavior->enable_all_maneuvers();
}

// pub::AI::lock_maneuvers — baseline 0x7dcd0, module storage..06d5dd01, 19 insns.
// Same shape as enable_maneuver: the callee's bool is dropped, true on success.
SERVER_API bool __cdecl lock_maneuvers(unsigned int Flid, bool bLock) {
    GameObject* Obj = Server_LookupLiveObject(Flid);
    if (Obj == nullptr) return false;
    IBehaviorManager* Behavior = GetBehaviorManager(Obj);
    if (Behavior == nullptr) return false;
    Behavior->lock_maneuvers(bLock);
    return true;
}

// pub::AI::get_scan_response — baseline 0x7dd10, module storage..06d5ddbc, 67 insns.
// The object looked up is the SECOND argument (06d5dd13 MOV EDI,[ESP+0x14] after three
// pushes); the first goes to IBehaviorManager::get_scan_response and to the client, the
// third only to the client.
//   no object                        -> 0 (the null EAX itself, 06d5dd24)
//   owner = CSimple(obj+0x10)->OwnerPlayer (no null test on the guarded pointer)
//   inlined is_valid_ship_owner(owner, TargetFlid)
//                                    -> IClient::SendServerScanNotify (+0x1b4, FLPACKET_SERVER_SCANNOTIFY)
//                                       (owner, ScannerFlid, GoodId), return 3
//   no behavior                      -> 0
//   otherwise                        -> IBehaviorManager::get_scan_response(ScannerFlid)
SERVER_API ScanResponse __cdecl get_scan_response(unsigned int ScannerFlid, unsigned int TargetFlid, unsigned int GoodId) {
    GameObject* Obj = Server_LookupLiveObject(TargetFlid);
    if (Obj == nullptr) return static_cast<ScanResponse>(0);
    unsigned int const OwnerPlayer = Obj->csimple()->OwnerPlayer;
    if (::Players.is_valid_ship_owner(OwnerPlayer, TargetFlid)) {
        g_pClientInterface->SendServerScanNotify(OwnerPlayer, ScannerFlid, GoodId);
        return static_cast<ScanResponse>(3);
    }
    IBehaviorManager* Behavior = GetBehaviorManager(Obj);
    if (Behavior == nullptr) return static_cast<ScanResponse>(0);
    return Behavior->get_scan_response(ScannerFlid);
}

// pub::AI::submit_forced_target — baseline 0x7ddc0, module storage..06d5ddfa, 22 insns.
// No message and no Flid mask:
//     obj = the routine(ship_id);                 null -> return 0 (EAX)
//     bm  = COMMON.DLL::GetBehaviorManager(obj);   null -> return 0
//     sys = bm[+0x8] (SystemManager*);             null -> return 0
//     return COMMON.DLL::TargetManager::submit_forced_target(sys + 0x142c, target_id);
// TargetManager (Common 0x062eec10: append to the 8-entry id array at this+0x1c, count at
// this+0x18; 1 appended, 0 full or already present) is the class at SystemManager+0x142C
// (RTTI .?AVTargetManager@@) -- SystemManager::Targeting in common_systemmanager.h.
SERVER_API int __cdecl submit_forced_target(unsigned int ship_id, unsigned int target_id) {
    GameObject* Obj = Server_LookupLiveObject(ship_id);
    if (Obj == nullptr) return 0;
    IBehaviorManager* Behavior = GetBehaviorManager(Obj);
    if (Behavior == nullptr) return 0;
    if (Behavior->Sys == nullptr) return 0;
    return Behavior->Sys->Targeting.submit_forced_target(target_id);
}

// pub::AI::remove_forced_target — baseline 0x7de00, module storage..06d5de3a, 22 insns.
// Identical chain ending in COMMON.DLL::TargetManager::remove_forced_target(sys + 0x142c,
// target_id) (Common 0x062eec50: remove the id and close the gap; 1 removed, 0 not found),
// whose result is returned.
SERVER_API int __cdecl remove_forced_target(unsigned int ship_id, unsigned int target_id) {
    GameObject* Obj = Server_LookupLiveObject(ship_id);
    if (Obj == nullptr) return 0;
    IBehaviorManager* Behavior = GetBehaviorManager(Obj);
    if (Behavior == nullptr) return 0;
    if (Behavior->Sys == nullptr) return 0;
    return Behavior->Sys->Targeting.remove_forced_target(target_id);
}

// pub::AI::set_player_enemy_clamp — baseline 0x7de40, module storage..06d5de87, 28 insns.
// The same chain, each failed step returning false, then
// COMMON.DLL::TargetManager::set_player_enemy_clamp(sys + 0x142c, min_rep, max_rep) (Common
// stores the pair in the field / the field) and `return true`. No message
// (0x41 is fired from 0x06d447fe, never from here).
SERVER_API bool __cdecl set_player_enemy_clamp(unsigned int player_id, int min_rep, int max_rep) {
    GameObject* Obj = Server_LookupLiveObject(player_id);
    if (Obj == nullptr) return false;
    IBehaviorManager* Behavior = GetBehaviorManager(Obj);
    if (Behavior == nullptr) return false;
    if (Behavior->Sys == nullptr) return false;
    Behavior->Sys->Targeting.set_player_enemy_clamp(min_rep, max_rep);
    return true;
}

// pub::AI::update_formation_state — baseline 0x7d7a0, module storage..06d5db33, 297 insns,
// read in full from the PE bytes (SEH frame + SUB ESP,0x54). ?update_formation_state@AI@pub@@
// YA?AW4FORMATION_RTYPE@@IIABVVector@@@Z -- FORMATION_RTYPE is the GLOBAL enum (common_pub.h).
// Argument 1 is the ship that changes formation, argument 2 its NEW leader (may be 0),
// argument 3 its offset. Only two return values exist: -3 when ship 1 does not resolve, else 0.

//   ship = the routine(ShipFlid)                       null -> return -3 (06d5d7d2)
//   ship->get_formation_leader(OldLeader)               (IObjInspect +0xb0)
//   NewLeader = the routine(LeaderFlid)
//   if (!SinglePlayer() && NewLeader == OldLeader) return 0;     (06d5d80d..06d5d81a)
//   if (!SinglePlayer() && OldLeader) (06d5d821..06d5d98f): a stack std::vector<unsigned>;
//       count = OldLeader IObjAffect +0x20; OldLeader +0x14c (array, 0x10); OldLeader's
//       get_id pushed ONLY when count != 0 (06d5d86f); then every follower's id except
//       the ship's own (compared by get_id); then IClient slot 120 (+0x1e0, the field)
//       (owner, &vector) to every follower and then to OldLeader whose cobject() (+0x150)
//       is a CShip ((Class & 0x503) == 0x503) with byte +0x2bd set, owner = CSimple+0xb4;
// frees the buffer. The ship itself is still a follower here, so it
//       is among the receivers; only its id is left out.
//   if (OldLeader && OldLeader id != ship id) OldLeader IObjAffect +0x1c (ship)     (06d5d9b6)
//       (in multiplayer a null OldLeader jumps straight past this, 06d5d834)
//   ship IObjAffect +0x10 (NewLeader), then +0x14 (Offset)                          (06d5d9c2/ce)
//   if (NewLeader && NewLeader != ship) NewLeader IObjAffect +0x18 (ship)            (06d5d9e2)
//   if (!SinglePlayer() && NewLeader) (06d5d9e5..06d5db17): the same broadcast for
//       NewLeader's formation, except that NewLeader's id is pushed unconditionally
//       and no follower is excluded.
//   return 0.
// Ported 1:1 2026-09-24 (claude-code:w5-server-06d4): the two broadcasts were the last
// TODO. baseline reads Followers[i] for i < count with no bound against the 16-entry array;
// so does the port. The old `(LeaderFlid & 0x3fffffff) == 0` test was invented.

// The receiver step baseline inlines four times (06d5d8e7..06d5d92c, 06d5d935..06d5d974,
// , 06d5dacf..06d5db0c).
static void Formation_SendIdsToOwner(IObjRW const* Obj, std::vector<unsigned int> const& Ids) {
    CObject const* const Cobj = Obj->cobject();                    // +0x150
    if (Cobj == nullptr)
        return;
    if ((static_cast<unsigned int>(Cobj->ObjectClass) & 0x503u) != 0x503u)  // +0x4c
        return;
    CShip const* const ShipCobj = static_cast<CShip const*>(Cobj);
    if (ShipCobj->bIsPlayer == 0)                                  // +0x2bd
        return;
    ::g_pClientInterface->SendServerMiscObjUpdateFormation(ShipCobj->OwnerPlayer, Ids);   // +0xb4, IClient +0x1e0
}

SERVER_API FORMATION_RTYPE __cdecl update_formation_state(unsigned int ShipFlid, unsigned int LeaderFlid, const Vector& Offset) {
    GameObject* Ship = Server_LookupLiveObject(ShipFlid);
    if (Ship == nullptr) return FORMATION_RTYPE_NOT_FOUND;       // -3

    // the out-slot is zeroed first, then get_formation_leader (+0xb0).
    IObjRW* OldLeader = nullptr;
    Ship->get_formation_leader(OldLeader);
    IObjRW* const NewLeader = Server_LookupLiveObject(LeaderFlid);
    if (!SinglePlayer() && NewLeader == OldLeader)
        return FORMATION_RTYPE_OK;

    if (!SinglePlayer() && OldLeader != nullptr) {
        std::vector<unsigned int> Ids;                              // ESP+0x14
        int const Count = OldLeader->get_formation_follower_count(); // IObjAffect +0x20
        IObjRW* Followers[0x10];                                    // ESP+0x24
        OldLeader->get_formation_followers(Followers, 0x10);        // +0x14c
        if (Count != 0)
            Ids.push_back(OldLeader->get_id());                     // / 06d5d890
        for (int I = 0; I < Count; ++I) {
            if (Followers[I]->get_id() != Ship->get_id())           // / 06d5d8af
                Ids.push_back(Followers[I]->get_id());              // / 06d5d8d3
        }
        for (int I = 0; I < Count; ++I)
            Formation_SendIdsToOwner(Followers[I], Ids);
        Formation_SendIdsToOwner(OldLeader, Ids);
    }                                                               // the routine

    // OldLeader's IObjAffect +0x1c, remove_formation_follower(ship),
    // unless OldLeader is the ship itself (compared by get_id()).
    if (OldLeader != nullptr && OldLeader->get_id() != Ship->get_id())
        (void)OldLeader->remove_formation_follower(Ship);
    // the ship's own +0x10 set_formation_leader and +0x14
    // set_formation_offset.
    (void)Ship->set_formation_leader(NewLeader);
    (void)Ship->set_formation_offset(Offset);
    // NewLeader's +0x18 add_formation_follower(ship), unless it is the
    // ship (compared by pointer here).
    if (NewLeader != nullptr && NewLeader != Ship)
        (void)NewLeader->add_formation_follower(Ship);

    if (!SinglePlayer() && NewLeader != nullptr) {
        std::vector<unsigned int> Ids;                              // ESP+0x14
        int const Count = NewLeader->get_formation_follower_count(); // IObjAffect +0x20
        IObjRW* Followers[0x10];                                    // ESP+0x24
        NewLeader->get_formation_followers(Followers, 0x10);        // +0x14c
        Ids.push_back(NewLeader->get_id());                         // / 06d5da4d
        for (int I = 0; I < Count; ++I)
            Ids.push_back(Followers[I]->get_id());
        for (int I = 0; I < Count; ++I)
            Formation_SendIdsToOwner(Followers[I], Ids);
        Formation_SendIdsToOwner(NewLeader, Ids);
    }                                                               // the routine
    return FORMATION_RTYPE_OK;
}

// pub::AI::refresh_state_graph — baseline 0x7db40, module storage..06d5db67, 14 insns.
// Lookup, GetBehaviorManager, then a tail JMP into IBehaviorManager::refresh_state_graph
// (06d5db61); either failed step just returns.
SERVER_API void __cdecl refresh_state_graph(unsigned int Flid) {
    GameObject* Obj = Server_LookupLiveObject(Flid);
    if (Obj == nullptr) return;
    IBehaviorManager* Behavior = GetBehaviorManager(Obj);
    if (Behavior == nullptr) return;
    Behavior->refresh_state_graph();
}

} // namespace AI

// pub::Phantom exports -- the record, the global list and the event pump are above
// namespace pub::SpaceObj (Server_PumpPhantomEvents).
namespace Phantom {

// pub::Phantom::Create (box) -- baseline module storage .. 06cfbfce.
// pump latch -> -5
// any of size.x/y/z not > 0 (FCOMP 0.0f, TEST AH,0x41; NaN
//                         included) -> -4
// (system_id & 0x3fffffff) == 0 -> -4
// the record (new 0x28, both lists empty)
// PhantomParms {system_id, +4 = 1, +5 = 0, name[0] = 0,
//                         Position = pos, Orientation = orient, Listener = record};
//                         record->TypeMask = type_mask, record->SystemId = system_id
// PhySys::CreatePhantom(parms, size, record->PhyHandle); its
//                         result is not tested
// out = record; push_back onto the global list; return 0
// PhantomParms bytes baseline leaves unwritten (+0x07 .. +0x0f) are zeroed here
// (deviations.md, zeroed indeterminate stack bytes).
SERVER_API int __cdecl Create(unsigned int SystemId, const Vector& Size, const Vector& Pos,
                              const Matrix& Orient, unsigned int TypeMask, void*& OutHandle) {
    if (GbPhantomPumpBusy) return -5;
    if (!(Size.x > 0.0f)) return -4;
    if (!(Size.y > 0.0f)) return -4;
    if (!(Size.z > 0.0f)) return -4;
    if ((SystemId & 0x3fffffffu) == 0) return -4;

    ServerPhantom* const Ph = new ServerPhantom();
    PhySys::PhantomParms Parms;
    memset(&Parms, 0, sizeof(Parms));
    Parms.SystemId = SystemId;
    Parms.Flag04 = 1;
    Parms.Flag05 = 0;
    Parms.Name[0] = 0;
    Parms.Position = Pos;
    Parms.Orientation = Orient;                                          // (9 dwords)
    Parms.Listener = Ph;
    Ph->TypeMask = TypeMask;
    Ph->SystemId = SystemId;
    PhySys::CreatePhantom(Parms, Size, Ph->PhyHandle);
    OutHandle = Ph;
    GPhantoms.push_back(Ph);
    return 0;
}

// pub::Phantom::Create (sphere) -- baseline module storage .. 06cfc1db.
// pump latch -> -5
// radius not > 0 (NaN included) -> -4
// (system_id & 0x3fffffff) == 0 -> -4
// the record
// PhantomParms {system_id, 1, 0, name[0] = 0, Position = center,
//                         Orientation = identity, Listener = record}; TypeMask, SystemId
// PhySys::CreatePhantom(parms, radius, record->PhyHandle)
// out = record; push_back; return 0
SERVER_API int __cdecl Create(unsigned int SystemId, float Radius, const Vector& Center,
                              unsigned int TypeMask, void*& OutHandle) {
    if (GbPhantomPumpBusy) return -5;
    if (!(Radius > 0.0f)) return -4;
    if ((SystemId & 0x3fffffffu) == 0) return -4;

    ServerPhantom* const Ph = new ServerPhantom();
    PhySys::PhantomParms Parms;
    memset(&Parms, 0, sizeof(Parms));
    Parms.SystemId = SystemId;
    Parms.Flag04 = 1;
    Parms.Flag05 = 0;
    Parms.Name[0] = 0;
    Parms.Position = Center;
    Parms.Orientation = Matrix(1.0f, 0.0f, 0.0f,
                               0.0f, 1.0f, 0.0f,
                               0.0f, 0.0f, 1.0f);
    Parms.Listener = Ph;
    Ph->TypeMask = TypeMask;
    Ph->SystemId = SystemId;
    PhySys::CreatePhantom(Parms, Radius, Ph->PhyHandle);
    OutHandle = Ph;
    GPhantoms.push_back(Ph);
    return 0;
}

// pub::Phantom::Destroy -- baseline module storage .. 06cfbd53.
//   latch -> -5; handle not on the global list -> -2;
//   PhySys::DestroyPhantom(record->PhyHandle) (06cfbd0c); the record's destructor
//   (the routine) and operator delete (06cfbd1e / 06cfbd24); the list node is unlinked
//   and freed, the count decremented (06cfbd2c .. 06cfbd4a); return 0.
SERVER_API int __cdecl Destroy(void* Handle) {
    if (GbPhantomPumpBusy) return -5;
    std::list<ServerPhantom*>::iterator It = FindPhantom(Handle);
    if (It == GPhantoms.end()) return -2;                                // / 06cfbd03
    PhySys::DestroyPhantom((*It)->PhyHandle);
    delete *It;                                                          // / 06cfbd24
    GPhantoms.erase(It);
    return 0;
}

// pub::Phantom::SetActive -- baseline module storage .. 06cfc01c.
//   latch -> -5; not found -> -2;
//   PhySys::SetCollisionStatePhantom(record->PhyHandle, active) (06cfc011); return 0.
SERVER_API int __cdecl SetActive(void* Handle, bool bActive) {
    if (GbPhantomPumpBusy) return -5;
    std::list<ServerPhantom*>::iterator It = FindPhantom(Handle);
    if (It == GPhantoms.end()) return -2;
    PhySys::SetCollisionStatePhantom((*It)->PhyHandle, bActive);
    return 0;
}

// pub::Phantom::Attach -- baseline module storage .. 06cfc26e.
//   latch -> -5; the routine(*obj_id) null -> -2; handle not found -> -3;
//   the object's CBase::Object when (class & 1) == 1, else null (06cfc22f .. 06cfc24b);
//   PhySys::AttachPhantom(cobject, record->PhyHandle) non-zero -> -1, else 0.
SERVER_API int __cdecl Attach(const unsigned int& ObjId, void* Handle) {
    if (GbPhantomPumpBusy) return -5;
    GameObject* const Obj = Server_LookupLiveObject(ObjId);
    if (Obj == nullptr) return -2;
    std::list<ServerPhantom*>::iterator It = FindPhantom(Handle);
    if (It == GPhantoms.end()) return -3;
    CObject* Raw = Obj->Object;
    if (Raw != nullptr && (static_cast<unsigned int>(Raw->ObjectClass) & 1u) != 1u)
        Raw = nullptr;                                                   // / 06cfc24b
    if (PhySys::AttachPhantom(Raw, (*It)->PhyHandle) != 0) return -1;
    return 0;
}

// pub::Phantom::Detach -- baseline module storage .. 06cfc2c0.
//   latch -> -5; not found -> -2; PhySys::DetachPhantom(record->PhyHandle) non-zero ->
//   -1, else 0.
SERVER_API int __cdecl Detach(void* Handle) {
    if (GbPhantomPumpBusy) return -5;
    std::list<ServerPhantom*>::iterator It = FindPhantom(Handle);
    if (It == GPhantoms.end()) return -2;
    if (PhySys::DetachPhantom((*It)->PhyHandle) != 0) return -1;
    return 0;
}

} // namespace Phantom

// ===========================================================================
// pub::Audio exports
// ===========================================================================
namespace Audio {

// pub::Audio::SetMusic — baseline 0x7eec0
// baseline 0x7eec0 (0x06d5eec0): no validation at all, just one IClient call and
// `return 0`. 0x1e8 / 4 = slot 122; the seven arguments are the player id followed by
// the Tryptich unpacked field by field (+0x00, +0x04, +0x08, +0x0c, the BYTE at +0x10
// zero-extended, and +0x14). The id 0x44 the port used is not a message id at all.
SERVER_API int __cdecl SetMusic(unsigned int PlayerId, const Tryptich& Track) {
    if (!g_pClientInterface) return 0;
    // Field14 holds the fade's float bits (baseline pushes the dword; the sender FLDs it).
    float Fade = 0.0f;
    memcpy(&Fade, &Track.Field14, sizeof(Fade));
    g_pClientInterface->SendServerSetMusic(PlayerId, Track.Field0, Track.Field4, Track.Field8,
                                           Track.FieldC, Track.Field10 != 0, Fade);
    return 0;
}

// pub::Audio::CancelMusic — baseline 0x7ef00
// baseline 0x7ef00 (0x06d5ef00): IClient::SendServerCancelMusic(PlayerId) (+0x1ec / 4 = 123),
// return 0.
SERVER_API int __cdecl CancelMusic(unsigned int PlayerId) {
    if (!g_pClientInterface) return 0;
    g_pClientInterface->SendServerCancelMusic(PlayerId);
    return 0;
}

// pub::Audio::PlaySoundEffect — baseline 0x7ef20
// baseline 0x7ef20 (0x06d5ef20): IClient::SendServerPlaySoundEffect(PlayerId, SoundId)
// (+0x1f0 / 4 = 124), return 0.
SERVER_API int __cdecl PlaySoundEffect(unsigned int PlayerId, unsigned int SoundId) {
    if (!g_pClientInterface) return 0;
    g_pClientInterface->SendServerPlaySoundEffect(PlayerId, SoundId);
    return 0;
}

} // namespace Audio

// ===========================================================================
// pub::Market exports — line-by-line verified against Server 0x385c0 - 0x387d0
// ===========================================================================
namespace Market {

// ---------------------------------------------------------------------------
// Rewritten 2026-09-17 against the raw listing, not the .c. Every one of these
// eight exports funnels through the same three calls --
// ()             the base database singleton
// (this, BaseId) the FBaseEntry lookup
//   FUN_06d12xxx(this, ...)    a helper whose `this` carries the BaseData* at
//                              this+0x04 and the market map at BaseData+0x10
//                              (`MOV ESI,[this+4]` / `ADD ESI,0x10`, e.g.
// / module storage)
// -- and every one of them returns -4 on failure, never -1: the constant is
// `MOV reg,0xfffffffc` at module storage, 06d1861b, 06d1866a, 06d18696,
// , 06d1871f, 06d1875a, 06d1879f and 06d187da. The port returned -1
// from six of the eight.

// Which of them pre-clear their out-parameter before the bad-id check is also
// per-function in baseline, and the port had guessed uniformly. baseline clears in
// IsGoodInDemand (module storage), GetNumCommoditiesForSale (module storage)
// and GetNumCommoditiesInDemand (module storage), and in nothing else.
// ---------------------------------------------------------------------------

// pub::Market::GetMinInventory - baseline 0x385c0 (module storage).
// Body is the routine: map::find(GoodId) over BaseData+0x10, and on a hit
// return the mapped value's +0x08 (module storage MOV EAX,[EAX+0x8], EAX being
// node+0x10). On a miss return 0 (module storage XOR EAX,EAX). That +0x08 slot
// is what names MarketGoodInfo::MinInventory.

// The port used to return MarketGoodInfo::Quantity, which was the struct's
// fifth dword -- baseline's TransactionType -- so this reported a transaction
// code where a stock level was asked for.
SERVER_API int __cdecl GetMinInventory(unsigned int BaseId, unsigned int GoodId, int& OutInv) {
    if ((BaseId & 0x3fffffff) == 0 || (GoodId & 0x3fffffff) == 0) return -4;
    // + the routine (BaseDB::find_base; a miss FDUMPs "Base %u is
    // invalid", line 0x659, and returns 0 -> -4); the helper reads [entry+4].
    FBaseEntry* Entry = GetBaseDB()->find_base(BaseId);
    if (!Entry) return -4;
    BaseData* base = Entry->Data;
    std::map<unsigned int, MarketGoodInfo>::const_iterator it = base->Market.find(GoodId);
    OutInv = (it != base->Market.end()) ? it->second.MinInventory : 0;
    return 0;
}

// pub::Market::GetPrice - baseline 0x38610 (module storage).
// Body is the routine: map::find(GoodId), and on a hit
// `module storage  FLD [EAX+0x4]` where EAX = node+0x10 -- the STORED price.
// On a miss it falls back to GoodList::find_by_id(GoodId)->Price
// (module storage / module storage FLD [EAX+0x58]).

// Nothing here recomputes anything. Common.dll computes the price once, at
// load time, and truncates it through ftol (module storage); see
// MarketGoodInfo in common_goods.h. The port multiplied nominal by a
// PriceMult field on every call and never truncated, so it disagreed with
// baseline by a fraction on every good with a multiplier -- and PriceMult was
// not even the field it was reading, since the struct was rotated by a slot.
SERVER_API int __cdecl GetPrice(unsigned int BaseId, unsigned int GoodId, float& OutPrice) {
    if ((BaseId & 0x3fffffff) == 0 || (GoodId & 0x3fffffff) == 0) return -4;
    // + the routine (BaseDB::find_base; a miss FDUMPs "Base %u is
    // invalid", line 0x659, and returns 0 -> -4); the helper reads [entry+4].
    FBaseEntry* Entry = GetBaseDB()->find_base(BaseId);
    if (!Entry) return -4;
    BaseData* base = Entry->Data;
    std::map<unsigned int, MarketGoodInfo>::const_iterator it = base->Market.find(GoodId);
    if (it != base->Market.end()) {
        OutPrice = it->second.Price;
        return 0;
    }
    // .. module storage -- the miss falls back to
    // GoodList::find_by_id(GoodId) and reads +0x58 with no null test.
    OutPrice = GoodList::find_by_id(GoodId)->Price;
    return 0;
}

// pub::Market::GetNominalPrice - baseline 0x38660 (module storage).
// GoodInfo+0x58 straight out of GoodList::find_by_id (module storage), -4 on
// a bad id or a miss. No pre-clear of the out-parameter (module storage is the
// only write).
SERVER_API int __cdecl GetNominalPrice(unsigned int GoodId, float& OutPrice) {
    if ((GoodId & 0x3fffffff) == 0) return -4;
    GoodInfo const* good = GoodList::find_by_id(GoodId);
    if (!good) return -4;
    OutPrice = good->Price;
    return 0;
}

// pub::Market::GetGoodJumpDist - baseline 0x38690 (module storage).
// GoodInfo+0x6c (module storage). Note baseline does NOT screen the good id here
// the way its neighbours do: it calls find_by_id unconditionally
// (module storage) and only checks the result. Failure is -4
// (module storage MOV ESI,0xfffffffc), not the -1 the port returned.
SERVER_API int __cdecl GetGoodJumpDist(unsigned int GoodId, unsigned int& OutDist) {
    GoodInfo const* good = GoodList::find_by_id(GoodId);
    if (!good) return -4;
    OutDist = (unsigned int)good->JumpDist;
    return 0;
}

// pub::Market::IsGoodInDemand - baseline 0x386c0 (module storage).
// Clears the bool first (module storage MOV byte ptr [ESI],0x0), then
// map::find(GoodId) and
// MOV  EAX,[EAX+0x10]   ; mapped value +0x10 = TransactionType
// TEST EAX,EAX / JZ true
// CMP  EAX,0x1 / JNZ false
// so the answer is `Type == 0 || Type == 1`. The port tested `RepMax >= 0`,
// which was the struct's fifth dword read under a name from a different slot
// and a comparison baseline does not make anywhere.
SERVER_API int __cdecl IsGoodInDemand(unsigned int BaseId, unsigned int GoodId, bool& OutDemand) {
    OutDemand = false;
    if ((BaseId & 0x3fffffff) == 0 || (GoodId & 0x3fffffff) == 0) return -4;
    // + the routine (BaseDB::find_base; a miss FDUMPs "Base %u is
    // invalid", line 0x659, and returns 0 -> -4); the helper reads [entry+4].
    FBaseEntry* Entry = GetBaseDB()->find_base(BaseId);
    if (!Entry) return -4;
    BaseData* base = Entry->Data;
    std::map<unsigned int, MarketGoodInfo>::const_iterator it = base->Market.find(GoodId);
    if (it != base->Market.end()) {
        OutDemand = (it->second.Type == TransactionBuyAndSell ||
                     it->second.Type == TransactionDemandOnly);
    }
    return 0;
}

// The two commodity filters, both read off baseline's node walk rather than a
// std::map iterator, so the offsets there are node-relative:
//   node+0x10 = mapped+0x00 = GoodId  (module storage / module storage)
//   node+0x20 = mapped+0x10 = Type    (module storage / module storage)
// baseline feeds GoodList::find_by_id the id out of the VALUE, not the map key,
// and requires GoodInfo+0x4c == 0 (module storage / module storage), the
// commodity category.
static bool MarketGoodIsCommodity(MarketGoodInfo const& Entry) {
    GoodInfo const* info = GoodList::find_by_id(Entry.GoodId);
    return info != nullptr && info->Category == (unsigned int)GoodCategoryCommodity;
}

// for sale: module storage TEST EAX,EAX / JZ accept / module storage CMP EAX,0x2
static bool MarketGoodForSale(MarketGoodInfo const& Entry) {
    return MarketGoodIsCommodity(Entry) &&
           (Entry.Type == TransactionBuyAndSell || Entry.Type == TransactionSaleOnly);
}

// in demand: module storage TEST EAX,EAX / JZ accept / module storage CMP EAX,0x1
static bool MarketGoodInDemand(MarketGoodInfo const& Entry) {
    return MarketGoodIsCommodity(Entry) &&
           (Entry.Type == TransactionBuyAndSell || Entry.Type == TransactionDemandOnly);
}

// pub::Market::GetNumCommoditiesForSale - baseline 0x38710, helper the routine.
SERVER_API int __cdecl GetNumCommoditiesForSale(unsigned int BaseId, int* OutCount) {
    *OutCount = 0;   // , before the id test and with no pointer test
    if ((BaseId & 0x3fffffff) == 0) return -4;
    // + the routine (BaseDB::find_base; a miss FDUMPs "Base %u is
    // invalid", line 0x659, and returns 0 -> -4); the helper reads [entry+4].
    FBaseEntry* Entry = GetBaseDB()->find_base(BaseId);
    if (!Entry) return -4;
    BaseData* base = Entry->Data;
    int count = 0;
    for (std::map<unsigned int, MarketGoodInfo>::const_iterator it = base->Market.begin();
         it != base->Market.end(); ++it) {
        if (MarketGoodForSale(it->second))
            count++;
    }
    *OutCount = count;
    return 0;
}

// pub::Market::GetCommoditiesForSale - baseline 0x38750, helper the routine.
// The helper reads the cap out of *InOutCount once, up front
// (module storage MOV EAX,[EDX]), tests it with `CMP EBX,cap / JGE skip`
// (module storage) so an over-cap good is skipped but the walk continues, and
// writes the final count back at module storage. No pre-clear on the failure
// paths.
SERVER_API int __cdecl GetCommoditiesForSale(unsigned int BaseId, unsigned int* const OutGoods, int* InOutCount) {
    // No pointer tests: module storage/06d18776 hand both straight to the routine.
    if ((BaseId & 0x3fffffff) == 0) return -4;
    // + the routine (BaseDB::find_base; a miss FDUMPs "Base %u is
    // invalid", line 0x659, and returns 0 -> -4); the helper reads [entry+4].
    FBaseEntry* Entry = GetBaseDB()->find_base(BaseId);
    if (!Entry) return -4;
    BaseData* base = Entry->Data;
    int const max_count = *InOutCount;
    int count = 0;
    for (std::map<unsigned int, MarketGoodInfo>::const_iterator it = base->Market.begin();
         it != base->Market.end(); ++it) {
        if (!MarketGoodForSale(it->second))
            continue;
        if (count >= max_count)
            continue;
        OutGoods[count++] = it->second.GoodId;
    }
    *InOutCount = count;
    return 0;
}

// pub::Market::GetNumCommoditiesInDemand - baseline 0x38790, helper the routine.
SERVER_API int __cdecl GetNumCommoditiesInDemand(unsigned int BaseId, int* OutCount) {
    *OutCount = 0;   // , before the id test and with no pointer test
    if ((BaseId & 0x3fffffff) == 0) return -4;
    // + the routine (BaseDB::find_base; a miss FDUMPs "Base %u is
    // invalid", line 0x659, and returns 0 -> -4); the helper reads [entry+4].
    FBaseEntry* Entry = GetBaseDB()->find_base(BaseId);
    if (!Entry) return -4;
    BaseData* base = Entry->Data;
    int count = 0;
    for (std::map<unsigned int, MarketGoodInfo>::const_iterator it = base->Market.begin();
         it != base->Market.end(); ++it) {
        if (MarketGoodInDemand(it->second))
            count++;
    }
    *OutCount = count;
    return 0;
}

// pub::Market::GetCommoditiesInDemand - baseline 0x387d0, helper the routine.
SERVER_API int __cdecl GetCommoditiesInDemand(unsigned int BaseId, unsigned int* const OutGoods, int* InOutCount) {
    // No pointer tests: module storage/06d187f6 hand both straight to the routine.
    if ((BaseId & 0x3fffffff) == 0) return -4;
    // + the routine (BaseDB::find_base; a miss FDUMPs "Base %u is
    // invalid", line 0x659, and returns 0 -> -4); the helper reads [entry+4].
    FBaseEntry* Entry = GetBaseDB()->find_base(BaseId);
    if (!Entry) return -4;
    BaseData* base = Entry->Data;
    int const max_count = *InOutCount;
    int count = 0;
    for (std::map<unsigned int, MarketGoodInfo>::const_iterator it = base->Market.begin();
         it != base->Market.end(); ++it) {
        if (!MarketGoodInDemand(it->second))
            continue;
        if (count >= max_count)
            continue;
        OutGoods[count++] = it->second.GoodId;
    }
    *InOutCount = count;
    return 0;
}

} // namespace Market

// ===========================================================================
// pub:: utility batch 2 — line-by-line verified against Server
// ===========================================================================

// pub::FindHardpoint — baseline RVA 0x7E270, slot 130
// ?FindHardpoint@pub@@YAHPBDIAAVVector@@AAVMatrix@@@Z
// Flow: GetShip → Archetype::Load (mesh) → HardpointPositionA (lookup) → release handle.
// Returns: 0 = found, -2 = load failed, -3 = hardpoint not found.
// , traced 2026-09-24 (claude-code:w5-server-06d4):
// GetShip's result is dereferenced straight away (06d5e27b MOV EAX,[EAX+0xc]) --
// baseline has no null test, so none here. The engine index is written into
// param_2's own stack slot (06d5e280 LEA ECX,[ESP+0x10]), so its initial value
// is the ship archetype id, not -1.
SERVER_API int __cdecl FindHardpoint(char const* Name, unsigned int ShipArchId,
                                     Vector& OutPos, Matrix& OutRot)
{
    Archetype::Ship* ShipArch = Archetype::GetShip(ShipArchId);
    char const* DaPath = ShipArch->DaArchetype.Value;
    long EngineIndex = static_cast<long>(ShipArchId);
    bool bLoaded = Archetype::Load(DaPath, &EngineIndex, nullptr);
    if (!bLoaded)
        return -2;

    bool bFound = HardpointPositionA(EngineIndex, Name, &OutPos, &OutRot);
    // IEngine::ReleaseHandle (slot 15, +0x3C) straight on
    // NEURONRUNTIME.DLL::NeuronRuntime::Engine -- no Common.dll wrapper, no null check.
    NeuronRuntime::Engine->ReleaseHandle(reinterpret_cast<void*>(EngineIndex));
    return bFound ? 0 : -3;
}

// pub::GetShipID — baseline RVA 0x7E2E0, slot 271
// ?GetShipID@pub@@YAHAAIPBD@Z
// Flow: validate name → CreateID → Archetype::GetShipID (returns large ID).
// Return code: 0 = found (non-zero ID), -2 = not found, -4 = null/empty name.
SERVER_API int __cdecl GetShipID(unsigned int& OutShipId, char const* ShipName)
{
    if (ShipName != nullptr && ShipName[0] != '\0')
    {
        unsigned int NicknameId = CreateID(ShipName);
        ID_String Name;
        Name.Id = NicknameId;
        unsigned int ShipId = Archetype::GetShipID(Name);
        OutShipId = ShipId;
        if ((ShipId & 0x3fffffff) != 0)
            return 0;
        return -2;
    }
    return -4;
}

// pub::TranslateGoodToMsgIdPrefix — baseline RVA 0x7E050, slot 466
// ?TranslateGoodToMsgIdPrefix@pub@@YAHIAAU?$TString@$0EA@@@@Z
// GoodList::find_by_id, then CommReferrable slot 1 (GetMsgIdPrefix) on the
// GoodInfo, and TString<64>::operator=(char const*) inlined on its Data.
// Returns: 0 = success (including empty prefix), -4 = good not found.
SERVER_API int __cdecl TranslateGoodToMsgIdPrefix(unsigned int GoodId,
                                                   TString<64>& OutPrefix)
{
    GoodInfo const* Info = GoodList::find_by_id(GoodId);
    if (Info == nullptr)
        return -4;

    OutPrefix = Info->GetMsgIdPrefix().Data;
    return 0;
}

// pub::TranslateShipToMsgIdPrefix — baseline RVA 0x7E0D0, slot 467
// ?TranslateShipToMsgIdPrefix@pub@@YAHIAAU?$TString@$0EA@@@@Z
// Archetype::GetShip, then slot 1 (GetMsgIdPrefix) of the CommReferrable
// subobject at +0x94, and TString<64>::operator=(char const*) inlined on its Data.
// Returns: 0 = success (including empty prefix), -4 = ship not found.
SERVER_API int __cdecl TranslateShipToMsgIdPrefix(unsigned int ShipArchId,
                                                   TString<64>& OutPrefix)
{
    Archetype::Ship* ShipArch = Archetype::GetShip(ShipArchId);
    if (ShipArch == nullptr)
        return -4;

    OutPrefix = ShipArch->GetMsgIdPrefix().Data;
    return 0;
}

} // namespace pub

