// Author: Giperion.

#include "client_gameobj.h"

#include "NeuronRuntime.h"
#include "cli_projectile_fx.h"   // CliEffectListClear / FCliEquipFirePacket / fire-host ports
#include "client_cliobj.h"       // Archetype::CliShipBayDoorSound (CliShip +0x58 / +0x5c)
#include "client_debris.h"       // FindDebrisArch / CreateDebrisGameobjForObject (the routine)
#include "client_equip_fx.h"     // EquippedObjRebuildEquipFx / EquippedObjClearEquipFx
#include "client_explosion.h"    // Action_Explode (the routine)
#include "client_fuse.h"         // FFuseBurnNode
#include "client_objdb.h"        // GetClientObjDb (the field)
#include "common_csys.h"         // Transform (the launch camera's Csys)
#include "common_id.h"           // CreateID
#include "common_ini.h"          // INI_Reader (Solar ReadIni, the routine)
#include "common_newchar.h"      // pub::StateGraph::get_state_graph
#include "common_server.h"       // IServer (the field)
#include "common_misc.h"         // SinglePlayer, GetFileSysPath (Common exports)
#include "neuroncomponents.h"               // FDUMP
#include "common_goods.h"        // FindHardpoint_PS / HardpointInfo
#include "common_physys.h"       // Collision::CalcCollisionDamage
#include "effect_type_info.h"    // EffectTypeWithinPerf (the routine)
#include "engine/sceneruntime.h"
#include "fade_to_black.h"       // SetFadeImmediate (the routine)
#include "engine_light.h"        // FGameLight, EngineLight setters (the routine)
#include "game_options.h"        // GetGameOption (the routine)
#include "globals.h"             // G_RenderSurfaceWidth (the field)
#include "menu_sound_arch.h"     // LookupClientSoundArch (the routine), Source3D factory
#ifdef OUTPOST_FRONTIER_DX9RENDERER
#include "nebula_volume.h"       // NebulaVolume::NoteOpaqueSphere (port-only Direct3D 9 nebula)
#endif
#include "nn_messaging.h"        // NnVoiceEnqueue (the routine)
#include "nn_modal.h"            // G_bNnScreenUp (the field), NnArrivalTextShow (the routine)
#include "hud_command_dispatch.h" // HudPostCommand (the routine)
#include "hud_damage_indicator.h" // GetHudDamageIndicator (the field), the routine / 004d6cd0
#include "hud_status_core.h"     // GetHudStatus (the field)
#include "jump_effect.h"         // FJumpShipEffect (Ship +0x188, dtor the routine)
#include "gate_tunnel.h"         // FGateTunnel::Light (+0x2a8, Ship slot 97 the branch)
#include "part_cache.h"          // CopyTransform12 (the routine)
#include "render/IFLAppearance.h" // / 00546380 / 00546400
#include "hud_waypoint.h"        // GetHudWaypoint (the field), the routine / 004f4ca0
#include "input_manager.h"       // GetClientMouseCursorDrawFlag (the routine)
#include "nn_ui_pulse.h"         // G_NnPaletteArgb_679b80 (the field)
#include "win3d_engine_submit.h" // / the routine (Solar waypoint marker)
#include "client_land_action.h"  // Action_Land (the routine)
#include "particle_effect.h"     // FParticleEffect, ParticleEffectArmDeath / Release
#include "player_behavior.h"     // GetPlayerObjRW (the routine)
#include "player_spawn.h"        // GetPlayerCharacterId (the field), GetPlayerConnectionId
#include "render/rendcomp.h"     // IRenderer2 (Guided slot 112's extents / centre)
#include "server_connect.h"      // GetLocalServerInterface (the field)
#include "soundmanager.h"        // ISoundSource
#include "space_camera.h"        // SetDockingCameraTransform / RequestSpaceCameraMode
#include "space_scene_obj_draw.h" // GameobjPickLodbaseline (the routine)
#include "space_state.h"         // SetPendingLandBase (the routine)
#include "star_sphere.h"         // FStarSphere (Solar +0xe4)
#include "star_sys.h"            // StarSysSuppressPlayerCollisions, GetAttitudeClass
#include "texture_panels.h"      // TexturePanelsCreate / Load (Solar "ring", the routine)
#include "tradelane_fx.h"        // TradeLaneShipGoHandler (the the branch tail)
#include "vis_effect_arch.h"     // FindEffectArch / EffectArchAddRef / EffectArchRelease

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <set>
#include <string>
#include <windows.h>

namespace
{
    char const kGameobjSourceFile[] = "Client\\space\\Gameobj.cpp";
    // the routine's FDUMP file argument.
    char const kEquippedObjSourceFile[] =
        "Client\\Space\\EquippedObj.cpp";
    // the routine's FDUMP file argument.
    char const kShipSourceFile[] = "Client\\space\\ship.cpp";

    // The 0x18-byte node of the GameObject effect list (next, prev, 0x10 companion) -- the
    // head and the list's own teardown live here; cli_projectile_fx.cpp owns the operations.
    struct FGameobjEffectListHead
    {
        FGameobjEffectListHead* Next;
        FGameobjEffectListHead* Prev;
        void* Companion[4];
    };
    static_assert(sizeof(FGameobjEffectListHead) == 0x18, "effect list node 0x18");
}

// ===========================================================================
// GameObject effect list (+0x28)
// ===========================================================================

FGameobjEffectListStorage::FGameobjEffectListStorage(unsigned char InAllocatorByte)
{
    // Inlined in the routine at 0x00539340..0x00539353: the allocator byte, the head node
    // operator_new(0x18) with next = prev = itself, count 0.
    AllocatorByte = InAllocatorByte;
    FGameobjEffectListHead* const NewHead =
        static_cast<FGameobjEffectListHead*>(::operator new(sizeof(FGameobjEffectListHead)));
    NewHead->Next = NewHead;
    NewHead->Prev = NewHead;
    Head = NewHead;
    Count = 0;
}

void* FGameobjEffectListStorage::Erase(void* Where)
{
    // (0x0053aab0..0x0053ab18). Next is read first (0x0053aaba), then the node is
    // unlinked (0x0053aabf..0x0053aac8); the companion teardown is the routine's sequence
    // inlined: particle +0x10 (the routine, cleared), sound +0xc (vt+8, cleared), light +0x14
    // (the routine, cleared), slot 0 (+8) = 0; the node freed, --count, *out = next.
    FGameobjEffectListHead* const Node = static_cast<FGameobjEffectListHead*>(Where);
    FGameobjEffectListHead* const Next = Node->Next;
    Node->Prev->Next = Node->Next;
    Node->Next->Prev = Node->Prev;
    CliEffectCompanionClear(Node->Companion);
    ::operator delete(Node);
    Count = Count - 1;
    return Next;
}

FGameobjEffectListStorage::~FGameobjEffectListStorage()
{
    // (0x0053aa40..0x0053aa97; also inlined in the routine at 0x0053940c..
    // and, through the routine / the routine, in the routine at 0x00539623..
    // ): erase every node from head->next up to the head, free the head, head and
    // count = 0.
    FGameobjEffectListHead* const ListHead = static_cast<FGameobjEffectListHead*>(Head);
    void* Node = ListHead->Next;
    while (Node != ListHead)
        Node = Erase(Node);
    ::operator delete(Head);
    Head = nullptr;
    Count = 0;
}

FGameobjEffectList::~FGameobjEffectList()
{
    // the routine(0) under EH state 0 (its unwind action is the routine, the
    // storage destructor), then the storage destructor (the base, below) on the normal path.
    CliEffectListClear(this, nullptr);
}

void GameobjCreateOfClass(FClientGameobj* Self, CObject::Class ObjectClass,
                          Archetype::Root* Arch)
{
    // (module storage-00539c04, RET 8), the whole body: Alloc(Class) into
    // CBase::Object (+0x10); Arch->create_instance(Object) (module storage, return value
    // dropped -- the engine stores the id at CObject+4 through
    // IEngineInstance::initialize_instance); NeuronRuntime::Engine vt+0x6c UpdateInstance(Object+4,
    // 0, 0); CBase::advise(true) on this+0xc. No null tests. open is NOT part of it -- the
    // caller does that afterwards.
    Self->Object = CObject::Alloc(ObjectClass);
    Arch->create_instance(static_cast<IEngineInstance*>(Self->Object));
    NeuronRuntime::Engine->UpdateInstance(reinterpret_cast<void*>(Self->Object->EngineInstanceId), 0u,
                                  nullptr);
    Self->advise(true);
}

namespace
{
    // The object narrowed by (ObjectClass & Mask) == Mask, null otherwise. `this + 0xc` (the
    // CBase sub-object) is never null for a live gameobj, so the tests baseline makes on it and on
    // `this` fold into the object test.
    CObject* NarrowObject(FClientGameobj const* Self, unsigned Mask)
    {
        CObject* const Obj = Self->Object;
        if (Obj == nullptr || (static_cast<unsigned>(Obj->ObjectClass) & Mask) != Mask)
            return nullptr;
        return Obj;
    }

    // The create tail the routine (solar, 0x303) and the branch (ship, 0x503) share:
    // (Class, Arch); the object narrowed by Class gets vt+0x4c open(Arch) with no
    // null test; then, narrowed again, vt+0xc4 CEqObj::connect(INotify*) with this+0xac as a
    // tail JMP (0x0054783d / 0x005465bd).
    void CreateOpenConnect(FClientEquippedObj* Self, CObject::Class ObjectClass,
                           Archetype::Root* Arch)
    {
        GameobjCreateOfClass(Self, ObjectClass, Arch);
        unsigned const Mask = static_cast<unsigned>(ObjectClass);
        NarrowObject(Self, Mask)->open(Arch);
        static_cast<CEqObj*>(NarrowObject(Self, Mask))->connect(static_cast<INotify*>(Self));
    }

    // (solar, 0x303) / the routine (ship, 0x503): CObject::Find(Id, Class) into
    // +0x10, CBase::advise(true), then the narrowed object's vt+0xc4 connect(this+0xac) with no
    // null test, and `Object != 0`.
    bool FindAndConnect(FClientEquippedObj* Self, CObject::Class ObjectClass, unsigned const& Id)
    {
        // The exe imports ?Find@CObject@@SAPAU1@ABIW4Class@1@@Z -- the reference overload; the
        // slot's own reference argument is pushed through unchanged (0x0054776b / 0x005465db).
        Self->Object = CObject::Find(Id, ObjectClass);
        Self->advise(true);
        static_cast<CEqObj*>(NarrowObject(Self, static_cast<unsigned>(ObjectClass)))
            ->connect(static_cast<INotify*>(Self));
        return Self->Object != nullptr;
    }

    // The server interface every multiplayer path talks to: the field.
    IServer* LocalServer()
    {
        return static_cast<IServer*>(GetLocalServerInterface());
    }

    // (.data, 3; its only reference is the read at 0x0053a016): how many
    // HitEffects slots the routine scans.
    constexpr int kHitEffectSlotCount = 3;
    // MOV [ESP+0x14],0x42c80000 -- the replace-by-lowest ceiling, 100.0f.
    constexpr float kHitEffectReplaceCeiling = 100.0f;
    static_assert(sizeof(FClientGameobj::HitEffects) / sizeof(FClientGameobj::HitEffects[0]) ==
                      kHitEffectSlotCount,
                  "HitEffects holds module state slots");

    // The companion of the object's archetype, or null -- CObject::get_archetype called twice
    // with a null test on the object and on the archetype, as 0x00539d27..0x00539d42 and
    // do.
    Archetype::ICliObj* CompanionOfObjectArchetype(CObject* Obj)
    {
        if (Obj == nullptr || Obj->get_archetype() == nullptr)
            return nullptr;
        return Obj->get_archetype()->Companion;
    }

    // The hit-effect placement shared by the routine (0x0053a0f9..0x0053a1e4) and the routine
    // (0x0053a377..0x0053a464): GetDirectionMatrix on {the object's +0x08 orientation copied,
    // zero translation} and the object-space direction; the result's columns taken in the order
    // 0, -2, 1 (the FCHS'd floats stored back over the matrix); the object-space point as the
    // translation. No null test on the object.
    void BuildHitEffectTransform(FTransform* Out, CObject* Obj, FVector const& Direction,
                                 FVector const& LocalPos)
    {
        Transform Basis;
        Basis.Rotation = Obj->Orientation;
        Basis.Translation.x = 0.0f;
        Basis.Translation.y = 0.0f;
        Basis.Translation.z = 0.0f;
        Vector Target;
        Target.x = Direction.X;
        Target.y = Direction.Y;
        Target.z = Direction.Z;
        Matrix const Facing = GetDirectionMatrix(Basis, Target);
        int Row = 0;
        for (Row = 0; Row < 3; ++Row)
        {
            Out->Rotation.D[Row][0] = Facing.E[Row][0];
            Out->Rotation.D[Row][1] = -Facing.E[Row][2];
            Out->Rotation.D[Row][2] = Facing.E[Row][1];
        }
        Out->Translation = LocalPos;
    }
}

// ===========================================================================
// GameObject
// ===========================================================================

FClientGameobj::FClientGameobj(EClientGameobjKind Kind)
    // the effect list is built as a member (EH state 1 -> 2): its
    // allocator byte takes the kind's low byte, the head node is operator_new(0x18) with
    // next = prev = itself, count 0 (FGameobjEffectListStorage's constructor). From here on an
    // exception destroys it through the routine (Unwind@005c0aa8).
    : EffectList(static_cast<unsigned char>(static_cast<int>(Kind)))
{
    // (RET 4). The IObjInspectImpl part (+0x08 Watchable, +0x10 Object zeroed,
    // the vtables) is the base constructors'; Actions (+0x20/+0x24 zeroed at 0x0053932f /
    // ) is ActionDB's. ListNext / ListPrev (+0x14 / +0x18), CachedAttitude (+0x9c)
    // and bWatchListed (+0xa8) are not written: the ObjDB link, the routine and the derived
    // constructors set them.
    unsigned char const KindByte = static_cast<unsigned char>(static_cast<int>(Kind));
    ManagerKind = static_cast<int>(Kind);

    // the low nibble of the flag byte is cleared; the rest of it keeps
    // whatever the allocation held.
    Flags = static_cast<unsigned char>(Flags & 0xf0u);
    Field_0x34 = 0.0f;
    DrawStateA = 0;
    DrawStateB = 0;
    ExplosionArch = nullptr;
    // Field_0x48: the routine -- FFontString's own constructor, run as a member.
    Field_0x7c = 0;
    SoundAllocatorByte = KindByte;
    SoundsBegin = nullptr;
    SoundsEnd = nullptr;
    SoundsCap = nullptr;
    Field_0xa4 = 0;
    AttitudeCountdown = -1;
    HitEffects[0] = nullptr;                                                  // ..c7
    HitEffects[1] = nullptr;
    HitEffects[2] = nullptr;
}

FClientGameobj::~FClientGameobj()
{
    // (this = the CBase sub-object; the offsets below are the gameobj's).
    // the routine on the font string.
    FontStringClearContents(&Field_0x48);

    // every sound element's interface is stopped (vt+0x60) and released
    // (vt+8); the count is taken once, (end - begin) / 8, and a null begin counts as empty.
    int const SoundCount = (SoundsBegin == nullptr) ? 0 : static_cast<int>(SoundsEnd - SoundsBegin);
    for (int Index = 0; Index < SoundCount; ++Index)
    {
        ISoundSource* const Source = SoundsBegin[Index].Source;
        if (Source == nullptr)
            continue;
        Source->Stop();
        SoundsBegin[Index].Source->Release();
    }

    // the three hit effects -- vt+4, the routine, cleared.
    for (int Index = 0; Index < 3; ++Index)
    {
        FParticleEffect* const Effect = HitEffects[Index];
        if (Effect == nullptr)
            continue;
        ParticleEffectArmDeath(Effect); // vt+4 (the branch, shared by both particle vtables)
        ParticleEffectRelease(HitEffects[Index]);
        HitEffects[Index] = nullptr;
    }

    CliEffectListClear(&EffectList, nullptr);
    Actions.free();
    if (ExplosionArch != nullptr)
        ExplosionArchRelease(ExplosionArch);
    advise(false);
    if (Object != nullptr)
        Object->Release();

    // the sound vector's buffer.
    ::operator delete(SoundsBegin);
    SoundsBegin = nullptr;
    SoundsEnd = nullptr;
    SoundsCap = nullptr;

    // the font string's own destructor -- the routine again, the routine
    // on every child slot left (none: the clear above emptied the range), the child buffer
    // freed, then the text buffer freed through msvcrt free when there is one (the clear
    // already freed and nulled it). Called here to keep baseline's member order against the
    // hand-written teardowns around it; ~FFontString then runs on an empty string.
    FontStringDestroy(&Field_0x48);

    // is the effect list's destructor inlined (the routine's body:
    // (0), then the erase-every-node loop through the routine / the routine and
    // the head freed) -- here ~FGameobjEffectList, run as a member after ~FFontString (+0x48)
    // and before ~ActionDB (+0x20), the order baseline's EH states 6 -> 7 -> 1 -> 0 give.
    // ~ActionDB, 0x0053968e ~CBase and 0x005396ab Watchable::unwatch are the member
    // and base destructors that run after this body.
}

unsigned int const FClientGameobj::get_id() const
{
    // MOV EAX,[ECX+0x10]; JZ -> 0; [EAX+0x4c] & 3 == 3 (CMP CL,3 / JNZ -> 0);
    // [EAX+0xb0].
    CObject* const Obj = NarrowObject(this, 0x3u);
    if (Obj == nullptr)
        return 0;
    return static_cast<CSimple const*>(Obj)->Id;
}

bool FClientGameobj::Slot154() const
{
    // .
    return ((Flags >> 2) & 1u) != 0;
}

bool FClientGameobj::Slot158() const
{
    // .
    return (Flags & 1u) != 0;
}

void FClientGameobj::Slot15c(bool bValue)
{
    // bit 0 = the argument; then the object's collisions go off whatever the
    // argument was (the tail JMP pushes 0 over it, 0x00539c2c).
    Flags = static_cast<unsigned char>((Flags & ~1u) | (static_cast<unsigned>(bValue) & 1u));
    if (Object != nullptr)
        Object->enable_collisions_r(false);
}

bool FClientGameobj::Slot160() const
{
    // .
    return ((Flags >> 1) & 1u) != 0;
}

void FClientGameobj::Slot164(bool bValue)
{
    // .
    Flags = static_cast<unsigned char>((Flags & ~2u) | ((static_cast<unsigned>(bValue) << 1) & 2u));
}

void FClientGameobj::create(Archetype::Root* Arch)
{
    // the routine's body inlined with class 1.
    GameobjCreateOfClass(this, CObject::ClassObject, Arch);
}

void FClientGameobj::Slot170()
{
    // no null tests on the object or its archetype.
    Archetype::Root* const Arch = Object->get_archetype();
    ID_String const* const ExplosionId = &Arch->ExplosionArch; // Arch+0x24

    if (!ExplosionId->IsEmpty())
    {
        ExplosionArch = Archetype::GetExplosion(*ExplosionId);
        if (ExplosionArch != nullptr)
        {
            ExplosionArchAddRef(ExplosionArch);
        }
        else
        {
            // FDUMP 0x100002, Gameobj.cpp line 0x85, the archetype's
            // nickname and the explosion id's string (the id's string is fetched first).
            char const* const ExplName = ExplosionId->get_string();
            char const* const ArchName = Object->get_archetype()->Nickname.get_string();
            FDUMP(0x100002, "%s(%d) : *** WARNING: Archtype(%s) explosion_arch(%s) not found.",
                  kGameobjSourceFile, 0x85, ArchName, ExplName);
        }
    }

    // vt+0x88 get_type (result not tested); any bit of 0x107ffdff and
    // bWatchListed -> the routine(this).
    uint Type = 0;
    get_type(Type);
    if ((Type & 0x107ffdffu) != 0u && bWatchListed != 0)
        GameobjWatchListAdd(this);
}

void FClientGameobj::update(float Dt)
{
    // .
    Actions.update(Dt);
    Slot1b8(Dt);
}

float FClientGameobj::Slot178(Vector& Center)
{
    // the object's vt+0x64, no null test.
    return Object->get_physical_radius_r(Center);
}

unsigned* FClientGameobj::Slot17c(unsigned* OutLod, void* Camera, unsigned const* CachedLod)
{
    // the body is GameobjPickLodbaseline (space_scene_obj_draw.cpp), which
    // EquippedObj's override the routine also calls directly.
    return GameobjPickLodbaseline(this, OutLod, Camera, CachedLod);
}

void FClientGameobj::Slot180()
{
    // RET.
}

void FClientGameobj::Slot184(void* Camera)
{
    // the body is GameobjUpdateDrawLightingbaseline (space_scene_obj_draw.cpp,
    // W3-B), the same split as Slot17c / GameobjPickLodbaseline.
    GameobjUpdateDrawLightingbaseline(this, Camera);
}

unsigned* FClientGameobj::Slot188(unsigned* OutLod, void* Camera, unsigned CachedLod,
                                  void* Matrix, void* Extra)
{
    // the body is GameobjSubmitDrawbaseline (space_scene_obj_draw.cpp, W3-B),
    // the same split as Slot17c / Slot184.
    return GameobjSubmitDrawbaseline(this, OutLod, Camera, CachedLod, Matrix, Extra);
}

unsigned FClientGameobj::Slot18c(unsigned* OutB)
{
    // .
    *OutB = DrawStateB;
    return DrawStateA;
}

void FClientGameobj::Slot190()
{
    // .
    DrawStateA = 0;
    DrawStateB = 0;
    Field_0x34 = 10.0f;
}

void FClientGameobj::Slot194(void const* A, void const* B, FClientGameobj* Other, void const* D)
{
    // Other->Slot1a0(A, D, this, B) (0x00539cae), then this->Slot1ac(A, B, Other,
    // D) (0x00539cc0). No null test on Other.
    Other->Slot1a0(A, D, this, B);
    Slot1ac(A, B, Other, D);
}

void FClientGameobj::Slot198(DamageList const& Damage)
{
    // RET 4.
    (void)Damage;
}

void FClientGameobj::Slot19c(int DestroyType)
{
    // .
    if (DestroyType == 0)
    {
        Slot15c(true);
        return;
    }
    if (DestroyType == 1)
    {
        Flags = static_cast<unsigned char>(Flags | 4u);
        Slot1b0(nullptr);
        return;
    }
    FDUMP(0x100001, "%s(%d) : *** ERROR: %s", kGameobjSourceFile, 0x2a4, "Invalid DestroyType!");
}

void FClientGameobj::Slot1a0(void const* Contact, void const* B, FClientGameobj* Other,
                             void const* D)
{
    // (RET 0x10). B and D are not read.
    (void)B;
    (void)D;
    Vector const* const Hit = static_cast<Vector const*>(Contact);
    // |Contact[0]|, summed x*x + y*y, then + z*z.
    float const Speed = sqrtf(Hit->x * Hit->x + Hit->y * Hit->y + Hit->z * Hit->z);
    // / 0x00539d11: Other's mass first, then this one's (vt+0x30 each);
    // Collision::CalcCollisionDamage(Speed, this mass, Other's mass).
    float const OtherMass = Other->get_mass();
    float const MyMass = get_mass();
    float const Damage = Collision::CalcCollisionDamage(Speed, MyMass, OtherMass);
    // this object's archetype companion (CObject::get_archetype, null
    // tests on both) picks the distance record for the damage -- the routine.
    FEffectArch* const Effect =
        Archetype::CliObjPickDistanceRecord(CompanionOfObjectArchetype(Object), Damage);
    // the routine(Other->get_id(), Effect, &Contact[0], &Contact[1], 0).
    SpawnSurfaceHitEffect(Other->get_id(), Effect, Hit, Hit + 1, nullptr);
}

void FClientGameobj::Slot1a4(void const* A, void const* B, FClientGameobj* Munition)
{
    // (RET 0xc). A points at the hit point and the surface normal (+0x0c); B is
    // not read.
    (void)B;
    Vector const* const Hit = static_cast<Vector const*>(A);
    // the munition's class & 7 object (no null test before +0x88) and
    // its Munition archetype.
    Archetype::Munition const* const MunArch =
        static_cast<Archetype::Munition const*>(NarrowObject(Munition, 0x7u)->Archetype);
    // this object's archetype companion picks the distance record for
    // the munition's hull_damage (+0x80) -- the routine.
    FEffectArch* const HullEffect =
        Archetype::CliObjPickDistanceRecord(CompanionOfObjectArchetype(Object), MunArch->HullDamage);
    // the routine keyed by the shot's owner (class & 0x27 object, no
    // null test before +0xe4).
    CProjectile const* const Shot = static_cast<CProjectile const*>(NarrowObject(Munition, 0x27u));
    SpawnSurfaceHitEffect(Shot->OwnerId, HullEffect, Hit, Hit + 1, nullptr);
    // a random live effect of the munition's companion (the class & 7
    // object narrowed again) -- the routine.
    FEffectArch* const LiveEffect = Archetype::CliMunitionPickLiveEffect(
        NarrowObject(Munition, 0x7u)->Archetype->Companion);
    // the class & 1 object's vt+0x54 get_velocity (no null test).
    Vector const Velocity = NarrowObject(Munition, 0x1u)->get_velocity();
    // the routine(LiveEffect, &A[0], &A[1], &Velocity, 0) -- the
    // PUSH 0 at 0x00539e4f is its fifth argument.
    SpawnSplashHitEffect(LiveEffect, Hit, Hit + 1, &Velocity, nullptr);
}

void FClientGameobj::Slot1a8(unsigned short SubObj, unsigned TargetId, unsigned short TargetSubObj)
{
    // RET 0xc.
    (void)SubObj;
    (void)TargetId;
    (void)TargetSubObj;
}

void FClientGameobj::Slot1ac(void const* A, void const* B, FClientGameobj* Other, void const* D)
{
    // (RET 0x10). A is not read; B and D are sub-object ids (low words).
    (void)A;
    // Other's vt+0x20 get_id; 0 -> nothing.
    if (Other->get_id() == 0)
        return;
    // in multiplayer only collisions of objects the local player owns
    // report -- Other's class & 3 object's +0xb4 OwnerPlayer, or the routine (the field)
    // when the narrow fails, against the field.
    if (!SinglePlayer())
    {
        CObject* const OtherObj = NarrowObject(Other, 0x3u);
        unsigned const Owner = (OtherObj != nullptr)
                                   ? static_cast<CSimple const*>(OtherObj)->OwnerPlayer
                                   : GetPlayerConnectionId();
        if (Owner != GetPlayerConnectionId())
            return;
    }
    // this velocity minus Other's (vt+4 each, Other's first).
    Vector const OtherVelocity = Other->get_velocity();
    Vector const MyVelocity = get_velocity();
    Vector Relative;
    Relative.x = MyVelocity.x - OtherVelocity.x;
    Relative.y = MyVelocity.y - OtherVelocity.y;
    Relative.z = MyVelocity.z - OtherVelocity.z;
    // this mass (vt+0x30), Other's mass, |Relative| summed x*x + z*z,
    // then + y*y; Collision::CalcCollisionDamage(|Relative|, Other's mass, this mass).
    float const MyMass = get_mass();
    float const OtherMass = Other->get_mass();
    float const Speed =
        sqrtf(Relative.x * Relative.x + Relative.z * Relative.z + Relative.y * Relative.y);
    float const Damage = Collision::CalcCollisionDamage(Speed, OtherMass, MyMass);
    // {this id, B's low word zero-extended to a dword, Other's id, D's
    // low word (+0x0e never written; zero here), the damage}.
    SSPObjCollisionInfo Info;
    Info.TargetObjectId = get_id();
    Info.TargetSubObjId = static_cast<unsigned short>(reinterpret_cast<uintptr_t>(B));
    Info.Pad_0x06 = 0;
    Info.ObjectId = Other->get_id();
    Info.SubObjId = static_cast<unsigned short>(reinterpret_cast<uintptr_t>(D));
    Info.Pad_0x0e = 0;
    std::memcpy(&Info.Field_0x10, &Damage, sizeof(float));
    // the field slot 55 (+0xdc) SPObjCollision(Info, the field),
    // no null test.
    LocalServer()->SPObjCollision(Info, GetPlayerConnectionId());
}

void FClientGameobj::Slot1b0(Vector const* Pos)
{
    // .
    if (ExplosionArch == nullptr)
    {
        Slot15c(true);
        return;
    }
    CliEffectListClear(&EffectList, nullptr);
    // Pos, or vt+0 get_position() when it is null, copied to the stack;
    // (ExplosionArch, this, &Copy) builds the explode action and ActionDB::add
    // (+0x20) takes it.
    Vector const* const Where = (Pos != nullptr) ? Pos : &get_position();
    Vector const Copy = *Where;
    Actions.add(Action_Explode::Create(ExplosionArch, this, &Copy));
}

void FClientGameobj::Slot1b4(unsigned SourceId, void* SoundArch, Vector const* Pos)
{
    // (RET 0xc). SoundArch is a client sound archetype (SoundArch.cpp).
    // none, or one whose slot 12 (+0x30, the lod link) is null -> nothing.
    if (SoundArch == nullptr || GetClientSoundArchLinked(SoundArch) == nullptr)
        return;

    // the pool's size, 0 with no block.
    int const Count = (SoundsBegin == nullptr) ? 0 : static_cast<int>(SoundsEnd - SoundsBegin);
    ISoundSource* Voice = nullptr;

    // the first entry keyed to SourceId whose voice's archetype (+0x18)
    // has the id (+4) of this archetype's link.
    int Index = 0;
    for (Index = 0; Index < Count; ++Index)
    {
        FGameobjSound* const Entry = &SoundsBegin[Index];
        if (Entry->Field_0x00 != SourceId)
            continue;
        void* const VoiceArch = GetClientSoundSourceArch(Entry->Source);
        void* const Linked = GetClientSoundArchLinked(SoundArch);
        if (GetClientSoundArchId(VoiceArch) == GetClientSoundArchId(Linked))
        {
            Voice = Entry->Source;
            break;
        }
    }

    // otherwise the first entry whose voice is idle (vt+0x14 IsActive
    // false) is taken over for SourceId.
    if (Voice == nullptr)
    {
        for (Index = 0; Index < Count; ++Index)
        {
            FGameobjSound* const Entry = &SoundsBegin[Index];
            if (!Entry->Source->IsActive())
            {
                Voice = Entry->Source;
                Entry->Field_0x00 = SourceId;
                break;
            }
        }
    }

    // otherwise a new Source3D (the routine(0x64) + the routine),
    // posed on this object's engine instance (vt+0x1c get_index) with an identity transform
    // (vt+0x94), and appended to the pool -- the VC6 vector insert at end, growing by
    // max(size, 1).
    if (Voice == nullptr)
    {
        Voice = static_cast<ISoundSource*>(CreateClientSoundSource3D());
        Transform Pose;
        Pose.Rotation = Matrix(1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f);
        Pose.Translation.x = 0.0f;
        Pose.Translation.y = 0.0f;
        Pose.Translation.z = 0.0f;
        int const Attach = static_cast<int>(get_index());
        Voice->SetPose(Attach, reinterpret_cast<float const*>(&Pose));

        FGameobjSound Added;
        Added.Field_0x00 = SourceId;
        Added.Source = Voice;
        if (SoundsCap - SoundsEnd < 1)
        {
            int const Size = (SoundsBegin == nullptr) ? 0 : static_cast<int>(SoundsEnd - SoundsBegin);
            int const Grow = (SoundsBegin == nullptr || Size <= 1) ? 1 : Size;
            int NewCapacity = Size + Grow;
            if (NewCapacity < 0)
                NewCapacity = 0;
            FGameobjSound* const Block = static_cast<FGameobjSound*>(
                ::operator new(static_cast<size_t>(NewCapacity) * sizeof(FGameobjSound)));
            int Copy = 0;
            for (Copy = 0; Copy < Size; ++Copy)
                Block[Copy] = SoundsBegin[Copy];
            Block[Size] = Added;
            ::operator delete(SoundsBegin);
            SoundsCap = Block + NewCapacity;
            SoundsEnd = Block + Size + 1;
            SoundsBegin = Block;
        }
        else
        {
            *SoundsEnd = Added;
            ++SoundsEnd;
        }
    }

    // the voice goes to Pos (vt+0x8c), takes the archetype's random
    // pitch (slot 11, +0x2c) through vt+0x98, and plays: a voice already playing is rewound
    // (vt+0x64 SetStartOffset(0)), an idle one starts on the archetype (vt+0x58 Start(Arch, 0)).
    Voice->Slot35(const_cast<Vector*>(Pos));
    Voice->SetPitch(GetClientSoundArchRandomPitch(SoundArch));
    if (Voice->IsActive())
        Voice->SetStartOffset(0);
    else
        Voice->Start(SoundArch, 0);
}

void FClientGameobj::SpawnSurfaceHitEffect(unsigned SourceId, FEffectArch* EffectArch,
                                           Vector const* Pos, Vector const* Normal,
                                           void* CompanionOut)
{
    // (RET 0x14). CompanionOut ([ESP+0xb0] after the four pushes) is never read.
    (void)CompanionOut;
    // the player's ship shows none of this while SURFACE_HIT_EFFECTS
    // (the routine(0x38)) is off.
    if (static_cast<void*>(this) == GetPlayerObjRW() &&
        GetGameOption(kGameOptSurfaceHitEffects) == 0)
        return;
    // .
    if (EffectArch == nullptr)
        return;

    // no VisEffect (+0x08) -> straight to the sound.
    if (EffectArch->VisEffect != nullptr)
    {
        // the routine(0x84) + the routine(0), then its vt[0] start
        // with the arch -- no null test on the allocation.
        FParticleEffect* const Effect = ParticleEffectCreate();
        if (ParticleEffectStart(Effect, EffectArch) == 0)
        {
            ParticleEffectRelease(Effect);
        }
        else
        {
            // the slot -- the first empty or dying (Flags08 bit 1) one,
            // else the one whose +0x78 is lowest below 100.0f (TEST AH,5 / JP: strictly below,
            // ordered), else slot 0.
            int Chosen = 0;
            float Best = kHitEffectReplaceCeiling;
            int Index = 0;
            for (Index = 0; Index < kHitEffectSlotCount; ++Index)
            {
                FParticleEffect* const Slot = HitEffects[Index];
                if (Slot == nullptr || (Slot->Flags08 & 2u) != 0)
                {
                    Chosen = Index;
                    break;
                }
                if (Slot->RunTime78 < Best)
                {
                    Best = Slot->RunTime78;
                    Chosen = Index;
                }
            }
            // the old one cleaned up (the routine), its vt+4 stop,
            // released (the routine) and cleared. Every HitEffects entry is a base
            // ParticleEffect made right here, so its vt+4 is the branch.
            if (HitEffects[Chosen] != nullptr)
            {
                ParticleEffectCleanupParticleFx(HitEffects[Chosen]);
                ParticleEffectArmDeath(HitEffects[Chosen]);
                ParticleEffectRelease(HitEffects[Chosen]);
                HitEffects[Chosen] = nullptr;
            }
            HitEffects[Chosen] = Effect;

            // Pos and Normal into object space through the field
            // (vt+0x3c InverseTransform, vt+0x34 InverseRotate on the object's +0x08
            // transform, no null test on the object).
            FTransform const& ObjXform =
                reinterpret_cast<FTransform const&>(Object->get_transform());
            FVector LocalPos;
            GameobjMathEngine()->InverseTransform(LocalPos, ObjXform,
                                                  reinterpret_cast<FVector const&>(*Pos));
            FVector LocalNormal;
            GameobjMathEngine()->InverseRotate(LocalNormal, ObjXform,
                                               reinterpret_cast<FVector const&>(*Normal));
            // the orientation along the local normal, placed at the
            // local point, attached to this gameobj (the routine).
            FTransform AttachXform;
            BuildHitEffectTransform(&AttachXform, Object, LocalNormal, LocalPos);
            ParticleEffectAttach(Effect, this, static_cast<int>(get_index()), &AttachXform);
        }
    }

    // the sound arch (+0x18) -> Slot1b4(SourceId, Sound, Pos).
    if (EffectArch->SndEffect != nullptr)
        Slot1b4(SourceId, EffectArch->SndEffect, Pos);
}

void FClientGameobj::SpawnSplashHitEffect(FEffectArch* EffectArch, Vector const* Pos,
                                          Vector const* Normal, Vector const* Velocity,
                                          void* CompanionOut)
{
    // (RET 0x14).
    // the player's ship shows none of this while SPLASH_HIT_EFFECTS
    // (the routine(0x39)) is off.
    if (static_cast<void*>(this) == GetPlayerObjRW() &&
        GetGameOption(kGameOptSplashHitEffects) == 0)
        return;
    // .
    if (EffectArch == nullptr)
        return;
    // the routine(EffectType).
    if (!EffectTypeWithinPerf(EffectArch->EffectType))
        return;

    // Pos (InverseTransform), Normal and Velocity (InverseRotate) into
    // object space. The object-space velocity is written and never read again: the buffer is
    // overwritten by the normal's copy at 0x0053a343..0x0053a355.
    FTransform const& ObjXform = reinterpret_cast<FTransform const&>(Object->get_transform());
    FVector LocalPos;
    GameobjMathEngine()->InverseTransform(LocalPos, ObjXform,
                                          reinterpret_cast<FVector const&>(*Pos));
    FVector LocalNormal;
    GameobjMathEngine()->InverseRotate(LocalNormal, ObjXform,
                                       reinterpret_cast<FVector const&>(*Normal));
    FVector LocalVelocity;
    GameobjMathEngine()->InverseRotate(LocalVelocity, ObjXform,
                                       reinterpret_cast<FVector const&>(*Velocity));

    // the routine on the negated WORLD velocity.
    Vector Incoming;
    Incoming.x = -Velocity->x;
    Incoming.y = -Velocity->y;
    Incoming.z = -Velocity->z;
    // the object-space normal scaled by 2 * (N.z*I.z + N.y*I.y +
    // N.x*I.x) (the routine), minus Incoming (the routine).
    float const Scale = (LocalNormal.Z * Incoming.z + LocalNormal.Y * Incoming.y +
                         LocalNormal.X * Incoming.x) * 2.0f;
    FVector Direction;
    Direction.X = LocalNormal.X * Scale - Incoming.x;
    Direction.Y = LocalNormal.Y * Scale - Incoming.y;
    Direction.Z = LocalNormal.Z * Scale - Incoming.z;

    // the orientation along that direction, at the local point.
    FTransform EffectXform;
    BuildHitEffectTransform(&EffectXform, Object, Direction, LocalPos);
    // the routine on the field (EffectArch, this, get_index,
    // &EffectXform, CompanionOut).
    CliCreateEffectOneShot(EffectArch, this, static_cast<int>(get_index()), &EffectXform,
                           CompanionOut);
}

void FClientGameobj::Slot1b8(float Dt)
{
    // RET 4.
    (void)Dt;
}

int FClientGameobj::GetCachedAttitude()
{
    // .
    int const Countdown = AttitudeCountdown;
    if (Countdown > 0)
    {
        AttitudeCountdown = Countdown - 1;
        return CachedAttitude;
    }
    // SETGE / DEC / AND 0xfffffff9 / ADD 8 -- 8 when the countdown was
    // exactly 0, 1 when it was negative (the ctor's -1: the first call refreshes again soon).
    AttitudeCountdown = (Countdown >= 0) ? 8 : 1;
    IObjRW* const Player = static_cast<IObjRW*>(GetPlayerObjRW());
    if (Player == nullptr)
    {
        CachedAttitude = 0;
        return CachedAttitude;
    }
    // PUSH this, PUSH player -- the routine(player, this).
    CachedAttitude = GetAttitudeClass(Player, this);
    return CachedAttitude;
}

bool FClientGameobj::GetFlag3IfSurfaceWidthUnchanged() const
{
    // .
    if (static_cast<unsigned>(Field_0x7c) != G_RenderSurfaceWidth)
        return false;
    return ((Flags >> 3) & 1u) != 0;
}

void FClientGameobj::SetFlag3(bool bValue)
{
    // .
    Flags = static_cast<unsigned char>((Flags & ~8u) | ((static_cast<unsigned>(bValue) << 3) & 8u));
    if ((Flags & 8u) != 0)
        Field_0x7c = static_cast<int>(G_RenderSurfaceWidth);
}

// IObjAffect -- FClientGameobj's own table, module storage (the routine stores it at +4,
// ). PORTED 2026-09-24: each entry is a baseline one-instruction error stub,
// MOV EAX,-3 / RET 4      module storage MOV EAX,-8 / RET 4
// XOR EAX,EAX / RET       module storage MOV EAX,-3 / RET 0x10
// MOV EAX,-3 / RET 0xc    module storage MOV EAX,-3 / RET 8
// MOV EAX,-3 / RET 0x14   module storage MOV EAX,-3 / RET
// -- the same pattern as Server.dll GameObject's module storage. Simple (005e0024) and Solar
// (005e0284) keep every entry; EquippedObj and Ship override some (see below).
int FClientGameobj::set_throttle(float) { return -3; }
int FClientGameobj::set_axis_throttle(Vector const&) { return -3; }
int FClientGameobj::set_nudge_vec(Vector const&) { return -3; }
int FClientGameobj::set_strafe_dir(StrafeDir) { return -3; }
int FClientGameobj::set_formation_leader(IObjRW*) { return -8; }
int FClientGameobj::set_formation_offset(Vector const&) { return -8; }
int FClientGameobj::add_formation_follower(IObjRW*) { return -8; }
int FClientGameobj::remove_formation_follower(IObjRW*) { return -8; }
int FClientGameobj::get_formation_follower_count() { return 0; }
int FClientGameobj::set_follow_leader(IObjRW*) { return -8; }
int FClientGameobj::set_follow_offset(Vector const&) { return -8; }
int FClientGameobj::add_follow_follower(IObjRW*) { return -8; }
int FClientGameobj::remove_follow_follower(IObjRW*) { return -8; }
int FClientGameobj::slot_34(unsigned short const*, unsigned, Vector const*, int) { return -3; }
int FClientGameobj::slot_38(unsigned short, int, int) { return -3; }
int FClientGameobj::slot_3c(unsigned short const*, std::vector<IObjRW*> const&, int) { return -3; }
int FClientGameobj::slot_40(unsigned short const*, IObjRW*, int) { return -3; }
int FClientGameobj::slot_44(unsigned short const*, IObjRW*, unsigned short, int) { return -3; }
int FClientGameobj::set_target(IObjRW*, unsigned short, int) { return -3; }
int FClientGameobj::slot_4c(unsigned short const*, bool, int) { return -3; }
int FClientGameobj::slot_50(bool, bool, int) { return -3; }
int FClientGameobj::slot_54(bool, int) { return -3; }
int FClientGameobj::slot_58(unsigned short, unsigned, int) { return -3; }
int FClientGameobj::slot_5c(int, unsigned const*, unsigned const*, int, RequestCB*) { return -3; }
int FClientGameobj::slot_60(int, unsigned const*, int) { return -3; }
int FClientGameobj::go_tradelane(IObjInspect const*, IObjInspect const*, bool, float) { return -3; }
int FClientGameobj::stop_tradelane() { return -3; }
int FClientGameobj::slot_6c(int, int) { return -3; }
int FClientGameobj::slot_70(unsigned) { return -3; }
int FClientGameobj::slot_74(unsigned) { return -3; }

// ===========================================================================
// Simple
// ===========================================================================

FClientSimpleGameobj::FClientSimpleGameobj(EClientGameobjKind Kind)
    : FClientGameobj(Kind)
{
    // GameObject's ctor, then +0xa8 = 0 (0x0054744d).
    bWatchListed = 0;
}

FClientSimpleGameobj::~FClientSimpleGameobj()
{
    // -> thunk_FUN_005394b0: nothing of Simple's own.
}

void FClientSimpleGameobj::Slot198(DamageList const& Damage)
{
    // multiplayer only. Every entry of the list whose sub-object is the root
    // (SUBOBJ_ID_ROOT) sets the CSimple's hit points through vt+0x98; the object is narrowed
    // with no null test on the result.
    if (SinglePlayer())
        return;
    for (std::list<DamageEntry>::const_iterator It = Damage.Entries.begin();
         It != Damage.Entries.end(); ++It)
    {
        if (It->SubObjId != SUBOBJ_ID_ROOT)
            continue;
        static_cast<CSimple*>(NarrowObject(this, 3u))->set_hit_pts(It->HitPts);
    }
}

void FClientSimpleGameobj::Slot19c(int DestroyType)
{
    // CSimple::set_hit_pts(0) (vt+0x98, no null test), then the routine.
    static_cast<CSimple*>(NarrowObject(this, 3u))->set_hit_pts(0.0f);
    FClientGameobj::Slot19c(DestroyType);
}

unsigned FClientSimpleGameobj::Slot1bc() const
{
    // the CSimple's +0xe0, no null test.
    return static_cast<CSimple const*>(NarrowObject(this, 3u))->TypeBits;
}

void FClientSimpleGameobj::SetWatchListed(int Unused, int bWatch)
{
    // (RET 8): the first argument is never read.
    (void)Unused;
    bool const bOn = (bWatch == 1);
    bWatchListed = bOn ? 1 : 0;
    if (bOn)
        GameobjWatchListAdd(this);
    else
        GameobjWatchListRemove(this);
}

// ===========================================================================
// EquippedObj
// ===========================================================================

namespace
{
    // The 0xc-byte node of the equip-FX list.
    struct FEquipFxNode
    {
        FEquipFxNode* Next;
        FEquipFxNode* Prev;
        INotify* Wrapper;
    };
    static_assert(sizeof(FEquipFxNode) == 0xc, "equip-FX node 0xc");
}

FEquippedFuseList::FEquippedFuseList(unsigned char InAllocatorByte)
{
    // Inlined in the routine: the allocator byte (0x00532c23), the head operator_new(0x40)
    // with only next = prev = itself written (0x00532c29..0x00532c30; the ClientFuse payload
    // stays raw memory), count 0 (0x00532c39).
    AllocatorByte = InAllocatorByte;
    FFuseBurnNode* const NewHead = static_cast<FFuseBurnNode*>(::operator new(sizeof(FFuseBurnNode)));
    NewHead->Next = NewHead;
    NewHead->Prev = NewHead;
    Head = NewHead;
    Count = 0;
}

FEquippedFuseList::~FEquippedFuseList()
{
    // (0x005359c0..0x00535a18): from head->next up to the head -- next read first,
    // unlink, the fuse's vt+0 with 0 (its destructor, no delete), the node freed, --count; then
    // the head freed and head = count = 0. `delete Node` runs ~FFuseBurnNode (the ClientFuse
    // destructor) and frees the node, the same two steps.
    FFuseBurnNode* const ListHead = static_cast<FFuseBurnNode*>(Head);
    FFuseBurnNode* Node = ListHead->Next;
    while (Node != ListHead)
    {
        FFuseBurnNode* const Next = Node->Next;
        Node->Prev->Next = Node->Next;
        Node->Next->Prev = Node->Prev;
        delete Node;
        Count = Count - 1;
        Node = Next;
    }
    ::operator delete(Head);
    Head = nullptr;
    Count = 0;
}

FEquipFxList::FEquipFxList(unsigned char InAllocatorByte)
{
    // Inlined in the routine: the allocator byte (0x00532bfc), the head operator_new(0xc) with
    // next = prev = itself (0x00532c02..0x00532c09), count 0 (0x00532c12).
    AllocatorByte = InAllocatorByte;
    FEquipFxNode* const NewHead = static_cast<FEquipFxNode*>(::operator new(sizeof(FEquipFxNode)));
    NewHead->Next = NewHead;
    NewHead->Prev = NewHead;
    Head = NewHead;
    Count = 0;
}

FEquipFxList::~FEquipFxList()
{
    // (0x004a2d30..0x004a2d7e, the shared pointer-list destructor): from
    // head->next up to the head -- next read first, unlink, the node freed (the wrapper it
    // points at is not touched), --count; then the head freed and head = count = 0.
    FEquipFxNode* const ListHead = static_cast<FEquipFxNode*>(Head);
    FEquipFxNode* Node = ListHead->Next;
    while (Node != ListHead)
    {
        FEquipFxNode* const Next = Node->Next;
        Node->Prev->Next = Node->Next;
        Node->Next->Prev = Node->Prev;
        ::operator delete(Node);
        Count = Count - 1;
        Node = Next;
    }
    ::operator delete(Head);
    Head = nullptr;
    Count = 0;
}

FClientEquippedObj::FClientEquippedObj(EClientGameobjKind Kind)
    : FClientSimpleGameobj(Kind)
    // the equip-FX list built as a member (EH state 0 -> 1; unwind
    // , Unwind@005c07cb).
    , EquipFx(static_cast<unsigned char>(static_cast<int>(Kind)))
    // the fuse list built as a member (EH state 1 -> 2; unwind
    // , Unwind@005c07d9).
    , Fuses(static_cast<unsigned char>(static_cast<int>(Kind)))
{
    // (RET 4). The flag store at 0x00532be9 sits before the two list builds in
    // baseline; a byte of this object nobody reads while they run.
    EquipFlags = static_cast<unsigned char>(EquipFlags & 0xfeu);

    // the sub-target map at +0xcc built empty (comparator/allocator
    // bytes, nil node, head node, size 0) -- SubTargets' own constructor in the port.
    Field_0xe0 = 0;
}

FClientEquippedObj::~FClientEquippedObj()
{
    // (this = the CBase sub-object; the offsets below are the gameobj's).
    // every equip-FX wrapper deleted through its vt+4(1), its node erased
    // (the routine) -- the routine's body. The port's wrapper deletion lives in
    // client_equip_fx.cpp.
    EquippedObjClearEquipFx(this);

    // the object narrowed by class & 0x103 (null when it fails; the
    // walk below reads through it with no further test).
    CEqObj* const EqObj = static_cast<CEqObj*>(NarrowObject(this, 0x103u));
    // every fuse of the archetype's fuse list (arch +0x78 list head;
    // the port's Archetype::EqObj keeps a vector* in that list's slot, null for an empty list)
    // looked up in the fuse DB (the routine + the routine) and, when found, Fuse::UnLoad'ed.
    Archetype::EqObj const* const EqArch = static_cast<Archetype::EqObj const*>(EqObj->Archetype);
    if (EqArch->Fuses != nullptr)
    {
        std::vector<Archetype::FGroupFuse> const& ArchFuses = *EqArch->Fuses;
        size_t FuseIndex = 0;
        for (FuseIndex = 0; FuseIndex < ArchFuses.size(); ++FuseIndex)
        {
            ClientFuse* const Proto = ClientFuseDB::Find(ArchFuses[FuseIndex].Fuse.Id);
            if (Proto != nullptr)
                Proto->UnLoad();
        }
    }
    // the same for every archgroup's fuse ignitions --
    // CArchGroupManager::Traverse at CEqObj+0x144 with a fresh traverser, CArchGroup::
    // FuseIgnitions(), each entry found in the fuse DB and UnLoad'ed.
    CArchGrpTraverser GroupCursor;
    CArchGroup* Group = EqObj->ArchGroups.Traverse(GroupCursor);
    while (Group != nullptr)
    {
        Archetype::FuseIgnitionList const& Ignitions = Group->FuseIgnitions();
        if (Ignitions.Items != nullptr)
        {
            std::vector<Archetype::FGroupFuse> const& GroupFuses = *Ignitions.Items;
            size_t GroupFuseIndex = 0;
            for (GroupFuseIndex = 0; GroupFuseIndex < GroupFuses.size(); ++GroupFuseIndex)
            {
                ClientFuse* const Proto = ClientFuseDB::Find(GroupFuses[GroupFuseIndex].Fuse.Id);
                if (Proto != nullptr)
                    Proto->UnLoad();
            }
        }
        Group = EqObj->ArchGroups.Traverse(GroupCursor);
    }

    // the sub-target map at +0xcc emptied (the routine, erase over
    // [begin, end)), then its head and nil nodes freed -- here, in baseline's order; SubTargets'
    // own destructor frees the rest after the body.
    SubTargets.clear();

    // is the fuse list's destructor inlined (the routine over
    // [begin, end), head freed) and 0x00532f5a..00532f8e the equip-FX list's (the routine over
    // [begin, end) -- empty after the clear above -- head freed): ~FEquippedFuseList (+0xc0) and
    // ~FEquipFxList (+0xb4), run as members after ~SubTargets (+0xcc), in that order.
    // the routine -- GameObject's destructor runs next.
}

void FClientEquippedObj::notify(Event Code, void* Param)
{
    // (INotify slot 0 on the +0xac sub-object, RET 8): switch on Code - 5
    // (JA above 3 -> nothing). The numeric codes are Common's; none of 5..8 has a name in
    // INotify::Event yet.
    switch (static_cast<int>(Code))
    {
    case 5:
    {
        // an attached part came off. Its debris object (taken over:
        // RetrieveDebrisObject, before the slot-85 test) goes to the routine with
        // &arch->DebrisType (+0x74) and, unless slot 85 (+0x154) is set,
        // &arch->SeparationExplosion (+0x70). AttachedEquipArch is fetched for +0x70 first.
        CAttachedEquip* const Attached = static_cast<CAttachedEquip*>(Param);
        CObject* const PartObject = Attached->RetrieveDebrisObject();
        if (Slot154())
        {
            SpawnPartDebris(PartObject, &Attached->AttachedEquipArch()->DebrisType, nullptr);
        }
        else
        {
            ID_String const* const Explosion = &Attached->AttachedEquipArch()->SeparationExplosion;
            SpawnPartDebris(PartObject, &Attached->AttachedEquipArch()->DebrisType, Explosion);
        }
        break;
    }
    case 6:
        // the part's effects stop -- the routine(GetRootIndex (vt+0x84),
        // no new owner) on the effect list (+0x28). The 0 pushed before the virtual call is
        // 's second argument.
        CliEffectListDetachPart(&EffectList,
                                static_cast<CAttachedEquip*>(Param)->GetRootIndex(), nullptr);
        break;
    case 7:
    {
        // a CArchGroup was destroyed -- the routine(the destroyed
        // object (RetreiveDestroyedObj, before the slot-85 test), &DebrisTypeName(),
        // slot 85 ? null : &ExplosionTypeName()); ExplosionTypeName is fetched first.
        CArchGroup* const Group = static_cast<CArchGroup*>(Param);
        CObject* const PartObject = Group->RetreiveDestroyedObj();
        if (Slot154())
        {
            SpawnPartDebris(PartObject, &Group->DebrisTypeName(), nullptr);
        }
        else
        {
            ID_String const* const Explosion = &Group->ExplosionTypeName();
            SpawnPartDebris(PartObject, &Group->DebrisTypeName(), Explosion);
        }
        break;
    }
    case 8:
        // the group's effects stop -- the routine(GetRootIndex, 0).
        CliEffectListDetachPart(&EffectList, static_cast<CArchGroup*>(Param)->GetRootIndex(),
                                nullptr);
        break;
    default:
        break;
    }
}

void FClientEquippedObj::SpawnPartDebris(CObject* PartObject, ID_String const* DebrisType,
                                         ID_String const* Explosion)
{
    // (RET 0xc).
    // the [Debris] record; a miss warns (the part archetype's nickname
    // string first, then the type's, then get_id, vt+0x20) and takes the default.
    FDebrisArch* DebrisArch = FindDebrisArch(DebrisType);
    if (DebrisArch == nullptr)
    {
        char const* const ArchName = PartObject->get_archetype()->Nickname.get_string();
        char const* const TypeName = DebrisType->get_string();
        FDUMP(0x100002,
              "%s(%d) : *** WARNING: ID(0x%X) Exploding: Couldn't find debris_type[%s] with "
              "archetype[%s], using default.",
              kEquippedObjSourceFile, 0x1e8, get_id(), TypeName, ArchName);
        DebrisArch = GetDefaultDebrisArch();
    }

    // the Debris gameobj (the routine), and the part's effects move
    // to it (the routine on +0x28 with the part's engine instance, +0x04).
    FClientDebrisGameobj* const Debris = CreateDebrisGameobjForObject(PartObject, DebrisArch);
    CliEffectListDetachPart(&EffectList, PartObject->EngineInstanceId, Debris);

    // a named explosion starts on the Debris gameobj at this Gameobj's
    // position (vt+0, the reference passed as is); an unknown one warns.
    if (Explosion == nullptr || Explosion->IsEmpty())
        return;
    Archetype::Explosion* const Expl = Archetype::GetExplosion(*Explosion);
    if (Expl != nullptr)
    {
        Debris->Actions.add(Action_Explode::Create(Expl, Debris, &get_position()));
        return;
    }
    FDUMP(0x100002, "%s(%d) : *** WARNING: Couldn't find separation_explosion[%s], skipping.",
          kEquippedObjSourceFile, 0x202, Explosion->get_string());
}

void FClientEquippedObj::Slot170()
{
    // ported as EquippedObjRebuildEquipFx (client_equip_fx.cpp).
    EquippedObjRebuildEquipFx(this);
}

void FClientEquippedObj::update(float Dt)
{
    // (RET 4). Callers: slot 93 of every EquippedObj class that does not override
    // it, the ship update the routine (0x00545157) and the solar update (0x005478c6), both
    // called directly.
    // GameObject's update the routine first.
    FClientGameobj::update(Dt);

    // the equip-FX list at +0xb8 (FEquipFxListNode, client_equip_fx.h),
    // the head re-read at every loop test (0x00534076). Each wrapper's slot 2 (+8, Update) gets
    // Dt; one that answers false is deleted through slot 1 (+4) with 1 -- after a null test
    // (0x00534044) -- its node unlinked and freed (the routine, operator delete), and +0xbc
    // decremented; the walk goes on from the old Next.
    FEquipFxListNode* Node = static_cast<FEquipFxListNode*>(EquipFx.Head)->Next;
    while (Node != static_cast<FEquipFxListNode*>(EquipFx.Head))
    {
        if (Node->Wrapper->Update(Dt))
        {
            Node = Node->Next;
            continue;
        }
        delete Node->Wrapper;
        FEquipFxListNode* const Next = Node->Next;
        Node->Prev->Next = Next;
        Next->Prev = Node->Prev;
        ::operator delete(Node);
        EquipFx.Count = EquipFx.Count - 1;
        Node = Next;
    }

    // every burning fuse at +0xc4, Fuse::Burn(Dt) on its ClientFuse
    // (node +8), head to head with no other test.
    FFuseBurnNode* const FuseListHead = static_cast<FFuseBurnNode*>(Fuses.Head);
    for (FFuseBurnNode* Node = FuseListHead->Next; Node != FuseListHead; Node = Node->Next)
        Node->Live.Burn(Dt);
}

unsigned* FClientEquippedObj::Slot17c(unsigned* OutLod, void* Camera, unsigned const* CachedLod)
{
    // (RET 0xc). 0x00533710..0x00533735: the object narrowed to a CEqObj (0x103,
    // else null -- the test of `this` is only the CBase this-adjustment), its +0x140 read with
    // no further test. 0x0053373b..0x00533757: >= 1.0f (FCOMP, TEST AH,1 / JNZ skips below and
    // unordered) -> LOD {1, 0xffff}, i.e. not drawn. Otherwise GameObject's pick the routine,
    // called directly (0x0053376a).
    CEqObj const* const EqObj = static_cast<CEqObj const*>(NarrowObject(this, 0x103u));
    if (EqObj->CloakPercent >= 1.0f)
    {
        *OutLod = 0xffff0001u;
        return OutLod;
    }
    return GameobjPickLodbaseline(this, OutLod, Camera, CachedLod);
}

void FClientEquippedObj::Slot180()
{
    // E9 0B6AEEFF = JMP 0x00401e90 (analysis labels the ICF-folded target
    // IDPMsgHandler::OnDisconnect), which is a bare RET -- the same empty body as GameObject's
    // own slot 96 (the branch).
}

unsigned* FClientEquippedObj::Slot188(unsigned* OutLod, void* Camera, unsigned CachedLod,
                                      void* Matrix, void* Extra)
{
    // the body is EquippedObjSubmitDrawbaseline (space_scene_obj_draw.cpp, W3-B).
    return EquippedObjSubmitDrawbaseline(this, OutLod, Camera, CachedLod, Matrix, Extra);
}

void FClientEquippedObj::Slot198(DamageList const& Damage)
{
    // (RET 4). 0x005339e5..0x00533a02: in multiplayer only, every entry of the
    // list (head's next up to the head) is applied to the object; each arm narrows the object
    // by 0x103 again (null when it is not a CEqObj, used with no test).
    if (!SinglePlayer())
    {
        for (std::list<DamageEntry>::const_iterator It = Damage.Entries.begin();
             It != Damage.Entries.end(); ++It)
        {
            DamageEntry const& Entry = *It;
            if (Entry.SubObjId == SUBOBJ_ID_ROOT)
            {
                // the hull -> vt+0x98 set_hit_pts(HitPts).
                static_cast<CEqObj*>(NarrowObject(this, 0x103u))->set_hit_pts(Entry.HitPts);
            }
            else if (Entry.SubObjId == SUBOBJ_ID_POWER)
            {
                // the power -> the CEqObj's +0x198 directly.
                static_cast<CEqObj*>(NarrowObject(this, 0x103u))->Power = Entry.HitPts;
            }
            else if (SubObjectID::IsEquipID(Entry.SubObjId))
            {
                // the equipment with that id (CEquipManager::FindByID
                // on +0xe4); none -> the next entry.
                CEqObj* const EqObj = static_cast<CEqObj*>(NarrowObject(this, 0x103u));
                CEquip* const Equip = EqObj->EquipManager.FindByID(Entry.SubObjId);
                if (Equip == nullptr)
                    continue;
                if (static_cast<int>(Damage.Cause) == 0x13)
                {
                    // cause 0x13 -> CEShield::ShieldBatteryBoost on
                    // it (no type test).
                    static_cast<CEShield*>(Equip)->ShieldBatteryBoost(Entry.HitPts);
                }
                else if (Equip->Type == 0x10000u &&
                         static_cast<CECargo*>(Equip)->Count > 1u && Entry.HitPts == 0.0f)
                {
                    // cargo (type 0x10000) stacked above one whose
                    // entry is exactly 0 (FCOMP 0.0 / TEST AH,0x44 / JP: only equal falls
                    // through) -> CECargo::RemoveFromStack(1).
                    static_cast<CECargo*>(Equip)->RemoveFromStack(1u);
                }
                else
                {
                    // otherwise vt+0x40 SetHitPoints(HitPts).
                    Equip->SetHitPoints(Entry.HitPts);
                }
            }
            else
            {
                // a collision group -> vt+0xe8
                // set_sub_obj_hit_pts(SubObjId, HitPts).
                static_cast<CEqObj*>(NarrowObject(this, 0x103u))
                    ->set_sub_obj_hit_pts(Entry.SubObjId, Entry.HitPts);
            }
        }
    }

    // vt+0xfc destroy_sub_objs(Damage, true), in single player too.
    static_cast<CEqObj*>(NarrowObject(this, 0x103u))->destroy_sub_objs(Damage, true);
    // CEquipManager::CleanUp on +0xe4.
    static_cast<CEqObj*>(NarrowObject(this, 0x103u))->EquipManager.CleanUp();
}

void FClientEquippedObj::Slot1a0(void const* Contact, void const* B, FClientGameobj* Other,
                                 void const* D)
{
    // (RET 0x10). B is the hit sub-object id (its low word).
    unsigned short const SubObj = static_cast<unsigned short>(reinterpret_cast<uintptr_t>(B));
    // SubObjectID::IsShieldEquipID.
    if (SubObjectID::IsShieldEquipID(SubObj))
    {
        // the body of the routine inlined -- the equipment SubObj of
        // the class 0x103 object (no test before +0xe4), then the first wrapper on the +0xb8
        // list whose +4 is it.
        FClientEquip* const Wrapper = EquippedObjFindEquipFx(this, SubObj);
        // a wrapper whose equipment (no test) has type 0x100 -- a
        // shield.
        if (Wrapper != nullptr && Wrapper->Equip->Type == 0x100u)
        {
            // this object's velocity (vtable +4), Other's velocity
            // (+4) and mass (+0x30) are fetched and dropped -- leftovers of the inlined body.
            (void)get_velocity();
            (void)Other->get_velocity();
            (void)Other->get_mass();
            // the wrapper's vtable +0x14 (Contact, Other, D).
            CliShieldEquipFxCollisionHit(Wrapper, Contact, Other, D);
            return;
        }
    }
    // the routine with the four arguments.
    FClientGameobj::Slot1a0(Contact, B, Other, D);
}

void FClientEquippedObj::Slot1a4(void const* A, void const* B, FClientGameobj* Munition)
{
    // (RET 0xc). B is the hit sub-object id (its low word).
    unsigned short const SubObj = static_cast<unsigned short>(reinterpret_cast<uintptr_t>(B));
    // SubObjectID::IsShieldEquipID.
    if (SubObjectID::IsShieldEquipID(SubObj))
    {
        // the routine inlined, as in slot 104.
        FClientEquip* const Wrapper = EquippedObjFindEquipFx(this, SubObj);
        // a shield wrapper (equipment type 0x100, no test of the
        // equipment) -> its vtable +0x18 (A, Munition).
        if (Wrapper != nullptr && Wrapper->Equip->Type == 0x100u)
        {
            CliShieldEquipFxMunitionHit(Wrapper, A, Munition);
            return;
        }
    }
    // the routine with the three arguments.
    FClientGameobj::Slot1a4(A, B, Munition);
}

void FClientEquippedObj::Slot1a8(unsigned short SubObj, unsigned TargetId, unsigned short TargetSubObj)
{
    // (RET 0xc): the map's operator[] -- lower_bound on SubObj, a {SubObj,
    // {0, SUBOBJ_ID_NONE}} node inserted at the hint when the key is missing (the routine) --
    // then the value overwritten with the two arguments (0x005357ce..0x005357e2).
    FEquipSubTarget& Entry = SubTargets[SubObj];
    Entry.TargetId = TargetId;
    Entry.TargetSubObj = TargetSubObj;
}

void FClientEquippedObj::ApplySubTarget(unsigned short SubObj)
{
    // (RET 4). 0x005357f8..0x00535836: insert {SubObj, {0, SUBOBJ_ID_NONE}}
    // (the routine, no replace), keep the iterator.
    std::pair<std::map<unsigned short, FEquipSubTarget>::iterator, bool> const Inserted =
        SubTargets.insert(std::make_pair(SubObj, FEquipSubTarget()));
    FEquipSubTarget const& Entry = Inserted.first->second;

    // no target id, or no Gameobj for it in the ObjDB
    // (the routine(id, 0)) -> nothing.
    if (Entry.TargetId == 0)
        return;
    FClientGameobj* const Target = ClientObjDb_FindObject(Entry.TargetId, 0);
    if (Target == nullptr)
        return;

    if (SubObj == SUBOBJ_ID_NONE)
    {
        // this object's own target (vt+0x74) and sub-target (vt+0x78);
        // unchanged when it is the same object (vt+0x20 ids) on the same sub-object.
        IObjRW* Current = nullptr;
        unsigned short CurrentSub = SUBOBJ_ID_NONE;
        get_target(Current);
        get_subtarget(CurrentSub);
        if (Current != nullptr)
        {
            unsigned const CurrentId = Current->get_id();
            unsigned const TargetGoId = Target->get_id();
            if (CurrentId == TargetGoId && CurrentSub == Entry.TargetSubObj)
                return;
        }
    }
    else
    {
        // the gun at SubObj (the object narrowed 0x103, no null test;
        // CEquipManager::FindByID, CEGun::cast) -- none, or not a gun -> nothing; unchanged
        // when its target (CEGun::GetTarget) is the Gameobj's object (+0xb0 id against vt+0x20)
        // on the same sub-object.
        CEqObj* const EqObj = static_cast<CEqObj*>(NarrowObject(this, 0x103u));
        CEquip* const Equip = EqObj->EquipManager.FindByID(SubObj);
        if (Equip == nullptr)
            return;
        CEGun* const Gun = CEGun::cast(Equip);
        if (Gun == nullptr)
            return;
        CSimple* GunTarget = nullptr;
        unsigned short GunSub = SUBOBJ_ID_NONE;
        Gun->GetTarget(GunTarget, GunSub);
        if (GunTarget != nullptr)
        {
            unsigned const TargetGoId = Target->get_id();
            if (GunTarget->Id == TargetGoId && GunSub == Entry.TargetSubObj)
                return;
        }
    }

    // / 0x00535959..0x0053596d: IObjAffect (+4) slot 17 (vt+0x44) with
    // (&SubObj, the target Gameobj, the stored sub-object, 0).
    static_cast<IObjAffect*>(this)->slot_44(&SubObj, static_cast<IObjRW*>(Target),
                                            Entry.TargetSubObj, 0);
}

void FClientEquippedObj::Slot1b0(Vector const* Pos)
{
    // (RET 4) -- the explosion of an equipped object.
    // every piece of equipment (CEquipTraverser default ctor,
    // CEquipManager::Traverse on +0xe4 of the object narrowed by 0x103, no null test): an
    // attached one (CAttachedEquip::cast) that is not a shield (type 0x100) gets vt+0x88
    // SetFate(2, separated); every one then gets vt+0x34 Destroy.
    CEquipTraverser Walk;
    for (;;)
    {
        CEqObj* const EqObj = static_cast<CEqObj*>(NarrowObject(this, 0x103u));
        CEquip* const Equip = EqObj->EquipManager.Traverse(Walk);
        if (Equip == nullptr)
            break;
        CAttachedEquip* const Attached = CAttachedEquip::cast(Equip);
        if (Attached != nullptr && Equip->Type != 0x100u)
            Attached->SetFate(DamageEntry::SubObjFate::Separated);
        Equip->Destroy();
    }

    // every collision group (the traverser's start byte set to 1
    // inline, CArchGroupManager::Traverse on +0x144) not already destroyed (State 3) gets
    // CArchGroup::Kill(1).
    CArchGrpTraverser Groups;
    for (;;)
    {
        CEqObj* const EqObj = static_cast<CEqObj*>(NarrowObject(this, 0x103u));
        CArchGroup* const Group = EqObj->ArchGroups.Traverse(Groups);
        if (Group == nullptr)
            break;
        if (Group->State == 3)
            continue;
        Group->Kill(DamageEntry::SubObjFate::Destroyed);
    }

    // CArchGroupManager::DestroyDeadGroups; 0x00533cf8..0x00533cff:
    // GameObject's slot 108 the routine(Pos).
    static_cast<CEqObj*>(NarrowObject(this, 0x103u))->ArchGroups.DestroyDeadGroups();
    FClientGameobj::Slot1b0(Pos);
}

bool FClientEquippedObj::Slot1c0(unsigned SourceId, ID_String const* FuseId,
                                 unsigned short SubObjId, float AtT, float Lifetime)
{
    // (RET 0x14) -- its body is HostIgniteFuse (client_equip_fx.cpp).
    return HostIgniteFuse(this, SourceId, *FuseId, SubObjId, AtT, Lifetime);
}

bool FClientEquippedObj::Slot1c4(ID_String const* FuseId, unsigned short SubObjId, float AtT)
{
    // (RET 0xc). 0x005343d4..0x0053440a: the +0xc4 list from the head's next;
    // the loop top leaves once a fuse was found (BL); a node whose Fuse +4 (the name) equals
    // *FuseId gets Fuse::UnBurn(SubObjId, AtT) and sets BL. Returns BL.
    FFuseBurnNode* const Sentinel = static_cast<FFuseBurnNode*>(Fuses.Head);
    bool bFound = false;
    for (FFuseBurnNode* Node = Sentinel->Next; Node != Sentinel && !bFound; Node = Node->Next)
    {
        if (Node->Live.Name().Id == FuseId->Id)
        {
            bFound = true;
            Node->Live.UnBurn(SubObjId, AtT);
        }
    }
    return bFound;
}

bool FClientEquippedObj::Slot1c8()
{
    // (bare RET). 0x00534423..0x00534449: the +0xc4 list; a node whose Fuse +0xc
    // (bDeathFuse) is set and which Fuse::IsSpent says is not spent -> true at once; the end
    // of the list -> false.
    FFuseBurnNode* const Sentinel = static_cast<FFuseBurnNode*>(Fuses.Head);
    for (FFuseBurnNode* Node = Sentinel->Next; Node != Sentinel; Node = Node->Next)
    {
        if (Node->Live.IsDeathFuse() && !Node->Live.IsSpent())
            return true;
    }
    return false;
}

bool FClientEquippedObj::is_dying() const
{
    // a tail jump through this object's own vtable +0x1c8 (slot 114).
    return const_cast<FClientEquippedObj*>(this)->Slot1c8();
}

int FClientEquippedObj::scan_cargo(IObjRW* Scanned, EquipDescVector& OutCargo) const
{
    // (RET 8). 0x0053565b..0x00535669: IObjInspectImpl::scan_cargo first; its
    // result is returned unchanged.
    int const Result = IObjInspectImpl::scan_cargo(Scanned, OutCargo);
    // only when this is the player's ship (the routine on get_id).
    if (IsPlayerShipId(get_id()))
    {
        // the scanned object's id, this id again, then
        // vt+0x104 (SPScanCargo) with the field.
        unsigned const ScannedId = Scanned->get_id();
        unsigned const SelfId = get_id();
        LocalServer()->SPScanCargo(SelfId, ScannedId, GetPlayerConnectionId());
    }
    return Result;
}

unsigned char FClientEquippedObj::Slot1cc(FCliEquipFirePacket const* Packet)
{
    // ported as EquippedObjHandleEquipMsg (cli_projectile_fx.cpp), which reads
    // the gameobj through its fire-host view (+0x10 object, +0xb8 equip-FX list).
    return EquippedObjHandleEquipMsg(this, Packet);
}

int FClientEquippedObj::slot_34(unsigned short const* EquipIds, unsigned Count,
                                Vector const* TargetPos, int SourceClient)
{
    // ported as CliShipFireWeapons (cli_projectile_fx.cpp), which takes the
    // IObjAffect sub-object (this + 4) as baseline's adjustor this.
    return static_cast<int>(CliShipFireWeapons(static_cast<IObjAffect*>(this), EquipIds, Count,
                                               TargetPos, SourceClient));
}

namespace
{
    // The tractor slots' target narrowing: IObjInspect slot 84 cobject, kept only when its class
    // carries all of 0x803 (CLoot), null otherwise (0x00534f08..0x00534f28 / 0x005350c4..0x005350e3).
    CLoot* TractorLootOf(IObjRW* Target)
    {
        CObject* const Obj = const_cast<CObject*>(Target->cobject());
        if (Obj == nullptr || (static_cast<unsigned>(Obj->ObjectClass) & 0x803u) != 0x803u)
            return nullptr;
        return static_cast<CLoot*>(Obj);
    }

    // The IObjAffect slots' "local request" test (0x00534e81..0x00534e9b): SourceClient -1, or
    // the player's own connection (the routine = the field).
    bool IsLocalRequest(int SourceClient)
    {
        return SourceClient == -1 ||
               static_cast<unsigned>(SourceClient) == GetPlayerConnectionId();
    }
}

int FClientEquippedObj::slot_3c(unsigned short const* EquipId,
                                std::vector<IObjRW*> const& Targets, int SourceClient)
{
    // (thiscall on the IObjAffect sub-object, RET 0xc) -- listing
    // (raw decode; analysis left the body as data).
    // local request; single player refuses a remote one with -5.
    const bool bLocal = IsLocalRequest(SourceClient);
    if (SinglePlayer() && !bLocal)
        return -5;

    // the object narrowed by 0x103 (no null test before the +0xe4
    // LEA), CEquipManager::FindByID(*EquipId); none -> -2.
    CEqObj* const EqObj = static_cast<CEqObj*>(NarrowObject(this, 0x103u));
    CEquip* const Equip = EqObj->EquipManager.FindByID(*EquipId);
    if (Equip == nullptr)
        return -2;

    // every target (not null-tested) through CETractor::AddTarget on
    // the equipment as found -- no cast, no destroyed test; a 0 (TractorOk) counts.
    CETractor* const Tractor = static_cast<CETractor*>(Equip);
    int Accepted = 0;
    for (std::vector<IObjRW*>::const_iterator It = Targets.begin(); It != Targets.end(); ++It)
    {
        if (Tractor->AddTarget(TractorLootOf(*It)) == TractorOk)
            Accepted = Accepted + 1;
    }
    // none accepted (or no targets) -> -4 (signed JLE).
    if (Accepted <= 0)
        return -4;

    // a remote request stops here with 0.
    if (!bLocal)
        return 0;

    // XTractorObjects {the gameobj's get_id (vt+0x20), *EquipId, the
    // get_id of EVERY target, accepted or not} to the field slot 5 TractorObjects
    // (the field, &Info), no null test; the id vector's buffer freed after.
    XTractorObjects Info;
    Info.ObjectId = get_id();
    Info.TractorEquipId = *EquipId;
    Info.Pad_0x06 = 0;
    for (std::vector<IObjRW*>::const_iterator It = Targets.begin(); It != Targets.end(); ++It)
        Info.TargetIds.push_back((*It)->get_id());
    LocalServer()->TractorObjects(GetPlayerConnectionId(), Info);
    return 0;
}

int FClientEquippedObj::slot_40(unsigned short const* EquipId, IObjRW* Target, int SourceClient)
{
    // (thiscall on the IObjAffect sub-object, RET 0xc) -- 0x00534e60..0x00534fe6,
    // raw decode (analysis left it uninspected). The single-target form of slot 15.
    // local request; single player refuses a remote one with -5.
    const bool bLocal = IsLocalRequest(SourceClient);
    if (SinglePlayer() && !bLocal)
        return -5;

    // no target -> -4.
    if (Target == nullptr)
        return -4;

    // the object narrowed by 0x103, FindByID(*EquipId); none -> -2.
    CEqObj* const EqObj = static_cast<CEqObj*>(NarrowObject(this, 0x103u));
    CEquip* const Equip = EqObj->EquipManager.FindByID(*EquipId);
    if (Equip == nullptr)
        return -2;

    // CETractor::AddTarget on the equipment as found; any failure
    // code -> -5.
    if (static_cast<CETractor*>(Equip)->AddTarget(TractorLootOf(Target)) != TractorOk)
        return -5;

    // a remote request stops here with 0.
    if (!bLocal)
        return 0;

    // XTractorObjects {get_id (vt+0x20), *EquipId, [Target's get_id]}
    // to the field slot 5 TractorObjects(the field, &Info); the buffer freed after.
    XTractorObjects Info;
    Info.ObjectId = get_id();
    Info.TractorEquipId = *EquipId;
    Info.Pad_0x06 = 0;
    Info.TargetIds.push_back(Target->get_id());
    LocalServer()->TractorObjects(GetPlayerConnectionId(), Info);
    return 0;
}

int FClientEquippedObj::slot_44(unsigned short const* EquipId, IObjRW* Target,
                                unsigned short SubTarget, int SourceClient)
{
    // ported as CliEquippedObjSetGunTarget (cli_projectile_fx.cpp).
    return CliEquippedObjSetGunTarget(static_cast<IObjAffect*>(this), EquipId, Target, SubTarget,
                                      SourceClient);
}

int FClientEquippedObj::slot_4c(unsigned short const* EquipId, bool bActivate, int SourceClient)
{
    // ported as CliShipActivateEquip (cli_projectile_fx.cpp); its value is
    // baseline's EAX (0 on every path, 0x00535459).
    return CliShipActivateEquip(static_cast<IObjAffect*>(this), EquipId,
                                static_cast<unsigned char>(bActivate), SourceClient);
}

int FClientEquippedObj::slot_58(unsigned short EquipId, unsigned Count, int SourceClient)
{
    // (thiscall on the IObjAffect sub-object, RET 0xc) -- listing
    // use a repair kit / shield battery from the cargo hold.
    // only a local request (SourceClient -1 or the field) goes on;
    // anything else -> -5. There is no single-player test here.
    if (!IsLocalRequest(SourceClient))
        return -5;

    // the object narrowed by 0x103 (no null test before the +0xe4
    // LEA), CEquipManager::FindByID(EquipId), CECargo::cast of the result.
    CEqObj* const EqObj = static_cast<CEqObj*>(NarrowObject(this, 0x103u));
    CECargo* const Cargo = CECargo::cast(EqObj->EquipManager.FindByID(EquipId));
    if (Cargo == nullptr)
    {
        // EquippedObj.cpp line 0x540, then -2.
        FDUMP(0x100002, "%s(%d) : *** WARNING: %s",
              "Client\\Space\\EquippedObj.cpp", 0x540,
              "EqObj::use_item() couldn't find target item on Client request.");
        return -2;
    }

    // the amount -- the stack's count (+0x28) or Count, the smaller
    // (unsigned JB).
    const unsigned UseCount = (Cargo->Count < Count) ? Cargo->Count : Count;

    // the cargo archetype's class type (+0xc, Root slot 4).
    const Archetype::AClassType ClassType = Cargo->Arch->get_class_type();
    if (ClassType == Archetype::AClassRepairKit)
    {
        // a repair kit only while the hull is below full --
        // CSimple::get_relative_health() < 1.0 (TEST AH,5 / JP: equal, above or unordered
        // skip), on the object narrowed again (no null test).
        CEqObj* const Hull = static_cast<CEqObj*>(NarrowObject(this, 0x103u));
        if (!(Hull->get_relative_health() < 1.0f))
            return 0;
    }
    else if (ClassType == Archetype::AClassShieldBattery)
    {
        // a shield battery only while some shield (CEquipTraverser
        // 0x100) is below full and has hit points at all: vt+0x44 GetRelativeHealth < 1.0
        // (TEST AH,5 / JP) and vt+0x38 GetMaxHitPoints > 0.0 (TEST AH,0x41 / JNZ). The object is
        // re-narrowed on every step, as baseline does.
        CEquipTraverser Walk(static_cast<int>(EquipTypeShield));
        for (;;)
        {
            CEqObj* const Owner = static_cast<CEqObj*>(NarrowObject(this, 0x103u));
            CEquip* const Shield = Owner->EquipManager.Traverse(Walk);
            if (Shield == nullptr)
                return 0;
            if (!(Shield->GetRelativeHealth() < 1.0f))
                continue;
            if (Shield->GetMaxHitPoints() > 0.0f)
                break;
        }
    }
    else
    {
        // -> 0x0053562e: any other cargo -> 0.
        return 0;
    }

    // SSPUseItem {get_id (vt+0x20), EquipId, the amount} to
    // slot 56 SPRequestUseItem(&Info, the field), no null test; 0.
    SSPUseItem Info;
    Info.ObjectId = get_id();
    Info.ItemId = EquipId;
    Info.Count = static_cast<unsigned short>(UseCount);
    LocalServer()->SPRequestUseItem(Info, GetPlayerConnectionId());
    return 0;
}

// ===========================================================================
// Solar
// ===========================================================================

FClientSolarGameobj::FClientSolarGameobj()
    : FClientEquippedObj(EClientGameobjKind::Solar)
{
    // EquippedObj's ctor (kind 2), then its own members. +0xf0..+0x1ef are not
    // written.
    Field_0xe4 = 0;
    DockScript.Length = 0;
    DockScript.Data[0] = '\0';
    Field_0x1f0 = 1;
    BurnColor[0] = 1.0f;
    BurnColor[1] = 1.0f;
    BurnColor[2] = 1.0f;
    Field_0x200.clear();
    Field_0x204 = nullptr;
}

FClientSolarGameobj::~FClientSolarGameobj()
{
    // .
    // the JumpGateEffect at +0x204, when set -- its destructor
    // (0x005476d3) and the global operator delete the routine. The field is not
    // written back.
    if (Field_0x204 != nullptr)
        delete static_cast<FJumpGateEffect*>(Field_0x204);
    // with an object, the CEqObj (0x103) gets vt+0x94
    // disconnect(IObjDB* = &the field) and vt+0xbc disconnect(INotify* = this+0xac); the
    // narrowing has no null test on the result.
    if (Object != nullptr)
    {
        CEqObj* const EqObj = static_cast<CEqObj*>(NarrowObject(this, 0x103u));
        EqObj->disconnect(GetClientObjDb());
        static_cast<CEqObj*>(NarrowObject(this, 0x103u))->disconnect(static_cast<INotify*>(this));
    }
    // the routine, EquippedObj's destructor, runs next.
}

// FClientSolarGameobj::notify (the routine) is defined after the ship's, next to the
// createeffect helpers both use.

void FClientSolarGameobj::create(Archetype::Root* Arch)
{
    // .
    CreateOpenConnect(this, CObject::ClassSolar, Arch);
}

bool FClientSolarGameobj::find(unsigned const& Id)
{
    // .
    return FindAndConnect(this, CObject::ClassSolar, Id);
}

void FClientSolarGameobj::Slot170()
{
    // (bare RET). 0x00547853: EquippedObj's slot 92 (the routine), called
    // directly; 0x00547858..0x0054786d: the nebula at get_position (vt+0) into +0x38 and its
    // fog factor into +0x3c (the routine, no hint).
    FClientEquippedObj::Slot170();
    FNebulaEffect* const Nebula =
        StarSysFindNebula(&get_position(), reinterpret_cast<float*>(&DrawStateB), nullptr);
    DrawStateA = static_cast<unsigned>(reinterpret_cast<uintptr_t>(Nebula));
}

void FClientSolarGameobj::update(float Dt)
{
    // (RET 4). 0x00547884..0x005478c0: in multiplayer only, the CSolar (0x303,
    // narrowed with no test of the result) runs the dumb CObject::update (vt+0x50).
    if (!SinglePlayer())
        NarrowObject(this, 0x303u)->update(Dt, CObject::UPDATE_DUMB);

    // EquippedObj's update, called directly.
    FClientEquippedObj::update(Dt);

    // the jump gate effect, when there is one (the routine).
    if (Field_0x204 != nullptr)
        static_cast<FJumpGateEffect*>(Field_0x204)->Update(Dt);

    // the star, when there is one, through its slot 2 (+8,
    // , RET 4).
    if (Field_0xe4 != nullptr)
    {
        typedef void(__thiscall * FStarUpdate)(FStarSphere*, float);
        Field_0xe4->UpdateIntensity(Dt);
    }
}

namespace
{
    // solar.cpp's 3DMathEngine, acquired (the routine) on first use; its only
    // site is the radius slot 0x005481bd..0x005481cb.
    I3DMathEngine* G_SolarMath = nullptr;
}

float FClientSolarGameobj::Slot178(Vector& Center)
{
    // (RET 4) -- the solar's radius about Center.
    // with a star (+0xe4): Center = the star owner's position
    // (the routine), the radius is the star's glow extent (the routine).
    if (Field_0xe4 != nullptr)
    {
        Vector StarPos;
        StarSphereGetOwnerPosition(Field_0xe4, &StarPos);
        Center = StarPos;
        return StarSphereMaxGlowScale(Field_0xe4);
    }

    // a planet / star / moon type (CSolar TypeBits +0xe0 & 7; the
    // object narrowed by 0x303, no test) -> the engine instance's extent: IEngine vt+0x88
    // GetInstanceExtent(get_index, 0, &Radius, &Center) with Radius zeroed first, then Center
    // taken to world space by the object's transform (vt+0x10) through solar.cpp's math
    // engine slot 10 TransformXform into a local, copied back.
    CSimple* const Simple = static_cast<CSimple*>(NarrowObject(this, 0x303u));
    if ((Simple->TypeBits & 7u) != 0)
    {
        float Radius = 0.0f;
        long const Inst = get_index();
        NeuronRuntime::Engine->GetInstanceExtent(reinterpret_cast<void*>(static_cast<intptr_t>(Inst)), 0u,
                                         &Radius, &Center.x);
        FTransform const& Xform = reinterpret_cast<FTransform const&>(get_orientation());
        if (G_SolarMath == nullptr)
            G_SolarMath = Acquire3DMathEngine();
        FVector World;
        G_SolarMath->TransformXform(World, Xform, reinterpret_cast<FVector const&>(Center));
        Center = reinterpret_cast<Vector const&>(World);
        return Radius;
    }

    // GameObject's slot 94 (the routine).
    return FClientGameobj::Slot178(Center);
}

unsigned* FClientSolarGameobj::Slot188(unsigned* OutLod, void* Camera, unsigned CachedLod,
                                       void* Matrix, void* Extra)
{
    // (RET 0x14) -- the solar draw. 0x00548227..0x00548233: the local LOD starts
    // {0, 0xffff}, and every path that draws nothing returns it (0x005484a9).
    unsigned Lod = 0xffff0000u;
    CSimple* const Simple = static_cast<CSimple*>(NarrowObject(this, 0x303u));

    // a star -> only when option 0x24 is on: the sphere's slot 3
    // (DrawSimple, +0xc) with the camera, and {4, 0xffff}.
    if (Field_0xe4 != nullptr)
    {
        if (GetGameOption(0x24) == 0)
        {
            *OutLod = Lod;
            return OutLod;
        }
        typedef void(__thiscall * FStarDrawSimple)(FStarSphere*, void*);
        Field_0xe4->DrawSimple(Camera);
        *OutLod = 0xffff0004u;
        return OutLod;
    }

    unsigned const TypeBits = Simple->TypeBits;
    bool bSubmit = false;
    if ((TypeBits & 7u) != 0)
    {
        // planet / star / moon -> drawn only with option 0x23 on.
        if (GetGameOption(0x23) == 0)
        {
            *OutLod = Lod;
            return OutLod;
        }
        bSubmit = true;
#ifdef OUTPOST_FRONTIER_DX9RENDERER
        // Port-only: the far bucket draws planets without depth; the Direct3D 9 volumetric
        // nebula ends its rays on their bounding sphere instead (nebula_volume.cpp). Only while
        // it is active.
        if (NebulaVolume::IsActive())
        {
            Vector Centre;
            const float Radius = Slot178(Centre);
            NebulaVolume::NoteOpaqueSphere(&Centre.x, Radius);
        }
#endif
    }
    else if ((TypeBits & 8u) != 0)
    {
        // bit 3 -> nothing.
        *OutLod = Lod;
        return OutLod;
    }
    else if (((TypeBits >> 9) & 1u) == 0)
    {
        // not a waypoint solar (bit 9) -> the ordinary draw.
        bSubmit = true;
    }

    if (bSubmit)
    {
        // EquippedObj's draw the routine into a temporary (the
        // Camera argument's stack slot), copied to *OutLod.
        unsigned Temp = 0;
        unsigned* const Result =
            EquippedObjSubmitDrawbaseline(this, &Temp, Camera, CachedLod, Matrix, Extra);
        *OutLod = *Result;
        return OutLod;
    }

    // the waypoint marker -- only while the software cursor is drawn
    // (the routine) and the HUD_Waypoint (the field) exists and is active (the routine);
    // its fade (the routine) is fetched before the active test.
    if (GetClientMouseCursorDrawFlag() == 0)
    {
        *OutLod = Lod;
        return OutLod;
    }
    HUD_Waypoint* const Waypoint = GetHudWaypoint();
    if (Waypoint == nullptr)
    {
        *OutLod = Lod;
        return OutLod;
    }
    bool const bActive = GetHudWaypoint()->IsActive();
    float const Fade = GetHudWaypoint()->GetScale();
    if (!bActive)
    {
        *OutLod = Lod;
        return OutLod;
    }

    // the colour is the palette entry the field and the size 1.0;
    // with a player ship, a distance above 2500 (the field; FCOM / TEST AH,0x41 / JNZ)
    // gives size = distance * 0.0005 (the field), capped at 99 (the field -> 0x42c60000).
    unsigned long Color = G_NnPaletteArgb_679b80;
    float Size = 1.0f;
    IObjRW* const Player = static_cast<IObjRW*>(GetPlayerObjRW());
    if (Player != nullptr)
    {
        Vector const& PlayerPos = Player->get_position();
        Vector const& Pos = get_position();
        double const Dx = static_cast<double>(Pos.x) - PlayerPos.x;
        double const Dy = static_cast<double>(Pos.y) - PlayerPos.y;
        double const Dz = static_cast<double>(Pos.z) - PlayerPos.z;
        double const Distance = std::sqrt((Dy * Dy + Dx * Dx) + Dz * Dz);
        if (Distance > 2500.0)
        {
            double const Scaled = Distance * static_cast<double>(0.0005f);
            Size = static_cast<float>(Scaled);
            if (Scaled > 99.0)
                Size = 99.0f;
        }
    }

    // the colour's alpha byte scaled by the fade (FILD * float, ftol);
    // (Camera, get_index, 1, &colour, 0, size, 0, 0) -- its answer is the new LOD
    // {answer, 0xffff}.
    unsigned char ColorBytes[4];
    std::memcpy(ColorBytes, &Color, sizeof(ColorBytes));
    ColorBytes[3] = static_cast<unsigned char>(
        static_cast<long>(static_cast<double>(ColorBytes[3]) * Fade));
    long const Inst = get_index();
    int const Answer = SubmitWin3DMaterialEngineInstance(
        Camera, reinterpret_cast<void*>(static_cast<intptr_t>(Inst)), 1, ColorBytes, 0, Size, 0,
        0u);
    Lod = 0xffff0000u | static_cast<unsigned short>(Answer);

    // an answer below 3 (unsigned) or a solar that is no longer a
    // waypoint (bit 9, re-read) returns that LOD.
    if (static_cast<unsigned short>(Answer) < 3 ||
        ((static_cast<CSimple*>(NarrowObject(this, 0x303u))->TypeBits >> 9) & 1u) == 0)
    {
        *OutLod = Lod;
        return OutLod;
    }

    // the outline pass -- {0xff, 0x80, 0xff, ftol(fade * 255)}
    // (the field) through the routine(Camera, get_index, 1, &colour, 0, size, 0).
    unsigned char Outline[4];
    Outline[0] = 0xff;
    Outline[1] = 0x80;
    Outline[2] = 0xff;
    Outline[3] = static_cast<unsigned char>(static_cast<long>(static_cast<double>(Fade) * 255.0f));
    long const OutlineInst = get_index();
    SubmitWin3DTintedEngineInstanceWrapped(
        Camera, reinterpret_cast<void*>(static_cast<intptr_t>(OutlineInst)), 1, Outline, 0, Size,
        0u);
    *OutLod = Lod;
    return OutLod;
}

void FClientSolarGameobj::Slot1a0(void const* Contact, void const* B, FClientGameobj* Other,
                                  void const* D)
{
    // (RET 0x10): a solar of type bits 0..3 (TypeBits +0xe0 & 0xf; the object
    // narrowed by 0x303, no test) ignores the hit; any other goes on to EquippedObj's slot 104
    // (a tail JMP to the routine with the arguments unchanged).
    if ((static_cast<CSimple*>(NarrowObject(this, 0x303u))->TypeBits & 0xfu) != 0)
        return;
    FClientEquippedObj::Slot1a0(Contact, B, Other, D);
}

void FClientSolarGameobj::Slot1a4(void const* A, void const* B, FClientGameobj* Munition)
{
    // (RET 0xc): the same test, then a tail JMP to the routine.
    if ((static_cast<CSimple*>(NarrowObject(this, 0x303u))->TypeBits & 0xfu) != 0)
        return;
    FClientEquippedObj::Slot1a4(A, B, Munition);
}

void FClientSolarGameobj::Slot1b0(Vector const* Pos)
{
    // (RET 4): a bare JMP to EquippedObj's slot 108 (the routine).
    FClientEquippedObj::Slot1b0(Pos);
}

void FClientSolarGameobj::ReadIni(INI_Reader& Reader)
{
    // (thiscall, RET 4).
    // the EquippedObj body the routine, called directly (not the solar's own
    // slot 92).
    FClientEquippedObj::Slot170();

    // every value of the (rewound) section; the first key that matches
    // wins, unknown keys are skipped.
    while (Reader.read_value())
    {
        if (Reader.is_value("ring"))
        {
            // value 0 into a TString<0x20> (the zone), value 1 into a
            // TString<0x104> (the ini), both through the routine (a null value leaves them
            // empty). A new TexturePanels (the routine) loads (ini, zone) through its slot 1;
            // nothing keeps the pointer but the panels' global list.
            TString<0x20> Zone;
            Zone = Reader.get_value_string(0);
            TString<0x104> File;
            File = Reader.get_value_string(1);
            TexturePanelsCreate()->Load(File.Data, Zone.Data);
        }
        else if (Reader.is_value("burn_color"))
        {
            // baseline tests "burn_color" a second time (always true) and
            // reads value 1, value 0, value 2; each / 255 (the field) into +0x1f4.
            float const Green = Reader.get_value_float(1);
            float const Red = Reader.get_value_float(0);
            float const Blue = Reader.get_value_float(2);
            float const kInv255 = 1.0f / 255.0f; // (0x3b808081)
            BurnColor[0] = Red * kInv255;
            BurnColor[1] = Green * kInv255;
            BurnColor[2] = Blue * kInv255;
        }
        else if (Reader.is_value("dock_script"))
        {
            // the inlined TString<0x104> assign into +0xe8 (a null value
            // gives length 0 and an empty string; otherwise clamped to 0x103).
            DockScript = Reader.get_value_string();
        }
        else if (Reader.is_value("voice"))
        {
            // the object narrowed to CSolar (0x303) -- baseline writes
            // through the result with no null test -- gets +0x13c = CreateID(value).
            char const* const Voice = Reader.get_value_string();
            CSolar* const Solar = static_cast<CSolar*>(NarrowObject(this, 0x303u));
            Solar->VoiceId = CreateID(Voice);
        }
        else if (Reader.is_value("jump_effect"))
        {
            // +0x200 = CreateID(value).
            Field_0x200.Id = CreateID(Reader.get_value_string());
        }
        else if (Reader.is_value("star"))
        {
            // +0xe4 = the routine(this) (a listed star), then its slot 1
            // Load(GetFileSysPath("solar\\stararch.ini", NeuronRuntime::Data), value).
            Field_0xe4 = StarSphereCreateListed(this);
            char Path[0x104];
            GetFileSysPath(Path, "solar\\stararch.ini", NeuronRuntime::Data);
            using FStarLoad = int(__thiscall*)(FStarSphere*, char const*, char const*);
            Field_0xe4->Load(Path, Reader.get_value_string());
        }
    }

    // the nebula at the solar's position into +0x38, its fog factor
    // into +0x3c (the routine, no hint).
    FNebulaEffect* const Nebula =
        StarSysFindNebula(&get_position(), reinterpret_cast<float*>(&DrawStateB), nullptr);
    DrawStateA = static_cast<unsigned>(reinterpret_cast<uintptr_t>(Nebula));
}

// ===========================================================================
// Ship
// ===========================================================================

FClientShipGameobj::FClientShipGameobj()
    : FClientEquippedObj(EClientGameobjKind::Ship)
{
    // EquippedObj's ctor (kind 1), then its own members in store order.
    Field_0xe4 = 0;
    Field_0xe8 = 0;
    Field_0xec = nullptr;
    Field_0xf0 = nullptr;
    Field_0xf4[0] = nullptr;
    Field_0xf4[1] = nullptr;
    Field_0xf4[2] = nullptr;
    Field_0xf4[3] = nullptr;
    Field_0x104 = nullptr;
    MeshHolderConstruct(&Field_0x108);
    Field_0x118.Rotation = Matrix(1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    Field_0x118.Translation.x = 0.0f;                                          // ..3e
    Field_0x118.Translation.y = 0.0f;
    Field_0x118.Translation.z = 0.0f;
    Field_0x148 = nullptr;
    Field_0x14c = nullptr;
    Field_0x150 = 0;
    Field_0x154 = 0;
    Field_0x15c = 0;
    // +0x160 .. +0x184 zeroed, one dword at a time.
    Field_0x160 = 0;
    int CompanionSlot = 0;
    for (CompanionSlot = 0; CompanionSlot < 4; ++CompanionSlot)
        Field_0x164[CompanionSlot] = nullptr;
    for (CompanionSlot = 0; CompanionSlot < 4; ++CompanionSlot)
        Field_0x174[CompanionSlot] = nullptr;
    Field_0x184 = 0;
    Field_0x18c = static_cast<unsigned char>(Field_0x18c & 0xfcu);
    Field_0x188 = nullptr;
    Field_0x18c = 0;
    Pad18d[0] = 0;
    Pad18d[1] = 0;
    Pad18d[2] = 0;
    Field_0x158 = 0;
    // +0x34 = rand() * (1 / 32768) (the field).
    Field_0x34 = static_cast<float>(static_cast<double>(rand()) * 3.0517578125e-05);
}

FClientShipGameobj::~FClientShipGameobj()
{
    // . It runs with ESI = this + 0xc (the CBase sub-object: [ESI+0x4] is the
    // object at +0x10), so every ESI-relative offset in the listing is 0xc below the gameobj
    // offset used here.
    // the routine on the mesh holder (+0x108).
    ClientMeshHolderRelease(&Field_0x108);
    // the archetype at +0x104, Archetype::Root::Release.
    if (Field_0x104 != nullptr)
    {
        Field_0x104->Release();
        Field_0x104 = nullptr;
    }
    // +0x148's vt+0x10, the pointer left as it is.
    if (Field_0x148 != nullptr)
    {
        Field_0x148->Release();
    }
    // the two sounds (+0x14c, +0xf0) -- vt+0x60 stop, vt+8 release.
    if (Field_0x14c != nullptr)
    {
        Field_0x14c->Stop();
        Field_0x14c->Release();
        Field_0x14c = nullptr;
    }
    if (Field_0xf0 != nullptr)
    {
        Field_0xf0->Stop();
        Field_0xf0->Release();
        Field_0xf0 = nullptr;
    }
    // the 0xdc-byte jump effect at +0x188 -- when set, its destructor
    // (0x0054456b), the global operator delete the routine, and the field zeroed.
    if (Field_0x188 != nullptr)
    {
        delete static_cast<FJumpShipEffect*>(Field_0x188);
        Field_0x188 = nullptr;
    }
    // with an object -- in single player only for a ship the local
    // player owns (the class & 3 object's +0xb4 OwnerPlayer, the routine = the field when
    // the narrow fails, against the routine), in multiplayer always -- the CEqObj (0x103, no
    // null test) gets vt+0x94 disconnect(IObjDB* = &the field); then, always, vt+0xbc
    // disconnect(INotify* = this + 0xac).
    if (Object != nullptr)
    {
        bool bDisconnectDb = true;
        if (SinglePlayer())
        {
            CObject* const Owned = NarrowObject(this, 0x3u);
            unsigned const Owner = (Owned != nullptr)
                                       ? static_cast<CSimple const*>(Owned)->OwnerPlayer
                                       : GetPlayerConnectionId();
            bDisconnectDb = Owner == GetPlayerConnectionId();
        }
        if (bDisconnectDb)
            static_cast<CEqObj*>(NarrowObject(this, 0x103u))->disconnect(GetClientObjDb());
        static_cast<CEqObj*>(NarrowObject(this, 0x103u))->disconnect(static_cast<INotify*>(this));
    }
    // the +0x160 member's destructor (the routine, inlined) --
    // companion +0x174, then companion +0x164, each: particle (the routine), sound (vt+8),
    // light (the routine), arch slot zeroed -- the routine's sequence.
    CliEffectCompanionClear(Field_0x174);
    CliEffectCompanionClear(Field_0x164);
    // the routine on +0x108 only restores the holder's vptr.
    // the companion at +0xf4 -- particle +0xfc, sound +0xf8, light
    // +0x100, then +0xf4 zeroed.
    CliEffectCompanionClear(Field_0xf4);
    // the routine, EquippedObj's destructor, runs next.
}

namespace
{
    char const kCreateEffectSourceFile[] = "Client\\space\\createeffect.h";

    // EffectArch's constructor / destructor (the routine / the routine), used for the stack
    // archetypes the notify handlers build, live with the registry in vis_effect_arch.cpp.
    // The stack archetype is EH state 0 of those frames (the routine: [ESP+0xf8] = 0 at
    // right after the routine, -1 at 0x005459aa before the routine), so its
    // destructor also runs on the unwind path: hence a scoped object.
    struct FScopedEffectArch
    {
        FScopedEffectArch()
        {
            EffectArchConstruct(&Arch);
        }
        ~FScopedEffectArch()
        {
            EffectArchDestruct(&Arch);
        }
        FEffectArch Arch;

    private:
        FScopedEffectArch(FScopedEffectArch const&) = delete;
        FScopedEffectArch& operator=(FScopedEffectArch const&) = delete;
    };

    // The createeffect.h companion {Arch, sound, particle, light} (the routine), EH state 1
    // of the same frames (the routine: byte [ESP+0xf8] = 1 at 0x005458b1, back to 0 at
    // before the routine): cleared by the routine on both paths.
    struct FScopedEffectCompanion
    {
        explicit FScopedEffectCompanion(FEffectArch* Arch)
        {
            Slots[0] = Arch;
            Slots[1] = nullptr;
            Slots[2] = nullptr;
            Slots[3] = nullptr;
        }
        ~FScopedEffectCompanion()
        {
            CliEffectCompanionClear(Slots);
        }
        void* Slots[4];

    private:
        FScopedEffectCompanion(FScopedEffectCompanion const&) = delete;
        FScopedEffectCompanion& operator=(FScopedEffectCompanion const&) = delete;
    };

    // (createeffect.h; thiscall on the effect factory the field, which it does
    // not read; RET 8) -- start whatever the companion holds with Arch, drop what will not
    // start, and report whether anything is still alive.
    bool CreateEffectStart(void* Companion16, FEffectArch* Arch)
    {
        void** const Companion = static_cast<void**>(Companion16);

        // the particle's slot 0 (Start); a refusal releases it.
        FParticleEffect* const Particle = static_cast<FParticleEffect*>(Companion[2]);
        if (Particle != nullptr && ParticleEffectStart(Particle, Arch) == 0)
        {
            ParticleEffectRelease(Particle);
            Companion[2] = nullptr;
        }

        // looping from bit 0 of +0x50, then Start(SndEffect, 0); no
        // sound archetype or a refusal releases the source.
        ISoundSource* const Sound = static_cast<ISoundSource*>(Companion[1]);
        if (Sound != nullptr)
        {
            Sound->SetLooping(static_cast<unsigned char>(Arch->Flags50 & 1u));
            if (Arch->SndEffect == nullptr || Sound->Start(Arch->SndEffect, 0) == 0)
            {
                Sound->Release();
                Companion[1] = nullptr;
            }
        }

        // type 1 with bit 0, colour (+0x4c), radius (+0x48), then
        // Enable (vt+4). Failure warns (line 0xce) and releases; otherwise a named animation
        // (+0x1c length, +0x20 name) is attached -- the lookup result is not null-tested.
        FGameLight* const Light = static_cast<FGameLight*>(Companion[3]);
        if (Light != nullptr)
        {
            EngineLightSetType((FEngineLight*)Light, 1, true);
            EngineLightSetColorBytesAlt((FEngineLight*)Light, &Arch->LgtColorR);
            EngineLightSetRadius((FEngineLight*)Light, Arch->LgtRadius);
            if (!GameLightEnable((FEngineLight*)Light))
            {
                ID_String Name;
                Name.Id = Arch->NicknameId;
                FDUMP(0x100002, "%s(%d) : *** WARNING: couldnt open light",
                      kCreateEffectSourceFile, 0xce, Name.get_string());
                GameLightRelease(Light);
                Companion[3] = nullptr;
            }
            else if (Arch->LgtEffectLen != 0)
            {
                GameLightAttachAnim(Light, GameLightFindAnimByName(Arch->LgtEffect),
                                    Arch->LgtRangeScale, Arch->LgtTimeScale, 0.0f);
            }
        }

        // alive -- a particle without the deactivated bit, a sound
        // whose IsActive says so, or a light with no animation keys.
        FParticleEffect* const LiveParticle = static_cast<FParticleEffect*>(Companion[2]);
        if (LiveParticle != nullptr && (LiveParticle->Flags08 & 2u) == 0u)
            return true;
        ISoundSource* const LiveSound = static_cast<ISoundSource*>(Companion[1]);
        if (LiveSound != nullptr && LiveSound->IsActive() != 0)
            return true;
        FGameLight* const LiveLight = static_cast<FGameLight*>(Companion[3]);
        if (LiveLight != nullptr && LiveLight->Anim.Keys == nullptr)
            return true;
        return false;
    }

    // The createeffect.h sequence the notify handlers inline (ship 0x00545883..0x005459a3,
    // solar 0x00547abd..0x00547baa / 0x00547cd5..0x00547dc4 / 0x00547f72..0x00548060). Only
    // with EFFECTS on: the companion {Arch, 0, 0, 0} (the routine; the archetype is not
    // AddRef'd), the factory's (the field) particle / sound / light as the archetype asks,
    // attached at EngineIndex with Pose (the routine / sound vt+0x94 / the routine -- out of
    // line as the routine in the solar copies), started (the routine), and on success copied
    // into Go's effect list (the routine) -- slot 0 there keeps the address of the caller's
    // stack archetype, exactly as baseline's does. The local companion is cleared
    // (the routine) either way.
    void CreateEffectOnGameobj(FClientGameobj* Go, FEffectArch* Arch, long EngineIndex,
                               float const* Pose)
    {
        if (GetGameOption(kGameOptEffectsGroup) == 0)
            return;

        FScopedEffectCompanion Scoped(Arch); // ; the routine at scope exit
        void** const Companion = Scoped.Slots;
        if (GetGameOption(kGameOptParticles) != 0 && Arch->VisEffect != nullptr &&
            Arch->VisEffect->Nickname[0] != '\0')
            Companion[2] = ParticleEffectCreate();
        if (Arch->SndEffect != nullptr)
            Companion[1] = CreateClientSoundSource3D();
        if (Arch->LgtRadius > 0.0f)
            Companion[3] = GameLightConstruct(static_cast<FGameLight*>(GameLightHeapAlloc(0x12c)));

        if (Companion[2] != nullptr)
            ParticleEffectAttach(static_cast<FParticleEffect*>(Companion[2]), Go, EngineIndex,
                                 reinterpret_cast<FTransform const*>(Pose));
        if (Companion[1] != nullptr)
            static_cast<ISoundSource*>(Companion[1])->SetPose(EngineIndex, Pose);
        if (Companion[3] != nullptr)
            CliExplosionAttachLocalXform(Companion[3], EngineIndex,
                                         reinterpret_cast<FTransform const*>(Pose));

        if (CreateEffectStart(Companion, Arch))
            CliEffectListInsert(&Go->EffectList, Companion);
    }

    // case 2 (0x005456ad..0x005459be): the bay doors' sound at the hull's
    // HpBaySurface hardpoint -- bay_doors_open_snd when the event's byte is set, else
    // bay_doors_close_snd. It is the createeffect.h sequence inlined on a stack EffectArch
    // that carries only the sound.
    void ShipBayDoorSound(FClientShipGameobj* Go, bool bOpen)
    {
        // The CShip narrowing (class & 0x503) is not null-tested before +0x88 is read.
        CObject* const Obj = Go->Object;
        CShip* const Ship = (Obj != nullptr &&
                             (static_cast<unsigned>(Obj->ObjectClass) & 0x503u) == 0x503u)
                                ? static_cast<CShip*>(Obj)
                                : nullptr;
        Archetype::Ship const* const ShipArch =
            static_cast<Archetype::Ship const*>(Ship->Archetype);

        // the hardpoint in the object's own frame; an empty name or a
        // miss leaves position zero and orientation identity (the CRC word is not written).
        HardpointInfo Hp;
        long HpHandle = 0;
        char const* const HpName = ShipArch->HpBaySurface.Value;
        if (HpName[0] == '\0' || !FindHardpoint_PS(Go->get_index(), HpName, HpHandle, Hp))
        {
            Hp.Position.x = 0.0f;
            Hp.Position.y = 0.0f;
            Hp.Position.z = 0.0f;
            Hp.Orient.E[0][0] = 1.0f;
            Hp.Orient.E[0][1] = 0.0f;
            Hp.Orient.E[0][2] = 0.0f;
            Hp.Orient.E[1][0] = 0.0f;
            Hp.Orient.E[1][1] = 1.0f;
            Hp.Orient.E[1][2] = 0.0f;
            Hp.Orient.E[2][0] = 0.0f;
            Hp.Orient.E[2][1] = 0.0f;
            Hp.Orient.E[2][2] = 1.0f;
        }

        // the stack EffectArch, its sound looked up (the routine) from
        // the CliShip companion's +0x58 / +0x5c id.
        FScopedEffectArch Scoped; // ; the routine at scope exit
        FEffectArch& Arch = Scoped.Arch;
        ID_String const* const SoundId = Archetype::CliShipBayDoorSound(ShipArch->Companion, bOpen);
        Arch.SndEffect = LookupClientSoundArch(SoundId);

        // the pose handed to the effects -- the 3x3 then the position.
        float Pose[12];
        Pose[0] = Hp.Orient.E[0][0];
        Pose[1] = Hp.Orient.E[0][1];
        Pose[2] = Hp.Orient.E[0][2];
        Pose[3] = Hp.Orient.E[1][0];
        Pose[4] = Hp.Orient.E[1][1];
        Pose[5] = Hp.Orient.E[1][2];
        Pose[6] = Hp.Orient.E[2][0];
        Pose[7] = Hp.Orient.E[2][1];
        Pose[8] = Hp.Orient.E[2][2];
        Pose[9] = Hp.Position.x;
        Pose[10] = Hp.Position.y;
        Pose[11] = Hp.Position.z;
        long const EngineIndex = Go->get_index();

        // ; the routine (0x005459b5) is Scoped's destructor.
        CreateEffectOnGameobj(Go, &Arch, EngineIndex, Pose);
    }
}

void FClientShipGameobj::PlayCargoJettisonSound()
{
    // (thiscall, bare RET) -- the bay-door sequence of the routine case 2 with
    // SND_CARGO_JETTISONED as the sound.
    // an empty SND_CARGO_JETTISONED (the Common export) ends here.
    if (SND_CARGO_JETTISONED.IsEmpty())
        return;

    // the CShip (class & 0x503; `this` and +0xc tested, the result not)
    // archetype's +0xe0 HpBaySurface, found in the object's own frame (FindHardpoint_PS on
    // get_index); 0x0054400d..0x0054407d: an empty name or a miss leaves position zero and the
    // orientation identity.
    CObject* const Obj = Object;
    CShip* const Ship =
        (Obj != nullptr && (static_cast<unsigned>(Obj->ObjectClass) & 0x503u) == 0x503u)
            ? static_cast<CShip*>(Obj)
            : nullptr;
    Archetype::Ship const* const ShipArch = static_cast<Archetype::Ship const*>(Ship->Archetype);
    HardpointInfo Hp;
    long HpHandle = 0;
    char const* const HpName = ShipArch->HpBaySurface.Value;
    if (HpName[0] == '\0' || !FindHardpoint_PS(get_index(), HpName, HpHandle, Hp))
    {
        Hp.Position.x = 0.0f;
        Hp.Position.y = 0.0f;
        Hp.Position.z = 0.0f;
        Hp.Orient = Matrix(1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    }

    // the stack EffectArch (the routine) with its sound looked up
    // (the routine) from SND_CARGO_JETTISONED.
    FScopedEffectArch Scoped; // ; the routine at scope exit
    FEffectArch& Arch = Scoped.Arch;
    Arch.SndEffect = LookupClientSoundArch(&SND_CARGO_JETTISONED);

    // the pose -- the 3x3, then the position.
    float Pose[12];
    Pose[0] = Hp.Orient.E[0][0];
    Pose[1] = Hp.Orient.E[0][1];
    Pose[2] = Hp.Orient.E[0][2];
    Pose[3] = Hp.Orient.E[1][0];
    Pose[4] = Hp.Orient.E[1][1];
    Pose[5] = Hp.Orient.E[1][2];
    Pose[6] = Hp.Orient.E[2][0];
    Pose[7] = Hp.Orient.E[2][1];
    Pose[8] = Hp.Orient.E[2][2];
    Pose[9] = Hp.Position.x;
    Pose[10] = Hp.Position.y;
    Pose[11] = Hp.Position.z;
    // get_index.
    long const EngineIndex = get_index();

    // the createeffect.h sequence (EFFECTS gate, factory, attach,
    // , the routine, the routine); 0x00544211: the routine.
    CreateEffectOnGameobj(this, &Arch, EngineIndex, Pose);
}

// The three NN voice lines the ship notify queues (static-init CreateID entries).
unsigned G_ShipVoiceCargoExpiredId = CreateID("cargo_expired");     // , the branch
unsigned G_ShipVoiceWeaponExpiredId = CreateID("weapon_expired");   // , the branch
unsigned G_ShipVoiceEngineCriticalId = CreateID("engine_critical"); // , the branch
// , the branch -- queued by slot 102 (the branch, 0x0054602b) below 20% hull.
unsigned G_ShipVoiceHullBreachId = CreateID("hull_breach_imminent");
// The request-answer and IObjAffect slot 27 voice lines (static-init CreateID entries; readers
// and the routine).
unsigned G_ShipVoiceTradeLaneDisruptedId = CreateID("nnvoice_trade_lane_disrupted"); // , the branch
unsigned G_ShipVoiceDockGrantedId = CreateID("dock_granted");               // , the branch
unsigned G_ShipVoiceDockDeniedHostileId = CreateID("dock_denied_hostile");  // , the branch
unsigned G_ShipVoiceDockDeniedLockedId = CreateID("dock_denied_locked");    // , the branch
unsigned G_ShipVoiceCannotDockId = CreateID("cannot_dock");                 // , the branch
unsigned G_ShipVoiceWaitingToDockId = CreateID("waiting_to_dock");          // , the branch
unsigned G_ShipVoiceDockTooFarId = CreateID("nnv_dock_too_far");            // , the branch
unsigned G_ShipVoiceFormationLostId = CreateID("nnv_formation_lost");       // , the branch

void FClientShipGameobj::notify(Event Code, void* Param)
{
    // (INotify slot 0 on the +0xac sub-object, RET 8): switch on Code - 2 (JA
    // above 7 -> straight to the tail), then always EquippedObj's notify (0x00545a4d).
    switch (static_cast<int>(Code))
    {
    case 2:
        ShipBayDoorSound(this, *static_cast<unsigned char const*>(Param) != 0);
        break;
    case 3:
        // the player's ship only -- the voice line "weapon_expired"
        // (the field) through the routine(id, 0), the NN voice queue.
        if (static_cast<void*>(static_cast<IObjRW*>(this)) == GetPlayerObjRW())
            NnVoiceEnqueue(G_ShipVoiceWeaponExpiredId, 0);
        break;
    case 4:
        // the same with "cargo_expired" (the field).
        if (static_cast<void*>(static_cast<IObjRW*>(this)) == GetPlayerObjRW())
            NnVoiceEnqueue(G_ShipVoiceCargoExpiredId, 0);
        break;
    case 9:
    {
        // the player's ship, a CArchGroup whose collision group type
        // has a bit of 0x3f00 and whose relative health is below 0.05 (the field; FCOMP
        // double / TEST AH,0x5 / JP: only C0 alone goes on, so equal and NaN stop) --
        // "engine_critical" (the field).
        if (static_cast<void*>(static_cast<IObjRW*>(this)) != GetPlayerObjRW())
            break;
        CArchGroup* const Group = static_cast<CArchGroup*>(Param);
        if (Group == nullptr)
            break;
        if ((Group->GetCollisionGroupType() & 0x3f00u) == 0u)
            break;
        if (!(static_cast<double>(Group->GetRelativeHealth()) < 0.05))
            break;
        NnVoiceEnqueue(G_ShipVoiceEngineCriticalId, 0);
        break;
    }
    default:
        break;
    }
    FClientEquippedObj::notify(Code, Param);
}

namespace
{
    // codes 0 and 1: the solar's open_sound (CliSolar +0x54) or close_sound
    // (+0x58) at the root, pose identity -- the same createeffect.h sequence as the ship's bay
    // doors, with the routine doing the attach.
    void SolarDoorSound(FClientSolarGameobj* Go, bool bOpen)
    {
        // The archetype is read through the 0x303 narrowing without a null test.
        CObject* const Obj = Go->Object;
        CObject* const Solar = (Obj != nullptr &&
                                (static_cast<unsigned>(Obj->ObjectClass) & 0x303u) == 0x303u)
                                   ? Obj
                                   : nullptr;
        FScopedEffectArch Scoped; // ; the routine at scope exit
        FEffectArch& Arch = Scoped.Arch;
        ID_String const* const SoundId =
            Archetype::CliSolarDoorSound(Solar->Archetype->Companion, bOpen);
        Arch.SndEffect = LookupClientSoundArch(SoundId);

        float Pose[12] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f};
        long const EngineIndex = Go->get_index();
        CreateEffectOnGameobj(Go, &Arch, EngineIndex, Pose);
    }
}

void FClientSolarGameobj::notify(Event Code, void* Param)
{
    // (INotify slot 0 on the +0xac sub-object, RET 8). Codes 0, 1 and 0x11; every
    // path ends in EquippedObj's notify (0x00548092).
    int const Kind = static_cast<int>(Code);
    if (Kind == 0)
    {
        // when the solar archetype's +0x81 byte is set, the open or
        // close sound by the event's byte.
        CObject* const Obj = Object;
        CObject* const Solar = (Obj != nullptr &&
                                (static_cast<unsigned>(Obj->ObjectClass) & 0x303u) == 0x303u)
                                   ? Obj
                                   : nullptr;
        Archetype::EqObj const* const SolarArch =
            static_cast<Archetype::EqObj const*>(Solar->Archetype);
        if (SolarArch->bDockingCamera)
            SolarDoorSound(this, *static_cast<unsigned char const*>(Param) != 0);
    }
    else if (Kind == 1)
    {
        // a sub-code in the first dword of Param -- 0, 2 and 3 (1 does nothing).
        // The "has a jump gate effect" test is TypeBits (slot 111) & 0x40, else & 0x800 -- two
        // calls, as baseline makes them.
        int const Sub = *static_cast<int const*>(Param);
        if (Sub == 0)
        {
            // open sound, then, if there is none, the JumpGateEffect at
            // +0x204 -- operator new(0xe0) and the routine(this) (0x00547e1f); a failed
            // allocation stores null.
            SolarDoorSound(this, true);
            if ((Slot1bc() & 0x40u) != 0u || (Slot1bc() & 0x800u) != 0u)
            {
                if (Field_0x204 == nullptr)
                    Field_0x204 = new (std::nothrow) FJumpGateEffect(this);
            }
        }
        else if (Sub == 2)
        {
            // close sound, then the routine on the effect, when there is
            // one (0x00547bda: its +0x18 fire flag = 1).
            SolarDoorSound(this, false);
            if ((Slot1bc() & 0x40u) != 0u || (Slot1bc() & 0x800u) != 0u)
            {
                if (Field_0x204 != nullptr)
                    JumpGateEffectFire(static_cast<FJumpGateEffect*>(Field_0x204));
            }
        }
        else if (Sub == 3)
        {
            // destroy the effect, when there is one (the routine at
            // + operator delete), and clear +0x204 either way.
            if ((Slot1bc() & 0x40u) != 0u || (Slot1bc() & 0x800u) != 0u)
            {
                if (Field_0x204 != nullptr)
                    delete static_cast<FJumpGateEffect*>(Field_0x204);
                Field_0x204 = nullptr;
            }
        }
    }
    else if (Kind == 0x11)
    {
        // a solar with TypeBits bit 7 -- the routine(this, event byte)
        // switches its trade lane ring and the result lands in +0x1f0 (0x00547981).
        CObject* const Obj = Object;
        CSimple const* const Simple =
            (Obj != nullptr && (static_cast<unsigned>(Obj->ObjectClass) & 0x303u) == 0x303u)
                ? static_cast<CSimple const*>(Obj)
                : nullptr;
        if ((Simple->TypeBits & 0x80u) != 0u)
            Field_0x1f0 =
                TradeLaneSolarLaneSwitch(this, *static_cast<unsigned char const*>(Param));
    }
    FClientEquippedObj::notify(Code, Param);
}

void FClientShipGameobj::create(Archetype::Root* Arch)
{
    // .
    CreateOpenConnect(this, CObject::ClassShip, Arch);
}

bool FClientShipGameobj::find(unsigned const& Id)
{
    // .
    return FindAndConnect(this, CObject::ClassShip, Id);
}

void FClientShipGameobj::Slot170()
{
    // (RET 0).
    FClientEquippedObj::Slot170();
    Field_0x18c = static_cast<unsigned char>(Field_0x18c & 0xfcu);
    // the jump state back to 0 (the routine).
    SetJumpState(0);

    // the pilot mesh named by the ship companion's pilot_mesh (+0x64),
    // through the CShip's archetype (+0x88) and its companion (+0x04). The CShip is narrowed
    // (0x503) with no null test of the result.
    CShip* const Ship = static_cast<CShip*>(NarrowObject(this, CObject::ClassShip));
    AttachPilotMesh(reinterpret_cast<ID_String const*>(
        reinterpret_cast<unsigned char const*>(Ship->Archetype->Companion) + 0x64));

    // the local player's ship (the CShip's +0xb4 against the field)
    // gets its behaviour interface for the "Player" state graph.
    if (Ship->OwnerPlayer == GetPlayerConnectionId())
    {
        int const GraphId = pub::StateGraph::get_state_graph("Player", pub::StateGraph::LEADER);
        Ship->create_behavior_interface(static_cast<IObjRW*>(this), GraphId);
    }

    // collisions back on unless the ship is launching or jumping.
    if (!Ship->is_launching() && !Ship->is_jumping())
        Ship->enable_collisions_r(true);
}

void FClientShipGameobj::AttachPilotMesh(ID_String const* PilotMesh)
{
    // (RET 4). 0x00544392..0x0054439c: an empty name does nothing.
    if (PilotMesh->IsEmpty())
        return;

    // "HpPilot" on the ship's own instance (vt+0x1c get_index); no
    // hardpoint, nothing.
    long Handle = 0;
    HardpointInfo PilotInfo;
    if (!FindHardpoint_PS(get_index(), "HpPilot", Handle, PilotInfo))
        return;

    // the simple archetype PilotMesh names, kept at +0x104.
    Field_0x104 = Archetype::GetSimple(PilotMesh->Id);
    if (Field_0x104 == nullptr)
        return;
    // its load (vt+0xc), then its companion's Load(0) (vt+0x20).
    Field_0x104->load();
    Field_0x104->Companion->Load(0);
    // an engine instance of it (no parent); -1 stops here, anything
    // else goes to +0x10c (the mesh holder's instance).
    long const Instance = Field_0x104->create_instance(nullptr);
    if (Instance == -1)
        return;
    Field_0x108.EngineInstance = reinterpret_cast<void*>(static_cast<intptr_t>(Instance));

    // "HpConnect" on the pilot mesh, into the same handle; the result
    // is not tested. baseline reads its stack HardpointInfo whether or not the lookup filled it;
    // the port starts it zeroed so a miss reads zeros instead of stale stack.
    HardpointInfo ConnectInfo = {};
    FindHardpoint_PS(Instance, "HpConnect", Handle, ConnectInfo);

    // +0x118 = {HpPilot's orientation, HpPilot's position minus
    // HpConnect's position} (twelve dwords, MOVSD.REP). Each difference is an x87 FSUB stored
    // to a float.
    Field_0x118.Rotation = PilotInfo.Orient;
    Field_0x118.Translation.x = PilotInfo.Position.x - ConnectInfo.Position.x;
    Field_0x118.Translation.y = PilotInfo.Position.y - ConnectInfo.Position.y;
    Field_0x118.Translation.z = PilotInfo.Position.z - ConnectInfo.Position.z;
}

IObjRW* FClientShipGameobj::PickNextTarget(int ConnectionId)
{
    // (RET 4). ConnectionId is pushed by the caller and never read.
    (void)ConnectionId;
    // two (distance, object) bests, the distances at 1.0e9f
    // (0x4e6e6b28): all candidates and the watch-listed ones.
    IObjRW* Nearest = nullptr;
    IObjRW* NearestListed = nullptr;
    float NearestDist = 1.0e9f;
    float NearestListedDist = 1.0e9f;

    // this object through cobject() (vt+0x150), narrowed to a CShip
    // ((class & 0x503) == 0x503) or null; no null test before the equip manager (+0xe4).
    CObject* Obj = const_cast<CObject*>(cobject());
    if (Obj == nullptr || (static_cast<unsigned>(Obj->ObjectClass) & 0x503u) != 0x503u)
        Obj = nullptr;
    CShip* const Ship = static_cast<CShip*>(Obj);

    // the scanner (type 0x80000); none, or an empty list -> Nearest (null).
    CEScanner* const Scanner = static_cast<CEScanner*>(Ship->EquipManager.FindFirst(0x80000u));
    if (Scanner == nullptr)
        return Nearest;

    for (int I = 0; I < Scanner->Scans.Count; ++I)
    {
        // the watched Watchable, back to its IObjRW (minus 8).
        IObjRW* const Other =
            static_cast<IObjRW*>(const_cast<Watchable*>(Scanner->Scans.Entries[I].target()));
        if (Other == nullptr)
            continue;
        // CShip::is_enemy, then the object's is_dying (vt+0x38).
        bool bListed = false;
        if (!Ship->is_enemy(Other))
            continue;
        if (Other->is_dying())
            continue;
        // a CSimple ((class & 3) == 3) counts only with TypeBits &
        // 0x7f0000 or when its gameobj (the routine(get_id, 0)) is watch-listed (+0xa8).
        CObject const* const OtherObj = Other->cobject();
        if (OtherObj != nullptr && (static_cast<unsigned>(OtherObj->ObjectClass) & 3u) == 3u)
        {
            FClientGameobj* const OtherGo = ClientObjDb_FindObject(Other->get_id(), 0);
            if (OtherGo != nullptr)
                bListed = OtherGo->bWatchListed != 0;
            if ((static_cast<CSimple const*>(OtherObj)->TypeBits & 0x7f0000u) == 0 && !bListed)
                continue;
        }
        // distance from this object's position (vt+0) to the other's.
        Vector const& Mine = get_position();
        Vector const& Theirs = Other->get_position();
        // x87 retains the differences and distance until the best-distance FST dword.
        double const Dx = static_cast<double>(Theirs.x) - Mine.x;
        double const Dy = static_cast<double>(Theirs.y) - Mine.y;
        double const Dz = static_cast<double>(Theirs.z) - Mine.z;
        double const Dist = std::sqrt(Dx * Dx + Dz * Dz + Dy * Dy);
        // TEST AH,0x5 + JP skips greater/equal/unordered;
        // less falls through and stores the new best distance as float.
        if (Dist < NearestDist)
        {
            NearestDist = static_cast<float>(Dist);
            Nearest = Other;
        }
        if (bListed && Dist < NearestListedDist)
        {
            NearestListedDist = static_cast<float>(Dist);
            NearestListed = Other;
        }
    }

    // the watch-listed best when there is one.
    if (NearestListed != nullptr)
        return NearestListed;
    return Nearest;
}

void FClientShipGameobj::SetTargetWithGuns(IObjRW* Target, unsigned short SubTarget)
{
    // (RET 8). 0x00544ba1..0x00544bb2: IObjAffect vt+0x48 on this object's
    // IObjAffect face with the player's connection id.
    static_cast<IObjAffect*>(this)->set_target(Target, SubTarget,
                                               static_cast<int>(GetPlayerConnectionId()));

    // every gun (CEquipTraverser(0x80)); the CShip is narrowed again on
    // each pass and its equip manager (+0xe4) used with no null test. Each gun's id word (+8)
    // goes to the routine -- FClientEquippedObj::slot_44, a direct (non-virtual) call.
    CEquipTraverser Walk(0x80);
    for (;;)
    {
        CShip* const Ship = static_cast<CShip*>(NarrowObject(this, CObject::ClassShip));
        CEquip* const Gun = Ship->EquipManager.Traverse(Walk);
        if (Gun == nullptr)
            break;
        unsigned short const GunId = Gun->Id;
        FClientEquippedObj::slot_44(&GunId, Target, SubTarget,
                                    static_cast<int>(GetPlayerConnectionId()));
    }
}

bool FClientShipGameobj::RequestTractor(IObjRW* Target)
{
    // (RET 4). 0x00544c30..0x00544c6e: the CShip's tractor (type 0x100000); none
    // -> false.
    CShip* const Ship = static_cast<CShip*>(NarrowObject(this, CObject::ClassShip));
    CEquip* const Tractor = Ship->EquipManager.FindFirst(0x100000u);
    if (Tractor == nullptr)
        return false;

    unsigned short const TractorId = Tractor->Id;
    IObjAffect* const Affect = static_cast<IObjAffect*>(this);
    // a Target and a tractor that is not destroyed (CEquip vt+4): a
    // CLoot target ((class & 0x803) == 0x803) gets vt+0x40 (id, Target, connection); any
    // other target nothing.
    if (Target != nullptr && !Tractor->IsDestroyed())
    {
        CObject const* const TargetObj = Target->cobject();
        if (TargetObj != nullptr &&
            (static_cast<unsigned>(TargetObj->ObjectClass) & 0x803u) == 0x803u)
        {
            Affect->slot_40(&TractorId, Target, static_cast<int>(GetPlayerConnectionId()));
        }
        return true;
    }
    // otherwise vt+0x4c (id, false, connection).
    Affect->slot_4c(&TractorId, false, static_cast<int>(GetPlayerConnectionId()));
    return true;
}

// / the field / the field (.data, read only by the ship update): the
// "fade the tunnel out" latch (initially set) and the two fade times.
unsigned char G_bJumpTunnelFadeOutPending = 1;
float G_JumpTunnelFadeOutTime = 1.5f;
float G_JumpTunnelFadeInTime = 1.5f;

// The three NN voice lines of the atmosphere / corona warning (static-init CreateID entries).
unsigned G_ShipVoiceDangerId = CreateID("danger");                           // , the branch
unsigned G_ShipVoiceEnteringSunCoronaId = CreateID("entering_sun_corona");   // , the branch
unsigned G_ShipVoiceEnteringAtmosphereId = CreateID("entering_atmosphere");  // , the branch

void FClientShipGameobj::update(float Dt)
{
    // (RET 4). 0x00545056..0x00545060: nothing for a gameobj slot 86 marks as
    // reaped.
    if (Slot158())
        return;

    // the CShip (0x503), kept for the whole function with no null
    // test of it anywhere below.
    CShip* const Ship = static_cast<CShip*>(NarrowObject(this, CObject::ClassShip));

    // the player's ship follows the DISABLE_COLLIDE option (0x37):
    // the value is latched on first use (the field, guard bit 0 of the field) and, when
    // the option changes, collisions go on for 0 and off otherwise.
    if (static_cast<void*>(static_cast<IObjRW*>(this)) == GetPlayerObjRW())
    {
        static int SDisableCollide = GetGameOption(kGameOptDisableCollide);
        if (SDisableCollide != GetGameOption(kGameOptDisableCollide))
        {
            SDisableCollide = GetGameOption(kGameOptDisableCollide);
            Ship->enable_collisions_r(SDisableCollide == 0);
        }
    }

    // the object's owner (class & 3 -> +0xb4, else the field).
    // The local player's objects run the full CObject::update; others, in multiplayer only,
    // the dumb one; in single player a foreign object is not updated here at all.
    CObject* const OwnerObject = NarrowObject(this, 0x3u);
    unsigned const Owner = (OwnerObject != nullptr)
                               ? static_cast<CSimple const*>(OwnerObject)->OwnerPlayer
                               : GetPlayerConnectionId();
    if (Owner == GetPlayerConnectionId())
        Ship->update(Dt, CObject::UPDATE_DEFAULT);
    else if (!SinglePlayer())
        Ship->update(Dt, CObject::UPDATE_DUMB);

    // EquippedObj's update, called directly.
    FClientEquippedObj::update(Dt);

    // the +0xe4 countdown. Only while it is non-zero (TEST AH,0x44 /
    // JNP: equal skips, NaN goes on) it runs down by Dt (FST keeps the x87 value for the
    // compare); at or below 0 (TEST AH,0x41 / JP: greater and NaN skip) close_bay runs +0xe8
    // times, the CShip narrowed again each time, and +0xe4 returns to 0.
    if (Field_0xe4 != 0.0f)
    {
        double const Remaining = static_cast<double>(Field_0xe4) - static_cast<double>(Dt);
        Field_0xe4 = static_cast<float>(Remaining);
        if (Remaining <= 0.0)
        {
            while (Field_0xe8 > 0)
            {
                static_cast<CShip*>(NarrowObject(this, CObject::ClassShip))->close_bay();
                Field_0xe8 = Field_0xe8 - 1;
            }
            Field_0xe4 = 0.0f;
        }
    }

    // the jump, while the state is non-zero and there is an effect.
    if (Field_0x158 != 0 && Field_0x188 != nullptr)
    {
        FJumpShipEffect* const JumpFx = static_cast<FJumpShipEffect*>(Field_0x188);
        // the step is Dt, or 0.1f (0x3dcccccd) when Dt is above
        // (0.1f; TEST AH,0x41 / JNZ keeps Dt for not-above and NaN); the effect's
        // clock advances by it.
        float JumpDt = Dt;
        if (Dt > 0.1f)
            JumpDt = 0.1f;
        JumpFx->Field_0x70 = JumpFx->Field_0x70 + JumpDt;

        // in the tunnel (state 2), once, when no more than 1.5 s of
        // JumpOutTunnelTime remain (TEST AH,0x41 / JP: greater and NaN skip), the screen fades
        // out to white (the routine).
        if (G_bJumpTunnelFadeOutPending != 0 && Field_0x158 == 2 &&
            static_cast<double>(JumpFx->JumpOutTunnelTime) -
                    static_cast<double>(JumpFx->Field_0x70) <=
                static_cast<double>(G_JumpTunnelFadeOutTime))
        {
            G_bJumpTunnelFadeOutPending = 0;
            float const White[3] = {1.0f, 1.0f, 1.0f};
            FadeOutColoured(White, G_JumpTunnelFadeOutTime);
        }

        // each state moves on once its time is below the clock
        // (FCOMP + TEST AH,0x5 / JP: only "less" goes on).
        switch (Field_0x158)
        {
        case 1:
            if (JumpFx->JumpOutTime < JumpFx->Field_0x70)
                SetJumpState(2);
            break;
        case 2:
            // the fade latch re-armed, the switch requested, and the
            // screen faded back in from white (the routine, starting opaque).
            if (JumpFx->JumpOutTunnelTime < JumpFx->Field_0x70)
            {
                G_bJumpTunnelFadeOutPending = 1;
                SetJumpState(3);
                float const White[3] = {1.0f, 1.0f, 1.0f};
                FadeInColoured(White, G_JumpTunnelFadeInTime, true);
            }
            break;
        case 3:
            // only once the jump in was ordered (+0x15c).
            if (JumpFx->JumpInTunnelTime < JumpFx->Field_0x70 && Field_0x15c != 0)
                SetJumpState(4);
            break;
        case 4:
            if (JumpFx->JumpInTime < JumpFx->Field_0x70)
                SetJumpState(5);
            break;
        case 5:
            // done once the clock is above 1.0f (TEST AH,0x41 / JNZ).
            if (JumpFx->Field_0x70 > 1.0f)
                SetJumpState(0);
            break;
        default:
            break;
        }

        // the effect, when it survived the switch (state 0 frees it),
        // gets the same clamped step (the routine).
        if (Field_0x188 != nullptr)
            static_cast<FJumpShipEffect*>(Field_0x188)->Update(JumpDt);
    }

    // PhySys's collision state of the object (+0x10, not narrowed).
    bool const bCollides = PhySys::GetCollisionState(Object);
    // The CSolar the ship is inside, which CShip::update keeps at +0x218, and its type bits
    // (+0xe0: 2 planet, 4 sun).
    CSolar* const Atmosphere = Ship->AtmosphereSolar;

    // the burn companion at +0xf4 is alive when its particle is not
    // finished (+8 bit 1 clear), its sound is active or its light's +0xd4 is 0 -- the
    // test, inlined here.
    if (CreateEffectCompanionAlive(Field_0xf4) != 0)
    {
        // it leaves the effect list (the routine) once the ship stops
        // colliding, leaves the atmosphere or is landing (+0x18c bit 3).
        if (!bCollides || Atmosphere == nullptr || (Field_0x18c & 0x8u) != 0)
            CliEffectListRemove(reinterpret_cast<unsigned char*>(this) + 0x28, Field_0xf4);
    }
    else if (bCollides && Atmosphere != nullptr && (Field_0x18c & 0x8u) == 0)
    {
        // "atmosphere_burn" started on the ship's own instance with an
        // identity transform, through the field, into the companion and the effect list.
        ID_String BurnEffect;
        BurnEffect.Id = CreateID("atmosphere_burn");
        FTransform Local;
        for (int Row = 0; Row < 3; ++Row)
        {
            for (int Col = 0; Col < 3; ++Col)
                Local.Rotation.D[Row][Col] = (Row == Col) ? 1.0f : 0.0f;
        }
        Local.Translation.X = 0.0f;
        Local.Translation.Y = 0.0f;
        Local.Translation.Z = 0.0f;
        int const Index = static_cast<int>(get_index());
        if (!BurnEffect.IsEmpty())
            CliCreateEffectOneShot(FindEffectArch(&BurnEffect.Id), this, Index, &Local,
                                   Field_0xf4);

        // the player hears "danger" and "entering_sun_corona" in a sun
        // (bit 2), else "danger" and "entering_atmosphere" in a planet (bit 1), through the NN
        // voice queue (the routine(id, 0)).
        if (static_cast<void*>(static_cast<IObjRW*>(this)) == GetPlayerObjRW())
        {
            unsigned const TypeBits = Atmosphere->TypeBits;
            if ((TypeBits & 0x4u) != 0)
            {
                NnVoiceEnqueue(G_ShipVoiceDangerId, 0);
                NnVoiceEnqueue(G_ShipVoiceEnteringSunCoronaId, 0);
            }
            else if ((TypeBits & 0x2u) != 0)
            {
                NnVoiceEnqueue(G_ShipVoiceDangerId, 0);
                NnVoiceEnqueue(G_ShipVoiceEnteringAtmosphereId, 0);
            }
        }
    }

    // the thruster sound at +0xf0 -- created (the routine(0x50) +
    // (this), class CliThrusterSound, vtable 005ddbfc) the first time the CShip
    // carries a thruster (CEquipManager::FindFirst(0x400)), a failed allocation storing null;
    // then, while set, ticked (the routine(Dt)) every update. Both live in menu_sound_arch.cpp.
    if (Field_0xf0 == nullptr && Ship->EquipManager.FindFirst(EquipTypeThruster) != nullptr)
        Field_0xf0 = CreateClientThrusterSound(this);
    if (Field_0xf0 != nullptr)
        TickClientThrusterSound(Field_0xf0, Dt);

    // +0x150 runs down by Dt while above 0 (TEST AH,0x41 / JNZ).
    if (Field_0x150 > 0.0f)
        Field_0x150 = Field_0x150 - Dt;
}

void FClientShipGameobj::Slot184(void* Camera)
{
    // (RET 4). 0x00546486..0x005464a1: only the player's ship in jump states 2
    // or 3 (the tunnel) lights itself; everything else is GameObject's slot 97 (the routine).
    if ((Field_0x158 == 2 || Field_0x158 == 3) && this == GetPlayerObjRW())
    {
        // the routine -- every registered light off and unlinked.
        EngineLightClearRegisteredList();
        // no jump effect -> done.
        FJumpShipEffect* const Effect = static_cast<FJumpShipEffect*>(Field_0x188);
        if (Effect == nullptr)
            return;
        // its jump_ambient bytes (the routine = [+0x1c]), when set,
        // into D3DRS_AMBIENT (the routine).
        if (Effect->Ambient != nullptr)
            EngineLightSetAmbientRgbBytes(Effect->Ambient);
        // no tunnel (+0x24) -> done.
        if (Effect->Tunnel == nullptr)
            return;
        // the ship's position (vt+0) copied into a local.
        Vector Pos = get_position();
        // the tunnel's light (+0x2a8); none -> done.
        FGameLight* const Light = Effect->Tunnel->Light;
        if (Light == nullptr)
            return;
        // the routine on the light -- its position is the ship's.
        EngineLightSetPosition((FEngineLight*)Light, &Pos.x);
        // the routine(&light, 1, Camera, &Pos).
        FEngineLight* const Lights[1] = {(FEngineLight*)Light};
        EngineLightApplyList(Lights, 1u, static_cast<ICamera*>(Camera), &Pos.x);
        return;
    }
    FClientGameobj::Slot184(Camera);
}

namespace
{
    // (cdecl) -- NeuronRuntime::Engine QI for IFLAppearance into a zeroed local (a smart
    // pointer: EH frame the branch); QI < 0 releases a non-null local and returns false;
    // otherwise Slot4 (+0x10) (Id, Out), true when it returned >= 0, then the release.
    bool GetAppearanceInstanceValue(int Id, void** Out)
    {
        IFLAppearance* Ifl = nullptr;
        if (NeuronRuntime::Engine->QueryInterface(Iids::FLAppearance, reinterpret_cast<void**>(&Ifl)) < 0)
        {
            if (Ifl != nullptr)
                Ifl->Release();
            return false;
        }
        bool const bOk = Ifl->GetInstanceValue(Id, Out) >= 0;
        if (Ifl != nullptr)
            Ifl->Release();
        return bOk;
    }

    // (cdecl) -- the same QI; on success Slot6 (+0x18) (First, Second); the local
    // is released whenever it is non-null. No result.
    void SetAppearanceDirectionalLight(float const* First, float const* Second)
    {
        IFLAppearance* Ifl = nullptr;
        if (NeuronRuntime::Engine->QueryInterface(Iids::FLAppearance, reinterpret_cast<void**>(&Ifl)) >= 0)
            Ifl->SetDirectionalLight(First, Second);
        if (Ifl != nullptr)
            Ifl->Release();
    }

    // (cdecl) -- the same around Slot7 (+0x1c) (Flag).
    void SetAppearanceDirectionalLightEnabled(unsigned char Flag)
    {
        IFLAppearance* Ifl = nullptr;
        if (NeuronRuntime::Engine->QueryInterface(Iids::FLAppearance, reinterpret_cast<void**>(&Ifl)) >= 0)
            Ifl->SetDirectionalLightEnabled(Flag);
        if (Ifl != nullptr)
            Ifl->Release();
    }
}

unsigned* FClientShipGameobj::Slot188(unsigned* OutLod, void* Camera, unsigned CachedLod,
                                      void* Matrix, void* Extra)
{
    // (RET 0x14) -- the ship draw.
    // from the cockpit camera (the routine) the player's own ship is
    // not drawn: *OutLod = {0, 0xffff}.
    if (IsFirstPersonCameraCurrent() && GetPlayerObjRW() == this)
    {
        *OutLod = 0xffff0000u;
        return OutLod;
    }

    // the pilot mesh -- option 0x1d on (the routine > 0), the cached
    // LOD's low word at least 3 (unsigned, JC skips), its high word below 2 and below 1 (two
    // signed JGE tests) and a bound pilot instance (the routine on +0x108).
    unsigned short const LodLow = static_cast<unsigned short>(CachedLod & 0xffffu);
    short const LodHigh = static_cast<short>(CachedLod >> 16);
    if (GetGameOption(0x1d) > 0 && LodLow >= 3 && LodHigh < 2 && LodHigh < 1 &&
        ClientMeshHolderHasInstance(&Field_0x108))
    {
        // the CEqObj's +0x140 fade (the object narrowed by 0x103, no
        // test) strictly above 0 (FCOMP 0.0 / TEST AH,0x41 / JNZ skips not-greater): the
        // appearance Slot8(3, &Extra) (the routine), Slot4(get_index, &Value) (the routine)
        // and Slot3(pilot instance, Value) (the routine).
        CEqObj* const EqObj = static_cast<CEqObj*>(NarrowObject(this, 0x103u));
        if (EqObj->CloakPercent > 0.0f)
        {
            GetAppearanceOverrideMaterial(3u, &Extra);
            // baseline leaves this local unwritten when the QI fails; zeroed here.
            void* Value = nullptr;
            GetAppearanceInstanceValue(static_cast<int>(get_index()), &Value);
            SetAppearanceInstanceValue(Field_0x108.EngineInstance, Value);
        }

        // vt+0x184 (Slot184) with the camera.
        Slot184(Camera);
        // the pilot's world transform -- vt+0x10 (the object's
        // transform) times +0x118 through ship.cpp's 3DMathEngine (the field) slot 7
        // MulTransform into a local.
        FTransform const& ShipXform = reinterpret_cast<FTransform const&>(get_orientation());
        FTransform PilotXform;
        GetShipMathEngine()->MulTransform(PilotXform, ShipXform,
                                          reinterpret_cast<FTransform const&>(Field_0x118));
        // copied (the routine) and set on the pilot instance
        // (the routine).
        FTransform Copy;
        CopyTransform12(&Copy, &PilotXform);
        ClientMeshHolderSetMatrix(&Field_0x108, &Copy);
        // NeuronRuntime::Renderer slot 7 (Camera, pilot instance, 1.0f, 8, 0,
        // Extra).
        NeuronRuntime::Renderer->RenderInstance(Camera, Field_0x108.EngineInstance, 1.0f, 8u, nullptr, Extra);
    }

    // the routine(0) -- the appearance Slot7(0).
    SetAppearanceDirectionalLightEnabled(0);

    // with a star (the routine's list non-empty, its first node's
    // sphere set), the unit direction from the star (the routine) to this ship's position
    // (vt+0), computed on the FPU (FSQRT, FDIVR 1.0 stored as a float), goes to the appearance
    // Slot6(&direction, &{1, 1, 1}) (the routine).
    FStarSphereList* const Stars = StarSphereList();
    if (Stars != nullptr && Stars->Count != 0 && Stars->Sentinel->Next->Sphere != nullptr)
    {
        Vector StarPos;
        StarSphereGetOwnerPosition(Stars->Sentinel->Next->Sphere, &StarPos);
        Vector const& Pos = get_position();
        double const Dz = static_cast<double>(Pos.z) - StarPos.z;
        double const Dy = static_cast<double>(Pos.y) - StarPos.y;
        double const Dx = static_cast<double>(Pos.x) - StarPos.x;
        float const White[3] = {1.0f, 1.0f, 1.0f};
        float const Inv = static_cast<float>(1.0 / std::sqrt(Dx * Dx + Dy * Dy + Dz * Dz));
        float const Dir[3] = {static_cast<float>(Dx * Inv), static_cast<float>(Dy * Inv),
                              static_cast<float>(Dz * Inv)};
        SetAppearanceDirectionalLight(Dir, White);
    }

    // EquippedObj's draw the routine into a local, copied to *OutLod.
    unsigned Lod = 0;
    unsigned* const Result =
        EquippedObjSubmitDrawbaseline(this, &Lod, Camera, CachedLod, Matrix, Extra);
    *OutLod = *Result;
    return OutLod;
}

void FClientShipGameobj::Slot198(DamageList const& Damage)
{
    // (RET 4). 0x00545f52..0x00545fac: damage of cause 6 on the player's ship ->
    // IObjAffect (+4) vt+0x50 slot_50(false, true, the field), then the ship's behavior
    // manager (CEqObj::get_behavior_interface on the object narrowed by 0x503, no test of the
    // narrowing), when there is one, IBehaviorManager::update_current_behavior_cruise(false).
    if (static_cast<int>(Damage.Cause) == 6 && this == GetPlayerObjRW())
    {
        static_cast<IObjAffect*>(this)->slot_50(false, true,
                                                static_cast<int>(GetPlayerConnectionId()));
        IBehaviorManager* const Behavior =
            static_cast<CEqObj*>(NarrowObject(this, 0x503u))->get_behavior_interface();
        if (Behavior != nullptr)
            Behavior->update_current_behavior_cruise(false);
    }

    // EquippedObj's slot 102 (the routine), called directly.
    FClientEquippedObj::Slot198(Damage);

    // the player's ship -- CSimple::get_relative_health against 0.2f
    // (the field; FCOMP / TEST AH,0x1 / JNZ: the jump is taken on strictly-less). At or
    // above it the "hull breach" latch +0x154 clears and the routine runs; below it, the
    // first time (latch clear) and only when the list is not a destroyed one (+0xc byte), the
    // latch is set and the "hull_breach_imminent" voice line (the field) is queued.
    if (this == GetPlayerObjRW())
    {
        CSimple* const Simple = static_cast<CSimple*>(NarrowObject(this, 0x503u));
        if (!(Simple->get_relative_health() < 0.2f))
        {
            Field_0x154 = 0;
            StarSysNoteDamage(this, &Damage);
            return;
        }
        if (Field_0x154 == 0 && !Damage.bDestroyed)
        {
            Field_0x154 = 1;
            NnVoiceEnqueue(G_ShipVoiceHullBreachId, 0);
        }
    }

    // the routine(this, Damage).
    StarSysNoteDamage(this, &Damage);
}

void FClientShipGameobj::Slot1a0(void const* Contact, void const* B, FClientGameobj* Other,
                                 void const* D)
{
    // (RET 0x10). 0x00545ea4..0x00545eaa: EquippedObj's slot 104 (the routine)
    // with the four arguments.
    FClientEquippedObj::Slot1a0(Contact, B, Other, D);
    // the player's ship with a live HUD_DamageIndicator (the field)
    // -> the routine(Contact): the arrow towards the contact point.
    if (this != GetPlayerObjRW())
        return;
    HUD_DamageIndicator* const Indicator = GetHudDamageIndicator();
    if (Indicator == nullptr)
        return;
    Indicator->FlashFromPosition(static_cast<Vector const*>(Contact));
}

void FClientShipGameobj::Slot1a4(void const* A, void const* B, FClientGameobj* Munition)
{
    // (RET 0xc). 0x00545ee3..0x00545ee8: EquippedObj's slot 105 (the routine).
    FClientEquippedObj::Slot1a4(A, B, Munition);

    // the hit sub-object id (B's low word) -- a shield equipment id
    // (SubObjectID::IsShieldEquipID) sets bit 1 of +0x18c, anything else bit 0.
    unsigned short const SubObj = static_cast<unsigned short>(reinterpret_cast<uintptr_t>(B));
    if (SubObjectID::IsShieldEquipID(SubObj))
        Field_0x18c |= 2;
    else
        Field_0x18c |= 1;

    // the player's ship with a live HUD_DamageIndicator -> the
    // munition's velocity (its vt+4, into a local) and the routine(A, &velocity).
    if (this != GetPlayerObjRW())
        return;
    if (GetHudDamageIndicator() == nullptr)
        return;
    Vector const Velocity = Munition->get_velocity();
    GetHudDamageIndicator()->FlashAlongRay(static_cast<Vector const*>(A), &Velocity);
}

void FClientShipGameobj::Slot1b0(Vector const* Pos)
{
    // (RET 4). 0x00545d86..0x00545e1c: the +0xf4 companion's stop prelude
    // (particle slot 1, sound vt+0x94(-1, identity pose) and vt+0x60, light the routine and
    // vt+8); 0x00545e1c..0x00545e7d: its release (particle the routine, sound vt+8, light
    // , each slot zeroed) and +0xf4 = 0 -- the same inline sequences the two
    // helpers carry.
    CliEffectCompanionArmStop(Field_0xf4);
    CliEffectCompanionClear(Field_0xf4);
    // EquippedObj's slot 108 (the routine).
    FClientEquippedObj::Slot1b0(Pos);
}

void FClientShipGameobj::Slot1b8(float Dt)
{
    // (RET 4) -- the lazy nebula lookup. 0x005455b0..0x005455c3: +0x34 += Dt.
    Field_0x34 += Dt;

    // jump states 2 and 3 (in the tunnel) -> no nebula, no factor.
    if (Field_0x158 == 2 || Field_0x158 == 3)
    {
        DrawStateA = 0;
        DrawStateB = 0;
        return;
    }

    // the refresh period -- 1.0 for other ships; for the player's
    // ship 0.1 while the fog factor (+0x3c, a float) is strictly between 0.0 and 1.0
    // (FCOMP 0.0 / TEST AH,0x41 / JNZ: not-greater jumps to 0.5; FCOMP 1.0 / TEST AH,0x5 /
    // JNP: strictly-less keeps 0.1), else 0.5.
    float Period = 1.0f;
    if (this == GetPlayerObjRW())
    {
        float const Factor = *reinterpret_cast<float const*>(&DrawStateB);
        if (Factor > 0.0f && Factor < 1.0f)
            Period = 0.1f;
        else
            Period = 0.5f;
    }

    // TEST AH,0x41 / JNZ -- only a clock strictly above the period
    // goes on.
    if (!(Field_0x34 > Period))
        return;

    // the routine(get_position() (vt+0), &+0x3c, hint +0x38) into
    // +0x38.
    FNebulaEffect* const Hint = reinterpret_cast<FNebulaEffect*>(static_cast<uintptr_t>(DrawStateA));
    FNebulaEffect* const Nebula =
        StarSysFindNebula(&get_position(), reinterpret_cast<float*>(&DrawStateB), Hint);
    DrawStateA = static_cast<unsigned>(reinterpret_cast<uintptr_t>(Nebula));

    // the clock reloaded; above the period it is reduced on the FPU
    // stack (FSUB / FCOM / TEST AH,0x41 / JZ loops while still greater) and stored once.
    double Clock = Field_0x34;
    if (!(Clock > Period))
        return;
    do
    {
        Clock -= Period;
    } while (Clock > Period);
    Field_0x34 = static_cast<float>(Clock);
}

bool FClientShipGameobj::Slot1c0(unsigned SourceId, ID_String const* FuseId,
                                 unsigned short SubObjId, float AtT, float Lifetime)
{
    // (RET 0x14). 0x00545a93..0x00545abe: the routine called directly with the
    // same five arguments; not lit -> that result.
    bool const bLit = FClientEquippedObj::Slot1c0(SourceId, FuseId, SubObjId, AtT, Lifetime);
    if (!bLit)
        return bLit;
    // only DEATH_COMM_FUSE goes on.
    if (FuseId->Id != DEATH_COMM_FUSE.Id)
        return bLit;

    // the ship (class 0x503; null otherwise, read anyway).
    CShip* const Ship = static_cast<CShip*>(NarrowObject(this, 0x503u));

    // the formation followers (+0x1d4..+0x1d8, 8-byte watchers whose
    // first dword is the Watchable); a follower whose id is the player's ship (the routine):
    // its object (vtable +0x150) narrowed by 0x503 (null otherwise, used anyway) ->
    // CEqObj::get_behavior_interface; non-null -> IBehaviorManager slot 0 with {7, &this id}.
    for (std::vector<FObjRWWatcher>::const_iterator It = Ship->FormationFollowers.begin();
         It != Ship->FormationFollowers.end(); ++It)
    {
        IObjRW* const Follower = It->get();
        if (Follower == nullptr || !IsPlayerShipId(Follower->get_id()))
            continue;
        CObject* const FollowerObj = const_cast<CObject*>(Follower->cobject());
        CEqObj* const FollowerShip =
            (FollowerObj != nullptr &&
             (static_cast<unsigned>(FollowerObj->ObjectClass) & 0x503u) == 0x503u)
                ? static_cast<CEqObj*>(FollowerObj)
                : nullptr;
        IBehaviorManager* const Behavior = FollowerShip->get_behavior_interface();
        if (Behavior == nullptr)
            continue;
        unsigned SelfId = get_id();
        FBehaviorManagerEvent Event;
        Event.Type = 7;
        Event.Data = &SelfId;
        Behavior->Slot00(&Event);
    }

    // the same over the follow followers (+0x1f8..+0x1fc).
    for (std::vector<FObjRWWatcher>::const_iterator It = Ship->FollowFollowers.begin();
         It != Ship->FollowFollowers.end(); ++It)
    {
        IObjRW* const Follower = It->get();
        if (Follower == nullptr || !IsPlayerShipId(Follower->get_id()))
            continue;
        CObject* const FollowerObj = const_cast<CObject*>(Follower->cobject());
        CEqObj* const FollowerShip =
            (FollowerObj != nullptr &&
             (static_cast<unsigned>(FollowerObj->ObjectClass) & 0x503u) == 0x503u)
                ? static_cast<CEqObj*>(FollowerObj)
                : nullptr;
        IBehaviorManager* const Behavior = FollowerShip->get_behavior_interface();
        if (Behavior == nullptr)
            continue;
        unsigned SelfId = get_id();
        FBehaviorManagerEvent Event;
        Event.Type = 7;
        Event.Data = &SelfId;
        Behavior->Slot00(&Event);
    }

    // the player's gameobj (the routine) with a ship object whose
    // CShip::get_target is this ship (ids compared) -> the player's IObjAffect face (+4)
    // set_target(0, SUBOBJ_ID_NONE, the field).
    FClientGameobj* const Player = static_cast<FClientGameobj*>(GetPlayerObjRW());
    if (Player == nullptr)
        return bLit;
    CObject* const PlayerObj = Player->Object;
    if (PlayerObj == nullptr ||
        (static_cast<unsigned>(PlayerObj->ObjectClass) & 0x503u) != 0x503u)
        return bLit;
    IObjRW* const Target = static_cast<CShip*>(PlayerObj)->get_target();
    if (Target == nullptr)
        return bLit;
    if (Target->get_id() != get_id())
        return bLit;
    static_cast<IObjAffect*>(Player)->set_target(nullptr, SUBOBJ_ID_NONE,
                                                 static_cast<int>(GetPlayerConnectionId()));
    return bLit;
}

unsigned char FClientShipGameobj::Slot1cc(FCliEquipFirePacket const* Packet)
{
    // ported as CliShipHandleEquipUpdate (cli_projectile_fx.cpp).
    return CliShipHandleEquipUpdate(this, Packet);
}

void FClientShipGameobj::Slot1d0()
{
    // (bare RET) -- the "fire failed" beep. 0x00545cc3..0x00545cd4: FCOMP 0.0 /
    // TEST AH,0x41 / JP -- a cooldown (+0x150) strictly above zero (or unordered) returns.
    if (!(Field_0x150 <= 0.0f))
        return;

    // the FIRE_FAILED_SOUND archetype, looked up once (the routine)
    // into +0x148 and acquired (its vt+0xc) when found.
    if (Field_0x148 == nullptr)
    {
        Field_0x148 = (Archetype::ISound*)LookupClientSoundArch(&FIRE_FAILED_SOUND);
        if (Field_0x148 != nullptr)
            AcquireClientSoundArch(Field_0x148);
    }

    // no archetype -> only the cooldown.
    if (Field_0x148 != nullptr)
    {
        if (Field_0x14c == nullptr)
        {
            // no source yet -> the routine(arch, 0, &+0x14c, 0), then
            // the cooldown.
            PlayClientSoundArch(Field_0x148, nullptr, reinterpret_cast<void**>(&Field_0x14c), 0);
            Field_0x150 = FIRE_FAILED_DELAY;
            return;
        }
        if (Field_0x14c->IsActive())
        {
            // still playing -> vt+0x64 SetStartOffset(0) (rewind),
            // then the cooldown.
            Field_0x14c->SetStartOffset(0);
            Field_0x150 = FIRE_FAILED_DELAY;
            return;
        }
        // stopped -> vt+0x58 Start(arch, 0).
        Field_0x14c->Start(Field_0x148, 0);
    }

    // +0x150 = FIRE_FAILED_DELAY.
    Field_0x150 = FIRE_FAILED_DELAY;
}

int FClientShipGameobj::set_throttle(float Throttle)
{
    // CShip::set_throttle on the object narrowed by 0x503, no null test.
    static_cast<CShip*>(NarrowObject(this, 0x503u))->set_throttle(Throttle);
    return 0;
}

int FClientShipGameobj::set_axis_throttle(Vector const& AxisThrottle)
{
    // CSteering::SetAxisThrottle on the CShip's +0x230.
    static_cast<CShip*>(NarrowObject(this, 0x503u))->Steering.SetAxisThrottle(AxisThrottle);
    return 0;
}

int FClientShipGameobj::set_nudge_vec(Vector const& NudgeVec)
{
    // .
    static_cast<CShip*>(NarrowObject(this, 0x503u))->set_nudge_vec(NudgeVec);
    return 0;
}

int FClientShipGameobj::set_strafe_dir(StrafeDir Dir)
{
    // (IObjAffect slot 3, RET 4). 0x00546705..0x00546739: unchanged
    // (CShip::get_strafe_dir on the object narrowed by 0x503, no test) -> 0.
    if (static_cast<CShip*>(NarrowObject(this, 0x503u))->get_strafe_dir() == Dir)
        return 0;

    // the engine (CEquipManager::FindFirst(0x20000) on +0xe4); one that
    // is not active (vt+4) is switched on through this face's vt+0x4c slot_4c(&its id, true,
    // ).
    CEquip* const Engine =
        static_cast<CEqObj*>(NarrowObject(this, 0x503u))->EquipManager.FindFirst(0x20000u);
    if (Engine != nullptr && !Engine->IsActive())
    {
        unsigned short EngineId = Engine->Id;
        static_cast<IObjAffect*>(this)->slot_4c(&EngineId, true,
                                                static_cast<int>(GetPlayerConnectionId()));
    }

    // CShip::set_strafe_dir(Dir); always 0.
    static_cast<CShip*>(NarrowObject(this, 0x503u))->set_strafe_dir(Dir);
    return 0;
}

int FClientShipGameobj::set_formation_leader(IObjRW* Leader)
{
    // the CShip's +0x1b8 BaseWatcher; a leader other than the one watched now
    // is set on the leader's Watchable (+8), null for a null leader -- BaseWatcher::set is the
    // inline compare + set_pointer at 0x0054717d..00547182.
    CShip* const Ship = static_cast<CShip*>(NarrowObject(this, 0x503u));
    Watchable const* const Target = (Leader != nullptr) ? static_cast<Watchable const*>(Leader)
                                                        : nullptr;
    Ship->FormationLeader.set(Target);
    return 0;
}

int FClientShipGameobj::set_formation_offset(Vector const& Offset)
{
    // three dwords into the CShip's +0x1c0.
    static_cast<CShip*>(NarrowObject(this, 0x503u))->FormationOffset = Offset;
    return 0;
}

int FClientShipGameobj::add_formation_follower(IObjRW* Follower)
{
    // tail JMP into CShip::add_formation_follower.
    return static_cast<int>(
        static_cast<CShip*>(NarrowObject(this, 0x503u))->add_formation_follower(Follower));
}

int FClientShipGameobj::remove_formation_follower(IObjRW* Follower)
{
    // tail JMP into CShip::remove_formation_follower.
    return static_cast<int>(
        static_cast<CShip*>(NarrowObject(this, 0x503u))->remove_formation_follower(Follower));
}

int FClientShipGameobj::slot_38(unsigned short EquipId, int Count, int SourceClient)
{
    // (IObjAffect slot 14, RET 0xc) -- cargo jettison.
    // from this client (-1 or the field) the request goes to the
    // server: IServer vt+0x20 JettisonCargo(the field, {get_id, EquipId, Count}); 0.
    if (SourceClient == -1 || static_cast<unsigned>(SourceClient) == GetPlayerConnectionId())
    {
        XJettisonCargo Info;
        Info.ObjectId = get_id();
        Info.ItemId = EquipId;
        Info.Count = static_cast<unsigned int>(Count);
        LocalServer()->JettisonCargo(GetPlayerConnectionId(), Info);
        return 0;
    }

    // from the server -- the cargo with that id (FindByID on +0xe4 of
    // the object narrowed by 0x503, CECargo::cast); none -> 0. Otherwise
    // CECargo::RemoveFromStack(Count), CShip::open_bay, one more bay close to run (+0xe8) and
    // the close countdown (+0xe4) = open_bay's result + JETTISON_CLEARANCE_TIME + itself (FADD
    // order kept).
    CEquip* const Equip =
        static_cast<CEqObj*>(NarrowObject(this, 0x503u))->EquipManager.FindByID(EquipId);
    CECargo* const Cargo = CECargo::cast(Equip);
    if (Cargo == nullptr)
        return 0;
    Cargo->RemoveFromStack(static_cast<unsigned>(Count));
    float const BayTime = static_cast<CShip*>(NarrowObject(this, 0x503u))->open_bay();
    ++Field_0xe8;
    Field_0xe4 = static_cast<float>(
        (static_cast<double>(BayTime) + CShip::JETTISON_CLEARANCE_TIME) + Field_0xe4);
    return 0;
}

int FClientShipGameobj::set_target(IObjRW* Target, unsigned short SubTarget, int SourceClient)
{
    // ported as CliShipSetTarget (cli_projectile_fx.cpp).
    return CliShipSetTarget(static_cast<IObjAffect*>(this), Target, SubTarget, SourceClient);
}

int FClientShipGameobj::slot_50(bool bEngage, bool bArg2, int SourceClient)
{
    // (IObjAffect slot 20, RET 0xc) -- cruise.
    // this client's own request (-1 or the field).
    bool const bLocal =
        SourceClient == -1 || static_cast<unsigned>(SourceClient) == GetPlayerConnectionId();
    bool bChanged = false;
    // in single player only this client's own requests go on; -5.
    if (SinglePlayer() && !bLocal)
        return -5;

    // the engine (FindFirst(0x20000), CEEngine::cast); none -> -2.
    CEEngine* const Engine = CEEngine::cast(
        static_cast<CEqObj*>(NarrowObject(this, 0x503u))->EquipManager.FindFirst(0x20000u));
    if (Engine == nullptr)
        return -2;

    // a cruise state (+0x54) that differs: changed; engaging from this
    // client first stops the thrusters (this face's vt+0x54 slot_54(false, the field)),
    // then CEEngine::EngageCruise(bEngage, bArg2).
    if (Engine->bCruiseActive != static_cast<unsigned char>(bEngage))
    {
        bChanged = true;
        if (bEngage && bLocal)
            static_cast<IObjAffect*>(this)->slot_54(false,
                                                    static_cast<int>(GetPlayerConnectionId()));
        Engine->EngageCruise(bEngage, bArg2);
    }

    // changed by this client -> IServer vt+8 ActivateCruise(
    // , {get_id, bEngage, bArg2}); 0.
    if (bChanged && bLocal)
    {
        XActivateCruise Info;
        Info.ObjectId = get_id();
        Info.Field_0x05 = static_cast<unsigned char>(bArg2);
        Info.bActivate = bEngage;
        LocalServer()->ActivateCruise(GetPlayerConnectionId(), Info);
    }
    return 0;
}

int FClientShipGameobj::slot_54(bool bActivate, int SourceClient)
{
    // (IObjAffect slot 21, RET 8) -- the thrusters.
    // this client's own request (-1 or the field).
    bool const bLocal =
        SourceClient == -1 || static_cast<unsigned>(SourceClient) == GetPlayerConnectionId();
    bool bChanged = false;
    // in single player only this client's own requests go on; -5.
    if (SinglePlayer() && !bLocal)
        return -5;

    // the engine (FindFirst(0x20000), not cast); none -> -5.
    CEquip* const Engine =
        static_cast<CEqObj*>(NarrowObject(this, 0x503u))->EquipManager.FindFirst(0x20000u);
    if (Engine == nullptr)
        return -5;
    // its cruise byte (+0x54) and vt+4 IsActive.
    unsigned char const bCruise = static_cast<CEEngine*>(Engine)->bCruiseActive;
    bool const bEngineActive = Engine->IsActive();
    if (bLocal)
    {
        // switching on while cruising -> -5.
        if (bActivate && bCruise != 0)
            return -5;
        // switching on with the engine off -> this face's vt+0x4c
        // slot_4c(&engine id, true, SourceClient).
        if (bActivate && !bEngineActive)
        {
            unsigned short EngineId = Engine->Id;
            static_cast<IObjAffect*>(this)->slot_4c(&EngineId, true, SourceClient);
        }
    }

    // every thruster (CEquipTraverser(0x400), Traverse on +0xe4) whose
    // IsActive differs from bActivate: changed, vt+0x30 Activate(bActivate).
    CEquipTraverser Walk(0x400);
    for (;;)
    {
        CEquip* const Thruster =
            static_cast<CEqObj*>(NarrowObject(this, 0x503u))->EquipManager.Traverse(Walk);
        if (Thruster == nullptr)
            break;
        if (Thruster->IsActive() == bActivate)
            continue;
        bChanged = true;
        Thruster->Activate(bActivate);
    }

    // changed by this client -> IServer vt+0xc ActivateThrusters(
    // , {get_id, bActivate}); 0.
    if (bChanged && bLocal)
    {
        XActivateThrusters Info;
        Info.ObjectId = get_id();
        Info.bActivate = bActivate;
        LocalServer()->ActivateThrusters(GetPlayerConnectionId(), Info);
    }
    return 0;
}

int FClientShipGameobj::slot_5c(int RequestType, unsigned const* TargetId,
                                unsigned const* SecondId, int bFlag, RequestCB* Callback)
{
    // keep the callback at +0xec, then IServer::RequestEvent (vt+0xf0) with
    // (RequestType, get_id(), *TargetId, *SecondId, bFlag, the field). get_id is called
    // after both ids are read (0x00546e22).
    Field_0xec = Callback;
    unsigned const Second = *SecondId;
    unsigned const Target = *TargetId;
    unsigned const SelfId = get_id();
    LocalServer()->RequestEvent(RequestType, SelfId, Target, Second,
                                static_cast<unsigned long>(bFlag), GetPlayerConnectionId());
    return 0;
}

int FClientShipGameobj::slot_60(int RequestType, unsigned const* TargetId, int Arg3)
{
    // drop the callback, then IServer::RequestCancel (vt+0xf4) with
    // (RequestType, get_id(), *TargetId, Arg3, the field).
    Field_0xec = nullptr;
    unsigned const Target = *TargetId;
    unsigned const SelfId = get_id();
    LocalServer()->RequestCancel(RequestType, SelfId, Target, static_cast<unsigned long>(Arg3),
                                 GetPlayerConnectionId());
    return 0;
}

int FClientShipGameobj::go_tradelane(IObjInspect const* EntranceRing, IObjInspect const* ExitRing,
                                     bool bFastExit, float Speed)
{
    // (IObjAffect slot 25, RET 0x10) -- listing 00546e90..00546f74.
    // no exit ring -> -4 (only the exit ring is tested).
    if (ExitRing == nullptr)
        return -4;

    // CShip::go_tradelane on the narrowed CShip (no null test of the
    // narrowing) with this gameobj as the tradelane object; non-zero -> -5.
    IObjRW* const Self = static_cast<IObjRW*>(this);
    CShip* const Ship = static_cast<CShip*>(NarrowObject(this, 0x503u));
    if (Ship->go_tradelane(EntranceRing, ExitRing, Self, bFastExit, Speed) != 0)
        return -5;

    // IServer::GoTradelane (vt+0x18) with {own id, entrance id, exit
    // id, bFastExit} and the field; the ids are read in that order.
    XGoTradelane Info;
    Info.ObjectId = Self->get_id();
    Info.TradelaneRing1 = EntranceRing->get_id();
    Info.TradelaneRing2 = ExitRing->get_id();
    Info.Field_0x0c = bFastExit;
    LocalServer()->GoTradelane(GetPlayerConnectionId(), Info);

    // both rings through the ObjDB (the routine(id, 2)), then
    // (Entrance, Exit, this, 0) -- its result is not used.
    FClientGameobj* const EntranceGo = ClientObjDb_FindObject(EntranceRing->get_id(), 2);
    FClientGameobj* const ExitGo = ClientObjDb_FindObject(ExitRing->get_id(), 2);
    TradeLaneShipGoHandler(this, EntranceGo, ExitGo);
    return 0;
}

int FClientShipGameobj::stop_tradelane()
{
    // (IObjAffect slot 26, RET) -- listing 00546f80..005470ea.
    // CShip::stop_tradelane on the narrowed CShip (no null test).
    CShip* const Ship = static_cast<CShip*>(NarrowObject(this, 0x503u));
    Ship->stop_tradelane();

    // the ship's own behavior manager (not null-tested) hears event 4,
    // {0, 0, 0, pad, 0.0f, 0, own id, entrance ring, 0} -- the same block the server's
    // notify_stop_tradelane builds.
    IBehaviorManager* const Behavior = static_cast<CEqObj*>(Ship)->get_behavior_interface();
    FBehaviorTradelaneEvent Block;
    Block.bEnteringLane = 0;
    Block.bExitRequested = 0;
    Block.bFastExit = 0;
    Block.Pad03 = 0;
    Block.Speed = 0.0f;
    Block.Field_0x08 = 0;
    Block.ShipId = get_id();
    Block.EntranceId = Ship->get_tradelane_entrance_ring();
    Block.ExitId = 0;
    FBehaviorManagerEvent Event;
    Event.Type = 4;
    Event.Data = &Block;
    Behavior->Slot00(&Event);

    // IServer::StopTradelane (vt+0x1c) with the field, own id,
    // entrance ring, exit ring (evaluated exit ring first, then entrance, then the id).
    unsigned const ExitId = Ship->get_tradelane_exit_ring();
    unsigned const EntranceId = Ship->get_tradelane_entrance_ring();
    unsigned const SelfId = get_id();
    LocalServer()->StopTradelane(GetPlayerConnectionId(), SelfId, EntranceId, ExitId);

    // the routine on this gameobj.
    TradeLaneShipStop(this);
    return 0;
}

int FClientShipGameobj::slot_6c(int ArgA, int ArgB)
{
    // (IObjAffect slot 27, RET 8; ArgB is not read). The player's ship only: the
    // voice line for ArgA -- 0 "nnv_dock_too_far" (the field), 1
    // "nnvoice_trade_lane_disrupted" (the field), 5 "nnv_formation_lost" (the field) --
    // when its id is non-zero, queued (the routine(id, 0)). Always 0.
    (void)ArgB;
    if (GetPlayerObjRW() != this)
        return 0;
    unsigned VoiceId = 0;
    switch (ArgA)
    {
    case 0:
        VoiceId = G_ShipVoiceDockTooFarId;
        break;
    case 1:
        VoiceId = G_ShipVoiceTradeLaneDisruptedId;
        break;
    case 5:
        VoiceId = G_ShipVoiceFormationLostId;
        break;
    default:
        return 0;
    }
    if (VoiceId != 0)
        NnVoiceEnqueue(VoiceId, 0);
    return 0;
}

void FClientShipGameobj::RequestReturned(int Result, int Code, int Extra)
{
    // (thiscall, RET 0xc). 0x00546d04..0x00546d36: no pending callback (+0xec)
    // -> the "stale request detected" warning (ship.cpp line 0x863).
    if (Field_0xec == nullptr)
    {
        FDUMP(0x100002, "%s(%d) : *** WARNING: %s", kShipSourceFile, 0x863,
              "stale request detected");
        return;
    }
    // the callback's slot 0 (Result, Code, Extra); false drops it.
    if (!Field_0xec->OnResult(Result, Code, Extra))
        Field_0xec = nullptr;

    // the player's ship only.
    if (GetPlayerObjRW() != this)
        return;

    // the docking voice line -- Result 0: Code 0 "dock_denied_hostile",
    // 1 "dock_denied_locked", 2 / 5 "cannot_dock", 3 "waiting_to_dock", 4 "dock_granted";
    // Result 2: Code 4 "dock_granted", 3 nothing, 0 / 1 / 2 / 5 "cannot_dock"; any other
    // Result or a Code above 5 (JA): nothing.
    unsigned VoiceId = 0;
    if (Result == 0)
    {
        switch (Code)
        {
        case 0:
            VoiceId = G_ShipVoiceDockDeniedHostileId;
            break;
        case 1:
            VoiceId = G_ShipVoiceDockDeniedLockedId;
            break;
        case 2:
        case 5:
            VoiceId = G_ShipVoiceCannotDockId;
            break;
        case 3:
            VoiceId = G_ShipVoiceWaitingToDockId;
            break;
        case 4:
            VoiceId = G_ShipVoiceDockGrantedId;
            break;
        default:
            return;
        }
    }
    else if (Result == 2)
    {
        switch (Code)
        {
        case 0:
        case 1:
        case 2:
        case 5:
            VoiceId = G_ShipVoiceCannotDockId;
            break;
        case 4:
            VoiceId = G_ShipVoiceDockGrantedId;
            break;
        default:
            return;
        }
    }
    else
    {
        return;
    }
    // a non-zero id -> the routine(id, 0).
    if (VoiceId != 0)
        NnVoiceEnqueue(VoiceId, 0);
}

int FClientShipGameobj::slot_70(unsigned Id)
{
    // IServer vt+0xc4 (LaunchComplete) with (Id, get_id()).
    unsigned const SelfId = get_id();
    LocalServer()->LaunchComplete(Id, SelfId);
    return 0;
}

int FClientShipGameobj::slot_74(unsigned Arg)
{
    // IServer vt+0xc8 (JumpInComplete) with (Arg, get_id()).
    unsigned const SelfId = get_id();
    LocalServer()->JumpInComplete(Arg, SelfId);
    return 0;
}

void FClientShipGameobj::RequestLand(FClientGameobj* DockGo, unsigned const* BaseId)
{
    // (RET 8). 0x00543b5e..0x00543b71: bit 3 of the +0x18c flag byte.
    Field_0x18c |= 8;

    if (DockGo != nullptr)
    {
        // IAction::operator new(0x50), Action_Land (the routine) with
        // this Ship and the dock's object narrowed to a CEqObj ((class & 0x303) == 0x303, null
        // otherwise), onto the action list (+0x20).
        CObject* const DockObject = DockGo->Object;
        CEqObj* Dock = nullptr;
        if (DockObject != nullptr &&
            (static_cast<unsigned>(DockObject->ObjectClass) & 0x303u) == 0x303u)
            Dock = static_cast<CEqObj*>(DockObject);
        Actions.add(new Action_Land(this, Dock));

        // the IObjAffect face (+4), slot 1 set_axis_throttle({0,0,0}).
        Vector Zero;
        Zero.x = 0.0f;
        Zero.y = 0.0f;
        Zero.z = 0.0f;
        static_cast<IObjAffect*>(this)->set_axis_throttle(Zero);

        // the camera only follows the watched player ship.
        if (static_cast<void*>(static_cast<IObjRW*>(this)) != GetPlayerObjRW())
            return;

        // unless the docking camera already has the view
        // (the routine), it goes to the dock's "HPDockCam" 'A' hardpoint -- one stack Csys,
        // identity orientation and zero position, filled by get_hardpoint (vt+0xc0, 0 is
        // success); on success the rear and turret views close (the routine) and the Csys goes
        // to the docking camera (the routine). A failed lookup leaves the camera alone.
        if (!IsDockingCameraCurrent())
        {
            Transform Csys;
            Csys.Rotation = Matrix(1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f);
            Csys.Translation.x = 0.0f;
            Csys.Translation.y = 0.0f;
            Csys.Translation.z = 0.0f;
            char HardpointName[0x40];
            sprintf(HardpointName, "%s%c", "HPDockCam", 0x41);
            if (DockGo->get_hardpoint(HardpointName, &Csys.Translation, &Csys.Rotation) == 0)
            {
                CloseRearAndTurretViewsForDocking();
                SetDockingCameraTransform(reinterpret_cast<const float*>(&Csys));
            }
        }

        // always, the docking camera's fade over the field (3.0f)
        // seconds (the routine).
        BeginDockingCameraSequence(3.0f);
        return;
    }

    // no dock.
    if (static_cast<void*>(static_cast<IObjRW*>(this)) == GetPlayerObjRW())
    {
        // The player's ship: an ERROR for base 0 (ship.cpp:0x2a8), the base asked for anyway
        // (the routine), and the hard cut to a covered screen (the routine(1.0f)).
        if (*BaseId == 0)
        {
            FDUMP(0x100001, "%s(%d) : *** ERROR: %s", kShipSourceFile, 0x2a8,
                  "Cannot land on BASE_ID_NONE");
        }
        SetPendingLandBase(BaseId);
        SetFadeImmediate(1.0f);
        return;
    }
    // Any other ship: slot 87 (vt+0x15c) with true.
    Slot15c(true);
}

void FClientGameobj::SetOwnerPlayer(unsigned PlayerId)
{
    // +0x10 narrowed by (class & 3) == 3 (CMP CL,3), null otherwise; tail JMP to
    // CSimple::SetOwnerPlayer with no null test.
    static_cast<CSimple*>(NarrowObject(this, 3u))->SetOwnerPlayer(PlayerId);
}

void FClientShipGameobj::SetJumpState(int State)
{
    // (RET 4). JA past 5 -> the warning; a jump table otherwise. The player test
    // (the routine) is made afresh at every use, as baseline does.
    switch (State)
    {
    case 0:
    {
        // the effect deleted (the routine + free) and cleared.
        if (Field_0x188 != nullptr)
        {
            delete static_cast<FJumpShipEffect*>(Field_0x188);
            Field_0x188 = nullptr;
        }
        // leaving state 5 on the player's ship: control exclusion
        // cleared on its CShip (0x503, no null test) and the NeuroNet screen byte dropped.
        if (Field_0x158 == 5 &&
            static_cast<void*>(static_cast<IObjRW*>(this)) == GetPlayerObjRW())
        {
            static_cast<CShip*>(NarrowObject(this, 0x503u))->set_control_exclusion(0);
            G_bNnScreenUp = false;
        }
        break;
    }
    case 1:
    {
        // the routine.
        if (Field_0x188 != nullptr)
            static_cast<FJumpShipEffect*>(Field_0x188)->StartJumpOut();
        // the player's ship takes the player camera (the routine).
        if (static_cast<void*>(static_cast<IObjRW*>(this)) == GetPlayerObjRW())
            RequestSpaceCameraMode("player");
        // and control exclusion 7 (after a 0 when 7 is already set),
        // with the NeuroNet screen byte raised.
        if (static_cast<void*>(static_cast<IObjRW*>(this)) == GetPlayerObjRW())
        {
            CShip* const Ship = static_cast<CShip*>(NarrowObject(this, 0x503u));
            if (Ship->is_control_excluded(7))
                Ship->set_control_exclusion(0);
            Ship->set_control_exclusion(7);
            G_bNnScreenUp = true;
        }
        // every ship drops its target.
        static_cast<CShip*>(NarrowObject(this, 0x503u))->set_target(nullptr);
        break;
    }
    case 2:
    {
        // the routine.
        if (Field_0x188 != nullptr)
            static_cast<FJumpShipEffect*>(Field_0x188)->EnterTunnel();
        // the "JumpOut" action taken off the list when still there.
        IAction* const JumpOut = Actions.find("JumpOut");
        if (JumpOut != nullptr)
            Actions.remove(JumpOut);
        // any ship but the player's goes through slot 87 with true;
        // in single player its CShip (no null test) also gets +0x214 = 1, an AddRef/Release
        // probe, and -- when that Release does not return 1 -- no owner and no control
        // exclusion.
        if (static_cast<void*>(static_cast<IObjRW*>(this)) != GetPlayerObjRW())
        {
            Slot15c(true);
            if (SinglePlayer())
            {
                CShip* const Ship = static_cast<CShip*>(NarrowObject(this, 0x503u));
                Ship->bJumpedOut = 1;               // MOV byte [EDI+0x214],1
                Ship->AddRef();
                if (Ship->Release() != 1)
                {
                    Ship->SetOwnerPlayer(0);
                    Ship->set_control_exclusion(0);
                }
            }
        }
        break;
    }
    case 3:
    {
        // with an effect, IDPMsgHandler::OnDisconnect on it (0x00401e90,
        // a bare RET the linker folded the effect's hook into -- nothing to do), then its +0xa4
        // (the destination system) as the pending jump system with flag 0 (the routine).
        if (Field_0x188 != nullptr)
        {
            unsigned const DestSystem = static_cast<FJumpShipEffect*>(Field_0x188)->Field_0xa4;
            SetPendingJumpSystem(&DestSystem, false);
        }
        break;
    }
    case 4:
    {
        // with an effect, the player's character jumps in at the
        // effect's +0x74 / +0x80 (CShip::jump_in with this gameobj and the effect's +0x10) and
        // the effect's jump-in hook runs; any other ship is just beamed there (CObject vt+0x7c,
        // not attached).
        FJumpShipEffect* const Effect = static_cast<FJumpShipEffect*>(Field_0x188);
        if (Effect != nullptr)
        {
            if (get_id() == GetPlayerCharacterId())
            {
                float const Speed = Effect->JumpInTime;
                static_cast<CShip*>(NarrowObject(this, 0x503u))
                    ->jump_in(static_cast<IObjRW*>(this), Effect->JumpInPosition,
                              Effect->JumpInOrientation, Speed);
                static_cast<FJumpShipEffect*>(Field_0x188)->StartJumpIn();
            }
            else
            {
                NarrowObject(this, 0x503u)
                    ->beam_object(Effect->JumpInPosition, Effect->JumpInOrientation, false);
            }
        }
        // the target dropped, +0x15c cleared.
        static_cast<CShip*>(NarrowObject(this, 0x503u))->set_target(nullptr);
        Field_0x15c = 0;
        break;
    }
    case 5:
    {
        // the routine with no null test; the player's character gets
        // the arrival caption (the routine).
        static_cast<FJumpShipEffect*>(Field_0x188)->FinishJump();
        if (get_id() == GetPlayerCharacterId())
            NnArrivalTextShow();
        break;
    }
    default:
        // .
        FDUMP(0x100002, "%s(%d) : *** WARNING: %s", kShipSourceFile, 0x272,
              "invalid jump state set in ship");
        break;
    }

    // the effect's clock restarts; the state is stored.
    if (Field_0x188 != nullptr)
        static_cast<FJumpShipEffect*>(Field_0x188)->Field_0x70 = 0.0f;
    Field_0x158 = State;
}

void FClientShipGameobj::BeginJumpOut(FClientGameobj* Gate)
{
    // (RET 4). 0x0054354c..0x00543566: any previous effect deleted.
    if (Field_0x188 != nullptr)
    {
        delete static_cast<FJumpShipEffect*>(Field_0x188);
        Field_0x188 = nullptr;
    }
    // operator new(0xdc) + the routine(this, Gate); null on a failed
    // new.
    Field_0x188 = new (std::nothrow) FJumpShipEffect(this, static_cast<FClientSolarGameobj*>(Gate));

    // the gate's type (vt+0x88 get_type), 0 without a gate.
    unsigned GateType = 0;
    if (Gate != nullptr)
        Gate->get_type(GateType);

    // state 1.
    SetJumpState(1);

    // IAction::operator new(0x64) + the routine(this, the effect's
    // JumpOutTime (+4, no null test), type), added to the action list.
    Actions.add(new Action_JumpOut(this, static_cast<FJumpShipEffect*>(Field_0x188)->JumpOutTime,
                                   GateType));
}

void FClientShipGameobj::BeginJumpIn(Vector const* Pos, Matrix const* Orient)
{
    // (RET 8). 0x0054364b.
    Field_0x15c = 1;

    // the player's ship: its effect (no null test) takes the exit
    // position and orientation, and the active camera's cut flag is raised (the routine).
    if (static_cast<void*>(static_cast<IObjRW*>(this)) == GetPlayerObjRW())
    {
        FJumpShipEffect* const Effect = static_cast<FJumpShipEffect*>(Field_0x188);
        Effect->JumpInPosition = *Pos;
        Effect->JumpInOrientation = *Orient;
        RaiseActiveSpaceCameraCut();
        return;
    }

    // any other ship: the previous effect deleted.
    if (Field_0x188 != nullptr)
    {
        delete static_cast<FJumpShipEffect*>(Field_0x188);
        Field_0x188 = nullptr;
    }
    // operator new(0xdc) + the routine(this), and the exit frame
    // copied in (no null test).
    Field_0x188 = new (std::nothrow) FJumpShipEffect(this);
    FJumpShipEffect* const Effect = static_cast<FJumpShipEffect*>(Field_0x188);
    Effect->JumpInPosition = *Pos;
    Effect->JumpInOrientation = *Orient;
    // state 4.
    SetJumpState(4);
}

void FClientShipGameobj::RequestSystemSwitch(FClientGameobj* Gate)
{
    // (RET 4): Gate's object narrowed by 0x303 (null otherwise, no later test);
    // its +0x1b4 into a local, the routine(&local, 0).
    CSolar const* const Solar = static_cast<CSolar const*>(NarrowObject(Gate, 0x303u));
    unsigned const DestSystem = Solar->DestSystem;
    SetPendingJumpSystem(&DestSystem, false);
}

void FClientShipGameobj::AirlockIn(Vector const& Pos, Matrix const& Orient)
{
    // `this` narrowed by 0x503 (null otherwise), tail JMP to CShip::airlock_in.
    static_cast<CShip*>(NarrowObject(this, 0x503u))->airlock_in(Pos, Orient);
}

// The launch camera's stack Csys: Matrix at +0x10, Vector at +0x34 of the frame, i.e. the
// orientation followed directly by the position -- the routine's twelve floats.
static_assert(offsetof(Transform, Translation) == 0x24, "Csys12: position after 3x3");
static_assert(sizeof(Transform) == 0x30, "Csys12: twelve floats");

void FClientShipGameobj::LaunchFromDock(FClientGameobj* DockGo, int LaunchIndex)
{
    // . 0x00543d5a: vt+0x20 get_id against the player's character id.
    if (get_id() != GetPlayerCharacterId())
        return;

    // the dock narrowed to a CEqObj ((class & 0x103) == 0x103), null
    // when the gameobj or its object is missing.
    CEqObj* DockEqObj = nullptr;
    if (DockGo != nullptr)
    {
        CObject* const DockObj = DockGo->Object;
        if (DockObj != nullptr &&
            (static_cast<unsigned>(DockObj->ObjectClass) & 0x103u) == 0x103u)
            DockEqObj = static_cast<CEqObj*>(DockObj);
    }

    // this gameobj's own object narrowed to a CShip (0x503). baseline
    // calls CShip::launch through the result with no null test (0x00543dc0).
    CShip* const Ship = static_cast<CShip*>(NarrowObject(this, 0x503u));
    Ship->launch(this, DockEqObj, LaunchIndex);

    // PUSH 0x401c0000 / PUSH 0 -- the double 7.0.
    StarSysSuppressPlayerCollisions(7.0);

    // the camera only follows the watched player ship.
    if (static_cast<void*>(static_cast<IObjRW*>(this)) != GetPlayerObjRW())
        return;

    // one Csys on the stack -- identity orientation at +0x10, zero
    // position at +0x34 -- initialised once, before the first lookup. Both get_hardpoint
    // calls write into the same pair, so a failed first lookup leaves whatever it wrote for
    // the second.
    Transform Csys;
    Csys.Rotation = Matrix(1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    Csys.Translation.x = 0.0f;
    Csys.Translation.y = 0.0f;
    Csys.Translation.z = 0.0f;

    // the hardpoint suffix is 'A' + index, printed with "%s%c" (.rdata 005dffa8).
    char HardpointName[0x40];
    int const Suffix = LaunchIndex + 0x41;
    sprintf(HardpointName, "%s%c", "HPLaunchCam", Suffix);

    // dock vt+0xc0 get_hardpoint(Name, &Pos, &Orient); 0 is success. No null
    // test on the dock here.
    bool bFound = DockGo->get_hardpoint(HardpointName, &Csys.Translation, &Csys.Rotation) == 0;
    if (!bFound)
    {
        // fall back to the docking hardpoint of the same index.
        sprintf(HardpointName, "%s%c", "HPDockCam", Suffix);
        bFound = DockGo->get_hardpoint(HardpointName, &Csys.Translation, &Csys.Rotation) == 0;
    }

    CloseRearAndTurretViewsForDocking(); // , on both arms (0x00543ebf / 0x00543eda)
    if (bFound)
    {
        // the routine(&Csys) -- orientation then position, twelve floats.
        SetDockingCameraTransform(reinterpret_cast<const float*>(&Csys));
    }
    else
    {
        // the routine("player").
        RequestSpaceCameraMode("player");
    }
}

void CloseRearAndTurretViewsForDocking()
{
    // . the field is read twice with no null test; each byte is compared with 1.
    // +0x5ba (rear view) -> the routine(0x39, 0).
    if (GetHudStatus()->bRearView == 1)
        HudPostCommand(0x39, 0);
    // +0x5b9 (turret view) -> the routine(0xbf, 0).
    if (GetHudStatus()->bTurretView == 1)
        HudPostCommand(0xbf, 0);
}

// ===========================================================================
// Projectile
// ===========================================================================

FClientProjectileGameobj::FClientProjectileGameobj(EClientGameobjKind Kind)
    : FClientSimpleGameobj(Kind)
{
    // 0x00542576 +0xac = -1.0f; 0x00542582..0x00542594 +0xb0..+0xbc = 0.
    ConstEffectDelay = -1.0f;
    OneShotCompanion[0] = nullptr;
    OneShotCompanion[1] = nullptr;
    OneShotCompanion[2] = nullptr;
    OneShotCompanion[3] = nullptr;
}

FClientProjectileGameobj::~FClientProjectileGameobj()
{
    // the companion released (particle the routine, sound vt+8, light
    // , then +0xb0 = 0) by the destructor body (0x00542622..0x0054266c) and again
    // by the member's own destructor (0x00542672..0x005426b8); then GameObject's (the routine).
    CliEffectCompanionClear(OneShotCompanion);
    CliEffectCompanionClear(OneShotCompanion);
}

namespace
{
    // The const_effect start shared by Projectile slots 92 and 93 (0x00542738..0x0054276a /
    // ): the effect under the companion's +0x58 id, started through
    // on this gameobj at Xform, into OneShotCompanion (+0xb0). The id is tested
    // for emptiness first, as both slots do right before the call.
    void ProjectileStartConstEffect(FClientProjectileGameobj* Self, ID_String const* Effect,
                                    long EngineIndex, Transform const& Xform)
    {
        if (Effect->IsEmpty())
            return;
        CliCreateEffectOneShot(FindEffectArch(&Effect->Id), Self, EngineIndex,
                               reinterpret_cast<FTransform const*>(&Xform),
                               Self->OneShotCompanion);
    }
}

void FClientProjectileGameobj::Slot170()
{
    // .
    FClientGameobj::Slot170();
    // the object narrowed by class & 7 (no null test before +0x88),
    // its archetype's companion, the companion's +0x58 const_effect.
    Archetype::ICliObj* const Companion = NarrowObject(this, 0x7u)->Archetype->Companion;
    ID_String const* const Effect = Archetype::CliProjConstEffect(Companion);
    if (Effect->IsEmpty())
        return;
    // a delay other than exactly 0 (NaN included) is armed.
    float const Delay = Archetype::CliProjConstEffectDelay(Companion);
    if (!(Delay == 0.0f))
    {
        ConstEffectDelay = Delay;
        return;
    }
    // Slot1c0(&Xform), get_index, start.
    Transform Xform;
    Slot1c0(&Xform);
    long const EngineIndex = get_index();
    ProjectileStartConstEffect(this, Effect, EngineIndex, Xform);
}

void FClientProjectileGameobj::update(float Dt)
{
    // (RET 4).
    if (Slot158())
        return;
    FClientGameobj::update(Dt);
    // a delay below 0 (or NaN) is not armed.
    if (!(ConstEffectDelay >= 0.0f))
        return;
    // count down; go on only at or below 0 (not above, not NaN).
    ConstEffectDelay = ConstEffectDelay - Dt;
    if (!(ConstEffectDelay <= 0.0f))
        return;
    ConstEffectDelay = -1.0f;
    Transform Xform;
    Slot1c0(&Xform);
    // the class & 7 object's archetype companion (no null test), then
    // get_index.
    Archetype::ICliObj* const Companion = NarrowObject(this, 0x7u)->Archetype->Companion;
    long const EngineIndex = get_index();
    ProjectileStartConstEffect(this, Archetype::CliProjConstEffect(Companion), EngineIndex,
                               Xform);
}

void FClientProjectileGameobj::Slot1a0(void const* Contact, void const* B,
                                       FClientGameobj* Other, void const* D)
{
    // RET 0x10.
    (void)Contact;
    (void)B;
    (void)Other;
    (void)D;
}

void FClientProjectileGameobj::Slot1b0(Vector const* Pos)
{
    // (RET 4): the routine(&EffectList, &OneShotCompanion), then GameObject's
    // slot 108 the routine called directly.
    CliEffectListRemove(&EffectList, OneShotCompanion);
    FClientGameobj::Slot1b0(Pos);
}

void FClientProjectileGameobj::Slot1c0(Transform* Out)
{
    // (RET 4): 1.0f at +0x00 / +0x10 / +0x20, every other dword of the 0x30 zero.
    Out->Rotation = Matrix(1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    Out->Translation.x = 0.0f;
    Out->Translation.y = 0.0f;
    Out->Translation.z = 0.0f;
}

// ===========================================================================
// Munition
// ===========================================================================

FClientMunitionGameobj::FClientMunitionGameobj(EClientGameobjKind Kind)
    : FClientProjectileGameobj(Kind)
{
    // the routine(Kind), then the three Munition vtables (C++).
}

FClientMunitionGameobj::~FClientMunitionGameobj()
{
    // The thunk the routine -> the routine: Projectile's destructor body, nothing of its own.
}

void FClientMunitionGameobj::Slot194(void const* A, void const* B, FClientGameobj* Other,
                                     void const* D)
{
    // (RET 0x10).
    Other->Slot1a4(A, D, this);
    Slot1ac(A, B, Other, D);
}

// ===========================================================================
// Beam
// ===========================================================================

FClientBeamGameobj::FClientBeamGameobj()
    : FClientMunitionGameobj(EClientGameobjKind::Beam)
{
    // the routine(3); the routine(+0xc0, 0, 0, 0, 0) -- stores the
    // four and AddRefs the non-null ones, so four nulls; +0xd0 = 0; Beam's vtables (C++).
    Companion[0] = nullptr;
    Companion[1] = nullptr;
    Companion[2] = nullptr;
    Companion[3] = nullptr;
    BeamAlloc = nullptr;
}

FClientBeamGameobj::~FClientBeamGameobj()
{
    // the companion at +0xc0 released (0x0053f212..0x0053f25c), the beam effect
    // at +0xd0 released and cleared (0x0053f262..0x0053f271), the companion released again by
    // its member destructor (0x0053f277..0x0053f2bd); then Projectile's (the routine).
    CliEffectCompanionClear(Companion);
    if (BeamAlloc != nullptr)
    {
        BeamEffectRelease(BeamAlloc);
        BeamAlloc = nullptr;
    }
    CliEffectCompanionClear(Companion);
}

void FClientBeamGameobj::create(Archetype::Root* Arch)
{
    // (RET 4): the routine(0x67, Arch), then vt+0x4c open(Arch) on the object
    // narrowed by class & 0x67 -- no null test (0x0053f31e calls through a null object).
    GameobjCreateOfClass(this, CObject::ClassBeam, Arch);
    NarrowObject(this, 0x67u)->open(Arch);
}

bool FClientBeamGameobj::find(unsigned const& Id)
{
    // XOR AL,AL / RET 4.
    (void)Id;
    return false;
}

void FClientBeamGameobj::update(float Dt)
{
    // (RET 4).
    FClientProjectileGameobj::update(Dt);
    // the class & 7 object's vt+0x50 update(Dt, UPDATE_DEFAULT), no
    // null test, result dropped.
    NarrowObject(this, 0x7u)->update(Dt, CObject::UPDATE_DEFAULT);
    if (Slot154())
        return;
    // the object's +0xe8 life below 0 -> Slot15c(true). TEST AH,1 /
    // JZ skips only when C0 is clear, so a NaN life (C0 set) also expires the beam.
    CProjectile const* const Projectile = static_cast<CProjectile const*>(NarrowObject(this, 0x7u));
    if (!(Projectile->Lifetime >= 0.0f))
        Slot15c(true);
}

void FClientBeamGameobj::Slot194(void const* A, void const* B, FClientGameobj* Other,
                                 void const* D)
{
    // (RET 0x10).
    if (Slot154())
        return;
    Other->Slot1a4(A, D, this);
    Slot1ac(A, B, Other, D);
    // whether Other is the player's ship (the routine).
    bool const bOtherIsPlayer = static_cast<void*>(static_cast<IObjRW*>(Other)) == GetPlayerObjRW();
    // the CBeam's velocity (+0xf8) zeroed -- no null test.
    CBeam* const BeamObj = static_cast<CBeam*>(NarrowObject(this, 0x67u));
    BeamObj->Velocity.x = 0.0f;
    BeamObj->Velocity.y = 0.0f;
    BeamObj->Velocity.z = 0.0f;
    // the beam effect marked hit (the routine(1, bOtherIsPlayer)),
    // released and cleared.
    if (BeamAlloc != nullptr)
    {
        BeamEffectSetHitFlags(BeamAlloc, true, bOtherIsPlayer);
        BeamEffectRelease(BeamAlloc);
        BeamAlloc = nullptr;
    }
    // the routine(&EffectList, &Companion).
    CliEffectListRemove(&EffectList, Companion);
    // Projectile's slot 108 the routine at A, called directly.
    FClientProjectileGameobj::Slot1b0(static_cast<Vector const*>(A));
}

void FClientBeamGameobj::Slot1a4(void const* A, void const* B, FClientGameobj* Munition)
{
    // RET 0xc.
    (void)A;
    (void)B;
    (void)Munition;
}

void FClientBeamGameobj::Slot1ac(void const* A, void const* B, FClientGameobj* Other,
                                 void const* D)
{
    // (RET 0x10). B is not read.
    (void)B;
    // in multiplayer only the player's own shots report -- the
    // projectile's owner (+0xe4, class & 7 object, no null test) against the field.
    if (!SinglePlayer())
    {
        CProjectile const* const Shot = static_cast<CProjectile const*>(NarrowObject(this, 0x7u));
        if (Shot->OwnerId != GetPlayerCharacterId())
            return;
    }
    CObject* const Obj = NarrowObject(this, 0x7u);
    Vector const* const HitPos = static_cast<Vector const*>(A);
    SSPMunitionCollisionInfo Info;
    Info.Field_0x00 = Obj->Archetype->Nickname.Id;
    Info.Field_0x04 = static_cast<CProjectile const*>(Obj)->OwnerId;
    Info.ObjectId = Other->get_id();
    // the low word of D, the sub-object that was hit. +0x0e is never written.
    Info.Field_0x0c = static_cast<unsigned short>(reinterpret_cast<uintptr_t>(D));
    Info.Pad_0x0e = 0;
    std::memcpy(&Info.Field_0x10, &HitPos->x, sizeof(float));
    std::memcpy(&Info.Field_0x14, &HitPos->y, sizeof(float));
    std::memcpy(&Info.Field_0x18, &HitPos->z, sizeof(float));
    // the field slot 53 (+0xd4) SPMunitionCollision(Info,
    // ), no null test.
    LocalServer()->SPMunitionCollision(Info, GetPlayerConnectionId());
}

unsigned FClientBeamGameobj::Slot1bc() const
{
    // MOV EAX,0x2000000 / RET.
    return 0x2000000u;
}

// ===========================================================================
// Mine / CounterMeasure / Guided / Loot -- shared bodies
// ===========================================================================

namespace
{
    // / 005df858 -- the FDUMP file arguments of Loot slot 92 (0x0053e666) and
    // Guided slot 112 (0x0053f142).
    char const kLootSourceFile[] = "Client\\Space\\Loot.cpp";
    char const kMunitionSourceFile[] = "Client\\Space\\Munition.cpp";

    // The object-update tail of Loot / Mine / Guided / CounterMeasure slot 93 (0x0053e6af..
    // , 0x0053e9af.., 0x0053ecbf.., 0x0053000f..): the owner is the class & 3
    // object's +0xb4 OwnerPlayer, or the routine when that narrow fails. This connection's own
    // object runs its vt+0x50 update(Dt, UPDATE_DEFAULT); anyone else's runs UPDATE_DUMB, and
    // only in multiplayer. The Mask-narrowed object is used with no null test.
    void GameobjOwnerUpdate(FClientGameobj* Self, unsigned Mask, float Dt)
    {
        CObject* const Owned = NarrowObject(Self, 0x3u);
        unsigned const Owner = (Owned != nullptr) ? static_cast<CSimple const*>(Owned)->OwnerPlayer
                                                  : GetPlayerConnectionId();
        if (Owner == GetPlayerConnectionId())
        {
            NarrowObject(Self, Mask)->update(Dt, CObject::UPDATE_DEFAULT);
            return;
        }
        if (SinglePlayer())
            return;
        NarrowObject(Self, Mask)->update(Dt, CObject::UPDATE_DUMB);
    }

    // (RET 4, Dt unread) -- Mine, CounterMeasure and Guided slot 110. FCOMP 0.0 /
    // TEST AH,0x44 / JP: only an exactly-zero +0x34 falls through (greater, less and NaN jump
    // out). The nebula at get_position() goes to +0x38 and its fog factor to +0x3c
    // (the routine, no hint), then +0x34 = 1.0f.
    void ProjectileLazyNebula(FClientGameobj* Self)
    {
        if (!(Self->Field_0x34 == 0.0f))
            return;
        FNebulaEffect* const Nebula = StarSysFindNebula(
            &Self->get_position(), reinterpret_cast<float*>(&Self->DrawStateB), nullptr);
        Self->DrawStateA = static_cast<unsigned>(reinterpret_cast<uintptr_t>(Nebula));
        Self->Field_0x34 = 1.0f;
    }

    // (RET 0xc) -- Mine and Guided slot 105: a munition whose object is a CBeam
    // (class & 0x67) goes on to GameObject's slot 105 (the tail JMP to the routine with the
    // arguments rewritten in place, unchanged); a null munition, a munition without an object
    // or any other class ends here.
    bool IsBeamMunition(FClientGameobj const* Munition)
    {
        if (Munition == nullptr)
            return false;
        return NarrowObject(Munition, 0x67u) != nullptr;
    }
}

// ---- Mine ------------------------------------------------------------------

FClientMineGameobj::FClientMineGameobj()
    : FClientProjectileGameobj(EClientGameobjKind::Mine)
{
    // the routine(5), then the vtables 005dedcc / IObjAffect /
    // CBase 005ded44 (C++).
}

FClientMineGameobj::~FClientMineGameobj()
{
    // thunk_FUN_005425f0, Projectile's body; nothing of Mine's own.
}

Vector FClientMineGameobj::get_velocity() const
{
    // (RET 4): TEST byte [this+0x44],4 -- a destroyed mine answers {0,0,0};
    // otherwise IObjInspectImpl::get_velocity (the Common import at 0x0053eafd).
    if ((Flags & 4u) != 0)
    {
        Vector Zero;
        Zero.x = 0.0f;
        Zero.y = 0.0f;
        Zero.z = 0.0f;
        return Zero;
    }
    return IObjInspectImpl::get_velocity();
}

void FClientMineGameobj::create(Archetype::Root* Arch)
{
    // (RET 4).
    GameobjCreateOfClass(this, CObject::ClassMine, Arch);
    NarrowObject(this, 0xfu)->open(Arch);
}

bool FClientMineGameobj::find(unsigned const& Id)
{
    // (RET 4): CObject::Find(Id, 0xf) into +0x10, advise(true), SETNZ.
    Object = CObject::Find(Id, CObject::ClassMine);
    advise(true);
    return Object != nullptr;
}

void FClientMineGameobj::update(float Dt)
{
    // (RET 4): Projectile's update the routine, then the owner tail on class & 0xf.
    FClientProjectileGameobj::update(Dt);
    GameobjOwnerUpdate(this, 0xfu, Dt);
}

void FClientMineGameobj::Slot1a4(void const* A, void const* B, FClientGameobj* Munition)
{
    // (RET 0xc).
    if (!IsBeamMunition(Munition))
        return;
    FClientGameobj::Slot1a4(A, B, Munition);
}

void FClientMineGameobj::Slot1b8(float Dt)
{
    // (RET 4).
    (void)Dt;
    ProjectileLazyNebula(this);
}

// ---- CounterMeasure ----------------------------------------------------------

FClientCounterMeasureGameobj::FClientCounterMeasureGameobj()
    : FClientProjectileGameobj(EClientGameobjKind::CounterMeasure)
{
    // the routine(6), then the vtables 005df01c / IObjAffect /
    // CBase 005def94 (C++).
}

FClientCounterMeasureGameobj::~FClientCounterMeasureGameobj()
{
    // Projectile's body.
}

void FClientCounterMeasureGameobj::create(Archetype::Root* Arch)
{
    // (RET 4).
    GameobjCreateOfClass(this, CObject::ClassCounterMeasure, Arch);
    NarrowObject(this, 0x17u)->open(Arch);
}

bool FClientCounterMeasureGameobj::find(unsigned const& Id)
{
    // (RET 4).
    Object = CObject::Find(Id, CObject::ClassCounterMeasure);
    advise(true);
    return Object != nullptr;
}

void FClientCounterMeasureGameobj::update(float Dt)
{
    // (RET 4).
    FClientProjectileGameobj::update(Dt);
    GameobjOwnerUpdate(this, 0x17u, Dt);
}

void FClientCounterMeasureGameobj::Slot1a4(void const* A, void const* B, FClientGameobj* Munition)
{
    // RET 0xc.
    (void)A;
    (void)B;
    (void)Munition;
}

void FClientCounterMeasureGameobj::Slot1b8(float Dt)
{
    // (RET 4).
    (void)Dt;
    ProjectileLazyNebula(this);
}

// ---- Guided ----------------------------------------------------------------

FClientGuidedGameobj::FClientGuidedGameobj()
    : FClientMunitionGameobj(EClientGameobjKind::Guided)
{
    // the routine(4) -- Projectile's ctor; Munition's ctor
    // adds only its vtables, which Guided's overwrite at once -- then the vtables
    // / IObjAffect 005df1ec / CBase 005df1e4 (C++).
}

FClientGuidedGameobj::~FClientGuidedGameobj()
{
    // Projectile's body.
}

void FClientGuidedGameobj::create(Archetype::Root* Arch)
{
    // (RET 4).
    GameobjCreateOfClass(this, CObject::ClassGuided, Arch);
    NarrowObject(this, 0xa7u)->open(Arch);
}

bool FClientGuidedGameobj::find(unsigned const& Id)
{
    // (RET 4).
    Object = CObject::Find(Id, CObject::ClassGuided);
    advise(true);
    return Object != nullptr;
}

void FClientGuidedGameobj::update(float Dt)
{
    // (RET 4).
    FClientProjectileGameobj::update(Dt);
    GameobjOwnerUpdate(this, 0xa7u, Dt);
}

void FClientGuidedGameobj::Slot1a4(void const* A, void const* B, FClientGameobj* Munition)
{
    // (RET 0xc) -- Mine's slot 105.
    if (!IsBeamMunition(Munition))
        return;
    FClientGameobj::Slot1a4(A, B, Munition);
}

void FClientGuidedGameobj::Slot1ac(void const* A, void const* B, FClientGameobj* Other,
                                   void const* D)
{
    // (RET 0x10). B is not read.
    (void)B;
    // in multiplayer only a collision with an object this connection
    // owns reports -- Other's class & 3 object's +0xb4 OwnerPlayer, or the routine when the
    // narrow fails, against the field.
    if (!SinglePlayer())
    {
        CObject* const OtherObj = NarrowObject(Other, 0x3u);
        unsigned const Owner = (OtherObj != nullptr)
                                   ? static_cast<CSimple const*>(OtherObj)->OwnerPlayer
                                   : GetPlayerConnectionId();
        if (Owner != GetPlayerConnectionId())
            return;
    }
    // this class & 7 object (no null test) -- its +0xe4 owner and its
    // archetype's nickname id; Other's get_id; D's low word; the hit point A.
    CObject* const Obj = NarrowObject(this, 0x7u);
    Vector const* const HitPos = static_cast<Vector const*>(A);
    SSPMunitionCollisionInfo Info;
    Info.Field_0x04 = static_cast<CProjectile const*>(Obj)->OwnerId;
    Info.Field_0x00 = Obj->Archetype->Nickname.Id;
    Info.ObjectId = Other->get_id();
    Info.Field_0x0c = static_cast<unsigned short>(reinterpret_cast<uintptr_t>(D));
    Info.Pad_0x0e = 0; // +0x0e is never written
    std::memcpy(&Info.Field_0x10, &HitPos->x, sizeof(float));
    std::memcpy(&Info.Field_0x14, &HitPos->y, sizeof(float));
    std::memcpy(&Info.Field_0x18, &HitPos->z, sizeof(float));
    // the field slot 53 (+0xd4) SPMunitionCollision(Info,
    // ), no null test.
    LocalServer()->SPMunitionCollision(Info, GetPlayerConnectionId());
}

void FClientGuidedGameobj::Slot1b0(Vector const* Pos)
{
    // (RET 4). 0x0053edc3..0x0053eddd: with a const_effect particle (+0xb8),
    // (0, -1, frame) on it -- no owner, no part, the frame this gameobj's vt+0x10
    // returns. Slot 4 (get_orientation) is ICF-folded with slot 3 (get_transform) in baseline
    // (both are Common 0x5F1F0), so the pointer it hands over is the object's whole Transform.
    FParticleEffect* const Particle = static_cast<FParticleEffect*>(OneShotCompanion[2]);
    if (Particle != nullptr)
    {
        Transform const& Frame = get_transform();
        ParticleEffectAttach(Particle, nullptr, -1, reinterpret_cast<FTransform const*>(&Frame));
    }
    // Projectile's slot 108 the routine, called directly.
    FClientProjectileGameobj::Slot1b0(Pos);
}

void FClientGuidedGameobj::Slot1b8(float Dt)
{
    // (RET 4).
    (void)Dt;
    ProjectileLazyNebula(this);
}

unsigned FClientGuidedGameobj::Slot1bc() const
{
    // MOV EAX,0x1000000 / RET.
    return 0x1000000u;
}

void FClientGuidedGameobj::Slot1c0(Transform* Out)
{
    // (RET 4).
    // the local frame -- the routine (identity rotation), zero
    // translation.
    Transform Local;
    Local.Rotation = Matrix(1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    Local.Translation.x = 0.0f;
    Local.Translation.y = 0.0f;
    Local.Translation.z = 0.0f;

    // the class & 7 object (no null test before +0x88), its archetype's
    // companion (a CliMunition), the HP_trail_parent text at +0x60 -- tested by its first byte.
    char const* const TrailParent =
        Archetype::CliMunitionHpTrailParent(NarrowObject(this, 0x7u)->Archetype->Companion);
    if (TrailParent[0] != '\0')
    {
        // FindHardpoint_PS(get_index(), name, &Handle (-1), &Info).
        long Handle = -1;
        HardpointInfo Hp;
        if (FindHardpoint_PS(get_index(), TrailParent, Handle, Hp))
        {
            // the hardpoint's position and orientation become the
            // frame, and the renderer is not asked.
            Local.Translation = Hp.Position;
            Local.Rotation = Hp.Orient;
            *Out = Local;
            return;
        }
        // FDUMP 0x100001, Munition.cpp line 0xe1, the raw object's
        // archetype nickname and the HP_trail_parent text (read again through the class & 7
        // object); then on to the renderer.
        char const* const TrailParentAgain =
            Archetype::CliMunitionHpTrailParent(NarrowObject(this, 0x7u)->Archetype->Companion);
        FDUMP(0x100001, "%s(%d) : *** ERROR: %s: Missing HP_trail_parent(%s).",
              kMunitionSourceFile, 0xe1, Object->get_archetype()->Nickname.get_string(),
              TrailParentAgain);
    }

    // the raw object's archetype mesh (+0x40) on NeuronRuntime::Renderer --
    // IRenderer2 slot 4 (+0x10) the six extents and slot 5 (+0x14) the centre, both at LOD
    // 1.0; when both answer, the translation is {centre x, centre y, extents[4]}.
    float Extents[6];
    void* const MeshId =
        reinterpret_cast<void*>(static_cast<intptr_t>(Object->get_archetype()->EngineIndex));
    if (NeuronRuntime::Renderer->GetBounds(MeshId, 1.0f, Extents) != 0)
    {
        float Center[3];
        void* const MeshIdAgain =
            reinterpret_cast<void*>(static_cast<intptr_t>(Object->get_archetype()->EngineIndex));
        if (NeuronRuntime::Renderer->GetCentroid(MeshIdAgain, 1.0f, Center) != 0)
        {
            Local.Translation.x = Center[0];
            Local.Translation.y = Center[1];
            Local.Translation.z = Extents[4];
        }
    }
    // the 0x30-byte frame copied out.
    *Out = Local;
}

int FClientGuidedGameobj::set_target(IObjRW* Target, unsigned short SubTarget, int SourceClient)
{
    // (RET 0xc; `this` is the IObjAffect sub-object in baseline). The class & 0xa7
    // object is used with no null test.
    CGuided* const Guided = static_cast<CGuided*>(NarrowObject(this, 0xa7u));
    // a munition with no seeker (+0x90) answers -3.
    Archetype::Munition const* const MunArch =
        static_cast<Archetype::Munition const*>(Guided->Archetype);
    if (MunArch->Seeker == 0)
        return -3;

    // the current target (the watcher at +0xf0, its Watchable minus
    // eight) and sub-target (+0xf8) both unchanged -> 0.
    Watchable const* const Watched = Guided->TargetWatch.target();
    IObjRW const* const Current =
        (Watched != nullptr)
            ? reinterpret_cast<IObjRW const*>(reinterpret_cast<unsigned char const*>(Watched) - 8)
            : nullptr;
    if (Current == Target && Guided->SubTarget == SubTarget)
        return 0;

    // the request is this connection's when SourceClient is -1 or
    // (baseline keeps the flag in Target's argument slot).
    bool const bOwnRequest =
        (SourceClient == -1) || (static_cast<unsigned>(SourceClient) == GetPlayerConnectionId());
    // in single player a foreign request changes nothing.
    if (SinglePlayer() && !bOwnRequest)
        return 0;

    // the watcher moves to Target's Watchable (IObjRW+8, null for a null
    // Target) when it differs -- BaseWatcher::set_pointer after the inline compare, which is
    // BaseWatcher::set. The seeker is re-read before each of the two stores.
    if (static_cast<Archetype::Munition const*>(Guided->Archetype)->Seeker != 0)
    {
        Watchable const* const NewWatched =
            (Target != nullptr) ? reinterpret_cast<Watchable const*>(
                                      reinterpret_cast<unsigned char const*>(Target) + 8)
                                : nullptr;
        Guided->TargetWatch.set(NewWatched);
    }
    // +0xf8 = SubTarget.
    if (static_cast<Archetype::Munition const*>(Guided->Archetype)->Seeker != 0)
        Guided->SubTarget = SubTarget;

    // this connection's request goes to the server --
    // slot 4 (+0x10) SetTarget(the field, {this id, SUBOBJ_ID_NONE,
    // Target's id or 0, SubTarget}). The pad words are never written.
    if (bOwnRequest)
    {
        XSetTarget Info;
        Info.ObjectId = get_id();
        Info.SubObjId = SUBOBJ_ID_NONE;
        Info.Pad_0x06 = 0;
        Info.TargetObjectId = (Target != nullptr) ? Target->get_id() : 0u;
        Info.TargetSubObjId = SubTarget;
        Info.Pad_0x0e = 0;
        LocalServer()->SetTarget(GetPlayerConnectionId(), Info);
    }
    return 0;
}

// ---- Loot ------------------------------------------------------------------

FClientLootGameobj::FClientLootGameobj()
    : FClientSimpleGameobj(EClientGameobjKind::Loot)
{
    // the routine(7), +0xac = 0 (0x0053e47a), then the vtables 005df5d4 /
    // IObjAffect / CBase 005df548 (C++).
    TractoredExplosionArch = nullptr;
}

FClientLootGameobj::~FClientLootGameobj()
{
    // (ESI = this + 0xc): the tractored explosion released (the routine) when
    // there is one (0x0053e511..0x0053e529), then GameObject's body the routine through the
    // base chain.
    if (TractoredExplosionArch != nullptr)
        ExplosionArchRelease(TractoredExplosionArch);
}

void FClientLootGameobj::create(Archetype::Root* Arch)
{
    // (RET 4).
    GameobjCreateOfClass(this, CObject::ClassLoot, Arch);
    NarrowObject(this, 0x803u)->open(Arch);
}

bool FClientLootGameobj::find(unsigned const& Id)
{
    // (RET 4).
    Object = CObject::Find(Id, CObject::ClassLoot);
    advise(true);
    return Object != nullptr;
}

void FClientLootGameobj::Slot170()
{
    // .
    FClientGameobj::Slot170();
    // the class & 0x803 object's archetype (no null test), a
    // LootCrate / Equipment, and its +0x68 tractored_explosion.
    Archetype::Equipment const* const Crate =
        static_cast<Archetype::Equipment const*>(NarrowObject(this, 0x803u)->get_archetype());
    ID_String const* const ExplosionId = &Crate->TractoredExplosion;
    if (!ExplosionId->IsEmpty())
    {
        TractoredExplosionArch = Archetype::GetExplosion(*ExplosionId);
        if (TractoredExplosionArch != nullptr)
        {
            ExplosionArchAddRef(TractoredExplosionArch);
        }
        else
        {
            // FDUMP 0x100002, Loot.cpp line 0x47, the raw object's
            // archetype nickname and the explosion id's string (fetched first).
            char const* const ExplName = ExplosionId->get_string();
            char const* const ArchName = Object->get_archetype()->Nickname.get_string();
            FDUMP(0x100002,
                  "%s(%d) : *** WARNING: Archtype(%s) tractored_explosion(%s) not found.",
                  kLootSourceFile, 0x47, ArchName, ExplName);
        }
    }
    // the routine(&get_position(), &DrawStateB, 0) into DrawStateA.
    FNebulaEffect* const Nebula =
        StarSysFindNebula(&get_position(), reinterpret_cast<float*>(&DrawStateB), nullptr);
    DrawStateA = static_cast<unsigned>(reinterpret_cast<uintptr_t>(Nebula));
}

void FClientLootGameobj::update(float Dt)
{
    // (RET 4): GameObject's update the routine, then the owner tail on 0x803.
    FClientGameobj::update(Dt);
    GameobjOwnerUpdate(this, 0x803u, Dt);
}

void FClientLootGameobj::Slot198(DamageList const& Damage)
{
    // (RET 4): multiplayer only.
    if (SinglePlayer())
        return;
    for (std::list<DamageEntry>::const_iterator It = Damage.Entries.begin();
         It != Damage.Entries.end(); ++It)
    {
        // the root entry -> the CLoot's vt+0x98 set_hit_pts.
        if (It->SubObjId == SUBOBJ_ID_ROOT)
        {
            static_cast<CSimple*>(NarrowObject(this, 0x803u))->set_hit_pts(It->HitPts);
            continue;
        }
        // any other entry. FCOMP 0.0 / TEST AH,0x41 / JNZ: only a value
        // above 0 falls through and becomes the contents' hit points (+0xf4); 0, below and NaN
        // cost a unit (+0xf0) and refill +0xf4 from the contents archetype's hit_pts (+0x1c).
        CLoot* const Loot = static_cast<CLoot*>(NarrowObject(this, 0x803u));
        if (It->HitPts > 0.0f)
        {
            Loot->ContentsHitPts = It->HitPts;
        }
        else
        {
            Loot->Units = Loot->Units - 1u;
            Loot->ContentsHitPts = Loot->ContentsArch->HitPts;
        }
    }
}

void FClientLootGameobj::Slot19c(int DestroyType)
{
    // (RET 4).
    if (DestroyType != 2)
    {
        FClientSimpleGameobj::Slot19c(DestroyType);
        return;
    }
    // tractored in -- no explosion: slot 87 with true; else
    // (TractoredExplosionArch, this, &get_position()) into ActionDB::add.
    if (TractoredExplosionArch == nullptr)
    {
        Slot15c(true);
        return;
    }
    Actions.add(Action_Explode::Create(TractoredExplosionArch, this, &get_position()));
}

FClientGameobj* ClientGameobj_CreateSolar()
{
    return new FClientSolarGameobj();
}


namespace
{
    // / the field. The static initialiser the branch allocates the empty
    // list head and registers the branch with atexit, which destroys every node's watcher
    // (the routine) and frees the head; a static std::list does both.
    std::list<BaseWatcher> G_GameobjWatchList;

    // The IObjRW a list watcher points at: its Watchable sub-object sits at IObjRW+8, and a
    // watcher with nothing behind it compares as null (0x0045aa6c..0045aa80).
    IObjRW const* WatchedGameobj(BaseWatcher const& Watcher)
    {
        Watchable const* const Target = Watcher.target();
        if (Target == nullptr)
            return nullptr;
        return reinterpret_cast<IObjRW const*>(reinterpret_cast<unsigned char const*>(Target) - 8);
    }

    // (out, first, last, &Obj): the first node from first up to last whose
    // watched object equals Obj. baseline builds a temporary BaseWatcher on Obj for every node
    // and compares the two targets minus eight; comparing against Obj itself is the same test.
    std::list<BaseWatcher>::iterator FindWatchedGameobj(IObjRW const* Obj)
    {
        std::list<BaseWatcher>::iterator It = G_GameobjWatchList.begin();
        for (; It != G_GameobjWatchList.end(); ++It)
        {
            if (WatchedGameobj(*It) == Obj)
                break;
        }
        return It;
    }
}

void GameobjWatchListAdd(IObjRW* Obj)
{
    // . 0x0045a8bc: the routine over the whole list; an existing entry ends it.
    // Otherwise 0x0045a8e5..0045a958: a watcher on Obj (+8 when Obj is non-null) is copied
    // into a new node linked in before the head (push_back; the routine is that copy) and
    // goes up by one.
    if (FindWatchedGameobj(Obj) != G_GameobjWatchList.end())
        return;
    // BaseWatcher(IObjRW*) is not a Common export, so the node is default-constructed and then
    // pointed at Obj+8 the way that constructor does it (set on an empty watcher is
    // set_pointer).
    G_GameobjWatchList.emplace_back();
    if (Obj != nullptr)
    {
        Watchable const* const Target =
            reinterpret_cast<Watchable const*>(reinterpret_cast<unsigned char const*>(Obj) + 8);
        G_GameobjWatchList.back().set(Target);
    }
}

void GameobjWatchListRemove(IObjRW* Obj)
{
    // . 0x0045a9a5: the same find; a hit is unlinked, its watcher released
    // (the routine: set_pointer(0) when it watches something), the node freed and
    // decremented.
    std::list<BaseWatcher>::iterator const It = FindWatchedGameobj(Obj);
    if (It == G_GameobjWatchList.end())
        return;
    G_GameobjWatchList.erase(It);
}

std::list<BaseWatcher> const& GameobjWatchList()
{
    return G_GameobjWatchList;
}

void ExplosionArchAddRef(Archetype::Explosion const* Expl)
{
    // (cdecl). 0x00536106: null explosion -> nothing. 0x0053610a..00536131: every
    // effect entry (baseline std::list at +0x24; the port keeps a vector) -> the routine on the
    // entry's effect id, and a hit gets the routine(1). 0x00536133..0053615d: every innards
    // debris id (vector +0x44..+0x48) -> Archetype::GetSimple, and a hit gets Root vt+0x0c
    // (slot 3, load).
    if (Expl == nullptr)
        return;
    for (size_t Index = 0; Index < Expl->Effects.size(); ++Index)
    {
        FEffectArch* const Fx = FindEffectArch(&Expl->Effects[Index].Fx.Id);
        if (Fx != nullptr)
            EffectArchAddRef(Fx, 1u);
    }
    for (size_t Index = 0; Index < Expl->InnardsDebrisObjects.size(); ++Index)
    {
        Archetype::Root* const Simple = Archetype::GetSimple(Expl->InnardsDebrisObjects[Index]);
        if (Simple != nullptr)
            Simple->load();
    }
}

void ExplosionArchRelease(Archetype::Explosion const* Expl)
{
    // (cdecl). Same two walks as the routine: a found effect gets the routine,
    // a found innards simple gets Archetype::Root::Release (0x005361c5, the Common import --
    // not a virtual call).
    if (Expl == nullptr)
        return;
    for (size_t Index = 0; Index < Expl->Effects.size(); ++Index)
    {
        FEffectArch* const Fx = FindEffectArch(&Expl->Effects[Index].Fx.Id);
        if (Fx != nullptr)
            EffectArchRelease(Fx);
    }
    for (size_t Index = 0; Index < Expl->InnardsDebrisObjects.size(); ++Index)
    {
        Archetype::Root* const Simple = Archetype::GetSimple(Expl->InnardsDebrisObjects[Index]);
        if (Simple != nullptr)
            Simple->Release();
    }
}
