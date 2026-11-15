// Author: Giperion.

#pragma once

// SceneRuntime.dll — sceneruntime / .txt
// Interface methods are __stdcall (this on the stack). C helpers are __cdecl.

#include "neuroncomponents.h"
#include "luaprofile.h"
#include "engine/sceneruntime.h"
#include "x86math.h"

#include <cstddef>
#include <list>
#include <map>
#include <set>
#include <string>
#include <vector>

static const GENRESULT GR_E_FAIL = (GENRESULT)0x80004005;


static const char* const G_NameEngine = "Engine";
static const char* const G_NameAnimation = "Animation";
static const char* const G_NameHardpoint = "Hardpoint";
static const char* const G_NameChannelArchetype = "ChannelArchetype";
static const char* const G_NameSearchPath = "SearchPath";
static const char* const G_NameFileSystem = "FileSystem";
static const char* const G_NameHardpointsFolder = "Hardpoints";
static const char* const G_NameCmpnd = "Cmpnd";
static const char* const G_NameRoot = "Root";
static const char* const G_NamePartStar = "Part*";
static const char* const G_NameCons = "Cons";
static const char* const G_NameStar = "*";
static const char* const G_NameFix = "Fix";
static const char* const G_NamePris = "Pris";
static const char* const G_NameRev = "Rev";
static const char* const G_NameCyl = "Cyl";
static const char* const G_NameSphere = "Sphere";
static const char* const G_NameTrans = "Trans";
static const char* const G_NameLoose = "Loose";
static const char* const G_NamePosition = "Position";
static const char* const G_NameOrientation = "Orientation";
static const char* const G_NameAxis = "Axis";
static const char* const G_NameMin = "Min";
static const char* const G_NameMax = "Max";
static const char* const G_NameFixed = "Fixed";
static const char* const G_NameRevolute = "Revolute";
static const char* const G_NamePrismatic = "Prismatic";
static const char* const G_NameObjectName = "Object name";
static const char* const G_NameFileName = "File name";
static const char* const G_Name3DMathEngine = "3DMathEngine";
static const char* const G_NameScript = "Script";
static const char* const G_NameParentDot = "..";
static const char* const G_NameFrames = "Frames";
static const char* const G_NameChannel = "Channel";
static const char* const G_NameHeader = "Header";
// "DOS": the implementation name ChannelArchetype::Load (0661d938) puts at +8 of
// its FileSystem open descriptor.
static const char* const G_NameDos = "DOS";
static const char* const G_NameJointMap = "Joint map";
static const char* const G_NameObjectMap = "Object map";
static const char* const G_NameEventMap = "Event map";
static const char* const G_NameParentName = "Parent name";
static const char* const G_NameChildName = "Child name";
static const char* const G_NameRootHeight = "Root height";

// ChannelArchetype Header flags (the branch stride).
static const unsigned long G_ChannelFlagScalar = 0x1;
static const unsigned long G_ChannelFlagPosition = 0x2;
static const unsigned long G_ChannelFlagQuat = 0x4;
static const unsigned long G_ChannelFlagScalarOnly = 0x8;
static const unsigned long G_ChannelFlagZeroPos = 0x10;
static const unsigned long G_ChannelFlagIdentQuat = 0x20;
static const unsigned long G_ChannelFlagCompQuat3 = 0x40;
static const unsigned long G_ChannelFlagCompAxis = 0x80;
static const unsigned long G_ChannelFlagCompQuat4 = 0x100;
static const unsigned long G_ChannelCompressMask = 0x1F0;
static const unsigned long G_MixKindScalar = 1;
static const unsigned long G_MixKindVec3 = 3;
static const unsigned long G_MixKindQuat = 4;
static const unsigned long G_MixKindPosQuat = 7;
static const float G_ShortToUnit = 3.051851e-05f;
static const unsigned long G_MixerTypeLeaf = 0;
static const unsigned long G_MixerTypeBlend = 1;
static const unsigned long G_MixerTypeOverlay = 2;
static const unsigned long G_MixerFlagKeep = 0x1;
static const unsigned long G_MixerFlagPose = 0x2;
static const unsigned long G_ChannelPlayFlag = 0x1;
static const unsigned long G_ChannelBlendFlag = 0x2;
static const unsigned long G_ChannelHoldFlag = 0x4;

// 06616d32: the first playback id IAnimation2::PlayScript hands out.
static const unsigned long G_AnimFirstPlaybackId = 0xED;

static const unsigned long G_EngineInstanceSize = 0xEC;
static const unsigned long G_TransformObjectSize = 0x48;
static const unsigned long G_InstanceFlagOwnedTransform = 2;
static const unsigned long G_AggDescSizeMath = 0xC;

static const unsigned long G_PriorityEngine = 0x40000000;
static const unsigned long G_PriorityAnimation = 0x40000000;
static const unsigned long G_PriorityHardpoint = 0x80000000;
static const unsigned long G_PriorityChannelArchetype = 0x80000000;

static const unsigned long G_AggDescSizeEngine = 0x8;
static const unsigned long G_AggDescSizeSearchPath = 0x8;
static const unsigned long G_AggDescSizeChannelArchetype = 0x8;
static const unsigned long G_AggDescSizeAnimation = 0x18;
static const unsigned long G_AggDescSizeHardpoint = 0x18;

static const unsigned long G_EngineObjectSize = 0x64;
static const unsigned long G_AnimationObjectSize = 0x60;
static const unsigned long G_HardpointObjectSize = 0x44;
static const unsigned long G_ChannelArchetypeObjectSize = 0x20;

struct FAggDesc8
{
    unsigned long Size;
    const char* Name;
};

// load_engine_components stack @ 0661fac5–0661fad9 + factory the routine / the routine.
struct FAggDesc18
{
    unsigned long Size;
    const char* Name;
    void* Inner;
    void** Outer;
    const char* Value;
    void* SystemContainer;
};

struct FNeuronComponentsFactory : IComponentFactory
{
    unsigned long RefCount;
    const char* Name;

    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;
};

// Nondelegating inner unknowns own the aggregate lifetime.
struct FAnimationObject;
struct FHardpointObject;

struct FAnimationInner : INeuronComponent
{
    unsigned long RefCount;      // +0x04
    FAnimationObject* Object;    // +0x08

    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;  
    unsigned long NEURONCOMPONENTS_API AddRef() override;                                 
    unsigned long NEURONCOMPONENTS_API Release() override;                                
};

struct FHardpointInner : INeuronComponent
{
    unsigned long RefCount;      // +0x04
    FHardpointObject* Object;    // +0x08

    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;  
    unsigned long NEURONCOMPONENTS_API AddRef() override;                                 
    unsigned long NEURONCOMPONENTS_API Release() override;                                
};

// Node 0x3C: circular list. QI target / IEngineComponent* at +0x38 (Engine QI, pass 2/3).
struct FEngineChildNode
{
    FEngineChildNode* Next;
    FEngineChildNode* Prev;
    char Name[0x30];
    // Shared node: Object holds an inner unknown or an EngineComponent facet.
    INeuronComponent* Object;
};

inline IEngineComponent* AsEngineComponent(INeuronComponent* Object)
{
    return (IEngineComponent*)Object;
}

// Caller name wrapper (create_compound / slot 12): Text @ +0, CRC @ +4. the routine reads +4.
using FArchetypeName = FEngineName;

// Compound child value; its archetype handle is managed by the engine.
struct FArchetypeChild
{
    char Name[0x40];
    unsigned long Crc;
    struct FArchetypeHandle* Handle;
};

// Handle +0x2C vector element, stride 0x64, appended by insert(end, 1, record) (the routine
// from 06621072). +0 child part, +4 parent part, +8 type; UTF remap (not linear).
struct FConstraintRecord
{
    FArchetypeChild* ChildPart;
    FArchetypeChild* ParentPart;
    unsigned long Payload[G_EngineJointPayloadDwords];  // 0x17, engine/sceneruntime.h
};

// Pooled archetype handle; destroy members before returning storage.
struct FArchetypeHandle
{
    std::string Name;                          // +0x00
    unsigned long Crc;                         // +0x10
    unsigned long RefCount;                    // +0x14
    std::vector<FArchetypeChild> Children;     // +0x18
    std::vector<FConstraintRecord> Constraints; // +0x28

    explicit FArchetypeHandle(const FArchetypeName* Source); 
};

// Archetype handles indexed by name CRC.
typedef std::map<unsigned long, FArchetypeHandle*> FArchetypeMap;

// Cached blocks are uninitialized; the first word links free entries.
struct FSceneRuntimeFreeList
{
    unsigned long ElementSize;
    void* FreeHead;
    long FreeLimit;
    long FreeCount;
};

struct FEngineInstance;

// Root instances indexed by signed pointer keys.
typedef std::map<long, FEngineInstance*> FEngineInstanceMap;

// Owns a lowercase name and the hardpoint's 0x48-byte payload.
struct FHardpointRecord
{
    unsigned long Crc;
    char* Name;
    unsigned char Data[0x48];

    FHardpointRecord(unsigned long InCrc, char const* InName);
    FHardpointRecord(FHardpointRecord const& Other);
    ~FHardpointRecord();
    FHardpointRecord& operator=(FHardpointRecord const& Other) = delete;
};

// baseline keeps one std::list of records per archetype handle, in a std::map keyed by the
// handle as a signed int (the routine compares with JGE; the dtor prints it with %d).
typedef std::list<FHardpointRecord> FHardpointList;
typedef std::map<long, FHardpointList> FHardpointMap;

// Engine-owned transform; only User is initialized by the constructor.
struct FInstanceTransform : IInstanceTransform
{
    FInstanceTransform() { User = 0; }   // ; every other member keeps the block's bytes

    GENRESULT NEURONCOMPONENTS_API Init(void* Instance) override;                                  
    GENRESULT NEURONCOMPONENTS_API AddRef(void* Instance) override;                                
    GENRESULT NEURONCOMPONENTS_API Release(void* Instance) override;                               
    GENRESULT NEURONCOMPONENTS_API SetPosition(void* Instance, const float* Position) override;     
    float* NEURONCOMPONENTS_API GetPosition(void* Instance) override;                              
    GENRESULT NEURONCOMPONENTS_API SetOrientation(void* Instance, const float* Rows) override;      
    float* NEURONCOMPONENTS_API GetOrientation(void* Instance) override;                           
    GENRESULT NEURONCOMPONENTS_API SetMatrix(void* Instance, const float* Matrix) override;         
    float* NEURONCOMPONENTS_API GetMatrix(void* Instance) override;                                
    GENRESULT NEURONCOMPONENTS_API GetExtra(void* Instance, float* OutExtra, float* OutVec) override; 
    GENRESULT NEURONCOMPONENTS_API SetExtra(void* Instance, float Extra, const float* Vec) override; 
    GENRESULT NEURONCOMPONENTS_API SetUser(void* Instance, unsigned long User) override;            
    unsigned long NEURONCOMPONENTS_API GetUser(void* Instance) override;                           
    unsigned long NEURONCOMPONENTS_API MarkWorldDirty(void* Instance) override; // formerly Slot13

    float Matrix[12];        // +0x04
    float Extra;             // +0x34
    float ExtraVec[3];       // +0x38
    unsigned long User;      // +0x44
};

// / pool the field size 0xEC.
// Parent list: +0x10 / +0x14 / +0x18; children +0x1C / +0x20 / +0x24 (the routine).
// Part record @ +0xE8 (the routine). 3x4 identity @ +0x2C.
struct FEngineInstance
{
    unsigned long Flags;
    IInstanceTransform* Transform;   // +0x04: owned FInstanceTransform (flag 2) or a caller's
    FArchetypeHandle* Archetype;
    FArchetypeHandle* ParentArchetype;
    FEngineInstance* Parent;
    FEngineInstance* NextSibling;
    FEngineInstance* PrevSibling;
    FEngineInstance* FirstChild;
    FEngineInstance* LastChild;
    unsigned long ChildCount;
    void* UserData;
    float Matrix[12];
    // type[0], rest pos[1..3], T[4..6], 3x3[7..15], axis[16..18].
    unsigned long Relative[G_EngineJointPayloadDwords];  // 0x17, engine/sceneruntime.h
    float JointState[7];
    unsigned char bWorldDirty;
    unsigned char PadD5[3];
    float Extent;
    float ExtentVec[3];
    FArchetypeChild* PartChild;
};

struct FAggDescC
{
    unsigned long Size;
    const char* Name;
    unsigned long Extra;
};

// Engine component lists, archetype map and root-instance cache.
struct FEngineObject : IEngine
{
    INeuronComponent* SearchPath;                       // baseline +0x04
    unsigned long InstanceLookupCount;              // baseline +0x08
    unsigned long InstanceCacheHits;                // baseline +0x0C
    FEngineInstanceMap Instances;                   // baseline +0x10
    FEngineInstanceMap::iterator CachedInstance;    // baseline +0x28
    unsigned char bInstanceCacheValid;              // baseline +0x2C
    FArchetypeMap Archetypes;                       // baseline +0x30
    unsigned char bInitialized;                     // baseline +0x44
    FEngineChildNode* ComponentList;                // baseline +0x4C (list +0x48)
    unsigned long ComponentCount;                   // baseline +0x50
    FEngineChildNode* EngineComponentList;          // baseline +0x58 (list +0x54)
    unsigned long EngineComponentCount;             // baseline +0x5C
    unsigned long RefCount;                         // baseline +0x60

    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;
    GENRESULT NEURONCOMPONENTS_API LoadEngineComponents(INeuronComponent* SystemContainer) override;
    unsigned long NEURONCOMPONENTS_API EnumerateEngineComponents(
        int(__cdecl* Callback)(const char*, void*)) override;
    GENRESULT NEURONCOMPONENTS_API NotifyEngineComponents(void* Arg) override;
    GENRESULT NEURONCOMPONENTS_API SetSearchPath(const char* Path) override;
    GENRESULT NEURONCOMPONENTS_API QuerySearchPath(char* Dest, unsigned long MaxChars) override;
    GENRESULT NEURONCOMPONENTS_API SetFilesystem(INeuronComponent* NewPath) override;
    GENRESULT NEURONCOMPONENTS_API GetSearchPath(void** Out) override;
    GENRESULT NEURONCOMPONENTS_API CreateFileSystem(const char* Path, void** Out, void* Factory) override;
    void* NEURONCOMPONENTS_API CreateCompoundArchetype(FEngineName* Name) override;
    void* NEURONCOMPONENTS_API CreateArchetypeHandle(FEngineName* Name, void* FileSystem) override;
    void* NEURONCOMPONENTS_API DuplicateArchetype(void* ArchIn, FEngineName* Name) override;
    unsigned long NEURONCOMPONENTS_API AddRefHandle(void* Handle) override;
    unsigned long NEURONCOMPONENTS_API ReleaseHandle(void* Handle) override;
    FEngineName* NEURONCOMPONENTS_API GetArchetypeName(FEngineName* Out, void* Handle) override;
    void* NEURONCOMPONENTS_API FindArchetype(FEngineName* Name) override;
    GENRESULT NEURONCOMPONENTS_API QueryEngineComponent(int Id, void* Name, void** Out) override;
    unsigned long NEURONCOMPONENTS_API IsCompoundArchetype(void* Handle) override;
    GENRESULT NEURONCOMPONENTS_API EnumerateArchetypeConstraints(
        void* Handle, int(__cdecl* Callback)(void*, void*, void*, void*),
        void* Context) override;
    GENRESULT NEURONCOMPONENTS_API EnumerateArchetypeChildren(
        void* Handle, int(__cdecl* Callback)(void*, void*, int, void*),
        void* Context) override;
    void* NEURONCOMPONENTS_API CreateInstanceFromName(FEngineName* Name, void* FileSystem,
                                             void* Transform) override;
    void* NEURONCOMPONENTS_API CreateInstance(void* Handle, void* Transform) override;
    GENRESULT NEURONCOMPONENTS_API DestroyInstance(void* Instance) override;
    void* NEURONCOMPONENTS_API GetInstanceArchetype(void* Instance) override;
    FEngineName* NEURONCOMPONENTS_API GetInstanceName(FEngineName* Out, void* Instance) override;
    GENRESULT NEURONCOMPONENTS_API UpdateInstance(void* Instance, unsigned long Flags, void* Arg) override;
    GENRESULT NEURONCOMPONENTS_API SetInstanceTransform(void* Instance, void* Transform) override;
    GENRESULT NEURONCOMPONENTS_API GetInstanceTransformObject(void* Instance, void** Out) override;
    GENRESULT NEURONCOMPONENTS_API QueryInstance(void* Instance, const char* Iid, void** Out) override;
    GENRESULT NEURONCOMPONENTS_API SetInstanceUserData(void* Instance, void* Value) override;
    void* NEURONCOMPONENTS_API GetInstanceUserData(void* Instance) override;
    GENRESULT NEURONCOMPONENTS_API SetInstanceExtent(void* Instance, unsigned long Flags, float Extent,
                                          const float* Vec) override;
    GENRESULT NEURONCOMPONENTS_API GetInstanceExtent(void* Instance, unsigned long Flags, float* OutExtent,
                                          float* OutVec) override;
    GENRESULT NEURONCOMPONENTS_API SetInstancePosition(void* Instance, void* Position) override;
    float* NEURONCOMPONENTS_API GetInstancePosition(void* Instance) override;
    GENRESULT NEURONCOMPONENTS_API SetInstanceOrientation(void* Instance, void* Orient) override;
    float* NEURONCOMPONENTS_API GetInstanceOrientation(void* Instance) override;
    GENRESULT NEURONCOMPONENTS_API SetInstanceMatrix(void* Instance, void* Matrix) override;
    float* NEURONCOMPONENTS_API GetInstanceTransform(void* Instance) override;
    GENRESULT NEURONCOMPONENTS_API NoOp41(void* A, void* B) override; // formerly UnusedSlot41; TODO: original operation purpose.
    float* NEURONCOMPONENTS_API GetIdentityMatrix(void* Unused) override;
    GENRESULT NEURONCOMPONENTS_API NoOp43(void* A, void* B) override; // formerly UnusedSlot43; TODO: original operation purpose.
    float* NEURONCOMPONENTS_API GetIdentityMatrix2(void* Unused) override;
    GENRESULT NEURONCOMPONENTS_API SetTransformUser(void* Instance, unsigned long User) override;
    unsigned long NEURONCOMPONENTS_API GetTransformUser(void* Instance) override;
    unsigned long NEURONCOMPONENTS_API CreateJoint(void* Parent, void* Child,
                                         const unsigned long* Payload) override;
    unsigned long NEURONCOMPONENTS_API DestroyJoint(void* Parent, void* Child) override;
    unsigned long* NEURONCOMPONENTS_API GetInstanceRelative(void* Instance) override;
    unsigned long NEURONCOMPONENTS_API SetJointParams(void* Instance, unsigned long Index,
                                            const float* Values) override;
    unsigned long NEURONCOMPONENTS_API GetJointParams(void* Instance, unsigned long Index,
                                            float* Out) override;
    void* NEURONCOMPONENTS_API FindMappedInstance(void* Instance) override;
    void* NEURONCOMPONENTS_API GetInstanceParent(void* Instance) override;
    void* NEURONCOMPONENTS_API GetRelatedInstance(void* From, unsigned long Flags, void* Cursor) override;
    void* NEURONCOMPONENTS_API GetInstanceRoot(void* Instance) override;
    void* NEURONCOMPONENTS_API FindArchetypeChild(void* Handle, void* ChildHandle) override;
    unsigned long NEURONCOMPONENTS_API RejectOperation(void* A, void* B) override; // formerly UnusedSlot57; TODO: original operation purpose.
};

// Track copies retain the channel and duplicate names; destruction releases them.
struct FAnimTrack
{
    struct FChannelArchetypeObject* Channel = nullptr; // +0x00
    unsigned long ParentCrc = 0;                       // +0x04
    unsigned long ChildCrc = 0;                        // +0x08
    char* ParentName = nullptr;                        // +0x0C (malloc'd by the routine)
    char* ChildName = nullptr;                         // +0x10

    FAnimTrack() = default;
    FAnimTrack(const FAnimTrack& Other);    
    ~FAnimTrack();                          
    FAnimTrack& operator=(const FAnimTrack&) = delete;
};

// Script animation value owns tracks and retains its longest channel.
struct FAnimNameEntry
{
    // baseline value +0: the channel archetype with the longest duration, AddRef'd; the previous
    // holder is Released (066117b1..e4). Read by GetNamedDuration (066147c0) and
    // GetScriptDuration (06615160) through IChannelArchetype::GetDuration.
    struct FChannelArchetypeObject* LongestChannel = nullptr;
    // baseline value +4: set to 1 once a "Root height" file was read (06611568).
    unsigned long bHasRootHeight = 0;
    // baseline value +8: the "Root height" float (06611551..59), read back by
    // GetNamedDurationOut (06615076..7d) and GetDurationIfNamed (06615264..6b).
    float RootHeight = 0.0f;
    char* Name = nullptr;                    // baseline value +0xC (_strdup, 0661146e)
    std::vector<FAnimTrack> Tracks;          // baseline value +0x10 vector, in file order

    FAnimNameEntry() = default;
    ~FAnimNameEntry();                       // (SceneRuntime.cpp)
    FAnimNameEntry(const FAnimNameEntry&) = delete;
    FAnimNameEntry& operator=(const FAnimNameEntry&) = delete;
};

// The script's name map, keyed by CRC32 of the name and compared unsigned (JC at 06614813 /
// / 06616bdf); every walk over it (EnumScriptNames 06614cf0, GetFirstInstanceField
// ) visits the keys in ascending order, as std::map does.
using FAnimNameMap = std::map<unsigned long, FAnimNameEntry>;

// Private reference-counted script handle; public callers use it opaquely.
class FAnimScript
{
public:
    // (vtable +0x00): OR EAX,-1 / RET 0xC -- two arguments ignored, answers -1.
    // The same body is also referenced from 06629714 (ICF fold); its name is not recovered.
    virtual unsigned long NEURONCOMPONENTS_API RejectOperation(void* ArgA, void* ArgB); // formerly Slot00
    // (vtable +0x04).
    virtual unsigned long NEURONCOMPONENTS_API AddRef();
    // (vtable +0x08); deletes the script at zero.
    virtual unsigned long NEURONCOMPONENTS_API Release();

    FAnimNameMap Names;                      // baseline +0x04..+0x17
    unsigned long RefCount = 1;              // baseline +0x18

protected:
    // Only Release destroys a script (the inlined destructor at 06612d55..dd: vtable reset,
    // map teardown, delete).
    ~FAnimScript() = default;
};

// Bind descriptor: kind, sink, context token and target instance.
struct FAnimBindOut
{
    unsigned long Kind;
    void* Extra;
    void* Typed;
    FEngineInstance* Instance;
};

// Constructors leave unspecified members uninitialized; release the archetype before freeing.
struct FTypedChannel
{
    explicit FTypedChannel(IChannelOwner* NewOwner); 
    explicit FTypedChannel(struct FChannelArchetypeObject* NewArchetype);    

    virtual unsigned long Interpolate(float* Out, const float* Left, const float* Right, double Alpha);
    virtual unsigned long BindHeader(unsigned long* Header);
    virtual unsigned long Apply(const float* Left, const float* Right, double Alpha);
    virtual unsigned long InterpolatePose(float* Out, const float* Left, const float* Right, double Alpha);
    virtual unsigned long SetTime(float Time);
    virtual unsigned long GetType();
    virtual unsigned long CopySampled(float* Out);
    virtual unsigned long BindMap(unsigned long* Map, void* Bind);
    virtual unsigned long SetOverlayPos(const float* Pos);
    virtual unsigned long SetOverlayExtra(const float* Extra);
    virtual unsigned long AddOverlayPos(const float* Delta);
    virtual float* GetSampled();
    virtual unsigned long TouchMapped(void* ArgA, void* ArgB);
    virtual unsigned long UpdateDeltaRate(float Dt); // formerly Slot13
    virtual unsigned long Step(float* Out, float Dt);

    IChannelOwner* Owner;                            // +0x04
    unsigned long Kind;                              // +0x08
    unsigned long Field0C;                           // +0x0C
    unsigned long Field10;                           // +0x10
    unsigned long Field14;                           // +0x14
    void* Bind;                                      // +0x18
    float Time;                                      // +0x1C
    // baseline +0x20: header[1] as the routine stores it (0661af37..3a); bit 1 loop, bit 2
    // ping-pong (the routine 0661b325 / 0661b38c, the routine 0661b5a0 / 0661b71a).
    unsigned long LoopFlags;                         // +0x20
    struct FChannelArchetypeObject* Archetype;       // +0x24
    float Weight;                                    // +0x28 (header[2])
    float Scale;                                     // +0x2C (header[4])
    // TODO: name this play-header position factor when its purpose is known.
    float Field_0x30;                                // +0x30
    unsigned long PlayFlags;                         // +0x34
    // baseline +0x38, a byte: bit 0 is set from the routine's result by IChannel::Play
    // (066164a0..a7), tested by IChannel::StopGraph (06616527) and by the standalone pass of
    // (066158f8), cleared by the routine (AND byte[+0x38],0xFE).
    unsigned char PlayState;                         // +0x38
};

// Type 1, operator new 0x44, table 066291a4: one float sampled at +0x3C.
struct FScalarTypedChannel : FTypedChannel
{
    explicit FScalarTypedChannel(IChannelOwner* NewOwner);
    explicit FScalarTypedChannel(struct FChannelArchetypeObject* NewArchetype);

    unsigned long Apply(const float* Left, const float* Right, double Alpha) override;          
    unsigned long InterpolatePose(float* Out, const float* Left, const float* Right, double Alpha) override; 
    unsigned long GetType() override;                                                         
    unsigned long CopySampled(float* Out) override;                                           
    unsigned long BindMap(unsigned long* Map, void* Bind) override;                           
    float* GetSampled() override;                                                             

    float Sampled[1];                                // +0x3C
    // baseline +0x40: 0xFFFFFFFF at construction (06612080 / 06615998), the bound instance's
    // joint type (IEngine+0xC4 result [0]) after slot 7 the routine, tested `== 2` by the
    // slot 3 wrap-lerp the branch.
    unsigned long JointType;                         // +0x40
};

// Type 2, operator new 0x48, table 06629168: a three-float vector sampled at +0x3C.
struct FVectorTypedChannel : FTypedChannel
{
    explicit FVectorTypedChannel(IChannelOwner* NewOwner);
    explicit FVectorTypedChannel(struct FChannelArchetypeObject* NewArchetype);

    unsigned long Apply(const float* Left, const float* Right, double Alpha) override;          
    unsigned long InterpolatePose(float* Out, const float* Left, const float* Right, double Alpha) override; 
    unsigned long GetType() override;                                                         
    unsigned long CopySampled(float* Out) override;                                           
    float* GetSampled() override;                                                             

    float Sampled[3];                                // +0x3C
};

// Type 4, operator new 0x4C, table 0662912c: a quaternion sampled at +0x3C.
struct FQuatTypedChannel : FTypedChannel
{
    explicit FQuatTypedChannel(IChannelOwner* NewOwner);
    explicit FQuatTypedChannel(struct FChannelArchetypeObject* NewArchetype);

    unsigned long Apply(const float* Left, const float* Right, double Alpha) override;          
    unsigned long InterpolatePose(float* Out, const float* Left, const float* Right, double Alpha) override; 
    unsigned long GetType() override;                                                         
    unsigned long CopySampled(float* Out) override;                                           
    float* GetSampled() override;                                                             

    float Sampled[4];                                // +0x3C
};

// Type 6, operator new 0x94, table 066290f0: position + orientation.
struct FPosQuatTypedChannel : FTypedChannel
{
    explicit FPosQuatTypedChannel(IChannelOwner* NewOwner);
    explicit FPosQuatTypedChannel(struct FChannelArchetypeObject* NewArchetype);

    unsigned long Interpolate(float* Out, const float* Left, const float* Right, double Alpha) override;
    unsigned long BindHeader(unsigned long* Header) override;
    unsigned long Apply(const float* Left, const float* Right, double Alpha) override;
    unsigned long InterpolatePose(float* Out, const float* Left, const float* Right, double Alpha) override;
    unsigned long GetType() override;
    unsigned long CopySampled(float* Out) override;
    unsigned long SetOverlayPos(const float* Pos) override;
    unsigned long SetOverlayExtra(const float* Extra) override;
    unsigned long AddOverlayPos(const float* Delta) override;
    float* GetSampled() override;
    unsigned long TouchMapped(void* ArgA, void* ArgB) override;
    unsigned long UpdateDeltaRate(float Dt) override; // formerly Slot13
    unsigned long Step(float* Out, float Dt) override;

    float Matrix[9];                                 // +0x3C
    float Delta[3];                                  // +0x60
    float StepDelta[3];                              // +0x6C (the branch)
    float AppliedPos[3];                             // +0x78
    float AppliedQuat[4];                            // +0x84
};

// baseline sizes, on the 32-bit target the port builds for.
static_assert(sizeof(void*) != 4 || sizeof(FTypedChannel) == 0x3C, "type-8 channel 0x3C");
static_assert(sizeof(void*) != 4 || sizeof(FScalarTypedChannel) == 0x44, "type-1 channel 0x44");
static_assert(sizeof(void*) != 4 || sizeof(FVectorTypedChannel) == 0x48, "type-2 channel 0x48");
static_assert(sizeof(void*) != 4 || sizeof(FQuatTypedChannel) == 0x4C, "type-4 channel 0x4C");
static_assert(sizeof(void*) != 4 || sizeof(FPosQuatTypedChannel) == 0x94, "type-6 channel 0x94");

// &typed+0x08: the 16-byte bind descriptor. FTypedChannel keeps Kind/Field0C/Field10/Field14
// as four consecutive dwords, which is FAnimBindOut's layout on this 32-bit target.
static_assert(offsetof(FTypedChannel, Field14) - offsetof(FTypedChannel, Kind) == 0xC,
              "bind descriptor is four consecutive dwords");
static_assert(sizeof(FAnimBindOut) == 0x10, "bind descriptor is 16 bytes");
inline FAnimBindOut* TypedBindDesc(FTypedChannel* This)
{
    return (FAnimBindOut*)(&This->Kind);
}

// Live typed channels keyed by pointer, also shared by playback records.
using FTypedChannelSet = std::set<FTypedChannel*>;

// Animation mixer: bind-key and descriptor maps hold linked evaluation nodes.

struct FMixerNode;

// / the routine / the routine: 0x101 dwords on the routine's frame
// (local_404), the node pointers first and the count at +0x400. No bound check in baseline.
struct FMixerStack
{
    FMixerNode* Nodes[0x100];
    unsigned long Count;
};

// The value of the inner map (node +0x1C): head, tail, count, and at +0xC the blend
// duration the routine records (06616307) and the routine reads back (06615425).
struct FMixerList
{
    FMixerNode* Head;
    FMixerNode* Tail;
    unsigned long Count;
    float BlendDuration;
};

// Nodes have no virtual destructor; MixerFreeNode deallocates their storage directly.
struct FMixerNode
{
    // Slot 0 (+0x00), thiscall RET 4: advance by Dt; 1 means "finished".
    virtual unsigned long Tick(float Dt) = 0;
    // Slot 1 (+0x04), thiscall RET 8: write this node's value into Out, popping any inputs it
    // needs from Stack. the routine enters it with a tail jump.
    virtual void Evaluate(float* Out, FMixerStack* Stack) = 0;
    // Advance the sampled delta rate; blend and overlay nodes ignore Dt.
    virtual void UpdateDeltaRate(float Dt) = 0; // formerly Slot02
    // Sample displacement into Out and return its address.
    virtual float* SampleDisplacement(float* Out, float Dt) = 0; // formerly Slot03

    unsigned long Type;     // +0x04: 0 leaf, 1 blend, 2 overlay
    FMixerNode* Prev;       // +0x08
    FMixerNode* Next;       // +0x0C
    unsigned char bRemove;  // +0x10: the routine's third pass unlinks and frees these
};

// operator_new(0x18), vtable the branch (the routine 06616202..0661621c).
struct FMixerLeaf : FMixerNode
{
    FTypedChannel* Typed;   // +0x14

    unsigned long Tick(float Dt) override;                    
    void Evaluate(float* Out, FMixerStack* Stack) override;   
    void UpdateDeltaRate(float Dt) override; // formerly Slot02
    float* SampleDisplacement(float* Out, float Dt) override; // formerly Slot03
};

// operator_new(0x48), vtable the branch (the routine 06616284..066162ca,
// 06615417..06615459).
struct FMixerBlend : FMixerNode
{
    float Time;             // +0x14
    float Duration;         // +0x18
    float InvDuration;      // +0x1C
    // +0x20, a byte: bit 0 "drop the outgoing side when done" (the routine 06615372),
    // bit 1 "carry the root's position through the blend" (06616334..3d, 0661d21d).
    unsigned char Flags;
    float PoseStart[3];     // +0x24: IEngine+0x90 of the root when the blend was attached
    float PoseRate[3];      // +0x30: predecessor's slot 3 over the duration, times InvDuration
    float PoseCtrl[3];      // +0x3C: new leaf's slot 3 times InvDuration, minus PoseRate

    unsigned long Tick(float Dt) override;                    
    void Evaluate(float* Out, FMixerStack* Stack) override;   
    void UpdateDeltaRate(float Dt) override; // formerly Slot02
    float* SampleDisplacement(float* Out, float Dt) override; // formerly Slot03
};

// operator_new(0x34), the routine, vtable the routine: the value the target had when
// the first blend was attached, replayed as the bottom of the stack.
struct FMixerOverlay : FMixerNode
{
    unsigned long Count;    // +0x14: dwords the branch copies (1/2/3/4/7, 0 = none)
    float Values[7];        // +0x18

    explicit FMixerOverlay(const FAnimBindOut* Desc);         

    unsigned long Tick(float Dt) override;                    
    void Evaluate(float* Out, FMixerStack* Stack) override;   
    void UpdateDeltaRate(float Dt) override; // formerly Slot02
    float* SampleDisplacement(float* Out, float Dt) override; // formerly Slot03
};

// , the inner map's ordering.
struct FAnimBindLess
{
    bool operator()(const FAnimBindOut& Left, const FAnimBindOut& Right) const;
};

using FMixerDescMap = std::map<FAnimBindOut, FMixerList, FAnimBindLess>;
// baseline compares the outer key as a signed int (the routine: `(int)param_1 < (int)node[3]`).
using FMixerKeyMap = std::map<long, FMixerDescMap>;

// Playback value: track channels, root instance, longest channel and retained script.
struct FAnimPlayback
{
    FAnimNameEntry* Name;                 // +0x00: the routine 06611fa2
    FEngineInstance* Root;                // +0x04: -1 from 06611f75, the bind root from 06612503
    // +0x08: VC6 vector (allocator byte, First +0xC, Last +0x10, End +0x14) of the channels
    // created, one per track, minus the ones the routine failed to bind.
    std::vector<FTypedChannel*> Typed;
    FTypedChannel* LongestTyped;          // +0x18: the routine 066125b2, 0 when none bound
    FAnimScript* Script;                  // +0x1C: the branch 06614983
};

// baseline Animation+0x14: playback id -> record. the routine / the routine compare the key
// signed (`CMP ECX,EDX; JL` at 066145a9..ab).
using FAnimPlaybackMap = std::map<long, FAnimPlayback>;

// Animation exposes four facets sharing the aggregate's controlling unknown.
struct FAnimation2Facet : IAnimation2
{
    unsigned long NEURONCOMPONENTS_API SetOverlayPos(unsigned long PlaybackId, float* Pos) override;
    unsigned long NEURONCOMPONENTS_API SetOverlayExtra(unsigned long PlaybackId, float* Extra) override;
    unsigned long NEURONCOMPONENTS_API AddOverlayPos(unsigned long PlaybackId, float* Delta) override;
};

// IChannel @ Animation +8 (slots 3..18 of 06629370: the branch .. the branch).
struct FAnimationChannelFacet : IChannel
{
    unsigned long NEURONCOMPONENTS_API CreateTyped(void* Owner, unsigned long Type, void* BindA,
                                        void* BindB) override;
    unsigned long NEURONCOMPONENTS_API CreateFromObject(void* Object, void* BindA, void* BindB) override;
    unsigned long NEURONCOMPONENTS_API Destroy(unsigned long Key) override;
    unsigned long NEURONCOMPONENTS_API GetObject(unsigned long Key, void** Out) override;
    unsigned long NEURONCOMPONENTS_API Play(unsigned long Key, float* Time) override;
    unsigned long NEURONCOMPONENTS_API StopGraph(unsigned long Key) override;
    float NEURONCOMPONENTS_API GetDuration(unsigned long Key) override;
    unsigned long NEURONCOMPONENTS_API Blend(unsigned long Key, float Duration) override;
    GENRESULT NEURONCOMPONENTS_API SetUser(unsigned long Key, unsigned long User) override;
    float NEURONCOMPONENTS_API GetUser(unsigned long Key) override;
    GENRESULT NEURONCOMPONENTS_API SetOverlayPos(unsigned long Key, float* Pos) override;
    GENRESULT NEURONCOMPONENTS_API SetOverlayExtra(unsigned long Key, float* Extra) override;
    unsigned long NEURONCOMPONENTS_API InMixer(unsigned long Key) override;
    unsigned long* NEURONCOMPONENTS_API GetHeader(unsigned long* Out, unsigned long Key) override;
    GENRESULT NEURONCOMPONENTS_API AddOverlayPos(unsigned long Key, float* Delta) override;
    GENRESULT NEURONCOMPONENTS_API GetMapped(unsigned long Key, unsigned char* Out) override;
};

struct FAnimationObject : FAnimation2Facet, IAggregateComponent, FAnimationChannelFacet, IRootData
{
    // baseline +0x10: the next playback id. the routine seeds it with 0xED (06616d32);
    // hands out the current value and increments it (066149d3..e5).
    unsigned long NextPlaybackId;
    // baseline +0x14: the playback map (VC6 _Tree, 0x14 bytes). Held through a pointer for the
    // same reason as Channels and Mixers; allocated by AnimationFactoryCreateInstance.
    FAnimPlaybackMap* Playbacks;
    unsigned char Pad18[0x10];
    // baseline +0x28: the typed-channel set (see FTypedChannelSet). Held through a pointer for
    // the same reason as Mixers below; allocated by AnimationFactoryCreateInstance.
    FTypedChannelSet* Channels;
    unsigned char Pad2C[0x10];
    // baseline +0x3C: the mixer map itself (VC6 _Tree). Held through a pointer here so the
    // object keeps its 0x60 size under every iterator-debug level; allocated by
    // AnimationFactoryCreateInstance, released by DestroyAnimationLists.
    FMixerKeyMap* Mixers;
    unsigned char Pad40[0x10];
    INeuronComponent* Controller;    // +0x50: desc +8 (the aggregating outer) or &Inner
    FAnimationInner Inner;       // +0x54

    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;
    void* NEURONCOMPONENTS_API OpenScript(void* FileSystem) override;
    GENRESULT NEURONCOMPONENTS_API CloseScript(void* Script) override;
    float NEURONCOMPONENTS_API GetScriptDuration(void* Script) override;
    float NEURONCOMPONENTS_API GetNamedDuration(void* Script, const char* Name) override;
    GENRESULT NEURONCOMPONENTS_API EnumScriptNames(void* Callback, void* Script, void* Context) override;
    GENRESULT NEURONCOMPONENTS_API FindScriptName(void* Callback, void* Script, const char* Name,
                                        void* Context) override;
    unsigned long NEURONCOMPONENTS_API PlayScript(void* Script, void* Root, const char* Name, void* ArgD,
                                         void* ArgE) override;
    GENRESULT NEURONCOMPONENTS_API DestroyScript(void* Script) override;
    unsigned long NEURONCOMPONENTS_API EvalPlayback(unsigned long PlaybackId, unsigned long Flags, float Time,
                                         float Scale, void* A, void* B, void* C, void* D) override;
    unsigned long NEURONCOMPONENTS_API EvalStored(unsigned long PlaybackId) override;
    unsigned long NEURONCOMPONENTS_API ApplyChannels(unsigned long PlaybackId) override;
    unsigned long NEURONCOMPONENTS_API StopPlayback(unsigned long PlaybackId) override;
    unsigned long NEURONCOMPONENTS_API StopChannels(unsigned long PlaybackId, void* Unused) override;
    float NEURONCOMPONENTS_API GetStoredDuration(unsigned long PlaybackId) override;
    unsigned long NEURONCOMPONENTS_API FilterObjects(void* Filter) override;
    unsigned long NEURONCOMPONENTS_API HasObject(unsigned long PlaybackId, void* Object) override;
    unsigned long NEURONCOMPONENTS_API TouchMapped(unsigned long PlaybackId, float* Pos, float* Extra) override;
    unsigned long NEURONCOMPONENTS_API GetFirstInstanceField(unsigned long PlaybackId, unsigned long* Out) override;
    unsigned long NEURONCOMPONENTS_API GetDurationIfNamed(unsigned long PlaybackId, float* Out) override;
    unsigned long NEURONCOMPONENTS_API GetNamedDurationOut(void* Script, const char* Name, float* Out) override;
    unsigned long NEURONCOMPONENTS_API FindObject(unsigned long PlaybackId, void* Query) override;
    unsigned long NEURONCOMPONENTS_API MixerWalk(unsigned long PlaybackId, float Time) override;
    unsigned long NEURONCOMPONENTS_API MixerWalkAll(float Time) override;
    void* NEURONCOMPONENTS_API CreateEmptyScript() override;
    unsigned long NEURONCOMPONENTS_API AddScriptNames(void* Script, void* FileSystem, const char** Names,
                                           int Count) override;
    unsigned long NEURONCOMPONENTS_API RemoveScriptNames(void* Script, const char** Names, int Count) override;
    unsigned long NEURONCOMPONENTS_API EnumFsScripts(void* Callback, void* FileSystem, void* Context) override;
    unsigned long NEURONCOMPONENTS_API HasScriptName(void* Script, const char* Name) override;
    unsigned long NEURONCOMPONENTS_API GetScriptNameCount(void* Script, unsigned long* Out) override;
    unsigned long NEURONCOMPONENTS_API EnumObjects(void* Callback, unsigned long PlaybackId,
                                         void* Context) override;
    unsigned long NEURONCOMPONENTS_API RegisterHandle(unsigned long Key) override;
    unsigned long NEURONCOMPONENTS_API HasHandle(unsigned long Key, unsigned char* Out) override;
    unsigned long NEURONCOMPONENTS_API HasAnyHandle(unsigned long Key) override;

    // IAggregateComponent @ +4, slot 3 (the branch).
    GENRESULT NEURONCOMPONENTS_API Initialize() override;
    // IRootData @ +0xC, slot 3 (the branch).
    unsigned char NEURONCOMPONENTS_API Eval(unsigned long PlaybackId, void* TouchArg, unsigned long Flags,
                                 float Time, float Scale, void* Arg6, void* Arg7,
                                 float Arg8) override;
};

// Hardpoint and EngineComponent facets share a cache and nondelegating inner unknown.
struct FHardpointObject : IHardpoint, IEngineComponent
{
    unsigned long LookupCount;
    unsigned long CacheHits;
    FHardpointMap Archetypes;
    unsigned long Field_0x24; // TODO: meaning unknown; baseline ctor stores 0, nothing else found
    FHardpointMap::iterator CachedNode;
    unsigned char bCacheValid;
    IEngine* Engine;
    INeuronComponent* Controller;    // desc +8 (the aggregating outer) or &Inner
    FHardpointInner Inner;

    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;
    unsigned long NEURONCOMPONENTS_API GetHardpoint(void* Handle, FHardpointKey const* Key,
                                         void* Out) override;
    unsigned long NEURONCOMPONENTS_API AddHardpoint(void* Handle, FHardpointKey const* Key,
                                         const void* Data) override;
    GENRESULT NEURONCOMPONENTS_API EnumerateHardpoints(FHardpointEnumProc Callback, void* Handle,
                                          void* Context) override;
    unsigned long NEURONCOMPONENTS_API ConnectHardpoints(void* InstA, FHardpointKey const* KeyA, void* InstB,
                                              FHardpointKey const* KeyB) override;
    char const* NEURONCOMPONENTS_API FindHardpoint(void* Handle, unsigned long const* Crc) override;
    int NEURONCOMPONENTS_API Initialize() override;
    int NEURONCOMPONENTS_API CreateArchetype(void* Arg1, void* Arg2) override;
    int NEURONCOMPONENTS_API DuplicateArchetype(void* Arg1, void* Arg2) override;
    int NEURONCOMPONENTS_API DestroyArchetype(void* Arg1) override;
    int NEURONCOMPONENTS_API QueryArchetype(void* Arg1, const char* Iid, void** Out) override;
    int NEURONCOMPONENTS_API CreateInstance(void* Arg1, void* Arg2) override;
    int NEURONCOMPONENTS_API DestroyInstance(void* Arg1) override;
    int NEURONCOMPONENTS_API UpdateInstance(void* Arg1, void* Arg2) override;
    int NEURONCOMPONENTS_API QueryInstance(void* Arg1, const char* Iid, void** Out) override;
    int NEURONCOMPONENTS_API Notify(void* Arg1) override;
};

// ChannelArchetype operator_new(0x20): the branch Header 0xC + Frames blob.
// Slots 3–6 load/duration/flags/sample; 7–10 pair/get-frame/range/exact (not IK).
struct FChannelArchetypeObject : IChannelArchetype
{
    unsigned long FrameCount;
    float SampleRate;
    unsigned long Flags;
    unsigned char* Frames;
    unsigned long Stride;
    float Duration;
    unsigned long RefCount;

    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;
    GENRESULT NEURONCOMPONENTS_API Load(void* FileSystem) override;
    GENRESULT NEURONCOMPONENTS_API GetDuration(float* Out) override;
    GENRESULT NEURONCOMPONENTS_API GetFlags(unsigned long* Out) override;
    GENRESULT NEURONCOMPONENTS_API Sample(float Time, const unsigned char** OutA, const unsigned char** OutB,
                                float* Alpha) override;
    GENRESULT NEURONCOMPONENTS_API SamplePair(float Time, const unsigned char** OutLo,
                                    const unsigned char** OutHi, float* TimeLo, float* TimeHi,
                                    float* Alpha) override;
    GENRESULT NEURONCOMPONENTS_API GetFrame(unsigned char bLast, const unsigned char** Out) override;
    GENRESULT NEURONCOMPONENTS_API CollectRange(float Time0, float Time1, float* OutKeys,
                                        unsigned long Capacity, unsigned long* Count) override;
    GENRESULT NEURONCOMPONENTS_API CollectAt(float Time, float* OutKeys, unsigned long Capacity,
                                     unsigned long* Count) override;
};

void SceneRuntimeFdump(int Flags, const char* File, int Line, const char* Kind, const char* Message);
bool SceneRuntimeIidEqual(const char* Left, const char* Right);

GENRESULT NEURONCOMPONENTS_API FactoryQueryInterface(FNeuronComponentsFactory* This, const char* Iid, void** Out);
unsigned long NEURONCOMPONENTS_API FactoryAddRef(FNeuronComponentsFactory* This);
unsigned long NEURONCOMPONENTS_API FactoryRelease(FNeuronComponentsFactory* This);

GENRESULT NEURONCOMPONENTS_API EngineFactoryCreateInstance(FNeuronComponentsFactory* This, AGGDESC* Desc, void** Out);
GENRESULT NEURONCOMPONENTS_API AnimationFactoryCreateInstance(FNeuronComponentsFactory* This, AGGDESC* Desc, void** Out);
GENRESULT NEURONCOMPONENTS_API HardpointFactoryCreateInstance(FNeuronComponentsFactory* This, AGGDESC* Desc, void** Out);
GENRESULT NEURONCOMPONENTS_API ChannelArchetypeFactoryCreateInstance(FNeuronComponentsFactory* This, AGGDESC* Desc,
                                                         void** Out);

void RegisterSceneRuntimeComponents();

// ---- Shared between SceneRuntime.cpp and SceneRuntimeMixer.cpp ------------------------------------------

// the IEngine the Animation component QIs in the branch.
extern FEngineObject* G_AnimEngine;
// create a 3DMathEngine through NEURONCOMPONENTS (no caching, no tests).
I3DMathEngine* Create3DMathEngine();
// Create-on-first-use over the routine. baseline caches it per call site
// (the field / the field / the field / the field / the field); one cache serves
// the ported sites except the routine, which has its own (the field).
I3DMathEngine* Acquire3DMathEngine();

// Typed-channel members the mixer drives (SceneRuntime.cpp).
void TypedApplyFromArchetype(FTypedChannel* This);                 
unsigned long TypedTick(FTypedChannel* This, float Dt);            

// The mixer-map key of a typed channel: typed+0x18 read as a signed int (the routine).
inline long TypedMixerKey(FTypedChannel* This)
{
    return (long)(ptrdiff_t)(This->Bind);
}

// Mixer list primitives (SceneRuntimeMixer.cpp).
void MixerUnlink(FMixerList* List, FMixerNode* Node);                             
void MixerUnlinkAndFree(FMixerList* List, FMixerNode* Node);                      
void MixerFreeList(FMixerList* List);                                             
void MixerUnlinkParentBlend(FMixerList* List, FMixerNode* Node, unsigned char bOuter); 
void MixerFreeNode(FMixerNode* Node);                                             

// Mixer maps on the Animation object (SceneRuntimeMixer.cpp).
FMixerDescMap* MixerFindKey(FAnimationObject* Animation, long Key);               
FMixerDescMap* MixerFindOrAddKey(FAnimationObject* Animation, long Key);          
void MixerDestroyAll(FAnimationObject* Animation);                                // (+0x3C part)
void MixerWalkPlayback(FAnimationObject* Animation, const FAnimBindOut* Desc, FMixerList* List,
                       float Dt);                                                 
void MixerBlendAttach(FAnimationObject* Animation, FTypedChannel* Typed, float Duration,
                      unsigned long Flags);                                       
void MixerUnbindTyped(FAnimationObject* Animation, FTypedChannel* Typed);         
unsigned char MixerHasLeaf(FAnimationObject* Animation, FTypedChannel* Typed);    // tail
void MixerWalkKey(FAnimationObject* Animation, long Key, float Dt);               // body
void MixerWalkAll(FAnimationObject* Animation, float Dt);                         // body

// Playback overlay writers (SceneRuntimeMixer.cpp). Inner is the playback root's entry in the
// mixer map (the routine on playback+4).
void PlaybackWriteOverlayPos(FAnimPlayback* Playback, const float* Pos,
                             FMixerDescMap* Inner);                               
void PlaybackWriteOverlayOrient(FAnimPlayback* Playback, const float* Matrix,
                                FMixerDescMap* Inner);                            
void PlaybackAddOverlayPos(FAnimPlayback* Playback, const float* Delta,
                           FMixerDescMap* Inner);                                 
