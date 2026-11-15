// Author: Giperion.

#pragma once

#include "beam_effect.h"
#include "effect_base.h" // FEffectBase (FCameraParticles head)
#include "vis_effect_arch.h"
#include "x86math.h"

#include "particlefx.h"
#include "common_goods.h"

#include <cstddef>

struct FGameLight; // engine_light.h

// baseline ParticleEffect — RTTI / Effect.cpp. Vtable the routine.
// Same 0x84 footprint as BeamEffect, but +0x04 = VisEffect*, +0x6c = IFxEffect*,
// +0x70 = FxNode*, +0x7c = ParticleFx flags (bit2 created, bit3 ale loaded, bit4 start).

struct FParticleEffect : FEffectBase
{
    explicit FParticleEffect(unsigned TypeId);
    unsigned char Start(FEffectArch* EffectArch) override;
    void Kill() override;
    float GetWorldPos(FVector* Out) override;
    void SetIntensity(float Intensity) override;
    unsigned char IsAlive() override { return 1; }
    unsigned char UpdateVisibility(float Dt, FSpaceCamera* Cam) override;
    unsigned char TickDraw(float Dt, FSpaceCamera* Cam) override;
    unsigned char BuildGeometry(FSpaceCamera* Cam) override;
    ~FParticleEffect() override;
    virtual void SetLod(float Lod);

    IFxEffect* FxEffect;           // +0x6C
    INeuronComponent* FxNode;          // +0x70
    float Radius74;                // +0x74  from EffectTypeInfo.Radius
    float RunTime78;               // +0x78  from EffectTypeInfo.RunTime
    unsigned char Flags7C;         // +0x7C  bit2=ale.eff, bit3=ale loaded, bit4=start
    unsigned char Pad7D[3];
    float Float80;                 // +0x80  = 1.0; the PARTICLEFX_LOD blend (the routine)
};

static_assert(sizeof(FParticleEffect) == 0x84, "ParticleEffect 0x84");
static_assert(offsetof(FParticleEffect, Vis) == 0x04, "Vis @ +0x04");
static_assert(offsetof(FParticleEffect, FxEffect) == 0x6c, "FxEffect @ +0x6c");
static_assert(offsetof(FParticleEffect, FxNode) == 0x70, "FxNode @ +0x70");
static_assert(offsetof(FParticleEffect, Flags7C) == 0x7c, "Flags7C @ +0x7c");

// NPC cruise exhaust (the routine local_58): sizeof 0x90 = ParticleEffect + float@+0x84.
// Vtable the routine — Start/ArmDeath shared with trail; destroy = the routine
// (frees 0x90); slots +0x08/+0x0c/+0x14 override radius/visibility for Scale@+0x84.
struct FParticleEffectCruise : FParticleEffect
{
    FParticleEffectCruise();
    float GetWorldPos(FVector* Out) override;
    void SetIntensity(float Scale) override;
    unsigned char UpdateVisibility(float Dt, FSpaceCamera* Cam) override;
    float CruiseScale;         // +0x84  = 1.0f at factory; the routine setter
    unsigned char Pad88[8];    // +0x88 … +0x8f — baseline alloc 0x90
};

static_assert(sizeof(FParticleEffectCruise) == 0x90, "ParticleEffectCruise 0x90");
static_assert(offsetof(FParticleEffectCruise, CruiseScale) == 0x84, "CruiseScale @ +0x84");

struct FJumpParticleEffect : FParticleEffect
{
    FJumpParticleEffect() : FParticleEffect(0x25u) {}
    unsigned char UpdateVisibility(float Dt, FSpaceCamera* Cam) override;
};

struct FParticleTunnel : FParticleEffect
{
    FParticleTunnel() : FParticleEffect(0) {}
    unsigned char UpdateVisibility(float Dt, FSpaceCamera* Cam) override;
    unsigned char TickDraw(float Dt, FSpaceCamera* Cam) override;
};

// — construct; param = initial EffectTypeId (0 from factory).
void ParticleEffectConstruct(FParticleEffect* This, unsigned TypeId);

// — Slot13(7)+Release at +0x6c; ParticleFx DetachNode + Release at +0x70.
void ParticleEffectCleanupParticleFx(FParticleEffect* This);

// — CreateInstance FxNode + LoadAle + CreateEffect + AttachNode(root).
unsigned char ParticleEffectCreateParticleFxNodes(FParticleEffect* This);

// — vtable[0] start(EffectArch*).
unsigned char ParticleEffectStart(FParticleEffect* This, FEffectArch* EffectArch);

// — vtable slot +4 (stop). Without an IFxEffect: link, the routine, dead.
// With one: link, freeze to world (dead too when the watched object is gone), pin the run time
// at +0x78 into [0, 1] (NaN kept), then IFxEffect vt+0x34(1) so the effect winds down.
void ParticleEffectArmDeath(FParticleEffect* This);

// — vtable[3] (+0x0c): intensity clamped to [0, 1] into the IFxEffect's vt+0x18;
// no-op without an IFxEffect. SpaceDust (the routine) drives its dust effects through it.
void ParticleEffectSetIntensity(FParticleEffect* This, float Intensity);

struct FSpaceCamera;

// — vtable[+0x14]: particles option, the watched ship/solar gates (cloaked
// CEqObj, field controller the field and its radius), perf priority, near plane, pbubble,
// PARTICLEFX_LOD blend into +0x80 (the routine), side planes -> bit 0 of +0x7c, returned.
unsigned char ParticleEffectUpdateVisibility(FParticleEffect* This, float Dt, FSpaceCamera* Cam);

// — vtable[+0x18]: run-time countdown / ale-finished test (dead on either), then
// and, with an IFxEffect, SetLod(+0x80), transform, update and tick by Dt.
unsigned char ParticleEffectTickDraw(FParticleEffect* This, float Dt, FSpaceCamera* Cam);

// — vtable[+0x1c]: clear the visibility bit; IFxEffect +0x24(Cam) (the draw).
unsigned char ParticleEffectBuildGeometry(FParticleEffect* This, FSpaceCamera* Cam);

// — vtable[+0x24]: LOD clamped to [0, 1] into IFxEffect +0x38; warns without one.
void ParticleEffectSetLod(FParticleEffect* This, float Lod);

// — cruise vtable[+0x08]: radius = +0x84 * +0x74, at least 10; world position.
float ParticleEffectCruiseGetWorldPos(FParticleEffectCruise* This, FVector* Out);

// — cruise vtable[+0x0c]: +0x84 = Scale, then the SetIntensity clamp into
// IFxEffect +0x18. Tradelane rings drive it (the routine).
void ParticleEffectCruiseSetScale(FParticleEffectCruise* This, float Scale);

// — cruise vtable[+0x14]: the routine, then a hidden effect fades +0x80 by
// Dt * 0.5 while staying visible.
unsigned char ParticleEffectCruiseUpdateVisibility(FParticleEffectCruise* This, float Dt,
                                                   FSpaceCamera* Cam);

// — destructor body (particlefx cleanup, base vptr, watcher cleared).
void ParticleEffectDestruct(FParticleEffect* This);

// — slot +0x20 of the routine: destruct; FreeMemory&1 → Heap::Free on the
// 0x84 pool (EffectFree).
void ParticleEffectDestroy(FParticleEffect* This, int FreeMemory);

// — slot +0x20 of the routine (cruise): destruct; FreeMemory&1 →
// (this, 0x90), i.e. free().
void ParticleEffectCruiseDestroy(FParticleEffect* This, int FreeMemory);

// / the routine (Release deletes through the object's own slot +0x20).
void ParticleEffectAddRef(FParticleEffect* This);
int ParticleEffectRelease(FParticleEffect* This);

// Shared attach the routine (same layout as BeamEffect).
void ParticleEffectAttach(FParticleEffect* This, void* WatchableOwner, int EngineIndex,
                          FTransform const* LocalXform);

// — EffectAlloc(0x84) (Heap pool) + Construct(0).
FParticleEffect* ParticleEffectCreate();

// NPC-cruise arm — EffectAlloc(0x90) (malloc) + Construct(0) + CruiseScale=1 +
// . Destroy path: the routine (ParticleEffectCruiseDestroy).
FParticleEffect* ParticleEffectCreateCruise();

// (slot 0 of the jump-effect factory, .rdata 005daab4) -- EffectAlloc(0x84) (the
// Heap pool), the routine(0x25), then the jump particle's own vtable the routine: the
// particle's table with slot +0x14 the branch (bit 0 of +0x7c raised, never culled) and slot
// +0x20 the routine (destruct, then the routine(this, 0x84)).
FParticleEffect* ParticleEffectCreateJump();

// (slot 0 of the tradelane factory the field, .rdata 005db760) -- EffectAlloc(0x84)
// (the Heap pool), then the branch: the routine(0) and the ParticleTunnel vtable
// (RTTI .?AUParticleTunnel@@): the particle's table with +0x14 the routine (bit 0
// of +0x7c = the drawing camera is a cockpit camera), +0x18 the branch (follow the camera's
// position, then the particle tick) and +0x20 the routine.
FParticleEffect* ParticleTunnelCreate();

// baseline CameraParticles (RTTI .?AUCameraParticles@@ at vtable the branch[-4], nine
// slots) -- the particle of the tradelane factory the field. No ParticleFx: it is a white
// screen flash. Start fades the screen out to white when the listener camera is near the effect;
// the draw tick counts the timer down, fades back in and kills the effect. 0x78 bytes: the
// Effect head (the routine's body inlined into the ctor the routine, type id 0x2c), then three
// fields of its own.
struct FCameraParticles : FEffectBase
{
    FCameraParticles();
    unsigned char Start(FEffectArch* Arch) override;
    float GetWorldPos(FVector* Out) override;
    unsigned char IsAlive() override { return 1; }
    unsigned char UpdateVisibility(float Dt, FSpaceCamera* Cam) override { return 1; }
    unsigned char TickDraw(float Dt, FSpaceCamera* Cam) override;
    unsigned char BuildGeometry(FSpaceCamera* Cam) override { return 1; }
    float Timer;                   // +0x6c  ctor: 0.4f + 0.4f (FLD 0x005d9a74 / FADD ST0,ST0)
    unsigned char bStartOpaque;    // +0x70  ctor 1; Start: distance factor <= 0.0f
    unsigned char Pad71[3];
    void* Camera;                  // +0x74  ctor 0; Start: the routine (the listener camera)
};
static_assert(sizeof(FCameraParticles) == 0x78, "CameraParticles 0x78 (module state)");
static_assert(offsetof(FCameraParticles, Timer) == 0x6c, "Timer @ +0x6c");
static_assert(offsetof(FCameraParticles, bStartOpaque) == 0x70, "bStartOpaque @ +0x70");
static_assert(offsetof(FCameraParticles, Camera) == 0x74, "Camera @ +0x74");

// (thiscall, no args) -- the CameraParticles constructor.
void CameraParticlesConstruct(FCameraParticles* This);

// (slot 0 of the factory the field, .rdata 005d99b8) -- the routine(0x78),
// which is malloc for any size but 0x84, then a tail jump into the routine; null memory returns
// null. The factory slot is the generic Effect* slot the one-shot creators read through the
// shared head only (+0x10, +0x4c, +0x58, vtable slots 0 and 8), so the object goes out as the
// slot's FParticleEffect* type; nothing reads a ParticleEffect-only field through it.
FEffectBase* CameraParticlesCreate();

// — companion alive? (particle Flags08 bit1 clear / sound / light).
unsigned char CreateEffectCompanionAlive(void* CompanionLocal);

// — createeffect one-shot. Particle + Source3D sound arms;
// light arm pending gamelit. ListOwner non-null → the routine(ListOwner+0x28, companion).
// baseline arg order after factory this: EffectArch, ListOwner, EngineIndex, Xform, CompanionOut.
// This wrapper is the routine on the field, the factory almost every caller passes.
unsigned char CliCreateEffectOneShot(FEffectArch* EffectArch, void* ListOwner,
                                     int EngineIndex, FTransform const* LocalXform,
                                     void* CompanionOut);

struct FCliEffectFactory;
// with its `this` factory explicit (slots 0/1/2 through it). Callers on another
// factory: 0x005148a6 (the routine, direct, the field) and 0x005140a7 (the routine forwards
// its own ECX: the field from 0x00514448 and 0x00516792, the field from the others).
unsigned char CliCreateEffectOneShotOn(FCliEffectFactory const* Factory, FEffectArch* EffectArch,
                                       void* ListOwner, int EngineIndex,
                                       FTransform const* LocalXform, void* CompanionOut);

// The effect factories the routine / the routine are thiscall on: .data objects holding only
// a vptr, stamped by static initializers, whose three slots make the particle (+0), the sound
// (+4) and the light (+8):
// (the branch) -> .rdata 005d99a8 {the branch, the branch, the branch}
// (the branch) -> .rdata 005d99b8 {the branch, the branch, the branch}
// (the branch) -> .rdata 005d99c8 {the branch, the branch, the branch}
// (the branch) -> .rdata 005daab4 {the branch, the branch, the branch}
// (the branch) -> .rdata 005db760 {the branch, the branch, the branch}
// All but the field have a table here; that one's only XREF is its own static init (0x004f6e60,
// W) -- nothing loads it as a factory (its sound slot the branch is `XOR EAX,EAX / RET`). The particle slot is the
// generic Effect* slot: the one-shot creators touch the object only through the shared head
// and vtable slots 0 and 8, so a CameraParticles goes through it as well.
struct ISoundSource;
struct FCliEffectFactory
{
    virtual FEffectBase* CreateParticle() const = 0;
    virtual ISoundSource* CreateSound() const = 0;
    virtual FGameLight* CreateLight() const = 0;
};

struct FStandardCliEffectFactory : FCliEffectFactory
{
    FEffectBase* CreateParticle() const override;
    ISoundSource* CreateSound() const override;
    FGameLight* CreateLight() const override;
};

struct FJumpCliEffectFactory : FCliEffectFactory
{
    FEffectBase* CreateParticle() const override;
    ISoundSource* CreateSound() const override;
    FGameLight* CreateLight() const override;
};

struct FCameraCliEffectFactory : FCliEffectFactory
{
    FEffectBase* CreateParticle() const override;
    ISoundSource* CreateSound() const override;
    FGameLight* CreateLight() const override;
};

struct FTunnelCliEffectFactory : FCliEffectFactory
{
    FEffectBase* CreateParticle() const override;
    ISoundSource* CreateSound() const override;
    FGameLight* CreateLight() const override;
};

extern FStandardCliEffectFactory const G_CliEffectFactoryC60;
extern FJumpCliEffectFactory const G_CliEffectFactoryJump;
extern FCameraCliEffectFactory const G_CliEffectFactoryCameraParticles;
extern FTunnelCliEffectFactory const G_CliEffectFactoryTunnel;

// (thiscall on Factory, RET 0x10) — world-space one-shot (no ListOwner attach).
// Copies 0x30 LocalXform into particle+0x10 / light+0xf0 (or EngineLightSetTransform when
// light EngIndex==-1); Pos3 → particle+0x4c / sound SetVelocity / GameLight+0x120.
// CompanionOut may be null (tradelane player leave/approach). No the routine.
unsigned char CliCreateEffectWorldOneShotOn(FCliEffectFactory const* Factory,
                                            FEffectArch* EffectArch, FTransform const* LocalXform,
                                            float const* Pos3, void* CompanionOut);

// on the field (the explosion's factory, 0x005364b1..0x005365a7; also 0x00514bde,
// , 0x0052e57c, 0x0053208b). The the field callers (0x00514ad1, 0x00514e70,
// , 0x0052b3c9) go through CliCreateEffectWorldOneShotOn with
// G_CliEffectFactoryCameraParticles.
unsigned char CliCreateEffectWorldOneShot(FEffectArch* EffectArch, FTransform const* LocalXform,
                                          float const* Pos3, void* CompanionOut);
