// Author: Giperion.

// rpc_packet.h -- the RPC packet base and the packet-type registry, shared by
// rpclocal.dll (src/rpclocal/) and remoteserver.dll (src/RemoteServer/).

// baseline compiles this code into both DLLs: remoteserver's IServerImpl::FireWeapon
// (module storage) allocates through its own serializer at IServerImpl+0x08
// (`MOV ECX,[EBP+0x8]` / CALL the routine), whose body is the same as rpclocal's
// (null out -> 4, (Channel << 16 | Opcode) map lookup, miss -> 0x19,
// else the factory's answer). Member bodies for rpclocal are in
// src/rpclocal/rpc_transport.cpp; nothing here is exported -- each module links its
// own definitions.

// Ground truth: implementation notes, rpclocal.
#pragma once

#include <iosfwd>
#include <map>
#include <string>

class CDAPacket;

// ===========================================================================
// FRpcRefObject -- the refcounted root every packet (and the serializer) derives from.

// Vtable module storage has ONE slot, the scalar deleting dtor the branch, whose dtor
// only stores that vptr back. The use count sits at +0x04; every baseline
// constructor that reaches this base writes 1 there (e.g. 06d9afb3 in FLPACKET_COMMON_
// FIREWEAPON's, 06d95e92 in the serializer's).
// ===========================================================================
class FRpcRefObject {
public:
    FRpcRefObject() : UsageCount(1) {}
    virtual ~FRpcRefObject();                   // / scalar the branch

    // , every instruction:
    // MOV EAX,[ECX+4] / TEST / JBE -> 06d8f4b8 MOV EAX,[ECX+4] / RET
    // DEC / MOV [ECX+4],EAX / 06d8f4ad JNZ -> 06d8f4b8 (re-reads the count)
    // MOV EAX,[ECX] / PUSH 1 / CALL [EAX] -- scalar deleting dtor, delete flag
    // XOR EAX,EAX / RET
    unsigned int Release();

    unsigned int UsageCount;                    // +0x04
};

// ===========================================================================
// FPacketBase -- baseline CDAPacket, exactly 0x20 bytes.

// Vtable module storage = { 06d9c5d0 scalar dtor, 06da13d0 Encode, 06da1470 Decode,
// the shared no-op dump }. Its dtor the routine is `MOV [ECX],06db4fbc ; JMP
// `. Every baseline packet constructor makes the same five base stores
// (e.g. the routine, FLPACKET_SERVER_LOGINRESPONSE):
//     [+0x04] = 1 ; [+0x08] = 0 ; [+0x10] = 0 ; [+0x1c] = 0 ; [+0x0c] = PacketId
// and leaves +0x14 / +0x18 alone.

// The 162 concrete classes derive from this one -- see src/rpclocal/rpc_packet_registry.cpp. The
// payload is theirs and starts at +0x20.

// class CDAPacket in src/include/common_server_types.h is the plain-struct mirror of
// this layout, so a FPacketBase* and a CDAPacket* are interchangeable by cast.
// ===========================================================================
class FPacketBase : public FRpcRefObject {
public:
    explicit FPacketBase(unsigned int InPacketId);

    // --- packet vtable, 4 slots, baseline order ---
    ~FPacketBase() override;                                                   // +0x00 the routine
    // +0x04 -- marshal the payload out for the network transport. baseline's arity is
    // `RET 0xc`, three stack arguments after `this`, and the first is a pointer TO the
    // buffer pointer (06d949dc/06d949e6). baseline never writes that pointer back; the
    // moving cursor is packet+0x18. OutLength gets the byte count, INCLUDING the two
    // header bytes. CDAPacket's own body, 06da13d0, writes the header and nothing else.
    virtual unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                                unsigned int* OutLength);
    // +0x08 -- the inverse. baseline takes the buffer pointer DIRECTLY here
    // (06d934e8/06d93506, no dereference) and its OutLength EXCLUDES the header, which
    // the dispatcher has already consumed. CDAPacket's own body is 06da1470.
    virtual unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                                unsigned int* OutLength);
    // +0x0c dump/trace slot. baseline's arity is `RET 0x4` and the single argument is a
    // std::ostream&: every one of the twelve real bodies pushes it straight into
    // MSVCP60's `std::operator<<(basic_ostream<char>&, char const*)` (IAT entry
    // ). The other 150 types share one no-op body, which ICF folded onto
    // IServerImpl::GetServerStats -- another `RET 0x4` -- so the mangled name in the
    // listing says nothing about what the slot is; see src/rpclocal/rpc_packet_dump.cpp.
    virtual void Dump(std::ostream& Out) const;

    unsigned int GetChannel() const { return PacketId >> 16; }
    unsigned int GetOpcode() const { return PacketId & 0xfff; }

    // Payload starts at +0x20 and belongs to the concrete packet class.
    unsigned char* Payload() { return reinterpret_cast<unsigned char*>(this) + 0x20; }
    const unsigned char* Payload() const { return reinterpret_cast<const unsigned char*>(this) + 0x20; }
    unsigned int* PayloadDwords() { return reinterpret_cast<unsigned int*>(Payload()); }
    const unsigned int* PayloadDwords() const { return reinterpret_cast<const unsigned int*>(Payload()); }

    void AddRef();

    CDAPacket* AsRaw() { return reinterpret_cast<CDAPacket*>(this); }

    // --- layout, mirroring CDAPacket exactly (UsageCount +0x04 is FRpcRefObject's) ---
    // +0x08 -- every baseline packet constructor zeroes it (e.g. 06d9c860's `MOV [EAX+8],0`),
    // but the connection queues do NOT use it: they are MSVC6 std::deque<CDAPacket*> and
    // store the pointer in their own map blocks (see FPacketQueue in src/rpclocal/rpc_transport.h).
    // TODO: no reader of this field has been found anywhere in rpclocal; semantics unknown.
    FPacketBase* Field_0x08;    // +0x08  purpose unproven, baseline only ever writes 0
    unsigned int PacketId;      // +0x0c  Channel << 16 | Opcode
    // These four are the MARSHALLING SCRATCH the codec slots run on -- proven 2026-09-17
    // against the CREATESHIP codec pair. The codecs address them straight off the packet.
    // Encode's entry 0x06d97d20 fills them at 06d97d31..06d97d43 (`LEA EBP,[EBX+0x10]`;
    // +0x00 = 0, +0x04 = the capacity argument, +0x08 = *the buffer-pointer argument,
    // +0x0c = 0), then the routine walks the payload with, per field:
    //     Cursor += width; if (Cursor > Limit) Error = code; if (Error) return code;
    //     *Ptr = value; Ptr += width;
    // (see 06da3733..06da3760). The decoder the routine is the same with the reads and
    // writes swapped and Error code 0x17 instead of 0x16. The constructor zeroes +0x10
    // and +0x1c and leaves +0x14 / +0x18 alone, as baseline's does.
    unsigned int MarshalCursor;   // +0x10  bytes emitted/consumed so far
    unsigned int MarshalLimit;    // +0x14  capacity of the caller's buffer
    unsigned int MarshalPtr;      // +0x18  moving read/write pointer into that buffer
    unsigned int MarshalError;    // +0x1c  0 = ok, 0x16 = encode overflow, 0x17 = decode overrun
};

// The compiler must place the vptr first and pack the seven dwords behind it, so the
// payload really does begin at +0x20 exactly as baseline's does.
static_assert(sizeof(FPacketBase) == 0x20, "FPacketBase must match the baseline CDAPacket base");

// ===========================================================================
// The baseline packet factory -- one per registered class.

// FLPACKET_COMMON_FIREWEAPON's, the routine, is the model for all 162:
// MOV  ESI,dword ptr [ESP + 0x4]   ; the single argument, CDAPacket**
// PUSH 0x40                        ; THE SIZE LIVES HERE, not in the registry
// MOV  dword ptr [ESI],0x0         ; *out = 0 before anything else
// CALL MSVCRT.DLL::operator_new
// JZ   the branch                ; -> MOV EAX,0x5 / RET
// CALL the routine                ; the class ctor, __thiscall, returns this
// MOV  dword ptr [ESI],EAX         ; *out = packet
// XOR  EAX,EAX / 06d9af99 RET      ; 0 = success
// Every one of the 162 ends in a BARE `RET`: __cdecl with one stack argument, which is
// what the caller does at 06d96366 `PUSH EBX` / 06d96367 `CALL dword ptr [EAX]` /
// `ADD ESP,0x4`. The generated body is RpcPacketFactory<T> in
// src/rpclocal/rpc_packet_registry.cpp.
typedef int (*FPacketFactoryFn)(FPacketBase** OutPacket);

// baseline's two return codes, read off 06d9af9a and 06d9af96.
const int kPacketFactoryOk = 0;
const int kPacketFactoryNoMemory = 5;
// 's duplicate-id answer, 06d960e4 `MOV EAX,0x42`.
const int kPacketRegisterDuplicate = 0x42;
// 's two failures: 06d9630d `MOV EAX,0x4` for a null out pointer and
// `MOV EAX,0x19` when the id is not in the map.
const int kPacketAllocateNullOut = 4;
const int kPacketAllocateUnknownType = 0x19;

// ===========================================================================
// FPacketTypeInfo / FPacketSerializer -- the baseline packet-type registry.

// baseline: the routine(Serializer, Channel << 16 | Opcode, "FLPACKET_...", Factory),
// 162 registrations spread over three registrators (the routine channel 1,
// channel 2, the routine channel 3). The serializer object itself lives
// at IServerImpl+0x08 (built by the routine) and at IClient+0x08.

// The table is a std::map keyed on the packet id, at serializer+0x08 (06d960a7
// `LEA ESI,[EDI+0x8]` / 06d960b5 CALL the routine, its lower_bound; the head node is
// [serializer+0xc], 06d960de). The mapped value is a POINTER to a 0x14-byte record
// (06d960f0 `PUSH 0x14` / operator_new) that holds the factory at +0x00 (06d9612b) and
// a MSVCP60 std::string with the name at +0x04 (06d96140 assign). There is no size and
// no counter in it: the counters live in GRpcPacketStats (src/rpclocal/rpc_transport.h).
// ===========================================================================
struct FPacketTypeInfo {
    FPacketFactoryFn Factory;    // +0x00
    std::string Name;            // +0x04
};

// The object itself is an FRpcRefObject: the routine allocates 0x1c bytes (06d95e79),
// stores the use count 1 at +0x04 (06d95e92) and the root vptr 06db3c54 (06d95e99), builds
// the map at +0x08 and finally installs its own vptr 06db3e18 (06d95f01), whose one slot
// is the scalar deleting destructor the routine (CALL the routine / TEST flag,1 /
// operator delete / RET 4). The only way that slot is reached is FRpcRefObject::Release
// (06d8f4b1 PUSH 1 / CALL [EAX]) from IServerImpl::Shutdown (06d8f360).
class FPacketSerializer : public FRpcRefObject {
public:
    FPacketSerializer();
    ~FPacketSerializer() override;                  // / scalar the routine

    // , THREE stack arguments (both returns are `RET 0xc`, 06d960ed
    // and 06d961a2) and no size among them: 0x42 for an id already in the map (06d960e4),
    // otherwise a fresh record inserted and 0 (06d9619c).
    int RegisterType(unsigned int PacketId, const char* Name, FPacketFactoryFn Factory);

    // (Serializer, Channel, Opcode, &OutPacket): 4 for a null out
    // pointer, 0x19 for an id not in the map, otherwise whatever the factory returns.
    int Allocate(unsigned int Channel, unsigned int Opcode, FPacketBase** OutPacket);

    // , reached from IServerImpl::DumpPacketStats and IClient slot 125.
    void DumpStats(const char* Name) const;

private:
    std::map<unsigned int, FPacketTypeInfo*> Types;   // serializer+0x08
};

// Compiled into each Remote DLL: raw lazy cache, no registration or AddRef.
FPacketSerializer* RpcGetRemotePacketSerializer();
