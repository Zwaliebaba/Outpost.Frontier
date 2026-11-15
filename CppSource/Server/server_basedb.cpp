// Author: Giperion.

// gf/BaseDB.cpp -- Server.dll's runtime base / room database.
// See src/include/server_basedb.h for the object model and the RTTI evidence.
// Every function below names the baseline address it was traced from.

#include "../include/server_basedb.h"
#include "../include/common_server.h"          // Players, PlayerDB
#include "../include/common_server_playerdb.h" // PlayerData
#include "../include/common_reputation.h"      // Reputation::Vibe::GetName
#include "../include/common_misc.h"            // malloc_strcpy, malloc_wstrcpy
#include "../include/common_net.h"             // MarshalBuffer
#include "../include/common_id.h"              // CreateID
#include "../include/common_text.h"            // FmtStr
#include "../include/rpc_iclient.h"            // IClient
#include "../include/neuroncomponents.h"                  // FDUMP
#include <cstring>
#include <stdexcept>

// , owned by IServerImpl.cpp.
extern IClient* g_pClientInterface;

namespace
{
    char const* const GBaseDBFile = "Server\\gf\\BaseDB.cpp";

    // The five handle pools (see FHandlePool). Static storage is zero-filled,
    // which is all their __xc initializers the branch / 06d122d0 / 06d12500 /
    // / 06d13330 do.
    FHandlePool GCharacterHandles;
    FHandlePool GBehaviorHandles;
    FHandlePool GAmbientScriptHandles;
    FHandlePool GMissionVendorHandles;
    FHandlePool GNewsHandles;

    // std::bitset<4096>'s _Xran -- builds
    // std::out_of_range("invalid bitset<N> position") and throws it (ThrowInfo
    // ). It never returns.
    void ThrowBitsetPosition()
    {
        throw std::out_of_range("invalid bitset<N> position");
    }

    // (thiscall on the player slot): the character name through the
    // reputation vibe at slot+0x318, falling back to u">>>BAD   NAME<<<"
    // (module storage) when Reputation::Vibe::GetName returns -2. The returned
    // pointer aliases the vibe's own name buffer. The same helper is ported
    // file-locally in server_chat.cpp and server_classes.cpp.
    wchar_t const* PlayerData_GetName(PlayerData const& Slot)
    {
        static wchar_t const BadName[] = L">>>BAD   NAME<<<";
        wchar_t const* DisplayName = BadName;
        FmtStr Name1;
        FmtStr Name2;
        if (::Reputation::Vibe::GetName(Slot.rep_vibe, Name1, Name2, DisplayName) == -2)
            DisplayName = BadName;
        return DisplayName;
    }
}

// ============================================================================
// FSessionList
// ============================================================================

// See FSessionList::free in server_basedb.h (module storage, ICF-folded
// OwnerList<CargoDesc>::free; inlined at module storage .. 06d11635).
void FSessionList::free()
{
    // Phase 1: a null cursor starts at Head (module storage), otherwise steps
    // to Next (module storage); every payload is deleted and its slot nulled.
    MetaNode<unsigned int>* Node = nullptr;
    while (List.traverse(Node, true))
    {
        delete Node->Data;
        Node->Data = nullptr;
    }
    // Phase 2: every node to FreeList (bUseHeap) or deleted; Tail = Head = 0.
    List.free();
}

// .
bool FSessionList::contains(unsigned int SessionId) const
{
    MetaNode<unsigned int>* Node = List.get_head();
    while (Node != nullptr)
    {
        if (*Node->Data == SessionId)
            break;
        Node = Node->Next;
    }
    return Node != nullptr;
}

// module storage duplicate scan; operator new(4) payload;
// (alloc_node); {Next 0, Prev Tail, Data}; tail link.
void FSessionList::add(unsigned int SessionId)
{
    if (contains(SessionId))
        return;
    unsigned int* Payload = new unsigned int;
    *Payload = SessionId;
    List.append(Payload);
}

// the first node whose payload matches (return if none); the
// payload is deleted first (module storage), then the node is unlinked
// (head / tail / middle, as MetaList::remove) and pushed on the free list
// when bUseHeap is set, else deleted.
void FSessionList::remove(unsigned int SessionId)
{
    MetaNode<unsigned int>* Node = List.get_head();
    while (Node != nullptr)
    {
        if (*Node->Data == SessionId)
            break;
        Node = Node->Next;
    }
    if (Node == nullptr)
        return;
    delete Node->Data;
    List.remove(Node);
}

// ============================================================================
// Record vtable slots. send_update: MarshalBuffer ctor, use_default,
// <Props>::marshal, IClient slot (sid, buffer base [+0x00], NumWritten [+0x10]).
// send_destroy: IClient slot (sid, <owner id>, key). the field is called
// through with no null test in all ten.
// ============================================================================

// (IClient +0x78).
void CharacterRecord::send_update(unsigned int SessionId)
{
    MarshalBuffer Buf;
    Buf.use_default();
    Props.marshal(&Buf);
    g_pClientInterface->SendServerGFUpdateChar(SessionId, Buf.get_buffer(), (unsigned int)Buf.get_num_written());
}

// (IClient +0x74): (sid, [this+0x14] = Props.RoomId, [this+0xc] = Props.Key).
void CharacterRecord::send_destroy(unsigned int SessionId)
{
    g_pClientInterface->SendServerGFDestroyCharacter(SessionId, Props.RoomId, Props.Key);
}

// (IClient +0x80).
void BehaviorRecord::send_update(unsigned int SessionId)
{
    MarshalBuffer Buf;
    Buf.use_default();
    Props.marshal(&Buf);
    g_pClientInterface->SendServerGFScriptBehavior(SessionId, Buf.get_buffer(), (unsigned int)Buf.get_num_written());
}

// (IClient +0x84): (sid, [this+0x10], [this+0xc]).
void BehaviorRecord::send_destroy(unsigned int SessionId)
{
    g_pClientInterface->SendServerGFDestroyScriptBehavior(SessionId, Props.Field04, (unsigned int)Props.BehaviorId);
}

// (IClient +0x8c).
void AmbientScriptRecord::send_update(unsigned int SessionId)
{
    MarshalBuffer Buf;
    Buf.use_default();
    Props.marshal(&Buf);
    g_pClientInterface->SendServerGFUpdateAmbientScript(SessionId, Buf.get_buffer(), (unsigned int)Buf.get_num_written());
}

// (IClient +0x90): (sid, [this+0x10], [this+0xc]).
void AmbientScriptRecord::send_destroy(unsigned int SessionId)
{
    g_pClientInterface->SendServerGFDestroyAmbientScript(SessionId, Props.RoomId, (unsigned int)Props.Key);
}

// (IClient +0x9c).
void MissionComputerRecord::send_update(unsigned int SessionId)
{
    MarshalBuffer Buf;
    Buf.use_default();
    Props.marshal(&Buf);
    g_pClientInterface->SendServerGFUpdateMissionComputer(SessionId, Buf.get_buffer(), (unsigned int)Buf.get_num_written());
}

// (IClient +0x98): (sid, [this+0x10], [this+0xc]).
void MissionComputerRecord::send_destroy(unsigned int SessionId)
{
    g_pClientInterface->SendServerGFDestroyMissionComputer(SessionId, Props.BaseId, (unsigned int)Props.Key);
}

// (IClient +0xb0).
void NewsBroadcastRecord::send_update(unsigned int SessionId)
{
    MarshalBuffer Buf;
    Buf.use_default();
    Props.marshal(&Buf);
    g_pClientInterface->SendServerGFUpdateNewsBroadcast(SessionId, Buf.get_buffer(), (unsigned int)Buf.get_num_written());
}

// (IClient +0xac): (sid, [this+0x10], [this+0xc]).
void NewsBroadcastRecord::send_destroy(unsigned int SessionId)
{
    g_pClientInterface->SendServerGFDestroyNewsBroadcast(SessionId, Props.BaseId, (unsigned int)Props.Key);
}

// .
void FAmbientScriptList::send_all(unsigned int SessionId, unsigned int RoomId)
{
    MetaNode<AmbientScriptRecord>* Node = nullptr;
    while (Records.traverse(Node, true))
    {
        AmbientScriptRecord* Record = Node->Data;
        if (BroadcastRecord::session_passes(SessionId, Record->Filter))
            Record->send_update(SessionId);
    }
    g_pClientInterface->SendServerGFCompleteAmbientScriptList(SessionId, RoomId);
}

// ============================================================================
// FHandlePool
// ============================================================================

// (pos, value): `CMP pos,0xfff / JBE` else the routine; then OR or
// AND-NOT of the bit.
void FHandlePool::set(unsigned int Position, bool bValue)
{
    if (Position > 0xfff)
        ThrowBitsetPosition();
    if (bValue)
        Bits[Position >> 5] |= 1u << (Position & 0x1f);
    else
        Bits[Position >> 5] &= ~(1u << (Position & 0x1f));
}

// , instruction for instruction:
//   Pos = Last + 1; bSearching = true; Pos >= 0x1000 -> wrap (Pos = 0)
//   test:  bit clear -> bSearching ? (range check, set, Last = Pos, return Pos+1)
//                                  : return -1
//          bit set   -> !bSearching ? return -1
//                       ++Pos; Pos >= 0x1000 -> Pos = 0
//                       else Pos == Last -> bSearching = false
//          (module storage: Pos >= 0x1000 throws -- unreachable after a wrap)
int FHandlePool::allocate()
{
    bool bSearching = true;
    unsigned int Position = Last + 1;
    if (Position >= 0x1000)
        Position = 0;
    for (;;)
    {
        if (Position >= 0x1000)
            ThrowBitsetPosition();
        if (!test(Position))
        {
            if (!bSearching)
                return -1;
            set(Position, true);
            Last = Position;
            return (int)Position + 1;
        }
        if (!bSearching)
            return -1;
        ++Position;
        if (Position >= 0x1000)
            Position = 0;
        else if (Position == Last)
            bSearching = false;
    }
}

// The inlined copy (module storage .. module storage in the routine; the same
// bytes at 06d12538 / 06d13088 / 06d13368): identical scan, but the handle is
// the bit index itself and stays 0xffffffff when the scan gives up. The
// caller creates its record either way.
unsigned int FHandlePool::allocate_index()
{
    bool bSearching = true;
    unsigned int Position = Last + 1;
    if (Position >= 0x1000)
        Position = 0;
    for (;;)
    {
        if (Position >= 0x1000)
            ThrowBitsetPosition();
        if (!test(Position))
        {
            if (!bSearching)
                return 0xffffffff;
            set(Position, true);
            Last = Position;
            return Position;
        }
        if (!bSearching)
            return 0xffffffff;
        ++Position;
        if (Position >= 0x1000)
            Position = 0;
        else if (Position == Last)
            bSearching = false;
    }
}

// ============================================================================
// FRoomEntry
// ============================================================================

// the three record lists (Sessions pointer 0, MetaList<T> ctor),
// the session list (the routine), then the body the routine also has.
FRoomEntry::FRoomEntry()
{
    init();
}

// clear the three record lists (NPC and behaviour inlined,
// ambient through the routine), FSessionList::free on the session list (module storage),
// , then the member destructors in reverse order (the routine,
// then each record list's clear() + ~MetaList<T>).
FRoomEntry::~FRoomEntry()
{
    Characters.clear();
    Behaviors.clear();
    AmbientScripts.clear();
    Sessions.free();
    init();
}

// +0/+4/+8 = 0; +0x0c/+0x24/+0x3c = &Sessions; then
// `CALL pub::AI::ContentCallback::combat_drift_event` with ECX = &Sessions --
// an ICF fold onto an empty Common.dll body (a bare RET at 06ce1090), so the
// source called some empty member of the session list there. It has no effect
// and nothing is emitted for it.
void FRoomEntry::init()
{
    BaseId = 0;
    RoomId = 0;
    Data = nullptr;
    Characters.Sessions = &Sessions;
    Behaviors.Sessions = &Sessions;
    AmbientScripts.Sessions = &Sessions;
}

// (thiscall room, sid; RET 4), traced module storage ..
// . The player's own character record for this room.
void FRoomEntry::enter(unsigned int SessionId)
{
    // the routine on the NPC pool.
    int const Handle = GCharacterHandles.allocate();
    // operator new(0x70); BroadcastRecord / CharacterProperties ctors.
    CharacterRecord* Record = new CharacterRecord();
    Record->Props.Key = (unsigned int)Handle;
    Record->Props.SessionId = SessionId;
    Record->Props.RoomId = RoomId;                  // [room+4]
    Record->Props.NameIds = 0;
    Record->Props.FactionId = 0xffffffff;
    PlayerData& Slot = Players[SessionId];          // &Players[sid-1]
    malloc_wstrcpy(&Record->Props.NameWide, PlayerData_GetName(Slot));

    // .. module storage: a local copy of the 0x34-byte Costume at
    // slot+0x2e4, then the four part ids, the accessory count and the first
    // `count` accessories -- not the whole struct.
    Costume const Source = Slot.costume;
    Record->Props.Cost.PartIds[0] = Source.PartIds[0];
    Record->Props.Cost.PartIds[1] = Source.PartIds[1];
    Record->Props.Cost.PartIds[2] = Source.PartIds[2];
    Record->Props.Cost.PartIds[3] = Source.PartIds[3];
    Record->Props.Cost.AccessoryCount = Source.AccessoryCount;
    for (int Index = 0; Index < (int)Record->Props.Cost.AccessoryCount; ++Index)
        Record->Props.Cost.AccessoryIds[Index] = Source.AccessoryIds[Index];

    malloc_strcpy(&Record->Props.Script, "scripts\\extras\\player_fidget.thn");
    Record->Props.BehaviorId = 0xffffffff;
    Record->Props.VoiceId = CreateID(Slot.voice);   // CreateID(slot+0x2c4)
    Record->Props.NameAnsi = nullptr;
    Record->Props.CharType = 1;
    Record->Props.Posture = 0;
    Record->Filter = 0;
    Record->bDirty = true;

    // .. module storage: alloc_node + tail link.
    Characters.Records.append(Record);

    // .. module storage: every session of the room (the filter
    // test the routine inlined), slot 0.
    MetaNode<unsigned int>* Session = nullptr;
    while (Characters.Sessions->traverse(Session, true))
    {
        unsigned int const Sid = *Session->Data;
        if (BroadcastRecord::session_passes(Sid, Record->Filter))
            Record->send_update(Sid);
    }
    Record->bDirty = false;
}

// (thiscall room, props, filter; RET 8), traced module storage ..
// .
int FRoomEntry::create_character(CharacterProperties const& Props, unsigned int RecordFilter)
{
    int const Handle = GCharacterHandles.allocate();
    CharacterRecord* Record = new CharacterRecord();        // new(0x70)
    Record->Props = Props;                                  // operator=
    Record->Props.Key = (unsigned int)Handle;

    // .. module storage. The placement the description names
    // (Props+0x18) is looked up by name in the room's character placements;
    // its posture (+0x38) goes to Props+0x20. A miss leaves the index at -1
    // and baseline reads placements[-1] regardless (module storage ..
    // ); no name at all keeps the posture at -1.
    unsigned int Posture = 0xffffffff;
    char const* const PlacementName = Props.NameAnsi;
    if (PlacementName != nullptr)
    {
        int Found = -1;
        int Index = 0;
        for (;;)
        {
            // count = begin ? (end - begin) / 4 : 0, re-read each pass.
            int const Count = (int)Data->CharacterPlacements.size();
            if (Index >= Count)
                break;
            char const* const Name = Data->CharacterPlacements.data()[Index]->Name;
            if (Name != nullptr && _stricmp(Name, PlacementName) == 0)
            {
                Found = Index;
                break;
            }
            ++Index;
        }
        CharPlaceInfo const* const Placement = Data->CharacterPlacements.data()[Found];
        char const* const FoundName = Placement->Name;
        Posture = (unsigned int)Placement->Posture;

        // .. module storage: a walk over the room's characters
        // for one on the same placement that the filter admits. It only picks
        // where the loop stops; nothing reads the result.
        MetaNode<CharacterRecord>* Node = nullptr;
        while (Characters.Records.traverse(Node, true))
        {
            CharacterRecord const* const Other = Node->Data;
            if (Other->Props.NameAnsi == nullptr || FoundName == nullptr)
                continue;
            if (_stricmp(Other->Props.NameAnsi, FoundName) != 0)
                continue;
            if (RecordFilter == 0 || Other->Filter == 0 || Other->Filter == RecordFilter)
                break;
        }
    }

    Record->Props.CharType = 0;
    Record->Props.Posture = Posture;
    Record->Filter = RecordFilter;
    Record->bDirty = true;
    Characters.Records.append(Record);

    // .. module storage: the routine over *(room+0xc), filter inlined.
    MetaNode<unsigned int>* Session = nullptr;
    while (Characters.Sessions->traverse(Session, true))
    {
        unsigned int const Sid = *Session->Data;
        if (BroadcastRecord::session_passes(Sid, Record->Filter))
            Record->send_update(Sid);
    }
    Record->bDirty = false;
    return Handle;
}

// (thiscall room, props, filter; RET 8), traced module storage ..
// .
unsigned int FRoomEntry::create_behavior(ScriptBehavior const& Props, unsigned int RecordFilter)
{
    unsigned int const Handle = GBehaviorHandles.allocate_index();
    BehaviorRecord* Record = new BehaviorRecord();                 // new(0x26c)
    Record->Props = Props;
    Record->Props.BehaviorId = (int)Handle;
    Record->Filter = RecordFilter;
    Record->bDirty = true;
    Behaviors.Records.append(Record);

    // .. module storage: the room's sessions, the routine, slot 0.
    MetaNode<unsigned int>* Session = nullptr;
    while (Behaviors.Sessions->traverse(Session, true))
    {
        unsigned int const Sid = *Session->Data;
        if (BroadcastRecord::session_passes(Sid, Record->Filter))
            Record->send_update(Sid);
    }
    Record->bDirty = false;
    return Handle;
}

// (thiscall room, props, filter; RET 8): the routine's twin on
// the ambient pool (the field), a 0x20-byte AmbientScriptRecord and the
// list at room+0x40.
unsigned int FRoomEntry::create_ambient_script(AmbientScriptProperties const& Props, unsigned int RecordFilter)
{
    unsigned int const Handle = GAmbientScriptHandles.allocate_index();
    AmbientScriptRecord* Record = new AmbientScriptRecord();            // new(0x20)
    Record->Props = Props;
    Record->Props.Key = (int)Handle;
    Record->Filter = RecordFilter;
    Record->bDirty = true;
    AmbientScripts.Records.append(Record);

    MetaNode<unsigned int>* Session = nullptr;
    while (AmbientScripts.Sessions->traverse(Session, true))
    {
        unsigned int const Sid = *Session->Data;
        if (BroadcastRecord::session_passes(Sid, Record->Filter))
            Record->send_update(Sid);
    }
    Record->bDirty = false;
    return Handle;
}

// (thiscall room, filter, index; RET 8): the name of placement
// `index` (read unchecked, module storage), then the first character on that
// placement whose filter admits `filter` -- its key, else 0.
unsigned int FRoomEntry::get_character_on_placement(unsigned int SessionFilter, int PlacementIndex) const
{
    char const* const PlacementName = Data->CharacterPlacements.data()[PlacementIndex]->Name;
    MetaNode<CharacterRecord>* Node = nullptr;
    while (Characters.Records.traverse(Node, true))
    {
        CharacterRecord const* const Record = Node->Data;
        if (Record->Props.NameAnsi == nullptr || PlacementName == nullptr)
            continue;
        if (_stricmp(Record->Props.NameAnsi, PlacementName) != 0)
            continue;
        if (SessionFilter == 0 || Record->Filter == 0 || Record->Filter == SessionFilter)
            return Record->Props.Key;
    }
    return 0;
}

// ============================================================================
// FBaseEntry
// ============================================================================

// members, then the the routine body.
FBaseEntry::FBaseEntry()
{
    init();
}

// delete[] Rooms (the routine with the routine, then the
// count-prefixed block), clear the offer and news lists, FSessionList::free on
// the session list, the routine, then the member destructors.
FBaseEntry::~FBaseEntry()
{
    delete[] Rooms;
    MissionVendors.clear();
    NewsBroadcasts.clear();
    Sessions.free();
    init();
}

// +0/+4/+8/+0xc = 0; +0x10/+0x28 = &Sessions; then the same
// empty combat_drift_event fold the routine has.
void FBaseEntry::init()
{
    BaseId = 0;
    Data = nullptr;
    Rooms = nullptr;
    NumRooms = 0;
    MissionVendors.Sessions = &Sessions;
    NewsBroadcasts.Sessions = &Sessions;
}

// (thiscall, BaseData*; RET 4): Data, BaseId = BaseData+0,
// NumRooms = BaseData+0x2c (the room list's size), Rooms = new FRoomEntry[n]
// (no test of the allocation), then one pass over the room list copying
// RoomData+0 / +4 / the RoomData pointer into each room.
void FBaseEntry::load(BaseData* Source)
{
    Data = Source;
    BaseId = Source->BaseId;
    NumRooms = (int)Source->Rooms.size();
    Rooms = new FRoomEntry[NumRooms];
    int Index = 0;
    for (std::list<RoomData*>::iterator It = Source->Rooms.begin(); It != Source->Rooms.end(); ++It, ++Index)
    {
        RoomData* const Room = *It;
        Rooms[Index].BaseId = Room->BaseId;
        Rooms[Index].RoomId = Room->RoomId;
        Rooms[Index].Data = Room;
    }
}

// (thiscall base, sid; RET 4). Default = BaseData->StartRoomId
// ([entry+4]+4); 0 FDUMPs "No start room in base %u" (line 0x454) and carries
// on. Then, room by room, the first behaviour record whose ScriptBehavior+0x10
// is non-zero names the start room and ends the search. IClient +0x70.
void FBaseEntry::send_start_room(unsigned int SessionId)
{
    unsigned int const DefaultRoom = Data->StartRoomId;
    if (DefaultRoom == 0)
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: No start room in base %u\n", GBaseDBFile, 0x454, Data->BaseId);
    }

    unsigned int StartRoom = DefaultRoom;
    for (int Index = 0; Index < NumRooms; ++Index)
    {
        MetaNode<BehaviorRecord>* Node = Rooms[Index].Behaviors.Records.get_head();
        while (Node != nullptr && Node->Data->Props.Field10 == 0)
            Node = Node->Next;
        if (Node != nullptr)
        {
            StartRoom = Node->Data->Props.Field10;
            break;
        }
    }

    g_pClientInterface->SendServerSetStartRoom(SessionId, BaseId, StartRoom);
}

// (thiscall base, props, filter; RET 8): the routine's twin on
// the mission-vendor pool (the field), a 0x6c-byte MissionComputerRecord
// and the list at base+0x14.
unsigned int FBaseEntry::create_mission_vendor(MissionVendorProperties const& Props, unsigned int RecordFilter)
{
    unsigned int const Handle = GMissionVendorHandles.allocate_index();
    MissionComputerRecord* Record = new MissionComputerRecord();        // new(0x6c)
    Record->Props = Props;
    Record->Props.Key = (int)Handle;
    Record->Filter = RecordFilter;
    Record->bDirty = true;
    MissionVendors.Records.append(Record);

    MetaNode<unsigned int>* Session = nullptr;
    while (MissionVendors.Sessions->traverse(Session, true))
    {
        unsigned int const Sid = *Session->Data;
        if (BroadcastRecord::session_passes(Sid, Record->Filter))
            Record->send_update(Sid);
    }
    Record->bDirty = false;
    return Handle;
}

// (thiscall base, handle, accepted, reason, sid; RET 0x10): a
// local MissionVendorAcceptance {Key = handle, TargetId = this base's id,
// bAccepted, Reason}, marshalled into the default buffer and sent through
// IClient +0xa4 (sid, buffer base, NumWritten).
void FBaseEntry::mission_vendor_acceptance(unsigned long Handle, bool bAccepted, FmtStr const& Reason,
                                           unsigned int SessionId)
{
    MissionVendorAcceptance Acceptance;
    Acceptance.Key = (int)Handle;
    Acceptance.TargetId = BaseId;
    Acceptance.bAccepted = bAccepted;
    Acceptance.Reason = Reason;
    MarshalBuffer Buf;
    Buf.use_default();
    Acceptance.marshal(&Buf);
    g_pClientInterface->SendServerGFMissionVendorAcceptance(SessionId, Buf.get_buffer(), (unsigned int)Buf.get_num_written());
}

// the news twin (pool the field, 0x6c-byte
// NewsBroadcastRecord, list at base+0x2c).
unsigned int FBaseEntry::create_news_broadcast(NewsBroadcastProperties const& Props, unsigned int RecordFilter)
{
    unsigned int const Handle = GNewsHandles.allocate_index();
    NewsBroadcastRecord* Record = new NewsBroadcastRecord();            // new(0x6c)
    Record->Props = Props;
    Record->Props.Key = (int)Handle;
    Record->Filter = RecordFilter;
    Record->bDirty = true;
    NewsBroadcasts.Records.append(Record);

    MetaNode<unsigned int>* Session = nullptr;
    while (NewsBroadcasts.Sessions->traverse(Session, true))
    {
        unsigned int const Sid = *Session->Data;
        if (BroadcastRecord::session_passes(Sid, Record->Filter))
            Record->send_update(Sid);
    }
    Record->bDirty = false;
    return Handle;
}

// ============================================================================
// BaseDB
// ============================================================================

namespace
{
    // , built by the __xc initializer the routine and torn down by
    // the atexit thunk the routine -- exactly what a namespace-scope object
    // with a constructor and a destructor compiles to.
    BaseDB GBaseDB;
}

// .
BaseDB* GetBaseDB()
{
    return &GBaseDB;
}

// `CALL the routine / MOV ECX,EAX / JMP the routine`.
void Server_SubsystemInit()
{
    GetBaseDB()->init();
}

// `CALL the routine / MOV ECX,EAX / JMP the routine`.
void Server_SubsystemShutdown()
{
    GetBaseDB()->shutdown();
}

// `PUSH [ESP+4] / CALL the routine / MOV ECX,EAX / CALL the routine / RET`.
void Server_SubsystemUpdate(float DeltaTime)
{
    GetBaseDB()->update(DeltaTime);
}

// the seven maps, then Bases = 0, NumBases = 0.
BaseDB::BaseDB()
    : Bases(nullptr)
    , NumBases(0)
{
}

// clear the seven maps (the routine / 159d0 x2 / 15b20 /
// 15c70 x2 / 15b20), delete[] Bases (the routine with the routine),
// Bases = NumBases = 0, then the map destructors.
BaseDB::~BaseDB()
{
    BaseMap.clear();
    LocationMap.clear();
    CharacterMap.clear();
    BehaviorMap.clear();
    MissionVendorMap.clear();
    NewsMap.clear();
    AmbientScriptMap.clear();
    delete[] Bases;
    Bases = nullptr;
    NumBases = 0;
}

// , traced module storage .. module storage:
//   list = BaseDataList_get(); NumBases = list size ([list+8]);
//   Bases = new FBaseEntry[NumBases] (count-prefixed, the routine);
//   one load() per BaseData in list order;
//   for each base: BaseMap.insert(base id -> base), and for each of its rooms
//   LocationMap.insert(room id ([room+4]) -> room). Both inserts are the
//   unique std::map insert (the routine / the inlined _Insert).
// No latch, no null test of the list, no clearing beforehand.
void BaseDB::init()
{
    std::list<BaseData*>* const List = BaseDataList_get()->get_base_data_list();
    NumBases = (int)List->size();
    Bases = new FBaseEntry[NumBases];
    int Index = 0;
    for (std::list<BaseData*>::iterator It = List->begin(); It != List->end(); ++It, ++Index)
        Bases[Index].load(*It);

    for (int BaseIndex = 0; BaseIndex < NumBases; ++BaseIndex)
    {
        FBaseEntry* const Base = &Bases[BaseIndex];
        BaseMap.insert(std::map<unsigned int, FBaseEntry*>::value_type(Base->BaseId, Base));
        for (int RoomIndex = 0; RoomIndex < Base->NumRooms; ++RoomIndex)
        {
            FRoomEntry* const Room = &Base->Rooms[RoomIndex];
            LocationMap.insert(std::map<unsigned int, FRoomEntry*>::value_type(Room->RoomId, Room));
        }
    }
}

// erase(begin, end) on the seven maps (the routine / 15920 x2 /
// 15a70 / 15bc0 x2 / 15a70), delete[] Bases, Bases = NumBases = 0.
void BaseDB::shutdown()
{
    BaseMap.clear();
    LocationMap.clear();
    CharacterMap.clear();
    BehaviorMap.clear();
    MissionVendorMap.clear();
    NewsMap.clear();
    AmbientScriptMap.clear();
    delete[] Bases;
    Bases = nullptr;
    NumBases = 0;
}

// (thiscall, float; RET 4), traced module storage ..
// . The float is never read. For every base with a subscriber:
// every room with a subscriber re-broadcasts its dirty character, behaviour
// and ambient records (the routine) and clears their flags; then, when the
// base still has a subscriber (re-tested at module storage), its dirty
// mission-vendor and news records.
void BaseDB::update(float DeltaTime)
{
    (void)DeltaTime;
    for (int BaseIndex = 0; BaseIndex < NumBases; ++BaseIndex)
    {
        FBaseEntry* const Base = &Bases[BaseIndex];
        if (Base->Sessions.empty())
            continue;

        for (int RoomIndex = 0; RoomIndex < Base->NumRooms; ++RoomIndex)
        {
            FRoomEntry* const Room = &Base->Rooms[RoomIndex];
            if (Room->Sessions.empty())
                continue;

            MetaNode<CharacterRecord>* Character = nullptr;
            while (Room->Characters.Records.traverse(Character, true))
            {
                CharacterRecord* const Record = Character->Data;
                if (!Record->bDirty)
                    continue;
                Room->Characters.broadcast(Record);
                Record->bDirty = false;
            }

            MetaNode<BehaviorRecord>* Behavior = nullptr;
            while (Room->Behaviors.Records.traverse(Behavior, true))
            {
                BehaviorRecord* const Record = Behavior->Data;
                if (Record->bDirty)
                {
                    Room->Behaviors.broadcast(Record);
                    Record->bDirty = false;
                }
            }

            MetaNode<AmbientScriptRecord>* Ambient = nullptr;
            while (Room->AmbientScripts.Records.traverse(Ambient, true))
            {
                AmbientScriptRecord* const Record = Ambient->Data;
                if (Record->bDirty)
                {
                    Room->AmbientScripts.broadcast(Record);
                    Record->bDirty = false;
                }
            }
        }

        if (Base->Sessions.empty())
            continue;

        MetaNode<MissionComputerRecord>* Offer = nullptr;
        while (Base->MissionVendors.Records.traverse(Offer, true))
        {
            MissionComputerRecord* const Record = Offer->Data;
            if (!Record->bDirty)
                continue;
            Base->MissionVendors.broadcast(Record);
            Record->bDirty = false;
        }

        MetaNode<NewsBroadcastRecord>* News = nullptr;
        while (Base->NewsBroadcasts.Records.traverse(News, true))
        {
            NewsBroadcastRecord* const Record = News->Data;
            if (!Record->bDirty)
                continue;
            Base->NewsBroadcasts.broadcast(Record);
            Record->bDirty = false;
        }
    }
}

// std::map::find on BaseMap; hit -> the value; miss ->
// FDUMP "Base %u is invalid" (line 0x659), 0.
FBaseEntry* BaseDB::find_base(unsigned int Id)
{
    std::map<unsigned int, FBaseEntry*>::iterator It = BaseMap.find(Id);
    if (It != BaseMap.end())
        return It->second;
    FDUMP(0x100001, "%s(%d) : *** ERROR: Base %u is invalid\n", GBaseDBFile, 0x659, Id);
    return nullptr;
}

// the same on LocationMap, "Location %u is invalid" (0x66d).
FRoomEntry* BaseDB::find_location(unsigned int Id)
{
    std::map<unsigned int, FRoomEntry*>::iterator It = LocationMap.find(Id);
    if (It != LocationMap.end())
        return It->second;
    FDUMP(0x100001, "%s(%d) : *** ERROR: Location %u is invalid\n", GBaseDBFile, 0x66d, Id);
    return nullptr;
}

// (IServerImpl::BaseEnter module storage): find in BaseMap; miss
// -> FDUMP 0x659, return; a null value or NumRooms <= 0 does nothing; else
// RoomData::load on every room. SessionId is pushed but never read.
void BaseDB::load_base(unsigned int SessionId, unsigned int Id)
{
    (void)SessionId;
    std::map<unsigned int, FBaseEntry*>::iterator It = BaseMap.find(Id);
    if (It == BaseMap.end())
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: Base %u is invalid\n", GBaseDBFile, 0x659, Id);
        return;
    }
    FBaseEntry* const Base = It->second;
    if (Base == nullptr)
        return;
    for (int Index = 0; Index < Base->NumRooms; ++Index)
    {
        // `ADD EAX,EDI / JZ`: baseline tests the element address.
        FRoomEntry* const Room = &Base->Rooms[Index];
        if (Room != nullptr)
            Room->Data->load();
    }
}

// the routine with RoomData::unload. SessionId unused.
void BaseDB::unload_base(unsigned int SessionId, unsigned int Id)
{
    (void)SessionId;
    std::map<unsigned int, FBaseEntry*>::iterator It = BaseMap.find(Id);
    if (It == BaseMap.end())
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: Base %u is invalid\n", GBaseDBFile, 0x659, Id);
        return;
    }
    FBaseEntry* const Base = It->second;
    if (Base == nullptr)
        return;
    for (int Index = 0; Index < Base->NumRooms; ++Index)
    {
        // `ADD EAX,EDI / JZ`: baseline tests the element address.
        FRoomEntry* const Room = &Base->Rooms[Index];
        if (Room != nullptr)
            Room->Data->unload();
    }
}

// (sid, room id; RET 8): find in LocationMap; miss -> FDUMP 0x66d;
// a non-null value -> FRoomEntry::enter(sid).
void BaseDB::enter_location(unsigned int SessionId, unsigned int Id)
{
    std::map<unsigned int, FRoomEntry*>::iterator It = LocationMap.find(Id);
    if (It == LocationMap.end())
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: Location %u is invalid\n", GBaseDBFile, 0x66d, Id);
        return;
    }
    if (It->second != nullptr)
        It->second->enter(SessionId);
}

// (sid, room id; RET 8), traced module storage .. module storage:
//   find in LocationMap (miss -> FDUMP 0x66d, return; null value -> return);
//   the handle of the first character record whose Props.SessionId == sid
//   (0 when none);
//   (handle & 0x3fffffff) == 0 -> return;
//   the first record matching the handle, when there is one: send_destroy to
//   every session its filter admits, delete it (the routine), unlink
//   (the routine = MetaList::remove);
//   then, found or not, clear pool bit (handle - 1) (inline the routine with
//   the the routine range throw). CharacterMap is not touched.
void BaseDB::exit_location(unsigned int SessionId, unsigned int Id)
{
    std::map<unsigned int, FRoomEntry*>::iterator It = LocationMap.find(Id);
    if (It == LocationMap.end())
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: Location %u is invalid\n", GBaseDBFile, 0x66d, Id);
        return;
    }
    FRoomEntry* const Room = It->second;
    if (Room == nullptr)
        return;

    unsigned int Handle = 0;
    MetaNode<CharacterRecord>* Node = nullptr;
    while (Room->Characters.Records.traverse(Node, true))
    {
        if (Node->Data->Props.SessionId == SessionId)
        {
            Handle = Node->Data->Props.Key;
            break;
        }
    }
    if ((Handle & 0x3fffffff) == 0)
        return;

    Node = nullptr;
    while (Room->Characters.Records.traverse(Node, true))
    {
        CharacterRecord* const Record = Node->Data;
        if (!Record->matches(Handle))
            continue;
        MetaNode<unsigned int>* Session = nullptr;
        while (Room->Characters.Sessions->traverse(Session, true))
        {
            unsigned int const Sid = *Session->Data;
            if (BroadcastRecord::session_passes(Sid, Record->Filter))
                Record->send_destroy(Sid);
        }
        delete Record;
        Room->Characters.Records.remove(Node);
        break;
    }
    GCharacterHandles.set(Handle - 1, false);
}

// (sid, base id, bFull; RET 0xc), traced module storage ..

//   find in BaseMap (miss -> FDUMP 0x659, return; null value -> return);
//   !bFull -> Sessions.remove(sid), return;
//   already subscribed -> return;
//   Sessions.add(sid); send_start_room(sid);
//   every offer the session passes (the routine): slot 0; IClient +0xa0
//   (sid, base id); every news broadcast likewise; IClient +0xb4 (sid, base id).
void BaseDB::base_info(unsigned int SessionId, unsigned int Id, bool bFull)
{
    std::map<unsigned int, FBaseEntry*>::iterator It = BaseMap.find(Id);
    if (It == BaseMap.end())
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: Base %u is invalid\n", GBaseDBFile, 0x659, Id);
        return;
    }
    FBaseEntry* const Base = It->second;
    if (Base == nullptr)
        return;
    if (!bFull)
    {
        Base->Sessions.remove(SessionId);
        return;
    }
    if (Base->Sessions.contains(SessionId))
        return;
    Base->Sessions.add(SessionId);
    Base->send_start_room(SessionId);

    unsigned int const BaseId = Base->BaseId;
    MetaNode<MissionComputerRecord>* Offer = nullptr;
    while (Base->MissionVendors.Records.traverse(Offer, true))
    {
        if (BroadcastRecord::session_passes(SessionId, Offer->Data->Filter))
            Offer->Data->send_update(SessionId);
    }
    g_pClientInterface->SendServerGFCompleteMissionComputerList(SessionId, BaseId);

    MetaNode<NewsBroadcastRecord>* News = nullptr;
    while (Base->NewsBroadcasts.Records.traverse(News, true))
    {
        if (BroadcastRecord::session_passes(SessionId, News->Data->Filter))
            News->Data->send_update(SessionId);
    }
    g_pClientInterface->SendServerGFCompleteNewsBroadcastList(SessionId, BaseId);
}

// (sid, room id, bFull; RET 0xc), traced module storage ..
// the room twin of base_info. Characters (filter inlined) then
// IClient +0x7c (sid, room id); behaviours (the routine) then IClient +0x88;
// (ambient records + IClient +0x94). Room id = [room+4].
void BaseDB::location_info(unsigned int SessionId, unsigned int Id, bool bFull)
{
    std::map<unsigned int, FRoomEntry*>::iterator It = LocationMap.find(Id);
    if (It == LocationMap.end())
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: Location %u is invalid\n", GBaseDBFile, 0x66d, Id);
        return;
    }
    FRoomEntry* const Room = It->second;
    if (Room == nullptr)
        return;
    if (!bFull)
    {
        Room->Sessions.remove(SessionId);
        return;
    }
    if (Room->Sessions.contains(SessionId))
        return;
    Room->Sessions.add(SessionId);

    MetaNode<CharacterRecord>* Character = nullptr;
    while (Room->Characters.Records.traverse(Character, true))
    {
        if (BroadcastRecord::session_passes(SessionId, Character->Data->Filter))
            Character->Data->send_update(SessionId);
    }
    g_pClientInterface->SendServerGFCompleteCharList(SessionId, Room->RoomId);

    MetaNode<BehaviorRecord>* Behavior = nullptr;
    while (Room->Behaviors.Records.traverse(Behavior, true))
    {
        if (BroadcastRecord::session_passes(SessionId, Behavior->Data->Filter))
            Behavior->Data->send_update(SessionId);
    }
    g_pClientInterface->SendServerGFCompleteScriptBehaviorList(SessionId, Room->RoomId);

    Room->AmbientScripts.send_all(SessionId, Room->RoomId);
}

// (props, filter; RET 8): find Props.RoomId (+0x08) in
// LocationMap; a miss FDUMPs 0x66d and goes on with a null room; the node's
// value otherwise. the routine on that room -- null included -- and a unique
// CharacterMap insert of (handle -> room). Returns the handle.
int BaseDB::create_character(CharacterProperties const& Props, unsigned int RecordFilter)
{
    FRoomEntry* Room = nullptr;
    std::map<unsigned int, FRoomEntry*>::iterator It = LocationMap.find(Props.RoomId);
    if (It != LocationMap.end())
        Room = It->second;
    else
        FDUMP(0x100001, "%s(%d) : *** ERROR: Location %u is invalid\n", GBaseDBFile, 0x66d, Props.RoomId);
    int const Handle = Room->create_character(Props, RecordFilter);
    CharacterMap.insert(std::map<unsigned int, FRoomEntry*>::value_type((unsigned int)Handle, Room));
    return Handle;
}

// (handle; RET 4): find in CharacterMap; miss -> FDUMP "Could
// not find npc character %u" (0x720) and on; a non-null room -> the room's
// (Characters.remove) and pool bit (handle - 1) cleared
// (the routine); then find again: hit -> erase, miss -> FDUMP (0x739).
void BaseDB::destroy_character(unsigned int Handle)
{
    std::map<unsigned int, FRoomEntry*>::iterator It = CharacterMap.find(Handle);
    if (It == CharacterMap.end())
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: Could not find npc character %u\n", GBaseDBFile, 0x720, Handle);
    }
    else if (It->second != nullptr)
    {
        It->second->Characters.remove(Handle);
        GCharacterHandles.set(Handle - 1, false);
    }
    It = CharacterMap.find(Handle);
    if (It == CharacterMap.end())
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: Could not find npc character %u\n", GBaseDBFile, 0x739, Handle);
        return;
    }
    CharacterMap.erase(It);
}

// (handle, behaviour; RET 8): find in CharacterMap; miss -> FDUMP
// 0x720, return; null room -> return; the first character record matching
// the handle (null when none -- baseline writes through it regardless):
// Props.BehaviorId = behaviour, bDirty = 1.
void BaseDB::set_character_behavior(unsigned int Handle, unsigned long Behavior)
{
    std::map<unsigned int, FRoomEntry*>::iterator It = CharacterMap.find(Handle);
    if (It == CharacterMap.end())
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: Could not find npc character %u\n", GBaseDBFile, 0x720, Handle);
        return;
    }
    FRoomEntry* const Room = It->second;
    if (Room == nullptr)
        return;
    CharacterRecord* Record = nullptr;
    MetaNode<CharacterRecord>* Node = nullptr;
    while (Room->Characters.Records.traverse(Node, true))
    {
        if (Node->Data->matches(Handle))
        {
            Record = Node->Data;
            break;
        }
    }
    Record->Props.BehaviorId = (unsigned int)Behavior;
    Record->bDirty = true;
}

// the routine for behaviours -- key ScriptBehavior+0x04,
// , BehaviorMap (+0x44).
unsigned int BaseDB::create_behavior(ScriptBehavior const& Props, unsigned int RecordFilter)
{
    FRoomEntry* Room = nullptr;
    std::map<unsigned int, FRoomEntry*>::iterator It = LocationMap.find(Props.Field04);
    if (It != LocationMap.end())
        Room = It->second;
    else
        FDUMP(0x100001, "%s(%d) : *** ERROR: Location %u is invalid\n", GBaseDBFile, 0x66d, Props.Field04);
    unsigned int const Handle = Room->create_behavior(Props, RecordFilter);
    BehaviorMap.insert(std::map<unsigned int, FRoomEntry*>::value_type(Handle, Room));
    return Handle;
}

// the routine for behaviours -- "Could not find behavior %u"
// (0x774 / 0x78d), the routine (Behaviors.remove), pool bit = the handle.
void BaseDB::destroy_behavior(unsigned long Handle)
{
    unsigned int const Key = (unsigned int)Handle;
    std::map<unsigned int, FRoomEntry*>::iterator It = BehaviorMap.find(Key);
    if (It == BehaviorMap.end())
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: Could not find behavior %u\n", GBaseDBFile, 0x774, Key);
    }
    else if (It->second != nullptr)
    {
        It->second->Behaviors.remove(Key);
        GBehaviorHandles.set(Key, false);
    }
    It = BehaviorMap.find(Key);
    if (It == BehaviorMap.end())
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: Could not find behavior %u\n", GBaseDBFile, 0x78d, Key);
        return;
    }
    BehaviorMap.erase(It);
}

// key MissionVendorProperties+0x04 (the base id) in BaseMap
// (miss -> FDUMP 0x659, null base passed on), the routine, MissionVendorMap.
unsigned int BaseDB::create_mission_vendor(MissionVendorProperties const& Props, unsigned int RecordFilter)
{
    FBaseEntry* Base = nullptr;
    std::map<unsigned int, FBaseEntry*>::iterator It = BaseMap.find(Props.BaseId);
    if (It != BaseMap.end())
        Base = It->second;
    else
        FDUMP(0x100001, "%s(%d) : *** ERROR: Base %u is invalid\n", GBaseDBFile, 0x659, Props.BaseId);
    unsigned int const Handle = Base->create_mission_vendor(Props, RecordFilter);
    MissionVendorMap.insert(std::map<unsigned int, FBaseEntry*>::value_type(Handle, Base));
    return Handle;
}

// "Could not find mission vendor offer %u" (0x7b2 / 0x7cb),
// (MissionVendors.remove), pool the field bit = the handle.
void BaseDB::destroy_mission_vendor(unsigned long Handle)
{
    unsigned int const Key = (unsigned int)Handle;
    std::map<unsigned int, FBaseEntry*>::iterator It = MissionVendorMap.find(Key);
    if (It == MissionVendorMap.end())
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: Could not find mission vendor offer %u\n", GBaseDBFile, 0x7b2, Key);
    }
    else if (It->second != nullptr)
    {
        It->second->MissionVendors.remove(Key);
        GMissionVendorHandles.set(Key, false);
    }
    It = MissionVendorMap.find(Key);
    if (It == MissionVendorMap.end())
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: Could not find mission vendor offer %u\n", GBaseDBFile, 0x7cb, Key);
        return;
    }
    MissionVendorMap.erase(It);
}

// (base id, filter; RET 8): find in BaseMap (a miss FDUMPs 0x659
// and goes on with a null base, whose list baseline then reads); count the
// offers where filter == 0, the record's filter == 0, or they are equal.
int BaseDB::count_mission_vendors(unsigned int Id, unsigned int SessionFilter)
{
    FBaseEntry* Base = nullptr;
    std::map<unsigned int, FBaseEntry*>::iterator It = BaseMap.find(Id);
    if (It != BaseMap.end())
        Base = It->second;
    else
        FDUMP(0x100001, "%s(%d) : *** ERROR: Base %u is invalid\n", GBaseDBFile, 0x659, Id);
    int Count = 0;
    MetaNode<MissionComputerRecord>* Node = nullptr;
    while (Base->MissionVendors.Records.traverse(Node, true))
    {
        unsigned int const RecordFilter = Node->Data->Filter;
        if (SessionFilter == 0 || RecordFilter == 0 || RecordFilter == SessionFilter)
            ++Count;
    }
    return Count;
}

// (handle, accepted, reason, sid; RET 0x10): find the handle in
// MissionVendorMap; a non-null base -> the routine; a miss -> FDUMP 0x7b2.
void BaseDB::mission_vendor_acceptance(unsigned long Handle, bool bAccepted, FmtStr const& Reason,
                                       unsigned int SessionId)
{
    std::map<unsigned int, FBaseEntry*>::iterator It = MissionVendorMap.find((unsigned int)Handle);
    if (It == MissionVendorMap.end())
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: Could not find mission vendor offer %u\n", GBaseDBFile, 0x7b2,
              (unsigned int)Handle);
        return;
    }
    if (It->second != nullptr)
        It->second->mission_vendor_acceptance(Handle, bAccepted, Reason, SessionId);
}

// NewsBroadcastProperties+0x04 (the base id) in BaseMap,
// , NewsMap (+0x6c).
unsigned int BaseDB::create_news_broadcast(NewsBroadcastProperties const& Props, unsigned int RecordFilter)
{
    FBaseEntry* Base = nullptr;
    std::map<unsigned int, FBaseEntry*>::iterator It = BaseMap.find(Props.BaseId);
    if (It != BaseMap.end())
        Base = It->second;
    else
        FDUMP(0x100001, "%s(%d) : *** ERROR: Base %u is invalid\n", GBaseDBFile, 0x659, Props.BaseId);
    unsigned int const Handle = Base->create_news_broadcast(Props, RecordFilter);
    NewsMap.insert(std::map<unsigned int, FBaseEntry*>::value_type(Handle, Base));
    return Handle;
}

// "Could not find news broadcast %u" (0x809 / 0x822),
// (NewsBroadcasts.remove), pool the field bit = the handle.
void BaseDB::destroy_news_broadcast(unsigned long Handle)
{
    unsigned int const Key = (unsigned int)Handle;
    std::map<unsigned int, FBaseEntry*>::iterator It = NewsMap.find(Key);
    if (It == NewsMap.end())
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: Could not find news broadcast %u\n", GBaseDBFile, 0x809, Key);
    }
    else if (It->second != nullptr)
    {
        It->second->NewsBroadcasts.remove(Key);
        GNewsHandles.set(Key, false);
    }
    It = NewsMap.find(Key);
    if (It == NewsMap.end())
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: Could not find news broadcast %u\n", GBaseDBFile, 0x822, Key);
        return;
    }
    NewsMap.erase(It);
}

// (props, filter; RET 8): key AmbientScriptProperties+0x04 (the
// room) in LocationMap. Unlike its siblings a miss (FDUMP 0x66d) or a null
// room returns 0xffffffff; otherwise the routine and an AmbientScriptMap
// (+0x80) insert.
unsigned int BaseDB::create_ambient_script(AmbientScriptProperties const& Props, unsigned int RecordFilter)
{
    std::map<unsigned int, FRoomEntry*>::iterator It = LocationMap.find(Props.RoomId);
    if (It == LocationMap.end())
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: Location %u is invalid\n", GBaseDBFile, 0x66d, Props.RoomId);
        return 0xffffffff;
    }
    FRoomEntry* const Room = It->second;
    if (Room == nullptr)
        return 0xffffffff;
    unsigned int const Handle = Room->create_ambient_script(Props, RecordFilter);
    AmbientScriptMap.insert(std::map<unsigned int, FRoomEntry*>::value_type(Handle, Room));
    return Handle;
}

// "Could not find ambient script %u" (0x84a / 0x863),
// (AmbientScripts.remove), pool the field bit = the handle.
void BaseDB::destroy_ambient_script(unsigned long Handle)
{
    unsigned int const Key = (unsigned int)Handle;
    std::map<unsigned int, FRoomEntry*>::iterator It = AmbientScriptMap.find(Key);
    if (It == AmbientScriptMap.end())
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: Could not find ambient script %u\n", GBaseDBFile, 0x84a, Key);
    }
    else if (It->second != nullptr)
    {
        It->second->AmbientScripts.remove(Key);
        GAmbientScriptHandles.set(Key, false);
    }
    It = AmbientScriptMap.find(Key);
    if (It == AmbientScriptMap.end())
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: Could not find ambient script %u\n", GBaseDBFile, 0x863, Key);
        return;
    }
    AmbientScriptMap.erase(It);
}
