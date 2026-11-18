// P4.4: the texture library at run time (FTextureLibrary of Shading.dll): groups, playbacks
// and their animation, reference counts, garbage collection and the record slots of
// ITextureLibrary2, through the reference's own code. TextureLibrary.cpp is compiled into
// oracle_shading.cpp (work package P1.7); this file uses the class's public members only. The
// Rust side is crates/neuron-client/tests/oracle_vectors.rs with
// crates/neuron-client/tests/support/texture.rs; the inputs come from
// crates/neuron-client/tests/fixtures/oracle/make_texture_fixtures.py.
//
// texture_runtime.list holds one case per line: operations separated by " ; ", their fields by
// single spaces. Each case starts with a new library (generic slot 0 set to 1, as Initialize
// sets it from the default TEXTURE_LOD_LOAD) on the recording renderer of oracle_texture.h.
// Records are named by the name their CRC finds in the canonical map; "-" is a null record.
// Records are made by "tex" (CreateTextureRecord) and "anim" (a record of several frames as
// LoadLegacyTextureRecord leaves one); nothing is loaded from files here, P1.7's suites cover
// loading. Floats are given and written as eight hex digits of the f32, doubles as sixteen.
// Outputs a callee may leave alone start as 0xCDCDCDCD (or -7 for ids and counts).
//
// Line formats, each prefixed with "case":
//   every operation  {"op","r"} plus its outputs ("id", "v", "h" and "uv", "text", "rec")
//   renderer calls   {"ev":"destroy","handle"} (oracle_texture.h)
//   "dump"           {"rec","handle","refs","rate","frames","children"} per canonical record in
//                    map order, {"pb","src","cur","count","el","dur","dir","wrap","refs","local",
//                    "use"} per playback, {"active"}, {"group","name","members"} per group

#include "oracle_texture.h"

#include <stdlib.h>

#include <string>
#include <vector>

#include "../Shading/TextureLibrary.h"

using namespace oracle;

// oracle_shading.cpp: FormatMapLoadDefaults of TextureLibrary.cpp's anonymous namespace.
void OracleTextureFormatMapDefaults();

namespace
{
    const unsigned int kUntouched = 0xCDCDCDCDu;

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

    std::string Name(const FTextureEntry* Entry)
    {
        return Entry == nullptr ? std::string("-") : HexC(Entry->GetName());
    }

    std::string Ints(const std::vector<int>& Values)
    {
        std::string Out;
        for (size_t Index = 0; Index < Values.size(); ++Index)
            Out += Format(Index == 0 ? "%d" : ",%d", Values[Index]);
        return Out;
    }

    std::string FrameFields(const unsigned long* Out)
    {
        return Format(",\"h\":\"%lu\",\"uv\":\"%08lx,%08lx,%08lx,%08lx\"", Out[0], Out[1], Out[2], Out[3],
                      Out[4]);
    }

    class FRuntimeCase
    {
    public:
        FRuntimeCase(Suite* InOut, unsigned int InCase)
            : Out(InOut), Case(InCase), Pipeline(InOut, InCase, 0, 256, 256), Library(new FTextureLibrary())
        {
            Library->Pipeline = &Pipeline;
            Library->GenericSlots[0] = 1;
        }

        // The library is abandoned with its records, as oracle_shading.cpp abandons its own.
        ~FRuntimeCase() { Library->Pipeline = nullptr; }

        FTextureEntry* Rec(const std::string& Text)
        {
            if (Text == "-")
                return nullptr;
            return Library->FindEntryByCrc(NEURONCOMPONENTS_CRC::GetCRC32(Text.c_str()));
        }

        void Line(unsigned int Op, GENRESULT Result, const std::string& Extra)
        {
            Out->Line(Format("{\"case\":\"%u\",\"op\":\"%u\",\"r\":\"%ld\"", Case, Op, (long)Result) + Extra + "}");
        }

        void Dump()
        {
            for (std::map<unsigned long, FTextureEntry*>::const_iterator It = Library->CanonicalRecords.begin();
                 It != Library->CanonicalRecords.end(); ++It)
            {
                const FTextureEntry* Entry = It->second;
                std::string Frames;
                for (size_t Index = 0; Index < Entry->Frames.size(); ++Index)
                {
                    const FTextureFrame& Frame = Entry->Frames[Index];
                    Frames += Format(Index == 0 ? "%lu:%08x,%08x,%08x,%08x" : "|%lu:%08x,%08x,%08x,%08x",
                                     Frame.ChildIndex, FloatBits(Frame.Uv[0]), FloatBits(Frame.Uv[1]),
                                     FloatBits(Frame.Uv[2]), FloatBits(Frame.Uv[3]));
                }
                std::string Children;
                for (size_t Index = 0; Index < Entry->Children.size(); ++Index)
                    Children += (Index == 0 ? "" : ",") + Name(Entry->Children[Index]);
                Out->Line(Format("{\"case\":\"%u\",\"rec\":\"", Case) + Name(Entry) +
                          Format("\",\"handle\":\"%lu\",\"refs\":\"%d\",\"rate\":\"%08x\",\"frames\":\"",
                                 Entry->GpuHandle, Entry->RefCount, FloatBits(Entry->TileRate)) +
                          Frames + "\",\"children\":\"" + Children + "\"}");
            }
            for (std::map<int, FTexturePlayback>::const_iterator It = Library->PlaybackRecords.begin();
                 It != Library->PlaybackRecords.end(); ++It)
            {
                const FTexturePlayback& P = It->second;
                Out->Line(Format("{\"case\":\"%u\",\"pb\":\"%d\",\"src\":\"", Case, It->first) + Name(P.Source) +
                          Format("\",\"cur\":\"%u\",\"count\":\"%u\",\"el\":\"%016llx\",\"dur\":\"%016llx\","
                                 "\"dir\":\"%08x\",\"wrap\":\"%d\",\"refs\":\"%u\",\"local\":\"%08x\",\"use\":\"%d\"}",
                                 P.CurrentFrame, P.FrameCount, DoubleBits(P.ElapsedTime), DoubleBits(P.TotalDuration),
                                 FloatBits(P.PlaybackDirection), P.WrapMode, P.RefCount, FloatBits(P.LocalRate),
                                 P.bUseLocalRate ? 1 : 0));
            }
            std::vector<int> Active;
            for (std::map<int, FTexturePlayback*>::const_iterator It = Library->ActivePlayback.begin();
                 It != Library->ActivePlayback.end(); ++It)
                Active.push_back(It->first);
            Out->Line(Format("{\"case\":\"%u\",\"active\":\"", Case) + Ints(Active) + "\"}");
            for (std::map<int, FTextureGroup>::const_iterator It = Library->GroupRecords.begin();
                 It != Library->GroupRecords.end(); ++It)
            {
                const char* GroupName = It->second.Name.get();
                Out->Line(Format("{\"case\":\"%u\",\"group\":\"%d\",\"name\":\"", Case, It->first) +
                          (GroupName == nullptr ? std::string("-") : HexC(GroupName)) + "\",\"members\":\"" +
                          Ints(It->second.Members) + "\"}");
            }
        }

        void Run(unsigned int Op, const std::vector<std::string>& F)
        {
            const std::string& K = F[0];
            FTextureLibrary* L = Library;
            if (K == "tex")
            {
                FTextureEntry* Entry = nullptr;
                L->CreateTextureRecord((unsigned long)Int(F[2]), F[1].c_str(), &Entry, false);
                Line(Op, GR_OK, "");
            }
            else if (K == "anim")
            {
                FTextureEntry* Entry = new FTextureEntry();
                Entry->SetName(F[1].c_str());
                Entry->TileRate = Bits(F[2]);
                const std::vector<std::string> Children = Split(F[3], ",");
                for (size_t Index = 0; Index < Children.size(); ++Index)
                    Entry->Children.push_back(Rec(Children[Index]));
                const std::vector<std::string> Frames = Split(F[4], "|");
                for (size_t Index = 0; Index < Frames.size(); ++Index)
                {
                    const std::vector<std::string> V = Split(Frames[Index], ":");
                    FTextureFrame Frame;
                    Frame.ChildIndex = (unsigned long)Int(V[0]);
                    for (int Corner = 0; Corner < 4; ++Corner)
                        Frame.Uv[Corner] = Bits(V[1 + Corner]);
                    Entry->Frames.push_back(Frame);
                }
                L->StoreByNameCrc(Entry);
                Line(Op, GR_OK, "");
            }
            else if (K == "named")
            {
                void* Tex = nullptr;
                Line(Op, L->FindOrCreateByName(F[1].c_str(), &Tex), "");
            }
            else if (K == "find")
            {
                const unsigned long Query[2] = {0, NEURONCOMPONENTS_CRC::GetCRC32(F[1].c_str())};
                void* Tex = nullptr;
                Line(Op, L->FindTextureByCrc((void*)Query, Int(F[2]) != 0 ? &Tex : nullptr), "");
            }
            else if (K == "play")
            {
                int Id = (int)kUntouched;
                const GENRESULT Result = L->CreatePlaybackGroup(Rec(F[1]), &Id);
                Line(Op, Result, Format(",\"id\":\"%d\"", Id));
            }
            else if (K == "playnull")
                Line(Op, L->CreatePlaybackGroup(Rec(F[1]), nullptr), "");
            else if (K == "release")
                Line(Op, L->ReleaseTextureRecord(Rec(F[1])), "");
            else if (K == "addref")
                Line(Op, L->AddRefById((int)Int(F[1])), "");
            else if (K == "decref")
                Line(Op, L->DecRefById((int)Int(F[1])), "");
            else if (K == "setel")
                Line(Op, L->SetElapsedTime((int)Int(F[1]), Bits(F[2])), "");
            else if (K == "getel" || K == "getrate")
            {
                float Value = FloatFromBits(kUntouched);
                const GENRESULT Result = K == "getel" ? L->GetElapsedTime((int)Int(F[1]), &Value)
                                                      : L->GetEffectiveRate((int)Int(F[1]), &Value);
                Line(Op, Result, Format(",\"v\":\"%08x\"", FloatBits(Value)));
            }
            else if (K == "setrate")
                Line(Op, L->SetLocalRate((int)Int(F[1]), Bits(F[2])), "");
            else if (K == "setwrap")
                Line(Op, L->SetWrapMode((int)Int(F[1]), (int)Int(F[2])), "");
            else if (K == "getwrap")
            {
                int Value = -7;
                const GENRESULT Result = L->GetWrapMode((int)Int(F[1]), &Value);
                Line(Op, Result, Format(",\"v\":\"%d\"", Value));
            }
            else if (K == "setidx")
                Line(Op, L->SetFrameIndex((int)Int(F[1]), (unsigned int)strtoul(F[2].c_str(), nullptr, 10)), "");
            else if (K == "getidx")
            {
                unsigned int Value = kUntouched;
                const GENRESULT Result = L->GetFrameIndex((int)Int(F[1]), &Value);
                Line(Op, Result, Format(",\"v\":\"%u\"", Value));
            }
            else if (K == "adv")
                Line(Op, L->AdvanceTime((int)Int(F[1]), Bits(F[2])), "");
            else if (K == "tick")
                Line(Op, L->TickAllPlayback((int)Int(F[1]), Bits(F[2])), "");
            else if (K == "frame" || K == "tframe")
            {
                unsigned long Frame[5] = {kUntouched, kUntouched, kUntouched, kUntouched, kUntouched};
                const GENRESULT Result = K == "frame" ? L->GetFrame((int)Int(F[1]), (int)Int(F[2]), Frame)
                                                      : L->GetTextureFrame(Rec(F[1]), (int)Int(F[2]), Frame);
                Line(Op, Result, FrameFields(Frame));
            }
            else if (K == "fhandle")
                Line(Op, L->SetFrameHandle(Rec(F[1]), (int)Int(F[2]), (unsigned long)Int(F[3])), "");
            else if (K == "frect")
                Line(Op, L->SetFrameRect(Rec(F[1]), (int)Int(F[2]), Bits(F[3]), Bits(F[4]), Bits(F[5]), Bits(F[6])),
                     "");
            else if (K == "settile")
                Line(Op, L->SetTileRate(Rec(F[1]), Bits(F[2])), "");
            else if (K == "gettile")
            {
                float Value = FloatFromBits(kUntouched);
                const GENRESULT Result = L->GetTileRate(Rec(F[1]), &Value);
                Line(Op, Result, Format(",\"v\":\"%08x\"", FloatBits(Value)));
            }
            else if (K == "count")
            {
                int Value = -7;
                const GENRESULT Result = L->GetFrameCount(Rec(F[1]), &Value);
                Line(Op, Result, Format(",\"v\":\"%d\"", Value));
            }
            else if (K == "sub")
                Line(Op, L->SetSubTexture(Rec(F[1]), (int)Int(F[2]), (int)Int(F[3])), "");
            else if (K == "rename")
                Line(Op, L->SetRecordName(Rec(F[1]), F[2].c_str()), "");
            else if (K == "copy" || K == "getname")
            {
                char Buffer[256];
                Buffer[0] = '\0';
                const unsigned long Size = (unsigned long)Int(F[2]);
                const GENRESULT Result = K == "copy" ? L->CopyName(Rec(F[1]), Buffer, Size)
                                                     : L->GetGroupName((int)Int(F[1]), Buffer, Size);
                Line(Op, Result, ",\"text\":\"" + HexC(Buffer) + "\"");
            }
            else if (K == "group")
            {
                int Id = -7;
                const GENRESULT Result = L->AllocateNewTextureId(&Id);
                Line(Op, Result, Format(",\"id\":\"%d\"", Id));
            }
            else if (K == "gadd")
                Line(Op, L->AddSubTexture((int)Int(F[1]), Rec(F[2])), "");
            else if (K == "gdel")
                Line(Op, L->RemoveSubTexture((int)Int(F[1]), Rec(F[2])), "");
            else if (K == "gname")
                Line(Op, L->SetGroupName((int)Int(F[1]), F[2] == "-" ? nullptr : F[2].c_str()), "");
            else if (K == "gc")
                Line(Op, L->GarbageCollect((int)Int(F[1])), "");
            else if (K == "enumg" || K == "subcount")
            {
                int Value = -7;
                const GENRESULT Result = K == "enumg" ? L->EnumGroup((int)Int(F[1]), &Value)
                                                      : L->GetSubCount((int)Int(F[1]), &Value);
                Line(Op, Result, Format(",\"v\":\"%d\"", Value));
            }
            else if (K == "enumt")
            {
                void* Record = nullptr;
                const GENRESULT Result =
                    L->EnumTexture((int)Int(F[1]), (int)Int(F[2]), Int(F[3]) != 0 ? &Record : nullptr);
                Line(Op, Result, ",\"rec\":\"" + Name((FTextureEntry*)Record) + "\"");
            }
            else if (K == "resolve")
            {
                void* Record = nullptr;
                const GENRESULT Result = L->ResolveTextureRecord((int)Int(F[1]), &Record);
                Line(Op, Result, ",\"rec\":\"" + Name((FTextureEntry*)Record) + "\"");
            }
            else if (K == "peek")
            {
                unsigned long Value = kUntouched;
                const GENRESULT Result = L->PeekNextGroupId(&Value);
                Line(Op, Result, Format(",\"v\":\"%lu\"", Value));
            }
            else if (K == "slot")
                Line(Op, L->SetGenericSlot((int)Int(F[1]), (unsigned long)Int(F[2])), "");
            else if (K == "getslot")
            {
                unsigned long Value = kUntouched;
                const GENRESULT Result = L->GetGenericSlot((int)Int(F[1]), &Value);
                Line(Op, Result, Format(",\"v\":\"%lu\"", Value));
            }
            else if (K == "pick")
            {
                const unsigned long Source = (unsigned long)Int(F[1]);
                unsigned long Value = 0xFFFFFFFFu;
                const GENRESULT Result = L->PickGpuFormat(&Source, &Value);
                Line(Op, Result, Format(",\"v\":\"%lu\"", Value));
            }
            else if (K == "fmt")
            {
                int Value = -7;
                const GENRESULT Result = L->GetGroupPixelFormat(Rec(F[1]), nullptr, &Value);
                Line(Op, Result, Format(",\"v\":\"%d\"", Value));
            }
            else if (K == "dump")
                Dump();
            else
            {
                fprintf(stderr, "texture_runtime: unknown operation %s\n", K.c_str());
                GFailed = true;
            }
        }

    private:
        Suite* Out;
        unsigned int Case;
        FOracleTexturePipeline Pipeline;
        FTextureLibrary* Library;
    };

    void TextureRuntimeSuite()
    {
        OracleTextureFormatMapDefaults();
        Suite Out("texture_runtime.jsonl");
        const std::vector<std::string> Lines = InputLines("texture_runtime.list");
        for (unsigned int Case = 0; Case < Lines.size(); ++Case)
        {
            FRuntimeCase Runner(&Out, Case);
            const std::vector<std::string> Ops = Split(Lines[Case], " ; ");
            for (unsigned int Op = 0; Op < Ops.size(); ++Op)
                Runner.Run(Op, Split(Ops[Op], " "));
        }
    }
}

ITextureLibrary2* OracleDefaultTextureLibrary()
{
    static FTextureLibrary* Library = nullptr;
    if (Library == nullptr)
        Library = new FTextureLibrary();
    OracleTextureFormatMapDefaults();
    return Library;
}

void RunTextureSuites()
{
    TextureRuntimeSuite();
    BigImageSuite();
}
