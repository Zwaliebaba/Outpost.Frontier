// Author: Giperion.

#include "particlefx_emitters.h"

#include "particlefx_common.h"
#include "particlefx_math.h"
#include "particlefx_ramp.h"

#include <cmath>
#include <cstring>

// particlefx.dll -- the shared emitter body and the three generic emitters.
// See particlefx_emitters.h for the baseline vtable map. Everything here was
// re-traced against particlefx on 2026-09-26 (ALC-1) and again, term by term with
// the x87 precision, on 2026-09-30 (ALC-5): values that stay on the FPU stack are
// double here, every FSTP float is a cast to float, and the properties are the ramps of
// the emitter's property set (ALC-6: the set objects themselves, shared with clones).
namespace ParticleFxImpl
{
    namespace
    {
        I3DMathEngine* G_ConeEmitter3DMath = nullptr; // .
        I3DMathEngine* G_CubeEmitter3DMath = nullptr; // .
        I3DMathEngine* G_SphereEmitter3DMath = nullptr; // .

        // Emitter property hashes (descriptor table 0x06257700, records of
        // {name, hash, kind, min, max}); descriptor-inner slot in parentheses.
        const unsigned long kEmitterLodCurveHash = 0xFE077E05;          // +0x68, kind 0x200
        const unsigned long kEmitterInitialParticlesHash = 0x0F9A9D52;  // +0x64, kind 2 (int)
        const unsigned long kEmitterFrequencyHash = 0x023C350C;         // +0x4c
        const unsigned long kEmitterMaxParticlesHash = 0xF5042852;      // +0x50
        const unsigned long kEmitterEmitCountHash = 0xE7221F95;         // +0x5c (keyed bursts)
        const unsigned long kEmitterInitLifeSpanHash = 0x0A635880;      // +0x54
        const unsigned long kEmitterPressureHash = 0x0AB180C5;          // +0x58
        const unsigned long kEmitterVelocityApproachHash = 0x0681233E;  // +0x60

        // Shape properties (per-type property sets, slots +0x6c..).
        const unsigned long kConeMinRadiusHash = 0x1C73D16B;   // +0x6c
        const unsigned long kConeMaxRadiusHash = 0xF636F07F;   // +0x70
        const unsigned long kConeMinSpreadHash = 0x100E7F55;   // +0x74 (degrees)
        const unsigned long kConeMaxSpreadHash = 0xFA4B5E41;   // +0x78 (degrees)
        const unsigned long kCubeWidthHash = 0xFD22A11C;       // +0x6c
        const unsigned long kCubeHeightHash = 0xEFB0F8CF;      // +0x70
        const unsigned long kCubeDepthHash = 0x14B74EB2;       // +0x74
        const unsigned long kCubeMinSpreadHash = 0xE282006B;   // +0x78 (degrees)
        const unsigned long kCubeMaxSpreadHash = 0x08C7217F;   // +0x7c (degrees)
        const unsigned long kSphereMinRadiusHash = 0xF231FF60; // +0x6c
        const unsigned long kSphereMaxRadiusHash = 0x1874DE74; // +0x70

        // The property tables the sets enumerate (.data 0x06257700 emitter, 0x06255e28
        // cone, 0x06255f30 cube, 0x06256c4c sphere; {name, hash, kind, min, max}).
        const FNodePropertyRow kEmitterPropertyRows[] = {
            {"Emitter_LODCurve", 0xFE077E05U, 0x200, 0.0f, 1.0f},
            {"Emitter_InitialParticles", 0x0F9A9D52U, 0x2, 0.0f, 1000.0f},
            {"Emitter_Frequency", 0x023C350CU, 0x202, 0.0f, kNodePropertyRowMax},
            {"Emitter_MaxParticles", 0xF5042852U, 0x202, 0.0f, kNodePropertyRowMax},
            {"Emitter_EmitCount", 0xE7221F95U, 0x202, 0.0f, 5000.0f},
            {"Emitter_InitLifeSpan", 0x0A635880U, 0x202, 0.0f, kNodePropertyRowMax},
            {"Emitter_Pressure", 0x0AB180C5U, 0x202, -kNodePropertyRowMax, kNodePropertyRowMax},
            {"Emitter_VelocityApproach", 0x0681233EU, 0x202, 0.0f, 1.0f},
            {nullptr, 0, 0, 0.0f, 1.0f},
        };
        const FNodePropertyRow kConePropertyRows[] = {
            {"ConeEmitter_MinRadius", 0x1C73D16BU, 0x202, 0.0f, kNodePropertyRowMax},
            {"ConeEmitter_MaxRadius", 0xF636F07FU, 0x202, 0.0f, kNodePropertyRowMax},
            {"ConeEmitter_MinSpread", 0x100E7F55U, 0x202, 0.0f, 90.0f},
            {"ConeEmitter_MaxSpread", 0xFA4B5E41U, 0x202, 0.0f, 90.0f},
            {nullptr, 0, 0, 0.0f, 1.0f},
        };
        const FNodePropertyRow kCubePropertyRows[] = {
            {"CubeEmitter_Width", 0xFD22A11CU, 0x202, 0.0f, kNodePropertyRowMax},
            {"CubeEmitter_Height", 0xEFB0F8CFU, 0x202, 0.0f, kNodePropertyRowMax},
            {"CubeEmitter_Depth", 0x14B74EB2U, 0x202, 0.0f, kNodePropertyRowMax},
            {"CubeEmitter_MinSpread", 0xE282006BU, 0x202, 0.0f, 90.0f},
            {"CubeEmitter_MaxSpread", 0x08C7217FU, 0x202, 0.0f, 90.0f},
            {nullptr, 0, 0, 0.0f, 1.0f},
        };
        const FNodePropertyRow kSpherePropertyRows[] = {
            {"SphereEmitter_MinRadius", 0xF231FF60U, 0x202, 0.0f, kNodePropertyRowMax},
            {"SphereEmitter_MaxRadius", 0x1874DE74U, 0x202, 0.0f, kNodePropertyRowMax},
            {nullptr, 0, 0, 0.0f, 1.0f},
        };

        // The shared ramp-getter hit (e.g. the routine 0x06245a82..0x06245a92): *Out =
        // the ramp, AddRef, 0.
        GENRESULT HandOutRamp(INestedSingleRamp* Ramp, INestedSingleRamp** Out)
        {
            *Out = Ramp;
            Ramp->AddRef();
            return GR_OK;
        }

        const float kTwoPi = 6.2831855f;     
        const float kDegToRad = 0.017453292f; 

        // (thiscall on the vector): L = (x*x + y*y) + z*z FSTP float; one
        // Newton step of the 0x5f3759df inverse square root, s = (1.5f - ((y*y)*L)*0.5f)
        // * y kept in the FPU (the field = 0.5, the field = 1.5); v[i] = (float)(s *
        // v[i]).
        __declspec(noinline) void FastNormalize3(float* Vec)
        {
            float LengthSq;
            StoreParticleFxFpuFloat(LengthSq, static_cast<double>(Vec[0]) * Vec[0] +
                                              static_cast<double>(Vec[1]) * Vec[1] +
                                              static_cast<double>(Vec[2]) * Vec[2]);
            long Bits = 0;
            std::memcpy(&Bits, &LengthSq, sizeof(Bits));
            Bits = 0x5F3759DF - (Bits >> 1);
            float Guess = 0.0f;
            std::memcpy(&Guess, &Bits, sizeof(Guess));
            const double Scale =
                (1.5 - static_cast<double>(Guess) * Guess * LengthSq * 0.5) * Guess;
            StoreParticleFxFpuFloat(Vec[0], Scale * Vec[0]);
            StoreParticleFxFpuFloat(Vec[1], Scale * Vec[1]);
            StoreParticleFxFpuFloat(Vec[2], Scale * Vec[2]);
        }

        // The cone / cube direction (0621bab7..0621badd, cube 0621d467..0621d491): the
        // spread stays in the FPU; FSIN of it is reused for x and z.
        //   x = (float)(cos(Az) * sin(Sp)); y = (float)cos(Sp); z = (float)-(sin(Az) * sin(Sp)).
        __declspec(noinline) void SpreadDirection(float Azimuth, double Spread, float* Dir)
        {
            const double SinSpread = ComputeParticleFxFpuSine(Spread);
            const double AzimuthExt = Azimuth;
            StoreParticleFxFpuFloat(Dir[0], ComputeParticleFxFpuCosine(AzimuthExt) * SinSpread);
            StoreParticleFxFpuFloat(Dir[1], ComputeParticleFxFpuCosine(Spread));
            StoreParticleFxFpuFloat(Dir[2], -(ComputeParticleFxFpuSine(AzimuthExt) * SinSpread));
        }
    } // namespace

    // ---- FFxEmitterBase ------------------------------------------------------

    // ---- Emitter property sets ------------------------------------------------

    // (after the routine): +0x4c FxAnimatedSingleRamp (the routine) whose
    // key 0 is the routine(100.0); +0x50 (the routine) 10000.0; +0x54 2.0; +0x58 10.0;
    // +0x5c 0; +0x60 0; +0x64 = 0; +0x68 the routine(1.0) (FxRampSingleRamp with an
    // FxRampSingle child in mode 4 keyed 1.0).
    FFxEmitterPropertySet::FFxEmitterPropertySet()
        : FrequencyRamp(nullptr), MaxParticlesRamp(nullptr), InitLifeSpanRamp(nullptr),
          PressureRamp(nullptr), EmitCountRamp(nullptr), VelocityApproachRamp(nullptr),
          InitialParticles(0), LodCurveRamp(nullptr)
    {
        MakeAnimatedSingleRamp(100.0f, &FrequencyRamp);
        MakeAnimatedSingleRamp(10000.0f, &MaxParticlesRamp);
        MakeAnimatedSingleRamp(2.0f, &InitLifeSpanRamp);
        MakeAnimatedSingleRamp(10.0f, &PressureRamp);
        MakeAnimatedSingleRamp(0.0f, &EmitCountRamp);
        MakeAnimatedSingleRamp(0.0f, &VelocityApproachRamp);
        MakeRampSingleRamp(1.0f, &LodCurveRamp);
    }

    FFxEmitterPropertySet::~FFxEmitterPropertySet()
    {
        ReleaseDescriptorSlot(FrequencyRamp);
        ReleaseDescriptorSlot(MaxParticlesRamp);
        ReleaseDescriptorSlot(InitLifeSpanRamp);
        ReleaseDescriptorSlot(PressureRamp);
        ReleaseDescriptorSlot(EmitCountRamp);
        ReleaseDescriptorSlot(VelocityApproachRamp);
        ReleaseDescriptorSlot(LodCurveRamp);
    }

    // four FxAnimatedSingleRamps keyed 0, 1, 0, 0.
    FFxConeEmitterPropertySet::FFxConeEmitterPropertySet()
        : MinRadiusRamp(nullptr), MaxRadiusRamp(nullptr), MinSpreadRamp(nullptr),
          MaxSpreadRamp(nullptr)
    {
        MakeAnimatedSingleRamp(0.0f, &MinRadiusRamp);
        MakeAnimatedSingleRamp(1.0f, &MaxRadiusRamp);
        MakeAnimatedSingleRamp(0.0f, &MinSpreadRamp);
        MakeAnimatedSingleRamp(0.0f, &MaxSpreadRamp);
    }

    FFxConeEmitterPropertySet::~FFxConeEmitterPropertySet()
    {
        ReleaseDescriptorSlot(MinRadiusRamp);
        ReleaseDescriptorSlot(MaxRadiusRamp);
        ReleaseDescriptorSlot(MinSpreadRamp);
        ReleaseDescriptorSlot(MaxSpreadRamp);
    }

    // five FxAnimatedSingleRamps keyed 5, 5, 5, 0, 15.
    FFxCubeEmitterPropertySet::FFxCubeEmitterPropertySet()
        : WidthRamp(nullptr), HeightRamp(nullptr), DepthRamp(nullptr), MinSpreadRamp(nullptr),
          MaxSpreadRamp(nullptr)
    {
        MakeAnimatedSingleRamp(5.0f, &WidthRamp);
        MakeAnimatedSingleRamp(5.0f, &HeightRamp);
        MakeAnimatedSingleRamp(5.0f, &DepthRamp);
        MakeAnimatedSingleRamp(0.0f, &MinSpreadRamp);
        MakeAnimatedSingleRamp(15.0f, &MaxSpreadRamp);
    }

    FFxCubeEmitterPropertySet::~FFxCubeEmitterPropertySet()
    {
        ReleaseDescriptorSlot(WidthRamp);
        ReleaseDescriptorSlot(HeightRamp);
        ReleaseDescriptorSlot(DepthRamp);
        ReleaseDescriptorSlot(MinSpreadRamp);
        ReleaseDescriptorSlot(MaxSpreadRamp);
    }

    // 0x0623bbd4..0x0623bc68: +0x6c an FxAnimatedSingleRamp (the routine)
    // with AddKey(0, the routine(0)); +0x70 the routine(1.0).
    FFxSphereEmitterPropertySet::FFxSphereEmitterPropertySet()
        : MinRadiusRamp(nullptr), MaxRadiusRamp(nullptr)
    {
        MakeAnimatedSingleRamp(0.0f, &MinRadiusRamp);
        MakeAnimatedSingleRamp(1.0f, &MaxRadiusRamp);
    }

    FFxSphereEmitterPropertySet::~FFxSphereEmitterPropertySet()
    {
        ReleaseDescriptorSlot(MinRadiusRamp);
        ReleaseDescriptorSlot(MaxRadiusRamp);
    }

    // ---- FFxEmitterBase ------------------------------------------------------

    FFxEmitterBase::FFxEmitterBase(const char* PrimaryIid, const char* FactoryName)
        : FGenericFxObjectBase(PrimaryIid, FactoryName),
          LastEmitTime_(0.0f),
          Pressure_(0.0f),
          InitLifeSpan_(0.0f),
          bResetEmitHistory_(1),
          EmitFraction_(0.0f)
    {
        PrevNodePos_[0] = 0.0f;
        PrevNodePos_[1] = 0.0f;
        PrevNodePos_[2] = 0.0f;
        // on +0xbc (identity rotation), then +0xe0..+0xe8 = 0.
        FillIdentity3x3(EmitterXform_);
        EmitterXform_[9] = 0.0f;
        EmitterXform_[10] = 0.0f;
        EmitterXform_[11] = 0.0f;
        bIsEmitterNode_ = 1; // byte +0x95
    }

    FFxEmitterBase::~FFxEmitterBase()
    {
    }

    // .
    GENRESULT NEURONCOMPONENTS_API FFxEmitterPropertySet::GetPropInt(unsigned long Hash, unsigned long* Out)
    {
        if (Hash != kEmitterInitialParticlesHash)
            return GR_ALE_CREATE_FAIL;
        *Out = InitialParticles;
        return GR_OK;
    }

    // .
    GENRESULT NEURONCOMPONENTS_API FFxEmitterPropertySet::SetPropInt(unsigned long Hash, unsigned long Value)
    {
        if (Hash != kEmitterInitialParticlesHash)
            return GR_ALE_CREATE_FAIL;
        InitialParticles = Value;
        return GR_OK;
    }

    // (0x06245aa0..0x06245ac7).
    GENRESULT NEURONCOMPONENTS_API FFxEmitterPropertySet::GetPropSingleRampRamp(unsigned long Hash,
                                                                     INestedSingleRamp** Out)
    {
        if (Hash != kEmitterLodCurveHash)
            return GR_ALE_CREATE_FAIL;
        return HandOutRamp(LodCurveRamp, Out);
    }

    // (0x06245a20..0x06245a9a): Frequency +0x4c, MaxParticles +0x50,
    // EmitCount +0x5c, Pressure +0x58, InitLifeSpan +0x54, VelocityApproach +0x60, in
    // that test order; else -8.
    GENRESULT NEURONCOMPONENTS_API FFxEmitterPropertySet::GetPropAnimatedSingleRamp(unsigned long Hash,
                                                                         INestedSingleRamp** Out)
    {
        if (Hash == kEmitterFrequencyHash)
            return HandOutRamp(FrequencyRamp, Out);
        if (Hash == kEmitterMaxParticlesHash)
            return HandOutRamp(MaxParticlesRamp, Out);
        if (Hash == kEmitterEmitCountHash)
            return HandOutRamp(EmitCountRamp, Out);
        if (Hash == kEmitterPressureHash)
            return HandOutRamp(PressureRamp, Out);
        if (Hash == kEmitterInitLifeSpanHash)
            return HandOutRamp(InitLifeSpanRamp, Out);
        if (Hash == kEmitterVelocityApproachHash)
            return HandOutRamp(VelocityApproachRamp, Out);
        return GR_ALE_CREATE_FAIL;
    }

    // the base rows (the routine); a non-zero result is returned, else the
    // emitter rows (the routine over .data 0x06257700).
    int NEURONCOMPONENTS_API FFxEmitterPropertySet::EnumerateProps(FNodePropertyRowCallback Callback,
                                                        void* Context)
    {
        const int Result = FFxNodePropertySet::EnumerateProps(Callback, Context);
        if (Result != 0)
            return Result;
        return WalkPropertyRows(kEmitterPropertyRows, Callback, Context);
    }

    GENRESULT NEURONCOMPONENTS_API FFxEmitterBase::AssociateAppearance(INeuronComponent* Appearance)
    {
        if (AssociatedAppearance_ != Appearance)
        {
            if (AssociatedAppearance_ != nullptr)
            {
                AssociatedAppearance_->Release();
                AssociatedAppearance_ = nullptr;
            }
            AssociatedAppearance_ = Appearance;
            if (Appearance != nullptr)
                Appearance->AddRef();
        }
        bResetEmitHistory_ = 1;
        if (AssociatedAppearance_ != nullptr)
            AsFxNode(AssociatedAppearance_)->AddEmitterRef();
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FFxEmitterBase::SetNodeFlags(unsigned long Flags)
    {
        if (AssociatedAppearance_ != nullptr && static_cast<long>(Flags) < 0)
            AsFxNode(AssociatedAppearance_)->ReleaseEmitterRef();
        NodeFlags_ = Flags;
        return GR_OK;
    }

    // (RET 8: node, Dt). Descriptor D = [node+0x9c]; every ramp is evaluated
    // with +0x1C Eval(node+0x8c, InnerTime).
    GENRESULT FFxEmitterBase::UpdateEmitter(float Dt)
    {
        // node vt+0x44.
        TickLifetime(Dt);

        // , 062460a9: no appearance -> done; a dead appearance
        // or a dead emitter drops the association.
        if (AssociatedAppearance_ == nullptr)
            return GR_OK;
        IGenericFx* const Appearance = AsFxNode(AssociatedAppearance_);
        if (static_cast<unsigned char>(Appearance->IsNodeDead()) != 0 ||
            static_cast<long>(NodeFlags_) < 0)
        {
            AssociatedAppearance_->Release();
            AssociatedAppearance_ = nullptr;
            return GR_OK;
        }

        // node slot 7 (the routine) -> +0xbc.
        ComputeRelativeTransform(AssociatedAppearance_, EmitterXform_);

        // D+0x58 -> +0xb0, D+0x54 -> +0xb4, D+0x4c -> the local
        // frequency, each FSTP float, at (+0x8c, Age).
        FFxEmitterPropertySet* const Set = EmitterProps(); // node+0x9c
        Pressure_ = static_cast<float>(Set->PressureRamp->Eval(AppearanceOuterTime_, Age_));
        InitLifeSpan_ =
            static_cast<float>(Set->InitLifeSpanRamp->Eval(AppearanceOuterTime_, Age_));
        const float Frequency =
            static_cast<float>(Set->FrequencyRamp->Eval(AppearanceOuterTime_, Age_));

        // Age - LastEmit is FST'd (Elapsed, float) but the FMUL by
        // the frequency uses the unrounded difference; ftol truncates.
        const double ElapsedExt = static_cast<double>(Age_) - LastEmitTime_;
        const float Elapsed = static_cast<float>(ElapsedExt);
        long EmitCount = static_cast<long>(ElapsedExt * Frequency);

        // first frame after construction / re-association:
        // + D+0x64 (InitialParticles, ADD), PrevNodePos = node+0x70.. (translation).
        if (bResetEmitHistory_ != 0)
        {
            bResetEmitHistory_ = 0;
            EmitCount =
                static_cast<long>(static_cast<unsigned long>(EmitCount) + Set->InitialParticles);
            PrevNodePos_[0] = NodeWorldXform_[9];
            PrevNodePos_[1] = NodeWorldXform_[10];
            PrevNodePos_[2] = NodeWorldXform_[11];
        }

        // keyed bursts. D+0x5c's +0x24 GetKeyValue(0, &Keys) (AddRefs);
        // Keys->GetKeyCount (+0x14) with no null check; every key with
        // LastEmitTime_ <= time <= Age adds its count.
        {
            INeuronComponent* KeyObject = nullptr;
            Set->EmitCountRamp->GetKeyValue(0, &KeyObject);
            IFxAnimatedSingle* Keys = static_cast<IFxAnimatedSingle*>(KeyObject);
            const long KeyCount = static_cast<long>(Keys->GetKeyCount());
            long KeyIndex = 0;
            for (KeyIndex = 0; KeyIndex < KeyCount; ++KeyIndex)
            {
                // GetKeyTime (+0x2C); a non-zero result skips the key.
                float KeyTime = 0.0f;
                if (Keys->GetKeyTime(static_cast<int>(KeyIndex), &KeyTime) != GR_OK)
                    continue;
                // FCOMP LastEmit; TEST AH,5; JNP skips when only C0 is set (time < last).
                if (KeyTime < LastEmitTime_)
                    continue;
                // FCOMP Age; AND 0x4100; JZ stops when neither C0 nor C3 (time > Age).
                if (KeyTime > Age_)
                    break;
                // GetKeyValue (+0x30) into a zeroed triple.
                float Value[3] = { 0.0f, 0.0f, 0.0f };
                if (Keys->GetKeyValue(static_cast<int>(KeyIndex), Value) != GR_OK)
                    continue;
                // `FILD count; FADD Value[0]; CALL ftol` -- the sum stays on the x87 stack.
                EmitCount = static_cast<long>(static_cast<double>(EmitCount) + Value[0]);
            }
            // the holder releases a non-null object.
            if (Keys != nullptr)
                Keys->Release();
        }

        // D+0x68 at (+0x8c, +0x90 LodInput) FIMUL count, FADD
        // +0xec, FST float +0xec; ftol of the unrounded sum; FILD the result, FSTP float
        // (EmitNowF), +0xec -= EmitNowF.
        const double FractionExt =
            Set->LodCurveRamp->Eval(AppearanceOuterTime_, LodInput_) * EmitCount + EmitFraction_;
        EmitFraction_ = static_cast<float>(FractionExt);
        long EmitNow = static_cast<long>(FractionExt);
        const float EmitNowF = static_cast<float>(EmitNow);
        EmitFraction_ = EmitFraction_ - EmitNowF;

        // nothing to emit. FCOMP 0.0; TEST AH,0x44; JP returns
        // unless exactly C3 (equal): a zero frequency settles the history at Age.
        if (EmitNow == 0)
        {
            if (Frequency == 0.0f)
                LastEmitTime_ = Age_;
            return GR_OK;
        }

        // how far the emitter moved since the last emit, in this
        // frame's local space (math +0x3C InverseTransform); 1.0f / EmitNowF stays in the
        // FPU for the per-particle step and is FST'd to float for the time step.
        float Delta[3] = { 0.0f, 0.0f, 0.0f };
        Acquire3DMath()->InverseTransform(*reinterpret_cast<FVector*>(Delta),
                                          *reinterpret_cast<const FTransform*>(NodeWorldXform_),
                                          *reinterpret_cast<const FVector*>(PrevNodePos_)); // +0x3C
        const double InvCountExt = 1.0 / static_cast<double>(EmitNowF);
        float FullDelta[3] = { Delta[0], Delta[1], Delta[2] };
        float Velocity[3] = { 0.0f, 0.0f, 0.0f };
        PrevNodePos_[0] = NodeWorldXform_[9];
        PrevNodePos_[1] = NodeWorldXform_[10];
        PrevNodePos_[2] = NodeWorldXform_[11];
        const float InvCount = static_cast<float>(InvCountExt);
        float Step[3] = { static_cast<float>(Delta[0] * InvCountExt),
                          static_cast<float>(Delta[1] * InvCountExt),
                          static_cast<float>(Delta[2] * InvCountExt) };

        // FCOMP 0.0; AND 0x4100; JNZ skips Elapsed <= 0 (and
        // unordered). N = 1.0f / -Elapsed (FCHS, FDIVR) stays in the FPU; the scaled
        // delta is FSTP float; D+0x60 Eval stays in the FPU for the product.
        if (Elapsed > 0.0f)
        {
            const double NegInvElapsed = 1.0 / -static_cast<double>(Elapsed);
            const float Scaled[3] = { static_cast<float>(Delta[0] * NegInvElapsed),
                                      static_cast<float>(Delta[1] * NegInvElapsed),
                                      static_cast<float>(Delta[2] * NegInvElapsed) };
            const double Approach = Set->VelocityApproachRamp->Eval(AppearanceOuterTime_, Age_);
            Velocity[0] = static_cast<float>(Scaled[0] * Approach);
            Velocity[1] = static_cast<float>(Scaled[1] * Approach);
            Velocity[2] = static_cast<float>(Scaled[2] * Approach);
        }

        // sub-frame spacing (the float InvCount times Elapsed); no
        // motion compensation when the appearance shares this emitter's parent
        // (outer +0x28 == node+8).
        float AgeOffset = Elapsed;
        const float TimeStep = InvCount * Elapsed;
        if (Appearance->GetAttachParent() == AttachParent_)
        {
            Step[0] = 0.0f;
            Step[1] = 0.0f;
            Step[2] = 0.0f;
            FullDelta[0] = 0.0f;
            FullDelta[1] = 0.0f;
            FullDelta[2] = 0.0f;
        }

        // D+0x50 FSTP float; FILD the appearance's live count
        // (outer +0xCC), FSUBR (kept in the FPU); FCOM EmitNowF; TEST AH,5; JP keeps N
        // unless only C0 is set (room < N), then N = ftol(room).
        const float MaxParticles =
            static_cast<float>(Set->MaxParticlesRamp->Eval(AppearanceOuterTime_, Age_));
        unsigned long LiveCount = 0;
        Appearance->GetLiveInstanceCount(&LiveCount);
        const double Room =
            static_cast<double>(MaxParticles) - static_cast<double>(static_cast<long>(LiveCount));
        if (Room < EmitNowF)
            EmitNow = static_cast<long>(Room);

        // outer +0xB4, N x node slot 20, outer +0xC0.
        Appearance->Pad45();
        if (EmitNow > 0)
        {
            do
            {
                AgeOffset = AgeOffset - TimeStep;
                FullDelta[0] = FullDelta[0] - Step[0];
                FullDelta[1] = FullDelta[1] - Step[1];
                FullDelta[2] = FullDelta[2] - Step[2];
                EmitParticle(AgeOffset, FullDelta, Velocity);
                --EmitNow;
            } while (EmitNow != 0);
        }
        Appearance->Pad48();

        // .
        LastEmitTime_ = Elapsed + LastEmitTime_;
        return GR_OK;
    }

    void FFxEmitterBase::SubmitParticle(const float* Pos3, const float* Vel3, const float& AgeOffset,
                                       const float* PosOffset3, const float* VelIn3,
                                       I3DMathEngine*& MathSlot)
    {
        // Q = W * AgeOffset (the routine, FSTP float each).
        float Q[3];
        StoreParticleFxFpuFloat(Q[2], static_cast<double>(Vel3[2]) * AgeOffset);
        StoreParticleFxFpuFloat(Q[1], static_cast<double>(Vel3[1]) * AgeOffset);
        StoreParticleFxFpuFloat(Q[0], static_cast<double>(Vel3[0]) * AgeOffset);
        // (cone; cube 0621d541.., sphere 0623be5c..): x and y sums stay
        // in the FPU until + PosOffset; the z sum is FSTP'd first.
        float Pos[3];
        const double SumX = static_cast<double>(Q[0]) + Pos3[0];
        const double SumY = static_cast<double>(Pos3[1]) + Q[1];
        float SumZ;
        StoreParticleFxFpuFloat(SumZ, static_cast<double>(Pos3[2]) + Q[2]);
        StoreParticleFxFpuFloat(Pos[0], SumX + PosOffset3[0]);
        StoreParticleFxFpuFloat(Pos[1], SumY + PosOffset3[1]);
        StoreParticleFxFpuFloat(Pos[2], static_cast<double>(SumZ) + PosOffset3[2]);
        float Vel[3];
        StoreParticleFxFpuFloat(Vel[0], static_cast<double>(Vel3[0]) + VelIn3[0]);
        StoreParticleFxFpuFloat(Vel[1], static_cast<double>(Vel3[1]) + VelIn3[1]);
        StoreParticleFxFpuFloat(Vel[2], static_cast<double>(Vel3[2]) + VelIn3[2]);

        I3DMathEngine* Math = MathSlot;
        if (Math == nullptr)
        {
            Math = Create3DMath();
            MathSlot = Math;
        }
        float Rotated[3] = { 0.0f, 0.0f, 0.0f };
        Math->Rotate(*reinterpret_cast<FVector*>(Rotated),
                     *reinterpret_cast<const FTransform*>(EmitterXform_),
                     *reinterpret_cast<const FVector*>(Vel)); // +0x30
        float Six[6];
        std::memcpy(Six + 3, Rotated, sizeof(Rotated));
        Math = MathSlot;
        if (Math == nullptr)
        {
            Math = Create3DMath();
            MathSlot = Math;
        }
        float Placed[3] = { 0.0f, 0.0f, 0.0f };
        Math->TransformXform(*reinterpret_cast<FVector*>(Placed),
                             *reinterpret_cast<const FTransform*>(EmitterXform_),
                             *reinterpret_cast<const FVector*>(Pos)); // +0x28

        std::memcpy(Six, Placed, sizeof(Placed));
        IGenericFx* const Appearance = AsFxNode(AssociatedAppearance_);
        if (ForwardEmitterPad46(Appearance, Six, AgeOffset, InitLifeSpan_) >= 0)
            ForwardEmitterPad47(AsFxNode(AssociatedAppearance_), Six, AgeOffset, InitLifeSpan_);
    }

    // ---- FxConeEmitter -------------------------------------------------------

    FxConeEmitter::FxConeEmitter(const char* PrimaryIid, const char* FactoryName)
        : FFxEmitterBase(PrimaryIid, FactoryName),
          MinSpreadRad_(0.0f),
          SpreadRangeRad_(0.0f),
          MinRadius_(0.0f),
          RadiusRange_(0.0f)
    {
    }

    FxConeEmitter::~FxConeEmitter()
    {
    }

    // operator_new(0x84), the routine on +4, the set's vtables, count 1;
    // node +0x7c / +0x80 / +0x9c / +0xf0.
    void FxConeEmitter::CreatePropertySet()
    {
        AdoptPropertySet(new FFxConeEmitterPropertySet());
    }

    // (0x0621b890..0x0621b9c1, RET 8; this = the prototype's node):
    // operator_new(0x10c) and the cone's constructor inline (0x0621b8cc..0x0621b976, the
    // same stores as the factory's 0x0621c546.. body), then the routine (the routine and
    // the +0x9c set cache) and the +0xf0 set cache (0x0621b99c..0x0621b9a7); *Out = the new
    // outer, 0. Both caches are PropertySet_ here.
    GENRESULT NEURONCOMPONENTS_API FxConeEmitter::CloneNode(INeuronComponent** Out)
    {
        FxConeEmitter* const Node = new FxConeEmitter(PrimaryIid_, FactoryName_);
        Node->InitNodeFromArchetype(this);
        *Out = Node;
        return GR_OK;
    }

    // (RET 0xc, through the outer thunk): 0x1C73D16B -> +0x6c, 0xF636F07F ->
    // +0x70, 0x100E7F55 -> +0x74, 0xFA4B5E41 -> +0x78 (AddRef'd, 0); otherwise JMP
    // .
    GENRESULT NEURONCOMPONENTS_API FFxConeEmitterPropertySet::GetPropAnimatedSingleRamp(
        unsigned long Hash, INestedSingleRamp** Out)
    {
        if (Hash == kConeMinRadiusHash)
            return HandOutRamp(MinRadiusRamp, Out);
        if (Hash == kConeMaxRadiusHash)
            return HandOutRamp(MaxRadiusRamp, Out);
        if (Hash == kConeMinSpreadHash)
            return HandOutRamp(MinSpreadRamp, Out);
        if (Hash == kConeMaxSpreadHash)
            return HandOutRamp(MaxSpreadRamp, Out);
        return FFxEmitterPropertySet::GetPropAnimatedSingleRamp(Hash, Out);
    }

    // the routine, then .data 0x06255e28.
    int NEURONCOMPONENTS_API FFxConeEmitterPropertySet::EnumerateProps(FNodePropertyRowCallback Callback,
                                                            void* Context)
    {
        const int Result = FFxEmitterPropertySet::EnumerateProps(Callback, Context);
        if (Result != 0)
            return Result;
        return WalkPropertyRows(kConePropertyRows, Callback, Context);
    }

    // (RET 8: node, Dt): t = (float)(Dt + Age); D = [node+0xf0];
    // +0xfc = D+0x6c Eval; +0x100 = D+0x70 Eval - [+0xfc]; +0xf4 = D+0x74 Eval * 0.017453292f;
    // +0xf8 = D+0x78 Eval * 0.017453292f - [+0xf4] (each FSTP float, Evals at (+0x8c, t));
    // then the routine(node, Dt).
    GENRESULT NEURONCOMPONENTS_API FxConeEmitter::Pad5(float Dt)
    {
        FFxConeEmitterPropertySet* const Set = ConeProps();
        const float Time = Dt + Age_;
        MinRadius_ = static_cast<float>(Set->MinRadiusRamp->Eval(AppearanceOuterTime_, Time));
        RadiusRange_ = static_cast<float>(Set->MaxRadiusRamp->Eval(AppearanceOuterTime_, Time) -
                                          MinRadius_);
        MinSpreadRad_ = static_cast<float>(Set->MinSpreadRamp->Eval(AppearanceOuterTime_, Time) *
                                           kDegToRad);
        SpreadRangeRad_ = static_cast<float>(
            Set->MaxSpreadRamp->Eval(AppearanceOuterTime_, Time) * kDegToRad - MinSpreadRad_);
        return UpdateEmitter(Dt);
    }

    // (thiscall, RET 0xc): Az = (float)(rand * 2pi); Sp = rand * [+0xf8] +
    // [+0xf4] (FPU); Dir from SpreadDirection; R = rand * [+0x100] + [+0xfc] (FPU);
    // P = (float)(Dir * R); W = (float)(Dir * [+0xb0]); the common tail.
    void FxConeEmitter::EmitParticle(float AgeOffset, const float* PosOffset3, const float* Vel3)
    {
        const float Azimuth = static_cast<float>(ParticleFxRand01() * kTwoPi);
        const double Spread = ParticleFxRand01() * SpreadRangeRad_ + MinSpreadRad_;
        float Dir[3];
        SpreadDirection(Azimuth, Spread, Dir);
        const double Radius = ParticleFxRand01() * RadiusRange_ + MinRadius_;
        float Pos[3];
        StoreParticleFxFpuFloat(Pos[2], Dir[2] * Radius);
        StoreParticleFxFpuFloat(Pos[1], Dir[1] * Radius);
        StoreParticleFxFpuFloat(Pos[0], Dir[0] * Radius);
        const float Vel[3] = { Dir[0] * Pressure_, Dir[1] * Pressure_, Dir[2] * Pressure_ };
        SubmitParticle(Pos, Vel, AgeOffset, PosOffset3, Vel3, G_ConeEmitter3DMath);
    }

    // ---- FxCubeEmitter -------------------------------------------------------

    FxCubeEmitter::FxCubeEmitter(const char* PrimaryIid, const char* FactoryName)
        : FFxEmitterBase(PrimaryIid, FactoryName),
          MinSpreadRad_(0.0f),
          SpreadRangeRad_(0.0f),
          Width_(0.0f),
          Height_(0.0f),
          Depth_(0.0f)
    {
    }

    FxCubeEmitter::~FxCubeEmitter()
    {
    }

    // operator_new(0x88), the routine on +4, the set's vtables, count 1.
    void FxCubeEmitter::CreatePropertySet()
    {
        AdoptPropertySet(new FFxCubeEmitterPropertySet());
    }

    // (RET 8): the cube's constructor inline (as its factory 0x0621dd26..),
    // (0x0621d32d), the +0xf0 set cache (0x0621d332..0x0621d33d); *Out, 0.
    GENRESULT NEURONCOMPONENTS_API FxCubeEmitter::CloneNode(INeuronComponent** Out)
    {
        FxCubeEmitter* const Node = new FxCubeEmitter(PrimaryIid_, FactoryName_);
        Node->InitNodeFromArchetype(this);
        *Out = Node;
        return GR_OK;
    }

    // (RET 0xc): 0xFD22A11C -> +0x6c, 0xEFB0F8CF -> +0x70, 0x14B74EB2 ->
    // +0x74, 0xE282006B -> +0x78, 0x08C7217F -> +0x7c (AddRef'd, 0); otherwise JMP
    // .
    GENRESULT NEURONCOMPONENTS_API FFxCubeEmitterPropertySet::GetPropAnimatedSingleRamp(
        unsigned long Hash, INestedSingleRamp** Out)
    {
        if (Hash == kCubeWidthHash)
            return HandOutRamp(WidthRamp, Out);
        if (Hash == kCubeHeightHash)
            return HandOutRamp(HeightRamp, Out);
        if (Hash == kCubeDepthHash)
            return HandOutRamp(DepthRamp, Out);
        if (Hash == kCubeMinSpreadHash)
            return HandOutRamp(MinSpreadRamp, Out);
        if (Hash == kCubeMaxSpreadHash)
            return HandOutRamp(MaxSpreadRamp, Out);
        return FFxEmitterPropertySet::GetPropAnimatedSingleRamp(Hash, Out);
    }

    // the routine, then .data 0x06255f30.
    int NEURONCOMPONENTS_API FFxCubeEmitterPropertySet::EnumerateProps(FNodePropertyRowCallback Callback,
                                                            void* Context)
    {
        const int Result = FFxEmitterPropertySet::EnumerateProps(Callback, Context);
        if (Result != 0)
            return Result;
        return WalkPropertyRows(kCubePropertyRows, Callback, Context);
    }

    // (RET 8): t = (float)(Dt + Age); +0xfc / +0x100 / +0x104 = D+0x6c /
    // +0x70 / +0x74 Eval; +0xf4 = D+0x78 Eval * 0.017453292f; +0xf8 = D+0x7c Eval *
    // 0.017453292f - [+0xf4] (FSTP float each); the routine(node, Dt).
    GENRESULT NEURONCOMPONENTS_API FxCubeEmitter::Pad5(float Dt)
    {
        FFxCubeEmitterPropertySet* const Set = CubeProps();
        const float Time = Dt + Age_;
        Width_ = static_cast<float>(Set->WidthRamp->Eval(AppearanceOuterTime_, Time));
        Height_ = static_cast<float>(Set->HeightRamp->Eval(AppearanceOuterTime_, Time));
        Depth_ = static_cast<float>(Set->DepthRamp->Eval(AppearanceOuterTime_, Time));
        MinSpreadRad_ = static_cast<float>(Set->MinSpreadRamp->Eval(AppearanceOuterTime_, Time) *
                                           kDegToRad);
        SpreadRangeRad_ = static_cast<float>(
            Set->MaxSpreadRamp->Eval(AppearanceOuterTime_, Time) * kDegToRad - MinSpreadRad_);
        return UpdateEmitter(Dt);
    }

    // (thiscall, RET 0xc): Az, Sp and Dir as the cone; then three draws,
    // each ((rand - 0.5f) * extent) FADD ST0,ST0 FSTP float: height (y), width (x),
    // depth (z); P = (x, y, z); W = (float)(Dir * [+0xb0]); the common tail.
    void FxCubeEmitter::EmitParticle(float AgeOffset, const float* PosOffset3, const float* Vel3)
    {
        const float Azimuth = static_cast<float>(ParticleFxRand01() * kTwoPi);
        const double Spread = ParticleFxRand01() * SpreadRangeRad_ + MinSpreadRad_;
        float Dir[3];
        SpreadDirection(Azimuth, Spread, Dir);
        float Pos[3];
        StoreParticleFxFpuFloat(Pos[1], (ParticleFxRand01() - 0.5) * Height_ * 2.0);
        StoreParticleFxFpuFloat(Pos[0], (ParticleFxRand01() - 0.5) * Width_ * 2.0);
        StoreParticleFxFpuFloat(Pos[2], (ParticleFxRand01() - 0.5) * Depth_ * 2.0);
        const float Vel[3] = { Dir[0] * Pressure_, Dir[1] * Pressure_, Dir[2] * Pressure_ };
        SubmitParticle(Pos, Vel, AgeOffset, PosOffset3, Vel3, G_CubeEmitter3DMath);
    }

    // ---- FxSphereEmitter -----------------------------------------------------

    FxSphereEmitter::FxSphereEmitter(const char* PrimaryIid, const char* FactoryName)
        : FFxEmitterBase(PrimaryIid, FactoryName), MinRadius_(0.0f), RadiusRange_(0.0f)
    {
    }

    FxSphereEmitter::~FxSphereEmitter()
    {
    }

    // operator_new(0x7c), the emitter set (the routine) on +4 plus the two
    // sphere ramps, the set's vtables, count 1 (0x0623bc81).
    void FxSphereEmitter::CreatePropertySet()
    {
        AdoptPropertySet(new FFxSphereEmitterPropertySet());
    }

    // (RET 8): the sphere's constructor inline (as its factory 0x0623ca46..),
    // (0x0623bb4b), the +0xf0 set cache (0x0623bb50..0x0623bb5b); *Out, 0.
    GENRESULT NEURONCOMPONENTS_API FxSphereEmitter::CloneNode(INeuronComponent** Out)
    {
        FxSphereEmitter* const Node = new FxSphereEmitter(PrimaryIid_, FactoryName_);
        Node->InitNodeFromArchetype(this);
        *Out = Node;
        return GR_OK;
    }

    // (RET 0xc): 0xF231FF60 -> +0x6c, 0x1874DE74 -> +0x70 (AddRef'd, 0);
    // otherwise JMP the routine.
    GENRESULT NEURONCOMPONENTS_API FFxSphereEmitterPropertySet::GetPropAnimatedSingleRamp(
        unsigned long Hash, INestedSingleRamp** Out)
    {
        if (Hash == kSphereMinRadiusHash)
            return HandOutRamp(MinRadiusRamp, Out);
        if (Hash == kSphereMaxRadiusHash)
            return HandOutRamp(MaxRadiusRamp, Out);
        return FFxEmitterPropertySet::GetPropAnimatedSingleRamp(Hash, Out);
    }

    // the routine, then .data 0x06256c4c.
    int NEURONCOMPONENTS_API FFxSphereEmitterPropertySet::EnumerateProps(FNodePropertyRowCallback Callback,
                                                              void* Context)
    {
        const int Result = FFxEmitterPropertySet::EnumerateProps(Callback, Context);
        if (Result != 0)
            return Result;
        return WalkPropertyRows(kSpherePropertyRows, Callback, Context);
    }

    // (RET 8): t = (float)(Dt + Age); +0xf4 = D+0x6c Eval; +0xf8 = D+0x70
    // Eval - [+0xf4] (FSTP float); the routine(node, Dt).
    GENRESULT NEURONCOMPONENTS_API FxSphereEmitter::Pad5(float Dt)
    {
        FFxSphereEmitterPropertySet* const Set = SphereProps();
        const float Time = Dt + Age_;
        MinRadius_ = static_cast<float>(Set->MinRadiusRamp->Eval(AppearanceOuterTime_, Time));
        RadiusRange_ = static_cast<float>(Set->MaxRadiusRamp->Eval(AppearanceOuterTime_, Time) -
                                          MinRadius_);
        return UpdateEmitter(Dt);
    }

    // (thiscall, RET 0xc): three draws (rand - 0.5f) FADD ST0,ST0 FSTP
    // float -- the first is y, the second x, the third z; the routine normalises;
    // R = rand * [+0xf8] + [+0xf4] (FPU); P = (float)(Dir * R); W = (float)(Dir *
    // [+0xb0]); the common tail.
    void FxSphereEmitter::EmitParticle(float AgeOffset, const float* PosOffset3, const float* Vel3)
    {
        float Dir[3];
        StoreParticleFxFpuFloat(Dir[1], (ParticleFxRand01() - 0.5) * 2.0);
        StoreParticleFxFpuFloat(Dir[0], (ParticleFxRand01() - 0.5) * 2.0);
        StoreParticleFxFpuFloat(Dir[2], (ParticleFxRand01() - 0.5) * 2.0);
        FastNormalize3(Dir);
        const double Radius = ParticleFxRand01() * RadiusRange_ + MinRadius_;
        float Pos[3];
        StoreParticleFxFpuFloat(Pos[2], Dir[2] * Radius);
        StoreParticleFxFpuFloat(Pos[1], Dir[1] * Radius);
        StoreParticleFxFpuFloat(Pos[0], Dir[0] * Radius);
        const float Vel[3] = { Dir[0] * Pressure_, Dir[1] * Pressure_, Dir[2] * Pressure_ };
        SubmitParticle(Pos, Vel, AgeOffset, PosOffset3, Vel3, G_SphereEmitter3DMath);
    }

} // namespace ParticleFxImpl
