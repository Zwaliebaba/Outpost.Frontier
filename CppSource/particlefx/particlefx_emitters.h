// Author: Giperion.

#pragma once

#include "particlefx_genericfx.h"

// particlefx.dll -- the three "generic" emitters (RegisterComponent names
// FxConeEmitter / FxCubeEmitter / FxSphereEmitter).

// baseline layout: each emitter has an outer IGenericFx vtable (cone 0624d320,
// cube 0624d680, sphere 0624f2f8) whose slots are this+4 adjustor thunks into
// the embedded node object's vtable (cone 0624d2c8, cube 0624d628, sphere
// ; shared base node vtables 0624d3c8 / 0624d728 / 0624f3a0). The port
// folds outer and node into one FGenericFxObjectBase-derived class; offsets
// below are NODE-relative.

// Per frame: IParticleFx Slot16 (the branch / the routine) calls outer +0x14
// (IGenericFx::Pad5) on every node. For an emitter that is the per-type update
// (cone the branch, cube the branch, sphere the branch): evaluate the
// shape properties at (node+0x8c, Dt + Age), cache them at node+0xf4.., then
// run the shared emitter body the routine (FFxEmitterBase::UpdateEmitter),
// which ticks the lifetime and spawns particles through node slot 20 (+0x50,
// the per-type sampler: cone the branch, cube the branch, sphere
// EmitParticle below).

// Properties (ALC-5 / ALC-6, 2026-09-30). baseline keeps them in the emitter's property set
// (node+0x9c / +0xf0 / +0x80, created by cone the routine, cube the routine, sphere
// node slot +0x48 -- and shared with clones by the routine): the emitter
// part built by the routine (+0x4c..+0x68) and the shape part by the per-type set ctor
// (cone the routine, cube the routine, sphere inline in the routine; +0x6c..). Every
// value but InitialParticles is a ramp with a default key, evaluated by the update. The
// sets below hold them; the reader fills the ramps in place through the getters.
namespace ParticleFxImpl
{
    __declspec(noinline) GENRESULT ForwardEmitterPad46(
        IGenericFx* Appearance, const float* Six, const float& AgeOffset, const float& LifeSpan);
    __declspec(noinline) GENRESULT ForwardEmitterPad47(
        IGenericFx* Appearance, const float* Six, const float& AgeOffset, const float& LifeSpan);

    // (set vtable 0x0624fe18, table .data 0x06257700). Set-proper offsets:
    class FFxEmitterPropertySet : public FFxNodePropertySet
    {
    public:
        FFxEmitterPropertySet();
        ~FFxEmitterPropertySet() override;

        // The emitter set's slots: +0x0c the routine / +0x10 the routine
        // (Emitter_InitialParticles -> +0x64), +0x3c the routine (Emitter_LODCurve),
        // +0x40 the routine (the six 0x202 ramps), +0x54 the routine (base rows, then
        // .data 0x06257700).
        GENRESULT NEURONCOMPONENTS_API GetPropInt(unsigned long Hash, unsigned long* Out) override;
        GENRESULT NEURONCOMPONENTS_API SetPropInt(unsigned long Hash, unsigned long Value) override;
        GENRESULT NEURONCOMPONENTS_API GetPropSingleRampRamp(unsigned long Hash,
                                                  INestedSingleRamp** Out) override;
        GENRESULT NEURONCOMPONENTS_API GetPropAnimatedSingleRamp(unsigned long Hash,
                                                      INestedSingleRamp** Out) override;
        int NEURONCOMPONENTS_API EnumerateProps(FNodePropertyRowCallback Callback, void* Context) override;

        INestedSingleRamp* FrequencyRamp;       // +0x4c Emitter_Frequency 0x023C350C, 100
        INestedSingleRamp* MaxParticlesRamp;     // +0x50 Emitter_MaxParticles 0xF5042852, 10000
        INestedSingleRamp* InitLifeSpanRamp;     // +0x54 Emitter_InitLifeSpan 0x0A635880, 2
        INestedSingleRamp* PressureRamp;         // +0x58 Emitter_Pressure 0x0AB180C5, 10
        INestedSingleRamp* EmitCountRamp;        // +0x5c Emitter_EmitCount 0xE7221F95, 0
        INestedSingleRamp* VelocityApproachRamp; // +0x60 Emitter_VelocityApproach 0x0681233E, 0
        unsigned long InitialParticles;          // +0x64 Emitter_InitialParticles 0x0F9A9D52, 0
        INestedSingleRamp* LodCurveRamp;         // +0x68 Emitter_LODCurve 0xFE077E05 (0x200), 1
    };

    // (cone set, 0x84 bytes with the outer).
    class FFxConeEmitterPropertySet final : public FFxEmitterPropertySet
    {
    public:
        FFxConeEmitterPropertySet();
        ~FFxConeEmitterPropertySet() override;

        // +0x40 the branch (the four shape ramps, else the routine); +0x54 the branch
        // (the routine, then .data 0x06255e28).
        GENRESULT NEURONCOMPONENTS_API GetPropAnimatedSingleRamp(unsigned long Hash,
                                                      INestedSingleRamp** Out) override;
        int NEURONCOMPONENTS_API EnumerateProps(FNodePropertyRowCallback Callback, void* Context) override;

        INestedSingleRamp* MinRadiusRamp; // +0x6c ConeEmitter_MinRadius 0x1C73D16B, 0
        INestedSingleRamp* MaxRadiusRamp; // +0x70 ConeEmitter_MaxRadius 0xF636F07F, 1
        INestedSingleRamp* MinSpreadRamp; // +0x74 ConeEmitter_MinSpread 0x100E7F55, 0 (deg)
        INestedSingleRamp* MaxSpreadRamp; // +0x78 ConeEmitter_MaxSpread 0xFA4B5E41, 0 (deg)
    };

    // (cube set, 0x88 bytes with the outer).
    class FFxCubeEmitterPropertySet final : public FFxEmitterPropertySet
    {
    public:
        FFxCubeEmitterPropertySet();
        ~FFxCubeEmitterPropertySet() override;

        // +0x40 the branch (the five shape ramps, else the routine); +0x54 the branch
        // (the routine, then .data 0x06255f30).
        GENRESULT NEURONCOMPONENTS_API GetPropAnimatedSingleRamp(unsigned long Hash,
                                                      INestedSingleRamp** Out) override;
        int NEURONCOMPONENTS_API EnumerateProps(FNodePropertyRowCallback Callback, void* Context) override;

        INestedSingleRamp* WidthRamp;     // +0x6c CubeEmitter_Width 0xFD22A11C, 5
        INestedSingleRamp* HeightRamp;    // +0x70 CubeEmitter_Height 0xEFB0F8CF, 5
        INestedSingleRamp* DepthRamp;     // +0x74 CubeEmitter_Depth 0x14B74EB2, 5
        INestedSingleRamp* MinSpreadRamp; // +0x78 CubeEmitter_MinSpread 0xE282006B, 0 (deg)
        INestedSingleRamp* MaxSpreadRamp; // +0x7c CubeEmitter_MaxSpread 0x08C7217F, 15 (deg)
    };

    // The sphere set, built inline by the routine (0x7c bytes with the outer).
    class FFxSphereEmitterPropertySet final : public FFxEmitterPropertySet
    {
    public:
        FFxSphereEmitterPropertySet();
        ~FFxSphereEmitterPropertySet() override;

        // +0x40 the branch (the two shape ramps, else the routine); +0x54 the branch
        // (the routine, then .data 0x06256c4c).
        GENRESULT NEURONCOMPONENTS_API GetPropAnimatedSingleRamp(unsigned long Hash,
                                                      INestedSingleRamp** Out) override;
        int NEURONCOMPONENTS_API EnumerateProps(FNodePropertyRowCallback Callback, void* Context) override;

        INestedSingleRamp* MinRadiusRamp; // +0x6c SphereEmitter_MinRadius 0xF231FF60, 0
        INestedSingleRamp* MaxRadiusRamp; // +0x70 SphereEmitter_MaxRadius 0x1874DE74, 1
    };

    class FFxEmitterBase : public FGenericFxObjectBase
    {
    public:
        // (cone; cube / sphere ctors identical): node+0x98..+0xb4 = 0,
        // +0xb8 = 1, +0xbc = identity, +0xec = 0, byte +0x95 = 1, +0xf0.. = 0.
        FFxEmitterBase(const char* PrimaryIid, const char* FactoryName);
        ~FFxEmitterBase() override;

        // swap the associated appearance (Release old, AddRef new),
        // always set +0xb8 = 1, then the (possibly unchanged) appearance's outer
        // +0xC4 (AddEmitterRef) when non-null. Returns 0.
        GENRESULT NEURONCOMPONENTS_API AssociateAppearance(INeuronComponent* Appearance) override;
        // with an appearance associated and the new flags negative
        // (dead), call its outer +0xC8 (ReleaseEmitterRef) first; then store.
        GENRESULT NEURONCOMPONENTS_API SetNodeFlags(unsigned long Flags) override;

    protected:
        FFxEmitterPropertySet* EmitterProps() const
        {
            return static_cast<FFxEmitterPropertySet*>(PropertySet_);
        }

        // (called by each per-type update with the frame Dt).
        GENRESULT UpdateEmitter(float Dt);

        // Node slot 20 (+0x50), thiscall RET 0xC: spawn one particle.
        // AgeOffset = how long ago inside this frame it was emitted, PosOffset3 =
        // the emitter-motion compensation (emitter-local), Vel3 = the inherited
        // velocity term.
        virtual void EmitParticle(float AgeOffset, const float* PosOffset3,
                                  const float* Vel3) = 0;

        // The common tail of all three samplers (cone 0621bb88..0621bca4 and its
        // cube / sphere twins). Pos3 = the sampled position P, Vel3 = the sampled
        // velocity W (both already FSTP float). Q = W * AgeOffset (float); position =
        // (Q + P) + PosOffset with x and y kept in the FPU and z rounded after Q + P;
        // velocity = W + VelIn; then Rotate / TransformXform by +0xbc and the
        // appearance's outer +0xB8 (Pad46) and, when that is >= 0, +0xBC (Pad47) with
        // (Six = {Pos, Vel}, AgeOffset, InitLifeSpan).
        void SubmitParticle(const float* Pos3, const float* Vel3, const float& AgeOffset,
                            const float* PosOffset3, const float* VelIn3,
                            I3DMathEngine*& MathSlot);

        float PrevNodePos_[3];      // +0xa0: world translation at the last emit
        float LastEmitTime_;        // +0xac: Age at which emission was last settled
        float Pressure_;            // +0xb0: Emitter_Pressure, this frame
        float InitLifeSpan_;        // +0xb4: Emitter_InitLifeSpan, this frame
        unsigned char bResetEmitHistory_; // +0xb8: set by ctor / AssociateAppearance
        float EmitterXform_[12];    // +0xbc: this node in the appearance's frame (slot 7)
        float EmitFraction_;        // +0xec: fractional particle carry-over
    };

    class FxConeEmitter final : public FFxEmitterBase
    {
    public:
        FxConeEmitter(const char* PrimaryIid, const char* FactoryName);
        ~FxConeEmitter() override;

        // (outer thunk the branch).
        GENRESULT NEURONCOMPONENTS_API Pad5(float Dt) override;
        // (outer thunk 0621c190).
        GENRESULT NEURONCOMPONENTS_API CloneNode(INeuronComponent** Out) override;

    protected:
        // (node slot +0x48).
        void CreatePropertySet() override;
        // .
        void EmitParticle(float AgeOffset, const float* PosOffset3, const float* Vel3) override;

    private:
        FFxConeEmitterPropertySet* ConeProps() const
        {
            return static_cast<FFxConeEmitterPropertySet*>(PropertySet_);
        }

        float MinSpreadRad_;   // +0xf4
        float SpreadRangeRad_; // +0xf8
        float MinRadius_;      // +0xfc
        float RadiusRange_;    // +0x100
    };

    class FxCubeEmitter final : public FFxEmitterBase
    {
    public:
        FxCubeEmitter(const char* PrimaryIid, const char* FactoryName);
        ~FxCubeEmitter() override;

        // .
        GENRESULT NEURONCOMPONENTS_API Pad5(float Dt) override;
        // (outer thunk 0621d970).
        GENRESULT NEURONCOMPONENTS_API CloneNode(INeuronComponent** Out) override;

    protected:
        // (node slot +0x48).
        void CreatePropertySet() override;
        // .
        void EmitParticle(float AgeOffset, const float* PosOffset3, const float* Vel3) override;

    private:
        FFxCubeEmitterPropertySet* CubeProps() const
        {
            return static_cast<FFxCubeEmitterPropertySet*>(PropertySet_);
        }

        float MinSpreadRad_;   // +0xf4
        float SpreadRangeRad_; // +0xf8
        float Width_;          // +0xfc
        float Height_;         // +0x100
        float Depth_;          // +0x104
    };

    class FxSphereEmitter final : public FFxEmitterBase
    {
    public:
        FxSphereEmitter(const char* PrimaryIid, const char* FactoryName);
        ~FxSphereEmitter() override;

        // (outer slot 5 the branch is an inlined copy of it).
        GENRESULT NEURONCOMPONENTS_API Pad5(float Dt) override;
        // (outer thunk 0623c390).
        GENRESULT NEURONCOMPONENTS_API CloneNode(INeuronComponent** Out) override;

    protected:
        // (node slot +0x48).
        void CreatePropertySet() override;
        // .
        void EmitParticle(float AgeOffset, const float* PosOffset3, const float* Vel3) override;

    private:
        FFxSphereEmitterPropertySet* SphereProps() const
        {
            return static_cast<FFxSphereEmitterPropertySet*>(PropertySet_);
        }

        float MinRadius_;   // +0xf4
        float RadiusRange_; // +0xf8
    };

} // namespace ParticleFxImpl
