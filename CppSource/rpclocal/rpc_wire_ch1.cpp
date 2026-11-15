// Author: Giperion.

// rpc_wire_ch1.cpp -- baseline channel-1 packet codec slots (vtable +0x04 Encode,
// +0x08 Decode), transcribed from rpclocal.

// As with rpc_wire_ch2.cpp, analysis produced no analysis for the encoder: it is an
// anonymous LAB_ block reached only from an .rdata vtable slot. The decoder DOES have a
// inspected body (rpclocal..26144) but it is read here off the listing
// anyway, because the .c loses which of the four quantised bytes lands in which
// quaternion component -- the thing most worth getting right in this file.

// Which packet class owns the pair, by the constructor recipe at the head of
// rpc_wire_ch2.cpp -- the routing header written right after the vptr:

//   ch1 0x01 FLPACKET_COMMON_UPDATEOBJECT, constructor the routine
// MOV dword ptr [EAX],.rdata:the routine
// MOV dword ptr [EAX + 0xc],0x10001
//     vtable 06db4378 = { dtor the routine, Encode the branch,
//                         Decode the routine, dump (IServerImpl::GetServerStats) }
//   and the two slots' own XREF headers agree: the branch is referenced from
// (== 06db4378 + 4) and the routine from 06db4380 (+ 8).

// Re-running that whole scan over the listing still yields exactly 162 constructor
// pairs, one per registry entry, so this attribution is not a guess.

// THE CONSTRUCTOR ALSO SETTLES THE QUATERNION, INDEPENDENTLY OF THE CODEC. 06d9af2d
// writes `MOV dword ptr [EAX + 0x34],0x3f800000` -- 1.0f -- and leaves packet+0x38,
// +0x3c and +0x40 at zero (06d9af34 / 06d9af37 / 06d9af3a). That is the identity
// quaternion seeded W-first at payload+0x14, so payload+0x14 is W and +0x18..+0x20 are
// X, Y, Z. The wire order derived below from the stores alone therefore really does put
// W last, and this is a second witness that owes nothing to the codec.

// THIS IS THE HOTTEST PACKET IN THE GAME -- one per moving object per tick -- and its
// codec is where the wire format earns its keep: a 0x2c-byte payload leaves as 26 bytes
// of body plus the 2 header bytes, because the orientation quaternion and one more
// float are quantised to a single byte each.

// Wire layout, with the listing address of each store (encode) and load (decode):

//     payload+0x00  byte                                    06da4a4b / 06da4d0c
//     payload+0x04  dword  ObjectId                         06da4a75 / 06da4d38
//     payload+0x08  three dwords (the routine) Position    06da4aba / 06da4d5f
//     payload+0x18  Orientation.x  quantised byte           06da4b31 / 06da4e22
//     payload+0x1c  Orientation.y  quantised byte           06da4b58 / 06da4e33
//     payload+0x20  Orientation.z  quantised byte           06da4b7f / 06da4e44
//     payload+0x14  Orientation.w  quantised byte           06da4ba2 / 06da4e51
//     payload+0x28  Field_0x28     quantised byte           06da4be4 / 06da4e88
//     payload+0x24  Field_0x24     raw float                06da4c1d / 06da4ea5

// THE QUATERNION TRAVELS x,y,z,w -- W LAST, AND THE TWO ENDS AGREE. On the encode side
// the four ftol results are parked in four different stack slots before any of them is
// emitted (06da4adc [ESP+0x28], 06da4aee [ESP+0x24], 06da4b00 [ESP+0x13], and the last
// one stays in DL), so the order they are COMPUTED in is not the order they are WRITTEN
// in; the writes are what count. On the decode side the four bytes are read into the
// same kind of rotating temp and land at +0x38, +0x3c, +0x40, +0x34 in that order
// (06da4e22 / 06da4e33 / 06da4e44 / 06da4e51). Since Quaternion is {w,x,y,z}, packet
// +0x34 is w and +0x38..+0x40 are x,y,z: both directions put w last.

// The scale is 127.0 (.rdata:the field, bytes 00 00 00 00 00 c0 5f 40) on the way
// out and 1/127 (.rdata:the field) on the way back, and every byte is read back
// SIGN-extended -- 06da4e00 `MOVSX EAX,byte ptr [ESP+0x18]`, 06da4e09, 06da4e1c
// `MOVSX EDX,DL`, 06da4e1f `MOVSX EAX,CL`, and 06da4e74 `MOVSX ECX,CL` for the fifth
// byte. That is the [-1,+1] tier, not the [0,1] percentage tier, for all five.

// Field_0x24 is NOT quantised: 06da4c1a `FLD float ptr [EDI+0x44]` / 06da4c1d
// `FSTP float ptr [EAX]` moves it as four raw bytes, and the decoder takes it back as a
// plain dword (06da4ea7 `MOV EDX,[EAX]`). Quantising it by analogy with its neighbour
// would have been an invention -- there is no FMUL anywhere near it.

// Both slots end in the byte-statistics tail every codec in the module shares
// (06da4c48..06da4c85 encode, 06da4edc..06da4f15 decode); see FPacketWireStats.

// Single-player: in SP a packet is queued by pointer and neither slot is ever called,
// so nothing here changes the SP path.

#include "rpc_containers.h"
#include "rpc_payloads.h"

#include <new>

// ===========================================================================
// ch1 0x01 FLPACKET_COMMON_UPDATEOBJECT -- Encode the branch, Decode the routine
// ===========================================================================

unsigned int RpcEncodeCommonUpdateObject(FPacketBase& Packet, unsigned char** BufferHolder,
                                         unsigned int Capacity, unsigned int* OutLength) {
    const FPayloadCommonUpdateObject* Wire =
        reinterpret_cast<const FPayloadCommonUpdateObject*>(Packet.Payload());

    // `LEA ESI,[EDI+0x10]` then +0x04 = the capacity argument, +0x08 = the
    // DEREFERENCED buffer-pointer argument (06da49f7 `MOV EDX,[ECX]`), +0x00 = 0,
    // +0x0c = 0. Exactly the constructor below.
    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));   // the routine

    Stream.PutByte(Wire->Field_0x00);
    Stream.PutDword(Wire->ObjectId);
    // the routine moves three dwords out of a stack copy of packet+0x28..+0x30
    // (06da4aa7 / 06da4aaf / 06da4ab6 -- the last of those is written AFTER a PUSH, so
    // its [ESP+0x20] is the same slot as [ESP+0x1c] before it; the three are contiguous).
    Stream.PutBytes(&Wire->Position, 12u);                         // bumps by 0xc

    Stream.PutQuantized(Wire->Orientation.x, FRpcMarshalStream::kOrientationScale);
    Stream.PutQuantized(Wire->Orientation.y, FRpcMarshalStream::kOrientationScale);
    Stream.PutQuantized(Wire->Orientation.z, FRpcMarshalStream::kOrientationScale);
    Stream.PutQuantized(Wire->Orientation.w, FRpcMarshalStream::kOrientationScale);
    Stream.PutQuantized(Wire->Field_0x28, FRpcMarshalStream::kOrientationScale);
    Stream.PutFloat(Wire->Field_0x24);                             // FSTP

    // the routine -- `XOR EAX,EAX ; RET 0xc`, emits nothing.
    if (Stream.Failed()) {
        // / 06da4c08 / 06da4c39 / the branch all return 0x16 WITHOUT writing
        // the out-length, so a short buffer leaves the caller's length alone.
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

unsigned int RpcDecodeCommonUpdateObject(FPacketBase& Packet, const unsigned char* Buffer,
                                         unsigned int Size, unsigned int* OutLength) {
    FPayloadCommonUpdateObject* Wire =
        reinterpret_cast<FPayloadCommonUpdateObject*>(Packet.Payload());

    // The asymmetry the ch2 file already established, re-checked here: 06da4cab
    // `MOV ECX,[ESP+param_1+0x14]` then 06da4cb4 `MOV [ESI+8],ECX` -- arg1 is the buffer
    // pointer ITSELF, with no second dereference, where the encoder's arg1 is a pointer
    // to it.
    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    // the routine at the START of the decoder: consumes nothing. The two
    // header bytes were eaten by the dispatcher, which is why this side's length
    // excludes them.
    if (Stream.Failed()) {
        return Stream.GetError();
    }

    unsigned char Small8 = 0;
    unsigned int Scratch = 0;
    if (!Stream.GetByte(Small8)) { return Stream.GetError(); }
    Wire->Field_0x00 = Small8;
    if (!Stream.GetDword(Scratch)) { return Stream.GetError(); }
    Wire->ObjectId = Scratch;
    if (!Stream.GetBytes(&Wire->Position, 12u)) { return Stream.GetError(); }

    if (!Stream.GetQuantizedSigned(Wire->Orientation.x,
                                   FRpcMarshalStream::kOrientationInvScale)) {
        return Stream.GetError();
    }
    if (!Stream.GetQuantizedSigned(Wire->Orientation.y,
                                   FRpcMarshalStream::kOrientationInvScale)) {
        return Stream.GetError();
    }
    if (!Stream.GetQuantizedSigned(Wire->Orientation.z,
                                   FRpcMarshalStream::kOrientationInvScale)) {
        return Stream.GetError();
    }
    if (!Stream.GetQuantizedSigned(Wire->Orientation.w,
                                   FRpcMarshalStream::kOrientationInvScale)) {
        return Stream.GetError();
    }
    if (!Stream.GetQuantizedSigned(Wire->Field_0x28,
                                   FRpcMarshalStream::kOrientationInvScale)) {
        return Stream.GetError();
    }
    // `MOV EDX,[EAX]` -- a plain dword, no FILD and no scale.
    if (!Stream.GetFloat(Wire->Field_0x24)) { return Stream.GetError(); }

    // the routine -- consumes nothing.
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// ===========================================================================
// ch1 0x1d FLPACKET_COMMON_TRACTOROBJECTS -- the branch / the branch

// Constructor the routine:
// MOV dword ptr [EAX],.rdata:the routine
// MOV dword ptr [EAX + 0xc],0x1001d
//   vtable 06db44b8 = { dtor the routine, Encode the branch, Decode the branch,
//                       IServerImpl::GetServerStats }
// and each slot's XREF header names exactly one .rdata address, 06db44bc and 06db44c0,
// which is 06db44b8 + 4 and + 8. One type, one pair; nothing shared here.

// THE CONSTRUCTOR ALSO SETTLES THE PAYLOAD, WITHOUT THE CODEC. 06d9bbd9 zeroes
// packet+0x20, 06d9bbdc zeroes the WORD at +0x24, 06d9bbe0 stores the byte in DL at
// +0x28 -- that is the one-byte VC6 std::allocator copied from the caller's
// uninitialised temporary, the trap this module is full of, NOT a field -- and
// /e6/e9 zero the three dwords at +0x2c, +0x30 and +0x34. So the payload is
// { dword, word, pad, { allocator, First, Last, End } }, which is the
// FPayloadCommonTractorObjects rpc_payloads.h already had. The codec agrees with it and
// changes nothing about it.

// Wire layout:
//     dword ObjectId          06da7a71..06da7a7b / 06da7c02..06da7c0b   packet+0x20
//     word  TractorEquipId    06da7aa0..06da7aac / 06da7c2c..06da7c36   packet+0x24
//     word  Count             06da7ae2..06da7ae7 / 06da7c58..06da7c64
//     Count * dword           06da7b0c..06da7b1f / 06da7cc5..06da7cd0

// THE COUNT IS NOT THE VECTOR'S SIZE FIELD -- there is no size field. 06da7ab1 tests
// First for null and 06da7ab9/06da7abe compute `(Last - First) >> 2`, the VC6
// end-minus-begin, as this port's FbaselineVector::Count() does. The ELEMENT LOOP does not
// use that number at all: 06da7aef compares First against Last directly and 06da7b1d
// walks to Last. Two different expressions of the same length, and only the first of
// them goes on the wire.

// THE DECODER'S 148-INSTRUCTION MIDDLE IS ONE INLINED std::vector::insert(end, 1, v).
// compute `(End - Last) >> 2` and take the fast path when there is
// room; the slow path at 06da7ced..06da7db5 is the growth, and its policy is exact:
//   * `TEST EDX,EDX` on First -- an empty vector grows by 1;
//   * /06da7cfb `(Last - First) >> 2` compared `JA 1` -- otherwise it grows by
//     the current size, but never by less than 1;
//   * adds the two, 06da7d1a clamps a negative to zero, and 06da7d1e
//     `LEA EDX,[EAX*0x4]` is the byte count handed to operator new.
// so the new capacity is `Size + max(Size, 1)`. Then the routine (_Ucopy) moves the old
// elements, the routine (_Ufill) writes the new one, the routine destroys and
// frees the old block. That is reproduced below rather than replaced by a
// doubling of our own: Cap is a field that goes back out on the next encode's `Last`
// arithmetic, and a different capacity policy would be a member baseline does not have.

// THE DECODER APPENDS. It never clears the vector the packet already holds -- the same
// behaviour as the equipment decoder, and equally deliberate: 06da7cc5 walks straight
// into the insert with whatever First/Last were.

// Single-player: SP queues packets by pointer and calls neither slot.
// ===========================================================================

// is VC6 vector<uint>::insert(end, 1, Value), capacity and all:
// RpcbaselineVectorAppend in rpc_containers.h, shared with the FIREWEAPON decoder
// (06da5945..06da5a1f, the same inlined routine over unsigned short). Its operator new
// is bare, as baseline's is (06da7d1e).

// Encode.
unsigned int RpcEncodeCommonTractorObjects(FPacketBase& Packet, unsigned char** BufferHolder,
                                           unsigned int Capacity, unsigned int* OutLength) {
    const FPayloadCommonTractorObjects* Wire =
        reinterpret_cast<const FPayloadCommonTractorObjects*>(Packet.Payload());

    // /06da7a16: Limit = arg2, Ptr = *arg1.
    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));   // the routine
    if (!Stream.PutDword(Wire->ObjectId)) {
        return FRpcMarshalStream::kEncodeOverflow;
    }
    if (!Stream.PutWord(Wire->TractorEquipId)) {
        return FRpcMarshalStream::kEncodeOverflow;
    }
    // null First means zero, otherwise (Last - First) >> 2.
    const unsigned int Count = Wire->TargetIds.Count();
    if (!Stream.PutWord((unsigned short)(Count & 0xffffu))) {
        return FRpcMarshalStream::kEncodeOverflow;
    }
    // the loop walks First..Last, NOT the count just written.
    for (const unsigned int* It = Wire->TargetIds.Begin;
         It != Wire->TargetIds.End; ++It) {
        if (!Stream.PutDword(*It)) {
            return FRpcMarshalStream::kEncodeOverflow;
        }
    }
    // the routine -- nothing more to emit.
    *OutLength = Stream.Length();                              // onwards
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// Decode.
unsigned int RpcDecodeCommonTractorObjects(FPacketBase& Packet, const unsigned char* Buffer,
                                           unsigned int Size, unsigned int* OutLength) {
    FPayloadCommonTractorObjects* Wire =
        reinterpret_cast<FPayloadCommonTractorObjects*>(Packet.Payload());

    // /06da7bb1: Limit = arg2, Ptr = arg1 directly.
    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    // the routine -- no header consumed.

    if (!Stream.GetDword(Wire->ObjectId)) {
        return Stream.GetError();
    }
    if (!Stream.GetWord(Wire->TractorEquipId)) {
        return Stream.GetError();
    }
    unsigned short Count = 0;
    if (!Stream.GetWord(Count)) {
        return Stream.GetError();
    }
    // /06da7c77 `CMP EAX,ESI ; JG` -- the zero-extended word is compared SIGNED
    // against zero, so a count of 0 skips straight to the tail.
    for (unsigned int Index = 0; Index < (unsigned int)Count; ++Index) {
        unsigned int Value = 0;
        if (!Stream.GetDword(Value)) {
            return Stream.GetError();
        }
        RpcbaselineVectorAppend(Wire->TargetIds, Value);
    }
    // the routine again -- nothing to consume.
    *OutLength = Stream.Length();                              // onwards
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}
