// Author: Giperion.

// rpc_wire_varlen.cpp -- the VARIABLE-LENGTH packet codec slots.

// Three baseline Encode/Decode vtable pairs, covering five of the 162 registered packet
// types. All three differ from everything in rpc_wire_fields.cpp in the same way: the
// number of bytes on the wire is not a property of the type, it is read out of the
// packet (a strlen, a list size, a block size) and written as a leading count.

// WHY FIVE TYPES AND NOT THREE. The vtable slots were read out of .rdata, not guessed,
// and two pairs of types share a body outright:

// addr .text:the branch   <- ch3 0x3a DESTROYCHAR   (ctor 06da0bcd)
// addr .text:the branch
// addr .text:the branch   <- ch3 0x06 SELECTCHARACTER (ctor 06da0bfd)
// addr .text:the branch

// addr .text:the branch   <- ch2 0x48 SETCOLLISIONGROUPS (ctor 06d9e915)
// addr .text:the branch
// addr .text:the branch   <- ch3 0x36 REQCOLLISIONGROUPS (ctor 06da1115)
// addr .text:the branch

// This CONTRADICTS the note at the head of the ch3 0x2d block in rpc_wire_fields.cpp,
// which claimed "every one of the 26 packet types still without a codec has a distinct
// encoder, checked by comparing full function extents". Two of them do not: these four
// types are served by two bodies. That note is corrected in place.

// HOW THE FOUR CONSTRUCTORS WERE FOUND. Earlier waves scanned for
// `MOV dword ptr [EAX + 0xc],<id>` and came up 152 short of 162, leaving ch2 0x2b,
// ch2 0x48 and ch3 0x36 looking as if they found their routing id "some other way".
// They do not. The constructors of those three have an SEH frame (they own a std::list
// or a std::string member), so MSVC keeps `this` in ESI instead of EAX and the very same
// store reads `MOV dword ptr [ESI + 0xc],<id>`:
// MOV dword ptr [ESI + 0xc],0x2002b
// MOV dword ptr [ESI + 0xc],0x20048
// MOV dword ptr [ESI + 0xc],0x30036
// (operand text only: the listing prints these immediates truncated in its byte
// column, so spelling the encoding out would be reconstruction, not reading.)
// Re-run across every base register the scan yields exactly 162 stores and exactly 162
// distinct ids -- one per registry row, none missing and none doubled. The routing
// header is written for these three like it is for the other 159; nothing is inert.

// THE ROUTING WORD ITSELF. the routine builds it from a dword the constructor writes at
// packet+0x0c: 06da1383 `MOV DX,word ptr [this + 0xc]` is the opcode and 06da1389
// `MOV BH,byte ptr [this + 0xe]` is the channel. So `0x30005` is not a scalar id, it is
// the byte image `05 00 03 00` -- opcode word at +0x0c, channel byte at +0x0e, a spare
// byte at +0x0f that nothing reads.

// Single-player: in SP a packet is queued by pointer and neither slot is ever called.

#include "rpc_containers.h"
#include "rpc_payloads.h"

#include <new>

// ===========================================================================
// ch3 0x06 FLPACKET_CLIENT_SELECTCHARACTER and ch3 0x3a FLPACKET_CLIENT_DESTROYCHAR --
// (133 instructions) / the branch (118).

// Both types are operator new(0x220) -- 06da00c5 `PUSH 0x220` in factory 06da00c0 and
// `PUSH 0x220` in factory 06da01c0, both read off the listing rather than
// carried over from the registry table -- so the payload is 0x220 - 0x20 == 0x200 and
// IS the CHARACTER_ID, exactly as rpc_payload_ch3.cpp already treats it. The type's own
// dump slot confirms the field is the character name and nothing else:
// ADD ECX,0x20
// PUSH .rdata:s_Character:_06db66ef+1   ;= "   Character: "
// CALL MSVCP60.DLL::std::operator<<

// THE WIRE FORMAT is { word Length; char Text[Length] } -- NO terminator on the wire and
// NO fixed run. 06daf883 `SCASB.REPNE ES:EDI` over payload+0x00 with ECX preloaded to
// -1 at 06daf87b, then 06daf88a `NOT ECX` / 06daf88f `DEC ECX`, is a plain strlen; that
// count is what 06daf8b2 `MOV word ptr [EAX],CX` puts on the wire and what 06daf8f5
// `MOVSD.REP ES:EDI,ESI` copies.

// DO NOT CONFUSE THIS WITH ch3 0x2d DBGCONSOLE, which looks similar and is not: there
// `ADD EDX,0x80` reserves the whole 128-byte run in one go and the bytes past
// the terminator go out on the wire as they sit in the packet. There is no SCASB and no
// length prefix anywhere in that pair. Here there is no fixed run at all and the bytes
// past the terminator never leave.

// The length is TRUNCATED TO 16 BITS before it is used again, not merely when it is
// stored: 06daf8bd `XOR EAX,EAX` / 06daf8bf `MOV AX,CX` rebuilds it from the word, and
// that truncated value is what feeds both the cursor bump at 06daf8c4 `ADD ECX,EAX` and
// the copy count at 06daf8e8 `MOV ECX,EAX`. Reproduced, although a CHARACTER_ID is only
// 0x200 bytes so nothing can reach the truncation in practice.
// ===========================================================================
unsigned int RpcEncodeWireCharacterId(FPacketBase& Packet, unsigned char** BufferHolder,
                                      unsigned int Capacity, unsigned int* OutLength) {
    const char* Name = reinterpret_cast<const char*>(Packet.Payload());

    // / 06daf846: Limit = arg2, Ptr = *arg1. 06daf849 / 06daf84f: Cursor = 0,
    // Error = 0.
    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));

    // the REPNE SCASB strlen over payload+0x00, unbounded, as
    // baseline's. The payload is a CHARACTER_ID the marshaller copied whole, terminator
    // included.
    unsigned int Length = 0;
    while (Name[Length] != '\0') {
        ++Length;
    }
    const unsigned short WireLength = (unsigned short)(Length & 0xffffu);

    // `ADD EAX,0x2` reserves, 06daf894 `JBE` checks, 06daf8b2
    // `MOV word ptr [EAX],CX` stores.
    Stream.PutWord(WireLength);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    // `ADD ECX,EAX` reserves the run, 06daf8f5 / 06daf8fc copy it.
    Stream.PutBytes(Name, (unsigned int)WireLength);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// Decode. kDecodeOverrun (0x17) at 06daf9c7 and 06daf9f9 in place of the
// encoder's 0x16, and both of its header/trailer hooks are the routine, the
// `XOR EAX,EAX ; RET 0xc` stub at 06da13c0..06da13c2, so neither can fail.

// After the copy, 06dafa2f `MOV byte ptr [EDX + EBX*0x1 + 0x20],0x0` terminates the
// payload at payload+Length. EDX there is the word read off the wire, zero-extended at
// /06daf9e0, and NOTHING bounds it against the 0x200-byte payload: the only
// check the length passes is the one against the INPUT buffer, at 06daf9f2 `CMP EAX,ECX`.
// A datagram longer than 0x220 bytes therefore writes past the packet in baseline, and in
// the port.
unsigned int RpcDecodeWireCharacterId(FPacketBase& Packet, const unsigned char* Buffer,
                                      unsigned int Size, unsigned int* OutLength) {
    char* Name = reinterpret_cast<char*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    unsigned short WireLength = 0;
    if (!Stream.GetWord(WireLength)) {                                 // / 06daf9db
        return Stream.GetError();
    }
    const unsigned int Length = (unsigned int)WireLength;
    if (!Stream.GetBytes(Name, Length)) {                              // / 06dafa1b
        return Stream.GetError();
    }
    Name[Length] = '\0';
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// ===========================================================================
// ch2 0x48 FLPACKET_SERVER_SETCOLLISIONGROUPS and ch3 0x36 FLPACKET_CLIENT_
// REQCOLLISIONGROUPS -- the branch (83 instructions) / the branch (82).

// Both are operator new(0x2c) -- 06d9e875 `PUSH 0x2c` and 06da03c5 `PUSH 0x2c` -- so the
// payload is 0x0c, which is exactly one baseline std::list header. FRpcCollisionGroupList
// and its node are already declared in rpc_containers.h and the list is built by the
// packet constructor (rpc_packet_registry.cpp); this file only adds the two wire slots.

// NEITHER SLOT DOES THE WORK ITSELF. The encoder's whole payload is one call at
// to the routine and the decoder's is one call at 06dacdbf to the routine.
// Both helpers are declared `RET 0xc`, and both are handed the slot's own three
// arguments -- 06daccf1..06daccf3 push OutLength, Capacity and BufferHolder -- but
// NEITHER HELPER READS ONE OF THEM. Every access in the routine goes through ECX:
// param_1+0x10 the cursor, +0x14 the limit, +0x18 the pointer, +0x1c the error,
// +0x24 the list head and +0x28 its size. Checklist class 2: the arity is real and the
// arguments are dead.

// THE WIRE FORMAT is { word Count; { word Id; float Health; } Count times }.
//   * `MOV SI,word ptr [param_1 + 0x28]` takes the count from the LIST HEADER's
//     size field, then 06d8f55b `MOV word ptr [EDX],SI` puts it on the wire;
//   * the loop that follows is driven by the LIST ITSELF -- 06d8f56c `MOV EDX,[EDI]` and
// `MOV EDX,[EDX]` with 06d8f5c6 `CMP EDX,EDI` as the sentinel test -- not
//     by that count. The two agree in every baseline sender, and the port keeps both
//     halves as baseline wrote them rather than deriving the count from the walk.
//   * per node: 06d8f58f `MOV BP,word ptr [EDX + 0x8]` is the LOW WORD of the value's
//     +0x00 only, and 06d8f5b6 `FLD float ptr [EDX + 0xc]` / 06d8f5bc `FSTP float ptr
//     [ESI]` is the health as a raw float. No quantizer on this pair, unlike the
//     CREATESHIP/CREATESOLAR header codec which sends the same node type through a
//     255.0 scale.
// ===========================================================================
unsigned int RpcEncodeWireCollisionGroups(FPacketBase& Packet, unsigned char** BufferHolder,
                                          unsigned int Capacity, unsigned int* OutLength) {
    const FRpcCollisionGroupList* List = RpcPacketCollisionGroups(&Packet);

    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));

    // begins here.
    const unsigned short Count = (unsigned short)(List->Size & 0xffffu);
    Stream.PutWord(Count);                                             // / 06d8f55b
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    // `TEST SI,SI` / 06d8f567 `JBE` -- TEST clears CF, so JBE is JZ here: a
    // zero count skips the walk entirely. 06d8f56e `CMP EDX,EDI` / 06d8f570 `JZ` skips
    // it again when the list is empty.
    // The dummy is dereferenced untested (06d8f56c); a non-zero count means it was built.
    if (Count != 0) {
        const FRpcCollisionGroupNode* Node = List->Head->Next;
        while (Node != List->Head) {
            Stream.PutWord((unsigned short)(Node->Field_0x00 & 0xffffu));
            if (Stream.Failed()) {
                return Stream.GetError();
            }
            // The value is moved as a float through the x87 stack, but FLD/FSTP of a
            // single-precision slot is a bit-exact 4-byte move; Field_0x04 already holds
            // the bit pattern, so no reinterpretation is needed either way.
            Stream.PutDword(Node->Field_0x04);                         // / 06d8f5bc
            if (Stream.Failed()) {
                return Stream.GetError();
            }
            Node = Node->Next;
        }
    }

    // the routine -- the trailer hook, the `XOR EAX,EAX ; RET 0xc` no-op.
    *OutLength = Stream.Length();
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// / the routine -- Decode.

// calls COMMON.DLL's std::list::erase(begin, end) on the payload's own list
// before reading anything, so a decode always starts from an empty list;
// RpcCollisionGroupListAssign(List, nullptr, 0) is that erase plus the Size bookkeeping
// baseline's callee does, and it is already verified against the routine.

// The append is baseline's own splice, not a helper: 06d8f660 `MOV [EDI+4],EAX`,
// `MOV EDX,[EAX+4]`, 06d8f66a `MOV [EDX],EAX`, 06d8f67c `INC EDX` /
// `MOV [EBX+8],EDX`.

// A BASELINE DEFECT REPRODUCED ONLY IN PART. 06d8f671 calls the routine -- the
// CollisionGroupDesc copy-constructor, 06d85500..06d85516, eight bytes with a null-dest
// guard -- to construct the new node's value, and its SOURCE is the stack slot at
// ESP+0x14. That slot is ESP-relative E-0x8 inside the `SUB ESP,0xc` frame opened at
// , and nothing ever writes it: the only slot of that frame that is written is
// E-0xc, at 06d8f635, and that one holds the loop bound (06d8f6e1 `CMP BP,word ptr
// [ESP + 0x10]` reads it back). So baseline constructs every node from eight uninitialised
// stack bytes. The two fields that follow overwrite +0x04 completely but only the LOW
// WORD of +0x00 (06d8f6af `MOV word ptr [EAX],DX`), which leaves the high half of every
// decoded CollisionGroupDesc::Id holding stack residue. The port zeroes instead; see
// implementation notes.
unsigned int RpcDecodeWireCollisionGroups(FPacketBase& Packet, const unsigned char* Buffer,
                                          unsigned int Size, unsigned int* OutLength) {
    FRpcCollisionGroupList* List = RpcPacketCollisionGroups(&Packet);

    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    // begins here. 06d8f5fa -- erase(begin, end).
    RpcCollisionGroupListAssign(List, nullptr, 0u);
    unsigned short Count = 0;
    if (!Stream.GetWord(Count)) {                                      // / 06d8f629
        return Stream.GetError();
    }
    // `TEST CX,CX` / 06d8f639 `JBE`, then the same test again at 06d8f641 --
    // MSVC emitted the zero check twice. Either one alone has the same effect.
    for (unsigned int Index = 0; Index < (unsigned int)Count; ++Index) {
        // COMMON.DLL list _Buynode: the node is used untested (06d8f660).
        FRpcCollisionGroupNode* New =
            static_cast<FRpcCollisionGroupNode*>(::operator new(sizeof(FRpcCollisionGroupNode),
                                                               std::nothrow));
        FRpcCollisionGroupNode* Dummy = List->Head;
        FRpcCollisionGroupNode* Tail = Dummy->Prev;
        New->Next = Dummy;
        New->Prev = Tail;
        Dummy->Prev = New;
        Tail->Next = New;
        // baseline's the routine copies eight uninitialised bytes here.
        New->Field_0x00 = 0;
        New->Field_0x04 = 0;
        ++List->Size;

        unsigned short Id = 0;
        if (!Stream.GetWord(Id)) {                                     // / 06d8f6ac
            return Stream.GetError();
        }
        New->Field_0x00 = (unsigned int)Id;                            // , low word only
        unsigned int Health = 0;
        if (!Stream.GetDword(Health)) {                                // / 06d8f6db
            return Stream.GetError();
        }
        New->Field_0x04 = Health;
    }

    // the routine -- the trailer hook, the same no-op.
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    *OutLength = Stream.Length();
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// ===========================================================================
// ch2 0x45 FLPACKET_SERVER_SET_MISSION_MESSAGE -- the branch (134 instructions) /
// (93). Its vtable is 06db516c, from 06d9d9b0
// `MOV dword ptr [EAX + 0xc],0x20045`, and module storage / 06db5174 are the two slots.
// Registry size 0x2c -> payload 0x0c, the {TextSize, TextBuffer, Field_0x08} triple that
// rpc_payloads.h already declares.

// THE WIRE ORDER IS NOT THE STRUCT ORDER, which is the whole reason this pair had to be
// transcribed rather than assumed: the LAST field of the payload goes out FIRST.
// `ADD EDX,0x4` / 06dabc1e `MOV EDX,dword ptr [EBP + 0x28]` / 06dabc21
//            `MOV dword ptr [ECX],EDX`   -- packet+0x28 == payload+0x08, first on the wire
// `ADD EDI,0x4` / 06dabc48 `MOV EDX,dword ptr [EBP + 0x20]` / 06dabc4b
//            `MOV dword ptr [ECX],EDX`   -- payload+0x00, the size
// `ADD EDI,ECX` / 06dabc85 `MOVSD.REP ES:EDI,ESI` from 06dabc77
//            `MOV ESI,dword ptr [EBP + 0x24]` -- payload+0x04, that many bytes
// A codec written from the declaration order would have put the size first and been
// byte-wrong for every packet of this type.

// The size/bytes pair is baseline's one variable-length primitive; the decoder reaches it
// through the real function the routine (06dabd72 `CALL the routine` with 06dabd64
// `LEA EDX,[EDI + 0x20]`, i.e. &payload+0x00), which is already modelled by
// RpcPutSizedBlock / RpcGetSizedBlock in rpc_containers.cpp -- allocation before the
// second bounds check included.
// ===========================================================================
unsigned int RpcEncodeServerSetMissionMessage(FPacketBase& Packet,
                                              unsigned char** BufferHolder,
                                              unsigned int Capacity,
                                              unsigned int* OutLength) {
    const FPayloadServerSetMissionMessage* Wire =
        reinterpret_cast<const FPayloadServerSetMissionMessage*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));
    Stream.PutDword(Wire->Field_0x08);                                 // / 06dabc21
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    RpcPutSizedBlock(Stream, Wire->TextSize, Wire->TextBuffer);        // / 06dabc5a
    if (Stream.Failed()) {
        return Stream.GetError();                                      // / 06dabc6a
    }
    // the routine -- the trailer hook, the no-op.
    *OutLength = Stream.Length();
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

unsigned int RpcDecodeServerSetMissionMessage(FPacketBase& Packet,
                                              const unsigned char* Buffer,
                                              unsigned int Size,
                                              unsigned int* OutLength) {
    FPayloadServerSetMissionMessage* Wire =
        reinterpret_cast<FPayloadServerSetMissionMessage*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    unsigned int Scratch = 0;
    if (!Stream.GetDword(Scratch)) {                                   // / 06dabd62
        return Stream.GetError();
    }
    Wire->Field_0x08 = Scratch;
    // The decoder overwrites the pointer WITHOUT freeing whatever the payload already
    // held; only the packet's destructor (the routine) ever frees packet+0x24. baseline
    // leaks on a second decode into the same packet, and so does this.
    if (!RpcGetSizedBlock(Stream, Wire->TextSize, Wire->TextBuffer)) {
        return Stream.GetError();                                      // the routine
    }
    // the routine -- the trailer hook.
    *OutLength = Stream.Length();
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// ===========================================================================
// ch2 0x3d FLPACKET_SERVER_REMOTEOBJECTCARGOUPDATE -- the branch (128 instructions) /
// (115). Its vtable is 06db3c70, written at 06d9ecfd
// `MOV dword ptr [EAX],.rdata:the routine`, with the id at 06d9ed03
// `MOV dword ptr [EAX + 0xc],0x2003d` -- EAX, not ESI: this constructor is one of the
// plain ones. module storage / 06db3c78 are the two slots.
// Registry size 0x2c -> payload 0x0c.

// The type carries the SAME equipment/cargo list the ch2 0x24 / 0x25 and ch3 0x16 / 0x17
// family carries, and reaches it through the very same out-of-line bodies: 06d8f902
// `CALL the routine` and 06d8fa17 `CALL the routine`. Those bodies were already
// transcribed for that family; they are now RpcPutEquipList / RpcGetEquipList in
// rpc_wire_generic.cpp, called from both places exactly as baseline calls them, rather
// than copied. Nothing about the record layout is re-derived here.

// THE WIRE ORDER IS AGAIN NOT THE STRUCT ORDER, and this one is a three-way shuffle:
// `ADD EDX,0x4`  / 06d8f8b5 `MOV ECX,dword ptr [EDI + 0x28]` / 06d8f8b8
//            `MOV dword ptr [EAX],ECX`  -- payload+0x08 leads
// `INC EDX`      / 06d8f8e9 `MOV DL,byte ptr [EDI + 0x24]`   / 06d8f8f4
//            `MOV byte ptr [EAX],DL`    -- payload+0x04, ONE byte, not a dword
// the list at payload+0x00 comes last
// The byte is a byte on both sides -- 06d8fa02 `MOV DL,byte ptr [EAX]` / 06d8fa14
// `MOV byte ptr [EDI + 0x24],DL` -- so the three padding bytes of Field_0x04 never
// travel and are not read back.
// ===========================================================================
unsigned int RpcEncodeServerRemoteObjectCargoUpdate(FPacketBase& Packet,
                                                    unsigned char** BufferHolder,
                                                    unsigned int Capacity,
                                                    unsigned int* OutLength) {
    const FPayloadServerRemoteObjectCargoUpdate* Wire =
        reinterpret_cast<const FPayloadServerRemoteObjectCargoUpdate*>(Packet.Payload());
    // payload+0x00 is the operator new(4) handle the list hangs off; the struct declares
    // it as a dword because every other user of the field moves it as one.
    FRpcEquipWireHandle* const* Slot =
        reinterpret_cast<FRpcEquipWireHandle* const*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));
    Stream.PutDword(Wire->Field_0x08);                                 // / 06d8f8b8
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    Stream.PutByte(Wire->Field_0x04);                                  // / 06d8f8f4
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    if (!RpcPutEquipList(Stream, Slot)) {                              // the routine
        return FRpcMarshalStream::kEncodeOverflow;
    }
    // the routine -- the trailer hook, the no-op.
    *OutLength = Stream.Length();
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

unsigned int RpcDecodeServerRemoteObjectCargoUpdate(FPacketBase& Packet,
                                                    const unsigned char* Buffer,
                                                    unsigned int Size,
                                                    unsigned int* OutLength) {
    FPayloadServerRemoteObjectCargoUpdate* Wire =
        reinterpret_cast<FPayloadServerRemoteObjectCargoUpdate*>(Packet.Payload());
    FRpcEquipWireHandle** Slot =
        reinterpret_cast<FRpcEquipWireHandle**>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    if (Stream.Failed()) {
        return Stream.GetError();
    }
    unsigned int Scratch = 0;
    if (!Stream.GetDword(Scratch)) {                                   // / 06d8f9d6
        return Stream.GetError();
    }
    Wire->Field_0x08 = Scratch;
    unsigned char Flag = 0;
    if (!Stream.GetByte(Flag)) {                                       // / 06d8fa02
        return Stream.GetError();
    }
    Wire->Field_0x04 = Flag;
    if (!RpcGetEquipList(Stream, Slot)) {                              // the routine
        return Stream.GetError();
    }
    // the routine -- the trailer hook.
    *OutLength = Stream.Length();
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}
