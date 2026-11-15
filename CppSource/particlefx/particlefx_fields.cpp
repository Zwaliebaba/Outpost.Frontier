// Author: Giperion.

#include "particlefx_fields.h"

#include "particlefx_common.h"
#include "particlefx_math.h"
#include "particlefx_ramp.h"

#include <cmath>
#include <cstring>
#include <new>

// particlefx.dll — typed field property sets and field behaviors.
// baseline branch, arithmetic and ownership evidence lives in each tracker item.
namespace ParticleFxImpl
{
    namespace
    {
        // (particlefx.dll .rdata bytes 00 00 A0 40 = 0x40A00000):
        // confirmed byte-for-byte wave 15 (sonnet:particlefx-wave15-lane2) as
        // exactly 5.0f. Used only by FxCollideField::BeginField below to scale
        // the confirmed-normalized plane normal into ScaledNormal_ (baseline raw
        // listing lines 54862-54876).
        const float kCollideNormalScale = 5.0f;

        // (particlefx.dll .rdata bytes fc a9 f1 d2 4d 62 50 3f, an
        // 8-byte double): confirmed byte-for-byte wave 63
        // (sonnet:particlefx-wave63-lane2) as the IEEE-754 double bit pattern for
        // exactly decimal 0.001 -- distinct from the 1.0f (the field) and
        // 1e-6f constants used elsewhere in this file. Used only by
        // FxCollideField::ApplyFieldToVel below as the post-impact velocity-
        // magnitude gate (baseline raw listing lines 53716-53720): below this
        // speed, baseline leaves the velocity untouched (only the impact
        // position is written) rather than attempting to reflect a
        // near-stationary particle.
        const double kCollideSpeedEpsilon = 0.001;

        // (.data, 0 at load): read once, by the dust field's Apply
        // (0x06212d80); no instruction in the image writes it.
        unsigned char G_DustAgeMinusUpdateDt = 0;

        I3DMathEngine* G_DustField3DMath = nullptr; // .
        I3DMathEngine* G_AirField3DMath = nullptr; // .
        I3DMathEngine* G_CollideField3DMath = nullptr; // .
        I3DMathEngine* G_GravityField3DMath = nullptr; // .

        // Radial Begin owns this cache, independent of other fields.
        I3DMathEngine* G_RadialField3DMath = nullptr;

        // The field sets' property tables (.data 0x06256240 gravity, 0x06255af4 air,
        // radial, 0x06256ccc turbulence, 0x06255d78 collide, 0x06255aa8 dust),
        // each enumerated after the base rows.
        const FNodePropertyRow kGravityPropertyRows[] = {
            {"GravityField_Gravity", 0xE02B8BD4U, 0x202, 0.0f, kNodePropertyRowMax},
            {nullptr, 0, 0, 0.0f, 1.0f},
        };
        const FNodePropertyRow kAirPropertyRows[] = {
            {"AirField_Magnitude", 0xE5E3524CU, 0x202, -kNodePropertyRowMax, kNodePropertyRowMax},
            {"AirField_Approach", 0x10423CEBU, 0x202, 0.0f, 2.0f},
            {nullptr, 0, 0, 0.0f, 1.0f},
        };
        const FNodePropertyRow kRadialPropertyRows[] = {
            {"RadialField_Radius", 0xFF28F620U, 0x202, 0.0f, kNodePropertyRowMax},
            {"RadialField_Attenuation", 0x1A433168U, 0x200, -1.0f, 1.0f},
            {"RadialField_Magnitude", 0x0064B3B9U, 0x202, -kNodePropertyRowMax, kNodePropertyRowMax},
            {"RadialField_Approach", 0xF3CF5EA5U, 0x202, 0.0f, 10.0f},
            {nullptr, 0, 0, 0.0f, 1.0f},
        };
        const FNodePropertyRow kTurbulencePropertyRows[] = {
            {"TurbulenceField_Magnitude", 0x1CEAC6D1U, 0x202, -kNodePropertyRowMax, kNodePropertyRowMax},
            {"TurbulenceField_Approach", 0x1B970B9AU, 0x202, 0.0f, 2.0f},
            {nullptr, 0, 0, 0.0f, 1.0f},
        };
        const FNodePropertyRow kCollidePropertyRows[] = {
            {"CollideField_Reflectivity", 0xF2468210U, 0x202, 0.0f, 1.0f},
            {"CollideField_Width", 0x138BDB51U, 0x202, 0.0f, kNodePropertyRowMax},
            {"CollideField_Height", 0x11336C98U, 0x202, 0.0f, kNodePropertyRowMax},
            {nullptr, 0, 0, 0.0f, 1.0f},
        };
        const FNodePropertyRow kDustFieldPropertyRows[] = {
            {"SphereEmitter_MaxRadius", 0x1874DE74U, 0x202, 60.1f, kNodePropertyRowMax},
            {nullptr, 0, 0, 0.0f, 1.0f},
        };

        // A ramp getter's hit: *Out = the ramp, AddRef, 0.
        GENRESULT HandOutFieldRamp(INestedSingleRamp* Ramp, INestedSingleRamp** Out)
        {
            *Out = Ramp;
            Ramp->AddRef();
            return GR_OK;
        }

        // Collide's owned members unwind in reverse construction order (0625146c),
        // and its destructor advances the EH state before each Release (06251564).
        struct FCollideRampCleanup
        {
            INestedSingleRamp** Slots[4];
            unsigned long Remaining;

            ~FCollideRampCleanup() noexcept(false) { ReleaseRemaining(); }

            void ReleaseRemaining()
            {
                while (Remaining != 0)
                {
                    --Remaining;
                    ReleaseDescriptorSlot(*Slots[Remaining]);
                }
            }
        };

        void MakeCollideAnimatedSingleRamp(float Weight, INestedSingleRamp** Out)
        {
            FAnimatedSingleRamp* const Ramp = new FAnimatedSingleRamp();
            struct FChildReference
            {
                FFxAnimatedSingle* Child;
                ~FChildReference() noexcept(false)
                {
                    if (Child != nullptr)
                        Child->Release();
                }
            };
            FChildReference ChildReference = {nullptr};
            FFxAnimatedSingle* const Child = new FFxAnimatedSingle();
            Child->SetWeight(Weight);
            ChildReference.Child = Child;
            Ramp->AddKey(0.0f, Child);
            *Out = Ramp;
        }
    } // namespace

    // ---- Field property sets ------------------------------------------------

    // Gravity create the routine (0x062233ba..0x06223427): +0x4c an FxAnimatedSingleRamp
    // (the routine, vtable 0624bac8) whose key 0 is an FxAnimatedSingle of 9.8
    // (0x411ccccd, the routine; +0x14 AddKey(0, key)).
    FFxGravityFieldPropertySet::FFxGravityFieldPropertySet() : GravityRamp(nullptr)
    {
        FCollideRampCleanup Cleanup = {{&GravityRamp, nullptr, nullptr, nullptr}, 1};
        MakeAnimatedSingleRamp(kDefaultGravity, &GravityRamp);
        Cleanup.Remaining = 0;
    }

    FFxGravityFieldPropertySet::~FFxGravityFieldPropertySet()
    {
        ReleaseDescriptorSlot(GravityRamp);
    }

    // Air create the routine (0x06214064..0x062140fb): +0x4c an FxAnimatedSingleRamp
    // (the routine) with an FxAnimatedSingle key of 1.0; +0x50 the routine(1.0).
    FFxAirFieldPropertySet::FFxAirFieldPropertySet() : ApproachRamp(nullptr), MagnitudeRamp(nullptr)
    {
        FCollideRampCleanup Cleanup = {{&ApproachRamp, &MagnitudeRamp, nullptr, nullptr}, 2};
        MakeAnimatedSingleRamp(kDefaultAirApproach, &ApproachRamp);
        MakeAnimatedSingleRamp(kDefaultAirMagnitude, &MagnitudeRamp);
        Cleanup.Remaining = 0;
    }

    FFxAirFieldPropertySet::~FFxAirFieldPropertySet()
    {
        ReleaseDescriptorSlot(ApproachRamp);
        ReleaseDescriptorSlot(MagnitudeRamp);
    }

    // (0x06235f50..0x062361cb): +0x4c / +0x50 / +0x54 zeroed, then each gets
    // an FxAnimatedSingleRamp (the routine) whose key 0 is an FxAnimatedSingle of weight
    // 10.0 (0x41200000), 0.3 (0x3e99999a) and 50.0 (0x42480000) (the routine, +0x14
    // AddKey). Last, +0x58 (0x062360fa..0x062361b4): the routine releases the slot; a new
    // FxRampSingleRamp (the routine) gets the routine(1.0) as its key 0 and is stored;
    // then GetKeyValue(0, &Child) (+0x24), Child->AddKey(1.0, &0.001f) (+0x14,
    // ), Child->SetMode(3) (+0x34), Child->Release().
    FFxRadialFieldPropertySet::FFxRadialFieldPropertySet()
        : MagnitudeRamp(nullptr), ApproachRamp(nullptr), RadiusRamp(nullptr), FalloffRamp(nullptr)
    {
        FCollideRampCleanup Cleanup = {{&MagnitudeRamp, &ApproachRamp, &RadiusRamp, &FalloffRamp}, 4};
        MakeAnimatedSingleRamp(kDefaultRadialMagnitude, &MagnitudeRamp);
        MakeAnimatedSingleRamp(kDefaultRadialApproach, &ApproachRamp);
        MakeAnimatedSingleRamp(kDefaultRadialRadius, &RadiusRamp);
        struct FChildReference
        {
            INeuronComponent* Child;
            ~FChildReference() noexcept(false)
            {
                if (Child != nullptr)
                    Child->Release();
            }
        };
        {
            FRampSingleRamp* const Ramp = new FRampSingleRamp();
            FChildReference ChildReference = {nullptr};
            IRampSingle* Child = nullptr;
            MakeRampSingle(1.0f, &Child);
            ChildReference.Child = Child;
            Ramp->AddKey(0.0f, Child);
            FalloffRamp = Ramp;
        }
        {
            FChildReference ChildReference = {nullptr};
            FalloffRamp->GetKeyValue(0, &ChildReference.Child);
            IRampSingle* const Child = static_cast<IRampSingle*>(ChildReference.Child);
            const float EndValue = 0.001f;
            Child->AddKey(1.0f, &EndValue);
            Child->SetMode(3);
        }
        Cleanup.Remaining = 0;
    }

    // a plain +8 Release of each ramp.
    FFxRadialFieldPropertySet::~FFxRadialFieldPropertySet()
    {
        ReleaseDescriptorSlot(MagnitudeRamp);
        ReleaseDescriptorSlot(ApproachRamp);
        ReleaseDescriptorSlot(RadiusRamp);
        ReleaseDescriptorSlot(FalloffRamp);
    }

    // Turbulence create the routine (0x0623d504..0x0623d59b): +0x4c an
    // FxAnimatedSingleRamp (the routine) with an FxAnimatedSingle key of 1.0; +0x50
    // (10.0).
    FFxTurbulenceFieldPropertySet::FFxTurbulenceFieldPropertySet()
        : ApproachRamp(nullptr), MagnitudeRamp(nullptr)
    {
        FCollideRampCleanup Cleanup = {{&ApproachRamp, &MagnitudeRamp, nullptr}, 2};
        MakeAnimatedSingleRamp(kDefaultTurbulenceApproach, &ApproachRamp);
        MakeAnimatedSingleRamp(kDefaultTurbulenceMagnitude, &MagnitudeRamp);
        Cleanup.Remaining = 0;
    }

    FFxTurbulenceFieldPropertySet::~FFxTurbulenceFieldPropertySet() noexcept(false)
    {
        FCollideRampCleanup Cleanup = {{&ApproachRamp, &MagnitudeRamp, nullptr}, 2};
        Cleanup.ReleaseRemaining();
    }

    // (0x06219a5d..0x06219c57): +0x4c/+0x50/+0x54 zeroed, then each gets an
    // FxAnimatedSingleRamp (the routine, vtable 0624bac8) whose key 0 is an
    // FxAnimatedSingle (the routine, +0x18 set) of 0.7 (0x3f333333), 10.0, 10.0.
    FFxCollideFieldPropertySet::FFxCollideFieldPropertySet()
        : ReflectRamp(nullptr), WidthRamp(nullptr), HeightRamp(nullptr)
    {
        FCollideRampCleanup Cleanup = {{&ReflectRamp, &WidthRamp, &HeightRamp}, 3};
        MakeCollideAnimatedSingleRamp(kDefaultCollideReflect, &ReflectRamp);
        MakeCollideAnimatedSingleRamp(kDefaultCollideWidth, &WidthRamp);
        MakeCollideAnimatedSingleRamp(kDefaultCollideHeight, &HeightRamp);
        Cleanup.Remaining = 0;
    }

    FFxCollideFieldPropertySet::~FFxCollideFieldPropertySet() noexcept(false)
    {
        FCollideRampCleanup Cleanup = {{&ReflectRamp, &WidthRamp, &HeightRamp}, 3};
        Cleanup.ReleaseRemaining();
    }

    // +0x4c an FxAnimatedSingleRamp (the routine, vtable 0624bac8) whose
    // key 0 is an FxAnimatedSingle (operator_new(0x10), vtable 0624bca0) given weight
    // 60.1 (0x42706666, +0x18 SetWeight) and added at time 0 (+0x14 AddKey).
    FFxDustFieldPropertySet::FFxDustFieldPropertySet() : RadiusRamp(nullptr)
    {
        FCollideRampCleanup Cleanup = {{&RadiusRamp, nullptr, nullptr}, 1};
        MakeCollideAnimatedSingleRamp(60.1f, &RadiusRamp);
        Cleanup.Remaining = 0;
    }

    FFxDustFieldPropertySet::~FFxDustFieldPropertySet()
    {
        ReleaseDescriptorSlot(RadiusRamp);
    }

    // ---- FxGravityField -------------------------------------------------

    FxGravityField::FxGravityField(const char* PrimaryIid, const char* FactoryName)
        : FGenericFxObjectBase(PrimaryIid, FactoryName), GravityPropertySet_(nullptr), Gravity_(0.0f)
    {
        Cache_[0] = 0.0f;
        Cache_[1] = 0.0f;
        Cache_[2] = 0.0f;
    }

    FxGravityField::~FxGravityField()
    {
    }

    // (node slot +0x48).
    void FxGravityField::CreatePropertySet()
    {
        FFxGravityFieldPropertySet* const Set = new FFxGravityFieldPropertySet();
        GravityPropertySet_ = Set;
        AdoptPropertySet(Set);
    }

    // (RET 8; this = the prototype's node): operator_new and the gravity
    // constructor inline (the stores of its factory 0x062237a6..), the routine (0x06223586:
    // and the +0x98 set cache), the +0x9c set cache (0x0622358b..0x06223595);
    // *Out = the new outer, 0. The borrowed cache is copied after base initialization.
    GENRESULT NEURONCOMPONENTS_API FxGravityField::CloneNode(INeuronComponent** Out)
    {
        FxGravityField* const Node = new FxGravityField(PrimaryIid_, FactoryName_);
        Node->InitNodeFromArchetype(this);
        Node->GravityPropertySet_ = GravityPropertySet_;
        *Out = Node;
        return GR_OK;
    }

    // Set getter the branch (RET 0xc): 0xE02B8BD4 -> [+0x4c], AddRef, 0; else -8.
    GENRESULT NEURONCOMPONENTS_API FFxGravityFieldPropertySet::GetPropAnimatedSingleRamp(
        unsigned long Hash, INestedSingleRamp** Out)
    {
        if (Hash != 0xE02B8BD4u)
            return GR_ALE_CREATE_FAIL;
        return HandOutFieldRamp(GravityRamp, Out);
    }

    // the base rows, then .data 0x06256240.
    int NEURONCOMPONENTS_API FFxGravityFieldPropertySet::EnumerateProps(FNodePropertyRowCallback Callback,
                                                             void* Context)
    {
        const int Result = FFxNodePropertySet::EnumerateProps(Callback, Context);
        if (Result != 0)
            return Result;
        return WalkPropertyRows(kGravityPropertyRows, Callback, Context);
    }

    // (outer +0x14, RET 8; node = arg + 4): [[node]+0x44](Dt) TickLifetime;
    // node+0xa0 = D+0x50 (inner +0x4c) ->Eval(node+0x8c, node+0x84) * Dt (FMUL
    // [ESP+0xc], FSTP float); XOR EAX,EAX.
    GENRESULT NEURONCOMPONENTS_API FxGravityField::Pad5(float Dt)
    {
        TickLifetime(Dt);
        Gravity_ = static_cast<float>(
            GravityProps()->GravityRamp->Eval(AppearanceOuterTime_, Age_) * Dt);
        return GR_OK;
    }

    void NEURONCOMPONENTS_API FxGravityField::BeginField(INeuronComponent* Appearance, float /*Dt*/)
    {
        // 0x06224219: node slot 7 (the routine) -> the field in the
        // appearance's space; (0, -[node+0xa0], 0) (FCHS, FSTP float) is rotated by it
        // (math engine +0x30) into node+0xa4..+0xac.
        float Xform12[12];
        ComputeRelativeTransform(Appearance, Xform12);
        const FVector Source = { 0.0f, -Gravity_, 0.0f };
        I3DMathEngine* Math = G_GravityField3DMath;
        if (Math == nullptr)
        {
            Math = Create3DMath();
            G_GravityField3DMath = Math;
        }
        FVector Result;
        Math->Rotate(Result, *reinterpret_cast<const FTransform*>(Xform12), Source);
        std::memcpy(Cache_, &Result, sizeof(Result));
    }

    // vel[i] += node+0xa4.. (FLD vel, FADD cache, FSTP).
    void NEURONCOMPONENTS_API FxGravityField::ApplyFieldToVel(float* PosVel6, float* /*Age*/, float /*Dt*/)
    {
        float* Vel = PosVel6 + 3;
        Vel[0] += Cache_[0];
        Vel[1] += Cache_[1];
        Vel[2] += Cache_[2];
    }

    // ---- FxAirField -------------------------------------------------------

    FxAirField::FxAirField(const char* PrimaryIid, const char* FactoryName)
        : FGenericFxObjectBase(PrimaryIid, FactoryName), AirPropertySet_(nullptr),
          Approach_(0.0f), Magnitude_(0.0f),
          Blend_(0.0f)
    {
        Cache_[0] = 0.0f;
        Cache_[1] = 0.0f;
        Cache_[2] = 0.0f;
    }

    FxAirField::~FxAirField()
    {
    }

    // (node slot +0x48).
    void FxAirField::CreatePropertySet()
    {
        FFxAirFieldPropertySet* const Set = new FFxAirFieldPropertySet();
        AirPropertySet_ = Set;
        AdoptPropertySet(Set);
    }

    // (RET 8): the air constructor inline (as its factory 0x06214d76..),
    // (0x06214272), the +0x9c set cache, then -- the only clone that copies
    // node state -- the three dwords +0xac..+0xb4, the Begin-time Cache_
    // (0x0621427d..0x062142a0); *Out, 0.
    GENRESULT NEURONCOMPONENTS_API FxAirField::CloneNode(INeuronComponent** Out)
    {
        FxAirField* const Node = new FxAirField(PrimaryIid_, FactoryName_);
        Node->InitNodeFromArchetype(this);
        Node->AirPropertySet_ = AirPropertySet_;
        std::memcpy(Node->Cache_, Cache_, sizeof(Cache_));
        *Out = Node;
        return GR_OK;
    }

    // Set getter the branch (RET 0xc): 0x10423CEB (AirField_Approach) -> [+0x4c],
    // (AirField_Magnitude) -> [+0x50]; AddRef, 0; else -8.
    GENRESULT NEURONCOMPONENTS_API FFxAirFieldPropertySet::GetPropAnimatedSingleRamp(unsigned long Hash,
                                                                          INestedSingleRamp** Out)
    {
        if (Hash == 0x10423CEBu)
            return HandOutFieldRamp(ApproachRamp, Out);
        if (Hash == 0xE5E3524Cu)
            return HandOutFieldRamp(MagnitudeRamp, Out);
        return GR_ALE_CREATE_FAIL;
    }

    // the base rows, then .data 0x06255af4.
    int NEURONCOMPONENTS_API FFxAirFieldPropertySet::EnumerateProps(FNodePropertyRowCallback Callback,
                                                         void* Context)
    {
        const int Result = FFxNodePropertySet::EnumerateProps(Callback, Context);
        if (Result != 0)
            return Result;
        return WalkPropertyRows(kAirPropertyRows, Callback, Context);
    }

    // (outer +0x14, RET 8; node = arg + 4): TickLifetime(Dt); node+0xa0 =
    // inner +0x4c ->Eval(node+0x8c, node+0x84); node+0xa4 = inner +0x50 ->Eval; node+0xa8
    // = Dt * [+0xa0] * 4.0 (the field) FST, FCOMP 1.0 (the field), AND AX,0x4100,
    // JNZ keeps it (C0|C3: <= 1 or unordered), else 1.0; XOR EAX,EAX.
    GENRESULT NEURONCOMPONENTS_API FxAirField::Pad5(float Dt)
    {
        TickLifetime(Dt);
        FFxAirFieldPropertySet* const Set = AirProps();
        Approach_ = static_cast<float>(Set->ApproachRamp->Eval(AppearanceOuterTime_, Age_));
        Magnitude_ = static_cast<float>(Set->MagnitudeRamp->Eval(AppearanceOuterTime_, Age_));
        Blend_ = Dt * Approach_ * 4.0f;
        if (Blend_ > 1.0f)
            Blend_ = 1.0f;
        return GR_OK;
    }

    void NEURONCOMPONENTS_API FxAirField::BeginField(INeuronComponent* Appearance, float /*Dt*/)
    {
        // 0x06214c69: node slot 7 (the routine), as Gravity; then
        // `MOV [esp+4],0; FSTP [esp+8]; MOV [esp+0xc],0` -- the
        // vector (0, [node+0xa4], 0), Y axis -- rotated (+0x30) into node+0xac..+0xb4.
        float Xform12[12];
        ComputeRelativeTransform(Appearance, Xform12);
        const FVector Source = { 0.0f, Magnitude_, 0.0f };
        I3DMathEngine* Math = G_AirField3DMath;
        if (Math == nullptr)
        {
            Math = Create3DMath();
            G_AirField3DMath = Math;
        }
        FVector Result;
        Math->Rotate(Result, *reinterpret_cast<const FTransform*>(Xform12), Source);
        std::memcpy(Cache_, &Result, sizeof(Result));
    }

    // vel[i] = (cache[i] - vel[i]) * [node+0xa8] + vel[i].
    void NEURONCOMPONENTS_API FxAirField::ApplyFieldToVel(float* PosVel6, float* /*Age*/, float /*Dt*/)
    {
        float* Vel = PosVel6 + 3;
        LerpVel3(Vel, Cache_, Blend_);
    }

    // ---- FxRadialField ------------------------------------------------------

    FxRadialField::FxRadialField(const char* PrimaryIid, const char* FactoryName)
        : FGenericFxObjectBase(PrimaryIid, FactoryName), RadialPropertySet_(nullptr),
          Magnitude_(0.0f), Approach_(0.0f),
          Blend_(0.0f), DistScale_(0.0f)
    {
        Cache_[0] = 0.0f;
        Cache_[1] = 0.0f;
        Cache_[2] = 0.0f;
    }

    FxRadialField::~FxRadialField()
    {
    }

    // (node slot +0x48): operator_new(0x64), the routine on +4.
    void FxRadialField::CreatePropertySet()
    {
        FFxRadialFieldPropertySet* const Set = new FFxRadialFieldPropertySet();
        AdoptPropertySet(Set);
        RadialPropertySet_ = Set;
    }

    // (RET 8): the radial constructor inline (as its factory 0x06237016..),
    // (0x0623641e), the +0x9c set cache (0x06236423..0x0623642d); *Out, 0.
    GENRESULT NEURONCOMPONENTS_API FxRadialField::CloneNode(INeuronComponent** Out)
    {
        FxRadialField* const Node = new FxRadialField(PrimaryIid_, FactoryName_);
        Node->InitNodeFromArchetype(this);
        Node->RadialPropertySet_ = RadialPropertySet_;
        *Out = Node;
        return GR_OK;
    }

    // Set getter +0x3c the branch (kind 0x200): 0x1A433168 -> [+0x58], AddRef, 0; else -8.
    GENRESULT NEURONCOMPONENTS_API FFxRadialFieldPropertySet::GetPropSingleRampRamp(unsigned long Hash,
                                                                         INestedSingleRamp** Out)
    {
        if (Hash != 0x1A433168u)
            return GR_ALE_CREATE_FAIL;
        return HandOutFieldRamp(FalloffRamp, Out);
    }

    // Set getter +0x40 the branch (kind 0x202): 0xFF28F620 -> [+0x54], 0x0064B3B9 ->
    // [+0x4c], 0xF3CF5EA5 -> [+0x50]; AddRef, 0; else -8.
    GENRESULT NEURONCOMPONENTS_API FFxRadialFieldPropertySet::GetPropAnimatedSingleRamp(
        unsigned long Hash, INestedSingleRamp** Out)
    {
        if (Hash == 0xFF28F620u)
            return HandOutFieldRamp(RadiusRamp, Out);
        if (Hash == 0x0064B3B9u)
            return HandOutFieldRamp(MagnitudeRamp, Out);
        if (Hash == 0xF3CF5EA5u)
            return HandOutFieldRamp(ApproachRamp, Out);
        return GR_ALE_CREATE_FAIL;
    }

    // the base rows, then .data 0x062569c8.
    int NEURONCOMPONENTS_API FFxRadialFieldPropertySet::EnumerateProps(FNodePropertyRowCallback Callback,
                                                            void* Context)
    {
        const int Result = FFxNodePropertySet::EnumerateProps(Callback, Context);
        if (Result != 0)
            return Result;
        return WalkPropertyRows(kRadialPropertyRows, Callback, Context);
    }

    // (0x06236450..0x06236527, RET 8): the radial field's own per-frame
    // node update (VT_B slot 3 = outer +0x14, the shared default the routine on the
    // other fields; VT_A reaches it through the ECX+=4 thunk the branch). The
    // descriptor D = [node+0x9c] is the outer, so D+0x58 / +0x54 / +0x50 are the inner
    // +0x54 / +0x50 / +0x4c ramps, each evaluated with +0x1C Eval(node+0x8c, node+0x84):
    // [[node]+0x44](Dt) -- TickLifetime (the branch) first.
    // R = Radius->Eval; FCOM double 0.001 (the field); TEST AH,5;
    //     JP skips the clamp when C0 = C2 (R >= 0.001, or unordered); fall-through
    //     (C0 set: R < 0.001) replaces R with float 0.001 (the field).
    //     1.0f (the field) / R -> node+0xac (FDIV in the FPU, FSTP float).
    // Approach->Eval -> node+0xa0 (float).
    // Magnitude->Eval, FCHS -> node+0xa4 (the port keeps it positive
    //     and negates at the one use in ApplyFieldToVel; negation is exact).
    // Dt * [+0xa0] * 4.0f (the field) -> node+0xa8 (FST), then FCOMP
    //     1.0; AND AX,0x4100; JNZ keeps it (C0 or C3: <= 1.0, or unordered), else
    //     node+0xa8 = 1.0.
    // Dt raw -> node+0xb0 (no reader in Apply or anywhere in the radial
    //     vtables' bodies; not kept).
    //   XOR EAX,EAX -> 0.
    GENRESULT NEURONCOMPONENTS_API FxRadialField::Pad5(float Dt)
    {
        TickLifetime(Dt);
        FFxRadialFieldPropertySet* const Set = RadialProps(); // node+0x9c
        const double Radius = Set->RadiusRamp->Eval(AppearanceOuterTime_, Age_);
        if (Radius < 0.001)
            DistScale_ = static_cast<float>(1.0 / static_cast<double>(0.001f));
        else
            DistScale_ = static_cast<float>(1.0 / Radius);
        Approach_ = static_cast<float>(Set->ApproachRamp->Eval(AppearanceOuterTime_, Age_));
        Magnitude_ = static_cast<float>(Set->MagnitudeRamp->Eval(AppearanceOuterTime_, Age_));
        Blend_ = Dt * Approach_ * 4.0f;
        if (Blend_ > 1.0f)
            Blend_ = 1.0f;
        return GR_OK;
    }

    // (RET 8, one stack argument: the appearance): node slot 7
    // (the routine, 0x06236f19) -> the field-in-appearance transform, then the math
    // engine's +0x28 transforms (0,0,0) into node+0xb4..+0xbc. Returns 0. The port's
    // trailing Dt is unused here, as in baseline.
    void NEURONCOMPONENTS_API FxRadialField::BeginField(INeuronComponent* Appearance, float /*Dt*/)
    {
        float Xform12[12] = {
            1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f,
            0.0f, 0.0f, 0.0f
        };
        ComputeRelativeTransform(Appearance, Xform12);
        const FVector Origin = { 0.0f, 0.0f, 0.0f };
        I3DMathEngine* Math = G_RadialField3DMath;
        if (Math == nullptr)
        {
            Math = Create3DMath();
            G_RadialField3DMath = Math;
        }
        FVector TransformedOrigin;
        Math->TransformXform(TransformedOrigin,
            *reinterpret_cast<const FTransform*>(Xform12), Origin);
        std::memcpy(Cache_, &TransformedOrigin, sizeof(TransformedOrigin));
    }

    void NEURONCOMPONENTS_API FxRadialField::ApplyFieldToVel(float* PosVel6, float* /*Age*/, float /*Dt*/)
    {
        float* Vel = PosVel6 + 3;
        // dx, dy FSTP float; dz is FST float at 0x0623663f but
        // ST0 keeps the unrounded difference, which multiplies the stored float.
        const float Dx = Cache_[0] - PosVel6[0];
        const float Dy = Cache_[1] - PosVel6[1];
        const double DzExt = static_cast<double>(Cache_[2]) - PosVel6[2];
        const float Dz = static_cast<float>(DzExt);
        // dz*dz + dx*dx + dy*dy in that order, FSQRT, times
        // node+0xac (1 / Radius, DistScale_), all in the FPU.
        const double DistExt =
            std::sqrt(DzExt * Dz + static_cast<double>(Dx) * Dx + static_cast<double>(Dy) * Dy) *
            DistScale_;
        // FST float [ESP+0x24]; FCOMP 1.0; AND AX,0x4100; JNZ keeps the stored
        // float when C0|C3 (<= 1 or unordered); fall-through (> 1) stores 1.0.
        float Dist = static_cast<float>(DistExt);
        if (DistExt > 1.0)
            Dist = 1.0f;
        // the falloff ramp's +0x1C Eval(node+0x8c, Dist), unchecked, FMUL
        // node+0xa4 (baseline stores the magnitude negated in the update; the port negates
        // here -- exact).
        const double Scale =
            RadialProps()->FalloffRamp->Eval(AppearanceOuterTime_, Dist) * -Magnitude_;
        // Tx = dx * k stays in ST0; Ty and Tz are FSTP float.
        const double TargetX = static_cast<double>(Dx) * Scale;
        const float TargetY = static_cast<float>(static_cast<double>(Dy) * Scale);
        const float TargetZ = static_cast<float>(static_cast<double>(Dz) * Scale);
        // baseline's node+0xa8: min(Dt * Approach * 4.0f, 1.0f) from the update (Blend_).
        LerpVel3(Vel, TargetX, TargetY, TargetZ, Blend_);
    }

    // ---- FxTurbulenceField --------------------------------------------------

    FxTurbulenceField::FxTurbulenceField(const char* PrimaryIid, const char* FactoryName)
        : FGenericFxObjectBase(PrimaryIid, FactoryName), TurbulencePropertySet_(nullptr),
          Approach_(0.0f), Blend_(0.0f),
          Magnitude_(0.0f)
    {
    }

    FxTurbulenceField::~FxTurbulenceField()
    {
    }

    // (node slot +0x48).
    void FxTurbulenceField::CreatePropertySet()
    {
        FFxTurbulenceFieldPropertySet* const Set = new FFxTurbulenceFieldPropertySet();
        TurbulencePropertySet_ = Set;
        AdoptPropertySet(Set);
    }

    // (RET 8): the turbulence constructor inline (as its factory 0x0623d966..),
    // (0x0623d6da), the +0x9c set cache (0x0623d6df..0x0623d6e9); *Out, 0.
    GENRESULT NEURONCOMPONENTS_API FxTurbulenceField::CloneNode(INeuronComponent** Out)
    {
        FxTurbulenceField* const Node = new FxTurbulenceField(PrimaryIid_, FactoryName_);
        Node->InitNodeFromArchetype(this);
        Node->TurbulencePropertySet_ = TurbulencePropertySet_;
        *Out = Node;
        return GR_OK;
    }

    // Set getter the branch (RET 0xc): 0x1B970B9A (TurbulenceField_Approach) -> [+0x4c],
    // (TurbulenceField_Magnitude) -> [+0x50]; AddRef, 0; else -8.
    GENRESULT NEURONCOMPONENTS_API FFxTurbulenceFieldPropertySet::GetPropAnimatedSingleRamp(
        unsigned long Hash, INestedSingleRamp** Out)
    {
        if (Hash == 0x1B970B9Au)
            return HandOutFieldRamp(ApproachRamp, Out);
        if (Hash == 0x1CEAC6D1u)
            return HandOutFieldRamp(MagnitudeRamp, Out);
        return GR_ALE_CREATE_FAIL;
    }

    // the base rows, then .data 0x06256ccc.
    int NEURONCOMPONENTS_API FFxTurbulenceFieldPropertySet::EnumerateProps(FNodePropertyRowCallback Callback,
                                                                void* Context)
    {
        const int Result = FFxNodePropertySet::EnumerateProps(Callback, Context);
        if (Result != 0)
            return Result;
        return WalkPropertyRows(kTurbulencePropertyRows, Callback, Context);
    }

    // (outer +0x14, RET 8; node = arg + 4): TickLifetime(Dt); node+0xa0 =
    // inner +0x4c ->Eval(node+0x8c, node+0x84); node+0xa8 = inner +0x50 ->Eval; node+0xa4
    // = Dt * [+0xa0] * 4.0 (the field) FST, FCOMP 1.0, AND AX,0x4100, JNZ keeps it
    // (C0|C3: <= 1 or unordered), else 1.0; XOR EAX,EAX.
    GENRESULT NEURONCOMPONENTS_API FxTurbulenceField::Pad5(float Dt)
    {
        TickLifetime(Dt);
        FFxTurbulenceFieldPropertySet* const Set = TurbulenceProps();
        const double Approach = Set->ApproachRamp->Eval(AppearanceOuterTime_, Age_);
        FFxTurbulenceFieldPropertySet* const MagnitudeSet = TurbulenceProps();
        Approach_ = static_cast<float>(Approach);
        Magnitude_ = static_cast<float>(MagnitudeSet->MagnitudeRamp->Eval(AppearanceOuterTime_, Age_));
        const double Blend = static_cast<double>(Dt) * Approach_ * 4.0;
        Blend_ = static_cast<float>(Blend);
        if (Blend > 1.0)
            Blend_ = 1.0f;
        return GR_OK;
    }

    // XOR EAX,EAX; RET 8.
    void NEURONCOMPONENTS_API FxTurbulenceField::BeginField(INeuronComponent* /*Appearance*/, float /*Dt*/)
    {
    }

    // (object frame = node + 4, RET 0xc: this, PosVel, unused):
    //   R1 = (rand - 0.5) doubled -> [ESP+8]; R2 likewise -> [ESP+4]; R3 stays in ST0
    //   (the routine, the field = 0.5, FADD ST0,ST0).
    //   L = R3*R3 + R2*R2 + R1*R1 (FSTP float); y = bits 0x5f3759df - (bits(L) SAR 1);
    //   s = (1.5 - y*y*L*0.5) * y (the field = 1.5).
    //   The direction is (R2*s, R1*s, R3*s) -- the second draw is x, the first is y.
    //   k = rand * [node+0xa8] (Magnitude); Target = direction * k;
    //   vel[i] = (Target[i] - vel[i]) * [node+0xa4] (Blend) + vel[i].
    // There is no zero-length guard: L = 0 gives s = finite and a zero direction.
    void NEURONCOMPONENTS_API FxTurbulenceField::ApplyFieldToVel(float* PosVel6, float* /*Age*/, float /*Dt*/)
    {
        float* Vel = PosVel6 + 3;
        // leaves its product in ST0 (ParticleFxRand01 returns it as double);
        // FSUB 0.5f, FADD ST0,ST0, then R1/R2 are FSTP float and R3 stays in the FPU.
        const float R1 = static_cast<float>((ParticleFxRand01() - 0.5) * 2.0);
        const float R2 = static_cast<float>((ParticleFxRand01() - 0.5) * 2.0);
        const double R3 = (ParticleFxRand01() - 0.5) * 2.0;
        const float LengthSq = static_cast<float>(R3 * R3 + static_cast<double>(R2) * R2 +
                                                  static_cast<double>(R1) * R1);
        long Bits = 0;
        std::memcpy(&Bits, &LengthSq, sizeof(Bits));
        const long GuessBits = 0x5f3759dfL - (Bits >> 1);
        float Guess = 0.0f;
        std::memcpy(&Guess, &GuessBits, sizeof(Guess));
        const double Scale =
            (1.5 - static_cast<double>(Guess) * Guess * LengthSq * 0.5) * Guess;
        float Dir[3];
        Dir[0] = static_cast<float>(R2 * Scale);
        Dir[1] = static_cast<float>(R1 * Scale);
        Dir[2] = static_cast<float>(R3 * Scale);
        // k = rand * [+0xac] (Magnitude) stays in the FPU; the x
        // product (FMUL ST1 at 0x0623e427) is never stored, y and z are FSTP float.
        const double Push = ParticleFxRand01() * Magnitude_;
        const double TargetX = static_cast<double>(Dir[0]) * Push;
        const float TargetY = static_cast<float>(static_cast<double>(Dir[1]) * Push);
        const float TargetZ = static_cast<float>(static_cast<double>(Dir[2]) * Push);
        LerpVel3(Vel, TargetX, TargetY, TargetZ, Blend_);
    }

    // ---- FxCollideField -----------------------------------------------------

    FxCollideField::FxCollideField(const char* PrimaryIid, const char* FactoryName)
        : FGenericFxObjectBase(PrimaryIid, FactoryName), CollidePropertySet_(nullptr),
          Reflect_(0.0f), UpdateDt_(0.0f)
    {
        unsigned long Index = 0;
        for (Index = 0; Index < 3; ++Index)
        {
            Normal_[Index] = 0.0f;
            Corner_[Index] = 0.0f;
            ScaledNormal_[Index] = 0.0f;
        }
        for (Index = 0; Index < 12; ++Index)
            BeginXform_[Index] = 0.0f;
        // calls the 3x3 identity constructor, then clears translation.
        BeginXform_[0] = BeginXform_[4] = BeginXform_[8] = 1.0f;
    }

    FxCollideField::~FxCollideField()
    {
    }

    // (node slot +0x48): operator_new(0x60), the routine on +4.
    void FxCollideField::CreatePropertySet()
    {
        FFxCollideFieldPropertySet* const Set = new FFxCollideFieldPropertySet();
        CollidePropertySet_ = Set;
        AdoptPropertySet(Set);
    }

    // (RET 8): the collide constructor inline (as its factory 0x0621ad16..),
    // (0x06219f10), the +0x9c set cache (0x06219f15..0x06219f20); *Out, 0.
    GENRESULT NEURONCOMPONENTS_API FxCollideField::CloneNode(INeuronComponent** Out)
    {
        FxCollideField* const Node = new FxCollideField(PrimaryIid_, FactoryName_);
        Node->InitNodeFromArchetype(this);
        // reloads the source's non-owning cache after the helper returns.
        Node->CollidePropertySet_ = CollidePropertySet_;
        *Out = Node;
        return GR_OK;
    }

    // (RET 8): the beam field constructor inline (as its factory
    // ..), the routine (0x0620e59c) and nothing else; *Out, 0.
    GENRESULT NEURONCOMPONENTS_API FLBeamField::CloneNode(INeuronComponent** Out)
    {
        FLBeamField* const Node = new (std::nothrow) FLBeamField(PrimaryIid_, FactoryName_);
        Node->InitNodeFromArchetype(this);
        *Out = Node;
        return GR_OK;
    }

    // Set getter the branch (RET 0xc; also reached by the +4 thunk 0x0621a6f0):
    // or 0xE462B789 -> [+0x4c]; 0x138BDB51 or 0x061AAF28 -> [+0x50];
    // or 0x16FF3474 -> [+0x54]; AddRef, 0; else -8.
    GENRESULT NEURONCOMPONENTS_API FFxCollideFieldPropertySet::GetPropAnimatedSingleRamp(
        unsigned long Hash, INestedSingleRamp** Out)
    {
        if (Hash == 0xF2468210u || Hash == 0xE462B789u)
            return HandOutFieldRamp(ReflectRamp, Out);
        if (Hash == 0x138BDB51u || Hash == 0x061AAF28u)
            return HandOutFieldRamp(WidthRamp, Out);
        if (Hash == 0x11336C98u || Hash == 0x16FF3474u)
            return HandOutFieldRamp(HeightRamp, Out);
        return GR_ALE_CREATE_FAIL;
    }

    // the base rows, then .data 0x06255d78.
    int NEURONCOMPONENTS_API FFxCollideFieldPropertySet::EnumerateProps(FNodePropertyRowCallback Callback,
                                                             void* Context)
    {
        const int Result = FFxNodePropertySet::EnumerateProps(Callback, Context);
        if (Result != 0)
            return Result;
        return WalkPropertyRows(kCollidePropertyRows, Callback, Context);
    }

    // (outer +0x14, RET 8; node = arg + 4): [[node]+0x44](Dt) TickLifetime;
    // node+0xa0 = D+0x50 (inner +0x4c) ->Eval(node+0x8c, node+0x84) (FSTP float);
    // node+0xa4 = Dt (raw MOV); XOR EAX,EAX.
    GENRESULT NEURONCOMPONENTS_API FxCollideField::Pad5(float Dt)
    {
        TickLifetime(Dt);
        Reflect_ = static_cast<float>(CollideProps()->ReflectRamp->Eval(AppearanceOuterTime_, Age_));
        UpdateDt_ = Dt;
        return GR_OK;
    }

    void NEURONCOMPONENTS_API FxCollideField::BeginField(INeuronComponent* Appearance, float /*Dt*/)
    {
        // (RET 8; EDI = arg + 4 = the node): 0x0621ab3d node slot 7
        // (the routine) with the appearance into node+0xcc -- the field in the
        // appearance's space.
        ComputeRelativeTransform(Appearance, BeginXform_);
        // math +0x30 Rotate(out, +0xcc, (0, 1, 0)) -> node+0xa8.
        const FVector NormalSource = { 0.0f, 1.0f, 0.0f };
        I3DMathEngine* Math = G_CollideField3DMath;
        if (Math == nullptr)
        {
            Math = Create3DMath();
            G_CollideField3DMath = Math;
        }
        FVector RotatedNormal;
        Math->Rotate(RotatedNormal, *reinterpret_cast<const FTransform*>(BeginXform_),
                     NormalSource);
        std::memcpy(Normal_, &RotatedNormal, sizeof(RotatedNormal));
        // len = sqrt((x*x + y*y) + z*z) (FSQRT, kept in the FPU),
        // FDIVR 1.0f -> inv; each component = (float)(inv * c). No zero-length guard.
        const double LengthSq = static_cast<double>(Normal_[0]) * Normal_[0] +
                                static_cast<double>(Normal_[1]) * Normal_[1] +
                                static_cast<double>(Normal_[2]) * Normal_[2];
        const double InvLength = 1.0 / ComputeParticleFxFpuSquareRoot(LengthSq);
        StoreParticleFxFpuFloat(Normal_[0], InvLength * Normal_[0]);
        StoreParticleFxFpuFloat(Normal_[1], InvLength * Normal_[1]);
        StoreParticleFxFpuFloat(Normal_[2], InvLength * Normal_[2]);
        // math +0x28 TransformXform(out, +0xcc, (1, 0, 1)) -> node+0xb4.
        const FVector CornerSource = { 1.0f, 0.0f, 1.0f };
        Math = G_CollideField3DMath;
        if (Math == nullptr)
        {
            Math = Create3DMath();
            G_CollideField3DMath = Math;
        }
        FVector TransformedCorner;
        Math->TransformXform(TransformedCorner, *reinterpret_cast<const FTransform*>(BeginXform_),
                             CornerSource);
        std::memcpy(Corner_, &TransformedCorner, sizeof(TransformedCorner));
        // node+0xc0 = normal * 5.0f (the field), FSTP float each.
        ScaledNormal_[0] = Normal_[0] * kCollideNormalScale;
        ScaledNormal_[1] = Normal_[1] * kCollideNormalScale;
        ScaledNormal_[2] = Normal_[2] * kCollideNormalScale;
    }

    void NEURONCOMPONENTS_API FxCollideField::ApplyFieldToVel(float* PosVel6, float* /*Age*/, float /*Dt*/)
    {
        // (RET 0xc: node, PosVel, unused; ALC-5 2026-09-30 term-by-term
        // trace). Every 3-vector temporary goes through the routine / the routine /
        // (FSTP float per component); dot products and scalars stay in the
        // FPU (double here). N = node+0xa8, C = node+0xb4, SN = node+0xc0, S = node+0xa4
        // (the update's Dt), R = node+0xa0 (Reflectivity).
        float* Pos = PosVel6;
        float* Vel = PosVel6 + 3;
        const float* N = Normal_;

        // / 0x0621a112: T0 = Pos + SN; T1 = T0 - C.
        const float T0[3] = { Pos[0] + ScaledNormal_[0], Pos[1] + ScaledNormal_[1],
                              Pos[2] + ScaledNormal_[2] };
        const float T1[3] = { T0[0] - Corner_[0], T0[1] - Corner_[1], T0[2] - Corner_[2] };
        // D0 = (T1z*Nz + T1y*Ny) + T1x*Nx; FST float, FCOMP 0.0
        // (the field); TEST AH,5; JNP exits when only C0 is set (D0 < 0); equal and
        // unordered go on.
        const double D0Ext = (static_cast<double>(T1[2]) * N[2] + static_cast<double>(T1[1]) * N[1]) +
                             static_cast<double>(T1[0]) * N[0];
        if (D0Ext < 0.0)
            return;
        const float D0 = static_cast<float>(D0Ext);

        // V1 = S * Vel (FSTP float each).
        const float S = UpdateDt_;
        const float V1[3] = { S * Vel[0], S * Vel[1], S * Vel[2] };
        // dot(V1, N) in the same z, y, x order; exits when < 0.
        const double VnExt = (static_cast<double>(V1[2]) * N[2] + static_cast<double>(V1[1]) * N[1]) +
                             static_cast<double>(V1[0]) * N[0];
        if (VnExt < 0.0)
            return;

        // V2 = V1 - SN; V3 = V2 + T0; V4 = V3 - C.
        const float V2[3] = { V1[0] - ScaledNormal_[0], V1[1] - ScaledNormal_[1],
                              V1[2] - ScaledNormal_[2] };
        const float V3[3] = { V2[0] + T0[0], V2[1] + T0[1], V2[2] + T0[2] };
        const float V4[3] = { V3[0] - Corner_[0], V3[1] - Corner_[1], V3[2] - Corner_[2] };
        // D1 = dot(V4, N); FCOM 0.0; AND EAX,0x4100; JNZ goes on
        // when C0|C3 (D1 <= 0 or unordered); the fall-through (D1 > 0) pops and returns.
        const double D1 = (static_cast<double>(V4[2]) * N[2] + static_cast<double>(V4[1]) * N[1]) +
                          static_cast<double>(V4[0]) * N[0];
        if (D1 > 0.0)
            return;

        // T = D0 / (D0 - D1) (FSUB ST0,ST1 with the stored float
        // D0, FDIVR), FSTP float.
        const float T = static_cast<float>(static_cast<double>(D0) /
                                           (static_cast<double>(D0) - D1));
        // V5 = V2 * T; V6 = V5 + T0; Pos = V6.
        const float V5[3] = { V2[0] * T, V2[1] * T, V2[2] * T };
        Pos[0] = V5[0] + T0[0];
        Pos[1] = V5[1] + T0[1];
        Pos[2] = V5[2] + T0[2];

        // Speed = sqrt((x*x + y*y) + z*z) FSTP float; FCOMP double
        // 0.001 (the field); AND EAX,0x4100; JNZ returns when C0|C3 (Speed <= 0.001 or
        // unordered) -- position already written.
        const float Speed = static_cast<float>(
            std::sqrt(static_cast<double>(Vel[0]) * Vel[0] + static_cast<double>(Vel[1]) * Vel[1] +
                      static_cast<double>(Vel[2]) * Vel[2]));
        if (!(static_cast<double>(Speed) > kCollideSpeedEpsilon))
            return;

        // Dir = (1.0f / Speed) * Vel, FSTP float each.
        const double InvSpeed = 1.0 / static_cast<double>(Speed);
        const float Dir[3] = { static_cast<float>(InvSpeed * Vel[0]),
                               static_cast<float>(InvSpeed * Vel[1]),
                               static_cast<float>(InvSpeed * Vel[2]) };
        // K = (float)(2 * dot(Dir, N)) (z, y, x order, FADD
        // ST0,ST0); the routine scales a copy of N by K; the routine subtracts Dir.
        const double DotDirN =
            (static_cast<double>(Dir[2]) * N[2] + static_cast<double>(Dir[1]) * N[1]) +
            static_cast<double>(Dir[0]) * N[0];
        const float K = static_cast<float>(DotDirN + DotDirN);
        float Reflected[3] = { K * N[0], K * N[1], K * N[2] };
        Reflected[0] = Reflected[0] - Dir[0];
        Reflected[1] = Reflected[1] - Dir[1];
        Reflected[2] = Reflected[2] - Dir[2];
        // Vel = Reflected, then Vel[i] = -(Speed * R) * Vel[i].
        const double VelScale = -(static_cast<double>(Speed) * Reflect_);
        Vel[0] = static_cast<float>(VelScale * Reflected[0]);
        Vel[1] = static_cast<float>(VelScale * Reflected[1]);
        Vel[2] = static_cast<float>(VelScale * Reflected[2]);

        // W = ((1.0f - T) * S) * Vel (FSTP float each);
        // Pos = Pos - W.
        const double Residual = (1.0 - static_cast<double>(T)) * S;
        const float W[3] = { static_cast<float>(Residual * Vel[0]),
                             static_cast<float>(Residual * Vel[1]),
                             static_cast<float>(Residual * Vel[2]) };
        Pos[0] = Pos[0] - W[0];
        Pos[1] = Pos[1] - W[1];
        Pos[2] = Pos[2] - W[2];
    }

    // ---- FLBeamField --------------------------------------------------------
    // No overrides: baseline's real Begin/Apply (the branch/the branch) are
    // genuinely no-ops, and the inherited base no-op is already byte-correct
    // - see particlefx_fields.h's FLBeamField doc comment.

    // ---- FLDustField --------------------------------------------------------

    FLDustField::FLDustField(const char* PrimaryIid, const char* FactoryName)
        : FGenericFxObjectBase(PrimaryIid, FactoryName), DustPropertySet_(nullptr),
          Radius_(0.0f), RadiusSq_(0.0f), UpdateDt_(0.0f)
    {
        unsigned long Index = 0;
        for (Index = 0; Index < 12; ++Index)
            BeginXform_[Index] = 0.0f;
        BeginXform_[0] = BeginXform_[4] = BeginXform_[8] = 1.0f;
    }

    FLDustField::~FLDustField()
    {
    }

    // (node slot +0x48): operator_new(0x58), the routine on +4.
    void FLDustField::CreatePropertySet()
    {
        FFxDustFieldPropertySet* const Set = new FFxDustFieldPropertySet();
        DustPropertySet_ = Set;
        AdoptPropertySet(Set);
    }

    // (RET 8): the dust field constructor inline (as its factory
    // ..), the routine (0x06212bf1), the +0x9c set cache (0x06212bf6..
    // ); *Out, 0.
    GENRESULT NEURONCOMPONENTS_API FLDustField::CloneNode(INeuronComponent** Out)
    {
        FLDustField* const Node = new FLDustField(PrimaryIid_, FactoryName_);
        Node->InitNodeFromArchetype(this);
        Node->DustPropertySet_ = DustPropertySet_;
        *Out = Node;
        return GR_OK;
    }

    // (RET 0xc): hash 0x1874DE74 -> *out = [+0x4c], AddRef, 0; else -8.
    GENRESULT NEURONCOMPONENTS_API FFxDustFieldPropertySet::GetPropAnimatedSingleRamp(unsigned long Hash,
                                                                           INestedSingleRamp** Out)
    {
        if (Hash != 0x1874DE74u)
            return GR_ALE_CREATE_FAIL;
        return HandOutFieldRamp(RadiusRamp, Out);
    }

    // the base rows, then .data 0x06255aa8.
    int NEURONCOMPONENTS_API FFxDustFieldPropertySet::EnumerateProps(FNodePropertyRowCallback Callback,
                                                          void* Context)
    {
        const int Result = FFxNodePropertySet::EnumerateProps(Callback, Context);
        if (Result != 0)
            return Result;
        return WalkPropertyRows(kDustFieldPropertyRows, Callback, Context);
    }

    // (RET 8: node, Dt): FLD Dt FSTP [+0xd8]; [[node]+0x44](Dt) TickLifetime;
    // R = D+0x50 (inner +0x4c) ->Eval([+0x8c], [+0x84]); FST float [+0xa0]; R*R (the
    // unrounded R) FSTP float [+0xa4]; XOR EAX,EAX.
    GENRESULT NEURONCOMPONENTS_API FLDustField::Pad5(float Dt)
    {
        UpdateDt_ = Dt;
        TickLifetime(Dt);
        const double Radius = DustProps()->RadiusRamp->Eval(AppearanceOuterTime_, Age_);
        Radius_ = static_cast<float>(Radius);
        RadiusSq_ = static_cast<float>(Radius * Radius);
        return GR_OK;
    }

    // (RET 8: node, appearance): [[node]+0x1c](node, appearance, node+0xa8)
    // -- slot 7 (the routine) into node+0xa8; XOR EAX,EAX. Apply does not read it.
    void NEURONCOMPONENTS_API FLDustField::BeginField(INeuronComponent* Appearance, float /*Dt*/)
    {
        ComputeRelativeTransform(Appearance, BeginXform_);
    }

    // (RET 0xc: node, particle position, particle age slot; the outer table
    // reaches it through the branch, arg1 += 4):
    // L = math +0x3c InverseTransform(node+0x4c, pos).
    //   D2 = (Ly*Ly + Lx*Lx) + Lz*Lz, FST float; FCOMP [+0xa4] (R^2); AND 0x4100; JNZ goes
    //     on when C0|C3 (<= R^2 or unordered); the fall-through stores 0x7f7fffff to the
    //     age and returns -1.
    //   A = 1.0 (double the field) - (R - Lz) / (R + R), in the FPU.
    //   F = 1.0f; FCOMP Lz with 0.0, TEST AH,5, JP skips when C0 is clear (Lz >= 0 or
    //     unordered); for Lz < 0: Band = (float)(R^2 * 0.8f) (the field); D2 (the
    //     stored float) FCOMP Band, AND 0x4100, JNZ skips when <= / unordered; else
    //     F = 1.0f - (D2 - Band) / (R^2 - Band).
    //   age = (float)(A * F * 10.0f) (the field); if byte the field != 0 the age is
    //     rewritten as (float)(that - [+0xd8]). the field has no writer anywhere in the
    //     image (one reference, the read at 0x06212d80; .data value 0), so that branch is
    //     never taken; G_DustAgeMinusUpdateDt mirrors it. Returns 0.
    void NEURONCOMPONENTS_API FLDustField::ApplyFieldToVel(float* PosVel6, float* Age, float /*Dt*/)
    {
        I3DMathEngine* Math = G_DustField3DMath;
        if (Math == nullptr)
        {
            Math = Create3DMath();
            G_DustField3DMath = Math;
        }
        float Local[3] = { 0.0f, 0.0f, 0.0f };
        Math->InverseTransform(*reinterpret_cast<FVector*>(Local),
                                          *reinterpret_cast<const FTransform*>(NodeWorldXform_),
                                          *reinterpret_cast<const FVector*>(PosVel6)); // +0x3C
        const double DistSqExt = (static_cast<double>(Local[1]) * Local[1] +
                                  static_cast<double>(Local[0]) * Local[0]) +
                                 static_cast<double>(Local[2]) * Local[2];
        float DistSq;
        StoreParticleFxFpuFloat(DistSq, DistSqExt);
        if (DistSqExt > RadiusSq_)
        {
            const unsigned long KillBits = kDustKillAgeBits;
            std::memcpy(Age, &KillBits, 4);
            return;
        }
        const double Height =
            1.0 - (static_cast<double>(Radius_) - Local[2]) /
                      (static_cast<double>(Radius_) + static_cast<double>(Radius_));
        double Fade = 1.0;
        if (Local[2] < 0.0f)
        {
            const float Band = RadiusSq_ * 0.8f;
            if (DistSq > Band)
                Fade = 1.0 - (static_cast<double>(DistSq) - Band) /
                                 (static_cast<double>(RadiusSq_) - Band);
        }
        const double NewAge = Height * Fade * 10.0f;
        *Age = static_cast<float>(NewAge);
        if (G_DustAgeMinusUpdateDt != 0)
            *Age = static_cast<float>(NewAge - UpdateDt_);
    }

} // namespace ParticleFxImpl
