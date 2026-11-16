// Author: Giperion.

// server_savefile.cpp -- the ini-writer object and the "FLS1" container.

// ADDED 2026-09-17 (wave 18). See src/include/server_savefile.h for the vtable
// and layout evidence; this file carries the evidence for the behaviour.

#include "../include/server_savefile.h"

#include <windows.h>
#include <cstring>

// ---------------------------------------------------------------------------
// .rdata:the field, bytes `46 4c 53 31` read out of EXE\Server.dll. The
// listing prints it as the dword `31534C46h`, which is the same four bytes and
// is NOT how baseline uses it: module storage PUSHes the ADDRESS and
// calls fprintf with only the stream and that pointer, so it goes
// out as a bare format string -- four literal characters, no conversion, no
// trailing newline.
// ---------------------------------------------------------------------------
char const Server_FLS1Magic[5] = "FLS1";

// ---------------------------------------------------------------------------
// The cipher key.

// is the start of the string
//   "Genesis - In the beginning Yahweh created the heavens and the ea..."
// and the encoder indexes it with `i & 3`, so only its first four bytes are ever
// reachable. They are `47 65 6e 65` = "Gene". The whole literal is kept here so
// the source shows the real datum rather than a four-byte transcription of it.
// ---------------------------------------------------------------------------
static char const* const FLS1KeyString =
    "Genesis - In the beginning Yahweh created the heavens and the ea";

void Server_FLS1Transform(char* Data, int Length) {
    // `CMP EBP,EBX` / `JLE` and module storage `CMP EAX,EBP` /
    // `JL` are both SIGNED, so baseline does nothing at all for a negative or zero
    // length. Kept as `int` for that reason rather than widened to size_t.
    if (Length <= 0)
        return;

    for (int I = 0; I < Length; ++I) {
        unsigned char const KeyByte =
            static_cast<unsigned char>(FLS1KeyString[I & 3]);
        // ADD DL,AL is an 8-bit add and wraps; OR DL,0x80 forces the top bit, so
        // the mask is never 0 and a run of identical plaintext never survives.
        unsigned char const Mask = static_cast<unsigned char>(
            static_cast<unsigned char>(KeyByte + static_cast<unsigned char>(I)) | 0x80u);
        Data[I] = static_cast<char>(static_cast<unsigned char>(Data[I]) ^ Mask);
    }
}

// ---------------------------------------------------------------------------
// SaveGame -- the { FILE*, std::string } pair (.?AUSaveGame@@).
// ---------------------------------------------------------------------------

SaveGame::SaveGame()
    : File(nullptr), Contents() {
    // allocator byte (uninitialised-byte idiom), _Ptr/_Len/_Res = 0,
    // FILE* = 0. The port's std::string default constructor is the same empty
    // string.
}

void SaveGame::CloseFile() {
    // (module storage .. module storage).
    if (File != nullptr) {                                  // TEST / JZ
        fclose(File);
        File = nullptr;
    }
}

void SaveGame::ReadTail(long Offset) {
    // (module storage .. module storage, RET 4).
    fseek(File, 0, SEEK_END);
    long const Size = ftell(File) - Offset;                 // , 06d4898f SUB EDI,EAX
    fseek(File, Offset, SEEK_SET);
    // .. module storage: MSVCP60's resize(Size) inlined -- erase
    // at Size when the length is not below it (CMP / JC), else an append of
    // (Size - length) zero bytes.
    Contents.resize(static_cast<size_t>(Size));
    // .. module storage: the writable pointer (the empty-string
    // static when _Ptr is null), then fread(p, 1, Size, File) at module storage.
    size_t const Read = fread(&Contents[0], 1, static_cast<size_t>(Size), File);
    // .. module storage: resize(Read) inlined the same way.
    Contents.resize(Read);
}

void SaveGame::Encode() {
    // (module storage .. module storage). The length is read
    // before the buffer is unshared (module storage `MOV EBP,[ECX + 0xc]`); the
    // unshare / freeze (module storage .. 06d48bdf) is MSVCP60 COW bookkeeping
    // that a non-COW std::string does not need; &Contents[0] is the same writable
    // pointer (the terminator when empty, where the signed length test stops the
    // loop before any access, as baseline's empty-string static does).
    int const Length = static_cast<int>(Contents.size());
    Server_FLS1Transform(&Contents[0], Length);
}

// ---------------------------------------------------------------------------
// SaveGame_Writer
// ---------------------------------------------------------------------------

SaveGame_Writer::SaveGame_Writer()
    : SaveGame(), ISave(), Path() {
    // baseline order (module storage .. module storage): SaveGame's constructor
    // on this+4, ISave's vptr 06d6b5c0, the path string's
    // allocator constructor, the concrete vptr 06d6b5a0. The path gets its value
    // in Open().
}

SaveGame_Writer::~SaveGame_Writer() {
    Close();
}

bool SaveGame_Writer::Open(char const* InPath) {
    // (w4-server-guards) no null test on InPath: both inlined opens strlen the
    // path straight away (module storage / module storage `SCASB.REPNE`) and all
    // callers pass a local char buffer.
    Path = InPath;                                           // assign
    File = fopen(InPath, "wt");
    return File != nullptr;                                 // TEST / module storage JNZ
}

bool SaveGame_Writer::HadWriteError() const {
    // `MOV EAX,[ESP + 0x28]` (the FILE*) then module storage
    // `f6400c20 TEST byte ptr [EAX + 0xc],0x20`. MSVCRT's FILE is
    // { char* _ptr; int _cnt; char* _base; int _flag; ... } so +0x0c is _flag,
    // and 0x20 is _IOERR. That is ferror(), inlined.
    // (w4-server-guards) no null test: module storage dereferences the FILE*
    // directly, and the only caller reaches it after a successful open
    // (module storage `JNZ the branch`).
    return ferror(File) != 0;
}

void SaveGame_Writer::Close() {
    CloseFile();
}

FILE* SaveGame_Writer::GetFile() const {
    // slot +0x00, module storage `MOV EAX,[ECX + 0x4]` / `RET`.
    return File;
}

void SaveGame_Writer::WriteSection(char const* Name) {
    // slot +0x04, module storage: fprintf(file, "\n[%s]\n", Name). RET 0x4.
    // None of the six writer slots (+0x04 .. +0x18) tests the FILE*: both callers
    // only write after a successful Open (the routine module storage,
    // module storage). The port's null guards were removed 2026-09-24.
    fprintf(File, "\n[%s]\n", Name);
}

void SaveGame_Writer::WriteFloat(char const* Key, float Value) {
    // slot +0x08, module storage. `FLD float ptr [ESP + 0x8]` then
    // `FSTP double ptr [ESP]` -- the argument arrives as a float and is widened
    // to a double for "%s = %g\n" (.rdata:s_%s_=_%g_06d6b5f4). RET 0x8.
    fprintf(File, "%s = %g\n", Key, static_cast<double>(Value));
}

void SaveGame_Writer::WriteUInt(char const* Key, unsigned int Value) {
    // slot +0x0c, module storage, "%s = %u\n" (.rdata:s_%s_=_%u_06d6b5e8). RET 0x8.
    fprintf(File, "%s = %u\n", Key, Value);
}

void SaveGame_Writer::WriteInt(char const* Key, int Value) {
    // slot +0x10, module storage, "%s = %d\n" (.rdata:s_%s_=_%d_06d6b5dc). RET 0x8.
    fprintf(File, "%s = %d\n", Key, Value);
}

void SaveGame_Writer::WriteString(char const* Key, char const* Value) {
    // slot +0x14, module storage, "%s = %s\n" (.rdata:s_%s_=_%s_06d6b578). RET 0x8.
    // This is the slot Server_WriteCostume dispatches (`CALL dword ptr [EAX + 0x14]`
    // at module storage and its three copies).
    fprintf(File, "%s = %s\n", Key, Value);
}

void SaveGame_Writer::WriteLine(char const* Text) {
    // slot +0x18, module storage. Two calls, and the first one is skipped when
    // the argument is null (module storage `TEST EAX,EAX` / `JZ the branch`):
    //   if (Text) fprintf(file, Text);        <- Text is the FORMAT, not an
    //                                            argument to one
    //   fprintf(file, "\n");                  <- .rdata:the field, one byte 0x0a
    // RET 0x4.

    // baseline really does pass the caller's string as the format string
    // (module storage PUSH EAX / module storage PUSH [ESI+4] / module storage
    // CALL fprintf -- two arguments, no "%s"), so the port does too.
    if (Text != nullptr)
        fprintf(File, Text);
    fprintf(File, "\n");
}
