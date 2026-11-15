// Author: Giperion.

#pragma once

// RNG pool, RC4, and MD4 from movie.dll, 0x06ace800 through 0x06acef00.
// The baseline address is on each definition. A 0/1 result is EAX.

// Magic 0x1423. The pool is 0x100 bytes at +0x20.
// +0x18 and +0x1c are not read or written in this block.
struct FRngState
{
    unsigned int Magic;
    unsigned int Size;
    unsigned int Flags;
    unsigned int Step;
    unsigned int Index;
    unsigned int Mode;
    unsigned int Field_0x18;
    unsigned int Field_0x1C;
    unsigned char Buffer[0x100];
};

// Counts are bit lengths. Final leaves the 16-byte digest at +0x58.
struct FMd4Context
{
    unsigned int State[4];
    unsigned int BitCountLow;
    unsigned int BitCountHigh;
    unsigned char Buffer[64];
    unsigned char Digest[16];
};

// . RET 0x10. Null state returns 0 and writes nothing.
int __stdcall RngInitState(FRngState* State, unsigned int Step, unsigned int Mode, unsigned int Flags);

// . RET 0xC. On success, Buffer receives state+0x20 and Size receives the dword at +0x04.
int __stdcall RngQueryBuffer(FRngState* State, unsigned char** Buffer, unsigned int* Size);

// . RET 0xC. Mode 1 folds with MD4 (16 bytes); any other mode folds with SHA-1 (20 bytes).
int __stdcall RngStir(FRngState* State, const void* Data, unsigned int Length);

// . RET 8. Size is the in/out byte count; its stack slot is lpcbData.
int __stdcall RngReadSeed(unsigned char* Data, unsigned int Size);

// . RET 8. Writes value "Seed" as type 3 (REG_BINARY).
int __stdcall RngWriteSeed(const unsigned char* Data, unsigned int Size);

// . RET 8. Creates HKLM\SOFTWARE\Microsoft\Cryptography\RNG.
int __stdcall RngOpenKey(unsigned int Access, void** Key);

// . RET 0xC. RC4 keystream XOR. Indices live at box+0x100 and box+0x101.
void __stdcall Rc4Crypt(unsigned char* Box, unsigned int Length, unsigned char* Data);

// . RET 0xC. Identity permutation, then KSA. KeyLength 0 does not wrap the key index.
void __stdcall Rc4Init(unsigned char* Box, unsigned int KeyLength, const unsigned char* Key);

// . RET 0xC. Four SHA-1 passes over Data sliced by Length>>2, folded into Blocks (0x50 bytes).
int __stdcall MixShaBlocks(const unsigned char* Data, unsigned int Length, unsigned char* Blocks);

// . RET 8. Both arguments are unread. EAX is 0.
int __stdcall RngTryReadBlock(void* Buffer, unsigned int* Count);

// . RET 4.
void __stdcall Md4Init(FMd4Context* Context);

// . RET 0xC.
void __stdcall Md4Update(FMd4Context* Context, const unsigned char* Data, unsigned int Length);

// . RET 4. Digest is written at Context+0x58.
void __stdcall Md4Final(FMd4Context* Context);

// . RET 8. Straight-line MD4 block. Body runs through 0x06acf366.
void __stdcall Md4Transform(unsigned int* State, const unsigned int* Block);
