// P4.5 (first increment): the parts of Shading.dll's materials that do not draw, through the
// reference's own code: FMaterialRegex (MaterialRegex.cpp), FMaterialLibrary (MaterialLibrary.cpp,
// with the global material records), FMaterialAnimation (MaterialAnimation.cpp) and
// FMaterialBatcher (MaterialBatcher.cpp). Those sources are compiled into the harness for P1.7
// (Oracle.vcxproj; MaterialAnimation.cpp through oracle_shading_anim.cpp). The Rust side is
// crates/neuron-client/tests/oracle_vectors.rs with crates/neuron-client/tests/support/material.rs;
// the inputs come from crates/neuron-client/tests/fixtures/oracle/make_material_fixtures.py.
//
// Materials are fakes (FOracleMaterial), numbered from 1 in each case in creation order and kept in
// one array, so that their addresses ascend with their numbers (the batcher orders its queue by
// material pointer). The component manager makes them by name through factories this file
// registers: DcDt, DcDtTwo, DcDtOcOt, DcDtEcOcOt, BtDetailMapMaterial, NebulaTwo, NullMaterial,
// HighGlassMaterial and Plain are materials; BadInit fails Initialize; BatchedMaterial has the
// batched face and accepts an inner type of the first list; NotMaterial is a component without
// the material face. A fake counts references (never below 0) and writes a line for every call
// the library or the batcher makes on it, "destroy" when its count reaches 0. Strings are written
// as hex, floats as the eight hex digits of the f32. Results a callee may leave alone start as
// 0xCDCDCDCD (or -7 for counts).
//
// material_regex.list: per line a pattern and its subjects (hex, comma-separated):
//   {"case","prog"(hex),"start"(%02x),"anch","must"("offset,length" or "-"),"match"(one 0/1 per
//   subject)}, or {"case","prog":"-","match"} when the pattern does not compile.
// material_library.list, material_global.list, material_animation.list and
// material_batcher.list: one case per line, operations separated by " ; ", fields by single
// spaces. Every operation writes {"case","op",...} after the lines of the calls it made; the
// operations are those of the Run methods below. Dumps: map entries {"case","crc","refs","mat",
// "mrefs"} in CRC order, groups {"case","group","gid","name","members"}, rules {"case","rule","key",
// "name","pat","type","ok"}, batching rules {"case","brule","kind","pat","ok"}, bins {"case","bin",
// "key","sort","records","queue"} and {"case","order","selected","tag","tagctx"}, animation map
// entries {"case","crc","data","drefs"}.
//
// The global material records are a process static; material_global.list is one script from
// the state the first use creates (no other suite of this mode touches them). Animation
// instances are linked into one process list; every animation case releases the instances it
// made. The batcher cases load their INI into the manager's profile parser, which keeps it.

#include "oracle_support.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "luaprofile.h"
#include "../Shading/MaterialAnimation.h"
#include "../Shading/MaterialBatcher.h"
#include "../Shading/MaterialRegex.h"

using namespace oracle;

// oracle_shading_anim.cpp: the per-child steps of LoadAnimationGroup on prepared data.
void* OracleAddAnimationGroup(FMaterialAnimation* Library, const std::vector<FImaData*>& Data);

namespace
{
    // FMaterialRegex keeps its program private; an explicit instantiation may name private
    // members (the access rules do not apply to it), which hands them out through a friend.
    template <typename Tag, typename Tag::Type Member>
    struct FPrivate
    {
        friend typename Tag::Type Get(Tag) { return Member; }
    };

    struct FProgramTag
    {
        typedef std::vector<unsigned char> FMaterialRegex::*Type;
        friend Type Get(FProgramTag);
    };
    struct FStartTag
    {
        typedef unsigned char FMaterialRegex::*Type;
        friend Type Get(FStartTag);
    };
    struct FAnchoredTag
    {
        typedef bool FMaterialRegex::*Type;
        friend Type Get(FAnchoredTag);
    };
    struct FMustOffsetTag
    {
        typedef int FMaterialRegex::*Type;
        friend Type Get(FMustOffsetTag);
    };
    struct FMustLengthTag
    {
        typedef int FMaterialRegex::*Type;
        friend Type Get(FMustLengthTag);
    };

    template struct FPrivate<FProgramTag, &FMaterialRegex::Program>;
    template struct FPrivate<FStartTag, &FMaterialRegex::StartCharacter>;
    template struct FPrivate<FAnchoredTag, &FMaterialRegex::bAnchored>;
    template struct FPrivate<FMustOffsetTag, &FMaterialRegex::MustOffset>;
    template struct FPrivate<FMustLengthTag, &FMaterialRegex::MustLength>;

    const unsigned long kUntouched = 0xCDCDCDCDu;

    const char* const kNormalTypes[] = {"DcDt",       "DcDtTwo",          "DcDtOcOt",
                                        "DcDtEcOcOt", "BtDetailMapMaterial", "NebulaTwo",
                                        "NullMaterial", "HighGlassMaterial", "Plain"};

    bool IsNormalType(const char* Name)
    {
        for (size_t Index = 0; Index < sizeof(kNormalTypes) / sizeof(kNormalTypes[0]); ++Index)
        {
            if (strcmp(kNormalTypes[Index], Name) == 0)
                return true;
        }
        return false;
    }

    std::vector<std::string> Split(const std::string& Text, const std::string& Separator)
    {
        std::vector<std::string> Parts;
        size_t Start = 0;
        for (;;)
        {
            const size_t End = Text.find(Separator, Start);
            if (End == std::string::npos)
            {
                Parts.push_back(Text.substr(Start));
                return Parts;
            }
            Parts.push_back(Text.substr(Start, End - Start));
            Start = End + Separator.size();
        }
    }

    long Int(const std::string& Text)
    {
        return strtol(Text.c_str(), nullptr, 10);
    }

    unsigned long Hex(const std::string& Text)
    {
        return strtoul(Text.c_str(), nullptr, 16);
    }

    float Bits(const std::string& Text)
    {
        return FloatFromBits((unsigned int)Hex(Text));
    }

    std::string Floats(const float* Values, int Count)
    {
        std::string Out;
        for (int Index = 0; Index < Count; ++Index)
            Out += Format(Index == 0 ? "%08x" : ",%08x", FloatBits(Values[Index]));
        return Out;
    }

    // --- material_regex ------------------------------------------------------------------

    void RegexSuite()
    {
        Suite Out("material_regex.jsonl");
        const std::vector<std::string> Lines = InputLines("material_regex.list");
        for (unsigned int Case = 0; Case < Lines.size(); ++Case)
        {
            const std::string& Line = Lines[Case];
            const size_t Space = Line.find(' ');
            const Bytes Pattern = Unhex(Line.substr(0, Space));
            const std::string SubjectText = Space == std::string::npos ? std::string() : Line.substr(Space + 1);
            const std::unique_ptr<FMaterialRegex> Regex = FMaterialRegex::Compile(CStr(Pattern));
            std::string Matches;
            const std::vector<std::string> Subjects = Split(SubjectText, ",");
            for (size_t Index = 0; Index < Subjects.size(); ++Index)
            {
                const Bytes Subject = Unhex(Subjects[Index]);
                Matches += FMaterialRegex::Search(Regex.get(), CStr(Subject)) ? "1" : "0";
            }
            if (Regex == nullptr)
            {
                Out.Line(Format("{\"case\":\"%u\",\"prog\":\"-\",\"match\":\"", Case) + Matches + "\"}");
                continue;
            }
            const std::vector<unsigned char>& Program = (*Regex).*Get(FProgramTag());
            const int MustOffset = (*Regex).*Get(FMustOffsetTag());
            const int MustLength = (*Regex).*Get(FMustLengthTag());
            const std::string Must = MustOffset == -1 ? std::string("-") : Format("%d,%d", MustOffset, MustLength);
            Out.Line(Format("{\"case\":\"%u\",\"prog\":\"", Case) + oracle::Hex(Program.data(), Program.size()) +
                     Format("\",\"start\":\"%02x\",\"anch\":\"%d\",\"must\":\"", (*Regex).*Get(FStartTag()),
                            (*Regex).*Get(FAnchoredTag()) ? 1 : 0) +
                     Must + "\",\"match\":\"" + Matches + "\"}");
        }
    }

    // --- the fake materials ----------------------------------------------------------------

    // Where the fakes, the renderer and the events sink write.
    struct FEventSink
    {
        Suite* Out = nullptr;
        unsigned int Case = 0;

        void Event(const std::string& Fields)
        {
            if (Out != nullptr)
                Out->Line(Format("{\"case\":\"%u\",", Case) + Fields + "}");
        }
    };

    FEventSink GSink;

    std::string ContextFields(const void* ContextArg, bool bArgs, unsigned long PrimType, unsigned long Start,
                              unsigned long Count, unsigned long IbStart, unsigned long Span, unsigned long Flags)
    {
        const FMaterialSubmitContext* Context = (const FMaterialSubmitContext*)ContextArg;
        const FRpVertexSrcDesc* Source = Context->SourceVertices;
        const unsigned long Bytes = (unsigned long)Source->Position.Count * Source->Position.Stride;
        std::string Fields = Format("\"inst\":\"%lu\",\"src\":\"%u\",\"pos\":\"", (unsigned long)(uintptr_t)Context->Inst,
                                    Source->SrcCount) +
                             (Source->Position.Base != nullptr ? oracle::Hex(Source->Position.Base, Bytes) : std::string()) +
                             "\"";
        Fields += Context->VbListing != nullptr
                      ? Format(",\"vb\":\"%lu,%lu,%lu\"", (unsigned long)(uintptr_t)Context->VbListing->VertexBuffer,
                               Context->VbListing->Fvf, Context->VbListing->BaseVertex)
                      : std::string(",\"vb\":\"-\"");
        Fields += Context->IbScratch != nullptr
                      ? Format(",\"ib\":\"%lu,%lu\"", (unsigned long)(uintptr_t)Context->IbScratch->IndexBuffer,
                               Context->IbScratch->Bias)
                      : std::string(",\"ib\":\"-\"");
        if (bArgs)
        {
            std::string Indices;
            for (unsigned long Index = 0; Index < Span; ++Index)
                Indices += Format(Index == 0 ? "%u" : ",%u", Context->Indices[IbStart + Index]);
            Fields += Format(",\"prim\":\"%lu\",\"start\":\"%lu\",\"count\":\"%lu\",\"ibs\":\"%lu\",\"span\":\"%lu\","
                             "\"flags\":\"%lu\",\"idx\":\"",
                             PrimType, Start, Count, IbStart, Span, Flags) +
                      Indices + "\"";
        }
        return Fields;
    }

    class FOracleMaterial;

    class FOracleBatchedFace final : public IBatchedMaterial
    {
    public:
        FOracleMaterial* Owner = nullptr;

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
        unsigned long NEURONCOMPONENTS_API AddRef() override;
        unsigned long NEURONCOMPONENTS_API Release() override;
        GENRESULT NEURONCOMPONENTS_API CreateInnerMaterial(void* Name) override;
        GENRESULT NEURONCOMPONENTS_API SetBatchBin(void* Value) override { return GR_OK; }
        GENRESULT NEURONCOMPONENTS_API GetBatchBin(void** Out) override { return GR_OK; }
        GENRESULT NEURONCOMPONENTS_API Bind(void* Context) override;
        GENRESULT NEURONCOMPONENTS_API Teardown(void* Context) override;
        GENRESULT NEURONCOMPONENTS_API Submit(void* Context, unsigned long PrimType, unsigned long Start,
                                              unsigned long Count, unsigned long IbStart, unsigned long IndexSpan,
                                              unsigned long Flags) override;
    };

    class FOracleMaterial final : public IMaterial
    {
    public:
        unsigned long Serial = 0;
        unsigned long RefCount = 0;
        std::string Kind;
        unsigned long Flags = 0;
        bool bBatched = false;
        FOracleBatchedFace Face;

        void Reset(unsigned long InSerial, const char* InKind, unsigned long InFlags, bool bInBatched)
        {
            Serial = InSerial;
            RefCount = 1;
            Kind = InKind;
            Flags = InFlags;
            bBatched = bInBatched;
            Face.Owner = this;
        }

        void Event(const char* Name, const std::string& Extra)
        {
            GSink.Event(Format("\"ev\":\"%s\",\"mat\":\"%lu\"", Name, Serial) + Extra);
        }

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            if (strcmp(Iid, Iids::Material) == 0)
            {
                AddRef();
                *Out = (IMaterial*)this;
                return GR_OK;
            }
            if (bBatched && strcmp(Iid, Iids::BatchedMaterial) == 0)
            {
                AddRef();
                *Out = (IBatchedMaterial*)&Face;
                return GR_OK;
            }
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return ++RefCount; }
        unsigned long NEURONCOMPONENTS_API Release() override
        {
            if (RefCount == 0)
                return 0;
            if (--RefCount == 0)
                Event("destroy", "");
            return RefCount;
        }

        GENRESULT NEURONCOMPONENTS_API Initialize(INeuronComponent* Outer) override
        {
            Event("init", "");
            return Kind == "BadInit" ? GR_GENERIC : GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API LoadFromFileSystem(void* FileSystem) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SaveProperties(void* FileSystem) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ApplyVerify(void* Context, void* Extra) override
        {
            Event("verify", "");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API ResolveTextures(float Delta) override
        {
            Event("resolve", Format(",\"d\":\"%08x\"", FloatBits(Delta)));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API Bind(void* Context) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Teardown(void* Context) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Submit(void* Context, unsigned long PrimType, unsigned long Start,
                                              unsigned long Count, unsigned long IbStart, unsigned long IndexSpan,
                                              unsigned long SubmitFlags) override
        {
            Event("submit", "," + ContextFields(Context, true, PrimType, Start, Count, IbStart, IndexSpan, SubmitFlags));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetName(const char* Name) override { return GR_OK; }
        GENRESULT NEURONCOMPONENTS_API GetName(char* Out, unsigned long Size) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API CopyClassName(char* Out, unsigned long Size) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetPassCount(int* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetRenderFlags(unsigned long* Out) override
        {
            Event("flags", "");
            *Out = Flags;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetParameterCount(unsigned long* OutCount) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetParameterDescriptor(unsigned long Index, void* OutDesc) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API GetParameterValue(unsigned long Index, void* InOutDesc) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API SetParameterValue(unsigned long Index, void* Value) override
        {
            return GR_GENERIC;
        }
        const char* __thiscall ClassName() override { return Kind.c_str(); }
        void* __thiscall ScalarDeletingDestructor(unsigned char bDelete) override { return this; }
        GENRESULT __thiscall InitializeFromDescriptor(void* Arg) override { return GR_OK; }
        GENRESULT __thiscall ApplyInitialRenderStates(void* Arg) override { return GR_OK; }
        GENRESULT __thiscall Setup() override { return GR_OK; }
        bool __thiscall ValueWithinDeclaredRange(unsigned long Index, void* Entry, void* Value) override
        {
            return false;
        }
        GENRESULT __thiscall StoreParameterValue(unsigned long Index, void* Entry, void* Value) override
        {
            return GR_GENERIC;
        }
        unsigned long __thiscall GetParameterTable(void** OutBase) override { return 0; }
        void* __thiscall GetParameterAddress(unsigned long Index) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API InnerQueryInterface(const char* Iid, void** Out) override
        {
            return QueryInterface(Iid, Out);
        }
        unsigned long NEURONCOMPONENTS_API InnerAddRef() override { return AddRef(); }
        unsigned long NEURONCOMPONENTS_API InnerRelease() override { return Release(); }
    };

    GENRESULT NEURONCOMPONENTS_API FOracleBatchedFace::QueryInterface(const char* Iid, void** Out)
    {
        return Owner->QueryInterface(Iid, Out);
    }
    unsigned long NEURONCOMPONENTS_API FOracleBatchedFace::AddRef()
    {
        return Owner->AddRef();
    }
    unsigned long NEURONCOMPONENTS_API FOracleBatchedFace::Release()
    {
        return Owner->Release();
    }
    GENRESULT NEURONCOMPONENTS_API FOracleBatchedFace::CreateInnerMaterial(void* Name)
    {
        Owner->Event("inner", ",\"type\":\"" + HexC((const char*)Name) + "\"");
        return IsNormalType((const char*)Name) ? GR_OK : GR_GENERIC;
    }
    GENRESULT NEURONCOMPONENTS_API FOracleBatchedFace::Bind(void* Context)
    {
        Owner->Event("bind", "," + ContextFields(Context, false, 0, 0, 0, 0, 0, 0));
        return GR_OK;
    }
    GENRESULT NEURONCOMPONENTS_API FOracleBatchedFace::Teardown(void* Context)
    {
        Owner->Event("teardown", "," + ContextFields(Context, false, 0, 0, 0, 0, 0, 0));
        return GR_OK;
    }
    GENRESULT NEURONCOMPONENTS_API FOracleBatchedFace::Submit(void* Context, unsigned long PrimType,
                                                              unsigned long Start, unsigned long Count,
                                                              unsigned long IbStart, unsigned long IndexSpan,
                                                              unsigned long Flags)
    {
        Owner->Event("bsubmit", "," + ContextFields(Context, true, PrimType, Start, Count, IbStart, IndexSpan, Flags));
        return GR_OK;
    }

    // The fakes of the current case, in creation order (and address order).
    const unsigned long kPoolSize = 256;
    FOracleMaterial GPool[kPoolSize];
    unsigned long GMade = 0;

    FOracleMaterial* MakeMaterial(const char* Kind, unsigned long Flags, bool bBatched)
    {
        if (GMade >= kPoolSize)
        {
            fprintf(stderr, "material: more than %lu fakes in a case\n", kPoolSize);
            GFailed = true;
            exit(1);
        }
        FOracleMaterial* Material = &GPool[GMade];
        ++GMade;
        Material->Reset(GMade, Kind, Flags, bBatched);
        return Material;
    }

    void StartCase(Suite* Out, unsigned int Case)
    {
        GSink.Out = Out;
        GSink.Case = Case;
        GMade = 0;
    }

    // A component without the material face.
    class FNotMaterial final : public INeuronComponent
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
    };

    FNotMaterial GNotMaterial;

    class FOracleMaterialFactory final : public IComponentFactory
    {
    public:
        explicit FOracleMaterialFactory(const char* InName) : Name(InName) {}

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            *Out = nullptr;
            if (strcmp(Iid, Iids::ComponentFactory) != 0)
                return GR_INTERFACE_UNSUPPORTED;
            *Out = (IComponentFactory*)this;
            return GR_OK;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }

        GENRESULT NEURONCOMPONENTS_API CreateInstance(AGGDESC* Desc, void** Instance) override
        {
            const FMaterialAggregateDesc* MaterialDesc = (const FMaterialAggregateDesc*)Desc;
            if (MaterialDesc->Size != sizeof(FMaterialAggregateDesc) || strcmp(MaterialDesc->NameAt4, Name) != 0)
            {
                *Instance = nullptr;
                return GR_INTERFACE_UNSUPPORTED;
            }
            if (strcmp(Name, "NotMaterial") == 0)
            {
                *Instance = &GNotMaterial;
                return GR_OK;
            }
            *Instance = (IMaterial*)MakeMaterial(Name, 0, strcmp(Name, "BatchedMaterial") == 0);
            return GR_OK;
        }

    private:
        const char* Name;
    };

    void RegisterOracleMaterials()
    {
        static bool bRegistered = false;
        if (bRegistered)
            return;
        bRegistered = true;
        static const char* const Names[] = {"DcDt",       "DcDtTwo",           "DcDtOcOt", "DcDtEcOcOt",
                                            "BtDetailMapMaterial", "NebulaTwo", "NullMaterial",
                                            "HighGlassMaterial", "Plain", "BadInit", "BatchedMaterial",
                                            "NotMaterial"};
        for (size_t Index = 0; Index < sizeof(Names) / sizeof(Names[0]); ++Index)
        {
            FOracleMaterialFactory* Factory = new FOracleMaterialFactory(Names[Index]);
            NEURONCOMPONENTS_Acquire()->RegisterComponent(Factory, Names[Index], (void*)(uintptr_t)0xFFFFFFFFu);
        }
    }

    bool LoadIni(const std::string& File)
    {
        IProfileParser* Parser = AcquireProfileParser();
        if (Parser == nullptr || Parser->LoadFile(File.c_str(), 0) != GR_OK)
        {
            fprintf(stderr, "material: cannot load %s\n", File.c_str());
            GFailed = true;
            return false;
        }
        return true;
    }

    // --- the renderer of the batcher -------------------------------------------------------

    class FOracleVertexBuffer final : public IRPVertexBuffer
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API CreateVb(unsigned int Fvf, unsigned int Count, unsigned int** Out,
                                                unsigned int Flags) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API DestroyVb(void** Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ResizeVb(void* Handle, unsigned int Fvf, unsigned int Count) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API CopyVertices(void* Handle, unsigned int* OutOffset,
                                                    const FRpVertexSrcDesc* SrcData, unsigned int StartVertex,
                                                    unsigned int Count) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API LockVb(void* Handle, unsigned int* Offset, void** Out,
                                              unsigned int Count) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API UnlockVb(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API UnsupportedVertexBufferOperation(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SelectVb(void* Handle) override
        {
            const unsigned long Id = (unsigned long)(uintptr_t)Handle;
            GSink.Event(Format("\"ev\":\"selvb\",\"h\":\"%lu\"", Id));
            return Id >= 100 ? GR_GENERIC : GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetVbInfo(void* Handle, unsigned int* OutFvf, unsigned int* OutCount) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API IsVbValid(void* Handle) override { return GR_GENERIC; }
    };

    class FOracleIndexBuffer final : public IRPIndexBuffer
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API CreateIb(unsigned int Count, unsigned int** Out, unsigned int Flags) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API DestroyIb(void** Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ResizeIb(void* Handle, unsigned int Count) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API CopyIndices(void* Handle, unsigned int* OutOffset,
                                                   const unsigned short* SrcData, unsigned int Count) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API LockIb(void* Handle, unsigned int* Offset, void** Out,
                                              unsigned int Count) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API UnlockIb(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SelectIb(void* Handle, unsigned int BaseVertexIndex, unsigned int Unused3,
                                                unsigned int Unused4) override
        {
            const unsigned long Id = (unsigned long)(uintptr_t)Handle;
            GSink.Event(Format("\"ev\":\"selib\",\"h\":\"%lu\",\"base\":\"%u\"", Id, BaseVertexIndex));
            return Id >= 100 ? GR_GENERIC : GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetIbCapacity(void* Handle, unsigned int* OutCapacity) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API IsIbValid(void* Handle) override { return GR_GENERIC; }
    };

    // GetPipelineState(7) answers Mode, or fails without writing when bModeFails; the matrix
    // setters write lines. Everything else the batcher never calls.
    class FOracleBatchPipeline final : public IRenderPipeline
    {
    public:
        FOracleVertexBuffer Vb;
        FOracleIndexBuffer Ib;
        int Mode = 1;
        bool bModeFails = false;

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            if (strcmp(Iid, Iids::RPVertexBuffer) == 0)
            {
                *Out = (IRPVertexBuffer*)&Vb;
                return GR_OK;
            }
            if (strcmp(Iid, Iids::RPIndexBuffer) == 0)
            {
                *Out = (IRPIndexBuffer*)&Ib;
                return GR_OK;
            }
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }

        GENRESULT NEURONCOMPONENTS_API GetPipelineState(int Id, int* Out) override
        {
            GSink.Event(Format("\"ev\":\"state\",\"id\":\"%d\"", Id));
            if (bModeFails)
                return GR_GENERIC;
            *Out = Mode;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetWorld(const float* M12) override
        {
            GSink.Event("\"ev\":\"world\",\"m\":\"" + Floats(M12, 12) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetView(const float* M12) override
        {
            GSink.Event("\"ev\":\"view\",\"m\":\"" + Floats(M12, 12) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetModelview(const float* M12) override
        {
            GSink.Event("\"ev\":\"modelview\",\"m\":\"" + Floats(M12, 12) + "\"");
            return GR_OK;
        }

        GENRESULT NEURONCOMPONENTS_API Startup(const char* SectionName) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ShutdownPipeline() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetPipelineState(int Id, int Value) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetDeviceInfo(void* Out520) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API QueryDeviceAbility(unsigned int Index, unsigned long* Out) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API GetDisplayModeCount(unsigned int* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetDisplayModeEnum(unsigned int Index, void* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SelectMode(int* Mode, unsigned int* OutIndex) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API CreateBuffers(HWND Hwnd, int* Mode, int* OutMode) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetModeBlob(void* OutAdapter, void* OutMode10Dwords) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API DestroyGpuResources() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ClearBuffers(unsigned long Flags, const void* Rects) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API SwapBuffers() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API LockBuffer(void* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API UnlockBuffer() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetBufferInterface(void* Out, void* Unused) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetDeviceStats(void* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetViewportRect(int X, int Y, int W, int H) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetViewportRect(int* OutX, int* OutY, int* OutW, int* OutH) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API SetDepthRange(float MinZ, float MaxZ) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetDepthRange(float* OutMinZ, float* OutMaxZ) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetWindow(HWND Hwnd, int X, int Y, int W, int H) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API GetWindow(HWND* OutHwnd, int* OutLeft, int* OutRight, int* OutW,
                                                 int* OutH) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API GetWorld(float* Out12) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetView(float* Out12) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetModelview(float* Out12) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetProjection(const float* M16) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetProjection(float* Out16) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetLookat(float EyeX, float EyeY, float EyeZ, float AtX, float AtY, float AtZ,
                                                 float UpX, float UpY, float UpZ) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API SetOrtho(float Left, float Right, float Bottom, float Top, float NearPlane,
                                                float FarPlane) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API SetPerspective(float FovDeg, float Aspect, float NearPlane,
                                                      float FarPlane) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API SetLight(void* Handle, const D3DLIGHT8* Light) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DestroyLight(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetLight(void* Handle, D3DLIGHT8* OutLight) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetLightEnable(void* Handle, int Enable) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetLightEnable(void* Handle, int* OutEnable) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetMaterial(const D3DMATERIAL8* Material) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetMaterial(D3DMATERIAL8* Material) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API CreateTexture(int Width, int Height, unsigned int* Format, unsigned int Levels,
                                                     unsigned int Flags, void** OutHandle) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API DestroyTexture(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API IsTexture(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API LockTexture(void* Handle, int Level, void* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API UnlockTexture(void* Handle, int Level) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureFormat(void* Handle, int* OutFormat) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureDim(void* Handle, unsigned int* OutW, unsigned int* OutH,
                                                     int* OutLevels) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API GetTextureInterface(void* Handle, void* UnusedA, void* UnusedB) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API SetTextureLevelData(void* Handle, int Level, int SrcW, int SrcH,
                                                           unsigned int Pitch, unsigned int* SrcFormat,
                                                           const void* Pixels, void* A, void* B) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API BlitTexture(void* DstHandle, int DstLevel, void* SrcHandle,
                                                   int SrcLevel) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API SetRenderTarget(void* Handle, void* UnusedA, void* UnusedB) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API GetRenderTarget(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API BeginScene() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API EndScene() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ResetRenderStatesToDefaults() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetRenderState(D3DRENDERSTATETYPE State, int Value) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API GetRenderState(D3DRENDERSTATETYPE State, int* Out) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API SetTextureStageState(int Stage, int Type, int Value) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API GetTextureStageState(int Stage, int Type, int* Out) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API SetTextureStageTransform(int Stage, const float* Matrix16) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API GetTextureStageTransform(int Stage, float* OutMatrix16) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API SetTextureStageTexture(int Stage, void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureStageTexture(int Stage, void** OutHandle) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API VerifyState() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DrawPrimitive(unsigned long Prim, unsigned int Fvf, const void* Verts,
                                                     unsigned int Count, void* Unused) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API DrawIndexedPrimitiveUp(unsigned long Prim, unsigned int Fvf, const void* Verts,
                                                              unsigned int VertCount, const void* Indices,
                                                              unsigned int IndexCount, unsigned long Unused) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API DrawPrimitiveVb(unsigned long Prim, void* Vb, unsigned int StartVertex,
                                                       unsigned int Count, void* Unused) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API DrawIndexedPrimitiveVb(unsigned long Prim, void* Vb, unsigned int BaseVertex,
                                                              unsigned int NumVerts, const void* Indices,
                                                              unsigned int IndexCount, unsigned long Unused) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API AddLight(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API RemoveLight(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API UpdateLight(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetWorldN(int WorldIndex, const float* M12) override { return GR_GENERIC; }
    };

    // The aggregate outer StartupFromOuter asks for the renderer.
    class FOracleOuter final : public INeuronComponent
    {
    public:
        FOracleBatchPipeline* Pipeline = nullptr;

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            if (strcmp(Iid, Iids::RenderPipeline8B) == 0)
            {
                *Out = (IRenderPipeline*)Pipeline;
                return GR_OK;
            }
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
    };

    class FOracleEvents final : public IMaterialBatcherEvents
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        void NEURONCOMPONENTS_API BeginFlush(unsigned char Clear) override
        {
            GSink.Event(Format("\"ev\":\"bflush\",\"clear\":\"%u\"", Clear));
        }
        void NEURONCOMPONENTS_API EndFlush(unsigned char Clear) override
        {
            GSink.Event(Format("\"ev\":\"eflush\",\"clear\":\"%u\"", Clear));
        }
        void NEURONCOMPONENTS_API BeginRecord(void* Material, void* FieldA, void* FieldB) override
        {
            GSink.Event(Format("\"ev\":\"brec\",\"mat\":\"%lu\",\"tag\":\"%lu\",\"ctx\":\"%lu\"",
                               ((FOracleMaterial*)(IMaterial*)Material)->Serial, (unsigned long)(uintptr_t)FieldA,
                               (unsigned long)(uintptr_t)FieldB));
        }
        void NEURONCOMPONENTS_API EndRecord(void* Material, void* FieldA, void* FieldB) override
        {
            GSink.Event(Format("\"ev\":\"erec\",\"mat\":\"%lu\",\"tag\":\"%lu\",\"ctx\":\"%lu\"",
                               ((FOracleMaterial*)(IMaterial*)Material)->Serial, (unsigned long)(uintptr_t)FieldA,
                               (unsigned long)(uintptr_t)FieldB));
        }
    };

    FOracleBatchPipeline GPipeline;
    FOracleOuter GOuter;
    FOracleEvents GEvents;

    // A batcher as the factory and Startup leave it, on the recording renderer.
    FMaterialBatcher* StartBatcher()
    {
        FMaterialBatcher* Batcher = new FMaterialBatcher();
        GOuter.Pipeline = &GPipeline;
        FMaterialAggregateDesc Desc = {};
        Desc.Size = sizeof(Desc);
        Desc.NameAt4 = "MaterialBatcher";
        Desc.OuterAt8 = &GOuter;
        Batcher->InitializeFromDescriptor(&Desc);
        if (Batcher->StartupFromOuter() != GR_OK)
        {
            fprintf(stderr, "material: batcher startup failed\n");
            GFailed = true;
        }
        return Batcher;
    }

    std::string Serial(IMaterial* Material)
    {
        if (Material == nullptr)
            return "-";
        return Format("%lu", ((FOracleMaterial*)Material)->Serial);
    }

    std::string MaterialFields(IMaterial* Material)
    {
        if (Material == nullptr)
            return ",\"mat\":\"-\",\"mrefs\":\"-\"";
        return ",\"mat\":\"" + Serial(Material) + Format("\",\"mrefs\":\"%lu\"", ((FOracleMaterial*)Material)->RefCount);
    }

    void BatchRuleLines(unsigned int Case, const FMaterialBatcher* Batcher, Suite* Out)
    {
        for (size_t Index = 0; Index < Batcher->RuleList.size(); ++Index)
        {
            const FBatchRule& Rule = Batcher->RuleList[Index];
            Out->Line(Format("{\"case\":\"%u\",\"brule\":\"%u\",\"kind\":\"%lu\",\"pat\":\"", Case, (unsigned int)Index,
                             Rule.Kind) +
                      HexC(Rule.Pattern.c_str()) + Format("\",\"ok\":\"%d\"}", Rule.Expression != nullptr ? 1 : 0));
        }
    }

    // --- material_library ----------------------------------------------------------------

    class FLibraryCase
    {
    public:
        FLibraryCase(Suite* InOut, unsigned int InCase) : Out(InOut), Case(InCase) { StartCase(InOut, InCase); }

        // Libraries and batchers are abandoned with their materials.
        FMaterialLibrary* L()
        {
            if (Library != nullptr)
                return Library;
            if (Batcher != nullptr)
                return &Batcher->Library;
            fprintf(stderr, "material_library: no library\n");
            GFailed = true;
            exit(1);
        }

        void Line(unsigned int Op, GENRESULT Result, const std::string& Extra)
        {
            Out->Line(Format("{\"case\":\"%u\",\"op\":\"%u\",\"r\":\"%ld\"", Case, Op, (long)Result) + Extra + "}");
        }

        IMaterial* Fake(const std::string& Text)
        {
            const unsigned long Index = strtoul(Text.c_str() + 1, nullptr, 10);
            return (IMaterial*)&GPool[Index - 1];
        }

        unsigned long Crc(const std::string& Name) { return NEURONCOMPONENTS_CRC::GetCRC32(Name.c_str()); }

        void Dump()
        {
            FMaterialLibrary* Lib = L();
            for (std::map<unsigned long, FCrcSlot>::const_iterator It = Lib->CrcMap.begin(); It != Lib->CrcMap.end();
                 ++It)
            {
                Out->Line(Format("{\"case\":\"%u\",\"crc\":\"%08lx\",\"refs\":\"%lu\"", Case, It->first,
                                 It->second.RefCount) +
                          MaterialFields(It->second.Material) + "}");
            }
            for (std::map<unsigned long, FMaterialGroup>::const_iterator It = Lib->Groups.begin();
                 It != Lib->Groups.end(); ++It)
            {
                std::string Members;
                for (size_t Index = 0; Index < It->second.Members.size(); ++Index)
                    Members += Format(Index == 0 ? "%08lx" : ",%08lx", It->second.Members[Index]);
                Out->Line(Format("{\"case\":\"%u\",\"group\":\"%lu\",\"gid\":\"%lu\",\"name\":\"", Case, It->first,
                                 It->second.Id) +
                          (It->second.bHasName ? HexC(It->second.Name.c_str()) : std::string("-")) +
                          "\",\"members\":\"" + Members + "\"}");
            }
            for (size_t Index = 0; Index < Lib->MapRules.size(); ++Index)
            {
                const FMaterialMapRule& Rule = Lib->MapRules[Index];
                Out->Line(Format("{\"case\":\"%u\",\"rule\":\"%u\",\"key\":\"%d\",\"name\":\"%d\",\"pat\":\"", Case,
                                 (unsigned int)Index, Rule.Key, Rule.bNamePattern ? 1 : 0) +
                          HexC(Rule.Pattern.c_str()) + "\",\"type\":\"" + HexC(Rule.Type.c_str()) +
                          Format("\",\"ok\":\"%d\"}", Rule.Expression != nullptr ? 1 : 0));
            }
            if (Batcher != nullptr)
                BatchRuleLines(Case, Batcher, Out);
        }

        void Run(unsigned int Op, const std::vector<std::string>& F)
        {
            const std::string& K = F[0];
            if (K == "library")
            {
                Library = new FMaterialLibrary();
                Line(Op, GR_OK, "");
            }
            else if (K == "batcher")
            {
                Batcher = StartBatcher();
                Line(Op, GR_OK, "");
            }
            else if (K == "ini")
                Line(Op, LoadIni(F[1]) ? GR_OK : GR_GENERIC, "");
            else if (K == "parsemap")
                Line(Op, L()->ParseMaterialMap(), "");
            else if (K == "rule")
                Line(Op, L()->AddMapRule((int)Int(F[1]), F[2].c_str(), F[3].c_str()), "");
            else if (K == "findrule" || K == "remap")
            {
                char Type[0x200];
                Type[0] = '\0';
                const GENRESULT Result = K == "findrule"
                                             ? L()->FindMapRule((int)Int(F[1]), F[2].c_str(), nullptr, Type)
                                             : L()->RemapType(F[1].c_str(), F[2].c_str(), nullptr, Type);
                Line(Op, Result, ",\"type\":\"" + (Result >= 0 ? HexC(Type) : std::string()) + "\"");
            }
            else if (K == "unrule")
                Line(Op, L()->RemoveMapRule((int)Int(F[1]), F[2].c_str()), "");
            else if (K == "resolve")
            {
                const char* Type = L()->ResolveType(F[1].c_str(), F[2] == "-" ? nullptr : F[2].c_str());
                Line(Op, GR_OK, ",\"type\":\"" + HexC(Type) + "\"");
            }
            else if (K == "brule")
                Line(Op, Batcher->AddRule((unsigned long)Int(F[1]), F[2].c_str()), "");
            else if (K == "bunrule")
                Line(Op, Batcher->RemoveRule((unsigned long)Int(F[1]), F[2].c_str()), "");
            else if (K == "bclear")
                Line(Op, Batcher->ClearRules(), "");
            else if (K == "create" || K == "generic")
            {
                const unsigned char bRemap = Int(F[3]) != 0 ? 1 : 0;
                FMaterialLibraryRecord* Record =
                    K == "create" ? L()->CreateAfterMap(F[1].c_str(), F[2].c_str(), bRemap, nullptr)
                                  : L()->CreateGenericRecord(F[1].c_str(), F[2].c_str(), bRemap, nullptr);
                std::string Text = ",\"rec\":\"-\"";
                for (std::map<unsigned long, FCrcSlot>::iterator It = L()->CrcMap.begin(); It != L()->CrcMap.end();
                     ++It)
                {
                    if (Record == &It->second)
                        Text = Format(",\"rec\":\"%08lx\"", It->first) + MaterialFields(Record->Material);
                }
                Line(Op, GR_OK, Text);
            }
            else if (K == "foc" || K == "find" || K == "enumm")
            {
                IMaterial* Material = (IMaterial*)(uintptr_t)kUntouched;
                GENRESULT Result;
                if (K == "foc")
                    Result = L()->FindOrCreate(F[1].c_str(), F[2].c_str(), nullptr, &Material);
                else if (K == "find")
                {
                    const unsigned long Query[2] = {0, Crc(F[1])};
                    Result = L()->FindMaterialByCrc((void*)Query, &Material);
                }
                else
                    Result = L()->EnumGroupMaterial((unsigned long)Int(F[1]), (unsigned long)Int(F[2]), &Material);
                Line(Op, Result, Result >= 0 ? MaterialFields(Material) : std::string(",\"mat\":\"-\""));
            }
            else if (K == "group")
            {
                unsigned long Id = kUntouched;
                const GENRESULT Result = L()->AllocateGroup(&Id);
                Line(Op, Result, Format(",\"v\":\"%lu\"", Id));
            }
            else if (K == "gdel")
                Line(Op, L()->DeleteGroup((int)Int(F[1])), "");
            else if (K == "gname")
                Line(Op, L()->SetGroupName((int)Int(F[1]), F[2] == "-" ? nullptr : F[2].c_str()), "");
            else if (K == "getname")
            {
                char Buffer[256];
                Buffer[0] = '\0';
                const GENRESULT Result = L()->GetGroupName((int)Int(F[1]), Buffer, (unsigned long)Int(F[2]));
                Line(Op, Result, ",\"text\":\"" + (Result >= 0 ? HexC(Buffer) : std::string()) + "\"");
            }
            else if (K == "gcount")
            {
                unsigned long Value = kUntouched;
                const GENRESULT Result = L()->GetGroupCount(&Value);
                Line(Op, Result, Format(",\"v\":\"%lu\"", Value));
            }
            else if (K == "enumg")
            {
                unsigned long Value = kUntouched;
                const GENRESULT Result = L()->EnumGroup((unsigned long)Int(F[1]), &Value);
                Line(Op, Result, Format(",\"v\":\"%lu\"", Value));
            }
            else if (K == "mcount")
            {
                int Value = -7;
                const GENRESULT Result = L()->GetGroupMaterialCount((unsigned long)Int(F[1]), &Value);
                Line(Op, Result, Format(",\"v\":\"%d\"", Value));
            }
            else if (K == "add")
            {
                IMaterial* Material = nullptr;
                if (F[3].compare(0, 4, "new:") == 0)
                    Material = (IMaterial*)MakeMaterial(F[3].c_str() + 4, 0, false);
                else if (F[3] != "-")
                    Material = Fake(F[3]);
                const unsigned long Record[2] = {0, Crc(F[2])};
                Line(Op, L()->AddMaterial((int)Int(F[1]), (void*)Record, Material), "");
            }
            else if (K == "remove")
            {
                const unsigned long Record[2] = {0, Crc(F[2])};
                Line(Op, L()->RemoveMaterial((unsigned long)Int(F[1]), (void*)Record), "");
            }
            else if (K == "verify")
                Line(Op, L()->ApplyVerifyGroup((int)Int(F[1]), nullptr, nullptr), "");
            else if (K == "resolvet")
                Line(Op, L()->ResolveTexturesGroup((int)Int(F[1]), Bits(F[2])), "");
            else if (K == "release")
            {
                const unsigned long Value = Fake(F[1])->Release();
                Line(Op, GR_OK, Format(",\"v\":\"%lu\"", Value));
            }
            else if (K == "addref")
            {
                const unsigned long Value = Fake(F[1])->AddRef();
                Line(Op, GR_OK, Format(",\"v\":\"%lu\"", Value));
            }
            else if (K == "unsup")
                Line(Op, L()->UnsupportedOperation(nullptr, nullptr), "");
            else if (K == "dump")
                Dump();
            else
            {
                fprintf(stderr, "material_library: unknown operation %s\n", K.c_str());
                GFailed = true;
            }
        }

    private:
        Suite* Out;
        unsigned int Case;
        FMaterialLibrary* Library = nullptr;
        FMaterialBatcher* Batcher = nullptr;
    };

    void LibrarySuite()
    {
        Suite Out("material_library.jsonl");
        const std::vector<std::string> Lines = InputLines("material_library.list");
        for (unsigned int Case = 0; Case < Lines.size(); ++Case)
        {
            FLibraryCase Runner(&Out, Case);
            const std::vector<std::string> Ops = Split(Lines[Case], " ; ");
            for (unsigned int Op = 0; Op < Ops.size(); ++Op)
                Runner.Run(Op, Split(Ops[Op], " "));
        }
        GSink.Out = nullptr;
    }

    // --- material_global -------------------------------------------------------------------

    void GlobalSuite()
    {
        Suite Out("material_global.jsonl");
        FMaterialLibrary Library;
        const std::vector<std::string> Lines = InputLines("material_global.list");
        for (unsigned int Case = 0; Case < Lines.size(); ++Case)
        {
            const std::vector<std::string> Ops = Split(Lines[Case], " ; ");
            for (unsigned int Op = 0; Op < Ops.size(); ++Op)
            {
                const std::vector<std::string> F = Split(Ops[Op], " ");
                const std::string Head = Format("{\"case\":\"%u\",\"op\":\"%u\"", Case, Op);
                const std::string& K = F[0];
                if (K == "set" || K == "setv")
                {
                    const unsigned long Value = strtoul(F[2].c_str(), nullptr, 10);
                    const unsigned long Index = strtoul(F[1].c_str(), nullptr, 10);
                    const GENRESULT Result = K == "set" ? Library.SetGlobalRecord(Index, &Value)
                                                        : Library.SetGlobalRecordValue(Index, &Value);
                    Out.Line(Head + Format(",\"r\":\"%ld\"}", (long)Result));
                }
                else if (K == "get")
                {
                    unsigned long Value = kUntouched;
                    const GENRESULT Result = Library.GetGlobalRecordValue(strtoul(F[1].c_str(), nullptr, 10), &Value);
                    Out.Line(Head + Format(",\"r\":\"%ld\",\"v\":\"%lu\"}", (long)Result, Value));
                }
                else if (K == "value")
                    Out.Line(Head + Format(",\"v\":\"%lu\"}",
                                           GetMaterialGlobalRecordValue(strtoul(F[1].c_str(), nullptr, 10))));
                else if (K == "act")
                {
                    ActivateMaterialGlobalRecord(strtoul(F[1].c_str(), nullptr, 10));
                    Out.Line(Head + "}");
                }
                else if (K == "init")
                {
                    InitializeMaterialGlobalRecords();
                    Out.Line(Head + "}");
                }
                else
                {
                    fprintf(stderr, "material_global: unknown operation %s\n", K.c_str());
                    GFailed = true;
                }
            }
        }
    }

    // --- material_animation ----------------------------------------------------------------

    std::vector<unsigned long> HexWords(const std::string& Text)
    {
        std::vector<unsigned long> Words;
        if (Text.empty())
            return Words;
        const std::vector<std::string> Parts = Split(Text, "/");
        for (size_t Index = 0; Index < Parts.size(); ++Index)
            Words.push_back(Hex(Parts[Index]));
        return Words;
    }

    // NAME:COUNT:FLAGS:DELTAS:KEYS, the deltas and keys as hex floats separated by '/'. The key
    // block is allocated as ParseMaKeys allocates it and filled from the words.
    FImaData* MakeAnimation(const std::string& Spec)
    {
        const std::vector<std::string> Parts = Split(Spec, ":");
        FImaData* Data = new FImaData();
        Data->NameCrc = NEURONCOMPONENTS_CRC::GetCRC32(Parts[0].c_str());
        FMaKeyBlock* Block = Data->KeyBlock();
        Block->Count = (unsigned long)Int(Parts[1]);
        Block->Flags = (unsigned long)Int(Parts[2]);
        const std::vector<unsigned long> Deltas = HexWords(Parts[3]);
        const std::vector<unsigned long> Keys = HexWords(Parts[4]);
        if (Block->Count != 0)
        {
            Block->Deltas = (FMaDelta*)operator new(Block->Count * 0x14);
            memcpy(Block->Deltas, Deltas.data(), Deltas.size() * 4 < Block->Count * 0x14 ? Deltas.size() * 4
                                                                                        : Block->Count * 0x14);
        }
        if (Block->Count > 1)
        {
            Block->Keys = (FMaKey*)operator new((Block->Count - 1) << 4);
            memcpy(Block->Keys, Keys.data(),
                   Keys.size() * 4 < ((Block->Count - 1) << 4) ? Keys.size() * 4 : ((Block->Count - 1) << 4));
        }
        return Data;
    }

    class FAnimationCase
    {
    public:
        FAnimationCase(Suite* InOut, unsigned int InCase) : Out(InOut), Case(InCase), Library(new FMaterialAnimation()) {}

        // Every instance this case made and still holds is released: the process list then
        // holds nothing of it. The library is abandoned.
        ~FAnimationCase()
        {
            for (size_t Index = 0; Index < Instances.size(); ++Index)
            {
                while (Instances[Index] != nullptr && Live[Index])
                {
                    if (Instances[Index]->Release() == 0)
                        Live[Index] = false;
                }
            }
        }

        std::string Tag(FImaData* Data)
        {
            std::map<FImaData*, std::string>::const_iterator Found = Tags.find(Data);
            return Found == Tags.end() ? std::string("?") : Found->second;
        }

        FImaInstance* Inst(const std::string& Text) { return Instances[(size_t)Int(Text)]; }

        void Run(unsigned int Op, const std::vector<std::string>& F)
        {
            const std::string Head = Format("{\"case\":\"%u\",\"op\":\"%u\"", Case, Op);
            const std::string& K = F[0];
            if (K == "group")
            {
                std::vector<FImaData*> Data;
                const std::vector<std::string> Specs = Split(F[1], ",");
                for (size_t Index = 0; Index < Specs.size(); ++Index)
                {
                    if (!Specs[Index].empty())
                        Data.push_back(MakeAnimation(Specs[Index]));
                }
                void* Group = OracleAddAnimationGroup(Library, Data);
                const unsigned int G = (unsigned int)Groups.size();
                Groups.push_back(Group);
                if (Group != nullptr)
                {
                    for (size_t Index = 0; Index < Data.size(); ++Index)
                        Tags[Data[Index]] = Format("%u.%u", G, (unsigned int)Index);
                }
                Out->Line(Head + ",\"v\":\"" + (Group != nullptr ? Format("%u", G) : std::string("-")) + "\"}");
            }
            else if (K == "unload")
            {
                const GENRESULT Result = Library->UnloadAnimationGroup(Groups[(size_t)Int(F[1])]);
                Out->Line(Head + Format(",\"r\":\"%ld\"}", (long)Result));
            }
            else if (K == "inst")
            {
                const unsigned long Query[2] = {0, NEURONCOMPONENTS_CRC::GetCRC32(F[1].c_str())};
                IImaInstance* Instance = nullptr;
                const GENRESULT Result = Library->CreateAnimationInstance(0, (void*)Query, &Instance);
                if (Result >= 0)
                {
                    Instances.push_back((FImaInstance*)Instance);
                    Live.push_back(true);
                    Out->Line(Head + Format(",\"r\":\"0\",\"v\":\"%u\"}", (unsigned int)Instances.size() - 1));
                }
                else
                    Out->Line(Head + Format(",\"r\":\"%ld\",\"v\":\"-\"}", (long)Result));
            }
            else if (K == "release")
            {
                const unsigned long Value = Inst(F[1])->Release();
                if (Value == 0)
                    Live[(size_t)Int(F[1])] = false;
                Out->Line(Head + Format(",\"v\":\"%lu\"}", Value));
            }
            else if (K == "addref")
                Out->Line(Head + Format(",\"v\":\"%lu\"}", Inst(F[1])->AddRef()));
            else if (K == "tick")
            {
                const GENRESULT Result = Library->TickAnimationInstances((int)Int(F[1]), Bits(F[2]));
                Out->Line(Head + Format(",\"r\":\"%ld\"}", (long)Result));
            }
            else if (K == "itick")
            {
                Inst(F[1])->Tick(Bits(F[2]));
                Out->Line(Head + "}");
            }
            else if (K == "reset")
            {
                Inst(F[1])->ResetTime();
                Out->Line(Head + "}");
            }
            else if (K == "uv")
            {
                float Matrix[16];
                const unsigned char Result = Inst(F[1])->FillUvMatrix(Matrix);
                Out->Line(Head + Format(",\"r\":\"%u\",\"m\":\"", Result) + Floats(Matrix, 16) + "\"}");
            }
            else if (K == "state")
            {
                FImaInstance* Instance = Inst(F[1]);
                Out->Line(Head + Format(",\"frame\":\"%lu\",\"time\":\"%08x\",\"refs\":\"%lu\",\"data\":\"",
                                        Instance->Frame, FloatBits(Instance->Time), Instance->RefCount) +
                          Tag(Instance->Data) + Format("\",\"drefs\":\"%lu\"}", Instance->Data->RefCount));
            }
            else if (K == "map")
            {
                for (size_t Index = 0; Index < Library->CrcMap.size(); ++Index)
                {
                    const FCrcAnimSlot& Slot = Library->CrcMap[Index];
                    Out->Line(Format("{\"case\":\"%u\",\"crc\":\"%08lx\",\"data\":\"", Case, Slot.Crc) + Tag(Slot.Data) +
                              Format("\",\"drefs\":\"%lu\"}", Slot.Data->RefCount));
                }
            }
            else
            {
                fprintf(stderr, "material_animation: unknown operation %s\n", K.c_str());
                GFailed = true;
            }
        }

    private:
        Suite* Out;
        unsigned int Case;
        FMaterialAnimation* Library;
        std::vector<void*> Groups;
        std::vector<FImaInstance*> Instances;
        std::vector<bool> Live;
        std::map<FImaData*, std::string> Tags;
    };

    void AnimationSuite()
    {
        Suite Out("material_animation.jsonl");
        const std::vector<std::string> Lines = InputLines("material_animation.list");
        for (unsigned int Case = 0; Case < Lines.size(); ++Case)
        {
            FAnimationCase Runner(&Out, Case);
            const std::vector<std::string> Ops = Split(Lines[Case], " ; ");
            for (unsigned int Op = 0; Op < Ops.size(); ++Op)
                Runner.Run(Op, Split(Ops[Op], " "));
        }
    }

    // --- material_batcher ------------------------------------------------------------------

    // A vertex source: positions only (stride and count as given), the index array optional.
    struct FSource
    {
        std::vector<unsigned long> Floats;
        std::vector<unsigned long> Remap;
        FRpVertexSrcDesc Desc;
    };

    class FBatcherCase
    {
    public:
        FBatcherCase(Suite* InOut, unsigned int InCase) : Out(InOut), Case(InCase)
        {
            StartCase(InOut, InCase);
            GPipeline.Mode = 1;
            GPipeline.bModeFails = false;
        }

        // The batcher is abandoned with its queues (and the sources, index arrays and matrices
        // they point to, which are never freed); its events sink is cleared first.
        ~FBatcherCase()
        {
            if (Batcher != nullptr)
                Batcher->SetEvents(nullptr);
        }

        void Bins()
        {
            for (unsigned int Index = 0; Index < 2; ++Index)
            {
                const FBatchBin* Bin = Batcher->Bins[Index].get();
                std::string Queue;
                for (std::map<IMaterial*, std::list<FBatchQueuedRecord>>::const_iterator It = Bin->Queue.begin();
                     It != Bin->Queue.end(); ++It)
                {
                    Queue += Format(Queue.empty() ? "%lu:%u" : ",%lu:%u", ((FOracleMaterial*)It->first)->Serial,
                                    (unsigned int)It->second.size());
                }
                Out->Line(Format("{\"case\":\"%u\",\"bin\":\"%u\",\"key\":\"%ld\",\"sort\":\"%lu\",\"records\":\"%lu\","
                                 "\"queue\":\"",
                                 Case, Index, Bin->Pad0, Bin->SortKind, Bin->RecordCount) +
                          Queue + "\"}");
            }
            unsigned int Selected = 9;
            for (unsigned int Index = 0; Index < 2; ++Index)
            {
                if (Batcher->SelectedBin == Batcher->Bins[Index].get())
                    Selected = Index;
            }
            Out->Line(Format("{\"case\":\"%u\",\"order\":\"%ld,%ld\",\"selected\":\"%u\",\"tag\":\"%lu\","
                             "\"tagctx\":\"%lu\"}",
                             Case, (long)Batcher->BinOrder[0], (long)Batcher->BinOrder[1], Selected,
                             (unsigned long)(uintptr_t)Batcher->Field78, (unsigned long)(uintptr_t)Batcher->Field7C));
            BatchRuleLines(Case, Batcher, Out);
        }

        void Run(unsigned int Op, const std::vector<std::string>& F)
        {
            const std::string Head = Format("{\"case\":\"%u\",\"op\":\"%u\"", Case, Op);
            const std::string& K = F[0];
            GENRESULT Result = GR_OK;
            if (K == "startup")
            {
                LoadIni(F[1]);
                Batcher = StartBatcher();
                Out->Line(Head + Format(",\"r\":\"0\",\"size\":\"%lu\",\"bin1\":\"%lu\"}", Batcher->BufferSize,
                                        Batcher->Bin1SortOrder));
                return;
            }
            if (K == "mat")
            {
                FOracleMaterial* Material = MakeMaterial("Q", Hex(F[1]), Int(F[2]) != 0);
                Out->Line(Head + Format(",\"v\":\"%lu\"}", Material->Serial));
                return;
            }
            if (K == "create")
            {
                FMaterialLibraryRecord* Record = Batcher->Library.CreateAfterMap(F[1].c_str(), F[2].c_str(), 1, nullptr);
                std::string Text = ",\"rec\":\"-\"";
                for (std::map<unsigned long, FCrcSlot>::iterator It = Batcher->Library.CrcMap.begin();
                     It != Batcher->Library.CrcMap.end(); ++It)
                {
                    if (Record == &It->second)
                        Text = Format(",\"rec\":\"%08lx\",\"mat\":\"", It->first) + Serial(Record->Material) + "\"";
                }
                Out->Line(Head + Text + "}");
                return;
            }
            if (K == "vsrc")
            {
                FSource* Source = new FSource();
                const std::vector<std::string> Words = Split(F[4], ",");
                for (size_t Index = 0; Index < Words.size(); ++Index)
                    Source->Floats.push_back(Hex(Words[Index]));
                if (F.size() > 5)
                {
                    const std::vector<std::string> Remap = Split(F[5], ",");
                    for (size_t Index = 0; Index < Remap.size(); ++Index)
                        Source->Remap.push_back((unsigned long)Int(Remap[Index]));
                }
                memset(&Source->Desc, 0, sizeof(Source->Desc));
                Source->Desc.Position.Base = (const unsigned char*)Source->Floats.data();
                Source->Desc.Position.ByteSize = 12;
                Source->Desc.Position.Stride = (unsigned int)Int(F[3]);
                Source->Desc.Position.IndexArray = Source->Remap.empty() ? nullptr : Source->Remap.data();
                Source->Desc.Position.Count = (unsigned int)Int(F[2]);
                Source->Desc.SrcFvf = 2;
                Source->Desc.SrcCount = (unsigned int)Int(F[2]);
                Sources[F[1]] = Source;
                return;
            }
            if (K == "idx")
            {
                std::vector<unsigned short>& Indices = IndexArrays[F[1]];
                const std::vector<std::string> Words = Split(F[2], ",");
                for (size_t Index = 0; Index < Words.size(); ++Index)
                    Indices.push_back((unsigned short)Int(Words[Index]));
                return;
            }
            if (K == "xf")
            {
                std::vector<float>& Matrix = Matrices[F[1]];
                const std::vector<std::string> Words = Split(F[2], ",");
                for (size_t Index = 0; Index < Words.size(); ++Index)
                    Matrix.push_back(Bits(Words[Index]));
                return;
            }
            if (K == "mode")
            {
                GPipeline.bModeFails = F[1] == "none";
                GPipeline.Mode = GPipeline.bModeFails ? 1 : (int)Int(F[1]);
                return;
            }
            if (K == "bins")
            {
                Bins();
                return;
            }
            if (K == "events")
                Result = Batcher->SetEvents(F[1] == "on" ? (void*)&GEvents : nullptr);
            else if (K == "sub")
            {
                // mat prim start count ibs span flags depth vsrc idx world view vb ib
                ++Submissions;
                FMaterialSubmitContext* Context = new FMaterialSubmitContext();
                FVbListing* Listing = new FVbListing();
                FIbScratch* Scratch = new FIbScratch();
                const std::vector<std::string> Vb = Split(F[13], ",");
                const std::vector<std::string> Ib = Split(F[14], ",");
                Listing->VertexBuffer = (void*)(uintptr_t)Int(Vb[0]);
                Listing->Fvf = (unsigned long)Int(Vb[1]);
                Listing->BaseVertex = (unsigned long)Int(Vb[2]);
                Scratch->IndexBuffer = (void*)(uintptr_t)Int(Ib[0]);
                Scratch->Bias = (unsigned long)Int(Ib[1]);
                Context->Inst = (void*)(uintptr_t)Submissions;
                Context->World = Matrices[F[11]].data();
                Context->View = Matrices[F[12]].data();
                Context->SourceVertices = &Sources[F[9]]->Desc;
                Context->Indices = IndexArrays[F[10]].data();
                Context->VbListing = Listing;
                Context->IbScratch = Scratch;
                Context->ReplacedMaterial = nullptr;
                Context->Field20 = nullptr;
                const unsigned long Index = strtoul(F[1].c_str() + 1, nullptr, 10);
                Result = Batcher->Submit((IMaterial*)&GPool[Index - 1], Context, (unsigned long)Int(F[2]),
                                         (unsigned long)Int(F[3]), (unsigned long)Int(F[4]), (unsigned long)Int(F[5]),
                                         (unsigned long)Int(F[6]), (void*)(uintptr_t)Hex(F[8]), Hex(F[7]));
            }
            else if (K == "rule")
                Result = Batcher->AddRule((unsigned long)Int(F[1]), F[2].c_str());
            else if (K == "unrule")
                Result = Batcher->RemoveRule((unsigned long)Int(F[1]), F[2].c_str());
            else if (K == "clearrules")
                Result = Batcher->ClearRules();
            else if (K == "select")
                Result = Batcher->SelectBin((unsigned long)Int(F[1]));
            else if (K == "sort")
                Result = Batcher->SetBinSortKind((unsigned long)Int(F[1]), (void*)(uintptr_t)Int(F[2]));
            else if (K == "order")
                Result = Batcher->SetBinOrder((unsigned long)Int(F[1]), Int(F[2]));
            else if (K == "flushbin")
                Result = Batcher->FlushBin((unsigned long)Int(F[1]), (unsigned char)Int(F[2]));
            else if (K == "flush")
                Result = Batcher->FlushAll((unsigned char)Int(F[1]));
            else if (K == "clearbins")
                Result = Batcher->ClearBins();
            else if (K == "noop")
                Result = Batcher->NoOp(nullptr);
            else if (K == "tag")
                Result = Batcher->SetRecordTag((void*)(uintptr_t)Int(F[1]));
            else if (K == "tagctx")
                Result = Batcher->SetRecordTagContext((void*)(uintptr_t)Int(F[1]));
            else
            {
                fprintf(stderr, "material_batcher: unknown operation %s\n", K.c_str());
                GFailed = true;
                return;
            }
            Out->Line(Head + Format(",\"r\":\"%ld\"}", (long)Result));
        }

    private:
        Suite* Out;
        unsigned int Case;
        FMaterialBatcher* Batcher = nullptr;
        unsigned long Submissions = 0;
        std::map<std::string, FSource*> Sources;
        std::map<std::string, std::vector<unsigned short>>& IndexArrays = *new std::map<std::string, std::vector<unsigned short>>();
        std::map<std::string, std::vector<float>>& Matrices = *new std::map<std::string, std::vector<float>>();
    };

    void BatcherSuite()
    {
        Suite Out("material_batcher.jsonl");
        const std::vector<std::string> Lines = InputLines("material_batcher.list");
        for (unsigned int Case = 0; Case < Lines.size(); ++Case)
        {
            FBatcherCase Runner(&Out, Case);
            const std::vector<std::string> Ops = Split(Lines[Case], " ; ");
            for (unsigned int Op = 0; Op < Ops.size(); ++Op)
                Runner.Run(Op, Split(Ops[Op], " "));
        }
        GSink.Out = nullptr;
    }
}

// oracle_material_kinds.cpp: the 22 material kinds and BatchedMaterial (material_kinds.jsonl).
void RunMaterialKindSuite();
// oracle_material_surface.cpp: the 19 surface materials (material_surface.jsonl).
void RunMaterialSurfaceSuite();
// oracle_mesh_render.cpp (compiled into oracle_mesh_data.cpp): RendComp at run time
// (mesh_render.jsonl).
void RunMeshRenderSuite();

void RunMaterialSuites()
{
    RegisterOracleMaterials();
    if (!OracleLoadMathEngine())
    {
        fprintf(stderr, "neuron-client: no 3DMathEngine for the material batcher\n");
        GFailed = true;
        return;
    }
    RegexSuite();
    GlobalSuite();
    LibrarySuite();
    AnimationSuite();
    BatcherSuite();
    RunMaterialKindSuite();
    RunMaterialSurfaceSuite();
    RunMeshRenderSuite();
}
