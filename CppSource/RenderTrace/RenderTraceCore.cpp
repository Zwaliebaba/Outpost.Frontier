// Renderer call trace session: see RenderTraceCore.h and README.md.

#include "RenderTraceCore.h"

#include "luaprofile.h"

#include <algorithm>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace RenderTrace
{
    namespace
    {
        const int kFdumpNotice = 0x100003;
        const int kFdumpWarning = 0x100002;
        const char kSourceFile[] = "RenderTrace\\RenderTraceCore.cpp";

        // Inline hex up to this many bytes; a blob file above it.
        const size_t kInlineBytes = 64;
        // Flush the line buffer when it grows past this.
        const size_t kFlushBytes = 4u << 20;
        // A single resolved region larger than this is not read (a broken descriptor).
        const unsigned long long kMaxRegionBytes = 64ull << 20;
        // A surface larger than this is not read (a broken size).
        const unsigned long long kMaxSurfaceBytes = 256ull << 20;

        // D3DRS_SOFTWAREVERTEXPROCESSING: DX8RENDERER resets its stream cache when it changes.
        const unsigned int kRenderStateSoftwareVertexProcessing = 153;

        volatile LONG GSessionCount = 0;

        void Notice(int Flags, int Line, const char* Text)
        {
            if (FDUMP == nullptr)
                return;
            if (Flags == kFdumpWarning)
                FDUMP(Flags, "%s(%d) : WARNING:General:%s", kSourceFile, Line, Text);
            else
                FDUMP(Flags, "%s(%d) : NOTICE:General:%s", kSourceFile, Line, Text);
        }

        void AppendJsonString(std::string& Out, const char* Text)
        {
            static const char kHex[] = "0123456789abcdef";
            Out += '"';
            for (const unsigned char* Cursor = (const unsigned char*)Text; *Cursor != 0; ++Cursor)
            {
                const unsigned char Char = *Cursor;
                if (Char == '"' || Char == '\\')
                {
                    Out += '\\';
                    Out += (char)Char;
                }
                else if (Char < 0x20 || Char >= 0x7F)
                {
                    // Bytes outside printable ASCII are written as \u00XX (one byte, one code unit).
                    Out += "\\u00";
                    Out += kHex[Char >> 4];
                    Out += kHex[Char & 0xF];
                }
                else
                {
                    Out += (char)Char;
                }
            }
            Out += '"';
        }

        void AppendHex(std::string& Out, const void* Data, size_t Size)
        {
            static const char kHex[] = "0123456789abcdef";
            const unsigned char* Bytes = (const unsigned char*)Data;
            for (size_t Index = 0; Index < Size; ++Index)
            {
                Out += kHex[Bytes[Index] >> 4];
                Out += kHex[Bytes[Index] & 0xF];
            }
        }

        void AppendWord(std::string& Out, unsigned int Word)
        {
            char Text[16];
            snprintf(Text, sizeof(Text), "\"%08x\"", Word);
            Out += Text;
        }

        void Trim(char* Text)
        {
            char* Start = Text;
            while (*Start == ' ' || *Start == '\t')
                ++Start;
            size_t Length = strlen(Start);
            while (Length > 0 && (Start[Length - 1] == ' ' || Start[Length - 1] == '\t' ||
                                  Start[Length - 1] == '\r' || Start[Length - 1] == '\n'))
                --Length;
            memmove(Text, Start, Length);
            Text[Length] = '\0';
        }

        bool ParseNumber(const char*& Cursor, unsigned int* Out)
        {
            if (*Cursor < '0' || *Cursor > '9')
                return false;
            unsigned long long Value = 0;
            while (*Cursor >= '0' && *Cursor <= '9')
            {
                Value = Value * 10 + (unsigned long long)(*Cursor - '0');
                if (Value > 0xFFFFFFFEull)
                    return false;
                ++Cursor;
            }
            *Out = (unsigned int)Value;
            return true;
        }

        // "all", "*", "N", "N-M", "N-", separated by commas or spaces; ';' ends the list.
        bool ParseFrames(const char* Spec, std::vector<std::pair<unsigned int, unsigned int> >* Out)
        {
            Out->clear();
            const char* Cursor = Spec;
            for (;;)
            {
                while (*Cursor == ' ' || *Cursor == '\t' || *Cursor == ',')
                    ++Cursor;
                if (*Cursor == '\0' || *Cursor == ';')
                    break;
                if (_strnicmp(Cursor, "all", 3) == 0)
                {
                    Out->push_back(std::make_pair(0u, 0xFFFFFFFFu));
                    Cursor += 3;
                    continue;
                }
                if (*Cursor == '*')
                {
                    Out->push_back(std::make_pair(0u, 0xFFFFFFFFu));
                    ++Cursor;
                    continue;
                }
                unsigned int Low = 0;
                if (!ParseNumber(Cursor, &Low))
                    return false;
                unsigned int High = Low;
                if (*Cursor == '-')
                {
                    ++Cursor;
                    if (*Cursor >= '0' && *Cursor <= '9')
                    {
                        if (!ParseNumber(Cursor, &High) || High < Low)
                            return false;
                    }
                    else
                    {
                        High = 0xFFFFFFFFu;
                    }
                }
                if (*Cursor != '\0' && *Cursor != ' ' && *Cursor != '\t' && *Cursor != ',' && *Cursor != ';')
                    return false;
                Out->push_back(std::make_pair(Low, High));
            }
            return !Out->empty();
        }

        void CreateDirectories(const std::string& Path)
        {
            for (size_t Index = 1; Index < Path.size(); ++Index)
            {
                if ((Path[Index] == '\\' || Path[Index] == '/') && Path[Index - 1] != ':')
                    CreateDirectoryA(Path.substr(0, Index).c_str(), nullptr);
            }
            CreateDirectoryA(Path.c_str(), nullptr);
        }

        bool IsDirectory(const std::string& Path)
        {
            const DWORD Attributes = GetFileAttributesA(Path.c_str());
            return Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        }

        unsigned int TexelBytes(unsigned int Fvf, unsigned int Set)
        {
            switch ((Fvf >> (Set * 2 + 16)) & 3)
            {
            case 0:
                return 8;
            case 1:
                return 12;
            case 2:
                return 16;
            default:
                return 4;
            }
        }
    }

    unsigned int FvfStride(unsigned int Fvf)
    {
        unsigned int Base = 0;
        if ((Fvf & 0xC) == 0xC)
            Base = 0x1C;
        else if ((Fvf & 4) == 4)
            Base = 0x10;
        else if ((Fvf & 2) == 2)
            Base = 0xC;
        if ((Fvf & 0x10) != 0)
            Base += 0xC;
        if ((Fvf & 0x40) != 0)
            Base += 4;
        if ((Fvf & 0x80) != 0)
            Base += 4;
        const unsigned int TexCount = (Fvf & 0xF00) >> 8;
        for (unsigned int Index = 0; Index < TexCount; ++Index)
            Base += TexelBytes(Fvf, Index);
        return Base;
    }

    unsigned int PrimCount(unsigned long Prim, unsigned int Count)
    {
        switch (Prim)
        {
        case 1:
            return Count;
        case 2:
            return (unsigned int)((int)(Count) / 2);
        case 3:
            return Count - 1;
        case 4:
            return (unsigned int)((int)(Count) / 3);
        case 5:
        case 6:
            return Count - 2;
        default:
            return 0;
        }
    }

    unsigned int VerticesRead(unsigned long Prim, unsigned int PrimitiveCount)
    {
        switch (Prim)
        {
        case 1:
            return PrimitiveCount;
        case 2:
            return PrimitiveCount * 2;
        case 3:
            return PrimitiveCount + 1;
        case 4:
            return PrimitiveCount * 3;
        case 5:
        case 6:
            return PrimitiveCount + 2;
        default:
            return 0;
        }
    }

    size_t DrawUpVertexBytes(unsigned long Prim, unsigned int Fvf, unsigned int Count)
    {
        const unsigned int Primitives = PrimCount(Prim, Count);
        if ((int)(Primitives) <= 0 || (int)(Count) <= 0)
            return 0;
        return (size_t)(VerticesRead(Prim, Primitives)) * FvfStride(Fvf);
    }

    size_t DrawIndexedUpVertexBytes(unsigned long Prim, unsigned int Fvf, unsigned int VertCount,
                                    unsigned int IndexCount)
    {
        if ((int)(PrimCount(Prim, IndexCount)) <= 0)
            return 0;
        return (size_t)(VertCount) * FvfStride(Fvf);
    }

    size_t DrawIndexedUpIndexBytes(unsigned long Prim, unsigned int IndexCount)
    {
        const unsigned int Primitives = PrimCount(Prim, IndexCount);
        if ((int)(Primitives) <= 0)
            return 0;
        return (size_t)(VerticesRead(Prim, Primitives)) * 2u;
    }

    size_t DrawIndexedVbIndexBytes(unsigned long Prim, unsigned int IndexCount)
    {
        if ((int)(PrimCount(Prim, IndexCount)) <= 0)
            return 0;
        return (size_t)(IndexCount) * 2u;
    }

    size_t SurfaceBytes(unsigned int Format, unsigned int Width, unsigned int Height, unsigned int Pitch)
    {
        // pfenum ids 1..8 (DX8RENDERER kPfBpp) and 10..14 (DXT1..DXT5).
        static const unsigned int kBits[9] = {0, 8, 24, 16, 16, 16, 16, 32, 32};
        if (Width == 0 || Height == 0)
            return 0;
        unsigned long long RowBytes = 0;
        unsigned long long Rows = 0;
        if (Format >= 1 && Format <= 8)
        {
            RowBytes = (unsigned long long)(Width) * (kBits[Format] / 8);
            Rows = Height;
        }
        else if (Format >= 10 && Format <= 14)
        {
            RowBytes = ((unsigned long long)(Width) + 3) / 4 * (Format == 10 ? 8u : 16u);
            Rows = ((unsigned long long)(Height) + 3) / 4;
        }
        else
        {
            return 0;
        }
        const unsigned long long Total = (Rows - 1) * Pitch + RowBytes;
        if (Total > kMaxSurfaceBytes)
            return 0;
        return (size_t)(Total);
    }

    unsigned long long Fnv1a64(const void* Data, size_t Size)
    {
        unsigned long long Hash = 0xcbf29ce484222325ull;
        const unsigned char* Bytes = (const unsigned char*)Data;
        for (size_t Index = 0; Index < Size; ++Index)
        {
            Hash ^= Bytes[Index];
            Hash *= 0x100000001b3ull;
        }
        return Hash;
    }

    // ---------------------------------------------------------------------------------------
    // FCall

    FCall::FCall(FSession& InSession, ECategory InCategory, const char* Interface, const char* Method)
        : Session(InSession), Category(InCategory), bActive(false), bWrites(false), bHasResult(false),
          bSynthetic(false), Result(GR_OK), SeqNo(0)
    {
        Session.Lock();
        if (!Session.Tracking())
            return;
        bWrites = Category == kAlways || Session.FrameRecorded();
        bActive = bWrites || Category == kState;
        if (!bActive)
            return;
        SeqNo = Session.NextSeq();
        char Head[192];
        snprintf(Head, sizeof(Head), "{\"seq\":%llu,\"frame\":%u,\"interface\":\"%s\",\"method\":\"%s\",\"args\":{",
                 SeqNo, Session.Frame(), Interface, Method);
        Line.reserve(256);
        Line = Head;
        First.push_back(true);
    }

    FCall::~FCall()
    {
        if (bActive)
        {
            while (First.size() > 1)
                EndObject();
            Line += '}';
            if (bHasResult)
            {
                char Text[32];
                snprintf(Text, sizeof(Text), ",\"ret\":%ld", (long)(Result));
                Line += Text;
            }
            if (!FirstSeen.empty())
            {
                Line += ",\"first_seen\":[";
                for (size_t Index = 0; Index < FirstSeen.size(); ++Index)
                {
                    char Text[32];
                    snprintf(Text, sizeof(Text), Index == 0 ? "%lld" : ",%lld", FirstSeen[Index]);
                    Line += Text;
                }
                Line += ']';
            }
            if (!NoteText.empty())
            {
                Line += ",\"note\":";
                AppendJsonString(Line, NoteText.c_str());
            }
            if (bSynthetic)
                Line += ",\"synthetic\":true";
            Line += '}';
            if (bWrites)
                Session.WriteLine(Line);
            if (!StateKey.empty())
                Session.StoreSlot(StateKey, Line);
        }
        Session.Unlock();
    }

    void FCall::Key(const char* Name)
    {
        if (!First.back())
            Line += ',';
        First.back() = false;
        Line += '"';
        Line += Name;
        Line += "\":";
    }

    void FCall::Int(const char* Name, long long Value)
    {
        if (!bActive)
            return;
        Key(Name);
        char Text[32];
        snprintf(Text, sizeof(Text), "%lld", Value);
        Line += Text;
    }

    void FCall::UInt(const char* Name, unsigned long long Value)
    {
        if (!bActive)
            return;
        Key(Name);
        char Text[32];
        snprintf(Text, sizeof(Text), "%llu", Value);
        Line += Text;
    }

    void FCall::Float(const char* Name, float Value)
    {
        if (!bActive)
            return;
        unsigned int Bits = 0;
        memcpy(&Bits, &Value, sizeof(Bits));
        Key(Name);
        AppendWord(Line, Bits);
    }

    void FCall::FloatAt(const char* Name, const void* Address)
    {
        if (!bActive)
            return;
        unsigned int Bits = 0;
        memcpy(&Bits, Address, sizeof(Bits));
        Key(Name);
        AppendWord(Line, Bits);
    }

    void FCall::Raw(const char* Name, const void* Value)
    {
        UInt(Name, (unsigned long long)(uintptr_t)(Value));
    }

    void FCall::Str(const char* Name, const char* Value)
    {
        if (!bActive)
            return;
        Key(Name);
        if (Value == nullptr)
            Line += "null";
        else
            AppendJsonString(Line, Value);
    }

    void FCall::Bool(const char* Name, bool Value)
    {
        if (!bActive)
            return;
        Key(Name);
        Line += Value ? "true" : "false";
    }

    void FCall::Null(const char* Name)
    {
        if (!bActive)
            return;
        Key(Name);
        Line += "null";
    }

    void FCall::Words(const char* Name, const void* Data, size_t Count)
    {
        if (!bActive)
            return;
        Key(Name);
        if (Data == nullptr)
        {
            Line += "null";
            return;
        }
        Line += '[';
        const unsigned char* Bytes = (const unsigned char*)(Data);
        for (size_t Index = 0; Index < Count; ++Index)
        {
            unsigned int Word = 0;
            memcpy(&Word, Bytes + Index * 4, sizeof(Word));
            if (Index != 0)
                Line += ',';
            AppendWord(Line, Word);
        }
        Line += ']';
    }

    void FCall::Bytes(const char* Name, const void* Data, size_t Size)
    {
        if (!bActive)
            return;
        Key(Name);
        if (Data == nullptr)
        {
            Line += "null";
            return;
        }
        char Text[48];
        snprintf(Text, sizeof(Text), "{\"size\":%llu,", (unsigned long long)(Size));
        Line += Text;
        if (Size <= kInlineBytes)
        {
            Line += "\"hex\":\"";
            AppendHex(Line, Data, Size);
            Line += "\"}";
        }
        else
        {
            Line += "\"blob\":\"";
            Line += Session.StoreBlob(Data, Size);
            Line += "\"}";
        }
    }

    void FCall::OutI32(const char* Name, const void* Pointer)
    {
        if (!bActive)
            return;
        if (Pointer == nullptr)
        {
            Null(Name);
            return;
        }
        int Value = 0;
        memcpy(&Value, Pointer, sizeof(Value));
        Int(Name, Value);
    }

    void FCall::OutU32(const char* Name, const void* Pointer)
    {
        if (!bActive)
            return;
        if (Pointer == nullptr)
        {
            Null(Name);
            return;
        }
        unsigned int Value = 0;
        memcpy(&Value, Pointer, sizeof(Value));
        UInt(Name, Value);
    }

    void FCall::OutF32(const char* Name, const void* Pointer)
    {
        if (!bActive)
            return;
        if (Pointer == nullptr)
        {
            Null(Name);
            return;
        }
        Words(Name, Pointer, 1);
    }

    void FCall::OutPtr(const char* Name, const void* Pointer)
    {
        if (!bActive)
            return;
        if (Pointer == nullptr)
        {
            Null(Name);
            return;
        }
        const void* Value = nullptr;
        memcpy(&Value, Pointer, sizeof(Value));
        Raw(Name, Value);
    }

    long long FCall::ResolveHandle(EHandleKind Kind, const void* Value)
    {
        if (Value == nullptr)
            return 0;
        if (Value == (const void*)((intptr_t)-1))
            return -1;
        bool bNew = false;
        const long long Id = Session.LookupHandle(Kind, Value, &bNew);
        if (bNew)
            FirstSeen.push_back(Id);
        return Id;
    }

    long long FCall::Handle(const char* Name, EHandleKind Kind, const void* Value)
    {
        if (!bActive)
            return 0;
        const long long Id = ResolveHandle(Kind, Value);
        Int(Name, Id);
        return Id;
    }

    void FCall::OutHandle(const char* Name, EHandleKind Kind, const void* Pointer)
    {
        if (!bActive)
            return;
        if (Pointer == nullptr)
        {
            Null(Name);
            return;
        }
        const void* Value = nullptr;
        memcpy(&Value, Pointer, sizeof(Value));
        Handle(Name, Kind, Value);
    }

    void FCall::HandleId(const char* Name, long long Id)
    {
        Int(Name, Id);
    }

    void FCall::BeginObject(const char* Name)
    {
        if (!bActive)
            return;
        Key(Name);
        Line += '{';
        First.push_back(true);
    }

    void FCall::EndObject()
    {
        if (!bActive || First.size() <= 1)
            return;
        First.pop_back();
        Line += '}';
    }

    void FCall::Return(GENRESULT InResult)
    {
        bHasResult = true;
        Result = InResult;
    }

    void FCall::KeepAsState(const char* InKey)
    {
        if (!bActive)
            return;
        StateKey = InKey;
    }

    void FCall::Note(const char* Text)
    {
        if (!bActive)
            return;
        if (!NoteText.empty())
            NoteText += "; ";
        NoteText += Text;
    }

    void FCall::Synthetic()
    {
        bSynthetic = true;
    }

    void FCall::Stream(const char* Name, const FRpVertexStream& Source, unsigned int Element, bool bIndexed,
                       unsigned int Start, unsigned int Count)
    {
        BeginObject(Name);
        UInt("ByteSize", Source.ByteSize);
        UInt("Stride", Source.Stride);
        UInt("Count", Source.Count);
        UInt("Element", Element);
        if (Source.Base == nullptr || Count == 0)
        {
            Null("data");
            EndObject();
            return;
        }
        unsigned long long Low = Start;
        unsigned long long High = (unsigned long long)(Start) + Count - 1;
        if (bIndexed)
        {
            if (Source.IndexArray == nullptr)
            {
                Null("index");
                Null("data");
                EndObject();
                return;
            }
            const unsigned long* Indices = Source.IndexArray + Start;
            Bytes("index", Indices, (size_t)(Count) * sizeof(*Indices));
            Low = Indices[0];
            High = Indices[0];
            for (unsigned int Index = 1; Index < Count; ++Index)
            {
                if (Indices[Index] < Low)
                    Low = Indices[Index];
                if (Indices[Index] > High)
                    High = Indices[Index];
            }
        }
        const unsigned long long Span = (High - Low) * Source.Stride + Element;
        UInt("first", Low);
        UInt("last", High);
        if (Span > kMaxRegionBytes)
        {
            Null("data");
            Note("vertex source region too large; not read");
        }
        else
        {
            Bytes("data", Source.Base + (size_t)(Low * Source.Stride), (size_t)(Span));
        }
        EndObject();
    }

    void FCall::VertexSource(const char* Name, const FRpVertexSrcDesc* Desc, unsigned int Start, unsigned int Count,
                             unsigned int DstFvf)
    {
        if (!bActive)
            return;
        if (Desc == nullptr)
        {
            Null(Name);
            return;
        }
        BeginObject(Name);
        UInt("Flags", Desc->Flags);
        UInt("SrcFvf", Desc->SrcFvf);
        UInt("SrcCount", Desc->SrcCount);
        const unsigned int SrcFvf = Desc->SrcFvf;
        if (DstFvf != 0 && DstFvf == SrcFvf && (Desc->Flags & 6u) == 2u)
        {
            // CopyVertexData's selector 3: a byte copy of Count vertices from Position.Base.
            const unsigned int VertexBytes = FvfStride(DstFvf);
            BeginObject("Interleaved");
            UInt("Stride", Desc->Position.Stride);
            UInt("VertexBytes", VertexBytes);
            UInt("first", Start);
            if (Desc->Position.Base == nullptr || Count == 0)
                Null("data");
            else
                Bytes("data", Desc->Position.Base + (size_t)(Start) * Desc->Position.Stride,
                      (size_t)(Count) * VertexBytes);
            EndObject();
        }
        else
        {
            const bool bIndexed = (Desc->Flags & 4u) != 0;
            Stream("Position", Desc->Position, 12, bIndexed, Start, Count);
            if ((SrcFvf & 0x10) != 0)
                Stream("Normal", Desc->Normal, 12, bIndexed, Start, Count);
            if ((SrcFvf & 0x40) != 0)
                Stream("Diffuse", Desc->Diffuse, 4, bIndexed, Start, Count);
            const unsigned int TexCount = (SrcFvf & 0xF00) >> 8;
            for (unsigned int Set = 0; Set < TexCount && Set < 2; ++Set)
            {
                const FRpVertexStream& Tex = Desc->Tex[Set];
                const unsigned int Element =
                    (Tex.ByteSize >= 1 && Tex.ByteSize <= 16) ? Tex.ByteSize : TexelBytes(SrcFvf, Set);
                Stream(Set == 0 ? "Tex0" : "Tex1", Tex, Element, bIndexed, Start, Count);
            }
        }
        EndObject();
    }

    // ---------------------------------------------------------------------------------------
    // FSession

    FSession::FSession()
        : CriticalSection(nullptr), Instance(0), bConfigured(false), bTracking(false), bFrameRecorded(false), FrameNo(0), Seq(0),
          SlotOrder(0), NextId(1), LastFrame(0), File(nullptr), bSwvpKnown(false), Swvp(0), SelectionSeq(0),
          SelectionFrame(0)
    {
        CRITICAL_SECTION* Section = new CRITICAL_SECTION;
        InitializeCriticalSection(Section);
        CriticalSection = Section;
        Selection.VbKind = 0;
        Selection.Vb = nullptr;
        Selection.VbFvf = 0;
        Selection.Ib = nullptr;
        Selection.IbBase = 0;
    }

    FSession::~FSession()
    {
        Close();
        CRITICAL_SECTION* Section = (CRITICAL_SECTION*)(CriticalSection);
        DeleteCriticalSection(Section);
        delete Section;
        CriticalSection = nullptr;
    }

    void FSession::Lock()
    {
        EnterCriticalSection((CRITICAL_SECTION*)(CriticalSection));
    }

    void FSession::Unlock()
    {
        LeaveCriticalSection((CRITICAL_SECTION*)(CriticalSection));
    }

    void FSession::Configure(const char* Section)
    {
        FScopedLock Guard(*this);
        if (bConfigured)
            return;
        bConfigured = true;

        char OutputDir[MAX_PATH * 2] = "";
        char Spec[1024] = "";
        IProfileParser* Parser = AcquireProfileParser();
        if (Parser != nullptr)
        {
            void* IniSection = Parser->OpenSection("RenderTrace", 3);
            if (IniSection != nullptr)
            {
                if (Parser->GetString(IniSection, "output", OutputDir, sizeof(OutputDir)) == 0)
                    OutputDir[0] = '\0';
                if (Parser->GetString(IniSection, "frames", Spec, sizeof(Spec)) == 0)
                    Spec[0] = '\0';
                Parser->CloseSection(IniSection);
            }
            Parser->Release();
        }
        char Env[MAX_PATH * 2];
        DWORD Length = GetEnvironmentVariableA("OUTPOST_RENDER_TRACE_DIR", Env, sizeof(Env));
        if (Length > 0 && Length < sizeof(Env))
            strcpy(OutputDir, Env);
        Length = GetEnvironmentVariableA("OUTPOST_RENDER_TRACE_FRAMES", Env, sizeof(Env));
        if (Length > 0 && Length < sizeof(Env) && Length < sizeof(Spec))
            strcpy(Spec, Env);
        // An INI comment may follow the value.
        char* Comment = strchr(OutputDir, ';');
        if (Comment != nullptr)
            *Comment = '\0';
        Trim(OutputDir);
        Trim(Spec);

        if (OutputDir[0] == '\0' && Spec[0] == '\0')
        {
            Notice(kFdumpNotice, __LINE__, "RenderTrace: no output or frames configured; forwarding only");
            return;
        }
        if (OutputDir[0] == '\0' || Spec[0] == '\0')
        {
            Notice(kFdumpWarning, __LINE__, "RenderTrace: both output and frames are needed; forwarding only");
            return;
        }
        if (!ParseFrames(Spec, &Frames))
        {
            Notice(kFdumpWarning, __LINE__, "RenderTrace: cannot parse frames; forwarding only");
            return;
        }
        LastFrame = 0;
        for (size_t Index = 0; Index < Frames.size(); ++Index)
            LastFrame = (std::max)(LastFrame, Frames[Index].second);
        FrameSpec = Spec;

        Directory = OutputDir;
        while (!Directory.empty() && (Directory[Directory.size() - 1] == '\\' || Directory[Directory.size() - 1] == '/'))
            Directory.erase(Directory.size() - 1);
        BlobDirectory = Directory + "\\blobs";
        CreateDirectories(BlobDirectory);
        if (!IsDirectory(BlobDirectory))
        {
            Notice(kFdumpWarning, __LINE__, "RenderTrace: cannot create the output folder; forwarding only");
            return;
        }
        Instance = (long)(InterlockedIncrement(&GSessionCount));
        char FileName[64];
        if (Instance == 1)
            snprintf(FileName, sizeof(FileName), "\\trace.jsonl");
        else
            snprintf(FileName, sizeof(FileName), "\\trace-%ld.jsonl", (long)(Instance));
        const std::string TracePath = Directory + FileName;
        File = fopen(TracePath.c_str(), "wb");
        if (File == nullptr)
        {
            Notice(kFdumpWarning, __LINE__, "RenderTrace: cannot open the trace file; forwarding only");
            return;
        }

        std::string Header = "{\"format\":\"outpost-render-trace\",\"version\":1,\"renderer\":\"";
        Header += kDx8RendererComponentName;
        Header += "\",\"section\":";
        if (Section != nullptr && Section[0] != '\0')
            AppendJsonString(Header, Section);
        else
            Header += "null";
        Header += ",\"frames\":";
        AppendJsonString(Header, FrameSpec.c_str());
        Header += '}';
        WriteLine(Header);

        std::string Text = "RenderTrace: writing ";
        Text += TracePath;
        Text += ", frames ";
        Text += FrameSpec;
        Notice(kFdumpNotice, __LINE__, Text.c_str());

        bTracking = true;
        FrameNo = 0;
        BeginFrame();
    }

    void FSession::Close()
    {
        FScopedLock Guard(*this);
        if (File != nullptr)
        {
            char Text[96];
            snprintf(Text, sizeof(Text), "{\"end\":true,\"frame\":%u,\"seq\":%llu}", FrameNo, Seq);
            WriteLine(Text);
            Flush();
            fclose(File);
            File = nullptr;
        }
        bTracking = false;
        bFrameRecorded = false;
        Handles.clear();
        PoolVbs.clear();
        PoolVbByFvf.clear();
        BufferLocks.clear();
        TextureLocks.clear();
        Slots.clear();
        StageTextures.clear();
        LiveLights.clear();
        PendingLights.clear();
        Blobs.clear();
        Buffer.clear();
    }

    bool FSession::FrameSelected(unsigned int Frame) const
    {
        for (size_t Index = 0; Index < Frames.size(); ++Index)
        {
            if (Frame >= Frames[Index].first && Frame <= Frames[Index].second)
                return true;
        }
        return false;
    }

    void FSession::BeginFrame()
    {
        bFrameRecorded = FrameSelected(FrameNo);
        if (!bFrameRecorded)
            return;

        std::vector<const FSlot*> Ordered;
        Ordered.reserve(Slots.size());
        for (std::map<std::string, FSlot>::const_iterator It = Slots.begin(); It != Slots.end(); ++It)
            Ordered.push_back(&It->second);
        struct FByOrder
        {
            bool operator()(const FSlot* Left, const FSlot* Right) const { return Left->Order < Right->Order; }
        };
        std::sort(Ordered.begin(), Ordered.end(), FByOrder());

        const size_t SelectionLines = Selection.Ib != nullptr ? 2 : 1;
        char Text[160];
        snprintf(Text, sizeof(Text), "{\"frame\":%u,\"begin_frame\":true,\"seq\":%llu,\"snapshot_lines\":%llu}", FrameNo,
                 Seq, (unsigned long long)(Ordered.size() + SelectionLines));
        WriteLine(Text);

        char Prefix[48];
        snprintf(Prefix, sizeof(Prefix), "{\"snapshot\":%u,", FrameNo);
        for (size_t Index = 0; Index < Ordered.size(); ++Index)
            WriteLine(Prefix + Ordered[Index]->Line.substr(1));

        // The stream selection last: draws and buffer calls change it as a side effect.
        char Head[160];
        snprintf(Head, sizeof(Head), "%s\"seq\":%llu,\"frame\":%u,\"synthetic\":true,\"interface\":", Prefix,
                 SelectionSeq, SelectionFrame);
        std::string VbLine = Head;
        VbLine += "\"IRPVertexBuffer\",\"method\":\"SelectVb\",\"args\":{\"Handle\":";
        const void* Vb = Selection.Vb;
        if (Selection.VbKind == 2)
        {
            std::map<unsigned int, const void*>::const_iterator Pool = PoolVbByFvf.find(Selection.VbFvf);
            Vb = Pool != PoolVbByFvf.end() ? Pool->second : nullptr;
        }
        if (Selection.VbKind == 0)
        {
            VbLine += "0";
        }
        else if (Selection.VbKind == 2 && Vb == nullptr)
        {
            snprintf(Text, sizeof(Text), "{\"pool_fvf\":%u}", Selection.VbFvf);
            VbLine += Text;
        }
        else if (Vb == (const void*)((intptr_t)-1))
        {
            VbLine += "-1";
        }
        else
        {
            snprintf(Text, sizeof(Text), "%lld", FindHandle(kVertexBuffer, Vb));
            VbLine += Text;
        }
        VbLine += "}}";
        WriteLine(VbLine);
        if (Selection.Ib != nullptr)
        {
            std::string IbLine = Head;
            const long long IbId =
                Selection.Ib == (const void*)((intptr_t)-1) ? -1 : FindHandle(kIndexBuffer, Selection.Ib);
            snprintf(Text, sizeof(Text),
                     "\"IRPIndexBuffer\",\"method\":\"SelectIb\",\"args\":{\"Handle\":%lld,\"BaseVertexIndex\":%u,"
                     "\"Unused3\":0,\"Unused4\":0}}",
                     IbId, Selection.IbBase);
            IbLine += Text;
            WriteLine(IbLine);
        }
    }

    void FSession::EndFrame()
    {
        FScopedLock Guard(*this);
        if (!bTracking)
            return;
        if (bFrameRecorded)
        {
            char Text[96];
            snprintf(Text, sizeof(Text), "{\"frame\":%u,\"end_frame\":true,\"seq\":%llu}", FrameNo, Seq);
            WriteLine(Text);
        }
        Flush();
        if (FrameNo >= LastFrame)
        {
            Close();
            Notice(kFdumpNotice, __LINE__, "RenderTrace: last selected frame written; trace closed");
            return;
        }
        ++FrameNo;
        BeginFrame();
    }

    long long FSession::NewHandle(EHandleKind Kind, const void* Value)
    {
        const long long Id = NextId++;
        Handles[std::make_pair((int)(Kind), Value)] = Id;
        return Id;
    }

    long long FSession::LookupHandle(EHandleKind Kind, const void* Value, bool* bNew)
    {
        std::map<std::pair<int, const void*>, long long>::const_iterator It =
            Handles.find(std::make_pair((int)(Kind), Value));
        if (It != Handles.end())
        {
            *bNew = false;
            return It->second;
        }
        *bNew = true;
        return NewHandle(Kind, Value);
    }

    long long FSession::FindHandle(EHandleKind Kind, const void* Value) const
    {
        std::map<std::pair<int, const void*>, long long>::const_iterator It =
            Handles.find(std::make_pair((int)(Kind), Value));
        return It != Handles.end() ? It->second : 0;
    }

    void FSession::RetireHandle(EHandleKind Kind, const void* Value)
    {
        Handles.erase(std::make_pair((int)(Kind), Value));
    }

    void FSession::MarkPoolVb(const void* Vb, unsigned int Fvf)
    {
        if (Vb == nullptr || Vb == (const void*)((intptr_t)-1))
            return;
        PoolVbs.insert(Vb);
        PoolVbByFvf[Fvf] = Vb;
    }

    bool FSession::IsPoolVb(const void* Vb) const
    {
        return PoolVbs.find(Vb) != PoolVbs.end();
    }

    void FSession::PushBufferLock(EHandleKind Kind, const void* BufferHandle, const FBufferLock& BufferLock)
    {
        BufferLocks[std::make_pair((int)(Kind), BufferHandle)].push_back(BufferLock);
    }

    bool FSession::PopBufferLock(EHandleKind Kind, const void* BufferHandle, FBufferLock* Out)
    {
        std::map<std::pair<int, const void*>, std::vector<FBufferLock> >::iterator It =
            BufferLocks.find(std::make_pair((int)(Kind), BufferHandle));
        if (It == BufferLocks.end() || It->second.empty())
            return false;
        *Out = It->second.back();
        It->second.pop_back();
        if (It->second.empty())
            BufferLocks.erase(It);
        return true;
    }

    bool FSession::FindVbRegion(const void* Address, const void** Vb, size_t* Offset) const
    {
        const unsigned char* Target = (const unsigned char*)(Address);
        for (std::map<std::pair<int, const void*>, std::vector<FBufferLock> >::const_iterator It = BufferLocks.begin();
             It != BufferLocks.end(); ++It)
        {
            if (It->first.first != kVertexBuffer)
                continue;
            for (size_t Index = It->second.size(); Index-- > 0;)
            {
                const FBufferLock& Region = It->second[Index];
                const size_t Size = (size_t)(Region.Count) * Region.Stride;
                if (Region.Bits != nullptr && Target >= Region.Bits && Target < Region.Bits + Size)
                {
                    *Vb = It->first.second;
                    *Offset = (size_t)(Target - Region.Bits);
                    return true;
                }
            }
        }
        return false;
    }

    void FSession::SetTextureLock(const void* Texture, int Level, const FTextureLock& TextureLock)
    {
        TextureLocks[std::make_pair(Texture, Level)] = TextureLock;
    }

    bool FSession::PopTextureLock(const void* Texture, int Level, FTextureLock* Out)
    {
        std::map<std::pair<const void*, int>, FTextureLock>::iterator It =
            TextureLocks.find(std::make_pair(Texture, Level));
        if (It == TextureLocks.end())
            return false;
        *Out = It->second;
        TextureLocks.erase(It);
        return true;
    }

    std::string FSession::SyntheticHead(const char* Interface, const char* Method) const
    {
        char Text[192];
        snprintf(Text, sizeof(Text),
                 "{\"seq\":%llu,\"frame\":%u,\"synthetic\":true,\"interface\":\"%s\",\"method\":\"%s\",", Seq, FrameNo,
                 Interface, Method);
        return Text;
    }

    void FSession::OnStageTexture(int Stage, const void* Texture)
    {
        StageTextures[Stage] = Texture;
    }

    void FSession::OnTextureUnbound(const void* Texture)
    {
        if (Texture == nullptr)
            return;
        // DX8RENDERER's UnbindTextureHandle: stages 0..7.
        for (std::map<int, const void*>::iterator It = StageTextures.begin(); It != StageTextures.end(); ++It)
        {
            if (It->second != Texture || It->first < 0 || It->first >= 8)
                continue;
            It->second = nullptr;
            char Key[32];
            snprintf(Key, sizeof(Key), "TEX:%d", It->first);
            char Args[96];
            snprintf(Args, sizeof(Args), "\"args\":{\"Stage\":%d,\"Handle\":0}}", It->first);
            StoreSlot(Key, SyntheticHead("IRenderPipeline", "SetTextureStageTexture") + Args);
        }
        // A lock left open on a texture that goes away is dropped.
        for (std::map<std::pair<const void*, int>, FTextureLock>::iterator It = TextureLocks.begin();
             It != TextureLocks.end();)
        {
            if (It->first.first == Texture)
                It = TextureLocks.erase(It);
            else
                ++It;
        }
    }

    void FSession::OnResetStates()
    {
        // ApplyDefaultTextureStages binds no texture on every stage.
        StageTextures.clear();
    }

    void FSession::OnDeviceReset()
    {
        // CreateBuffers re-applies the default render states, texture stages, transforms and
        // viewport and resets the stream cache; the light records survive and are resent.
        for (std::map<std::string, FSlot>::iterator It = Slots.begin(); It != Slots.end();)
        {
            if (It->first.compare(0, 2, "L:") != 0)
                It = Slots.erase(It);
            else
                ++It;
        }
        StageTextures.clear();
        bSwvpKnown = false;
        ResetSelection();
        // LightManager Reset marks every light with data dirty: the next flush reads them all.
        PendingLights = LiveLights;
    }

    void FSession::OnRenderState(unsigned int State, int Value)
    {
        if (State != kRenderStateSoftwareVertexProcessing)
            return;
        if (!bSwvpKnown || Swvp != Value)
            ResetSelection();
        bSwvpKnown = true;
        Swvp = Value;
    }

    void FSession::OnLightGone(long long LightId)
    {
        char Prefix[48];
        snprintf(Prefix, sizeof(Prefix), "L:%lld:", LightId);
        EraseSlotsWithPrefix(Prefix);
    }

    void FSession::OnLightData(const void* Light)
    {
        if (Light == nullptr)
            return;
        const long long Id = FindHandle(kLight, Light);
        if (Id <= 0)
            return;
        LiveLights[Id] = Light;
        PendingLights[Id] = Light;
    }

    void FSession::OnLightEnabled(const void* Light)
    {
        const long long Id = FindHandle(kLight, Light);
        if (Id > 0 && LiveLights.find(Id) != LiveLights.end())
            PendingLights[Id] = Light;
    }

    void FSession::OnLightRemoved(const void* Light)
    {
        const long long Id = FindHandle(kLight, Light);
        if (Id <= 0)
            return;
        LiveLights.erase(Id);
        PendingLights.erase(Id);
    }

    void FSession::TakePendingLights(std::vector<std::pair<long long, const void*> >* Out)
    {
        Out->assign(PendingLights.begin(), PendingLights.end());
        PendingLights.clear();
    }

    void FSession::WriteDefaults(const unsigned int* States, const unsigned long* RenderValues, const bool* RenderRead,
                                 size_t RenderCount, const int* PipelineValues, const bool* PipelineRead,
                                 size_t PipelineCount)
    {
        if (!bTracking)
            return;
        std::string Line = "{\"defaults\":{\"render\":{";
        char Text[64];
        bool bFirst = true;
        bool bMissing = false;
        for (size_t Index = 0; Index < RenderCount; ++Index)
        {
            if (!RenderRead[Index])
            {
                bMissing = true;
                continue;
            }
            snprintf(Text, sizeof(Text), "%s\"%u\":%lu", bFirst ? "" : ",", States[Index], RenderValues[Index]);
            Line += Text;
            bFirst = false;
        }
        Line += "},\"pipeline\":{";
        bFirst = true;
        for (size_t Index = 0; Index < PipelineCount; ++Index)
        {
            if (!PipelineRead[Index])
            {
                bMissing = true;
                continue;
            }
            snprintf(Text, sizeof(Text), "%s\"%u\":%d", bFirst ? "" : ",", (unsigned int)(Index), PipelineValues[Index]);
            Line += Text;
            bFirst = false;
        }
        snprintf(Text, sizeof(Text), "}},\"seq\":%llu,\"frame\":%u,\"synthetic\":true", Seq, FrameNo);
        Line += Text;
        if (bMissing)
            Line += ",\"note\":\"a getter failed; its entry is left out\"";
        Line += '}';
        WriteLine(Line);
        StoreSlot("DEFAULTS", Line);
    }

    void FSession::WriteFrameImage(const void* Bmp, size_t Size, const char* Note)
    {
        if (!bTracking || !bFrameRecorded)
            return;
        char Text[160];
        // BITMAPFILEHEADER (14 bytes) then BITMAPINFOHEADER: biWidth at 18, biHeight at 22
        // (negative for a top-down image).
        const unsigned char* Bytes = (const unsigned char*)(Bmp);
        if (Bytes != nullptr && (Size < 26 || Bytes[0] != 'B' || Bytes[1] != 'M'))
        {
            Bytes = nullptr;
            Note = "PrintScreen wrote no BMP file";
        }
        if (Bytes == nullptr)
        {
            std::string Line;
            snprintf(Text, sizeof(Text), "{\"frame\":%u,\"frame_image\":null,\"note\":", FrameNo);
            Line = Text;
            AppendJsonString(Line, Note != nullptr ? Note : "no image");
            Line += '}';
            WriteLine(Line);
            return;
        }
        int Width = 0;
        int Height = 0;
        memcpy(&Width, Bytes + 18, sizeof(Width));
        memcpy(&Height, Bytes + 22, sizeof(Height));
        if (Height < 0)
            Height = -Height;
        const std::string Hash = StoreBlob(Bmp, Size);
        snprintf(Text, sizeof(Text), "{\"frame\":%u,\"frame_image\":\"%s\",\"width\":%d,\"height\":%d}", FrameNo,
                 Hash.c_str(), Width, Height);
        WriteLine(Text);
    }

    std::string FSession::FrameImagePath() const
    {
        char Folder[MAX_PATH + 1];
        const DWORD Length = GetTempPathA(sizeof(Folder), Folder);
        std::string Path;
        if (Length > 0 && Length < sizeof(Folder))
            Path = Folder;
        else
            Path = Directory + "\\";
        if (!Path.empty() && Path[Path.size() - 1] != '\\' && Path[Path.size() - 1] != '/')
            Path += '\\';
        char Name[96];
        snprintf(Name, sizeof(Name), "outpost-render-trace-%lu-%ld.bmp", (unsigned long)(GetCurrentProcessId()), Instance);
        return Path + Name;
    }

    void FSession::EraseSlotsWithPrefix(const char* Prefix)
    {
        const size_t Length = strlen(Prefix);
        for (std::map<std::string, FSlot>::iterator It = Slots.begin(); It != Slots.end();)
        {
            if (It->first.compare(0, Length, Prefix) == 0)
                It = Slots.erase(It);
            else
                ++It;
        }
    }

    void FSession::OnVbGone(const void* Vb)
    {
        if (Vb == nullptr)
            return;
        if (Selection.VbKind == 1 && Selection.Vb == Vb)
            ResetSelection();
        else if (Selection.VbKind == 2)
        {
            std::map<unsigned int, const void*>::const_iterator Pool = PoolVbByFvf.find(Selection.VbFvf);
            if (Pool != PoolVbByFvf.end() && Pool->second == Vb)
                ResetSelection();
        }
    }

    void FSession::OnIbGone(const void* Ib)
    {
        if (Ib != nullptr && Selection.Ib == Ib)
            ClearIb();
    }

    void FSession::ResetSelection()
    {
        Selection.VbKind = 0;
        Selection.Vb = nullptr;
        Selection.VbFvf = 0;
        Selection.Ib = nullptr;
        Selection.IbBase = 0;
        SelectionSeq = Seq;
        SelectionFrame = FrameNo;
    }

    void FSession::SelectVb(const void* Vb)
    {
        if (Vb == nullptr)
        {
            ResetSelection();
            return;
        }
        Selection.VbKind = 1;
        Selection.Vb = Vb;
        Selection.VbFvf = 0;
        SelectionSeq = Seq;
        SelectionFrame = FrameNo;
    }

    void FSession::SelectPoolVb(unsigned int Fvf)
    {
        std::map<unsigned int, const void*>::const_iterator Pool = PoolVbByFvf.find(Fvf);
        if (Pool != PoolVbByFvf.end())
        {
            SelectVb(Pool->second);
            return;
        }
        Selection.VbKind = 2;
        Selection.Vb = nullptr;
        Selection.VbFvf = Fvf;
        SelectionSeq = Seq;
        SelectionFrame = FrameNo;
    }

    void FSession::SelectIb(const void* Ib, unsigned int BaseVertex)
    {
        if (Ib == nullptr)
        {
            ClearIb();
            return;
        }
        Selection.Ib = Ib;
        Selection.IbBase = BaseVertex;
        SelectionSeq = Seq;
        SelectionFrame = FrameNo;
    }

    void FSession::ClearIb()
    {
        Selection.Ib = nullptr;
        Selection.IbBase = 0;
        SelectionSeq = Seq;
        SelectionFrame = FrameNo;
    }

    void FSession::WriteLine(const std::string& Line)
    {
        if (File == nullptr)
            return;
        Buffer += Line;
        Buffer += '\n';
        if (Buffer.size() >= kFlushBytes)
            Flush();
    }

    void FSession::Flush()
    {
        if (File == nullptr || Buffer.empty())
            return;
        fwrite(Buffer.data(), 1, Buffer.size(), File);
        fflush(File);
        Buffer.clear();
    }

    void FSession::StoreSlot(const std::string& Key, const std::string& Line)
    {
        FSlot& Slot = Slots[Key];
        Slot.Order = ++SlotOrder;
        Slot.Line = Line;
    }

    std::string FSession::StoreBlob(const void* Data, size_t Size)
    {
        const unsigned long long Hash = Fnv1a64(Data, Size);
        char Name[24];
        snprintf(Name, sizeof(Name), "%016llx", Hash);
        if (Blobs.insert(Hash).second && !BlobDirectory.empty())
        {
            const std::string Path = BlobDirectory + "\\" + Name + ".bin";
            if (GetFileAttributesA(Path.c_str()) == INVALID_FILE_ATTRIBUTES)
            {
                FILE* Blob = fopen(Path.c_str(), "wb");
                if (Blob != nullptr)
                {
                    fwrite(Data, 1, Size, Blob);
                    fclose(Blob);
                }
            }
        }
        return Name;
    }
}
