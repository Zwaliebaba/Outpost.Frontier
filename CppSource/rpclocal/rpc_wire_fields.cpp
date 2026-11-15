// Author: Giperion.

// rpc_wire_fields.cpp -- the PLAIN FIELD-LIST packet codec slots.

// Seventeen more of baseline's Encode/Decode vtable pairs, covering twenty-three of the 162
// registered packet types. Every body in this file is the same skeleton -- routing
// header, then a fixed list of fixed-width fields, then the statistics tail -- and
// differs from its neighbours only in the WIDTH and the ORDER of that list. None of them
// allocates, none of them calls out, and none of them has a conditional branch beyond
// the per-field bounds check.

// HOW EACH BODY WAS TIED TO ITS TYPES. Not by the XREF header, which under-reports (see
// rpc_wire_generic.cpp), and not by hand either: the mapping was rebuilt mechanically
// for ALL 162 types at once and then checked against the port. For each registry row the
// factory's constructor (the address already recorded per line in rpc_packet_registry.cpp)
// was scanned for its `MOV dword ptr [reg],<vtable>`, and the four .rdata dwords at that
// vtable were read out. Slot +0x04 is the encoder and slot +0x08 the decoder in every
// one of the 162 cases.

// THE OTHER TWO SLOTS WERE DESCRIBED WRONG HERE and are corrected 2026-09-17 by counting
// them instead of sampling them. It used to say slot +0x00 "is the type's deleting
// destructor" and slot +0x0c "is the same shared address for all of them". Both halves
// overstate:
//   * slot +0x00 takes only 51 distinct values over the 162 types. ONE address,
// , fills 111 of them -- it is the MSVC scalar-deleting-destructor
//     shape (06d9af53 the base dtor, 06d9af58 `TEST byte ptr [ESP + 0x4],0x1`,
// the delete), and it is shared precisely because those 111 types have
//     nothing of their own to destroy. The 50 that do get their own, e.g. the routine
//     for the two COLLISIONGROUPS types, whose 06da1133 calls the routine to tear the
//     std::list down first;
//   * slot +0x0c takes 12 distinct values, not one. 150 types share 06d8eae0, which
//     analysis labels IServerImpl::GetServerStats and which is in fact the entire body
//     `RET 0x4` -- a no-op dump that the linker folded onto an unrelated symbol, so the
//     NAME says nothing about the owner. The other 12 have a real ostream dump, e.g.
// for ch3 0x06 / 0x3a, whose 06dafac4 pushes the literal
//     "   Character: ".

// That table also RE-VERIFIED the 110 codec rows this port already had: every existing
// (channel, opcode) -> port codec assignment maps onto exactly one baseline body, and
// every baseline body onto exactly one port codec, in both directions and with no
// collisions. The earlier waves' constructor-scan attribution is therefore confirmed by
// an independent exhaustive pass, not merely left uncontradicted.

// NEAR-IDENTICAL IS NOT IDENTICAL, and this file is the proof. Seven separate baseline
// bodies here are 133 instructions long; the linker did NOT fold them, and diffing them
// instruction by instruction against the branch shows why -- each differs in exactly
// the field widths and the two or three jump displacements those widths move:
// +0x81 `ADD EDX,0x4` where 06dae620 has `ADD EDX,0x2`, and +0xa7
//                 `MOV EDX,dword ptr [EDI + 0x24]` where 06dae620 has `MOV DX,word ptr`
// +0xde `MOV CX,word ptr [EDI + 0x26]` for a dword at +0x28
// +0xa7 reads +0x26 and +0xdc reads +0x24 -- the two fields SWAP
// +0xa5 `MOV DL,byte ptr [EDI + 0x24]`
// +0xa5 `MOV DL,byte ptr [EDI + 0x28]` then +0xd8 the dword at +0x24 --
//                 again a swap, and a different one from 06da67a0's
// +0xda `MOV CL,byte ptr [EDI + 0x28]`
// Two of the seven reorder their fields on the wire relative to the payload struct. A
// codec written from the struct's declaration order would have been byte-wrong for
// exactly those two and byte-right for the other five, which is the failure mode this
// per-body diff exists to catch.

// A LISTING TRAP MET WHILE COUNTING. Walking from a LAB_ address until the first `??`
// padding byte over-runs when the NEXT function starts with no padding in front of it:
// reads as 136 instructions that way, and the last three are
// MOV EAX,[.rdata:the field]
// MOV [.data:the field],EAX
// RET
// which is the branch, an unrelated float-constant initializer that happens to sit
// immediately after 06db1b60's `RET 0xc` at 06db1c9d. The body is 133 instructions, the
// same as its six siblings, and the tracker's own insn_count agrees.

// THE ROUTING HEADER. Every encoder here calls the routine, which cannot fail -- see
// the derivation on FRpcMarshalStream::PutHeaderUnchecked. The `if (result) return 0x16`
// branch each of them carries after that call is dead code in baseline, and the port
// writes the header through PutHeaderUnchecked with no check after it, as baseline does.

// Single-player: in SP a packet is queued by pointer and neither slot is ever called.
// Nothing in this file touches the SP path.

#include "rpc_containers.h"
#include "rpc_payloads.h"

// ===========================================================================
// dword, word, dword -- the branch / the branch.
//   ch1 0x14 FLPACKET_COMMON_JETTISONCARGO        (FPayloadCommonJettisonCargo)
//   ch3 0x3d FLPACKET_CLIENT_BADLANDSOBJCOLLISION (FPayloadClientBadLandsObjCollision)
// The two payload structs are declared separately in rpc_payloads.h because their FIELDS
// mean different things; their LAYOUT is the same three slots, which is why one baseline
// body serves both. The marshalling here goes through the ch1 struct.
// ===========================================================================
unsigned int RpcEncodeWireDwordWordDword(FPacketBase& Packet, unsigned char** BufferHolder,
                                         unsigned int Capacity, unsigned int* OutLength) {
    const FPayloadCommonJettisonCargo* Wire =
        reinterpret_cast<const FPayloadCommonJettisonCargo*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));
    Stream.PutDword(Wire->ObjectId);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.PutWord(Wire->ItemId);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.PutDword(Wire->Count);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

unsigned int RpcDecodeWireDwordWordDword(FPacketBase& Packet, const unsigned char* Buffer,
                                         unsigned int Size, unsigned int* OutLength) {
    FPayloadCommonJettisonCargo* Wire =
        reinterpret_cast<FPayloadCommonJettisonCargo*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    unsigned int ScratchDword = 0;
    unsigned short ScratchWord = 0;
    if (!Stream.GetDword(ScratchDword)) {
        return Stream.GetError();
    }
    Wire->ObjectId = ScratchDword;
    if (!Stream.GetWord(ScratchWord)) {
        return Stream.GetError();
    }
    Wire->ItemId = ScratchWord;
    if (!Stream.GetDword(ScratchDword)) {
        return Stream.GetError();
    }
    Wire->Count = ScratchDword;
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// ===========================================================================
// dword, dword, word -- the branch / the branch.
//   ch2 0x33 FLPACKET_SERVER_SETMISSIONOBJECTIVESTATE (FPayloadServerSetMissionObjectiveState)
//   ch2 0x4a FLPACKET_SERVER_SETGUIDEDTARGET          (FPayloadServerSetGuidedTarget)
// ===========================================================================
unsigned int RpcEncodeWireDwordDwordWord(FPacketBase& Packet, unsigned char** BufferHolder,
                                         unsigned int Capacity, unsigned int* OutLength) {
    const FPayloadServerSetMissionObjectiveState* Wire =
        reinterpret_cast<const FPayloadServerSetMissionObjectiveState*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));
    Stream.PutDword(Wire->Field_0x00);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.PutDword(Wire->Field_0x04);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.PutWord(Wire->Field_0x08);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

unsigned int RpcDecodeWireDwordDwordWord(FPacketBase& Packet, const unsigned char* Buffer,
                                         unsigned int Size, unsigned int* OutLength) {
    FPayloadServerSetMissionObjectiveState* Wire =
        reinterpret_cast<FPayloadServerSetMissionObjectiveState*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    unsigned int ScratchDword = 0;
    unsigned short ScratchWord = 0;
    if (!Stream.GetDword(ScratchDword)) {
        return Stream.GetError();
    }
    Wire->Field_0x00 = ScratchDword;
    if (!Stream.GetDword(ScratchDword)) {
        return Stream.GetError();
    }
    Wire->Field_0x04 = ScratchDword;
    if (!Stream.GetWord(ScratchWord)) {
        return Stream.GetError();
    }
    Wire->Field_0x08 = ScratchWord;
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// ===========================================================================
// dword, word, word -- the branch / the branch.
//   ch2 0x51 FLPACKET_SERVER_USE_ITEM         (FPayloadServerUseItem)
//   ch3 0x40 FLPACKET_CLIENT_REQUEST_USE_ITEM (FPayloadClientUseItem)
// ===========================================================================
unsigned int RpcEncodeWireDwordWordWord(FPacketBase& Packet, unsigned char** BufferHolder,
                                        unsigned int Capacity, unsigned int* OutLength) {
    const FPayloadServerUseItem* Wire =
        reinterpret_cast<const FPayloadServerUseItem*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));
    Stream.PutDword(Wire->Field_0x00);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.PutWord(Wire->Field_0x04);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.PutWord(Wire->Field_0x06);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

unsigned int RpcDecodeWireDwordWordWord(FPacketBase& Packet, const unsigned char* Buffer,
                                        unsigned int Size, unsigned int* OutLength) {
    FPayloadServerUseItem* Wire =
        reinterpret_cast<FPayloadServerUseItem*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    unsigned int ScratchDword = 0;
    unsigned short ScratchWord = 0;
    if (!Stream.GetDword(ScratchDword)) {
        return Stream.GetError();
    }
    Wire->Field_0x00 = ScratchDword;
    if (!Stream.GetWord(ScratchWord)) {
        return Stream.GetError();
    }
    Wire->Field_0x04 = ScratchWord;
    if (!Stream.GetWord(ScratchWord)) {
        return Stream.GetError();
    }
    Wire->Field_0x06 = ScratchWord;
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// ===========================================================================
// dword, dword, word, word -- the branch / the branch.
//   ch1 0x04 FLPACKET_COMMON_SETTARGET     (FPayloadCommonSetTarget)
//   ch2 0x38 FLPACKET_SERVER_ITEMTRACTORED (FPayloadServerItemTractored)
// The pair of 32-bit ids leads and the pair of 16-bit sub-object ids follows, which is
// the reorder FPayloadCommonSetTarget's comment already records against XSetTarget --
// confirmed here from the wire side at 06da8bc1 / 06da8bf0 / 06da8c1f / 06da8c50.
// ===========================================================================
unsigned int RpcEncodeWireDwordDwordWordWord(FPacketBase& Packet, unsigned char** BufferHolder,
                                             unsigned int Capacity, unsigned int* OutLength) {
    const FPayloadCommonSetTarget* Wire =
        reinterpret_cast<const FPayloadCommonSetTarget*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));
    Stream.PutDword(Wire->ObjectId);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.PutDword(Wire->TargetObjectId);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.PutWord(Wire->SubObjId);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.PutWord(Wire->TargetSubObjId);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

unsigned int RpcDecodeWireDwordDwordWordWord(FPacketBase& Packet, const unsigned char* Buffer,
                                             unsigned int Size, unsigned int* OutLength) {
    FPayloadCommonSetTarget* Wire =
        reinterpret_cast<FPayloadCommonSetTarget*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    unsigned int ScratchDword = 0;
    unsigned short ScratchWord = 0;
    if (!Stream.GetDword(ScratchDword)) {
        return Stream.GetError();
    }
    Wire->ObjectId = ScratchDword;
    if (!Stream.GetDword(ScratchDword)) {
        return Stream.GetError();
    }
    Wire->TargetObjectId = ScratchDword;
    if (!Stream.GetWord(ScratchWord)) {
        return Stream.GetError();
    }
    Wire->SubObjId = ScratchWord;
    if (!Stream.GetWord(ScratchWord)) {
        return Stream.GetError();
    }
    Wire->TargetSubObjId = ScratchWord;
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// ===========================================================================
// four flat dwords -- the branch / the branch.
//   ch3 0x12 FLPACKET_CLIENT_GFGOODBUY    (FPayloadClientDwordQuad)
//   ch3 0x37 FLPACKET_CLIENT_COMMCOMPLETE (FPayloadClientDwordQuad)
// NOT the same body as ch2 0x31 / 0x53 / ch3 0x30's four-dword pair (the branch /
// , already in the table as RpcEncodeWireDwordBlock12): that one is three
// dwords behind a leading id, this one is four flat fields. Two types that serialize the
// same number of bytes are still two types.
// ===========================================================================
unsigned int RpcEncodeWireFourDwords(FPacketBase& Packet, unsigned char** BufferHolder,
                                     unsigned int Capacity, unsigned int* OutLength) {
    const FPayloadClientDwordQuad* Wire =
        reinterpret_cast<const FPayloadClientDwordQuad*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));
    Stream.PutDword(Wire->Field_0x00);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.PutDword(Wire->Field_0x04);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.PutDword(Wire->Field_0x08);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.PutDword(Wire->Field_0x0c);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

unsigned int RpcDecodeWireFourDwords(FPacketBase& Packet, const unsigned char* Buffer,
                                     unsigned int Size, unsigned int* OutLength) {
    FPayloadClientDwordQuad* Wire =
        reinterpret_cast<FPayloadClientDwordQuad*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    unsigned int Scratch = 0;
    if (!Stream.GetDword(Scratch)) {
        return Stream.GetError();
    }
    Wire->Field_0x00 = Scratch;
    if (!Stream.GetDword(Scratch)) {
        return Stream.GetError();
    }
    Wire->Field_0x04 = Scratch;
    if (!Stream.GetDword(Scratch)) {
        return Stream.GetError();
    }
    Wire->Field_0x08 = Scratch;
    if (!Stream.GetDword(Scratch)) {
        return Stream.GetError();
    }
    Wire->Field_0x0c = Scratch;
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// ===========================================================================
// dword, byte, byte -- the branch / the branch.
//   ch1 0x0e FLPACKET_COMMON_ACTIVATECRUISE   (FPayloadCommonActivateCruise)
//   ch3 0x44 FLPACKET_CLIENT_REQINVINCIBILITY (FPayloadClientInvincibility)
// The two bytes go out in payload order, +0x04 then +0x05 (06db1c03 / 06db1c34), which
// is what keeps REQINVINCIBILITY's documented reason/bool crossing intact on the wire.
// ===========================================================================
unsigned int RpcEncodeWireDwordTwoBytes(FPacketBase& Packet, unsigned char** BufferHolder,
                                        unsigned int Capacity, unsigned int* OutLength) {
    const FPayloadCommonActivateCruise* Wire =
        reinterpret_cast<const FPayloadCommonActivateCruise*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));
    Stream.PutDword(Wire->ObjectId);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.PutByte(Wire->bActivate);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.PutByte(Wire->Field_0x05);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

unsigned int RpcDecodeWireDwordTwoBytes(FPacketBase& Packet, const unsigned char* Buffer,
                                        unsigned int Size, unsigned int* OutLength) {
    FPayloadCommonActivateCruise* Wire =
        reinterpret_cast<FPayloadCommonActivateCruise*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    unsigned int ScratchDword = 0;
    unsigned char ScratchByte = 0;
    if (!Stream.GetDword(ScratchDword)) {
        return Stream.GetError();
    }
    Wire->ObjectId = ScratchDword;
    if (!Stream.GetByte(ScratchByte)) {
        return Stream.GetError();
    }
    Wire->bActivate = ScratchByte;
    if (!Stream.GetByte(ScratchByte)) {
        return Stream.GetError();
    }
    Wire->Field_0x05 = ScratchByte;
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// ===========================================================================
// ch1 0x08 FLPACKET_COMMON_ACTIVATEEQUIP -- the branch / the branch.

// THE WIRE REORDERS, and this is one of the two 133-instruction bodies that does. The
// payload is {ObjectId +0x00, bActivate +0x04, EquipId +0x06} but the encoder emits
// MOV ECX,dword ptr [EDI + 0x20]   -> ObjectId
// MOV DX,word  ptr [EDI + 0x26]    -> EquipId
// MOV CL,byte  ptr [EDI + 0x24]    -> bActivate
// i.e. the 16-bit id overtakes the bool. The decoder undoes it in the same order
// (06da6955 / 06da697d / 06da69a1). rpc_payloads.h had already noted the same swap
// between the payload and XActivateEquip; this is the wire confirming it a third time.
// ===========================================================================
unsigned int RpcEncodeCommonActivateEquip(FPacketBase& Packet, unsigned char** BufferHolder,
                                          unsigned int Capacity, unsigned int* OutLength) {
    const FPayloadCommonActivateEquip* Wire =
        reinterpret_cast<const FPayloadCommonActivateEquip*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));
    Stream.PutDword(Wire->ObjectId);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.PutWord(Wire->EquipId);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.PutByte(Wire->bActivate);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

unsigned int RpcDecodeCommonActivateEquip(FPacketBase& Packet, const unsigned char* Buffer,
                                          unsigned int Size, unsigned int* OutLength) {
    FPayloadCommonActivateEquip* Wire =
        reinterpret_cast<FPayloadCommonActivateEquip*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    unsigned int ScratchDword = 0;
    unsigned short ScratchWord = 0;
    unsigned char ScratchByte = 0;
    if (!Stream.GetDword(ScratchDword)) {
        return Stream.GetError();
    }
    Wire->ObjectId = ScratchDword;
    if (!Stream.GetWord(ScratchWord)) {
        return Stream.GetError();
    }
    Wire->EquipId = ScratchWord;
    if (!Stream.GetByte(ScratchByte)) {
        return Stream.GetError();
    }
    Wire->bActivate = ScratchByte;
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// ===========================================================================
// ch1 0x0f FLPACKET_COMMON_GOTRADELANE -- the branch / the branch.
// Three dwords then a byte, payload order throughout
// (06da6bf1 / 06da6c20 / 06da6c4f / 06da6c7c out, 06da6d55 / 06da6d7d / 06da6da5 /
// back).
// ===========================================================================
unsigned int RpcEncodeCommonGoTradelane(FPacketBase& Packet, unsigned char** BufferHolder,
                                        unsigned int Capacity, unsigned int* OutLength) {
    const FPayloadCommonGoTradelane* Wire =
        reinterpret_cast<const FPayloadCommonGoTradelane*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));
    Stream.PutDword(Wire->ObjectId);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.PutDword(Wire->TradelaneRing1);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.PutDword(Wire->TradelaneRing2);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.PutByte(Wire->Field_0x0c);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

unsigned int RpcDecodeCommonGoTradelane(FPacketBase& Packet, const unsigned char* Buffer,
                                        unsigned int Size, unsigned int* OutLength) {
    FPayloadCommonGoTradelane* Wire =
        reinterpret_cast<FPayloadCommonGoTradelane*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    unsigned int ScratchDword = 0;
    unsigned char ScratchByte = 0;
    if (!Stream.GetDword(ScratchDword)) {
        return Stream.GetError();
    }
    Wire->ObjectId = ScratchDword;
    if (!Stream.GetDword(ScratchDword)) {
        return Stream.GetError();
    }
    Wire->TradelaneRing1 = ScratchDword;
    if (!Stream.GetDword(ScratchDword)) {
        return Stream.GetError();
    }
    Wire->TradelaneRing2 = ScratchDword;
    if (!Stream.GetByte(ScratchByte)) {
        return Stream.GetError();
    }
    Wire->Field_0x0c = ScratchByte;
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// ===========================================================================
// ch2 0x0a FLPACKET_SERVER_ACTIVATEOBJECT -- the branch / the branch.
// dword, byte, dword -- payload order (06da8ea0 / 06da8ed3 / 06da8f06 out,
// / 06da900b / 06da902d back).
// ===========================================================================
unsigned int RpcEncodeServerActivateObject(FPacketBase& Packet, unsigned char** BufferHolder,
                                           unsigned int Capacity, unsigned int* OutLength) {
    const FPayloadServerActivateObject* Wire =
        reinterpret_cast<const FPayloadServerActivateObject*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));
    Stream.PutDword(Wire->ObjectId);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.PutByte(Wire->bActivate);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.PutDword(Wire->Field_0x08);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

unsigned int RpcDecodeServerActivateObject(FPacketBase& Packet, const unsigned char* Buffer,
                                           unsigned int Size, unsigned int* OutLength) {
    FPayloadServerActivateObject* Wire =
        reinterpret_cast<FPayloadServerActivateObject*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    unsigned int ScratchDword = 0;
    unsigned char ScratchByte = 0;
    if (!Stream.GetDword(ScratchDword)) {
        return Stream.GetError();
    }
    Wire->ObjectId = ScratchDword;
    if (!Stream.GetByte(ScratchByte)) {
        return Stream.GetError();
    }
    Wire->bActivate = ScratchByte;
    if (!Stream.GetDword(ScratchDword)) {
        return Stream.GetError();
    }
    Wire->Field_0x08 = ScratchDword;
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// ===========================================================================
// ch2 0x47 FLPACKET_SERVER_CFGINTERFACENOTIFICATION -- the branch / the branch.

// THE SECOND REORDERING BODY, and it reorders differently from ch1 0x08's. The payload
// is {dword +0x00, dword +0x04, byte +0x08} and the encoder emits
// MOV ECX,dword ptr [EDI + 0x20]   -> Field_0x00
// MOV DL,byte  ptr [EDI + 0x28]    -> Field_0x08
// MOV ECX,dword ptr [EDI + 0x24]   -> Field_0x04
// so the trailing byte overtakes the second dword. The decoder mirrors it exactly
// (06dab3b5 / 06dab3db / 06dab3fd). Serializing in declaration order would put three
// correct values in two wrong places.
// ===========================================================================
unsigned int RpcEncodeServerCfgInterfaceNotification(FPacketBase& Packet,
                                                     unsigned char** BufferHolder,
                                                     unsigned int Capacity,
                                                     unsigned int* OutLength) {
    const FPayloadServerCfgInterfaceNotification* Wire =
        reinterpret_cast<const FPayloadServerCfgInterfaceNotification*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));
    Stream.PutDword(Wire->Field_0x00);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.PutByte(Wire->Field_0x08);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.PutDword(Wire->Field_0x04);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

unsigned int RpcDecodeServerCfgInterfaceNotification(FPacketBase& Packet,
                                                     const unsigned char* Buffer,
                                                     unsigned int Size,
                                                     unsigned int* OutLength) {
    FPayloadServerCfgInterfaceNotification* Wire =
        reinterpret_cast<FPayloadServerCfgInterfaceNotification*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    unsigned int ScratchDword = 0;
    unsigned char ScratchByte = 0;
    if (!Stream.GetDword(ScratchDword)) {
        return Stream.GetError();
    }
    Wire->Field_0x00 = ScratchDword;
    if (!Stream.GetByte(ScratchByte)) {
        return Stream.GetError();
    }
    Wire->Field_0x08 = ScratchByte;
    if (!Stream.GetDword(ScratchDword)) {
        return Stream.GetError();
    }
    Wire->Field_0x04 = ScratchDword;
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// ===========================================================================
// ch3 0x14 FLPACKET_CLIENT_MISSIONRESPONSE -- the branch / the branch.
// dword, dword, byte -- payload order (06daef30 / 06daef65 / 06daef98 out,
// / 06daf09d / 06daf0bf back).
// ===========================================================================
unsigned int RpcEncodeClientMissionResponse(FPacketBase& Packet, unsigned char** BufferHolder,
                                            unsigned int Capacity, unsigned int* OutLength) {
    const FPayloadClientMissionResponse* Wire =
        reinterpret_cast<const FPayloadClientMissionResponse*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));
    Stream.PutDword(Wire->Field_0x00);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.PutDword(Wire->MissionId);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.PutByte(Wire->bAccepted);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

unsigned int RpcDecodeClientMissionResponse(FPacketBase& Packet, const unsigned char* Buffer,
                                            unsigned int Size, unsigned int* OutLength) {
    FPayloadClientMissionResponse* Wire =
        reinterpret_cast<FPayloadClientMissionResponse*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    unsigned int ScratchDword = 0;
    unsigned char ScratchByte = 0;
    if (!Stream.GetDword(ScratchDword)) {
        return Stream.GetError();
    }
    Wire->Field_0x00 = ScratchDword;
    if (!Stream.GetDword(ScratchDword)) {
        return Stream.GetError();
    }
    Wire->MissionId = ScratchDword;
    if (!Stream.GetByte(ScratchByte)) {
        return Stream.GetError();
    }
    Wire->bAccepted = ScratchByte;
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// ===========================================================================
// ch2 0x44 FLPACKET_SERVER_REQUEST_RETURNED -- the branch / the branch.
// dword then THREE consecutive bytes, +0x04 / +0x05 / +0x06
// (06daa891 / 06daa8be / 06daa8e9 / 06daa914 out, 06daa9f5 / 06daaa1b / 06daaa3f /
// back). The payload's own signed/unsigned split is a property of the
// RECEIVER, not of the wire: all three travel as raw bytes here.
// ===========================================================================
unsigned int RpcEncodeServerRequestReturned(FPacketBase& Packet, unsigned char** BufferHolder,
                                            unsigned int Capacity, unsigned int* OutLength) {
    const FPayloadServerRequestReturned* Wire =
        reinterpret_cast<const FPayloadServerRequestReturned*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));
    Stream.PutDword(Wire->Field_0x00);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.PutByte(Wire->Field_0x04);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.PutByte((unsigned char)Wire->Field_0x05);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.PutByte((unsigned char)Wire->Field_0x06);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

unsigned int RpcDecodeServerRequestReturned(FPacketBase& Packet, const unsigned char* Buffer,
                                            unsigned int Size, unsigned int* OutLength) {
    FPayloadServerRequestReturned* Wire =
        reinterpret_cast<FPayloadServerRequestReturned*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    unsigned int ScratchDword = 0;
    unsigned char ScratchByte = 0;
    if (!Stream.GetDword(ScratchDword)) {
        return Stream.GetError();
    }
    Wire->Field_0x00 = ScratchDword;
    if (!Stream.GetByte(ScratchByte)) {
        return Stream.GetError();
    }
    Wire->Field_0x04 = ScratchByte;
    if (!Stream.GetByte(ScratchByte)) {
        return Stream.GetError();
    }
    Wire->Field_0x05 = (signed char)ScratchByte;
    if (!Stream.GetByte(ScratchByte)) {
        return Stream.GetError();
    }
    Wire->Field_0x06 = (signed char)ScratchByte;
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// ===========================================================================
// ch3 0x32 FLPACKET_CLIENT_REQUEST_CANCEL -- the branch / the branch.
// byte, dword, dword, byte -- payload order, and the LEADING field is the one-byte
// truncation rpc_payloads.h records (06daa5cf / 06daa5fc / 06daa62b / 06daa658 out,
// / 06daa759 / 06daa781 / 06daa7a3 back).
// ===========================================================================
unsigned int RpcEncodeClientRequestCancel(FPacketBase& Packet, unsigned char** BufferHolder,
                                          unsigned int Capacity, unsigned int* OutLength) {
    const FPayloadClientRequestCancel* Wire =
        reinterpret_cast<const FPayloadClientRequestCancel*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));
    Stream.PutByte(Wire->EventType);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.PutDword(Wire->Field_0x04);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.PutDword(Wire->Field_0x08);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.PutByte(Wire->Field_0x0c);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

unsigned int RpcDecodeClientRequestCancel(FPacketBase& Packet, const unsigned char* Buffer,
                                          unsigned int Size, unsigned int* OutLength) {
    FPayloadClientRequestCancel* Wire =
        reinterpret_cast<FPayloadClientRequestCancel*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    unsigned int ScratchDword = 0;
    unsigned char ScratchByte = 0;
    if (!Stream.GetByte(ScratchByte)) {
        return Stream.GetError();
    }
    Wire->EventType = ScratchByte;
    if (!Stream.GetDword(ScratchDword)) {
        return Stream.GetError();
    }
    Wire->Field_0x04 = ScratchDword;
    if (!Stream.GetDword(ScratchDword)) {
        return Stream.GetError();
    }
    Wire->Field_0x08 = ScratchDword;
    if (!Stream.GetByte(ScratchByte)) {
        return Stream.GetError();
    }
    Wire->Field_0x0c = ScratchByte;
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// ===========================================================================
// ch2 0x27 FLPACKET_SERVER_REQUESTCREATESHIPRESP -- the branch / the branch.
// byte then dword (06da804e / 06da8081 out, 06da8162 / 06da818a back).
// ===========================================================================
unsigned int RpcEncodeServerRequestCreateShipResp(FPacketBase& Packet,
                                                  unsigned char** BufferHolder,
                                                  unsigned int Capacity,
                                                  unsigned int* OutLength) {
    const FPayloadServerRequestCreateShipResp* Wire =
        reinterpret_cast<const FPayloadServerRequestCreateShipResp*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));
    Stream.PutByte(Wire->Field_0x00);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.PutDword(Wire->Field_0x04);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

unsigned int RpcDecodeServerRequestCreateShipResp(FPacketBase& Packet,
                                                  const unsigned char* Buffer,
                                                  unsigned int Size,
                                                  unsigned int* OutLength) {
    FPayloadServerRequestCreateShipResp* Wire =
        reinterpret_cast<FPayloadServerRequestCreateShipResp*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    unsigned int ScratchDword = 0;
    unsigned char ScratchByte = 0;
    if (!Stream.GetByte(ScratchByte)) {
        return Stream.GetError();
    }
    Wire->Field_0x00 = ScratchByte;
    if (!Stream.GetDword(ScratchDword)) {
        return Stream.GetError();
    }
    Wire->Field_0x04 = ScratchDword;
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// ===========================================================================
// ch3 0x25 FLPACKET_CLIENT_DBGDESTROYOBJECT -- the branch / the branch.
// dword then word (06db1330 / 06db1365 out, 06db1444 / 06db146e back).

// The type has NO sender in the release image -- nothing anywhere writes 0x30025 into a
// packet except its own constructor the routine (06da0faa), and the ch3 dispatcher
// answers it with an FDUMP. Only the two widths are proven, so the payload struct keeps
// Field_0xNN names; the packet is 0x28 bytes, so the remaining two payload bytes are
// never touched by either direction.
// ===========================================================================
unsigned int RpcEncodeClientDbgDestroyObject(FPacketBase& Packet, unsigned char** BufferHolder,
                                             unsigned int Capacity, unsigned int* OutLength) {
    const FPayloadClientDbgDestroyObject* Wire =
        reinterpret_cast<const FPayloadClientDbgDestroyObject*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));
    Stream.PutDword(Wire->Field_0x00);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.PutWord(Wire->Field_0x04);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

unsigned int RpcDecodeClientDbgDestroyObject(FPacketBase& Packet, const unsigned char* Buffer,
                                             unsigned int Size, unsigned int* OutLength) {
    FPayloadClientDbgDestroyObject* Wire =
        reinterpret_cast<FPayloadClientDbgDestroyObject*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    unsigned int ScratchDword = 0;
    unsigned short ScratchWord = 0;
    if (!Stream.GetDword(ScratchDword)) {
        return Stream.GetError();
    }
    Wire->Field_0x00 = ScratchDword;
    if (!Stream.GetWord(ScratchWord)) {
        return Stream.GetError();
    }
    Wire->Field_0x04 = ScratchWord;
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// ===========================================================================
// ch2 0x2a FLPACKET_SERVER_ADJUSTATTITUDE -- the branch encode only.

// ONE HALF OF THIS TYPE'S PAIR WAS ALREADY PORTED WITHOUT ANYONE NOTICING. Its DECODER
// slot is the branch, which is the very body the twelve-type dword-pair family already
// routes to as RpcDecodeWireDwordPair; the table below simply reuses it, and this file
// adds only the encoder.

// The encoder is NOT shared, and the difference is four instructions. Diffed against
// , the dword-pair family's encoder, the two are identical for 113/112
// instructions except that where 06da9110 has
//     +0xa7  MOV EDX,dword ptr [EDI + 0x24]
//     +0xaa  MOV dword ptr [EAX],EDX
// has
//     +0xa7  FLD  float ptr [EDI + 0x24]
//     +0xaa  FSTP float ptr [EAX]
// Same four bytes, moved through the x87 stack instead of a GPR -- which is MSVC's
// signature for a `float` lvalue and is the third independent confirmation that
// FPayloadServerAdjustAttitude::Field_0x04 is a float (the sender at 06d88f96 and the
// receiver at 06d8131a are the other two).
// ===========================================================================
unsigned int RpcEncodeServerAdjustAttitude(FPacketBase& Packet, unsigned char** BufferHolder,
                                           unsigned int Capacity, unsigned int* OutLength) {
    const FPayloadServerAdjustAttitude* Wire =
        reinterpret_cast<const FPayloadServerAdjustAttitude*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));
    Stream.PutDword(Wire->Field_0x00);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.PutFloat(Wire->Field_0x04);                                 // FLD / 06daa16a FSTP
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// ===========================================================================
// ch2 0x08 FLPACKET_SERVER_CHARSELECTVERIFIED -- the branch / the branch.

// THE PAYLOAD HOLE IS ON THE WIRE TOO, and this settles it from a third direction. The
// payload is four dwords with +0x04 never written and never read; the codec skips it
// outright. The encoder emits the dword at +0x20 and then EIGHT bytes starting at +0x28:
// MOV  ECX,dword  ptr [EDI + 0x20]   ; 06dabe71 ADD EDX,0x8
// FLD  double ptr [EDI + 0x28]
// FSTP double ptr [EAX]
// so the wire is 2 + 4 + 8 = 14 bytes and payload+0x04 is simply absent. The DECODER is
// not symmetric in form -- it reads the eight bytes as two plain dwords,
// MOV EDX,dword ptr [EAX]       -> 06dabfa3  MOV [EDI + 0x28],EDX
// MOV ECX,dword ptr [EAX + 0x4] -> 06dabfaa  MOV [EDI + 0x2c],ECX
// which is bit-exact. The encoder's FLD/FSTP is not: x87 quiets a signalling NaN on the
// way through. The encoder below moves the eight bytes through the same FLD/FSTP pair and
// so keeps that case; the decoder reads them as two plain dwords, as baseline does.
// ===========================================================================
unsigned int RpcEncodeServerCharSelectVerified(FPacketBase& Packet,
                                               unsigned char** BufferHolder,
                                               unsigned int Capacity,
                                               unsigned int* OutLength) {
    const FPayloadServerCharSelectVerified* Wire =
        reinterpret_cast<const FPayloadServerCharSelectVerified*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));
    Stream.PutDword(Wire->Field_0x00);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    // The eight bytes at payload+0x08..+0x0f, reserved in ONE advance of 8
    // (06dabe71 `ADD EDX,0x8`) and then written without a second check.
    if (!Stream.Reserve(8, FRpcMarshalStream::kEncodeOverflow)) {
        return Stream.GetError();
    }
    // `MOV EAX,[EBX]`, `FLD double ptr [EDI+0x28]`, `FSTP double ptr
    // [EAX]` -- MSVC's eight-byte block copy through the x87 stack. It is bit-exact for
    // every value except a signalling NaN, which FLD quiets on the way through; moving
    // the bytes through the same two instructions keeps that case too. (The decoder,
    // / 06dabfa6, reads the window back as two plain dwords.)
    const void* DoubleSource = &Wire->Field_0x08;
    void* DoubleDest = reinterpret_cast<void*>(Packet.MarshalPtr);
    __asm {
        mov eax, DoubleSource
        fld qword ptr [eax]
        mov eax, DoubleDest
        fstp qword ptr [eax]
    }
    Packet.MarshalPtr += 8u;
    *OutLength = Stream.Length();
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

unsigned int RpcDecodeServerCharSelectVerified(FPacketBase& Packet,
                                               const unsigned char* Buffer,
                                               unsigned int Size,
                                               unsigned int* OutLength) {
    FPayloadServerCharSelectVerified* Wire =
        reinterpret_cast<FPayloadServerCharSelectVerified*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    unsigned int Scratch = 0;
    if (!Stream.GetDword(Scratch)) {
        return Stream.GetError();
    }
    Wire->Field_0x00 = Scratch;
    // baseline reserves eight bytes once (06dabf85 ADD EDX,0x8) and then lifts both dwords
    // out of that one window, so the second dword has no bounds check of its own.
    if (!Stream.Reserve(8, FRpcMarshalStream::kDecodeOverrun)) {
        return Stream.GetError();
    }
    if (!Stream.GetDwordUncounted(Scratch)) {
        return Stream.GetError();
    }
    Wire->Field_0x08 = Scratch;
    if (!Stream.GetDwordUncounted(Scratch)) {
        return Stream.GetError();
    }
    Wire->Field_0x0c = Scratch;
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// ===========================================================================
// ch3 0x2d FLPACKET_CLIENT_DBGCONSOLE -- {dword, 128 raw bytes}.
// Encoder the branch (122 instructions), decoder the branch (108).

// THIS BLOCK USED TO CLAIM "neither body is shared: every one of the 26 packet types
// still without a codec has a distinct encoder, checked by comparing full function
// extents rather than prologues". THAT IS FALSE and was disproved 2026-09-17 by reading
// the vtable slots out of .rdata for all 162 types at once. Two of the remaining bodies
// serve two registered types each:
// / 06db5c98  -> the branch   ch3 0x3a and ch3 0x06
// / 06db5df8  -> the branch   ch2 0x48 and ch3 0x36
// Both are transcribed in rpc_wire_varlen.cpp. Comparing function extents can only show
// that two ADDRESSES differ; it cannot show that two vtables do not point at the SAME
// address, which is what sharing actually is. The DBGCONSOLE pair below is genuinely
// unshared -- module storage / 06db5dbc are its only referents -- but that is a fact
// about this pair, not about the other twenty-five.

// Two things in the baseline bodies that are easy to lose:
//   * the 128 bytes are ONE reservation and ONE copy, not thirty-two dword fields --
// `ADD EDX,0x80` guards the whole run and 06db1728 `MOVSD.REP ES:EDI,ESI`
//     moves it with ECX preloaded to 0x20 at 06db1720. So a short buffer fails once,
//     before any byte is written, exactly as PutBytes does.
//   * `CALL the routine` sits between the copy and the length store. That is
//     the same `XOR EAX,EAX ; RET 0xc` stub the decode side uses for its header hook
//     (06da13c0..06da13c2), so it is a footer that cannot fail and has no effect. It is
//     modelled as nothing rather than as a call, and the `if (result) return 0x16`
//     branch behind it at 06db1747 is unreachable for the same reason the header's is.
// ===========================================================================
unsigned int RpcEncodeClientDbgConsole(FPacketBase& Packet, unsigned char** BufferHolder,
                                       unsigned int Capacity, unsigned int* OutLength) {
    const FPayloadClientDbgConsole* Wire =
        reinterpret_cast<const FPayloadClientDbgConsole*>(Packet.Payload());

    // / 06db1686: Limit = arg2, Ptr = *arg1. 06db1689 / 06db168f: Cursor = 0,
    // Error = 0.
    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));
    // `ADD EDX,0x4`, then 06db16e5 `MOV ECX,dword ptr [EBP + 0x20]` lifts
    // payload+0x00 and 06db16e8 `MOV dword ptr [EAX],ECX` stores it.
    Stream.PutDword(Wire->Field_0x00);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.PutBytes(Wire->Text_0x04, 128u);                            // / 06db1728
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// Decode. The mirror, with kDecodeOverrun (0x17) in place of 0x16 at
// and 06db1823, and the header hook the routine at 06db17cb consuming nothing:
// the dispatcher already ate the two routing bytes, which is why this side's reported
// length excludes them and RpcRecordDecodedPacket adds 2 back. That +2 is visible in
// this very body -- 06db1895 `ADD EAX,0x2` into the total, 06db189f `ADD EDX,0x2` onto
// the old max before 06db18a2 `CMP EBX,EDX` / 06db18a4 `JBE`, and 06db18a6 `ADD EBX,0x2`
// into the new max.
unsigned int RpcDecodeClientDbgConsole(FPacketBase& Packet, const unsigned char* Buffer,
                                       unsigned int Size, unsigned int* OutLength) {
    FPayloadClientDbgConsole* Wire =
        reinterpret_cast<FPayloadClientDbgConsole*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    unsigned int Scratch = 0;
    if (!Stream.GetDword(Scratch)) {
        return Stream.GetError();
    }
    Wire->Field_0x00 = Scratch;
    if (!Stream.GetBytes(Wire->Text_0x04, 128u)) {                     // / 06db1846
        return Stream.GetError();
    }
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}
