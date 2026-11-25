// Author: Giperion.

#pragma once

#include "common_metalist.h" // MetaList<T>, OwnerList<T>
#include "common_goods.h"    // CharacterProperties, MissionVendorProperties, ...
#include "common_iface.h"    // ScriptBehavior
#include <map>

class BaseData;
class RoomData;
struct FmtStr;

// ============================================================================
// gf/BaseDB.cpp -- Server.dll's runtime base / room database.

// baseline keeps one static BaseDB object at .data:the field (constructed by
// the __xc initializer the routine -> the routine, destroyed through the
// atexit thunk the routine -> the routine). the routine is the accessor:
// its whole body is `MOV EAX,0x06d8da70 / RET`.

// Every live game-framework record (NPC characters, their script behaviours,
// ambient scripts, mission-vendor offers, news broadcasts) is a heap
// BroadcastRecord in a per-room or per-base record list, and every session
// that asked for a base's / room's info is a payload in that base's / room's
// session list. Creating, changing or destroying a record pushes the change
// to every session in the list that passes the record's session filter.

// Class names come from Server.dll's RTTI where it has any (the COL at
// vtable-4): BroadcastRecord (module storage), CharacterRecord (06d68374),
// BehaviorRecord (06d68394), AmbientScriptRecord (06d683c0),
// MissionComputerRecord (06d6843c), NewsBroadcastRecord (06d6844c), and the
// MetaList<T> instantiations the record lists use (06d68348 CharacterRecord,
// BehaviorRecord, 06d68338 AmbientScriptRecord, 06d683d8
// MissionComputerRecord, 06d683d0 NewsBroadcastRecord, 06d68324 unsigned int).
// The base, room and record-list classes have no vtable and therefore no
// recoverable name; FBaseEntry / FRoomEntry / FRecordList / FSessionList /
// FHandlePool are the port's names for them. Implementation:
// src/Server/server_basedb.cpp.
// ============================================================================

// ----------------------------------------------------------------------------
// FSessionList -- the ids of the sessions subscribed to a base or a room
// (base+0x40, room+0x54), each a heap-allocated unsigned int it owns.

// baseline's object has the dynamic type MetaList<unsigned int>, not a class of
// its own: the constructor the routine stores exactly one vptr, MetaList<
// unsigned int>'s (module storage -> module storage, whose only slot is the
// scalar deleting destructor the routine), and Server.dll has no OwnerList<
// unsigned int> vftable or TypeDescriptor at all. The payload ownership lives
// OUTSIDE MetaList's virtual destructor: the routine, called directly as the
// member destructor (module storage room, module storage base, and the unwind
// funclets 06d61a87 / 06d61ac7 / 06d61b5c / 06d61b8c), deletes every payload
// and releases the nodes, and only THEN runs MetaList<unsigned int>'s
// destructor inline (vptr restamp module storage, Tail = Head = 0, free_heap).
// A class DERIVED from MetaList would carry its own vftable (its destructor
// overrides the virtual one), so this is a non-polymorphic class holding the
// MetaList at +0x00 -- the same shape FRecordList<T> has below.
// ----------------------------------------------------------------------------
class FSessionList
{
public:
    // MetaList<unsigned int>(false) on the member -- Head, Tail,
    // FreeList = 0, bUseHeap = 0 -- and the only vptr store is MetaList's.
    FSessionList()
        : List(false)
    {
    }

    // free() inlined (module storage .. 06d11635), then the
    // member's ~MetaList<unsigned int> (module storage .. 06d11661).
    ~FSessionList() { free(); }

    // The room / base destructors call it before the member destructor runs
    // (module storage room, module storage base). baseline's target is the
    // ICF-folded OwnerList<CargoDesc>::free body (module storage): the element
    // type has a trivial destructor, so the code is the same for any T.
    // Phase 1: delete every payload and null its slot. Phase 2: every node to
    // FreeList (bUseHeap) or deleted, then Tail = Head = 0 (MetaList::free).
    void free();

    // the first node whose payload equals SessionId; payloads are
    // dereferenced with no null test (module storage).
    bool contains(unsigned int SessionId) const;

    // duplicate scan, operator new(4) payload, the routine node
    // (MetaList::alloc_node), tail link.
    void add(unsigned int SessionId);

    // find, delete the payload, unlink, pool-or-delete the node.
    void remove(unsigned int SessionId);

    // `MOV EAX,[ECX+4] / TEST / SETZ` -- Head == 0.
    bool empty() const { return List.get_head() == nullptr; }

    // MetaList<unsigned int>::traverse on the list itself: the record lists walk
    // the sessions through their FSessionList pointer, which is the MetaList's
    // own address (it sits at +0x00).
    bool traverse(MetaNode<unsigned int>*& Node, bool bForward) const
    {
        return List.traverse(Node, bForward);
    }

private:
    MetaList<unsigned int> List; // +0x00 .. +0x13

    FSessionList(FSessionList const&) = delete;
    FSessionList& operator=(FSessionList const&) = delete;
};

// ----------------------------------------------------------------------------
// BroadcastRecord -- the abstract base of every game-framework record.
// Layout: vptr +0x00, Filter +0x04 (0 = every session, else the one session
// the record is visible to), bDirty +0x08 (set by the creators and by
// BaseDB::set_character_behavior, cleared once a broadcast went out), the
// Common.dll properties object at +0x0c. Its own vtable (module storage) has
// three _purecall slots:
//   +0x00 send_update(sid)          marshal the properties, IClient "update"
//   +0x04 send_destroy(sid)         IClient "destroy" (sid, owner id, key)
//   +0x08 matches(unsigned const&)  the branch in every derived vtable,
//                                   `return this[0xc] == *key` (ICF-shared)
// The destructors are NOT virtual: the record lists delete through the
// concrete type (the routine / 16e60 / 16f80 / 17020 are called directly).
// ----------------------------------------------------------------------------
class BroadcastRecord
{
public:
    BroadcastRecord()
        : Filter(0)
        , bDirty(false)
    {
    }

    virtual void send_update(unsigned int SessionId) = 0;
    virtual void send_destroy(unsigned int SessionId) = 0;
    virtual bool matches(unsigned int const& Key) const = 0;

    // (called or inlined at every dispatch site):
    // `return sid == 0 || filter == 0 || filter == sid;`
    static bool session_passes(unsigned int SessionId, unsigned int RecordFilter)
    {
        return SessionId == 0 || RecordFilter == 0 || RecordFilter == SessionId;
    }

    unsigned int Filter; // +0x04
    bool bDirty;         // +0x08

protected:
    ~BroadcastRecord() {}
};

// operator new(0x70), vtable module storage. Slots: the routine (IClient
// +0x78), the branch (IClient +0x74 with Props.RoomId, Props.Key).
class CharacterRecord : public BroadcastRecord
{
public:
    void send_update(unsigned int SessionId) override;
    void send_destroy(unsigned int SessionId) override;
    bool matches(unsigned int const& Key) const override { return Props.Key == Key; }

    CharacterProperties Props; // +0x0c
};

// operator new(0x26c), vtable module storage. Slots: the routine (IClient
// +0x80), the branch (IClient +0x84 with Props.Field04 = room id, key).
class BehaviorRecord : public BroadcastRecord
{
public:
    void send_update(unsigned int SessionId) override;
    void send_destroy(unsigned int SessionId) override;
    bool matches(unsigned int const& Key) const override { return (unsigned int)Props.BehaviorId == Key; }

    ScriptBehavior Props; // +0x0c
};

// operator new(0x20), vtable module storage. Slots: the routine (IClient
// +0x8c), the branch (IClient +0x90 with Props.RoomId, key).
class AmbientScriptRecord : public BroadcastRecord
{
public:
    void send_update(unsigned int SessionId) override;
    void send_destroy(unsigned int SessionId) override;
    bool matches(unsigned int const& Key) const override { return (unsigned int)Props.Key == Key; }

    AmbientScriptProperties Props; // +0x0c
};

// operator new(0x6c), vtable module storage. Slots: the routine (IClient
// +0x9c), the branch (IClient +0x98 with Props.BaseId, key).
class MissionComputerRecord : public BroadcastRecord
{
public:
    void send_update(unsigned int SessionId) override;
    void send_destroy(unsigned int SessionId) override;
    bool matches(unsigned int const& Key) const override { return (unsigned int)Props.Key == Key; }

    MissionVendorProperties Props; // +0x0c
};

// operator new(0x6c), vtable module storage. Slots: the routine (IClient
// +0xb0), the branch (IClient +0xac with Props.BaseId, key).
class NewsBroadcastRecord : public BroadcastRecord
{
public:
    void send_update(unsigned int SessionId) override;
    void send_destroy(unsigned int SessionId) override;
    bool matches(unsigned int const& Key) const override { return (unsigned int)Props.Key == Key; }

    NewsBroadcastProperties Props; // +0x0c
};

// ----------------------------------------------------------------------------
// FRecordList<T> -- 0x18 bytes: the owning base's / room's session list
// pointer at +0x00 (set by the owner's init() to its own FSessionList) and a
// MetaList<T> of heap records at +0x04. No vtable of its own.
// ----------------------------------------------------------------------------
template <typename T>
class FRecordList
{
public:
    FRecordList()
        : Sessions(nullptr)
    {
    }

    // The member destructor inlined in the routine / the routine and the
    // out-of-line the routine / 16410 / 16640 / 16940 / 16b70: clear(), then
    // ~MetaList<T> (the routine + the routine).
    ~FRecordList() { clear(); }

    // (ambient; inlined for the others in the routine /
    // ): delete every record and null its slot, MetaList::free,
    // Sessions = 0.
    void clear()
    {
        MetaNode<T>* Node = nullptr;
        while (Records.traverse(Node, true))
        {
            delete Node->Data;
            Node->Data = nullptr;
        }
        Records.free();
        Sessions = nullptr;
    }

    // / 16500 / 16800 / 16a30 / 16c60: the first record whose
    // matches(Key) answers true, else return; send_destroy to every session
    // the record's filter admits; delete the record; unlink and pool-or-delete
    // the node.
    void remove(unsigned int const& Key)
    {
        MetaNode<T>* Node = nullptr;
        for (;;)
        {
            if (!Records.traverse(Node, true))
                return;
            if (Node->Data->matches(Key))
                break;
        }
        T* Record = Node->Data;
        MetaNode<unsigned int>* Session = nullptr;
        while (Sessions->traverse(Session, true))
        {
            unsigned int const SessionId = *Session->Data;
            if (BroadcastRecord::session_passes(SessionId, Record->Filter))
                Record->send_destroy(SessionId);
        }
        delete Record;
        Records.remove(Node);
    }

    // send_update to every session the record's filter admits.
    void broadcast(T* Record)
    {
        MetaNode<unsigned int>* Session = nullptr;
        while (Sessions->traverse(Session, true))
        {
            unsigned int const SessionId = *Session->Data;
            if (BroadcastRecord::session_passes(SessionId, Record->Filter))
                Record->send_update(SessionId);
        }
    }

    FSessionList* Sessions; // +0x00
    MetaList<T> Records;    // +0x04

private:
    FRecordList(FRecordList const&) = delete;
    FRecordList& operator=(FRecordList const&) = delete;
};

// The ambient-script list has one member of its own.
class FAmbientScriptList : public FRecordList<AmbientScriptRecord>
{
public:
    // (sid, room id): send_update of every record the session
    // passes, then IClient +0x94 (sid, room id).
    void send_all(unsigned int SessionId, unsigned int RoomId);
};

// ----------------------------------------------------------------------------
// FHandlePool -- std::bitset<4096> (0x200 bytes) + the last-allocated cursor
// at +0x200. Five in baseline, each zeroed by its own __xc initializer:
//   NPC characters     the field / e528  (the branch)
//   script behaviours  the field / e320  (the branch)
//   ambient scripts    the field / e118  (the branch)
//   mission vendors    the field / df10  (the branch)
//   news broadcasts    the field / dd08  (the branch)
// ----------------------------------------------------------------------------
class FHandlePool
{
public:
    bool test(unsigned int Position) const
    {
        return (Bits[Position >> 5] & (1u << (Position & 0x1f))) != 0;
    }

    // out-of-range throws through the routine.
    void set(unsigned int Position, bool bValue);

    // (NPC pool): the scan below; returns the bit index + 1, or
    // -1 when every bit is taken.
    int allocate();

    // The copy the four other creators inline (e.g. module storage ..
    // ): the same scan, returning the bit index itself, or
    // when every bit is taken.
    unsigned int allocate_index();

    unsigned int Bits[0x80]; // +0x000
    unsigned int Last;       // +0x200
};

// ----------------------------------------------------------------------------
// FRoomEntry -- 0x68 bytes, one per room of a base.
// ----------------------------------------------------------------------------
class FRoomEntry
{
public:
    // members, then the init() body.
    FRoomEntry();
    // .
    ~FRoomEntry();

    // .
    void init();

    // the entering player's own character record.
    void enter(unsigned int SessionId);

    // an NPC character; returns its handle.
    int create_character(CharacterProperties const& Props, unsigned int RecordFilter);
    // .
    unsigned int create_behavior(ScriptBehavior const& Props, unsigned int RecordFilter);
    // .
    unsigned int create_ambient_script(AmbientScriptProperties const& Props, unsigned int RecordFilter);

    // .
    unsigned int get_character_on_placement(unsigned int SessionFilter, int PlacementIndex) const;

    unsigned int BaseId;                       // +0x00  RoomData+0x00
    unsigned int RoomId;                       // +0x04  RoomData+0x04
    RoomData* Data;                            // +0x08
    FRecordList<CharacterRecord> Characters;   // +0x0c (list +0x10)
    FRecordList<BehaviorRecord> Behaviors;     // +0x24 (list +0x28)
    FAmbientScriptList AmbientScripts;         // +0x3c (list +0x40)
    FSessionList Sessions;                     // +0x54

private:
    FRoomEntry(FRoomEntry const&) = delete;
    FRoomEntry& operator=(FRoomEntry const&) = delete;
};

// ----------------------------------------------------------------------------
// FBaseEntry -- 0x54 bytes, one per base.
// ----------------------------------------------------------------------------
class FBaseEntry
{
public:
    // members, then the init() body.
    FBaseEntry();
    // .
    ~FBaseEntry();

    // .
    void init();
    // .
    void load(BaseData* Source);

    // IClient +0x70 (sid, base id, start room).
    void send_start_room(unsigned int SessionId);

    // .
    unsigned int create_mission_vendor(MissionVendorProperties const& Props, unsigned int RecordFilter);
    // .
    void mission_vendor_acceptance(unsigned long Handle, bool bAccepted, FmtStr const& Reason,
                                   unsigned int SessionId);
    // .
    unsigned int create_news_broadcast(NewsBroadcastProperties const& Props, unsigned int RecordFilter);

    unsigned int BaseId;                               // +0x00
    BaseData* Data;                                    // +0x04
    FRoomEntry* Rooms;                                 // +0x08  new[] with a count prefix
    int NumRooms;                                      // +0x0c
    FRecordList<MissionComputerRecord> MissionVendors; // +0x10 (list +0x14)
    FRecordList<NewsBroadcastRecord> NewsBroadcasts;   // +0x28 (list +0x2c)
    FSessionList Sessions;                             // +0x40

private:
    FBaseEntry(FBaseEntry const&) = delete;
    FBaseEntry& operator=(FBaseEntry const&) = delete;
};

// ----------------------------------------------------------------------------
// BaseDB -- the field.
// ----------------------------------------------------------------------------
class BaseDB
{
public:
    // .
    BaseDB();
    // .
    ~BaseDB();

    // (IServerImpl::Startup through the thunk the routine).
    void init();
    // (IServerImpl::Shutdown through the routine).
    void shutdown();
    // (IServerImpl::ElapseTime through the routine).
    void update(float DeltaTime);

    // / the routine: find; a miss FDUMPs and returns 0.
    FBaseEntry* find_base(unsigned int Id);
    FRoomEntry* find_location(unsigned int Id);

    // / the routine: RoomData::load / unload of every room.
    void load_base(unsigned int SessionId, unsigned int Id);
    void unload_base(unsigned int SessionId, unsigned int Id);
    // / the routine.
    void enter_location(unsigned int SessionId, unsigned int Id);
    void exit_location(unsigned int SessionId, unsigned int Id);
    // / the routine.
    void base_info(unsigned int SessionId, unsigned int Id, bool bFull);
    void location_info(unsigned int SessionId, unsigned int Id, bool bFull);

    // / the routine / the routine.
    int create_character(CharacterProperties const& Props, unsigned int RecordFilter);
    void destroy_character(unsigned int Handle);
    void set_character_behavior(unsigned int Handle, unsigned long Behavior);
    // / the routine.
    unsigned int create_behavior(ScriptBehavior const& Props, unsigned int RecordFilter);
    void destroy_behavior(unsigned long Handle);
    // / the routine / the routine / the routine.
    unsigned int create_mission_vendor(MissionVendorProperties const& Props, unsigned int RecordFilter);
    void destroy_mission_vendor(unsigned long Handle);
    int count_mission_vendors(unsigned int Id, unsigned int SessionFilter);
    void mission_vendor_acceptance(unsigned long Handle, bool bAccepted, FmtStr const& Reason,
                                   unsigned int SessionId);
    // / the routine.
    unsigned int create_news_broadcast(NewsBroadcastProperties const& Props, unsigned int RecordFilter);
    void destroy_news_broadcast(unsigned long Handle);
    // / the routine.
    unsigned int create_ambient_script(AmbientScriptProperties const& Props, unsigned int RecordFilter);
    void destroy_ambient_script(unsigned long Handle);

    FBaseEntry* Bases;                                    // +0x00  new[] with a count prefix
    int NumBases;                                         // +0x04
    std::map<unsigned int, FBaseEntry*> BaseMap;          // +0x08  base id -> base
    std::map<unsigned int, FRoomEntry*> LocationMap;      // +0x1c  room id -> room
    std::map<unsigned int, FRoomEntry*> CharacterMap;     // +0x30  character handle -> room
    std::map<unsigned int, FRoomEntry*> BehaviorMap;      // +0x44  behaviour handle -> room
    std::map<unsigned int, FBaseEntry*> MissionVendorMap; // +0x58  offer handle -> base
    std::map<unsigned int, FBaseEntry*> NewsMap;          // +0x6c  broadcast handle -> base
    std::map<unsigned int, FRoomEntry*> AmbientScriptMap; // +0x80  ambient handle -> room

private:
    BaseDB(BaseDB const&) = delete;
    BaseDB& operator=(BaseDB const&) = delete;
};

// `return &the field;`
BaseDB* GetBaseDB();

// The three IServerImpl forwarding thunks (each `CALL the routine / MOV ECX,EAX`
// then the member). baseline has no symbol for them; the names are the port's.
// -> BaseDB::init (IServerImpl::Startup, module storage).
void Server_SubsystemInit();
// -> BaseDB::shutdown (IServerImpl::Shutdown, module storage).
void Server_SubsystemShutdown();
// -> BaseDB::update(dt) (IServerImpl::ElapseTime, module storage;
// the float is ElapseTime's own argument, pushed from ESI).
void Server_SubsystemUpdate(float DeltaTime);

// Oracle instrumentation (design/migration-plan.md, Phase 0; work package P3.6e, decision
// record 0008 points 7 and 10): the base database's events for the Tier B base session trace
// (Content.dll's OracleDumpBaseSession, crates/game-logic/tests/base_session.rs). Inert unless
// the harness set a hook through OracleSetBaseDbTrace; no game module sets one. Event 1 is a
// record made (A the handle, B the session filter, Data its marshalled properties), 2 a destroy
// (A the handle, B 1 when the handle's map had it), 3 set_character_behavior (A the
// character, B the behaviour, C 1 when the record was changed), 4 a vendor's answer (A the
// offer, B accepted, C the session, Data the marshalled MissionVendorAcceptance), 5
// ReportWhyMissionVendorEmpty (A the client, B the reason). Kind: 1 character, 2 behaviour,
// 3 ambient script, 4 mission vendor offer, 5 news broadcast, 0 for events 3 to 5.
void OracleBaseDbTrace(int Event, int Kind, unsigned int A, unsigned int B, unsigned int C,
                       void const* Data, unsigned int Size);
