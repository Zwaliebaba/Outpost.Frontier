// Author: Giperion.

// rpc_packet_registry.cpp -- the 162 baseline packet classes and the three registrators.

// GENERATED from rpclocal; every number below is an instruction
// operand, quoted with its address. Regenerate rather than hand-edit.

// THE HIERARCHY (measured 2026-09-24 on the listing and the .rdata vtables):
//   * the refcounted root, vtable 06db3c54 = { the branch }       -> FRpcRefObject
//   * CDAPacket, vtable 06db4fbc = { 06d9c5d0, 06da13d0, 06da1470,
//     no-op dump }                                                  -> FPacketBase
//   * an abstract create-object class, vtable 06db3fb4, slot 0 the purecall stub
// ; its dtor part is inlined into CREATESHIP's and CREATESOLAR's
//                                                                   -> FRpcCreateObjectPacket
//   * an equipment-list class, vtable 06db5e54, dtor the routine, under the five
//     classes whose own dtor is `vptr store ; JMP the routine`     -> FRpcEquipListPacket
//   * the 162 concrete classes, one vtable each                     -> FPacket_<name>

// FACTORIES. All 162 have the same 19-instruction body, e.g. the routine:
// MOV [ESI],0 ; 06d9af75 PUSH 0x40 ; CALL operator_new ; 06d9af87 JZ -> MOV EAX,5 ;
// CALL ctor ; 06d9af94 MOV [ESI],EAX ; 06d9af96 XOR EAX,EAX ; RET
// -- __cdecl, one argument, no null test on it. RpcPacketFactory<T> below is that body.

// CONSTRUCTORS store exactly what the baseline ctor stores and nothing else: baseline's
// operator new does not zero, so neither does the port. The CDAPacket stores (+0x04 = 1,
// +0x08, +0x10, +0x1c = 0, +0x0c = id) are FRpcRefObject's and FPacketBase's; the rest
// is listed per class in the order the listing makes them. A byte the listing copies out
// of an uninitialised stack slot -- the empty allocator object of an MSVCP60 container,
// or the top byte of a pushed register -- is stored as 0; nothing ever reads it.

// DESTRUCTORS are the baseline bodies, [ESI+n] for [ESI+n]; the implicit call to
// ~FPacketBase at the end is baseline's closing `JMP the routine`. A class without one
// shares baseline's scalar dtor 06d9af50, whose dtor is the thunk 06da11e0 -> 06da1370.

// ENCODE / DECODE / DUMP are the class's own vtable slots, forwarding to the codec bodies
// in rpc_wire_*.cpp / rpc_containers.cpp / rpc_packet_dump.cpp. Five classes keep
// CDAPacket's own 06da13d0 / 06da1470, and 150 keep the no-op dump.

#include "rpc_transport.h"
#include "rpc_containers.h"
#include "rpc_payloads.h"
#include "common_misc.h"        // SUBOBJ_ID_NONE, the default of three word fields

#include <cstdlib>
#include <new>
#include <ostream>

namespace {

// A field at a packet-relative offset, exactly as the baseline bodies address it.
template<typename T>
inline T& RpcPacketField(FPacketBase* Packet, unsigned int Offset) {
    return *reinterpret_cast<T*>(reinterpret_cast<unsigned char*>(Packet) + Offset);
}

// One node of an MSVCP60 std::list<T*> embedded in a packet: { _Next, _Prev, _Myval }.
struct FRpcPacketListNode {
    FRpcPacketListNode* Next;   // +0x00
    FRpcPacketListNode* Prev;   // +0x04
    void* Value;                // +0x08
};

// The 19-instruction factory every class registers; see the header comment.
template<typename T>
int RpcPacketFactory(FPacketBase** OutPacket) {
    *OutPacket = nullptr;                               // MOV dword ptr [ESI],0x0
    T* Packet = new (std::nothrow) T;                   // PUSH size ; CALL operator_new
    if (Packet == nullptr) {
        return kPacketFactoryNoMemory;                  // MOV EAX,0x5
    }
    *OutPacket = Packet;                                // MOV dword ptr [ESI],EAX
    return kPacketFactoryOk;                            // XOR EAX,EAX
}

// Vtable 06db5e54 { 06da26f0, ... }: the five classes that carry one equipment handle
// POINTER at +0x20. baseline folds its constructor into the derived ones.
class FRpcEquipListPacket : public FPacketBase {
protected:
    explicit FRpcEquipListPacket(unsigned int InPacketId) : FPacketBase(InPacketId) {}
public:

    ~FRpcEquipListPacket() override {
        FRpcEquipWireHandle* Handle = RpcPacketField<FRpcEquipWireHandle*>(this, 0x20u);
        if (Handle != nullptr) {                        // TEST / 06da2201 JZ
            RpcEquipHandleDestroy(Handle);              // CALL the routine
            ::operator delete(Handle);                  // CALL the routine
        }
    }
    unsigned char EquipBody[0x04];   // +0x20 the handle pointer
};

// Vtable 06db3fb4, slot 0 the purecall stub the branch: abstract. CREATESHIP and
// CREATESOLAR derive from it. Its constructor and destructor parts are inlined into both
// concrete ones; what they build and tear down is the create-object header at +0x20,
// which the port holds as the first member of each class's payload,
// FRpcCreateObjectWire (rpc_containers.h). That type's constructor and destructor ARE
// this class's parts: the head stores 06d9cca2..06d9ccd2, the COMMON.DLL list ctor
// and DamageList ctor 06d9ccf3, and -- after 06d97bf9 / 06d9e78a store this
// vptr -- the DamageList (06d97bff), the list (06d97c54), the handle (06d97c94) and the
// id block (06d97ca7). Held as a real member, the compiler also emits the EH unwind baseline
// has for it (FuncInfo_06db70a4, states 1..3). The destruction order is unchanged:
// derived body, then the payload member (header last), then FPacketBase.
class FRpcCreateObjectPacket : public FPacketBase {
protected:
    explicit FRpcCreateObjectPacket(unsigned int InPacketId) : FPacketBase(InPacketId) {}
public:
    ~FRpcCreateObjectPacket() override = 0;
};

// / 06d9e78a: the vptr store is all that is left of this part; the members it
// destroys belong to FRpcCreateObjectWire and are gone by the time this body runs.
FRpcCreateObjectPacket::~FRpcCreateObjectPacket() {
}

static_assert(sizeof(FRpcEquipListPacket) == 0x24u, "equipment-list packets are 0x24 bytes");
static_assert(sizeof(FRpcCreateObjectPacket) == 0x20u,
              "the create-object header is the payload's first member, at +0x20");

// ===========================================================================
// Channel 1 -- the classes registered by the routine, in its registration order.
// ===========================================================================

// FLPACKET_COMMON_FIREWEAPON -- id 0x10002, registered at 06d9aced.
// Factory the routine: operator new(0x40) + ctor the routine. Vtable module storage =
// { 06d9b000, 06da4f60, 06da5550, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_COMMON_FIREWEAPON : public FPacketBase {
public:
    FPacket_FLPACKET_COMMON_FIREWEAPON() : FPacketBase(0x10002u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 4, 0x7f7fffffu);
        RpcPacketStoreField(this, 0x28u, 4, 0x7f7fffffu);
        RpcPacketStoreField(this, 0x2cu, 4, 0x7f7fffffu);
        RpcPacketStoreField(this, 0x30u, 1, 0u);   // byte copied from an uninitialised stack slot
        RpcPacketStoreField(this, 0x34u, 4, 0u);
        RpcPacketStoreField(this, 0x38u, 4, 0u);
        RpcPacketStoreField(this, 0x3cu, 4, 0u);
    }

    ~FPacket_FLPACKET_COMMON_FIREWEAPON() override {
        // No test: 06d9b023 MOV EAX,[ESI+0x34] / 06d9b027 CALL free, then the three
        // dwords of the hardpoint vector cleared (06d9b031..06d9b037).
        ::operator delete(RpcPacketField<void*>(this, 0x34u));
        RpcPacketField<unsigned int>(this, 0x34u) = 0;
        RpcPacketField<unsigned int>(this, 0x38u) = 0;
        RpcPacketField<unsigned int>(this, 0x3cu) = 0;
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeCommonFireWeapon(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeCommonFireWeapon(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x20];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_COMMON_FIREWEAPON) == 0x40u, "FLPACKET_COMMON_FIREWEAPON is operator new(0x40)");

// FLPACKET_COMMON_UPDATEOBJECT -- id 0x10001, registered at 06d9ad03.
// Factory the routine: operator new(0x4c) + ctor the routine. Vtable module storage =
// { 06d9af50, 06da49e0, 06da4ca0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_COMMON_UPDATEOBJECT : public FPacketBase {
public:
    FPacket_FLPACKET_COMMON_UPDATEOBJECT() : FPacketBase(0x10001u) {
        RpcPacketStoreField(this, 0x28u, 4, 0u);
        RpcPacketStoreField(this, 0x2cu, 4, 0u);
        RpcPacketStoreField(this, 0x30u, 4, 0u);
        RpcPacketStoreField(this, 0x34u, 4, 0x3f800000u);
        RpcPacketStoreField(this, 0x38u, 4, 0u);
        RpcPacketStoreField(this, 0x3cu, 4, 0u);
        RpcPacketStoreField(this, 0x40u, 4, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeCommonUpdateObject(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeCommonUpdateObject(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x2c];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_COMMON_UPDATEOBJECT) == 0x4cu, "FLPACKET_COMMON_UPDATEOBJECT is operator new(0x4c)");

// FLPACKET_COMMON_CHATMSG -- id 0x10005, registered at 06d9ad19.
// Factory the routine: operator new(0x30) + ctor the routine. Vtable module storage =
// { 06d9b7a0, 06da6420, 06da65d0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_COMMON_CHATMSG : public FPacketBase {
public:
    FPacket_FLPACKET_COMMON_CHATMSG() : FPacketBase(0x10005u) {
        RpcPacketStoreField(this, 0x2cu, 4, 0u);
        RpcPacketStoreField(this, 0x28u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 4, 0u);
        RpcPacketStoreField(this, 0x20u, 4, 0u);
    }

    ~FPacket_FLPACKET_COMMON_CHATMSG() override {
        if (RpcPacketField<void*>(this, 0x2cu) != nullptr) {
            ::operator delete(RpcPacketField<void*>(this, 0x2cu));
        }
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeCommonChatMsg(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeCommonChatMsg(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x10];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_COMMON_CHATMSG) == 0x30u, "FLPACKET_COMMON_CHATMSG is operator new(0x30)");

// FLPACKET_COMMON_ACTIVATEEQUIP -- id 0x10008, registered at 06d9ad2f.
// Factory the routine: operator new(0x28) + ctor the routine. Vtable module storage =
// { 06d9af50, 06da67a0, 06da68f0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_COMMON_ACTIVATEEQUIP : public FPacketBase {
public:
    FPacket_FLPACKET_COMMON_ACTIVATEEQUIP() : FPacketBase(0x10008u) {
        RpcPacketStoreField(this, 0x26u, 2, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeCommonActivateEquip(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeCommonActivateEquip(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x8];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_COMMON_ACTIVATEEQUIP) == 0x28u, "FLPACKET_COMMON_ACTIVATEEQUIP is operator new(0x28)");

// FLPACKET_COMMON_SETTARGET -- id 0x10004, registered at 06d9ad45.
// Factory the routine: operator new(0x2c) + ctor the routine. Vtable module storage =
// { 06d9af50, 06da8b50, 06da8cd0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_COMMON_SETTARGET : public FPacketBase {
public:
    FPacket_FLPACKET_COMMON_SETTARGET() : FPacketBase(0x10004u) {
        RpcPacketStoreField(this, 0x28u, 2, 0u);
        RpcPacketStoreField(this, 0x2au, 2, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDwordDwordWordWord(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDwordDwordWordWord(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0xc];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_COMMON_SETTARGET) == 0x2cu, "FLPACKET_COMMON_SETTARGET is operator new(0x2c)");

// FLPACKET_COMMON_ACTIVATECRUISE -- id 0x1000e, registered at 06d9ad5b.
// Factory the routine: operator new(0x28) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db1b60, 06da6a50, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_COMMON_ACTIVATECRUISE : public FPacketBase {
public:
    FPacket_FLPACKET_COMMON_ACTIVATECRUISE() : FPacketBase(0x1000eu) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDwordTwoBytes(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDwordTwoBytes(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x8];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_COMMON_ACTIVATECRUISE) == 0x28u, "FLPACKET_COMMON_ACTIVATECRUISE is operator new(0x28)");

// FLPACKET_COMMON_ACTIVATETHRUSTERS -- id 0x10015, registered at 06d9ad71.
// Factory the routine: operator new(0x28) + ctor the routine. Vtable module storage =
// { 06d9af50, 06dab0e0, 06daf230, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_COMMON_ACTIVATETHRUSTERS : public FPacketBase {
public:
    FPacket_FLPACKET_COMMON_ACTIVATETHRUSTERS() : FPacketBase(0x10015u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDwordByte(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDwordByte(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x8];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_COMMON_ACTIVATETHRUSTERS) == 0x28u, "FLPACKET_COMMON_ACTIVATETHRUSTERS is operator new(0x28)");

// FLPACKET_COMMON_GOTRADELANE -- id 0x1000f, registered at 06d9ad87.
// Factory the routine: operator new(0x30) + ctor the routine. Vtable module storage =
// { 06d9af50, 06da6b80, 06da6cf0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_COMMON_GOTRADELANE : public FPacketBase {
public:
    FPacket_FLPACKET_COMMON_GOTRADELANE() : FPacketBase(0x1000fu) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeCommonGoTradelane(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeCommonGoTradelane(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x10];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_COMMON_GOTRADELANE) == 0x30u, "FLPACKET_COMMON_GOTRADELANE is operator new(0x30)");

// FLPACKET_COMMON_STOPTRADELANE -- id 0x10010, registered at 06d9ad9d.
// Factory the routine: operator new(0x2c) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db0160, 06da6e40, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_COMMON_STOPTRADELANE : public FPacketBase {
public:
    FPacket_FLPACKET_COMMON_STOPTRADELANE() : FPacketBase(0x10010u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 4, 0u);
        RpcPacketStoreField(this, 0x28u, 4, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireThreeDwords(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireThreeDwords(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0xc];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_COMMON_STOPTRADELANE) == 0x2cu, "FLPACKET_COMMON_STOPTRADELANE is operator new(0x2c)");

// FLPACKET_COMMON_SET_WEAPON_GROUP -- id 0x10011, registered at 06d9adb3.
// Factory the routine: operator new(0x2c) + ctor the routine. Vtable module storage =
// { 06d9b0c0, 06da6190, 06da5bc0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_COMMON_SET_WEAPON_GROUP : public FPacketBase {
public:
    FPacket_FLPACKET_COMMON_SET_WEAPON_GROUP() : FPacketBase(0x10011u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x28u, 1, 0u);
    }

    ~FPacket_FLPACKET_COMMON_SET_WEAPON_GROUP() override {
        // the byte at +0x28 says whether the packet owns the block at +0x20 -- the
        // IClient-side sender sets it on the CALLER's pointer, the IServerImpl-side one on
        // its own copy, the wire decoder on its own allocation.
        if (RpcPacketField<unsigned char>(this, 0x28u) != 0 &&
            RpcPacketField<void*>(this, 0x20u) != nullptr) {
            ::operator delete(RpcPacketField<void*>(this, 0x20u));
            RpcPacketField<void*>(this, 0x20u) = nullptr;
        }
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireBlobBufferFirst(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireOwnedString(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0xc];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_COMMON_SET_WEAPON_GROUP) == 0x2cu, "FLPACKET_COMMON_SET_WEAPON_GROUP is operator new(0x2c)");

// FLPACKET_COMMON_PLAYER_TRADE -- id 0x10012, registered at 06d9adc9.
// Factory the routine: operator new(0x44) + ctor the routine. Vtable module storage =
// { 06d9af50, 06da7030, 06da7430, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_COMMON_PLAYER_TRADE : public FPacketBase {
public:
    FPacket_FLPACKET_COMMON_PLAYER_TRADE() : FPacketBase(0x10012u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 4, 0u);
        RpcPacketStoreField(this, 0x28u, 4, 0u);
        RpcPacketStoreField(this, 0x34u, 4, 0xffffffffu);
        RpcPacketStoreField(this, 0x38u, 4, 0u);
        RpcPacketStoreField(this, 0x3cu, 4, 0u);
        RpcPacketStoreField(this, 0x2cu, 4, 0u);
        RpcPacketStoreField(this, 0x30u, 1, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeCommonPlayerTrade(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeCommonPlayerTrade(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x24];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_COMMON_PLAYER_TRADE) == 0x44u, "FLPACKET_COMMON_PLAYER_TRADE is operator new(0x44)");

// FLPACKET_COMMON_SET_VISITED_STATE -- id 0x10013, registered at 06d9addf.
// Factory the routine: operator new(0x2c) + ctor the routine. Vtable module storage =
// { 06d9b180, 06da6190, 06da62d0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_COMMON_SET_VISITED_STATE : public FPacketBase {
public:
    FPacket_FLPACKET_COMMON_SET_VISITED_STATE() : FPacketBase(0x10013u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x28u, 1, 0u);
    }

    ~FPacket_FLPACKET_COMMON_SET_VISITED_STATE() override {
        // the byte at +0x28 says whether the packet owns the block at +0x20 -- the
        // IClient-side sender sets it on the CALLER's pointer, the IServerImpl-side one on
        // its own copy, the wire decoder on its own allocation.
        if (RpcPacketField<unsigned char>(this, 0x28u) != 0 &&
            RpcPacketField<void*>(this, 0x20u) != nullptr) {
            ::operator delete(RpcPacketField<void*>(this, 0x20u));
            RpcPacketField<void*>(this, 0x20u) = nullptr;
        }
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireBlobBufferFirst(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireOwnedBuffer(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0xc];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_COMMON_SET_VISITED_STATE) == 0x2cu, "FLPACKET_COMMON_SET_VISITED_STATE is operator new(0x2c)");

// FLPACKET_COMMON_JETTISONCARGO -- id 0x10014, registered at 06d9adf5.
// Factory the routine: operator new(0x2c) + ctor the routine. Vtable module storage =
// { 06d9af50, 06dae620, 06da78d0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_COMMON_JETTISONCARGO : public FPacketBase {
public:
    FPacket_FLPACKET_COMMON_JETTISONCARGO() : FPacketBase(0x10014u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 2, 0u);
        RpcPacketStoreField(this, 0x28u, 4, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDwordWordDword(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDwordWordDword(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0xc];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_COMMON_JETTISONCARGO) == 0x2cu, "FLPACKET_COMMON_JETTISONCARGO is operator new(0x2c)");

// FLPACKET_COMMON_REQUEST_BEST_PATH -- id 0x10016, registered at 06d9ae0b.
// Factory the routine: operator new(0x2c) + ctor the routine. Vtable module storage =
// { 06d9b240, 06da6190, 06da62d0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_COMMON_REQUEST_BEST_PATH : public FPacketBase {
public:
    FPacket_FLPACKET_COMMON_REQUEST_BEST_PATH() : FPacketBase(0x10016u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x28u, 1, 0u);
    }

    ~FPacket_FLPACKET_COMMON_REQUEST_BEST_PATH() override {
        // the byte at +0x28 says whether the packet owns the block at +0x20 -- the
        // IClient-side sender sets it on the CALLER's pointer, the IServerImpl-side one on
        // its own copy, the wire decoder on its own allocation.
        if (RpcPacketField<unsigned char>(this, 0x28u) != 0 &&
            RpcPacketField<void*>(this, 0x20u) != nullptr) {
            ::operator delete(RpcPacketField<void*>(this, 0x20u));
            RpcPacketField<void*>(this, 0x20u) = nullptr;
        }
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireBlobBufferFirst(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireOwnedBuffer(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0xc];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_COMMON_REQUEST_BEST_PATH) == 0x2cu, "FLPACKET_COMMON_REQUEST_BEST_PATH is operator new(0x2c)");

// FLPACKET_COMMON_REQUEST_PLAYER_STATS -- id 0x10018, registered at 06d9ae21.
// Factory the routine: operator new(0x2c) + ctor the routine. Vtable module storage =
// { 06d9b300, 06da6190, 06da62d0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_COMMON_REQUEST_PLAYER_STATS : public FPacketBase {
public:
    FPacket_FLPACKET_COMMON_REQUEST_PLAYER_STATS() : FPacketBase(0x10018u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x28u, 1, 0u);
    }

    ~FPacket_FLPACKET_COMMON_REQUEST_PLAYER_STATS() override {
        // the byte at +0x28 says whether the packet owns the block at +0x20 -- the
        // IClient-side sender sets it on the CALLER's pointer, the IServerImpl-side one on
        // its own copy, the wire decoder on its own allocation.
        if (RpcPacketField<unsigned char>(this, 0x28u) != 0 &&
            RpcPacketField<void*>(this, 0x20u) != nullptr) {
            ::operator delete(RpcPacketField<void*>(this, 0x20u));
            RpcPacketField<void*>(this, 0x20u) = nullptr;
        }
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireBlobBufferFirst(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireOwnedBuffer(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0xc];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_COMMON_REQUEST_PLAYER_STATS) == 0x2cu, "FLPACKET_COMMON_REQUEST_PLAYER_STATS is operator new(0x2c)");

// FLPACKET_COMMON_POP_UP_DIALOG -- id 0x1001b, registered at 06d9ae37.
// Factory the routine: operator new(0x34) + ctor the routine. Vtable module storage =
// { 06d9b3d0, 06da5de0, 06da5fe0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_COMMON_POP_UP_DIALOG : public FPacketBase {
public:
    FPacket_FLPACKET_COMMON_POP_UP_DIALOG() : FPacketBase(0x1001bu) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 4, 0u);
        RpcPacketStoreField(this, 0x28u, 4, 0u);
        RpcPacketStoreField(this, 0x2cu, 4, 0u);
        RpcPacketStoreField(this, 0x30u, 4, 0u);
    }

    ~FPacket_FLPACKET_COMMON_POP_UP_DIALOG() override {
        // baseline frees +0x2c FIRST and +0x24 second.
        if (RpcPacketField<void*>(this, 0x2cu) != nullptr) {
            ::operator delete(RpcPacketField<void*>(this, 0x2cu));
        }
        if (RpcPacketField<void*>(this, 0x24u) != nullptr) {
            ::operator delete(RpcPacketField<void*>(this, 0x24u));
        }
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeCommonPopUpDialog(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeCommonPopUpDialog(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x14];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_COMMON_POP_UP_DIALOG) == 0x34u, "FLPACKET_COMMON_POP_UP_DIALOG is operator new(0x34)");

// FLPACKET_COMMON_REQUEST_GROUP_POSITIONS -- id 0x10017, registered at 06d9ae4d.
// Factory the routine: operator new(0x2c) + ctor the routine. Vtable module storage =
// { 06d9b4a0, 06da6190, 06da62d0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_COMMON_REQUEST_GROUP_POSITIONS : public FPacketBase {
public:
    FPacket_FLPACKET_COMMON_REQUEST_GROUP_POSITIONS() : FPacketBase(0x10017u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x28u, 1, 0u);
    }

    ~FPacket_FLPACKET_COMMON_REQUEST_GROUP_POSITIONS() override {
        // the byte at +0x28 says whether the packet owns the block at +0x20 -- the
        // IClient-side sender sets it on the CALLER's pointer, the IServerImpl-side one on
        // its own copy, the wire decoder on its own allocation.
        if (RpcPacketField<unsigned char>(this, 0x28u) != 0 &&
            RpcPacketField<void*>(this, 0x20u) != nullptr) {
            ::operator delete(RpcPacketField<void*>(this, 0x20u));
            RpcPacketField<void*>(this, 0x20u) = nullptr;
        }
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireBlobBufferFirst(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireOwnedBuffer(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0xc];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_COMMON_REQUEST_GROUP_POSITIONS) == 0x2cu, "FLPACKET_COMMON_REQUEST_GROUP_POSITIONS is operator new(0x2c)");

// FLPACKET_COMMON_SET_MISSION_LOG -- id 0x10019, registered at 06d9ae63.
// Factory the routine: operator new(0x2c) + ctor the routine. Vtable module storage =
// { 06d9b560, 06da6190, 06da62d0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_COMMON_SET_MISSION_LOG : public FPacketBase {
public:
    FPacket_FLPACKET_COMMON_SET_MISSION_LOG() : FPacketBase(0x10019u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x28u, 1, 0u);
    }

    ~FPacket_FLPACKET_COMMON_SET_MISSION_LOG() override {
        // the byte at +0x28 says whether the packet owns the block at +0x20 -- the
        // IClient-side sender sets it on the CALLER's pointer, the IServerImpl-side one on
        // its own copy, the wire decoder on its own allocation.
        if (RpcPacketField<unsigned char>(this, 0x28u) != 0 &&
            RpcPacketField<void*>(this, 0x20u) != nullptr) {
            ::operator delete(RpcPacketField<void*>(this, 0x20u));
            RpcPacketField<void*>(this, 0x20u) = nullptr;
        }
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireBlobBufferFirst(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireOwnedBuffer(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0xc];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_COMMON_SET_MISSION_LOG) == 0x2cu, "FLPACKET_COMMON_SET_MISSION_LOG is operator new(0x2c)");

// FLPACKET_COMMON_REQUEST_RANK_LEVEL -- id 0x1001a, registered at 06d9ae79.
// Factory the routine: operator new(0x2c) + ctor the routine. Vtable module storage =
// { 06d9b6e0, 06da6190, 06da62d0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_COMMON_REQUEST_RANK_LEVEL : public FPacketBase {
public:
    FPacket_FLPACKET_COMMON_REQUEST_RANK_LEVEL() : FPacketBase(0x1001au) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x28u, 1, 0u);
    }

    ~FPacket_FLPACKET_COMMON_REQUEST_RANK_LEVEL() override {
        // the byte at +0x28 says whether the packet owns the block at +0x20 -- the
        // IClient-side sender sets it on the CALLER's pointer, the IServerImpl-side one on
        // its own copy, the wire decoder on its own allocation.
        if (RpcPacketField<unsigned char>(this, 0x28u) != 0 &&
            RpcPacketField<void*>(this, 0x20u) != nullptr) {
            ::operator delete(RpcPacketField<void*>(this, 0x20u));
            RpcPacketField<void*>(this, 0x20u) = nullptr;
        }
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireBlobBufferFirst(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireOwnedBuffer(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0xc];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_COMMON_REQUEST_RANK_LEVEL) == 0x2cu, "FLPACKET_COMMON_REQUEST_RANK_LEVEL is operator new(0x2c)");

// FLPACKET_COMMON_SET_INTERFACE_STATE -- id 0x1001c, registered at 06d9ae8f.
// Factory the routine: operator new(0x2c) + ctor the routine. Vtable module storage =
// { 06d9b620, 06da6190, 06da62d0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_COMMON_SET_INTERFACE_STATE : public FPacketBase {
public:
    FPacket_FLPACKET_COMMON_SET_INTERFACE_STATE() : FPacketBase(0x1001cu) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x28u, 1, 0u);
    }

    ~FPacket_FLPACKET_COMMON_SET_INTERFACE_STATE() override {
        // the byte at +0x28 says whether the packet owns the block at +0x20 -- the
        // IClient-side sender sets it on the CALLER's pointer, the IServerImpl-side one on
        // its own copy, the wire decoder on its own allocation.
        if (RpcPacketField<unsigned char>(this, 0x28u) != 0 &&
            RpcPacketField<void*>(this, 0x20u) != nullptr) {
            ::operator delete(RpcPacketField<void*>(this, 0x20u));
            RpcPacketField<void*>(this, 0x20u) = nullptr;
        }
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireBlobBufferFirst(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireOwnedBuffer(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0xc];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_COMMON_SET_INTERFACE_STATE) == 0x2cu, "FLPACKET_COMMON_SET_INTERFACE_STATE is operator new(0x2c)");

// FLPACKET_COMMON_TRACTOROBJECTS -- id 0x1001d, registered at 06d9aea5.
// Factory the routine: operator new(0x38) + ctor the routine. Vtable module storage =
// { 06d9bc00, 06da7a00, 06da7b90, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_COMMON_TRACTOROBJECTS : public FPacketBase {
public:
    FPacket_FLPACKET_COMMON_TRACTOROBJECTS() : FPacketBase(0x1001du) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 2, 0u);
        RpcPacketStoreField(this, 0x28u, 1, 0u);   // byte copied from an uninitialised stack slot
        RpcPacketStoreField(this, 0x2cu, 4, 0u);
        RpcPacketStoreField(this, 0x30u, 4, 0u);
        RpcPacketStoreField(this, 0x34u, 4, 0u);
    }

    ~FPacket_FLPACKET_COMMON_TRACTOROBJECTS() override {
        // No test: 06d9bc23 / 06d9bc27 CALL free, then the three vector dwords cleared.
        ::operator delete(RpcPacketField<void*>(this, 0x2cu));
        RpcPacketField<unsigned int>(this, 0x2cu) = 0;
        RpcPacketField<unsigned int>(this, 0x30u) = 0;
        RpcPacketField<unsigned int>(this, 0x34u) = 0;
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeCommonTractorObjects(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeCommonTractorObjects(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x18];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_COMMON_TRACTOROBJECTS) == 0x38u, "FLPACKET_COMMON_TRACTOROBJECTS is operator new(0x38)");

// ===========================================================================
// Channel 2 -- the classes registered by the routine, in its registration order.
// ===========================================================================

// FLPACKET_SERVER_CONNECTRESPONSE -- id 0x20001, registered at 06d9bc8d.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db0310, 06db03f0, 06da8240 }.
class FPacket_FLPACKET_SERVER_CONNECTRESPONSE : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_CONNECTRESPONSE() : FPacketBase(0x20001u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDword(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDword(*this, Buffer, Size, OutLength);
    }
    void Dump(std::ostream& Out) const override {
        RpcDumpServerConnectResponse(*this, Out);
    }
    unsigned char Body[0x4];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_CONNECTRESPONSE) == 0x24u, "FLPACKET_SERVER_CONNECTRESPONSE is operator new(0x24)");

// FLPACKET_SERVER_LOGINRESPONSE -- id 0x20002, registered at 06d9bca3.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d9af50, 06da82e0, 06da83c0, 06da84a0 }.
class FPacket_FLPACKET_SERVER_LOGINRESPONSE : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_LOGINRESPONSE() : FPacketBase(0x20002u) {
        RpcPacketStoreField(this, 0x20u, 1, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireByte(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireByte(*this, Buffer, Size, OutLength);
    }
    void Dump(std::ostream& Out) const override {
        RpcDumpServerLoginResponse(*this, Out);
    }
    unsigned char Body[0x4];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_LOGINRESPONSE) == 0x24u, "FLPACKET_SERVER_LOGINRESPONSE is operator new(0x24)");

// FLPACKET_SERVER_DAMAGEOBJECT -- id 0x20005, registered at 06d9bcb9.
// Factory the routine: operator new(0x40) + ctor the routine. Vtable module storage =
// { 06d909b0, 06d90480, 06d906c0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_DAMAGEOBJECT : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_DAMAGEOBJECT() : FPacketBase(0x20005u) {
    }

    ~FPacket_FLPACKET_SERVER_DAMAGEOBJECT() override {
        // is the DamageList destructor inlined: every node erased with
        // --Size, the sentinel freed, Head and Size cleared. The payload holds Common.dll's
        // DamageList itself, whose destructor runs after this body.
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeServerDamageObject(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeServerDamageObject(*this, Buffer, Size, OutLength);
    }
    FPayloadServerDamageObject Wire;   // +0x20; Field_0x00 is left alone, as baseline's ctor does
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_DAMAGEOBJECT) == 0x20u + sizeof(FPayloadServerDamageObject),
              "FLPACKET_SERVER_DAMAGEOBJECT: the payload member starts at +0x20");

// FLPACKET_SERVER_DESTROYOBJECT -- id 0x20006, registered at 06d9bccf.
// Factory the routine: operator new(0x28) + ctor the routine. Vtable module storage =
// { 06d9af50, 06dab0e0, 06daf230, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_DESTROYOBJECT : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_DESTROYOBJECT() : FPacketBase(0x20006u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 1, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDwordByte(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDwordByte(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x8];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_DESTROYOBJECT) == 0x28u, "FLPACKET_SERVER_DESTROYOBJECT is operator new(0x28)");

// FLPACKET_SERVER_CREATESHIP -- id 0x20004, registered at 06d9bce5.
// Factory the routine: operator new(0xec) + ctor the routine. Vtable module storage =
// { 06d98640, 06d97d20, 06d98250, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_CREATESHIP : public FRpcCreateObjectPacket {
public:
    // . Wire.Common is built first, by FRpcCreateObjectWire's constructors
    // (06d9cca2..06d9ccf3: the head, then COMMON.DLL's std::list<CollisionGroupDesc> ctor
    // at +0x74 and DamageList ctor at +0x80); the body is the tail, 06d9ccfd..06d9cd77.
    FPacket_FLPACKET_SERVER_CREATESHIP() : FRpcCreateObjectPacket(0x20004u) {
        Wire.Field_0x7c = 0;              // byte +0x9c
        Wire.Field_0x80 = 0;              // +0xa0
        Wire.Field_0x84 = 0.0f;           // +0xa4 (integer 0 == +0.0f)
        Wire.Field_0x88 = 0.0f;           // +0xa8
        Wire.Field_0x8c = 0;              // +0xac
        Wire.Field_0x90 = 0.0f;           // +0xb0
        Wire.Field_0x94 = 0.0f;           // +0xb4
        Wire.Field_0x98 = 0.0f;           // +0xb8
        Wire.Field_0x9c = 0;              // +0xbc
        Wire.Field_0xa0 = 0;              // word +0xc0
        Wire.Field_0xa2 = 0;              // byte +0xc2
        Wire.Field_0xa4 = 0;              // word +0xc4
        Wire.NameASize = 0;               // +0xc8
        Wire.NameABuffer = nullptr;       // +0xcc
        Wire.NameBSize = 0;               // +0xd0
        Wire.NameBBuffer = nullptr;       // +0xd4
        Wire.DisplayNameLength = 0;       // +0xd8
        Wire.DisplayName = nullptr;       // +0xdc
        Wire.Affiliation = 0;             // +0xe0
        Wire.Attitude = 0.0f;             // +0xe4
        Wire.Field_0xc8 = 0;              // byte +0xe8
    }

    ~FPacket_FLPACKET_SERVER_CREATESHIP() override {
        if (Wire.DisplayName != nullptr) {                     // (+0xdc)
            ::operator delete(Wire.DisplayName);
            Wire.DisplayName = nullptr;
        }
        if (Wire.NameBBuffer != nullptr) {                     // (+0xd4)
            ::operator delete(Wire.NameBBuffer);
        }
        if (Wire.NameABuffer != nullptr) {                     // (+0xcc)
            ::operator delete(Wire.NameABuffer);
        }
        // stores the abstract base vptr 06db3fb4; Wire.Common's destructor
        // (06d97bff..06d97cb1) and ~FPacketBase follow.
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeServerCreateShip(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeServerCreateShip(*this, Buffer, Size, OutLength);
    }
    FPayloadServerCreateShip Wire;   // +0x20; baseline's class is operator new(0xec)
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_CREATESHIP) == 0x20u + sizeof(FPayloadServerCreateShip),
              "FLPACKET_SERVER_CREATESHIP: the payload member starts at +0x20");

// FLPACKET_SERVER_REQUEST_RETURNED -- id 0x20044, registered at 06d9bcfb.
// Factory the routine: operator new(0x28) + ctor the routine. Vtable module storage =
// { 06d9af50, 06daa820, 06daa990, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_REQUEST_RETURNED : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_REQUEST_RETURNED() : FPacketBase(0x20044u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeServerRequestReturned(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeServerRequestReturned(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x8];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_REQUEST_RETURNED) == 0x28u, "FLPACKET_SERVER_REQUEST_RETURNED is operator new(0x28)");

// FLPACKET_SERVER_CHARACTERINFO -- id 0x20003, registered at 06d9bd11.
// Factory the routine: operator new(0x30) + ctor the routine. Vtable module storage =
// { 06d95b70, 06d949d0, 06d934d0, 06d93310 }.
class FPacket_FLPACKET_SERVER_CHARACTERINFO : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_CHARACTERINFO() : FPacketBase(0x20003u) {
        RpcPacketBuildListSentinel(this, 0x24u, 0x0cu);   // operator new(0xc), Head
    }

    ~FPacket_FLPACKET_SERVER_CHARACTERINFO() override {
        // every record first (two id blocks, the collision list, the
        // equipment handle, the two strings, the record), then 06d93472..06d934ab the
        // nodes through the routine and the sentinel.
        RpcCharacterInfoWireListRelease(
            reinterpret_cast<FRpcCharacterInfoWireList*>(Payload() + 0x04));
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeServerCharacterInfo(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeServerCharacterInfo(*this, Buffer, Size, OutLength);
    }
    void Dump(std::ostream& Out) const override {
        RpcDumpServerCharacterInfo(*this, Out);
    }
    unsigned char Body[0x10];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_CHARACTERINFO) == 0x30u, "FLPACKET_SERVER_CHARACTERINFO is operator new(0x30)");

// FLPACKET_SERVER_LAUNCH -- id 0x20007, registered at 06d9bd27.
// Factory the routine: operator new(0x48) + ctor the routine. Vtable module storage =
// { 06d9af50, 06daab40, 06daad40, 06daaee0 }.
class FPacket_FLPACKET_SERVER_LAUNCH : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_LAUNCH() : FPacketBase(0x20007u) {
        RpcPacketStoreField(this, 0x2cu, 4, 0u);
        RpcPacketStoreField(this, 0x30u, 4, 0u);
        RpcPacketStoreField(this, 0x34u, 4, 0u);
        RpcPacketStoreField(this, 0x38u, 4, 0x3f800000u);
        RpcPacketStoreField(this, 0x3cu, 4, 0u);
        RpcPacketStoreField(this, 0x40u, 4, 0u);
        RpcPacketStoreField(this, 0x44u, 4, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeServerLaunch(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeServerLaunch(*this, Buffer, Size, OutLength);
    }
    void Dump(std::ostream& Out) const override {
        RpcDumpServerLaunch(*this, Out);
    }
    unsigned char Body[0x28];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_LAUNCH) == 0x48u, "FLPACKET_SERVER_LAUNCH is operator new(0x48)");

// FLPACKET_SERVER_CHARSELECTVERIFIED -- id 0x20008, registered at 06d9bd3d.
// Factory the routine: operator new(0x30) + ctor the routine. Vtable module storage =
// { 06d9af50, 06dabdf0, 06dabf10, 06dac020 }.
class FPacket_FLPACKET_SERVER_CHARSELECTVERIFIED : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_CHARSELECTVERIFIED() : FPacketBase(0x20008u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeServerCharSelectVerified(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeServerCharSelectVerified(*this, Buffer, Size, OutLength);
    }
    void Dump(std::ostream& Out) const override {
        RpcDumpServerCharSelectVerified(*this, Out);
    }
    unsigned char Body[0x10];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_CHARSELECTVERIFIED) == 0x30u, "FLPACKET_SERVER_CHARSELECTVERIFIED is operator new(0x30)");

// FLPACKET_SERVER_ACTIVATEOBJECT -- id 0x2000a, registered at 06d9bd53.
// Factory the routine: operator new(0x2c) + ctor the routine. Vtable module storage =
// { 06d9af50, 06da8e30, 06da8f80, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_ACTIVATEOBJECT : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_ACTIVATEOBJECT() : FPacketBase(0x2000au) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeServerActivateObject(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeServerActivateObject(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0xc];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_ACTIVATEOBJECT) == 0x2cu, "FLPACKET_SERVER_ACTIVATEOBJECT is operator new(0x2c)");

// FLPACKET_SERVER_LAND -- id 0x2000b, registered at 06d9bd69.
// Factory the routine: operator new(0x2c) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db0160, 06da6e40, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_LAND : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_LAND() : FPacketBase(0x2000bu) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireThreeDwords(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireThreeDwords(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0xc];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_LAND) == 0x2cu, "FLPACKET_SERVER_LAND is operator new(0x2c)");

// FLPACKET_SERVER_GFCOMPLETENEWSBROADCASTLIST -- id 0x2000e, registered at 06d9bd7f.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db0310, 06db03f0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_GFCOMPLETENEWSBROADCASTLIST : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_GFCOMPLETENEWSBROADCASTLIST() : FPacketBase(0x2000eu) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDword(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDword(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x4];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_GFCOMPLETENEWSBROADCASTLIST) == 0x24u, "FLPACKET_SERVER_GFCOMPLETENEWSBROADCASTLIST is operator new(0x24)");

// FLPACKET_SERVER_GFCOMPLETEMISSIONCOMPUTERLIST -- id 0x20010, registered at 06d9bd95.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db0310, 06db03f0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_GFCOMPLETEMISSIONCOMPUTERLIST : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_GFCOMPLETEMISSIONCOMPUTERLIST() : FPacketBase(0x20010u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDword(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDword(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x4];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_GFCOMPLETEMISSIONCOMPUTERLIST) == 0x24u, "FLPACKET_SERVER_GFCOMPLETEMISSIONCOMPUTERLIST is operator new(0x24)");

// FLPACKET_SERVER_GFCOMPLETECHARLIST -- id 0x2000f, registered at 06d9bdab.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db0310, 06db03f0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_GFCOMPLETECHARLIST : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_GFCOMPLETECHARLIST() : FPacketBase(0x2000fu) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDword(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDword(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x4];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_GFCOMPLETECHARLIST) == 0x24u, "FLPACKET_SERVER_GFCOMPLETECHARLIST is operator new(0x24)");

// FLPACKET_SERVER_GFCOMPLETESCRIPTBEHAVIORLIST -- id 0x20011, registered at 06d9bdc1.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db0310, 06db03f0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_GFCOMPLETESCRIPTBEHAVIORLIST : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_GFCOMPLETESCRIPTBEHAVIORLIST() : FPacketBase(0x20011u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDword(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDword(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x4];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_GFCOMPLETESCRIPTBEHAVIORLIST) == 0x24u, "FLPACKET_SERVER_GFCOMPLETESCRIPTBEHAVIORLIST is operator new(0x24)");

// FLPACKET_SERVER_GFCOMPLETEAMBIENTSCRIPTLIST -- id 0x20013, registered at 06d9bdd7.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db0310, 06db03f0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_GFCOMPLETEAMBIENTSCRIPTLIST : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_GFCOMPLETEAMBIENTSCRIPTLIST() : FPacketBase(0x20013u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDword(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDword(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x4];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_GFCOMPLETEAMBIENTSCRIPTLIST) == 0x24u, "FLPACKET_SERVER_GFCOMPLETEAMBIENTSCRIPTLIST is operator new(0x24)");

// FLPACKET_SERVER_SYSTEM_SWITCH_OUT -- id 0x20021, registered at 06d9bded.
// Factory the routine: operator new(0x28) + ctor the routine. Vtable module storage =
// { 06d9af50, 06da9110, 06d95d40, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_SYSTEM_SWITCH_OUT : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_SYSTEM_SWITCH_OUT() : FPacketBase(0x20021u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDwordPair(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDwordPair(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x8];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_SYSTEM_SWITCH_OUT) == 0x28u, "FLPACKET_SERVER_SYSTEM_SWITCH_OUT is operator new(0x28)");

// FLPACKET_SERVER_SYSTEM_SWITCH_IN -- id 0x20022, registered at 06d9be03.
// Factory the routine: operator new(0x44) + ctor the routine. Vtable module storage =
// { 06d9af50, 06da9260, 06da9430, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_SYSTEM_SWITCH_IN : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_SYSTEM_SWITCH_IN() : FPacketBase(0x20022u) {
        RpcPacketStoreField(this, 0x28u, 4, 0u);
        RpcPacketStoreField(this, 0x2cu, 4, 0u);
        RpcPacketStoreField(this, 0x30u, 4, 0u);
        RpcPacketStoreField(this, 0x34u, 4, 0x3f800000u);
        RpcPacketStoreField(this, 0x38u, 4, 0u);
        RpcPacketStoreField(this, 0x3cu, 4, 0u);
        RpcPacketStoreField(this, 0x40u, 4, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeServerSystemSwitchIn(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeServerSystemSwitchIn(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x24];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_SYSTEM_SWITCH_IN) == 0x44u, "FLPACKET_SERVER_SYSTEM_SWITCH_IN is operator new(0x44)");

// FLPACKET_SERVER_SETSHIPARCH -- id 0x20023, registered at 06d9be19.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db0310, 06db03f0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_SETSHIPARCH : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_SETSHIPARCH() : FPacketBase(0x20023u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDword(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDword(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x4];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_SETSHIPARCH) == 0x24u, "FLPACKET_SERVER_SETSHIPARCH is operator new(0x24)");

// FLPACKET_SERVER_SETEQUIPMENT -- id 0x20024, registered at 06d9be2f.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d91320, 06d916a0, 06d95bf0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_SETEQUIPMENT : public FRpcEquipListPacket {
public:
    FPacket_FLPACKET_SERVER_SETEQUIPMENT() : FRpcEquipListPacket(0x20024u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
    }

    ~FPacket_FLPACKET_SERVER_SETEQUIPMENT() override {
        // Own vptr store, then JMP the routine -- ~FRpcEquipListPacket.
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireEquipList(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireEquipList(*this, Buffer, Size, OutLength);
    }
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_SETEQUIPMENT) == 0x24u, "FLPACKET_SERVER_SETEQUIPMENT is operator new(0x24)");

// FLPACKET_SERVER_SETCARGO -- id 0x20025, registered at 06d9be45.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d91680, 06d916a0, 06d95bf0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_SETCARGO : public FRpcEquipListPacket {
public:
    FPacket_FLPACKET_SERVER_SETCARGO() : FRpcEquipListPacket(0x20025u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
    }

    ~FPacket_FLPACKET_SERVER_SETCARGO() override {
        // Own vptr store, then JMP the routine -- ~FRpcEquipListPacket.
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireEquipList(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireEquipList(*this, Buffer, Size, OutLength);
    }
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_SETCARGO) == 0x24u, "FLPACKET_SERVER_SETCARGO is operator new(0x24)");

// FLPACKET_SERVER_REQUESTCREATESHIPRESP -- id 0x20027, registered at 06d9be5b.
// Factory the routine: operator new(0x28) + ctor the routine. Vtable module storage =
// { 06d9af50, 06da7fe0, 06da8100, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_REQUESTCREATESHIPRESP : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_REQUESTCREATESHIPRESP() : FPacketBase(0x20027u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeServerRequestCreateShipResp(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeServerRequestCreateShipResp(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x8];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_REQUESTCREATESHIPRESP) == 0x28u, "FLPACKET_SERVER_REQUESTCREATESHIPRESP is operator new(0x28)");

// FLPACKET_SERVER_SETSTARTROOM -- id 0x2000d, registered at 06d9be71.
// Factory the routine: operator new(0x28) + ctor the routine. Vtable module storage =
// { 06d9af50, 06da9110, 06d95d40, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_SETSTARTROOM : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_SETSTARTROOM() : FPacketBase(0x2000du) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDwordPair(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDwordPair(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x8];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_SETSTARTROOM) == 0x28u, "FLPACKET_SERVER_SETSTARTROOM is operator new(0x28)");

// FLPACKET_SERVER_GFDESTROYCHARACTER -- id 0x20015, registered at 06d9be87.
// Factory the routine: operator new(0x28) + ctor the routine. Vtable module storage =
// { 06d9af50, 06da9110, 06d95d40, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_GFDESTROYCHARACTER : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_GFDESTROYCHARACTER() : FPacketBase(0x20015u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDwordPair(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDwordPair(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x8];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_GFDESTROYCHARACTER) == 0x28u, "FLPACKET_SERVER_GFDESTROYCHARACTER is operator new(0x28)");

// FLPACKET_SERVER_GFDESTROYNEWSBROADCAST -- id 0x20014, registered at 06d9be9d.
// Factory the routine: operator new(0x28) + ctor the routine. Vtable module storage =
// { 06d9af50, 06da9110, 06d95d40, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_GFDESTROYNEWSBROADCAST : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_GFDESTROYNEWSBROADCAST() : FPacketBase(0x20014u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDwordPair(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDwordPair(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x8];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_GFDESTROYNEWSBROADCAST) == 0x28u, "FLPACKET_SERVER_GFDESTROYNEWSBROADCAST is operator new(0x28)");

// FLPACKET_SERVER_GFDESTROYMISSIONCOMPUTER -- id 0x20016, registered at 06d9beb3.
// Factory the routine: operator new(0x28) + ctor the routine. Vtable module storage =
// { 06d9af50, 06da9110, 06d95d40, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_GFDESTROYMISSIONCOMPUTER : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_GFDESTROYMISSIONCOMPUTER() : FPacketBase(0x20016u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDwordPair(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDwordPair(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x8];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_GFDESTROYMISSIONCOMPUTER) == 0x28u, "FLPACKET_SERVER_GFDESTROYMISSIONCOMPUTER is operator new(0x28)");

// FLPACKET_SERVER_GFDESTROYSCRIPTBEHAVIOR -- id 0x20017, registered at 06d9bec9.
// Factory the routine: operator new(0x28) + ctor the routine. Vtable module storage =
// { 06d9af50, 06da9110, 06d95d40, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_GFDESTROYSCRIPTBEHAVIOR : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_GFDESTROYSCRIPTBEHAVIOR() : FPacketBase(0x20017u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDwordPair(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDwordPair(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x8];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_GFDESTROYSCRIPTBEHAVIOR) == 0x28u, "FLPACKET_SERVER_GFDESTROYSCRIPTBEHAVIOR is operator new(0x28)");

// FLPACKET_SERVER_GFDESTROYAMBIENTSCRIPT -- id 0x20019, registered at 06d9bedf.
// Factory the routine: operator new(0x28) + ctor the routine. Vtable module storage =
// { 06d9af50, 06da9110, 06d95d40, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_GFDESTROYAMBIENTSCRIPT : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_GFDESTROYAMBIENTSCRIPT() : FPacketBase(0x20019u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDwordPair(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDwordPair(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x8];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_GFDESTROYAMBIENTSCRIPT) == 0x28u, "FLPACKET_SERVER_GFDESTROYAMBIENTSCRIPT is operator new(0x28)");

// FLPACKET_SERVER_GFSCRIPTBEHAVIOR -- id 0x2001a, registered at 06d9bef5.
// Factory the routine: operator new(0x28) + ctor the routine. Vtable module storage =
// { 06d976e0, 06d97570, 06d978f0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_GFSCRIPTBEHAVIOR : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_GFSCRIPTBEHAVIOR() : FPacketBase(0x2001au) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 4, 0u);
    }

    ~FPacket_FLPACKET_SERVER_GFSCRIPTBEHAVIOR() override {
        if (RpcPacketField<void*>(this, 0x24u) != nullptr) {
            ::operator delete(RpcPacketField<void*>(this, 0x24u));
        }
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireBlobSizeFirst(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireBlobSizeFirst(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x8];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_GFSCRIPTBEHAVIOR) == 0x28u, "FLPACKET_SERVER_GFSCRIPTBEHAVIOR is operator new(0x28)");

// FLPACKET_SERVER_GFUPDATEAMBIENTSCRIPT -- id 0x2001f, registered at 06d9bf0b.
// Factory the routine: operator new(0x28) + ctor the routine. Vtable module storage =
// { 06d978a0, 06da6190, 06d97730, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_GFUPDATEAMBIENTSCRIPT : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_GFUPDATEAMBIENTSCRIPT() : FPacketBase(0x2001fu) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 4, 0u);
    }

    ~FPacket_FLPACKET_SERVER_GFUPDATEAMBIENTSCRIPT() override {
        if (RpcPacketField<void*>(this, 0x20u) != nullptr) {
            ::operator delete(RpcPacketField<void*>(this, 0x20u));
        }
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireBlobBufferFirst(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireBlobBufferFirst(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x8];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_GFUPDATEAMBIENTSCRIPT) == 0x28u, "FLPACKET_SERVER_GFUPDATEAMBIENTSCRIPT is operator new(0x28)");

// FLPACKET_SERVER_MISSIONSAVEA -- id 0x2005b, registered at 06d9bf21.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db0310, 06db03f0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_MISSIONSAVEA : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_MISSIONSAVEA() : FPacketBase(0x2005bu) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDword(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDword(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x4];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_MISSIONSAVEA) == 0x24u, "FLPACKET_SERVER_MISSIONSAVEA is operator new(0x24)");

// FLPACKET_SERVER_GFUPDATEMISSIONCOMPUTER -- id 0x2001d, registered at 06d9bf37.
// Factory the routine: operator new(0x28) + ctor the routine. Vtable module storage =
// { 06d97a90, 06d97570, 06d978f0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_GFUPDATEMISSIONCOMPUTER : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_GFUPDATEMISSIONCOMPUTER() : FPacketBase(0x2001du) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 4, 0u);
    }

    ~FPacket_FLPACKET_SERVER_GFUPDATEMISSIONCOMPUTER() override {
        if (RpcPacketField<void*>(this, 0x24u) != nullptr) {
            ::operator delete(RpcPacketField<void*>(this, 0x24u));
        }
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireBlobSizeFirst(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireBlobSizeFirst(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x8];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_GFUPDATEMISSIONCOMPUTER) == 0x28u, "FLPACKET_SERVER_GFUPDATEMISSIONCOMPUTER is operator new(0x28)");

// FLPACKET_SERVER_GFUPDATENEWSBROADCAST -- id 0x2001e, registered at 06d9bf4d.
// Factory the routine: operator new(0x28) + ctor the routine. Vtable module storage =
// { 06d97b70, 06d97570, 06d978f0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_GFUPDATENEWSBROADCAST : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_GFUPDATENEWSBROADCAST() : FPacketBase(0x2001eu) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 4, 0u);
    }

    ~FPacket_FLPACKET_SERVER_GFUPDATENEWSBROADCAST() override {
        if (RpcPacketField<void*>(this, 0x24u) != nullptr) {
            ::operator delete(RpcPacketField<void*>(this, 0x24u));
        }
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireBlobSizeFirst(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireBlobSizeFirst(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x8];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_GFUPDATENEWSBROADCAST) == 0x28u, "FLPACKET_SERVER_GFUPDATENEWSBROADCAST is operator new(0x28)");

// FLPACKET_SERVER_GFMISSIONVENDORACCEPTANCE -- id 0x20020, registered at 06d9bf63.
// Factory the routine: operator new(0x28) + ctor the routine. Vtable module storage =
// { 06d97ab0, 06d97570, 06d978f0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_GFMISSIONVENDORACCEPTANCE : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_GFMISSIONVENDORACCEPTANCE() : FPacketBase(0x20020u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 4, 0u);
    }

    ~FPacket_FLPACKET_SERVER_GFMISSIONVENDORACCEPTANCE() override {
        if (RpcPacketField<void*>(this, 0x24u) != nullptr) {
            ::operator delete(RpcPacketField<void*>(this, 0x24u));
        }
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireBlobSizeFirst(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireBlobSizeFirst(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x8];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_GFMISSIONVENDORACCEPTANCE) == 0x28u, "FLPACKET_SERVER_GFMISSIONVENDORACCEPTANCE is operator new(0x28)");

// FLPACKET_SERVER_GFMISSIONVENDORWHYEMPTY -- id 0x2005a, registered at 06d9bf79.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d9af50, 06da82e0, 06da83c0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_GFMISSIONVENDORWHYEMPTY : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_GFMISSIONVENDORWHYEMPTY() : FPacketBase(0x2005au) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireByte(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireByte(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x4];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_GFMISSIONVENDORWHYEMPTY) == 0x24u, "FLPACKET_SERVER_GFMISSIONVENDORWHYEMPTY is operator new(0x24)");

// FLPACKET_SERVER_GFUPDATECHAR -- id 0x20026, registered at 06d9bf8f.
// Factory the routine: operator new(0x28) + ctor the routine. Vtable module storage =
// { 06d97520, 06d97570, 06d978f0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_GFUPDATECHAR : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_GFUPDATECHAR() : FPacketBase(0x20026u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 4, 0u);
    }

    ~FPacket_FLPACKET_SERVER_GFUPDATECHAR() override {
        if (RpcPacketField<void*>(this, 0x24u) != nullptr) {
            ::operator delete(RpcPacketField<void*>(this, 0x24u));
        }
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireBlobSizeFirst(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireBlobSizeFirst(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x8];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_GFUPDATECHAR) == 0x28u, "FLPACKET_SERVER_GFUPDATECHAR is operator new(0x28)");

// FLPACKET_SERVER_CREATELOOT -- id 0x20028, registered at 06d9bfa5.
// Factory the routine: operator new(0x34) + ctor the routine. Vtable module storage =
// { 06d9d1d0, 06d928c0, 06d92d20, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_CREATELOOT : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_CREATELOOT() : FPacketBase(0x20028u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 1, 0u);
        RpcPacketBuildListSentinel(this, 0x28u, 0x50u);   // operator new(0x50), Head
    }

    ~FPacket_FLPACKET_SERVER_CREATELOOT() override {
        // the list at +0x28 (Head +0x2c, Size +0x30).
        RpcPacketDestroyEmbeddedList(this, 0x28u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeServerCreateLoot(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeServerCreateLoot(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x14];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_CREATELOOT) == 0x34u, "FLPACKET_SERVER_CREATELOOT is operator new(0x34)");

// FLPACKET_SERVER_SETREPUTATION -- id 0x20029, registered at 06d9bfbb.
// Factory the routine: operator new(0x68) + ctor the routine. Vtable module storage =
// { 06d91eb0, 06d91ed0, 06d92350, 06d92860 }.
class FPacket_FLPACKET_SERVER_SETREPUTATION : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_SETREPUTATION() : FPacketBase(0x20029u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 4, 0u);
        RpcPacketStoreField(this, 0x28u, 4, 0u);
        RpcPacketStoreField(this, 0x2cu, 4, 0u);
        RpcPacketStoreField(this, 0x38u, 4, 0x00000002u);
        RpcPacketStoreField(this, 0x30u, 4, 0u);
        RpcPacketStoreField(this, 0x34u, 4, 0x00000001u);
        RpcPacketStoreField(this, 0x3cu, 4, 0x00000004u);
        RpcPacketStoreField(this, 0x40u, 4, 0x00000008u);
        RpcPacketStoreField(this, 0x44u, 4, 0x00000010u);
        RpcPacketBuildListSentinel(this, 0x48u, 0x0cu);   // operator new(0xc), Head
        RpcPacketStoreField(this, 0x54u, 1, 0u);
        RpcPacketStoreField(this, 0x58u, 4, 0u);
        RpcPacketStoreField(this, 0x60u, 4, 0u);
        RpcPacketStoreField(this, 0x64u, 4, 0u);
        RpcPacketStoreField(this, 0x5cu, 4, 0xffffffffu);
    }

    ~FPacket_FLPACKET_SERVER_SETREPUTATION() override {
        // free every node's value first, testing each for null.
        FRpcPacketListNode* Sentinel = RpcPacketField<FRpcPacketListNode*>(this, 0x4cu);
        for (FRpcPacketListNode* Node = Sentinel->Next; Node != Sentinel; Node = Node->Next) {
            if (Node->Value != nullptr) {
                ::operator delete(Node->Value);
            }
        }
        if (RpcPacketField<void*>(this, 0x64u) != nullptr) {
            ::operator delete(RpcPacketField<void*>(this, 0x64u));
        }
        RpcPacketDestroyEmbeddedList(this, 0x48u);   // , the routine
        if (RpcPacketField<void*>(this, 0x2cu) != nullptr) {
            ::operator delete(RpcPacketField<void*>(this, 0x2cu));
        }
        if (RpcPacketField<void*>(this, 0x24u) != nullptr) {
            ::operator delete(RpcPacketField<void*>(this, 0x24u));
        }
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeServerSetReputation(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeServerSetReputation(*this, Buffer, Size, OutLength);
    }
    void Dump(std::ostream& Out) const override {
        RpcDumpServerSetReputation(*this, Out);
    }
    unsigned char Body[0x48];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_SETREPUTATION) == 0x68u, "FLPACKET_SERVER_SETREPUTATION is operator new(0x68)");

// FLPACKET_SERVER_ADJUSTATTITUDE -- id 0x2002a, registered at 06d9bfd1.
// Factory the routine: operator new(0x28) + ctor the routine. Vtable module storage =
// { 06d9af50, 06daa0c0, 06d95d40, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_ADJUSTATTITUDE : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_ADJUSTATTITUDE() : FPacketBase(0x2002au) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeServerAdjustAttitude(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDwordPair(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x8];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_ADJUSTATTITUDE) == 0x28u, "FLPACKET_SERVER_ADJUSTATTITUDE is operator new(0x28)");

// FLPACKET_SERVER_SETGROUPFEELINGS -- id 0x2002b, registered at 06d9bfe7.
// Factory the routine: operator new(0x34) + ctor the routine. Vtable module storage =
// { 06d91830, 06d91890, 06d91a40, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_SETGROUPFEELINGS : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_SETGROUPFEELINGS() : FPacketBase(0x2002bu) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 4, 0u);
        RpcPacketBuildListSentinel(this, 0x28u, 0x0cu);   // operator new(0xc), Head
    }

    ~FPacket_FLPACKET_SERVER_SETGROUPFEELINGS() override {
        // free every node's value first, testing each for null.
        FRpcPacketListNode* Sentinel = RpcPacketField<FRpcPacketListNode*>(this, 0x2cu);
        for (FRpcPacketListNode* Node = Sentinel->Next; Node != Sentinel; Node = Node->Next) {
            if (Node->Value != nullptr) {
                ::operator delete(Node->Value);
            }
        }
        if (RpcPacketField<void*>(this, 0x20u) != nullptr) {
            ::operator delete(RpcPacketField<void*>(this, 0x20u));
        }
        RpcPacketDestroyEmbeddedList(this, 0x28u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeServerSetGroupFeelings(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeServerSetGroupFeelings(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x14];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_SETGROUPFEELINGS) == 0x34u, "FLPACKET_SERVER_SETGROUPFEELINGS is operator new(0x34)");

// FLPACKET_SERVER_CREATEMINE -- id 0x2002c, registered at 06d9bffd.
// Factory the routine: operator new(0x68) + ctor the routine. Vtable module storage =
// { 06d9af50, 06da95d0, 06da98d0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_CREATEMINE : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_CREATEMINE() : FPacketBase(0x2002cu) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 4, 0u);
        RpcPacketStoreField(this, 0x28u, 4, 0u);
        RpcPacketStoreField(this, 0x2cu, 2, 0u);
        RpcPacketStoreField(this, 0x30u, 4, 0x3f800000u);
        RpcPacketStoreField(this, 0x34u, 4, 0u);
        RpcPacketStoreField(this, 0x38u, 4, 0u);
        RpcPacketStoreField(this, 0x3cu, 4, 0u);
        RpcPacketStoreField(this, 0x40u, 4, 0u);
        RpcPacketStoreField(this, 0x44u, 4, 0u);
        RpcPacketStoreField(this, 0x48u, 4, 0u);
        RpcPacketStoreField(this, 0x4cu, 4, 0u);
        RpcPacketStoreField(this, 0x50u, 4, 0u);
        RpcPacketStoreField(this, 0x54u, 4, 0u);
        RpcPacketStoreField(this, 0x58u, 4, 0u);
        RpcPacketStoreField(this, 0x5cu, 4, 0u);
        RpcPacketStoreField(this, 0x60u, 4, 0u);
        RpcPacketStoreField(this, 0x64u, 4, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeServerCreateMine(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeServerCreateMine(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x48];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_CREATEMINE) == 0x68u, "FLPACKET_SERVER_CREATEMINE is operator new(0x68)");

// FLPACKET_SERVER_CREATECOUNTER -- id 0x2002d, registered at 06d9c013.
// Factory the routine: operator new(0x68) + ctor the routine. Vtable module storage =
// { 06d9af50, 06da9b50, 06da9e40, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_CREATECOUNTER : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_CREATECOUNTER() : FPacketBase(0x2002du) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 4, 0u);
        RpcPacketStoreField(this, 0x28u, 4, 0u);
        RpcPacketStoreField(this, 0x2cu, 4, 0u);
        RpcPacketStoreField(this, 0x30u, 2, SUBOBJ_ID_NONE);   // COMMON.DLL SUBOBJ_ID_NONE
        RpcPacketStoreField(this, 0x34u, 4, 0x3f800000u);
        RpcPacketStoreField(this, 0x38u, 4, 0u);
        RpcPacketStoreField(this, 0x3cu, 4, 0u);
        RpcPacketStoreField(this, 0x40u, 4, 0u);
        RpcPacketStoreField(this, 0x44u, 4, 0u);
        RpcPacketStoreField(this, 0x48u, 4, 0u);
        RpcPacketStoreField(this, 0x4cu, 4, 0u);
        RpcPacketStoreField(this, 0x50u, 4, 0u);
        RpcPacketStoreField(this, 0x54u, 4, 0u);
        RpcPacketStoreField(this, 0x58u, 4, 0u);
        RpcPacketStoreField(this, 0x5cu, 4, 0u);
        RpcPacketStoreField(this, 0x60u, 4, 0u);
        RpcPacketStoreField(this, 0x64u, 4, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeServerCreateCounter(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeServerCreateCounter(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x48];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_CREATECOUNTER) == 0x68u, "FLPACKET_SERVER_CREATECOUNTER is operator new(0x68)");

// FLPACKET_SERVER_EXPLODEASTEROIDMINE -- id 0x20031, registered at 06d9c029.
// Factory the routine: operator new(0x30) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db14f0, 06dad820, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_EXPLODEASTEROIDMINE : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_EXPLODEASTEROIDMINE() : FPacketBase(0x20031u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 4, 0u);
        RpcPacketStoreField(this, 0x28u, 4, 0u);
        RpcPacketStoreField(this, 0x2cu, 4, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDwordBlock12(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDwordBlock12(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x10];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_EXPLODEASTEROIDMINE) == 0x30u, "FLPACKET_SERVER_EXPLODEASTEROIDMINE is operator new(0x30)");

// FLPACKET_SERVER_LOADHINT -- id 0x2004d, registered at 06d9c03f.
// Factory the routine: operator new(0x2c) + ctor the routine. Vtable module storage =
// { 06d912c0, 06d90a50, 06d90ba0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_LOADHINT : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_LOADHINT() : FPacketBase(0x2004du) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 4, 0u);
        RpcPacketStoreField(this, 0x28u, 4, 0u);
    }

    ~FPacket_FLPACKET_SERVER_LOADHINT() override {
        if (RpcPacketField<void*>(this, 0x20u) != nullptr) {
            ::operator delete(RpcPacketField<void*>(this, 0x20u));
            RpcPacketField<void*>(this, 0x20u) = nullptr;
        }
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeServerLoadHint(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeServerLoadHint(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0xc];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_LOADHINT) == 0x2cu, "FLPACKET_SERVER_LOADHINT is operator new(0x2c)");

// FLPACKET_SERVER_SETDIRECTIVE -- id 0x2004e, registered at 06d9c055.
// Factory the routine: operator new(0xb4) + ctor the routine. Vtable module storage =
// { 06d9af50, 06d90d10, 06d90f70, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_SETDIRECTIVE : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_SETDIRECTIVE() : FPacketBase(0x2004eu) {
        RpcPacketStoreField(this, 0x24u, 4, 0u);
        RpcPacketStoreField(this, 0x28u, 1, 0u);
        RpcPacketStoreField(this, 0x2cu, 4, 0u);
        RpcPacketStoreField(this, 0x30u, 4, 0u);
        RpcPacketStoreField(this, 0x34u, 4, 0u);
        RpcPacketStoreField(this, 0x38u, 4, 0xffffffffu);
        RpcPacketStoreField(this, 0x3cu, 4, 0u);
        RpcPacketStoreField(this, 0x40u, 4, 0u);
        RpcPacketStoreField(this, 0x44u, 4, 0u);
        RpcPacketStoreField(this, 0x48u, 4, 0u);
        RpcPacketStoreField(this, 0x4cu, 4, 0u);
        RpcPacketStoreField(this, 0x50u, 4, 0u);
        RpcPacketStoreField(this, 0x54u, 4, 0u);
        RpcPacketStoreField(this, 0x58u, 4, 0u);
        RpcPacketStoreField(this, 0x5cu, 4, 0u);
        RpcPacketStoreField(this, 0x60u, 4, 0x00000003u);
        RpcPacketStoreField(this, 0x64u, 4, 0u);
        RpcPacketStoreField(this, 0x68u, 4, 0u);
        RpcPacketStoreField(this, 0x6cu, 4, 0u);
        RpcPacketStoreField(this, 0x70u, 4, 0u);
        RpcPacketStoreField(this, 0x74u, 4, 0u);
        RpcPacketStoreField(this, 0x78u, 4, 0u);
        RpcPacketStoreField(this, 0x7cu, 4, 0u);
        RpcPacketStoreField(this, 0x80u, 4, 0u);
        RpcPacketStoreField(this, 0x84u, 4, 0u);
        RpcPacketStoreField(this, 0x88u, 4, 0u);
        RpcPacketStoreField(this, 0x8cu, 4, 0u);
        RpcPacketStoreField(this, 0x90u, 4, 0u);
        RpcPacketStoreField(this, 0x94u, 4, 0u);
        RpcPacketStoreField(this, 0x98u, 4, 0u);
        RpcPacketStoreField(this, 0x9cu, 4, 0u);
        RpcPacketStoreField(this, 0xa0u, 4, 0u);
        RpcPacketStoreField(this, 0xa4u, 4, 0u);
        RpcPacketStoreField(this, 0xa8u, 4, 0u);
        RpcPacketStoreField(this, 0xacu, 1, 0u);
        RpcPacketStoreField(this, 0xadu, 1, 0u);
        RpcPacketStoreField(this, 0xaeu, 1, 0u);
        RpcPacketStoreField(this, 0xafu, 1, 0u);
        RpcPacketStoreField(this, 0xb0u, 1, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeServerSetDirective(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeServerSetDirective(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x94];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_SETDIRECTIVE) == 0xb4u, "FLPACKET_SERVER_SETDIRECTIVE is operator new(0xb4)");

// FLPACKET_SERVER_CREATEGUIDED -- id 0x20037, registered at 06d9c06b.
// Factory the routine: operator new(0x58) + ctor the routine. Vtable module storage =
// { 06d9af50, 06da84e0, 06da8810, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_CREATEGUIDED : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_CREATEGUIDED() : FPacketBase(0x20037u) {
        RpcPacketStoreField(this, 0x3cu, 4, 0u);
        RpcPacketStoreField(this, 0x40u, 4, 0u);
        RpcPacketStoreField(this, 0x44u, 4, 0u);
        RpcPacketStoreField(this, 0x48u, 4, 0x3f800000u);
        RpcPacketStoreField(this, 0x4cu, 4, 0u);
        RpcPacketStoreField(this, 0x50u, 4, 0u);
        RpcPacketStoreField(this, 0x54u, 4, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeServerCreateGuided(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeServerCreateGuided(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x38];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_CREATEGUIDED) == 0x58u, "FLPACKET_SERVER_CREATEGUIDED is operator new(0x58)");

// FLPACKET_SERVER_ITEMTRACTORED -- id 0x20038, registered at 06d9c081.
// Factory the routine: operator new(0x2c) + ctor the routine. Vtable module storage =
// { 06d9af50, 06da8b50, 06da8cd0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_ITEMTRACTORED : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_ITEMTRACTORED() : FPacketBase(0x20038u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 4, 0u);
        RpcPacketStoreField(this, 0x28u, 2, 0u);
        RpcPacketStoreField(this, 0x2au, 2, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDwordDwordWordWord(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDwordDwordWordWord(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0xc];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_ITEMTRACTORED) == 0x2cu, "FLPACKET_SERVER_ITEMTRACTORED is operator new(0x2c)");

// FLPACKET_SERVER_SETREMOVEITEM -- id 0x2002f, registered at 06d9c097.
// Factory the routine: operator new(0x2c) + ctor the routine. Vtable module storage =
// { 06d9c4e0, 06db0160, 06da6e40, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_SETREMOVEITEM : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_SETREMOVEITEM() : FPacketBase(0x2002fu) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 4, 0xffffffffu);
        RpcPacketStoreField(this, 0x28u, 4, 0xffffffffu);
    }

    ~FPacket_FLPACKET_SERVER_SETREMOVEITEM() override {
        // Own vptr store, then straight into CDAPacket's dtor: nothing to free.
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireThreeDwords(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireThreeDwords(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0xc];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_SETREMOVEITEM) == 0x2cu, "FLPACKET_SERVER_SETREMOVEITEM is operator new(0x2c)");

// FLPACKET_SERVER_SETADDITEM -- id 0x2002e, registered at 06d9c0ad.
// Factory the routine: operator new(0x40) + ctor the routine. Vtable module storage =
// { 06d96f70, 06d96f90, 06d97210, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_SETADDITEM : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_SETADDITEM() : FPacketBase(0x2002eu) {
        RpcPacketStoreField(this, 0x28u, 4, 0u);
        RpcPacketStoreField(this, 0x39u, 1, 0u);
        RpcPacketStoreField(this, 0x3cu, 4, 0u);
    }

    ~FPacket_FLPACKET_SERVER_SETADDITEM() override {
        if (RpcPacketField<void*>(this, 0x3cu) != nullptr) {
            ::operator delete(RpcPacketField<void*>(this, 0x3cu));
        }
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeServerSetAddItem(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeServerSetAddItem(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x20];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_SETADDITEM) == 0x40u, "FLPACKET_SERVER_SETADDITEM is operator new(0x40)");

// FLPACKET_SERVER_SETCASH -- id 0x20030, registered at 06d9c0c3.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db0310, 06db03f0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_SETCASH : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_SETCASH() : FPacketBase(0x20030u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDword(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDword(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x4];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_SETCASH) == 0x24u, "FLPACKET_SERVER_SETCASH is operator new(0x24)");

// FLPACKET_SERVER_REQUESTSPACESCRIPT -- id 0x20032, registered at 06d9c0d9.
// Factory the routine: operator new(0x3c) + ctor the routine. Vtable module storage =
// { 06d986c0, 06d986e0, 06d98850, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_REQUESTSPACESCRIPT : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_REQUESTSPACESCRIPT() : FPacketBase(0x20032u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x28u, 4, 0u);
        RpcPacketStoreField(this, 0x2cu, 4, 0u);
        RpcPacketStoreField(this, 0x30u, 4, 0u);
        RpcPacketStoreField(this, 0x34u, 1, 0x00000001u);
        RpcPacketStoreField(this, 0x38u, 4, 0u);
    }

    ~FPacket_FLPACKET_SERVER_REQUESTSPACESCRIPT() override {
        if (RpcPacketField<void*>(this, 0x38u) != nullptr) {
            ::operator delete(RpcPacketField<void*>(this, 0x38u));
        }
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeServerRequestSpaceScript(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeServerRequestSpaceScript(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x1c];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_REQUESTSPACESCRIPT) == 0x3cu, "FLPACKET_SERVER_REQUESTSPACESCRIPT is operator new(0x3c)");

// FLPACKET_SERVER_SETMISSIONOBJECTIVESTATE -- id 0x20033, registered at 06d9c0ef.
// Factory the routine: operator new(0x2c) + ctor the routine. Vtable module storage =
// { 06d9af50, 06daaf90, 06dacea0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_SETMISSIONOBJECTIVESTATE : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_SETMISSIONOBJECTIVESTATE() : FPacketBase(0x20033u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDwordDwordWord(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDwordDwordWord(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0xc];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_SETMISSIONOBJECTIVESTATE) == 0x2cu, "FLPACKET_SERVER_SETMISSIONOBJECTIVESTATE is operator new(0x2c)");

// FLPACKET_SERVER_CFGINTERFACENOTIFICATION -- id 0x20047, registered at 06d9c105.
// Factory the routine: operator new(0x2c) + ctor the routine. Vtable module storage =
// { 06d9af50, 06dab200, 06dab350, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_CFGINTERFACENOTIFICATION : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_CFGINTERFACENOTIFICATION() : FPacketBase(0x20047u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeServerCfgInterfaceNotification(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeServerCfgInterfaceNotification(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0xc];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_CFGINTERFACENOTIFICATION) == 0x2cu, "FLPACKET_SERVER_CFGINTERFACENOTIFICATION is operator new(0x2c)");

// FLPACKET_SERVER_MARKOBJ -- id 0x20046, registered at 06d9c11b.
// Factory the routine: operator new(0x28) + ctor the routine. Vtable module storage =
// { 06d9af50, 06dab0e0, 06daf230, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_MARKOBJ : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_MARKOBJ() : FPacketBase(0x20046u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDwordByte(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDwordByte(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x8];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_MARKOBJ) == 0x28u, "FLPACKET_SERVER_MARKOBJ is operator new(0x28)");

// FLPACKET_SERVER_SETMISSIONOBJECTIVES -- id 0x20035, registered at 06d9c131.
// Factory the routine: operator new(0x48) + ctor the routine. Vtable module storage =
// { 06d8fbf0, 06d8fc90, 06d8ffd0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_SETMISSIONOBJECTIVES : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_SETMISSIONOBJECTIVES() : FPacketBase(0x20035u) {
        RpcPacketStoreField(this, 0x28u, 4, 0u);
        RpcPacketStoreField(this, 0x2cu, 4, 0u);
        RpcPacketStoreField(this, 0x30u, 4, 0u);
        RpcPacketStoreField(this, 0x34u, 4, 0u);
        RpcPacketStoreField(this, 0x38u, 4, 0u);
        RpcPacketBuildListSentinel(this, 0x3cu, 0x0cu);   // operator new(0xc), Head
    }

    ~FPacket_FLPACKET_SERVER_SETMISSIONOBJECTIVES() override {
        // every record, then its text block before the record itself.
        FRpcPacketListNode* Sentinel = RpcPacketField<FRpcPacketListNode*>(this, 0x40u);
        for (FRpcPacketListNode* Node = Sentinel->Next; Node != Sentinel; Node = Node->Next) {
            unsigned char* Record = static_cast<unsigned char*>(Node->Value);
            if (Record != nullptr) {
                void* Text = *reinterpret_cast<void**>(Record + 0x08);
                if (Text != nullptr) {
                    ::operator delete(Text);
                }
                ::operator delete(Record);
            }
        }
        RpcPacketDestroyEmbeddedList(this, 0x3cu);   // , the routine
        if (RpcPacketField<void*>(this, 0x34u) != nullptr) {
            ::operator delete(RpcPacketField<void*>(this, 0x34u));
        }
        if (RpcPacketField<void*>(this, 0x2cu) != nullptr) {
            ::operator delete(RpcPacketField<void*>(this, 0x2cu));
        }
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeServerSetMissionObjectives(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeServerSetMissionObjectives(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x28];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_SETMISSIONOBJECTIVES) == 0x48u, "FLPACKET_SERVER_SETMISSIONOBJECTIVES is operator new(0x48)");

// FLPACKET_SERVER_REPLACEMISSIONOBJECTIVE -- id 0x20034, registered at 06d9c147.
// Factory the routine: operator new(0x30) + ctor the routine. Vtable module storage =
// { 06d9d7b0, 06dab4b0, 06dab630, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_REPLACEMISSIONOBJECTIVE : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_REPLACEMISSIONOBJECTIVE() : FPacketBase(0x20034u) {
        RpcPacketStoreField(this, 0x28u, 4, 0u);
        RpcPacketStoreField(this, 0x2cu, 4, 0u);
    }

    ~FPacket_FLPACKET_SERVER_REPLACEMISSIONOBJECTIVE() override {
        if (RpcPacketField<void*>(this, 0x2cu) != nullptr) {
            ::operator delete(RpcPacketField<void*>(this, 0x2cu));
        }
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeServerReplaceMissionObjective(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeServerReplaceMissionObjective(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x10];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_REPLACEMISSIONOBJECTIVE) == 0x30u, "FLPACKET_SERVER_REPLACEMISSIONOBJECTIVE is operator new(0x30)");

// FLPACKET_SERVER_SET_STORY_CUE -- id 0x20043, registered at 06d9c15d.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db0310, 06db03f0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_SET_STORY_CUE : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_SET_STORY_CUE() : FPacketBase(0x20043u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDword(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDword(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x4];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_SET_STORY_CUE) == 0x24u, "FLPACKET_SERVER_SET_STORY_CUE is operator new(0x24)");

// FLPACKET_SERVER_SET_CAMERA -- id 0x2004b, registered at 06d9c173.
// Factory the routine: operator new(0x44) + ctor the routine. Vtable module storage =
// { 06d9af50, 06dab780, 06dab960, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_SET_CAMERA : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_SET_CAMERA() : FPacketBase(0x2004bu) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 4, 0u);
        RpcPacketStoreField(this, 0x28u, 4, 0u);
        RpcPacketStoreField(this, 0x2cu, 4, 0x3f800000u);
        RpcPacketStoreField(this, 0x30u, 4, 0u);
        RpcPacketStoreField(this, 0x34u, 4, 0u);
        RpcPacketStoreField(this, 0x38u, 4, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeServerSetCamera(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeServerSetCamera(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x24];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_SET_CAMERA) == 0x44u, "FLPACKET_SERVER_SET_CAMERA is operator new(0x44)");

// FLPACKET_SERVER_REVERT_CAMERA -- id 0x2004c, registered at 06d9c189.
// Factory the routine: operator new(0x20) + ctor the routine. Vtable module storage =
// { 06d9af50, 06dae0d0, 06dabad0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_REVERT_CAMERA : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_REVERT_CAMERA() : FPacketBase(0x2004cu) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireEmpty(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireEmpty(*this, Buffer, Size, OutLength);
    }
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_REVERT_CAMERA) == 0x20u, "FLPACKET_SERVER_REVERT_CAMERA is operator new(0x20)");

// FLPACKET_SERVER_SET_MISSION_MESSAGE -- id 0x20045, registered at 06d9c19f.
// Factory the routine: operator new(0x2c) + ctor the routine. Vtable module storage =
// { 06d9d9c0, 06dabbb0, 06dabd00, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_SET_MISSION_MESSAGE : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_SET_MISSION_MESSAGE() : FPacketBase(0x20045u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 4, 0u);
    }

    ~FPacket_FLPACKET_SERVER_SET_MISSION_MESSAGE() override {
        if (RpcPacketField<void*>(this, 0x24u) != nullptr) {
            ::operator delete(RpcPacketField<void*>(this, 0x24u));
        }
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeServerSetMissionMessage(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeServerSetMissionMessage(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0xc];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_SET_MISSION_MESSAGE) == 0x2cu, "FLPACKET_SERVER_SET_MISSION_MESSAGE is operator new(0x2c)");

// FLPACKET_SERVER_REPAIROBJECT -- id 0x2003c, registered at 06d9c1b5.
// Factory the routine: operator new(0x2c) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db0160, 06da6e40, 06dac2b0 }.
class FPacket_FLPACKET_SERVER_REPAIROBJECT : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_REPAIROBJECT() : FPacketBase(0x2003cu) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 4, 0u);
        RpcPacketStoreField(this, 0x28u, 4, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireThreeDwords(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireThreeDwords(*this, Buffer, Size, OutLength);
    }
    void Dump(std::ostream& Out) const override {
        RpcDumpServerRepairObject(*this, Out);
    }
    unsigned char Body[0xc];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_REPAIROBJECT) == 0x2cu, "FLPACKET_SERVER_REPAIROBJECT is operator new(0x2c)");

// FLPACKET_SERVER_SETNUMKILLS -- id 0x2003e, registered at 06d9c1cb.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db0310, 06db03f0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_SETNUMKILLS : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_SETNUMKILLS() : FPacketBase(0x2003eu) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDword(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDword(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x4];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_SETNUMKILLS) == 0x24u, "FLPACKET_SERVER_SETNUMKILLS is operator new(0x24)");

// FLPACKET_SERVER_SETMISSIONSUCCESSES -- id 0x2003f, registered at 06d9c1e1.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db0310, 06db03f0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_SETMISSIONSUCCESSES : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_SETMISSIONSUCCESSES() : FPacketBase(0x2003fu) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDword(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDword(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x4];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_SETMISSIONSUCCESSES) == 0x24u, "FLPACKET_SERVER_SETMISSIONSUCCESSES is operator new(0x24)");

// FLPACKET_SERVER_SETMISSIONFAILURES -- id 0x20040, registered at 06d9c1f7.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db0310, 06db03f0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_SETMISSIONFAILURES : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_SETMISSIONFAILURES() : FPacketBase(0x20040u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDword(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDword(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x4];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_SETMISSIONFAILURES) == 0x24u, "FLPACKET_SERVER_SETMISSIONFAILURES is operator new(0x24)");

// FLPACKET_SERVER_BURNFUSE -- id 0x20041, registered at 06d9c20d.
// Factory the routine: operator new(0x3c) + ctor the routine. Vtable module storage =
// { 06d9af50, 06dac3c0, 06dac5c0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_BURNFUSE : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_BURNFUSE() : FPacketBase(0x20041u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 4, 0u);
        RpcPacketStoreField(this, 0x28u, 4, 0u);
        RpcPacketStoreField(this, 0x2cu, 2, SUBOBJ_ID_NONE);   // COMMON.DLL SUBOBJ_ID_NONE
        RpcPacketStoreField(this, 0x30u, 4, 0u);
        RpcPacketStoreField(this, 0x34u, 4, 0xbf800000u);
        RpcPacketStoreField(this, 0x38u, 1, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeServerBurnFuse(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeServerBurnFuse(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x1c];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_BURNFUSE) == 0x3cu, "FLPACKET_SERVER_BURNFUSE is operator new(0x3c)");

// FLPACKET_SERVER_CREATESOLAR -- id 0x20042, registered at 06d9c223.
// Factory the routine: operator new(0xb8) + ctor the routine. Vtable module storage =
// { 06d9e720, 06dac7c0, 06daca80, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_CREATESOLAR : public FRpcCreateObjectPacket {
public:
    // . Wire.Common is built first, by FRpcCreateObjectWire's constructors
    // (06d9e66b..06d9e6c8: the head -- 06d9e67b..06d9e68a is the routine inlined -- then
    // COMMON.DLL's list ctor at +0x74 and DamageList ctor at +0x80); the body is the tail,
    // . +0x9e/+0x9f and Affiliation / Attitude (+0xb0 / +0xb4) are not
    // stored by baseline's constructor and are not stored here.
    FPacket_FLPACKET_SERVER_CREATESOLAR() : FRpcCreateObjectPacket(0x20042u) {
        Wire.NameASize = 0;               // +0xa0
        Wire.NameABuffer = nullptr;       // +0xa4
        Wire.NameBSize = 0;               // +0xa8
        Wire.NameBBuffer = nullptr;       // +0xac
        Wire.Field_0x7c = 0;              // byte +0x9c
        Wire.Field_0x7d = 0;              // byte +0x9d
    }

    ~FPacket_FLPACKET_SERVER_CREATESOLAR() override {
        if (Wire.NameBBuffer != nullptr) {                     // (+0xac)
            ::operator delete(Wire.NameBBuffer);
        }
        if (Wire.NameABuffer != nullptr) {                     // (+0xa4)
            ::operator delete(Wire.NameABuffer);
        }
        // stores the abstract base vptr 06db3fb4; Wire.Common's destructor
        // (06d9e791..06d9e83c) and ~FPacketBase follow.
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeServerCreateSolar(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeServerCreateSolar(*this, Buffer, Size, OutLength);
    }
    FPayloadServerCreateSolar Wire;   // +0x20; baseline's class is operator new(0xb8)
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_CREATESOLAR) == 0x20u + sizeof(FPayloadServerCreateSolar),
              "FLPACKET_SERVER_CREATESOLAR: the payload member starts at +0x20");

// FLPACKET_SERVER_SETCOLLISIONGROUPS -- id 0x20048, registered at 06d9c239.
// Factory the routine: operator new(0x2c) + ctor the routine. Vtable module storage =
// { 06da1130, 06dacca0, 06dacd70, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_SETCOLLISIONGROUPS : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_SETCOLLISIONGROUPS() : FPacketBase(0x20048u) {
        RpcPacketBuildListSentinel(this, 0x20u, 0x10u);   // operator new(0x10), Head
    }

    ~FPacket_FLPACKET_SERVER_SETCOLLISIONGROUPS() override {
        // (one body, shared with the other collision-group class):
        // the list at +0x20 (Head +0x24, Size +0x28).
        RpcPacketDestroyEmbeddedList(this, 0x20u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireCollisionGroups(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireCollisionGroups(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0xc];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_SETCOLLISIONGROUPS) == 0x2cu, "FLPACKET_SERVER_SETCOLLISIONGROUPS is operator new(0x2c)");

// FLPACKET_SERVER_SETHULLSTATUS -- id 0x20049, registered at 06d9c24f.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db1cd0, 06db03f0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_SETHULLSTATUS : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_SETHULLSTATUS() : FPacketBase(0x20049u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireFloat(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDword(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x4];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_SETHULLSTATUS) == 0x24u, "FLPACKET_SERVER_SETHULLSTATUS is operator new(0x24)");

// FLPACKET_SERVER_SETGUIDEDTARGET -- id 0x2004a, registered at 06d9c265.
// Factory the routine: operator new(0x2c) + ctor the routine. Vtable module storage =
// { 06d9af50, 06daaf90, 06dacea0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_SETGUIDEDTARGET : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_SETGUIDEDTARGET() : FPacketBase(0x2004au) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 4, 0u);
        RpcPacketStoreField(this, 0x28u, 2, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDwordDwordWord(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDwordDwordWord(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0xc];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_SETGUIDEDTARGET) == 0x2cu, "FLPACKET_SERVER_SETGUIDEDTARGET is operator new(0x2c)");

// FLPACKET_SERVER_SENDCOMM -- id 0x2004f, registered at 06d9c27b.
// Factory the routine: operator new(0x78) + ctor the routine. Vtable module storage =
// { 06d9eaf0, 06dad000, 06dad320, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_SENDCOMM : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_SENDCOMM() : FPacketBase(0x2004fu) {
        RpcPacketStoreField(this, 0x2cu, 4, 0u);
        RpcPacketStoreField(this, 0x30u, 4, 0u);
        RpcPacketStoreField(this, 0x34u, 4, 0u);
        RpcPacketStoreField(this, 0x38u, 4, 0u);
        RpcPacketStoreField(this, 0x5cu, 4, 0u);
        RpcPacketStoreField(this, 0x64u, 4, 0u);
        RpcPacketStoreField(this, 0x74u, 1, 0x00000001u);
    }

    ~FPacket_FLPACKET_SERVER_SENDCOMM() override {
        if (RpcPacketField<void*>(this, 0x64u) != nullptr) {
            ::operator delete(RpcPacketField<void*>(this, 0x64u));
        }
        RpcPacketField<void*>(this, 0x64u) = nullptr;           // , unconditional
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeServerSendComm(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeServerSendComm(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x58];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_SENDCOMM) == 0x78u, "FLPACKET_SERVER_SENDCOMM is operator new(0x78)");

// FLPACKET_SERVER_USE_ITEM -- id 0x20051, registered at 06d9c291.
// Factory the routine: operator new(0x28) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db19b0, 06dad690, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_USE_ITEM : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_USE_ITEM() : FPacketBase(0x20051u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 2, 0u);
        RpcPacketStoreField(this, 0x26u, 2, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDwordWordWord(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDwordWordWord(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x8];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_USE_ITEM) == 0x28u, "FLPACKET_SERVER_USE_ITEM is operator new(0x28)");

// FLPACKET_SERVER_SCANNOTIFY -- id 0x20039, registered at 06d9c2a7.
// Factory the routine: operator new(0x28) + ctor the routine. Vtable module storage =
// { 06d9af50, 06da9110, 06d95d40, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_SCANNOTIFY : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_SCANNOTIFY() : FPacketBase(0x20039u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDwordPair(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDwordPair(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x8];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_SCANNOTIFY) == 0x28u, "FLPACKET_SERVER_SCANNOTIFY is operator new(0x28)");

// FLPACKET_SERVER_PLAYERLIST -- id 0x20052, registered at 06d9c2bd.
// Factory the routine: operator new(0x30) + ctor the routine. Vtable module storage =
// { 06d99930, 06d99990, 06d99b60, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_PLAYERLIST : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_PLAYERLIST() : FPacketBase(0x20052u) {
        RpcPacketStoreField(this, 0x20u, 4, 0x00000003u);
        RpcPacketStoreField(this, 0x24u, 4, 0u);
        RpcPacketStoreField(this, 0x28u, 4, 0u);
        RpcPacketStoreField(this, 0x2cu, 1, 0u);
    }

    ~FPacket_FLPACKET_SERVER_PLAYERLIST() override {
        RpcPacketField<unsigned int>(this, 0x28u) = 0;          // , before the test
        if (RpcPacketField<void*>(this, 0x24u) != nullptr) {
            ::free(RpcPacketField<void*>(this, 0x24u));          // MSVCRT free, not delete
            RpcPacketField<void*>(this, 0x24u) = nullptr;
        }
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeServerPlayerList(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeServerPlayerList(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x10];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_PLAYERLIST) == 0x30u, "FLPACKET_SERVER_PLAYERLIST is operator new(0x30)");

// FLPACKET_SERVER_REMOTEOBJECTCARGOUPDATE -- id 0x2003d, registered at 06d9c2d3.
// Factory the routine: operator new(0x2c) + ctor the routine. Vtable module storage =
// { 06d8f820, 06d8f840, 06d8f970, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_REMOTEOBJECTCARGOUPDATE : public FRpcEquipListPacket {
public:
    FPacket_FLPACKET_SERVER_REMOTEOBJECTCARGOUPDATE() : FRpcEquipListPacket(0x2003du) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 1, 0u);
        RpcPacketStoreField(this, 0x28u, 4, 0u);
    }

    ~FPacket_FLPACKET_SERVER_REMOTEOBJECTCARGOUPDATE() override {
        // Own vptr store, then JMP the routine -- ~FRpcEquipListPacket.
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeServerRemoteObjectCargoUpdate(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeServerRemoteObjectCargoUpdate(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x8];   // +0x24
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_REMOTEOBJECTCARGOUPDATE) == 0x2cu, "FLPACKET_SERVER_REMOTEOBJECTCARGOUPDATE is operator new(0x2c)");

// FLPACKET_SERVER_MISCOBJUPDATE -- id 0x20054, registered at 06d9c2e9.
// Factory the routine: operator new(0x6c) + ctor the routine. Vtable module storage =
// { 06d98aa0, 06d98d30, 06d990d0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_MISCOBJUPDATE : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_MISCOBJUPDATE() : FPacketBase(0x20054u) {
        Wire.Field_0x08 = 0;
        Wire.Field_0x14 = 0;
        Wire.Flags = 0;
        Wire.Field_0x04 = 0;
        Wire.Field_0x18 = 0;
        Wire.Field_0x1c = 0;
        Wire.Field_0x30 = 0;
        Wire.Field_0x34 = 0xffffffffu;
        Wire.Field_0x0c = 0;
        Wire.Field_0x10 = 0;
        Wire.Field_0x48 = 0;
    }

    ~FPacket_FLPACKET_SERVER_MISCOBJUPDATE() override {
        // frees the vector at +0x5c first, 06d98a4e..06d98a8d then the
        // string at +0x44. Both are real std members here (see the class), destroyed in
        // reverse declaration order: Ids, then Text -- the same order.
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeServerMiscObjUpdate(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeServerMiscObjUpdate(*this, Buffer, Size, OutLength);
    }
    FPayloadServerMiscObjUpdate Wire;   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_MISCOBJUPDATE) == 0x20u + sizeof(FPayloadServerMiscObjUpdate),
              "FLPACKET_SERVER_MISCOBJUPDATE: the payload member starts at +0x20");

// FLPACKET_SERVER_FORMATION_UPDATE -- id 0x20053, registered at 06d9c2ff.
// Factory the routine: operator new(0x30) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db14f0, 06dad820, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_FORMATION_UPDATE : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_FORMATION_UPDATE() : FPacketBase(0x20053u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 4, 0u);
        RpcPacketStoreField(this, 0x28u, 4, 0u);
        RpcPacketStoreField(this, 0x2cu, 4, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDwordBlock12(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDwordBlock12(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x10];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_FORMATION_UPDATE) == 0x30u, "FLPACKET_SERVER_FORMATION_UPDATE is operator new(0x30)");

// FLPACKET_SERVER_OBJECTCARGOUPDATE -- id 0x20055, registered at 06d9c315.
// Factory the routine: operator new(0x34) + ctor the routine. Vtable module storage =
// { 06d9eeb0, 06dad9b0, 06dadb80, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_OBJECTCARGOUPDATE : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_OBJECTCARGOUPDATE() : FPacketBase(0x20055u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 4, 0u);
        RpcPacketStoreField(this, 0x28u, 2, 0u);
        RpcPacketStoreField(this, 0x2au, 2, 0x00000001u);
        RpcPacketStoreField(this, 0x2cu, 4, 0x3f800000u);
        RpcPacketStoreField(this, 0x30u, 1, 0u);
    }

    ~FPacket_FLPACKET_SERVER_OBJECTCARGOUPDATE() override {
        // Own vptr store, then straight into CDAPacket's dtor: nothing to free.
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeServerObjectCargoUpdate(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeServerObjectCargoUpdate(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x14];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_OBJECTCARGOUPDATE) == 0x34u, "FLPACKET_SERVER_OBJECTCARGOUPDATE is operator new(0x34)");

// FLPACKET_SERVER_SENDNNMESSAGE -- id 0x20056, registered at 06d9c32b.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db0310, 06db03f0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_SENDNNMESSAGE : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_SENDNNMESSAGE() : FPacketBase(0x20056u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDword(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDword(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x4];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_SENDNNMESSAGE) == 0x24u, "FLPACKET_SERVER_SENDNNMESSAGE is operator new(0x24)");

// FLPACKET_SERVER_SET_MUSIC -- id 0x20057, registered at 06d9c341.
// Factory the routine: operator new(0x38) + ctor the routine. Vtable module storage =
// { 06d9af50, 06dadd60, 06dadf30, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_SET_MUSIC : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_SET_MUSIC() : FPacketBase(0x20057u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeServerSetMusic(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeServerSetMusic(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x18];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_SET_MUSIC) == 0x38u, "FLPACKET_SERVER_SET_MUSIC is operator new(0x38)");

// FLPACKET_SERVER_CANCEL_MUSIC -- id 0x20058, registered at 06d9c357.
// Factory the routine: operator new(0x20) + ctor the routine. Vtable module storage =
// { 06d9af50, 06dae0d0, 06dabad0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_CANCEL_MUSIC : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_CANCEL_MUSIC() : FPacketBase(0x20058u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireEmpty(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireEmpty(*this, Buffer, Size, OutLength);
    }
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_CANCEL_MUSIC) == 0x20u, "FLPACKET_SERVER_CANCEL_MUSIC is operator new(0x20)");

// FLPACKET_SERVER_PLAY_SOUND_EFFECT -- id 0x20059, registered at 06d9c36d.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db0310, 06db03f0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_SERVER_PLAY_SOUND_EFFECT : public FPacketBase {
public:
    FPacket_FLPACKET_SERVER_PLAY_SOUND_EFFECT() : FPacketBase(0x20059u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDword(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDword(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x4];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_SERVER_PLAY_SOUND_EFFECT) == 0x24u, "FLPACKET_SERVER_PLAY_SOUND_EFFECT is operator new(0x24)");

// ===========================================================================
// Channel 3 -- the classes registered by the routine, in its registration order.
// ===========================================================================

// FLPACKET_CLIENT_REQCHANGECASH -- id 0x3001c, registered at 06d9f10d.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db0310, 06db03f0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_REQCHANGECASH : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_REQCHANGECASH() : FPacketBase(0x3001cu) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDword(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDword(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x4];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_REQCHANGECASH) == 0x24u, "FLPACKET_CLIENT_REQCHANGECASH is operator new(0x24)");

// FLPACKET_CLIENT_REQSETCASH -- id 0x3001b, registered at 06d9f123.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db0310, 06db03f0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_REQSETCASH : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_REQSETCASH() : FPacketBase(0x3001bu) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDword(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDword(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x4];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_REQSETCASH) == 0x24u, "FLPACKET_CLIENT_REQSETCASH is operator new(0x24)");

// FLPACKET_CLIENT_REQREMOVEITEM -- id 0x30019, registered at 06d9f139.
// Factory the routine: operator new(0x28) + ctor the routine. Vtable module storage =
// { 06da0600, 06da9110, 06d95d40, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_REQREMOVEITEM : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_REQREMOVEITEM() : FPacketBase(0x30019u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 4, 0xffffffffu);
    }

    ~FPacket_FLPACKET_CLIENT_REQREMOVEITEM() override {
        // Own vptr store, then straight into CDAPacket's dtor: nothing to free.
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDwordPair(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDwordPair(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x8];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_REQREMOVEITEM) == 0x28u, "FLPACKET_CLIENT_REQREMOVEITEM is operator new(0x28)");

// FLPACKET_CLIENT_REQMODIFYITEM -- id 0x3001a, registered at 06d9f14f.
// Factory the routine: operator new(0x38) + ctor the routine. Vtable module storage =
// { 06d9a200, 06d9a220, 06d9a410, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_REQMODIFYITEM : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_REQMODIFYITEM() : FPacketBase(0x3001au) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 4, 0u);
        RpcPacketStoreField(this, 0x28u, 4, 0u);
        RpcPacketStoreField(this, 0x2cu, 4, 0u);
        RpcPacketStoreField(this, 0x30u, 1, 0u);
        RpcPacketStoreField(this, 0x34u, 4, 0u);
    }

    ~FPacket_FLPACKET_CLIENT_REQMODIFYITEM() override {
        if (RpcPacketField<void*>(this, 0x34u) != nullptr) {
            ::operator delete(RpcPacketField<void*>(this, 0x34u));
        }
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeClientReqModifyItem(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeClientReqModifyItem(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x18];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_REQMODIFYITEM) == 0x38u, "FLPACKET_CLIENT_REQMODIFYITEM is operator new(0x38)");

// FLPACKET_CLIENT_REQCARGO -- id 0x30017, registered at 06d9f165.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d95d00, 06d916a0, 06d95bf0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_REQCARGO : public FRpcEquipListPacket {
public:
    FPacket_FLPACKET_CLIENT_REQCARGO() : FRpcEquipListPacket(0x30017u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
    }

    ~FPacket_FLPACKET_CLIENT_REQCARGO() override {
        // Own vptr store, then JMP the routine -- ~FRpcEquipListPacket.
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireEquipList(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireEquipList(*this, Buffer, Size, OutLength);
    }
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_REQCARGO) == 0x24u, "FLPACKET_CLIENT_REQCARGO is operator new(0x24)");

// FLPACKET_CLIENT_REQADDITEM -- id 0x30018, registered at 06d9f17b.
// Factory the routine: operator new(0x38) + ctor the routine. Vtable module storage =
// { 06d99d70, 06d99d90, 06d99f80, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_REQADDITEM : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_REQADDITEM() : FPacketBase(0x30018u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 4, 0u);
        RpcPacketStoreField(this, 0x28u, 4, 0u);
        RpcPacketStoreField(this, 0x2cu, 4, 0u);
        RpcPacketStoreField(this, 0x30u, 1, 0u);
        RpcPacketStoreField(this, 0x34u, 4, 0u);
    }

    ~FPacket_FLPACKET_CLIENT_REQADDITEM() override {
        if (RpcPacketField<void*>(this, 0x34u) != nullptr) {
            ::operator delete(RpcPacketField<void*>(this, 0x34u));
        }
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeClientReqAddItem(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeClientReqAddItem(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x18];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_REQADDITEM) == 0x38u, "FLPACKET_CLIENT_REQADDITEM is operator new(0x38)");

// FLPACKET_CLIENT_REQSHIPARCH -- id 0x30015, registered at 06d9f191.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db0310, 06db03f0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_REQSHIPARCH : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_REQSHIPARCH() : FPacketBase(0x30015u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDword(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDword(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x4];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_REQSHIPARCH) == 0x24u, "FLPACKET_CLIENT_REQSHIPARCH is operator new(0x24)");

// FLPACKET_CLIENT_REQEQUIPMENT -- id 0x30016, registered at 06d9f1a7.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d95bd0, 06d916a0, 06d95bf0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_REQEQUIPMENT : public FRpcEquipListPacket {
public:
    FPacket_FLPACKET_CLIENT_REQEQUIPMENT() : FRpcEquipListPacket(0x30016u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
    }

    ~FPacket_FLPACKET_CLIENT_REQEQUIPMENT() override {
        // Own vptr store, then JMP the routine -- ~FRpcEquipListPacket.
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireEquipList(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireEquipList(*this, Buffer, Size, OutLength);
    }
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_REQEQUIPMENT) == 0x24u, "FLPACKET_CLIENT_REQEQUIPMENT is operator new(0x24)");

// FLPACKET_CLIENT_MUNCOLLISION -- id 0x30003, registered at 06d9f1bd.
// Factory the routine: operator new(0x3c) + ctor the routine. Vtable module storage =
// { 06d9af50, 06dae280, 06dae430, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_MUNCOLLISION : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_MUNCOLLISION() : FPacketBase(0x30003u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 4, 0u);
        RpcPacketStoreField(this, 0x28u, 4, 0u);
        RpcPacketStoreField(this, 0x2cu, 2, SUBOBJ_ID_NONE);   // COMMON.DLL SUBOBJ_ID_NONE
        RpcPacketStoreField(this, 0x30u, 4, 0u);
        RpcPacketStoreField(this, 0x34u, 4, 0u);
        RpcPacketStoreField(this, 0x38u, 4, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeClientMunCollision(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeClientMunCollision(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x1c];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_MUNCOLLISION) == 0x3cu, "FLPACKET_CLIENT_MUNCOLLISION is operator new(0x3c)");

// FLPACKET_CLIENT_BADLANDSOBJCOLLISION -- id 0x3003d, registered at 06d9f1d3.
// Factory the routine: operator new(0x2c) + ctor the routine. Vtable module storage =
// { 06d9af50, 06dae620, 06da78d0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_BADLANDSOBJCOLLISION : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_BADLANDSOBJCOLLISION() : FPacketBase(0x3003du) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 2, 0u);
        RpcPacketStoreField(this, 0x28u, 4, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDwordWordDword(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDwordWordDword(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0xc];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_BADLANDSOBJCOLLISION) == 0x2cu, "FLPACKET_CLIENT_BADLANDSOBJCOLLISION is operator new(0x2c)");

// FLPACKET_CLIENT_OBJCOLLISION -- id 0x3000c, registered at 06d9f1e9.
// Factory the routine: operator new(0x34) + ctor the routine. Vtable module storage =
// { 06d9af50, 06dae770, 06dae910, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_OBJCOLLISION : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_OBJCOLLISION() : FPacketBase(0x3000cu) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 2, 0u);
        RpcPacketStoreField(this, 0x28u, 4, 0u);
        RpcPacketStoreField(this, 0x2cu, 2, 0u);
        RpcPacketStoreField(this, 0x30u, 4, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeClientObjCollision(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeClientObjCollision(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x14];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_OBJCOLLISION) == 0x34u, "FLPACKET_CLIENT_OBJCOLLISION is operator new(0x34)");

// FLPACKET_CLIENT_REQUEST_EVENT -- id 0x30031, registered at 06d9f1ff.
// Factory the routine: operator new(0x34) + ctor the routine. Vtable module storage =
// { 06d9af50, 06daa240, 06daa3e0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_REQUEST_EVENT : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_REQUEST_EVENT() : FPacketBase(0x30031u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeClientRequestEvent(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeClientRequestEvent(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x14];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_REQUEST_EVENT) == 0x34u, "FLPACKET_CLIENT_REQUEST_EVENT is operator new(0x34)");

// FLPACKET_CLIENT_REQUEST_CANCEL -- id 0x30032, registered at 06d9f215.
// Factory the routine: operator new(0x30) + ctor the routine. Vtable module storage =
// { 06d9af50, 06daa560, 06daa6d0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_REQUEST_CANCEL : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_REQUEST_CANCEL() : FPacketBase(0x30032u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeClientRequestCancel(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeClientRequestCancel(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x10];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_REQUEST_CANCEL) == 0x30u, "FLPACKET_CLIENT_REQUEST_CANCEL is operator new(0x30)");

// FLPACKET_CLIENT_CARGOSCAN -- id 0x3002b, registered at 06d9f22b.
// Factory the routine: operator new(0x28) + ctor the routine. Vtable module storage =
// { 06d9af50, 06da9110, 06d95d40, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_CARGOSCAN : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_CARGOSCAN() : FPacketBase(0x3002bu) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDwordPair(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDwordPair(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x8];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_CARGOSCAN) == 0x28u, "FLPACKET_CLIENT_CARGOSCAN is operator new(0x28)");

// FLPACKET_CLIENT_REQUESTLAUNCH -- id 0x30004, registered at 06d9f241.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db0310, 06db03f0, 06daf370 }.
class FPacket_FLPACKET_CLIENT_REQUESTLAUNCH : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_REQUESTLAUNCH() : FPacketBase(0x30004u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDword(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDword(*this, Buffer, Size, OutLength);
    }
    void Dump(std::ostream& Out) const override {
        RpcDumpClientRequestLaunch(*this, Out);
    }
    unsigned char Body[0x4];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_REQUESTLAUNCH) == 0x24u, "FLPACKET_CLIENT_REQUESTLAUNCH is operator new(0x24)");

// FLPACKET_CLIENT_REQUESTCHARINFO -- id 0x30005, registered at 06d9f257.
// Factory the routine: operator new(0x20) + ctor the routine. Vtable module storage =
// { 06d9af50, 06da13d0, 06da1470, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_REQUESTCHARINFO : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_REQUESTCHARINFO() : FPacketBase(0x30005u) {
    }
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_REQUESTCHARINFO) == 0x20u, "FLPACKET_CLIENT_REQUESTCHARINFO is operator new(0x20)");

// FLPACKET_CLIENT_SKIP_AUTOSAVE -- id 0x30042, registered at 06d9f26d.
// Factory the routine: operator new(0x20) + ctor the routine. Vtable module storage =
// { 06d9af50, 06da13d0, 06da1470, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_SKIP_AUTOSAVE : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_SKIP_AUTOSAVE() : FPacketBase(0x30042u) {
    }
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_SKIP_AUTOSAVE) == 0x20u, "FLPACKET_CLIENT_SKIP_AUTOSAVE is operator new(0x20)");

// FLPACKET_CLIENT_SELECTCHARACTER -- id 0x30006, registered at 06d9f283.
// Factory the routine: operator new(0x220) + ctor the routine. Vtable module storage =
// { 06d9af50, 06daf830, 06daf970, 06dafab0 }.
class FPacket_FLPACKET_CLIENT_SELECTCHARACTER : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_SELECTCHARACTER() : FPacketBase(0x30006u) {
        RpcPacketStoreField(this, 0x20u, 1, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireCharacterId(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireCharacterId(*this, Buffer, Size, OutLength);
    }
    void Dump(std::ostream& Out) const override {
        RpcDumpWireCharacterIdDump(*this, Out);
    }
    unsigned char Body[0x200];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_SELECTCHARACTER) == 0x220u, "FLPACKET_CLIENT_SELECTCHARACTER is operator new(0x220)");

// FLPACKET_CLIENT_COMMCOMPLETE -- id 0x30037, registered at 06d9f299.
// Factory the routine: operator new(0x30) + ctor the routine. Vtable module storage =
// { 06d9af50, 06daffc0, 06d9ab00, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_COMMCOMPLETE : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_COMMCOMPLETE() : FPacketBase(0x30037u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireFourDwords(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireFourDwords(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x10];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_COMMCOMPLETE) == 0x30u, "FLPACKET_CLIENT_COMMCOMPLETE is operator new(0x30)");

// FLPACKET_CLIENT_REQUESTNEWCHARINFO -- id 0x30038, registered at 06d9f2af.
// Factory the routine: operator new(0x20) + ctor the routine. Vtable module storage =
// { 06d9af50, 06da13d0, 06da1470, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_REQUESTNEWCHARINFO : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_REQUESTNEWCHARINFO() : FPacketBase(0x30038u) {
    }
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_REQUESTNEWCHARINFO) == 0x20u, "FLPACKET_CLIENT_REQUESTNEWCHARINFO is operator new(0x20)");

// FLPACKET_CLIENT_CREATENEWCHAR -- id 0x30039, registered at 06d9f2c5.
// Factory the routine: operator new(0xb0) + ctor the routine. Vtable module storage =
// { 06d9af50, 06daf410, 06daf5b0, 06daf760 }.
class FPacket_FLPACKET_CLIENT_CREATENEWCHAR : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_CREATENEWCHAR() : FPacketBase(0x30039u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeClientCreateNewChar(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeClientCreateNewChar(*this, Buffer, Size, OutLength);
    }
    void Dump(std::ostream& Out) const override {
        RpcDumpClientCreateNewChar(*this, Out);
    }
    unsigned char Body[0x90];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_CREATENEWCHAR) == 0xb0u, "FLPACKET_CLIENT_CREATENEWCHAR is operator new(0xb0)");

// FLPACKET_CLIENT_DESTROYCHAR -- id 0x3003a, registered at 06d9f2db.
// Factory the routine: operator new(0x220) + ctor the routine. Vtable module storage =
// { 06d9af50, 06daf830, 06daf970, 06dafab0 }.
class FPacket_FLPACKET_CLIENT_DESTROYCHAR : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_DESTROYCHAR() : FPacketBase(0x3003au) {
        RpcPacketStoreField(this, 0x20u, 1, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireCharacterId(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireCharacterId(*this, Buffer, Size, OutLength);
    }
    void Dump(std::ostream& Out) const override {
        RpcDumpWireCharacterIdDump(*this, Out);
    }
    unsigned char Body[0x200];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_DESTROYCHAR) == 0x220u, "FLPACKET_CLIENT_DESTROYCHAR is operator new(0x220)");

// FLPACKET_CLIENT_LOGIN -- id 0x30001, registered at 06d9f2f1.
// Factory the routine: operator new(0x424) + ctor the routine. Vtable module storage =
// { 06d9af50, 06dafb10, 06dafd40, 06daff50 }.
class FPacket_FLPACKET_CLIENT_LOGIN : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_LOGIN() : FPacketBase(0x30001u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeClientLogin(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeClientLogin(*this, Buffer, Size, OutLength);
    }
    void Dump(std::ostream& Out) const override {
        RpcDumpClientLogin(*this, Out);
    }
    unsigned char Body[0x404];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_LOGIN) == 0x424u, "FLPACKET_CLIENT_LOGIN is operator new(0x424)");

// FLPACKET_CLIENT_ENTERBASE -- id 0x30007, registered at 06d9f307.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db0310, 06db03f0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_ENTERBASE : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_ENTERBASE() : FPacketBase(0x30007u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDword(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDword(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x4];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_ENTERBASE) == 0x24u, "FLPACKET_CLIENT_ENTERBASE is operator new(0x24)");

// FLPACKET_CLIENT_REQUESTBASEINFO -- id 0x30008, registered at 06d9f31d.
// Factory the routine: operator new(0x28) + ctor the routine. Vtable module storage =
// { 06d9af50, 06dab0e0, 06daf230, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_REQUESTBASEINFO : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_REQUESTBASEINFO() : FPacketBase(0x30008u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDwordByte(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDwordByte(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x8];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_REQUESTBASEINFO) == 0x28u, "FLPACKET_CLIENT_REQUESTBASEINFO is operator new(0x28)");

// FLPACKET_CLIENT_REQUESTLOCATIONINFO -- id 0x30009, registered at 06d9f333.
// Factory the routine: operator new(0x28) + ctor the routine. Vtable module storage =
// { 06d9af50, 06dab0e0, 06daf230, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_REQUESTLOCATIONINFO : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_REQUESTLOCATIONINFO() : FPacketBase(0x30009u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDwordByte(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDwordByte(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x8];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_REQUESTLOCATIONINFO) == 0x28u, "FLPACKET_CLIENT_REQUESTLOCATIONINFO is operator new(0x28)");

// FLPACKET_CLIENT_GFREQUESTSHIPINFO -- id 0x3000a, registered at 06d9f349.
// Factory the routine: operator new(0x20) + ctor the routine. Vtable module storage =
// { 06d9af50, 06da13d0, 06da1470, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_GFREQUESTSHIPINFO : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_GFREQUESTSHIPINFO() : FPacketBase(0x3000au) {
    }
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_GFREQUESTSHIPINFO) == 0x20u, "FLPACKET_CLIENT_GFREQUESTSHIPINFO is operator new(0x20)");

// FLPACKET_CLIENT_SYSTEM_SWITCH_OUT_COMPLETE -- id 0x3000b, registered at 06d9f35f.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db0310, 06db03f0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_SYSTEM_SWITCH_OUT_COMPLETE : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_SYSTEM_SWITCH_OUT_COMPLETE() : FPacketBase(0x3000bu) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDword(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDword(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x4];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_SYSTEM_SWITCH_OUT_COMPLETE) == 0x24u, "FLPACKET_CLIENT_SYSTEM_SWITCH_OUT_COMPLETE is operator new(0x24)");

// FLPACKET_CLIENT_EXITBASE -- id 0x3000d, registered at 06d9f375.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db0310, 06db03f0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_EXITBASE : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_EXITBASE() : FPacketBase(0x3000du) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDword(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDword(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x4];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_EXITBASE) == 0x24u, "FLPACKET_CLIENT_EXITBASE is operator new(0x24)");

// FLPACKET_CLIENT_ENTERLOCATION -- id 0x3000e, registered at 06d9f38b.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db0310, 06db03f0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_ENTERLOCATION : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_ENTERLOCATION() : FPacketBase(0x3000eu) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDword(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDword(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x4];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_ENTERLOCATION) == 0x24u, "FLPACKET_CLIENT_ENTERLOCATION is operator new(0x24)");

// FLPACKET_CLIENT_EXITLOCATION -- id 0x3000f, registered at 06d9f3a1.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db0310, 06db03f0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_EXITLOCATION : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_EXITLOCATION() : FPacketBase(0x3000fu) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDword(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDword(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x4];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_EXITLOCATION) == 0x24u, "FLPACKET_CLIENT_EXITLOCATION is operator new(0x24)");

// FLPACKET_CLIENT_REQUESTCREATESHIP -- id 0x30010, registered at 06d9f3b7.
// Factory the routine: operator new(0x20) + ctor the routine. Vtable module storage =
// { 06d9af50, 06da13d0, 06da1470, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_REQUESTCREATESHIP : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_REQUESTCREATESHIP() : FPacketBase(0x30010u) {
    }
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_REQUESTCREATESHIP) == 0x20u, "FLPACKET_CLIENT_REQUESTCREATESHIP is operator new(0x20)");

// FLPACKET_CLIENT_INTERFACEITEMUSED -- id 0x30035, registered at 06d9f3cd.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db0310, 06db03f0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_INTERFACEITEMUSED : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_INTERFACEITEMUSED() : FPacketBase(0x30035u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDword(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDword(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x4];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_INTERFACEITEMUSED) == 0x24u, "FLPACKET_CLIENT_INTERFACEITEMUSED is operator new(0x24)");

// FLPACKET_CLIENT_RTCDONE -- id 0x30047, registered at 06d9f3e3.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db0310, 06db03f0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_RTCDONE : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_RTCDONE() : FPacketBase(0x30047u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDword(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDword(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x4];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_RTCDONE) == 0x24u, "FLPACKET_CLIENT_RTCDONE is operator new(0x24)");

// FLPACKET_CLIENT_ABORT_MISSION -- id 0x30041, registered at 06d9f3f9.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db0310, 06db03f0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_ABORT_MISSION : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_ABORT_MISSION() : FPacketBase(0x30041u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDword(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDword(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x4];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_ABORT_MISSION) == 0x24u, "FLPACKET_CLIENT_ABORT_MISSION is operator new(0x24)");

// FLPACKET_CLIENT_LAUNCHCOMPLETE -- id 0x3003e, registered at 06d9f40f.
// Factory the routine: operator new(0x28) + ctor the routine. Vtable module storage =
// { 06d9af50, 06da9110, 06d95d40, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_LAUNCHCOMPLETE : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_LAUNCHCOMPLETE() : FPacketBase(0x3003eu) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDwordPair(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDwordPair(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x8];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_LAUNCHCOMPLETE) == 0x28u, "FLPACKET_CLIENT_LAUNCHCOMPLETE is operator new(0x28)");

// FLPACKET_CLIENT_JUMPINCOMPLETE -- id 0x30043, registered at 06d9f425.
// Factory the routine: operator new(0x28) + ctor the routine. Vtable module storage =
// { 06d9af50, 06da9110, 06d95d40, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_JUMPINCOMPLETE : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_JUMPINCOMPLETE() : FPacketBase(0x30043u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 4, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDwordPair(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDwordPair(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x8];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_JUMPINCOMPLETE) == 0x28u, "FLPACKET_CLIENT_JUMPINCOMPLETE is operator new(0x28)");

// FLPACKET_CLIENT_HAIL -- id 0x3003f, registered at 06d9f43b.
// Factory the routine: operator new(0x2c) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db0160, 06da6e40, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_HAIL : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_HAIL() : FPacketBase(0x3003fu) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 4, 0u);
        RpcPacketStoreField(this, 0x28u, 4, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireThreeDwords(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireThreeDwords(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0xc];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_HAIL) == 0x2cu, "FLPACKET_CLIENT_HAIL is operator new(0x2c)");

// FLPACKET_CLIENT_GFGOODBUY -- id 0x30012, registered at 06d9f451.
// Factory the routine: operator new(0x30) + ctor the routine. Vtable module storage =
// { 06d9af50, 06daffc0, 06d9ab00, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_GFGOODBUY : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_GFGOODBUY() : FPacketBase(0x30012u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireFourDwords(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireFourDwords(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x10];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_GFGOODBUY) == 0x30u, "FLPACKET_CLIENT_GFGOODBUY is operator new(0x30)");

// FLPACKET_CLIENT_GFGOODSELL -- id 0x30011, registered at 06d9f467.
// Factory the routine: operator new(0x2c) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db0160, 06da6e40, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_GFGOODSELL : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_GFGOODSELL() : FPacketBase(0x30011u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireThreeDwords(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireThreeDwords(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0xc];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_GFGOODSELL) == 0x2cu, "FLPACKET_CLIENT_GFGOODSELL is operator new(0x2c)");

// FLPACKET_CLIENT_GFGOODVAPORIZED -- id 0x3003c, registered at 06d9f47d.
// Factory the routine: operator new(0x2c) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db0160, 06da6e40, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_GFGOODVAPORIZED : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_GFGOODVAPORIZED() : FPacketBase(0x3003cu) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireThreeDwords(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireThreeDwords(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0xc];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_GFGOODVAPORIZED) == 0x2cu, "FLPACKET_CLIENT_GFGOODVAPORIZED is operator new(0x2c)");

// FLPACKET_CLIENT_GFSELECTOBJECT -- id 0x30013, registered at 06d9f493.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db0310, 06db03f0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_GFSELECTOBJECT : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_GFSELECTOBJECT() : FPacketBase(0x30013u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDword(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDword(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x4];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_GFSELECTOBJECT) == 0x24u, "FLPACKET_CLIENT_GFSELECTOBJECT is operator new(0x24)");

// FLPACKET_CLIENT_SAVEGAME -- id 0x3001e, registered at 06d9f4a9.
// Factory the routine: operator new(0x228) + ctor the routine. Vtable module storage =
// { 06d9ac90, 06d9a800, 06d9a950, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_SAVEGAME : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_SAVEGAME() : FPacketBase(0x3001eu) {
        RpcPacketStoreField(this, 0x20u, 1, 0u);
    }

    ~FPacket_FLPACKET_CLIENT_SAVEGAME() override {
        if (RpcPacketField<void*>(this, 0x220u) != nullptr) {
            ::operator delete(RpcPacketField<void*>(this, 0x220u));
        }
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeClientSaveGame(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeClientSaveGame(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x208];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_SAVEGAME) == 0x228u, "FLPACKET_CLIENT_SAVEGAME is operator new(0x228)");

// FLPACKET_CLIENT_MINEASTEROID -- id 0x30020, registered at 06d9f4bf.
// Factory the routine: operator new(0x3c) + ctor the routine. Vtable module storage =
// { 06d9af50, 06daeac0, 06daeca0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_MINEASTEROID : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_MINEASTEROID() : FPacketBase(0x30020u) {
        RpcPacketStoreField(this, 0x24u, 4, 0u);
        RpcPacketStoreField(this, 0x28u, 4, 0u);
        RpcPacketStoreField(this, 0x2cu, 4, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeClientMineAsteroid(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeClientMineAsteroid(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x1c];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_MINEASTEROID) == 0x3cu, "FLPACKET_CLIENT_MINEASTEROID is operator new(0x3c)");

// FLPACKET_CLIENT_MISSIONRESPONSE -- id 0x30014, registered at 06d9f4d5.
// Factory the routine: operator new(0x2c) + ctor the routine. Vtable module storage =
// { 06d9af50, 06daeec0, 06daf010, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_MISSIONRESPONSE : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_MISSIONRESPONSE() : FPacketBase(0x30014u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeClientMissionResponse(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeClientMissionResponse(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0xc];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_MISSIONRESPONSE) == 0x2cu, "FLPACKET_CLIENT_MISSIONRESPONSE is operator new(0x2c)");

// FLPACKET_CLIENT_TRADERESPONSE -- id 0x30027, registered at 06d9f4eb.
// Factory the routine: operator new(0x28) + ctor the routine. Vtable module storage =
// { 06d9a7b0, 06da9110, 06d9a660, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_TRADERESPONSE : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_TRADERESPONSE() : FPacketBase(0x30027u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 4, 0u);
    }

    ~FPacket_FLPACKET_CLIENT_TRADERESPONSE() override {
        ::operator delete(RpcPacketField<void*>(this, 0x24u));   // , no test
        RpcPacketField<void*>(this, 0x24u) = nullptr;
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDwordPair(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireTradeResponse(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x8];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_TRADERESPONSE) == 0x28u, "FLPACKET_CLIENT_TRADERESPONSE is operator new(0x28)");

// FLPACKET_CLIENT_DBGCREATESHIP -- id 0x30022, registered at 06d9f501.
// Factory the routine: operator new(0x90) + ctor the routine. Vtable module storage =
// { 06da0ea0, 06db0560, 06db0b30, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_DBGCREATESHIP : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_DBGCREATESHIP() : FPacketBase(0x30022u) {
        RpcPacketStoreField(this, 0x34u, 4, 0u);
        RpcPacketStoreField(this, 0x38u, 4, 0u);
        RpcPacketStoreField(this, 0x3cu, 4, 0u);
        RpcPacketStoreField(this, 0x40u, 4, 0u);
        RpcPacketStoreField(this, 0x44u, 4, 0u);
        RpcPacketStoreField(this, 0x48u, 4, 0u);
        RpcPacketStoreField(this, 0x4cu, 4, 0x3f800000u);
        RpcPacketStoreField(this, 0x50u, 4, 0u);
        RpcPacketStoreField(this, 0x54u, 4, 0u);
        RpcPacketStoreField(this, 0x58u, 4, 0u);
        RpcPacketStoreField(this, 0x5cu, 4, 0u);
        RpcPacketStoreField(this, 0x60u, 4, 0u);
        RpcPacketStoreField(this, 0x64u, 4, 0u);
        RpcPacketStoreField(this, 0x68u, 4, 0u);
        RpcPacketStoreField(this, 0x6cu, 4, 0u);
        RpcPacketStoreField(this, 0x84u, 4, 0u);
        RpcPacketStoreField(this, 0x80u, 4, 0u);
        RpcPacketStoreField(this, 0x7cu, 4, 0u);
        RpcPacketStoreField(this, 0x78u, 4, 0u);
        RpcPacketStoreField(this, 0x74u, 4, 0u);
        RpcPacketStoreField(this, 0x70u, 4, 0u);
        RpcPacketStoreField(this, 0x88u, 4, 0u);
        RpcPacketStoreField(this, 0x8cu, 4, 0u);
    }

    ~FPacket_FLPACKET_CLIENT_DBGCREATESHIP() override {
        if (RpcPacketField<void*>(this, 0x60u) != nullptr) {
            ::operator delete(RpcPacketField<void*>(this, 0x60u));
        }
        if (RpcPacketField<void*>(this, 0x88u) != nullptr) {
            ::operator delete(RpcPacketField<void*>(this, 0x88u));
        }
        if (RpcPacketField<void*>(this, 0x8cu) != nullptr) {
            ::operator delete(RpcPacketField<void*>(this, 0x8cu));
        }
        if (RpcPacketField<void*>(this, 0x84u) != nullptr) {
            ::operator delete(RpcPacketField<void*>(this, 0x84u));
        }
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeClientDbgCreateShip(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeClientDbgCreateShip(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x70];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_DBGCREATESHIP) == 0x90u, "FLPACKET_CLIENT_DBGCREATESHIP is operator new(0x90)");

// FLPACKET_CLIENT_DBGLOADSYSTEM -- id 0x30023, registered at 06d9f517.
// Factory the routine: operator new(0x28) + ctor the routine. Vtable module storage =
// { 06da0ef0, 06da6190, 06db10e0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_DBGLOADSYSTEM : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_DBGLOADSYSTEM() : FPacketBase(0x30023u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 4, 0u);
    }

    ~FPacket_FLPACKET_CLIENT_DBGLOADSYSTEM() override {
        if (RpcPacketField<void*>(this, 0x20u) != nullptr) {
            ::operator delete(RpcPacketField<void*>(this, 0x20u));
        }
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireBlobBufferFirst(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireBlobBufferFirstSkipEmpty(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x8];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_DBGLOADSYSTEM) == 0x28u, "FLPACKET_CLIENT_DBGLOADSYSTEM is operator new(0x28)");

// FLPACKET_CLIENT_DBGFREESYSTEM -- id 0x3002e, registered at 06d9f52d.
// Factory the routine: operator new(0x20) + ctor the routine. Vtable module storage =
// { 06da0f40, 06dae0d0, 06dabad0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_DBGFREESYSTEM : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_DBGFREESYSTEM() : FPacketBase(0x3002eu) {
    }

    ~FPacket_FLPACKET_CLIENT_DBGFREESYSTEM() override {
        // Own vptr store, then straight into CDAPacket's dtor: nothing to free.
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireEmpty(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireEmpty(*this, Buffer, Size, OutLength);
    }
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_DBGFREESYSTEM) == 0x20u, "FLPACKET_CLIENT_DBGFREESYSTEM is operator new(0x20)");

// FLPACKET_CLIENT_DOCK -- id 0x30024, registered at 06d9f543.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db0310, 06db03f0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_DOCK : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_DOCK() : FPacketBase(0x30024u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDword(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDword(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x4];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_DOCK) == 0x24u, "FLPACKET_CLIENT_DOCK is operator new(0x24)");

// FLPACKET_CLIENT_DBGCONSOLE -- id 0x3002d, registered at 06d9f559.
// Factory the routine: operator new(0xa4) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db1670, 06db17a0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_DBGCONSOLE : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_DBGCONSOLE() : FPacketBase(0x3002du) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeClientDbgConsole(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeClientDbgConsole(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x84];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_DBGCONSOLE) == 0xa4u, "FLPACKET_CLIENT_DBGCONSOLE is operator new(0xa4)");

// FLPACKET_CLIENT_DBGDESTROYOBJECT -- id 0x30025, registered at 06d9f56f.
// Factory the routine: operator new(0x28) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db12c0, 06db13e0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_DBGDESTROYOBJECT : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_DBGDESTROYOBJECT() : FPacketBase(0x30025u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeClientDbgDestroyObject(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeClientDbgDestroyObject(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x8];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_DBGDESTROYOBJECT) == 0x28u, "FLPACKET_CLIENT_DBGDESTROYOBJECT is operator new(0x28)");

// FLPACKET_CLIENT_DBGRELOCATE_SHIP -- id 0x30030, registered at 06d9f585.
// Factory the routine: operator new(0x30) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db14f0, 06dad820, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_DBGRELOCATE_SHIP : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_DBGRELOCATE_SHIP() : FPacketBase(0x30030u) {
        RpcPacketStoreField(this, 0x24u, 4, 0u);
        RpcPacketStoreField(this, 0x28u, 4, 0u);
        RpcPacketStoreField(this, 0x2cu, 4, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDwordBlock12(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDwordBlock12(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x10];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_DBGRELOCATE_SHIP) == 0x30u, "FLPACKET_CLIENT_DBGRELOCATE_SHIP is operator new(0x30)");

// FLPACKET_CLIENT_SETMANEUVER -- id 0x3002f, registered at 06d9f59b.
// Factory the routine: operator new(0x2c) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db0160, 06da6e40, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_SETMANEUVER : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_SETMANEUVER() : FPacketBase(0x3002fu) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 4, 0u);
        RpcPacketStoreField(this, 0x28u, 4, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireThreeDwords(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireThreeDwords(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0xc];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_SETMANEUVER) == 0x2cu, "FLPACKET_CLIENT_SETMANEUVER is operator new(0x2c)");

// FLPACKET_CLIENT_REQCOLLISIONGROUPS -- id 0x30036, registered at 06d9f5b1.
// Factory the routine: operator new(0x2c) + ctor the routine. Vtable module storage =
// { 06da1130, 06dacca0, 06dacd70, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_REQCOLLISIONGROUPS : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_REQCOLLISIONGROUPS() : FPacketBase(0x30036u) {
        RpcPacketBuildListSentinel(this, 0x20u, 0x10u);   // operator new(0x10), Head
    }

    ~FPacket_FLPACKET_CLIENT_REQCOLLISIONGROUPS() override {
        // (one body, shared with the other collision-group class):
        // the list at +0x20 (Head +0x24, Size +0x28).
        RpcPacketDestroyEmbeddedList(this, 0x20u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireCollisionGroups(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireCollisionGroups(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0xc];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_REQCOLLISIONGROUPS) == 0x2cu, "FLPACKET_CLIENT_REQCOLLISIONGROUPS is operator new(0x2c)");

// FLPACKET_CLIENT_REQHULLSTATUS -- id 0x3003b, registered at 06d9f5c7.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db1cd0, 06db03f0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_REQHULLSTATUS : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_REQHULLSTATUS() : FPacketBase(0x3003bu) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireFloat(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDword(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x4];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_REQHULLSTATUS) == 0x24u, "FLPACKET_CLIENT_REQHULLSTATUS is operator new(0x24)");

// FLPACKET_CLIENT_REQUEST_USE_ITEM -- id 0x30040, registered at 06d9f5dd.
// Factory the routine: operator new(0x28) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db19b0, 06dad690, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_REQUEST_USE_ITEM : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_REQUEST_USE_ITEM() : FPacketBase(0x30040u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 2, 0u);
        RpcPacketStoreField(this, 0x26u, 2, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDwordWordWord(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDwordWordWord(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x8];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_REQUEST_USE_ITEM) == 0x28u, "FLPACKET_CLIENT_REQUEST_USE_ITEM is operator new(0x28)");

// FLPACKET_CLIENT_REQINVINCIBILITY -- id 0x30044, registered at 06d9f5f3.
// Factory the routine: operator new(0x28) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db1b60, 06da6a50, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_REQINVINCIBILITY : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_REQINVINCIBILITY() : FPacketBase(0x30044u) {
        RpcPacketStoreField(this, 0x20u, 4, 0u);
        RpcPacketStoreField(this, 0x24u, 1, 0u);
        RpcPacketStoreField(this, 0x25u, 1, 0u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDwordTwoBytes(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDwordTwoBytes(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x8];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_REQINVINCIBILITY) == 0x28u, "FLPACKET_CLIENT_REQINVINCIBILITY is operator new(0x28)");

// FLPACKET_CLIENT_MISSIONSAVEB -- id 0x30045, registered at 06d9f609.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db0310, 06db03f0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_MISSIONSAVEB : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_MISSIONSAVEB() : FPacketBase(0x30045u) {
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireDword(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDword(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x4];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_MISSIONSAVEB) == 0x24u, "FLPACKET_CLIENT_MISSIONSAVEB is operator new(0x24)");

// FLPACKET_CLIENT_REQDIFFICULTYSCALE -- id 0x30046, registered at 06d9f61f.
// Factory the routine: operator new(0x24) + ctor the routine. Vtable module storage =
// { 06d9af50, 06db1cd0, 06db03f0, no-op dump (ICF'd onto IServerImpl::GetServerStats) }.
class FPacket_FLPACKET_CLIENT_REQDIFFICULTYSCALE : public FPacketBase {
public:
    FPacket_FLPACKET_CLIENT_REQDIFFICULTYSCALE() : FPacketBase(0x30046u) {
        RpcPacketStoreField(this, 0x20u, 4, 0x3f800000u);
    }
    unsigned int Encode(unsigned char** BufferHolder, unsigned int Capacity,
                        unsigned int* OutLength) override {
        return RpcEncodeWireFloat(*this, BufferHolder, Capacity, OutLength);
    }
    unsigned int Decode(const unsigned char* Buffer, unsigned int Size,
                        unsigned int* OutLength) override {
        return RpcDecodeWireDword(*this, Buffer, Size, OutLength);
    }
    unsigned char Body[0x4];   // +0x20
};
static_assert(sizeof(FPacket_FLPACKET_CLIENT_REQDIFFICULTYSCALE) == 0x24u, "FLPACKET_CLIENT_REQDIFFICULTYSCALE is operator new(0x24)");

}  // namespace

// , __cdecl, one argument. 0x06d9ace5 TEST ESI,ESI / JZ -> MOV EAX,0x4;
// otherwise every RegisterType result is ignored and the function returns 0.
int RpcRegisterChannel1Types(FPacketSerializer* Serializer) {
    if (Serializer == nullptr) {
        return 4;
    }
    Serializer->RegisterType(0x10002u, "FLPACKET_COMMON_FIREWEAPON",
                             &RpcPacketFactory<FPacket_FLPACKET_COMMON_FIREWEAPON>);
    Serializer->RegisterType(0x10001u, "FLPACKET_COMMON_UPDATEOBJECT",
                             &RpcPacketFactory<FPacket_FLPACKET_COMMON_UPDATEOBJECT>);
    Serializer->RegisterType(0x10005u, "FLPACKET_COMMON_CHATMSG",
                             &RpcPacketFactory<FPacket_FLPACKET_COMMON_CHATMSG>);
    Serializer->RegisterType(0x10008u, "FLPACKET_COMMON_ACTIVATEEQUIP",
                             &RpcPacketFactory<FPacket_FLPACKET_COMMON_ACTIVATEEQUIP>);
    Serializer->RegisterType(0x10004u, "FLPACKET_COMMON_SETTARGET",
                             &RpcPacketFactory<FPacket_FLPACKET_COMMON_SETTARGET>);
    Serializer->RegisterType(0x1000eu, "FLPACKET_COMMON_ACTIVATECRUISE",
                             &RpcPacketFactory<FPacket_FLPACKET_COMMON_ACTIVATECRUISE>);
    Serializer->RegisterType(0x10015u, "FLPACKET_COMMON_ACTIVATETHRUSTERS",
                             &RpcPacketFactory<FPacket_FLPACKET_COMMON_ACTIVATETHRUSTERS>);
    Serializer->RegisterType(0x1000fu, "FLPACKET_COMMON_GOTRADELANE",
                             &RpcPacketFactory<FPacket_FLPACKET_COMMON_GOTRADELANE>);
    Serializer->RegisterType(0x10010u, "FLPACKET_COMMON_STOPTRADELANE",
                             &RpcPacketFactory<FPacket_FLPACKET_COMMON_STOPTRADELANE>);
    Serializer->RegisterType(0x10011u, "FLPACKET_COMMON_SET_WEAPON_GROUP",
                             &RpcPacketFactory<FPacket_FLPACKET_COMMON_SET_WEAPON_GROUP>);
    Serializer->RegisterType(0x10012u, "FLPACKET_COMMON_PLAYER_TRADE",
                             &RpcPacketFactory<FPacket_FLPACKET_COMMON_PLAYER_TRADE>);
    Serializer->RegisterType(0x10013u, "FLPACKET_COMMON_SET_VISITED_STATE",
                             &RpcPacketFactory<FPacket_FLPACKET_COMMON_SET_VISITED_STATE>);
    Serializer->RegisterType(0x10014u, "FLPACKET_COMMON_JETTISONCARGO",
                             &RpcPacketFactory<FPacket_FLPACKET_COMMON_JETTISONCARGO>);
    Serializer->RegisterType(0x10016u, "FLPACKET_COMMON_REQUEST_BEST_PATH",
                             &RpcPacketFactory<FPacket_FLPACKET_COMMON_REQUEST_BEST_PATH>);
    Serializer->RegisterType(0x10018u, "FLPACKET_COMMON_REQUEST_PLAYER_STATS",
                             &RpcPacketFactory<FPacket_FLPACKET_COMMON_REQUEST_PLAYER_STATS>);
    Serializer->RegisterType(0x1001bu, "FLPACKET_COMMON_POP_UP_DIALOG",
                             &RpcPacketFactory<FPacket_FLPACKET_COMMON_POP_UP_DIALOG>);
    Serializer->RegisterType(0x10017u, "FLPACKET_COMMON_REQUEST_GROUP_POSITIONS",
                             &RpcPacketFactory<FPacket_FLPACKET_COMMON_REQUEST_GROUP_POSITIONS>);
    Serializer->RegisterType(0x10019u, "FLPACKET_COMMON_SET_MISSION_LOG",
                             &RpcPacketFactory<FPacket_FLPACKET_COMMON_SET_MISSION_LOG>);
    Serializer->RegisterType(0x1001au, "FLPACKET_COMMON_REQUEST_RANK_LEVEL",
                             &RpcPacketFactory<FPacket_FLPACKET_COMMON_REQUEST_RANK_LEVEL>);
    Serializer->RegisterType(0x1001cu, "FLPACKET_COMMON_SET_INTERFACE_STATE",
                             &RpcPacketFactory<FPacket_FLPACKET_COMMON_SET_INTERFACE_STATE>);
    Serializer->RegisterType(0x1001du, "FLPACKET_COMMON_TRACTOROBJECTS",
                             &RpcPacketFactory<FPacket_FLPACKET_COMMON_TRACTOROBJECTS>);
    return 0;
}

// , __cdecl, one argument. 0x06d9bc85 TEST ESI,ESI / JZ -> MOV EAX,0x4;
// otherwise every RegisterType result is ignored and the function returns 0.
int RpcRegisterChannel2Types(FPacketSerializer* Serializer) {
    if (Serializer == nullptr) {
        return 4;
    }
    Serializer->RegisterType(0x20001u, "FLPACKET_SERVER_CONNECTRESPONSE",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_CONNECTRESPONSE>);
    Serializer->RegisterType(0x20002u, "FLPACKET_SERVER_LOGINRESPONSE",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_LOGINRESPONSE>);
    Serializer->RegisterType(0x20005u, "FLPACKET_SERVER_DAMAGEOBJECT",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_DAMAGEOBJECT>);
    Serializer->RegisterType(0x20006u, "FLPACKET_SERVER_DESTROYOBJECT",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_DESTROYOBJECT>);
    Serializer->RegisterType(0x20004u, "FLPACKET_SERVER_CREATESHIP",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_CREATESHIP>);
    Serializer->RegisterType(0x20044u, "FLPACKET_SERVER_REQUEST_RETURNED",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_REQUEST_RETURNED>);
    Serializer->RegisterType(0x20003u, "FLPACKET_SERVER_CHARACTERINFO",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_CHARACTERINFO>);
    Serializer->RegisterType(0x20007u, "FLPACKET_SERVER_LAUNCH",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_LAUNCH>);
    Serializer->RegisterType(0x20008u, "FLPACKET_SERVER_CHARSELECTVERIFIED",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_CHARSELECTVERIFIED>);
    Serializer->RegisterType(0x2000au, "FLPACKET_SERVER_ACTIVATEOBJECT",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_ACTIVATEOBJECT>);
    Serializer->RegisterType(0x2000bu, "FLPACKET_SERVER_LAND",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_LAND>);
    Serializer->RegisterType(0x2000eu, "FLPACKET_SERVER_GFCOMPLETENEWSBROADCASTLIST",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_GFCOMPLETENEWSBROADCASTLIST>);
    Serializer->RegisterType(0x20010u, "FLPACKET_SERVER_GFCOMPLETEMISSIONCOMPUTERLIST",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_GFCOMPLETEMISSIONCOMPUTERLIST>);
    Serializer->RegisterType(0x2000fu, "FLPACKET_SERVER_GFCOMPLETECHARLIST",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_GFCOMPLETECHARLIST>);
    Serializer->RegisterType(0x20011u, "FLPACKET_SERVER_GFCOMPLETESCRIPTBEHAVIORLIST",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_GFCOMPLETESCRIPTBEHAVIORLIST>);
    Serializer->RegisterType(0x20013u, "FLPACKET_SERVER_GFCOMPLETEAMBIENTSCRIPTLIST",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_GFCOMPLETEAMBIENTSCRIPTLIST>);
    Serializer->RegisterType(0x20021u, "FLPACKET_SERVER_SYSTEM_SWITCH_OUT",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_SYSTEM_SWITCH_OUT>);
    Serializer->RegisterType(0x20022u, "FLPACKET_SERVER_SYSTEM_SWITCH_IN",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_SYSTEM_SWITCH_IN>);
    Serializer->RegisterType(0x20023u, "FLPACKET_SERVER_SETSHIPARCH",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_SETSHIPARCH>);
    Serializer->RegisterType(0x20024u, "FLPACKET_SERVER_SETEQUIPMENT",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_SETEQUIPMENT>);
    Serializer->RegisterType(0x20025u, "FLPACKET_SERVER_SETCARGO",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_SETCARGO>);
    Serializer->RegisterType(0x20027u, "FLPACKET_SERVER_REQUESTCREATESHIPRESP",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_REQUESTCREATESHIPRESP>);
    Serializer->RegisterType(0x2000du, "FLPACKET_SERVER_SETSTARTROOM",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_SETSTARTROOM>);
    Serializer->RegisterType(0x20015u, "FLPACKET_SERVER_GFDESTROYCHARACTER",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_GFDESTROYCHARACTER>);
    Serializer->RegisterType(0x20014u, "FLPACKET_SERVER_GFDESTROYNEWSBROADCAST",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_GFDESTROYNEWSBROADCAST>);
    Serializer->RegisterType(0x20016u, "FLPACKET_SERVER_GFDESTROYMISSIONCOMPUTER",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_GFDESTROYMISSIONCOMPUTER>);
    Serializer->RegisterType(0x20017u, "FLPACKET_SERVER_GFDESTROYSCRIPTBEHAVIOR",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_GFDESTROYSCRIPTBEHAVIOR>);
    Serializer->RegisterType(0x20019u, "FLPACKET_SERVER_GFDESTROYAMBIENTSCRIPT",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_GFDESTROYAMBIENTSCRIPT>);
    Serializer->RegisterType(0x2001au, "FLPACKET_SERVER_GFSCRIPTBEHAVIOR",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_GFSCRIPTBEHAVIOR>);
    Serializer->RegisterType(0x2001fu, "FLPACKET_SERVER_GFUPDATEAMBIENTSCRIPT",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_GFUPDATEAMBIENTSCRIPT>);
    Serializer->RegisterType(0x2005bu, "FLPACKET_SERVER_MISSIONSAVEA",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_MISSIONSAVEA>);
    Serializer->RegisterType(0x2001du, "FLPACKET_SERVER_GFUPDATEMISSIONCOMPUTER",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_GFUPDATEMISSIONCOMPUTER>);
    Serializer->RegisterType(0x2001eu, "FLPACKET_SERVER_GFUPDATENEWSBROADCAST",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_GFUPDATENEWSBROADCAST>);
    Serializer->RegisterType(0x20020u, "FLPACKET_SERVER_GFMISSIONVENDORACCEPTANCE",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_GFMISSIONVENDORACCEPTANCE>);
    Serializer->RegisterType(0x2005au, "FLPACKET_SERVER_GFMISSIONVENDORWHYEMPTY",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_GFMISSIONVENDORWHYEMPTY>);
    Serializer->RegisterType(0x20026u, "FLPACKET_SERVER_GFUPDATECHAR",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_GFUPDATECHAR>);
    Serializer->RegisterType(0x20028u, "FLPACKET_SERVER_CREATELOOT",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_CREATELOOT>);
    Serializer->RegisterType(0x20029u, "FLPACKET_SERVER_SETREPUTATION",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_SETREPUTATION>);
    Serializer->RegisterType(0x2002au, "FLPACKET_SERVER_ADJUSTATTITUDE",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_ADJUSTATTITUDE>);
    Serializer->RegisterType(0x2002bu, "FLPACKET_SERVER_SETGROUPFEELINGS",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_SETGROUPFEELINGS>);
    Serializer->RegisterType(0x2002cu, "FLPACKET_SERVER_CREATEMINE",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_CREATEMINE>);
    Serializer->RegisterType(0x2002du, "FLPACKET_SERVER_CREATECOUNTER",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_CREATECOUNTER>);
    Serializer->RegisterType(0x20031u, "FLPACKET_SERVER_EXPLODEASTEROIDMINE",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_EXPLODEASTEROIDMINE>);
    Serializer->RegisterType(0x2004du, "FLPACKET_SERVER_LOADHINT",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_LOADHINT>);
    Serializer->RegisterType(0x2004eu, "FLPACKET_SERVER_SETDIRECTIVE",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_SETDIRECTIVE>);
    Serializer->RegisterType(0x20037u, "FLPACKET_SERVER_CREATEGUIDED",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_CREATEGUIDED>);
    Serializer->RegisterType(0x20038u, "FLPACKET_SERVER_ITEMTRACTORED",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_ITEMTRACTORED>);
    Serializer->RegisterType(0x2002fu, "FLPACKET_SERVER_SETREMOVEITEM",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_SETREMOVEITEM>);
    Serializer->RegisterType(0x2002eu, "FLPACKET_SERVER_SETADDITEM",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_SETADDITEM>);
    Serializer->RegisterType(0x20030u, "FLPACKET_SERVER_SETCASH",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_SETCASH>);
    Serializer->RegisterType(0x20032u, "FLPACKET_SERVER_REQUESTSPACESCRIPT",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_REQUESTSPACESCRIPT>);
    Serializer->RegisterType(0x20033u, "FLPACKET_SERVER_SETMISSIONOBJECTIVESTATE",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_SETMISSIONOBJECTIVESTATE>);
    Serializer->RegisterType(0x20047u, "FLPACKET_SERVER_CFGINTERFACENOTIFICATION",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_CFGINTERFACENOTIFICATION>);
    Serializer->RegisterType(0x20046u, "FLPACKET_SERVER_MARKOBJ",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_MARKOBJ>);
    Serializer->RegisterType(0x20035u, "FLPACKET_SERVER_SETMISSIONOBJECTIVES",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_SETMISSIONOBJECTIVES>);
    Serializer->RegisterType(0x20034u, "FLPACKET_SERVER_REPLACEMISSIONOBJECTIVE",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_REPLACEMISSIONOBJECTIVE>);
    Serializer->RegisterType(0x20043u, "FLPACKET_SERVER_SET_STORY_CUE",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_SET_STORY_CUE>);
    Serializer->RegisterType(0x2004bu, "FLPACKET_SERVER_SET_CAMERA",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_SET_CAMERA>);
    Serializer->RegisterType(0x2004cu, "FLPACKET_SERVER_REVERT_CAMERA",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_REVERT_CAMERA>);
    Serializer->RegisterType(0x20045u, "FLPACKET_SERVER_SET_MISSION_MESSAGE",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_SET_MISSION_MESSAGE>);
    Serializer->RegisterType(0x2003cu, "FLPACKET_SERVER_REPAIROBJECT",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_REPAIROBJECT>);
    Serializer->RegisterType(0x2003eu, "FLPACKET_SERVER_SETNUMKILLS",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_SETNUMKILLS>);
    Serializer->RegisterType(0x2003fu, "FLPACKET_SERVER_SETMISSIONSUCCESSES",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_SETMISSIONSUCCESSES>);
    Serializer->RegisterType(0x20040u, "FLPACKET_SERVER_SETMISSIONFAILURES",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_SETMISSIONFAILURES>);
    Serializer->RegisterType(0x20041u, "FLPACKET_SERVER_BURNFUSE",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_BURNFUSE>);
    Serializer->RegisterType(0x20042u, "FLPACKET_SERVER_CREATESOLAR",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_CREATESOLAR>);
    Serializer->RegisterType(0x20048u, "FLPACKET_SERVER_SETCOLLISIONGROUPS",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_SETCOLLISIONGROUPS>);
    Serializer->RegisterType(0x20049u, "FLPACKET_SERVER_SETHULLSTATUS",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_SETHULLSTATUS>);
    Serializer->RegisterType(0x2004au, "FLPACKET_SERVER_SETGUIDEDTARGET",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_SETGUIDEDTARGET>);
    Serializer->RegisterType(0x2004fu, "FLPACKET_SERVER_SENDCOMM",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_SENDCOMM>);
    Serializer->RegisterType(0x20051u, "FLPACKET_SERVER_USE_ITEM",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_USE_ITEM>);
    Serializer->RegisterType(0x20039u, "FLPACKET_SERVER_SCANNOTIFY",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_SCANNOTIFY>);
    Serializer->RegisterType(0x20052u, "FLPACKET_SERVER_PLAYERLIST",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_PLAYERLIST>);
    Serializer->RegisterType(0x2003du, "FLPACKET_SERVER_REMOTEOBJECTCARGOUPDATE",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_REMOTEOBJECTCARGOUPDATE>);
    Serializer->RegisterType(0x20054u, "FLPACKET_SERVER_MISCOBJUPDATE",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_MISCOBJUPDATE>);
    Serializer->RegisterType(0x20053u, "FLPACKET_SERVER_FORMATION_UPDATE",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_FORMATION_UPDATE>);
    Serializer->RegisterType(0x20055u, "FLPACKET_SERVER_OBJECTCARGOUPDATE",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_OBJECTCARGOUPDATE>);
    Serializer->RegisterType(0x20056u, "FLPACKET_SERVER_SENDNNMESSAGE",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_SENDNNMESSAGE>);
    Serializer->RegisterType(0x20057u, "FLPACKET_SERVER_SET_MUSIC",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_SET_MUSIC>);
    Serializer->RegisterType(0x20058u, "FLPACKET_SERVER_CANCEL_MUSIC",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_CANCEL_MUSIC>);
    Serializer->RegisterType(0x20059u, "FLPACKET_SERVER_PLAY_SOUND_EFFECT",
                             &RpcPacketFactory<FPacket_FLPACKET_SERVER_PLAY_SOUND_EFFECT>);
    return 0;
}

// , __cdecl, one argument. 0x06d9f105 TEST ESI,ESI / JZ -> MOV EAX,0x4;
// otherwise every RegisterType result is ignored and the function returns 0.
int RpcRegisterChannel3Types(FPacketSerializer* Serializer) {
    if (Serializer == nullptr) {
        return 4;
    }
    Serializer->RegisterType(0x3001cu, "FLPACKET_CLIENT_REQCHANGECASH",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_REQCHANGECASH>);
    Serializer->RegisterType(0x3001bu, "FLPACKET_CLIENT_REQSETCASH",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_REQSETCASH>);
    Serializer->RegisterType(0x30019u, "FLPACKET_CLIENT_REQREMOVEITEM",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_REQREMOVEITEM>);
    Serializer->RegisterType(0x3001au, "FLPACKET_CLIENT_REQMODIFYITEM",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_REQMODIFYITEM>);
    Serializer->RegisterType(0x30017u, "FLPACKET_CLIENT_REQCARGO",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_REQCARGO>);
    Serializer->RegisterType(0x30018u, "FLPACKET_CLIENT_REQADDITEM",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_REQADDITEM>);
    Serializer->RegisterType(0x30015u, "FLPACKET_CLIENT_REQSHIPARCH",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_REQSHIPARCH>);
    Serializer->RegisterType(0x30016u, "FLPACKET_CLIENT_REQEQUIPMENT",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_REQEQUIPMENT>);
    Serializer->RegisterType(0x30003u, "FLPACKET_CLIENT_MUNCOLLISION",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_MUNCOLLISION>);
    Serializer->RegisterType(0x3003du, "FLPACKET_CLIENT_BADLANDSOBJCOLLISION",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_BADLANDSOBJCOLLISION>);
    Serializer->RegisterType(0x3000cu, "FLPACKET_CLIENT_OBJCOLLISION",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_OBJCOLLISION>);
    Serializer->RegisterType(0x30031u, "FLPACKET_CLIENT_REQUEST_EVENT",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_REQUEST_EVENT>);
    Serializer->RegisterType(0x30032u, "FLPACKET_CLIENT_REQUEST_CANCEL",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_REQUEST_CANCEL>);
    Serializer->RegisterType(0x3002bu, "FLPACKET_CLIENT_CARGOSCAN",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_CARGOSCAN>);
    Serializer->RegisterType(0x30004u, "FLPACKET_CLIENT_REQUESTLAUNCH",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_REQUESTLAUNCH>);
    Serializer->RegisterType(0x30005u, "FLPACKET_CLIENT_REQUESTCHARINFO",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_REQUESTCHARINFO>);
    Serializer->RegisterType(0x30042u, "FLPACKET_CLIENT_SKIP_AUTOSAVE",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_SKIP_AUTOSAVE>);
    Serializer->RegisterType(0x30006u, "FLPACKET_CLIENT_SELECTCHARACTER",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_SELECTCHARACTER>);
    Serializer->RegisterType(0x30037u, "FLPACKET_CLIENT_COMMCOMPLETE",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_COMMCOMPLETE>);
    Serializer->RegisterType(0x30038u, "FLPACKET_CLIENT_REQUESTNEWCHARINFO",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_REQUESTNEWCHARINFO>);
    Serializer->RegisterType(0x30039u, "FLPACKET_CLIENT_CREATENEWCHAR",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_CREATENEWCHAR>);
    Serializer->RegisterType(0x3003au, "FLPACKET_CLIENT_DESTROYCHAR",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_DESTROYCHAR>);
    Serializer->RegisterType(0x30001u, "FLPACKET_CLIENT_LOGIN",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_LOGIN>);
    Serializer->RegisterType(0x30007u, "FLPACKET_CLIENT_ENTERBASE",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_ENTERBASE>);
    Serializer->RegisterType(0x30008u, "FLPACKET_CLIENT_REQUESTBASEINFO",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_REQUESTBASEINFO>);
    Serializer->RegisterType(0x30009u, "FLPACKET_CLIENT_REQUESTLOCATIONINFO",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_REQUESTLOCATIONINFO>);
    Serializer->RegisterType(0x3000au, "FLPACKET_CLIENT_GFREQUESTSHIPINFO",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_GFREQUESTSHIPINFO>);
    Serializer->RegisterType(0x3000bu, "FLPACKET_CLIENT_SYSTEM_SWITCH_OUT_COMPLETE",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_SYSTEM_SWITCH_OUT_COMPLETE>);
    Serializer->RegisterType(0x3000du, "FLPACKET_CLIENT_EXITBASE",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_EXITBASE>);
    Serializer->RegisterType(0x3000eu, "FLPACKET_CLIENT_ENTERLOCATION",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_ENTERLOCATION>);
    Serializer->RegisterType(0x3000fu, "FLPACKET_CLIENT_EXITLOCATION",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_EXITLOCATION>);
    Serializer->RegisterType(0x30010u, "FLPACKET_CLIENT_REQUESTCREATESHIP",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_REQUESTCREATESHIP>);
    Serializer->RegisterType(0x30035u, "FLPACKET_CLIENT_INTERFACEITEMUSED",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_INTERFACEITEMUSED>);
    Serializer->RegisterType(0x30047u, "FLPACKET_CLIENT_RTCDONE",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_RTCDONE>);
    Serializer->RegisterType(0x30041u, "FLPACKET_CLIENT_ABORT_MISSION",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_ABORT_MISSION>);
    Serializer->RegisterType(0x3003eu, "FLPACKET_CLIENT_LAUNCHCOMPLETE",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_LAUNCHCOMPLETE>);
    Serializer->RegisterType(0x30043u, "FLPACKET_CLIENT_JUMPINCOMPLETE",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_JUMPINCOMPLETE>);
    Serializer->RegisterType(0x3003fu, "FLPACKET_CLIENT_HAIL",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_HAIL>);
    Serializer->RegisterType(0x30012u, "FLPACKET_CLIENT_GFGOODBUY",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_GFGOODBUY>);
    Serializer->RegisterType(0x30011u, "FLPACKET_CLIENT_GFGOODSELL",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_GFGOODSELL>);
    Serializer->RegisterType(0x3003cu, "FLPACKET_CLIENT_GFGOODVAPORIZED",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_GFGOODVAPORIZED>);
    Serializer->RegisterType(0x30013u, "FLPACKET_CLIENT_GFSELECTOBJECT",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_GFSELECTOBJECT>);
    Serializer->RegisterType(0x3001eu, "FLPACKET_CLIENT_SAVEGAME",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_SAVEGAME>);
    Serializer->RegisterType(0x30020u, "FLPACKET_CLIENT_MINEASTEROID",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_MINEASTEROID>);
    Serializer->RegisterType(0x30014u, "FLPACKET_CLIENT_MISSIONRESPONSE",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_MISSIONRESPONSE>);
    Serializer->RegisterType(0x30027u, "FLPACKET_CLIENT_TRADERESPONSE",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_TRADERESPONSE>);
    Serializer->RegisterType(0x30022u, "FLPACKET_CLIENT_DBGCREATESHIP",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_DBGCREATESHIP>);
    Serializer->RegisterType(0x30023u, "FLPACKET_CLIENT_DBGLOADSYSTEM",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_DBGLOADSYSTEM>);
    Serializer->RegisterType(0x3002eu, "FLPACKET_CLIENT_DBGFREESYSTEM",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_DBGFREESYSTEM>);
    Serializer->RegisterType(0x30024u, "FLPACKET_CLIENT_DOCK",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_DOCK>);
    Serializer->RegisterType(0x3002du, "FLPACKET_CLIENT_DBGCONSOLE",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_DBGCONSOLE>);
    Serializer->RegisterType(0x30025u, "FLPACKET_CLIENT_DBGDESTROYOBJECT",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_DBGDESTROYOBJECT>);
    Serializer->RegisterType(0x30030u, "FLPACKET_CLIENT_DBGRELOCATE_SHIP",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_DBGRELOCATE_SHIP>);
    Serializer->RegisterType(0x3002fu, "FLPACKET_CLIENT_SETMANEUVER",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_SETMANEUVER>);
    Serializer->RegisterType(0x30036u, "FLPACKET_CLIENT_REQCOLLISIONGROUPS",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_REQCOLLISIONGROUPS>);
    Serializer->RegisterType(0x3003bu, "FLPACKET_CLIENT_REQHULLSTATUS",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_REQHULLSTATUS>);
    Serializer->RegisterType(0x30040u, "FLPACKET_CLIENT_REQUEST_USE_ITEM",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_REQUEST_USE_ITEM>);
    Serializer->RegisterType(0x30044u, "FLPACKET_CLIENT_REQINVINCIBILITY",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_REQINVINCIBILITY>);
    Serializer->RegisterType(0x30045u, "FLPACKET_CLIENT_MISSIONSAVEB",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_MISSIONSAVEB>);
    Serializer->RegisterType(0x30046u, "FLPACKET_CLIENT_REQDIFFICULTYSCALE",
                             &RpcPacketFactory<FPacket_FLPACKET_CLIENT_REQDIFFICULTYSCALE>);
    return 0;
}

// The callers, as in baseline: IServerImpl::Startup runs the routine (06d8f2cf) and
// (06d8f2df), IClient slot 10 runs the routine (06d8ef4e) and the routine
// (06d8ef5e), each on the serializer the routine (RpcGetPacketSerializer) hands back.
// That is a singleton over .data:the field, so both halves fill ONE table and the
// second channel-1 registration answers 0x42 per id (06d960e4), which these registrators
// ignore.
