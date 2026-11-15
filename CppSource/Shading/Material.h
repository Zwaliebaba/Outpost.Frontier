// Author: Giperion.

#pragma once

#include "ShadingInternal.h"

class FMaterial;

// Property accessors return records from these shared contiguous schemas.
FShadingPropertyRecord* GetBtDetailPropertySchema();
FShadingPropertyRecord* GetDetailMapPropertySchema();
FShadingPropertyRecord* GetAtmospherePropertySchema();
FShadingPropertyRecord* GetSinglePassPropertySchema();
FShadingPropertyRecord* GetIllumDetailPropertySchema();
FShadingPropertyRecord* GetEcEtPropertySchema();
FShadingPropertyRecord* GetDcDtEtPropertySchema();

class FMaterialInner final : public INeuronComponent
{
public:
    FMaterialInner();

    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;

    FMaterial* OwnerFromThis();
};

class FMaterial final : public IMaterial
{
public:
    FMaterial(const char* FactoryName, const char* ClassName, EMaterialBind BindKind,
              EMaterialSubmit SubmitKind);
    ~FMaterial() noexcept(false);

    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;

    GENRESULT NEURONCOMPONENTS_API Initialize(INeuronComponent* Outer) override;
    GENRESULT NEURONCOMPONENTS_API LoadFromFileSystem(void* FileSystem) override;
    GENRESULT NEURONCOMPONENTS_API SaveProperties(void* FileSystem) override;
    GENRESULT NEURONCOMPONENTS_API ApplyVerify(void* Context, void* Extra) override;
    GENRESULT NEURONCOMPONENTS_API ResolveTextures(float Delta) override;
    GENRESULT NEURONCOMPONENTS_API Bind(void* Context) override;
    GENRESULT NEURONCOMPONENTS_API Teardown(void* Context) override;
    GENRESULT NEURONCOMPONENTS_API Submit(void* Context, unsigned long PrimType, unsigned long Start,
                               unsigned long Count, unsigned long Zero, unsigned long Field,
                               unsigned long Flags) override;
    GENRESULT NEURONCOMPONENTS_API SetName(const char* Name) override;
    GENRESULT NEURONCOMPONENTS_API GetName(char* Out, unsigned long Size) override;
    GENRESULT NEURONCOMPONENTS_API CopyClassName(char* Out, unsigned long Size) override;
    GENRESULT NEURONCOMPONENTS_API GetPassCount(int* Out) override;
    GENRESULT NEURONCOMPONENTS_API GetRenderFlags(unsigned long* Out) override;
    GENRESULT NEURONCOMPONENTS_API GetParameterCount(unsigned long* OutCount) override;
    GENRESULT NEURONCOMPONENTS_API GetParameterDescriptor(unsigned long Index, void* OutDesc) override;
    GENRESULT NEURONCOMPONENTS_API GetParameterValue(unsigned long Index, void* InOutDesc) override;
    GENRESULT NEURONCOMPONENTS_API SetParameterValue(unsigned long Index, void* Value) override;
    const char* __thiscall ClassName() override;
    void* __thiscall ScalarDeletingDestructor(unsigned char bDelete) override;
    GENRESULT __thiscall InitializeFromDescriptor(void* Arg) override;
    GENRESULT __thiscall ApplyInitialRenderStates(void* Arg) override;
    GENRESULT __thiscall Setup() override;
    bool __thiscall ValueWithinDeclaredRange(unsigned long Index, void* Entry, void* Value) override;
    GENRESULT __thiscall StoreParameterValue(unsigned long Index, void* Entry, void* Value) override;
    unsigned long __thiscall GetParameterTable(void** OutBase) override;
    void* __thiscall GetParameterAddress(unsigned long Index) override;
    GENRESULT NEURONCOMPONENTS_API InnerQueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API InnerAddRef() override;
    unsigned long NEURONCOMPONENTS_API InnerRelease() override;
    void ConfigureAggregation(FMaterialAggregateDesc* Desc);
    IRenderPipeline* Pipeline; // +4
    ITextureLibrary2* TexLib;    // Borrowed from the outer component.
    IVertexBufferManager* VBMgr;
    IRPDraw* RPDraw;
    const char* InstanceName;    // +14  the branch GetCRC32 → IMatAnimLibrary slot 5
    FTexBlock* TexBlocks;        // +18
    unsigned long TexCount;      // +1C
    float DiffuseR;              // +20
    float DiffuseG;
    float DiffuseB;
    float Opacity;               // +2C
    float Ambient[4];            // +30
    float Specular[4];           // +40
    float Emissive[4];           // +50
    float SpecularPower;         // +60
    unsigned char bAlphablend;   // +64  "Oc"
    unsigned char bAlphatest;    // +65
    unsigned char Pad66[2];
    IImaInstance* MatAnim;               // +68  DcDt IMAInstance; Atmosphere baseline = FadeOff float
    unsigned long Pad6C;         // +6C  Atmosphere baseline = Alpha float
    FMaterialInner Inner;        // +70  SinglePass inner QI; Atmosphere baseline = Scale float
    unsigned char bAtmosphereReady; // +74  byte the routine / the routine / the branch
    unsigned char Pad75[3];
    float FadeOff;               // Atmosphere property 6, default 1.0 (the field)
    float Alpha;                 // Atmosphere property 7, default 0.75 (the field)
    float Scale;                 // Atmosphere property 8, default the field ≈ 1.025
    unsigned long DetailFlipU;
    unsigned long DetailFlipV;
    float DetailTileRate0;
    float DetailTileRate1;
    unsigned long DetailPassCount; // Detail +0x74, illum/masked +0x78: 1 or 2 passes
    bool bDetailSecondStageVerified; // Detail +0x79, illum/masked +0x7D
    bool bDetailMultiPassEnabled; // Detail +0x78, illum/masked +0x7C: global record0 gate


    unsigned long RefCount;
    const char* FactoryName;
    const char* ClassNameText;  // slot 20 answers with this, else FactoryName
    EMaterialBind BindKind;
    EMaterialSubmit SubmitKind;
    INeuronComponent* Outer;
    bool bSecondaryTextureEnabled;
    bool bSecondaryTextureVerified;
    unsigned long TextureAllocationCount;


    void SetInstanceName(const char* Name);
    GENRESULT InitializeOwnedSchema();
    GENRESULT LoadDeclaredSchema(IFileSystem* FileSystem);
    GENRESULT ApplyVerifyMatAnim();
    void ApplyOcFlag();
    void BindDcDtTables(void* Context);
    void BindLightMapTables(void* Context);
    void BindNebulaTables(void* Context);
    void BindAtmosphereTables();
    void BindDcDtEtTables(void* Context);
    void BindEcEtTables(void* Context);
    void BindDetailMapTables(void* Context);
    GENRESULT ApplyVerifyDetailFamily();
    GENRESULT ApplyVerifyIllumDetail();
    bool ApplyDetailPassState(unsigned long Pass);
    bool ApplyIllumDetailPassState(unsigned long Pass);
    bool ApplyMaskedDetailPassState(unsigned long Pass);
    void BindBtDetailTables(void* Context);
    GENRESULT SubmitIndexedListing(FMaterialSubmitContext* Context, unsigned long PrimType,
                                   unsigned long Start, unsigned long Count, unsigned long IbStart,
                                   unsigned long IndexSpan);
    GENRESULT SubmitCopiedVertices(FMaterialSubmitContext* Context, unsigned long PrimType,
                                   unsigned long Start, unsigned long Count, unsigned long IbStart,
                                   unsigned long IndexSpan);
    GENRESULT SubmitAlphaTest(void* Context, unsigned long PrimType, unsigned long Start,
                              unsigned long Count, unsigned long IbStart, unsigned long IndexSpan,
                              unsigned long Flags);
    GENRESULT SubmitKernel(void* Context, unsigned long PrimType, unsigned long Start, unsigned long Count,
                           unsigned long Zero, unsigned long Field, unsigned long Flags);
    GENRESULT SubmitDcDtTwoSecondPass(void* Context, unsigned long PrimType, unsigned long Start,
                                 unsigned long Count, unsigned long Zero, unsigned long Field);
    GENRESULT SubmitBtDetailTwoSecondPass(void* Context, unsigned long PrimType,
                                          unsigned long Start, unsigned long Count,
                                          unsigned long IbStart, unsigned long IndexSpan);
    GENRESULT SubmitAtmosphereLock(void* Context, unsigned long PrimType, unsigned long Start,
                                   unsigned long Count, unsigned long Zero, unsigned long Field);
};

class FBatchedFace final : public IBatchedMaterial
{
public:
    FBatchedFace();

    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;
    GENRESULT NEURONCOMPONENTS_API CreateInnerMaterial(void* Name) override;
    GENRESULT NEURONCOMPONENTS_API SetBatchBin(void* Value) override;
    GENRESULT NEURONCOMPONENTS_API GetBatchBin(void** Out) override;
    GENRESULT NEURONCOMPONENTS_API Bind(void* Context) override;
    GENRESULT NEURONCOMPONENTS_API Teardown(void* Context) override;
    GENRESULT NEURONCOMPONENTS_API Submit(void* Context, unsigned long PrimType, unsigned long Start, unsigned long Count,
                              unsigned long Zero, unsigned long Field, unsigned long Flags) override;

    class FBatchedMaterial* OwnerFromThis();
};

class FBatchedController final : public INeuronComponent
{
public:
    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;
    class FBatchedMaterial* OwnerFromThis();
};

class FBatchedMaterial final : public IMaterial
{
public:
    FBatchedMaterial();
    ~FBatchedMaterial() noexcept(false);

    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;

    GENRESULT NEURONCOMPONENTS_API Initialize(INeuronComponent* Outer) override;
    GENRESULT NEURONCOMPONENTS_API LoadFromFileSystem(void* FileSystem) override;
    GENRESULT NEURONCOMPONENTS_API SaveProperties(void* FileSystem) override;
    GENRESULT NEURONCOMPONENTS_API ApplyVerify(void* Context, void* Extra) override;
    GENRESULT NEURONCOMPONENTS_API ResolveTextures(float Delta) override;
    GENRESULT NEURONCOMPONENTS_API Bind(void* Context) override;
    GENRESULT NEURONCOMPONENTS_API Teardown(void* Context) override;
    GENRESULT NEURONCOMPONENTS_API Submit(void* Context, unsigned long PrimType, unsigned long Start,
                               unsigned long Count, unsigned long Zero, unsigned long Field,
                               unsigned long Flags) override;
    GENRESULT NEURONCOMPONENTS_API SetName(const char* Name) override;
    GENRESULT NEURONCOMPONENTS_API GetName(char* Out, unsigned long Size) override;
    GENRESULT NEURONCOMPONENTS_API CopyClassName(char* Out, unsigned long Size) override;
    GENRESULT NEURONCOMPONENTS_API GetPassCount(int* Out) override;
    GENRESULT NEURONCOMPONENTS_API GetRenderFlags(unsigned long* Out) override;
    GENRESULT NEURONCOMPONENTS_API GetParameterCount(unsigned long* OutCount) override;
    GENRESULT NEURONCOMPONENTS_API GetParameterDescriptor(unsigned long Index, void* OutDesc) override;
    GENRESULT NEURONCOMPONENTS_API GetParameterValue(unsigned long Index, void* InOutDesc) override;
    GENRESULT NEURONCOMPONENTS_API SetParameterValue(unsigned long Index, void* Value) override;
    const char* __thiscall ClassName() override;
    void* __thiscall ScalarDeletingDestructor(unsigned char bDelete) override;
    GENRESULT __thiscall InitializeFromDescriptor(void* Arg) override;
    GENRESULT __thiscall ApplyInitialRenderStates(void* Arg) override;
    GENRESULT __thiscall Setup() override;
    bool __thiscall ValueWithinDeclaredRange(unsigned long Index, void* Entry, void* Value) override;
    GENRESULT __thiscall StoreParameterValue(unsigned long Index, void* Entry, void* Value) override;
    unsigned long __thiscall GetParameterTable(void** OutBase) override;
    void* __thiscall GetParameterAddress(unsigned long Index) override;
    GENRESULT NEURONCOMPONENTS_API InnerQueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API InnerAddRef() override;
    unsigned long NEURONCOMPONENTS_API InnerRelease() override;
    void ConfigureAggregation(FMaterialAggregateDesc* Desc);
    GENRESULT InitializeFromDescriptor(FMaterialAggregateDesc* Desc);
    FBatchedFace Batched; // +4  QI 1.11_IBatchedMaterial (vptr only)
    unsigned long Field8; // +8  the branch / the routine default -1
    INeuronComponent* InnerController; // +C retained nondelegating factory controller
    IMaterial* Inner;     // +10 the branch
    void* Batcher;        // +14 Initialize QI IMaterialBatcher
    unsigned long RefCount;
    INeuronComponent* Outer;

    FBatchedController Controller;
    IMaterial* InnerMaterial() const;
};
