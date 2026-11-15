// Author: Giperion.

#include "character_select_equip.h"

#include "server_characterset.h"
#include "common_id.h"
#include "common_newchar.h"
#include "common_reputation.h" // Reputation::Vibe::SetClientID
#include "common_server.h"     // IServer::SetVisitedState
#include "common_misc.h"       // SinglePlayer
#include "common_universe.h"
#include "neuroncomponents.h"       // FDUMP
#include "gf_object_ship.h" // GfObjectShip (the routine) and its equipment entry points
#include "globals.h"       // SeedClientAccumulatedTime (the field)
#include "login_mgr.h"   // G_LoginMgr (the field)
#include "menu_ui_init.h" // PlayOrRewindUiSoundSlot (the routine)
#include "player_spawn.h" // GetPlayerConnectionId (the field)

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include <map>

namespace
{
    // — client EquipDescList singleton (a std::list<EquipDesc>, as in baseline).
    EquipDescList G_PlayerEquipDescList;

    // — collision-group desc list (baseline std::list at EquipDescList+0xc area).
    std::list<CollisionGroupDesc> G_PlayerCollisionGroups;

    // — selected ship archetype id.
    unsigned G_PlayerShipArchetypeId = 0;

    // — relative health scale (1.0 = full). baseline .bss (zero at load); written only
    // by the routine (0x004c4a94, = 0) and the routine (0x004c4f84), XREF[3] with the getter.
    float G_PlayerHealthScale = 0.0f;

    // — credits.
    unsigned G_PlayerCredits = 0;

    // / the field / the field -- the kill, mission-success and
    // mission-failure counts IClient slots 78..80 store through the routine / 80 / 90. The
    // listing has no reader of any of the three (each XREF[1] is that setter's own write);
    // uninitialised .data, so zero at load.
    unsigned G_PlayerNumKills = 0;
    unsigned G_PlayerMissionSuccesses = 0;
    unsigned G_PlayerMissionFailures = 0;

    // — scratch scalar (the routine/4660); meaning TBD, spawn copies it.
    unsigned G_PlayerSpawnScratchScalar = 0;

    // — the player's rank level (the routine / the routine); -1 while a
    // RequestRankLevel answer is pending (the routine).
    int G_PlayerRankLevel = 0;

    // — spawn system id written by the routine / read by the routine.
    int G_PlayerSpawnSystemId = -1;

    // / the field — star system / base (the routine/4910).
    unsigned G_PlayerStarSystemId = 0;
    unsigned G_PlayerBaseId = 0;

    // — faction / slot scratch (the routine).
    unsigned G_PlayerFactionScratch = 0;

    // ..ec / the field..e0 -- the player ship's last position and orientation,
    // written by the space frame (0x0054b192 / 0x0054b1b0) through the routine / the routine.
    float G_PlayerLastPosition[3] = {};
    float G_PlayerLastOrientation[9] = {};

    // cluster — 4-dword header + count @ +0x30 + up to 8 rank dwords.
    // Sibling the field cluster (the routine) for the other rank buffer.
    static const int kMaxRepScratchTail = 8;
    unsigned G_RepVibeHeader[4] = {};
    int G_RepVibeCount = 0;
    unsigned G_RepVibeTail[kMaxRepScratchTail] = {};

    unsigned G_RepRankHeader[4] = {};
    int G_RepRankCount = 0;
    unsigned G_RepRankTail[kMaxRepScratchTail] = {};

    // / the field — generation counters bumped by equip/collision sync.
    unsigned G_EquipListGeneration = 0;
    unsigned G_CollisionListGeneration = 0;

    // 0x30 bytes up to the field: the selected character's name.
    wchar_t G_PlayerCharacterName[24] = {};

    // the player's visit map: system (or object) id -> visit flags (baseline VC6
    // std::map<unsigned, unsigned char>: head the field, size the field, node key +0xc,
    // value +0x10). Built by the static initialiser the branch (the routine) and destroyed by
    // its atexit entry the branch; a namespace-scope std::map has the same lifetime.
    std::map<unsigned, unsigned char> G_PlayerVisitMap;

    // The body the routine and the routine share (0x004c3d20..0x004c3d74 /
    // ): the costume's four part dwords, its accessory count (src+0x30,
    // stored as it is), then that many accessory dwords from src+0x10 -- the loop bound re-read
    // from the stored count on every pass (JLE skips it for a count <= 0). No clamp, no null
    // test, and the rest of the tail keeps its old values.
    void CopyRepBuffer(unsigned* OutHeader4, int* OutCount, unsigned* OutTail,
                       unsigned const* SourceHeader4, int Count, unsigned const* TailDwords)
    {
        OutHeader4[0] = SourceHeader4[0];
        OutHeader4[1] = SourceHeader4[1];
        OutHeader4[2] = SourceHeader4[2];
        OutHeader4[3] = SourceHeader4[3];
        *OutCount = Count;
        for (int Index = 0; Index < *OutCount; ++Index)
            OutTail[Index] = TailDwords[Index];
    }

    // The three members the routine writes live on the connect context itself,
    // G_LoginMgr (the field, login_mgr.h): Characters (+0x1b4), SaveFailureCount
    // (+0x1cc) and bCharacterInfoArrived (+0x1d4).
}

unsigned GetPlayerShipArchetypeIdbaseline()
{
    return G_PlayerShipArchetypeId;
}

EquipDescList& GetPlayerEquipDescListbaseline()
{
    return G_PlayerEquipDescList;
}

unsigned GetPlayerEquipRevisionbaseline()
{
    // MOV EAX,[the field] / RET.
    return G_EquipListGeneration;
}

unsigned GetPlayerCollisionRevisionbaseline()
{
    // MOV EAX,[the field] / RET.
    return G_CollisionListGeneration;
}

bool ModifyPlayerEquipbaseline(unsigned short Id, CacheString const* Hardpoint, int Count,
                             float Health, unsigned char bEquipped)
{
    // (cdecl). Added by the NN_Inventory lane (its first ported caller is
    // , nn_inventory.cpp).
    EquipDesc* const Item = G_PlayerEquipDescList.find_equipment_item(Id);
    if (Item == nullptr)
        return false;
    ++G_EquipListGeneration;

    // , 004c4507..004c4528: the same hardpoint (pointer compare) -- update
    // the item where it is.
    if (Item->Hardpoint == Hardpoint->Value)
    {
        Item->set_count(Count);
        Item->set_status(Health);
        Item->set_equipped(bEquipped);
        return true;
    }

    // another hardpoint -- copy the item, remove it whole, and add the
    // copy back with the new values (combined when its archetype is combinable).
    EquipDesc Moved(*Item);
    int const Removed = G_PlayerEquipDescList.remove_equipment_item(Id, -1);
    Moved.set_hardpoint(*Hardpoint);
    Moved.set_count(Count);
    Moved.set_status(Health);
    Moved.set_equipped(bEquipped);
    int const Added = G_PlayerEquipDescList.add_equipment_item(Moved, arch_is_combinable(Moved.ArchId));

    // when the GF ObjectShip singleton (the routine, the field) exists,
    // (Removed, Moved.ArchId, Moved.Id, &Moved.Hardpoint, Moved.Count) and
    // (Added, Moved.ArchId, Moved.Id, Hardpoint, Count, Health) tell it about the
    // move (the copy sits at ESP+0x10: +0x12 Id, +0x14 ArchId, +0x18 Hardpoint, +0x24 Count).
    FObjectShip* const Ship = GfObjectShip();
    if (Ship != nullptr)
    {
        Ship->UnmountEquipment(Removed, Moved.ArchId, Moved.Id,
                               reinterpret_cast<CacheString const*>(&Moved.Hardpoint),
                               static_cast<unsigned>(Moved.Count));
        Ship->MountEquipment(Added, Moved.ArchId, Moved.Id, Hardpoint, Count, Health);
    }
    return true;
}

EquipDesc* FindPlayerNonMissionEquipbaseline(unsigned ArchId)
{
    // traverse_equipment_type(ArchId, null), then (ArchId, previous) until an
    // entry whose byte +0x18 (bMission) is zero -- that entry is returned, null at the end.
    EquipDesc* Desc = G_PlayerEquipDescList.traverse_equipment_type(ArchId, nullptr);
    while (Desc != nullptr)
    {
        if (!Desc->bMission)
        {
            return Desc;
        }
        Desc = G_PlayerEquipDescList.traverse_equipment_type(ArchId, Desc);
    }
    return nullptr;
}

void AddPlayerEquipbaseline(unsigned char UnusedFlag, EquipDesc const* Desc)
{
    // (cdecl, plain RET). The first argument is never read (0x004c4177 reads only
    // Stack[0x8]).
    (void)UnusedFlag;
    // the field++ before anything else.
    ++G_EquipListGeneration;
    // add_equipment_item(desc, arch_is_combinable(desc.ArchId)).
    int const Added = G_PlayerEquipDescList.add_equipment_item(*Desc, arch_is_combinable(Desc->ArchId));
    // with the GF ObjectShip singleton (the routine),
    // (Added, ArchId +4, Id +2, &Hardpoint +8, Count +0x14, Health +0x10).
    FObjectShip* const Ship = GfObjectShip();
    if (Ship != nullptr)
    {
        Ship->MountEquipment(Added, Desc->ArchId, Desc->Id,
                             reinterpret_cast<CacheString const*>(&Desc->Hardpoint), Desc->Count,
                             Desc->Health);
    }
}

bool RemovePlayerEquipbaseline(unsigned char UnusedFlag, unsigned short Id, int Count)
{
    // (cdecl). The first argument is never read (only Stack[0x8] / Stack[0xc]).
    (void)UnusedFlag;
    // find_equipment_item(Id); none -> false.
    EquipDesc* const Item = G_PlayerEquipDescList.find_equipment_item(Id);
    if (Item == nullptr)
        return false;
    // the item copied field by field before it goes; the field bumped.
    EquipDesc const Removed(*Item);
    ++G_EquipListGeneration;
    // remove_equipment_item(Id, Count); true from here on.
    int const RemovedIndex = G_PlayerEquipDescList.remove_equipment_item(Id, Count);
    // when the GF ObjectShip singleton (the routine, the field) exists,
    // (RemovedIndex, Removed.ArchId, Removed.Id, &Removed.Hardpoint, Count) tells
    // it (the copy sits at ESP+0xc: +0xe Id, +0x10 ArchId, +0x14 Hardpoint; Count is EDI).
    FObjectShip* const Ship = GfObjectShip();
    if (Ship != nullptr)
    {
        Ship->UnmountEquipment(RemovedIndex, Removed.ArchId, Removed.Id,
                               reinterpret_cast<CacheString const*>(&Removed.Hardpoint),
                               static_cast<unsigned>(Count));
    }
    return true;
}

std::list<CollisionGroupDesc>& GetPlayerCollisionGroupListbaseline()
{
    return G_PlayerCollisionGroups;
}

unsigned const* GetPlayerRepVibeScratchbaseline()
{
    return G_RepVibeHeader;
}

int GetPlayerRepVibeScratchCountbaseline()
{
    return G_RepVibeCount;
}

unsigned const* GetPlayerRepVibeScratchTailbaseline()
{
    return G_RepVibeTail;
}

unsigned GetPlayerSpawnScratchScalarbaseline()
{
    return G_PlayerSpawnScratchScalar;
}

int GetPlayerSpawnSystemIdbaseline()
{
    return G_PlayerSpawnSystemId;
}

float GetPlayerHealthScalebaseline()
{
    return G_PlayerHealthScale;
}

int GetPlayerRankLevelbaseline()
{
    // MOV EAX,[the field].
    return G_PlayerRankLevel;
}

void SetPlayerRankLevelbaseline(int Level)
{
    // (cdecl): [the field] = Level. Caller 0x0053c467.
    G_PlayerRankLevel = Level;
}

unsigned GetPlayerCreditsbaseline()
{
    return G_PlayerCredits;
}

unsigned GetPlayerStarSystemIdbaseline()
{
    return G_PlayerStarSystemId;
}

unsigned GetPlayerBaseIdbaseline()
{
    return G_PlayerBaseId;
}

unsigned GetPlayerFactionScratchbaseline()
{
    return G_PlayerFactionScratch;
}

void SetPlayerShipArchetypeIdbaseline(unsigned ShipArchId)
{
    // (cdecl). 0x004c3e20..0x004c3e2d: unchanged -> nothing.
    if (G_PlayerShipArchetypeId == ShipArchId)
        return;

    // the field = ShipArchId; 0x004c3e40..0x004c3e71: every node of the
    // collision-group list the field unlinked and freed (the inlined erase loop). No
    // generation counter here (the field is bumped by the routine and the routine only).
    G_PlayerShipArchetypeId = ShipArchId;
    G_PlayerCollisionGroups.clear();

    // a real id -> Archetype::GetShip; found ->
    // get_undamaged_collision_group_list(&the field); not found -> FDUMP.
    if ((ShipArchId & 0x3fffffffU) != 0)
    {
        Archetype::Ship* ShipArch = Archetype::GetShip(ShipArchId);
        if (ShipArch != nullptr)
        {
            ShipArch->get_undamaged_collision_group_list(G_PlayerCollisionGroups);
        }
        else
        {
            FDUMP(0x100001, "%s(%d) : *** ERROR: Error retrieving ship type %u",
                  "Client\\nn\\Player.cpp", 0xae, ShipArchId);
        }
    }

    // with the GF ObjectShip up, the routine(&the field),
    // (the field), then tail-JMP the routine.
    FObjectShip* const Ship = GfObjectShip();
    if (Ship == nullptr)
        return;
    Ship->SetEquipment(&G_PlayerEquipDescList);
    Ship->SetShipArchetype(G_PlayerShipArchetypeId); // (the field)
    Ship->RebuildModel();
}

void SyncPlayerEquipDescListbaseline(EquipDescList const& Source)
{
    // — the singleton list is emptied first, unconditionally (inlined node
    // erase loop, module storage-004c40ac), and only then list::operator= from Source runs
    // when Source is not the singleton itself (module storage). baseline has no
    // EquipDescList::clear export; this is the inlined container clear.
    G_PlayerEquipDescList.Items.clear();
    if (&Source != &G_PlayerEquipDescList)
        G_PlayerEquipDescList.Items = Source.Items;
    ++G_EquipListGeneration;
}

void SyncPlayerCollisionGroupListbaseline(std::list<CollisionGroupDesc> const& Source)
{
    // (cdecl, RET). 0x004c3f34: Source == &the field skips straight to the
    // counter. 0x004c3f44..0x004c3fe1 is VC6 std::list::operator= inlined: overwrite the
    // existing nodes value by value (8 bytes each), erase the surplus (the routine, size--),
    // append the rest (operator new(0x10), size++) -- the same result as this operator=.
    if (&Source != &G_PlayerCollisionGroups)
        G_PlayerCollisionGroups = Source;
    // the field++ on both paths.
    ++G_CollisionListGeneration;
    // when the GF ObjectShip singleton exists (the routine ->
    // ), tail-JMP thunk_FUN_00447210 on it (RebuildModel).
    FObjectShip* const Ship = GfObjectShip();
    if (Ship != nullptr)
        Ship->RebuildModel();
}

void SetPlayerRepRankBufferbaseline(unsigned const* SourceHeader4, int Count,
                                  unsigned const* TailDwords)
{

    CopyRepBuffer(G_RepRankHeader, &G_RepRankCount, G_RepRankTail, SourceHeader4, Count, TailDwords);
}

void SetPlayerRepVibeScratchbaseline(unsigned const* SourceHeader4, int Count,
                                   unsigned const* TailDwords)
{

    CopyRepBuffer(G_RepVibeHeader, &G_RepVibeCount, G_RepVibeTail, SourceHeader4, Count, TailDwords);
}

void SetPlayerSpawnScratchScalarbaseline(unsigned Value)
{
    G_PlayerSpawnScratchScalar = Value;
}

void SetPlayerSpawnSystemIdbaseline(int SystemId)
{
    G_PlayerSpawnSystemId = SystemId;
}

void SetPlayerHealthScalebaseline(float Scale)
{
    G_PlayerHealthScale = Scale;
}

void SetPlayerCreditsbaseline(unsigned Credits, int bFromCharacterSelect)
{
    // (cdecl). With the byte param_2 set (character select, 0x004c4d17) a plain
    // store. Otherwise (IClient SETCASH, 0x005a5f70) 0x004c4cfd `CMP ESI,[the field] / JLE`
    // -- a signed compare -- plays UI sound slot 0x26 through the routine only when the new
    // amount is GREATER than the old one, then stores it.
    if (bFromCharacterSelect == 0
        && static_cast<int>(Credits) > static_cast<int>(G_PlayerCredits))
        PlayOrRewindUiSoundSlot(0x26);
    G_PlayerCredits = Credits;
}

void SetPlayerNumKillsbaseline(unsigned Kills)
{
    // MOV EAX,[ESP+4] / MOV [the field],EAX / RET.
    G_PlayerNumKills = Kills;
}

void SetPlayerMissionSuccessesbaseline(unsigned Successes)
{
    // MOV EAX,[ESP+4] / MOV [the field],EAX / RET.
    G_PlayerMissionSuccesses = Successes;
}

void SetPlayerMissionFailuresbaseline(unsigned Failures)
{
    // MOV EAX,[ESP+4] / MOV [the field],EAX / RET.
    G_PlayerMissionFailures = Failures;
}

namespace
{
    // The tail the routine (004c484f..004c48f1) and the routine (004c4943..004c49e5) share
    // byte for byte: the system's own visit byte (ISystem vt+0x48); with neither 0x80 nor 0x01
    // set, bit 0 is added and the result goes through the body of the routine with the server
    // notification on.
    void MarkPlayerSystemVisited(unsigned SystemId)
    {
        Universe::ISystem const* const System = Universe::get_system(SystemId);
        if (System == nullptr)
            return;
        unsigned char Flags = System->GetVisit();
        if ((Flags & 0x81u) != 0)
            return;
        Flags = static_cast<unsigned char>(Flags | 1u);
        SetPlayerVisitFlagsbaseline(G_PlayerStarSystemId, Flags, true);
    }
}

void SetPlayerStarSystemIdbaseline(unsigned SystemId)
{
    // the field = SystemId, then the visit tail on that system.
    G_PlayerStarSystemId = SystemId;
    MarkPlayerSystemVisited(SystemId);
}

void SetPlayerBaseIdbaseline(unsigned BaseId)
{
    // (baseline takes the id through a pointer). the field = BaseId; a base id
    // with any of the low 30 bits set resolves its system -- Universe::get_base, then the base's
    // vt+0x10 (the port's IBase::SystemId), with no null test on either -- into the field,
    // and the visit tail runs on it.
    G_PlayerBaseId = BaseId;
    if ((BaseId & 0x3fffffffU) == 0)
        return;
    Universe::IBase* const Base = Universe::get_base(BaseId);
    G_PlayerStarSystemId = Base->SystemId;
    MarkPlayerSystemVisited(G_PlayerStarSystemId);
}

bool GetPlayerVisitFlagsbaseline(unsigned Id, unsigned char* OutFlags, unsigned Arg3,
                               unsigned Arg4)
{
    // lower_bound (the routine), then the key test -- a find. A hit writes the
    // node's flags (+0x10) and returns 1; a miss returns 0 and leaves *OutFlags alone. The
    // third and fourth arguments are never read (0x004c4da0..0x004c4df1 touch only
    // [ESP+0xc] and [ESP+0x10] of the frame, i.e. arguments 1 and 2).
    (void)Arg3;
    (void)Arg4;
    std::map<unsigned, unsigned char>::const_iterator const It = G_PlayerVisitMap.find(Id);
    if (It == G_PlayerVisitMap.end())
        return false;
    *OutFlags = It->second;
    return true;
}

void SetPlayerVisitFlagsbaseline(unsigned Id, unsigned char Flags, bool bNotifyServer)
{
    // .
    // find (the routine); an entry that already has every bit of Flags
    // is left as it is.
    std::map<unsigned, unsigned char>::iterator const Found = G_PlayerVisitMap.find(Id);
    if (Found != G_PlayerVisitMap.end() &&
        static_cast<unsigned char>(Found->second & Flags) == Flags)
        return;

    // insert {Id, 0} (the routine) and store Flags over the node's value
    // -- a plain store, not an OR with what was there.
    std::pair<std::map<unsigned, unsigned char>::iterator, bool> const Inserted =
        G_PlayerVisitMap.insert(std::make_pair(Id, static_cast<unsigned char>(0)));
    Inserted.first->second = Flags;

    if (!bNotifyServer)
        return;
    // IServer vt+0x11c SetVisitedState(the field, record, 9); the
    // record is one entry: count 1, the id, the flags byte.
    unsigned char Record[9];
    int const Count = 1;
    memcpy(Record, &Count, 4);
    memcpy(Record + 4, &Id, 4);
    Record[8] = Flags;
    G_Server->SetVisitedState(GetPlayerConnectionId(), Record, 9);
}

void ClearPlayerVisitMapbaseline()
{
    // The map clear the routine (004c4b16..004c4ba1) and the routine (004c4c27..004c4ca2)
    // inline: the whole tree freed, or node-by-node erase.
    G_PlayerVisitMap.clear();
}

void ClearPlayerShipArchAndHealthbaseline()
{
    // / 0x004c4a94 (the routine only): two plain stores, not the setters.
    G_PlayerShipArchetypeId = 0;
    G_PlayerHealthScale = 0.0f;
}

void SetPlayerCollisionGroupHealthbaseline(unsigned short Id, float Health)
{
    // (cdecl): the first node of the field whose id (+0x08) is Id gets the
    // health (+0x0c); the field is bumped whether or not one matched (0x004c401d /
    // ).
    for (std::list<CollisionGroupDesc>::iterator It = G_PlayerCollisionGroups.begin();
         It != G_PlayerCollisionGroups.end(); ++It)
    {
        if (It->Id == Id)
        {
            It->Health = Health;
            break;
        }
    }
    ++G_CollisionListGeneration;
}

void SetPlayerFactionScratchbaseline(unsigned Value)
{
    G_PlayerFactionScratch = Value;
}

float const* GetPlayerLastPositionbaseline()
{
    // MOV EAX, the field / RET.
    return G_PlayerLastPosition;
}

void SetPlayerLastPositionbaseline(float const* Position)
{
    // three dword copies from the argument.
    G_PlayerLastPosition[0] = Position[0];
    G_PlayerLastPosition[1] = Position[1];
    G_PlayerLastPosition[2] = Position[2];
}

float const* GetPlayerLastOrientationbaseline()
{
    // MOV EAX, the field / RET.
    return G_PlayerLastOrientation;
}

void SetPlayerLastOrientationbaseline(float const* Orientation)
{
    // REP MOVSD of nine dwords from the argument.
    for (int Index = 0; Index < 9; ++Index)
        G_PlayerLastOrientation[Index] = Orientation[Index];
}

void InitCreateParmsScratchbaseline(void* ParmsBytes)
{
    // (thiscall, returns this) -- the exe's compiled CSimple::CreateParms default
    // constructor: +0x00..+0x58 zero except HitPts +0x0c = -1.0f and the matrix diagonal
    // +0x1c / +0x2c / +0x3c = 1.0f. No null test.
    unsigned* Words = static_cast<unsigned*>(ParmsBytes);
    for (int Index = 0; Index < 0x17; ++Index)
        Words[Index] = 0;
    Words[3] = 0xbf800000;  // -1.0f HitPts sentinel style
    Words[7] = 0x3f800000;
    Words[0xb] = 0x3f800000;
    Words[0xf] = 0x3f800000;
}

bool ApplyCharacterSelectEquipFields(CCharacterSet::CharacterInfo const* CharInfo)
{
    // extract tail (module storage .. module storage, after the
    // IServer::CharacterSelect wait succeeds). Every read below is a named member
    // of the shared record (src/include/server_characterset.h); the baseline
    // offset is on each line.
    if (CharInfo == nullptr)
        return false;

    SetPlayerSpawnScratchScalarbaseline(CharInfo->VoiceId);          // +0x26c the routine
    SetPlayerHealthScalebaseline(CharInfo->HullStatus);              // +0x244 the routine
    SetPlayerShipArchetypeIdbaseline(CharInfo->ShipArchetype);       // +0x230 the routine

    // +0x254 the routine, then +0x248 the routine (module storage LEA ECX,[ESI+0x248];
    // PUSH ECX): the record's own EquipDescList (server_characterset.h) is handed over and
    // list-assigned into the client singleton.
    SyncPlayerCollisionGroupListbaseline(CharInfo->CollisionGroups);
    SyncPlayerEquipDescListbaseline(CharInfo->Equipment);

    SetPlayerCreditsbaseline(static_cast<unsigned>(CharInfo->Money), 1);    // +0x234 the routine

    // +0x2a8 the routine and +0x274 the routine: the base and the comm Costume
    // (four parts, the accessory count at +0x30, the accessory ids).
    Costume const& BaseCostume = CharInfo->BaseCostume;
    SetPlayerRepRankBufferbaseline(BaseCostume.PartIds, static_cast<int>(BaseCostume.AccessoryCount),
                                 BaseCostume.AccessoryIds);
    Costume const& ComCostume = CharInfo->ComCostume;
    SetPlayerRepVibeScratchbaseline(ComCostume.PartIds, static_cast<int>(ComCostume.AccessoryCount),
                                  ComCostume.AccessoryIds);

    unsigned BaseId = CharInfo->BaseId;                            // +0x264
    if ((BaseId & 0x3fffffffU) != 0)
    {
        SetPlayerStarSystemIdbaseline(0);
        SetPlayerBaseIdbaseline(BaseId);
    }
    else
    {
        SetPlayerBaseIdbaseline(0);
        SetPlayerStarSystemIdbaseline(CharInfo->SystemId);           // +0x260
    }

    SetPlayerFactionScratchbaseline(CharInfo->LocationId);           // +0x268 the routine

    // .. module storage -- wcscpy(the field, CharInfo->Name); an empty
    // name is the _Nullstr, i.e. L"". Unbounded, as baseline (the buffer is 24 wchars).
    wcscpy(G_PlayerCharacterName, CharInfo->Name.c_str());

    return true;
}

void CharacterSelect_ReceiveCharacterInfo(CCharacterSet const& Set)
{
    // (module storage .. module storage, thiscall on the field,
    // `RET 4`), reached from IClient slot 13 (the branch).
    // +0x1d4 = 1 before anything else.
    G_LoginMgr.bCharacterInfoArrived = 1;

    // .. module storage -- the traverse idiom over the set's list.
    // Each node's record is copied into a fresh record (operator new(0x2dc) and the
    // inlined constructor, module storage .. module storage) and appended to +0x1b4
    // (the routine). The node's Data is used without a null test (module storage).
    MetaNode<CCharacterSet::CharacterInfo>* Node = nullptr;
    while (Set.Characters.traverse(Node, true))
    {
        CCharacterSet::CharacterInfo const* Source = Node->Data;
        CCharacterSet::CharacterInfo* Copy = new CCharacterSet::CharacterInfo();
        Copy->Id = Source->Id;
        Copy->bIsRestart = Source->bIsRestart;
        Copy->bIsAutoSave = Source->bIsAutoSave;
        Copy->Name = Source->Name;
        Copy->Description = Source->Description;
        Copy->DescripStrId = Source->DescripStrId;
        Copy->TimestampLow = Source->TimestampLow;
        Copy->TimestampHigh = Source->TimestampHigh;
        Copy->ShipArchetype = Source->ShipArchetype;
        Copy->Money = Source->Money;
        Copy->NumKills = Source->NumKills;
        Copy->NumMissionSuccesses = Source->NumMissionSuccesses;
        Copy->NumMissionFailures = Source->NumMissionFailures;
        Copy->HullStatus = Source->HullStatus;
        Copy->Equipment = Source->Equipment;
        Copy->CollisionGroups = Source->CollisionGroups;
        Copy->SystemId = Source->SystemId;
        Copy->BaseId = Source->BaseId;
        Copy->LocationId = Source->LocationId;
        Copy->VoiceId = Source->VoiceId;
        Copy->Rank = Source->Rank;
        CharacterInfoCopyCostume(Copy->ComCostume, Source->ComCostume);   // the routine
        CharacterInfoCopyCostume(Copy->BaseCostume, Source->BaseCostume); // the routine
        G_LoginMgr.Characters.append(Copy);                        // the routine
    }

    // .. module storage -- +0x1cc = the set's +0x14.
    G_LoginMgr.SaveFailureCount = Set.SaveFailureCount;
}

wchar_t* GetPlayerCharacterNamebaseline()
{
    return G_PlayerCharacterName;
}

OwnerList<CCharacterSet::CharacterInfo>& CharacterSelect_GetConnectCharacters()
{
    return G_LoginMgr.Characters;
}

void NotifyCharacterSelectComplete(unsigned const Response4[4])
{
    // after its first store (module storage, CLoginMgr+0x21c = 1, done by
    // CLoginMgr::OnCharacterSelectResponse). The response is read without a null test.
    SetPlayerSpawnSystemIdbaseline(static_cast<int>(Response4[0])); // (005ab38f)

    if (!SinglePlayer())
    {
        // Reputation::Vibe::SetClientID(Response[0], the field).
        Reputation::Vibe::SetClientID(static_cast<int>(Response4[0]), GetPlayerConnectionId());
    }

    // / 005ab3c9..005ab3d5: both paths store +8 / +0xc into
    // / the field -- the low and high dwords of the client clock double that
    // advances every frame (client_timing.cpp), copied bit for bit.
    double ClientTime = 0.0;
    memcpy(&ClientTime, &Response4[2], sizeof(ClientTime));
    SeedClientAccumulatedTime(ClientTime);
}
