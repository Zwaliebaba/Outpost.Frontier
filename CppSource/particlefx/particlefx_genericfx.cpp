// Author: Giperion.

#include "particlefx_genericfx.h"

#include "particlefx_common.h"
#include "particlefx_ramp.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>

// particlefx.dll — NEURONCOMPONENTS particle FX scaffolding.
// Out-of-line method bodies for FGenericFxObjectBase (shared COM/tree machinery,
// the property sets and the base set's protocol) and the trivial FxNode leaf. Field/
// emitter/appearance-specific logic that used to live here as FactoryName_
// string branches now lives in each leaf's own class in particlefx_fields.cpp
// / particlefx_emitters.cpp / particlefx_appearances.cpp — see particlefx_genericfx.h
// for the split rationale.
namespace ParticleFxImpl
{
    namespace
    {
        // The base set's hashes (.data 0x062572b0 table below).
        const unsigned long kNodeClassNameHash = 0xF54EF296u;
        const unsigned long kNodeNameHash = 0xEFA8CE01u;
        const unsigned long kNodeLifeSpanHash = 0xF27FDE7Du;
        const unsigned long kNodeTransformHash = 0xE13A59A1u;
        const unsigned long kAppearanceLodCurveHash = 0x0C53B008u;

        // The base set's property table, .data 0x062572b0 (walked by the routine).
        const FNodePropertyRow kNodePropertyRows[] = {
            {"Node_ClassName", kNodeClassNameHash, 0x103, 0.0f, 0.0f},
            {"Node_Name", kNodeNameHash, 0x103, 0.0f, 0.0f},
            {"Node_LifeSpan", kNodeLifeSpanHash, 3, 0.0f, kNodePropertyRowMax},
            {"Node_Transform", kNodeTransformHash, 0x105, 0.0f, 0.0f},
            {nullptr, 0, 0, 0.0f, 0.0f},
        };

        // / the field / the field (ImpFxNode.cpp).
        const char kImpFxNodeCppPath[] =
            "ALIL\\ImpFxNode.cpp";

        I3DMathEngine* G_NodeTransform3DMath = nullptr; // .

        I3DMathEngine* NodeTransformMath()
        {
            if (G_NodeTransform3DMath == nullptr)
                G_NodeTransform3DMath = Create3DMath();
            return G_NodeTransform3DMath;
        }

#pragma optimize("", off)
#pragma float_control(except, on, push)
        // divide before the raw copies, store its float result last.
        __declspec(noinline) void InitializeListParticle(FPooledParticle& Particle,
            FGenericFxObjectBase* Owner, const float* Six, const float& Age, const float& Duration)
        {
            const double Reciprocal = 1.0 / static_cast<double>(Duration);
            std::memcpy(&Particle.PosX, Six, sizeof(float) * 6);
            std::memcpy(&Particle.Age, &Age, sizeof(Age));
            Particle.Appearance = Owner;
            std::memcpy(&Particle.Duration, &Duration, sizeof(Duration));
            Particle.InvDuration = static_cast<float>(Reciprocal);
        }
#pragma float_control(pop)
#pragma optimize("", on)

        // 's by-value COM argument, including its EH unwind release.
        struct FScopedFieldReference
        {
            explicit FScopedFieldReference(INeuronComponent* FieldIn) : Field(FieldIn)
            {
                if (Field != nullptr)
                    Field->AddRef();
            }
            ~FScopedFieldReference()
            {
                if (Field != nullptr)
                    Field->Release();
            }

            FScopedFieldReference(const FScopedFieldReference&) = delete;
            FScopedFieldReference& operator=(const FScopedFieldReference&) = delete;
            INeuronComponent* Field;
        };

        template <typename TOwner>
        struct FMemberCleanup
        {
            TOwner* Owner;
            void (TOwner::*Action)();
            bool bActive;

            FMemberCleanup(TOwner* InOwner, void (TOwner::*InAction)())
                : Owner(InOwner), Action(InAction), bActive(true) {}
            ~FMemberCleanup() noexcept(false)
            {
                if (bActive)
                    (Owner->*Action)();
            }
            void Dismiss() { bActive = false; }
            FMemberCleanup(const FMemberCleanup&) = delete;
            FMemberCleanup& operator=(const FMemberCleanup&) = delete;
        };

        template <typename TInterface>
        struct FPointerCleanup
        {
            TInterface*& Slot;
            bool bActive;

            explicit FPointerCleanup(TInterface*& InSlot) : Slot(InSlot), bActive(true) {}
            ~FPointerCleanup() noexcept(false)
            {
                if (bActive)
                    ReleaseDescriptorSlot(Slot);
            }
            void Dismiss() { bActive = false; }
            FPointerCleanup(const FPointerCleanup&) = delete;
            FPointerCleanup& operator=(const FPointerCleanup&) = delete;
        };
    } // namespace

    // ---- FFxHashedName ----

    FFxHashedName::FFxHashedName() : Hash(HashNameCrc(nullptr)), Buffer(nullptr), Length(0), Capacity(0)
    {
    }

    FFxHashedName::~FFxHashedName()
    {
        ReleaseBuffer();
    }

    void FFxHashedName::ReleaseBuffer()
    {
        // baseline name teardown leaves its pointer and scalar fields untouched before
        // Transform::Release. Only disarm the later automatic member destructor.
        if (!bBufferReleased_)
        {
            delete[] Buffer;
            bBufferReleased_ = true;
        }
    }

    // 0x0623f9ef..0x0623faa6 (= the routine). The port's buffer is always
    // owned once allocated, so the owned flag (+0x34 / +0x0c) is implied by Buffer.
    void FFxHashedName::Assign(const char* Text)
    {
        if (Text == nullptr)
        {
            // free when owned, then buffer / capacity / length = 0.
            delete[] Buffer;
            Buffer = nullptr;
            Capacity = 0;
            Length = 0;
        }
        else if (Text != Buffer)
        {
            // length stored first; reuse only when length < capacity.
            Length = static_cast<unsigned long>(std::strlen(Text));
            if (!(static_cast<long>(Length) < static_cast<long>(Capacity)))
            {
                delete[] Buffer;
                Buffer = new char[Length + 1];
                Capacity = Length;
            }
            std::memcpy(Buffer, Text, Length + 1);
        }
        // every path.
        Hash = HashNameCrc(Buffer);
    }

    // ---- Property sets ----

    // (0x0623f7b0..0x0623f8c5): the two hashed strings empty, +0x48 = 3.0f
    // (0x0623f80f), a new FxAnimatedTransform (operator_new(0x34) + the routine + vtable
    // + count 1) stored with AddRef and its creation reference released
    // (0x0623f84c..0x0623f877), then Reset (+0xc, 0x0623f880); last, the name cleared and
    // its CRC taken (0x0623f883..0x0623f8b3).
    FFxNodePropertySet::FFxNodePropertySet() : Transform(nullptr), LifeSpan(3.0f), RefCount_(1)
    {
        FMemberCleanup<FFxNodePropertySet> ConstructionScope(this,
            &FFxNodePropertySet::CleanupFailedConstruction);
        FFxAnimatedTransform* const Created = new (std::nothrow) FFxAnimatedTransform();
        Transform = Created;
        if (Transform != nullptr)
            Transform->AddRef();
        Created->Release();
        Transform->Reset();
        Name.Assign(nullptr);
        ConstructionScope.Dismiss();
    }

    void FFxNodePropertySet::CleanupFailedConstruction()
    {
        Name.ReleaseBuffer();
        ReleaseDescriptorSlot(Transform);
    }

    // and the inlined base teardown in Gravity 062242d0: Node_Name
    // buffer, Transform, ClassName buffer. Release Name storage here so automatic
    // member destruction cannot defer it past Transform; ClassName still unwinds last.
    FFxNodePropertySet::~FFxNodePropertySet() noexcept(false)
    {
        Name.ReleaseBuffer();
        ReleaseDescriptorSlot(Transform);
    }

    // Outer +0x08 (sphere 0623cde0; the same body per class): the count drops only while
    // non-zero; at zero it is set to 1 around the destructor, then the object is freed.
    unsigned long NEURONCOMPONENTS_API FFxNodePropertySet::Release()
    {
        if (RefCount_ != 0)
            --RefCount_;
        if (RefCount_ != 0)
            return RefCount_;
        RefCount_ = 1;
        // baseline frees storage only after the virtual destructor returns successfully.
        this->~FFxNodePropertySet();
        ::operator delete(this);
        return 0;
    }

    // Basic 06218670 (0x06218670..0x06218718, table 0x0624ca9c); FxNode's 06240c50 is the
    // same code over 0x0624f8c0. Both tables have offset 0 in every row, so the hit is
    // `LEA ESI,[EAX+EDX]` with EAX = 0 (0x06218708): this, AddRef, 0.
    GENRESULT NEURONCOMPONENTS_API FFxNodePropertySet::QueryInterface(const char* Iid, void** Out)
    {
        if (IidEqual(Iid, PrimaryIid()) || IidEqual(Iid, Iids::NeuronComponent))
        {
            AddRef();
            *Out = static_cast<IFxNodePropertySet*>(this);
            return GR_OK;
        }
        *Out = nullptr;
        return GR_INTERFACE_UNSUPPORTED;
    }

    const char* FFxNodePropertySet::PrimaryIid() const
    {
        return Iids::FxNodePropertySet;
    }

    // Table 0x0624f8c0: {"1.11_IFxNode", 0}, {"1.11_INeuronComponent", 0}.
    const char* FFxNodeOwnPropertySet::PrimaryIid() const
    {
        return Iids::FxNode;
    }

    // (0x0623f8d0..0x0623f916): [+0x24]->Reset (+0xc); an owned Node_Name
    // buffer freed, then buffer / owned / capacity / length = 0; hash = the routine(0);
    // `XOR EAX,EAX; RET 8`.
    GENRESULT FFxNodePropertySet::Initialize(const AGGDESC* /*Desc*/)
    {
        Transform->Reset();
        Name.Assign(nullptr);
        return GR_OK;
    }

    // (0x06243ff0..0x06244113): after the routine, +0x4c the list's
    // allocator byte (uninitialised), +0x50 a new self-linked 0xc head, +0x54 = 0, then
    // +0x58 an FxRampSingleRamp holding one FxRampSingle key 1.0 at time 0.
    FFxAppearancePropertySet::FFxAppearancePropertySet()
        : EffectScaleRamp(nullptr)
    {
        FPointerCleanup<INestedSingleRamp> PublishedScope(EffectScaleRamp);
        FRampSingleRamp* const Ramp = new FRampSingleRamp();
        IRampSingle* TemporaryChild = nullptr;
        FPointerCleanup<IRampSingle> ChildScope(TemporaryChild);
        const float Value = 1.0f;
        FRampSingle* const Child = new FRampSingle();
        Child->SetMode(4);
        Child->AddKey(0.0f, &Value);
        TemporaryChild = Child;
        Ramp->AddKey(0.0f, Child);
        EffectScaleRamp = Ramp;
        ChildScope.Dismiss();
        Child->Release();
        PublishedScope.Dismiss();
    }

    FFxAppearancePropertySet::~FFxAppearancePropertySet()
    {
        ReleaseDescriptorSlot(EffectScaleRamp);
    }

    // The billboard builders zero +0x60 (Flags) and +0x64 (TextureCrc, `MOV [ESI+0x60],EDI`
    // on the set) and seed the blend pair {5, 2} (basic 0x062155c3 / 0x062155ca); each
    // class's builder in particlefx_appearances.cpp then builds its ramps.
    FAppearanceRampDescriptor::FAppearanceRampDescriptor()
        : Flags(0), TextureCrc(0), SizeRamp(nullptr), ColorRamp(nullptr),
          PrimarySizeRamp(nullptr), TexFrameRamp(nullptr), AxisOverrideRamp(nullptr),
          FrameRamp(nullptr), CommonTexFrameRamp(nullptr), LocalTransform(nullptr)
    {
        BlendInfo[0] = 5;
        BlendInfo[1] = 2;
    }

    // The ramps and the transform are smart-pointer members in baseline; their teardown
    // releases each one (the routine / the routine / the routine / the routine).
    FAppearanceRampDescriptor::~FAppearanceRampDescriptor()
    {
        ReleaseDescriptorSlot(SizeRamp);
        ReleaseDescriptorSlot(ColorRamp);
        ReleaseDescriptorSlot(PrimarySizeRamp);
        ReleaseDescriptorSlot(TexFrameRamp);
        ReleaseDescriptorSlot(AxisOverrideRamp);
        ReleaseDescriptorSlot(FrameRamp);
        ReleaseDescriptorSlot(CommonTexFrameRamp);
        ReleaseDescriptorSlot(LocalTransform);
    }

    // 0x0622f59f..0x0622f61b: both names assigned "" (the routine with
    // ) and hashed, +0x9c = 0.
    FFxParticleAppearancePropertySet::FFxParticleAppearancePropertySet() : Flags(0)
    {
        LifeName.Assign("");
        DeathName.Assign("");
    }

    // (0x06224731..0x062247f2): MeshId 0, MeshName "" hashed, the byte +0x80
    // = 0, +0x84 a new FxAnimatedTransform (the routine, stored with AddRef, the creation
    // reference released; no Reset, unlike the base set's).
    FFxMeshAppearancePropertySet::FFxMeshAppearancePropertySet()
        : MeshId(0), bUseParticleTransform(0), ParticleTransform(nullptr)
    {
        MeshName.Assign("");
        FFxAnimatedTransform* const Created = new FFxAnimatedTransform();
        ParticleTransform = Created;
        ParticleTransform->AddRef();
        Created->Release();
    }

    FFxMeshAppearancePropertySet::~FFxMeshAppearancePropertySet()
    {
        ReleaseDescriptorSlot(ParticleTransform);
    }

    // ---- FGenericFxObjectBase ----

    FGenericFxObjectBase::FGenericFxObjectBase(const char* PrimaryIid, const char* FactoryName)
        : RefCount_(1),
          // 0x0623fb89: node flags start at 0.
          NodeFlags_(0), PrimaryIid_(PrimaryIid), FactoryName_(FactoryName),
          PropertySet_(nullptr),
          AssociatedAppearance_(nullptr),
          AssociatedFields_(nullptr), FieldCount_(0), FieldCapacity_(0),
          AttachParent_(nullptr), FirstChild_(nullptr), PrevSibling_(nullptr),
          NextSibling_(nullptr), LiveInstanceCount_(0),
          // 0x0623fc02..0x0623fc14: +0x90 = 1.0f, bytes +0x94..+0x96 = 0.
          LodInput_(1.0f), bIsAppearanceNode_(0), bIsEmitterNode_(0), bIsFieldNode_(0),
          // Appearance init the routine 0x06244393: node+0xb0 = 0.
          EmitterRefCount_(0),
          InstanceListHead_(nullptr), InstanceListTail_(nullptr),
          // 0x0623fbec..0x0623fbfc: +0x84 = 0, +0x88 = 0x42c80000 (100.0f),
          // +0x8c = 0.
          Age_(0.0f), Duration_(100.0f), AppearanceOuterTime_(0.0f),
          // wave117 (sonnet:wave117:globals-final): see
          // DustPrevCameraDistance_'s own doc comment (particlefx_genericfx.h)
          // -- baseline's FLDustAppearance-only [this+0x12C] previous-frame
          // distance, zero-initialized same as every other dword in this
          // neighbourhood (no explicit ctor write found for it either).
          DustPrevCameraDistance_(0.0f), Descriptor_(nullptr)
    {
        // 0x0623fb9b..0x0623fbe0: +0x1c (raw) and +0x4c (composed) are both
        // identity -- 1.0f on the three diagonal dwords, every other dword 0.
        unsigned long Index = 0;
        for (Index = 0; Index < 12; ++Index)
        {
            const float Value = (Index == 0 || Index == 4 || Index == 8) ? 1.0f : 0.0f;
            NodeLocalXform_[Index] = Value;
            NodeWorldXform_[Index] = Value;
        }
        // The appearance family tag is set by the shared appearance init the routine
        // (0x0624439f, byte +0x94 = 1) and the field tag by each of the seven field
        // constructors (0x0620e566, 0x06212b5a, 0x06214211, 0x06219e6e, 0x06223531,
        // , 0x0623d691: byte +0x96 = 1). The port has no appearance or field
        // base class, so the tag follows the component's primary interface here; the
        // emitter tag (+0x95) is set by FFxEmitterBase.
        if (IidEqual(PrimaryIid_, Iids::FxAppearance))
            bIsAppearanceNode_ = 1;
        if (IidEqual(PrimaryIid_, Iids::FxField))
            bIsFieldNode_ = 1;
    }

    namespace
    {
        // the name every texture key carries.
        const char kTexInfoKeyName[] = "???";
        // / the field.
        const char kTexInfoCppPath[] =
            "ALIL\\ImpFxTexInfo.cpp";
        const char kTexInfoResolveFailFmt[] = "couldnt get effect texture %s";

        ITextureLibrary2* TexInfoLibrary()
        {
            return static_cast<ITextureLibrary2*>(G_TextureLibrary);
        }

        // (cdecl): _vsnprintf(Buffer, 0x2000, Format, args); returns Buffer.
        // baseline's MSVCRT _vsnprintf leaves the buffer unterminated on truncation; the
        // one caller formats "???" into a 30-character message, far below 0x2000.
        char* FormatTexInfoMessage(char* Buffer, const char* Format, ...)
        {
            va_list Args;
            va_start(Args, Format);
            _vsnprintf(Buffer, 0x2000, Format, Args);
            va_end(Args);
            return Buffer;
        }
    } // namespace

    // (ECX = wrapper): +0 = 0, +4 = -1, +8..+0x18 = 0, +0x1c = 0, +0x20 = 0.
    FFxTextureWrapper::FFxTextureWrapper()
        : TexCrc(0), Handle(-1), FrameCount(0), Field_0x20(0)
    {
        Frame[0] = 0.0f;
        Frame[1] = 0.0f;
        Frame[2] = 0.0f;
        Frame[3] = 0.0f;
        Frame[4] = 0.0f;
    }

    // Handle != -1 and the field != 0 -> library +0x40(Handle), Handle =
    // -1; then TexCrc = 0 on every path.
    void FFxTextureWrapper::ReleaseHandle()
    {
        if (Handle != -1 && G_TextureLibrary != nullptr)
        {
            TexInfoLibrary()->DecRefById(Handle);
            Handle = -1;
        }
        TexCrc = 0;
    }

    // (RET 4, returns 0): only when Key->Crc differs from TexCrc -- the
    // release inlined, then TexCrc = Key->Crc.
    void FFxTextureWrapper::SetTexture(const FFxTextureKey* Key)
    {
        if (TexCrc == Key->Crc)
            return;
        if (Handle != -1 && G_TextureLibrary != nullptr)
        {
            TexInfoLibrary()->DecRefById(Handle);
            Handle = -1;
        }
        TexCrc = 0;
        TexCrc = Key->Crc;
    }

    // (no arguments, 0x2010-byte frame):
    //   Handle != -1 or TexCrc == 0 -> return.
    //   library +0x30({"???", TexCrc}, &Record) < 0 -> FDUMP(0x100002, "%s(%d) :
    //     WARNING:General:%s", ImpFxTexInfo.cpp, 0x63, "couldnt get effect texture ???").
    //   Otherwise: +0x34(Record, &Id) with Id starting at -1, +0x68(Record, &FrameCount),
    //   +0x38(Record), Handle = Id, Frame[0] = 0, +0x8c(Id, 0), +0x70(Id, -1, Frame).
    // No test of the field anywhere in this body.
    void FFxTextureWrapper::Resolve()
    {
        int Id = -1;
        if (Handle != -1)
            return;
        if (TexCrc == 0)
            return;
        FFxTextureKey Key;
        Key.Crc = TexCrc;
        Key.Name = kTexInfoKeyName;
        void* Record = nullptr;
        if (TexInfoLibrary()->FindTextureByCrc(&Key, &Record) < 0)
        {
            char Message[0x2000];
            FDUMP(0x100002, kFdumpWarningFmt, kTexInfoCppPath, 0x63,
                  FormatTexInfoMessage(Message, kTexInfoResolveFailFmt, kTexInfoKeyName));
            return;
        }
        TexInfoLibrary()->CreatePlaybackGroup(Record, &Id);
        TexInfoLibrary()->GetFrameCount(Record, &FrameCount);
        TexInfoLibrary()->ReleaseTextureRecord(Record);
        Handle = Id;
        Frame[0] = 0.0f; // MOV dword [+8], 0
        TexInfoLibrary()->SetWrapMode(Id, 0);
        TexInfoLibrary()->GetFrame(Handle, -1, reinterpret_cast<unsigned long*>(Frame));
    }

    // (RET 8): Handle != -1 -> library +0x70(Handle, FrameIndex, Out).
    void FFxTextureWrapper::LookupFrame(int FrameIndex, float* Out)
    {
        if (Handle == -1)
            return;
        TexInfoLibrary()->GetFrame(Handle, FrameIndex, reinterpret_cast<unsigned long*>(Out));
    }

    FGenericFxObjectBase::~FGenericFxObjectBase() noexcept(false)
    {
        if (NameEqual(FactoryName_, "FxRectAppearance"))
        {
            DestroyListAppearance(&G_PoolRect);
            return;
        }
        if (NameEqual(FactoryName_, "FxBasicAppearance"))
        {
            DestroyListAppearance(&G_PoolBasic);
            return;
        }
        // wave24 (sonnet:particlefx-wave24-lane1, 2026-09-11): closes
        // implementation tracker item 132111 step 8 / item 132921 — the
        // destructor-time particle-list teardown wave23 deliberately
        // deferred (this comment replaces that wave's TODO).
        
        // baseline evidence, re-verified against the raw bytes this wave:
        // (item 132921, particlefx) is the real
        // destructor body for this leaf family. Its FIRST action — before
        // touching anything else, including the attached-field array at
        // instance+0x9c/0xa0 (ReleaseAssociatedFields below) or the
        // AssociatedAppearance-equivalent sub-object at instance+0x7c
        // (AssociatedAppearance_->Release() below) — is to erase-range the
        // particle list at instance+0xd8/+0xdc via the routine
        // (particlefx): `while (Node != End) Node =
        // *the routine(ListHeader, &Next, Node);` — i.e. repeatedly
        // unlink-and-free exactly one node per iteration, always fetching
        // that node's OWN next pointer (the routine's own out-param)
        // BEFORE that node is freed, so the walk never touches an
        // already-freed node. the routine itself (particlefx
        // 15516-15540) does exactly what this port's own IntegratePool
        // expiry branch already does: unlink from the instance list,
        // decrement the owning instance's live-count field (only if the
        // node's stored owner pointer is non-null), THEN free the node to
        // the shared arena via `the routine(CachedPool, Node)` —
        // is this port's own already-confirmed FreePoolItem
        // (see FreePoolItem's "the routine" doc comment, particlefx_common.cpp)
        // — the SAME function this destructor's sibling IntegratePool
        // already calls on expiry, not a distinct one. (ALC-4: the
        // call on instance+0xb4 that follows the list teardown
        // at 0x0621910c is the texture wrapper's release -- Texture_ below.)
        
        // So the fix belongs at the very top of this destructor, before
        // AssociatedAppearance_->Release()/ReleaseAssociatedFields() below,
        // mirroring baseline's order exactly.
        
        // No double-free risk: only nodes still reachable from
        // InstanceListHead_/InstanceListTail_ at the moment this destructor
        // runs are touched here. A particle that already aged out during
        // this instance's own lifetime was already unlinked from this same
        // list and freed back to the pool by IntegratePool (particlefx_common.cpp)
        // long before this point, so it is no longer reachable from
        // InstanceListHead_ and cannot be freed a second time here. Nor can
        // the pool independently free it again later: FreePoolItem's own
        // unlink-from-Pool->UsedHead + append-to-Pool->FreeHead happens
        // exactly once, right here, for each node this loop visits, and no
        // other code path reaches InstanceListHead_ once this destructor
        // has run (the instance itself is being destroyed).
        if (InstanceListHead_ != nullptr)
        {
            FAppearancePool* InstancePool = PoolForAppearanceFactory(FactoryName_);
            FPooledParticle* Node = InstanceListHead_;
            while (Node != nullptr)
            {
                FPooledParticle* Next = Node->Next;
                if (InstancePool == &G_PoolRect || InstancePool == &G_PoolPerp)
                {
                    Next = EraseListParticle(InstancePool, Node);
                }
                else
                {
                    UnlinkParticleNode(&InstanceListHead_, &InstanceListTail_, Node);
                    if (LiveInstanceCount_ != 0)
                        --LiveInstanceCount_;
                    ReleaseAppearancePoolItem(InstancePool, Node);
                }
                Node = Next;
            }
        }
        // the routine on the wrapper at +0xb4, after the list
        // teardown and before the field array (+0x9c/+0xa0).
        Texture_.ReleaseHandle();
        if (AssociatedAppearance_ != nullptr)
            AssociatedAppearance_->Release();
        ReleaseAssociatedFields();
        // The node-base destructor releases +0x7c, the set's outer: the set dies with its
        // last node (archetype and clones share it).
        Descriptor_ = nullptr;
        ReleaseDescriptorSlot(PropertySet_);
    }

    // FLBeamField's set creator the routine: a base set (the routine) under the field
    // vtables 0x0624fda8 / 0x0624fcf0, whose QI answers "1.11_IFxNodePropertySet". Every
    // other node class overrides this.
    void FGenericFxObjectBase::CreatePropertySet()
    {
        AdoptPropertySet(new FFxNodePropertySet());
    }

    // FxNode's set creator the routine (node vtable 0x0624f930 slot +0x48): the same base
    // set under FxNode's vtables 0x0624f9e0 / 0x0624f980.
    void FxNode::CreatePropertySet()
    {
        AdoptPropertySet(new (std::nothrow) FFxNodeOwnPropertySet());
    }

    // The tail every creator shares (FxNode 0x0623ff5f..0x0623ff99, sphere 0x0623bc8f..
    // ): Release the old +0x7c, store the new one with AddRef, Release the
    // creation reference, cache the set.
    void FGenericFxObjectBase::AdoptPropertySet(FFxNodePropertySet* Set)
    {
        if (PropertySet_ != Set)
        {
            if (PropertySet_ != nullptr)
                PropertySet_->Release();
            PropertySet_ = Set;
            if (PropertySet_ != nullptr)
                PropertySet_->AddRef();
        }
        Set->Release();
        OnPropertySetChanged();
    }

    void FGenericFxObjectBase::OnPropertySetChanged()
    {
    }

    // (0x0623fde0..0x0623fef2, RET 8, this = node), called by every node
    // factory right after construction (sphere 0x0623cb8d; a non-zero result destroys the
    // node): node slot +0x48 creates the set; a null creation descriptor FDUMPs (0x100002,
    // ImpFxNode.cpp line 0xb3) and goes straight to the tail; otherwise the set's
    // class-name string (+4) is assigned the descriptor's +4 name (0x0623fe1e..0x0623fedb).
    // Tail 0x0623fee0: Duration = [set+0x48]. Returns 0.
    GENRESULT FGenericFxObjectBase::InitializeFromFactory(const FAggDesc8* Desc)
    {
        CreatePropertySet();
        if (Desc == nullptr)
        {
            FDUMP(0x100002, "%s(%d) : WARNING:General:%s", kImpFxNodeCppPath, 0xb3,
                  "you should set NEURONCOMPONENTSDESC for create FxNode\n");
        }
        else
        {
            PropertySet_->ClassName.Assign(Desc->NameAt4);
        }
        std::memcpy(&Duration_, &PropertySet_->LifeSpan, sizeof(Duration_));
        return GR_OK;
    }

    // Every node class's outer QI is the same table walk (e.g. Gravity 06223bb0 over
    // {"1.11_IFxField", 0}, {"1.11_IFxNode", 0}, {"1.11_INeuronComponent", 0}):
    // string compare per row; hit -> AddRef(this + offset), Out = this + offset, 0;
    // miss -> *Out = 0,
    // . Out is written unconditionally (no null test in baseline; the port's
    // old `Out == nullptr -> GR_GENERIC` guard was removed in ALC-5).
    GENRESULT NEURONCOMPONENTS_API FGenericFxObjectBase::QueryInterface(const char* Iid, void** Out)
    {
        if (IidEqual(Iid, PrimaryIid_) || IidEqual(Iid, Iids::NeuronComponent) ||
            IidEqual(Iid, Iids::FxNode))
        {
            AddRef();
            *Out = static_cast<IGenericFx*>(this);
            return GR_OK;
        }
        *Out = nullptr;
        return GR_INTERFACE_UNSUPPORTED;
    }

    unsigned long NEURONCOMPONENTS_API FGenericFxObjectBase::Release()
    {
        // / the routine / the routine: conditional decrement,
        // store 1 at zero, destroy, then free storage only on successful return.
        if (RefCount_ != 0)
            --RefCount_;
        const unsigned long Next = RefCount_;
        if (Next == 0)
        {
            RefCount_ = 1;
            this->~FGenericFxObjectBase();
            ::operator delete(this);
            return 0;
        }
        return Next;
    }

    // (particlefx): `PUSH Dt; CALL [[this]+0x44];
    // XOR EAX,EAX; RET 8` -- TickLifetime through the object's own vtable.
    GENRESULT NEURONCOMPONENTS_API FGenericFxObjectBase::Pad5(float Dt)
    {
        TickLifetime(Dt);
        return GR_OK;
    }

    // (particlefx): index 0 -> &node+0x4c,
    // index 1 -> &node+0x1c, anything else -> 0xfffffffe with *OutSlot untouched.
    GENRESULT NEURONCOMPONENTS_API FGenericFxObjectBase::GetNodeTransform(unsigned long Index,
                                                               float** OutSlot)
    {
        if (Index == 0)
        {
            *OutSlot = NodeWorldXform_;
            return GR_OK;
        }
        if (Index == 1)
        {
            *OutSlot = NodeLocalXform_;
            return GR_OK;
        }
        return static_cast<GENRESULT>(0xfffffffe);
    }

    // (particlefx): index 0 -> copy 12 dwords to
    // node+0x4c and OR flag 0x8; index 1 -> copy to node+0x1c and OR flag 0x4;
    // anything else -> 0xfffffffe, nothing written.
    GENRESULT NEURONCOMPONENTS_API FGenericFxObjectBase::SetNodeTransform(unsigned long Index,
                                                               const float* Source12)
    {
        if (Index == 0)
        {
            std::memcpy(NodeWorldXform_, Source12, sizeof(NodeWorldXform_));
            NodeFlags_ |= 0x8;
            return GR_OK;
        }
        if (Index == 1)
        {
            std::memcpy(NodeLocalXform_, Source12, sizeof(NodeLocalXform_));
            NodeFlags_ |= 0x4;
            return GR_OK;
        }
        return static_cast<GENRESULT>(0xfffffffe);
    }

    // (particlefx), RET 0xC, always returns 0. The
    // first stack argument is never read: every branch takes the parent transform from
    // the second (0x06240313 / 0x0624028d / 0x06240389 / 0x062403d4 all resolve to
    // [entry+0xc]). anim = [[this+0x80]+0x24]; Evaluate is anim+0x2c with
    // (TimeA = this+0x8c, TimeB = this+0x84 Age, Out); Mul is I3DMathEngine +0x1c
    // (the field / the routine, cached independently) pushed as (Dst, A, B).
    //   flag 0x8 set                  -> clear it, keep node+0x4c as set from outside.
    //   flag 0x4 clear, 0x2 or ident. -> raw = identity, world = *Parent12.
    //   flag 0x4 clear, animated      -> raw = Evaluate(...), world = Mul(Parent, raw).
    //   flag 0x4 set,   0x2 or ident. -> world = Mul(Parent, raw).
    //   flag 0x4 set,   animated      -> Local = identity, Evaluate into Local,
    //                                    world = Mul(Parent, raw),
    //                                    world = Mul(world, Local)   (0x062402dd).
    // The IsIdentity call (anim+0x20) is skipped when flag 0x2 is set, as in baseline.
    GENRESULT NEURONCOMPONENTS_API FGenericFxObjectBase::ComposeNodeTransform(void* /*Unused*/,
                                                                   const float* Parent12)
    {
        const unsigned long Flags = NodeFlags_;
        if ((Flags & 0x8) != 0)
        {
            NodeFlags_ = Flags & ~0x8ul;
            return GR_OK;
        }

        const bool bSkipAnim = (Flags & 0x2) != 0 || FindNodeAnimatedTransform()->IsIdentity() != 0;

        if ((Flags & 0x4) == 0)
        {
            if (bSkipAnim)
            {
                // .
                unsigned long Index = 0;
                for (Index = 0; Index < 12; ++Index)
                    NodeLocalXform_[Index] = (Index == 0 || Index == 4 || Index == 8) ? 1.0f : 0.0f;
                std::memcpy(NodeWorldXform_, Parent12, sizeof(NodeWorldXform_));
                return GR_OK;
            }
            // .
            IAnimatedTransform* const Anim = FindNodeAnimatedTransform();
            unsigned long TimeA = 0;
            unsigned long TimeB = 0;
            std::memcpy(&TimeB, &Age_, sizeof(TimeB));
            std::memcpy(&TimeA, &AppearanceOuterTime_, sizeof(TimeA));
            Anim->Evaluate(TimeA, TimeB, NodeLocalXform_);
            float Composed[12];
            NodeTransformMath()->MulTransform(*reinterpret_cast<FTransform*>(Composed),
                                          *reinterpret_cast<const FTransform*>(Parent12),
                                          *reinterpret_cast<const FTransform*>(NodeLocalXform_));
            std::memcpy(NodeWorldXform_, Composed, sizeof(NodeWorldXform_));
            return GR_OK;
        }

        if (bSkipAnim)
        {
            // .
            float Composed[12];
            NodeTransformMath()->MulTransform(*reinterpret_cast<FTransform*>(Composed),
                                          *reinterpret_cast<const FTransform*>(Parent12),
                                          *reinterpret_cast<const FTransform*>(NodeLocalXform_));
            std::memcpy(NodeWorldXform_, Composed, sizeof(NodeWorldXform_));
            return GR_OK;
        }

        // .
        float Local[12] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f };
        IAnimatedTransform* const Anim = FindNodeAnimatedTransform();
        unsigned long TimeA = 0;
        unsigned long TimeB = 0;
        std::memcpy(&TimeB, &Age_, sizeof(TimeB));
        std::memcpy(&TimeA, &AppearanceOuterTime_, sizeof(TimeA));
        Anim->Evaluate(TimeA, TimeB, Local);
        float Composed[12];
        NodeTransformMath()->MulTransform(*reinterpret_cast<FTransform*>(Composed),
                                      *reinterpret_cast<const FTransform*>(Parent12),
                                      *reinterpret_cast<const FTransform*>(NodeLocalXform_));
        std::memcpy(NodeWorldXform_, Composed, sizeof(NodeWorldXform_));
        NodeTransformMath()->MulTransform(*reinterpret_cast<FTransform*>(Composed),
                                      *reinterpret_cast<const FTransform*>(NodeWorldXform_),
                                      *reinterpret_cast<const FTransform*>(Local));
        std::memcpy(NodeWorldXform_, Composed, sizeof(NodeWorldXform_));
        return GR_OK;
    }

    // (particlefx), RET 0xC, returns 0:
    //   Other->vt+0x40(Other, 0, &p)                    (p overwrites the Other slot)
    //   I3DMathEngine +0x5c GeneralInverse writes an uninitialized output buffer;
    //   its result is copied over the identity scratch before the second call.
    //   Rel = I3DMathEngine +0x1c MulTransform(Inv, this+0x4c); *Out12 = Rel.
    GENRESULT FGenericFxObjectBase::ComputeRelativeTransform(INeuronComponent* Other, float* Out12)
    {
        float* OtherWorld = nullptr;
        AsFxNode(Other)->GetNodeTransform(0, &OtherWorld);
        float Inverse[12];
        float W;
        NodeTransformMath()->GeneralInverse(*reinterpret_cast<FTransform*>(Inverse), W,
                             *reinterpret_cast<const FTransform*>(OtherWorld)); // +0x5C
        float Rel[12];
        NodeTransformMath()->MulTransform(*reinterpret_cast<FTransform*>(Rel),
                           *reinterpret_cast<const FTransform*>(Inverse),
                           *reinterpret_cast<const FTransform*>(NodeWorldXform_)); // +0x1C
        std::memcpy(Out12, Rel, sizeof(Rel));
        return GR_OK;
    }

    // (0x0623fc30..0x0623fc89, RET 4): Duration = [[Archetype+0x80]+0x48]
    // (0x0623fc38..0x0623fc41); the own +0x7c Released (0x0623fc47..0x0623fc54), the
    // archetype's +0x7c stored and AddRef'd (0x0623fc5b..0x0623fc68), +0x80 copied. The
    // appearance clone ctors then copy the set cache (basic the routine 0x06215e4b), which
    // OnPropertySetChanged does here.
    void FGenericFxObjectBase::InitNodeFromArchetype(FGenericFxObjectBase* Archetype)
    {
        // ..fc41 transports the stored float bits without an x87 load/store.
        std::memcpy(&Duration_, &Archetype->PropertySet_->LifeSpan, sizeof(Duration_));
        if (PropertySet_ != nullptr)
        {
            PropertySet_->Release();
            PropertySet_ = nullptr;
        }
        // reloads the archetype's live reference after the Release callback.
        PropertySet_ = Archetype->PropertySet_;
        if (PropertySet_ != nullptr)
            PropertySet_->AddRef();
        OnPropertySetChanged();
    }

    // (0x06244120..0x06244318, RET 4): the routine (0x0624412c), the dword
    // +0xa8 -- the appearance set cache, the same set as +0x80 -- copied (0x06244131..
    // ), then the field vector +0x98 assigned (0x06244143..).
    void FGenericFxObjectBase::InitAppearanceFromArchetype(FGenericFxObjectBase* Archetype)
    {
        InitNodeFromArchetype(Archetype);
        CopyAssociatedFieldsFrom(Archetype);
    }

    // (0x0623fc90..0x0623fdd4, RET 8; this = the prototype's node): operator_new
    // (0xa0) and FxNode's constructor inline (the routine's body, vtables 0x0624b190 /
    // , count +0x9c = 1), then the routine's body inline: Duration =
    // [[proto+0x80]+0x48], the new +0x7c Released (null) and set to the prototype's with
    // AddRef, +0x80 copied. *Out = the new outer, `XOR EAX,EAX`.
    GENRESULT NEURONCOMPONENTS_API FxNode::CloneNode(INeuronComponent** Out)
    {
        FxNode* const Node = new FxNode(PrimaryIid_, FactoryName_);
        Node->InitNodeFromArchetype(this);
        *Out = Node;
        return GR_OK;
    }

    // (particlefx): FLD Dt; FADD [+0x84]; FST [+0x84];
    // FCOMP [+0x88]; AND AX,0x4100; JNZ skip -- C0 (Age < Duration) or C3 (equal)
    // skips, so the lifetime bit is set only when Age > Duration.
    GENRESULT NEURONCOMPONENTS_API FGenericFxObjectBase::TickLifetime(float Dt)
    {
        Age_ += Dt;
        if (Age_ > Duration_)
        {
            NodeFlags_ |= 1;
        }
        return GR_OK;
    }

    // ALC-1 (2026-09-26): outer +0x64 is SetNodeAge (inline, particlefx_genericfx.h),
    // not a per-tick body. The emitter tick that used to live here (the routine)
    // is FFxEmitterBase::UpdateEmitter (particlefx_emitters.cpp), reached through the
    // baseline per-node update slot +0x14. The appearance pool integrate that used
    // to live here belongs to each appearance's own +0x14 update (basic
    // etc.) -- see the implementation tracker for which are ported.

    // particlefx-wave15-lane1 (2026-09-10): investigated wave14-lane2's flagged
    // "second, independent per-emitter-node particle-instance mechanism"
    // (implementation tracker items 0x0623a000/a070/a2e0/a360/a4a0/b410/e890/e950/
    // ea30/eb40 and 0x0622f6a0/f880/f8b0/f960/fa10/fcf0/062306e0/06231d30).
    // Re-verified every claim against the raw baseline bytes (particlefx/
    // .txt), extended the trace with 3 functions wave14 had not found, and
    // concluded NO field belongs on this shared base class for either
    // cluster — full reasoning below, since this is exactly the kind of
    // "genuinely need something outside this lane's file ownership" case
    // this project's rules say to document precisely rather than guess.
    
    // === Cluster A: per-node private particle-instance list (a-cluster) ===
    // Fully closed algorithmically (byte-verified, particlefx
    // and 36772-36794):
    //   - A "node" object embeds a private {Head, Tail, FAppearancePool*}
    //     triple at some byte offset (confirmed at +0xe4/+0xe8/+0xec via
    // 's `the routine((void*)(node+0xe4), node+0xc)`), plus
    //     a plain pointer-to-Appearance at +0xc.
    //   - Insert (the routine): AllocPoolItem-equivalent
    //     (the routine(*(node+0xe4+8))) on the PRIVATE pool, copies the same
    //     6 floats + Age/Duration/InvDuration layout as FPooledParticle
    //     (confirming stride 0x30, NOT kStrideParticle's 0x74 — this reuses
    //     bare FPooledParticle, not the particle-with-child-slot variant),
    //     THEN `*(int*)(Appearance+0xe4) += 1` — an owner refcount on the
    //     pointed-to Appearance object, one increment per still-live item —
    //     and finally links the new item onto the node's own list using the
    //     item's own Next/Prev fields (FPooledParticle offset 0x0/0x4, which
    //     this port's AllocPoolItem/FreePoolItem never touch — see
    //     particlefx_common.cpp's AllocPoolItem/FreePoolItem: this port's
    //     free/used bookkeeping lives entirely in a SEPARATE parallel
    //     Pool->FreeNodes[] array, confirmed by direct read, so an item's own
    //     first 8 bytes really are unused by the shared-pool mechanism and
    //     free for exactly this kind of second, independent list).
    //   - Erase-one (the routine, NEW this wave, not in wave14's trace):
    //     unlinks via the routine/a070 (the node's own list), decrements
    //     the SAME `item's stored Appearance+0xe4` refcount, then frees the
    //     item back to the private pool via the routine — confirmed to be
    //     this port's own already-ported FreePoolItem, called on the SAME
    //     private Pool* stored at the list-wrapper's own +8.
    //   - Erase-all (the routine, NEW this wave): repeatedly calls
    // until the list is empty.
    //   - Age/integrate-and-erase-expired (the routine): walks the same
    //     list (via begin/end helper thunks the routine/the routine, which
    //     are themselves trivial wrappers, not real std::list — analysis's
    //     iterator-shaped analysis of a plain intrusive walk), ages
    //     Age += Dt, integrates Pos += Vel*Dt while Age <= Duration, and
    //     calls the routine to erase once Age exceeds Duration. Same
    //     field layout as this port's IntegratePool, over the SAME list.
    //   - Driver (the routine, NEW this wave): a per-frame Update-style
    //     function taking (node, Dt) that calls `the routine(*(node+0x18),
    //     node+0xd8, node+0x98)` (a DIFFERENT container — a std::vector of
    //     POINTERS TO POLYMORPHIC OBJECTS, each with its own vtable, walked
    //     via IsDone at vtbl+0x1c / submit at vtbl+0x9c+0xa4 / finalize at
    //     vtbl+0xa0 — genuinely not FPooledParticle-shaped, so NOT the same
    //     container as the a2e0 list) followed by `the routine(node+0xd8,
    //     Dt)` (aging a THIRD {Head,Tail,Pool*} triple at +0xd8, separate
    //     from the +0xe4 one the routine inserts into). the routine also
    //     touches +0x84/+0x8c/+0x90/+0x94/+0xb0/+0xb4/+0xb8/+0xd0/+0xf0 on
    //     the same node, none of which correspond to any field this port's
    //     FGenericFxObjectBase currently declares, and none of which this
    //     wave had budget to trace further.
    
    // Why nothing was added here: the routine's owner-refcount write target
    // is `*(Appearance-pointer-read-from-node+0xc) + 0xe4`, i.e. it hits
    // offset +0xe4 on whatever object node+0xc points at — and node ITSELF
    // also has its list-wrapper AT +0xe4 (per the routine's own call). Both
    // "node" and "the pointed-to Appearance" are members of the very same
    // FGenericFxObject-family hierarchy baseline used before this port's
    // wave-13 split — i.e. baseline overloads the SAME base-class byte offset
    // with two different per-leaf-type meanings (a list-wrapper on emitter-
    // shaped instances, a plain owner-refcount int on appearance-shaped
    // instances), exactly the FactoryName_-gated god-class pattern wave 13
    // deliberately eliminated. Reproducing that here by adding an "0xe4-
    // equivalent" field to FGenericFxObjectBase would silently reintroduce
    // the same cross-type field-sharing bug wave 13's split was written to
    // prevent (see this file's own header comment). The structurally correct
    // fix is per-leaf: an `OwnerRefCount_` int on whichever appearance
    // leaf(s) can be a `AssociatedAppearance_` target of an emitter, and a
    // private tracked-instance list (Head/Tail + a private FAppearancePool,
    // using this port's own already-shared AllocPoolItem/FreePoolItem/
    // AllocateAppearancePool/FreeAppearancePool from particlefx_common.h) on
    // whichever emitter leaf(s) (particlefx_emitters.h/.cpp) turn out to drive
    // — both files outside this lane's ownership
    // (particlefx_genericfx.h/.cpp only). the routine's own owning object and
    // its extra +0x18..+0xf0 fields were not identified this wave (may be a
    // higher-level per-type render-batch aggregate rather than a single leaf
    // instance) and are flagged as a separate open trace target.
    
    // === Cluster B: per-particle child COM object (f6a0/d30-cluster) ===
    // RESOLVED 2026-09-26 (ALC-1): ported as FxParticleAppearance (particlefx_appearances.cpp).
    // The "arity mismatch" below is two interleaved calls (HashNodeName's RET 8 leaves the
    // &child push for CreateEffect), this+0xC0 is the class's own property set
    // (ParticleApp_LifeName / DeathName), and the children are IFxEffect objects, not
    // ramps. The analysis below is kept as history and is wrong on those three points.
    // (FxParticleAppearance's real per-particle child-object
    // dispatch, gated by `*(*(this+0x30))+0x74 != 0` — a per-instance
    // archetype/config object read from this word-index+0x30 i.e. byte
    // offset +0xC0, whose own +0x74 is an enable flag and +0x70 is a
    // const char* name) creates a "child" object via two globals: DAT_
    // /b90/b94/b9c, confirmed NOT the shared G_ParticleFx/
    // G_FxMeshFactory globals (particlefx_common.h/.cpp).
    
    // particlefx-wave16-lane1 (2026-09-10) re-verified this entire cluster
    // against the raw bytes again (per this project's rule to never just
    // trust a prior wave's summary) and made ONE correction to wave15's
    // trace: wave15 had the field=Unknown / the field=EffectLibrary;
    // direct analysis of the Bind*/Get* thunk cluster (particlefx
    // ) shows the opposite. The thunk cluster is one
    // 9-slot vtable (base module storage, confirmed by a single XREF from
    // its own vtable-install site at 0622ef2f): slot0 Initialize
    // (the branch, XOR EAX,EAX no-op), slots1-3 are the 3 Bind* thunks
    // (each `MOV EAX,[ESP+8]; MOV [DAT_xxx],EAX`), slots4-6 are the matching
    // Get*+AddRef thunks (each `MOV EAX,[DAT_xxx]; if(EAX) AddRef; store to
    // out-param`), slot7/8 are BindMeshFactory/GetMeshFactory (already
    // confirmed both ways as the field). Reading the Bind/Get pairs by
    // the DAT_ each one touches: the branch (Bind slot1) writes
    // , and the branch (Get slot4) reads the field — a
    // matched Bind/Get pair, and since IFxRuntime declares BindParticleFx
    // before BindUnknown before BindEffectLibrary, slot1/4 = ParticleFx (this
    // part already matched wave15). the branch (Bind slot2) writes
    // , matched by the branch (Get slot5) reading
    // — slot2/5 = BindUnknown/GetUnknown, so the field =
    // Unknown (wave15 said EffectLibrary — corrected here). the branch
    // (Bind slot3) writes the field, matched by the branch (Get slot6)
    // reading the field — slot3/6 = BindEffectLibrary/GetEffectLibrary,
    // so the field = EffectLibrary (wave15 said Unknown — corrected
    // here). the field = MeshFactory stands as wave15 already had it.
    
    // With that correction, the routine's two calls are: `the field
    // (ParticleFx)-vtbl+0x14(ParticleFx, archetype->NameAt0x70, &out)` then
    // `the field (EffectLibrary)-vtbl+0x18(EffectLibrary, this[6], out-
    // from-first-call)`. This port's own documented IParticleFx+0x14 is
    // HashNodeName(Name) — RET 8, i.e. this+1 explicit arg — but the actual
    // call site passes 2 explicit args (name AND an out-param), one more
    // than HashNodeName takes. This port's own documented IFxEffectLibrary
    // +0x18 is CreateEffect(Unused, Key, Out) — RET 0x10, i.e. this+3
    // explicit args — but the actual call site passes only 2 explicit args
    // (this[6] and the first call's result), one FEWER than CreateEffect
    // takes. Both mismatches independently reproduce wave15's core finding:
    // the arity genuinely does not cleanly match either documented
    // interface at those byte offsets, so this wave — like wave15 — could
    // NOT conclusively confirm whether the created child is a sub-effect, a
    // light, or a sound, and is NOT guessing an interface shape just to make
    // something compile. Additionally new this wave: the property path that
    // POPULATES the this+0xC0 archetype/config pointer in the first place
    // (i.e. what SetProp* call from a .ale file would ever make the +0x74
    // enable-flag gate true) was not located either — this is a second,
    // deeper layer of ambiguity beyond what wave15 flagged, meaning even a
    // correctly-guessed factory call would have no real trigger path today.
    
    // (analysis IS available — particlefx on, contrary
    // to wave15's note; independently re-read this wave) fills an
    // already-pool-allocated buffer (from FxParticleAppearance's own private
    // pool at this[0x2f], a SEPARATE per-instance pool from the shared
    // appearance pools, allocated via the routine) at +0x8 (owner "this"
    // backpointer), +0xc..+0x20 (the same 6-float Six copy), +0x24/+0x28
    // (Age/Duration), +0x2c (1/Duration via FDIV — confirms InvDuration),
    // +0x6c (the caller-supplied child interface pointer, AddRef'd via its
    // own vtbl+4), +0x70 (zeroed) — an exact FPooledParticle-shaped record
    // repurposed to carry a child object reference instead of representing
    // a rendered particle itself, exactly as wave15 concluded. The result is
    // then linked onto a list at this+0x2d via the routine, and a
    // transform-like float block at this[0x2e] is reset to identity via
    // . the routine is a large function whose own body (further
    // in, particlefx ~31619) separately calls the routine again on an
    // unrelated nested sub-object (param_1+0xc of ITS OWN param_1) — this is
    // NOT recursion on the same FxParticleAppearance instance, just the same
    // symbol reused for a nested component elsewhere in that larger
    // function; not traced further this wave (out of scope for this
    // cluster).
    
    // Given wave6 independently confirmed FxParticleAppearance has zero
    // extra fields today, giving it the fields this needs (this+0x2d list
    // sentinel, this+0x2f private pool, this+0xC0 archetype/config pointer)
    // is squarely an particlefx_appearances.h/.cpp change — but populating them
    // correctly requires resolving BOTH open ambiguities above first
    // (the factory call's real interface, AND the property path that would
    // ever enable the gate), so this wave does not add them speculatively
    // (would be exactly the "fabricate a plausible-looking interface" this
    // project's rules forbid).
    
    // (The hook described next was removed 2026-09-26.)
    // Net effect this wave: added a new protected virtual hook,
    // OnParticleEmitted(void* PoolItem), called from EmitOne immediately
    // after EmitOneParticle succeeds (see EmitOne below) — the correct
    // baseline-matching call order, since the child-object dispatch is
    // logically part of "an emit just happened," not a separate frame-timer
    // event. FxParticleAppearance (particlefx_appearances.h/.cpp) overrides it
    // as a documented no-op that names exactly this gap, rather than either
    // silently inheriting the base's no-op (losing the discoverability of
    // "this is where wave17+ should look") or fabricating a construction
    // call. implementation tracker items in both clusters updated with this wave's
    // corrected trace and the hook-wiring outcome.
    GENRESULT NEURONCOMPONENTS_API FGenericFxObjectBase::EmitOne(const float* Six, unsigned long Extra,
                                                        float Duration)
    {
        FAppearancePool* Pool = PoolForAppearanceFactory(FactoryName_);
        if (Pool == nullptr || Six == nullptr || Duration == 0.0f)
            return GR_GENERIC;
        float Age = 0.0f;
        std::memcpy(&Age, &Extra, 4);
        void* PoolItem = EmitOneParticle(Pool, this, Six, Age, Duration);
        if (PoolItem == nullptr)
            return GR_GENERIC;
        // wave23 (sonnet:particlefx-wave23-lane1, 2026-09-11): link the freshly
        // emitted particle onto THIS instance's own active list (baseline's
        // confirmed instance+0xd8/+0xdc link-in step, the routine —
        // implementation tracker item 132111) instead of only relying on
        // Pool->UsedHead (shared across every instance of this factory) for
        // per-tick aging/field-application. Done here rather than inside
        // EmitOneParticle (particlefx_common.cpp, which only sees an opaque
        // `void* Appearance` and would need particlefx_genericfx.h's full type
        // to reach InstanceListHead_/Tail_ — see particlefx_common.h's forward-
        // declaration comment for why that would create a header cycle) —
        // same net effect as baseline doing it inline in one function, since
        // EmitOneParticle has exactly one call site (here).
        FPooledParticle* Particle = static_cast<FPooledParticle*>(PoolItem);
        AppendParticleNode(&InstanceListHead_, &InstanceListTail_, Particle);
        ++LiveInstanceCount_;
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FGenericFxObjectBase::GetLiveInstanceCount(unsigned long* Out)
    {
        *Out = ResolveLiveInstanceCount();
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FGenericFxObjectBase::GetQueuedVertCount(unsigned long* Out)
    {
        unsigned long VertsPer = 0;
        unsigned long IndicesPer = 0;
        AppearanceDrawUnits(FactoryName_, &VertsPer, &IndicesPer);
        *Out = ResolveLiveInstanceCount() * VertsPer;
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FGenericFxObjectBase::GetQueuedIndexCount(unsigned long* Out)
    {
        unsigned long VertsPer = 0;
        unsigned long IndicesPer = 0;
        AppearanceDrawUnits(FactoryName_, &VertsPer, &IndicesPer);
        *Out = ResolveLiveInstanceCount() * IndicesPer;
        return GR_OK;
    }

    // ---- The base set's property protocol (vtable 0x0624f8d8; FxNode's 0x0624f9e0 has
    // the same bodies at 0x06240590..0x06240800) ----

    // +0x04 06212e60 / +0x08 06212e70 / +0x0c 06209bc0 / +0x10 06209bd0: `MOV EAX,-8;
    // RET 0xc`.
    GENRESULT NEURONCOMPONENTS_API FFxNodePropertySet::GetPropBool(unsigned long /*Hash*/, unsigned char* /*Out*/)
    {
        return GR_ALE_CREATE_FAIL;
    }

    GENRESULT NEURONCOMPONENTS_API FFxNodePropertySet::SetPropBool(unsigned long /*Hash*/, unsigned char /*Value*/)
    {
        return GR_ALE_CREATE_FAIL;
    }

    GENRESULT NEURONCOMPONENTS_API FFxNodePropertySet::GetPropInt(unsigned long /*Hash*/, unsigned long* /*Out*/)
    {
        return GR_ALE_CREATE_FAIL;
    }

    GENRESULT NEURONCOMPONENTS_API FFxNodePropertySet::SetPropInt(unsigned long /*Hash*/, unsigned long /*Value*/)
    {
        return GR_ALE_CREATE_FAIL;
    }

    // +0x14 the routine: Node_LifeSpan -> *Out = [+0x48] (a dword copy), 0; else -8.
    GENRESULT NEURONCOMPONENTS_API FFxNodePropertySet::GetPropFloat(unsigned long Hash, float* Out)
    {
        if (Hash != kNodeLifeSpanHash)
            return GR_ALE_CREATE_FAIL;
        std::memcpy(Out, &LifeSpan, sizeof(LifeSpan));
        return GR_OK;
    }

    // +0x18 the routine: Node_LifeSpan -> FLD / FSTP float [+0x48], 0; else -8.
    GENRESULT NEURONCOMPONENTS_API FFxNodePropertySet::SetPropFloat(unsigned long Hash, float Value)
    {
        if (Hash != kNodeLifeSpanHash)
            return GR_ALE_CREATE_FAIL;
        LifeSpan = Value;
        return GR_OK;
    }

    // +0x1c 06212e80 / +0x20 06212e90: -8.
    GENRESULT NEURONCOMPONENTS_API FFxNodePropertySet::GetPropPair(unsigned long /*Hash*/, unsigned long* /*Out2*/)
    {
        return GR_ALE_CREATE_FAIL;
    }

    GENRESULT NEURONCOMPONENTS_API FFxNodePropertySet::SetPropPair(unsigned long /*Hash*/,
                                                const unsigned long* /*In2*/)
    {
        return GR_ALE_CREATE_FAIL;
    }

    // +0x24 06209be0 / +0x28 06209bf0 / +0x2c 06209c00: -8 in every set class.
    GENRESULT NEURONCOMPONENTS_API FFxNodePropertySet::GetPropColorRamp(unsigned long /*Hash*/, IRampColor** /*Out*/)
    {
        return GR_ALE_CREATE_FAIL;
    }

    GENRESULT NEURONCOMPONENTS_API FFxNodePropertySet::GetPropSingleRamp(unsigned long /*Hash*/,
                                                      IRampSingle** /*Out*/)
    {
        return GR_ALE_CREATE_FAIL;
    }

    GENRESULT NEURONCOMPONENTS_API FFxNodePropertySet::GetPropAnimatedSingle(unsigned long /*Hash*/,
                                                          IFxAnimatedSingle** /*Out*/)
    {
        return GR_ALE_CREATE_FAIL;
    }

    // +0x30 the routine: Node_Name -> *Out = [+0x38], Node_ClassName -> *Out = [+0x14],
    // 0; else -8.
    GENRESULT NEURONCOMPONENTS_API FFxNodePropertySet::GetPropName(unsigned long Hash, const char** Out)
    {
        if (Hash == kNodeNameHash)
        {
            *Out = Name.Buffer;
            return GR_OK;
        }
        if (Hash == kNodeClassNameHash)
        {
            *Out = ClassName.Buffer;
            return GR_OK;
        }
        return GR_ALE_CREATE_FAIL;
    }

    // +0x34 the routine: Node_Name -> assign the hashed name, 0; Node_ClassName ->
    // FDUMP(0x100002, ImpFxNode.cpp line 0x65, "You can't set class name!!\n") and -1
    // (`OR EAX,0xffffffff`, 0x0623fad8); else -8.
    GENRESULT NEURONCOMPONENTS_API FFxNodePropertySet::SetPropName(unsigned long Hash, const char* Text)
    {
        if (Hash == kNodeNameHash)
        {
            Name.Assign(Text);
            return GR_OK;
        }
        if (Hash == kNodeClassNameHash)
        {
            FDUMP(0x100002, "%s(%d) : WARNING:General:%s", kImpFxNodeCppPath, 0x65,
                  "You can't set class name!!\n");
            return GR_GENERIC;
        }
        return GR_ALE_CREATE_FAIL;
    }

    // +0x38 06212ea0 / +0x3c 06212eb0 / +0x40 06225930: -8.
    GENRESULT NEURONCOMPONENTS_API FFxNodePropertySet::GetPropColorRampRamp(unsigned long /*Hash*/,
                                                         IRampColorRamp** /*Out*/)
    {
        return GR_ALE_CREATE_FAIL;
    }

    GENRESULT NEURONCOMPONENTS_API FFxNodePropertySet::GetPropSingleRampRamp(unsigned long /*Hash*/,
                                                          INestedSingleRamp** /*Out*/)
    {
        return GR_ALE_CREATE_FAIL;
    }

    GENRESULT NEURONCOMPONENTS_API FFxNodePropertySet::GetPropAnimatedSingleRamp(unsigned long /*Hash*/,
                                                              INestedSingleRamp** /*Out*/)
    {
        return GR_ALE_CREATE_FAIL;
    }

    // +0x50 the routine: Node_Transform -> *Out = [+0x24], AddRef, 0; else -8.
    GENRESULT NEURONCOMPONENTS_API FFxNodePropertySet::GetPropTransform(unsigned long Hash, IAnimatedTransform** Out)
    {
        if (Hash != kNodeTransformHash)
            return GR_ALE_CREATE_FAIL;
        *Out = Transform;
        (*Out)->AddRef();
        return GR_OK;
    }

    // +0x54 the routine: the base table.
    int NEURONCOMPONENTS_API FFxNodePropertySet::EnumerateProps(FNodePropertyRowCallback Callback,
                                                     void* Context)
    {
        return WalkPropertyRows(kNodePropertyRows, Callback, Context);
    }

    // The outer +0x0c / +0x10 / +0x14 (FxNode 06240450 / 06240520 / 06240540, basic
    // / 06218a90 / 06218ab0): the Node_Name assignment (the the routine body,
    // FFxHashedName::Assign), its buffer (outer +0x3c = set +0x38), its CRC (outer +0x30
    // = set +0x2c); each returns 0.
    GENRESULT NEURONCOMPONENTS_API FFxNodePropertySet::SetInstanceName(const char* Text)
    {
        Name.Assign(Text);
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FFxNodePropertySet::GetInstanceName(const char** Out)
    {
        *Out = Name.Buffer;
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FFxNodePropertySet::GetInstanceNameHash(unsigned long* Out)
    {
        *Out = Name.Hash;
        return GR_OK;
    }

    // (0x0623f920..0x0623f94d): the hash of the first row is tested before
    // the first call; after each call the next row's hash (`MOV EAX,[ESI+0x18]`).
    int FFxNodePropertySet::WalkPropertyRows(const FNodePropertyRow* Rows,
                                               FNodePropertyRowCallback Callback, void* Context)
    {
        const FNodePropertyRow* Row = Rows;
        while (Row->Hash != 0)
        {
            const int Result = Callback(Row, Context);
            if (Result != 0)
                return Result;
            Row += 1;
        }
        return 0;
    }

    // (0x06244330..0x06244357).
    GENRESULT NEURONCOMPONENTS_API FFxAppearancePropertySet::GetPropSingleRampRamp(unsigned long Hash,
                                                                        INestedSingleRamp** Out)
    {
        if (Hash != kAppearanceLodCurveHash)
            return GR_ALE_CREATE_FAIL;
        *Out = EffectScaleRamp;
        (*Out)->AddRef();
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FGenericFxObjectBase::AssociateAppearance(INeuronComponent* Appearance)
    {
        if (AssociatedAppearance_ == Appearance)
            return GR_OK;
        if (AssociatedAppearance_ != nullptr)
            AssociatedAppearance_->Release();
        AssociatedAppearance_ = Appearance;
        if (AssociatedAppearance_ != nullptr)
            AssociatedAppearance_->AddRef();
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FGenericFxObjectBase::AssociateField(INeuronComponent* Field)
    {
        // the argument is a by-value smart pointer (AddRef when non-null
        // at 062443d1), inserted at end through the routine (vector insert n=1 at
        // [+0xa0]), then the copy is Released (0624440c). A null field is stored as a
        // null slot like any other; baseline has no early return. The sole insertion
        // contract is end/n=1; grow to size + max(size, 1), copy/AddRef the old
        // entries, construct/AddRef the new one, then destroy/Release old storage.
        FScopedFieldReference Argument(Field);
        INeuronComponent** Next = nullptr;
        unsigned long Capacity = 0;
        unsigned long Index = 0;
        if (FieldCount_ == FieldCapacity_)
        {
            Capacity = FieldCount_ + (FieldCount_ > 1 ? FieldCount_ : 1);
            Next = new INeuronComponent*[Capacity];
            for (Index = 0; Index < FieldCount_; ++Index)
            {
                Next[Index] = AssociatedFields_[Index];
                if (Next[Index] != nullptr)
                    Next[Index]->AddRef();
            }
            Next[FieldCount_] = Field;
            if (Field != nullptr)
                Field->AddRef();
            for (Index = 0; Index < FieldCount_; ++Index)
            {
                if (AssociatedFields_[Index] != nullptr)
                {
                    AssociatedFields_[Index]->Release();
                    AssociatedFields_[Index] = nullptr;
                }
            }
            delete[] AssociatedFields_;
            AssociatedFields_ = Next;
            FieldCapacity_ = Capacity;
        }
        else
        {
            AssociatedFields_[FieldCount_] = Field;
            if (Field != nullptr)
                Field->AddRef();
        }
        FieldCount_ += 1;
        return GR_OK;
    }

    // (appearance +0xB4, entered through `ADD this,0xc` on every
    // appearance vtable): for each field in node+0x9c..+0xa0 call the field's +0x9c
    // with [node+0x18], the appearance. That slot is the port's BeginField.
    // BeginField's Dt is port-only -- baseline Begin takes the appearance alone
    // (Gravity the branch, Air the branch and Radial the branch read one stack
    // argument) -- so 0.0f goes in here. No field's BeginField reads it: the radial
    // blend that needs Dt lives in its own update (FxRadialField::Pad5 = the branch).
    GENRESULT NEURONCOMPONENTS_API FGenericFxObjectBase::Pad45()
    {
        INeuronComponent** Position = AssociatedFields_;
        INeuronComponent** End = Position;
        if (FieldCount_ != 0)
            End += FieldCount_;
        while (Position != End)
        {
            AsFxNode(*Position)->BeginField(static_cast<IGenericFx*>(this), 0.0f);
            ++Position;
        }
        return GR_OK;
    }

    // (appearance +0xB8, the same `ADD this,0xc` thunk on all eight
    // appearance vtables, e.g. basic the branch, beam the routine, dust
    // ). The emitter calls it before the spawn (+0xBC) and spawns only
    // when it is not negative.
    // FLD Duration; FCOMP double [the field] (0.01); TEST AH,0x5;
    //     JP -> continue. Parity is even when neither C0 nor C2 is set, i.e.
    //     Duration >= 0.01 (or unordered); Duration < 0.01 falls through to
    //     `OR EAX,-1` -> return -1.
    //   The Six copy and the Age copy live across the whole field walk (one copy,
    //   not one per field). Per field: +0xa4(&SixCopy, &AgeCopy), then
    // FCOMP AgeCopy vs Age; TEST AH,0x44; JNP -> next field
    //   (odd parity = C3 alone = equal). Otherwise 0x06244634: FCOMP AgeCopy vs
    //   Duration; AND 0x4100; JZ -> return -1 (neither C0 nor C3: AgeCopy >
    //   Duration).
    //   Return 0 after the walk.
    // Field +0xa4 is the port's ApplyFieldToVel; its Dt is port-only (baseline RET 0xc,
    // two arguments) and nothing has elapsed for a particle being spawned, so 0.0f.
    GENRESULT NEURONCOMPONENTS_API FGenericFxObjectBase::Pad46(const float* Six, float Age, float Duration)
    {
        if (static_cast<double>(Duration) < 0.01)
            return GR_GENERIC;

        float SixCopy[6];
        for (unsigned long CopyIndex = 0; CopyIndex < 6; ++CopyIndex)
            SixCopy[CopyIndex] = Six[CopyIndex];

        float AgeCopy = Age;
        for (unsigned long FieldIndex = 0; FieldIndex < FieldCount_; ++FieldIndex)
        {
            IGenericFx* Field = QueryGenericFx(AssociatedFields_[FieldIndex]);
            Field->ApplyFieldToVel(SixCopy, &AgeCopy, 0.0f);
            Field->Release();
            if (AgeCopy != Age && AgeCopy > Duration)
                return GR_GENERIC;
        }
        return GR_OK;
    }

    // borrowed field End calls over a captured vector range.
    GENRESULT NEURONCOMPONENTS_API FGenericFxObjectBase::Pad48()
    {
        INeuronComponent** Position = AssociatedFields_;
        INeuronComponent** End = Position;
        if (FieldCount_ != 0)
            End += FieldCount_;
        while (Position != End)
        {
            AsFxNode(*Position)->Pad40();
            ++Position;
        }
        return GR_OK;
    }

    // Beam the routine / Dust the routine: cache the successor, unlink, decrement
    // the item's owning node count when present, and return the item to its pool.
    FPooledParticle* FGenericFxObjectBase::EraseListParticle(FAppearancePool* Pool,
                                                            FPooledParticle* Particle)
    {
        FPooledParticle* const Next = Particle->Next;
        UnlinkParticleNode(&InstanceListHead_, &InstanceListTail_, Particle);
        FGenericFxObjectBase* const Owner =
            static_cast<FGenericFxObjectBase*>(Particle->Appearance);
        if (Owner != nullptr)
            --Owner->LiveInstanceCount_;
        ReleaseAppearancePoolItem(Pool, Particle);
        return Next;
    }

    // Beam the routine / Dust the routine: the pool's oldest used link identifies
    // an item whose owner performs the list erase; the caller tests FreeCount.
    void FGenericFxObjectBase::EvictOldestListParticle(FAppearancePool* Pool)
    {
        const unsigned long Index =
            static_cast<unsigned long>(Pool->UsedHead - Pool->FreeNodes);
        FPooledParticle* const Particle = reinterpret_cast<FPooledParticle*>(
            static_cast<unsigned char*>(Pool->Items) + Index * Pool->Stride);
        FGenericFxObjectBase* const Owner =
            static_cast<FGenericFxObjectBase*>(Particle->Appearance);
        Owner->EraseListParticle(Pool, Particle);
    }

    // Shared body of the four billboard spawns (see the header). Traced on basic
    // ; rect the routine/the routine, perp the routine/the routine
    // and oriented the routine are the same instructions against their own pool
    // globals.
    // if the pool's free count (+0x10, basic the field) is 0, take
    //     the oldest used item -- the used-head link (+0x14) turned into an index
    //     ((link - links) >> 3) times the stride, plus the items base -- and erase it
    //     from ITS owner's list: the routine (rect the routine -> the routine,
    //     perp the routine -> the routine, oriented the routine) unlinks it from
    //     owner+0xd8, DECs owner+0xe4 when the owner is non-null (0x06217b29) and
    //     frees it to the list's pool (the routine on [list+8]).
    // alloc from [node+0xe0] (the routine). Item +0xc..+0x20 = Six,
    //     +8 = node, +0x24 = Age, +0x28 = Duration, +0x2c = 1.0 / Duration (no
    //     zero check), INC node+0xe4.
    // item->next = 0; append at the tail (+0xdc), or make it the head
    //     (+0xd8) when the list is empty. Returns 0.
    // An alloc failure leaves EAX = 0 and the next store faults; after the eviction
    // above that only happens with an unallocated pool, so no check here either.
    // Until 2026-09-26 these four classes spawned into a second, private list that
    // nothing drew (PrivateListSpawn in particlefx_appearances.cpp), and EmitOne skipped
    // a zero Duration and a full pool instead of evicting.
    FPooledParticle* FGenericFxObjectBase::SpawnListParticle(FAppearancePool* Pool,
                                                             const float* Six, const float& Age,
                                                             const float& Duration)
    {
        if (Pool->FreeCount == 0)
        {
            const unsigned long OldestIndex =
                static_cast<unsigned long>(Pool->UsedHead - Pool->FreeNodes);
            FPooledParticle* Oldest = reinterpret_cast<FPooledParticle*>(
                static_cast<unsigned char*>(Pool->Items) + OldestIndex * Pool->Stride);
            FGenericFxObjectBase* OldestOwner =
                static_cast<FGenericFxObjectBase*>(Oldest->Appearance);
            UnlinkParticleNode(&OldestOwner->InstanceListHead_, &OldestOwner->InstanceListTail_,
                               Oldest);
            if (OldestOwner != nullptr)
                --OldestOwner->LiveInstanceCount_;
            ReleaseAppearancePoolItem(Pool, Oldest);
        }

        FPooledParticle* Particle = static_cast<FPooledParticle*>(AllocPoolItem(Pool));
        if (Particle != nullptr)
        {
            InitializeListParticle(*Particle, this, Six, Age, Duration);
            ++LiveInstanceCount_;
        }
        AppendParticleNode(&InstanceListHead_, &InstanceListTail_, Particle);
        return Particle;
    }

    // Shared per-node update of the five list appearances. Traced on basic
    // (node this, RET 8); rect the routine, perp the routine, oriented
    // and dust the routine have the same body and differ only in the
    // draw-context counters they zero, the descriptor ramp FrameRamp names (basic +0x84,
    // oriented / rect +0x88, perp / dust +0x80) and their field-walk / integrate twins.
    // zero the class's draw-context u16 counters +0xa / +0xc (basic
    // / the field, rect 0x06238789, perp 0x06233049, oriented
    // , dust 0x0620fb79) -- the caller passes its own class's context.
    // Age (node+0x84) += Dt.
    // if node+0xb0 (emitter refs) == 0 and the list head (node+0xd8) is
    //     null, node flags (+0x4) |= 1.
    // the routine(wrapper node+0xb4, &{"???", [desc]+0x64}); when the
    //     wrapper's handle is still -1, the routine.
    // ([desc+0x60] & 8) == 8 (`AND EDX,8; CMP DL,8; JNZ`, fall-through =
    //     set): Pos = (float) FrameRamp->Eval(node+0x8c, Age) (FSTP dword); if
    //     FrameCount != 1 and Handle != -1: library +0x70(Handle, ftol((double)(unsigned
    //     64) (FrameCount - 1) * Pos), &Frame) -- FILD qword of {FrameCount - 1, 0}.
    //     Otherwise (bit clear): FrameCount != 1 -> the routine(Dt).
    // the routine([node+0x18], &list, &node+0x98) -- the field walk,
    //     ApplyFieldsToPool.
    // the routine(&list, Dt) -- the integrate, IntegratePool.
    void FGenericFxObjectBase::UpdateListAppearance(FAlDrawContext& DrawContext,
                                                    FAppearancePool* Pool, float Dt,
                                                    INestedSingleRamp* FrameRamp)
    {
        DrawContext.Field_0xA = 0;
        DrawContext.PrimCount = 0;
        Age_ += Dt;
        if (EmitterRefCount_ == 0 && InstanceListHead_ == nullptr)
            NodeFlags_ |= 1;

        FFxTextureKey Key;
        Key.Name = kTexInfoKeyName;
        Key.Crc = Descriptor_->TextureCrc;
        Texture_.SetTexture(&Key);
        if (Texture_.Handle == -1)
            Texture_.Resolve();
        if ((Descriptor_->Flags & 8u) == 8u)
        {
            const float Pos = static_cast<float>(FrameRamp->Eval(AppearanceOuterTime_, Age_));
            if (Texture_.FrameCount != 1 && Texture_.Handle != -1)
            {
                const unsigned long Span = static_cast<unsigned long>(Texture_.FrameCount - 1);
                const long long Frame = static_cast<long long>(
                    static_cast<double>(Span) * static_cast<double>(Pos));
                TexInfoLibrary()->GetFrame(Texture_.Handle, static_cast<int>(Frame),
                                           reinterpret_cast<unsigned long*>(Texture_.Frame));
            }
        }
        else if (Texture_.FrameCount != 1)
        {
            Texture_.Advance(Dt);
        }

        ApplyFieldsToPool(this, false);
        IntegratePool(this, Pool, Dt);
    }


    // Internal borrowed first-entry convenience, separate from IFxAppearance+0xDC.
    INeuronComponent* NEURONCOMPONENTS_API FGenericFxObjectBase::GetAssociatedField()
    {
        if (FieldCount_ == 0 || AssociatedFields_ == nullptr)
            return nullptr;
        return AssociatedFields_[0];
    }

    // , node+0x88 / IFxAppearance+0xDC. The error branch does not
    // write Out; the valid branch writes before AddRef and has no null-field guard.
    GENRESULT NEURONCOMPONENTS_API FGenericFxObjectBase::GetAssociatedFieldAt(unsigned long Index,
                                                                INeuronComponent** Out)
    {
        if (static_cast<long>(Index) < 0 || AssociatedFields_ == nullptr || Index >= FieldCount_)
        {
            FDUMP(0x100001, "%s(%d) : ERROR:General:%s",
                  "ALIL\\ImpFxAppearance.cpp",
                  0x5c, "Index range over.\n");
            return GR_GENERIC;
        }
        INeuronComponent* Field = AssociatedFields_[Index];
        *Out = Field;
        Field->AddRef();
        return GR_OK;
    }

    INeuronComponent* FGenericFxObjectBase::BorrowAssociatedFieldAt(unsigned long Index)
    {
        if (Index >= FieldCount_ || AssociatedFields_ == nullptr)
            return nullptr;
        return AssociatedFields_[Index];
    }

    // uses &Value as its iterator, even though the captured end is
    // the associated-field array end. Preserve its unchecked pointer/control flow;
    // words beyond Value and physical baseline caller-stack contents are undefined.
    // Keep optimizers and Debug /RTC from replacing that boundary with a guard.
#pragma optimize("", off)
#pragma runtime_checks("", off)
    __declspec(noinline) GENRESULT NEURONCOMPONENTS_API FGenericFxObjectBase::Pad44(INeuronComponent* Value)
    {
        INeuronComponent* FinalValue = Value;
        if (Value != nullptr)
            Value->AddRef();

        FPointerCleanup<INeuronComponent> Cleanup(Value);
        INeuronComponent** Cursor = &Value;
        INeuronComponent** const ShiftEnd = AssociatedFields_ + FieldCount_;
        if (Cursor + 1 != ShiftEnd)
        {
            do
            {
                if (*Cursor != nullptr)
                {
                    (*Cursor)->Release();
                    *Cursor = nullptr;
                }
                *Cursor = Cursor[1];
                if (*Cursor != nullptr)
                    (*Cursor)->AddRef();
                ++Cursor;
            } while (Cursor + 1 != ShiftEnd);
            FinalValue = Value;
        }

        INeuronComponent** const TailEnd = AssociatedFields_ + FieldCount_;
        Cursor = TailEnd - 1;
        while (Cursor != TailEnd)
        {
            if (*Cursor != nullptr)
            {
                (*Cursor)->Release();
                *Cursor = nullptr;
            }
            ++Cursor;
        }
        const unsigned long LiveCount = FieldCount_;
        Cleanup.Dismiss();
        FieldCount_ = LiveCount - 1;
        if (FinalValue != nullptr)
            FinalValue->Release();
        return GR_OK;
    }
#pragma runtime_checks("", restore)
#pragma optimize("", on)

    // 's erase branch: each shifted slot uses the routine
    // (Release old, clear, copy next, AddRef new); the routine then destroys
    // the duplicate last slot before the vector end decreases.
    void FGenericFxObjectBase::RemoveAssociatedFieldAt(unsigned long Index)
    {
        if (AssociatedFields_ == nullptr || Index >= FieldCount_)
            return;
        for (unsigned long Shift = Index; Shift + 1 < FieldCount_; ++Shift)
        {
            if (AssociatedFields_[Shift] != nullptr)
            {
                AssociatedFields_[Shift]->Release();
                AssociatedFields_[Shift] = nullptr;
            }
            AssociatedFields_[Shift] = AssociatedFields_[Shift + 1];
            if (AssociatedFields_[Shift] != nullptr)
                AssociatedFields_[Shift]->AddRef();
        }
        if (AssociatedFields_[FieldCount_ - 1] != nullptr)
            AssociatedFields_[FieldCount_ - 1]->Release();
        AssociatedFields_[FieldCount_ - 1] = nullptr;
        FieldCount_ -= 1;
    }

    // captured shift end, freshly read tail end, then live end decrement.
    INeuronComponent** FGenericFxObjectBase::EraseAssociatedField(INeuronComponent** Position)
    {
        INeuronComponent** const ShiftEnd = AssociatedFields_ + FieldCount_;
        INeuronComponent** Destination = Position;
        INeuronComponent** Source = Position + 1;
        while (Source != ShiftEnd)
        {
            if (*Destination != nullptr)
            {
                (*Destination)->Release();
                *Destination = nullptr;
            }
            *Destination = *Source;
            if (*Destination != nullptr)
                (*Destination)->AddRef();
            Destination = Source;
            Source = Destination + 1;
        }
        INeuronComponent** const TailEnd = AssociatedFields_ + FieldCount_;
        Destination = TailEnd - 1;
        while (Destination != TailEnd)
        {
            if (*Destination != nullptr)
            {
                (*Destination)->Release();
                *Destination = nullptr;
            }
            ++Destination;
        }
        --FieldCount_;
        return Position;
    }

    // 0x06244143..0x062441ef (and the reallocating branch from
    // ), after the +0xa8 set cache: the field vector is assigned from the
    // archetype's unless it is the same object
    // (CMP at 0x06244149). Size <= own size: element-wise smart-pointer assignment
    // (the routine: AddRef new, Release old) and the surplus slots destroyed
    // (the routine: Release); otherwise the elements are copied into new storage
    // (the routine: copy + AddRef; operator_new at 0x062442f2 is not null-tested). The
    // net effect for every path: this list holds the archetype's fields, each AddRef'd
    // once, and the old ones are Released.
    void FGenericFxObjectBase::CopyAssociatedFieldsFrom(FGenericFxObjectBase* Source)
    {
        INeuronComponent** Next = nullptr;
        unsigned long Index = 0;
        if (Source == this)
            return;
        if (Source->FieldCount_ > FieldCapacity_)
            Next = new INeuronComponent*[Source->FieldCount_];
        for (Index = 0; Index < Source->FieldCount_; ++Index)
        {
            if (Source->AssociatedFields_[Index] != nullptr)
                Source->AssociatedFields_[Index]->AddRef();
        }
        for (Index = 0; Index < FieldCount_; ++Index)
        {
            if (AssociatedFields_[Index] != nullptr)
                AssociatedFields_[Index]->Release();
        }
        if (Next != nullptr)
        {
            delete[] AssociatedFields_;
            AssociatedFields_ = Next;
            FieldCapacity_ = Source->FieldCount_;
        }
        for (Index = 0; Index < Source->FieldCount_; ++Index)
            AssociatedFields_[Index] = Source->AssociatedFields_[Index];
        FieldCount_ = Source->FieldCount_;
    }

    void FGenericFxObjectBase::DetachRectList()
    {
        while (InstanceListHead_ != nullptr)
        {
            FPooledParticle* Particle = InstanceListHead_;
            InstanceListHead_ = Particle->Next;
            Particle->Next = nullptr;
            Particle->Prev = nullptr;
        }
        InstanceListHead_ = nullptr;
        InstanceListTail_ = nullptr;
    }

    void FGenericFxObjectBase::ReleaseRectFields()
    {
        INeuronComponent** Position = AssociatedFields_;
        INeuronComponent** End = Position;
        if (FieldCount_ != 0)
            End += FieldCount_;
        while (Position != End)
        {
            ReleaseDescriptorSlot(*Position);
            ++Position;
        }
        delete[] AssociatedFields_;
        AssociatedFields_ = nullptr;
        FieldCount_ = 0;
        FieldCapacity_ = 0;
    }

    void FGenericFxObjectBase::ReleaseRectPropertySet()
    {
        ReleaseDescriptorSlot(PropertySet_);
        Descriptor_ = nullptr;
    }

    void FGenericFxObjectBase::DestroyListAppearance(FAppearancePool* Pool)
    {
        FMemberCleanup<FGenericFxObjectBase> PropertyScope(this,
            &FGenericFxObjectBase::ReleaseRectPropertySet);
        FMemberCleanup<FGenericFxObjectBase> FieldsScope(this,
            &FGenericFxObjectBase::ReleaseRectFields);
        FMemberCleanup<FFxTextureWrapper> TextureScope(&Texture_, &FFxTextureWrapper::ReleaseHandle);
        FMemberCleanup<FGenericFxObjectBase> ListScope(this, &FGenericFxObjectBase::DetachRectList);
        FPooledParticle* Particle = InstanceListHead_;
        while (Particle != nullptr)
            Particle = EraseListParticle(Pool, Particle);
        ListScope.Dismiss();
        DetachRectList();
        TextureScope.Dismiss();
        Texture_.ReleaseHandle();
        FieldsScope.Dismiss();
        ReleaseRectFields();
        PropertyScope.Dismiss();
        ReleaseRectPropertySet();
    }

    void FGenericFxObjectBase::ReleaseAssociatedFields()
    {
        unsigned long Index = 0;
        for (Index = 0; Index < FieldCount_; ++Index)
        {
            if (AssociatedFields_[Index] != nullptr)
                AssociatedFields_[Index]->Release();
        }
        delete[] AssociatedFields_;
        AssociatedFields_ = nullptr;
        FieldCount_ = 0;
        FieldCapacity_ = 0;
    }

    unsigned long FGenericFxObjectBase::ResolveLiveInstanceCount() const
    {
        // wave23 (sonnet:particlefx-wave23-lane1, 2026-09-11): LiveInstanceCount_
        // is now maintained incrementally and per-INSTANCE (EmitOne
        // increments it when linking a new particle onto InstanceListHead_;
        // IntegratePool decrements it when unlinking an expired one) instead
        // of being resynced from PoolUsedCount(Pool) on every emit.
        // PoolUsedCount counts every live particle of this FACTORY across
        // every instance sharing the pool, not just this instance's own —
        // using it here was part of the same wrong-scope bug family
        // implementation tracker item 132111 tracks (GetLiveInstanceCount/
        // GetQueuedVertCount/GetQueuedIndexCount would otherwise report
        // OTHER instances' particles as this instance's own). PoolUsedCount
        // itself is untouched (still correct for whatever future use wants
        // a genuine pool-wide count) — see particlefx_common.h/.cpp.
        
        // 2026-09-26: the appearance slots read the raw count, nothing else -- basic
        // `*Out = [outer+0xf0]` (node+0xe4), the branch `<< 2`,
        // `* 6`. The fallback that reported 1 for an empty node with
        // an appearance, a field or a property was invented; it made the emitter's
        // MaxParticles check (the routine) and the draw budget count particles
        // that do not exist.
        return LiveInstanceCount_;
    }

    // (wave71's UpdateInstanceBoundsSphere and its sqrt merge copy were removed
    // 2026-09-26 by ALC-1: the routine is FLBeamAppearance::GetBoundingSphere, see
    // particlefx_appearances.cpp. Its "Ramp(+0x7c) * OuterTime * 0.5" reading was
    // wrong: the listing multiplies the +0x7c ramp's value by the +0x78 ramp's value
    // (not by node+0x8c), then halves it; the ramps are the list-owner's descriptor, and
    // the walk ends at the list tail, not at a null link.)

} // namespace ParticleFxImpl
