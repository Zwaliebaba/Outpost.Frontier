// Author: Giperion.

#ifndef SERVER_SAVEFILE_H
#define SERVER_SAVEFILE_H

// server_savefile.h -- the ini-writer object baseline's character serializer
// builds on its own stack, and the "FLS1" container every saved character file
// is wrapped in.

// ADDED 2026-09-17 (wave 18). Everything here is read off Server
// and, where the listing truncates an operand, off the bytes of EXE\Server.dll
// itself. Nothing here comes from the .c analysis.

// ---------------------------------------------------------------------------
// THE TWO VTABLES

// .rdata:the branch is seven dwords, every one of them .text:the branch,
// and module storage is `ff258c4cd606  JMP dword ptr [->MSVCRT.DLL::purecall]`.
// Seven purecall slots means an abstract base with seven pure virtuals and no
// virtual destructor -- there is no eighth slot and no deleting-destructor slot.
// That base is ISave below.

// .rdata:PTR_GetID_06d6b5a0 is the concrete override table:
//   +0x00  module storage   `8b4104 MOV EAX,dword ptr [ECX + 0x4]` / `c3 RET`
//   +0x04  module storage   fprintf(file, "\n[%s]\n", a)            RET 0x4
//   +0x08  module storage   fprintf(file, "%s = %g\n", a, (double)b) RET 0x8
//   +0x0c  module storage   fprintf(file, "%s = %u\n", a, b)        RET 0x8
//   +0x10  module storage   fprintf(file, "%s = %d\n", a, b)        RET 0x8
//   +0x14  module storage   fprintf(file, "%s = %s\n", a, b)        RET 0x8
//   +0x18  module storage   if (a) fprintf(file, a); fprintf(file, "\n")  RET 0x4
// The `RET n` of each one is what fixes its arity; analysis's own signatures were
// not used. Every one of them reads the FILE* from `[ECX + 0x4]`
// (e.g. module storage `8b4104 MOV EAX,dword ptr [ECX + 0x4]`), which is what
// puts the FILE* at +0x04 and the vptr at +0x00.

// SLOT 0 IS NOT CPlayerGroup::GetID. analysis prints module storage as
// `CPlayerGroup::GetID` because `MOV EAX,[ECX+4] / RET` is three bytes that the
// linker folded across every class that has such an accessor. An ICF-folded name
// does not identify its owner. Here the dword at +0x04 is the FILE* -- proved by
// the six sibling slots that fprintf to exactly that dword -- so slot 0 is this
// class's FILE* accessor, whatever the fold named it.

// ---------------------------------------------------------------------------
// THE CLASS HIERARCHY (RTTI, bytes read out of EXE\Server.dll)

// The COL in front of the concrete table, module storage, is 06d70a5c:
//   COL  06d70a5c  -> TypeDescriptor ".?AUSaveGame_Writer@@", CHD 06d70a4c
//   CHD  06d70a4c  attributes 1 (multiple inheritance), 3 bases, BCA 06d70a3c
//   BCA  06d70a3c  = [ BCD 06d70a24 SaveGame_Writer  contained 2, PMD(0,-1,0)
//                      BCD 06d70a0c SaveGame         contained 0, PMD(4,-1,0)
//                      BCD 06d709f4 ISave            contained 0, PMD(0,-1,0) ]
// and the COL in front of the purecall table, module storage, is 06d709e0
// (".?AUISave@@", CHD 06d709d0, one base). So the concrete writer is
// `struct SaveGame_Writer : SaveGame, ISave` -- the BCA lists bases in
// declaration order, SaveGame first -- and the compiler moves the polymorphic
// ISave to +0 and the non-polymorphic SaveGame to +4, exactly as the PMDs say.
// The constructor order agrees: every inlined writer constructor builds the
// SaveGame part FIRST (module storage CALL the routine on writer+4, and
// in PlayerDB::create_restart_file), then stamps ISave's table
// (module storage / module storage), then the path member, then the concrete
// table (module storage / module storage). All four vptr operands are truncated
// in the listing; the real bytes are `c7 44 24 28 c0 b5 d6 06`,
// `c7 44 24 18 c0 b5 d6 06`, `c7 44 24 24 a0 b5 d6 06` and
// `c7 44 24 14 a0 b5 d6 06`.

// THE OBJECT LAYOUT (baseline, MSVCP60 std::string = 0x10 bytes)
//   +0x00  vptr (ISave)  06d6b5c0 during construction, then 06d6b5a0
//   +0x04  SaveGame      FILE* at +0x04 (module storage stores fopen's result),
//                        its std::string buffer at +0x08 (_Ptr +0x0c, _Len +0x10:
// / module storage are the fwrite data and
//                        count)
//   +0x18  std::string   the path (_Ptr +0x1c, module storage)
// The port keeps the hierarchy and the member order; the byte offsets differ
// only because std::string is not MSVCP60's here. Nothing outside Server.dll
// sees these classes (Common's Reputation::Save takes the ISave* only), so the
// layout is not ABI.
// ---------------------------------------------------------------------------

#include <cstdio>
#include <string>

// ---------------------------------------------------------------------------
// ISave -- baseline's abstract writer base (vtable module storage, all purecall).

// No virtual destructor: baseline's table has exactly seven slots and none of them
// is a destructor. Adding one here would insert a slot baseline does not have, so
// the one place that owns a writer owns it by value.

// A struct, not a class: Server's RTTI names it .?AUISave@@, and Common.dll's
// export ?Save@Reputation@@YAXPAUISave@@@Z takes `struct ISave *`. This is the
// only declaration; Common.dll's Reputation::Save includes this header.
// ---------------------------------------------------------------------------
struct ISave {
public:
    virtual FILE* GetFile() const = 0;                                  // +0x00
    virtual void WriteSection(char const* Name) = 0;                    // +0x04
    virtual void WriteFloat(char const* Key, float Value) = 0;          // +0x08
    virtual void WriteUInt(char const* Key, unsigned int Value) = 0;    // +0x0c
    virtual void WriteInt(char const* Key, int Value) = 0;              // +0x10
    virtual void WriteString(char const* Key, char const* Value) = 0;   // +0x14
    virtual void WriteLine(char const* Text) = 0;                       // +0x18

protected:
    ~ISave() {}   // non-virtual and protected: baseline has no destructor slot.
};

// ---------------------------------------------------------------------------
// SaveGame -- baseline .?AUSaveGame@@ (TypeDescriptor module storage), the
// { FILE*, std::string } pair every character-file reader and writer works on.
// Non-polymorphic: no vptr, it appears in RTTI only as SaveGame_Writer's base.

// Its methods, each a thiscall whose ECX is the pair itself -- the writer's
// sites pass writer+4 (module storage / module storage `LEA ESI,[reg + 0x4]`),
// the bare-pair sites pass the pair (module storage .. 06d5386b):
// constructor
// CloseFile
// ReadTail   (RET 4)
// Encode

// NO USER DESTRUCTOR. baseline tears the SaveGame part of a writer down with the
// string destructor only: the open-failure exit of PlayerData_SerializeToFile
// runs the writer's destructor inline -- module storage concrete vptr,
// CALL the routine, module storage _Tidy of the path,
// _Tidy of the buffer -- and there is no FILE* test after the
// opaque call, so SaveGame's own destructor does not fclose. The EH unwind for
// the half-built writer in PlayerDB::create_restart_file agrees: state 0
// (module storage) is Unwind@06d63920 -> the routine on EBP-0x554 = writer+4,
// and the routine is only the _Tidy of the std::string at +4 (ICF-shared by
// every { dword, std::string } type). The closing destructor the routine
// belongs to FSaveFileBuffer below, not to SaveGame.
// ---------------------------------------------------------------------------
struct SaveGame {
    // (module storage .. module storage, plain RET): the string's
    // allocator byte at +4 (the `MOV CL,[ESP+3]` uninitialised-byte idiom),
    // _Ptr/_Len/_Res = 0 at +8/+0xc/+0x10, then FILE* = 0 at +0.
    SaveGame();

    // (module storage .. module storage, plain RET):
    // `if (File) { fclose(File); File = 0; }`. Callers module storage,
    // (bare pair), module storage / module storage (writer+4).
    void CloseFile();

    // (module storage .. module storage, RET 4): fseek end, size =
    // ftell - Offset, fseek(Offset), resize(size), fread through the writable
    // pointer, resize(read). 17 call sites.
    void ReadTail(long Offset);

    // (module storage .. module storage, plain RET): length read
    // first (module storage `MOV EBP,[ECX + 0xc]`), the buffer unshared, then the
    // FLS1 cipher loop over it (Server_FLS1Transform). 13 call sites.
    void Encode();

    FILE* File;              // +0x00
    std::string Contents;    // +0x04
};

// ---------------------------------------------------------------------------
// SaveGame_Writer -- baseline .?AUSaveGame_Writer@@, the concrete writer
// (vtable module storage, COL 06d70a5c). Bases in baseline declaration order.

// The writer saves text directly. Legacy decoding lives on the SaveGame base.
// ---------------------------------------------------------------------------
struct SaveGame_Writer : public SaveGame, public ISave {
public:
    SaveGame_Writer();

    // (plain RET, own EH frame): concrete vptr, then the
    // body (the inline copy at module storage .. 06d4cfcd CALLs it),
    // then the path and the SaveGame part.
    ~SaveGame_Writer();

    // Open a plain-text save for writing.
    bool Open(char const* InPath);

    void Close();

    // `f6400c20 TEST byte ptr [EAX + 0xc],0x20`. EAX is the FILE*,
    // +0x0c is MSVCRT's `_flag` and 0x20 is _IOERR -- an inlined ferror(). baseline
    // tests it once, right before the closeout, and FDUMPs
    // "*** WARNING: Error writing ..." from PlayerData.cpp line 0xc6d
    // (module storage pushes 0xc6d) when it is set.
    bool HadWriteError() const;

    FILE* GetFile() const override;                                         // +0x00
    void WriteSection(char const* Name) override;                           // +0x04
    void WriteFloat(char const* Key, float Value) override;                 // +0x08
    void WriteUInt(char const* Key, unsigned int Value) override;           // +0x0c
    void WriteInt(char const* Key, int Value) override;                     // +0x10
    void WriteString(char const* Key, char const* Value) override;          // +0x14
    void WriteLine(char const* Text) override;                              // +0x18

    // +0x04 .. +0x18 is the SaveGame base (File, Contents).
    std::string Path;        // +0x18

private:
    SaveGame_Writer(SaveGame_Writer const&);
    SaveGame_Writer& operator=(SaveGame_Writer const&);
};

// ---------------------------------------------------------------------------
// The "FLS1" container.

// EVERY saved character file is wrapped, not just multiplayer ones -- see the
// note on Server_FLS1Transform in server_savefile.cpp for the proof.

// .rdata:the field is the four bytes `46 4c 53 31` = "FLS1" (read out of
// EXE\Server.dll; the listing prints it as the dword `31534C46h`). baseline passes
// it to fprintf as the WHOLE format string, with no arguments -- module storage
// PUSHes it and module storage calls fprintf with a two-argument stack.
// ---------------------------------------------------------------------------
extern char const Server_FLS1Magic[5];   // "FLS1"

// The stand-alone reading pair: the routine builds one at ESP+0x34,
// PlayerDB::ReadCharacterName (module storage) at ESP+0xc, the routine at S+0x4c,
// (the template) at S+0x54, PlayerDB::create_restart_file (the
// template) at ESP+0x44.

// It is NOT a plain SaveGame, because its destructor closes the file and
// SaveGame's does not (see SaveGame above): the routine is called out of line
// on it by create_restart_file (module storage, module storage) and by the EH
// unwinds Unwind@06d63080 / 06d63268 / 06d632e2 / 06d638d0 / 06d63936 -- unwind
// funclets only ever call destructors. Its constructor is SaveGame's own
// (module storage CALL the routine on the template), and ReadTail / Encode /
// CloseFile are called on it with the same ECX, so it is a SaveGame plus a
// closing destructor. It is non-polymorphic and never a base of a polymorphic
// class, so it leaves no RTTI and its baseline name is unknown; FSaveFileBuffer is
// the port's name for it.
struct FSaveFileBuffer : public SaveGame {
    // (module storage .. module storage, plain RET):
    // `if (File) { fclose(File); File = 0; }` (module storage .. module storage,
    // CloseFile's body inlined), then the string's _Tidy -- here the base's own
    // member destructor. the routine, the routine, the routine and
    // PlayerDB::ReadCharacterName inline the same sequence at their exits.
    ~FSaveFileBuffer() {
        CloseFile();
    }
};

// , module storage .. module storage:

//   MOV  BL,byte ptr [EAX + ESI*0x1]     ; in[i]
//   MOV  ECX,EAX
//   AND  ECX,0x3
//   MOV  DL,byte ptr [ECX + 0x06d6b318]  ; key[i & 3]
//   ADD  DL,AL                           ; + (unsigned char)i, 8-bit wrap
//   OR   DL,0x80
//   XOR  BL,DL
//   MOV  byte ptr [EAX + ESI*0x1],BL

// XOR, so the same routine encodes and decodes. The `MOV DL,[ECX + disp32]` at
// really is a memory load: its bytes are `8a 91 18 b3 d6 06`,
// opcode 0x8a with mod=10/rm=001, i.e. base ECX plus disp32. That check matters,
// because the sibling instruction at module storage looks like the same thing in
// analysis's output and is not -- see PlayerData_SerializeToFile.
void Server_FLS1Transform(char* Data, int Length);

// ---------------------------------------------------------------------------
// The account-path helpers and the last-written-path global, shared by the
// serializer (server_pub.cpp) and the save wrappers (server_savegame.cpp).
// ---------------------------------------------------------------------------

// GetUserDataPath + "Accts" / "Accts\MultiPlayer" into the
// global 0x104 buffer the field, whose address it returns.
char const* Server_GetAccountsRoot();

// the MP account directory name, "%02x-%08x".
void Server_AccountDirName(char* Out, wchar_t const* AccountName);

// baseline CRC engine the field as configured by the branch (width 31, poly
// 0x8005, reflected, XorIn/XorOut 0): reset, the routine over ByteCount bytes,
// . Used for account directory names, character-ID file names and
// the account/password file XOR keys (low 16 bits of the result).
unsigned int Server_AccountCrc(unsigned char const* Bytes, unsigned int ByteCount);

// (0x104 bytes): the path the routine last opened for
// writing (safe_strcpy at module storage); the routine copies it to
// AutoSave.fl (module storage).
extern char GServerLastSavePath[0x104];

#endif // SERVER_SAVEFILE_H
