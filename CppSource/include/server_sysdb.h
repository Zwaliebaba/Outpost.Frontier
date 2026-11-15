// Author: Giperion.

#pragma once

#include <list>
#include <set>

#include "common_universe.h"
#include "common_cobjdb.h"
#include "common_metalist.h"
#include "server_exports.h"
// CRemotePhysicsSimulation. The tracked record embeds one BY VALUE, so the
// definition has to be visible here and not only in the .cpp -- see the
// tracker comment below for the `LEA ECX,[ESI + 0x8]` that settles it.
#include "common_server.h"

// server_gameobject.h -- what baseline stores in the lists below. NOT SERVER_API:
// baseline Server.dll exports no GameObject symbol at all (EXE/Server.dll's export
// table has no ?...@GameObject@@ name), and a dllexport on this forward
// declaration made the whole class dllexport -- which exported its implicit copy
// constructor, operator= and all three ??_7 tables (5 names baseline lacks).
class GameObject;
class EqObj;   // server_gameobject.h (StarSystem::tractor_loot)
class Loot;    // server_gameobject.h; `::Loot` inside StarSystem, whose +0x48 block is named Loot
class Solar;   // server_gameobject.h (StarSystem::create_solar)
class Ship;    // server_gameobject.h (StarSystem::create_ship)
struct FShipCreateRequest; // server_createinfo.h (StarSystem::create_ship)

// common_misc.h's DamageList. Only ever used through a pointer here
// (StarSystem_DamageObject below), so a declaration is enough and this header
// stays cheap to include.
struct DamageList;

// server_gameobject.h -- StarSystem's asteroid factories hand these back, and
// the explosion takes its record by reference. Pointer/reference only here.
namespace SrvAsteroid {
class MedAsteroid;
class MedMine;
}
struct ExplosionDamageEvent;
struct FLootRecord;   // server_gameobject.h, the 0x6c-byte loot element
struct FServerLaunchRecord;  // server_gameobject.h, the ServerEquip launch record (0x54)
struct FServerBulletRecord;  // server_gameobject.h, SrvGun::launch_bullets' record (0x48)
struct FCommonUpdateObjectInfo;  // rpc_iclient_args.h, the IClient SendCommonUpdateObject argument (0x2c)

// The record StarSystem::send_mine_explosion (the routine) fans out and
// SystemPlayerView_SendMineExplosion (the routine) hands to IClient +0xd0
// (FLPACKET_SERVER_EXPLODEASTEROIDMINE, whose payload is these four dwords).
// Built by MedMine::update (module storage .. 06d04f80): the mine's own id
// (vt +0x20) at +0, then Direction scaled by ExplosionOffset at +4 .. +0xc.
struct FMineExplosion {
    unsigned int MineId;              // +0x00  the routine resolves it (module storage)
    Vector Offset;                    // +0x04
};
static_assert(sizeof(FMineExplosion) == 0x10, "FMineExplosion is four dwords");

// The record GameObject::on_collision (+0x1a4, module storage .. 06ceeae6) builds
// on its stack and StarSystem::collision_damage (the routine) reads: the
// collision source (this object), the sub-object it was hit on, the partner and
// the partner's sub-object, and Collision::CalcCollisionDamage's result.
struct FCollisionDamage {
    unsigned int SourceId;            // +0x00  module storage  get_id()
    unsigned int SourceSubObjId;      // +0x04  module storage  MOVZX of the event's word
    unsigned int TargetId;            // +0x08  module storage  partner get_id()
    unsigned short TargetSubObjId;    // +0x0c  module storage
    unsigned short Pad0e;
    float Damage;                     // +0x10  module storage
};
static_assert(sizeof(FCollisionDamage) == 0x14, "FCollisionDamage is the 0x14-byte collision record");

// rpc_iclient_args.h -- the IClient +0x15c (BURNFUSE) argument EqObj::light_fuse /
// unlight_fuse build and StarSystem::broadcast_fuse fans out.
struct FServerBurnFuseInfo;

// server_createinfo.h's CREATESHIP record, the first argument of
// SystemPlayerView_CreateShipForPlayer below. Pointer-only here as well.
struct CShipCreateInfo;
struct Costume;           // common_reputation.h, 13 dwords (FLPACKET_SERVER_SENDCOMM)
struct CSolarCreateInfo;  // server_createinfo.h, CREATESOLAR's record (the routine)
struct FServerCreateLootRequest;  // server_createinfo.h, CREATELOOT's 0x14-byte record

// ---------------------------------------------------------------------------
// ObjectList -- the type of StarSystem's ten dispatch blocks (+0x34 .. +0xFB).

// baseline's RTTI names it: the vtable the StarSystem constructor stamps on every
// block, module storage (one slot, the routine, the scalar deleting
// destructor), has its COL at module storage whose type descriptor
// is ".?AUObjectList@@"; its base is MetaList<GameObject>
// (vtable module storage -> the routine, type descriptor module storage
// ".?AV?$MetaList@UGameObject@@@@"). So a block is exactly Common's
// MetaList<T> (src/include/common_metalist.h): vptr +0x00, Head +0x04, Tail
// +0x08, bUseHeap byte +0x0C, FreeList +0x10, nodes {Next, Prev, Data} of 0xC
// bytes, NULL-terminated -- module storage `MOV ESI,[EBP+0x38]` for the head,
// `MOV ESI,[ESI]` to advance, `TEST ESI,ESI` to stop.

// node->Data IS the IObjRW wrapper: module storage loads it straight into ECX
// and calls vtable slots +0x20/+0x158/+0x160 on it, and module storage deletes
// it virtually through its CBase sub-object at +0x0c.

// These are NOT the same nodes as the player list at StarSystem+0x0c. That list
// is circular through a self-referencing sentinel and its element is EMBEDDED at
// node+0x08 -- module storage is `LEA ECX,[ESI + 0x8]`, an address computation.
// Do not unify them.

// The two constructors differ in ONE byte, and it matters:
//   default  -- MetaList(true): the StarSystem constructor (module storage)
//               writes bUseHeap = 1 inline for +0x34/+0x48/+0x5c/+0x70
//               (module storage .. 06d06d96), through the routine(block, 1)
//               for +0x84 .. +0xd4 (module storage .. 06d06e04) and through
// for +0xe8 (module storage `MOV byte [EAX+0xc],1`).
//   copy     -- MetaList(false), and NOTHING is copied: the StarSystem copy
//               constructor (module storage) builds +0x34 .. +0xd4 with
// (block, 0) (`PUSH 0x0` at module storage ..
// ) and +0xe8 with the routine, which ignores its
//               source argument and stores bUseHeap = 0 (module storage,
//               `RET 0x4`). The pair constructor the routine does the same.
// SysDB's build loop (the routine) default-constructs a temporary and inserts
// a COPY of it, so every StarSystem that lives in SysDB::SysMap has
// bUseHeap = 0 on all ten blocks: nodes are deleted, never pooled.

// There is no destructor body of ObjectList's own: the routine runs
// , which re-stamps MetaList<GameObject>'s vtable and does
// MetaList::free() then free_heap() -- MetaList<T>'s own destructor.
// ---------------------------------------------------------------------------
struct ObjectList : public MetaList<GameObject> {
    ObjectList() : MetaList<GameObject>(true) {}
    ObjectList(ObjectList const& Other) : MetaList<GameObject>(false) { (void)Other; }

    // StarSystem reads and writes the list's fields directly throughout
    // (e.g. module storage `[EBP+0x38]`, module storage `[EBX+0xd8]`).
    using MetaList<GameObject>::Head;
    using MetaList<GameObject>::Tail;
    using MetaList<GameObject>::bUseHeap;
    using MetaList<GameObject>::FreeList;
    using MetaList<GameObject>::free_node;

    // (module storage, `RET 0x4`): the first node whose object's
    // IObjInspect::get_id (vtable +0x20, module storage) equals Id, or null.
    MetaNode<GameObject>* find_by_id(unsigned int Id) const;

    // (module storage, `RET 0x4`): how many nodes' objects answer
    // a non-zero `Mask & get_type_bits()` (vtable +0x154, module storage /
    // `TEST EBX,EAX`). Unguarded: node->Data is dereferenced
    // straight away (module storage).
    unsigned int count_type(unsigned int Mask) const;

    // (module storage, `RET 0x14`; the name is the port's): up to
    // MaxCount CSimple ids (+0xb0) into OutIds of the objects in this block whose
    // CSimple (& 3) TypeBits (+0xe0) meet TypeMask -- not destroyed (vt +0x160)
    // when the mask has 0x7f0000 -- and that lie within Radius of *Pos (squared
    // distance <= Radius * Radius as a float); a negative Radius takes every
    // match. Returns the count. Callers: StarSystem::scan_objects (module storage
    // ships, module storage solars).
    unsigned int collect_in_sphere(unsigned int* OutIds, unsigned int MaxCount, Vector const* Pos,
                                   float Radius, unsigned int TypeMask) const;

    ObjectList& operator=(ObjectList const&) = delete;
};

static_assert(sizeof(ObjectList) == 0x14, "ObjectList is baseline's 0x14-byte MetaList");

// The node of an ObjectList. Kept as a name because the per-block walks all
// over Server.dll spell it this way.
typedef MetaNode<GameObject> FSystemListNode;

// ---------------------------------------------------------------------------
// The per-player object tracker that hangs off StarSystem+0x08.

// StarSystem+0x08 is the std::list OBJECT itself, in the MSVC6 shape:
// allocator byte at +0x08, _Head at +0x0C, _Size at +0x10.
// StarSystem::add_player proves it -- module storage is `LEA ECX,[EDI + 0x8]`,
// the list's own `this`, and module storage pushes `[EDI + 0xc]`, the
// sentinel, as the insert position.

// The list's ELEMENT IS EMBEDDED at node+0x08: module storage, module storage,
// and module storage are all `LEA ECX,[ESI+8]`, address
// computations, not loads. The element is a small polymorphic object --
// StarSystem::remove_player disposes of it through `(**(code**)node[2])(0)`,
// a virtual call on node+0x08 -- and it owns a SECOND list, of every object
// this player currently knows about.

// The element's constructor is the routine (module storage, `RET 0x4`, one
// argument: the session id) and its destructor the routine; slot 0 of its
// vtable (.rdata:the routine -> module storage) is the scalar deleting
// destructor. Layout, read off that constructor and off the routine /
// / the routine / the routine:
//   +0x00  vptr                                        (module storage)
//   +0x04  the constructor never touches it
//   +0x08  std::list<SystemTrackedObject*>: allocator byte (module storage),
//   +0x0C    _Head -- operator new(0xc), self-linked (module storage..06cf5214)
//   +0x10    _Size, zeroed                             (module storage)
//   +0x14  an MSVC6 _Tree (std::set/std::map): allocator bytes at +0x14/+0x15
//          (module storage / module storage), _Nil = operator new(0x14) at
//          +0x1C (module storage..06cf524f), _Head = operator new(0x14) at
//          +0x18 with _Left = _Right = itself and _Parent = _Nil
//          (module storage..06cf5275), a byte at +0x20, _Size at +0x24, a byte
//          at +0x28. CORRECTED 2026-09-17 (wave 13): the routine does NOT run
//          a lower_bound over it -- it runs MSVC6's `_Tree::insert(const
//          value_type&)` descent, `_Ans = comp(_V, _Key(_X))` with `_Y`
//          trailing (module storage .. module storage), followed by the
//          uniqueness check at module storage .. module storage and a call into
//          `_Insert` (the routine). It is a std::set<unsigned int> of object
//          ids; see the member's own comment below.
//   +0x30  two dwords, zeroed by the constructor and handed to the record's
//   +0x34    CRemotePhysicsSimulation, slot 1 (module storage .. module storage)
//   +0x38  the player (session) id -- the constructor's only argument
//          (module storage; read back at module storage / module storage /
// , and as node+0x40 in StarSystem::remove_player)
//   +0x3C  Vector -- the player's own position, subtracted componentwise at
// / module storage / module storage
// Total 0x48 bytes, so a node is 0x08 + 0x48 = 0x50.

// CAUTION on the allocator bytes at +0x08/+0x14/+0x15. baseline's constructor
// copies each one from an uninitialised `allocator()` temporary that the
// compiler happens to overlap with the incoming argument slot, so the listing
// reads `MOV AL,[ESP+0x14]` / `MOV [ESI+8],AL` against the SAME stack slot
// that later feeds the player id at +0x38 (module storage, module storage and
// analysis calls all three `param_1`). They are NOT a copy of
// the player id; they are MSVC6's 1-byte std::allocator member and carry no
// value at all.

// The tracked RECORD is a separate heap block of 0x2F8 bytes, allocated by
// (module storage `PUSH 0x2f8`, module storage operator new):
//   +0x000  GameObject*            (module storage zeroes it, module storage
//           stores the real one)
//   +0x004  never written
//   +0x008  CRemotePhysicsSimulation BY VALUE. module storage is
//           `LEA ECX,[ESI + 0x8]` followed by a direct CALL to
//           COMMON.DLL::CRemotePhysicsSimulation::CRemotePhysicsSimulation,
//           and the routine (`ADD ECX,0x8` then a tail jump into the
//           destructor) is called with ECX = the RECORD, never the element.
//           0x2E8 bytes, per the vector-deleting destructor at module storage,
//           and 0x08 + 0x2E8 = 0x2F0 = exactly the flag offset below.
//   +0x2F0  "the client has been told this object exists". Seeded to
//           COMMON.DLL::SinglePlayer() (module storage / module storage), so
//           in single player every record starts already-created.
// ---------------------------------------------------------------------------

// One tracked object, the Data of a node in the element's inner list.
struct SystemTrackedObject {
    // +0x00 -- the object being tracked. module storage loads it and calls the
    // wrapper's get_id through vtable +0x20; module storage compares it against
    // the candidate object directly.
    GameObject* Object;
    // +0x04 -- baseline never writes it. It is the alignment gap that the
    // simulation's 8-byte alignment forces. Placeholder.
    unsigned int Field_0x04;
    // +0x08 -- the network-physics interpolator, BY VALUE (0x2E8 bytes).
    CRemotePhysicsSimulation Simulation;
    // +0x2F0 -- the routine returns it verbatim as the visibility answer
    // (module storage), and the routine only sends the destroy notification
    // when it is set (module storage).
    bool bCreatedOnClient;
};

struct SystemTrackedNode {
    SystemTrackedNode* Next;          // +0x00
    SystemTrackedNode* Prev;          // +0x04
    SystemTrackedObject* Data;        // +0x08  a real pointer (module storage MOV)
};

// +0x08 .. +0x13 of Observer: baseline's MSVC6 std::list<SystemTrackedObject*>
// {allocator byte +0, _Head +4, _Size +8}. Its three special members are the
// parts of Observer's constructor / copy constructor / destructor that belong to
// this member, and they run where member construction and destruction put them:
//   ctor       module storage .. 06cf5217  allocator byte; operator new(0xc)
//              self-linked sentinel; size 0 -- before the _Tree is built
//   copy ctor  module storage .. 06d0f778  allocator byte; a fresh sentinel; one
//              new node per source node carrying the SAME record pointer
//              (the routine is a pointer copy); ++size each
//   dtor       module storage .. 06cf53ca  the routine erase over what is left
//              (nothing: ~Observer's body has emptied it), free the sentinel,
//              head = size = 0 -- after the _Tree teardown, as the member
//              declared first is destroyed last.
struct FObserverTrackedList {
    FObserverTrackedList();
    FObserverTrackedList(FObserverTrackedList const& Source);
    ~FObserverTrackedList();

    unsigned char Alloc;              // +0x00  MSVC6 std::allocator, no value
    unsigned char Pad0[3];
    SystemTrackedNode* Head;          // +0x04  self-referencing sentinel
    unsigned int Size;                // +0x08
};

// Observer -- baseline's own name: RTTI TypeDescriptor module storage
// ".?AVObserver@@" (a `class`, CHD 06d6c888: one entry, no base class), source
// file Server\space\Observer.cpp. The per-player view
// element embedded at StarSystem player-list node+0x08.

// Polymorphic in baseline, with a one-slot vftable module storage (COL 06d6c898):
//   slot 0  the routine  the compiler's scalar deleting destructor around
//                         ~Observer (the routine): destroy, then free if flag&1.
// The table's extent is exactly one dword (06d66988..06d6698b; the next dword is
// alignment ahead of the double the field). Three places stamp it --
// the constructor (module storage), the copy constructor (module storage) and the
// destructor (module storage) -- and three places call through it, every one a
// std::list erase that destroys the element in place with flag 0 and frees the
// node itself afterwards:
// LEA ECX,[ESI+0x8] / MOV EDX,[ECX] / PUSH 0 / CALL [EDX]
//                   (StarSystem::remove_player, the routine)
// (the routine, the player list's destructor)
// (the routine, the player list's erase(first, last))
// The port spells those three as `Element->~Observer()` through a pointer, which
// MSVC compiles to the same `PUSH 0 / CALL [vptr+0]`.

// The node is a raw heap block in both baseline and this port (operator new
// 0x50 in baseline), and the element is placement-constructed into it, exactly as
// MSVC6's std::list<Observer> constructs its value into _Buynode's storage.
class Observer {
public:
    // (module storage, `RET 0x4`) -- builds the tracked-object
    // list's sentinel and the pending-damage set, zeroes the rest; the only value
    // that comes in is the session id.
    explicit Observer(unsigned int InPlayerId);

    // (module storage) -- run in place on a raw node by the list's
    // insert (the routine, module storage) and by StarSystem's copy constructor
    // (the routine, module storage). The tracked records are shared with the
    // source, not duplicated (the routine copies the pointer); the
    // pending-damage set is copied.
    Observer(Observer const& Source);

    // (module storage) -- destroys every tracked record (each
    // one's CRemotePhysicsSimulation first, module storage), frees the inner
    // nodes, the pending-damage set and the sentinel. Virtual: slot 0.
    virtual ~Observer();

    // +0x00  vptr (module storage)
    unsigned int Field_0x04;          // +0x04  never written by either constructor
    FObserverTrackedList Tracked;     // +0x08 .. +0x13
    // +0x14 .. +0x27 -- baseline's _Tree. DECODED 2026-09-17 (wave 13). The
    // wave-12 note here said the port "has no consumer for it yet (the routine
    // is its only reader)". Both halves were wrong: the routine is its only
    // WRITER, and it has three readers.

    // It is `std::set<unsigned int>`, and the key is an OBJECT ID. The layout
    // reads straight off the constructor (module storage .. module storage):
    // allocator bytes at +0x14/+0x15, `_Nil` = operator new(0x14) at +0x1C with
    // _Color = _Black and _Isnil = 1, `_Head` = operator new(0x14) at +0x18 with
    // _Parent = _Nil and _Left = _Right = itself, `_Multi` at +0x20 and `_Size`
    // at +0x24. Node = {_Left +0, _Parent +4, _Right +8, _Value +0xC,
    // _Color +0x10, _Isnil +0x11} = 0x14 bytes, which is why every walk of it
    // compares against `node + 0x0C`.

    // The same 0x14-byte shape appears at a base of its own in the routine,
    // which takes a SECOND set of the same type by pointer and reads it as
    // {_Head +4, _Nil +8, _Multi +0xC, _Size +0x10} -- exactly this block
    // rebased to +0x14. That is the independent confirmation.

    // WHAT IT IS FOR. the routine (module storage) is the per-player
    // FLPACKET_SERVER_DAMAGEOBJECT gate. When the player cannot see the object
    // and nothing else forces the send, baseline does not drop the update: it
    // remembers the object id here and lets the per-frame observer update
    // (the routine) retry it. That retry is the reader:
    // the loop opens on `[EBP+0x18]`'s _Lmost and walks with
    // (operator++), resolving each id and keeping
    //                   the ones the routine now calls visible;
    // builds the damage record through the object's own
    //                   vtable +0x1a8 with mask 0x40000000;
    // CALL dword ptr [EDI + 0xfc]  -- IClient SendServerDamageObject;
    // 8d4d14        LEA  ECX,[EBP + 0x14]
    // e8c6130000    CALL the routine   ; set::erase(key)
    // and the routine (system switch-out) clears the whole set.
    // destroys it.

    // A real std::set, because baseline has one: a member of Observer, built by
    // its constructors and torn down by its destructor. Do not memcpy an
    // Observer.
    std::set<unsigned int> PendingDamageIds;   // +0x14 .. +0x27
    // +0x28 -- CORRECTED 2026-09-17 (wave 12). This byte used to be labelled
    // `SetMulti`, i.e. the last member of the _Tree above. It is not: the _Tree
    // ends at +0x28, and this is a bool of Observer's own.

    // Proof, on the raw bytes rather than on the constructor's zero-fill.
    // (module storage, the "an object left this system" notifier)
    // takes the view in ECX:
    // 8be9      MOV  EBP,this          ; EBP = the view
    // 8b450c    MOV  EAX,[EBP + 0xc]   ; ... its tracked list
    // and, after dropping the object from the tracked list, does
    // 8b4538    MOV  EAX,[EBP + 0x38]        ; this view's player id
    // 8b35....  MOV  ESI,[.data:Players]
    // 69c918040000  IMUL this,this,0x418
    // 3b5431cc  CMP  EDX,[this + ESI*1 + -0x34]  ; slot+0x3E4,
    //                                                 ; PlayerData::ship_object_id
    // 7504      JNZ  the branch
    // c6452801  MOV  byte ptr [EBP + 0x28],0x1
    // i.e. it is LATCHED when the object that just left IS this player's own
    // ship. MSVC never re-writes a live _Tree's `_Multi`, and turning a set into
    // a multiset halfway through would be meaningless; this is a state flag.

    // Every one of the five "create this object on the client" notifiers gates
    // on it and returns silently when it is set -- module storage
    // (ship), module storage (solar), module storage (countermeasure),
    // (loot), module storage (guided), module storage (mine) --
    // and module storage reads it back inside the routine itself. So: "this
    // player no longer has a ship in this system; stop telling them about it".
    // The name is the port's, baseline names nothing here.
    unsigned char bShipGone;          // +0x28
    unsigned char Pad3[3];
    unsigned int Field_0x2C;          // +0x2C  never written by the constructor
    // +0x30 .. +0x37 -- CORRECTED 2026-09-17 (wave 14). These were two
    // `unsigned int` placeholders called Field_0x30/Field_0x34. They are ONE
    // double, and the x87 opcodes say so outright. The per-frame observer
    // update (the routine) opens by accumulating its own `double` argument
    // into it:
    // dd8424c8020000  FLD   double ptr [ESP + 0x2c8]  ; the arg
    // dc4530          FADD  double ptr [EBP + 0x30]
    // dd5d30          FSTP  double ptr [EBP + 0x30]
    // and reads it back narrowed to a float for the UPDATEOBJECT payload:
    // dd4530          FLD   double ptr [EBP + 0x30]
    // d99c248c000000  FSTP  float  ptr [ESP + 0x8c]
    // The "two dwords" reading came from module storage/module storage, where
    // SystemPlayerView_AddTrackedObject pushes the pair as two PUSHes -- which
    // is simply how a double is passed on the stack. Checklist class: direction
    // confused with type.

    // It is the observer's own clock: the total time this view has been
    // running, stamped into every UPDATEOBJECT so the client can order samples.
    double ElapsedTime;               // +0x30 .. +0x37
    unsigned int PlayerId;            // +0x38
    Vector Position;                  // +0x3C
};

// The player-list node. The element really is embedded, so `View` must stay the
// third member and must not become a pointer.
struct SystemPlayerNode {
    SystemPlayerNode* Next;           // +0x00
    SystemPlayerNode* Prev;           // +0x04
    Observer View;                    // +0x08
};

// (module storage, `RET 0x4`) -- start tracking an object for this
// player. Allocates the 0x2F8-byte record, constructs its
// CRemotePhysicsSimulation in place, seeds "created on the client" from
// SinglePlayer(), push_backs the record and bumps the size. Returns the record,
// exactly as baseline does (EAX at module storage).
SystemTrackedObject* SystemPlayerView_AddTrackedObject(Observer* View, GameObject* Obj);

// "this object is not tracked yet; should it become visible?"
// Reached only from the routine's miss path (module storage).
//   true  when the object is null, when COMMON.DLL::SinglePlayer(), or when the
//         object's own CSimple::OwnerPlayer is this view's player
//         (module storage / module storage / module storage)
//   otherwise a squared-distance test against one of three radii, chosen by
//   ownership and grouping:
//     object has no owning player            -> 2500^2 (module storage = 6.25e6)
//     owner is in a different group, or this
//     player is in no group at all           -> 5000^2 (module storage = 2.5e7)
//     owner is in the SAME group             -> 10000^2 (module storage = 1e8)
//   The group value is PlayerData+0x404: baseline indexes `Players + id*0x418`
//   and reads -0x14 off that (module storage/module storage and
// /module storage), and 0x418 - 0x14 = 0x404. It is only ever
//   tested for zero and compared between two players, which is exactly what
//   common_server_playerdb.h already names PlayerData::group_ptr, the
//   CPlayerGroup*.
bool SystemPlayerView_ShouldBeVisible(Observer* View, GameObject* Obj);

// "can this player see this object right now?" `RET 0x4`.
bool SystemPlayerView_IsObjectVisible(Observer* View, GameObject* Obj);

// (module storage, `RET 0x4`) -- the same question by object id:
// true in single player; otherwise the id is resolved (the routine, the port's
// Server_LookupGameObject) and the answer is the routine's for the object, a
// null object included (module storage pushes 0 on the miss path).
bool SystemPlayerView_IsObjectIdVisible(Observer* View, unsigned int ObjId);

// (module storage, `RET 0x4`) -- multiplayer only: the tracked
// record for Rec->ObjectId gets FLPACKET_COMMON_UPDATEOBJECT (IClient +0xd8,
// SendCommonUpdateObject) when already created on the client (+0x2F0), and its
// CRemotePhysicsSimulation::RemoteUpdate(pos, orientation, time) either way.
// Only caller: StarSystem::sp_obj_update.
void SystemPlayerView_SendObjectUpdate(Observer* View, FCommonUpdateObjectInfo const* Rec);

// (module storage, `RET 0x4`) -- if ObjId is a CShip this view can
// see, IClient +0x1c4 (SendServerMiscObjUpdateObjectGroup)(view's PlayerId, ObjId, the ship's +0x1b4 group
// id). Only caller: StarSystem_SendGroupChange.
void SystemPlayerView_SendGroupChange(Observer* View, unsigned int ObjId);

// (module storage, `RET 0x4`) -- collect into Owners the owning
// player (CSimple::OwnerPlayer, +0xb4) of every tracked object this view has
// already created on its client (+0x2F0). Local chat's recipient list.
void SystemPlayerView_CollectCreatedOwners(Observer* View, std::set<unsigned int>* Owners);

// (module storage, `RET 0x4`) -- FLPACKET_SERVER_REMOTEOBJECTCARGOUPDATE
// for one view. If Obj is tracked by this view and already created on its client
// (+0x2F0), the ship's CEqObj::get_equip_desc_list (CObject vtable +0xac) fills a
// local EquipDescVector and IClient +0x154 (SendServerRemoteObjectCargoUpdate) sends it with (&view->PlayerId,
// the ship's CSimple::Id, &vector, 1). The ship is the 0x503-classed cobject(); baseline
// does not null-test it. Only caller: StarSystem_SendRemoteCargoUpdate.
void SystemPlayerView_SendRemoteCargoUpdate(Observer* View, GameObject* Obj);

// "this object has been destroyed". `RET 0x8`.
bool SystemPlayerView_OnObjectDestroyed(Observer* View, GameObject* Obj, int Kind);

// (module storage, `RET 0xc`) -- the per-player
// FLPACKET_SERVER_DAMAGEOBJECT gate. Ported 2026-09-17 (wave 13).

// baseline's three stack arguments are (object id, a record the caller already
// built, a second record the caller built locally). Both records are MSVC6
// std::lists -- the only thing this function does with either is test the
// SECOND one's _Size at +0x08 (module storage `MOV ECX,[EAX + 0x8]`) and hand
// whichever it picked to IClient SendServerDamageObject as a bare pointer. The sole caller,
// (module storage .. module storage), passes its own `param_2`
// and a std::list it filled on the stack at [ESP+0x10].

// Both records are Common's DamageList (the caller's param_2 and its stack
// DamageList), and rpclocal's DAMAGEOBJECT marshaller reads the SendServerDamageObject pointer
// as that same type (rpc_payload_ch2.cpp, MarshalDamageObject: DamageList::operator=).
// The +0x08 _Size test reads DeferredRecord->Count (the port's mirror of it).
void SystemPlayerView_DamageObject(Observer* View, unsigned int ObjId,
                                   DamageList* PrimaryRecord,
                                   DamageList* DeferredRecord);

// (module storage, `RET 0x8` -- ONE stack argument, a `double`).
// The per-frame observer update. Ported 2026-09-17 (wave 14) in
// src/Server/server_observer.cpp.

// analysis types the argument as two `undefined4`s; it is one double. The
// prologue leaves ESP at (entry - 0x2c4) and the very first thing the body does
// with it is
// dd8424c8020000  FLD   double ptr [ESP + 0x2c8]
// which is exactly the first stack argument, and `RET 0x8` at module storage
// confirms the eight bytes.

// Everything below the top-level `if (SinglePlayer()) return;`
// (module storage/module storage) runs in five phases, each one re-checking the
// IClient bandwidth gauge (see the kBandwidthCeiling note in the .cpp):
//   1. split the tracked list in two -- already-created records get
//      CRemotePhysicsSimulation::Doit(Dt), not-yet-created ones are queued;
//   2. for every not-yet-created record that is now visible, send the matching
//      CREATE* packet and move it to the created list;
//   3. for every created record: if it is no longer visible send
//      DESTROYOBJECT and clear its flag, otherwise ask CheckForSync and, when
//      it says yes, insert the record into a third list kept in DESCENDING
//      CRemotePhysicsSimulation::GetSimulationTime() order;
//   4. drain that third list into UPDATEOBJECT packets;
//   5. retry the deferred-damage set (PendingDamageIds) -- resolve each id,
//      keep the ones that are visible again, build the object's DamageList and
//      send DAMAGEOBJECT, erasing the id from the set.
void SystemPlayerView_Update(Observer* View, double Dt);

// Observer.cpp's matrix -> quaternion step: the lazily filled file-static
// .data:the field (Server_Acquire3DMathEngine, the routine) and its
// vtable +0x50, I3DMathEngine::MatrixToQuaternion. Its three baseline users are
// / module storage (the update above) and module storage
// (SystemPlayerView_AddTrackedObject). Defined in src/Server/server_observer.cpp.
void Observer_MatrixToQuat(Quaternion& Out, Matrix const& In);

// (module storage, thiscall on the block, RET 8) — walk one of
// StarSystem's ObjectLists, call GameObject::update(Dt) (vtable +0x16c) on each
// node's object, and when update returns false MetaList::remove the node and
// append its object to Deferred (always the +0xe8 list).
void StarSystem_TickObjectList(ObjectList* List, float Dt, ObjectList* Deferred);

// (module storage, `RET 0x8`) -- "this object has left the
// system". Ported 2026-09-17 (wave 13). Drops the object from the tracked list,
// CLEARS the whole deferred-damage set, latches bShipGone when the departing
// object is this player's own ship, and sends
// FLPACKET_SERVER_SYSTEM_SWITCH_OUT (IClient +0x0e4 == SendServerSystemSwitchOut) when either the
// record had been created on the client or bShipGone is now set.

// The wire payload is two dwords built on the stack at module storage /
// `{ *param_1, *param_2 }`, i.e. the object id and the
// destination the caller points at. baseline passes both by pointer.
void SystemPlayerView_OnObjectLeftSystem(Observer* View,
                                         unsigned int const* ObjId,
                                         unsigned int const* Destination);

// (module storage, `RET 0xc`) -- "this object has landed".
// Ported 2026-09-24. Drops the object from the tracked list, and when the
// record had been created on the client or the object is this player's own
// ship, sends FLPACKET_SERVER_LAND (IClient +0x0ec == SendServerLand) with the payload
// {*ObjId, *Arg2, *Arg3}; outside single player the player's own ship landing
// first autosaves the slot (the routine). Its only caller is the per-system
// step the routine at module storage, inside the deferred-object block
// .. module storage, ported 2026-09-24 in StarSystem_Update
// (IServerImpl.cpp), which walks the dock queue (SystemDockNode below).
// Arg2 is the caller's ESP+0x10 local (module storage; zeroed at 06d0c20e and
// set from a vtable +0x20 get_id at 06d0c248) and Arg3 is EBX+0x18.
void SystemPlayerView_OnObjectLanded(Observer* View,
                                     unsigned int const* ObjId,
                                     unsigned int const* Arg2,
                                     unsigned int const* Arg3);

// ---------------------------------------------------------------------------
// The five "tell this client the object exists" notifiers, and the six of the
// seven SystemPlayerView_AddTrackedObject call sites that live inside them.

// baseline has no names for them; the FDUMP each one issues when it is handed an
// already-destroyed object does, and it is baseline's own text, from
// Server\space\Observer.cpp (the path string lives at
// and is pushed at module storage as `68dc69d606`) -- so this
// family is NOT in StarSys.cpp, which is where the system itself lives:
// line 0x19b  "Ship(%d) 'd..."            -> IClient +0x0bc
// line 0x23c  "CounterMeasure(%d) ..."    -> IClient +0x0cc
// line 0x256  "Loot(%d) 'dea..."          -> IClient +0x0c0
// (none)                                  -> IClient +0x0c8
// line 0x28a  "Mine(%d) 'd..."            -> IClient +0x0c4
// and those five vtable offsets are, in rpc_iclient.h's own table,
// SendServerCreateShip, SendServerCreateCounter, SendServerCreateLoot,
// SendServerCreateGuided and SendServerCreateMine. The FDUMP text and the packet name agree independently in
// all four cases that have both, which is what fixes the routine as the guided
// one without a string of its own.

// All five share one body:
//   1. return at once when View->bShipGone is set;
//   2. per-object: when GameObject::is_destroyed() (baseline vtable +0x160), FDUMP
//      and return without tracking anything;
//   3. SystemPlayerView_AddTrackedObject(View, Obj);
//   4. outside single player, drop out silently unless
//      SystemPlayerView_ShouldBeVisible(View, Obj);
//   5. mark the record bCreatedOnClient and hand the payload to the IClient slot.
// The ship one (the routine) is the odd one out and does NOT follow 4/5 -- see
// its own comment in server_classes.cpp.

// The first argument is the record the packet carries. baseline builds it in the
// caller (the routine for ships, the routine for loot, ...) and these five
// only forward it. All five record types are published in
// src/include/server_createinfo.h, the one declaration rpclocal's marshallers
// read too. The ship notifier takes its CShipCreateInfo by type, because it
// WRITES a field of it (+0xd4, module storage); the other four only forward
// theirs and still take it as `void*`.
// TODO(port): type the other four once their callers build the records.
void SystemPlayerView_CreateShipForPlayer(Observer* View, CShipCreateInfo* Info, GameObject* Obj);
// (module storage, `RET 0x8`) -- CREATESOLAR for one view: nothing when
// bShipGone; otherwise IClient +0xb8 (SendServerCreateSolar)(PlayerId, Info) when the solar should
// be visible to this view or Info->Field_0xac is set. No tracked record is made.
// Only caller the routine (pub::SpaceObj::CreateSolar's per-player walk).
void SystemPlayerView_CreateSolarForPlayer(Observer* View, CSolarCreateInfo* Info, GameObject* Obj);
// (module storage, thiscall on the view, RET 0x54: 21 dwords) --
// FLPACKET_SERVER_SENDCOMM for one view. Outside single player both the sender and
// the receiver must be visible to the view (the routine + the routine each); then
// IClient +0x110 (SendServerSendComm) with the view's PlayerId, the 20 caller dwords and
// "the receiver is this view's own ship". Only caller StarSystem::send_comm.
void SystemPlayerView_SendComm(Observer* View, unsigned int SenderId,
                               unsigned int ReceiverId, unsigned int VoiceId,
                               Costume const& Cost, unsigned int CommType,
                               unsigned int* TextIds, int TextCount, unsigned int Mood,
                               float Duration);
void SystemPlayerView_CreateCounterForPlayer(Observer* View, void* Payload, GameObject* Obj);
void SystemPlayerView_CreateMineForPlayer(Observer* View, void* Payload, GameObject* Obj);
// / the routine take a std::vector<GameObject*> rather than one
// object: module storage is `MOV ESI,[EBX + 0x4]` against `CMP ESI,[EBX + 0x8]`,
// the vector's first/last pair. The loot one walks the whole range; the guided
// one uses only `*First` (module storage `MOV EAX,[ESI]` after checking the
// range is non-empty at module storage..06cf6d11). The port passes the range
// explicitly instead of a container so that neither the iterator-debug pointer
// nor a copy of the vector can creep in.
void SystemPlayerView_CreateLootForPlayer(Observer* View, FServerCreateLootRequest* Payload,
                                          GameObject* const* First, GameObject* const* Last);
void SystemPlayerView_CreateGuidedForPlayer(Observer* View, void* Payload,
                                            GameObject* const* First, GameObject* const* Last);

// (module storage, thiscall on the view, RET 4) -- the per-player
// half of StarSystem::send_mine_explosion. In multiplayer the mine is resolved
// by id (the routine) and the packet goes out only when this player can see it
// (the routine); in single player it always goes. IClient +0xd0 (SendServerExplodeAsteroidMine,
// FLPACKET_SERVER_EXPLODEASTEROIDMINE) with the view's player id and Rec.
void SystemPlayerView_SendMineExplosion(Observer* View, FMineExplosion* Rec);

// ---------------------------------------------------------------------------
// The element of StarSystem's second list (+0x14 .. +0x1F): one object that has
// finished docking with something this step and is waiting for the system's
// own update (the routine) to land it or hand it over.

// The shape comes from the three functions that touch the element:
// (module storage, the only producer) builds it on the stack at
//     ESP+0x14 .. ESP+0x28: two watchers zeroed at module storage..06d0d3ac,
//     `*param_3` at +0x10 and the byte `*param_4` at +0x14 (module storage ..
// );
// (module storage) copy-constructs it into the node at node+8:
//     each watcher re-watches its source's object (`[src] - 8 + 8`, i.e. the
//     same Watchable, module storage..06d0f890 and module storage..06d0f8c1),
//     then the dword at +0x10 and the byte at +0x14 are copied;
// (module storage) destroys it: the watcher at +0x08 first
//     (module storage..06d0d4d8), then the one at +0x00 (module storage..
// ) -- reverse declaration order, which is what a C++ destructor
//     does anyway.
// Both watchers watch the Watchable sub-object of a GameObject wrapper
// (the routine stores `param + 8`; the routine subtracts 8 back at
// and module storage).

// Names are the port's. baseline's element reads: +0x00 is the object that docks
// (the ship), +0x08 what it docked with (null when IServerImpl::Dock queues a
// forced landing), +0x10 the base id the LAND packet carries, and +0x14 the
// byte that selects "land" (1) over "dock without landing" (0: the
// moor_small/moor_medium/moor_large docking spheres, types 3/4/5 -- see
// pub::SpaceObj::InstantDock's docking-sphere test).
// ---------------------------------------------------------------------------
struct FSystemDockEntry {
    BaseWatcher Object;               // +0x00
    BaseWatcher Target;               // +0x08
    unsigned int BaseId;              // +0x10
    bool bLand;                       // +0x14

    FSystemDockEntry() : BaseId(0), bLand(false) {}
    // . BaseWatcher's own implicit copy would copy the watcher
    // chain link raw; baseline re-links each watcher instead.
    FSystemDockEntry(FSystemDockEntry const& Other)
        : BaseId(Other.BaseId), bLand(Other.bLand)
    {
        Object.set(Other.Object.target());
        Target.set(Other.Target.target());
    }
    FSystemDockEntry& operator=(FSystemDockEntry const&) = delete;

    // The watched Watchable back to its wrapper -- baseline's `[w] - 8` with the
    // null test (module storage .. module storage).
    static GameObject* WatchedObject(BaseWatcher const& Watcher);
};

static_assert(offsetof(FSystemDockEntry, Target) == 0x08, "FSystemDockEntry+0x08 target watcher");
static_assert(offsetof(FSystemDockEntry, BaseId) == 0x10, "FSystemDockEntry+0x10 base id");
static_assert(offsetof(FSystemDockEntry, bLand) == 0x14, "FSystemDockEntry+0x14 land flag");

// The list node. The element is embedded, like the player list's
// (SystemPlayerNode): 0x08 header + 0x18 element = 0x20, which is the size the
// constructor's sentinel (module storage `PUSH 0x20`) and every node use.
struct SystemDockNode {
    SystemDockNode* Next;             // +0x00
    SystemDockNode* Prev;             // +0x04
    FSystemDockEntry Entry;           // +0x08
};

static_assert(sizeof(SystemDockNode) == 0x20, "SystemDockNode is baseline's 0x20-byte node");

// StarSystem -- runtime per-system state in Server.dll SysDB, 0x100 bytes.
// baseline: std::map<unsigned int, StarSystem> SysDB::SysMap.

// POLYMORPHIC, and an IObjDB. RTTI settles the hierarchy: the vtable at
// has its COL at module storage -> module storage, type
// descriptor module storage ".?AVStarSystem@@", class hierarchy
// with two entries (StarSystem, IObjDB), single inheritance.
// The constructor (module storage) stores IObjDB's vtable first
// (module storage, module storage) and its own last (module storage). There is
// no virtual destructor: the vtable has exactly IObjDB's four slots.
//   slot 0 +0x00  module storage  GetNext
//   slot 1 +0x04  module storage  FindById   (-> the routine)
//   slot 2 +0x08  module storage  GetZoneDb  (ICF-shared with CPlayerGroup::GetID:
//                                             `MOV EAX,[ECX+4]; RET` = SystemInfo)
//   slot 3 +0x0c  module storage  GetBucket  (`MOV EAX,[ECX+0x20]; RET 4`)

// Layout (constructor module storage, copy constructor module storage,
// destructor body inlined in ~pair module storage):
//   +0x00 vptr   +0x04 SystemInfo   +0x08 player list   +0x14 dock queue
//   +0x20 Spatial   +0x24 a std::vector of 0xC-byte elements
//   +0x34 .. +0xFB ten ObjectLists   +0xFC bLoaded
class StarSystem : public IObjDB {
public:
    // . Only caller: SysDB's build loop (module storage), which
    // builds a temporary and inserts a copy of it.
    StarSystem();
    // (called at module storage to build the map's value_type
    // pair) and the same body inlined into the node constructor the routine.
    StarSystem(StarSystem const& Other);
    // The body of the routine after its first `LEA ESI,[ECX+0x4]` --
    // ~pair<unsigned const, StarSystem> is ~StarSystem inlined.
    ~StarSystem();
    StarSystem& operator=(StarSystem const&) = delete;

    // (module storage, `RET 0x4`): SysDB's per-entry init, run on
    // the mapped value right after the insert (module storage).
    void init(unsigned int SystemId);

    // -- IObjDB ---------------------------------------------------------------
    IObjRW* __thiscall GetNext(void* IterState) override;
    IObjRW* __thiscall FindById(unsigned const* ObjectId) override;
    Universe::ISystem* __thiscall GetZoneDb() override;
    void* __thiscall GetBucket(unsigned TypeMask) override;

    // (module storage, `RET 0x4`, 16 callers): the object with
    // this id in any of seven blocks, searched in the order +0x34, +0x48,
    // +0x70, +0x98, +0xac, +0xc0, +0x5c; null for an id with no bit inside
    // . +0x84, +0xd4 and +0xe8 are never searched.
    GameObject* find_object(unsigned int Id);

    // (module storage, `RET 0x8`, 11 callers): the same, but the
    // type mask picks ONE block (-1 falls back to find_object).
    GameObject* find_object(unsigned int Id, unsigned int TypeMask);

    // +0x04: the Universe system this entry stands for.

    // CORRECTED 2026-09-17 (wave 6). This was "secondary vtable/data pointer"
    // and the system's own id was believed to live at +0x10. Both were wrong.
    // the initializer SysDB's build loop runs on every freshly
    // inserted entry, called as the routine(node + 0x10, sysId) where node+0x10
    // IS the mapped StarSystem -- stores Universe::get_system(sysId) here.
    // So +0x04 is a Universe::ISystem*, and every "the system's own nickname id"
    // read in the module is the virtual call `(**(code**)**(undefined4**)(this+4))()`
    // on it -- the routine/the routine build their event payloads that way.
    Universe::ISystem* SystemInfo;

    // +0x08..+0x13: std::list of the players currently in this system.
    // MSVC6 std::list is {allocator byte (padded), _Head, _Size}: the ctor
    // writes the allocator byte at +0x08, the head node from
    // (0,0) (operator new(0x50), self-linked) at +0x0C and zero
    // at +0x10. count_players walks `**(this+0xC)` to the sentinel `*(this+0xC)`;
    // remove_player decrements `*(this+0x10)` after unlinking a node.
    // Node layout: SystemPlayerNode above.
    unsigned char ListAlloc;
    unsigned char Pad1[3];
    void* PlayerListHead;              // +0x0C  _Head (self-referencing sentinel)
    unsigned int PlayerListSize;       // +0x10  _Size -- NOT the system id

    // +0x14..+0x1F: a second std::list, same shape (allocator byte at +0x14, a
    // fresh operator_new(0x20) self-referencing head at +0x18, size at +0x1C).
    // It is the dock queue -- SystemDockNode / FSystemDockEntry above.
    // queue_dock (the routine) is its only producer, the landing block of the
    // system update (the routine, module storage .. module storage) its only
    // consumer, and erase_docks (the routine) empties it. The field names
    // predate that decoding.
    unsigned char ObjectListAlloc;     // +0x14
    unsigned char Pad2[3];
    void* ObjectListHead;              // +0x18  _Head
    unsigned int ObjectListSize;       // +0x1C  _Size

    // +0x20: the system's spatial partition. load() stores
    // COMMON.DLL::NewSpatial("BSP") here (module storage .. 06d07067) and bulk-
    // adds every solar; GetBucket returns it (module storage); unload frees it
    // with FreeSpatial and nulls it (module storage .. 06d075e1). The copy
    // constructor copies the pointer (module storage); the destructor does not
    // free it.
    ISpatialPartition* Spatial;

    // +0x24..+0x33: an MSVC6 std::vector {allocator byte +0x24, _First +0x28,
    // _Last +0x2C, _End +0x30} whose element is 0xC bytes (the copy
    // constructor sizes it with `IMUL 0x2aaaaaab / SAR 1`, module storage,
    // and the routine moves three dwords per element). The destructor frees
    // _First (module storage) and unload empties it (the routine,
    // ). What the element is has not been traced; no port code
    // fills it.
    unsigned char VectorAlloc;         // +0x24
    unsigned char Pad3[3];
    void* VectorBegin;                 // +0x28
    void* VectorEnd;                   // +0x2C
    void* VectorCapacity;              // +0x30

    // +0x34 .. +0xFB: the ten dispatch blocks (ObjectList above). The type
    // mask each one answers to in GetNext / find_object(Id, Mask) names it.
    ObjectList Ships;                  // +0x34  mask 0x7f0000 (IClient CREATESHIP in add_player)
    ObjectList Loot;                   // +0x48  mask 0x10000000 (Loot::get_type_bits, CREATELOOT)
    ObjectList Solars;                 // +0x5C  mask 0xfdff (CREATESOLAR; the BSP is built from it)
    ObjectList List70;                 // +0x70  mask 0x1000000 (module storage get_type_bits)
    ObjectList Beams;                  // +0x84  mask 0x2000000; the MP step calls CBeam::move on
                                       //        every object in it (module storage)
    ObjectList List98;                 // +0x98  mask 0x4000000 (module storage get_type_bits)
    ObjectList ListAC;                 // +0xAC  mask 0x8000000 (module storage get_type_bits)
    ObjectList ListC0;                 // +0xC0  masks 0x20000000 / 0x60000000; torn down next to
                                       //        the asteroid fields in unload (the routine)
    ObjectList ListD4;                 // +0xD4  in no GetNext mask; every object in it is fed to
                                       //        the per-step +0x16c pass (module storage)
    ObjectList Deferred;               // +0xE8  objects whose update() answered false; the step
                                       //        deletes them (module storage .. 06d0c1ac)

    // +0xFC: loaded flag (add_player tests it for zero at module storage,
    // load() for one at module storage).
    unsigned char bLoaded;
    unsigned char Pad4[3];

    // count_players — baseline RVA 0x2BCB0, ordinal 476
    // Iterates the player list at this+0x0C, counting entries where node[0x10] != ExcludeId.
    // The ONLY StarSystem member Server.dll exports (?count_players@StarSystem@@QBEII@Z),
    // so it carries SERVER_API itself rather than the class.
    SERVER_API unsigned int count_players(unsigned int ExcludeId) const;

    // ------------------------------------------------------------------
    // remove_player — baseline the routine(this, SessionId), the first half of
    // both the routine and the routine (the two "player left this system"
    // entry points). Two independent passes:

    //  (a) the player list at this+0x0C: walk it from `**(this+0xC)` until the
    //      sentinel; on the first node whose dword at node+0x40 (node[0x10])
    //      equals SessionId, unlink it (`[node[1]] = node[0]` /
    //      `[node[0]+4] = node[1]`), call its own `(**(code**)node[2])(0)`
    //      disposer, free the node, and decrement the count at this+0x10.
    //      baseline breaks after the first match.

    //  (b) the Ships list (head at this+0x38): for every node,
    //      take obj = node->Data (a real pointer -- module storage is a MOV, not
    //      a LEA), resolve its CBase::Object at +0x10 under the usual
    //      `(ObjectClass & 3) == 3` guard, and if that CSimple's OwnerPlayer
    //      (+0xb4) equals SessionId and `obj->is_destroyed()` (vtable +0x160) is
    //      false, call `obj->destroy(0, obj->get_id())`
    //      (vtable +0x158 with vtable +0x20's result as the second argument).

    // PORTED 2026-09-17 (wave 8). The three vtable offsets are no longer opaque:
    // +0x20 is IObjInspect::get_id (slot 8) and +0x158/+0x160 are two of the
    // four GameObject extension slots, now named in server_gameobject.h.
    void remove_player(unsigned int SessionId);

    // player_switched_out -- baseline the routine (module storage, thiscall,
    // RET 8; the name is the port's). The system-switch twin of the routine
    // (PlayerDB_LeaveSystem in server_classes.cpp). Only caller:
    // IServerImpl::SystemSwitchOutComplete (module storage), pushing
    // (SessionId, the destination system id).
    // remove_player(SessionId) (the routine)
    // the routine on Players[SessionId]
    //                               (PlayerData_FlushPendingVisits)
    // DestSystemId != 0 -> controller event 0x3a
    //                               {SystemInfo->vtable[0](), DestSystemId,
    //                               SessionId}
    // single player and bLoaded (+0xfc): clear it,
    //                               event 0x3d {system id}, unload() (the routine)
    void player_switched_out(unsigned int SessionId, unsigned int DestSystemId);

    // activate_object -- baseline the routine (module storage, thiscall, RET 0xc;
    // the name is the port's). Only caller: pub::SpaceObj::Activate
    // (module storage), pushing (obj, activate, index).
    // __RTDynamicCast<EqObj>(Obj); null -> false
    // the routine: CEqObj::activate(bOn, Index)
    //                               (vt +0xb4 on the & 0x103 narrowing)
    // Failed -> false; not Changed -> true
    // ACTIVATEOBJECT {id, bOn, Index} through
    //                               IClient SendServerActivateObject to every player when
    //                               cobject() is a solar (& 0x303), else only
    //                               where SystemPlayerView_IsObjectVisible
    // true
    bool activate_object(GameObject* Obj, bool bOn, unsigned int Index);

    // spawn_mined_loot -- baseline the routine (module storage, thiscall, RET
    // 0x10; the name is the port's). Only caller: IServerImpl::MineAsteroid
    // (module storage), pushing (&pos, crate archetype, loot archetype, count).
    // a fresh id (CFLIDMaker, inlined allocate)
    // GetEquipment(ContentsArchId); null -> return
    // container = GetEquipment(ContainerArchId) if
    //                               non-zero, else/null -> contents'
    //                               get_loot_appearance(); null -> return
    // FLootRecord {id, container, its HitPts,
    //                               contents, its HitPts, Count, 0, 0,
    //                               random_inside_sphere(20), Pos, identity,
    //                               random_inside_sphere(3), 1, 0}
    // create_loot(0, 3, &rec, 1)
    void spawn_mined_loot(Vector const& Pos, unsigned int ContainerArchId,
                          unsigned int ContentsArchId, int Count);

    // send_tractor_objects -- baseline the routine (module storage, thiscall,
    // RET 8, SEH frame; the name is the port's). Callers the routine
    // (module storage) and the routine (module storage), pushing (&the id of
    // the player to skip, &the XTractorObjects they built).
    // for every player node but *SkipPlayerId
    //                               that sees Info.ObjectId (the routine):
    //                               a copy {ObjectId, TractorEquipId, the ids
    //                               of Info.TargetIds that player sees}, IClient
    //                               +0x14 (SendCommonTractorObjects) with node+0x40, then the
    //                               copy's vector block freed (the routine)
    void send_tractor_objects(unsigned int const& SkipPlayerId, XTractorObjects const& Info);

    // tractor_loot -- baseline the routine (module storage, thiscall, RET 0x10;
    // the name is the port's). Only caller: SrvTractor::notify (module storage),
    // pushing (loot, ship, the ship's free space for the contents, the tractor's
    // equipment id).
    // CLoot (& 0x803), Take = min(Space, Units)
    //                               (unsigned), the contents' archetype id
    // Health: 1.0f; a non-combinable good carries
    //                               ContentsHitPts / HitPts; a combinable
    //                               commodity with decay > 0 taken whole carries
    //                               max(ContentsHitPts - Loot::DecayAccum, 0) /
    //                               HitPts; any other combinable good 1.0f
    // add_cargo(Ship, arch, Take, Health, bTemporary)
    // controller event 0x18 {loot id, ship id,
    //                               cargo sub id, -, Arch2Good(arch), Take}
    // ITEMTRACTORED (IClient +0x100, SendServerItemTractored) to
    //                               every player that sees the ship
    // Take < Units: Units -= Take. Otherwise the
    //                               tractor drops the target (unless a player
    //                               ship in single player), loot->destroy(2,
    //                               ship id), Units = 0
    void tractor_loot(::Loot* LootObj, EqObj* Ship, unsigned int Space, unsigned short TractorId);

    // use_item -- baseline the routine (module storage, thiscall, RET 0xc; the
    // name follows its packet, FLPACKET_SERVER_USE_ITEM). Only caller: the EqObj
    // IObjAffect method the branch (module storage), after
    // CECargo::RemoveFromStack, pushing (the EqObj, equip id, count).
    // a player's ship (is_valid_ship_owner,
    //                               inlined): the routine(slot, id, count, 0)
    //                               (PlayerData_RemoveEquipment)
    // USE_ITEM {id, EquipId, count} through
    //                               IClient +0x104 (SendServerUseItem) to every player
    //                               that sees the object (the routine)
    void use_item(GameObject* Obj, unsigned short EquipId, int Count);

    // sp_obj_update -- baseline the routine (module storage, thiscall, RET 8; the
    // name follows its caller). Only caller: IServerImpl::SPObjUpdate
    // (module storage), pushing (&info, the sender's session id).
    // the ship (+0x34 block) whose CSimple id is
    //                               Info.ObjectId; none -> return
    // PhySys::RCSet(ship, pos, orientation, time)
    // FCommonUpdateObjectInfo from Info
    // the routine (SystemPlayerView_SendObjectUpdate)
    //                               for every player but SessionId
    void sp_obj_update(SSPObjUpdateInfo const& Info, unsigned int SessionId);

    // scan_objects -- baseline the routine (module storage, thiscall, RET 0x14;
    // the name is the port's). Callers in pub::System / pub::SpaceObj
    // (module storage, 06cf9c98, 06cfa0c4, 06cfa105, 06cfad4f).
    // TypeMask & 0x7f0000: the ship block's
    //                               collect_in_sphere (the routine)
    // TypeMask & 0xfdff == 0 -> return
    // Radius not > 0: the solar block's
    //                               collect_in_sphere into the rest (06d0be11)
    // otherwise GetBucket(0xfdff)'s Fill (vt +0x1c)
    //                               with {Pos, Radius} into a 0x800-slot block;
    //                               each hit whose get_type (vt +0x88) succeeds
    //                               and meets TypeMask adds its get_id()
    // Returns how many ids were written.
    unsigned int scan_objects(unsigned int* OutIds, unsigned int MaxCount, Vector const* Pos,
                              float Radius, unsigned int TypeMask);

    // ------------------------------------------------------------------
    // add_player -- baseline the routine(this, SessionId), module storage,
    // `RET 0x4`. The counterpart remove_player never had; called from
    // , module storage and module storage.

    // Its head is the only thing in the whole module that ever grows the
    // player list, and it is four steps:
    // MOV AL,[EDI+0xfc] / CMP AL,BL / JNZ -- when the system
    //                   is not loaded yet, load it first (the routine, a
    //                   thiscall: ECX is still `this`; it sets +0xfc to 1,
    //                   starts PhySys and reads the system ini).
    // TEST EBP,0x3fffffff / JZ -- a session id with no bit
    //                   inside 0x3fffffff is rejected outright. Note the mask:
    //                   it is NOT `!= 0`, the top two bits do not count.
    // a Observer is built on the STACK from the
    //                   session id (the routine), handed to
    //                   `std::list<Observer>::insert` at
    // with `LEA ECX,[EDI+0x8]` as the list and
    //                   `[EDI+0xc]` (the sentinel) as the position -- so it is
    //                   a push_back -- and then destroyed again
    //                   (module storage).
    // the live element is recovered as
    //                   `[[EDI+0xc] + 4] + 8`, i.e. sentinel->prev->View.

    // Everything past module storage is the initial synchronisation, and it is
    // three passes over three of the system's dispatch blocks. Which block is
    // which is not a guess: each pass ends in a different IClient create slot.
    //   block at +0x34 (head +0x38)  SHIPS   -- module storage / module storage
    //   block at +0x48 (head +0x4c)  LOOT    -- module storage / module storage
    //   block at +0x5c (head +0x60)  SOLARS  -- module storage
    // The counts come from the routine(block, 0xffffffff), which is NOT a plain
    // size: it walks the NULL-terminated list at block+0x04 and counts only the
    // nodes whose object answers a non-zero `mask & get_type_bits()`
    // (module storage `CALL [EAX + 0x154]` / module storage `TEST EBX,EAX`).

    // The single-player / multiplayer split is the opposite way round from what
    // it looks like, and module storage `JZ the branch` settles it: the JZ is
    // taken when SinglePlayer() returned FALSE.
    //   SINGLE PLAYER -- build the wire payload for every object and send it
    //     straight away (CREATESHIP at module storage, CREATELOOT at
    // ). Consistent with AddTrackedObject seeding
    //     bCreatedOnClient from SinglePlayer(): the local client is told
    //     everything at once.
    //   MULTIPLAYER -- only register each object in the joining player's tracked
    //     list (module storage and module storage, two of the seven
    // call sites) and leave bCreatedOnClient false, so the
    //     per-frame range check sends the creates later.
    // The one thing multiplayer does eagerly is the SOLAR pass
    // (module storage..06d082f5), preceded by PlayerDB::SendSystemID at
    // . Per node: a DYNAMIC solar (CSolar+0x1a9) gets CREATESOLAR
    // (IClient SendServerCreateSolar, payload from the routine); every other node, static
    // solars included, gets its damage list (SendServerDamageObject) if it has one. Every
    // solar then gets SETREPUTATION (SendServerSetReputation), one SETTARGET per
    // functioning gun with a target (SendCommonSetTarget) and one ACTIVATEOBJECT per
    // active state (SendServerActivateObject).

    // PORTED: the load call, the mask guard, the insert, SendSystemID, both
    // multiplayer tracked-list passes (wave 12), both single-player create
    // passes and the multiplayer solar walk (2026-09-24).
    // ------------------------------------------------------------------
    void add_player(unsigned int SessionId);

    // ------------------------------------------------------------------
    // load -- baseline the routine (module storage). add_player's first
    // statement, and the only thing that ever puts objects into a system.

    // ARITY CORRECTION. analysis declares it `void the routine(void)` -- no
    // `this` at all, which is why every earlier note called the system id
    // unreachable from inside it. The listing says otherwise:
    // 8bd9          MOV  EBX,ECX            ; ECX IS `this`
    // 80bbfc000...  CMP  byte ptr [EBX + 0xfc],0x1
    // c3            RET                     ; no stack args
    // so it is `void __thiscall StarSystem::load()`. Note also that the guard
    // compares against 1, not against zero, while add_player's own guard
    // (module storage `CMP AL,BL` with EBX = 0) compares against zero.

    // Body, in order:
    // sysId = SystemInfo->vtable[0]()
    // COMMON.DLL::PhySys::Startup(sysId)   -- cdecl, one arg.
    //                   (It is PhySys::Startup, not "PhySys::Start".)
    // if SinglePlayer():
    // the routine(this)  -- wrap every CObject the client
    //                       already built: CObject::FindFirst(0x303) / AddRef /
    //                       operator new(0x90) / the routine / the routine /
    //                       vtable +0x170 / vtable +0x1bc / Release, push_back
    //                       onto the +0x5c block. FDUMPs "failed to find any
    //                       solar objects\n" (StarSys.cpp line 0x129) when the
    //                       walk found nothing.
    // the routine(sysId)
    //   otherwise:
    // name = SystemInfo->vtable[0x14]()
    // Universe::get_filename(&path, name)
    // CmnAsteroid::Load(sysId)
    // the routine(sysId, &path)
    // INI_Reader over `path`, and for every [Object]
    //                       section the routine(this, reader) (module storage).
    // count = the routine(this + 0x5c, 0xffffffff)
    // this->Spatial = COMMON.DLL::NewSpatial("BSP")
    // when that returned non-null, build a count * 0x14 array,
    //                   one 0x14-byte record per solar: {object, its position
    //                   (three dwords copied off vtable[0]'s result), its radius}
    //                   and hand it to the partition's vtable +0x14 as
    //                   (array, count) -- module storage pushes the count first
    //                   and the array second, so the array is the FIRST
    //                   argument. Then free the array (module storage).
    //                   The radius is a FLOAT, not the dword the analysis
    //                   shows: module storage is `FLD float ptr [EAX + 0x38]`
    //                   against module storage `FSTP float ptr [EDI]`.
    // bLoaded = 1
    // the routine(0x3c, &sysId)   -- "system loaded" event.

    // PORTED 2026-09-17 (wave 12) as this member; since 2026-09-24
    // (claude-code:w5-server-06d0) the populate half is baseline's too:
    // load_solars (the routine) and load_object (the routine) below. The
    // port's former StarSystem_LoadObjects rewrite is gone.
    // ------------------------------------------------------------------
    void load();

    // ------------------------------------------------------------------
    // load_solars -- baseline the routine (module storage, thiscall, no stack
    // arguments; the name is the port's). Single player only: the client
    // already built the system's solars, so each one is wrapped.
    // CObject::FindFirst(0x303); null -> the
    //                               warning below
    //   per object (module storage .. 06d0727c):
    //     CObject::AddRef; operator new(0x90) + Solar ctor (the routine);
    //     attach_existing(object when its class & 0x303 == 0x303, else 0)
    //     (the routine); enter_system(this, 0) (vt +0x170); check_fuses
    //     (vt +0x1bc); CObject::Release; the wrapper appended to Solars
    //     (+0x5c, MetaList append inlined); ++count; CObject::FindNext
    // count == 0 -> FDUMP(0x100002, "%s(%d) :
    //                               *** WARNING: %s", StarSys.cpp, 0x129,
    //                               "failed to find any solar objects\n")
    // ------------------------------------------------------------------
    void load_solars();

    // ------------------------------------------------------------------
    // load_object -- baseline the routine (module storage, thiscall, RET 4; the
    // name is the port's). Multiplayer: one [Object] section of the system file.
    // a CSolar::CreateParms on the stack (the
    //                               inlined ctor: the routine, the routine,
    //                               EquipDescVector, the CollisionGroupDesc
    //                               list, the ID_String clears and zeroes, and
    //                               the anim-name std::list at +0xe4 whose ctor
    // allocates the 0xc head node)
    // CSolar::ReadObj(Reader, &Arch, &Parms);
    //                               its bool is dropped
    //   Arch == 0 (module storage .. 06d0a043): the parms destroyed, return 0
    //   otherwise (module storage .. 06d09fd7):
    //     Arch vt +0xc (Archetype::Root::load); operator new(0x90) + Solar
    //     ctor; create(Arch) (vt +0x164); CSolar::init(Parms) on the CBase
    //     object under the 0x303 guard; the wrapper appended to Solars;
    //     Archetype::Root::Release; enter_system(this, 0) (vt +0x170);
    //     check_fuses (vt +0x1bc); the parms destroyed; return the wrapper
    // ------------------------------------------------------------------
    GameObject* load_object(INI_Reader& Reader);

    // ------------------------------------------------------------------
    // create_solar -- baseline the routine (module storage .. 06d0a763,
    // thiscall, RET 4; the name is the port's). A scripted solar from a
    // CREATESOLAR request; only caller pub::SpaceObj::CreateSolar
    // (module storage). Returns the new wrapper (EBX). Req is written to:
    // +0xa8 and +0xb0 per player before the routine hands it on.
    // operator new(0x90) + Solar ctor
    //                               (the routine); Archetype::GetSolar(Req
    //                               +0x08), load (vt +0xc), create (vt +0x164),
    //                               Root::Release; the 0x303 CSolar kept
    // CSolar::CreateParms at [S+0x28]: the routine
    //                               (the CSimple / CEqObj halves), then the
    //                               CSolar tail: ID_String clears +0xb8 / +0xc0
    //                               / +0xcc, +0xbc / +0xc4 / +0xc5 bytes 0,
    //                               +0xc8 / +0xd0..+0xe0 zero, the anim list at
    //                               +0xe4 (the routine head, size 0)
    // Id, ObjDb = this, OwnerPlayer 0, HitPts =
    //                               FILD +0x68, Pos, Orient = QuaternionToMatrix
    //                               (+0x58) via the field, +0xbc <- +0xb4,
    //                               +0x5c and +0xe0 <- +0x0c, +0x60 <- +0x10,
    //                               +0x64..+0x70 <- +0x14..+0x20, +0x98 <- +0x48,
    //                               +0x94 <- +0x44 and that many dwords from +0x24
    // +0x6c != 0 -> Equipment = Loadout::Get(+0x6c)
    //                               ->Items (vector operator=); then
    //                               EquipDescVector::append(Req +0x70)
    // ArchGroupDescs = Req +0x80 (list operator=,
    //                               inlined)
    // +0xb8 <- +0xb8, +0xc4 = 1, +0xc5 <- +0xac;
    //                               CSolar::init
    // +0xbc & 0x3fffffff -> mark_for_player(+0xbc, 1)
    // enter_system(this, 0) (vt +0x170),
    //                               check_fuses (vt +0x1bc)
    // Solars append (the routine node, inlined link)
    // GetBucket(0xfdff) non-null -> its vt +0x10
    //                               with {wrapper, get_position(), CSolar radius
    //                               +0x38} (the radius read first)
    // CObject::enable_collisions_r(true)
    // every player node: Req +0xa8 = is_marked_by
    //                               (node +0x40), Req +0xb0 = Players[id].rep_vibe,
    //                               SystemPlayerView_CreateSolarForPlayer
    //                               (the routine)(&node->View, &Req, wrapper)
    // the parms destroyed (anim list cleared and
    //                               its head freed, the group list, the vector)
    // ------------------------------------------------------------------
    ::Solar* create_solar(CSolarCreateInfo& Req);

    // ------------------------------------------------------------------
    // create_ship -- baseline the routine (module storage .. 06d0aef2, thiscall,
    // RET 4; the name is the port's). Callers pub::SpaceObj::Create
    // (module storage) and IServerImpl::PlayerLaunch (module storage). Returns
    // the new wrapper (EBX). S = ESP after the four register pushes.
    // operator new(0xc8) + Ship ctor (the routine)
    // a CShipCreateInfo copy of Req at [S+0x2c]
    //                               (the routine + the derived tail)
    //   owner +0xac != 0 and SinglePlayer (module storage .. 06d0a9ec):
    //     find(Req +0x04) (vt +0x168); CShip = the 0x503 narrowing or 0
    //   otherwise (module storage .. 06d0ad15):
    //     Archetype::GetShip(copy +0x08), load (vt +0xc), create (vt +0x164),
    //     Root::Release; CShip = the 0x503 narrowing or 0;
    //     CShip::CreateParms at [S+0x10c]: the routine, then +0xe8 / +0xec /
    //     +0xf0 = 0, byte +0xf4 = 0; Id, ObjDb = this, OwnerPlayer <- +0xac,
    //     HitPts FILD +0x68, Pos, Orient = QuaternionToMatrix(+0x58) via
    // , Velocity <- +0xb8, AngularVelocity <- +0xc4, +0x60 <-
    //     +0x10, +0xf4 <- byte +0xdc, +0x64..+0x70 <- +0x14..+0x20, +0x98 <-
    //     +0x48, +0xf0 <- +0xb4, +0x94 <- +0x44 and that many dwords from
    //     +0x24; +0x6c != 0 -> Equipment = Loadout::Get(+0x6c)->Items;
    //     EquipDescVector::append(copy +0x70); ArchGroupDescs = copy +0x80;
    //     +0xec <- +0xb0, +0x5c <- +0x0c; CShip::init (vt +0x10c); owner != 0
    //     and Players[owner].group_ptr != 0 -> CShip +0x1b4 = group +0x04; the
    //     parms destroyed
    // Ship::FEquipState from Req +0xe0 / +0xe4;
    //                               Req +0xe8 & 0x3fffffff -> mark_for_player(it, 1)
    // enter_system(this, 0) (vt +0x170),
    //                               set_equip_state (the routine)
    // Ships append (the routine, inlined link)
    // !SinglePlayer || owner == 0 ->
    //                               CObject::enable_collisions_r(true) on the CShip
    // every player node but the owner: copy +0xa8 =
    //                               is_marked_by(id), SystemPlayerView_
    //                               CreateShipForPlayer (the routine)(&view,
    //                               &copy, wrapper)
    // the copy destroyed
    // ------------------------------------------------------------------
    ::Ship* create_ship(FShipCreateRequest const& Req);

    // ------------------------------------------------------------------
    // on_object_destroyed -- baseline the routine (module storage, RET 0x8).
    // GameObject::destroy's middle statement: `MOV ECX,[ESI + 0x28]` then
    // `CALL the routine` at module storage's caller module storage, so the
    // `this` really is the object's own OwnerSystem.

    //  (a) resolve the object's CSimple::OwnerPlayer (+0xb4) under the usual
    //      (ObjectClass & 3) == 3 guard (module storage..06d0b182); when it is
    //      zero, skip straight to (b).
    //  (b) PlayerDB::is_valid(&Players, &sessionId) and, only when the slot's
    //      ship object id (+0x3E4) is this object's own get_id()
    //      (module storage `CMP [ESI + EAX + -0x34],EBX`):
    // the routine(slot)            terminate the trade
    // PUSH 1 / the routine(slot)   cancel the request
    // CMP EBP,0x1 -> the routine(slot, 0.0f, false)
    // slot->ship_object_id = 0
    // the routine(0x37, {slot->system_id, sessionId})
    //  (c) unconditionally, for every node of the player list at +0x0C, call
    // (node + 8, Obj, Kind).

    // Kind is the FIRST argument of GameObject::destroy; the second (the object
    // id) never reaches here.
    // ------------------------------------------------------------------
    void on_object_destroyed(GameObject* Obj, int Kind);

    // ------------------------------------------------------------------
    // find_solar -- baseline the routine (module storage, RET 0x4; the key is
    // passed by pointer, `CMP EDX,[EBX]` at module storage). Walks the SOLAR
    // block's list (node = [this+0x60], module storage, then node = node->Next)
    // and returns node->Data -- the GameObject wrapper -- of the first node
    // whose CSolar::Nickname (+0x1a4, module storage) equals *NicknameId.
    // The loop stops on the node after the match (module storage `TEST EAX,EAX`
    // / JNZ exit), so the FIRST match wins. Class test per node:
    // [Data+0x10] & 0x303 == 0x303 (module storage..06d0c827), otherwise the
    // object pointer is zeroed and +0x1a4 is read off it anyway -- baseline
    // relies on the block holding solars only.
    // Callers: pub::System::Find (module storage), SystemSwitchOutComplete
    // (module storage), the routine (module storage).
    // ------------------------------------------------------------------
    GameObject* find_solar(unsigned int const* NicknameId) const;

    // ------------------------------------------------------------------
    // create_med_asteroid / create_med_mine -- baseline the routine /
    // (thiscall, RET 4; the one argument is a POINTER to the id,
    // handed on unchanged as find()'s `unsigned int const&`). Both bodies are
    // the same four steps (module storage .. 06d09c7b / module storage ..
    // ):
    //   operator new(0x68 / 0x88) + the class constructor (a null allocation
    //   leaves the object pointer 0 and carries on);
    //   push the object onto the +0xc0 block (ListC0): a pooled node from
    //   FreeList (+0xd0) when bUseHeap (+0xcc) is set and the pool is not
    //   empty, else operator new(0xc); Next = 0, Prev = Tail, Data = object;
    //   linked after Tail, or made Head when there is no Tail; Tail = node --
    //   MetaList::append(T*) inlined;
    //   object->find(*Id)                           (vtable +0x168);
    //   object->enter_system(this, nullptr)         (vtable +0x170);
    // and return the object. Only caller: FSrvAsteroidCube::apply_marker
    // (module storage / module storage).
    // ------------------------------------------------------------------
    SrvAsteroid::MedAsteroid* create_med_asteroid(unsigned int const& ObjId);
    SrvAsteroid::MedMine* create_med_mine(unsigned int const& ObjId);

    // ------------------------------------------------------------------
    // send_mine_explosion -- baseline the routine (module storage, thiscall,
    // RET 4). Copies the four dwords of *Rec onto the stack (module storage ..
    // ) and hands that copy to SystemPlayerView_SendMineExplosion
    // (the routine) for every node of the player list at +0x0c, in list
    // order. Only caller: MedMine::update (module storage).
    // ------------------------------------------------------------------
    void send_mine_explosion(FMineExplosion const& Rec);

    // ------------------------------------------------------------------
    // explode -- baseline the routine (module storage, thiscall, RET 4). One
    // explosion in this system. Seven callers: module storage, module storage
    // (EqObj::explode), module storage, module storage, module storage,
    // and MedMine::update (module storage).
    //  (1) Inflictor = find_object(Blast.InflictorId) (module storage).
    //  (2) PhySys::FindSphereCollisions(system id, Blast.Position,
    //      Blast.Explosion->Radius, parts[0x3c], 0x3c) (module storage); a
    //      std::vector<CObject*> reserved to that count collects GetRoot(part)
    //      for every part whose PhySys::GetCollisionGroup is not the
    //      record's SkipCollisionGroup (stricmp, both non-null).
    //  (3) Damage pass, only when Explosion->HullDamage > 0 or
    //      EnergyDamage > 0 (module storage / module storage): per root of
    //      class (& 3) == 3 whose CSimple id is not the exploding object's and
    //      not already seen: find_object(id); live objects -- unless the
    //      GbAllowPvp gate says two players do not hurt each other --
    //      get a DamageList {cause, inflictor id, inflictor's OwnerPlayer},
    //      explosion_hit(Blast, list) and apply_damage_list(list).
    //  (4) Impulse pass, only when Explosion->Impulse != 0 (module storage):
    //      per root of class (& 3) == 3 with OwnerPlayer 0 -- duplicates
    //      included -- the unit vector from the blast to its centre of mass
    //      (a random unit vector when they coincide) times Impulse, applied
    //      with PhySys::AddImpulseAtPoint at Blast.Position when the object has
    //      a physics body (CObject+0x54).
    // ------------------------------------------------------------------
    void explode(ExplosionDamageEvent const& Blast);

    // ------------------------------------------------------------------
    // munition_collision -- baseline the routine (module storage, thiscall,
    // RET 4). One munition hitting one object. Three callers, each passing a
    // 0x1c-byte SSPMunitionCollisionInfo: IServerImpl::SPMunitionCollision
    // (module storage, the client's record as is), Guided +0x1a4
    // (module storage) and Bullet +0x1a4 (module storage). The record, as
    // this body reads it: +0x00 the munition archetype id
    // (Archetype::GetEquipment), +0x04 the shooter's object id, +0x08 the
    // object hit, +0x0c the sub-object hit (word), +0x10 the hit position.
    //  (1) Target = find_object(+0x08), Shooter = find_object(+0x04).
    //  (2) GbAllowPvp == 0 and a Shooter that is_player(): return
    //      unless there is a Target and it is not a player.
    //  (3) No Target, or a destroyed one: return.
    //  (4) FMunitionImpact {shooter, munition, sub-object, hit position} and
    //      a DamageList {cause 2 for an ObjectType 0x2000000 munition, 3 for
    // , else 1; inflictor = shooter id; inflictor owner = the
    //      Shooter's CSimple OwnerPlayer when the Shooter exists};
    //      Target->munition_hit, then Target->apply_damage_list.
    // ------------------------------------------------------------------
    void munition_collision(SSPMunitionCollisionInfo const& Info);

    // ------------------------------------------------------------------
    // create_loot -- baseline the routine (module storage, thiscall, RET 0x10;
    // the name is the port's). Every caller pushes (Count, Records, Mode,
    // source->get_id()) with ECX = the source's OwnerSystem:
    //   Ship jettison tick   module storage .. 06d017f6  (Mode 1, Count 1)
    //   EqObj dump_cargo     module storage               (Mode 2)
    //   EqObj drop_loot      module storage area          (Mode 2)
    //   EqObj destroy_equip  module storage area, twice   (1 when the cause is
    //                        0x16, else 2)
    // One Loot per record (operator new(0x70), module storage). Records is
    // only read (EBP walks it at a 0x6c stride, module storage).
    // ------------------------------------------------------------------
    void create_loot(unsigned int SourceObjId, int Mode, FLootRecord const* Records, int Count);

    // ------------------------------------------------------------------
    // Four members EqObj/GameObject reach through OwnerSystem, ported
    // 2026-09-24 (claude-code:w5-server-06ce). Names are the port's.

    // notify_object_left -- the routine (module storage, RET 4). Walks the
    // ship block (+0x34, head +0x38) then the solar block (+0x5c, head +0x60)
    // and calls every object's GameObject +0x178 (on_object_left_system) with
    // ObjId. Caller: GameObject::leave_system (module storage).
    void notify_object_left(unsigned int ObjId);

    // collision_damage -- the routine (module storage, RET 8). Damages Target
    // for a collision: skipped when the field is clear and both sides are
    // players, and when Target is null or already destroyed; otherwise a
    // cause-1 DamageList (inflictor = the source, its owner player) through
    // Target's +0x17c then +0x188. Caller: GameObject::on_collision.
    void collision_damage(FCollisionDamage const& Rec, GameObject* Target);

    // badlands_collision -- the routine (module storage, RET 4). The object
    // named by Info.ObjectId (+4), found in this system (the routine), is
    // destroyed -- kind 1, killer Info.Field_0x00 -- unless it is missing,
    // already destroyed (vt +0x160) or invincible (vt +0x19c). Only caller
    // IServerImpl::SPBadLandsObjCollision (module storage).
    void badlands_collision(SSPBadLandsObjCollisionInfo const& Info);

    // collect_dockable_solars -- the routine (module storage, RET 8). Appends to
    // Out every object of the solar block (+0x5c) whose CEqObj has a docking
    // base id (+0x164) -- all of them when BaseId is 0, else the ones whose id
    // equals it -- and, only when BaseId is 0, every one without a docking base
    // whose TypeBits (+0xe0) lie inside 0xc40. Callers IServerImpl::
    // SPRequestInvincibility (module storage) and the routine (module storage).
    void collect_dockable_solars(std::list<GameObject*>& Out, unsigned int BaseId);

    // send_comm -- the routine (module storage, thiscall, RET 0x58: 22 dwords, the
    // Costume by value). pub::SpaceObj::SendComm's system half: resolve the sender's
    // position (when it has an id), then for every valid player view either send
    // FLPACKET_SERVER_SENDCOMM (the routine) or skip it, by who the view's ship is,
    // bBroadcast and a 10000-unit radius. Only caller pub::SpaceObj::SendComm
    // (module storage).
    void send_comm(unsigned int SenderId, unsigned int ReceiverId, unsigned int VoiceId,
                   Costume const& Cost, unsigned int CommType, unsigned int* TextIds,
                   int TextCount, unsigned int Mood, float Duration, bool bBroadcast);

    // broadcast_fuse -- the routine (module storage, RET 4). IClient +0x15c
    // (BURNFUSE) to every player of this system who can see Info.ObjectId
    // (the routine). Callers: EqObj::light_fuse / unlight_fuse.
    void broadcast_fuse(FServerBurnFuseInfo const& Info);

    // ------------------------------------------------------------------
    // The IObjAffect notifiers Ship/EqObj/Guided call after changing a ship's
    // controls (2026-09-24, claude-code:w5-server-06d0). Names are the port's;
    // bodies and instruction maps in server_classes.cpp. Every one tells the
    // players of this system who can see the object (the routine /
    // ), and the ones with SourceClient skip the player whose id
    // equals it -- the client that asked for the change.

    // (RET 0x10) -- IClient +0x04 (XActivateEquip). EqObj slot_4c.
    void notify_activate_equip(unsigned int const& ObjId, unsigned short const& EquipId,
                               bool bActivate, int const& SourceClient);
    // (RET 0x10) -- IClient +0x08 (XActivateCruise). Ship slot_50.
    void notify_cruise(unsigned int const& ObjId, bool bEngage, bool bArg2,
                       int const& SourceClient);
    // (RET 0xc) -- IClient +0x0c (XActivateThrusters). Ship slot_54.
    void notify_thrusters(unsigned int const& ObjId, bool bActivate, int const& SourceClient);
    // (RET 0x14) -- IClient +0x10 (XSetTarget). Ship set_target,
    // EqObj slot_44, Guided set_target.
    void notify_set_target(unsigned int const& ObjId, unsigned short const& SubObjId,
                           IObjRW* Target, unsigned short const& SubTarget,
                           int const& SourceClient);
    // (RET 0xc) -- the owner's PlayerData loses the items
    // (the routine), then IClient +0x20 (XJettisonCargo). Ship slot_38.
    void notify_jettison(GameObject* Obj, unsigned short EquipId, int Count);
    // (RET 8; the name is the port's) -- a ship has gone into a
    // dock (base, gate, lane). Callers: EqObj::process_dock_queue
    // (module storage / 06ce7b5b) and pub::SpaceObj::InstantDock
    // (module storage), each pushing (Dock, ShipObj) so ShipObj is the first
    // argument. ShipObj must be a Ship: the body writes its +0x90 / +0x94.
    // ShipObj +0x90 (DeathSequenceObjId) != 0 ->
    //                               return (already on its way out)
    // every unowned ship in the ship block with a
    //                               behavior interface gets a type-3 behavior
    //                               event {3, &{Dock id, Dock get_type (vt
    //                               +0x88), ShipObj id}} through IBehaviorManager
    //                               slot 0 -- GAP, see the body
    // controller event 0x11 {ShipObj id, Dock id,
    //                               system id}
    // every player view: the routine(view,
    //                               &ShipObj id, &Dock id)
    // ShipObj +0x90 = Dock id
    //   owned ship (module storage .. 06d0cbe3): Dock type bits 0x40 or 0x800
    //       -> ShipObj set_invincible({2, 1, 1, 0.0f}) (vt +0x194)
    //   unowned, single player (module storage .. 06d0cb32): CShip +0x214 = 0
    //   unowned, multiplayer (module storage .. 06d0cb9f):
    //       CObject::enable_collisions_r(false), then ShipObj +0x94 =
    //       12.0f (module storage) when the Dock's type bits have 0x40 or
    //       0x800, else 0.0f (module storage)
    void complete_dock(GameObject* ShipObj, GameObject* Dock);

    // ------------------------------------------------------------------
    // The system-transfer trio (names are the port's). Callers:
    // IServerImpl::SystemSwitchOutComplete (module storage / 06cef921 /
    // ), pub::SpaceObj::JumpIn / Relocate (module storage / 06cfa32f /
    // / 06cfa43c / 06cfa47b) and InstantDock (module storage /
    // ).

    // arrive_ship -- the routine (RET 0x10), pushed (Obj, JumpObj, &Pos, &Orient):
    // Bits = JumpObj ? JumpObj->get_type_bits() : 0
    // Obj's CSimple::OwnerPlayer (inlined (& 3)
    //                               accessor, read unguarded); owned -> skip
    //   unowned: Bits 0x40 or 0x800 -> CShip::jump_in(Obj, Pos, Orient, 0.5f)
    //            (module storage PUSH 0x3f000000; JumpObj argument is Obj
    //            itself, EDI); else Bits 0x400 -> CShip::airlock_in(Pos, Orient);
    //            the CShip is the 0x503-narrowed object, not null-tested
    // FPayloadServerSystemSwitchIn {Obj id, Bits,
    //                               Pos, MatrixToQuaternion(Orient)} (the
    //                               StarSys.cpp engine, .data:the field)
    // every player view with the routine(view,
    //                               Obj): IClient +0xe8 (SendServerSystemSwitchIn)(node+0x40, &rec)
    void arrive_ship(GameObject* Obj, GameObject* JumpObj, Vector const& Pos, Matrix const& Orient);

    // remove_ship -- the routine (RET 8), pushed (Obj, OutParms):
    // Owner = Obj's CSimple::OwnerPlayer (inlined
    //                               (& 3) accessor, read unguarded)
    // every player view whose PlayerId (+0x40) is
    //                               not Owner: the routine(view, Obj, 0)
    // Server_LookupCacheForget(Obj->get_id())
    // the first Ships node (head +0x38) holding Obj
    // found: MetaList::remove inlined (FreeList
    //                               +0x44 when bUseHeap +0x40, else operator
    //                               delete the routine)
    // / 06d0d0ea   Obj->leave_system(OutParms) (vt +0x174) on
    //                               both the found and the not-found path
    void remove_ship(GameObject* Obj, PhySys::CreateParms* OutParms);

    // add_ship -- the routine (RET 0x10), pushed (Obj, Parms, &Pos, &Orient):
    // Parms +0x14 = SystemInfo->vtable[0]() (the
    //                               system id; Parms not null-tested)
    // the raw object under the (& 1) test, NOT
    //                               null-tested after it: +0x2c = Pos, then
    //                               +0x08 = Orient (REP MOVSD, 9 dwords)
    // NeuronRuntime::Engine vt +0x6c UpdateInstance(its
    //                               +0x04 instance, 0, 0)
    // Obj->enter_system(this, Parms) (vt +0x170)
    // Ships.append(Obj), inlined (the routine)
    // CShipCreateInfo (ctor the routine inlined
    //                               tail); the 0x503-narrowed CShip non-null ->
    // (&Info, CShip)
    // every player view: Info +0xa8 =
    //                               Obj->is_marked_by(PlayerId) (the routine),
    //                               then the routine(view, &Info, Obj)
    // the CShipCreateInfo destructor, inlined
    void add_ship(GameObject* Obj, PhySys::CreateParms* Parms, Vector const& Pos, Matrix const& Orient);

    // launch -- the routine (module storage, thiscall, RET 0xc). baseline's own
    // name, from its FDUMP "StarSystem::launch() called on a player!"
    // (StarSys.cpp line 0x9f2). `this` is not read. Only caller:
    // pub::SpaceObj::InstantDock's launch arm (module storage), after add_ship.
    // ShipObj's CSimple::OwnerPlayer (inlined (& 3)
    //                               accessor, read unguarded) != 0 -> FDUMP
    //                               0x100001 "%s(%d) : *** ERROR: %s" and return
    // Dock's object under (& 0x103), null tests on
    //                               Dock and on the object
    // ShipObj's object under (& 0x503), not tested
    // CShip::launch(ShipObj, the CEqObj, LaunchType)
    // Ship::apply_launch_invincibility (the routine)
    void launch(GameObject* ShipObj, GameObject* Dock, int LaunchType);
    // (RET 0x14) -- controller event 0x16, CShip::use_tradelane(true),
    // the unowned ships' behaviors, IClient +0x18 (XGoTradelane).
    // IServerImpl::GoTradelane and Ship go_tradelane.
    void notify_go_tradelane(unsigned int ShipId, unsigned int EntranceId, unsigned int ExitId,
                             bool bFastExit, float Speed);
    // (RET 0xc) -- controller event 0x17, CShip::use_tradelane(false),
    // the unowned ships' behaviors, IClient +0x1c. IServerImpl::StopTradelane and
    // Ship stop_tradelane.
    void notify_stop_tradelane(unsigned int ShipId, unsigned int EntranceId, unsigned int ExitId);

    // remove_from_list_d4 -- the routine (module storage, RET 4). Unlinks the
    // first node of ListD4 (+0xD4) whose object is Obj (MetaList::remove,
    // inlined). Caller: EqObj::destroy_notify, once per DockQueue entry.
    void remove_from_list_d4(GameObject* Obj);

    // add_to_list_d4 -- the routine (module storage, RET 4; the name is the
    // port's). Appends Obj to ListD4 (+0xD4, MetaList append inlined via
    // ) when its get_type_bits() (vt +0x154) has any bit of 0xfdff;
    // otherwise nothing. Caller: the EqObj dock-queue insert (module storage),
    // once the queue entry is linked.
    void add_to_list_d4(GameObject* Obj);

    // ------------------------------------------------------------------
    // remove_solar -- baseline the routine (module storage, thiscall, RET 4; the
    // name is the port's). Only caller: Solar::destroy (module storage).
    // Server_LookupCacheForget(&Obj->get_id())
    //                               (the routine)
    // the first Solars node (+0x5c, head +0x60)
    //                               whose Data is Obj; none -> return false
    // MetaList::remove inlined: unlink, then the
    //                               node to FreeList (+0x6c) when bUseHeap (+0x68),
    //                               else its deleting destructor the routine
    // GetBucket(0xfdff) (vt +0xc); non-null ->
    //                               its vtable +0xc (remove) with Obj
    // Deferred (+0xe8) append(Obj) (alloc_node
    // , linked after the tail)
    //   return true
    bool remove_solar(GameObject* Obj);

    // ------------------------------------------------------------------
    // The ServerEquip launch/notify entry points (declared 2026-09-24 by
    // claude-code:w5-server-06ce for its callers; bodies are the 06d0* lane's).
    // create_mine -- the routine (RET 4). Caller SrvMineDropper::launch
    // (module storage), one call per ProjLaunchInfo.
    void create_mine(FServerLaunchRecord const& Rec);
    // create_counter_measure -- the routine (RET 4). Caller
    // SrvCounterMeasureDropper::launch (module storage).
    void create_counter_measure(FServerLaunchRecord const& Rec);
    // create_guided -- the routine (RET 4). Caller SrvGun::launch_guided
    // (module storage).
    void create_guided(FServerLaunchRecord const& Rec);
    // create_bullet -- the routine (RET 4). Caller SrvGun::launch_bullets
    // (module storage).
    void create_bullet(FServerBulletRecord const& Rec);
    // notify_fire_weapon -- the routine (RET 0x10). Caller SrvGun::fire_forward
    // (module storage): the shooter's id, the one-element id vector, the aim point
    // (rotated into the lock target's frame when there is one) and the client.
    void notify_fire_weapon(unsigned int const& ObjId, std::vector<unsigned short> const& EquipIds,
                            Vector const& Target, int SourceClient);

    // ------------------------------------------------------------------
    // add_cargo / remove_cargo -- baseline the routine (module storage,
    // RET 0x14) and the routine (module storage, RET 0x0C). The in-space half
    // of pub::Player::AddCargo / RemoveCargo: the docked half writes the
    // PlayerData EquipDescList directly, these two write the LIVE ship object's
    // CEquipManager and then mirror the result back into the slot.

    // Their names are baseline's own: the failure path FDUMPs
    //   "StarSys::add_cargo().  Failed to get client cargo description."
    // from Server\space\StarSys.cpp line 0xd8e
    // (module storage .. module storage).

    // ARITY CORRECTION for remove_cargo: analysis declares it
    // `the routine(int* param_1, uint param_2)` with the `this` lost, but the
    // epilogue is `RET 0xc` (module storage) -- three stack arguments -- and the
    // third is read at module storage as `MOV EBX,[ESP + 0x40]`, which resolves
    // to entry+0x0C once the 0x24-byte frame and the four register pushes are
    // accounted for. It is the COUNT; the analysis passes `param_2` twice
    // (once as the equipment id, once as the amount) because of that.

    // baseline reaches `this` through the routine's third out-parameter (the
    // object's owning container, module storage `LEA EDX,[ESP+0x4]`); this port
    // has no owner chain in its FLID registry, so pub::Player:: calls these
    // through GameObject::OwnerSystem, which is the same StarSystem -- see the
    // member's own comment in server_gameobject.h.
    // ------------------------------------------------------------------
    unsigned short add_cargo(GameObject* Obj, unsigned int ArchId, unsigned int Count,
                             float Health, bool bMission);
    void remove_cargo(GameObject* Obj, unsigned short EquipId, int Count);

    // ------------------------------------------------------------------
    // unload — baseline the routine(this). The single-player "last player left,
    // tear the system down" pass that the routine/the routine run after
    // firing event 0x3d. For each of the ten sub-object dispatch blocks it
    // walks the block's node list twice: once issuing a VIRTUAL DELETE on every
    // node whose Data is non-null, then again freeing the nodes through
    // and nulling the block's head/tail pair.

    // The first pass is not a "notification": module storage reads the vtable at
    // Data+0x0c, sets `this` to Data+0x0c and calls slot 0 with the deleting
    // flag 1. Data+0x0c is the CBase sub-object of the IObjRW wrapper (see
    // server_gameobject.h's layout note), so that line destroys the whole
    // object through a secondary base -- `delete static_cast<CBase*>(Obj)`.

    // The whole body, in order (re-read 2026-09-24 against the listing):
    // erase the whole player list (the routine:
    //                               each view's own destructor, the node freed,
    //                               --size);
    //   +0x34, +0x48, +0x70, +0x84, +0x98, +0xac -- each: delete every non-null
    //                               Data, then every node through the routine
    //                               (MetaList::free_node) and Head = Tail = 0;
    // +0xe8: traverse + delete Data, then
    //                               MetaList::free (the routine);
    // the routine: the same two passes over +0x5c, then
    //                   FreeSpatial(+0x20) and +0x20 = 0 when it was set;
    // the routine: the routine(sysId) (SrvAsteroid_
    //                   UnloadSystem), CmnAsteroid::Unload(sysId), then the two
    //                   passes over +0xc0;
    // the routine: the +0x24 vector emptied (_Last = _First);
    // PhySys::Shutdown(sysId).
    // +0xd4 is never touched, and +0xfc is not written (shutdown clears it).
    void unload();

    // ------------------------------------------------------------------
    // queue_dock -- baseline the routine (module storage, thiscall, RET 0x10).
    // push_back one FSystemDockEntry onto the dock queue at +0x14:
    // a stack element with both watchers zeroed;
    // BaseWatcher::set(nullptr) on the second
    //                               one (a no-op on a zeroed watcher);
    // watcher 1 := Object ? Object + 8 : null,
    //                               set_pointer only when it differs;
    // watcher 2 := Target ? Target + 8 : null,
    //                               the same way;
    // +0x10 := *BaseId, +0x14 := *bLand;
    // operator new(0x20), linked in before the
    //                               sentinel ([this+0x18]) -- a push_back;
    // the routine: copy the element into node+8;
    // ++[this+0x1c];
    // the stack element's watchers are released.
    // Callers: IServerImpl::Dock (module storage, Target null, bLand 1),
    // pub::SpaceObj::InstantDock (module storage) and the routine
    // (module storage, the dock-bay tick of an EqObj).
    // ------------------------------------------------------------------
    void queue_dock(GameObject* Object, GameObject* Target,
                    unsigned int const* BaseId, bool const* bLand);

    // ------------------------------------------------------------------
    // erase_docks -- baseline the routine (module storage, thiscall on
    // `this + 0x14`, RET 0xc): std::list<FSystemDockEntry>::erase(First, Last).
    // For every node from First up to (not including) Last: unlink it
    // (module storage .. 06d0f435), destroy the element (the routine at
    // ), free the node (module storage) and --[this+0x1c]
    // (module storage .. 06d0f44c; unguarded). Returns Last (module storage).
    // ------------------------------------------------------------------
    SystemDockNode* erase_docks(SystemDockNode* First, SystemDockNode* Last);

    // ------------------------------------------------------------------
    // shutdown -- baseline the routine (module storage, thiscall, no stack
    // arguments). When the system is loaded: clear +0xfc (module storage),
    // fire the routine(0x3d, &systemId) with the id read off SystemInfo's
    // vtable slot 0 (module storage .. 06d07157) and tear the system down with
    // (StarSystem::unload, module storage). Nothing when it is
    // not loaded. Callers: the landing block of the routine (module storage,
    // single player only) and module storage.
    // ------------------------------------------------------------------
    void shutdown();
};

static const unsigned int STARSYSTEM_SIZE = 0x100;

static_assert(sizeof(StarSystem) == 0x100, "StarSystem is baseline's 0x100 bytes");
static_assert(offsetof(StarSystem, SystemInfo) == 0x04, "StarSystem+0x04 SystemInfo");
static_assert(offsetof(StarSystem, PlayerListHead) == 0x0C, "StarSystem+0x0C player list head");

// (module storage, thiscall, `RET 0x4`) -- SystemPlayerView_SendGroupChange
// (the routine) on every view in the system's player list. Callers
// CPlayerGroup::AddMember / DelMember after they stamp a ship's group id.
void StarSystem_SendGroupChange(StarSystem& System, unsigned int ObjId);

// (module storage, thiscall, `RET 0x4`) -- SystemPlayerView_SendRemoteCargoUpdate
// (the routine) on every view in the system's player list whose PlayerId is not
// Obj's own CSimple::OwnerPlayer. Only caller: IServerImpl::AcceptTrade
// (module storage / module storage), once per trading ship.
void StarSystem_SendRemoteCargoUpdate(StarSystem& System, GameObject* Obj);
static_assert(offsetof(StarSystem, ObjectListHead) == 0x18, "StarSystem+0x18 dock queue head");
static_assert(offsetof(StarSystem, Spatial) == 0x20, "StarSystem+0x20 spatial partition");
static_assert(offsetof(StarSystem, VectorBegin) == 0x28, "StarSystem+0x28 vector _First");
static_assert(offsetof(StarSystem, Ships) == 0x34, "StarSystem+0x34 first ObjectList");
static_assert(offsetof(StarSystem, Solars) == 0x5C, "StarSystem+0x5C solar ObjectList");
static_assert(offsetof(StarSystem, Deferred) == 0xE8, "StarSystem+0xE8 last ObjectList");
static_assert(offsetof(StarSystem, bLoaded) == 0xFC, "StarSystem+0xFC loaded flag");

// Backward compatibility alias for existing code
typedef StarSystem FStarSystem;

// SysDB::SysMap -- baseline .data RVA 0xADA2C, export ordinal 460:
//   ?SysMap@SysDB@@3V?$map@IVStarSystem@@U?$less@I@std@@V?$allocator@VStarSystem@@@3@@std@@A
// VC6 spelled the default allocator with the MAPPED type (allocator<StarSystem>).
// Ours is the same map as an ordinary modern std::map with its default allocator,
// so its decorated name differs from baseline's on purpose (project rule, 2026-09-24).
namespace SysDB {
    typedef std::map<unsigned int, StarSystem> SysMapType;
    extern SERVER_API SysMapType SysMap;
}

// (module storage, `RET 0x8`, thiscall on the StarSystem) -- find
// PlayerId's node in the system's player list (node+0x40 == View.PlayerId) and
// run SystemPlayerView_CollectCreatedOwners on its view. No match: nothing.
void StarSystem_CollectCreatedOwners(StarSystem* System, unsigned int PlayerId,
                                     std::set<unsigned int>* Owners);

// SysDB lookup — equivalent of baseline the routine
// Returns FStarSystem* for given system_id, or nullptr if not found
FStarSystem* SysDB_LookupSystem(unsigned int system_id);

// SysDB init — equivalent of baseline the routine
// Creates FStarSystem entries for all Universe systems
void SysDB_Initialize();

// SysDB cleanup — equivalent of baseline the routine
void SysDB_Shutdown();

// Mark system as loaded/active
void SysDB_SetSystemLoaded(unsigned int system_id, bool loaded);

// (.data, initial value 1). Written once, by the routine
// (module storage), from the byte at IServer::Startup's info +0x210
// (module storage `MOV AL,byte ptr [ESI + 0x210]`). Read only by three
// StarSystem members, each as `if (!flag && obj && obj->vt[+0xbc]()) ...`:
// (module storage, explosion), the routine (module storage,
// munition collision) and the routine (module storage, object collision).
// Server.dll does not name it; OutpostServer.exe does: its Server Configuration dialog
// (module storage -> FServerControl module storage) fills info +0x210 from the
// "Allow Players to Harm Other Players" checkbox, and the three readers drop
// player-on-player damage while it is 0. Defined in src/Server/IServerImpl.cpp.
extern unsigned char GbAllowPvp;

// The one-entry cache of the routine (the object lookup by id; see
// server_gameobject.h, Server_LookupGameObject). baseline's storage, defined in
// src/Server/IServerImpl.cpp. Readers and writers: the routine (Server_FindObject),
// (Server_LookupCacheForget), the routine / the routine
// (Server_TakeInTransit / Server_DestroyInTransit), all in server_classes.cpp,
// and the routine (Server_CleanupPhysics, IServerImpl.cpp).
// watcher on the last object found (its Watchable, IObjRW+8);
// module storage reads it, -8 back to the IObjRW.
//                 CRT init the branch (the routine(0): both dwords zero),
//                 atexit the routine -> the routine (set_pointer(0) when
//                 something is watched).
// the StarSystem that object was found in (module storage);
//                 zeroed by the routine (module storage) at server shutdown.
// / the field  hit / miss counters (module storage /
// ), never read by anything else.
struct BaseWatcher;
extern BaseWatcher GLookupCacheObject;
extern StarSystem* GLookupCacheSystem;
extern unsigned int GLookupCacheHits;
extern unsigned int GLookupCacheMisses;

// (module storage, cdecl, one argument; the name is the port's).
// Forgets the cached object when it is the one with this id: the watcher's
// Watchable - 8 is the wrapper, its CBase::Object under the (& 3) == 3 guard
// gives the CSimple id (+0xb0, read with no null test on the guard's result);
// equal -> BaseWatcher::set_pointer(0) on GLookupCacheObject and
// GLookupCacheSystem = 0. Callers: StarSystem::remove_solar (module storage)
// and the routine (StarSystem object removal).
void Server_LookupCacheForget(unsigned int const& ObjId);

// ---------------------------------------------------------------------------
// (module storage, `RET 0x8`) -- "this object has just taken
// damage". Ported 2026-09-17 (wave 14) in src/Server/server_observer.cpp.

// baseline's three callers (module storage, module storage, module storage) all
// reach it as a thiscall on a StarSystem with (IObjRW*, DamageList*). It does
// two unrelated things:
//   * when the damaged object IS the current ship of a valid player, it walks
//     the DamageList and mirrors every entry into that player's own PlayerData
//     -- equipment through CEquipManager::FindByID + PlayerData_ModifyEquipment
//     / PlayerData_RemoveEquipment, collision groups through
//     CArchGroupManager::FindByID + PlayerData_SetCollisionGroupHealth, and the
//     root subobject through PlayerData_SetRelativeHealth;
//   * then it tells every player in the system, via
//     SystemPlayerView_DamageObject. For a SOLAR it first builds a reduced
//     DamageList holding only the entries whose Fate != None, and that reduced
//     list is the one a player who cannot currently see the object receives.
void StarSystem_DamageObject(StarSystem* Sys, GameObject* Obj, DamageList* Damage);
