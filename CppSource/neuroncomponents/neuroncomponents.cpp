// Author: Giperion.

#include "neuroncomponents.h"
#include "luaprofile.h"
#include "msvcrt_compat.h"

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <map>
#include <new>

#pragma comment(lib, "version.lib")
#pragma comment(lib, "user32.lib")

// NEURONCOMPONENTS manager, built-in INI parser and default diagnostic handler.

// Exported data (neuroncomponents.def "FDUMP DATA"), zero until DllMain installs DefaultDump. Defined
// ahead of every use so this module reaches it directly rather than through the dllimport
// declaration neuroncomponents.h gives every other module.
extern "C" void(__cdecl* FDUMP)(int flags, const char* fmt, ...) = nullptr;

namespace
{
    // baseline neuroncomponents.dll the field (not IEEE 0xEDB88320). VMeshRef+4 / material CRCs
    // are exporter hashes of this table; IEEE GetCRC32 misses every on-disk key.
    static const unsigned int G_Crc32Table[256] = {
        0x00000000u, 0x09073096u, 0x120E612Cu, 0x1B0951BAu, 0xFF6DC419u, 0xF66AF48Fu, 0xED63A535u, 0xE46495A3u,
        0xFEDB8832u, 0xF7DCB8A4u, 0xECD5E91Eu, 0xE5D2D988u, 0x01B64C2Bu, 0x08B17CBDu, 0x13B82D07u, 0x1ABF1D91u,
        0xFDB71064u, 0xF4B020F2u, 0xEFB97148u, 0xE6BE41DEu, 0x02DAD47Du, 0x0BDDE4EBu, 0x10D4B551u, 0x19D385C7u,
        0x036C9856u, 0x0A6BA8C0u, 0x1162F97Au, 0x1865C9ECu, 0xFC015C4Fu, 0xF5066CD9u, 0xEE0F3D63u, 0xE7080DF5u,
        0xFB6E20C8u, 0xF269105Eu, 0xE96041E4u, 0xE0677172u, 0x0403E4D1u, 0x0D04D447u, 0x160D85FDu, 0x1F0AB56Bu,
        0x05B5A8FAu, 0x0CB2986Cu, 0x17BBC9D6u, 0x1EBCF940u, 0xFAD86CE3u, 0xF3DF5C75u, 0xE8D60DCFu, 0xE1D13D59u,
        0x06D930ACu, 0x0FDE003Au, 0x14D75180u, 0x1DD06116u, 0xF9B4F4B5u, 0xF0B3C423u, 0xEBBA9599u, 0xE2BDA50Fu,
        0xF802B89Eu, 0xF1058808u, 0xEA0CD9B2u, 0xE30BE924u, 0x076F7C87u, 0x0E684C11u, 0x15611DABu, 0x1C662D3Du,
        0xF6DC4190u, 0xFFDB7106u, 0xE4D220BCu, 0xEDD5102Au, 0x09B18589u, 0x00B6B51Fu, 0x1BBFE4A5u, 0x12B8D433u,
        0x0807C9A2u, 0x0100F934u, 0x1A09A88Eu, 0x130E9818u, 0xF76A0DBBu, 0xFE6D3D2Du, 0xE5646C97u, 0xEC635C01u,
        0x0B6B51F4u, 0x026C6162u, 0x196530D8u, 0x1062004Eu, 0xF40695EDu, 0xFD01A57Bu, 0xE608F4C1u, 0xEF0FC457u,
        0xF5B0D9C6u, 0xFCB7E950u, 0xE7BEB8EAu, 0xEEB9887Cu, 0x0ADD1DDFu, 0x03DA2D49u, 0x18D37CF3u, 0x11D44C65u,
        0x0DB26158u, 0x04B551CEu, 0x1FBC0074u, 0x16BB30E2u, 0xF2DFA541u, 0xFBD895D7u, 0xE0D1C46Du, 0xE9D6F4FBu,
        0xF369E96Au, 0xFA6ED9FCu, 0xE1678846u, 0xE860B8D0u, 0x0C042D73u, 0x05031DE5u, 0x1E0A4C5Fu, 0x170D7CC9u,
        0xF005713Cu, 0xF90241AAu, 0xE20B1010u, 0xEB0C2086u, 0x0F68B525u, 0x066F85B3u, 0x1D66D409u, 0x1461E49Fu,
        0x0EDEF90Eu, 0x07D9C998u, 0x1CD09822u, 0x15D7A8B4u, 0xF1B33D17u, 0xF8B40D81u, 0xE3BD5C3Bu, 0xEABA6CADu,
        0xEDB88320u, 0xE4BFB3B6u, 0xFFB6E20Cu, 0xF6B1D29Au, 0x12D54739u, 0x1BD277AFu, 0x00DB2615u, 0x09DC1683u,
        0x13630B12u, 0x1A643B84u, 0x016D6A3Eu, 0x086A5AA8u, 0xEC0ECF0Bu, 0xE509FF9Du, 0xFE00AE27u, 0xF7079EB1u,
        0x100F9344u, 0x1908A3D2u, 0x0201F268u, 0x0B06C2FEu, 0xEF62575Du, 0xE66567CBu, 0xFD6C3671u, 0xF46B06E7u,
        0xEED41B76u, 0xE7D32BE0u, 0xFCDA7A5Au, 0xF5DD4ACCu, 0x11B9DF6Fu, 0x18BEEFF9u, 0x03B7BE43u, 0x0AB08ED5u,
        0x16D6A3E8u, 0x1FD1937Eu, 0x04D8C2C4u, 0x0DDFF252u, 0xE9BB67F1u, 0xE0BC5767u, 0xFBB506DDu, 0xF2B2364Bu,
        0xE80D2BDAu, 0xE10A1B4Cu, 0xFA034AF6u, 0xF3047A60u, 0x1760EFC3u, 0x1E67DF55u, 0x056E8EEFu, 0x0C69BE79u,
        0xEB61B38Cu, 0xE266831Au, 0xF96FD2A0u, 0xF068E236u, 0x140C7795u, 0x1D0B4703u, 0x060216B9u, 0x0F05262Fu,
        0x15BA3BBEu, 0x1CBD0B28u, 0x07B45A92u, 0x0EB36A04u, 0xEAD7FFA7u, 0xE3D0CF31u, 0xF8D99E8Bu, 0xF1DEAE1Du,
        0x1B64C2B0u, 0x1263F226u, 0x096AA39Cu, 0x006D930Au, 0xE40906A9u, 0xED0E363Fu, 0xF6076785u, 0xFF005713u,
        0xE5BF4A82u, 0xECB87A14u, 0xF7B12BAEu, 0xFEB61B38u, 0x1AD28E9Bu, 0x13D5BE0Du, 0x08DCEFB7u, 0x01DBDF21u,
        0xE6D3D2D4u, 0xEFD4E242u, 0xF4DDB3F8u, 0xFDDA836Eu, 0x19BE16CDu, 0x10B9265Bu, 0x0BB077E1u, 0x02B74777u,
        0x18085AE6u, 0x110F6A70u, 0x0A063BCAu, 0x03010B5Cu, 0xE7659EFFu, 0xEE62AE69u, 0xF56BFFD3u, 0xFC6CCF45u,
        0xE00AE278u, 0xE90DD2EEu, 0xF2048354u, 0xFB03B3C2u, 0x1F672661u, 0x166016F7u, 0x0D69474Du, 0x046E77DBu,
        0x1ED16A4Au, 0x17D65ADCu, 0x0CDF0B66u, 0x05D83BF0u, 0xE1BCAE53u, 0xE8BB9EC5u, 0xF3B2CF7Fu, 0xFAB5FFE9u,
        0x1DBDF21Cu, 0x14BAC28Au, 0x0FB39330u, 0x06B4A3A6u, 0xE2D03605u, 0xEBD70693u, 0xF0DE5729u, 0xF9D967BFu,
        0xE3667A2Eu, 0xEA614AB8u, 0xF1681B02u, 0xF86F2B94u, 0x1C0BBE37u, 0x150C8EA1u, 0x0E05DF1Bu, 0x0702EF8Du,
    };

    // Signed bytes index the preceding CRC-table bytes as well as the ASCII fold table.
    unsigned int FoldCrcChar(char Ch)
    {
        const int Index = (int)((signed char)Ch);
        if (Index < 0)
        {
            const unsigned int Offset = 0x400u + (unsigned int)Index; // 0x380..0x3FF
            return (G_Crc32Table[Offset >> 2] >> ((Offset & 3u) * 8u)) & 0xFFu;
        }
        if (Index >= 'A' && Index <= 'Z')
            return (unsigned int)(Index + ('a' - 'A'));
        return (unsigned int)Index;
    }

    // One CRC step as baseline 065b6105..6124: EBX = fold(ch) ^ (crc & 0xff); crc = table[EBX] ^ (crc >> 8).
    unsigned int StepCrc(unsigned int Crc, char Ch)
    {
        const unsigned int Index = FoldCrcChar(Ch) ^ (Crc & 0xFFu);
        return G_Crc32Table[Index] ^ (Crc >> 8);
    }

    // , the fold table of the exported stricmp (unsigned index, ASCII only).
    unsigned char FoldStricmpChar(unsigned char Ch)
    {
        if (Ch >= 'A' && Ch <= 'Z')
            return (unsigned char)(Ch + ('a' - 'A'));
        return Ch;
    }

    // ---------------------------------------------------------------------------------------
    // FDUMP helpers

    const int DumpError = 0x100001;
    const int DumpWarning = 0x100002;
    const int DumpNotice = 0x100003;

    const char GNeuronComponentsSourceFile[] = "NEURONCOMPONENTS\\NeuronComponents.cpp";
    const char GErrorFormat[] = "%s(%d) : ERROR:General:%s";
    const char GWarningFormat[] = "%s(%d) : WARNING:General:%s";
    const char GNoticeFormat[] = "%s(%d) : NOTICE:General:%s";

    // _vsnprintf into the caller's 0x2000-byte buffer, returns the buffer.
    char* FormatDumpText(char* Buffer, const char* Format, ...)
    {
        va_list Args;
        va_start(Args, Format);
        _vsnprintf(Buffer, 0x2000, Format, Args);
        va_end(Args);
        return Buffer;
    }

    // set when the user answers Ignore to a warning dialog.
    bool GbIgnoreWarnings = false;

    // , installed into FDUMP by DllMain. baseline returns int (0, or 1 after
    // Retry); the port's FDUMP pointer type (neuroncomponents.h) returns void, so the value is dropped.
    void __cdecl DefaultDump(int Flags, const char* Format, ...)
    {
        char Text[0x1000];
        va_list Args;
        va_start(Args, Format);
        vsprintf(Text, Format, Args);
        va_end(Args);
        OutputDebugStringA(Text);

        // SHL ESI,0x1c; SAR ESI,0x1c -- the low nibble, sign-extended.
        const int Severity = (int)((unsigned int)Flags << 28) >> 28;
        if ((unsigned int)Severity < 3u)
            OutputDebugStringA("\n");

        if (Severity == 0 || Severity == 1 || (!GbIgnoreWarnings && Severity == 2))
        {
            // 0x40012 = MB_ABORTRETRYIGNORE | MB_ICONERROR | MB_TOPMOST.
            const int Answer = MessageBoxA(nullptr, Text, "NEURONCOMPONENTS::STANDARD_DUMP", 0x40012);
            if (Answer == IDABORT)
            {
                PostQuitMessage(-1);
                abort();
            }
            if (Answer == IDRETRY)
            {
                __debugbreak();
                return;
            }
            if (Answer == IDIGNORE && Severity == 2)
                GbIgnoreWarnings = true;
        }
    }

    // baseline copies every INI / registry string with operator new(strlen + 1) and a byte loop,
    // and frees it with operator delete (the routine). A null source stays null.
    char* DuplicateString(const char* Source)
    {
        if (Source == nullptr)
            return nullptr;
        const size_t Bytes = std::strlen(Source) + 1;
        char* Copy = new char[Bytes];
        std::memcpy(Copy, Source, Bytes);
        return Copy;
    }

    void FreeString(char* Text)
    {
        delete[] Text;
    }

    // ---------------------------------------------------------------------------------------
    // INI database (the built-in profile parser's storage)

    // baseline 0x8-byte key record: ctor the routine, dtor the routine. Value is null for a
    // line without '=' (distinct from an empty value).
    struct FIniKeyValue
    {
        FIniKeyValue(const char* KeyIn, const char* ValueIn)
            : Key(DuplicateString(KeyIn))
            , Value(DuplicateString(ValueIn))
        {
        }

        ~FIniKeyValue()
        {
            FreeString(Key);
            Key = nullptr;
            FreeString(Value);
            Value = nullptr;
        }

        FIniKeyValue(const FIniKeyValue&) = delete;
        FIniKeyValue& operator=(const FIniKeyValue&) = delete;

        char* Key;
        char* Value;
    };

    // baseline 0x10-byte section: {Name, std::list<FIniKeyValue*>}. Ctor the routine, dtor
    // . The section pointer itself is the IProfileParser section handle
    // (the routine is the identity cast).
    class FIniSection
    {
    public:
        explicit FIniSection(const char* NameIn) : Name(DuplicateString(NameIn)) {}

        ~FIniSection()
        {
            FreeString(Name);
            Name = nullptr;
            for (std::list<FIniKeyValue*>::iterator It = Keys.begin(); It != Keys.end(); It = Keys.erase(It))
            {
                if (*It != nullptr)
                    delete *It;
            }
        }

        FIniSection(const FIniSection&) = delete;
        FIniSection& operator=(const FIniSection&) = delete;

        // . A failed allocation stores a null record, as baseline does.
        bool AddKey(const char* Key, const char* Value)
        {
            FIniKeyValue* Pair = new (std::nothrow) FIniKeyValue(Key, Value);
            Keys.push_back(Pair);
            return true;
        }

        // . LineNumber is 1-based. "key = value", or the bare key when the
        // line had no '='. The "key = value" path is an unbounded sprintf, as baseline.
        bool GetLine(int LineNumber, unsigned int BufferSize, char* Buffer) const
        {
            std::list<FIniKeyValue*>::const_iterator It = FindLine(LineNumber);
            if (It == Keys.end())
                return false;
            const FIniKeyValue* Pair = *It;
            if (Pair->Value != nullptr)
            {
                sprintf(Buffer, "%s = %s", Pair->Key, Pair->Value);
                return true;
            }
            strncpy(Buffer, Pair->Key, BufferSize);
            Buffer[BufferSize - 1] = '\0';
            return true;
        }

        // the length GetLine would produce (0 when out of range).
        int GetLineLength(int LineNumber) const
        {
            std::list<FIniKeyValue*>::const_iterator It = FindLine(LineNumber);
            if (It == Keys.end())
                return 0;
            const FIniKeyValue* Pair = *It;
            const int KeyLength = (int)(std::strlen(Pair->Key));
            if (Pair->Value == nullptr)
                return KeyLength;
            return KeyLength + (int)(std::strlen(Pair->Value)) + 3;
        }

        // first key with _stricmp == 0. A key without a value yields "".
        bool GetValue(const char* Key, unsigned int BufferSize, char* Buffer) const
        {
            for (std::list<FIniKeyValue*>::const_iterator It = Keys.begin(); It != Keys.end(); ++It)
            {
                const FIniKeyValue* Pair = *It;
                if (_stricmp(Pair->Key, Key) != 0)
                    continue;
                if (Pair->Value != nullptr)
                {
                    strncpy(Buffer, Pair->Value, BufferSize);
                    Buffer[BufferSize - 1] = '\0';
                    return true;
                }
                Buffer[0] = '\0';
                return true;
            }
            return false;
        }

        // .
        int GetValueLength(const char* Key) const
        {
            for (std::list<FIniKeyValue*>::const_iterator It = Keys.begin(); It != Keys.end(); ++It)
            {
                const FIniKeyValue* Pair = *It;
                if (_stricmp(Pair->Key, Key) != 0)
                    continue;
                if (Pair->Value == nullptr)
                    return 0;
                return (int)(std::strlen(Pair->Value));
            }
            return 0;
        }

        char* Name;
        std::list<FIniKeyValue*> Keys;

    private:
        // The walk shared by 065b1230 / 065b12a0: n < 1 or past the end -> end().
        std::list<FIniKeyValue*>::const_iterator FindLine(int LineNumber) const
        {
            if (LineNumber < 1)
                return Keys.end();
            std::list<FIniKeyValue*>::const_iterator It = Keys.begin();
            if (It == Keys.end())
                return It;
            int Remaining = LineNumber;
            while (--Remaining != 0)
            {
                ++It;
                if (It == Keys.end())
                    return It;
            }
            return It;
        }
    };

    // Lexer token codes (baseline the routine return values).
    enum EIniToken : int
    {
        IniTokenWord = 0,
        IniTokenInclude = 1,
        IniTokenOpenBracket = 2,
        IniTokenCloseBracket = 3,
        IniTokenEquals = 4,
        IniTokenOverflow = 100,
        IniTokenNewline = 0x65,
        IniTokenEnd = 0x66,
    };

    // baseline 0x418-byte lexer: {+0 Buffer, +4 Size, +8 Cursor, +0xC Current, +0x10 Text[0x400],
    // +0x410 Length, +0x414 LastToken}. Ctor the routine, dtor the routine.
    class FIniLexer
    {
    public:
        FIniLexer() : Buffer(nullptr), Size(0) { Reset(); }

        ~FIniLexer()
        {
            delete[] Buffer;
            Buffer = nullptr;
        }

        FIniLexer(const FIniLexer&) = delete;
        FIniLexer& operator=(const FIniLexer&) = delete;

        // the whole file into one buffer ("rb"). A short read frees the
        // buffer and fails without resetting the cursor.
        bool LoadFile(const char* Path)
        {
            delete[] Buffer;
            Buffer = nullptr;
            FILE* File = fopen(Path, "rb");
            if (File == nullptr)
            {
                Reset();
                return false;
            }
            fseek(File, 0, SEEK_END);
            const long FileSize = ftell(File);
            fseek(File, 0, SEEK_SET);
            Buffer = new char[(size_t)FileSize];
            if (fread(Buffer, 1, (size_t)FileSize, File) != (size_t)FileSize)
            {
                delete[] Buffer;
                Buffer = nullptr;
                fclose(File);
                return false;
            }
            Size = (unsigned int)FileSize;
            fclose(File);
            Reset();
            return true;
        }

        // copy of the caller's buffer.
        bool LoadBuffer(const char* Source, unsigned int SourceSize)
        {
            delete[] Buffer;
            Buffer = nullptr;
            Buffer = new char[SourceSize];
            std::memcpy(Buffer, Source, SourceSize);
            Cursor = Buffer;
            Size = SourceSize;
            Reset();
            return true;
        }

        // .
        int Next()
        {
            LastToken = Scan();
            return LastToken;
        }

        // The inline "read one char" every baseline lexer path repeats: past the end -> -1.
        void Advance()
        {
            if (Cursor < Buffer + Size)
            {
                Current = (signed char)(*Cursor);
                ++Cursor;
            }
            else
            {
                Current = -1;
            }
        }

        // baseline +0x10. A word longer than 0x400 chars runs past it into Length, exactly as
        // the baseline loop (no bound) does.
        char Text[0x400];
        int Length;

    private:
        // .
        void Reset()
        {
            Length = 0;
            Current = '\n';
            Cursor = Buffer;
        }

        // .
        void TerminateText() { Text[Length] = '\0'; }

        int SingleCharToken(int Token)
        {
            Text[0] = (char)Current;
            ++Length;
            TerminateText();
            Advance();
            return Token;
        }

        // (switch on Current + 1, jump table 065b1ee8 / index 065b1f08).
        int Scan()
        {
            Length = 0;
            for (;;)
            {
                switch (Current)
                {
                case -1:
                    return IniTokenEnd;
                case '\t':
                case '\r':
                case ' ':
                    Advance();
                    continue;
                case ';':
                    // Comment: up to (and keeping current) the newline, or the end.
                    for (;;)
                    {
                        if (!(Cursor < Buffer + Size))
                        {
                            Current = -1;
                            break;
                        }
                        Current = (signed char)(*Cursor);
                        ++Cursor;
                        if (Current == '\n' || Current == -1)
                            break;
                    }
                    Length = 0;
                    continue;
                case '\n':
                    return SingleCharToken(IniTokenNewline);
                case '[':
                    return SingleCharToken(IniTokenOpenBracket);
                case ']':
                    return SingleCharToken(IniTokenCloseBracket);
                case '=':
                    return SingleCharToken(IniTokenEquals);
                default:
                    break;
                }

                // Word (065b1d45): everything up to a delimiter or the end. strchr also matches
                // a NUL byte (it finds the terminator), so NUL ends a word too.
                do
                {
                    Text[Length] = (char)Current;
                    ++Length;
                    Advance();
                } while (std::strchr(" \r\n\t[];=", Current) == nullptr && Current != -1);
                TerminateText();
                return _stricmp(Text, "@include") == 0 ? IniTokenInclude : IniTokenWord;
            }
        }

        char* Buffer;
        unsigned int Size;
        char* Cursor;
        int Current;
        int LastToken;
    };

    // baseline database at parser object +0x08: {std::list<FIniSection*> sorted by name,
    // CurrentSection}. Ctor the routine, dtor the routine.
    class FIniDatabase
    {
    public:
        FIniDatabase() : CurrentSection(nullptr) { LoadFile(nullptr); }

        ~FIniDatabase() { Clear(); }

        FIniDatabase(const FIniDatabase&) = delete;
        FIniDatabase& operator=(const FIniDatabase&) = delete;

        // a null path just empties the database.
        bool LoadFile(const char* Path)
        {
            Clear();
            CurrentSection = nullptr;
            if (Path == nullptr)
                return true;
            return ParseFile(Path);
        }

        // .
        bool LoadBuffer(const char* Source, unsigned int SourceSize)
        {
            Clear();
            CurrentSection = nullptr;
            if (Source == nullptr)
                return true;
            return ParseBuffer(Source, SourceSize);
        }

        // .
        void Clear()
        {
            for (std::list<FIniSection*>::iterator It = Sections.begin(); It != Sections.end(); It = Sections.erase(It))
            {
                if (*It != nullptr)
                    delete *It;
            }
        }

        // (thunk the routine): the list is sorted, so the walk stops at the
        // first name that sorts after the one asked for.
        FIniSection* FindSection(const char* Name) const
        {
            for (std::list<FIniSection*>::const_iterator It = Sections.begin(); It != Sections.end(); ++It)
            {
                const int Order = _stricmp((*It)->Name, Name);
                if (Order == 0)
                    return *It;
                if (Order > 0)
                    return nullptr;
            }
            return nullptr;
        }

        // .
        static FIniSection* SectionFromHandle(void* Handle) { return (FIniSection*)Handle; }

        std::list<FIniSection*> Sections;

    private:
        // inserted before the first section that sorts after it, so
        // duplicates are kept and a new one lands behind its equals.
        FIniSection* AddSection(const char* Name)
        {
            std::list<FIniSection*>::iterator Position = Sections.begin();
            while (Position != Sections.end() && _stricmp((*Position)->Name, Name) <= 0)
                ++Position;
            FIniSection* Section = new (std::nothrow) FIniSection(Name);
            Sections.insert(Position, Section);
            return Section;
        }

        // space-joins tokens until StopToken, a newline or the end
        // (the end reports a newline). 100 when the phrase would not fit.
        static int ReadPhrase(FIniLexer& Lexer, int StopToken, int MaxLength, char* Out)
        {
            bool bFirst = true;
            int Used = 0;
            int Token = Lexer.Next();
            if (Token == IniTokenOverflow)
            {
                Out[Used] = '\0';
                return Token;
            }
            for (;;)
            {
                if (Token == IniTokenNewline)
                {
                    Out[Used] = '\0';
                    return Token;
                }
                if (Token == IniTokenEnd)
                {
                    Out[Used] = '\0';
                    return IniTokenNewline;
                }
                if (Token == StopToken)
                {
                    Out[Used] = '\0';
                    return Token;
                }
                if (Used + Lexer.Length + 1 >= MaxLength - 1)
                {
                    Out[Used] = '\0';
                    return IniTokenOverflow;
                }
                if (!bFirst)
                {
                    Out[Used] = ' ';
                    ++Used;
                }
                std::strcpy(Out + Used, Lexer.Text);
                bFirst = false;
                Used += Lexer.Length;
                Token = Lexer.Next();
                if (Token == IniTokenOverflow)
                {
                    Out[Used] = '\0';
                    return Token;
                }
            }
        }

        // .
        bool ParseFile(const char* Path)
        {
            FIniLexer Lexer;
            if (!Lexer.LoadFile(Path))
                return false;
            return Parse(Lexer, Path);
        }

        // .
        bool ParseBuffer(const char* Source, unsigned int SourceSize)
        {
            FIniLexer Lexer;
            if (!Lexer.LoadBuffer(Source, SourceSize))
                return false;
            return Parse(Lexer, "(local buffer)");
        }

        // "first tail" when the phrase after the first word is not empty (065b1941 / 19d7).
        static void BuildKey(char* Out, const char* First, const char* Tail)
        {
            if (std::strlen(Tail) != 0)
                sprintf(Out, "%s %s", First, Tail);
            else
                std::strcpy(Out, First);
        }

        // (switch 065b1808, jump table 065b1abc). Unexpected input hits
        // INT3 exactly where baseline has one. A key before the first section dereferences the
        // null CurrentSection, as baseline does (no check at 065b1981 / 19aa / 1a31).
        bool Parse(FIniLexer& Lexer, const char* SourceName)
        {
            char First[0x40c];
            char Drive[0x100];
            char Directory[0x100];
            char Tail[0x404];
            char FileName[0x104];
            char Extension[0x104];
            char Line[0xc04];
            char Value[0x400];

            int Token = Lexer.Next();
            while (Token != IniTokenEnd)
            {
                switch (Token)
                {
                case IniTokenWord:
                {
                    std::strcpy(First, Lexer.Text);
                    const int TailResult = ReadPhrase(Lexer, IniTokenEquals, 0x400, Tail);
                    if (TailResult == IniTokenEquals)
                    {
                        if (ReadPhrase(Lexer, IniTokenNewline, 0x400, Value) != IniTokenNewline)
                        {
                            __debugbreak(); 
                            return false;
                        }
                        BuildKey(Line, First, Tail);
                        CurrentSection->AddKey(Line, Value);
                    }
                    else if (TailResult == IniTokenNewline)
                    {
                        BuildKey(Line, First, Tail);
                        CurrentSection->AddKey(Line, nullptr);
                    }
                    else
                    {
                        __debugbreak(); 
                        return false;
                    }
                    break;
                }
                case IniTokenInclude:
                {
                    if (Lexer.Next() != IniTokenWord)
                        return false;
                    _splitpath(Lexer.Text, Drive, Directory, FileName, Extension);
                    if (Drive[0] == '\0' && Directory[0] != '\\')
                    {
                        // Relative include: next to the file being parsed.
                        _splitpath(SourceName, Drive, Directory, FileName, Extension);
                        sprintf(Line, "%s%s%s", Drive, Directory, Lexer.Text);
                    }
                    else
                    {
                        std::strcpy(Line, Lexer.Text);
                    }
                    ParseFile(Line); // result ignored (065b18da)
                    Lexer.Advance();
                    break;
                }
                case IniTokenOpenBracket:
                {
                    const int NameResult = ReadPhrase(Lexer, IniTokenCloseBracket, 0x400, First);
                    if (NameResult == IniTokenCloseBracket)
                    {
                        CurrentSection = AddSection(First);
                    }
                    else if (NameResult != IniTokenNewline)
                    {
                        __debugbreak(); 
                        return false;
                    }
                    break;
                }
                case IniTokenNewline:
                    break;
                default:
                    // Token 100 (065b180f) and 3..0x63 (065b1a7a): INT3, then keep going.
                    __debugbreak();
                    break;
                }
                Token = Lexer.Next();
            }
            return true;
        }

        FIniSection* CurrentSection;
    };

    // ---------------------------------------------------------------------------------------
    // Built-in profile parser

    struct IProfileParser2;

    // The visitor IProfileParser2::EnumerateSections / EnumerateKeys call: vtable slot 0,
    // stdcall (visitor, face, name, value, context); a zero return stops the walk.
    struct IProfileParserVisitor
    {
        virtual int NEURONCOMPONENTS_API Visit(IProfileParser2* Parser, const char* Name, const char* Value,
                                    void* Context) = 0;
    };

    // "1.11_IProfileParser2", vtable 065b7118 on the parser object (+4). Private to neuroncomponents.dll:
    // SetINIConfig (flag 2) is its only baseline user. The slots it shares with IProfileParser
    // carry the same names so one override serves both faces.
    struct IProfileParser2 : INeuronComponent
    {
        virtual GENRESULT NEURONCOMPONENTS_API LoadFile(const char* Path, unsigned int Flags) = 0; // formerly UnnamedSlot3; +0x0C
        virtual GENRESULT NEURONCOMPONENTS_API LoadFromMemory(const char* Buffer, unsigned int Size) = 0;      // +0x10 065b45a0
        virtual void* NEURONCOMPONENTS_API OpenSection(const char* Name, unsigned int Flags) = 0;              // +0x14 065b49a0
        virtual int NEURONCOMPONENTS_API CloseSection(void* Handle) = 0;                                       // +0x18 065b49b0
        virtual int NEURONCOMPONENTS_API ReadLine(void* Handle, int Index, char* Buffer,
                                       unsigned int BufferSize) = 0;                                // +0x1C 065b49c0
        virtual int NEURONCOMPONENTS_API GetString(void* Handle, const char* Key, char* Buffer,
                                        unsigned int BufferSize) = 0;                               // +0x20 065b49d0
        virtual int NEURONCOMPONENTS_API EnumerateSections(IProfileParserVisitor* Visitor, void* Context) = 0; // +0x24 065b45c0
        virtual int NEURONCOMPONENTS_API EnumerateKeys(IProfileParserVisitor* Visitor, void* Handle,
                                            void* Context) = 0;                                     // +0x28 065b4620
    };

    class FProfileParserObject;

    // Inner (non-delegating) unknown at parser object +0x1c, vtable 065b7168.
    class FProfileParserInner final : public INeuronComponent
    {
    public:
        explicit FProfileParserInner(FProfileParserObject* OwnerIn) : RefCount(1), Owner(OwnerIn) {}

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
        unsigned long NEURONCOMPONENTS_API AddRef() override { return ++RefCount; } 
        unsigned long NEURONCOMPONENTS_API Release() override;

        unsigned long RefCount;      // +0x20
        FProfileParserObject* Owner; // +0x24
    };

    // baseline 0x28-byte parser object built by the routine: +0 IProfileParser (065b7144),
    // +4 IProfileParser2 (065b7118), +8 database, +0x18 outer, +0x1c inner unknown.
    class FProfileParserObject final : public IProfileParser, public IProfileParser2
    {
    public:
        FProfileParserObject() : Outer(nullptr), Inner(this) {}

        // (Clear, then the database dtor).
        ~FProfileParserObject() { Database.Clear(); }

        FProfileParserObject(const FProfileParserObject&) = delete;
        FProfileParserObject& operator=(const FProfileParserObject&) = delete;

        // Both faces forward their INeuronComponent slots to the outer unknown
        // (065b4960/4970/4980, adjustors 065b49e0/49f0/4a00).
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            return Outer->QueryInterface(Iid, Out);
        }

        unsigned long NEURONCOMPONENTS_API AddRef() override { return Outer->AddRef(); }

        unsigned long NEURONCOMPONENTS_API Release() override { return Outer->Release(); }

        // load an INI file. Flag 1 is refused.
        // formerly UnnamedSlot3
        GENRESULT NEURONCOMPONENTS_API LoadFile(const char* Path, unsigned int Flags) override
        {
            if ((Flags & 1) != 0)
                return GR_GENERIC;
            return Database.LoadFile(Path) ? GR_OK : GR_GENERIC;
        }

        // Callback(this, section name, Context) for every section, in
        // sorted order; a zero return stops the walk and returns 0.
        // formerly UnnamedSlot4
        int NEURONCOMPONENTS_API EnumerateSections(FSectionEnumCallback Callback, void* Context) override
        {
            if (Callback == nullptr)
                return 0;
            for (std::list<FIniSection*>::iterator It = Database.Sections.begin(); It != Database.Sections.end();
                 ++It)
            {
                if (Callback((IProfileParser*)this, (*It)->Name, Context) == 0)
                    return 0;
            }
            return 1;
        }

        // .
        GENRESULT NEURONCOMPONENTS_API LoadFromMemory(const char* Buffer, unsigned int Size) override
        {
            return Database.LoadBuffer(Buffer, Size) ? GR_OK : GR_GENERIC;
        }

        // only flags == 3 is answered.
        void* NEURONCOMPONENTS_API OpenSection(const char* Name, unsigned int Flags) override
        {
            if (Flags != 3)
                return nullptr;
            return Database.FindSection(Name);
        }

        // .
        int NEURONCOMPONENTS_API CloseSection(void* /*Handle*/) override { return 1; }

        // strlen of the line written, or with no buffer the length the
        // line would need. Index is 0-based here and 1-based in the section.
        int NEURONCOMPONENTS_API ReadLine(void* Handle, int Index, char* Buffer, unsigned int BufferSize) override
        {
            FIniSection* Section = FIniDatabase::SectionFromHandle(Handle);
            if (Section == nullptr)
                return 0;
            if (Buffer == nullptr)
                return Section->GetLineLength(Index + 1);
            if (!Section->GetLine(Index + 1, BufferSize, Buffer))
                return 0;
            return (int)(std::strlen(Buffer));
        }

        // same contract for a key's value.
        int NEURONCOMPONENTS_API GetString(void* Handle, const char* Key, char* Buffer, unsigned int BufferSize) override
        {
            FIniSection* Section = FIniDatabase::SectionFromHandle(Handle);
            if (Section == nullptr)
                return 0;
            if (Buffer == nullptr)
                return Section->GetValueLength(Key);
            if (!Section->GetValue(Key, BufferSize, Buffer))
                return 0;
            return (int)(std::strlen(Buffer));
        }

        // Visitor->Visit(face, section name, nullptr, Context).
        int NEURONCOMPONENTS_API EnumerateSections(IProfileParserVisitor* Visitor, void* Context) override
        {
            if (Visitor == nullptr)
                return 0;
            for (std::list<FIniSection*>::iterator It = Database.Sections.begin(); It != Database.Sections.end();
                 ++It)
            {
                if (Visitor->Visit((IProfileParser2*)this, (*It)->Name, nullptr, Context) == 0)
                    return 0;
            }
            return 1;
        }

        // Visitor->Visit(face, key, value, Context) for one section.
        int NEURONCOMPONENTS_API EnumerateKeys(IProfileParserVisitor* Visitor, void* Handle, void* Context) override
        {
            if (Visitor == nullptr || Handle == nullptr)
                return 0;
            FIniSection* Section = FIniDatabase::SectionFromHandle(Handle);
            if (Section == nullptr)
                return 0;
            for (std::list<FIniKeyValue*>::iterator It = Section->Keys.begin(); It != Section->Keys.end(); ++It)
            {
                if (Visitor->Visit((IProfileParser2*)this, (*It)->Key, (*It)->Value, Context) == 0)
                    return 0;
            }
            return 1;
        }

        // post-construction init, always 0.
        GENRESULT Initialize(AGGDESC* /*Desc*/) { return GR_OK; }

        FIniDatabase Database;     // +0x08
        INeuronComponent* Outer;       // +0x18
        FProfileParserInner Inner; // +0x1c
    };

    // baseline QI table 065b7174: {iid, face}. baseline stores the byte offset of the face (0 / 4);
    // the port's object layout is its own, so the face is named instead. Neither record uses
    // the high-bit (indirect) form the generic baseline walk also supports.
    enum class EParserFace
    {
        ProfileParser,
        ProfileParser2,
    };

    struct FParserQueryRecord
    {
        const char* Iid;
        EParserFace Face;
    };

    const FParserQueryRecord GParserQueryTable[] = {
        {Iids::ProfileParser, EParserFace::ProfileParser},
        {Iids::ProfileParser2, EParserFace::ProfileParser2},
        {nullptr, EParserFace::ProfileParser},
    };

    // table walk with strcmp; the face is AddRef'd (which reaches the
    // outer) and returned. A miss writes null and returns -3.
    GENRESULT NEURONCOMPONENTS_API FProfileParserInner::QueryInterface(const char* Iid, void** Out)
    {
        for (int Index = 0; GParserQueryTable[Index].Iid != nullptr; ++Index)
        {
            if (std::strcmp(GParserQueryTable[Index].Iid, Iid) != 0)
                continue;
            INeuronComponent* Face = nullptr;
            if (GParserQueryTable[Index].Face == EParserFace::ProfileParser)
                Face = (IProfileParser*)Owner;
            else
                Face = (IProfileParser2*)Owner;
            Face->AddRef();
            *Out = Face;
            return GR_OK;
        }
        *Out = nullptr;
        return GR_INTERFACE_UNSUPPORTED;
    }

    // at zero the count is reset to 1 and the owner object deleted.
    unsigned long NEURONCOMPONENTS_API FProfileParserInner::Release()
    {
        if (RefCount > 0)
            --RefCount;
        if (RefCount != 0)
            return RefCount;
        RefCount = 1;
        FProfileParserObject* Object = Owner;
        if (Object != nullptr)
            delete Object;
        return 0;
    }

    // Factories "ProfileParser" (the routine, vtable 065b7108) and "ProfileParser2"
    // (the routine, vtable 065b70f8): 0xC bytes {vtable, RefCount, Name}, identical slots.
    class FProfileParserFactory final : public IComponentFactory
    {
    public:
        explicit FProfileParserFactory(const char* NameIn) : RefCount(1), Name(NameIn) {}

        // .
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            *Out = nullptr;
            if (std::strcmp(Iid, Iids::ComponentFactory) != 0)
                return GR_INTERFACE_UNSUPPORTED;
            *Out = (IComponentFactory*)this;
            AddRef();
            return GR_OK;
        }

        // .
        unsigned long NEURONCOMPONENTS_API AddRef() override { return ++RefCount; }

        // .
        unsigned long NEURONCOMPONENTS_API Release() override
        {
            if (RefCount > 0)
                --RefCount;
            if (RefCount != 0)
                return RefCount;
            RefCount = 1;
            delete this;
            return 0;
        }

        // . The descriptor is the 0x14-byte aggregate form: +4 the factory
        // name, +8 the controlling outer unknown, +0xC the inner-unknown output.
        GENRESULT NEURONCOMPONENTS_API CreateInstance(AGGDESC* Desc, void** Instance) override
        {
            FAGGDESC14* Aggregate = (FAGGDESC14*)Desc;
            if (Aggregate->Size != 0x14 || std::strcmp(Aggregate->NameAt4, Name) != 0)
            {
                *Instance = nullptr;
                return GR_INTERFACE_UNSUPPORTED;
            }
            FProfileParserObject* Object = new (std::nothrow) FProfileParserObject();
            if (Object == nullptr)
            {
                *Instance = nullptr;
                return GR_OUT_OF_MEMORY;
            }
            INeuronComponent* OuterIn = Aggregate->OuterUnknown;
            Object->Outer = OuterIn;
            if (OuterIn == nullptr)
                Object->Outer = &Object->Inner;
            else
                *Aggregate->InnerUnknown = (INeuronComponent*)(&Object->Inner);

            const GENRESULT Result = Object->Initialize(Desc);
            if (Result != GR_OK)
            {
                if (Aggregate->InnerUnknown != nullptr)
                    *Aggregate->InnerUnknown = nullptr;
                delete Object;
                *Instance = nullptr;
                return Result;
            }
            *Instance = (IProfileParser*)Object;
            return GR_OK;
        }

    private:
        unsigned long RefCount; // +4
        const char* Name;       // +8
    };

    // ---------------------------------------------------------------------------------------
    // COManager

    // baseline registry link, payload of a 0x18-byte list node: {+8 factory, +0xC name,
    // +0x10 priority, +0x14 name of the library that registered it}.
    struct FFactoryLink
    {
        IComponentFactory* Factory;
        char* Name;
        unsigned long Priority;
        char* LibraryName;
    };

    // / the routine / the inline copy in the routine.
    void DestroyLink(FFactoryLink& Link)
    {
        if (Link.Factory != nullptr)
        {
            Link.Factory->Release();
            Link.Factory = nullptr;
        }
        Link.Priority = 0;
        FreeString(Link.Name);
        Link.Name = nullptr;
        FreeString(Link.LibraryName);
        Link.LibraryName = nullptr;
    }

    // baseline library record (0x10-byte list node): {+8 HMODULE, +0xC name without extension}.
    struct FLibraryRecord
    {
        HMODULE Module;
        char* Name;
    };

    typedef std::list<FFactoryLink> FFactoryLinkList;
    // baseline std::map keyed by NEURONCOMPONENTS_CRC::GetCRC32(name), ascending (COManager +0x08).
    typedef std::map<unsigned long, FFactoryLinkList> FComponentRegistry;

    class FCOManager final : public ICOManager
    {
    public:
        FCOManager();

        FCOManager(const FCOManager&) = delete;
        FCOManager& operator=(const FCOManager&) = delete;

        // ICOManager here, every other IID through the aggregated
        // parser's inner unknown.
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            *Out = nullptr;
            if (std::strcmp(Iid, Iids::COManager) == 0)
            {
                AddRef();
                *Out = (ICOManager*)this;
                return GR_OK;
            }
            if (ParserInner != nullptr)
                return ParserInner->QueryInterface(Iid, Out);
            return GR_INTERFACE_UNSUPPORTED;
        }

        // (both slots).
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }

        GENRESULT NEURONCOMPONENTS_API CreateInstance(AGGDESC* Desc, void** Instance) override;
        GENRESULT NEURONCOMPONENTS_API RegisterComponent(INeuronComponent* Factory, const char* Name, void* Priority) override;
        GENRESULT NEURONCOMPONENTS_API UnregisterComponent(INeuronComponent* Factory, const char* Name) override;
        GENRESULT NEURONCOMPONENTS_API EnumerateComponents(const char* Name, FComponentEnumCallback Callback,
                                                void* Context) override;
        GENRESULT NEURONCOMPONENTS_API AddLibrary(const char* DllName) override;
        GENRESULT NEURONCOMPONENTS_API RemoveLibrary(const char* DllName) override;
        GENRESULT NEURONCOMPONENTS_API Shutdown() override;
        GENRESULT NEURONCOMPONENTS_API SetINIConfig(const char* IniFile, unsigned int Flags) override;

        // , registered with _onexit by NEURONCOMPONENTS_Acquire.
        void TearDown();

    private:
        bool LoadConfiguration();
        bool LoadLibraries();
        bool UnregisterLibraryComponents(const char* LibraryName);

        bool bInitialized;                   // +0x04
        FComponentRegistry Registry;         // +0x08
        std::list<FLibraryRecord> Libraries; // +0x20 (count +0x24)
        IProfileParser* Parser;              // +0x28
        INeuronComponent* ParserInner;           // +0x2c
        char CurrentLibrary[0x104];          // +0x30
        char* DllPath;                       // +0x134
    };

    // .
    FCOManager::FCOManager()
        : bInitialized(false)
        , Parser(nullptr)
        , ParserInner(nullptr)
        , DllPath(nullptr)
    {
        CurrentLibrary[0] = '\0';

        IComponentFactory* Factory = new (std::nothrow) FProfileParserFactory("ProfileParser");
        RegisterComponent(Factory, "ProfileParser", (void*)((uintptr_t)0x80000000u));
        Factory->Release();

        Factory = new (std::nothrow) FProfileParserFactory("ProfileParser2");
        RegisterComponent(Factory, "ProfileParser2", (void*)((uintptr_t)0x80000000u));

        // {0x14, "ProfileParser2", outer = this, &this+0x2c, 0}: the parser is aggregated, its
        // IProfileParser* lands in +0x28 and its inner unknown in +0x2c.
        FAGGDESC14 Desc = {};
        Desc.Size = 0x14;
        Desc.NameAt4 = "ProfileParser2";
        Desc.OuterUnknown = (ICOManager*)this;
        Desc.InnerUnknown = &ParserInner;
        Factory->CreateInstance((AGGDESC*)(&Desc), (void**)(&Parser));
        Factory->Release();
    }

    // . The lookup name is desc+4; none, or exactly "COManager"
    // (case-sensitive), answers with the manager itself.
    GENRESULT NEURONCOMPONENTS_API FCOManager::CreateInstance(AGGDESC* Desc, void** Instance)
    {
        *Instance = nullptr;
        if (Desc == nullptr || Desc->interface_name == nullptr || std::strcmp(Desc->interface_name, "COManager") == 0)
        {
            *Instance = (ICOManager*)this;
            AddRef();
            return GR_OK;
        }

        FComponentRegistry::iterator Bucket = Registry.find(NEURONCOMPONENTS_CRC::GetCRC32(Desc->interface_name));
        if (Bucket == Registry.end())
            return GR_INTERFACE_UNSUPPORTED;
        FFactoryLinkList& Links = Bucket->second;
        if (Links.empty())
            return GR_INTERFACE_UNSUPPORTED;

        GENRESULT Result = GR_INTERFACE_UNSUPPORTED;
        for (FFactoryLinkList::iterator It = Links.begin(); It != Links.end(); ++It)
        {
            Result = It->Factory->CreateInstance(Desc, Instance);
            if (Result == GR_OK)
                return GR_OK;
        }
        return Result;
    }

    // . The link goes in front of the first one whose priority is <= the
    // new one (unsigned compare, JNC at 065b2276), so among equal priorities the most recent
    // registration wins.
    GENRESULT NEURONCOMPONENTS_API FCOManager::RegisterComponent(INeuronComponent* Factory, const char* Name, void* Priority)
    {
        if (Factory == nullptr || Name == nullptr)
            return GR_INVALID_PARMS;

        FFactoryLinkList& Links = Registry[NEURONCOMPONENTS_CRC::GetCRC32(Name)];
        const unsigned long NewPriority = (unsigned long)((uintptr_t)Priority);
        FFactoryLinkList::iterator Position = Links.begin();
        while (Position != Links.end() && NewPriority < Position->Priority)
            ++Position;

        FFactoryLink Link;
        Link.Factory = (IComponentFactory*)Factory;
        Link.Name = DuplicateString(Name);
        Link.Priority = NewPriority;
        Link.LibraryName = DuplicateString(CurrentLibrary);
        Factory->AddRef();
        Links.insert(Position, Link);
        return GR_OK;
    }

    // . Without a name every bucket is searched. The first link holding
    // the factory is destroyed (factory released), an emptied bucket erased.
    GENRESULT NEURONCOMPONENTS_API FCOManager::UnregisterComponent(INeuronComponent* Factory, const char* Name)
    {
        FComponentRegistry::iterator First = Registry.begin();
        FComponentRegistry::iterator Last = Registry.end();
        if (Name != nullptr)
        {
            First = Registry.find(NEURONCOMPONENTS_CRC::GetCRC32(Name));
            if (First == Registry.end())
                return GR_INVALID_PARMS;
            Last = First;
            ++Last;
        }

        for (FComponentRegistry::iterator Bucket = First; Bucket != Last; ++Bucket)
        {
            FFactoryLinkList& Links = Bucket->second;
            for (FFactoryLinkList::iterator It = Links.begin(); It != Links.end(); ++It)
            {
                if ((INeuronComponent*)(It->Factory) != Factory)
                    continue;
                DestroyLink(*It);
                Links.erase(It);
                if (Links.empty())
                    Registry.erase(Bucket);
                return GR_OK;
            }
        }
        return GR_INVALID_PARMS;
    }

    // Callback(factory, name, priority, Context) for every link (of one
    // bucket when a name is given); a zero return stops the walk. Always 0 unless Callback is
    // null.
    GENRESULT NEURONCOMPONENTS_API FCOManager::EnumerateComponents(const char* Name, FComponentEnumCallback Callback,
                                                        void* Context)
    {
        if (Callback == nullptr)
            return GR_INVALID_PARMS;

        FComponentRegistry::iterator First = Registry.begin();
        FComponentRegistry::iterator Last = Registry.end();
        if (Name != nullptr)
        {
            First = Registry.find(NEURONCOMPONENTS_CRC::GetCRC32(Name));
            if (First == Registry.end())
                return GR_OK;
            Last = First;
            ++Last;
        }

        for (FComponentRegistry::iterator Bucket = First; Bucket != Last; ++Bucket)
        {
            FFactoryLinkList& Links = Bucket->second;
            for (FFactoryLinkList::iterator It = Links.begin(); It != Links.end(); ++It)
            {
                if (Callback((INeuronComponent*)(It->Factory), It->Name,
                             (void*)((uintptr_t)(It->Priority)), Context) == 0)
                    return GR_OK;
            }
        }
        return GR_OK;
    }

    // . SearchPathA over DllPath is the only lookup: a name it cannot find
    // is an ERROR dump and -1, there is no fallback directory.
    GENRESULT NEURONCOMPONENTS_API FCOManager::AddLibrary(const char* DllName)
    {
        if (DllName == nullptr)
            return GR_INVALID_PARMS;

        char FoundPath[0x104];
        char FileName[0x104];
        char Message[0x2000];
        if (SearchPathA(DllPath, DllName, nullptr, 0x104, FoundPath, nullptr) == 0)
        {
            FDUMP(DumpError, GErrorFormat, GNeuronComponentsSourceFile, 0x25a,
                  FormatDumpText(Message, "NEURONCOMPONENTS: AddLibrary: unable to find dll '%s' in path\n", DllName));
            return GR_GENERIC;
        }

        // Components the library registers from its DllMain are tagged with this name.
        _splitpath(FoundPath, nullptr, nullptr, FileName, nullptr);
        std::strcpy(CurrentLibrary, FileName);

        HMODULE Module = LoadLibraryA(FoundPath);
        if (Module == nullptr)
        {
            FDUMP(DumpError, GErrorFormat, GNeuronComponentsSourceFile, 0x264,
                  FormatDumpText(Message, "NEURONCOMPONENTS: AddLibrary: unable to load dll '%s', ignoring...\n", DllName));
            return GR_INVALID_PARMS;
        }

        unsigned int Major;
        unsigned int Minor;
        unsigned int Build;
        NEURONCOMPONENTS_GetDllVersion(FoundPath, &Major, &Minor, &Build);
        FDUMP(DumpNotice, GNoticeFormat, GNeuronComponentsSourceFile, 0x26a,
              FormatDumpText(Message, "NEURONCOMPONENTS: AddLibrary: DLL '%s' [%d.%d.%d]\n", FoundPath, Major, Minor, Build));

        FLibraryRecord Record;
        Record.Module = Module;
        Record.Name = DuplicateString(FileName);
        Libraries.push_back(Record);
        CurrentLibrary[0] = '\0';
        return GR_OK;
    }

    // .
    GENRESULT NEURONCOMPONENTS_API FCOManager::RemoveLibrary(const char* DllName)
    {
        if (DllName == nullptr)
            return GR_INVALID_PARMS;

        char FileName[0x104];
        _splitpath(DllName, nullptr, nullptr, FileName, nullptr);
        for (std::list<FLibraryRecord>::iterator It = Libraries.begin(); It != Libraries.end(); ++It)
        {
            if (_stricmp(It->Name, FileName) != 0)
                continue;
            UnregisterLibraryComponents(It->Name);
            FreeLibrary(It->Module);
            FreeString(It->Name);
            It->Name = nullptr;
            Libraries.erase(It);
            return GR_OK;
        }
        return GR_GENERIC;
    }

    // every link registered while LibraryName was loading.
    bool FCOManager::UnregisterLibraryComponents(const char* LibraryName)
    {
        FComponentRegistry::iterator Bucket = Registry.begin();
        while (Bucket != Registry.end())
        {
            FFactoryLinkList& Links = Bucket->second;
            FFactoryLinkList::iterator It = Links.begin();
            while (It != Links.end())
            {
                if (_stricmp(LibraryName, It->LibraryName) != 0)
                {
                    ++It;
                    continue;
                }
                DestroyLink(*It);
                It = Links.erase(It);
            }
            if (Links.empty())
                Bucket = Registry.erase(Bucket);
            else
                ++Bucket;
        }
        return true;
    }

    // . The parser's inner unknown is released (which deletes the parser)
    // but +0x28 keeps its stale pointer, as baseline.
    GENRESULT NEURONCOMPONENTS_API FCOManager::Shutdown()
    {
        if (ParserInner != nullptr)
        {
            ParserInner->Release();
            ParserInner = nullptr;
        }

        while (!Registry.empty())
        {
            FComponentRegistry::iterator Bucket = Registry.begin();
            FFactoryLinkList& Links = Bucket->second;
            while (!Links.empty())
            {
                DestroyLink(Links.front());
                Links.pop_front();
            }
            Registry.erase(Bucket);
        }

        while (!Libraries.empty())
        {
            FLibraryRecord& Record = Libraries.front();
            FreeLibrary(Record.Module);
            FreeString(Record.Name);
            Record.Name = nullptr;
            Libraries.pop_front();
        }

        FreeString(DllPath);
        DllPath = nullptr;
        bInitialized = false;
        return GR_OK;
    }

    // . Flag 2: IniFile is the INI text itself; flag 1: refused by the
    // parser; both: -2. The parser result is returned, LoadConfiguration's is not.
    GENRESULT NEURONCOMPONENTS_API FCOManager::SetINIConfig(const char* IniFile, unsigned int Flags)
    {
        unsigned int Major;
        unsigned int Minor;
        unsigned int Build;
        NEURONCOMPONENTS_GetVersion(&Major, &Minor, &Build);

        GENRESULT Result = GR_INVALID_PARMS;
        if ((Flags & 3) == 3)
            return Result;

        bool bParsed = true;
        if ((Flags & 2) != 0)
        {
            IProfileParser2* Parser2 = nullptr;
            if (Parser->QueryInterface(Iids::ProfileParser2, (void**)(&Parser2)) < 0)
            {
                bParsed = false;
            }
            else
            {
                Result = Parser2->LoadFromMemory(IniFile, (unsigned int)(std::strlen(IniFile)));
                if (Parser2 != nullptr)
                    Parser2->Release();
            }
        }
        else
        {
            Result = Parser->LoadFile(IniFile, (Flags & 1) != 0 ? 1u : 0u);
        }

        if (bParsed && Result == GR_OK)
        {
            LoadConfiguration();
            return Result;
        }
        FDUMP(DumpWarning, GWarningFormat, GNeuronComponentsSourceFile, 0x2e6, "NEURONCOMPONENTS: SetINIConfig: unable to initialize parser");
        return Result;
    }

    // [NEURONCOMPONENTS] DllPath / IgnoreNEURONCOMPONENTSEnv, the NEURONCOMPONENTS environment variable,
    // then [Libraries].
    bool FCOManager::LoadConfiguration()
    {
        if (bInitialized)
            return bInitialized;

        char Drive[0x20];
        char BasePath[0x104];
        char Value[0x400];
        char ModulePath[0x104];
        char Directory[0x104];
        char Message[0x2000];

        bool bUseEnvironment = true;
        void* Section = Parser->OpenSection("NEURONCOMPONENTS", 3);
        if (Section != nullptr)
        {
            if (Parser->GetString(Section, "DllPath", Value, 0x400) != 0)
            {
                GetModuleFileNameA(nullptr, ModulePath, 0x104);
                _splitpath(ModulePath, Drive, Directory, nullptr, nullptr);
                _makepath(BasePath, Drive, Directory, nullptr, nullptr);

                if (Value[0] == '\\' || Value[0] == '/' || Value[1] == ':')
                {
                    FreeString(DllPath);
                    DllPath = DuplicateString(Value);
                }
                else
                {
                    // Relative to the exe directory: drop its trailing separator, climb one
                    // directory per leading "..\", skip ".\". A value like ".x" never advances
                    // the cursor and loops forever, exactly as baseline (065b2c26..2ca4).
                    char* Tail = BasePath + std::strlen(BasePath) - 1;
                    if (*Tail == '/' || *Tail == '\\')
                        *Tail = '\0';
                    const char* Cursor = Value;
                    if (Value[0] == '.')
                    {
                        do
                        {
                            if (Cursor[1] == '.')
                            {
                                if (Tail != BasePath)
                                {
                                    for (;;)
                                    {
                                        if (*Tail == '/' || *Tail == '\\')
                                            break;
                                        --Tail;
                                        if (Tail == BasePath)
                                            break;
                                    }
                                }
                                if (Tail == nullptr)
                                {
                                    // baseline keeps this branch (065b2c57) although Tail always
                                    // points into BasePath.
                                    FDUMP(DumpError, GErrorFormat, GNeuronComponentsSourceFile, 0x328,
                                          "DllPath key in ini file specifies invalid path");
                                    std::strcpy(BasePath, "\\");
                                    Tail = BasePath;
                                    Cursor += 3;
                                }
                                else
                                {
                                    *Tail = '\0';
                                    Cursor += 3;
                                }
                            }
                            else if (Cursor[1] == '\\' || Cursor[1] == '/')
                            {
                                Cursor += 2;
                            }
                        } while (*Cursor == '.');
                    }
                    std::strcat(BasePath, "\\");
                    std::strcat(BasePath, Cursor);
                    // baseline does not free the previous DllPath here (065b2cf6).
                    DllPath = DuplicateString(BasePath);
                }
            }

            if (Parser->GetString(Section, "IgnoreNEURONCOMPONENTSEnv", Value, 0x400) != 0)
            {
                if (_strnicmp(Value, "true", std::strlen("true")) == 0)
                    bUseEnvironment = false;
                else if (_strnicmp(Value, "false", std::strlen("false")) != 0 && MsvcrtAtoi(Value) != 0)
                    bUseEnvironment = false;
            }
        }

        if (bUseEnvironment)
        {
            const char* Environment = getenv("NEURONCOMPONENTS");
            if (Environment != nullptr)
            {
                FreeString(DllPath);
                DllPath = DuplicateString(Environment);
            }
        }

        FDUMP(DumpNotice, GNoticeFormat, GNeuronComponentsSourceFile, 0x354,
              FormatDumpText(Message, "NEURONCOMPONENTS: initialize: dllpath: '%s'\n", DllPath));
        if (!LoadLibraries())
        {
            FDUMP(DumpWarning, GWarningFormat, GNeuronComponentsSourceFile, 0x357,
                  "NEURONCOMPONENTS: initialize: unable to load libraries from [Libraries]");
            return false;
        }
        bInitialized = true;
        return true;
    }

    // one AddLibrary per [Libraries] line (first word of the line).
    bool FCOManager::LoadLibraries()
    {
        bool bLoaded = false;
        void* Section = Parser->OpenSection("Libraries", 3);
        if (Section == nullptr)
        {
            FDUMP(DumpError, GErrorFormat, GNeuronComponentsSourceFile, 0x369,
                  "NEURONCOMPONENTS: load_libraries: [Libraries] group not found in INI\n");
            return bLoaded;
        }
        FDUMP(DumpNotice, GNoticeFormat, GNeuronComponentsSourceFile, 0x370,
              "NEURONCOMPONENTS: initialize: Loading DLL's from [Libraries] group.\n");

        char Line[0x104];
        int LineIndex = 1;
        if (Parser->ReadLine(Section, 0, Line, 0x104) == 0)
            return bLoaded;
        do
        {
            char* Cursor = Line;
            char Ch = *Cursor;
            if (Ch != '\0')
            {
                while (Ch == ' ' || Ch == '\t' || Ch == '\n' || Ch == '\r')
                {
                    Ch = *++Cursor;
                    if (Ch == '\0')
                        break;
                }
            }

            char* Start = Cursor;
            Ch = *Cursor;
            if (Ch != ';')
            {
                if (Ch == '"')
                {
                    // baseline tests for the closing quote before advancing, so it stops on the
                    // opening one and the name becomes the single character '"' (065b2f7a).
                    for (;;)
                    {
                        if (Ch == '"')
                            break;
                        Ch = *++Cursor;
                        if (Ch == '\0')
                            break;
                    }
                    Cursor[1] = '\0';
                }
                else
                {
                    while (Ch != '\0' && Ch != ' ' && Ch != '\t' && Ch != ';' && Ch != '\n' && Ch != '\r')
                        Ch = *++Cursor;
                    *Cursor = '\0';
                }
                if (AddLibrary(Start) >= 0)
                    bLoaded = true;
            }
        } while (Parser->ReadLine(Section, LineIndex++, Line, 0x104) != 0);
        return bLoaded;
    }

    // frees the library records (no FreeLibrary) and destroys every
    // registry link (factories released). The parser and DllPath are left alone.
    void FCOManager::TearDown()
    {
        while (!Libraries.empty())
        {
            FreeString(Libraries.front().Name);
            Libraries.front().Name = nullptr;
            Libraries.pop_front();
        }
        for (FComponentRegistry::iterator Bucket = Registry.begin(); Bucket != Registry.end(); ++Bucket)
        {
            FFactoryLinkList& Links = Bucket->second;
            while (!Links.empty())
            {
                DestroyLink(Links.front());
                Links.pop_front();
            }
        }
        Registry.clear();
    }

    // ---------------------------------------------------------------------------------------
    // RegExp component: Henry Spencer's regexp (regcomp / regexec) behind IRegExp

    // Node opcodes (OP(p) = p[0]; NEXT offset in p[1..2], big-endian; operand from p+3).
    enum ERegexOpcode : char
    {
        RegOpEnd = 0,
        RegOpBol = 1,
        RegOpEol = 2,
        RegOpAny = 3,
        RegOpAnyOf = 4,
        RegOpAnyBut = 5,
        RegOpBranch = 6,
        RegOpBack = 7,
        RegOpExactly = 8,
        RegOpNothing = 9,
        RegOpStar = 10,
        RegOpPlus = 11,
        RegOpWordStart = 12, // "\<"
        RegOpWordEnd = 13,   // "\>"
        RegOpOpen = 20,
        RegOpClose = 30,
    };

    const int RegSubExpressions = 10;
    const unsigned char RegMagic = 0x9c;

    // Parse flags.
    enum ERegexParseFlag : int
    {
        RegWorst = 0,
        RegHasWidth = 1,
        RegSimple = 2,
        RegSpStart = 4,
    };

    // baseline compiled program, malloc'd as 0x60 + code size: startp +0, endp +0x28,
    // regstart +0x50, reganch +0x51, regmust +0x54, regmlen +0x58, program +0x5c.
    struct FRegexpProgram
    {
        const char* StartP[RegSubExpressions];
        const char* EndP[RegSubExpressions];
        char RegStart;
        char RegAnch;
        char* RegMust;
        int RegMLen;
        char Program[1];
    };

    // Compiler / matcher globals (not reentrant, as baseline).
    const char* GRegParse = nullptr; // ; borrowed pattern cursor
    int GRegNpar = 0;             
    char GRegDummy = 0;           
    char* GRegCode = nullptr;     
    long GRegSize = 0;            
    const char* GRegInput = nullptr; // ; borrowed subject cursor
    const char* GRegBol = nullptr;   // ; subject start
    const char** GRegStartP = nullptr; // ; writable capture pointers
    const char** GRegEndP = nullptr;   // ; writable capture pointers

    char* RegBranch(int* FlagP);
    char* RegPiece(int* FlagP);
    char* RegAtom(int* FlagP);

    char* RegOperand(char* Node) { return Node + 3; }

    // .
    char* RegNext(char* Node)
    {
        if (Node == &GRegDummy)
            return nullptr;
        const int Offset = (((unsigned char)(Node[1])) << 8) + (unsigned char)(Node[2]);
        if (Offset == 0)
            return nullptr;
        if (Node[0] == RegOpBack)
            return Node - Offset;
        return Node + Offset;
    }

    // emits a node (or only counts it in the sizing pass).
    char* RegNode(char Op)
    {
        char* Ret = GRegCode;
        if (Ret == &GRegDummy)
        {
            GRegSize += 3;
            return Ret;
        }
        char* Ptr = Ret;
        *Ptr++ = Op;
        *Ptr++ = '\0';
        *Ptr++ = '\0';
        GRegCode = Ptr;
        return Ret;
    }

    // .
    void RegC(char Byte)
    {
        if (GRegCode != &GRegDummy)
            *GRegCode++ = Byte;
        else
            ++GRegSize;
    }

    // inserts a node in front of Operand, moving the code up 3 bytes.
    void RegInsert(char Op, char* Operand)
    {
        if (GRegCode == &GRegDummy)
        {
            GRegSize += 3;
            return;
        }
        char* Source = GRegCode;
        GRegCode += 3;
        char* Destination = GRegCode;
        while (Source > Operand)
            *--Destination = *--Source;
        char* Place = Operand;
        *Place++ = Op;
        *Place++ = '\0';
        *Place++ = '\0';
    }

    // points the last node of a chain at Value.
    void RegTail(char* Node, char* Value)
    {
        if (Node == &GRegDummy)
            return;
        char* Scan = Node;
        for (;;)
        {
            char* Temp = RegNext(Scan);
            if (Temp == nullptr)
                break;
            Scan = Temp;
        }
        int Offset = 0;
        if (Scan[0] == RegOpBack)
            Offset = (int)(Scan - Value);
        else
            Offset = (int)(Value - Scan);
        Scan[1] = (char)((Offset >> 8) & 0xff);
        Scan[2] = (char)(Offset & 0xff);
    }

    // RegTail on the operand of a BRANCH.
    void RegOpTail(char* Node, char* Value)
    {
        if (Node == nullptr || Node == &GRegDummy || Node[0] != RegOpBranch)
            return;
        RegTail(RegOperand(Node), Value);
    }

    // alternation, optionally parenthesised. Alternatives are separated
    // by '|' or a newline.
    char* Reg(int bParen, int* FlagP)
    {
        int ParenNumber = 0;
        char* Ret = nullptr;
        int Flags = 0;

        *FlagP = RegHasWidth;
        if (bParen != 0)
        {
            if (GRegNpar >= RegSubExpressions)
                return nullptr;
            ParenNumber = GRegNpar;
            ++GRegNpar;
            Ret = RegNode((char)(RegOpOpen + ParenNumber));
        }

        char* Branch = RegBranch(&Flags);
        if (Branch == nullptr)
            return nullptr;
        if (Ret != nullptr)
            RegTail(Ret, Branch);
        else
            Ret = Branch;
        for (;;)
        {
            if ((Flags & RegHasWidth) == 0)
                *FlagP &= ~RegHasWidth;
            *FlagP |= Flags & RegSpStart;
            if (*GRegParse != '|' && *GRegParse != '\n')
                break;
            ++GRegParse;
            Branch = RegBranch(&Flags);
            if (Branch == nullptr)
                return nullptr;
            RegTail(Ret, Branch);
        }

        char* Ender = RegNode(bParen != 0 ? (char)(RegOpClose + ParenNumber) : RegOpEnd);
        RegTail(Ret, Ender);
        for (char* Scan = Ret; Scan != nullptr; Scan = RegNext(Scan))
            RegOpTail(Scan, Ender);

        if (bParen != 0)
        {
            if (*GRegParse++ != ')')
                return nullptr;
        }
        else if (*GRegParse != '\0')
        {
            return nullptr;
        }
        return Ret;
    }

    // one alternative - a run of pieces.
    char* RegBranch(int* FlagP)
    {
        *FlagP = RegWorst;
        char* Ret = RegNode(RegOpBranch);
        char* Chain = nullptr;
        while (*GRegParse != '\0' && *GRegParse != ')' && *GRegParse != '\n' && *GRegParse != '|')
        {
            int Flags = 0;
            char* Latest = RegPiece(&Flags);
            if (Latest == nullptr)
                return nullptr;
            *FlagP |= Flags & RegHasWidth;
            if (Chain == nullptr)
                *FlagP |= Flags & RegSpStart;
            else
                RegTail(Chain, Latest);
            Chain = Latest;
        }
        if (Chain == nullptr)
            RegNode(RegOpNothing);
        return Ret;
    }

    bool RegIsMultiplier(char Ch) { return Ch == '*' || Ch == '+' || Ch == '?'; }

    // an atom with an optional '*', '+' or '?'.
    char* RegPiece(int* FlagP)
    {
        int Flags = 0;
        char* Ret = RegAtom(&Flags);
        if (Ret == nullptr)
            return nullptr;

        const char Op = *GRegParse;
        if (!RegIsMultiplier(Op))
        {
            *FlagP = Flags;
            return Ret;
        }
        if ((Flags & RegHasWidth) == 0 && Op != '?')
            return nullptr;
        *FlagP = Op != '+' ? (RegWorst | RegSpStart) : (RegWorst | RegHasWidth);

        if (Op == '*' && (Flags & RegSimple) != 0)
        {
            RegInsert(RegOpStar, Ret);
        }
        else if (Op == '*')
        {
            // x* as (x&|), where & means "self".
            RegInsert(RegOpBranch, Ret);
            RegOpTail(Ret, RegNode(RegOpBack));
            RegOpTail(Ret, Ret);
            RegTail(Ret, RegNode(RegOpBranch));
            RegTail(Ret, RegNode(RegOpNothing));
        }
        else if (Op == '+' && (Flags & RegSimple) != 0)
        {
            RegInsert(RegOpPlus, Ret);
        }
        else if (Op == '+')
        {
            // x+ as x(&|).
            char* Next = RegNode(RegOpBranch);
            RegTail(Ret, Next);
            RegTail(RegNode(RegOpBack), Ret);
            RegTail(Next, RegNode(RegOpBranch));
            RegTail(Ret, RegNode(RegOpNothing));
        }
        else if (Op == '?')
        {
            // x? as (x|).
            RegInsert(RegOpBranch, Ret);
            RegTail(Ret, RegNode(RegOpBranch));
            char* Next = RegNode(RegOpNothing);
            RegTail(Ret, Next);
            RegOpTail(Ret, Next);
        }
        ++GRegParse;
        if (RegIsMultiplier(*GRegParse))
            return nullptr;
        return Ret;
    }

    // the lowest level. Besides Spencer's original this build knows "\<"
    // and "\>" (word start / end), backslash escapes inside literal runs, and the class
    // escapes \- \\ \d \n \r \s \t (any other class escape is dropped).
    char* RegAtom(int* FlagP)
    {
        char* Ret = nullptr;
        *FlagP = RegWorst;

        const char First = *GRegParse++;
        bool bLiteral = false;
        switch (First)
        {
        case '^':
            return RegNode(RegOpBol);
        case '$':
            return RegNode(RegOpEol);
        case '.':
            Ret = RegNode(RegOpAny);
            *FlagP |= RegHasWidth | RegSimple;
            return Ret;
        case '[':
        {
            if (*GRegParse == '^')
            {
                Ret = RegNode(RegOpAnyBut);
                ++GRegParse;
            }
            else
            {
                Ret = RegNode(RegOpAnyOf);
            }
            if (*GRegParse == ']' || *GRegParse == '-')
                RegC(*GRegParse++);
            while (*GRegParse != '\0' && *GRegParse != ']')
            {
                const char Ch = *GRegParse++;
                if (Ch == '-')
                {
                    if (*GRegParse == ']' || *GRegParse == '\0')
                    {
                        RegC('-');
                    }
                    else
                    {
                        int Class = (unsigned char)(GRegParse[-2]) + 1;
                        const int ClassEnd = (unsigned char)(*GRegParse);
                        if (Class > ClassEnd + 1)
                            return nullptr;
                        for (; Class <= ClassEnd; ++Class)
                            RegC((char)Class);
                        ++GRegParse;
                    }
                }
                else if (Ch == '\\')
                {
                    const char Escaped = *GRegParse++;
                    switch (Escaped)
                    {
                    case '-':
                        RegC('-');
                        break;
                    case '\\':
                        RegC('\\');
                        break;
                    case 'd':
                        for (int Digit = '0'; Digit <= '9'; ++Digit)
                            RegC((char)Digit);
                        break;
                    case 'n':
                        RegC('\n');
                        break;
                    case 'r':
                        RegC('\r');
                        break;
                    case 's':
                        RegC(' ');
                        RegC('\t');
                        RegC('\n');
                        RegC('\r');
                        break;
                    case 't':
                        RegC('\t');
                        break;
                    default:
                        break;
                    }
                }
                else
                {
                    RegC(Ch);
                }
            }
            RegC('\0');
            if (*GRegParse != ']')
                return nullptr;
            ++GRegParse;
            *FlagP |= RegHasWidth | RegSimple;
            return Ret;
        }
        case '(':
        {
            int Flags = 0;
            Ret = Reg(1, &Flags);
            if (Ret == nullptr)
                return nullptr;
            *FlagP |= Flags & (RegHasWidth | RegSpStart);
            return Ret;
        }
        case '\0':
        case '\n':
        case '|':
        case ')':
        case '?':
        case '+':
        case '*':
            return nullptr;
        case '\\':
        {
            const char Escaped = *GRegParse++;
            if (Escaped == '\0')
                return nullptr;
            if (Escaped == '<')
                return RegNode(RegOpWordStart);
            if (Escaped == '>')
                return RegNode(RegOpWordEnd);
            bLiteral = true; // the escaped char starts a literal run (065b50c1 -> 50e5)
            break;
        }
        default:
            bLiteral = true;
            break;
        }

        if (!bLiteral)
            return nullptr;

        // Literal run (065b50e5): back up to the first char, then copy chars while the next
        // one is not special. Before a multiplier the last char is given back (unless it is
        // the only one) so the multiplier applies to it alone.
        --GRegParse;
        Ret = RegNode(RegOpExactly);
        const char* LastStart = nullptr;
        for (;;)
        {
            const char Ch = *GRegParse;
            ++GRegParse;
            const char Next = *GRegParse;
            if (Next == '*' || Next == '+' || Next == '?')
            {
                if (LastStart == nullptr)
                    RegC(Ch);
                else
                    GRegParse = LastStart;
                break;
            }
            if (Next == '\0' || Next == '\n' || Next == '$' || Next == '(' || Next == ')' || Next == '.' ||
                Next == '[' || Next == '^' || Next == '|')
            {
                RegC(Ch);
                break;
            }
            if (Next == '\\')
            {
                RegC(Ch);
                const char Escaped = GRegParse[1];
                if (Escaped == '\0' || Escaped == '<' || Escaped == '>')
                    break;
                LastStart = GRegParse;
                ++GRegParse;
                continue;
            }
            RegC(Ch);
            LastStart = GRegParse;
        }
        RegC('\0');
        *FlagP |= RegHasWidth;
        if (LastStart == nullptr)
            *FlagP |= RegSimple;
        return Ret;
    }

    // two passes (size, then emit). Unlike Spencer's original there is
    // no regerror; every failure just returns null (the second-pass failure leaks the block,
    // as baseline).
    FRegexpProgram* RegComp(const char* Expression)
    {
        if (Expression == nullptr)
            return nullptr;

        int Flags = 0;
        GRegParse = Expression;
        GRegNpar = 1;
        GRegSize = 0;
        GRegCode = &GRegDummy;
        RegC((char)RegMagic);
        if (Reg(0, &Flags) == nullptr)
            return nullptr;
        if (GRegSize >= 0x7fff)
            return nullptr;

        FRegexpProgram* Program = (FRegexpProgram*)(malloc(sizeof(FRegexpProgram) + GRegSize));
        if (Program == nullptr)
            return nullptr;

        GRegParse = Expression;
        GRegNpar = 1;
        GRegCode = Program->Program;
        RegC((char)RegMagic);
        if (Reg(0, &Flags) == nullptr)
            return nullptr;

        Program->RegStart = '\0';
        Program->RegAnch = 0;
        Program->RegMust = nullptr;
        Program->RegMLen = 0;
        char* Scan = Program->Program + 1;
        if (RegNext(Scan)[0] == RegOpEnd)
        {
            Scan = RegOperand(Scan);
            if (Scan[0] == RegOpExactly)
                Program->RegStart = *RegOperand(Scan);
            else if (Scan[0] == RegOpBol)
                ++Program->RegAnch;

            if ((Flags & RegSpStart) != 0)
            {
                char* Longest = nullptr;
                int Length = 0;
                for (; Scan != nullptr; Scan = RegNext(Scan))
                {
                    if (Scan[0] == RegOpExactly && (int)(std::strlen(RegOperand(Scan))) >= Length)
                    {
                        Longest = RegOperand(Scan);
                        Length = (int)(std::strlen(RegOperand(Scan)));
                    }
                }
                Program->RegMust = Longest;
                Program->RegMLen = Length;
            }
        }
        return Program;
    }

    int RegMatch(char* Program);

    // (switch on op - 3, table 065b59e4): how many times the simple node
    // at Node matches from GRegInput; GRegInput is left after the run.
    int RegRepeat(char* Node)
    {
        int Count = 0;
        const char* Scan = GRegInput;
        char* Operand = RegOperand(Node);
        switch (Node[0])
        {
        case RegOpAny:
            Count = (int)(std::strlen(Scan));
            Scan += Count;
            break;
        case RegOpExactly:
            while (*Operand == *Scan)
            {
                ++Count;
                ++Scan;
            }
            break;
        case RegOpAnyOf:
            while (*Scan != '\0' && std::strchr(Operand, *Scan) != nullptr)
            {
                ++Count;
                ++Scan;
            }
            break;
        case RegOpAnyBut:
            while (*Scan != '\0' && std::strchr(Operand, *Scan) == nullptr)
            {
                ++Count;
                ++Scan;
            }
            break;
        default:
            Count = 0;
            break;
        }
        GRegInput = Scan;
        return Count;
    }

    // baseline calls isalnum with the sign-extended char (MOVSX); the port passes the byte value
    // so the modern CRT's range assertion cannot fire. Same result for ASCII.
    bool RegIsWordChar(char Ch) { return isalnum((unsigned char)Ch) != 0 || Ch == '_'; }

    // (switch 065b5629, tables 058a8 / 058e4): the matcher proper.
    int RegMatch(char* Program)
    {
        char* Scan = Program;
        while (Scan != nullptr)
        {
            char* Next = RegNext(Scan);
            const int Op = (int)(Scan[0]);
            if (Op == RegOpBol)
            {
                if (GRegInput != GRegBol)
                    return 0;
            }
            else if (Op == RegOpEol)
            {
                if (*GRegInput != '\0')
                    return 0;
            }
            else if (Op == RegOpWordStart)
            {
                if (!RegIsWordChar(*GRegInput))
                    return 0;
                if (GRegInput > GRegBol && RegIsWordChar(GRegInput[-1]))
                    return 0;
            }
            else if (Op == RegOpWordEnd)
            {
                if (RegIsWordChar(*GRegInput))
                    return 0;
            }
            else if (Op == RegOpAny)
            {
                if (*GRegInput == '\0')
                    return 0;
                ++GRegInput;
            }
            else if (Op == RegOpExactly)
            {
                char* Operand = RegOperand(Scan);
                if (*Operand != *GRegInput)
                    return 0;
                const int Length = (int)(std::strlen(Operand));
                if (Length > 1 && std::strncmp(Operand, GRegInput, Length) != 0)
                    return 0;
                GRegInput += Length;
            }
            else if (Op == RegOpAnyOf)
            {
                if (*GRegInput == '\0' || std::strchr(RegOperand(Scan), *GRegInput) == nullptr)
                    return 0;
                ++GRegInput;
            }
            else if (Op == RegOpAnyBut)
            {
                if (*GRegInput == '\0' || std::strchr(RegOperand(Scan), *GRegInput) != nullptr)
                    return 0;
                ++GRegInput;
            }
            else if (Op == RegOpNothing || Op == RegOpBack)
            {
            }
            else if (Op > RegOpOpen && Op < RegOpOpen + RegSubExpressions)
            {
                const int Number = Op - RegOpOpen;
                const char* Save = GRegInput;
                if (RegMatch(Next) == 0)
                    return 0;
                if (GRegStartP[Number] == nullptr)
                    GRegStartP[Number] = Save;
                return 1;
            }
            else if (Op > RegOpClose && Op < RegOpClose + RegSubExpressions)
            {
                const int Number = Op - RegOpClose;
                const char* Save = GRegInput;
                if (RegMatch(Next) == 0)
                    return 0;
                if (GRegEndP[Number] == nullptr)
                    GRegEndP[Number] = Save;
                return 1;
            }
            else if (Op == RegOpBranch)
            {
                if (Next[0] != RegOpBranch)
                {
                    Next = RegOperand(Scan); // no choice, avoid recursion
                }
                else
                {
                    do
                    {
                        const char* Save = GRegInput;
                        if (RegMatch(RegOperand(Scan)) != 0)
                            return 1;
                        GRegInput = Save;
                        Scan = RegNext(Scan);
                    } while (Scan != nullptr && Scan[0] == RegOpBranch);
                    return 0;
                }
            }
            else if (Op == RegOpStar || Op == RegOpPlus)
            {
                char NextCh = '\0';
                if (Next[0] == RegOpExactly)
                    NextCh = *RegOperand(Next);
                const int Minimum = Op == RegOpStar ? 0 : 1;
                const char* Save = GRegInput;
                int Number = RegRepeat(RegOperand(Scan));
                while (Number >= Minimum)
                {
                    if (NextCh == '\0' || *GRegInput == NextCh)
                    {
                        if (RegMatch(Next) != 0)
                            return 1;
                    }
                    --Number;
                    GRegInput = Save + Number;
                }
                return 0;
            }
            else if (Op == RegOpEnd)
            {
                return 1;
            }
            else
            {
                return 0; // OPEN+0, CLOSE+0 and unknown opcodes (065b589f)
            }
            Scan = Next;
        }
        return 0;
    }

    // one attempt at Start.
    int RegTry(FRegexpProgram* Program, const char* Start)
    {
        GRegInput = Start;
        GRegStartP = Program->StartP;
        GRegEndP = Program->EndP;
        for (int Index = 0; Index < RegSubExpressions; ++Index)
        {
            Program->StartP[Index] = nullptr;
            Program->EndP[Index] = nullptr;
        }
        if (RegMatch(Program->Program + 1) != 0)
        {
            Program->StartP[0] = Start;
            Program->EndP[0] = GRegInput;
            return 1;
        }
        return 0;
    }

    // .
    int RegExec(FRegexpProgram* Program, const char* String)
    {
        if (Program == nullptr || String == nullptr)
            return 0;
        if ((unsigned char)(Program->Program[0]) != RegMagic)
            return 0;

        if (Program->RegMust != nullptr)
        {
            const char* Scan = std::strchr(String, Program->RegMust[0]);
            while (Scan != nullptr)
            {
                if (std::strncmp(Scan, Program->RegMust, Program->RegMLen) == 0)
                    break;
                ++Scan;
                Scan = std::strchr(Scan, Program->RegMust[0]);
            }
            if (Scan == nullptr)
                return 0;
        }

        GRegBol = String;
        if (Program->RegAnch != 0)
            return RegTry(Program, String);

        const char* Scan = String;
        if (Program->RegStart != '\0')
        {
            Scan = std::strchr(Scan, Program->RegStart);
            while (Scan != nullptr)
            {
                if (RegTry(Program, Scan) != 0)
                    return 1;
                ++Scan;
                Scan = std::strchr(Scan, Program->RegStart);
            }
            return 0;
        }
        for (;;)
        {
            if (RegTry(Program, Scan) != 0)
                return 1;
            if (*Scan++ == '\0')
                return 0;
        }
    }

    // baseline RegExp object (0x18): +0 vtable 065b71bc, +4 capture buffer (operator new),
    // +8 program (malloc), +0xC last match result, +0x10 subject, +0x14 reference count.
    // Construction the routine (+4, +8, +0x10 zeroed; +0xC is left as allocated, as baseline).
    class FRegExp final : public IRegExp
    {
    public:
        FRegExp() : CaptureBuffer(nullptr), Program(nullptr), Subject(nullptr), RefCount(1) {}

        // .
        ~FRegExp()
        {
            if (Program != nullptr)
                free(Program);
            FreeBuffer();
        }

        FRegExp(const FRegExp&) = delete;
        FRegExp& operator=(const FRegExp&) = delete;

        // QI table 065b71dc {"1.11_IRegExp", "1.11_INeuronComponent"}, both
        // at offset 0; *Out is written only on a miss.
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            if (std::strcmp(Iids::RegExp, Iid) == 0 || std::strcmp(Iids::NeuronComponent, Iid) == 0)
            {
                INeuronComponent* Face = (IRegExp*)this;
                Face->AddRef();
                *Out = Face;
                return GR_OK;
            }
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }

        // .
        unsigned long NEURONCOMPONENTS_API AddRef() override { return ++RefCount; }

        // .
        unsigned long NEURONCOMPONENTS_API Release() override
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
        // formerly Slot3
        bool NEURONCOMPONENTS_API CompilePattern(const char* Pattern) override
        {
            if (Program != nullptr)
            {
                free(Program);
                Program = nullptr;
            }
            Program = RegComp(Pattern);
            return Program != nullptr;
        }

        // .
        // formerly Slot4
        int NEURONCOMPONENTS_API Search(const char* SubjectIn) override
        {
            Subject = SubjectIn;
            MatchResult = 0;
            if (Program != nullptr)
                MatchResult = RegExec(Program, SubjectIn);
            return MatchResult;
        }

        // .
        // formerly Slot5
        char* NEURONCOMPONENTS_API CopyCapture(int Index, char* Buffer, int Size, int Skip, int bNullOnFail) override
        {
            if (Program != nullptr && MatchResult != 0 && Program->StartP[Index] != nullptr &&
                Program->EndP[Index] != nullptr)
            {
                int Length = (int)(Program->EndP[Index] - Program->StartP[Index]) - Skip;
                char* Destination = Buffer;
                if (Destination != nullptr)
                {
                    if (Length > Size)
                        Length = Size;
                }
                else
                {
                    AllocateBuffer((unsigned int)(Length + 1));
                    Destination = CaptureBuffer;
                }
                std::memcpy(Destination, Program->StartP[Index] + Skip, (size_t)Length);
                Destination[Length] = '\0';
                return Destination;
            }

            char* Destination = Buffer;
            if (Destination == nullptr)
            {
                AllocateBuffer(1);
                Destination = CaptureBuffer;
            }
            Destination[0] = '\0';
            return bNullOnFail != 0 ? nullptr : Destination;
        }

        // .
        // formerly Slot6
        int NEURONCOMPONENTS_API GetCaptureOffsetFromMatch(int Index) override
        {
            if (Program == nullptr || MatchResult == 0 || Program->StartP[Index] == nullptr)
                return -1;
            return (int)(Program->StartP[Index] - Program->StartP[0]);
        }

        // .
        // formerly Slot7
        int NEURONCOMPONENTS_API GetCaptureOffsetFromSubject(int Index) override
        {
            if (Program == nullptr || MatchResult == 0 || Program->StartP[Index] == nullptr)
                return -1;
            return (int)(Program->StartP[Index] - Subject);
        }

    private:
        // .
        void AllocateBuffer(unsigned int Bytes)
        {
            FreeBuffer();
            CaptureBuffer = new char[Bytes];
        }

        // .
        void FreeBuffer()
        {
            if (CaptureBuffer != nullptr)
            {
                delete[] CaptureBuffer;
                CaptureBuffer = nullptr;
            }
        }

        char* CaptureBuffer;     // +0x04
        FRegexpProgram* Program; // +0x08
        int MatchResult;         // +0x0C, not initialised by baseline
        const char* Subject;     // +0x10
        unsigned long RefCount;  // +0x14
    };

    // "RegExp" factory, vtable 065b71ac: 0xC bytes {vtable, RefCount, Name}; QI / AddRef /
    // Release are the code the parser factories use (065b4700 / 4740 / 5c90).
    class FRegExpFactory final : public IComponentFactory
    {
    public:
        explicit FRegExpFactory(const char* NameIn) : RefCount(1), Name(NameIn) {}

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            *Out = nullptr;
            if (std::strcmp(Iid, Iids::ComponentFactory) != 0)
                return GR_INTERFACE_UNSUPPORTED;
            *Out = (IComponentFactory*)this;
            AddRef();
            return GR_OK;
        }

        unsigned long NEURONCOMPONENTS_API AddRef() override { return ++RefCount; }

        unsigned long NEURONCOMPONENTS_API Release() override
        {
            if (RefCount > 0)
                --RefCount;
            if (RefCount != 0)
                return RefCount;
            RefCount = 1;
            delete this;
            return 0;
        }

        // plain 8-byte AGGDESC whose name matches (strcmp).
        GENRESULT NEURONCOMPONENTS_API CreateInstance(AGGDESC* Desc, void** Instance) override
        {
            if (Desc->size != 8 || std::strcmp(Desc->interface_name, Name) != 0)
            {
                *Instance = nullptr;
                return GR_INTERFACE_UNSUPPORTED;
            }
            FRegExp* Object = new (std::nothrow) FRegExp();
            if (Object == nullptr)
            {
                *Instance = nullptr;
                return GR_OUT_OF_MEMORY;
            }
            *Instance = (IRegExp*)Object;
            return GR_OK;
        }

    private:
        unsigned long RefCount;
        const char* Name;
    };

    // , called from DllMain.
    void RegisterRegExpComponent()
    {
        FRegExpFactory* Factory = new (std::nothrow) FRegExpFactory("RegExp");
        if (Factory == nullptr)
            return;
        NEURONCOMPONENTS_Acquire()->RegisterComponent(Factory, "RegExp",
                                           (void*)((uintptr_t)0x40000000u));
        Factory->Release();
    }

    // (the manager, constructed in place) and its once-bit the field.
    // The object is never destroyed; the routine runs at exit instead.
    alignas(FCOManager) unsigned char GManagerStorage[sizeof(FCOManager)];
    bool GbManagerConstructed = false;

    FCOManager* GetManager() { return (FCOManager*)GManagerStorage; }

    void __cdecl TearDownManager() { GetManager()->TearDown(); }
}

// .
BOOL APIENTRY DllMain(HMODULE Module, DWORD Reason, LPVOID /*Reserved*/)
{
    if (Reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(Module);
        FDUMP = DefaultDump;
        // . This constructs the COManager (NEURONCOMPONENTS_Acquire) while neuroncomponents.dll attaches.
        RegisterRegExpComponent();
    }
    return TRUE;
}

unsigned long __cdecl NEURONCOMPONENTS_CRC::GetCRC32(const char* Str)
{
    // (export thunk 065b4380).
    if (Str == nullptr)
        return 0;
    unsigned int Crc = 0xFFFFFFFFu;
    for (const char* Cursor = Str; *Cursor != '\0'; ++Cursor)
        Crc = StepCrc(Crc, *Cursor);
    return ~Crc;
}

unsigned long __cdecl NEURONCOMPONENTS_CRC::GetCRC32(const char* Begin, const char* End)
{
    // A nonempty range never terminates; volatile reads preserve the faulting access.
    if (Begin == End)
        return 0;
    volatile unsigned int Crc = 0xFFFFFFFFu;
    for (const volatile char* Cursor = Begin;; ++Cursor)
        Crc = StepCrc(Crc, *Cursor);
}

unsigned long __cdecl NEURONCOMPONENTS_CRC::GetContinuedCRC32(unsigned long InitialCrc, const char* Str)
{
    // (export thunk 065b43a0).
    if (Str == nullptr)
        return InitialCrc;
    unsigned int Crc = ~(unsigned int)InitialCrc;
    for (const char* Cursor = Str; *Cursor != '\0'; ++Cursor)
        Crc = StepCrc(Crc, *Cursor);
    return ~Crc;
}

unsigned long __cdecl NEURONCOMPONENTS_CRC::GetContinuedCRC32(unsigned long InitialCrc, char Ch)
{
    // (export thunk 065b43b0).
    return ~StepCrc(~(unsigned int)InitialCrc, Ch);
}

int __cdecl NEURONCOMPONENTS_CRC::CompareStringsI(const char* A, const char* B)
{
    // JMP _stricmp.
    return _stricmp(A, B);
}

// , exported as "stricmp": fold table the field (ASCII only), result -1/0/1.
extern "C" int __cdecl NeuronComponentsStricmp(const char* A, const char* B)
{
    const unsigned char* Left = (const unsigned char*)A;
    const unsigned char* Right = (const unsigned char*)B;
    for (;;)
    {
        const unsigned char LeftCh = FoldStricmpChar(*Left);
        const unsigned char RightCh = FoldStricmpChar(*Right);
        if (LeftCh != RightCh)
            return LeftCh < RightCh ? -1 : 1;
        if (LeftCh == 0)
            return 0;
        ++Left;
        ++Right;
    }
}

namespace LogStream
{
    // (XOR AL,AL) and 065b43e0 (RET): empty in the shipping build.
    bool __cdecl Startup(const char* /*Path*/)
    {
        return false;
    }

    void __cdecl FlushToDisk()
    {
    }

    void __cdecl LogEvent(const char* /*Name*/, float /*Value*/, unsigned long /*Flags*/)
    {
    }

    void __cdecl LogNamedEvent(const char* /*Name*/, const char* /*Value*/, unsigned long /*Flags*/)
    {
    }

    void __cdecl Update(float /*Dt*/)
    {
    }
}

// the once-bit is set before the constructor runs; the routine is
// registered with _onexit.
extern "C" ICOManager* NEURONCOMPONENTS_Acquire()
{
    if (!GbManagerConstructed)
    {
        GbManagerConstructed = true;
        new (GManagerStorage) FCOManager();
        atexit(TearDownManager);
    }
    return GetManager();
}

// . The outputs are zeroed first; the version reported is the PRODUCT
// version (VS_FIXEDFILEINFO +0x10/+0x14). A failed VerQueryValueA leaks the block, as baseline.
extern "C" GENRESULT __cdecl NEURONCOMPONENTS_GetDllVersion(const char* Module, unsigned int* Major,
                                                 unsigned int* Minor, unsigned int* Build)
{
    *Major = 0;
    *Minor = 0;
    *Build = 0;

    HMODULE ModuleHandle = GetModuleHandleA(Module);
    if (ModuleHandle == nullptr)
        return GR_GENERIC;

    char Path[0x104];
    if (GetModuleFileNameA(ModuleHandle, Path, 0x104) == 0)
        return GR_GENERIC;

    DWORD Handle = 0;
    const DWORD Size = GetFileVersionInfoSizeA(Path, &Handle);
    if (Size == 0)
        return GR_GENERIC;

    unsigned char* Block = new unsigned char[Size];
    if (!GetFileVersionInfoA(Path, 0, Size, Block))
    {
        delete[] Block;
        return GR_GENERIC;
    }

    VS_FIXEDFILEINFO* Info = nullptr;
    UINT InfoLength = 0;
    if (!VerQueryValueA(Block, "\\", (void**)(&Info), &InfoLength))
        return GR_GENERIC;

    const VS_FIXEDFILEINFO Fixed = *Info;
    delete[] Block;
    *Major = HIWORD(Fixed.dwProductVersionMS);
    *Minor = LOWORD(Fixed.dwProductVersionMS);
    *Build = LOWORD(Fixed.dwProductVersionLS);
    return GR_OK;
}

// .
extern "C" GENRESULT __cdecl NEURONCOMPONENTS_GetVersion(unsigned int* Major, unsigned int* Minor,
                                              unsigned int* Build)
{
    char Message[0x2000];
    const GENRESULT Result = NEURONCOMPONENTS_GetDllVersion("NEURONCOMPONENTS.DLL", Major, Minor, Build);
    if (Result >= 0)
    {
        FDUMP(DumpNotice, GNoticeFormat, GNeuronComponentsSourceFile, 0x457,
              FormatDumpText(Message, "Current NEURONCOMPONENTS.DLL version: %d.%d.%d\n", *Major, *Minor, *Build));
        FDUMP(DumpNotice, GNoticeFormat, GNeuronComponentsSourceFile, 0x458,
              FormatDumpText(Message, " Compiled against version: %d.%d.%d\n", 1, 0xb, 0xad));
    }
    return Result;
}
