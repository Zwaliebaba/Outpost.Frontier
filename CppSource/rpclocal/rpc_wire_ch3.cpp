// Author: Giperion.

// rpc_wire_ch3.cpp -- the nine channel-3 packet codec slots that were still on the
// fallback path, plus the two channel-3 types whose payloads are near-twins.

// Every slot address below was read out of .rdata at the vtable the type's constructor
// publishes, and the constructor's LAST vtable store is the one that counts -- ch3 0x03
// MUNCOLLISION writes .rdata:the routine at 06da07a7 and only then the real
// .rdata:the routine at 06da07cc. Thirteen of the 162 constructors do that.

//   ch3 0x01 LOGIN            vtable 06db5ca4  enc the branch  dec the routine
//   ch3 0x03 MUNCOLLISION     vtable 06db5b54  enc the branch  dec the branch
//   ch3 0x0c OBJCOLLISION     vtable 06db5b74  enc the branch  dec the branch
//   ch3 0x18 REQADDITEM       vtable 06db4054  enc the branch  dec the branch
//   ch3 0x1a REQMODIFYITEM    vtable 06db4070  enc the branch  dec the branch
//   ch3 0x1e SAVEGAME         vtable 06db40a8  enc the branch  dec the branch
//   ch3 0x20 MINEASTEROID     vtable 06db5b84  enc the branch  dec the branch
//   ch3 0x31 REQUEST_EVENT    vtable 06db5dd4  enc the branch  dec the branch
//   ch3 0x39 CREATENEWCHAR    vtable 06db5c74  enc the branch  dec the branch

// ===========================================================================
// REQADDITEM AND REQMODIFYITEM ARE NOT THE SAME BODY -- and the way they are not is
// exactly the trap.

// Their four bodies measure 221 / 221 / 186 / 186 instructions, pair for pair, and every
// one of the 221 opcodes matches its opposite number. Diffing them position by position
// leaves nine differing positions in the encoders, of which two are the CALL rel32
// displacements to the shared helpers and one is the routing header, so SIX are real:

//   pos  ch3 0x18                                   ch3 0x1a
//    48  06d99e03 MOV ECX,dword ptr [EBP + 0x20]    06d9a293 MOV ECX,dword ptr [EBP + 0x28]
//    70  06d99e32 MOV EDX,dword ptr [EBP + 0x24]    06d9a2c2 MOV EDX,dword ptr [EBP + 0x20]
//   136  06d99ebc MOV EDX,dword ptr [EBP + 0x28]    06d9a34c MOV EDX,dword ptr [EBP + 0x24]
//   140  06d99ec6 MOV EAX,dword ptr [EBP + 0x28]    06d9a356 MOV EAX,dword ptr [EBP + 0x24]
//   160  06d99eef MOV EAX,dword ptr [EBP + 0x28]    06d9a37f MOV EAX,dword ptr [EBP + 0x24]

// So the three dwords at packet+0x20 / +0x24 / +0x28 go out in the order (0x20, 0x24,
// 0x28) for REQADDITEM and (0x28, 0x20, 0x24) for REQMODIFYITEM, and the field that
// doubles as the trailing block's length is packet+0x28 in one and packet+0x24 in the
// other. Serialising either in declaration order would put the wrong dword first and
// give the block the wrong length. The decoders carry the same permutation
// (06d99fee/06d9a016/06d9a08e against 06d9a47e/06d9a4a6/06d9a51e), so the two
// directions agree with each other and only the two TYPES differ.

// This is not a defect in baseline: FPayloadClientReqAddItem already declares
// HardpointSize at payload+0x08 and FPayloadClientReqModifyItem declares it at
// payload+0x04, from the SENDERS, which were read in an earlier wave. Two independent
// readings of the same permutation.

// ch2 0x55 OBJECTCARGOUPDATE and ch2 0x57 SET_MUSIC are the other equal-length pair
// (207 / 166 both ways); they are in rpc_wire_closeout.cpp and are genuinely different
// field lists, not a permutation.
// ===========================================================================

#include "rpc_containers.h"
#include "rpc_payloads.h"

#include <new>
#include <cstring>
#include <cwchar>

// wcslen / wcscpy spelled out the way the rest of this port spells them (see
// RpcCh3WideLength in rpc_payload_ch3.cpp): baseline reaches MSVCRT for them, and the
// loops below compute the same thing without dragging the _CRT_SECURE_NO_WARNINGS
// switch into the project.
static unsigned int RpcWideLength(const wchar_t* Text) {
    unsigned int Length = 0;
    if (Text == nullptr) {
        return 0;
    }
    while (Text[Length] != 0) {
        ++Length;
    }
    return Length;
}


// ===========================================================================
// ch3 0x01 FLPACKET_CLIENT_LOGIN -- the branch (220) / the routine (197).

// Wire: { byte CreateFlag, byte LenA, byte LenB, LenA wide chars, LenB wide chars }.

// THREE THINGS BASELINE DOES HERE THAT LOOK LIKE MISTAKES AND ARE NOT:

//  * the two lengths are wcslen TRUNCATED TO A BYTE and the NUL is NOT counted --
// `CALL wcslen` then 06dafb9d `MOV byte ptr [ESP + 0x1c],AL`, AL only. A
//    name of 256 wide characters therefore travels as length 0, i.e. as no name.
//  * the encoder LOWERCASES THE PACKET IN PLACE before sending: 06dafc19
//    `LEA ESI,[EBX + 0x20]` / 06dafc1d `CALL _wcslwr`. It is the account name, not a
//    copy of it, so a second Encode of the same packet sees the lowered text. Kept,
//    because a copy would change what a re-sent packet contains.
//  * only the account name is lowered. The password at packet+0x220 is not.

// The decoder zeroes BOTH buffers first (06dafd87..06dafd93, `MOV ECX,0x100 ; REP STOSD
// ; STOSW` from packet+0x20 -- 0x402 bytes) and NUL-terminates each string after the
// copy (06dafe60 and 06dafec4, `MOV word ptr [EBX + reg*0x2 + 0x20],0x0` and
// `... + 0x220],0x0`).
// ===========================================================================
unsigned int RpcEncodeClientLogin(FPacketBase& Packet, unsigned char** BufferHolder,
                                  unsigned int Capacity, unsigned int* OutLength) {
    FPayloadClientLogin* Wire =
        reinterpret_cast<FPayloadClientLogin*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));
    if (!Stream.PutByte(Wire->CreateFlag)) {                              // /06dafb92
        return Stream.GetError();
    }
    // / 06dafba8, both results kept in AL only.
    const unsigned char LenA =
        (unsigned char)(RpcWideLength(Wire->AccountName) & 0xffu);
    const unsigned char LenB =
        (unsigned char)(RpcWideLength(Wire->Password) & 0xffu);
    if (!Stream.PutByte(LenA)) {
        return Stream.GetError();
    }
    if (!Stream.PutByte(LenB)) {
        return Stream.GetError();
    }
    if (LenA != 0) {
        // MSVCRT `_wcslwr` through IAT module storage, on the packet itself.
        _wcslwr(Wire->AccountName);
        // `SHL EAX,0x1` -- the reservation and the copy are both 2*LenA.
        if (!Stream.PutBytes(Wire->AccountName, (unsigned int)LenA * 2u)) {
            return Stream.GetError();
        }
    }
    if (LenB != 0) {
        if (!Stream.PutBytes(Wire->Password, (unsigned int)LenB * 2u)) {
            return Stream.GetError();
        }
    }
    // the routine -- writes nothing.
    *OutLength = Stream.Length();
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

unsigned int RpcDecodeClientLogin(FPacketBase& Packet, const unsigned char* Buffer,
                                  unsigned int Size, unsigned int* OutLength) {
    FPayloadClientLogin* Wire =
        reinterpret_cast<FPayloadClientLogin*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    // 0x100 dwords plus one word from packet+0x20, i.e. 0x402 bytes
    // -- both wide buffers and the two bytes after them. The CreateFlag at payload+0x400
    // is inside that run and is overwritten by the read below.
    ::memset(Wire->AccountName, 0, 0x402u);
    if (!Stream.GetByte(Wire->CreateFlag)) {
        return Stream.GetError();
    }
    unsigned char LenA = 0;
    if (!Stream.GetByte(LenA)) {
        return Stream.GetError();
    }
    unsigned char LenB = 0;
    if (!Stream.GetByte(LenB)) {
        return Stream.GetError();
    }
    if (LenA != 0) {
        if (!Stream.GetBytes(Wire->AccountName, (unsigned int)LenA * 2u)) {
            return Stream.GetError();
        }
        Wire->AccountName[LenA] = L'\0';
    }
    if (LenB != 0) {
        if (!Stream.GetBytes(Wire->Password, (unsigned int)LenB * 2u)) {
            return Stream.GetError();
        }
        Wire->Password[LenB] = L'\0';
    }
    // the routine -- consumes nothing.
    *OutLength = Stream.Length();
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// ===========================================================================
// ch3 0x03 FLPACKET_CLIENT_MUNCOLLISION -- the branch (168) / the branch (158).
// Flat: dword, dword, dword, WORD, Vector. The word is the only 2-byte field and it
// leaves payload+0x0e as a hole neither side touches.
// ===========================================================================
unsigned int RpcEncodeClientMunCollision(FPacketBase& Packet, unsigned char** BufferHolder,
                                         unsigned int Capacity, unsigned int* OutLength) {
    const FPayloadClientMunCollision* Wire =
        reinterpret_cast<const FPayloadClientMunCollision*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));
    Stream.PutDword(Wire->Field_0x00);
    Stream.PutDword(Wire->Field_0x04);
    Stream.PutDword(Wire->ObjectId);
    Stream.PutWord(Wire->Field_0x0c);
    // reserves 0xc, 06dae39c..06dae3a4 load the three dwords into a stack temp
    // and 06dae3ba `CALL the routine` copies them out -- the three-dword copier, not a
    // memcpy of the packet.
    Stream.PutDword(*reinterpret_cast<const unsigned int*>(&Wire->Field_0x10));
    Stream.PutDword(*reinterpret_cast<const unsigned int*>(&Wire->Field_0x14));
    Stream.PutDword(*reinterpret_cast<const unsigned int*>(&Wire->Field_0x18));
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    // the routine.
    *OutLength = Stream.Length();
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

unsigned int RpcDecodeClientMunCollision(FPacketBase& Packet, const unsigned char* Buffer,
                                         unsigned int Size, unsigned int* OutLength) {
    FPayloadClientMunCollision* Wire =
        reinterpret_cast<FPayloadClientMunCollision*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.GetDword(Wire->Field_0x00);
    Stream.GetDword(Wire->Field_0x04);
    Stream.GetDword(Wire->ObjectId);
    Stream.GetWord(Wire->Field_0x0c);
    // reserves 0xc, 06dae543 `CALL the routine` with ECX = &packet+0x30.
    Stream.GetDword(*reinterpret_cast<unsigned int*>(&Wire->Field_0x10));
    Stream.GetDword(*reinterpret_cast<unsigned int*>(&Wire->Field_0x14));
    Stream.GetDword(*reinterpret_cast<unsigned int*>(&Wire->Field_0x18));
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// ===========================================================================
// ch3 0x0c FLPACKET_CLIENT_OBJCOLLISION -- the branch (186) / the branch (151).
// dword, WORD, dword, WORD, float. Both 2-byte fields leave a 2-byte hole behind them
// in the struct and neither hole reaches the wire.
// ===========================================================================
unsigned int RpcEncodeClientObjCollision(FPacketBase& Packet, unsigned char** BufferHolder,
                                         unsigned int Capacity, unsigned int* OutLength) {
    const FPayloadClientObjCollision* Wire =
        reinterpret_cast<const FPayloadClientObjCollision*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));
    Stream.PutDword(Wire->ObjectId);
    Stream.PutWord(Wire->SubObjId);
    Stream.PutDword(Wire->TargetObjectId);
    Stream.PutWord(Wire->TargetSubObjId);
    // FLD float ptr [EDI + 0x30] / 06dae8a6 FSTP float ptr [EAX].
    Stream.PutFloat(*reinterpret_cast<const float*>(&Wire->Field_0x10));
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

unsigned int RpcDecodeClientObjCollision(FPacketBase& Packet, const unsigned char* Buffer,
                                         unsigned int Size, unsigned int* OutLength) {
    FPayloadClientObjCollision* Wire =
        reinterpret_cast<FPayloadClientObjCollision*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.GetDword(Wire->ObjectId);
    Stream.GetWord(Wire->SubObjId);
    Stream.GetDword(Wire->TargetObjectId);
    Stream.GetWord(Wire->TargetSubObjId);
    // reads it as a plain dword, not through the x87 stack.
    Stream.GetDword(Wire->Field_0x10);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// ===========================================================================
// ch3 0x18 FLPACKET_CLIENT_REQADDITEM -- the branch (221) / the branch (186).
// Wire order: ArchId, Count, Health, bMounted, HardpointSize, HardpointSize bytes.
// See the permutation note at the head of this file before touching the order.
// ===========================================================================
unsigned int RpcEncodeClientReqAddItem(FPacketBase& Packet, unsigned char** BufferHolder,
                                       unsigned int Capacity, unsigned int* OutLength) {
    const FPayloadClientReqAddItem* Wire =
        reinterpret_cast<const FPayloadClientReqAddItem*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));
    Stream.PutDword(Wire->ArchId);                                        // packet+0x20
    Stream.PutDword((unsigned int)Wire->Count);                           // packet+0x24
    Stream.PutFloat(Wire->Health);                                        // packet+0x2c
    Stream.PutByte(Wire->bMounted);                                       // packet+0x30
    Stream.PutDword(Wire->HardpointSize);                                 // packet+0x28
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    // `TEST EAX,EAX ; 06d99ecb JBE` -- a zero length skips the block outright.
    if (Wire->HardpointSize != 0) {
        if (!Stream.PutBytes(Wire->Hardpoint, Wire->HardpointSize)) {
            return Stream.GetError();
        }
    }
    *OutLength = Stream.Length();
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

unsigned int RpcDecodeClientReqAddItem(FPacketBase& Packet, const unsigned char* Buffer,
                                       unsigned int Size, unsigned int* OutLength) {
    FPayloadClientReqAddItem* Wire =
        reinterpret_cast<FPayloadClientReqAddItem*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.GetDword(Wire->ArchId);
    Stream.GetDword(*reinterpret_cast<unsigned int*>(&Wire->Count));
    Stream.GetDword(*reinterpret_cast<unsigned int*>(&Wire->Health));
    Stream.GetByte(Wire->bMounted);
    Stream.GetDword(Wire->HardpointSize);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    // operator new(HardpointSize) -- baseline allocates unconditionally, before
    // the byte run's own bounds check, and the packet's destructor frees it.
    Wire->Hardpoint = static_cast<char*>(
        ::operator new(Wire->HardpointSize, std::nothrow));               // , bare
    if (!Stream.GetBytes(Wire->Hardpoint, Wire->HardpointSize)) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// ===========================================================================
// ch3 0x1a FLPACKET_CLIENT_REQMODIFYITEM -- the branch (221) / the branch (186).
// Same shape, DIFFERENT order: EquipId, Count, Health, bMounted, HardpointSize, block.
// ===========================================================================
unsigned int RpcEncodeClientReqModifyItem(FPacketBase& Packet, unsigned char** BufferHolder,
                                          unsigned int Capacity, unsigned int* OutLength) {
    const FPayloadClientReqModifyItem* Wire =
        reinterpret_cast<const FPayloadClientReqModifyItem*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));
    Stream.PutDword(Wire->EquipId);                                       // packet+0x28
    Stream.PutDword((unsigned int)Wire->Count);                           // packet+0x20
    Stream.PutFloat(Wire->Health);                                        // packet+0x2c
    Stream.PutByte(Wire->bMounted);                                       // packet+0x30
    Stream.PutDword(Wire->HardpointSize);                                 // packet+0x24
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    if (Wire->HardpointSize != 0) {
        if (!Stream.PutBytes(Wire->Hardpoint, Wire->HardpointSize)) {
            return Stream.GetError();
        }
    }
    *OutLength = Stream.Length();
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

unsigned int RpcDecodeClientReqModifyItem(FPacketBase& Packet, const unsigned char* Buffer,
                                          unsigned int Size, unsigned int* OutLength) {
    FPayloadClientReqModifyItem* Wire =
        reinterpret_cast<FPayloadClientReqModifyItem*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.GetDword(Wire->EquipId);
    Stream.GetDword(*reinterpret_cast<unsigned int*>(&Wire->Count));
    Stream.GetDword(*reinterpret_cast<unsigned int*>(&Wire->Health));
    Stream.GetByte(Wire->bMounted);
    Stream.GetDword(Wire->HardpointSize);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Wire->Hardpoint = static_cast<char*>(
        ::operator new(Wire->HardpointSize, std::nothrow));               // , bare
    if (!Stream.GetBytes(Wire->Hardpoint, Wire->HardpointSize)) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// ===========================================================================
// ch3 0x1e FLPACKET_CLIENT_SAVEGAME -- the branch (133) / the branch (128).

// { dword DescriptionCount, 2*DescriptionCount bytes }. The 0x200-byte CHARACTER_ID at
// payload+0x000 is NOT serialized by either side -- the encoder's first and only field
// read is 06d9a875 `MOV ECX,dword ptr [EBP + 0x224]`, payload+0x204. That is baseline's
// wire format, not an omission here.

// The count is in WIDE CHARACTERS: 06d9a891 `LEA EDX,[EAX + EAX*0x1]` reserves 2*n on
// the encode side and 06d9a8c6 `SHL EAX,0x1` is the copy length; the decoder does the
// same twice, 06d9a9c9 for the operator new() argument and 06d9a9e3 for the reservation.
// ===========================================================================
unsigned int RpcEncodeClientSaveGame(FPacketBase& Packet, unsigned char** BufferHolder,
                                     unsigned int Capacity, unsigned int* OutLength) {
    const FPayloadClientSaveGame* Wire =
        reinterpret_cast<const FPayloadClientSaveGame*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));
    if (!Stream.PutDword(Wire->DescriptionCount)) {
        return Stream.GetError();
    }
    if (Wire->DescriptionCount != 0) {
        if (!Stream.PutBytes(Wire->Description,
                             Wire->DescriptionCount * 2u)) {
            return Stream.GetError();
        }
    }
    *OutLength = Stream.Length();
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

unsigned int RpcDecodeClientSaveGame(FPacketBase& Packet, const unsigned char* Buffer,
                                     unsigned int Size, unsigned int* OutLength) {
    FPayloadClientSaveGame* Wire =
        reinterpret_cast<FPayloadClientSaveGame*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    if (!Stream.GetDword(Wire->DescriptionCount)) {
        return Stream.GetError();
    }
    if (Wire->DescriptionCount != 0) {
        Wire->Description = static_cast<wchar_t*>(
            ::operator new(Wire->DescriptionCount * 2u, std::nothrow));   // , bare
        if (!Stream.GetBytes(Wire->Description,
                             Wire->DescriptionCount * 2u)) {
            return Stream.GetError();
        }
    }
    *OutLength = Stream.Length();
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// ===========================================================================
// ch3 0x20 FLPACKET_CLIENT_MINEASTEROID -- the branch (202) / the branch (160).
// dword, Vector, dword, dword, dword.
// ===========================================================================
unsigned int RpcEncodeClientMineAsteroid(FPacketBase& Packet, unsigned char** BufferHolder,
                                         unsigned int Capacity, unsigned int* OutLength) {
    const FPayloadClientMineAsteroid* Wire =
        reinterpret_cast<const FPayloadClientMineAsteroid*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));
    Stream.PutDword(Wire->SystemId);
    // reserves 0xc; 06daeb75..06daeb7d load the three floats into a stack temp
    // in the order (0x24, 0x2c, 0x28) -- the LOAD order, which is the compiler's, not
    // the wire order. the routine at 06daeb93 then copies the temp out in index order.
    Stream.PutFloat(Wire->PositionX);
    Stream.PutFloat(Wire->PositionY);
    Stream.PutFloat(Wire->PositionZ);
    Stream.PutDword(Wire->LootId);
    Stream.PutDword(Wire->SecondaryLoot);
    Stream.PutDword(Wire->Quantity);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

unsigned int RpcDecodeClientMineAsteroid(FPacketBase& Packet, const unsigned char* Buffer,
                                         unsigned int Size, unsigned int* OutLength) {
    FPayloadClientMineAsteroid* Wire =
        reinterpret_cast<FPayloadClientMineAsteroid*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.GetDword(Wire->SystemId);
    // reserves 0xc, 06daed3a `CALL the routine` with ECX = &packet+0x24.
    Stream.GetDword(*reinterpret_cast<unsigned int*>(&Wire->PositionX));
    Stream.GetDword(*reinterpret_cast<unsigned int*>(&Wire->PositionY));
    Stream.GetDword(*reinterpret_cast<unsigned int*>(&Wire->PositionZ));
    Stream.GetDword(Wire->LootId);
    Stream.GetDword(Wire->SecondaryLoot);
    Stream.GetDword(Wire->Quantity);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// ===========================================================================
// ch3 0x31 FLPACKET_CLIENT_REQUEST_EVENT -- the branch (185) / the branch (151).
// byte, dword, dword, dword, byte.
// ===========================================================================
unsigned int RpcEncodeClientRequestEvent(FPacketBase& Packet, unsigned char** BufferHolder,
                                         unsigned int Capacity, unsigned int* OutLength) {
    const FPayloadClientRequestEvent* Wire =
        reinterpret_cast<const FPayloadClientRequestEvent*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));
    Stream.PutByte(Wire->EventType);
    Stream.PutDword(Wire->Field_0x04);
    Stream.PutDword(Wire->Field_0x08);
    Stream.PutDword(Wire->Field_0x0c);
    Stream.PutByte(Wire->Field_0x10);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

unsigned int RpcDecodeClientRequestEvent(FPacketBase& Packet, const unsigned char* Buffer,
                                         unsigned int Size, unsigned int* OutLength) {
    FPayloadClientRequestEvent* Wire =
        reinterpret_cast<FPayloadClientRequestEvent*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.GetByte(Wire->EventType);
    Stream.GetDword(Wire->Field_0x04);
    Stream.GetDword(Wire->Field_0x08);
    Stream.GetDword(Wire->Field_0x0c);
    Stream.GetByte(Wire->Field_0x10);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// ===========================================================================
// ch3 0x39 FLPACKET_CLIENT_CREATENEWCHAR -- the branch (180) / the branch (163).

// { word NameLength, NameLength wide chars, dword, dword, dword, dword }, and the two
// string halves are the only two callers of the routine / the routine in the module.
// Those two are NOT the {dword size, bytes} pair the routine handles; they are their
// own primitive and they differ from it in three ways worth spelling out:

//  * the length is a WORD, not a dword -- 06da1526 `MOV word ptr [EBX],AX`;
//  * the length is wcslen WITHOUT the NUL, and the decoder re-terminates instead,
// `MOV word ptr [EBX + ECX*0x1],0x0` at offset 2*len of a 2*len+2 block;
//  * the bounds test is `JC`, not `JBE` -- 06da150d / 06da153e / 06da159d / 06da15c0.
//    Cursor == Limit is an ERROR for these two where it is fine everywhere else, so a
//    string that exactly fills the buffer is rejected. Reproduced with Reserve() plus an
//    explicit equality test rather than by reusing Put/GetBytes.

// One more asymmetry, left as baseline has it: when the length is zero the routine skips
// and never writes the advanced buffer pointer back (06da1612 is inside
// the skipped block), so the two length bytes it consumed from the cursor are not
// consumed from the pointer. The decoder below reproduces that exactly; see
// implementation notes.
// ===========================================================================
unsigned int RpcEncodeClientCreateNewChar(FPacketBase& Packet, unsigned char** BufferHolder,
                                          unsigned int Capacity, unsigned int* OutLength) {
    const FPayloadClientCreateNewChar* Wire =
        reinterpret_cast<const FPayloadClientCreateNewChar*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));
    if (!Stream.PutWideString(Wire->Name)) {                              // the routine
        return FRpcMarshalStream::kEncodeOverflow;
    }
    Stream.PutDword(Wire->Field_0x80);
    Stream.PutDword(Wire->Field_0x84);
    Stream.PutDword(Wire->Field_0x88);
    Stream.PutDword(Wire->Field_0x8c);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

unsigned int RpcDecodeClientCreateNewChar(FPacketBase& Packet, const unsigned char* Buffer,
                                          unsigned int Size, unsigned int* OutLength) {
    FPayloadClientCreateNewChar* Wire =
        reinterpret_cast<FPayloadClientCreateNewChar*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    wchar_t* Name = nullptr;
    if (!Stream.GetWideString(Name)) {                                    // the routine
        return FRpcMarshalStream::kDecodeOverrun;
    }
    // `MOV EAX,dword ptr [ESP + 0x18]` / 06daf618 `TEST EAX,EAX` / 06daf61a
    // `JZ the branch` -- baseline DOES guard the wcscpy against the null an empty string
    // leaves behind, so the destination keeps whatever it held.
    if (Name != nullptr) {
        // CALL MSVCRT wcscpy, unbounded, as baseline's: the wire length is a word,
        // so a name longer than the 0x40-wchar_t field runs over Field_0x80..Field_0x8c
        // (which the reads below then overwrite) and on past the end of the packet.
        wcscpy(Wire->Name, Name);
        // CALL the routine -- the CRT free of the temporary the routine
        // allocated with operator new.
        ::operator delete(Name);
    }
    Stream.GetDword(Wire->Field_0x80);
    Stream.GetDword(Wire->Field_0x84);
    Stream.GetDword(Wire->Field_0x88);
    Stream.GetDword(Wire->Field_0x8c);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}
