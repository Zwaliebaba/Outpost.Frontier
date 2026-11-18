// Oracle.exe render-replay (decision record 0004, point 15; work package P4.3).
//
// Plays version-1 renderer traces (CppSource/RenderTrace/README.md) into the real Direct3D 8
// renderer, DX8RENDERER.dll, and writes the image of each selected frame. The images are the
// reference the Rust image replay compares with: for the synthetic traces in
// crates/neuron-client/tests/vectors/render/<name>/ they are committed as expected.bmp.
//
// Usage: Oracle.exe render-replay <vectors directory> <output directory>
//
// Needs, beside Oracle.exe: DX8RENDERER.dll (built with the Oracle project, which references
// it) and x86math.dll (DX8RENDERER's 3DMathEngine); from the system, d3d8.dll and ddraw.dll.
//
// Every folder of the vectors directory that holds a trace.jsonl is one trace; its images go
// to <output directory>\<name>\expected.bmp (the first selected frame) and frame-<N>.bmp (any
// later one). For each trace the player
//   - creates DX8RENDERER's "DirectX8" component through neuroncomponents, not aggregated,
//     and a window whose client area is the back-buffer size of the trace's first
//     CreateBuffers;
//   - plays every line in file order: the calls through the seven renderer interfaces (and
//     IAggregateComponent::Initialize), snapshot lines like calls, LightData lines into the
//     player's D3DLIGHT8 of that light (the renderer reads it at its next light flush, as it
//     read the client's); the defaults lines are compared with what the renderer's getters
//     return and are not applied (the renderer reads its own INI);
//   - maps trace ids to the real handles, keeps one D3DLIGHT8 per light id (a light handle
//     is the address of the caller's struct), and writes unlock data from the trace (inline
//     or blobs\<hash>.bin) into the memory the real lock returned, at the recorded offsets;
//   - answers the methods of record 0004 point 13 that are not replayed (mode enumeration,
//     device information, render targets, BlitTexture, PrintScreen, ...) from the trace.
//
// The image. RenderTrace's frame images are PrintScreen copies of the front buffer, which in
// a window is the screen: on a CI runner the window may be covered or off screen. The player
// reads the back buffer instead, after the selected frame's last call before SwapBuffers
// (its EndScene) and before the SwapBuffers is forwarded, through the renderer's own
// IRenderPipeline::LockBuffer: a CopyRects of the back buffer into a system-memory surface,
// read whatever the window shows. LockBuffer needs LOCKABLE_BACKBUFFER in the renderer's
// INI section, which the shipped INI does not set; the player loads an engine INI of its own
// into neuroncomponents' profile parser before any trace,
//     [RenderPipeline]
//     LOCKABLE_BACKBUFFER = true
// so DX8RENDERER's Startup reads it from the section it uses by default. The key only adds
// D3DPRESENTFLAG_LOCKABLE_BACKBUFFER to the present parameters; it sets no render state, so
// the defaults are the built-in tables, which is what the synthetic traces' defaults lines
// hold. A back buffer is what the swap would present, with no gamma ramp (a ramp acts on the
// output, and DX8RENDERER only sets one in full screen) and with Direct3D 8's own pixel
// centres. It is written as a 24-bit bottom-up BMP.
//
// Every call whose result is negative is printed with its name and result, beside the
// renderer's own FDUMP lines. A call that failed where the trace recorded success fails the
// trace, except for calls that cannot change the image (gamma, getters, VerifyState), which
// are reported as warnings; a CreateBuffers that fails ends the trace with a note that no
// Direct3D 8 HAL device could be created. The exit code is 0 when every trace played and
// wrote its images, 1 otherwise, 2 for a usage or set-up error.
//
// Not replayed yet (the synthetic traces do not use them; the player stops with a message
// naming the method): CopyVertices and CopyVertexData (they need the source descriptor
// rebuilt), LoadTexture and LoadCubemap (they read the data tree), a snapshot selection of a
// pooled buffer never handed out ({"pool_fvf":F}).

// std::min below, not the <windows.h> macro.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "oracle_support.h"

#include "neuroncomponents.h"
#include "luaprofile.h"
#include "render/IRenderPipeline.h"

using namespace oracle;

namespace
{
    const char kWindowClass[] = "OutpostOracleRenderReplay";
    const char kDefaultSectionIni[] = "[RenderPipeline]\r\nLOCKABLE_BACKBUFFER = true\r\n";

    // ---------------------------------------------------------------------------------------
    // A failure that ends the trace being played.

    struct FReplayFailure
    {
        std::string Message;
    };

    // ---------------------------------------------------------------------------------------
    // JSON: what the trace writes (objects, arrays, strings, integers, true, false, null).

    struct FJson
    {
        enum EKind
        {
            kNull,
            kBool,
            kNumber,
            kString,
            kArray,
            kObject,
        };

        FJson() : Kind(kNull), Bool(false), Number(0) {}

        const FJson* Get(const char* Key) const
        {
            if (Kind != kObject)
                return nullptr;
            for (size_t Index = 0; Index < Keys.size(); ++Index)
            {
                if (Keys[Index] == Key)
                    return &Values[Index];
            }
            return nullptr;
        }

        EKind Kind;
        bool Bool;
        long long Number;
        std::string Text;
        std::vector<FJson> Values; // array items, or object values
        std::vector<std::string> Keys;
    };

    class FJsonReader
    {
    public:
        explicit FJsonReader(const std::string& Text) : Cursor(Text.c_str()), End(Text.c_str() + Text.size()) {}

        bool Read(FJson* Out, std::string* Error)
        {
            if (!Value(Out, 0))
            {
                *Error = Problem.empty() ? "malformed JSON" : Problem;
                return false;
            }
            Skip();
            if (Cursor != End)
            {
                *Error = "text after the JSON value";
                return false;
            }
            return true;
        }

    private:
        void Skip()
        {
            while (Cursor < End && (*Cursor == ' ' || *Cursor == '\t' || *Cursor == '\r' || *Cursor == '\n'))
                ++Cursor;
        }

        bool Literal(const char* Word)
        {
            const size_t Length = strlen(Word);
            if ((size_t)(End - Cursor) < Length || strncmp(Cursor, Word, Length) != 0)
                return false;
            Cursor += Length;
            return true;
        }

        static int HexDigit(char Ch)
        {
            if (Ch >= '0' && Ch <= '9')
                return Ch - '0';
            if (Ch >= 'a' && Ch <= 'f')
                return Ch - 'a' + 10;
            if (Ch >= 'A' && Ch <= 'F')
                return Ch - 'A' + 10;
            return -1;
        }

        bool String(std::string* Out)
        {
            ++Cursor; // the opening quote
            while (Cursor < End && *Cursor != '"')
            {
                char Ch = *Cursor++;
                if (Ch != '\\')
                {
                    *Out += Ch;
                    continue;
                }
                if (Cursor >= End)
                    return false;
                Ch = *Cursor++;
                switch (Ch)
                {
                case '"':
                case '\\':
                case '/':
                    *Out += Ch;
                    break;
                case 'b':
                    *Out += '\b';
                    break;
                case 'f':
                    *Out += '\f';
                    break;
                case 'n':
                    *Out += '\n';
                    break;
                case 'r':
                    *Out += '\r';
                    break;
                case 't':
                    *Out += '\t';
                    break;
                case 'u':
                {
                    if (End - Cursor < 4)
                        return false;
                    int Code = 0;
                    for (int Index = 0; Index < 4; ++Index)
                    {
                        const int Digit = HexDigit(Cursor[Index]);
                        if (Digit < 0)
                            return false;
                        Code = Code * 16 + Digit;
                    }
                    Cursor += 4;
                    // The proxy writes \u00XX for one byte.
                    *Out += Code < 0x100 ? (char)Code : '?';
                    break;
                }
                default:
                    return false;
                }
            }
            if (Cursor >= End)
                return false;
            ++Cursor; // the closing quote
            return true;
        }

        bool NumberValue(FJson* Out)
        {
            bool bNegative = false;
            if (*Cursor == '-')
            {
                bNegative = true;
                ++Cursor;
            }
            if (Cursor >= End || *Cursor < '0' || *Cursor > '9')
                return false;
            unsigned long long Value = 0;
            while (Cursor < End && *Cursor >= '0' && *Cursor <= '9')
            {
                const unsigned long long Next = Value * 10 + (unsigned long long)(*Cursor - '0');
                if (Next / 10 != Value)
                {
                    Problem = "integer out of range";
                    return false;
                }
                Value = Next;
                ++Cursor;
            }
            if (Cursor < End && (*Cursor == '.' || *Cursor == 'e' || *Cursor == 'E'))
            {
                Problem = "a trace holds integers only";
                return false;
            }
            Out->Kind = FJson::kNumber;
            Out->Number = bNegative ? -(long long)(Value) : (long long)(Value);
            return true;
        }

        bool Value(FJson* Out, int Depth)
        {
            Skip();
            if (Cursor >= End || Depth > 32)
                return false;
            const char Ch = *Cursor;
            if (Ch == '{')
            {
                Out->Kind = FJson::kObject;
                ++Cursor;
                Skip();
                if (Cursor < End && *Cursor == '}')
                {
                    ++Cursor;
                    return true;
                }
                for (;;)
                {
                    Skip();
                    if (Cursor >= End || *Cursor != '"')
                        return false;
                    std::string Key;
                    if (!String(&Key))
                        return false;
                    Skip();
                    if (Cursor >= End || *Cursor != ':')
                        return false;
                    ++Cursor;
                    Out->Keys.push_back(Key);
                    Out->Values.push_back(FJson());
                    if (!Value(&Out->Values.back(), Depth + 1))
                        return false;
                    Skip();
                    if (Cursor < End && *Cursor == ',')
                    {
                        ++Cursor;
                        continue;
                    }
                    if (Cursor < End && *Cursor == '}')
                    {
                        ++Cursor;
                        return true;
                    }
                    return false;
                }
            }
            if (Ch == '[')
            {
                Out->Kind = FJson::kArray;
                ++Cursor;
                Skip();
                if (Cursor < End && *Cursor == ']')
                {
                    ++Cursor;
                    return true;
                }
                for (;;)
                {
                    Out->Values.push_back(FJson());
                    if (!Value(&Out->Values.back(), Depth + 1))
                        return false;
                    Skip();
                    if (Cursor < End && *Cursor == ',')
                    {
                        ++Cursor;
                        continue;
                    }
                    if (Cursor < End && *Cursor == ']')
                    {
                        ++Cursor;
                        return true;
                    }
                    return false;
                }
            }
            if (Ch == '"')
            {
                Out->Kind = FJson::kString;
                return String(&Out->Text);
            }
            if (Literal("true"))
            {
                Out->Kind = FJson::kBool;
                Out->Bool = true;
                return true;
            }
            if (Literal("false"))
            {
                Out->Kind = FJson::kBool;
                return true;
            }
            if (Literal("null"))
            {
                Out->Kind = FJson::kNull;
                return true;
            }
            return NumberValue(Out);
        }

        const char* Cursor;
        const char* End;
        std::string Problem;
    };

    // ---------------------------------------------------------------------------------------
    // Helpers.

    unsigned int* WordPtr(std::vector<unsigned int>& Words)
    {
        return Words.empty() ? nullptr : &Words[0];
    }

    unsigned char* BytePtr(std::vector<unsigned char>& Bytes)
    {
        return Bytes.empty() ? nullptr : &Bytes[0];
    }

    bool ParseHexWord(const std::string& Text, unsigned int* Out)
    {
        if (Text.size() != 8)
            return false;
        unsigned int Value = 0;
        for (size_t Index = 0; Index < Text.size(); ++Index)
        {
            const char Ch = Text[Index];
            int Digit = -1;
            if (Ch >= '0' && Ch <= '9')
                Digit = Ch - '0';
            else if (Ch >= 'a' && Ch <= 'f')
                Digit = Ch - 'a' + 10;
            else if (Ch >= 'A' && Ch <= 'F')
                Digit = Ch - 'A' + 10;
            if (Digit < 0)
                return false;
            Value = Value * 16 + (unsigned int)(Digit);
        }
        *Out = Value;
        return true;
    }

    void __cdecl ReplayDump(int /*Flags*/, const char* Format, ...)
    {
        char Text[0x2000];
        va_list Args;
        va_start(Args, Format);
        _vsnprintf(Text, sizeof(Text), Format, Args);
        va_end(Args);
        Text[sizeof(Text) - 1] = '\0';
        size_t Length = strlen(Text);
        while (Length > 0 && (Text[Length - 1] == '\n' || Text[Length - 1] == '\r'))
            Text[--Length] = '\0';
        fprintf(stderr, "  FDUMP: %s\n", Text);
        fflush(stderr);
    }

    LRESULT CALLBACK ReplayWindowProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
    {
        return DefWindowProcA(Window, Message, WParam, LParam);
    }

    void PumpMessages()
    {
        MSG Message;
        while (PeekMessageA(&Message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&Message);
            DispatchMessageA(&Message);
        }
    }

    void PutU16(std::vector<unsigned char>& Out, unsigned int Value)
    {
        Out.push_back((unsigned char)(Value & 0xFF));
        Out.push_back((unsigned char)((Value >> 8) & 0xFF));
    }

    void PutU32(std::vector<unsigned char>& Out, unsigned long Value)
    {
        PutU16(Out, (unsigned int)(Value & 0xFFFF));
        PutU16(Out, (unsigned int)((Value >> 16) & 0xFFFF));
    }

    // Bytes per pixel of a back-buffer pfenum the conversion knows (DX8RENDERER kPfToD3d):
    // 2 R8G8B8, 3 R5G6B5, 4 X1R5G5B5, 6 A1R5G5B5, 7 A8R8G8B8, 8 X8R8G8B8. 0 for any other.
    unsigned int PixelBytes(unsigned int Format)
    {
        switch (Format)
        {
        case 2:
            return 3;
        case 3:
        case 4:
        case 6:
            return 2;
        case 7:
        case 8:
            return 4;
        default:
            return 0;
        }
    }

    // One pixel as B, G, R (alpha dropped; 5- and 6-bit channels widened by replicating their
    // top bits).
    void PixelToBgr(unsigned int Format, const unsigned char* Pixel, unsigned char* Bgr)
    {
        if (Format == 2 || Format == 7 || Format == 8)
        {
            Bgr[0] = Pixel[0];
            Bgr[1] = Pixel[1];
            Bgr[2] = Pixel[2];
            return;
        }
        const unsigned int Value = (unsigned int)(Pixel[0]) | ((unsigned int)(Pixel[1]) << 8);
        unsigned int R;
        unsigned int G;
        unsigned int B;
        if (Format == 3)
        {
            R = (Value >> 11) & 0x1F;
            G = (Value >> 5) & 0x3F;
            B = Value & 0x1F;
            Bgr[1] = (unsigned char)((G << 2) | (G >> 4));
        }
        else
        {
            R = (Value >> 10) & 0x1F;
            G = (Value >> 5) & 0x1F;
            B = Value & 0x1F;
            Bgr[1] = (unsigned char)((G << 3) | (G >> 2));
        }
        Bgr[0] = (unsigned char)((B << 3) | (B >> 2));
        Bgr[2] = (unsigned char)((R << 3) | (R >> 2));
    }

    bool WriteBytes(const std::string& Path, const std::vector<unsigned char>& Bytes)
    {
        MakeParentDirectories(Path);
        FILE* File = fopen(Path.c_str(), "wb");
        if (File == nullptr)
            return false;
        const size_t Written = Bytes.empty() ? 0 : fwrite(&Bytes[0], 1, Bytes.size(), File);
        const bool bClosed = fclose(File) == 0;
        return Written == Bytes.size() && bClosed;
    }

    // ---------------------------------------------------------------------------------------
    // The player.

    struct FOutcome
    {
        GENRESULT Result;
        bool bCalled;
        bool bWarnOnly;
    };

    struct FBufferLock
    {
        unsigned char* Bits;
        unsigned int First;
        bool bFirstKnown;
    };

    struct FTextureLock
    {
        unsigned char* Bits;
        unsigned int Pitch;
    };

    class FPlayer
    {
    public:
        FPlayer(const std::string& Folder, const std::string& Name, const std::string& OutDir)
            : Folder_(Folder), Name_(Name), OutDir_(OutDir), Window_(nullptr), Object_(nullptr), Pipeline_(nullptr),
              VbMgr_(nullptr), Draw_(nullptr), Ib_(nullptr), Vb_(nullptr), Gamma_(nullptr), Texture_(nullptr),
              Aggregate_(nullptr), Seq_(0), bSelected_(false), Frame_(0), Images_(0), Calls_(0), Errors_(0),
              Warnings_(0), bDeviceFailed_(false), bCheckDefaults_(false)
        {
        }

        ~FPlayer() { Teardown(); }

        // Plays the trace; true when it played to its end without an error and wrote the image of
        // every selected frame.
        bool Run();
        bool DeviceFailed() const { return bDeviceFailed_; }

    private:
        FPlayer(const FPlayer&);
        FPlayer& operator=(const FPlayer&);

        void Fail(const char* Format, ...);
        void Error(const char* Format, ...);
        void Warn(const char* Format, ...);
        std::string Where() const;

        void CreateWindowFor(unsigned int Width, unsigned int Height);
        void CreateRenderer();
        void Teardown();
        void PlayLine(const std::string& Text);
        void PlayCall(const FJson& Line);
        void CheckDefaults(const FJson& Defaults);
        void CaptureFrame();

        bool DispatchManual(const char* Interface, const char* Method, const FJson& Args, FOutcome* Out);
        bool DispatchGenerated(const char* Interface, const char* Method, const FJson& Args, FOutcome* Out);

        // Arguments.
        const FJson& Arg(const FJson& Args, const char* Name);
        bool IsNull(const FJson& Args, const char* Name);
        long long Int(const FJson& Args, const char* Name);
        unsigned long long UInt(const FJson& Args, const char* Name);
        float Float(const FJson& Args, const char* Name);
        std::string Str(const FJson& Args, const char* Name, bool* bNull);
        std::vector<unsigned int> WordsArg(const FJson& Args, const char* Name, bool* bNull);
        std::vector<unsigned char> BytesArg(const FJson& Args, const char* Name, bool* bNull);
        std::vector<unsigned char> BlobBytes(const std::string& Hash);
        void* HandleOf(long long Id, const char* Name);
        void* TextureArg(const FJson& Args, const char* Name);
        void* VbArg(const FJson& Args, const char* Name);
        void* IbArg(const FJson& Args, const char* Name);
        void* LightArg(const FJson& Args, const char* Name);
        D3DLIGHT8* Light(long long Id);
        void WriteLight(D3DLIGHT8* Target, const FJson& Args, const char* Name);
        void CompareOutBits(const FJson& Args, const char* Name, unsigned int Got);
        void CompareOutTexture(const FJson& Args, const char* Name, void* Got);
        void WriteLocked(const FBufferLock& Lock, const FJson& Args, const char* FirstName, unsigned int Stride);

        std::string Folder_;
        std::string Name_;
        std::string OutDir_;
        HWND Window_;
        IRenderPipeline* Object_;
        IRenderPipeline* Pipeline_;
        IVertexBufferManager* VbMgr_;
        IRPDraw* Draw_;
        IRPIndexBuffer* Ib_;
        IRPVertexBuffer* Vb_;
        IGammaControl* Gamma_;
        IRPTexture* Texture_;
        IAggregateComponent* Aggregate_;

        std::map<long long, void*> Handles_;
        std::map<long long, D3DLIGHT8*> Lights_;
        std::map<long long, std::vector<FBufferLock> > BufferLocks_;
        std::map<std::pair<long long, int>, FTextureLock> TextureLocks_;
        std::vector<std::pair<long long, FVertexMemoryLock> > VertexMemoryLocks_;

        long long Seq_;
        std::string Method_;
        bool bSelected_;
        long long Frame_;
        int Images_;
        unsigned long Calls_;
        unsigned long Errors_;
        unsigned long Warnings_;
        bool bDeviceFailed_;
        bool bCheckDefaults_;
    };

    std::string FPlayer::Where() const
    {
        return Format("%s: seq %lld %s", Name_.c_str(), Seq_, Method_.c_str());
    }

    void FPlayer::Fail(const char* Pattern, ...)
    {
        char Text[1024];
        va_list Args;
        va_start(Args, Pattern);
        _vsnprintf(Text, sizeof(Text), Pattern, Args);
        va_end(Args);
        Text[sizeof(Text) - 1] = '\0';
        FReplayFailure Failure;
        Failure.Message = Where() + ": " + Text;
        throw Failure;
    }

    void FPlayer::Error(const char* Pattern, ...)
    {
        char Text[1024];
        va_list Args;
        va_start(Args, Pattern);
        _vsnprintf(Text, sizeof(Text), Pattern, Args);
        va_end(Args);
        Text[sizeof(Text) - 1] = '\0';
        fprintf(stderr, "render-replay: error: %s: %s\n", Where().c_str(), Text);
        fflush(stderr);
        ++Errors_;
    }

    void FPlayer::Warn(const char* Pattern, ...)
    {
        char Text[1024];
        va_list Args;
        va_start(Args, Pattern);
        _vsnprintf(Text, sizeof(Text), Pattern, Args);
        va_end(Args);
        Text[sizeof(Text) - 1] = '\0';
        fprintf(stderr, "render-replay: warning: %s: %s\n", Where().c_str(), Text);
        fflush(stderr);
        ++Warnings_;
    }

    // --- Arguments ---------------------------------------------------------------------------

    const FJson& FPlayer::Arg(const FJson& Args, const char* Name)
    {
        const FJson* Value = Args.Get(Name);
        if (Value == nullptr)
            Fail("no argument %s", Name);
        return *Value;
    }

    bool FPlayer::IsNull(const FJson& Args, const char* Name)
    {
        return Arg(Args, Name).Kind == FJson::kNull;
    }

    long long FPlayer::Int(const FJson& Args, const char* Name)
    {
        const FJson& Value = Arg(Args, Name);
        if (Value.Kind != FJson::kNumber)
            Fail("argument %s is not a number", Name);
        return Value.Number;
    }

    unsigned long long FPlayer::UInt(const FJson& Args, const char* Name)
    {
        return (unsigned long long)(Int(Args, Name));
    }

    float FPlayer::Float(const FJson& Args, const char* Name)
    {
        const FJson& Value = Arg(Args, Name);
        unsigned int Bits = 0;
        if (Value.Kind != FJson::kString || !ParseHexWord(Value.Text, &Bits))
            Fail("argument %s is not a float's 8 hex digits", Name);
        return FloatFromBits(Bits);
    }

    std::string FPlayer::Str(const FJson& Args, const char* Name, bool* bNull)
    {
        const FJson& Value = Arg(Args, Name);
        *bNull = Value.Kind == FJson::kNull;
        if (*bNull)
            return std::string();
        if (Value.Kind != FJson::kString)
            Fail("argument %s is not a string", Name);
        return Value.Text;
    }

    // Words of an array argument; at least 32 when it is not null, so that a getter's output
    // buffer (at most 26 words) is never short.
    std::vector<unsigned int> FPlayer::WordsArg(const FJson& Args, const char* Name, bool* bNull)
    {
        const FJson& Value = Arg(Args, Name);
        std::vector<unsigned int> Words;
        *bNull = Value.Kind == FJson::kNull;
        if (*bNull)
            return Words;
        if (Value.Kind != FJson::kArray)
            Fail("argument %s is not an array of words", Name);
        for (size_t Index = 0; Index < Value.Values.size(); ++Index)
        {
            unsigned int Word = 0;
            if (Value.Values[Index].Kind != FJson::kString || !ParseHexWord(Value.Values[Index].Text, &Word))
                Fail("argument %s: item %u is not 8 hex digits", Name, (unsigned int)(Index));
            Words.push_back(Word);
        }
        if (Words.size() < 32)
            Words.resize(32, 0);
        return Words;
    }

    std::vector<unsigned char> FPlayer::BlobBytes(const std::string& Hash)
    {
        Bytes Data;
        const std::string Path = Folder_ + "\\blobs\\" + Hash + ".bin";
        if (!ReadWholeFile(Path, Data))
            Fail("cannot read blob %s", Path.c_str());
        if (Fnv64(Data.empty() ? nullptr : &Data[0], Data.size()) != Hash)
            Fail("blob %s does not hash to its name", Hash.c_str());
        return Data;
    }

    // {"size":N,"hex":"..."} or {"size":N,"blob":"<hash>"}, or null.
    std::vector<unsigned char> FPlayer::BytesArg(const FJson& Args, const char* Name, bool* bNull)
    {
        const FJson& Value = Arg(Args, Name);
        std::vector<unsigned char> Data;
        *bNull = Value.Kind == FJson::kNull;
        if (*bNull)
            return Data;
        const FJson* Size = Value.Get("size");
        const FJson* Hex = Value.Get("hex");
        const FJson* Blob = Value.Get("blob");
        if (Size == nullptr || Size->Kind != FJson::kNumber)
            Fail("argument %s is not byte data", Name);
        if (Hex != nullptr && Hex->Kind == FJson::kString)
        {
            if (Hex->Text.size() % 2 != 0)
                Fail("argument %s: odd hex length", Name);
            // Not oracle::Unhex, which appends a NUL for its C-string callers.
            for (size_t Index = 0; Index < Hex->Text.size(); Index += 2)
            {
                unsigned int Byte = 0;
                if (!ParseHexWord("000000" + Hex->Text.substr(Index, 2), &Byte))
                    Fail("argument %s: bad hex digit", Name);
                Data.push_back((unsigned char)(Byte));
            }
        }
        else if (Blob != nullptr && Blob->Kind == FJson::kString)
        {
            Data = BlobBytes(Blob->Text);
        }
        else
        {
            Fail("argument %s has neither hex nor blob", Name);
        }
        if ((long long)(Data.size()) != Size->Number)
            Fail("argument %s: %u bytes, the line says %lld", Name, (unsigned int)(Data.size()), Size->Number);
        return Data;
    }

    void* FPlayer::HandleOf(long long Id, const char* Name)
    {
        if (Id == 0)
            return nullptr;
        if (Id == -1)
            return (void*)((intptr_t)-1);
        std::map<long long, void*>::const_iterator It = Handles_.find(Id);
        if (It == Handles_.end())
            Fail("argument %s: id %lld names no object the player created", Name, Id);
        return It->second;
    }

    void* FPlayer::TextureArg(const FJson& Args, const char* Name)
    {
        return HandleOf(Int(Args, Name), Name);
    }

    void* FPlayer::VbArg(const FJson& Args, const char* Name)
    {
        const FJson& Value = Arg(Args, Name);
        if (Value.Kind == FJson::kObject && Value.Get("pool_fvf") != nullptr)
            Fail("argument %s: a pooled buffer never handed out ({\"pool_fvf\":..}) is not replayed yet", Name);
        return HandleOf(Int(Args, Name), Name);
    }

    void* FPlayer::IbArg(const FJson& Args, const char* Name)
    {
        return HandleOf(Int(Args, Name), Name);
    }

    // A light handle is the address of the caller's D3DLIGHT8 (README.md, LightData): the player
    // keeps one per id, zeroed when first seen, alive until the trace ends.
    D3DLIGHT8* FPlayer::Light(long long Id)
    {
        std::map<long long, D3DLIGHT8*>::iterator It = Lights_.find(Id);
        if (It != Lights_.end())
            return It->second;
        D3DLIGHT8* Created = new D3DLIGHT8;
        memset(Created, 0, sizeof(*Created));
        Lights_[Id] = Created;
        return Created;
    }

    void* FPlayer::LightArg(const FJson& Args, const char* Name)
    {
        const long long Id = Int(Args, Name);
        if (Id == 0)
            return nullptr;
        return Light(Id);
    }

    void FPlayer::WriteLight(D3DLIGHT8* Target, const FJson& Args, const char* Name)
    {
        bool bNull = false;
        std::vector<unsigned int> Words = WordsArg(Args, Name, &bNull);
        if (bNull)
            return;
        if (Arg(Args, Name).Values.size() != sizeof(D3DLIGHT8) / 4)
            Fail("argument %s is not the 26 words of a D3DLIGHT8", Name);
        memcpy(Target, WordPtr(Words), sizeof(D3DLIGHT8));
    }

    void FPlayer::CompareOutBits(const FJson& Args, const char* Name, unsigned int Got)
    {
        const FJson& Value = Arg(Args, Name);
        unsigned int Want = 0;
        if (Value.Kind == FJson::kNumber)
            Want = (unsigned int)(Value.Number);
        else if (Value.Kind != FJson::kString || !ParseHexWord(Value.Text, &Want))
            return;
        if (Want != Got)
            Warn("%s is 0x%08x, the trace recorded 0x%08x", Name, Got, Want);
    }

    void FPlayer::CompareOutTexture(const FJson& Args, const char* Name, void* Got)
    {
        void* Want = TextureArg(Args, Name);
        if (Want != Got)
            Warn("%s is not the texture the trace recorded", Name);
    }

    // Unlock data goes where the trace says the C++ renderer locked (README.md, replay
    // contract): the player's lock must have returned the same first vertex or index.
    void FPlayer::WriteLocked(const FBufferLock& Lock, const FJson& Args, const char* FirstName, unsigned int Stride)
    {
        bool bNull = false;
        std::vector<unsigned char> Data = BytesArg(Args, "data", &bNull);
        if (bNull)
            return;
        const FJson& First = Arg(Args, FirstName);
        if (First.Kind == FJson::kNumber && Lock.bFirstKnown && (unsigned int)(First.Number) != Lock.First)
            Fail("the trace's lock starts at %u, the renderer's at %u", (unsigned int)(First.Number), Lock.First);
        const size_t Limit = (size_t)(UInt(Args, "Count")) * Stride;
        const size_t Size = std::min(Limit, Data.size());
        if (Lock.Bits == nullptr)
            Fail("no locked memory to write to");
        if (Size > 0)
            memcpy(Lock.Bits, &Data[0], Size);
    }

    // --- Set-up ------------------------------------------------------------------------------

    void FPlayer::CreateWindowFor(unsigned int Width, unsigned int Height)
    {
        static bool bRegistered = false;
        HINSTANCE Instance = GetModuleHandleA(nullptr);
        if (!bRegistered)
        {
            WNDCLASSA Class;
            memset(&Class, 0, sizeof(Class));
            Class.lpfnWndProc = ReplayWindowProc;
            Class.hInstance = Instance;
            Class.hCursor = LoadCursor(nullptr, IDC_ARROW);
            Class.lpszClassName = kWindowClass;
            if (RegisterClassA(&Class) == 0)
                Fail("RegisterClassA failed (error %lu)", GetLastError());
            bRegistered = true;
        }
        const DWORD Style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        RECT Area = {0, 0, (LONG)(Width), (LONG)(Height)};
        AdjustWindowRect(&Area, Style, FALSE);
        const std::string Title = "Oracle render-replay: " + Name_;
        Window_ = CreateWindowExA(0, kWindowClass, Title.c_str(), Style, 0, 0, Area.right - Area.left,
                                  Area.bottom - Area.top, nullptr, nullptr, Instance, nullptr);
        if (Window_ == nullptr)
            Fail("CreateWindowExA failed (error %lu)", GetLastError());
        ShowWindow(Window_, SW_SHOWNOACTIVATE);
        UpdateWindow(Window_);
        PumpMessages();
    }

    void FPlayer::CreateRenderer()
    {
        ICOManager* Manager = NEURONCOMPONENTS_Acquire();
        if (Manager == nullptr)
            Fail("no neuroncomponents manager");
        // Not aggregated (RenderTrace's CreateReal aggregates; nothing here needs it): no outer
        // unknown, no section name, so the renderer uses [RenderPipeline].
        FAGGDESC14 Desc;
        Desc.Size = sizeof(FAGGDESC14);
        Desc.NameAt4 = kDx8RendererComponentName;
        Desc.OuterUnknown = nullptr;
        Desc.InnerUnknown = nullptr;
        Desc.Reserved10 = 0;
        void* Instance = nullptr;
        const GENRESULT Created = Manager->CreateInstance((AGGDESC*)(&Desc), &Instance);
        if (Created != GR_OK || Instance == nullptr)
            Fail("CreateInstance(\"%s\") returned 0x%08lx", kDx8RendererComponentName, (unsigned long)(Created));
        Object_ = (IRenderPipeline*)(Instance);
        struct FFace
        {
            const char* Iid;
            void** Slot;
        };
        const FFace Faces[] = {
            {Iids::RenderPipeline8B, (void**)(&Pipeline_)},
            {Iids::VertexBufferManager, (void**)(&VbMgr_)},
            {Iids::RPDraw, (void**)(&Draw_)},
            {Iids::RPIndexBuffer, (void**)(&Ib_)},
            {Iids::RPVertexBuffer, (void**)(&Vb_)},
            {Iids::GammaControl, (void**)(&Gamma_)},
            {Iids::RPTexture, (void**)(&Texture_)},
            {Iids::AggregateComponent, (void**)(&Aggregate_)},
        };
        for (size_t Index = 0; Index < sizeof(Faces) / sizeof(Faces[0]); ++Index)
        {
            const GENRESULT Result = Object_->QueryInterface(Faces[Index].Iid, Faces[Index].Slot);
            if (Result != GR_OK || *Faces[Index].Slot == nullptr)
                Fail("QueryInterface(%s) returned 0x%08lx", Faces[Index].Iid, (unsigned long)(Result));
        }
    }

    void FPlayer::Teardown()
    {
        INeuronComponent* const Faces[] = {Pipeline_, VbMgr_, Draw_, Ib_, Vb_, Gamma_, Texture_, Aggregate_};
        for (size_t Index = 0; Index < sizeof(Faces) / sizeof(Faces[0]); ++Index)
        {
            if (Faces[Index] != nullptr)
                Faces[Index]->Release();
        }
        Pipeline_ = nullptr;
        VbMgr_ = nullptr;
        Draw_ = nullptr;
        Ib_ = nullptr;
        Vb_ = nullptr;
        Gamma_ = nullptr;
        Texture_ = nullptr;
        Aggregate_ = nullptr;
        // The last reference: the renderer's destructor shuts the pipeline down.
        if (Object_ != nullptr)
        {
            Object_->Release();
            Object_ = nullptr;
        }
        for (std::map<long long, D3DLIGHT8*>::iterator It = Lights_.begin(); It != Lights_.end(); ++It)
            delete It->second;
        Lights_.clear();
        Handles_.clear();
        if (Window_ != nullptr)
        {
            DestroyWindow(Window_);
            Window_ = nullptr;
            PumpMessages();
        }
    }

    // --- Lines -------------------------------------------------------------------------------

    bool FPlayer::Run()
    {
        Bytes Raw;
        const std::string TracePath = Folder_ + "\\trace.jsonl";
        if (!ReadWholeFile(TracePath, Raw))
        {
            fprintf(stderr, "render-replay: %s: cannot read %s\n", Name_.c_str(), TracePath.c_str());
            return false;
        }
        std::vector<std::string> Lines;
        {
            std::string Current;
            for (size_t Index = 0; Index < Raw.size(); ++Index)
            {
                const char Ch = (char)(Raw[Index]);
                if (Ch == '\n')
                {
                    Lines.push_back(Current);
                    Current.clear();
                }
                else if (Ch != '\r')
                {
                    Current += Ch;
                }
            }
            if (!Current.empty())
                Lines.push_back(Current);
        }

        try
        {
            if (Lines.empty())
                Fail("empty trace");
            FJson Header;
            std::string Problem;
            if (!FJsonReader(Lines[0]).Read(&Header, &Problem))
                Fail("header: %s", Problem.c_str());
            const FJson* FormatName = Header.Get("format");
            const FJson* Version = Header.Get("version");
            if (FormatName == nullptr || FormatName->Text != "outpost-render-trace")
                Fail("the first line is not a trace header");
            if (Version == nullptr || Version->Kind != FJson::kNumber || Version->Number != 1)
                Fail("trace format version %lld; render-replay plays version 1",
                     Version != nullptr ? Version->Number : 0LL);

            // The back-buffer size of the first CreateBuffers (its mode's words 2 and 3).
            unsigned int Width = 0;
            unsigned int Height = 0;
            for (size_t Index = 1; Index < Lines.size() && Width == 0; ++Index)
            {
                if (Lines[Index].find("\"method\":\"CreateBuffers\"") == std::string::npos)
                    continue;
                FJson Line;
                if (!FJsonReader(Lines[Index]).Read(&Line, &Problem))
                    Fail("line %u: %s", (unsigned int)(Index + 1), Problem.c_str());
                const FJson* Args = Line.Get("args");
                bool bNull = false;
                std::vector<unsigned int> Mode = WordsArg(Args != nullptr ? *Args : FJson(), "Mode", &bNull);
                if (!bNull)
                {
                    Width = Mode[2];
                    Height = Mode[3];
                }
            }
            if (Width == 0 || Height == 0 || Width > 4096 || Height > 4096)
                Fail("no CreateBuffers with a usable back-buffer size");

            CreateWindowFor(Width, Height);
            CreateRenderer();

            for (size_t Index = 1; Index < Lines.size(); ++Index)
            {
                if (Lines[Index].empty())
                    continue;
                PlayLine(Lines[Index]);
                if (Lines[Index].find("\"end\":true") != std::string::npos)
                    break;
            }
            Seq_ = 0;
            Method_ = "end";
            if (Images_ == 0)
                Error("no selected frame was played to its SwapBuffers: no image written");
        }
        catch (const FReplayFailure& Failure)
        {
            fprintf(stderr, "render-replay: error: %s\n", Failure.Message.c_str());
            ++Errors_;
        }
        Teardown();
        printf("render-replay %s: %lu calls, %d image%s, %lu error%s, %lu warning%s\n", Name_.c_str(), Calls_, Images_,
               Images_ == 1 ? "" : "s", Errors_, Errors_ == 1 ? "" : "s", Warnings_, Warnings_ == 1 ? "" : "s");
        fflush(stdout);
        return Errors_ == 0;
    }

    void FPlayer::PlayLine(const std::string& Text)
    {
        FJson Line;
        std::string Problem;
        if (!FJsonReader(Text).Read(&Line, &Problem))
            Fail("%s in line: %.80s", Problem.c_str(), Text.c_str());
        if (Line.Kind != FJson::kObject)
            Fail("a line that is not an object: %.80s", Text.c_str());
        if (const FJson* Defaults = Line.Get("defaults"))
        {
            // Snapshot copies repeat the line of their CreateBuffers; the first one is checked.
            if (Line.Get("snapshot") == nullptr && bCheckDefaults_)
                CheckDefaults(*Defaults);
            bCheckDefaults_ = false;
            return;
        }
        if (Line.Get("begin_frame") != nullptr)
        {
            const FJson* Frame = Line.Get("frame");
            Frame_ = Frame != nullptr ? Frame->Number : 0;
            bSelected_ = true;
            return;
        }
        if (Line.Get("end_frame") != nullptr)
        {
            bSelected_ = false;
            return;
        }
        if (Line.Get("frame_image") != nullptr || Line.Get("end") != nullptr || Line.Get("format") != nullptr)
            return;
        if (Line.Get("method") == nullptr)
            Fail("a line that is no call, marker or synthetic line: %.80s", Text.c_str());
        PlayCall(Line);
    }

    void FPlayer::PlayCall(const FJson& Line)
    {
        const FJson* InterfaceName = Line.Get("interface");
        const FJson* MethodName = Line.Get("method");
        const FJson* SeqNo = Line.Get("seq");
        const FJson* Args = Line.Get("args");
        const FJson* Ret = Line.Get("ret");
        Seq_ = SeqNo != nullptr ? SeqNo->Number : 0;
        Method_ = (InterfaceName != nullptr ? InterfaceName->Text : std::string("?")) + "::" +
                  (MethodName != nullptr ? MethodName->Text : std::string("?"));
        if (InterfaceName == nullptr || MethodName == nullptr || InterfaceName->Kind != FJson::kString ||
            MethodName->Kind != FJson::kString)
            Fail("a call without interface or method");
        const FJson NoArgs;
        const FJson& Arguments = Args != nullptr ? *Args : NoArgs;
        FOutcome Out;
        Out.Result = GR_OK;
        Out.bCalled = false;
        Out.bWarnOnly = false;
        const char* Interface = InterfaceName->Text.c_str();
        const char* Method = MethodName->Text.c_str();
        if (!DispatchManual(Interface, Method, Arguments, &Out) && !DispatchGenerated(Interface, Method, Arguments, &Out))
            Fail("unknown method: render-replay does not know %s", Method_.c_str());
        ++Calls_;
        if (!Out.bCalled)
            return;
        const bool bHasRet = Ret != nullptr && Ret->Kind == FJson::kNumber;
        const long long Want = bHasRet ? Ret->Number : 0;
        if (bHasRet && (long long)(Out.Result) == Want)
        {
            if (Out.Result < 0)
                printf("  %s: seq %lld %s returned 0x%08lx, as recorded\n", Name_.c_str(), Seq_, Method_.c_str(),
                       (unsigned long)(Out.Result));
            return;
        }
        if (!bHasRet)
        {
            if (Out.Result < 0)
                printf("  %s: seq %lld %s returned 0x%08lx (no result recorded)\n", Name_.c_str(), Seq_,
                       Method_.c_str(), (unsigned long)(Out.Result));
            return;
        }
        if (Want >= 0 && Out.Result < 0 && !Out.bWarnOnly)
            Error("returned 0x%08lx (%ld); the trace recorded %lld", (unsigned long)(Out.Result), (long)(Out.Result), Want);
        else
            Warn("returned 0x%08lx (%ld); the trace recorded %lld", (unsigned long)(Out.Result), (long)(Out.Result), Want);
    }

    // The defaults line after CreateBuffers holds what the traced renderer's getters returned;
    // this renderer's are compared with it. A difference is reported, not applied: the player
    // replays into the real renderer, which reads its own INI.
    void FPlayer::CheckDefaults(const FJson& Defaults)
    {
        Method_ = "defaults";
        unsigned long Differences = 0;
        const FJson* Render = Defaults.Get("render");
        if (Render != nullptr && Render->Kind == FJson::kObject)
        {
            for (size_t Index = 0; Index < Render->Keys.size(); ++Index)
            {
                const unsigned long State = strtoul(Render->Keys[Index].c_str(), nullptr, 10);
                int Value = 0;
                const GENRESULT Result = Pipeline_->GetRenderState((D3DRENDERSTATETYPE)(State), &Value);
                const unsigned long Want = (unsigned long)(Render->Values[Index].Number);
                if (Result != GR_OK || (unsigned long)(unsigned int)(Value) != Want)
                {
                    Warn("render state %lu is %lu here, %lu in the trace", State, (unsigned long)(unsigned int)(Value), Want);
                    ++Differences;
                }
            }
        }
        const FJson* Pipeline = Defaults.Get("pipeline");
        if (Pipeline != nullptr && Pipeline->Kind == FJson::kObject)
        {
            for (size_t Index = 0; Index < Pipeline->Keys.size(); ++Index)
            {
                const int Id = atoi(Pipeline->Keys[Index].c_str());
                int Value = 0;
                const GENRESULT Result = Pipeline_->GetPipelineState(Id, &Value);
                const long Want = (long)(Pipeline->Values[Index].Number);
                if (Result != GR_OK || (long)(Value) != Want)
                {
                    Warn("pipeline state %d is %d here, %ld in the trace", Id, Value, Want);
                    ++Differences;
                }
            }
        }
        printf("  %s: defaults line checked, %lu difference%s\n", Name_.c_str(), Differences,
               Differences == 1 ? "" : "s");
    }

    // The selected frame's image: the back buffer through LockBuffer, before SwapBuffers.
    void FPlayer::CaptureFrame()
    {
        unsigned long Desc[5] = {0, 0, 0, 0, 0};
        const GENRESULT Locked = Pipeline_->LockBuffer(Desc);
        if (Locked < 0)
        {
            Error("LockBuffer returned 0x%08lx: the back buffer cannot be read (is LOCKABLE_BACKBUFFER set?)",
                  (unsigned long)(Locked));
            return;
        }
        const unsigned int Width = (unsigned int)(Desc[0]);
        const unsigned int Height = (unsigned int)(Desc[1]);
        const unsigned int Pitch = (unsigned int)(Desc[2]);
        const unsigned char* Bits = (const unsigned char*)((uintptr_t)(Desc[3]));
        const unsigned int PixelFormat = (unsigned int)(Desc[4]);
        const unsigned int Bpp = PixelBytes(PixelFormat);
        std::vector<unsigned char> Bmp;
        if (Bpp == 0 || Bits == nullptr || Width == 0 || Height == 0 || Pitch < Width * Bpp)
        {
            Pipeline_->UnlockBuffer();
            Error("the back buffer (pfenum %u, %ux%u, pitch %u) has no conversion to 24-bit RGB", PixelFormat, Width,
                  Height, Pitch);
            return;
        }
        // BITMAPFILEHEADER, BITMAPINFOHEADER (40 bytes, bottom-up, 24 bits, no compression, no
        // resolution), rows padded to four bytes.
        const unsigned long Row = (Width * 3 + 3) & ~3ul;
        const unsigned long ImageBytes = Row * Height;
        Bmp.push_back('B');
        Bmp.push_back('M');
        PutU32(Bmp, 54 + ImageBytes);
        PutU32(Bmp, 0);
        PutU32(Bmp, 54);
        PutU32(Bmp, 40);
        PutU32(Bmp, Width);
        PutU32(Bmp, Height);
        PutU16(Bmp, 1);
        PutU16(Bmp, 24);
        PutU32(Bmp, 0);
        PutU32(Bmp, ImageBytes);
        PutU32(Bmp, 0);
        PutU32(Bmp, 0);
        PutU32(Bmp, 0);
        PutU32(Bmp, 0);
        for (unsigned int Y = Height; Y-- > 0;)
        {
            const unsigned char* Source = Bits + (size_t)(Y) * Pitch;
            for (unsigned int X = 0; X < Width; ++X)
            {
                unsigned char Bgr[3];
                PixelToBgr(PixelFormat, Source + (size_t)(X) * Bpp, Bgr);
                Bmp.push_back(Bgr[0]);
                Bmp.push_back(Bgr[1]);
                Bmp.push_back(Bgr[2]);
            }
            for (unsigned long Pad = Width * 3; Pad < Row; ++Pad)
                Bmp.push_back(0);
        }
        const GENRESULT Unlocked = Pipeline_->UnlockBuffer();
        if (Unlocked < 0)
            Error("UnlockBuffer returned 0x%08lx", (unsigned long)(Unlocked));
        const std::string File = Images_ == 0 ? std::string("expected.bmp") : Format("frame-%lld.bmp", Frame_);
        const std::string Path = OutDir_ + "\\" + Name_ + "\\" + File;
        if (!WriteBytes(Path, Bmp))
        {
            Error("cannot write %s", Path.c_str());
            return;
        }
        ++Images_;
        printf("  %s: frame %lld: %ux%u back buffer (pfenum %u) -> %s\n", Name_.c_str(), Frame_, Width, Height,
               PixelFormat, Path.c_str());
    }

    // --- The hand-written methods ------------------------------------------------------------
    //
    // The 30 methods RenderTrace records by hand (make_render_trace.py "manual"), the
    // synthetic LightData line and IAggregateComponent::Initialize. Their argument names are
    // those RenderTrace.cpp writes.

    bool FPlayer::DispatchManual(const char* Interface, const char* Method, const FJson& Args, FOutcome* Out)
    {
        const std::string Name = std::string(Interface) + "::" + Method;
        Out->bCalled = true;

        if (Name == "IAggregateComponent::Initialize")
        {
            Out->Result = Aggregate_->Initialize();
            return true;
        }

        // IRenderPipeline: device, frames, back buffer.
        if (Name == "IRenderPipeline::CreateBuffers")
        {
            bool bNullMode = false;
            std::vector<unsigned int> Mode = WordsArg(Args, "Mode", &bNullMode);
            int OutMode[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
            const bool bOut = !IsNull(Args, "OutMode");
            Out->Result = Pipeline_->CreateBuffers(Window_, bNullMode ? nullptr : (int*)(WordPtr(Mode)),
                                                   bOut ? OutMode : nullptr);
            if (Out->Result < 0)
            {
                bDeviceFailed_ = true;
                Fail("CreateBuffers returned 0x%08lx: no Direct3D 8 HAL device could be created for a %ux%u "
                     "windowed back buffer on this machine",
                     (unsigned long)(Out->Result), Mode.size() > 3 ? Mode[2] : 0u, Mode.size() > 3 ? Mode[3] : 0u);
            }
            if (bOut)
            {
                bool bNull = false;
                std::vector<unsigned int> Recorded = WordsArg(Args, "OutMode", &bNull);
                std::string Got;
                for (int Index = 0; Index < 10; ++Index)
                    Got += Format(Index == 0 ? "%08x" : " %08x", (unsigned int)(OutMode[Index]));
                printf("  %s: CreateBuffers mode blob %s\n", Name_.c_str(), Got.c_str());
                for (int Index = 0; Index < 10; ++Index)
                {
                    if (Recorded[Index] != (unsigned int)(OutMode[Index]))
                        printf("  %s: (note) mode word %d is %08x here, %08x in the trace\n", Name_.c_str(), Index,
                               (unsigned int)(OutMode[Index]), Recorded[Index]);
                }
            }
            bCheckDefaults_ = true;
            return true;
        }
        if (Name == "IRenderPipeline::SwapBuffers")
        {
            if (bSelected_)
                CaptureFrame();
            Out->Result = Pipeline_->SwapBuffers();
            PumpMessages();
            return true;
        }
        if (Name == "IRenderPipeline::LockBuffer")
        {
            unsigned long Desc[5] = {0, 0, 0, 0, 0};
            Out->Result = Pipeline_->LockBuffer(Desc);
            return true;
        }
        if (Name == "IRenderPipeline::UnlockBuffer")
        {
            Out->Result = Pipeline_->UnlockBuffer();
            return true;
        }

        // Lights. The handle is the player's D3DLIGHT8 of that id.
        if (Name == "IRenderPipeline::SetLight")
        {
            D3DLIGHT8 Data;
            memset(&Data, 0, sizeof(Data));
            WriteLight(&Data, Args, "Light");
            Out->Result = Pipeline_->SetLight(LightArg(Args, "Handle"), IsNull(Args, "Light") ? nullptr : &Data);
            return true;
        }
        if (Name == "IRenderPipeline::AddLight" || Name == "IRenderPipeline::UpdateLight")
        {
            void* Handle = LightArg(Args, "Handle");
            // The struct as the caller filled it before the call.
            if (Handle != nullptr)
                WriteLight((D3DLIGHT8*)(Handle), Args, "Light");
            Out->Result = Method[0] == 'A' ? Pipeline_->AddLight(Handle) : Pipeline_->UpdateLight(Handle);
            return true;
        }
        if (Name == "IRenderPipeline::LightData")
        {
            // Synthetic: what the renderer read from the caller's struct at this flush; no call.
            void* Handle = LightArg(Args, "Handle");
            if (Handle != nullptr)
                WriteLight((D3DLIGHT8*)(Handle), Args, "Light");
            Out->bCalled = false;
            return true;
        }
        if (Name == "IRenderPipeline::SetLightEnable")
        {
            Out->Result = Pipeline_->SetLightEnable(LightArg(Args, "Handle"), (int)(Int(Args, "Enable")));
            return true;
        }
        if (Name == "IRenderPipeline::RemoveLight")
        {
            Out->Result = Pipeline_->RemoveLight(LightArg(Args, "Handle"));
            return true;
        }
        if (Name == "IRenderPipeline::DestroyLight")
        {
            Out->Result = Pipeline_->DestroyLight(LightArg(Args, "Handle"));
            return true;
        }

        // Textures.
        if (Name == "IRenderPipeline::CreateTexture")
        {
            unsigned int PixelFormat = (unsigned int)(UInt(Args, "Format"));
            void* Created = nullptr;
            Out->Result = Pipeline_->CreateTexture((int)(Int(Args, "Width")), (int)(Int(Args, "Height")), &PixelFormat,
                                                   (unsigned int)(UInt(Args, "Levels")), (unsigned int)(UInt(Args, "Flags")),
                                                   &Created);
            const long long Id = Int(Args, "OutHandle");
            if (Out->Result >= 0 && Created != nullptr && Id > 0)
                Handles_[Id] = Created;
            return true;
        }
        if (Name == "IRenderPipeline::DestroyTexture")
        {
            const long long Id = Int(Args, "Handle");
            Out->Result = Pipeline_->DestroyTexture(TextureArg(Args, "Handle"));
            if (Out->Result == GR_OK)
                Handles_.erase(Id);
            return true;
        }
        if (Name == "IRenderPipeline::LockTexture")
        {
            const long long Id = Int(Args, "Handle");
            const int Level = (int)(Int(Args, "Level"));
            unsigned long Desc[5] = {0, 0, 0, 0, 0};
            Out->Result = Pipeline_->LockTexture(TextureArg(Args, "Handle"), Level, Desc);
            if (Out->Result >= 0)
            {
                FTextureLock Lock;
                Lock.Bits = (unsigned char*)((uintptr_t)(Desc[3]));
                Lock.Pitch = (unsigned int)(Desc[2]);
                TextureLocks_[std::make_pair(Id, Level)] = Lock;
                const FJson& Recorded = Arg(Args, "Out");
                const FJson* Pitch = Recorded.Get("Pitch");
                if (Pitch != nullptr && (unsigned int)(Pitch->Number) != Lock.Pitch)
                    printf("  %s: (note) texture %lld level %d pitch is %u here, %u in the trace; rows are copied "
                           "one by one\n",
                           Name_.c_str(), Id, Level, Lock.Pitch, (unsigned int)(Pitch->Number));
            }
            return true;
        }
        if (Name == "IRenderPipeline::UnlockTexture")
        {
            const long long Id = Int(Args, "Handle");
            const int Level = (int)(Int(Args, "Level"));
            std::map<std::pair<long long, int>, FTextureLock>::iterator It = TextureLocks_.find(std::make_pair(Id, Level));
            bool bNull = false;
            std::vector<unsigned char> Data = BytesArg(Args, "data", &bNull);
            if (!bNull)
            {
                if (It == TextureLocks_.end() || It->second.Bits == nullptr)
                    Fail("texture %lld level %d is not locked", Id, Level);
                // Whole rows at the recorded pitch into rows at the real one (rows of 4x4 blocks
                // for DXT1 to DXT5, pfenum 10 to 14).
                const unsigned int PixelFormat = (unsigned int)(UInt(Args, "Format"));
                const unsigned int Height = (unsigned int)(UInt(Args, "Height"));
                const unsigned int Rows = PixelFormat >= 10 && PixelFormat <= 14 ? (Height + 3) / 4 : Height;
                const size_t SourcePitch = (size_t)(UInt(Args, "Pitch"));
                const size_t Copy = std::min(SourcePitch, (size_t)(It->second.Pitch));
                for (unsigned int Y = 0; Y < Rows; ++Y)
                {
                    const size_t From = (size_t)(Y) * SourcePitch;
                    if (From >= Data.size())
                        break;
                    memcpy(It->second.Bits + (size_t)(Y) * It->second.Pitch, &Data[From],
                           std::min(Copy, Data.size() - From));
                }
            }
            if (It != TextureLocks_.end())
                TextureLocks_.erase(It);
            Out->Result = Pipeline_->UnlockTexture(TextureArg(Args, "Handle"), Level);
            return true;
        }
        if (Name == "IRenderPipeline::SetTextureLevelData")
        {
            bool bNullFormat = IsNull(Args, "SrcFormat");
            unsigned int SrcFormat = bNullFormat ? 0u : (unsigned int)(UInt(Args, "SrcFormat"));
            bool bNullPixels = false;
            std::vector<unsigned char> Pixels = BytesArg(Args, "Pixels", &bNullPixels);
            bool bNullPalette = false;
            std::vector<unsigned char> Palette = BytesArg(Args, "B", &bNullPalette);
            // A is never read by the renderer (RenderTrace.cpp); it is passed as null.
            Out->Result = Pipeline_->SetTextureLevelData(
                TextureArg(Args, "Handle"), (int)(Int(Args, "Level")), (int)(Int(Args, "SrcW")), (int)(Int(Args, "SrcH")),
                (unsigned int)(UInt(Args, "Pitch")), bNullFormat ? nullptr : &SrcFormat,
                bNullPixels ? nullptr : BytePtr(Pixels), nullptr, bNullPalette ? nullptr : BytePtr(Palette));
            return true;
        }

        // IVertexBufferManager.
        if (Name == "IVertexBufferManager::LockVertexMemory")
        {
            FVertexMemoryLock Lock;
            memset(&Lock, 0, sizeof(Lock));
            Out->Result = VbMgr_->LockVertexMemory((unsigned int)(UInt(Args, "Fvf")), (unsigned int)(UInt(Args, "Count")),
                                                   &Lock);
            const FJson& Recorded = Arg(Args, "Out");
            if ((int)(Out->Result) >= 0 && Recorded.Kind == FJson::kObject)
            {
                const FJson* Id = Recorded.Get("VertexBuffer");
                const FJson* Base = Recorded.Get("BaseVertex");
                if (Id == nullptr || Id->Kind != FJson::kNumber)
                    Fail("LockVertexMemory without the pooled buffer's id");
                // The pooled buffer keeps one id for the whole trace.
                Handles_[Id->Number] = Lock.Listing.VertexBuffer;
                if (Base != nullptr && (unsigned long)(Base->Number) != Lock.Listing.BaseVertex)
                    printf("  %s: (note) LockVertexMemory base vertex is %lu here, %lu in the trace\n", Name_.c_str(),
                           Lock.Listing.BaseVertex, (unsigned long)(Base->Number));
                VertexMemoryLocks_.push_back(std::make_pair(Id->Number, Lock));
            }
            return true;
        }
        if (Name == "IVertexBufferManager::UnlockVertexMemory")
        {
            const long long Id = Int(Args, "VertexBuffer");
            if (VertexMemoryLocks_.empty() || VertexMemoryLocks_.back().first != Id)
                Fail("UnlockVertexMemory of buffer %lld, which has no open LockVertexMemory", Id);
            FVertexMemoryLock Lock = VertexMemoryLocks_.back().second;
            VertexMemoryLocks_.pop_back();
            FBufferLock Region;
            Region.Bits = (unsigned char*)(Lock.Vertices);
            Region.First = (unsigned int)(Lock.Listing.BaseVertex);
            Region.bFirstKnown = true;
            if (!IsNull(Args, "data"))
                WriteLocked(Region, Args, "FirstVertex", (unsigned int)(UInt(Args, "Stride")));
            Out->Result = VbMgr_->UnlockVertexMemory(&Lock);
            return true;
        }
        if (Name == "IVertexBufferManager::CopyVertexData")
            Fail("CopyVertexData is not replayed yet (the converted vertices are in the following unlock; the source "
                 "descriptor would have to be rebuilt)");

        // IRPIndexBuffer.
        if (Name == "IRPIndexBuffer::CreateIb")
        {
            unsigned int* Created = nullptr;
            Out->Result = Ib_->CreateIb((unsigned int)(UInt(Args, "Count")), &Created, (unsigned int)(UInt(Args, "Flags")));
            const long long Id = Int(Args, "Out");
            if (Out->Result != GR_GENERIC && Created != nullptr && Id > 0)
                Handles_[Id] = Created;
            return true;
        }
        if (Name == "IRPIndexBuffer::DestroyIb" || Name == "IRPVertexBuffer::DestroyVb")
        {
            const long long Id = Int(Args, "Handle");
            const bool bIb = strcmp(Interface, "IRPIndexBuffer") == 0;
            void* Handle = bIb ? IbArg(Args, "Handle") : VbArg(Args, "Handle");
            Out->Result = bIb ? Ib_->DestroyIb(&Handle) : Vb_->DestroyVb(&Handle);
            if (Out->Result == GR_OK && Id > 0)
                Handles_.erase(Id);
            return true;
        }
        if (Name == "IRPIndexBuffer::CopyIndices")
        {
            const bool bNullOffset = IsNull(Args, "OutOffset");
            unsigned int Offset = bNullOffset ? 0u : (unsigned int)(UInt(Args, "OffsetIn"));
            bool bNullData = false;
            std::vector<unsigned char> Data = BytesArg(Args, "SrcData", &bNullData);
            Out->Result = Ib_->CopyIndices(IbArg(Args, "Handle"), bNullOffset ? nullptr : &Offset,
                                           (const unsigned short*)(bNullData ? nullptr : BytePtr(Data)),
                                           (unsigned int)(UInt(Args, "Count")));
            if (!bNullOffset && Out->Result >= 0 && Offset != (unsigned int)(UInt(Args, "OutOffset")))
                Fail("CopyIndices landed at index %u, the trace's at %u", Offset, (unsigned int)(UInt(Args, "OutOffset")));
            return true;
        }
        if (Name == "IRPIndexBuffer::LockIb" || Name == "IRPVertexBuffer::LockVb")
        {
            const long long Id = Int(Args, "Handle");
            const bool bIb = strcmp(Interface, "IRPIndexBuffer") == 0;
            void* Handle = bIb ? IbArg(Args, "Handle") : VbArg(Args, "Handle");
            const bool bNullOffset = IsNull(Args, "Offset");
            unsigned int Offset = bNullOffset ? 0u : (unsigned int)(UInt(Args, "OffsetIn"));
            void* Bits = nullptr;
            const unsigned int Count = (unsigned int)(UInt(Args, "Count"));
            Out->Result = bIb ? Ib_->LockIb(Handle, bNullOffset ? nullptr : &Offset, &Bits, Count)
                              : Vb_->LockVb(Handle, bNullOffset ? nullptr : &Offset, &Bits, Count);
            if (Out->Result >= 0 && Bits != nullptr)
            {
                FBufferLock Lock;
                Lock.Bits = (unsigned char*)(Bits);
                Lock.First = Offset;
                Lock.bFirstKnown = !bNullOffset;
                BufferLocks_[Id].push_back(Lock);
            }
            return true;
        }
        if (Name == "IRPIndexBuffer::UnlockIb" || Name == "IRPVertexBuffer::UnlockVb")
        {
            const long long Id = Int(Args, "Handle");
            const bool bIb = strcmp(Interface, "IRPIndexBuffer") == 0;
            void* Handle = bIb ? IbArg(Args, "Handle") : VbArg(Args, "Handle");
            std::map<long long, std::vector<FBufferLock> >::iterator It = BufferLocks_.find(Id);
            const bool bLocked = It != BufferLocks_.end() && !It->second.empty();
            if (!IsNull(Args, "data"))
            {
                if (!bLocked)
                    Fail("buffer %lld has unlock data but no open lock", Id);
                WriteLocked(It->second.back(), Args, bIb ? "FirstIndex" : "FirstVertex",
                            bIb ? 2u : (unsigned int)(UInt(Args, "Stride")));
            }
            if (bLocked)
                It->second.pop_back();
            Out->Result = bIb ? Ib_->UnlockIb(Handle) : Vb_->UnlockVb(Handle);
            return true;
        }

        // IRPVertexBuffer.
        if (Name == "IRPVertexBuffer::CreateVb")
        {
            unsigned int* Created = nullptr;
            Out->Result = Vb_->CreateVb((unsigned int)(UInt(Args, "Fvf")), (unsigned int)(UInt(Args, "Count")), &Created,
                                        (unsigned int)(UInt(Args, "Flags")));
            const long long Id = Int(Args, "Out");
            if (Out->Result >= 0 && Created != nullptr && Id > 0)
                Handles_[Id] = Created;
            return true;
        }
        if (Name == "IRPVertexBuffer::CopyVertices")
            Fail("CopyVertices is not replayed yet (the source descriptor would have to be rebuilt from the "
                 "resolved streams)");

        // IRPTexture.
        if (Name == "IRPTexture::LoadTexture" || Name == "IRPTexture::LoadCubemap")
            Fail("%s reads the data tree through a file system and is not replayed yet", Method);

        Out->bCalled = false;
        return false;
    }

    // The 80 methods RenderTrace records by its generic path.
#include "oracle_render_replay_generated.h"

    // The trace folders under Root (those that hold a trace.jsonl), in name order.
    std::vector<std::string> TraceFolders(const std::string& Root)
    {
        std::vector<std::string> Names;
        WIN32_FIND_DATAA Found;
        HANDLE Search = FindFirstFileA((Root + "\\*").c_str(), &Found);
        if (Search == INVALID_HANDLE_VALUE)
            return Names;
        do
        {
            const std::string Name = Found.cFileName;
            if (Name == "." || Name == ".." || (Found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                continue;
            if (GetFileAttributesA((Root + "\\" + Name + "\\trace.jsonl").c_str()) != INVALID_FILE_ATTRIBUTES)
                Names.push_back(Name);
        } while (FindNextFileA(Search, &Found));
        FindClose(Search);
        std::sort(Names.begin(), Names.end());
        return Names;
    }

    bool FullPath(const char* Path, std::string* Out)
    {
        char Full[MAX_PATH] = {};
        const DWORD Length = GetFullPathNameA(Path, MAX_PATH, Full, nullptr);
        if (Length == 0 || Length >= MAX_PATH)
            return false;
        *Out = Full;
        while (Out->size() > 3 && ((*Out)[Out->size() - 1] == '\\' || (*Out)[Out->size() - 1] == '/'))
            Out->erase(Out->size() - 1);
        return true;
    }

    // The engine INI of the player (see the top of this file) into neuroncomponents' parser.
    bool LoadPlayerIni()
    {
        IProfileParser* Parser = AcquireProfileParser();
        if (Parser == nullptr)
        {
            fprintf(stderr, "render-replay: no profile parser\n");
            return false;
        }
        char Folder[MAX_PATH + 1] = {};
        const DWORD Length = GetTempPathA(sizeof(Folder), Folder);
        std::string Path = (Length > 0 && Length < sizeof(Folder)) ? std::string(Folder) : std::string(".\\");
        Path += Format("outpost-render-replay-%lu.ini", GetCurrentProcessId());
        std::vector<unsigned char> Text(kDefaultSectionIni, kDefaultSectionIni + sizeof(kDefaultSectionIni) - 1);
        bool bLoaded = false;
        if (WriteBytes(Path, Text))
            bLoaded = Parser->LoadFile(Path.c_str(), 0) == GR_OK;
        DeleteFileA(Path.c_str());
        Parser->Release();
        if (!bLoaded)
            fprintf(stderr, "render-replay: cannot load the player's engine INI (%s)\n", Path.c_str());
        return bLoaded;
    }
}

int RunRenderReplayMode(int Argc, char** Argv)
{
    std::string Root;
    std::string OutDir;
    if (Argc != 4 || !FullPath(Argv[2], &Root) || !FullPath(Argv[3], &OutDir))
    {
        fprintf(stderr, "usage: Oracle.exe render-replay <vectors directory> <output directory>\n");
        return 2;
    }
    FDUMP = ReplayDump;
    const std::vector<std::string> Names = TraceFolders(Root);
    if (Names.empty())
    {
        fprintf(stderr, "render-replay: no <name>\\trace.jsonl under %s\n", Root.c_str());
        return 2;
    }
    if (!LoadPlayerIni())
        return 2;
    // DX8RENDERER creates neuroncomponents' "3DMathEngine" (x86math.dll) in Initialize and calls
    // it without a null check (the Z flip of SetView and SetModelview).
    if (!OracleLoadMathEngine())
    {
        fprintf(stderr, "render-replay: x86math.dll and its 3DMathEngine are needed by DX8RENDERER\n");
        return 2;
    }
    // DX8RENDERER.dll beside Oracle.exe; its DllMain registers "DirectX8" with neuroncomponents.
    if (LoadLibraryA("DX8RENDERER.dll") == nullptr)
    {
        fprintf(stderr, "render-replay: cannot load DX8RENDERER.dll (error %lu); it is built beside Oracle.exe, and "
                        "needs d3d8.dll and ddraw.dll\n",
                GetLastError());
        return 2;
    }
    CreateDirectoryA(OutDir.c_str(), nullptr);
    printf("render-replay: %u trace%s under %s\n", (unsigned int)(Names.size()), Names.size() == 1 ? "" : "s",
           Root.c_str());
    fflush(stdout);
    unsigned int Failed = 0;
    bool bDeviceFailed = false;
    for (size_t Index = 0; Index < Names.size(); ++Index)
    {
        FPlayer Player(Root + "\\" + Names[Index], Names[Index], OutDir);
        if (!Player.Run())
            ++Failed;
        if (Player.DeviceFailed())
        {
            bDeviceFailed = true;
            // The same machine cannot do better for the next trace.
            break;
        }
    }
    if (bDeviceFailed)
    {
        fprintf(stderr, "render-replay: Direct3D 8 device creation failed (DeviceType Hardware, the only type "
                        "DX8RENDERER creates); no images can be made on this machine\n");
        return 1;
    }
    printf("render-replay: %u of %u traces failed\n", Failed, (unsigned int)(Names.size()));
    return Failed == 0 ? 0 : 1;
}
