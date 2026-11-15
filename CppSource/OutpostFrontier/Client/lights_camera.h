// Author: Giperion.

#pragma once

// LightsCamera particle holder (Client\supt\LightsCamera.cpp) -- the two
// words an ParticleFx particle effect is kept in by its owners: SceneScript_Psys (+0x6c, ScriptPsys.cpp)
// and the Gf ObjectShip's engine effects (callers 0x00446c83 / 0x00446ca5 / 0x00446e9d /
// ). The three functions below are its whole interface.

struct FVisEffectArch;
struct ICamera;
struct IFxEffect;

struct FLightsCameraParticles
{
    // (thiscall, RET 4). Without NeuronRuntime::ParticleFx: false, nothing touched. Otherwise
    // Vis is kept and AddRef'd (the routine), an "FxNode" is created through NeuronRuntime::NeuronComponents
    // (AGGDESC size 8), NeuronRuntime::FxEffectLibrary slot 6 builds the effect for Vis's effect_crc
    // into Effect, and the node is attached under ParticleFx's root (slots 25 / 20). Any failure
    // FDUMPs "ParticleEffect could not create ParticleFx effect" and drops Vis again. The node
    // reference is released on both paths. True only on success.
    bool Create(FVisEffectArch* InVis);

    // (thiscall). With an effect: its slot 13 with 7, Release, Effect = 0, and
    // Vis released (the routine) and cleared. Without one, nothing.
    void Destroy();

    // (thiscall, RET 4). With an effect and both the EFFECTS (0x25) and PARTICLES
    // (0x26) game options on: the effect's slot 4 (0, &scratch), then slot 15 its bounding
    // sphere; when that succeeds and the sphere is in Camera's view (the routine), slot 9 draws
    // it with Camera.
    void Draw(ICamera* Camera);

    IFxEffect* Effect;   // +0x00
    FVisEffectArch* Vis; // +0x04
};
