// Author: Giperion.

#include "neuroncomponents.h"
#include "luaprofile.h"
#include "system.h"

#include <new>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Aggregate that loads System profile entries and fans interface queries out to its children.


static const unsigned long kAggDescSize = 0x14;
static const unsigned long kRegisterPriority = 0x40000000;
static const unsigned int kFdumpNotice = 0x100003;
static const int kLoadComponentsLine = 0x123;

static const char kComponentName[] = "SystemContainer";  // s_SystemContainer_06f13080
static const char kSystemSection[] = "System";            // s_System_06f13154
static const char kStatusOk[] = "[OK]";
static const char kStatusFailed[] = "[FAILED]";           // s_[FAILED]_06f13148
static const char kEmptyExtra[] = "";                     // (zero dword)
static const char kLoadFormat[] = "SystemContainer: LoadSystemComponents: Loading '%s' [%s] returned %s";
static const char kbaselineSource[] =
    "System\\SysContainer.cpp";
static const char kFdumpPrefix[] = "%s(%d) : NOTICE:General:%s";
static const unsigned int kFdumpScratchSize = 0x2000; // _vsnprintf cap
static const unsigned int kLineBufferSize = 0x100;    // ReadLine buffer @06f11246

// AGGDESC size 0x14 used by this factory and when creating [System] children.
// +8 = outer IUnknown (aggregation), +0xC = inner out, +0x10 = optional extra.
struct FSystemAggDesc
{
    unsigned long Size;
    const char* Name;
    INeuronComponent* Outer;
    INeuronComponent** InnerOut;
    const char* Extra;
};

// Optional child lifecycle facet; cached pointers are borrowed after QI/Release.
struct ISystemComponent : INeuronComponent
{
    virtual GENRESULT NEURONCOMPONENTS_API Initialize() = 0; // +0x0C — unused on children by this DLL
    virtual GENRESULT NEURONCOMPONENTS_API Shutdown() = 0;   // +0x10
};

// ISystemContainer (+0x10 Shutdown .. +0x1C AddComponent) is public: src/include/system.h.

// Fans requests out to children and stops at the first GR_OK result.
struct IDAConnectionPointContainer : INeuronComponent
{
    virtual GENRESULT NEURONCOMPONENTS_API FindConnectionPoint(const char* Iid, void** Out) = 0; // +0x0C
    virtual GENRESULT NEURONCOMPONENTS_API EnumConnectionPoints(void* Arg0, void* Arg1) = 0;     // +0x10
};

// operator_new(0x14) node. baseline zeroes only Next/Inner (06f112d6/06f114ef); Object is the
// CreateInstance out-param, SystemComponent/Aggregate are the QI out-params.
struct FComponentNode
{
    FComponentNode* Next;
    INeuronComponent* Inner;
    INeuronComponent* Object;
    ISystemComponent* SystemComponent;
    IAggregateComponent* Aggregate;
};

class FSystemContainer;

class FInnerUnknown final : public INeuronComponent
{
public:
    FInnerUnknown();

    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;

    unsigned long RefCount;
    FSystemContainer* Outer;
};

class FSystemContainer final : public ISystemContainer, public IDAConnectionPointContainer
{
public:
    FSystemContainer();
    ~FSystemContainer();

    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;

    GENRESULT NEURONCOMPONENTS_API Initialize() override;
    GENRESULT NEURONCOMPONENTS_API Shutdown() override;
    GENRESULT NEURONCOMPONENTS_API LoadSystemComponents() override;
    GENRESULT NEURONCOMPONENTS_API UnloadSystemComponents() override;
    GENRESULT NEURONCOMPONENTS_API AddComponent(AGGDESC* Desc) override;

    GENRESULT NEURONCOMPONENTS_API FindConnectionPoint(const char* Iid, void** Out) override;
    GENRESULT NEURONCOMPONENTS_API EnumConnectionPoints(void* Arg0, void* Arg1) override;

    GENRESULT BindOuter(FSystemAggDesc* Desc);

    FComponentNode* Head;
    FComponentNode* Tail;
    FInnerUnknown Inner;
    INeuronComponent* Controlling;
    unsigned long bAggregated;
    unsigned long bLoaded;
};

static_assert(offsetof(FSystemContainer, Head) == 0x08, "Head at +8");
static_assert(offsetof(FSystemContainer, Tail) == 0x0C, "Tail at +0xC");
static_assert(offsetof(FSystemContainer, Inner) == 0x10, "Inner at +0x10");
static_assert(offsetof(FSystemContainer, Controlling) == 0x1C, "Controlling at +0x1C");
static_assert(offsetof(FSystemContainer, bAggregated) == 0x20, "bAggregated at +0x20");
static_assert(offsetof(FSystemContainer, bLoaded) == 0x24, "bLoaded at +0x24");
static_assert(sizeof(FSystemContainer) == 0x28, "calloc(0x28, 1) @06f118f6");
static_assert(sizeof(FComponentNode) == 0x14, "FComponentNode is 0x14");

class FSystemContainerFactory final : public IComponentFactory
{
public:
    FSystemContainerFactory();

    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;
    GENRESULT NEURONCOMPONENTS_API CreateInstance(AGGDESC* Desc, void** Instance) override;

    unsigned long RefCount;
    const char* Name;
};

static_assert(sizeof(FSystemContainerFactory) == 0x0C, "factory is 0xC");

ICOManager* GNeuronComponents = nullptr;
HMODULE GModule = nullptr;

// Inlined smart-pointer release of node->Inner (06f111ac..b9, 06f11350..5c, 06f115ad..b9).
static void ReleaseNodeInner(FComponentNode* Node)
{
    if (Node->Inner != nullptr)
    {
        Node->Inner->Release();
        Node->Inner = nullptr;
    }
}

// _vsnprintf(Buffer, 0x2000, Format, ...) and return Buffer. No forced terminator.
static char* FormatNotice(char* Buffer, const char* Format, ...)
{
    va_list Args;
    va_start(Args, Format);
    _vsnprintf(Buffer, kFdumpScratchSize, Format, Args);
    va_end(Args);
    return Buffer;
}

static void LinkAndProbeNode(FSystemContainer* Container, FComponentNode* Node)
{
    if (Container->Tail != nullptr)
        Container->Tail->Next = Node;
    else
        Container->Head = Node;
    Container->Tail = Node;

    if (Node->Inner->QueryInterface(Iids::SystemComponent,
                                    (void**)(&Node->SystemComponent)) == GR_OK)
        Node->SystemComponent->Release();

    if (Node->Inner->QueryInterface(Iids::AggregateComponent,
                                    (void**)(&Node->Aggregate)) == GR_OK)
        Node->Aggregate->Release();
}

FInnerUnknown::FInnerUnknown() : RefCount(1), Outer(nullptr) {}

GENRESULT NEURONCOMPONENTS_API FInnerUnknown::QueryInterface(const char* Iid, void** Out)
{
    struct FQiEntry
    {
        const char* Iid;
        unsigned long ByteOffset;
    };
    static const FQiEntry Table[] = {
        {Iids::SystemContainer, 0},            // / 06f12060
        {Iids::AggregateComponent, 0},         // / 06f12068
        {Iids::SystemComponent, 0},            // / 06f12070
        {Iids::DAConnectionPointContainer, 4}, // / 06f12078
        {nullptr, 0},
    };

    for (unsigned int Index = 0; Table[Index].Iid != nullptr; ++Index)
    {
        if (strcmp(Table[Index].Iid, Iid) == 0)
        {
            unsigned char* Base = (unsigned char*)(Outer);
            unsigned long Offset = Table[Index].ByteOffset;
            INeuronComponent* Face = nullptr;
            if ((Offset & 0x80000000u) != 0)
                Face = *(INeuronComponent**)(Base + (Offset & 0x7FFFFFFFu));
            else
                Face = (INeuronComponent*)(Base + Offset);
            Face->AddRef();
            *Out = Face;
            return GR_OK;
        }
    }

    *Out = nullptr;
    FComponentNode* Node = Outer->Head;
    while (Node != nullptr)
    {
        GENRESULT Result = Node->Inner->QueryInterface(Iid, Out);
        if (Result == GR_OK)
            return Result;
        Node = Node->Next;
    }
    return GR_INTERFACE_UNSUPPORTED;
}

// (shared with the factory's AddRef).
unsigned long NEURONCOMPONENTS_API FInnerUnknown::AddRef()
{
    ++RefCount;
    return RefCount;
}

// last release runs the container destructor (the routine) and free()s the
// calloc'd block.
unsigned long NEURONCOMPONENTS_API FInnerUnknown::Release()
{
    if (RefCount > 0)
        --RefCount;
    if (RefCount != 0)
        return RefCount;

    RefCount = 1;
    FSystemContainer* Owner = Outer;
    if (Owner != nullptr)
    {
        // This object uses zero-filled storage and placement construction.
        Owner->~FSystemContainer();
        free(Owner);
    }
    return 0;
}

// Inline constructor in the branch (06f11905..06f11928) on calloc'd memory: Inner.RefCount=1,
// Inner.Outer=this, Controlling=&Inner; Head/Tail/bAggregated/bLoaded are calloc zeros.
FSystemContainer::FSystemContainer()
    : Head(nullptr), Tail(nullptr), Controlling(nullptr), bAggregated(0), bLoaded(0)
{
    Inner.Outer = this;
    Controlling = &Inner;
}

// AddRef the controlling unknown, then UnloadSystemComponents (the routine).
FSystemContainer::~FSystemContainer()
{
    Controlling->AddRef();
    UnloadSystemComponents();
}

// / 06f11170 / 06f11180: tail-jumps through Controlling (+0x1C).
GENRESULT NEURONCOMPONENTS_API FSystemContainer::QueryInterface(const char* Iid, void** Out)
{
    return Controlling->QueryInterface(Iid, Out);
}

unsigned long NEURONCOMPONENTS_API FSystemContainer::AddRef()
{
    return Controlling->AddRef();
}

unsigned long NEURONCOMPONENTS_API FSystemContainer::Release()
{
    return Controlling->Release();
}

GENRESULT NEURONCOMPONENTS_API FSystemContainer::Initialize()
{
    GENRESULT Result = GR_OK;
    FComponentNode* Previous = nullptr;
    FComponentNode* Node = Head;
    while (Node != nullptr)
    {
        if (Node->Aggregate != nullptr)
        {
            Result = Node->Aggregate->Initialize();
            if (Result != GR_OK)
            {
                if (Previous != nullptr)
                    Previous->Next = Node->Next;
                else
                    Head = Node->Next;
                ReleaseNodeInner(Node);
                delete Node;
                if (bAggregated != 0)
                    return Result;
                Node = Head;
                Previous = nullptr;
                continue;
            }
        }
        Previous = Node;
        Node = Node->Next;
    }
    return Result;
}

GENRESULT NEURONCOMPONENTS_API FSystemContainer::Shutdown()
{
    GENRESULT Result = (GENRESULT)((intptr_t)((ISystemContainer*)(this)));
    FComponentNode* Node = Head;
    while (Node != nullptr)
    {
        Result = (GENRESULT)((intptr_t)(Node->SystemComponent));
        if (Node->SystemComponent != nullptr)
            Result = Node->SystemComponent->Shutdown();
        Node = Node->Next;
    }
    return Result;
}

// (+ tail the branch): read [System] from the NEURONCOMPONENTS profile parser and
// aggregate one child per line ("Name [= Extra]").
GENRESULT NEURONCOMPONENTS_API FSystemContainer::LoadSystemComponents()
{
    if (bLoaded != 0)
        return GR_OK;

    IProfileParser* Parser = nullptr;
    if (GNeuronComponents->QueryInterface(Iids::ProfileParser, (void**)(&Parser)) == GR_OK)
    {
        void* Section = Parser->OpenSection(kSystemSection, 3);
        if (Section != nullptr)
        {
            char Line[kLineBufferSize];
            int LineIndex = 0;
            while (Parser->ReadLine(Section, LineIndex++, Line, kLineBufferSize) != 0)
            {
                char* Name = Line;
                while (*Name == ' ' || *Name == '\t')
                    ++Name;
                if (*Name == ';' || *Name == '\0')
                    continue;

                FSystemAggDesc Desc;
                Desc.Size = kAggDescSize;
                Desc.Name = nullptr;
                Desc.Outer = nullptr;
                Desc.InnerOut = nullptr;
                Desc.Extra = nullptr;

                char* Extra = strchr(Name, '=');
                if (Extra != nullptr)
                {
                    *Extra = '\0';
                    do
                    {
                        ++Extra;
                    } while (*Extra == ' ' || *Extra == '\t');
                }

                // operator_new(0x14); baseline has no null path (a null node faults at
                // ), so the throwing form is used here.
                FComponentNode* Node = new FComponentNode;
                Node->Inner = nullptr;
                Node->Next = nullptr;

                Desc.Name = Name;
                Desc.Outer = (ISystemContainer*)(this);
                Desc.InnerOut = &Node->Inner;
                Desc.Extra = Extra;

                char* Token = strchr(Name, ' ');
                if (Token != nullptr)
                    *Token = '\0';
                Token = strchr(Name, '\t');
                if (Token != nullptr)
                    *Token = '\0';

                const char* Status = nullptr;
                if (GNeuronComponents->CreateInstance((AGGDESC*)(&Desc),
                                           (void**)(&Node->Object)) != GR_OK)
                {
                    Status = kStatusFailed;
                    ReleaseNodeInner(Node);
                    delete Node;
                }
                else
                {
                    Status = kStatusOk;
                    LinkAndProbeNode(this, Node);
                }

                char Scratch[kFdumpScratchSize];
                FDUMP((int)(kFdumpNotice), kFdumpPrefix, kbaselineSource, kLoadComponentsLine,
                      FormatNotice(Scratch, kLoadFormat, Desc.Name,
                                   Desc.Extra != nullptr ? Desc.Extra : kEmptyExtra, Status));
            }

            if (bAggregated == 0)
                Initialize();
            bLoaded = 1;
        }
    }

    if (Parser != nullptr)
        Parser->Release();
    return GR_OK;
}

// destroy nodes from the head; Head follows each deletion.
GENRESULT NEURONCOMPONENTS_API FSystemContainer::UnloadSystemComponents()
{
    while (Head != nullptr)
    {
        FComponentNode* Node = Head;
        FComponentNode* Next = Node->Next;
        ReleaseNodeInner(Node);
        delete Node;
        Head = Next;
    }
    Tail = nullptr;
    bLoaded = 0;
    return GR_OK;
}

// aggregate one child from a caller descriptor. Outer/InnerOut are borrowed for
// the CreateInstance call and restored on both exits. No Initialize call here.
GENRESULT NEURONCOMPONENTS_API FSystemContainer::AddComponent(AGGDESC* Desc)
{
    if (Desc == nullptr)
        return GR_INVALID_PARMS;

    FSystemAggDesc* SystemDesc = (FSystemAggDesc*)(Desc);
    INeuronComponent* SavedOuter = SystemDesc->Outer;
    INeuronComponent** SavedInnerOut = SystemDesc->InnerOut;

    FComponentNode* Node = new (std::nothrow) FComponentNode;
    if (Node == nullptr)
        return GR_OUT_OF_MEMORY;
    Node->Inner = nullptr;
    Node->Next = nullptr;

    SystemDesc->Outer = (ISystemContainer*)(this);
    SystemDesc->InnerOut = &Node->Inner;
    GENRESULT Result = GNeuronComponents->CreateInstance(Desc, (void**)(&Node->Object));
    if (Result != GR_OK)
    {
        SystemDesc->Outer = SavedOuter;
        SystemDesc->InnerOut = SavedInnerOut;
        ReleaseNodeInner(Node);
        delete Node;
        return Result;
    }

    LinkAndProbeNode(this, Node);
    SystemDesc->Outer = SavedOuter;
    SystemDesc->InnerOut = SavedInnerOut;
    return Result;
}

// default -1, RET 0xC, stop on GR_OK. Args forwarded as-is.
GENRESULT NEURONCOMPONENTS_API FSystemContainer::FindConnectionPoint(const char* Iid, void** Out)
{
    GENRESULT Result = GR_GENERIC;
    IDAConnectionPointContainer* Cached = nullptr;
    FComponentNode* Node = Head;
    while (Node != nullptr)
    {
        if (Cached != nullptr)
        {
            Cached->Release();
            Cached = nullptr;
        }
        if (Node->Inner->QueryInterface(Iids::DAConnectionPointContainer,
                                        (void**)(&Cached)) == GR_OK)
        {
            Result = Cached->FindConnectionPoint(Iid, Out);
            if (Result == GR_OK)
                break;
        }
        Node = Node->Next;
    }
    if (Cached != nullptr)
        Cached->Release();
    return Result;
}

// default 1, RET 0xC, stop on GR_OK. Not OLE Enum (1 arg).
GENRESULT NEURONCOMPONENTS_API FSystemContainer::EnumConnectionPoints(void* Arg0, void* Arg1)
{
    GENRESULT Result = (GENRESULT)(1);
    IDAConnectionPointContainer* Cached = nullptr;
    FComponentNode* Node = Head;
    while (Node != nullptr)
    {
        if (Cached != nullptr)
        {
            Cached->Release();
            Cached = nullptr;
        }
        if (Node->Inner->QueryInterface(Iids::DAConnectionPointContainer,
                                        (void**)(&Cached)) == GR_OK)
        {
            Result = Cached->EnumConnectionPoints(Arg0, Arg1);
            if (Result == GR_OK)
                break;
        }
        Node = Node->Next;
    }
    if (Cached != nullptr)
        Cached->Release();
    return Result;
}

// refuse an Extra of exactly "SystemContainer" (CMPSB ECX=0x10 over the 16-byte
// literal incl. NUL == strcmp), else adopt the outer unknown when one is given.
GENRESULT FSystemContainer::BindOuter(FSystemAggDesc* Desc)
{
    if (Desc->Extra != nullptr && strcmp(Desc->Extra, kComponentName) == 0)
        return GR_GENERIC;
    if (Desc->Outer != nullptr)
    {
        Controlling = Desc->Outer;
        *Desc->InnerOut = &Inner;
        bAggregated = 1;
    }
    return GR_OK;
}

// Inline constructor in the routine (06f117c0..06f117ce).
FSystemContainerFactory::FSystemContainerFactory() : RefCount(1), Name(kComponentName) {}

// *Out = 0, CMPSB 0x17 bytes vs "1.11_IComponentFactory" (== strcmp).
GENRESULT NEURONCOMPONENTS_API FSystemContainerFactory::QueryInterface(const char* Iid, void** Out)
{
    *Out = nullptr;
    if (strcmp(Iid, Iids::ComponentFactory) != 0)
        return GR_INTERFACE_UNSUPPORTED;
    *Out = (IComponentFactory*)(this);
    AddRef();
    return GR_OK;
}

unsigned long NEURONCOMPONENTS_API FSystemContainerFactory::AddRef()
{
    ++RefCount;
    return RefCount;
}

// .
unsigned long NEURONCOMPONENTS_API FSystemContainerFactory::Release()
{
    if (RefCount > 0)
        --RefCount;
    if (RefCount != 0)
        return RefCount;
    RefCount = 1;
    delete this;
    return 0;
}

// .
GENRESULT NEURONCOMPONENTS_API FSystemContainerFactory::CreateInstance(AGGDESC* Desc, void** Instance)
{
    FSystemAggDesc* SystemDesc = (FSystemAggDesc*)(Desc);
    if (SystemDesc->Size != kAggDescSize || strcmp(SystemDesc->Name, Name) != 0)
    {
        *Instance = nullptr;
        return GR_INTERFACE_UNSUPPORTED;
    }

    // Pair zero-filled placement construction with the existing C-runtime teardown.
    void* Memory = calloc(sizeof(FSystemContainer), 1);
    if (Memory == nullptr)
    {
        *Instance = nullptr;
        return GR_OUT_OF_MEMORY;
    }
    FSystemContainer* Object = new (Memory) FSystemContainer();

    GENRESULT Result = Object->BindOuter(SystemDesc);
    if (Result != GR_OK)
    {
        Object->~FSystemContainer();
        free(Object);
        *Instance = nullptr;
        return Result;
    }

    *Instance = (ISystemContainer*)(Object);
    return Result;
}

// operator_new(0xC) (silent return on null), register, drop the creation ref.
static void RegisterSystemContainer(ICOManager* NeuronComponents)
{
    FSystemContainerFactory* Factory = new (std::nothrow) FSystemContainerFactory();
    if (Factory == nullptr)
        return;
    NeuronComponents->RegisterComponent(Factory, kComponentName, (void*)(kRegisterPriority));
    Factory->Release();
}

BOOL WINAPI DllMain(HINSTANCE Instance, DWORD Reason, LPVOID)
{
    // DEC reason; JNZ → return 1. DETACH / THREAD_* are no-ops
    // (no UnregisterComponent, no Release of the field).
    if (Reason == DLL_PROCESS_ATTACH)
    {
        GModule = Instance;
        DisableThreadLibraryCalls(Instance);
        GNeuronComponents = NEURONCOMPONENTS_Acquire();
        // a second NEURONCOMPONENTS_Acquire call feeds the registration, not the field.
        RegisterSystemContainer(NEURONCOMPONENTS_Acquire());
    }
    return TRUE;
}
