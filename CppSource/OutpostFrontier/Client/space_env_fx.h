// Author: Giperion.

#pragma once

// baseline "space environment FX" subsystem: OutpostFrontier.exe .text 0x00500000-0x00520000.
// Confirmed baseline source file (embedded ERROR/WARNING format strings, module storage+
// and module storage): "Client\Space\JumpEffect.cpp" -- this is
// baseline's JumpGateEffect / JumpShipEffectDesc class pair.

// Evidence chain (see implementation tracker notes on the routine, the routine, the routine,
// /the branch, and OutpostFrontier around line 165163+):
//   - the routine parses [JumpGateEffect]/[JumpShipEffect] sections of the per-system FX
//     INI, allocating small effect-node structs (operator_new(0x70) / operator_new(0x14)).
//     The port's LoadJumpGateEffectsIni() (space_scene_load.cpp) is an explicit stub that
//     only logs and never builds these nodes or calls RegisterSceneRenderable.
//   - the routine is JumpGateEffect's constructor (embedded string "null solar passed to
//     JumpGateEffect constructor"): takes a Solar object, looks up a JumpGateEffectNode by
//     ID_String key (default "jump_effect_default" via CreateID) in the static node list
//     built by the routine/the branch, then resolves hardpoints via FindHardpoint_PS.
//   - the routine lazily default-constructs a global JumpShipEffectDesc (embedded string
//     "no JumpShipEffectDesc was loaded, creating a default") -- confirms JumpShipEffectNode
//     is actually 5 consecutive ID_String fields (operator_new(0x14) = 5*4 bytes, each
//     cleared via ID_String::clear at offsets 0x0/0x4/0x8/0xc/0x10).
//   - the routine (paired with ctor-ish the branch, both registered via
//     COMMON.DLL::NewSpatial) is the destructor of a "space env FX" renderable object
//     (Nebula/Clouds/ObjectFog/TexturePanels/Lightning/BackgroundLightning per
//     space_scene_load.cpp's System.ini section dispatch), sharing vtable the branch.
//   - the routine/the routine/the routine/the routine/the routine/the routine and
//     friends implement resizable float-vector buffers (VC6 std::vector<float>-style: a
//     [begin,end,capacity] triple manipulated via the routine "capacity",
// "grow/insert", the routine "shrink/destroy tail") filled with
//     rand()-jittered values -- the classic OutpostFrontier "flicker" arrays used to animate
//     nebula/cloud/lightning color and vertex offsets over time.

// None of this renderable/particle-array engine exists in the port yet: the space scene
// loader defers the whole [Nebula]/[Clouds]/[ObjectFog]/[TexturePanels]/[Lightning]/
// [BackgroundLightning]/[JumpGateEffect]/[JumpShipEffect] System.ini surface (see
// space_scene_load.cpp, LoadJumpGateEffectsIni() and the System.ini header dispatch).
// This header is a placeholder so a future pass has named types to hang real ports off
// instead of re-deriving the struct layout from scratch every time.

#include "common_id.h"
#include "common_archetype.h" // CacheString

// One glow ring of a [JumpGateEffect] section (0xc bytes, five of them). the routine is its
// constructor, not a destructor: ID_String::clear on +0, CacheString::clear on +4, return
// this (the loader the routine runs it on each of the five, 0x00502192). Keys:
// glow_ring_effect (+0, CreateID), glow_ring_hp (+4, StringAlloc), glow_create_time (+8).
struct FJumpGateGlowRing
{
    ID_String Effect;  // +0x00
    CacheString Hp;    // +0x04
    float CreateTime;  // +0x08

    FJumpGateGlowRing()
    {
        Effect.clear();
        Hp.clear();
    }
};
static_assert(sizeof(FJumpGateGlowRing) == 0xc, "glow ring 0xc");

// The payload of one the field list node (the node is operator new(0x70): next, prev, then
// this 0x68-byte record, filled by the routine from a [JumpGateEffect] section and copied in
// by the routine). Offsets from the loader's stack record [ESP+0x24] and from the readers
// (+0x00, +0x04.., +0x40) and the routine (+0x44..+0x63).
struct FJumpEffectDesc
{
    ID_String Nickname;               // +0x00  nickname
    FJumpGateGlowRing Rings[5];       // +0x04
    int RingCount;                    // +0x40  min of the three per-ring key counts
    float JumpOutTime;                // +0x44  jump_out_time
    float JumpOutTunnelTime;          // +0x48  jump_out_tunnel_time
    float JumpInTunnelTime;           // +0x4c  jump_in_tunnel_time
    float JumpInTime;                 // +0x50  jump_in_time
    float KillTimeBeforeDone;         // +0x54  kill_time_before_done
    ID_String JumpTunnelEffect;       // +0x58  jump_tunnel_effect
    ID_String JumpTunnel;             // +0x5c  jump_tunnel (a GateTunnel id)
    unsigned char JumpAmbient[3];     // +0x60  jump_ambient (three ftol'd floats)
    unsigned char JumpBackgroundColor[3]; // +0x63 jump_background_color
    unsigned char Pad66[2];
};
static_assert(sizeof(FJumpEffectDesc) == 0x68, "jump effect desc 0x68 (list node 0x70)");

// JumpShipEffectDesc (baseline name -- see the routine's embedded warning string).
// operator_new(0x14) = 5 consecutive ID_String fields, each ID_String::clear()'d by
// 's default-construction path. The [JumpShipEffect] loader the routine fills
// Ids[1] jump_out_effect (0x0050260e), Ids[2] jump_in_effect (0x0050263c), Ids[3]
// jump_done_effect_player (0x00502667), Ids[4] jump_done_effect_nonplayer (0x00502692);
// Ids[0] is never written by it (meaning unknown, TODO).
struct JumpShipEffectDesc
{
    ID_String Ids[5];

    // baseline's lazy-default-construct path (the routine) explicitly clears all 5 ids
    // rather than relying on ID_String's (trivial) default state.
    JumpShipEffectDesc()
    {
        for (int Index = 0; Index < 5; ++Index)
            Ids[Index].clear();
    }
};

// --- batch47a additions: further System.ini "space env fx" descriptor sections in the same
// range, all still confirmed-but-unported. ---

// (a per-frame Draw()-style helper of the JumpGateEffect family) walks fixed
// {type,value,next} triples at the field/the field/the field (SetTextureStageState,
// one table per texture stage) and the field (SetRenderState) through the global Pipeline
// object (Pipeline_exref), using slot offsets 0xf8/0x108/0xf0/0x110 -- these match
// kPipelineSetTextureStageState/kPipelineSetTextureStageTexture/kPipelineSetRenderState/
// kPipelineVerifyState in src/include/render/RenderPipeline.h EXACTLY. This *confirms* (not just
// suspects, per batch46a's open question) that this whole area's Draw() methods are ordinary
// Pipeline consumers, structurally identical to the already-ported PipelineApplyRsTable/
// PipelineApplyTssTable table-walk helpers -- only the owning renderable class/vtable and its
// state tables are unported, not the Pipeline layer itself.

// "[spines]" (Nebula lightning-bolt spine) descriptor -- the routine parses this System.ini
// section by nickname (is_header("spines"), is_value("nickname") + stricmp). Confirmed fields
// (offsets relative to the target object, via its extraout_ECX-based stores):
struct NebulaSpineDesc
{
    // +0x10 CacheString Shape;  // "shape=" (the routine = shape-id hash; zeroed when the key's value is empty)
    // +0x38 float MinRadius;    // "min_radius="
    // +0x3c float MaxRadius;    // "max_radius="
    // "radius_scale=" (default 1.0) and "spine=" (5 floats/entry, forms an animation/keyframe
    // list) are also parsed but not yet mapped to stable offsets.
};

// "[star_glow]" descriptor -- the routine. Owning class is StarSphere (the routine).
// Port: src/OutpostFrontier/Client/star_sphere.h (FStarGlowDesc) + star_sphere.cpp.
struct StarGlowDesc
{
    // See FStarGlowDesc — shape length@+0 / name@+4 / scale@+0x24 / colors / alpha@+0x40.
};

// "[lens_glow]" descriptor -- the routine into SunEffect body (+0x288..+0x2f4).
// Port: LensGlowLoadFromIni / FSunEffectBody in star_sphere.cpp.
struct LensGlowDesc
{
    // Embedded in FSunEffectBody Raw[0x30c]; not a standalone allocation.
};

// / the routine / the routine — Draw helpers; class live, draw still partial.
// See star_sphere.cpp FStarSphereVt slots 4–6.
