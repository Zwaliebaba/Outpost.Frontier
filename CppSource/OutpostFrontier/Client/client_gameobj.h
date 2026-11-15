// Author: Giperion.

#pragma once

#include "client_mesh_load.h" // FMeshHolder (Ship +0x108, the routine)
#include "common_archetype.h"
#include "common_csys.h" // Transform (Ship +0x118)
#include "common_cequip.h" // ActionDB, INotify
#include "common_cobject.h"
#include "common_iface.h"
#include "common_misc.h" // DamageList
#include "common_tstring.h" // TString (Solar +0xe8)
#include "font_string.h" // FFontString (GameObject +0x48)

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>

struct FCliEquipFirePacket; // cli_projectile_fx.h
struct FEffectArch;         // vis_effect_arch.h
struct FParticleEffect;
struct FStarSphere;         // star_sphere.h (Solar +0xe4)
struct ISoundSource;
namespace Archetype { struct ISound; }

// ===========================================================================
// The client's GameObject family -- baseline Client\space\Gameobj.cpp.

// RTTI and constructors (sizes from the operator_new calls, vtables from the ctor stores):
//   GameObject  : IObjInspectImpl  ctor the routine(kind)  0xac  IObjRW 005de7ec
//                                  IObjAffect 005de76c, CBase 005de764 -> the routine
//   Simple      : GameObject       ctor the routine(kind)  0xac  IObjRW 005e00a4
//                                  IObjAffect 005e0024, CBase 005e0018 -> the routine
//   EquippedObj : Simple, INotify  ctor the routine(kind)  0xe4  IObjRW 005de23c
//                                  IObjAffect 005de1bc, CBase 005de1b0 -> the routine,
//                                  INotify at +0xac 005de1a8 -> the routine
//   Ship        : EquippedObj      ctor the routine        0x190 IObjRW 005dfd6c
//                                  IObjAffect 005dfcec, CBase 005dfce0 -> the routine,
//                                  INotify 005dfcd8 -> the routine
//   Solar       : EquippedObj      ctor the routine        0x208 IObjRW 005e0304
//                                  IObjAffect 005e0284, CBase 005e0278 -> the routine,
//                                  INotify 005e0270 -> the routine
// Projectile / Munition / Beam / Mine / CounterMeasure / Guided and Loot are declared after
// Ship (below); Debris lives in client_debris.h. The asteroid branches are not modelled yet.

// THE EXTENSION VTABLE IS DECLARED IN BASELINE SLOT ORDER. IObjInspect's 85 slots come first
// (common_iface.h), so the first virtual below is slot 85 (+0x154) and every following one is
// the next baseline slot; the byte offset and the base body sit next to each. Neither
// IObjInspectImpl's nor these classes' destructors add a primary slot -- they override CBase's
// (the +0x0c sub-object), where baseline keeps them. Do not insert a virtual between two of these
// without a baseline slot for it. Slot names are not recovered (the exe exports none of them):
// `create`, `find` and `update` borrow Server.dll GameObject's names for the slots of the same
// shape and meaning (server_gameobject.h); every other slot is `SlotNNN` after its byte offset.
// ===========================================================================

enum class EClientGameobjKind : int
{
    None = 0,
    Ship = 1,
    Solar = 2,
    Beam = 3, // pushes 3 into the routine (0x0052de9f)
    Guided = 4, // pushes 4 into the routine (0x0053d0cd)
    Mine = 5, // pushes 5 into the routine (0x0053ca8d)
    CounterMeasure = 6, // pushes 6 into the routine (0x0053cdbd)
    Loot = 7, // pushes 7 into the routine (0x0053e471)
    MedAsteroid = 8, // pushes 8 into the routine (cli_asteroid_med.h)
    DynamicAsteroid = 9, // pushes 9 into the routine (cli_asteroid_dynamic.h)
    Debris = 10, // / the routine push 0xa into the routine (0x00530656)
};

// GameObject+0x28 -- the gameobj's effect list: a VC6 std::list whose node is 0x18 bytes
// (next, prev, then the 0x10-byte effect companion {slot0, ISoundSource*, particle, light}).
// inserts, the routine removes one, the routine clears, the routine sweeps
// (cli_projectile_fx.h / space_scene_obj_draw.cpp).

// Two layers, as baseline's EH tables show: FGameobjEffectListStorage is the bare list (its
// constructor is inlined at 0x00539335..0x00539353, its destructor is the routine and the
// unwind action of the routine while the routine runs); FGameobjEffectList is the member
// whose destructor is the routine -- the routine(0) and then the storage's destructor.
// unwinds a half-built GameObject through the routine (Unwind@005c0aa8), so the
// list is a real sub-object with its own destructor, not a teardown written into the owner.
struct FGameobjEffectListStorage
{
    explicit FGameobjEffectListStorage(unsigned char InAllocatorByte);
    ~FGameobjEffectListStorage();
    FGameobjEffectListStorage(FGameobjEffectListStorage const&) = delete;
    FGameobjEffectListStorage& operator=(FGameobjEffectListStorage const&) = delete;

    // (thiscall, RET 8: out iterator, Where): unlink Where, release its
    // companion (the routine's sequence), free the node, --Count; returns the next node.
    void* Erase(void* Where);

    unsigned char AllocatorByte; // +0x00  the ctor copies the kind's low byte here
    unsigned char Pad01[3];
    void* Head;                  // +0x04  sentinel node, operator_new(0x18), next/prev = self
    int Count;                   // +0x08
};

struct FGameobjEffectList : public FGameobjEffectListStorage
{
    explicit FGameobjEffectList(unsigned char InAllocatorByte)
        : FGameobjEffectListStorage(InAllocatorByte)
    {
    }
    // .
    ~FGameobjEffectList();
};
static_assert(sizeof(FGameobjEffectList) == 0x0c, "GameObject effect list 0xc");

// EquippedObj+0xb4 -- the list of equipment-effect wrappers (VC6 std::list<ClientEquip*>, node
// 0xc {next, prev, wrapper}). Its constructor is inlined in the routine at 0x00532bf0..
// (EH state 0 -> 1), its destructor is the shared list destructor the routine --
// the unwind action of the EquippedObj constructor (Unwind@005c07cb) and destructor
// (Unwind@005c0808) -- and the routine inlines it (the routine over [begin, end), head
// freed) at 0x00532f5a..0x00532f8e. The nodes' wrappers are not deleted by it (the owner's
// destructor deletes them first, the routine).
struct FEquipFxList
{
    explicit FEquipFxList(unsigned char InAllocatorByte);
    ~FEquipFxList();
    FEquipFxList(FEquipFxList const&) = delete;
    FEquipFxList& operator=(FEquipFxList const&) = delete;

    unsigned char AllocatorByte; // +0x00  the kind's low byte
    unsigned char Pad01[3];
    void* Head;                  // +0x04  sentinel, operator_new(0xc), next = prev = itself
    int Count;                   // +0x08
};
static_assert(sizeof(FEquipFxList) == 0x0c, "EquippedObj equip-FX list 0xc");

// EquippedObj+0xc0 -- the list of burning fuses (VC6 std::list<ClientFuse>, node 0x40 =
// FFuseBurnNode {next, prev, ClientFuse}). Its constructor is inlined in the routine at
// (EH state 1 -> 2), its destructor is the routine -- the unwind action
// of both the EquippedObj constructor (Unwind@005c07d9) and destructor (Unwind@005c0816) --
// and the routine inlines it (the routine over [begin, end), head freed) at 0x00532f27..57.
struct FEquippedFuseList
{
    explicit FEquippedFuseList(unsigned char InAllocatorByte);
    ~FEquippedFuseList();
    FEquippedFuseList(FEquippedFuseList const&) = delete;
    FEquippedFuseList& operator=(FEquippedFuseList const&) = delete;

    unsigned char AllocatorByte; // +0x00  the kind's low byte
    unsigned char Pad01[3];
    void* Head;                  // +0x04  sentinel, operator_new(0x40), only the links written
    int Count;                   // +0x08
};
static_assert(sizeof(FEquippedFuseList) == 0x0c, "EquippedObj fuse list 0xc");

// GameObject+0x84 element: the destructor stops (vt+0x60, ISoundSource::Stop) and releases
// (vt+0x8) the interface at +4 of every 8-byte element.
struct FGameobjSound
{
    unsigned Field_0x00;  // +0x00  not read by any function ported so far
    ISoundSource* Source; // +0x04
};
static_assert(sizeof(FGameobjSound) == 8, "GameObject sound element 8 bytes");

// EquippedObj+0xcc map value (baseline node +0x10 / +0x14): the target object id and its
// sub-object. A new entry is {0, SUBOBJ_ID_NONE} (0x005356d5..0x005356f3,
// ).
struct FEquipSubTarget
{
    FEquipSubTarget() : TargetId(0), TargetSubObj(SUBOBJ_ID_NONE) {}

    unsigned TargetId;            // +0x00 (node +0x10)
    unsigned short TargetSubObj;  // +0x04 (node +0x14)
};

class FClientGameobj : public IObjInspectImpl
{
public:
    // (RET 4). Kind lands in ManagerKind and its low byte in the two allocator
    // bytes (+0x28, +0x80).
    explicit FClientGameobj(EClientGameobjKind Kind);
    // (this = the CBase sub-object); deleting wrapper the routine.
    ~FClientGameobj() override;

    // IObjInspect slot 8 (+0x20) the branch -- every client gameobj vtable holds it (15
    // XREFs: 005de80c GameObject, 005dca34 MedAsteroid, 005dccfc DynamicAsteroid, ...): the
    // object narrowed to a CSimple ((class & 3) == 3) gives its Id (+0xb0); no object or another
    // class gives 0. Common's IObjInspectImpl::get_id has no such test.
    unsigned int const get_id() const override;

    // ---- the extension vtable, baseline slot order --------------------------------------
    // 85 +0x154 the branch: bit 2 of Flags (set by destroy(1)).
    virtual bool Slot154() const;
    // 86 +0x158 the branch: bit 0 of Flags. the routine (the ObjDB reap) removes and deletes
    // every gameobj that answers true.
    virtual bool Slot158() const;
    // 87 +0x15c the routine (RET 4): bit 0 of Flags = the argument; then, when there is an
    // object, CObject::enable_collisions_r(false) (a tail JMP, whatever the argument was).
    virtual void Slot15c(bool bValue);
    // 88 +0x160 the branch: bit 1 of Flags.
    virtual bool Slot160() const;
    // 89 +0x164 the branch (RET 4): bit 1 of Flags = the argument.
    virtual void Slot164(bool bValue);
    // 90 +0x168 the branch (RET 4): CObject::Alloc(1) into Object, Arch->create_instance,
    // NeuronRuntime::Engine vt+0x6c UpdateInstance(Object+4, 0, 0), CBase::advise(true). Simple,
    // EquippedObj and the projectile branch make it pure again (0x005b8064). No caller reads
    // a result (0x004f4709, 0x005266ee, 0x0053ad5f, 0x00548c03, 0x0054b591 all go on without
    // looking at EAX).
    virtual void create(Archetype::Root* Arch);
    // 91 +0x16c pure in GameObject (RET 4): attach to the CObject with this id (Ship
    // , Solar the routine: CObject::Find(Id, Class), advise(true), connect).
    virtual bool find(unsigned const& Id) = 0;
    // 92 +0x170 the routine: resolve the archetype's explosion into ExplosionArch (+ preload),
    // FDUMP a warning when the archetype names one that does not exist, then join the watch list
    // when get_type & 0x107ffdff and bWatchListed.
    virtual void Slot170();
    // 93 +0x174 the routine (RET 4): Actions.update(Dt), then Slot1b8(Dt).
    virtual void update(float Dt);
    // 94 +0x178 the routine (RET 4): the object's vt+0x64 (the physical radius about Center).
    virtual float Slot178(Vector& Center);
    // 95 +0x17c the routine (RET 0xc): the LOD pick. Returns its first argument: a non-zero
    // low word in *CachedLod is copied straight to *OutLod (0x0053982c..0x0053983a), otherwise
    // the pick runs (Slot178 radius, camera vt+0x40, ...). The pick's port is the free function
    // GameobjPickLod in space_scene_obj_draw.cpp -- see client_gameobj.cpp.
    virtual unsigned* Slot17c(unsigned* OutLod, void* Camera, unsigned const* CachedLod);
    // 96 +0x180 the branch: empty (EquippedObj overrides).
    virtual void Slot180();
    // 97 +0x184 the routine (RET 4): per-object lighting (the routine). Body: the free
    // function GameobjUpdateDrawLightingbaseline in space_scene_obj_draw.cpp.
    virtual void Slot184(void* Camera);
    // 98 +0x188 the routine (RET 0x14): draw submit -- Slot180, Slot17c(&Lod, Camera,
    // &CachedLod), the LOD gate (a failed gate writes the LOD to *OutLod and returns),
    // the effect-list sweep, Slot184(Camera), the player flags, get_index, the envmap arm,
    // IRenderer2::Slot7(Camera, Inst, 1.0f, Flags, Matrix, Extra). Matrix = baseline arg 4
    // (flags |= 0x120 when non-null), Extra = baseline arg 5 (the IFLAppearance Slot8 out-param
    // when it was null). Body: GameobjSubmitDrawbaseline in space_scene_obj_draw.cpp; the draw
    // drain calls this slot through the node the branch.
    virtual unsigned* Slot188(unsigned* OutLod, void* Camera, unsigned CachedLod, void* Matrix,
                              void* Extra);
    // 99 +0x18c the branch (RET 4): *OutB = DrawStateB; return DrawStateA.
    virtual unsigned Slot18c(unsigned* OutB);
    // 100 +0x190 the branch: DrawStateA = DrawStateB = 0, Field_0x34 = 10.0f.
    virtual void Slot190();
    // 101 +0x194 the routine (RET 0x10): the collision pair --
    // Other->Slot1a0(A, D, this, B), then this->Slot1ac(A, B, Other, D).
    virtual void Slot194(void const* A, void const* B, FClientGameobj* Other, void const* D);
    // 102 +0x198 the branch (RET 4): empty. Simple the routine applies the root entry.
    virtual void Slot198(DamageList const& Damage);
    // 103 +0x19c the routine (RET 4): DestroyType 0 -> Slot15c(true); 1 -> Flags |= 4 and
    // Slot1b0(nullptr); anything else FDUMPs "Invalid DestroyType!".
    virtual void Slot19c(int DestroyType);
    // 104 +0x1a0 the routine (RET 0x10): surface-hit effect (PickDistanceRecord 0x00407450,
    // ). Contact points at two Vectors: the hit point (+0x00) and the surface
    // normal (+0x0c); B and D are not read.
    virtual void Slot1a0(void const* Contact, void const* B, FClientGameobj* Other,
                         void const* D);
    // 105 +0x1a4 the routine (RET 0xc): munition-hit effect (0x00408a70, the routine). The
    // munition gameobj is the THIRD argument: the routine reads its +0x10 object from
    // [ESP+0xc] (0x00539d84), and Munition's slot 101 pushes `this` last (0x0053ed94).
    virtual void Slot1a4(void const* A, void const* B, FClientGameobj* Munition);
    // 106 +0x1a8 the branch (RET 0xc): empty. The arguments are EquippedObj's
    // (the routine): a gun hardpoint's sub-object id, the target object id and its sub-object.
    virtual void Slot1a8(unsigned short SubObj, unsigned TargetId, unsigned short TargetSubObj);
    // 107 +0x1ac the routine (RET 0x10): multiplayer collision report through the server
    // interface the field vt+0xdc.
    virtual void Slot1ac(void const* A, void const* B, FClientGameobj* Other, void const* D);
    // 108 +0x1b0 the routine (RET 4): explode at Pos (null = get_position()) through
    // + Actions.add, or Slot15c(true) when there is no explosion.
    virtual void Slot1b0(Vector const* Pos);
    // 109 +0x1b4 the routine (RET 0xc): the hit sound. SourceId keys the per-source voice
    // pool at +0x80; SoundArch is an EffectArch's +0x18 (vt+0x30 tested first); Pos goes to the
    // voice's vt+0x8c. Caller: the routine (0x0053a21f).
    virtual void Slot1b4(unsigned SourceId, void* SoundArch, Vector const* Pos);
    // 110 +0x1b8 the branch (RET 4): empty.
    virtual void Slot1b8(float Dt);

    // ---- non-virtual members --------------------------------------------------------
    // the cached attitude towards the player, refreshed through the routine
    // every eighth call (AttitudeCountdown).
    int GetCachedAttitude();
    // / the routine (callers 0x0051cf24 / 0x0051cfb5): read / write +0xa4.
    unsigned char GetFieldA4() const { return Field_0xa4; }
    void SetFieldA4(unsigned char Value) { Field_0xa4 = Value; }
    // (callers 0x004d2763 .. 0x004d376a): the address of the font string at +0x48.
    FFontString* GetFontString48() { return &Field_0x48; }
    // (callers 0x004d2756 / 0x004d2d9a): Flags bit 3, but only while +0x7c still
    // equals the surface width the field; false otherwise.
    bool GetFlag3IfSurfaceWidthUnchanged() const;
    // (RET 4): Flags bit 3 = the argument; when it ends up set, +0x7c takes the
    // current surface width the field.
    void SetFlag3(bool bValue);

    // (RET 0x14): the surface-hit particle -- a new ParticleEffect for
    // EffectArch->VisEffect in one of the three HitEffects slots, attached at Pos / Normal in
    // object space -- then Slot1b4(SourceId, EffectArch->SndEffect, Pos). Skipped entirely for
    // the player's ship while SURFACE_HIT_EFFECTS (option 0x38) is off. The fifth argument
    // is never read ([ESP+0xb0] at that frame is not touched): slots 104/105 pass 0, the shield
    // wrapper's hit slots (0x0052e84d, 0x0052e9bd) pass the address of a zeroed local effect
    // companion, which therefore stays empty.
    // Callers: slot 104 (0x00539d67), slot 105 (0x00539e08), 0x0052e84d, 0x0052e9bd.
    void SpawnSurfaceHitEffect(unsigned SourceId, FEffectArch* EffectArch, Vector const* Pos,
                               Vector const* Normal, void* CompanionOut);
    // (RET 0x14): the splash-hit one-shot -- EffectArch through the routine at
    // Pos with the direction mirrored off Normal. Skipped for the player's ship while
    // SPLASH_HIT_EFFECTS (option 0x39) is off, and when the effect type fails the perf test
    // . Callers: slot 105 (0x00539e61), 0x0052ea3b.
    void SpawnSplashHitEffect(FEffectArch* EffectArch, Vector const* Pos, Vector const* Normal,
                              Vector const* Velocity, void* CompanionOut);

    CObject* GetObject() const { return Object; }

    // (thiscall, RET 4) -- the object narrowed to a CSimple ((class & 3) == 3, else
    // null, used with no test), then a tail JMP to CSimple::SetOwnerPlayer(PlayerId). Callers:
    // the IClient system-switch-out and land handlers (0x0053d60e, 0x0053d794), 0x00548205.
    void SetOwnerPlayer(unsigned PlayerId);

    // IObjAffect -- GameObject's own table, module storage (the routine stores it at +4,
    // ). Every entry is a baseline error stub with the same RET n / value pattern
    // as Server.dll GameObject's; see client_gameobj.cpp. Simple and Solar keep these entries,
    // EquippedObj overrides six and Ship nineteen (see those classes).
    int set_throttle(float Throttle) override;
    int set_axis_throttle(Vector const& AxisThrottle) override;
    int set_nudge_vec(Vector const& NudgeVec) override;
    int set_strafe_dir(StrafeDir Dir) override;
    int set_formation_leader(IObjRW* Leader) override;
    int set_formation_offset(Vector const& Offset) override;
    int add_formation_follower(IObjRW* Follower) override;
    int remove_formation_follower(IObjRW* Follower) override;
    int get_formation_follower_count() override;
    int set_follow_leader(IObjRW* Leader) override;
    int set_follow_offset(Vector const& Offset) override;
    int add_follow_follower(IObjRW* Follower) override;
    int remove_follow_follower(IObjRW* Follower) override;
    int slot_34(unsigned short const* EquipIds, unsigned Count, Vector const* TargetPos,
                int SourceClient) override;
    int slot_38(unsigned short EquipId, int Count, int SourceClient) override;
    int slot_3c(unsigned short const* EquipId, std::vector<IObjRW*> const& Targets,
                int SourceClient) override;
    int slot_40(unsigned short const* EquipId, IObjRW* Target, int SourceClient) override;
    int slot_44(unsigned short const* EquipId, IObjRW* Target, unsigned short SubTarget,
                int SourceClient) override;
    int set_target(IObjRW* Target, unsigned short SubTarget, int SourceClient) override;
    int slot_4c(unsigned short const* EquipId, bool bActivate, int SourceClient) override;
    int slot_50(bool bEngage, bool bArg2, int SourceClient) override;
    int slot_54(bool bActivate, int SourceClient) override;
    int slot_58(unsigned short EquipId, unsigned Count, int SourceClient) override;
    int slot_5c(int RequestType, unsigned const* TargetId, unsigned const* SecondId,
                int bFlag, RequestCB* Callback) override;
    int slot_60(int RequestType, unsigned const* TargetId, int Arg3) override;
    int go_tradelane(IObjInspect const* EntranceRing, IObjInspect const* ExitRing,
                     bool bFastExit, float Speed) override;
    int stop_tradelane() override;
    int slot_6c(int ArgA, int ArgB) override;
    int slot_70(unsigned Id) override;
    int slot_74(unsigned Arg) override;

    // ---- data (IObjInspectImpl ends at +0x14) ---------------------------------------
    FClientGameobj* ListNext;          // +0x14  ObjDB bucket link (the routine / 00542390)
    FClientGameobj* ListPrev;          // +0x18
    int ManagerKind;                   // +0x1c  ObjDB bucket (1 ship, 2 solar, 3 beam, ...)
    ActionDB Actions;                  // +0x20  update(Dt) by slot 93, add by slot 108
    FGameobjEffectList EffectList;     // +0x28
    float Field_0x34;                  // +0x34  ctor 0; slot 100 sets 10.0f; Ship ctor rand()/32768
    unsigned DrawStateA;               // +0x38  slot 99 / slot 100; lighting the routine arg
    unsigned DrawStateB;               // +0x3c
    Archetype::Explosion* ExplosionArch; // +0x40 slot 92 resolves; the dtor releases
    unsigned char Flags;               // +0x44  bit0 slot 86/87, bit1 slot 88/89, bit2 slot 85
    unsigned char Pad45[3];
    FFontString Field_0x48;            // +0x48  the routine; the routine returns its address
    int Field_0x7c;                    // +0x7c  surface width the field (the routine / 0053a9f0)
    unsigned char SoundAllocatorByte;  // +0x80  the ctor copies the kind's low byte here
    unsigned char Pad81[3];
    FGameobjSound* SoundsBegin;        // +0x84
    FGameobjSound* SoundsEnd;          // +0x88
    FGameobjSound* SoundsCap;          // +0x8c
    FParticleEffect* HitEffects[3];    // +0x90  the routine fills (the field = 3)
    int CachedAttitude;                // +0x9c  the routine
    int AttitudeCountdown;             // +0xa0  ctor -1
    unsigned char Field_0xa4;          // +0xa4  the routine get / the routine set
    unsigned char Pada5[3];
    // +0xa8 -- read by slot 92 (the routine) and the HUD; written by Simple's ctor
    // (the routine, 0) and the routine. GameObject's own ctor leaves it alone.
    unsigned char bWatchListed;        // +0xa8
    unsigned char Pada9[3];
};

static_assert(offsetof(FClientGameobj, ListNext) == 0x14, "Gameobj ListNext +0x14");
static_assert(offsetof(FClientGameobj, ListPrev) == 0x18, "Gameobj ListPrev +0x18");
static_assert(offsetof(FClientGameobj, ManagerKind) == 0x1c, "Gameobj ManagerKind +0x1c");
static_assert(offsetof(FClientGameobj, Actions) == 0x20, "Gameobj ActionDB +0x20");
static_assert(offsetof(FClientGameobj, EffectList) == 0x28, "Gameobj effect list +0x28");
static_assert(offsetof(FClientGameobj, Field_0x34) == 0x34, "Gameobj +0x34");
static_assert(offsetof(FClientGameobj, DrawStateA) == 0x38, "Gameobj +0x38");
static_assert(offsetof(FClientGameobj, DrawStateB) == 0x3c, "Gameobj +0x3c");
static_assert(offsetof(FClientGameobj, ExplosionArch) == 0x40, "Gameobj explosion +0x40");
static_assert(offsetof(FClientGameobj, Flags) == 0x44, "Gameobj flags +0x44");
static_assert(offsetof(FClientGameobj, Field_0x48) == 0x48, "Gameobj font string +0x48");
static_assert(offsetof(FClientGameobj, Field_0x7c) == 0x7c, "Gameobj +0x7c");
static_assert(offsetof(FClientGameobj, SoundsBegin) == 0x84, "Gameobj sounds +0x84");
static_assert(offsetof(FClientGameobj, HitEffects) == 0x90, "Gameobj hit effects +0x90");
static_assert(offsetof(FClientGameobj, CachedAttitude) == 0x9c, "Gameobj attitude +0x9c");
static_assert(offsetof(FClientGameobj, AttitudeCountdown) == 0xa0, "Gameobj countdown +0xa0");
static_assert(offsetof(FClientGameobj, Field_0xa4) == 0xa4, "Gameobj +0xa4");
static_assert(offsetof(FClientGameobj, bWatchListed) == 0xa8, "Gameobj +0xa8");
static_assert(sizeof(FClientGameobj) == 0xac, "GameObject sizeof 0xac");

// Simple : GameObject -- the routine. Adds no data; its ctor zeroes bWatchListed.
class FClientSimpleGameobj : public FClientGameobj
{
public:
    explicit FClientSimpleGameobj(EClientGameobjKind Kind);
    // is the deleting wrapper; the body is GameObject's (thunk to the routine).
    ~FClientSimpleGameobj() override;

    // 90 +0x168: pure again in Simple (0x005b8064).
    void create(Archetype::Root* Arch) override = 0;
    // 102 +0x198 the routine (RET 4): multiplayer only -- the root entry's hit points go to
    // CSimple::set_hit_pts (vt+0x98).
    void Slot198(DamageList const& Damage) override;
    // 103 +0x19c the routine (RET 4): CSimple::set_hit_pts(0), then GameObject's slot 103.
    void Slot19c(int DestroyType) override;

    // 111 +0x1bc the branch: the CSimple's TypeBits (+0xe0).
    virtual unsigned Slot1bc() const;

    // (RET 8): bWatchListed = (bWatch == 1), then join or leave the watch list.
    // The first argument is not read.
    void SetWatchListed(int Unused, int bWatch);
};
static_assert(sizeof(FClientSimpleGameobj) == 0xac, "Simple sizeof 0xac");

// EquippedObj : Simple, INotify -- the routine. INotify's vptr lands at +0xac (right after
// Simple), the way baseline's ctor stores 005de1a8 there.
class FClientEquippedObj : public FClientSimpleGameobj, public INotify
{
public:
    explicit FClientEquippedObj(EClientGameobjKind Kind);
    // ; deleting wrapper the routine.
    ~FClientEquippedObj() override;

    // INotify -- the routine.
    void notify(Event Code, void* Param) override;

    // IObjInspect slot 14 (+0x38) the branch: `MOV EAX,[ECX]; JMP [EAX+0x1c8]` -- the object
    // is dying while a death fuse on it still burns (Slot1c8, virtual). Slot 14 of the
    // EquippedObj (module storage), Ship (005dfda4) and Solar (005e033c) vtables.
    bool is_dying() const override;
    // IObjInspect slot 72 (+0x120) the branch (RET 8): Common's scan_cargo, then -- for the
    // player's ship -- IServer::SPScanCargo(this id, the scanned object's id, the player's
    // connection). Slot 72 of the same three vtables (005de35c / 005dfe8c / 005e0424).
    int scan_cargo(IObjRW* Scanned, EquipDescVector& OutCargo) const override;

    // 90 +0x168: pure (0x005b8064).
    void create(Archetype::Root* Arch) override = 0;
    // 92 +0x170 the routine.
    void Slot170() override;
    // 93 +0x174 the routine.
    void update(float Dt) override;
    // 95 +0x17c the routine.
    unsigned* Slot17c(unsigned* OutLod, void* Camera, unsigned const* CachedLod) override;
    // 96 +0x180 the branch.
    void Slot180() override;
    // 98 +0x188 the routine.
    unsigned* Slot188(unsigned* OutLod, void* Camera, unsigned CachedLod, void* Matrix,
                      void* Extra) override;
    // 102 +0x198 the routine.
    void Slot198(DamageList const& Damage) override;
    // 104 +0x1a0 the routine.
    void Slot1a0(void const* Contact, void const* B, FClientGameobj* Other,
                 void const* D) override;
    // 105 +0x1a4 the routine.
    void Slot1a4(void const* A, void const* B, FClientGameobj* Munition) override;
    // 106 +0x1a8 the routine (RET 0xc): SubTargets[SubObj] = {TargetId, TargetSubObj}.
    void Slot1a8(unsigned short SubObj, unsigned TargetId, unsigned short TargetSubObj) override;

    // (RET 4): re-aim the gun at SubObj on the target SubTargets holds for it
    // (inserting the {0, SUBOBJ_ID_NONE} default when there is none), through IObjAffect slot 17
    // when the gun's -- or, for SUBOBJ_ID_NONE, this object's -- target differs. Caller: the
    // multiplayer arm of slot_34 (0x00534bf6).
    void ApplySubTarget(unsigned short SubObj);
    // 108 +0x1b0 the routine.
    void Slot1b0(Vector const* Pos) override;

    // 112 +0x1c0 the routine (RET 0x14): ignite the fuse FuseId on this object -- the first
    // burning fuse of that name on the +0xc4 list gets Fuse::IgniteAt(SourceId, SubObjId,
    // AtT); otherwise a copy of the ClientFuseDB prototype is appended, bound to this object,
    // randomised, given Lifetime unless it is -1, and ignited. False only when the fuse is
    // not in the database (FDUMP). Callers: the equip-FX rebuild the routine (0x00533ee7,
    // ), BURNFUSE the routine, the ship override the routine. Body:
    // EquippedObjIgniteFuse (client_equip_fx.cpp).
    virtual bool Slot1c0(unsigned SourceId, ID_String const* FuseId, unsigned short SubObjId,
                         float AtT, float Lifetime);
    // 113 +0x1c4 the routine (RET 0xc): the first fuse named FuseId on the +0xc4 list gets
    // Fuse::UnBurn(SubObjId, AtT); true when one did.
    virtual bool Slot1c4(ID_String const* FuseId, unsigned short SubObjId, float AtT);
    // 114 +0x1c8 the routine (bare RET): true when a death fuse (Fuse +0xc) on the +0xc4 list
    // is not yet spent (Fuse::IsSpent).
    virtual bool Slot1c8();
    // 115 +0x1cc the routine (RET 4): the equipment message -- 0x18 fire, 0x19 activate.
    // Port: EquippedObjHandleEquipMsg (cli_projectile_fx.cpp).
    virtual unsigned char Slot1cc(FCliEquipFirePacket const* Packet);

    // (RET 0xc): a part that came off becomes a Debris gameobj (its [Debris]
    // record by DebrisType, the default with a warning when there is none), the part's effects
    // move to it (the routine), and a named Explosion (null = none) starts on it at this
    // Gameobj's position. Callers: notify 5 and 7 (0x00534783 .. 0x00534824).
    void SpawnPartDebris(CObject* PartObject, ID_String const* DebrisType,
                         ID_String const* Explosion);

    // IObjAffect overrides (module storage): slots 13, 15, 16, 17, 19, 22.
    int slot_34(unsigned short const* EquipIds, unsigned Count, Vector const* TargetPos,
                int SourceClient) override;
    int slot_3c(unsigned short const* EquipId, std::vector<IObjRW*> const& Targets,
                int SourceClient) override;
    int slot_40(unsigned short const* EquipId, IObjRW* Target, int SourceClient) override;
    int slot_44(unsigned short const* EquipId, IObjRW* Target, unsigned short SubTarget,
                int SourceClient) override;
    int slot_4c(unsigned short const* EquipId, bool bActivate, int SourceClient) override;
    int slot_58(unsigned short EquipId, unsigned Count, int SourceClient) override;

    // ---- data -------------------------------------------------------------------------
    // (+0xac is INotify's vptr.)
    unsigned char EquipFlags;          // +0xb0  the ctor clears bit 0 only
    unsigned char Padb1[3];
    // +0xb4 .. +0xbf: std::list of equipment-effect wrappers, node 0xc {next, prev, wrapper}
    // (client_equip_fx.cpp owns the wrappers) -- a RAII member (FEquipFxList).
    FEquipFxList EquipFx;              // +0xb4 (allocator byte), +0xb8 head, +0xbc count
    // +0xc0 .. +0xcb: std::list of burning fuses, node 0x40 (FFuseBurnNode, client_fuse.h) --
    // a RAII member whose destructor is the routine (FEquippedFuseList).
    FEquippedFuseList Fuses;           // +0xc0 (allocator byte), +0xc4 head, +0xc8 count
    // +0xcc .. +0xdf: baseline's VC6 std::map<unsigned short, FEquipSubTarget> (comparator and
    // allocator bytes, head, nil, multi flag, size; nodes 0x1c with the key word at +0x0c and
    // the value at +0x10) -- the target each gun hardpoint was last told to track, written by
    // slot 106 (the routine) and applied by the routine. The port keeps a std::map of its own
    // in the same 0x14 bytes (no baseline ABI).
    std::map<unsigned short, FEquipSubTarget> SubTargets; // +0xcc
    unsigned char PadSubTargets[0x14 - sizeof(std::map<unsigned short, FEquipSubTarget>)];
    unsigned char Field_0xe0;          // +0xe0
    unsigned char Pade1[3];
};
static_assert(offsetof(FClientEquippedObj, EquipFlags) == 0xb0, "EquippedObj +0xb0");
static_assert(offsetof(FClientEquippedObj, EquipFx) == 0xb4, "EquippedObj equip-FX list +0xb4");
static_assert(offsetof(FClientEquippedObj, Fuses) == 0xc0, "EquippedObj fuse list +0xc0");
static_assert(offsetof(FClientEquippedObj, SubTargets) == 0xcc, "EquippedObj sub-target map +0xcc");
static_assert(offsetof(FClientEquippedObj, Field_0xe0) == 0xe0, "EquippedObj +0xe0");
static_assert(sizeof(FClientEquippedObj) == 0xe4, "EquippedObj sizeof 0xe4");

class FClientSolarGameobj : public FClientEquippedObj
{
public:
    // .
    FClientSolarGameobj();
    // ; deleting wrapper the routine.
    ~FClientSolarGameobj() override;

    // INotify -- the routine.
    void notify(Event Code, void* Param) override;

    // 90 +0x168 the routine: the routine(0x303, Arch), open, CEqObj::connect(INotify*).
    void create(Archetype::Root* Arch) override;
    // 91 +0x16c the routine.
    bool find(unsigned const& Id) override;
    // 92 +0x170 the routine.
    void Slot170() override;
    // 93 +0x174 the routine.
    void update(float Dt) override;
    // 94 +0x178 the routine.
    float Slot178(Vector& Center) override;
    // 98 +0x188 the routine.
    unsigned* Slot188(unsigned* OutLod, void* Camera, unsigned CachedLod, void* Matrix,
                      void* Extra) override;
    // 104 +0x1a0 the routine.
    void Slot1a0(void const* Contact, void const* B, FClientGameobj* Other,
                 void const* D) override;
    // 105 +0x1a4 the routine.
    void Slot1a4(void const* A, void const* B, FClientGameobj* Munition) override;
    // 108 +0x1b0 the routine.
    void Slot1b0(Vector const* Pos) override;

    // (thiscall, RET 4) -- the client half of an [Object] section, read after the
    // spawn the routine rewound the reader (0x00548c73 set_state, call at 0x00548c7c): ring,
    // burn_color, dock_script, voice, jump_effect, star; then the nebula at the position.
    void ReadIni(INI_Reader& Reader);

    // ---- data -------------------------------------------------------------------------
    // +0xe4 -- the star (StarEffects::Star, FStarSphere, vtable 005db658) a sun solar gets
    // from its "star" key (the routine at 0x0054880d, ReadIni); the update ticks it through
    // its slot 2 (0x005478db). space_scene_obj_draw.cpp tests it.
    FStarSphere* Field_0xe4;           // +0xe4  ctor 0
    // +0xe8 -- "dock_script" (ReadIni 0x0054872c..0x00548783, the inlined TString assign). The
    // ctor zeroes the length and the first char only.
    TString<0x104> DockScript;         // +0xe8 .. +0x1ef
    unsigned char Field_0x1f0;         // +0x1f0 ctor 1
    unsigned char Pad1f1[3];
    float BurnColor[3];                // +0x1f4 ctor 1.0f x3; "burn_color" / 255 (ReadIni)
    ID_String Field_0x200;             // +0x200 ctor ID_String::clear; "jump_effect" CreateID
    void* Field_0x204;                 // +0x204 ctor 0; the dtor runs the routine and frees it
};
static_assert(offsetof(FClientSolarGameobj, DockScript) == 0xe8, "Solar +0xe8 dock_script");
static_assert(offsetof(FClientSolarGameobj, BurnColor) == 0x1f4, "Solar +0x1f4 burn_color");
static_assert(offsetof(FClientSolarGameobj, Field_0x1f0) == 0x1f0, "Solar +0x1f0");
static_assert(offsetof(FClientSolarGameobj, Field_0x200) == 0x200, "Solar +0x200");
static_assert(sizeof(FClientSolarGameobj) == 0x208, "Solar sizeof 0x208");

class FClientShipGameobj : public FClientEquippedObj
{
public:
    // .
    FClientShipGameobj();
    // ; deleting wrapper the routine.
    ~FClientShipGameobj() override;

    // INotify -- the routine.
    void notify(Event Code, void* Param) override;

    // 90 +0x168 the branch: the routine(0x503, Arch), open, CEqObj::connect(INotify*).
    void create(Archetype::Root* Arch) override;
    // 91 +0x16c the routine.
    bool find(unsigned const& Id) override;
    // 92 +0x170 the routine.
    void Slot170() override;
    // 93 +0x174 the routine.
    void update(float Dt) override;
    // 97 +0x184 the routine.
    void Slot184(void* Camera) override;
    // 98 +0x188 the routine.
    unsigned* Slot188(unsigned* OutLod, void* Camera, unsigned CachedLod, void* Matrix,
                      void* Extra) override;
    // 102 +0x198 the routine.
    void Slot198(DamageList const& Damage) override;
    // 104 +0x1a0 the routine.
    void Slot1a0(void const* Contact, void const* B, FClientGameobj* Other,
                 void const* D) override;
    // 105 +0x1a4 the routine.
    void Slot1a4(void const* A, void const* B, FClientGameobj* Munition) override;
    // 108 +0x1b0 the routine.
    void Slot1b0(Vector const* Pos) override;
    // 110 +0x1b8 the routine.
    void Slot1b8(float Dt) override;
    // 112 +0x1c0 the routine (RET 0x14): EquippedObj's ignite (the routine) and, when it
    // lit DEATH_COMM_FUSE, the formation / follow followers' behaviours are told (event 7 with
    // this ship's id) and the player drops this ship as its target.
    bool Slot1c0(unsigned SourceId, ID_String const* FuseId, unsigned short SubObjId, float AtT,
                 float Lifetime) override;
    // 115 +0x1cc the routine (RET 4). Port: CliShipHandleEquipUpdate (cli_projectile_fx.cpp).
    unsigned char Slot1cc(FCliEquipFirePacket const* Packet) override;
    // 116 +0x1d0 the branch (bare RET).
    virtual void Slot1d0();

    // IObjAffect overrides (module storage).
    int set_throttle(float Throttle) override;
    int set_axis_throttle(Vector const& AxisThrottle) override;
    int set_nudge_vec(Vector const& NudgeVec) override;
    int set_strafe_dir(StrafeDir Dir) override;
    int set_formation_leader(IObjRW* Leader) override;
    int set_formation_offset(Vector const& Offset) override;
    int add_formation_follower(IObjRW* Follower) override;
    int remove_formation_follower(IObjRW* Follower) override;
    int slot_38(unsigned short EquipId, int Count, int SourceClient) override;
    int set_target(IObjRW* Target, unsigned short SubTarget, int SourceClient) override;
    int slot_50(bool bEngage, bool bArg2, int SourceClient) override;
    int slot_54(bool bActivate, int SourceClient) override;
    int slot_5c(int RequestType, unsigned const* TargetId, unsigned const* SecondId,
                int bFlag, RequestCB* Callback) override;
    int slot_60(int RequestType, unsigned const* TargetId, int Arg3) override;
    int go_tradelane(IObjInspect const* EntranceRing, IObjInspect const* ExitRing,
                     bool bFastExit, float Speed) override;
    int stop_tradelane() override;
    int slot_6c(int ArgA, int ArgB) override;
    int slot_70(unsigned Id) override;
    int slot_74(unsigned Arg) override;

    // (__thiscall, RET 8) -- the client side of a launch, called by the IClient
    // launch handler (the branch at 0x0053d877) with the dock gameobj found in bucket 0 and
    // the launch-point index. Only the player's character launches (get_id vs the field):
    // CShip::launch, seven seconds of suppressed player collisions (the routine), and -- when
    // this is the watched player ship -- the docking camera placed on the dock's
    // "HPLaunchCam<A+Index>" hardpoint, else "HPDockCam<A+Index>", else the player camera.
    void LaunchFromDock(FClientGameobj* DockGo, int LaunchIndex);

    // (__thiscall, RET 8) -- the client side of a landing, called by the IClient
    // land handler the routine (0x0053d7cf) with the dock gameobj (null when it is not in the
    // ObjDB) and the base id. Flag bit 3 of +0x18c; with a dock, an Action_Land
    // (client_land_action.h), the axis throttle zeroed and, for the watched player ship, the
    // docking camera on the dock's "HPDockCam" 'A' hardpoint and its three-second fade. Without
    // a dock the player's ship asks for the base straight away and cuts the screen to black;
    // any other ship goes through slot 87 with true.
    void RequestLand(FClientGameobj* DockGo, unsigned const* BaseId);

    // (__thiscall, RET 4) -- the jump state machine: 0 idle, 1 jump out (the
    // Action_JumpOut swing), 2 into the tunnel, 3 system switch requested, 4 jump in, 5 done.
    // Each state runs its entry work (and the JumpShipEffect's hook); any other value warns
    // (ship.cpp line 0x272). The state lands in +0x158 and the effect's clock (+0x70) restarts.
    void SetJumpState(int State);
    // (__thiscall, RET 4) -- the jump out through Gate (the gate / hole solar
    // gameobj): a fresh JumpShipEffect for it at +0x188, state 1, and an Action_JumpOut over the
    // effect's JumpOutTime. Caller: the IClient system-switch-out handler (0x0053d66e).
    void BeginJumpOut(FClientGameobj* Gate);
    // (__thiscall, RET 8) -- the jump in at Pos / Orient: +0x15c = 1; the player's
    // ship stores them in its effect and raises the camera cut, any other ship gets a fresh
    // no-gate JumpShipEffect with them and goes to state 4. Caller: the IClient system-switch-in
    // handler (0x0053d70f).
    void BeginJumpIn(Vector const* Pos, Matrix const* Orient);
    // (__thiscall on the ship, RET 4; `this` is never read) -- Gate narrowed to a
    // CSolar ((class & 0x303) == 0x303), its DestSystem (+0x1b4) stored as the pending jump
    // system (the routine, flag 0). Caller: the system-switch-out handler (0x0053d65d) for a
    // gate type with neither bit 0x40 nor 0x800.
    void RequestSystemSwitch(FClientGameobj* Gate);
    // (__thiscall, RET 8) -- the object narrowed to a CShip ((class & 0x503) ==
    // 0x503), then a tail JMP to CShip::airlock_in(Pos, Orient). Caller: the system-switch-in
    // handler (0x0053d6d5).
    void AirlockIn(Vector const& Pos, Matrix const& Orient);
    // (__thiscall, RET 4) -- the pilot mesh: with a non-empty PilotMesh and an
    // "HpPilot" hardpoint on the ship, the simple archetype PilotMesh names goes to +0x104
    // (load, companion Load(0), create_instance -> +0x10c), and +0x118 becomes {HpPilot's
    // orientation, HpPilot's position minus the pilot mesh's "HpConnect" position}. Caller:
    // slot 92 (the routine at 0x00544801) with the ship companion's pilot_mesh (+0x64).
    void AttachPilotMesh(ID_String const* PilotMesh);
    // (__thiscall, RET 4; the pushed connection id is never read) -- the next
    // target: every object in the ship's scanner list (CEquipManager::FindFirst(0x80000),
    // ScanList +0x24 / count +0x824) that the CShip calls an enemy and that is not dying; a
    // CSimple also needs TypeBits & 0x7f0000 or a watch-listed gameobj (+0xa8). The nearest
    // watch-listed one wins, else the nearest of all; null without a scanner. Caller: HUD_Target
    // event 0x26 (0x004e2a6b).
    IObjRW* PickNextTarget(int ConnectionId);
    // (__thiscall, RET 8) -- the player's target: IObjAffect set_target (vt+0x48)
    // with the player's connection, then every gun (CEquipTraverser(0x80)) re-aimed at it
    // through the routine (FClientEquippedObj::slot_44, called directly). Callers: HUD_Target
    // events 0x24 / 0x25 / 0x26.
    void SetTargetWithGuns(IObjRW* Target, unsigned short SubTarget);
    // (__thiscall, RET 4) -- the tractor: false without a tractor
    // (FindFirst(0x100000)); with a Target and a tractor that is not destroyed, a CLoot target
    // ((class & 0x803) == 0x803) gets IObjAffect vt+0x40; otherwise vt+0x4c with false. True
    // whenever there is a tractor. Caller: HUD_Target event 0x8b (0x004e2ab5).
    bool RequestTractor(IObjRW* Target);
    // (__thiscall, RET 0xc) -- the answer to the request slot_5c sent: the pending
    // callback (+0xec) hears (Result, Code, Extra) and is dropped when it answers false (no
    // callback: "stale request detected"); the player's ship then plays the docking voice line
    // for Result 0 / 2 and Code 0..5. Caller: the IClient REQUEST_RETURNED handler the branch
    // (0x0053bfa4), on whatever gameobj the id names.
    void RequestReturned(int Result, int Code, int Extra);
    // (__thiscall, bare RET) -- SND_CARGO_JETTISONED at the hull's HpBaySurface
    // hardpoint (the bay-door sequence of the notify, case 2, with that sound). Caller: the
    // CREATELOOT handler the routine (0x0053bc5f) for a kind-1 request, on the owner's gameobj.
    void PlayCargoJettisonSound();

    // ---- data (the routine zeroes all of it; names not recovered yet) ----------------
    // +0xe4 / +0xe8 -- a countdown and a count read by the update (the routine,
    // ): while +0xe4 is non-zero it runs down by Dt; once it reaches 0,
    // CShip::close_bay runs +0xe8 times and both return to 0. The writer is the server arm of
    // IObjAffect slot 14 (the routine, 0x005468ba..0x005468cd): one more close per jettison,
    // the countdown += open_bay's result + CShip::JETTISON_CLEARANCE_TIME.
    float Field_0xe4;                  // +0xe4
    int Field_0xe8;                    // +0xe8
    RequestCB* Field_0xec;             // +0xec  slot_5c (00546df4) stores its callback, slot_60 clears
    ISoundSource* Field_0xf0;          // +0xf0  the dtor stops (vt+0x60) and releases (vt+8) it
    // +0xf4 .. +0x103 -- a 0x10 effect companion (arch, sound, particle, light): the dtor
    // releases +0xfc (the routine), +0xf8 (vt+8), +0x100 (the routine), then zeroes +0xf4.
    void* Field_0xf4[4];
    Archetype::Root* Field_0x104;      // +0x104 the dtor releases it (0x00544501)
    FMeshHolder Field_0x108;           // +0x108 the routine (the dtor runs the routine)
    Transform Field_0x118;             // +0x118 identity orientation, zero position
    Archetype::ISound* Field_0x148;     // +0x148 fire-failed sound archetype
    ISoundSource* Field_0x14c;         // +0x14c the dtor stops and releases it
    float Field_0x150;                 // +0x150 the update runs it down by Dt while above 0
    unsigned char Field_0x154;         // +0x154
    unsigned char Pad155[3];
    // +0x158 -- the state the routine (switch 0..5) stores last (0x00543b12). Readers: the
    // draw pass (0x0051c221 compares 2 and 3), StarSys music (0x005550c4), 0x0054380d (== 5).
    int Field_0x158;
    unsigned char Field_0x15c;         // +0x15c the routine sets 1 (0x0054364b)
    unsigned char Pad15d[3];
    // +0x160 .. +0x183 -- one 0x24-byte member whose destructor is the routine (inlined in the
    // Ship dtor at 0x00544613..0x0054468c): a dword, then two 0x10 effect companions. They are
    // the tradelane ring FX binders tradelane_fx.cpp reaches by offset (+0x164 / +0x174). The
    // earlier "0xdc-byte object at +0x17c" was the dtor's ESI-relative +0x17c misread: the Ship
    // dtor runs with ESI = this + 0xc, so that object is +0x188.
    unsigned Field_0x160;              // +0x160
    void* Field_0x164[4];              // +0x164 companion (arch, sound, particle, light)
    void* Field_0x174[4];              // +0x174 companion
    unsigned Field_0x184;              // +0x184 (tradelane_fx.cpp reads bytes +0x184/+0x185)
    // +0x188 -- a 0xdc-byte object built by the routine (the routine(this, Arg)) or
    // (the routine(this)), destroyed by the routine + free (the routine case 0,
    // , the routine, the dtor). the routine's tail clears its +0x70.
    void* Field_0x188;
    // +0x18c -- flag byte: bit 0 / bit 1 set by slot 105 (the routine, 0x00545f07), both
    // cleared by slot 92 (the routine, 0x005447c6), bit 3 set by the routine (0x00543b71).
    // The ctor clears bits 0..1 and then stores 0 over the whole dword (0x00543498/0x005434a5).
    unsigned char Field_0x18c;
    unsigned char Pad18d[3];
};
static_assert(offsetof(FClientShipGameobj, Field_0xec) == 0xec, "Ship +0xec");
static_assert(offsetof(FClientShipGameobj, Field_0xf0) == 0xf0, "Ship +0xf0");
static_assert(offsetof(FClientShipGameobj, Field_0x104) == 0x104, "Ship +0x104");
static_assert(offsetof(FClientShipGameobj, Field_0x108) == 0x108, "Ship mesh holder +0x108");
static_assert(offsetof(FClientShipGameobj, Field_0x118) == 0x118, "Ship transform +0x118");
static_assert(offsetof(FClientShipGameobj, Field_0x148) == 0x148, "Ship +0x148");
static_assert(offsetof(FClientShipGameobj, Field_0x158) == 0x158, "Ship +0x158");
static_assert(offsetof(FClientShipGameobj, Field_0x160) == 0x160, "Ship +0x160");
static_assert(offsetof(FClientShipGameobj, Field_0x164) == 0x164, "Ship companion +0x164");
static_assert(offsetof(FClientShipGameobj, Field_0x174) == 0x174, "Ship companion +0x174");
static_assert(offsetof(FClientShipGameobj, Field_0x184) == 0x184, "Ship +0x184");
static_assert(offsetof(FClientShipGameobj, Field_0x188) == 0x188, "Ship +0x188");
static_assert(offsetof(FClientShipGameobj, Field_0x18c) == 0x18c, "Ship +0x18c");
static_assert(sizeof(FClientShipGameobj) == 0x190, "Ship sizeof 0x190 (operator_new(400))");

// ===========================================================================
// The projectile branch: Projectile : Simple, Munition : Projectile, Beam : Munition.
//   Projectile  ctor the routine(kind)  0xc0  IObjRW 005df95c, IObjAffect 005df8dc,
//                                             CBase 005df8d0 -> the routine
//   Munition    ctor the routine(kind)  0xc0  IObjRW 005dd8cc, IObjAffect 005dd84c,
//                                             CBase 005dd844 -> the routine
//   Beam        ctor inlined in the routine (0x0052de9f..0x0052ded2)
//                                       0xd4  IObjRW 005dd67c, IObjAffect 005dd5fc,
//                                             CBase 005dd5f4 -> the routine
//   Mine        ctor inlined in the routine (0x0053ca8d..0x0053caa4): the routine(5) and the
//                                       0xc0  IObjRW 005dedcc, IObjAffect 005ded4c,
//                                             CBase 005ded44 -> the routine
//   CounterMeasure  inlined in the routine (0x0053cdbd..0x0053cdd4): the routine(6)
//                                       0xc0  IObjRW 005df01c, IObjAffect 005def9c,
//                                             CBase 005def94 -> the routine
//   Guided      inlined in the routine (0x0053d0cd..0x0053d0e4): the routine(4)
//                                       0xc0  IObjRW 005df26c, IObjAffect 005df1ec,
//                                             CBase 005df1e4 -> the routine
// RTTI (COL at vftable[-4]): Mine 005eef54 and CounterMeasure 005eefe4 are Projectile's
// children, Guided 005ef074 is Munition's. The three add no data (operator new(0xc0), the
// Projectile size) and share the deleting destructor the routine (Projectile's body). IObjAffect
// is Simple's table unchanged in all of them except Guided's slot 18 (the branch).
// ===========================================================================

class FClientProjectileGameobj : public FClientSimpleGameobj
{
public:
    // (RET 4): Simple's ctor, ConstEffectDelay = -1.0f, the companion zeroed.
    explicit FClientProjectileGameobj(EClientGameobjKind Kind);
    // (this = the CBase sub-object); deleting wrapper the routine.
    ~FClientProjectileGameobj() override;

    // 90 +0x168: pure (0x005b8064).
    void create(Archetype::Root* Arch) override = 0;
    // 92 +0x170 the routine: GameObject's slot 92, then the munition companion's const_effect
    // -- started now through the routine when its delay is 0, else the delay is armed.
    void Slot170() override;
    // 93 +0x174 the routine (RET 4): unless done, GameObject's update, then the armed delay
    // counts down and, on reaching 0, starts the const_effect.
    void update(float Dt) override;
    // 104 +0x1a0 the routine: RET 0x10, empty.
    void Slot1a0(void const* Contact, void const* B, FClientGameobj* Other,
                 void const* D) override;
    // 108 +0x1b0 the routine (RET 4): drop the companion's node from the effect list
    // (the routine), then GameObject's slot 108.
    void Slot1b0(Vector const* Pos) override;
    // 112 +0x1c0 the branch (RET 4): *Out = identity rotation, zero translation.
    virtual void Slot1c0(Transform* Out);

    float ConstEffectDelay;     // +0xac  -1.0f = not armed
    void* OneShotCompanion[4];  // +0xb0  {EffectArch, ISoundSource*, particle, light}
};
static_assert(offsetof(FClientProjectileGameobj, ConstEffectDelay) == 0xac, "Projectile +0xac");
static_assert(offsetof(FClientProjectileGameobj, OneShotCompanion) == 0xb0, "Projectile +0xb0");
static_assert(sizeof(FClientProjectileGameobj) == 0xc0, "Projectile sizeof 0xc0");

class FClientMunitionGameobj : public FClientProjectileGameobj
{
public:
    // (RET 4): Projectile's ctor with the kind, then Munition's vtables.
    explicit FClientMunitionGameobj(EClientGameobjKind Kind);
    // Projectile's body (the thunk the routine); deleting wrapper the routine.
    ~FClientMunitionGameobj() override;

    // 101 +0x194 the routine (RET 0x10): Other->Slot1a4(A, D, this), then
    // this->Slot1ac(A, B, Other, D).
    void Slot194(void const* A, void const* B, FClientGameobj* Other, void const* D) override;
};
static_assert(sizeof(FClientMunitionGameobj) == 0xc0, "Munition sizeof 0xc0");

struct FBeamEffect; // beam_effect.h

class FClientBeamGameobj : public FClientMunitionGameobj
{
public:
    // Munition's ctor with kind 3, the routine on +0xc0 with four
    // nulls, +0xd0 = 0, Beam's vtables.
    FClientBeamGameobj();
    // (this = the CBase sub-object); deleting wrapper the routine.
    ~FClientBeamGameobj() override;

    // 90 +0x168 the routine (RET 4): the routine(0x67, Arch), then the CBeam's vt+0x4c
    // open(Arch) with no null test.
    void create(Archetype::Root* Arch) override;
    // 91 +0x16c the routine: XOR AL,AL / RET 4.
    bool find(unsigned const& Id) override;
    // 93 +0x174 the branch (RET 4): Projectile's update, the object's vt+0x50
    // update(Dt, CObject::UPDATE_DEFAULT), then Slot15c(true) once the object's +0xe8 life
    // goes below 0 (unless Slot154).
    void update(float Dt) override;
    // 101 +0x194 the branch (RET 0x10): unless Slot154 -- the pair (Other->Slot1a4,
    // this->Slot1ac), the CBeam's +0xf8 velocity zeroed, the beam effect flagged hit
    // (the routine(1, Other == player)) and released, the companion's node dropped, then
    // Projectile's slot 108 at A.
    void Slot194(void const* A, void const* B, FClientGameobj* Other, void const* D) override;
    // 105 +0x1a4 the branch: RET 0xc, empty.
    void Slot1a4(void const* A, void const* B, FClientGameobj* Munition) override;
    // 107 +0x1ac the branch (RET 0x10): the munition-collision report to the server,
    // IServer::SPMunitionCollision (slot 53, +0xd4) -- in multiplayer only for the player's
    // own shots.
    void Slot1ac(void const* A, void const* B, FClientGameobj* Other, void const* D) override;
    // 111 +0x1bc the branch: 0x2000000.
    unsigned Slot1bc() const override;

    void* Companion[4];       // +0xc0  the routine; the effect list gets a copy
    FBeamEffect* BeamAlloc;   // +0xd0  the routine's beam effect
};
static_assert(offsetof(FClientBeamGameobj, Companion) == 0xc0, "Beam +0xc0");
static_assert(offsetof(FClientBeamGameobj, BeamAlloc) == 0xd0, "Beam +0xd0");
static_assert(sizeof(FClientBeamGameobj) == 0xd4, "Beam sizeof 0xd4");

// Mine : Projectile (baseline Client\Space\Mine.cpp has no string of its own;
// the bodies sit at 0x0053e930..0x0053eae0 beside Loot.cpp's).
class FClientMineGameobj : public FClientProjectileGameobj
{
public:
    // the routine(5), then Mine's three vtables.
    FClientMineGameobj();
    // -> thunk_FUN_005425f0: Projectile's body.
    ~FClientMineGameobj() override;

    // IObjInspect slot 1 (+0x04) the branch (RET 4): a destroyed mine (Flags bit 2) reports a
    // zero velocity, any other asks IObjInspectImpl::get_velocity.
    Vector get_velocity() const override;

    // 90 +0x168 the routine (RET 4): the routine(0xf, Arch), then vt+0x4c open(Arch) on the
    // object narrowed by class & 0xf (no null test).
    void create(Archetype::Root* Arch) override;
    // 91 +0x16c the routine (RET 4): CObject::Find(Id, 0xf), advise(true), Object != 0.
    bool find(unsigned const& Id) override;
    // 93 +0x174 the routine (RET 4): Projectile's update, then the CMine's vt+0x50 update --
    // UPDATE_DEFAULT when the object's owner is this connection, UPDATE_DUMB in multiplayer.
    void update(float Dt) override;
    // 105 +0x1a4 the branch (RET 0xc): only a CBeam (class & 0x67) munition reaches
    // GameObject's munition-hit effect (a tail JMP to the routine).
    void Slot1a4(void const* A, void const* B, FClientGameobj* Munition) override;
    // 110 +0x1b8 the routine (RET 4): while Field_0x34 is exactly 0, the nebula at the position
    // into DrawStateA / its fog factor into DrawStateB, and Field_0x34 = 1.0f.
    void Slot1b8(float Dt) override;
};
static_assert(sizeof(FClientMineGameobj) == 0xc0, "Mine sizeof 0xc0 (operator new(0xc0))");

// CounterMeasure : Projectile (bodies at 0x0052ff90..0x005300c0).
class FClientCounterMeasureGameobj : public FClientProjectileGameobj
{
public:
    // the routine(6), then CounterMeasure's three vtables.
    FClientCounterMeasureGameobj();
    // -> thunk_FUN_005425f0: Projectile's body.
    ~FClientCounterMeasureGameobj() override;

    // 90 +0x168 the routine (RET 4): the routine(0x17, Arch), open on class & 0x17.
    void create(Archetype::Root* Arch) override;
    // 91 +0x16c the routine (RET 4): CObject::Find(Id, 0x17), advise(true), Object != 0.
    bool find(unsigned const& Id) override;
    // 93 +0x174 the routine (RET 4): as Mine's with class & 0x17.
    void update(float Dt) override;
    // 105 +0x1a4 the branch: RET 0xc, empty.
    void Slot1a4(void const* A, void const* B, FClientGameobj* Munition) override;
    // 110 +0x1b8 the routine: Mine's.
    void Slot1b8(float Dt) override;
};
static_assert(sizeof(FClientCounterMeasureGameobj) == 0xc0, "CounterMeasure sizeof 0xc0");

// Guided : Munition -- the missile (baseline Munition.cpp, .rdata 005df858; bodies at
// ).
class FClientGuidedGameobj : public FClientMunitionGameobj
{
public:
    // the routine(4), then Guided's three vtables.
    FClientGuidedGameobj();
    // -> thunk_FUN_005425f0: Projectile's body.
    ~FClientGuidedGameobj() override;

    // 90 +0x168 the routine (RET 4): the routine(0xa7, Arch), open on class & 0xa7.
    void create(Archetype::Root* Arch) override;
    // 91 +0x16c the routine (RET 4): CObject::Find(Id, 0xa7), advise(true), Object != 0.
    bool find(unsigned const& Id) override;
    // 93 +0x174 the routine (RET 4): as Mine's with class & 0xa7.
    void update(float Dt) override;
    // 105 +0x1a4 the branch: Mine's (CBeam munitions only).
    void Slot1a4(void const* A, void const* B, FClientGameobj* Munition) override;
    // 107 +0x1ac the branch (RET 0x10): the munition-collision report to the server
    // (IServer::SPMunitionCollision, slot 53) -- in multiplayer only when Other belongs to this
    // connection.
    void Slot1ac(void const* A, void const* B, FClientGameobj* Other, void const* D) override;
    // 108 +0x1b0 the routine (RET 4): the const_effect particle (OneShotCompanion +8) is let go
    // of this gameobj (the routine with no owner, part -1 and the gameobj's frame), then
    // Projectile's slot 108.
    void Slot1b0(Vector const* Pos) override;
    // 110 +0x1b8 the routine: Mine's.
    void Slot1b8(float Dt) override;
    // 111 +0x1bc the branch: 0x1000000.
    unsigned Slot1bc() const override;
    // 112 +0x1c0 the branch (RET 4): the const_effect / trail frame -- the munition
    // companion's HP_trail_parent hardpoint, else the mesh's bounds on the renderer.
    void Slot1c0(Transform* Out) override;

    // IObjAffect slot 18 (+0x48) the branch (RET 0xc): re-target a seeking missile, and tell
    // the server (IServer::SetTarget) when the request came from this connection.
    int set_target(IObjRW* Target, unsigned short SubTarget, int SourceClient) override;
};
static_assert(sizeof(FClientGuidedGameobj) == 0xc0, "Guided sizeof 0xc0");

// Loot : Simple (baseline Client\Space\Loot.cpp, .rdata 005df7e0).
//   ctor the routine  0xb0  IObjRW 005df5d4, IObjAffect 005df554, CBase 005df548 -> the routine
class FClientLootGameobj : public FClientSimpleGameobj
{
public:
    // the routine(7), +0xac = 0, Loot's three vtables.
    FClientLootGameobj();
    // (this = the CBase sub-object); deleting wrapper the routine.
    ~FClientLootGameobj() override;

    // 90 +0x168 the branch (RET 4): the routine(0x803, Arch), open on class & 0x803.
    void create(Archetype::Root* Arch) override;
    // 91 +0x16c the branch (RET 4): CObject::Find(Id, 0x803), advise(true), Object != 0.
    bool find(unsigned const& Id) override;
    // 92 +0x170 the branch: GameObject's slot 92, the crate's tractored_explosion into +0xac,
    // then the nebula lookup (the routine) into DrawStateA / DrawStateB.
    void Slot170() override;
    // 93 +0x174 the routine (RET 4): GameObject's update, then the CLoot's vt+0x50 update.
    void update(float Dt) override;
    // 102 +0x198 the routine (RET 4): multiplayer only -- the root entry sets the hit points,
    // any other entry the contents' hit points (a unit lost when they ran out).
    void Slot198(DamageList const& Damage) override;
    // 103 +0x19c the branch (RET 4): DestroyType 2 (tractored in) explodes with the
    // tractored_explosion; anything else goes to Simple's slot 103.
    void Slot19c(int DestroyType) override;

    Archetype::Explosion* TractoredExplosionArch; // +0xac  slot 92 resolves; the dtor releases
};
static_assert(offsetof(FClientLootGameobj, TractoredExplosionArch) == 0xac, "Loot +0xac");
static_assert(sizeof(FClientLootGameobj) == 0xb0, "Loot sizeof 0xb0 (operator new(0xb0))");

// before the docking camera takes the view, close the HUD's rear view
// (command 0x39 when HUD_Status +0x5ba == 1) and turret view (command 0xbf when +0x5b9 == 1).
// Callers: the ship's land (0x00543cb5) and launch (0x00543ebf / 0x00543eda) paths.
void CloseRearAndTurretViewsForDocking();

// operator new + the Solar constructor the routine (space_solar_world.cpp's solar build).
FClientGameobj* ClientGameobj_CreateSolar();

// (RET 8) -- CObject::Alloc(Class) into Self->Object, Arch->create_instance,
// NeuronRuntime::Engine UpdateInstance(Object+4, 0, 0), CBase::advise(true). The caller opens the
// object afterwards. Callers: GameObject create (the branch inlines it), the ship / solar
// create tails, Beam the routine, Debris the routine.
void GameobjCreateOfClass(FClientGameobj* Self, CObject::Class ObjectClass,
                          Archetype::Root* Arch);

// take a reference on everything an explosion archetype will spawn: each
// effect's EffectArch (the routine) and each innards debris simple (Root vt+0x0c, load).
// Eleven baseline callers (the Gameobj/equipment explosion preloads). Null is a no-op.
void ExplosionArchAddRef(Archetype::Explosion const* Expl);
// the matching release: EffectArch the routine and Root::Release.
void ExplosionArchRelease(Archetype::Explosion const* Expl);

// The Gameobj watch list: baseline std::list<BaseWatcher> the field (head) / the field
// (count), built by the static initialiser the branch and torn down by the branch.
// A Gameobj goes in when its +0xa8 flag is set (the routine, the routine); the HUD walks
// the list at 0x004d2102 and drops dead entries through the routine(0).

// (cdecl) -- append a watcher on Obj unless one already watches it.
void GameobjWatchListAdd(IObjRW* Obj);
// (cdecl) -- erase the first entry watching Obj (a null Obj matches an entry
// whose object has gone), if any.
void GameobjWatchListRemove(IObjRW* Obj);
// Read access for the HUD walk: the list itself, in insertion order.
std::list<BaseWatcher> const& GameobjWatchList();
