// Author: Giperion.

// server_savegame.cpp -- Server.dll's character-file family: the parser that
// reads a saved character into the slot's record container, the login-scan and
// post-save wrappers around it, the three save wrappers that end in the
// serializer, and the record -> live-slot copy CharacterSelect runs.

// REWRITTEN 2026-09-24 (claude:srv-save). Every function here is a port of one
// baseline function, named in its header comment, and read off
// Server (string operands off the bytes of EXE\Server.dll,
// where the listing truncates them). What used to be here was a hand-rolled
// `key = value` scanner that wrote straight onto the LIVE slot; baseline never
// does that. baseline parses into the 0x398-byte record that lives in the map at
// PlayerData+0x50 (PlayerData::FSavedCharacter), keyed by the file name, and
// CharacterSelect copies that record onto the slot later.

// Still open (not accepted, see each function's own notes):
//  * 's MP-only front section (module storage .. module storage:
//    events 0x3a / 8 / 7, the CHARACTER_ID compare, the IClient +0x1bc/+0x1c0
//    and reputation broadcast loops) is only approximated by
//    IServerImpl::CharacterSelect; its tail (module storage .. module storage)
//    is PlayerData_LoadSelectedCharacter below.

#include "../include/common_pub.h"
#include "../include/common_server.h"          // Players (PlayerDB::operator[])
#include "../include/common_server_playerdb.h"
#include "../include/server_savegame.h"
#include "../include/server_savefile.h"        // FLS1, account paths, last saved path
#include "../include/server_playerdb.h"        // PlayerDB::init, load_user_data (oracle export)
#include "../include/server_characterset.h"    // CCharacterSet (oracle export)
#include "../include/common_net.h"             // IsMPServer
#include "../include/common_universe.h"
#include "../include/common_archetype.h"
#include "../include/common_cobject.h"         // SubObjectID::EquipIdMaker
#include "../include/common_goods.h"           // Loadout::ReadEquipLine / ReadCargoLine
#include "../include/common_ini.h"             // INI_Reader, StringAlloc, RefreshLoadingProgress
#include "../include/common_misc.h"            // HexToWide, safe_strcpy
#include "../include/common_id.h"              // CreateID, MakeLocationID, ID_String
#include "../include/common_csys.h"            // EulerMatrix
#include "../include/common_reputation.h"      // Reputation::*, CostumeDescriptions
#include "../include/common_text.h"            // FmtStr
#include "../include/common_newchar.h"         // NewChar::TheDB
#include "../include/common_server_types.h"    // SCreateCharacterInfo
#include "../include/neuroncomponents.h"                  // FDUMP
#include "../include/rpc_iclient.h"            // IClient slots 91 / 92 / 97 / 98
#include "../include/msvcrt_compat.h"          // MsvcrtAtoi (baseline MSVCRT.DLL atoi wraps)

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#include <ctime>
#include <list>
#include <map>
#include <string>
#include <utility>
#include <vector>

// IServerImpl.cpp: the rpclocal client interface (the field) and the single-player
// [Time] / feelings load (the routine). The controller broadcast (the routine,
// Server_ControllerDispatch) is declared in common_server_controller.h.
extern IClient* g_pClientInterface;
void Server_LoadSinglePlayerState(INI_Reader& Reader);

namespace {

// the __FILE__ every FDUMP in this family passes.
char const* const PlayerDbSource = "Server\\PlayerDB.cpp";

// ---------------------------------------------------------------------------
// (module storage .. module storage), cdecl (Dst, Size, Src).
// A null Src returns 0 and leaves Dst alone; otherwise the length is clamped to
// Size - 1 (`CMP EAX,ECX / JC / LEA EAX,[ECX-1]`), copied, NUL-terminated at
// Dst[Len] and returned. Every caller stores the result as a TString length.
// ---------------------------------------------------------------------------
unsigned int CopyBoundedString(char* Dst, unsigned int Size, char const* Src) {
    if (Src == nullptr)
        return 0;
    unsigned int Length = static_cast<unsigned int>(strlen(Src));
    if (!(Length < Size))
        Length = Size - 1;
    memcpy(Dst, Src, Length);
    Dst[Length] = '\0';
    return Length;
}

// FSaveFileBuffer (a SaveGame with a closing destructor) and SaveGame's
// methods -- the routine is SaveGame::ReadTail, the routine SaveGame::Encode
// -- are declared in server_savefile.h and defined in server_savefile.cpp;
// PlayerDB::ReadCharacterName uses them too.

} // namespace

namespace {

// The {Costume*, CostumeDescriptions const*} pair the routine keeps for each of
// its two costumes (module storage .. module storage: [ESP+0x1c]/[ESP+0x20] for
// record+0x2d4, [ESP+0x24]/[ESP+0x28] for record+0x32c).
struct FCostumeTarget {
    Costume* Target;
    CostumeDescriptions const* Descriptions;
};

// ---------------------------------------------------------------------------
// (thiscall on FCostumeTarget, `RET 0xc`): (TypeId, Name, File).
// find_bodypart(TypeId, Name) (module storage); a miss OR a part whose Id is 0
// both warn (module storage / module storage) and return 0.
// ---------------------------------------------------------------------------
unsigned int CostumeTarget_FindPartId(FCostumeTarget const& Holder, int TypeId,
                                      char const* Name, char const* File) {
    unsigned int Id = 0;
    bodypart const* Part = Holder.Descriptions->find_bodypart(TypeId, Name);
    if (Part != nullptr)
        Id = Part->Id;
    if (Id == 0) {
        FDUMP(0x100002, "%s(%d) : *** WARNING: invalid costume part (%s) in (%s)\n",
              PlayerDbSource, 0x25a, Name, File);
    }
    return Id;
}

// ---------------------------------------------------------------------------
// (thiscall on FCostumeTarget, `RET 8`): (Name, File). Same shape
// as the routine over find_accessory(Name) (module storage), line 0x263.
// ---------------------------------------------------------------------------
unsigned int CostumeTarget_FindAccessoryId(FCostumeTarget const& Holder, char const* Name,
                                           char const* File) {
    unsigned int Id = 0;
    accessory const* Accessory = Holder.Descriptions->find_accessory(Name);
    if (Accessory != nullptr)
        Id = Accessory->Id;
    if (Id == 0) {
        FDUMP(0x100002, "%s(%d) : *** WARNING: invalid accessory (%s) in character (%s)\n",
              PlayerDbSource, 0x263, Name, File);
    }
    return Id;
}

// ---------------------------------------------------------------------------
// (thiscall on Costume, `RET 4`):
//   MOV EAX,[ECX+0x30] / MOV [ECX+EAX*4+0x10],EDX / INC [ECX+0x30]
// No bound and no zero test: an id of 0 (the miss above) is appended too.
// ---------------------------------------------------------------------------
void Costume_AddAccessory(Costume& Target, unsigned int AccessoryId) {
    Target.AccessoryIds[Target.AccessoryCount] = AccessoryId;
    ++Target.AccessoryCount;
}

// ---------------------------------------------------------------------------
// (`RET 0xc`: Out, Reader, Index). One `log` line back into a
// FmtStr -- the inverse of the writer the routine ("%u, %u" then
// ", %u, %u, %u" per Val, recursing for tag 0x21):
// MadLibId = get_value_int((*Index)++)
// Count    = get_value_int((*Index)++)
// begin_mad_lib(MadLibId) / 06d4cb0f end_mad_lib()
//   loop Count times (06d4cb1b signed JLE):
//     Data  = get_value_int((*Index)++)       06d4cb2f
//     Tag   = get_value_int((*Index)++)       06d4cb3f
//     Flags = (byte)get_value_int((*Index)++) 06d4cb4f
//     if ((short)Tag == 0x21) Data = new FmtStr(<recursive read>)   06d4cb51
//     append(new Val(Tag, Data, Flags))       06d4cb8f .. 06d4cbca
//   *Out = copy of the local                  06d4cbe5
// ---------------------------------------------------------------------------
FmtStr ReadMissionLogEntry(INI_Reader& Reader, unsigned int& Index) {
    unsigned int const MadLibId = static_cast<unsigned int>(Reader.get_value_int(Index++));
    int const Count = Reader.get_value_int(Index++);

    FmtStr Local;
    Local.begin_mad_lib(MadLibId);
    Local.end_mad_lib();

    for (int I = 0; I < Count; ++I) {
        int const Data = Reader.get_value_int(Index++);
        int const Tag = Reader.get_value_int(Index++);
        unsigned char const Flags = static_cast<unsigned char>(Reader.get_value_int(Index++));
        void* DataPointer = reinterpret_cast<void*>(static_cast<intptr_t>(Data));
        if (static_cast<short>(Tag) == 0x21) {
            // operator new(0x18) then this same function into it (module storage /
            // ). The Val below owns it (FmtStr::Val::~Val deletes a
            // tag-0x21 subtree).
            DataPointer = new FmtStr(ReadMissionLogEntry(Reader, Index));
        }
        FmtStr::Val* Node = new FmtStr::Val(static_cast<unsigned short>(Tag), DataPointer, Flags);
        Local.append(Node);
    }
    return FmtStr(Local);
}

// ---------------------------------------------------------------------------
// The Reputation::RepGroupCB the routine builds on its frame for `group` /
// `initial_rep` (module storage .. module storage): vptr the branch,
// +0x04 the record, +0x08 the player's own group id.

// Execute is the branch (thiscall, `RET 4`):
// get_feelings_towards(*GroupId, this->GroupId, &f)
// FCOMP double [06d64f00] = 1.0 ; TEST AH,0x1 ; JNZ keep
// f = 0x3f733333 (0.95f)          -- i.e. if (!(f < 1.0)) f = 0.95f
// record->Relations.insert(end, 1, {*GroupId, f})
//   returns 1
// ---------------------------------------------------------------------------
struct FRepGroupRelationsCB : public ::Reputation::RepGroupCB {
    PlayerData::FSavedCharacter* Record;
    unsigned int GroupId;

    char Execute(unsigned const* OtherGroupId) override {
        float Feeling = 0.0f;
        ::Reputation::get_feelings_towards(*OtherGroupId, GroupId, Feeling);
        if (!(Feeling < 1.0))
            Feeling = 0.95f;
        ::Reputation::Relation Entry;
        Entry.GroupId = *OtherGroupId;
        Entry.Feeling = Feeling;
        Record->Relations.push_back(Entry);
        return 1;
    }
};

// TString<16> from an INI string the way every rep-group key builds it: a null
// string gives Length 0 / Data[0] 0 (module storage), anything else goes
// through the routine with size 0x10 (module storage).
TString<16> MakeGroupName(char const* Text) {
    TString<16> Name;
    Name.Length = 0;
    Name.Data[0] = '\0';
    if (Text != nullptr)
        Name.Length = CopyBoundedString(Name.Data, 0x10, Text);
    return Name;
}

// The costume block, one call per costume (module storage .. module storage for
// the com_ costume on the suffix, module storage .. module storage for the bare
// costume on the whole name). Every compare is REPE CMPSB with the terminator
// counted (ECX = strlen + 1), i.e. an EXACT, case-SENSITIVE match.
// Returns false when nothing matched, so the key chain continues.
bool LoadCostumeKey(INI_Reader& Reader, FCostumeTarget const& Holder, char const* Suffix,
                    char const* File) {
    if (strcmp(Suffix, "body") == 0) {
        Holder.Target->PartIds[1] =
            CostumeTarget_FindPartId(Holder, 0, Reader.get_value_string(), File);
        return true;
    }
    if (strcmp(Suffix, "head") == 0) {
        Holder.Target->PartIds[0] =
            CostumeTarget_FindPartId(Holder, 1, Reader.get_value_string(), File);
        return true;
    }
    if (strcmp(Suffix, "lefthand") == 0) {
        Holder.Target->PartIds[2] =
            CostumeTarget_FindPartId(Holder, 2, Reader.get_value_string(), File);
        return true;
    }
    if (strcmp(Suffix, "righthand") == 0) {
        Holder.Target->PartIds[3] =
            CostumeTarget_FindPartId(Holder, 3, Reader.get_value_string(), File);
        return true;
    }
    if (strcmp(Suffix, "hair") == 0) {
        FDUMP(0x100002,
              "%s(%d) : *** WARNING: hair is no longer a valid bodypart in character (%s)\n",
              PlayerDbSource, 0x286, File);
        return true;
    }
    if (strcmp(Suffix, "accessory") == 0) {
        unsigned int const Id =
            CostumeTarget_FindAccessoryId(Holder, Reader.get_value_string(), File);
        Costume_AddAccessory(*Holder.Target, Id);
        return true;
    }
    if (strcmp(Suffix, "costume") == 0) {
        Holder.Descriptions->get_costume(Reader.get_value_string(), *Holder.Target);
        return true;
    }
    return false;
}

// `equip` / `base_equip` / `cargo` / `base_cargo` (module storage ..
// ): insert a default EquipDesc at the end of the list, read the
// line into it (result ignored), then stamp it from the RECORD's EquipIdMaker
// (module storage CreateEquipID on record+0x360, module storage set_id).
void LoadEquipKey(INI_Reader& Reader, PlayerData::FSavedCharacter& Record,
                  EquipDescList& List, bool bCargo) {
    List.Items.insert(List.Items.end(), EquipDesc());
    EquipDesc& Item = List.Items.back();
    if (bCargo)
        Loadout::ReadCargoLine(Reader, Item);
    else
        Loadout::ReadEquipLine(Reader, Item);
    Item.set_id(Record.EquipIdMaker.CreateEquipID());
}

// `collision_group` / `base_collision_group` (module storage .. module storage
// and module storage .. module storage).
void LoadCollisionGroupKey(INI_Reader& Reader, PlayerData::FSavedCharacter& Record,
                           std::list<CollisionGroupDesc>& Groups, int LineNotFound,
                           int LineBeforeShip) {
    CacheString Name;
    Name.Value = StringAlloc(Reader.get_value_string(0), false);
    if (Record.ShipArchetype == 0) {
        FDUMP(0x100002, "%s(%d) : *** WARNING: collision_group %s specified BEFORE ship. Skipping.",
              PlayerDbSource, LineBeforeShip, Name.Value);
        return;
    }
    // No null test on GetShip: baseline calls get_group_by_name on whatever it
    // returns (module storage .. module storage).
    Archetype::Ship const* Ship = Archetype::GetShip(Record.ShipArchetype);
    Archetype::CollisionGroup const* Group = Ship->get_group_by_name(Name);
    if (Group == nullptr) {
        FDUMP(0x100002, "%s(%d) : *** WARNING: collision_group(%s) not found on ship(%s). Skipping.",
              PlayerDbSource, LineNotFound, Name.Value, Ship->Nickname.get_string());
        return;
    }
    // the first node whose Id matches gets Health = value 1;
    // no match does nothing.
    for (std::list<CollisionGroupDesc>::iterator It = Groups.begin(); It != Groups.end(); ++It) {
        if (It->Id == Group->Id) {
            It->Health = (float)Reader.get_value_float(1);         // FSTP float
            return;
        }
    }
}

// The account directory name: the routine's "%02x-%08x" hash in MP, the
// narrowed wide account name in SP (wcstombs(buf, slot, 0x104)).
void BuildAccountDirName(char* Out, PlayerData const& Slot) {
    if (IsMPServer()) {
        Server_AccountDirName(Out, Slot.AccountName);
    } else {
        Out[0] = '\0';
        wcstombs(Out, Slot.AccountName, 0x104);
    }
}

// / module storage / module storage -- <accts>\<account dir>.
void BuildAccountDir(char* Out, PlayerData const& Slot) {
    char AccountDirName[0x104];
    BuildAccountDirName(AccountDirName, Slot);
    _makepath(Out, nullptr, Server_GetAccountsRoot(), AccountDirName, nullptr);
}

} // namespace

// ===========================================================================
// PlayerData_LoadSaveRecord -- baseline the routine (module storage ..
// ), thiscall on the PlayerData, (Path, Record), `RET 8`.
// ===========================================================================
bool __cdecl PlayerData_LoadSaveRecord(PlayerData& Slot, char const* Path,
                                       PlayerData::FSavedCharacter& Record) {
    (void)Slot; // only forwarded as the log reader's unused ECX (module storage)

    bool bSawPlayer = false;
    FSaveFileBuffer Buffer;
    INI_Reader Reader;

    // fopen(path, "rt"), then the first four bytes.
    Buffer.File = fopen(Path, "rt");
    if (Buffer.File == nullptr)
        return false;
    char Magic[4] = { 0, 0, 0, 0 };
    fread(Magic, 1, 4, Buffer.File);
    if (memcmp(Magic, Server_FLS1Magic, 4) == 0) {
        // "FLS1": close, reopen "rb" (module storage), read
        // from offset 4 and decode in place (the routine).
        fclose(Buffer.File);
        Buffer.File = fopen(Path, "rb");
        if (Buffer.File == nullptr)
            return false;
        Buffer.ReadTail(4);
        Buffer.Encode();
    } else if (memcmp(Magic, "FLSG", 4) == 0) {
        // module storage = "FLSG": straight to the exit,
        // whose inlined the routine (module storage .. module storage) closes
        // the file -- ~FSaveFileBuffer here.
        return false;
    } else {
        // plain text: the whole file from offset 0.
        Buffer.ReadTail(0);
    }


    Reader.open_memory(Buffer.Contents.c_str(),
                       static_cast<unsigned int>(Buffer.Contents.size()));

    FCostumeTarget ComCostume = { &Record.ComCostume, nullptr };
    FCostumeTarget BaseCostume = { &Record.BaseCostume, nullptr };

    while (Reader.read_header()) {
        RefreshLoadingProgress();
        int CommentDepth = 0;
        if (!Reader.is_header("Player"))
            continue;

        // ---- per-[Player] reset, module storage .. module storage ----------
        Record.Position[0] = 0.0f;
        Record.Position[1] = 0.0f;
        Record.Position[2] = 0.0f;
        for (int Row = 0; Row < 3; ++Row)
            for (int Col = 0; Col < 3; ++Col)
                Record.Orientation[Row][Col] = (Row == Col) ? 1.0f : 0.0f;
        bSawPlayer = true;
        Record.SystemId = 0;
        Record.BaseId = 0;
        Record.LocationId = 0;             // +0x27c (last_base) is NOT reset
        Record.Description = L"";
        Record.DescripStrId = 0;
        Record.Money = 0x7b0;
        Record.Interface = 3;
        ComCostume.Descriptions = GetCostumeDescriptions();
        BaseCostume.Descriptions = GetCostumeDescriptions();
        Record.WeaponGroups.clear();       // erase(0, npos)
        Record.Visits.clear();             // erase(begin, end)

        while (Reader.read_value()) {
            // block comments.
            if (Reader.is_value("/*"))
                ++CommentDepth;
            else if (Reader.is_value("*/"))
                --CommentDepth;
            if (CommentDepth > 0)          // signed JG
                continue;

            if (Reader.is_value("name")) {
                wchar_t Wide[0x400];
                HexToWide(Reader.get_value_string(), Wide);
                Record.Name.assign(Wide, wcslen(Wide));
                continue;
            }
            if (Reader.is_value("description")) {
                wchar_t Wide[0x400];
                HexToWide(Reader.get_value_string(), Wide);
                Record.Description.assign(Wide, wcslen(Wide));
                continue;
            }
            if (Reader.is_value("descrip_strid")) {
                Record.DescripStrId = static_cast<unsigned int>(Reader.get_value_int(0));
                continue;
            }
            if (Reader.is_value("tstamp")) {
                if (Reader.is_value_empty(0) || Reader.is_value_empty(1)) {
                    Record.TimestampHigh = 0;
                    Record.TimestampLow = 0;
                } else {
                    Record.TimestampHigh = static_cast<unsigned int>(Reader.get_value_int(0));
                    Record.TimestampLow = static_cast<unsigned int>(Reader.get_value_int(1));
                }
                continue;
            }
            if (Reader.is_value("ship_archetype")) {
                // MSVCRT.DLL::atoi: wraps mod 2^32, so a hash id above
                // INT_MAX round-trips; the UCRT atoi would clamp it to 0x7fffffff.
                Record.ShipArchetype =
                    static_cast<unsigned int>(MsvcrtAtoi(Reader.get_value_string()));
                if (Record.ShipArchetype == 0) {
                    ID_String Nickname;
                    Nickname.Id = CreateID(Reader.get_value_string());
                    Record.ShipArchetype = Archetype::GetShipID(Nickname);
                }
                // no null test on GetShip.
                Archetype::Ship const* Ship = Archetype::GetShip(Record.ShipArchetype);
                Ship->get_undamaged_collision_group_list(Record.CollisionGroups);
                Ship->get_undamaged_collision_group_list(Record.BaseCollisionGroups);
                continue;
            }
            if (Reader.is_value("system")) {
                Record.SystemId = Universe::get_system_id(Reader.get_value_string());
                if (Record.SystemId == 0) {
                    FDUMP(0x100001,
                          "%s(%d) : *** ERROR: game (%s) has invalid system name (%s) in player file (%s)\n",
                          PlayerDbSource, 0x376, Reader.get_file_name(),
                          Reader.get_value_string(), Path);
                }
                continue;
            }
            if (Reader.is_value("base")) {
                Record.BaseId = Universe::get_base_id(Reader.get_value_string());
                if (Record.BaseId == 0) {
                    FDUMP(0x100001,
                          "%s(%d) : *** ERROR: game (%s) has invalid base name (%s) in player file (%s)\n",
                          PlayerDbSource, 0x37e, Reader.get_file_name(),
                          Reader.get_value_string(), Path);
                }
                continue;
            }
            if (Reader.is_value("last_base")) {
                Record.LastBaseId = Universe::get_base_id(Reader.get_value_string());
                if (Record.LastBaseId == 0) {
                    FDUMP(0x100001,
                          "%s(%d) : *** ERROR: game (%s) has invalid last_base name (%s) in player file (%s)\n",
                          PlayerDbSource, 0x386, Reader.get_file_name(),
                          Reader.get_value_string(), Path);
                }
                continue;
            }
            if (Reader.is_value("location")) {
                Record.LocationId = static_cast<unsigned int>(Reader.get_value_int(0));
                continue;
            }
            if (Reader.is_value("rank")) {
                Record.Rank = static_cast<unsigned int>(Reader.get_value_int(0));
                continue;
            }
            if (Reader.is_value("group") || Reader.is_value("initial_rep")) {
                // get_id, empty the relations (the routine),
                // then enumerate every group through the callback.
                TString<16> const GroupName = MakeGroupName(Reader.get_value_string());
                FRepGroupRelationsCB Callback;
                Callback.Record = &Record;
                Callback.GroupId = ::Reputation::get_id(GroupName);
                Record.Relations.clear();
                ::Reputation::enumerate(&Callback);
                continue;
            }
            if (Reader.is_value("rep_group")) {
                // an empty value resolves "" (module storage).
                TString<16> const GroupName = Reader.is_value_empty(0)
                    ? MakeGroupName("")
                    : MakeGroupName(Reader.get_value_string(0));
                Record.RepGroup = ::Reputation::get_id(GroupName);
                continue;
            }
            if (Reader.is_value("house")) {
                float const Feeling = (float)Reader.get_value_float(0);   // FSTP float
                TString<16> const GroupName = MakeGroupName(Reader.get_value_string(1));
                ::Reputation::Relation Entry;
                Entry.GroupId = ::Reputation::get_id(GroupName);
                Entry.Feeling = Feeling;
                Record.Relations.push_back(Entry);   // insert(end, 1, v)
                continue;
            }
            if (Reader.is_value("voice")) {
                char const* Voice = Reader.get_value_string();
                if (Voice == nullptr) {
                    Record.Voice.Length = 0;
                    Record.Voice.Data[0] = '\0';
                } else {
                    Record.Voice.Length = CopyBoundedString(Record.Voice.Data, 0x20, Voice);
                }
                continue;
            }

            // the costume block, on the raw key name.
            {
                char const* const KeyName = Reader.get_name_ptr();
                char const* const File = Reader.get_file_name();
                bool bHandled = false;
                if (strstr(KeyName, "com_") == KeyName)
                    bHandled = LoadCostumeKey(Reader, ComCostume, KeyName + strlen("com_"), File);
                if (!bHandled) {
                    // name and file are fetched again.
                    char const* const BareName = Reader.get_name_ptr();
                    bHandled = LoadCostumeKey(Reader, BaseCostume, BareName,
                                              Reader.get_file_name());
                }
                if (bHandled)
                    continue;
            }

            // / 06d47de8 / 06d47e12 / 06d47e3c -- MSVCRT ftol on the
            // unrounded ST0 (64-bit truncation, low dword stored).
            if (Reader.is_value("money")) {
                Record.Money = (int)(long long)Reader.get_value_float(0);
            } else if (Reader.is_value("num_kills")) {
                Record.NumKills = (int)(long long)Reader.get_value_float(0);
            } else if (Reader.is_value("num_misn_successes")) {
                Record.NumMissionSuccesses = (int)(long long)Reader.get_value_float(0);
            } else if (Reader.is_value("num_misn_failures")) {
                Record.NumMissionFailures = (int)(long long)Reader.get_value_float(0);
            } else if (Reader.is_value("pos")) {
                Record.Position[0] = (float)Reader.get_value_float(0);   // ..90 FSTP float
                Record.Position[1] = (float)Reader.get_value_float(1);
                Record.Position[2] = (float)Reader.get_value_float(2);
            } else if (Reader.is_value("rotate")) {
                Vector Angles;
                Angles.x = (float)Reader.get_value_float(0);             // ..e1 FSTP float
                Angles.y = (float)Reader.get_value_float(1);
                Angles.z = (float)Reader.get_value_float(2);
                Matrix const Rotation = EulerMatrix(Angles);
                memcpy(Record.Orientation, Rotation.E, sizeof(Record.Orientation)); // REP MOVSD x9
            } else if (Reader.is_value("starting_ring")) {
                Record.StartingRing = CreateID(Reader.get_value_string());
            } else if (Reader.is_value("equip")) {
                LoadEquipKey(Reader, Record, Record.Equipment, false);
            } else if (Reader.is_value("base_equip")) {
                LoadEquipKey(Reader, Record, Record.BaseEquipment, false);
            } else if (Reader.is_value("cargo")) {
                LoadEquipKey(Reader, Record, Record.Equipment, true);
            } else if (Reader.is_value("base_cargo")) {
                LoadEquipKey(Reader, Record, Record.BaseEquipment, true);
            } else if (Reader.is_value("collision_group")) {
                LoadCollisionGroupKey(Reader, Record, Record.CollisionGroups, 0x41f, 0x424);
            } else if (Reader.is_value("base_collision_group")) {
                LoadCollisionGroupKey(Reader, Record, Record.BaseCollisionGroups, 0x444, 0x449);
            } else if (Reader.is_value("wg")) {
                // the whole raw line, then "\n".
                Record.WeaponGroups.append(Reader.get_line_ptr());
                Record.WeaponGroups.append("\n");
            } else if (Reader.is_value("hull_status")) {
                Record.HullStatus = (float)Reader.get_value_float(0);    // FSTP float
            } else if (Reader.is_value("base_hull_status")) {
                Record.BaseHullStatus = (float)Reader.get_value_float(0); // FSTP float
            } else if (Reader.is_value("visit")) {
                // insert({id, 0}).first->second = (byte)v1.
                unsigned int const SystemId = static_cast<unsigned int>(Reader.get_value_int(0));
                unsigned char const Flags = static_cast<unsigned char>(Reader.get_value_int(1));
                Record.Visits.insert(std::make_pair(SystemId, static_cast<unsigned char>(0)))
                    .first->second = Flags;
            } else if (Reader.is_value("interface")) {
                Record.Interface = static_cast<unsigned int>(Reader.get_value_int(0));
            } else if (Reader.is_value("log")) {
                unsigned int LogIndex = 0;
                Record.MissionLog.push_back(ReadMissionLogEntry(Reader, LogIndex));
            } else {
                // `logid` is tested and the result dropped.
                Reader.is_value("logid");
            }
        }
    }

    // "Deck2" folds back to "Deck".
    if (MakeLocationID(Record.BaseId, "Deck2") == Record.LocationId)
        Record.LocationId = MakeLocationID(Record.BaseId, "Deck");

    // .. module storage -- the inlined the routine closes the
    // file: ~FSaveFileBuffer.
    return bSawPlayer;
}

// ===========================================================================
// PlayerData_LoadSavedCharacter -- baseline the routine (module storage ..
// ), thiscall, (Dir, FileName), `RET 8`.
// ===========================================================================
bool __cdecl PlayerData_LoadSavedCharacter(PlayerData& Slot, char const* Dir,
                                           char const* FileName) {
    CHARACTER_ID Key;
    safe_strcpy(Key.Buffer, 0x200, FileName);
    PlayerData::FSavedCharacter& Record = Slot.saved_characters[Key];

    char Path[0x104];
    _makepath(Path, nullptr, Dir, FileName, nullptr);

    if (PlayerData_LoadSaveRecord(Slot, Path, Record)) {
        safe_strcpy(Record.FileName, 0x200, FileName);
        return true;
    }
    FDUMP(0x100002, "%s(%d) : *** WARNING: Failed to load saved game %s",
          PlayerDbSource, 0x4a8, Path);
    std::map<CHARACTER_ID, PlayerData::FSavedCharacter>::iterator It =
        Slot.saved_characters.find(Key);
    if (It != Slot.saved_characters.end())
        Slot.saved_characters.erase(It);
    return false;
}

// ===========================================================================
// PlayerData_ReloadSavedCharacter -- baseline the routine (module storage ..
// ), thiscall, (FileName), `RET 4`.
// ===========================================================================
void __cdecl PlayerData_ReloadSavedCharacter(PlayerData& Slot, char const* FileName) {
    // the extension is masked to NULL in MP:
    // `NEG AL / SBB EAX,EAX / NOT EAX / AND EAX,"fl"`.
    char KeyName[0x104];
    _makepath(KeyName, nullptr, nullptr, FileName, IsMPServer() ? nullptr : "fl");

    char AccountDir[0x104];
    BuildAccountDir(AccountDir, Slot);

    char Path[0x104];
    _makepath(Path, nullptr, AccountDir, KeyName, nullptr);

    CHARACTER_ID Key;
    safe_strcpy(Key.Buffer, 0x200, KeyName);
    PlayerData::FSavedCharacter& Record = Slot.saved_characters[Key];
    if (PlayerData_LoadSaveRecord(Slot, Path, Record)) {
        safe_strcpy(Record.FileName, 0x200, KeyName);
        return;
    }
    std::map<CHARACTER_ID, PlayerData::FSavedCharacter>::iterator It =
        Slot.saved_characters.find(Key);
    if (It != Slot.saved_characters.end())
        Slot.saved_characters.erase(It);
}

// ===========================================================================
// PlayerData_SaveGame -- baseline the routine (module storage .. module storage),
// thiscall, (Name, Description, DescripStrId), `RET 0xc`.
// ===========================================================================
bool __cdecl PlayerData_SaveGame(PlayerData& Slot, char const* Name,
                                 wchar_t const* Description, unsigned int DescripStrId) {
    std::string FileName;
    if (Name[0] != '\0') {
        // REP MOVSD of 0x80 dwords, cut at the first '.'.
        char Copy[0x200];
        memcpy(Copy, Name, sizeof(Copy));
        char* Dot = strchr(Copy, '.');
        if (Dot != nullptr)
            *Dot = '\0';
        FileName.assign(Copy, strlen(Copy));
    } else {
        char AccountDir[0x104];
        BuildAccountDir(AccountDir, Slot);

        // ESI = rand(); up to 200 probes of <dir>\Save%04x.fl
        // (the extension is NOT masked in MP here). A name is free when
        // GetFileAttributesA fails with ERROR_FILE_NOT_FOUND.
        int Seed = rand();
        for (int Attempt = 1; ; ++Attempt) {
            char Candidate[0x20];
            sprintf(Candidate, "Save%04x", static_cast<int>(static_cast<short>(Seed))); // MOVSX
            char Probe[0x104];
            _makepath(Probe, nullptr, AccountDir, Candidate, "fl");
            if (GetFileAttributesA(Probe) == INVALID_FILE_ATTRIBUTES &&
                GetLastError() == ERROR_FILE_NOT_FOUND) {
                FileName.assign(Candidate, strlen(Candidate));
                break;
            }
            ++Seed;
            if (!(Attempt < 0xc8))
                break;                                                   // name stays ""
        }
    }

    if (PlayerData_SerializeToFile(Slot, FileName.c_str(), Description, DescripStrId)) {
        PlayerData_ReloadSavedCharacter(Slot, FileName.c_str());
        return true;
    }
    ++Slot.save_failure_count;
    return false;
}

// ===========================================================================
// PlayerData_MissionSave -- baseline the routine (module storage .. module storage),
// thiscall, (DescripStrId), `RET 4`.
// ===========================================================================
bool __cdecl PlayerData_MissionSave(PlayerData& Slot, unsigned int DescripStrId) {
    if (!PlayerData_SaveGame(Slot, "", nullptr, DescripStrId))
        return false;

    // .. module storage -- the account name (the inline copy of
    // 's hash in MP, wcstombs in SP), then module storage
    // GetUserDataPath + "Accts[\MultiPlayer]" into the global the field, which
    // is exactly what Server_GetAccountsRoot builds.
    char AccountDirName[0x104];
    BuildAccountDirName(AccountDirName, Slot);
    char AccountDir[0x104];
    _makepath(AccountDir, nullptr, Server_GetAccountsRoot(), AccountDirName, nullptr);

    // <dir>\AutoSave.fl, extension unmasked.
    char Destination[0x104];
    _makepath(Destination, nullptr, AccountDir, "AutoSave", "fl");

    // CopyFileA(last written path, dest, FALSE); its BOOL is
    // what AL carries out.
    return CopyFileA(GServerLastSavePath, Destination, FALSE) != 0;
}

// ===========================================================================
// PlayerData_AutoSave -- baseline the routine (module storage .. module storage).
// ===========================================================================
bool __cdecl PlayerData_AutoSave(PlayerData& Slot) {
    if (IsMPServer()) {
        // .. module storage
        if (IsMPServer() && !Slot.selected_character.is_valid())
            return false;
        if (Slot.system_id == 0)                                        // +0x3e0
            return false;
    }

    if (IsMPServer()) {
        // find(selected) with NO end() test: CharacterSelect
        // stores +0x64 only after its own find succeeded (module storage), so
        // baseline never reaches here without a record.
        std::map<CHARACTER_ID, PlayerData::FSavedCharacter>::iterator It =
            Slot.saved_characters.find(Slot.selected_character);

        // .. module storage -- "<date> <time>".
        char Time[0x10];
        char Date[0x10];
        wchar_t Description[0x200];
        _tzset();
        _strtime(Time);
        _strdate(Date);
        swprintf(Description, 0x200, L"%S %S", Date, Time);
        return PlayerData_SerializeToFile(Slot, It->second.FileName, Description, 0);
    }

    // single player. Universe::IBase vtable +0x54 is the
    // port's IBase::bAutosaveForbidden field.
    bool bAllowed = true;
    if ((Slot.current_base & 0x3fffffff) != 0) {
        Universe::IBase const* Base = Universe::get_base(Slot.current_base);
        if (Base->bAutosaveForbidden)
            bAllowed = false;
    }
    if (Slot.bSkipNextAutosave == 0 && bAllowed) {
        char Name[0x200];
        safe_strcpy(Name, 0x200, "AutoSave");
        return PlayerData_SaveGame(Slot, Name, nullptr, 0x65c);
    }
    Slot.bSkipNextAutosave = 0;
    return false;
}

// ===========================================================================
// PlayerData_SkipNextAutosave -- baseline the routine.
// ===========================================================================
void __cdecl PlayerData_SkipNextAutosave(PlayerData& Slot) {
    Slot.bSkipNextAutosave = 1;
}

// ===========================================================================
// PlayerData_ApplySavedCharacter -- the record -> slot copy of baseline
// , module storage .. module storage. EBP is the record, EBX the
// slot.
// ===========================================================================
void __cdecl PlayerData_ApplySavedCharacter(PlayerData& Slot,
                                            PlayerData::FSavedCharacter const& Record,
                                            unsigned int SessionId) {
    // .. module storage
    Slot.current_base = Record.BaseId;                                  // +0x3ec
    Slot.last_base_id = Record.LastBaseId;                              // +0x3f0
    Slot.current_location = Record.LocationId;                          // +0x3f8
    if ((Record.BaseId & 0x3fffffff) != 0) {
        Slot.last_base_id = Record.BaseId;
        // IBase vtable +0x10 -- the port's IBase::SystemId field.
        Slot.system_id = Universe::get_base(Record.BaseId)->SystemId;
    } else {
        Slot.system_id = Record.SystemId;
    }

    // .. module storage -- the rep vibe is rebuilt.
    if (Slot.rep_vibe != -1) {
        ::Reputation::Vibe::Free(Slot.rep_vibe);
        Slot.rep_vibe = -1;
    }
    Server_VibeID_Alloc(reinterpret_cast<unsigned int*>(&Slot.rep_vibe));
    ::Reputation::Vibe::Alloc(Slot.rep_vibe);
    ::Reputation::Vibe::SetClientID(Slot.rep_vibe, SessionId);
    FmtStr const EmptyName;
    ::Reputation::Vibe::Set(Slot.rep_vibe, Record.RepGroup, Record.Rank,
                            static_cast<unsigned char>(Record.Relations.size()),
                            Record.Relations.empty() ? nullptr : &Record.Relations[0],
                            EmptyName, EmptyName, Record.Name.c_str());

    // the com_ costume: four parts, the count, then only
    // `count` accessory ids.
    for (int Part = 0; Part < 4; ++Part)
        Slot.com_costume.PartIds[Part] = Record.ComCostume.PartIds[Part];
    Slot.com_costume.AccessoryCount = Record.ComCostume.AccessoryCount;
    for (int I = 0; I < static_cast<int>(Slot.com_costume.AccessoryCount); ++I)
        Slot.com_costume.AccessoryIds[I] = Record.ComCostume.AccessoryIds[I];

    // the routine(slot+0x2c4, 0x20, record+0x30c).
    Slot.voice_length = CopyBoundedString(Slot.voice, 0x20, Record.Voice.Data);

    // the bare costume, same shape.
    for (int Part = 0; Part < 4; ++Part)
        Slot.costume.PartIds[Part] = Record.BaseCostume.PartIds[Part];
    Slot.costume.AccessoryCount = Record.BaseCostume.AccessoryCount;
    for (int I = 0; I < static_cast<int>(Slot.costume.AccessoryCount); ++I)
        Slot.costume.AccessoryIds[I] = Record.BaseCostume.AccessoryIds[I];

    // .. module storage
    Slot.initial_pos[0] = Record.Position[0];
    Slot.initial_pos[1] = Record.Position[1];
    Slot.initial_pos[2] = Record.Position[2];
    memcpy(Slot.initial_ornt, Record.Orientation, sizeof(Slot.initial_ornt));

    // .. module storage
    Slot.ship_archetype = Record.ShipArchetype;                         // +0x264
    Slot.relative_health = Record.HullStatus;                           // +0x268
    Slot.equipment.Items = Record.Equipment.Items;                      // list::operator=
    Slot.collision_groups = Record.CollisionGroups;                     // +0x26c

    // .. module storage
    Slot.cash = Record.Money;                                           // +0x31c
    Slot.equip_id_maker = Record.EquipIdMaker;                          // +0x3c8
    Slot.num_kills = Record.NumKills;                                   // +0x334
    Slot.num_mission_successes = Record.NumMissionSuccesses;            // +0x338
    Slot.num_mission_failures = Record.NumMissionFailures;              // +0x33c
    Slot.save_failure_count = 0;                                        // +0x344
    Slot.current_location = 0;                                          // +0x3f8, after the copy above
    Slot.Field_0x3FC = 0;
    Slot.ship_object_id = 0;                                            // +0x3e4
    Slot.created_ship_id = 0;                                           // +0x3e8
    Slot.exited_base_id = 0;                                            // +0x410
    Slot.bSkipNextAutosave = 0;                                        // +0x340
    Slot.starting_ring = Record.StartingRing;                           // +0x414
    Slot.rank_level = Record.Rank;                                      // +0x284

    // .. module storage
    Slot.weapon_group_name = Record.WeaponGroups;                       // +0x380
    Slot.visit_map = Record.Visits;                                     // +0x3a0
    Slot.mission_log = Record.MissionLog;                               // +0x390
    Slot.interface_state = Record.Interface;                            // +0x39c
}

// ===========================================================================
// PlayerData_SendVisitedState -- baseline the routine (module storage ..
// ), thiscall on the slot, (SessionId), `RET 4`.
// n = [this+0x3b0] (visit map size) ; 06d4b52d size = n*5 + 4
// block = operator new(size) ; block[0] = n
// per map node in order: dword key (+0xc), byte flags (+0x10), 5-byte stride
// IClient +0x170 (slot 92)(SessionId, block, size) -- rpclocal's
//             sender (0x06d8b540, ch1 op 0x13) keeps the block and frees it
// ===========================================================================
void __cdecl PlayerData_SendVisitedState(PlayerData& Slot, unsigned int SessionId) {
    unsigned int const Count = static_cast<unsigned int>(Slot.visit_map.size());
    unsigned int const Size = Count * 5 + 4;
    unsigned char* const Block = static_cast<unsigned char*>(::operator new(Size));
    memcpy(Block, &Count, 4);
    unsigned char* Cursor = Block + 4;
    for (std::map<unsigned int, unsigned char>::const_iterator It = Slot.visit_map.begin();
         It != Slot.visit_map.end(); ++It) {
        memcpy(Cursor, &It->first, 4);
        Cursor[4] = It->second;
        Cursor += 5;
    }
    g_pClientInterface->SendCommonSetVisitedState(SessionId, Block, Size);
}

namespace {

// (thiscall on FmtStr, plain RET): 6 + per Val 7, 0x17 for tag
// 0x4e, (this function on the nested FmtStr) + 3 for tag 0x21. the routine
// inlines the same loop for each log entry (module storage .. module storage).
int MissionLogEntrySize(FmtStr const& Entry) {
    int Size = 6;
    for (FmtStr::Val const* Node = Entry.Head; Node != nullptr; Node = Node->Next) {
        int const Tag = Node->TypeTag;
        if (Tag == 0x21)
            Size += MissionLogEntrySize(*Node->Subtree) + 3;
        else
            Size += (Tag == 0x4e) ? 0x17 : 7;   // SETE / DEC / AND 0xfffffff0 / ADD 0x17
    }
    return Size;
}

} // namespace

// ===========================================================================
// PlayerData_SendMissionLog -- baseline the routine (module storage ..
// ), thiscall on the slot, (SessionId), `RET 4`.
//   size = 8 + sum(entry sizes)             module storage .. module storage
//   block = operator new(size); [0] = 1, [1] = list size (+0x398)
//   cursor = block + 8, offset = 8; AppendMissionLogData(&entry, cursor, offset, size)
//   per entry (module storage), then IClient +0x184 (slot 97)(SessionId, block,
//   offset) -- the running offset, not `size` (module storage). rpclocal keeps and
//   frees the block (flag 1, sender 0x06d8b7f0).
// ===========================================================================
void __cdecl PlayerData_SendMissionLog(PlayerData& Slot, unsigned int SessionId) {
    int Size = 8;
    for (std::list<FmtStr>::const_iterator It = Slot.mission_log.begin();
         It != Slot.mission_log.end(); ++It)
        Size += MissionLogEntrySize(*It);

    unsigned char* const Block = static_cast<unsigned char*>(::operator new(static_cast<size_t>(Size)));
    unsigned int const Header[2] = { 1u, static_cast<unsigned int>(Slot.mission_log.size()) };
    memcpy(Block, Header, sizeof(Header));
    unsigned char* Cursor = Block + 8;
    int Offset = 8;
    for (std::list<FmtStr>::const_iterator It = Slot.mission_log.begin();
         It != Slot.mission_log.end(); ++It)
        AppendMissionLogData(&*It, Cursor, Offset, Size);
    g_pClientInterface->SendCommonSetMissionLog(SessionId, Block, static_cast<unsigned int>(Offset));
}

// ===========================================================================
// PlayerData_LoadSelectedCharacter -- the second half of baseline the routine,
// .. module storage (both the SP and MP paths join here).
// EBX is the slot, EBP the found record (node + 0x20c), [ESP+0x1924] the session.
// ===========================================================================
bool __cdecl PlayerData_LoadSelectedCharacter(PlayerData& Slot,
                                              PlayerData::FSavedCharacter const& Record,
                                              unsigned int SessionId) {
    // .. module storage -- <accts>\<account dir>\<record file name>:
    // in MP / wcstombs in SP, the Accts root into the field,
    // then _makepath(S+0x9c, NULL, S+0x9c, record+0x000, NULL).
    char AccountDir[0x104];
    BuildAccountDir(AccountDir, Slot);
    char Path[0x104];
    _makepath(Path, nullptr, AccountDir, Record.FileName, nullptr);

    // .. module storage -- the file again, the same FLS1 / FLSG /
    // plain three-way as the routine. open_memory runs on every path, over an
    // empty buffer when fopen failed or the file is FLSG (module storage).
    FSaveFileBuffer Buffer;                                             // S+0x4c, SaveGame ctor inlined
    INI_Reader Reader;                                                  // S+0x3a8
    Buffer.File = fopen(Path, "rt");
    if (Buffer.File != nullptr) {
        char Magic[4] = { 0, 0, 0, 0 };
        fread(Magic, 1, 4, Buffer.File);
        if (memcmp(Magic, Server_FLS1Magic, 4) == 0) {
            fclose(Buffer.File);
            Buffer.File = fopen(Path, "rb");
            if (Buffer.File != nullptr) {
                Buffer.ReadTail(4);
                Buffer.Encode();
            }
        } else if (memcmp(Magic, "FLSG", 4) != 0) {
            Buffer.ReadTail(0);
        }
    }
    Reader.open_memory(Buffer.Contents.c_str(),
                       static_cast<unsigned int>(Buffer.Contents.size()));

    // .. module storage -- single player only: [Time], feelings,
    // controller event 0x40 (the routine, IServerImpl.cpp).
    if (SinglePlayer())
        Server_LoadSinglePlayerState(Reader);

    // .. module storage -- the record -> slot copy.
    PlayerData_ApplySavedCharacter(Slot, Record, SessionId);

    // this->the routine(SessionId).
    PlayerData_SendVisitedState(Slot, SessionId);

    // .. module storage -- IClient +0x16c (slot 91)(SessionId,
    // weapon groups c_str, strlen). rpclocal's sender leaves the not-owned flag
    // clear (payload+0x08 = 0), so the string's own buffer goes out as is.
    char const* const WeaponGroups = Slot.weapon_group_name.c_str();
    g_pClientInterface->SendCommonSetWeaponGroup(SessionId,
                                                 reinterpret_cast<const unsigned char*>(WeaponGroups),
                                                 static_cast<unsigned int>(strlen(WeaponGroups)));

    // this->the routine(SessionId).
    PlayerData_SendMissionLog(Slot, SessionId);

    // .. module storage -- a 4-byte operator new block holding
    // +0x39c, IClient +0x188 (slot 98)(SessionId, block, 4); rpclocal owns it.
    unsigned int* const InterfaceBlock = static_cast<unsigned int*>(::operator new(4));
    *InterfaceBlock = Slot.interface_state;
    // A fresh operator-new block: SendCommonSetInterfaceState's packet takes it over.
    g_pClientInterface->SendCommonSetInterfaceState(SessionId,
                                                    reinterpret_cast<unsigned char*>(InterfaceBlock), 4);

    // .. module storage -- the routine(5, &{SessionId, &reader}):
    // the character-loaded broadcast hands the controllers the open reader.
    struct FCharacterLoadedEvent {
        unsigned int SessionId;                                         // S+0x64
        INI_Reader* Reader;                                             // S+0x68
    };
    FCharacterLoadedEvent Event;
    Event.SessionId = SessionId;
    Event.Reader = &Reader;
    Server_ControllerDispatch(0, pub::Controller::EMessage::PlayerLoad, &Event);

    // .. module storage -- ~FmtStr, ~INI_Reader, fclose, the buffer.
    if (Buffer.File != nullptr) {
        fclose(Buffer.File);
        Buffer.File = nullptr;
    }
    return true;
}

// ===========================================================================
// PlayerData_CreateNewCharacter -- baseline the routine (module storage ..
// ), thiscall on the PlayerData, (SCreateCharacterInfo const*),
// `RET 4`. Only caller: PlayerDB::create_new_character, module storage.

// Frame (S = ESP after the four register pushes): +0x13 result byte, +0x14 pilot,
// +0x18 package, +0x1c home-base record, +0x6c faction, +0x24 the SaveGame_Writer
// (vptr 06d6b5c0 then 06d6b5a0, FILE* +0x28, buffer +0x2c, path +0x3c), +0x54 the
// template {FILE*, string}, +0x70 "<hash>.fl", +0x88 hash / module path / template
// path, +0x18c the output path, +0x594 one substituted value, +0x994 the joined
// value line, +0xd94 the INI_Reader.
// ===========================================================================
namespace {

// (cdecl, (Out, AccountName)), module storage .. module storage:
// MP -- the the routine hash inlined (module storage .. module storage), SP --
// wcstombs(buf, name, 0x104) (module storage); both then GetUserDataPath +
// "Accts[\MultiPlayer]" into the field (what Server_GetAccountsRoot builds)
// and _makepath(Out, NULL, the field, buf, NULL).
void BuildAccountDirForName(char* Out, wchar_t const* AccountName) {
    char AccountDirName[0x104];
    if (IsMPServer())
        Server_AccountDirName(AccountDirName, AccountName);
    else
        wcstombs(AccountDirName, AccountName, 0x104);
    _makepath(Out, nullptr, Server_GetAccountsRoot(), AccountDirName, nullptr);
}

} // namespace

bool __cdecl PlayerData_CreateNewCharacter(PlayerData& Slot, SCreateCharacterInfo const& Info) {
    // the routine(S+0x18c, this). The `this == NULL` arm
    // (module storage, empty directory) is unreachable: the only caller passes
    // EBP = &m_PlayerArray[SessionId - 1] (module storage).
    char AccountDir[0x104];
    BuildAccountDirForName(AccountDir, Slot.AccountName);

    // the routine over the NEW character's name (always the
    // hash, in SP too), then "<hash>" + "." + "fl" (module storage / 06d6aea4).
    char HashName[0x104];
    Server_AccountDirName(HashName, Info.Name);
    std::string FileName(HashName);
    FileName.append(".");
    FileName.append("fl");

    // _makepath(S+0x18c, NULL, S+0x18c, hash, "fl"); baseline
    // uses the same buffer as directory and destination.
    char Path[0x104];
    _makepath(Path, nullptr, AccountDir, HashName, "fl");
    _strlwr(HashName);

    // .. module storage -- NewChar::TheDB slots 0, 1, 3, 2.
    // Common's Load (Common.dll module storage) fills baseline's four trees as
    // [Faction] -> +0x04 (slot 6 insert, slot 0 find), [Package] -> +0x18
    // (slot 7 / slot 1), [Pilot] -> +0x2c (slot 8 / slot 2), and every faction
    // `base` -> +0x40 as {CreateID(base), base} (slot 9 / slot 3). The port calls
    // those finds by name; common_newchar.h declares them in this slot order
    // (labels corrected 2026-09-24).
    NewChar::IDatabase const* Database = NewChar::TheDB;
    FNewCharFaction const* Faction = Database->FindFaction(Info.Field_0x30);   // S+0x6c
    FNewCharPackage const* Package = Database->FindPackage(Info.Field_0x38);   // S+0x18
    FNewCharExtra const* HomeBase = Database->FindExtra(Info.Field_0x34);      // S+0x1c
    FNewCharPilot const* Pilot = Database->FindPilot(Info.Field_0x3c);         // S+0x14

    // .. module storage -- all four missing: false, no file.
    if (Faction == nullptr && Package == nullptr && HomeBase == nullptr && Pilot == nullptr)
        return false;

    bool bResult = false;                                // S+0x13, module storage
    {
        SaveGame_Writer Writer;
        // .. module storage -- path assigned, fopen(path, "w+t").
        if (Writer.Open(Path)) {
            // .. module storage -- the template beside the exe.
            char ModulePath[0x104];
            GetModuleFileNameA(nullptr, ModulePath, 0x104);
            char Drive[_MAX_DRIVE];
            char Dir[0x104];
            _splitpath(ModulePath, Drive, Dir, nullptr, nullptr);
            char TemplatePath[0x104];
            _makepath(TemplatePath, Drive, Dir,
                      SinglePlayer() ? "NewCharacter.fl" : "MPNewCharacter.fl", nullptr);

            FSaveFileBuffer Template;                    // S+0x54, SaveGame ctor inlined
            INI_Reader Reader;                           // S+0xd94

            // fopen "rt", then the same FLS1 / FLSG / plain
            // three-way the routine uses.
            bool bOpened = false;
            Template.File = fopen(TemplatePath, "rt");
            if (Template.File != nullptr) {
                char Magic[4] = { 0, 0, 0, 0 };
                fread(Magic, 1, 4, Template.File);
                if (memcmp(Magic, Server_FLS1Magic, 4) == 0) {
                    fclose(Template.File);
                    Template.File = fopen(TemplatePath, "rb");
                    if (Template.File != nullptr) {
                        Template.ReadTail(4);
                        Template.Encode();
                        bOpened = true;
                    }
                } else if (memcmp(Magic, "FLSG", 4) != 0) {
                    // plain text from offset 0. "FLSG" instead
                    // jumps straight to module storage, file left open until the
                    // teardown at module storage.
                    Template.ReadTail(0);
                    bOpened = true;
                }
            }

            if (bOpened) {

                Reader.open_memory(Template.Contents.c_str(),
                                   static_cast<unsigned int>(Template.Contents.size()));
                // .. module storage -- any one missing: close the
                // reader, result stays false, but the (empty) output is still
                // finalized below.
                if (Faction != nullptr && Package != nullptr && HomeBase != nullptr &&
                    Pilot != nullptr) {
                    FILE* const Out = Writer.File;       // [writer+4], module storage
                    // S+0x994. baseline never clears it between keys: a key with no
                    // values (module storage JLE straight to the fprintf) prints
                    // whatever the previous key joined. Its first contents are
                    // stack garbage in baseline; "" here is the only defined choice.
                    char Joined[0x400];
                    Joined[0] = '\0';
                    while (Reader.read_header()) {
                        fprintf(Out, "\n[%s]\n", Reader.get_header_ptr());
                        while (Reader.read_value()) {
                            char const* const Key = Reader.get_name_ptr();      // S+0x68
                            if (_stricmp(Key, "%%Package%%") == 0) {
                                // ship, then the loadout.
                                // package +0x10 / +0x20, each through the c_str idiom.
                                fprintf(Out, "%s = %s\n", "ship_archetype", Package->Ship.c_str());
                                // No null test on Loadout::Get (module storage reads [EAX+8]).
                                Loadout::Map const* Map =
                                    Loadout::Get(Loadout::GetID(Package->Loadout.c_str()));
                                for (std::vector<EquipDesc>::const_iterator It = Map->Items.begin();
                                     It != Map->Items.end(); ++It) {
                                    // stride 0x20, byte +0xc picks
                                    // equip vs cargo; an unknown arch is skipped.
                                    Archetype::Equipment const* Equipment =
                                        Archetype::GetEquipment(It->ArchId);
                                    if (Equipment == nullptr)
                                        continue;
                                    char Line[0x100];
                                    if (It->bEquipped) {
                                        sprintf(Line, "%u, %s", Equipment->Nickname.Id, It->Hardpoint);
                                        fprintf(Out, "%s = %s\n", "equip", Line);
                                    } else {
                                        sprintf(Line, "%u, %d", Equipment->Nickname.Id, It->Count);
                                        fprintf(Out, "%s = %s\n", "cargo", Line);
                                    }
                                }
                                continue;
                            }

                            // every value, substituted, joined
                            // with ", " (module storage).
                            int const Count = static_cast<int>(Reader.get_num_parameters());
                            for (int Index = 0; Index < Count; ++Index) {
                                if (Index == 0)
                                    Joined[0] = '\0';
                                else
                                    strcat(Joined, ", ");
                                char const* Value = Reader.get_value_string(static_cast<unsigned int>(Index));
                                char Scratch[0x400];                              // S+0x594
                                if (Value[0] == '%') {
                                    if (_stricmp(Value, "%%Name%%") == 0) {
                                        WideToHex(Info.Name, Scratch);
                                        Value = Scratch;
                                    } else if (_stricmp(Value, "%%Faction%%") == 0) {
                                        Value = Faction->RepGroup.c_str();        // faction +0x08 (the string's pointer)
                                    } else if (_stricmp(Value, "%%Money%%") == 0) {
                                        sprintf(Scratch, "%u", static_cast<unsigned int>(Package->Money)); // package +0x2c
                                        Value = Scratch;
                                    } else if (_stricmp(Value, "%%Base_Costume%%") == 0) {
                                        Value = Pilot->Body.c_str();              // pilot +0x08
                                    } else if (_stricmp(Value, "%%Comm_Costume%%") == 0) {
                                        Value = Pilot->Comm.c_str();              // pilot +0x28
                                    } else if (_stricmp(Value, "%%Voice%%") == 0) {
                                        Value = Pilot->Voice.c_str();             // pilot +0x48
                                    } else if (_stricmp(Value, "%%Home_System%%") == 0) {
                                        // record +0x00 is the key the
                                        // find matched; a missing base or system keeps
                                        // the literal placeholder (module storage /
                                        // jump past the store).
                                        Universe::IBase const* Base = Universe::get_base(Info.Field_0x34);
                                        if (Base != nullptr) {
                                            // IBase vtable +0x10 / ISystem vtable +0x08 --
                                            // the port's plain SystemId / Nickname fields.
                                            Universe::ISystem const* System =
                                                Universe::get_system(Base->SystemId);
                                            if (System != nullptr)
                                                Value = System->Nickname;
                                        }
                                    } else if (_stricmp(Value, "%%Home_Base%%") == 0) {
                                        // [record+0x08] is the
                                        // string's pointer, read through the c_str idiom.
                                        Value = HomeBase->Value.c_str();
                                    }
                                }
                                strcat(Joined, Value);
                            }
                            fprintf(Out, "%s = %s\n", Key, Joined);
                        }
                        fprintf(Out, "\n");
                    }
                    bResult = true;
                }
                Reader.close();                                                   // / 06d4bf5b
            }

            // .. module storage -- the writer's encode-and-rewrite,
            // on every path that opened the output.
            Writer.Close();

            // .. module storage -- ~INI_Reader, close the template.
            if (Template.File != nullptr) {
                fclose(Template.File);
                Template.File = nullptr;
            }
        }

        if (bResult) {
            // this->the routine("<hash>.fl").
            PlayerData_ReloadSavedCharacter(Slot, FileName.c_str());
            if (!SinglePlayer()) {
                FDUMP(0x100003, "LOG: Create character on account %S (\"%s\")\n",
                      Slot.AccountName, Path);
            }
        }
        // the writer's destructor; its FILE* is already null.
    }
    return bResult;
}

// DeleteCharacterSave / CharacterSaveExists were removed 2026-09-24
// (claude:srv-save-2). Both built a path from a bare character NAME under the
// accounts root, which baseline never does. IServerImpl::DestroyCharacter now
// runs baseline's the routine (PlayerData_DestroyCharacter, server_classes.cpp),
// and CharacterSaveExists had no caller.

// ===========================================================================
// Oracle instrumentation (P3.2, the "saves" kind of Oracle.exe dump): the record
// PlayerData_LoadSaveRecord fills from a save, written to OutPath one field per line as
// {"k":"<key>","v":"<value>"}: numbers in decimal, ids and float bits as eight hex digits,
// texts as the hex of their bytes (wide texts four digits per unit), lists one line per
// element. crates/game-logic/tests/save_dump.rs renders the same lines. A new export; no
// game module calls it. Returns the loader's answer.
// ===========================================================================
namespace {
    void OracleSaveLine(FILE* Out, char const* Key, char const* Value) {
        fprintf(Out, "{\"k\":\"%s\",\"v\":\"%s\"}\n", Key, Value);
    }

    void OracleSaveUint(FILE* Out, char const* Key, unsigned int Value) {
        char Text[32];
        sprintf_s(Text, "%u", Value);
        OracleSaveLine(Out, Key, Text);
    }

    void OracleSaveHex(FILE* Out, char const* Key, unsigned int Value) {
        char Text[32];
        sprintf_s(Text, "%08x", Value);
        OracleSaveLine(Out, Key, Text);
    }

    void OracleSaveFloat(FILE* Out, char const* Key, float Value) {
        unsigned int Bits;
        memcpy(&Bits, &Value, sizeof(Bits));
        OracleSaveHex(Out, Key, Bits);
    }

    std::string OracleBytesHex(void const* Data, size_t Length) {
        static char const Digits[] = "0123456789abcdef";
        unsigned char const* Raw = static_cast<unsigned char const*>(Data);
        std::string Text;
        for (size_t Index = 0; Index < Length; ++Index) {
            Text += Digits[Raw[Index] >> 4];
            Text += Digits[Raw[Index] & 0xf];
        }
        return Text;
    }

    std::string OracleWideHex(std::wstring const& Text) {
        std::string Out;
        char Unit[8];
        for (size_t Index = 0; Index < Text.size(); ++Index) {
            sprintf_s(Unit, "%04x", static_cast<unsigned int>(static_cast<unsigned short>(Text[Index])));
            Out += Unit;
        }
        return Out;
    }

    void OracleSaveEquip(FILE* Out, char const* Key, EquipDescList const& List) {
        for (std::list<EquipDesc>::const_iterator It = List.Items.begin(); It != List.Items.end(); ++It) {
            unsigned int HealthBits;
            memcpy(&HealthBits, &It->Health, sizeof(HealthBits));
            char const* const Hardpoint = It->Hardpoint != nullptr ? It->Hardpoint : "";
            char Text[1024];
            sprintf_s(Text, "%u,%08x,%s,%u,%08x,%d,%u,%u", static_cast<unsigned int>(It->Id), It->ArchId,
                      OracleBytesHex(Hardpoint, strlen(Hardpoint)).c_str(),
                      static_cast<unsigned int>(It->bEquipped), HealthBits, It->Count, It->bMission ? 1u : 0u,
                      It->Owner);
            OracleSaveLine(Out, Key, Text);
        }
    }

    void OracleSaveGroups(FILE* Out, char const* Key, std::list<CollisionGroupDesc> const& Groups) {
        for (std::list<CollisionGroupDesc>::const_iterator It = Groups.begin(); It != Groups.end(); ++It) {
            unsigned int HealthBits;
            memcpy(&HealthBits, &It->Health, sizeof(HealthBits));
            char Text[64];
            sprintf_s(Text, "%u,%08x", static_cast<unsigned int>(It->Id), HealthBits);
            OracleSaveLine(Out, Key, Text);
        }
    }

    void OracleSaveCostume(FILE* Out, char const* Key, Costume const& Value) {
        std::string Text;
        char Part[16];
        for (int Index = 0; Index < 4; ++Index) {
            sprintf_s(Part, "%s%08x", Index ? "," : "", Value.PartIds[Index]);
            Text += Part;
        }
        unsigned int const Count = Value.AccessoryCount < 8 ? Value.AccessoryCount : 8;
        for (unsigned int Index = 0; Index < Count; ++Index) {
            sprintf_s(Part, ",%08x", Value.AccessoryIds[Index]);
            Text += Part;
        }
        OracleSaveLine(Out, Key, Text.c_str());
    }
}

extern "C" SERVER_API bool __cdecl OracleDumpSaveRecord(char const* SavePath, char const* OutPath) {
    static PlayerData Unused;
    PlayerData::FSavedCharacter Record;
    bool const bSaw = PlayerData_LoadSaveRecord(Unused, SavePath, Record);
    FILE* Out = fopen(OutPath, "wb");
    if (Out == nullptr)
        return bSaw;
    OracleSaveUint(Out, "saw", bSaw ? 1u : 0u);
    OracleSaveLine(Out, "name", OracleWideHex(Record.Name).c_str());
    OracleSaveLine(Out, "description", OracleWideHex(Record.Description).c_str());
    OracleSaveUint(Out, "descrip_strid", Record.DescripStrId);
    OracleSaveHex(Out, "tstamp_high", Record.TimestampHigh);
    OracleSaveHex(Out, "tstamp_low", Record.TimestampLow);
    OracleSaveHex(Out, "ship_archetype", Record.ShipArchetype);
    OracleSaveUint(Out, "money", static_cast<unsigned int>(Record.Money));
    OracleSaveUint(Out, "num_kills", static_cast<unsigned int>(Record.NumKills));
    OracleSaveUint(Out, "num_misn_successes", static_cast<unsigned int>(Record.NumMissionSuccesses));
    OracleSaveUint(Out, "num_misn_failures", static_cast<unsigned int>(Record.NumMissionFailures));
    OracleSaveFloat(Out, "hull_status", Record.HullStatus);
    OracleSaveEquip(Out, "equip", Record.Equipment);
    OracleSaveGroups(Out, "collision_group", Record.CollisionGroups);
    OracleSaveFloat(Out, "base_hull_status", Record.BaseHullStatus);
    OracleSaveEquip(Out, "base_equip", Record.BaseEquipment);
    OracleSaveGroups(Out, "base_collision_group", Record.BaseCollisionGroups);
    OracleSaveHex(Out, "base", Record.BaseId);
    OracleSaveHex(Out, "last_base", Record.LastBaseId);
    OracleSaveHex(Out, "location", Record.LocationId);
    OracleSaveHex(Out, "system", Record.SystemId);
    for (int Index = 0; Index < 3; ++Index)
        OracleSaveFloat(Out, "pos", Record.Position[Index]);
    for (int Row = 0; Row < 3; ++Row)
        for (int Col = 0; Col < 3; ++Col)
            OracleSaveFloat(Out, "orient", Record.Orientation[Row][Col]);
    OracleSaveHex(Out, "starting_ring", Record.StartingRing);
    OracleSaveUint(Out, "rank", Record.Rank);
    for (size_t Index = 0; Index < Record.Relations.size(); ++Index) {
        unsigned int Bits;
        memcpy(&Bits, &Record.Relations[Index].Feeling, sizeof(Bits));
        char Text[32];
        sprintf_s(Text, "%08x,%08x", Record.Relations[Index].GroupId, Bits);
        OracleSaveLine(Out, "relation", Text);
    }
    OracleSaveHex(Out, "rep_group", Record.RepGroup);
    OracleSaveCostume(Out, "com_costume", Record.ComCostume);
    OracleSaveLine(Out, "voice", OracleBytesHex(Record.Voice.Data, Record.Voice.Length).c_str());
    OracleSaveCostume(Out, "costume", Record.BaseCostume);
    OracleSaveLine(Out, "wg", OracleBytesHex(Record.WeaponGroups.data(), Record.WeaponGroups.size()).c_str());
    for (std::list<FmtStr>::const_iterator It = Record.MissionLog.begin(); It != Record.MissionLog.end(); ++It) {
        unsigned char Flat[0x4000];
        int const Size = It->flatten(Flat, sizeof(Flat));
        OracleSaveLine(Out, "log", Size > 0 ? OracleBytesHex(Flat, static_cast<size_t>(Size)).c_str() : "");
    }
    OracleSaveUint(Out, "interface", Record.Interface);
    for (std::map<unsigned int, unsigned char>::const_iterator It = Record.Visits.begin(); It != Record.Visits.end();
         ++It) {
        char Text[32];
        sprintf_s(Text, "%08x,%u", It->first, static_cast<unsigned int>(It->second));
        OracleSaveLine(Out, "visit", Text);
    }
    fclose(Out);
    return bSaw;
}

// Oracle instrumentation (P3.2): a template through the save writer. The
// record PlayerData_LoadSaveRecord reads is applied to a fresh slot of the
// account "OracleSaves" (PlayerData_ApplySavedCharacter, client 1), and
// PlayerData_SerializeToFile writes it as "oraclesave" under the accounts
// root; the bytes it wrote go to OutPath, one {"k":"line","v":hex} per line
// with its "\n" (the last line may have none).
extern "C" SERVER_API bool __cdecl OracleWriteSaveRecord(char const* SavePath, char const* OutPath) {
    PlayerData* const Slot = new PlayerData();
    PlayerData::FSavedCharacter Record;
    if (!PlayerData_LoadSaveRecord(*Slot, SavePath, Record))
        return false;
    wcscpy_s(Slot->AccountName, L"OracleSaves");
    PlayerData_ApplySavedCharacter(*Slot, Record, 1);

    char Dir[260];
    sprintf_s(Dir, "%s\\OracleSaves", Server_GetAccountsRoot());
    for (char* Cursor = Dir; *Cursor != '\0'; ++Cursor) {
        if (*Cursor == '\\' && Cursor != Dir && Cursor[-1] != ':') {
            *Cursor = '\0';
            CreateDirectoryA(Dir, nullptr);
            *Cursor = '\\';
        }
    }
    CreateDirectoryA(Dir, nullptr);

    if (!PlayerData_SerializeToFile(*Slot, "oraclesave", nullptr, 0))
        return false;
    FILE* In = fopen(GServerLastSavePath, "rb");
    if (In == nullptr)
        return false;
    std::string Bytes;
    char Chunk[4096];
    size_t Read;
    while ((Read = fread(Chunk, 1, sizeof(Chunk), In)) > 0)
        Bytes.append(Chunk, Read);
    fclose(In);
    FILE* Out = fopen(OutPath, "wb");
    if (Out == nullptr)
        return false;
    size_t Start = 0;
    while (Start < Bytes.size()) {
        size_t End = Bytes.find('\n', Start);
        End = End == std::string::npos ? Bytes.size() : End + 1;
        OracleSaveLine(Out, "line", OracleBytesHex(Bytes.data() + Start, End - Start).c_str());
        Start = End;
    }
    fclose(Out);
    return true;
}

// Oracle instrumentation (P3.2): the single-player account flow on a fresh
// account "OracleLogin". PlayerDB::init(2, true), the account folder emptied,
// PlayerDB::login with CreateFlag 1 (load_user_data: create_account, the restart file, the login),
// then PlayerData_BuildCharacterSet; then, as a multiplayer game
// (SetMultiPlayer(true) around the call), PlayerData_CreateNewCharacter with
// "Oracle Pilot" and the shipped new-player choices, and the list again; and
// PlayerData_DestroyCharacter on the restart file. Written to OutPath as
// {"k":...,"v":...} lines; files as one "restart" / "created" line per line of
// the file, hex.
namespace {
    void OracleFileLines(FILE* Out, char const* Key, char const* Path) {
        FILE* In = fopen(Path, "rb");
        if (In == nullptr) {
            OracleSaveLine(Out, Key, "missing");
            return;
        }
        std::string Bytes;
        char Chunk[4096];
        size_t Read;
        while ((Read = fread(Chunk, 1, sizeof(Chunk), In)) > 0)
            Bytes.append(Chunk, Read);
        fclose(In);
        size_t Start = 0;
        while (Start < Bytes.size()) {
            size_t End = Bytes.find('\n', Start);
            End = End == std::string::npos ? Bytes.size() : End + 1;
            OracleSaveLine(Out, Key, OracleBytesHex(Bytes.data() + Start, End - Start).c_str());
            Start = End;
        }
    }

    void OracleCharacterSet(FILE* Out, PlayerData& Slot) {
        CCharacterSet Set;
        PlayerData_BuildCharacterSet(Slot, Set);
        MetaNode<CCharacterSet::CharacterInfo>* Node = nullptr;
        while (Set.Characters.traverse(Node, true)) {
            CCharacterSet::CharacterInfo const& Info = *Node->Data;
            char Text[512];
            sprintf_s(Text, "%s,%u,%u,%s,%u,%08x,%d,%08x,%08x,%08x,%u,%08x",
                      OracleBytesHex(Info.Id.Buffer, strlen(Info.Id.Buffer)).c_str(),
                      static_cast<unsigned int>(Info.bIsRestart),
                      static_cast<unsigned int>(Info.bIsAutoSave),
                      OracleWideHex(Info.Name).c_str(), Info.DescripStrId, Info.ShipArchetype,
                      Info.Money, Info.SystemId, Info.BaseId, Info.LocationId, Info.Rank,
                      Info.VoiceId);
            OracleSaveLine(Out, "character", Text);
        }
        OracleSaveUint(Out, "save_failure_count", Set.SaveFailureCount);
    }
}

extern "C" SERVER_API bool __cdecl OracleSinglePlayerAccount(char const* OutPath) {
    Players.init(2, true);
    PlayerData& Slot = Players[1];

    char Dir[260];
    sprintf_s(Dir, "%s\\OracleLogin", Server_GetAccountsRoot());
    {
        char Pattern[260];
        sprintf_s(Pattern, "%s\\*", Dir);
        WIN32_FIND_DATAA Found;
        HANDLE Find = FindFirstFileA(Pattern, &Found);
        if (Find != INVALID_HANDLE_VALUE) {
            do {
                if ((Found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                    char File[260];
                    sprintf_s(File, "%s\\%s", Dir, Found.cFileName);
                    DeleteFileA(File);
                }
            } while (FindNextFileA(Find, &Found));
            FindClose(Find);
        }
        RemoveDirectoryA(Dir);
    }

    FILE* Out = fopen(OutPath, "wb");
    if (Out == nullptr)
        return false;
    SLoginInfo Info;
    memset(&Info, 0, sizeof(Info));
    wcscpy_s(Info.AccountName, L"OracleLogin");
    Info.CreateFlag = 1;
    unsigned char const Result = Players.login(Info, 1);
    OracleSaveUint(Out, "login", Result);
    char Restart[260];
    sprintf_s(Restart, "%s\\Restart.fl", Dir);
    OracleFileLines(Out, "restart", Restart);
    OracleCharacterSet(Out, Slot);

    SCreateCharacterInfo Create;
    memset(&Create, 0, sizeof(Create));
    wcscpy_s(Create.Name, L"Oracle Pilot");
    Create.Field_0x30 = CreateID("new_player");
    Create.Field_0x34 = CreateID("he01_01_base");
    Create.Field_0x38 = CreateID("ge_fighter");
    Create.Field_0x3c = CreateID("rellan");
    SetMultiPlayer(true);
    bool const bCreated = PlayerData_CreateNewCharacter(Slot, Create);
    SetMultiPlayer(false);
    OracleSaveUint(Out, "created", bCreated ? 1u : 0u);
    char Hash[260];
    Server_AccountDirName(Hash, Create.Name);
    char Created[260];
    sprintf_s(Created, "%s\\%s.fl", Dir, Hash);
    OracleFileLines(Out, "created_file", Created);
    OracleCharacterSet(Out, Slot);

    CHARACTER_ID RestartId;
    safe_strcpy(RestartId.Buffer, 0x200, "Restart.fl");
    OracleSaveUint(Out, "destroy_restart",
                   PlayerData_DestroyCharacter(Slot, RestartId, 1) ? 1u : 0u);
    fclose(Out);
    return true;
}
