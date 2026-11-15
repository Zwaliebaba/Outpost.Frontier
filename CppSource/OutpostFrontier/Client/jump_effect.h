// Author: Giperion.

#pragma once

// baseline Client\Space\JumpEffect.cpp (path string .rdata 0x005da80c) -- the JumpShipEffect,
// the 0xdc-byte object a client Ship keeps at +0x188 while it jumps (FClientShipGameobj::
// Field_0x188). Built by the routine (the routine(ship, gate solar)) or the routine
// (the routine(ship)), destroyed by the routine + operator delete (the routine case 0,
// , the routine and the Ship dtor the routine at 0x0054456b).

// Ported here: both JumpShipEffect constructors (the routine, the routine), its destructor
// , the camera-shake amplitude the routine, the descriptor getter the routine,
// and the JumpGateEffect constructor / destructor / ring (the routine, the routine,
// , the routine), the routine, the [JumpGateEffect]/[JumpShipEffect] loader
// (its record copy the routine is the implicit copy) and the teardown
// , and the four jump-state hooks the routine / the routine / the routine /
// and the update the routine (W3-A; called by the ship update the routine), and
// the JumpGateEffect update the routine (W3-A; called by the solar update the routine).

#include "common_csys.h"
#include "common_id.h"
#include "space_env_fx.h" // JumpShipEffectDesc (0x14, the routine), FJumpEffectDesc

#include <cstddef>

struct ISoundSource;          // soundmanager.h
struct FParticleEffect;       // particle_effect.h
struct FGameLight;            // engine_light.h
struct FGateTunnel;           // gate_tunnel.h
class FClientShipGameobj;     // client_gameobj.h

// The 0x10-byte effect companion {slot0, ISoundSource*, particle, GameLight}. Its destructor is
// (CliEffectCompanionClear): the routine's EH states 3..0 run it on the four
// members in reverse order (0x00503c07, then inline 0x00503c0c.., 0x00503c43.., 0x00503c78..).
struct FJumpFxCompanion
{
    void* Slot0;               // +0x00
    ISoundSource* Sound;       // +0x04
    FParticleEffect* Particle; // +0x08
    FGameLight* Light;         // +0x0c

    FJumpFxCompanion() : Slot0(nullptr), Sound(nullptr), Particle(nullptr), Light(nullptr) {}
    ~FJumpFxCompanion();

    FJumpFxCompanion(FJumpFxCompanion const&) = delete;
    FJumpFxCompanion& operator=(FJumpFxCompanion const&) = delete;
};
static_assert(sizeof(FJumpFxCompanion) == 0x10, "effect companion 0x10");

// One glow ring of a JumpGateEffect (0x24 bytes, five at +0x2c). Constructor the branch
// (byte +0 and dwords +4..+0x20 zeroed), destructor the routine (the companion at +0x14
// released: particle +0x1c, sound +0x18, light +0x20, then +0x14 = 0 -- the routine's body).
struct FJumpGateRingFx
{
    unsigned char bCreated;  // +0x00  the routine sets it when the ring's effect is made
    unsigned char Pad01[3];
    float CreateTime;        // +0x04  descriptor ring +8
    Vector Position;         // +0x08  the ring hardpoint (FindHardpoint_PS, parent space)
    FJumpFxCompanion Fx;     // +0x14

    FJumpGateRingFx();
};
static_assert(sizeof(FJumpGateRingFx) == 0x24, "gate ring 0x24");

class FClientSolarGameobj; // client_gameobj.h

// The JumpGateEffect a jump gate / hole solar keeps at +0x204 (operator new(0xe0)): built by
// the solar notify (0x00547e1f) or, as a temporary, by the routine (0x0050358a).
struct FJumpGateEffect
{
    // (thiscall, RET 4).
    explicit FJumpGateEffect(FClientSolarGameobj* Solar);
    // (thiscall). Callers: the solar dtor 0x005476d3 / notify 0x005479da, and
    // inline in the routine (0x005037ed..0x00503896).
    ~FJumpGateEffect();

    FJumpGateEffect(FJumpGateEffect const&) = delete;
    FJumpGateEffect& operator=(FJumpGateEffect const&) = delete;

    // (thiscall, RET 4) -- the per-frame update. Caller: the solar update
    // (0x005478d6) when the solar has one. The clock runs until bFire; each ring
    // whose create time the clock has passed starts its effect once, at its hardpoint.
    void Update(float Dt);

    FJumpEffectDesc* Desc;             // +0x00  the the field record, or null
    FClientSolarGameobj* Solar;        // +0x04
    float Clock;                       // +0x08  the routine advances it
    Vector JumpOutPosition;            // +0x0c  the archetype's JumpOutHp (parent space)
    unsigned char bFire;               // +0x18  the routine sets it
    unsigned char Pad19[3];
    FJumpFxCompanion Fx;               // +0x1c
    FJumpGateRingFx Rings[5];          // +0x2c
};
static_assert(offsetof(FJumpGateEffect, JumpOutPosition) == 0x0c, "JumpGateEffect +0x0c");
static_assert(offsetof(FJumpGateEffect, bFire) == 0x18, "JumpGateEffect +0x18");
static_assert(offsetof(FJumpGateEffect, Fx) == 0x1c, "JumpGateEffect +0x1c");
static_assert(offsetof(FJumpGateEffect, Rings) == 0x2c, "JumpGateEffect rings +0x2c");
static_assert(sizeof(FJumpGateEffect) == 0xe0, "JumpGateEffect operator_new(0xe0)");

// (thiscall) -- bFire = 1. Caller: solar notify 0x00547bda.
void JumpGateEffectFire(FJumpGateEffect* Gate);

struct FJumpShipEffect
{
    // (thiscall, RET 4) -- the no-gate constructor.
    explicit FJumpShipEffect(FClientShipGameobj* Ship);
    // (thiscall, RET 8) -- the constructor for a jump through GateSolar (a jump
    // gate / hole solar gameobj). Caller: the routine (0x00543595), not ported.
    FJumpShipEffect(FClientShipGameobj* Ship, FClientSolarGameobj* GateSolar);
    // (thiscall, RET 0).
    ~FJumpShipEffect();

    FJumpShipEffect(FJumpShipEffect const&) = delete;
    FJumpShipEffect& operator=(FJumpShipEffect const&) = delete;

    // (thiscall, RET 0) -- the camera-shake amplitude for the ship's jump state
    // (+0x68 -> +0x158), left on the FPU. Caller: HudCameraUpdateShake 0x0051dba8.
    double ShakeAmplitude() const;

    // The four state hooks the routine (FClientShipGameobj::SetJumpState) calls. Its case 3
    // calls a fifth, IDPMsgHandler::OnDisconnect (0x00401e90) -- a bare RET the linker folded
    // it into, so there is nothing to port for it.
    // (thiscall, RET 0), state 1: with a tunnel, tunnel state 1 and the tunnel
    // effect (+0x18) started at +0xa8 into FxB.
    void StartJumpOut();
    // (thiscall, RET 0), state 2: tunnel state 2; FxB moved onto the ship, and the
    // descriptor's jump_out_effect started at the ship into FxA and moved onto the ship too.
    void EnterTunnel();
    // (thiscall, RET 0), state 4: tunnel state 3; the descriptor's jump_in_effect
    // started at the ship into FxC and moved onto it; +0xd8 = 1.
    void StartJumpIn();
    // (thiscall, RET 0), state 5: tunnel state 0; the descriptor's jump done
    // effect (player or non-player) started on the ship into FxD.
    void FinishJump();

    // (thiscall, RET 4) -- the per-frame update. Caller: the ship update
    // at 0x0054536f (FClientShipGameobj::update), with its clamped step. States
    // 2/3 advance the tunnel; in state 4, once the clock passes JumpInTime - KillTimeBeforeDone,
    // the tunnel effect FxB is stopped and released and +0x6d latches.
    void Update(float Dt);

    JumpShipEffectDesc* Desc;        // +0x00  the routine / the field
    // +0x04 .. +0x20 -- the routine copies them from the gate's FJumpEffectDesc (+0x44..+0x63;
    // JumpOutTime is 0 when the gate's type has bit 0x800). the routine divides by JumpOutTime
    // in state 1 and by JumpInTime in state 4, against the clock at +0x70.
    float JumpOutTime;                 // +0x04  ctor 0
    float JumpOutTunnelTime;           // +0x08  ctor 5.0f
    float JumpInTunnelTime;            // +0x0c  ctor 5.0f
    float JumpInTime;                  // +0x10  the routine 0.5f, the routine 0
    float KillTimeBeforeDone;          // +0x14  ctor 0.5f
    ID_String TunnelEffect;            // +0x18  clear(); the routine: jump_tunnel_effect
    unsigned char const* Ambient;      // +0x1c  &desc jump_ambient (the routine), else 0
    unsigned char const* Color;        // +0x20  &desc jump_background_color; the routine
    FGateTunnel* Tunnel;               // +0x24  the GateTunnel (the routine), player only
    FJumpFxCompanion FxA;              // +0x28
    FJumpFxCompanion FxB;              // +0x38
    FJumpFxCompanion FxC;              // +0x48
    FJumpFxCompanion FxD;              // +0x58  also linked into the ship's +0x28 effect list
    FClientShipGameobj* Ship;          // +0x68
    unsigned char Field_0x6c;          // +0x6c  ctor 0
    unsigned char Field_0x6d;          // +0x6d  ctor 0; the routine sets 1, the routine 0
    unsigned char Pad6e[2];
    float Field_0x70;                  // +0x70  ctor 0; the effect's clock (the routine)
    // +0x74 / +0x80 -- where the ship comes out of the jump: the routine copies 3 dwords of
    // its Vector argument to +0x74 (0x0054366b..0x00543679 / 0x005436ff..0x0054370d) and 9 of
    // its Matrix argument to +0x80 (MOVSD.REP ECX=9); the routine case 4 hands both to
    // CShip::jump_in / CObject::beam_object as (Vector const&, Matrix const&).
    Vector JumpInPosition;             // +0x74
    Matrix JumpInOrientation;          // +0x80
    unsigned Field_0xa4;               // +0xa4  the routine: the gate CSolar's +0x1b4 (player)
    Transform Field_0xa8;              // +0xa8  identity; the routine: the gate hardpoint
    unsigned char Field_0xd8;          // +0xd8  ctor 0; the routine clears it
    unsigned char Padd9[3];
};
static_assert(offsetof(FJumpShipEffect, TunnelEffect) == 0x18, "JumpShipEffect +0x18");
static_assert(offsetof(FJumpShipEffect, Color) == 0x20, "JumpShipEffect colour +0x20");
static_assert(offsetof(FJumpShipEffect, FxA) == 0x28, "JumpShipEffect +0x28");
static_assert(offsetof(FJumpShipEffect, FxD) == 0x58, "JumpShipEffect +0x58");
static_assert(offsetof(FJumpShipEffect, Ship) == 0x68, "JumpShipEffect ship +0x68");
static_assert(offsetof(FJumpShipEffect, Field_0x70) == 0x70, "JumpShipEffect +0x70");
static_assert(offsetof(FJumpShipEffect, JumpInPosition) == 0x74, "JumpShipEffect +0x74");
static_assert(offsetof(FJumpShipEffect, JumpInOrientation) == 0x80, "JumpShipEffect +0x80");
static_assert(offsetof(FJumpShipEffect, Field_0xa4) == 0xa4, "JumpShipEffect +0xa4");
static_assert(offsetof(FJumpShipEffect, Field_0xa8) == 0xa8, "JumpShipEffect transform +0xa8");
static_assert(offsetof(FJumpShipEffect, Field_0xd8) == 0xd8, "JumpShipEffect +0xd8");
static_assert(sizeof(FJumpShipEffect) == 0xdc, "JumpShipEffect operator_new(0xdc)");

// (cdecl) -- the field, the JumpShipEffectDesc the [JumpShipEffect] section
// of the jump-effect INI (the routine, not ported) loads; the first call without one warns
// and news a default (five cleared ids).
JumpShipEffectDesc* JumpShipEffectDescGet();

// (cdecl, no arguments; caller: the space init the routine at 0x0054ac15) --
// reads the [Data] jump_effect INI: every [JumpGateEffect] section is appended to the
// record list, a [JumpShipEffect] section replaces the field.
void JumpEffectIniLoad();

// (cdecl, no arguments; caller: the space teardown at 0x0054afb1) -- empties the
// list and frees the field.
void JumpEffectDescsClear();
