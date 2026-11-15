// Author: Giperion.

#pragma once

#include "beam_effect.h"
#include "client_equip_fx.h" // FClientEquip
#include "vis_effect_arch.h"
#include "x86math.h"

#include "common_cequip.h"
#include "common_csys.h"
#include "common_cobject.h"
#include "common_id.h"
#include "engine_light.h"

#include <cstddef>

class FClientGameobj;     // client_gameobj.h
class FClientBeamGameobj; // client_gameobj.h -- baseline Beam (0xd4, the routine alloc)
struct FParticleEffect;   // particle_effect.h
struct ISoundSource;      // soundmanager.h

// CliLauncher fire host — baseline CliLauncher.cpp (the routine / the routine).
// A ClientEquip (client_equip_fx.h): Equip +0x04 is the CELauncher, Host +0x08 the ship's
// gameobj, SeparationExplosion +0x0c. sizeof 0x40 for the launcher / mine / CM (operator
// new(0x40) at 0x005331ea / 0x005332e6; the routine zeroes through +0x3c) and 0x44 for the
// gun (operator new(0x44) at 0x005333ea; its +0x40 lives in FCliGunFx). baseline never builds a
// plain launcher: the routine's only callers are the mine / CM / gun constructors.

// baseline vtables (six slots each; slots 3..5 are the fire hooks TryFire / FireBarrelAim /
// FireDirect):
//   launcher the branch {the branch, the routine, the routine, the routine,
// , the routine}
//   mine     the routine {the routine, the routine, the routine, ...same 3..5}
//   CM       the routine {the routine, the routine, the routine, ...same 3..5}
//   gun      the branch {the branch, the routine, the routine, the routine,
// , the routine}
// (the routine is the scalar deleting destructor over the same body the routine.)

// / the routine's unwind maps (FuncInfo 005fa4dc / 005fa510) name two member
// objects with destructors after the ClientEquip base (the routine): the +0x10 use-animation
// handle (the routine) and the +0x30 effect companion (the routine). Constructed in that
// order, destroyed in reverse after the class body.

// The +0x10 use-animation playback handle, -1 for none.
struct FLauncherAnimHandle
{
    int Value; // IAnimation PlayScript handle
    // a handle other than -1 -> IAnimation vtable +0x38 (StopPlayback) and +0x28
    // (DestroyScript) on it, then -1. No AnimDB::Rem (unlike the class body's release).
    ~FLauncherAnimHandle();
};

// The +0x30 0x10-byte effect companion {arch, ISoundSource*, effect, FGameLight*}.
struct FLauncherEffectCompanion
{
    void* Slots[4];
    // (CliEffectCompanionClear).
    ~FLauncherEffectCompanion();
};

struct FCliLauncherFx : FClientEquip
{
    // The +0x04 equipment as the launcher it always is here (the routine's argument).
    CELauncher* LaunchEquip() const { return static_cast<CELauncher*>(Equip); }

    // Fields as the routine fills them and the routine releases them.
    FLauncherAnimHandle AnimJointIndex;        // +0x10  use_animation PlayScript handle, -1 none
    void* DryFireSoundArch;                    // +0x14  launcher companion +0x80, Acquired
    void* OneShotSoundArch;                    // +0x18  munition companion +0x5c, Acquired
    ISoundSource* FireVoice;                   // +0x1c  made on first shot by the routine
    FEffectArch* ConstEffectArch;              // +0x20  munition companion +0x58, AddRef'd
    Archetype::Explosion* MunitionExplosion;   // +0x24  munition explosion_arch, AddRef'd
    void* MuzzleEffect;                        // +0x28  live particle (PlayMuzzleFx)
    FEffectArch* DefaultEffectArch;            // +0x2c  "multi_sparks02" (the field)
    // +0x30 the 0x10-byte effect companion {arch, ISoundSource*, effect, FGameLight*} the
    // Update fills with "multi_sparks02" (the routine, 0x0052ce6b).
    FLauncherEffectCompanion Companion;

    FCliLauncherFx();
    // (the body of the scalar deleting the routine / the routine).
    ~FCliLauncherFx() override;

    // (RET 8): code 0xa -- the use-animation dropped, the companion out of the
    // host's effect list, the muzzle effect killed and released -- then the routine.
    void notify(Event Code, void* Param) override;
    // (RET 4).
    bool Update(float Dt) override;

    // The three fire slots every launcher-family table adds after Update (see the tables
    // above). baseline names are unknown; these names say what each does.
    // Slot 3 -- the routine (RET 4): CELauncher::Fire (vtable +0x90) at Target; FireOk plays
    // the muzzle, consumes, updates the player's ammo and battle music (true); the refusals
    // play the dry-fire sound or notify the player's ship (false). Gun: the routine.
    virtual unsigned char TryFire(Vector const* Target);
    // Slot 4 -- the routine (no args): aim 1e10 along the averaged barrel direction, slot 3
    // on it, and on success IServer::FireWeapon (the field slot 0). Shared by every table.
    virtual unsigned char FireBarrelAim();
    // Slot 5 -- the routine (RET 4, Target unread): with an equipment, the muzzle, the
    // multiplayer resource consumption and the battle-music note. Gun: the routine.
    virtual void FireDirect(Vector const* Target);

    // (RET 4, returns through a hidden ID_String): the one-shot sound's linked
    // archetype (slot 12) nickname id (+0x04), or a cleared id -- the key the routine groups
    // the launchers that fired together by.
    ID_String OneShotSoundId() const;
    // (RET 8): with a one-shot sound (+0x18), the fire voice (+0x1c, made on first
    // use) goes to Pos, attenuated by the archetype's attenuation times (ShotCount - 1),
    // negated, pitched by its random pitch, and started (idle) or rewound (playing).
    void PlayFireSound(Vector const* Pos, int ShotCount);

protected:
    // The code-0xa body the branch shares with the mine / CM / gun notifies (each inlines it
    // after its own player voice line): 0x0052ccbd..0x0052cd1c.
    void DropOnDestroying();
};
static_assert(sizeof(FCliLauncherFx) == 0x40, "CliLauncherFx 0x40");
static_assert(offsetof(FCliLauncherFx, AnimJointIndex) == 0x10, "AnimJoint @ +0x10");
static_assert(offsetof(FCliLauncherFx, FireVoice) == 0x1c, "FireVoice @ +0x1c");
static_assert(offsetof(FCliLauncherFx, MuzzleEffect) == 0x28, "MuzzleEffect @ +0x28");
static_assert(offsetof(FCliLauncherFx, DefaultEffectArch) == 0x2c, "sparks arch @ +0x2c");
static_assert(offsetof(FCliLauncherFx, Companion) == 0x30, "companion @ +0x30");

// the mine dropper -- the launcher with vtable the routine.
struct FCliMineDropperFx : FCliLauncherFx
{
    // (RET 8).
    void notify(Event Code, void* Param) override;
};

// the countermeasure dropper -- vtable the routine.
struct FCliCmDropperFx : FCliLauncherFx
{
    // (RET 8).
    void notify(Event Code, void* Param) override;
};

// the gun -- vtable the branch, +0x40 its turret-motion voice.
struct FCliGunFx : FCliLauncherFx
{
    // (RET 8).
    void notify(Event Code, void* Param) override;
    // (RET 4).
    bool Update(float Dt) override;
    // Slot 3 -- the routine (RET 4): the routine (non-virtual), then an unguided munition
    // (Seeker +0x90 == 0) launches its projectiles (the routine).
    unsigned char TryFire(Vector const* Target) override;
    // Slot 5 -- the routine (RET 4): the routine's body, then the target point for an owned
    // host and the projectiles of an unguided munition.
    void FireDirect(Vector const* Target) override;

    // +0x40 the looping turret-motion voice (the routine). No destructor of the gun releases
    // it: the gun's scalar deleting destructor the routine runs only the routine.
    ISoundSource* TurretVoice;
};
static_assert(sizeof(FCliGunFx) == 0x44, "CliGunFx 0x44");

// — MineDropper (base + Mine vtable stamp).
FCliLauncherFx* CliMineEquipFxConstruct(CEquip* Equip, void* Host);
// — CmDropper.
FCliLauncherFx* CliCmEquipFxConstruct(CEquip* Equip, void* Host);
// — Gun arm (TurretVoice Source3D via CreateClientSoundSource3D).
FCliLauncherFx* CliGunEquipFxConstruct(CEquip* Equip, void* Host);

// Gun archetype Companion (ICliObj) — muzzle fields used by the routine.
// baseline Gun::ReadUnknown: flash_particle_name@+0x54, flash_radius@+0x58,
// light_anim name@+0x60 (char buf) with length@+0x5c (0 = no anim).
struct FCliGunFxCompanion
{
    unsigned char Pad00[0x54];
    ID_String OneShotEffect; // +0x54  flash_particle_name
    float FlashRadius;       // +0x58  the routine Range
    unsigned LightAnimLen;   // +0x5c  0 → skip player-light arm
    char LightAnimName[32];  // +0x60  the routine (NOT ID_String)
};
static_assert(offsetof(FCliGunFxCompanion, OneShotEffect) == 0x54, "gun fx @ +0x54");
static_assert(offsetof(FCliGunFxCompanion, FlashRadius) == 0x58, "flash radius @ +0x58");
static_assert(offsetof(FCliGunFxCompanion, LightAnimLen) == 0x5c, "anim len @ +0x5c");
static_assert(offsetof(FCliGunFxCompanion, LightAnimName) == 0x60, "anim name @ +0x60");

class FClientEquippedObj; // client_gameobj.h

// Client equip-update packet (10 dwords) — the routine / the routine / the routine.
struct FCliEquipFirePacket
{
    int Op;                      // +0x00  0x18 fire / 0x19 activate / 0x1a|0x1b→0x19 / 0x1c / 0xbb
    unsigned char Opaque04[0xc];
    unsigned short HardpointId;  // +0x10  0 → null ids / count 0
    unsigned short Pad12;
    int ActivateSubOp;           // +0x14  0x19 cases 0..4; rewritten by the routine to 3
    Vector TargetPos;            // +0x18  also 0x1c payload dword pair with +0x14
    unsigned char Pad24[4];      // +0x24  tenth dword of the 10-dword copy
};
static_assert(offsetof(FCliEquipFirePacket, HardpointId) == 0x10, "fire pkt hp @ +0x10");
static_assert(offsetof(FCliEquipFirePacket, ActivateSubOp) == 0x14, "activate sub @ +0x14");
static_assert(offsetof(FCliEquipFirePacket, TargetPos) == 0x18, "fire pkt tgt @ +0x18");
static_assert(sizeof(FCliEquipFirePacket) == 0x28, "fire pkt 0x28 (10 dwords)");


void BeamEffectSetProgressFromSpeed(FBeamEffect* This, float Speed);
// (thiscall, RET 8) -- Flags70 bit 1 = bHit, bit 2 = bHitPlayer (the other bits
// kept). Caller: Beam slot 101 the branch (0x0053f738) with (1, Other == player).
void BeamEffectSetHitFlags(FBeamEffect* This, bool bHit, bool bHitPlayer);

// (thiscall on the 0x10 companion, RET 0x10) -- +0x00 Arch, +0x04 Sound,
// +0x08 Particle, +0x0c Light (baseline argument order: Arch, Particle, Sound, Light), AddRef
// each non-null one. Callers: Beam activate the routine (Beam +0xc0) and the effect paths.
void CliEffectCompanionInit(void* Companion16, void* Arch, FEffectBase* Particle,
                            ISoundSource* Sound, FGameLight* Light);

// — insert payload (0x10) into intrusive list at ListOwner+0x28.
// Payload layout matches the routine: [0] arch/slot0, [1] sound, [2] particle/beam, [3] light.
void CliEffectListInsert(void* ListOwnerPlus28, void const* Payload16);

// — find matching payload in list at ListOwner+0x28, ArmDeath/stop/release,
// clear companion (the routine), return 1 if found.
unsigned char CliEffectListRemove(void* ListOwnerPlus28, void* Payload16);

// — release particle(+8)/sound(+4)/light(+0xc) and zero 0x10 companion.
void CliEffectCompanionClear(void* Companion16);

// The inline stop sequence that precedes a companion clear (the routine, the routine,
// , the routine): particle vt+4 (ArmDeath), sound vt+0x94(-1, identity pose) and
// vt+0x60 (Stop), light the routine then vt+8 (disable).
void CliEffectCompanionArmStop(void* Companion16);

// (thiscall on the list at ListOwner+0x28, RET 4) — stop and release every
// node's companion (or, with a non-null ArchFilter, only the nodes whose slot 0 is non-null and
// holds ArchFilter in its first dword), then erase EVERY node. Callers: GameObject's dtor
// (0x00539565, 0x0053961e), the list's own dtor the routine (0x00539407) and slot 108
// (0x0053a892).
void CliEffectListClear(void* ListOwnerPlus28, void* ArchFilter);

// (thiscall on the list at ListOwner+0x28, RET 8) — every node whose engine
// instance is PartIndex or a descendant leaves the list: with NewOwner, a still-alive one is
// moved to the end of NewOwner's list (the routine + the routine); the rest are stopped and
// released. Callers: EquippedObj notify 5/6/7/8 (0x005347c7, 0x00534842), the routine.
void CliEffectListDetachPart(void* ListOwnerPlus28, long PartIndex, FClientGameobj* NewOwner);

// — copy 4 pointers + AddRef particle(+8) / sound(+4) / light(+0xc).
void CliEffectListCopyPayload(void* DstSlots, void const* SrcSlots);

// — copy velocity/offset float[3] into GameLight+0x120.
void GameLightSetFreeMotionOffset(FGameLight* Light, float const* Offset3);

// — explosion FX local transform (when SlotExplosionFx non-null).
void CliExplosionAttachLocalXform(void* ExplosionFx, int EngineIndex, FTransform const* LocalXform);

// (thiscall on the Beam, RET 8, non-virtual) -- the Beam's activate: beam effect
// + light into the +0xc0 companion, insert it into the effect list, then Projectile slot 92
// called directly. Caller: CliLauncher fire the routine.
void CliBeamActivateEffects(FClientBeamGameobj* This, Vector const* LookDir,
                            unsigned char bUseLookDir);

// — CliLauncher fire → new Beam → create/init → activate → register.
void CliLauncherFireProjectiles(FCliLauncherFx* Launcher, Vector const* Target);

// — muzzle: AnimDB / player gamelit / ParticleEffect(004f8060+004f8160).
void CliLauncherPlayMuzzleFx(FCliLauncherFx* Launcher);

// The fire hooks the routine / the routine / the routine / the routine / the routine are
// the FCliLauncherFx / FCliGunFx virtuals TryFire / FireBarrelAim / FireDirect above.

// — the EquippedObj IObjAffect slot 13 (+0x34) fire dispatcher; AdjustorThis is
// the host's IObjAffect sub-object (Host+4). Local shots take the wrappers' slot 3 / 4 and send
// IServer::FireWeapon, another player's take slot 5 (multiplayer: sub-target re-aim and the
// target-relative point turned into world space); then shared one-shot sounds play once.
// Returns 0.
unsigned CliShipFireWeapons(void* AdjustorThis, unsigned short const* HardpointIds,
                            unsigned HardpointCount, Vector const* TargetPos, int PlayerId);

// — EquippedObj adjustor vt+0x4c ActivateEquip(id*, bOn, playerId). Returns
// baseline's EAX: 0 on every path (0x00535459).
int CliShipActivateEquip(void* AdjustorThis, unsigned short const* HardpointId,
                         unsigned char bOn, int PlayerId);

// — EquippedObj message: 0x18 fire, 0x19 ActivateEquip cases 0..4.
// Returns 1 if handled (0x18), 0 otherwise (baseline 0x19 leaves local_29=0).
unsigned char EquippedObjHandleEquipMsg(FClientEquippedObj* Host,
                                        FCliEquipFirePacket const* Packet);

// — preprocess: op 0x1a/0x1b → 0x19 + next launcher hardpoint (subop 3).
void CliShipPreprocessEquipUpdate(FClientEquippedObj* Host, FCliEquipFirePacket* Packet);

// — CShip equip-update: gate + copy + preprocess + EquippedObjHandleEquipMsg.
// 0x1c → FindById + Host+4 vt+0x48 (the branch) + the routine walk; 0xbb → LeadAimFire.
unsigned char CliShipHandleEquipUpdate(FClientEquippedObj* Host,
                                       FCliEquipFirePacket const* Packet);

// — lead-aim → Host+4 vt+0x34 (CliShipFireWeapons).
void CliShipLeadAimFire(FClientEquippedObj* Host);

// — CShip IObjAffect vt+0x48 set_target(target, sub, player), RET 0xc.
// AdjustorThis = Host+4.
int CliShipSetTarget(void* AdjustorThis, IObjRW* Target, unsigned short SubId, int PlayerId);

// — EquippedObj IObjAffect vt+0x44 (hp*, target, sub, player), RET 0x10.
// AdjustorThis = Host+4.
int CliEquippedObjSetGunTarget(void* AdjustorThis, unsigned short const* HardpointId,
                               IObjRW* Target, unsigned short SubId, int PlayerId);

// — ClientObjDb_FindObject (client_objdb.h).
// — per-gun SetTarget + IServer::SetTarget (wired in 0x1c walk).
// — CShip Host+4 vt+0x48 set_target/sub + IServer::SetTarget.
