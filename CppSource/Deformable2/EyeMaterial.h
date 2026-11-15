// Author: Giperion.

#pragma once

// EyeMaterial uses a shared material base and an aggregated inner unknown.

#include "Deformable2.h"

// Values retain their tagged DWORD representation.
// String cells use the process heap; component cells hold references.
struct FMaterialParamValue
{
    int Type;                // +0x00
    unsigned long Value[4];  // +0x04

    // per cell / the inline pattern at 065fe373: Type = 0, Value[0] = 0 (the other
    // three dwords are left as they were).
    FMaterialParamValue();
    // ..e391 / 065fe6cb..e6ee: default construction, then Clear() when the wanted type
    // is not 0, then Type = InType. Value[0] is 0 either way.
    explicit FMaterialParamValue(int InType);
    // at every scope exit (065fe52e, 065fe8c2, ...), and per cell in the routine.
    ~FMaterialParamValue();

    // (thiscall RET 4); inlined twice in the branch.
    FMaterialParamValue& operator=(const FMaterialParamValue& Src);
    FMaterialParamValue(const FMaterialParamValue&) = delete;

    // 5/7 HeapFree Value[0]; 6 Release; Value[0] = 0 in every case.
    void Clear();
    // (thiscall RET 4): HeapFree Value[0], then a HeapAlloc copy of Src (null Src
    // leaves 0).
    void DupString(const char* Src);
    // (thiscall RET 4): Value[0] = NewCom, AddRef it, Release the old one.
    void SwapCom(INeuronComponent* NewCom);
    // (thiscall RET 8: Type, Arg). Arg is the value itself for 1/8/9/10, a pointer
    // to the value for 2/3/4, the string for 5/7 and the object for 6.
    void Set(int InType, unsigned long Arg);
};

static_assert(sizeof(FMaterialParamValue) == 0x14, "material parameter cell is 0x14 bytes");

// One entry of a material's parameter table (0x4C bytes, the stride of IMaterial slots
// 17/18/19 and kMaterialParamDescSize).
struct FMaterialParamDesc
{
    unsigned long Flags;      // +0x00 bit0 = skipped by Load/SaveProperties; 0x20000 / 0x40000
                              //       pick the U / V bit pair of an address-mode dword (type 9)
    const char* Key;          // +0x04 file key ("Dc", "Dt_name", ...)
    const char* Name;         // +0x08 display name
    const char* Description;  // +0x0C every EyeMaterial entry: the empty string module storage
    FMaterialParamValue Value; // +0x10 default
    FMaterialParamValue Min;   // +0x24
    FMaterialParamValue Max;   // +0x38

    // (thiscall, no args): the four header dwords and every cell's Type/Value[0].
    FMaterialParamDesc();
    // The body of the routine per entry: the header is zeroed, then the cells are cleared
    // Max, Min, Value -- member destruction order.
    ~FMaterialParamDesc();
    FMaterialParamDesc(const FMaterialParamDesc&) = delete;
    FMaterialParamDesc& operator=(const FMaterialParamDesc&) = delete;

    void Init(int Type, unsigned long InFlags, const char* InKey, const char* InName,
              const char* InDescription, unsigned long Default, unsigned long InMin,
              unsigned long InMax);
};

static_assert(sizeof(FMaterialParamDesc) == 0x4C, "material parameter descriptor is 0x4C bytes");
static_assert(offsetof(FMaterialParamDesc, Value) == 0x10, "descriptor value cell +0x10");
static_assert(offsetof(FMaterialParamDesc, Min) == 0x24, "descriptor min cell +0x24");
static_assert(offsetof(FMaterialParamDesc, Max) == 0x38, "descriptor max cell +0x38");

// One texture row (0x2C bytes), one per type-7 parameter (the routine counts them).
struct FMaterialTextureRow
{
    char* Name;                 // +0x00 owned char[] from StoreParameterValue
    int Handle;                 // +0x04 ITextureLibrary2 playback group id, -1 = none
    unsigned long Frame;        // +0x08 current stage handle (+0x70 GetFrame)
    unsigned char Field0C[0x10]; // +0x0C TODO: never read or written by this module (the
                                 //       ctor at 065fe2c0 skips it)
    unsigned long WrapMode;     // +0x1C "Texture Wrap Mode", default 4
    unsigned long AddressU;     // +0x20 "Texture U Address Mode"
    unsigned long AddressV;     // +0x24 "Texture V Address Mode"
    unsigned char bSingleFrame; // +0x28 Setup: GetFrameCount == 1
    unsigned char Pad29[3];

    // ..e2dc: Name 0, Handle -1, Frame 0, +0x1C 4, +0x20 0, +0x24 0, +0x28 0.
    FMaterialTextureRow();
    // Rows release their owned name buffers in reverse array order.
    ~FMaterialTextureRow();
    FMaterialTextureRow(const FMaterialTextureRow&) = delete;
    FMaterialTextureRow& operator=(const FMaterialTextureRow&) = delete;
};

static_assert(sizeof(FMaterialTextureRow) == 0x2C, "texture row is 0x2C bytes");
static_assert(offsetof(FMaterialTextureRow, Name) == 0x00, "row name +0x00");
static_assert(offsetof(FMaterialTextureRow, WrapMode) == 0x1C, "row wrap mode +0x1C");
static_assert(offsetof(FMaterialTextureRow, AddressU) == 0x20, "row address U +0x20");
static_assert(offsetof(FMaterialTextureRow, AddressV) == 0x24, "row address V +0x24");
static_assert(offsetof(FMaterialTextureRow, bSingleFrame) == 0x28, "row single-frame +0x28");

// RGBA colours and shininess share the D3DMATERIAL8 storage layout.
struct FMaterialColors
{
    float Diffuse[4];   // +0x00
    float Ambient[4];   // +0x10
    float Specular[4];  // +0x20
    float Emissive[4];  // +0x30
    float Power;        // +0x40
};

static_assert(sizeof(FMaterialColors) == 0x44, "D3DMATERIAL8 is 0x44 bytes");
static_assert(offsetof(FMaterialColors, Diffuse) == 0x00, "material diffuse +0x00");
static_assert(offsetof(FMaterialColors, Ambient) == 0x10, "material ambient +0x10");
static_assert(offsetof(FMaterialColors, Specular) == 0x20, "material specular +0x20");
static_assert(offsetof(FMaterialColors, Emissive) == 0x30, "material emissive +0x30");
static_assert(offsetof(FMaterialColors, Power) == 0x40, "material power +0x40");

// baseline vtable 06601598.
class FEyeMaterialImpl : public IMaterial
{
public:
    FEyeMaterialImpl();
    virtual ~FEyeMaterialImpl();

    GENRESULT NEURONCOMPONENTS_API Initialize(INeuronComponent* Outer) override;
    GENRESULT NEURONCOMPONENTS_API LoadFromFileSystem(void* FileSystem) override;
    GENRESULT NEURONCOMPONENTS_API SaveProperties(void* FileSystem) override;
    GENRESULT NEURONCOMPONENTS_API ApplyVerify(void* Context, void* Extra) override;
    GENRESULT NEURONCOMPONENTS_API ResolveTextures(float Delta) override;
    GENRESULT NEURONCOMPONENTS_API Bind(void* Context) override;
    GENRESULT NEURONCOMPONENTS_API Teardown(void* Context) override;
    GENRESULT NEURONCOMPONENTS_API Submit(void* Context, unsigned long PrimType, unsigned long Start,
                               unsigned long Count, unsigned long IbStart,
                               unsigned long IndexSpan, unsigned long Flags) override;
    GENRESULT NEURONCOMPONENTS_API SetName(const char* InName) override;
    GENRESULT NEURONCOMPONENTS_API GetName(char* Out, unsigned long Size) override;
    GENRESULT NEURONCOMPONENTS_API CopyClassName(char* Out, unsigned long Size) override;
    GENRESULT NEURONCOMPONENTS_API GetPassCount(int* Out) override;
    GENRESULT NEURONCOMPONENTS_API GetParameterCount(unsigned long* OutCount) override;
    GENRESULT NEURONCOMPONENTS_API GetParameterDescriptor(unsigned long Index, void* OutDesc) override;
    GENRESULT NEURONCOMPONENTS_API GetParameterValue(unsigned long Index, void* InOutDesc) override;
    GENRESULT NEURONCOMPONENTS_API SetParameterValue(unsigned long Index, void* Value) override;
    void* __thiscall ScalarDeletingDestructor(unsigned char bDelete) override;
    GENRESULT __thiscall InitializeFromDescriptor(void* Arg) override;
    GENRESULT __thiscall ApplyInitialRenderStates(void* Arg) override;
    GENRESULT __thiscall Setup() override;
    bool __thiscall ValueWithinDeclaredRange(unsigned long Index, void* Entry,
                                             void* Value) override;
    GENRESULT __thiscall StoreParameterValue(unsigned long Index, void* Entry,
                                             void* Value) override;
    unsigned long __thiscall GetParameterTable(void** OutBase) override;
    void* __thiscall GetParameterAddress(unsigned long Index) override;

    IRenderPipeline* Pipeline;          // +0x04
    ITextureLibrary2* TextureLib;         // +0x08
    IVertexBufferManager* VertexBufferMgr; // +0x0C
    IRPDraw* Draw;                        // +0x10
    char* Name;                           // +0x14
    FMaterialTextureRow* TextureRows;     // +0x18 new[] block (count cookie at -4)
    unsigned long TextureRowCount;        // +0x1C
    FMaterialColors Colors;               // +0x20
    unsigned char bTranslucent;           // +0x64 GetRenderFlags: 0xA : 8
    unsigned char bOneBitAlpha;           // +0x65 texture format has 1 alpha bit
    unsigned char Pad66[2];

protected:
    // The tail of the branch (065ff3bb..f46d) and, inlined a second time, of the routine
    // behind the "Oc" key (065ff675..f721). Returns the value Setup leaves in EAX.
    GENRESULT UpdateTranslucency();
};

// The inner (non-delegating) unknown at +0x6C. Its vtable is 0660156c (IMaterial slots
// 29..31 of 066014f8): the branch / the branch / the branch.
class FEyeMaterial;

struct FEyeMaterialInner : INeuronComponent
{
    unsigned long RefCount;  // +0x04 (object +0x70) = 1 at construction
    FEyeMaterial* Owner;     // +0x08 (object +0x74)

    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;
};

static_assert(sizeof(FEyeMaterialInner) == 0xC, "EyeMaterial inner unknown is 0xC bytes");

// baseline vtable 066014f8.
class FEyeMaterial : public FEyeMaterialImpl
{
public:
    FEyeMaterial();

    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;
    GENRESULT NEURONCOMPONENTS_API Bind(void* Context) override;
    GENRESULT NEURONCOMPONENTS_API Teardown(void* Context) override;
    GENRESULT NEURONCOMPONENTS_API GetRenderFlags(unsigned long* Out) override;
    const char* __thiscall ClassName() override;
    unsigned long __thiscall GetParameterTable(void** OutBase) override;
    void* __thiscall GetParameterAddress(unsigned long Index) override;
    GENRESULT NEURONCOMPONENTS_API InnerQueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API InnerAddRef() override;
    unsigned long NEURONCOMPONENTS_API InnerRelease() override;

    INeuronComponent* Controlling;  // +0x68 desc+8, or &Inner when that is null
    FEyeMaterialInner Inner;    // +0x6C
};

static_assert(sizeof(FEyeMaterial) == 0x78, "EyeMaterial is operator new(0x78)");
static_assert(offsetof(FEyeMaterial, Pipeline) == 0x04, "EyeMaterial pipeline +0x04");
static_assert(offsetof(FEyeMaterial, TextureLib) == 0x08, "EyeMaterial texture library +0x08");
static_assert(offsetof(FEyeMaterial, VertexBufferMgr) == 0x0C, "EyeMaterial VB manager +0x0C");
static_assert(offsetof(FEyeMaterial, Draw) == 0x10, "EyeMaterial IRPDraw +0x10");
static_assert(offsetof(FEyeMaterial, Name) == 0x14, "EyeMaterial name +0x14");
static_assert(offsetof(FEyeMaterial, TextureRows) == 0x18, "EyeMaterial texture rows +0x18");
static_assert(offsetof(FEyeMaterial, TextureRowCount) == 0x1C, "EyeMaterial row count +0x1C");
static_assert(offsetof(FEyeMaterial, Colors) == 0x20, "EyeMaterial D3DMATERIAL8 +0x20");
static_assert(offsetof(FEyeMaterial, bTranslucent) == 0x64, "EyeMaterial translucent +0x64");
static_assert(offsetof(FEyeMaterial, bOneBitAlpha) == 0x65, "EyeMaterial 1-bit alpha +0x65");
static_assert(offsetof(FEyeMaterial, Controlling) == 0x68, "EyeMaterial controlling +0x68");
static_assert(offsetof(FEyeMaterial, Inner) == 0x6C, "EyeMaterial inner +0x6C");

bool RegisterEyeMaterialFactory();
