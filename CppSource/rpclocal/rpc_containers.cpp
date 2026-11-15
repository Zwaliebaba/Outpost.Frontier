// Author: Giperion.

// rpc_containers.cpp -- the port of rpclocal's container serializer.

// Every function below names the baseline function it reproduces and, where the baseline
// body is not obvious, quotes the instruction that fixes the behaviour. The layouts the
// code walks are documented once, at the top of rpc_containers.h.

// Ownership, as baseline has it:
//   * the equipment handle (payload+0x00 for the four EQUIPMENT/CARGO types) owns its
//     list header, its dummy node, every node, every 0x18 record and every hardpoint
//     string that hangs off a record;
//   * the collision-group list header IS the payload, and owns its dummy node and every
//     node;
//   * the EquipDescList and std::vector<EquipDesc> the receivers build are stack
//     temporaries that die with the call.

// baseline's per-type packet classes do the allocation and the teardown in their own
// constructor/destructor, and so does this port (rpc_packet_registry.cpp); what is left
// here are the list and handle primitives those bodies and the codecs share.

#include "rpc_containers.h"
#include "rpc_payloads.h"

// baseline rpclocal imports COMMON.DLL::EquipDesc::is_internal (module storage) and
// ::make_internal (module storage) by name and uses them as the hardpoint gate on both
// sides of the wire. The project links Common.lib, so the port calls the same two.
#include "common_goods.h"
// ch2 0x04 CREATESHIP / 0x42 CREATESOLAR share a payload codec that calls four more
// COMMON.DLL entry points baseline imports by name, plus one data import. IAT slots read
// off the call bytes, not off the analysis label:
// `ff15 0831db06` -> [06db3108]  Archetype::LargeIDToSmallID
// `ff15 0030db06` -> [06db3000]  Archetype::SmallIDToLargeID
// `ff15 1031db06` -> [06db3110]  Archetype::GetArchMaxHitPts
// `ff15 5c30db06` -> [06db305c]  SinglePlayer
// `a1   0031db06` -> [06db3100]  SUBOBJ_ID_NONE  (a data import: the slot
//                                           holds the ADDRESS of the ushort, and
// reads the word through it)
// The three Archetype ones arrive with common_goods.h -> common_archetype.h;
// SinglePlayer and SUBOBJ_ID_NONE come from common_misc.h.
#include "common_misc.h"

#include <cstddef>
#include <cstring>
#include <map>
#include <new>
#include <string>

// ===========================================================================
// Small helpers. baseline calls MSVCRT's operator new -- the VC6 one, which returns NULL
// on failure rather than throwing -- and the routine (operator delete) directly. The
// port asks for the same contract with std::nothrow and, like baseline, tests the result
// only where the baseline site does: a new-expression with an initializer compiles to
// `TEST EAX,EAX ; JZ`, a bare operator new call does not, and a bare call that fails
// faults on the very next store in baseline and here alike.
// ===========================================================================

// strlen, spelled the way every baseline hardpoint site spells it -- OR ECX,-1 ;
// XOR EAX,EAX ; REPNE SCASB ; NOT ECX, strlen+1 in one step (06d8d04f..06d8d054) --
// with no null test in front. Callers add the 1 back explicitly.
static unsigned int RpcContainerStringLength(const char* Text) {
    unsigned int Length = 0;
    while (Text[Length] != '\0') {
        ++Length;
    }
    return Length;
}

// operator new(SizeWithNul) with no test, then baseline's byte loop that stops after
// copying the NUL (06d8d070..06d8d078, 06d91503..06d9150b). The loop is bounded by the
// terminator, not by the size it allocated.
static char* RpcContainerCopyString(const char* Text, unsigned int SizeWithNul) {
    char* Block = static_cast<char*>(::operator new(SizeWithNul, std::nothrow));
    char* Out = Block;
    char Byte;
    do {
        Byte = *Text;
        ++Text;
        *Out = Byte;
        ++Out;
    } while (Byte != '\0');
    return Block;
}

static void RpcContainerInitRecord(FRpcEquipWireRecord* Record) {
    // / 06d8cffe..06d8d014 / 06d9149f..06d914b7: all eight
    // fields written as zero before anything else, in exactly this order.
    Record->ArchId = 0;
    Record->Count = 0;
    Record->Status = 0.0f;
    Record->Id = 0;
    Record->bEquipped = 0;
    Record->Field_0x0f = 0;
    Record->HardpointSize = 0;
    Record->Pad_0x12[0] = 0;
    Record->Pad_0x12[1] = 0;
    Record->Hardpoint = nullptr;
}

static void RpcContainerFreeRecord(FRpcEquipWireRecord* Record) {
    // at 06d9135b: `TEST EDI,EDI ; JZ` on the record, then
    // `if (rec+0x14) operator delete(rec+0x14)` (06d91362), then operator delete(rec).
    // The HardpointSize field is NOT consulted.
    if (Record == nullptr) {
        return;
    }
    if (Record->Hardpoint != nullptr) {
        ::operator delete(Record->Hardpoint);
    }
    ::operator delete(Record);
}

// ===========================================================================
// `MOV EAX,dword ptr [ECX]; RET`
// ===========================================================================
FRpcEquipWireListHeader* RpcEquipHandleGetList(const FRpcEquipWireHandle* Handle) {
    return Handle->List;
}

// ===========================================================================
// push_back

// baseline splits into two halves. When *this is still null (06d9159a
// `CMP dword ptr [EDI],0x0`) it builds the list: operator new(0xc) for the header,
// one byte of allocator into header+0x00, operator new(0xc) for the dummy node, which
// it links to itself (`MOV [EAX],EAX ; MOV [EAX+0x4],EAX`), header+0x04 = the dummy,
// header+0x08 = 0. Then the common half links a new operator new(0xc) node in front of
// the dummy and bumps the size.
// ===========================================================================
// The "build the list" half on its own. baseline inlines it twice, identically: push_back
// and the deep copy 06d91426..06d91469. The header allocation is a
// new-expression, so it is tested (06d915af / 06d91436) and a null header is stored
// into the handle all the same (06d915e8 / 06d91467) before the caller bails out on it.
// The dummy's operator new (06d915c3 / 06d91446) is a bare call: no test.
static FRpcEquipWireListHeader* RpcEquipHandleBuildList(FRpcEquipWireHandle* Handle) {
    FRpcEquipWireListHeader* Header =
        static_cast<FRpcEquipWireListHeader*>(::operator new(sizeof(FRpcEquipWireListHeader),
                                                             std::nothrow));
    if (Header != nullptr) {
        // +0x00 is the list's (empty) allocator object: baseline copies one byte of an
        // allocator temporary that holds no value. +0x01..+0x03 are never written.
        Header->Allocator = 0;
        Header->Pad_0x01[0] = 0;
        Header->Pad_0x01[1] = 0;
        Header->Pad_0x01[2] = 0;
        FRpcEquipWireNode* Dummy =
            static_cast<FRpcEquipWireNode*>(::operator new(sizeof(FRpcEquipWireNode),
                                                           std::nothrow));
        Dummy->Next = Dummy;
        Dummy->Prev = Dummy;
        Dummy->Value = nullptr;   // never written by baseline; the dummy's value is never read
        Header->Head = Dummy;
        Header->Size = 0;
    }
    Handle->List = Header;
    return Header;
}

// The common half of push_back (06d915ec..06d9161f), which the deep copy also inlines
// (06d9150d..06d9153f). The node's operator new is a bare call; the only test is the
// placement-new one on &Node->Value (06d91615 / 06d91539), which is never null.
static void RpcEquipHandleLinkBack(FRpcEquipWireListHeader* Header, FRpcEquipWireRecord* Record) {
    FRpcEquipWireNode* Dummy = Header->Head;
    FRpcEquipWireNode* Tail = Dummy->Prev;
    FRpcEquipWireNode* Node =
        static_cast<FRpcEquipWireNode*>(::operator new(sizeof(FRpcEquipWireNode), std::nothrow));
    Node->Next = Dummy;
    // `TEST EDI,EDI ; JNZ ... ; MOV EDI,EAX` -- when the dummy's Prev
    // is still null the new node becomes its own predecessor. Our dummy is always
    // self-linked, so the branch never fires; it is reproduced anyway.
    if (Tail == nullptr) {
        Tail = Node;
    }
    Node->Prev = Tail;
    Dummy->Prev = Node;
    Node->Prev->Next = Node;
    Node->Value = Record;
    ++Header->Size;
}

void RpcEquipHandlePushBack(FRpcEquipWireHandle* Handle, FRpcEquipWireRecord* Record) {
    if (Handle->List == nullptr && RpcEquipHandleBuildList(Handle) == nullptr) {
        return;   // JZ the branch
    }
    RpcEquipHandleLinkBack(Handle->List, Record);
}

// ===========================================================================
// the handle's destructor
// ===========================================================================
void RpcEquipHandleDestroy(FRpcEquipWireHandle* Handle) {
    // `MOV EAX,[EBX] ; TEST EAX,EAX ; JZ` -- the handle itself is not tested.
    if (Handle->List == nullptr) {
        return;
    }
    // Phase 1 (06d91358): free the records, leaving the chain intact. The loop bound
    // re-reads the handle's dummy every round (06d91378..06d9137c).
    for (FRpcEquipWireNode* Node = Handle->List->Head->Next; Node != Handle->List->Head;
         Node = Node->Next) {
        RpcContainerFreeRecord(Node->Value);
    }
    // Phase 2 (06d91381): the handle is re-read and re-tested, then every node is
    // unlinked and freed with the size decremented as baseline does, then the dummy.
    FRpcEquipWireListHeader* Header = Handle->List;
    if (Header == nullptr) {
        return;
    }
    FRpcEquipWireNode* Dummy = Header->Head;
    FRpcEquipWireNode* Node = Dummy->Next;
    while (Node != Dummy) {
        FRpcEquipWireNode* Next = Node->Next;
        Node->Prev->Next = Node->Next;
        Node->Next->Prev = Node->Prev;
        ::operator delete(Node);
        --Header->Size;
        Node = Next;
    }
    ::operator delete(Dummy);
    Header->Head = nullptr;
    Header->Size = 0;
    ::operator delete(Header);
    // baseline leaves the caller's handle pointing at the freed header (it never writes
    // [EBX] back). The port nulls it because RpcCreateObjectHeaderRelease runs on the
    // receiver statics, whose handle is filled again by the next packet; how baseline
    // keeps those statics alive is a separate ledger item (the "channel-2 close" entry,
    // CREATESHIP / CREATESOLAR dummy nodes). Not a guard: nothing is skipped.
    Handle->List = nullptr;
}

// The destructor of a handle object -- stack locals, record and header members. baseline
// runs the routine on every one of them on both the normal and the unwind path (see the
// struct in rpc_containers.h).
__declspec(dllexport) FRpcEquipWireHandle::~FRpcEquipWireHandle() {
    RpcEquipHandleDestroy(this);
}

// ===========================================================================
// deep copy, `this` = the destination handle, argument = the source.

// baseline bails out at once when the SOURCE has no list (06d9141c), lazily builds the
// destination list when it has none (06d91422..06d91469), then for every source node
// allocates a fresh operator new(0x18) record, zeroes it, copies the six scalar fields
// -- including the byte at +0x0f -- and, when HardpointSize is non-zero, allocates
// operator new(HardpointSize) and copies the name up to its NUL.
// ===========================================================================
void RpcEquipHandleCopy(FRpcEquipWireHandle* Dest, const FRpcEquipWireHandle* Source) {
    if (Source->List == nullptr) {
        return;
    }
    if (Dest->List == nullptr && RpcEquipHandleBuildList(Dest) == nullptr) {
        return;
    }
    // first node, and the loop bound re-reads the source's dummy
    // every round (06d91548..06d91557).
    for (const FRpcEquipWireNode* Node = Source->List->Head->Next; Node != Source->List->Head;
         Node = Node->Next) {
        const FRpcEquipWireRecord* From = Node->Value;
        FRpcEquipWireRecord* To =
            static_cast<FRpcEquipWireRecord*>(::operator new(sizeof(FRpcEquipWireRecord),
                                                             std::nothrow));
        if (To == nullptr) {
            // `CMP ESI,EBP ; JZ the branch` -- the new-expression's own test:
            // baseline skips this node and keeps walking.
            continue;
        }
        RpcContainerInitRecord(To);
        To->HardpointSize = From->HardpointSize;
        To->ArchId = From->ArchId;
        To->Status = From->Status;
        To->Count = From->Count;
        To->Id = From->Id;
        To->bEquipped = From->bEquipped;
        To->Field_0x0f = From->Field_0x0f;
        if (To->HardpointSize != 0) {                                     // JBE
            To->Hardpoint = RpcContainerCopyString(From->Hardpoint, To->HardpointSize);
        }
        RpcEquipHandleLinkBack(Dest->List, To);
    }
}

// ===========================================================================
// `if (packet+0x20 == 0) { packet+0x20 = operator new(4); *it = 0; }
//                  if (packet+0x20) the routine(packet+0x20, Source);`
// ===========================================================================
// The payload's four bytes hold a POINTER to the handle, not the handle itself:
// baseline allocates the handle with operator new(4) and stores its address at
// packet+0x20 (06d8fa9a..06d8fab2), then hands THAT address to the routine as the
// destination. The receiver side reads the same dword back and passes it as the SOURCE
// (06d95d20). Both indirections are reproduced.
void RpcPacketEquipAssign(FPacketBase* Packet, const FRpcEquipWireHandle* Source) {
    FRpcEquipWireHandle** Slot = reinterpret_cast<FRpcEquipWireHandle**>(Packet->Payload());
    if (*Slot == nullptr) {
        FRpcEquipWireHandle* Handle =
            static_cast<FRpcEquipWireHandle*>(::operator new(sizeof(FRpcEquipWireHandle),
                                                             std::nothrow));
        if (Handle != nullptr) {
            Handle->List = nullptr;
        }
        *Slot = Handle;
    }
    if (*Slot != nullptr) {
        RpcEquipHandleCopy(*Slot, Source);
    }
}

// ===========================================================================
// the inverse: `if (packet+0x20) the routine(Dest, packet+0x20)`.
// ===========================================================================
__declspec(dllexport) void RpcPacketEquipExtract(const FPacketBase* Packet, FRpcEquipWireHandle* Dest) {
    FRpcEquipWireHandle* const* Slot =
        reinterpret_cast<FRpcEquipWireHandle* const*>(Packet->Payload());
    if (*Slot != nullptr) {
        RpcEquipHandleCopy(Dest, *Slot);
    }
}

// ===========================================================================
// std::vector<EquipDesc> -> handle

// baseline walks `[param_1+0x4] .. [param_1+0x8]` with a stride of 0x20 and, for each
// element, either skips it (when param_3 is 0 and the element's bEquipped byte at
// +0x0c is non-zero -- 06d8c770) or builds one 0x18 record. The hardpoint is copied
// only when COMMON.DLL::EquipDesc::is_internal() returns false.
// ===========================================================================
void RpcEquipHandleFromVector(FRpcEquipWireHandle* Handle,
                              const EquipDescVector* Source,
                              bool bAll) {
    // EBP = first, [ESP+..] = last, `CMP EBP,EAX ; JZ` -- the only
    // test before the loop.
    const EquipDesc* Item = Source->Begin;
    const EquipDesc* End = Source->End;

    for (; Item != End; ++Item) {
        if (!bAll && Item->bEquipped) {
            // `CMP byte ptr [EBP+0xc],BL ; JNZ next` -- with param_3
            // clear only the UNequipped items travel, which is exactly the cargo half.
            continue;
        }
        // operator new(0x18) is a new-expression: `CMP EAX,EBX ; JZ` skips only
        // the zeroing (06d8c78b), and the stores below then go through the null pointer
        // exactly as 06d8c7af does.
        FRpcEquipWireRecord* Record =
            static_cast<FRpcEquipWireRecord*>(::operator new(sizeof(FRpcEquipWireRecord),
                                                             std::nothrow));
        if (Record != nullptr) {
            RpcContainerInitRecord(Record);
        }
        Record->ArchId = Item->ArchId;
        Record->Count = Item->Count;
        Record->Status = Item->Health;
        Record->Id = Item->Id;
        Record->bEquipped = Item->bEquipped;

        // baseline gates the string copy on COMMON.DLL::EquipDesc::is_internal() called on
        // the element itself (06d8c7cd, imported by name at module storage), which
        // compares the hardpoint against Common.dll's interned cargo-bay name.

        // is_internal() is the ONLY gate (06d8c7d5 JNZ): a non-internal hardpoint travels
        // even when it is the empty string, as size 1 and a lone NUL. The size is stored
        // (06d8c7ea) before the operator new it sizes (06d8c7ef, MOVZX of the word).
        if (!Item->is_internal()) {
            Record->HardpointSize =
                (unsigned short)(RpcContainerStringLength(Item->Hardpoint) + 1u);
            Record->Hardpoint = RpcContainerCopyString(Item->Hardpoint, Record->HardpointSize);
        }
        RpcEquipHandlePushBack(Handle, Record);
    }
}

// ===========================================================================
// handle -> std::vector<EquipDesc>
// ===========================================================================
void RpcEquipDescVectorRelease(EquipDescVector* Vector) {
    // baseline 06d88(0)0d / 06d84dca: `std::vector<EquipDesc>::_Destroy(first, last)`
    // then `operator delete(first)`. EquipDesc owns no heap (its hardpoint is an interned
    // StringAlloc string), so _Destroy runs trivial destructors; operator delete(nullptr)
    // is a no-op, as MSVCRT's is.
    for (EquipDesc* It = Vector->Begin; It != Vector->End; ++It) {
        It->~EquipDesc();
    }
    ::operator delete(Vector->Begin);
    Vector->Begin = nullptr;
    Vector->End = nullptr;
    Vector->Cap = nullptr;
}

__declspec(dllexport) void RpcEquipVectorFromHandle(EquipDescVector* Dest,
                              const FRpcEquipWireHandle* Source) {
    const FRpcEquipWireListHeader* Header = RpcEquipHandleGetList(Source);
    if (Header == nullptr) {
        // a null list leaves the vector untouched.
        return;
    }

    // reserve(list.Size) when the current capacity is smaller -- one
    // allocation of `Size << 5` bytes, i.e. Size * sizeof(EquipDesc), into which baseline
    // first MOVES the elements it already had. Every caller of this function hands in a
    // freshly initialised, empty vector, so the port drops the move and just replaces
    // the block; on an empty vector the two are the same thing.
    const unsigned int Wanted = Header->Size;
    if (Wanted != 0) {
        RpcEquipDescVectorRelease(Dest);
        // a bare operator new, no test.
        EquipDesc* Block = static_cast<EquipDesc*>(
            ::operator new(sizeof(EquipDesc) * Wanted, std::nothrow));
        Dest->Begin = Block;
        Dest->End = Block;
        Dest->Cap = Block + Wanted;
    }

    const FRpcEquipWireNode* Dummy = Header->Head;
    for (const FRpcEquipWireNode* Node = Dummy->Next; Node != Dummy; Node = Node->Next) {
        const FRpcEquipWireRecord* Record = Node->Value;
        if (Record == nullptr) {
            // `TEST EDI,EDI ; JZ next` -- a null record is skipped.
            continue;
        }
        // EquipDesc::EquipDesc() on a local, 06d84e31 vector insert of one copy at
        // end, then the setters run on the new last element (ESI = end - 0x20, 06d84e3c):
        // set_arch_id(rec+0x00) 06d84e42, set_count(rec+0x04) 06d84e4e,
        // set_status(rec+0x08) 06d84e5a, set_id(word rec+0x0c) 06d84e69,
        // set_equipped(byte rec+0x0e) 06d84e77. The block above holds exactly
        // Header->Size slots and at most that many records are appended, so the insert
        // never reallocates; constructing in place is the same object.
        EquipDesc* Slot = new (Dest->End) EquipDesc();
        ++Dest->End;
        Slot->set_arch_id(Record->ArchId);
        Slot->set_count(Record->Count);
        Slot->set_status(Record->Status);
        Slot->set_id(Record->Id);
        Slot->set_equipped(Record->bEquipped);
        // `CMP word [EDI+0x10],0 ; JBE`: set_hardpoint(StringAlloc(rec+0x14, 0))
        // (06d84e8a / 06d84e9e) when rec+0x10 != 0, else make_internal() (06d84ea8).
        // Interning is not cosmetic: EquipDesc::set_hardpoint just stores the pointer, so
        // handing it the RECORD's copy would leave the EquipDesc pointing into the
        // packet's owned block, which the packet destructor frees.
        if (Record->HardpointSize != 0) {
            CacheString Hp;
            Hp.Value = StringAlloc(Record->Hardpoint, false);
            Slot->set_hardpoint(Hp);
        } else {
            Slot->make_internal();
        }
    }
}

// ===========================================================================
// / the routine -- handle -> equipment list

// baseline builds its EquipDescList with COMMON.DLL::std::list<EquipDesc>::_Buynode:
// once with (0, 0) for the dummy node, then once per element with (dummy, dummy->Prev).
// The element is default-constructed (EquipDesc::EquipDesc) and blitted into the node
// with the routine, after which the five setters and the hardpoint decision run
// against the node's own copy, in this exact order:
//   set_arch_id, set_count, set_status, set_id, set_equipped,
//   then set_hardpoint(StringAlloc(rec+0x14, 0)) when rec+0x10 != 0, else make_internal.

// The port uses ITS compiler's container for the same job -- baseline's node chain was
// its compiler's std::list too -- and keeps the wire side byte-exact. What it no longer
// does is hand IServer a MIRROR of EquipDescList: the element really is a Common.dll
// EquipDesc, built through the very setters baseline imports by name, and it goes into a
// real EquipDescList. Appending is a raw tail insert (baseline's _Buynode(dummy,
// dummy->Prev) at 06d87f56), NOT add_equipment_item -- baseline does no combinable
// merging here and never touches NextId.
// ===========================================================================
__declspec(dllexport) void RpcEquipListFromHandle(EquipDescList& Dest, const FRpcEquipWireHandle* Source) {
    const FRpcEquipWireListHeader* Header = RpcEquipHandleGetList(Source);
    if (Header == nullptr) {
        // a null list means an empty EquipDescList, not a failure.
        return;
    }

    const FRpcEquipWireNode* Dummy = Header->Head;
    for (const FRpcEquipWireNode* Node = Dummy->Next; Node != Dummy; Node = Node->Next) {
        // `MOV ESI,[EBX+0x8]` -- the record is used with no test.
        const FRpcEquipWireRecord* Record = Node->Value;
        // `LEA ECX,[ESP+0x24] ; CALL EquipDesc::EquipDesc` -- one
        // default-constructed temp per element.
        EquipDesc Desc;
        Desc.set_arch_id(Record->ArchId);           // set_arch_id(rec+0x00)
        Desc.set_count(Record->Count);              // set_count  (rec+0x04)
        Desc.set_status(Record->Status);            // set_status (rec+0x08)
        Desc.set_id(Record->Id);                    // set_id     (rec+0x0c)
        Desc.set_equipped(Record->bEquipped);  // set_equipped(rec+0x0e)
        // `CMP word ptr [ESI+0x10],0x0 ; JBE make_internal`, i.e. a
        // zero length is the internal case; otherwise StringAlloc(rec+0x14, 0) at
        // and set_hardpoint at 06d87fe4. Interning is not cosmetic:
        // set_hardpoint only stores the pointer, and the RECORD's copy dies with the
        // packet.
        if (Record->HardpointSize != 0) {
            CacheString Hardpoint;
            Hardpoint.Value = StringAlloc(Record->Hardpoint, false);
            Desc.set_hardpoint(Hardpoint);
        } else {
            Desc.make_internal();
        }
        Dest.Items.push_back(Desc);
    }
}

// ===========================================================================
// IServerImpl::ReqEquipment 0x06d8cff0..0x06d8d08a -- equipment list -> handle

// `this = param_1` (the EquipDescList), `EAX = [this+0x4]` is the dummy node and
// `ESI = [EAX]` the first real one, so every field offset below is NODE-relative and
// the EquipDesc itself starts at node+0x08:

// MOV EAX,[ESI+0x0c]                -> EquipDesc+0x04 ArchId    -> wire+0x00
// MOV EAX,[ESI+0x1c]                -> EquipDesc+0x14 Count     -> wire+0x04
// MOV EDX,[ESI+0x18]                -> EquipDesc+0x10 Health    -> wire+0x08
// MOV AX,word ptr [ESI+0x0a]        -> EquipDesc+0x02 Id        -> wire+0x0c
// MOV AL,byte ptr [ESI+0x14]        -> EquipDesc+0x0c bEquipped -> wire+0x0e
// LEA ECX,[ESI+0x8] ; CALL EquipDesc::is_internal
// JNZ skip                          -- internal equipment sends no hardpoint
// strlen via REPNE SCASB, +1 into wire+0x10, operator new that
//                       many bytes into wire+0x14, byte copy including the terminator

// Two fields baseline deliberately leaves at the zero the routine..06d8d014 wrote:
// wire+0x0f (EquipDesc::bMission) and everything Owner-shaped. REQEQUIPMENT/REQCARGO do
// not carry them; only ch2 0x03 CHARACTERINFO does. Reproduced as found.
// ===========================================================================
void RpcEquipHandleFromList(FRpcEquipWireHandle* Handle, const EquipDescList& Source) {
    for (std::list<EquipDesc>::const_iterator It = Source.Items.begin();
         It != Source.Items.end(); ++It) {
        // operator new(0x18) is a new-expression: `CMP EAX,EBX ; JZ` (06d8cffa)
        // skips only the zeroing, and the stores below then go through the null pointer
        // exactly as 06d8d020 does.
        FRpcEquipWireRecord* Record =
            static_cast<FRpcEquipWireRecord*>(::operator new(sizeof(FRpcEquipWireRecord),
                                                             std::nothrow));
        if (Record != nullptr) {
            RpcContainerInitRecord(Record);
        }
        Record->ArchId = It->ArchId;
        Record->Count = It->Count;
        Record->Status = It->Health;
        Record->Id = It->Id;
        Record->bEquipped = It->bEquipped;

        // the ONLY gate is EquipDesc::is_internal(). There is no
        // "length != 0" test anywhere in the sender, so an empty but non-internal
        // hardpoint really does travel as size 1 and a lone terminator. No null test on
        // the hardpoint either (06d8d052 REPNE SCASB): a non-internal Hardpoint always
        // points at an interned string. The size is written at 06d8d058, BEFORE the
        // operator new it sizes at 06d8d060 (MOVZX of that word).
        if (!It->is_internal()) {
            Record->HardpointSize =
                (unsigned short)(RpcContainerStringLength(It->Hardpoint) + 1u);
            Record->Hardpoint = RpcContainerCopyString(It->Hardpoint, Record->HardpointSize);
        }
        RpcEquipHandlePushBack(Handle, Record);
    }
}

// ===========================================================================
// Collision groups
// ===========================================================================

// `LEA EAX,[param_1 + 0x20]; RET`.
FRpcCollisionGroupList* RpcPacketCollisionGroups(FPacketBase* Packet) {
    return reinterpret_cast<FRpcCollisionGroupList*>(Packet->Payload());
}

__declspec(dllexport) const FRpcCollisionGroupList* RpcPacketCollisionGroups(const FPacketBase* Packet) {
    return reinterpret_cast<const FRpcCollisionGroupList*>(Packet->Payload());
}

// Stand-in for the concrete packet constructors (0x06d9e8b0 / 0x06da10b0) and the
// create-object record constructors, which build the list's dummy node. The packet
// classes build it themselves now (rpc_packet_registry.cpp); the records still come here. The dummy's
// operator new is the bare call of std::list's constructor -- no test.
void RpcCollisionGroupListInit(FRpcCollisionGroupList* List) {
    if (List->Head != nullptr) {
        return;   // already constructed
    }
    FRpcCollisionGroupNode* Dummy =
        static_cast<FRpcCollisionGroupNode*>(::operator new(sizeof(FRpcCollisionGroupNode),
                                                            std::nothrow));
    Dummy->Next = Dummy;
    Dummy->Prev = Dummy;
    Dummy->Field_0x00 = 0;   // the dummy's value is never read
    Dummy->Field_0x04 = 0;
    List->Allocator = 0;
    List->Pad_0x01[0] = 0;
    List->Pad_0x01[1] = 0;
    List->Pad_0x01[2] = 0;
    List->Head = Dummy;
    List->Size = 0;
}

// list assignment, in baseline's three phases, from an array of 8-byte
// values. The phases live in RpcCollisionGroupListAssignRange (rpc_containers.h).
void RpcCollisionGroupListAssign(FRpcCollisionGroupList* List,
                                 const FRpcCollisionGroupDescMirror* Items,
                                 unsigned int Count) {
    RpcCollisionGroupListInit(List);
    RpcCollisionGroupListAssignRange(List, Items, Items + Count);
}

// ===========================================================================
// The CREATELOOT list (ch2 0x28)
// ===========================================================================

// `LEA EAX,[this + 0x28]; RET`, i.e. payload+0x08.
FRpcLootWireList* RpcPacketLootList(FPacketBase* Packet) {
    return reinterpret_cast<FRpcLootWireList*>(Packet->Payload() + 8);
}

__declspec(dllexport) const FRpcLootWireList* RpcPacketLootList(const FPacketBase* Packet) {
    return reinterpret_cast<const FRpcLootWireList*>(Packet->Payload() + 8);
}

// Stand-in for the CREATELOOT packet constructor's list constructor; bare operator new.
void RpcPacketLootListInit(FPacketBase* Packet) {
    FRpcLootWireList* List = RpcPacketLootList(Packet);
    if (List->Head != nullptr) {
        return;   // already constructed
    }
    FRpcLootWireNode* Dummy =
        static_cast<FRpcLootWireNode*>(::operator new(sizeof(FRpcLootWireNode),
                                                      std::nothrow));
    Dummy->Next = Dummy;
    Dummy->Prev = Dummy;
    unsigned char* Value = reinterpret_cast<unsigned char*>(&Dummy->Value);
    for (unsigned int i = 0; i < (unsigned int)sizeof(FRpcLootWireRecord); ++i) {
        Value[i] = 0;   // the dummy's value is never read
    }
    List->Allocator = 0;
    List->Pad_0x01[0] = 0;
    List->Pad_0x01[1] = 0;
    List->Pad_0x01[2] = 0;
    List->Head = Dummy;
    List->Size = 0;
}

// push_back. baseline reads the dummy node out of packet+0x2c, allocates
// operator new(0x50) with no test (06d9316d), links it at the tail, copies the value with
// and increments packet+0x30.
void RpcPacketLootPushBack(FPacketBase* Packet, const FRpcLootWireRecord& Record) {
    RpcPacketLootListInit(Packet);
    FRpcLootWireList* List = RpcPacketLootList(Packet);
    FRpcLootWireNode* Dummy = List->Head;
    FRpcLootWireNode* Tail = Dummy->Prev;
    FRpcLootWireNode* New =
        static_cast<FRpcLootWireNode*>(::operator new(sizeof(FRpcLootWireNode),
                                                      std::nothrow));
    New->Next = Dummy;
    if (Tail == nullptr) {
        // `TEST EBX,EBX ; JNZ ... ; MOV EBX,EAX`.
        Tail = New;
    }
    New->Prev = Tail;
    Dummy->Prev = New;
    New->Prev->Next = New;
    New->Value = Record;
    ++List->Size;
}

void RpcPacketLootListRelease(FPacketBase* Packet) {
    FRpcLootWireList* List = RpcPacketLootList(Packet);
    if (List->Head == nullptr) {
        return;   // never constructed
    }
    FRpcLootWireNode* Dummy = List->Head;
    FRpcLootWireNode* Node = Dummy->Next;
    while (Node != Dummy) {
        FRpcLootWireNode* Next = Node->Next;
        ::operator delete(Node);
        Node = Next;
    }
    ::operator delete(Dummy);
    List->Head = nullptr;
    List->Size = 0;
}

// ===========================================================================
// The reputation relation list (ch2 0x29, 0x2b)
// ===========================================================================

void RpcRelationListPushBack(FRpcRelationList* List, const FRpcRelation& Value) {
    // `PUSH 0x8 ; CALL operator_new` -- the value lives on its own, and the
    // result is stored through with no test (06d88e9d).
    FRpcRelation* Block =
        static_cast<FRpcRelation*>(::operator new(sizeof(FRpcRelation), std::nothrow));
    Block->Field_0x00 = Value.Field_0x00;
    Block->Field_0x04 = Value.Field_0x04;
    RpcRelationListLink(List, Block);
}

void RpcRelationListLink(FRpcRelationList* List, FRpcRelation* Block) {
    // The dummy is read untested (the routine 06d8cad4): every list this reaches was built
    // by its packet's constructor (06d9d30c..06d9d31a for 0x29, 06d9c59f for 0x2b).
    FRpcRelationNode* Dummy = List->Head;
    // the node's operator new(0xc) at 06d8cade is bare as well.
    FRpcRelationNode* Tail = Dummy->Prev;
    FRpcRelationNode* New =
        static_cast<FRpcRelationNode*>(::operator new(sizeof(FRpcRelationNode),
                                                      std::nothrow));
    New->Next = Dummy;
    if (Tail == nullptr) {
        // 06d8cae6 `TEST EDI,EDI` / 06d8caea `JNZ` / 06d8caec `MOV EDI,EAX`:
        // `if (piVar2 == 0) piVar2 = piVar1;`
        Tail = New;
    }
    New->Prev = Tail;
    Dummy->Prev = New;
    New->Prev->Next = New;
    New->Value = Block;   // the placement test on &New->Value, never null
    ++List->Size;
}

// ===========================================================================
// The mission-objective record list (ch2 0x35)
// ===========================================================================

void RpcMissionObjectiveListPushBack(FRpcMissionObjectiveList* List,
                                     FRpcMissionObjectiveRecord* Record) {
    // 06d8fc16: `if (param_1 != 0)` -- a null record is silently dropped and
    // neither counter moves. baseline's own test.
    if (Record == nullptr) {
        return;
    }
    // The dummy is read untested: the packet constructor built it (06d9daad).
    FRpcMissionObjectiveNode* Dummy = List->Head;
    FRpcMissionObjectiveNode* Tail = Dummy->Prev;
    // operator new(0xc): a bare call, no test.
    FRpcMissionObjectiveNode* New =
        static_cast<FRpcMissionObjectiveNode*>(
            ::operator new(sizeof(FRpcMissionObjectiveNode), std::nothrow));
    New->Next = Dummy;
    if (Tail == nullptr) {
        Tail = New;
    }
    New->Prev = Tail;
    Dummy->Prev = New;
    New->Prev->Next = New;
    New->Value = Record;   // the placement test on &New->Value, never null
    ++List->Size;
}

__declspec(dllexport) const FRpcMissionObjectiveRecord* RpcMissionObjectiveListAt(
    const FRpcMissionObjectiveList* List, unsigned int Index) {
    // `MOV ECX,[ECX+0x40] ; MOV EAX,[ECX]` -- the dummy is read untested.
    const FRpcMissionObjectiveNode* Dummy = List->Head;
    const FRpcMissionObjectiveNode* Node = Dummy->Next;
    unsigned int At = 0;
    while (Node != Dummy) {
        if (At == Index) {
            return Node->Value;
        }
        Node = Node->Next;
        ++At;
    }
    return nullptr;
}

// ===========================================================================
// The seam between the baseline-shaped containers some payloads still keep (the equipment
// handle, the SETCOLLISIONGROUPS / REQCOLLISIONGROUPS collision image) and the Common.dll
// types the shared CEqObjCreateInfo (src/include/server_createinfo.h) really holds. See
// the declarations in rpc_containers.h.
// ===========================================================================

// One CollisionGroupDesc as baseline's node copy moves it: two dwords (06d8c9c8 /
// ), the first holding the ushort id and the struct's two padding bytes.
static void RpcCollisionGroupDescDecode(CollisionGroupDesc& Desc, unsigned int Word0,
                                        unsigned int Word1) {
    Desc.Id = static_cast<unsigned short>(Word0 & 0xffffu);
    Desc.Pad02 = static_cast<unsigned short>(Word0 >> 16);
    std::memcpy(&Desc.Health, &Word1, sizeof(Word1));
}

// overwrite the nodes already in Dest, erase the surplus, append
// the rest -- which is what std::list assignment is. The receiver's static has been
// cleared by clear() since the previous packet, so in practice only the append runs.
__declspec(dllexport) void RpcCollisionGroupListFromWire(std::list<CollisionGroupDesc>& Dest,
                                   const FRpcCollisionGroupList* Source) {
    const FRpcCollisionGroupNode* Dummy = Source->Head;
    const FRpcCollisionGroupNode* Node = Dummy->Next;
    std::list<CollisionGroupDesc>::iterator It = Dest.begin();
    while (It != Dest.end() && Node != Dummy) {
        RpcCollisionGroupDescDecode(*It, Node->Field_0x00, Node->Field_0x04);
        ++It;
        Node = Node->Next;
    }
    Dest.erase(It, Dest.end());
    while (Node != Dummy) {
        CollisionGroupDesc Desc;
        RpcCollisionGroupDescDecode(Desc, Node->Field_0x00, Node->Field_0x04);
        Dest.push_back(Desc);
        Node = Node->Next;
    }
}

// , inlined in the routine: one operator new(0x18) wire record per
// EquipDesc of the record's EquipDescVector, with NO bEquipped filter (this is not
// 's cargo path). Per element: ArchId <- +0x04 (06d8c923), Count <- +0x14
// (06d8c929), Status <- +0x10 (06d8c92f FLD/FSTP), Id <- +0x02 (06d8c935), bEquipped <-
// +0x0c (06d8c93d); then, unless COMMON.DLL::EquipDesc::is_internal() (06d8c945), the
// hardpoint: strlen+1 into +0x10 (06d8c962) and an operator new'd byte copy into +0x14.
// Pushed with the routine (06d8c989).
static void RpcEquipHandleFromEquipDescVector(FRpcEquipWireHandle* Handle,
                                              const EquipDescVector& Source) {
    for (const EquipDesc* Item = Source.Begin; Item != Source.End; ++Item) {
        // operator new(0x18) is a new-expression: `CMP EAX,EBP ; JZ` (06d8c902)
        // skips only the zeroing, and the stores below go through the result either way.
        FRpcEquipWireRecord* Record =
            static_cast<FRpcEquipWireRecord*>(::operator new(sizeof(FRpcEquipWireRecord),
                                                             std::nothrow));
        if (Record != nullptr) {
            RpcContainerInitRecord(Record);
        }
        Record->ArchId = Item->ArchId;
        Record->Count = Item->Count;
        Record->Status = Item->Health;
        Record->Id = Item->Id;
        Record->bEquipped = Item->bEquipped;
        // is_internal() is the only gate (06d8c94d); no null test on the hardpoint
        // (06d8c957 REPNE SCASB), and the size is stored (06d8c962) before the operator
        // new it sizes (06d8c967).
        if (!Item->is_internal()) {
            Record->HardpointSize =
                static_cast<unsigned short>(RpcContainerStringLength(Item->Hardpoint) + 1u);
            Record->Hardpoint = RpcContainerCopyString(Item->Hardpoint, Record->HardpointSize);
        }
        RpcEquipHandlePushBack(Handle, Record);
    }
}

// baseline appends through COMMON.DLL::std::vector<EquipDesc>::insert(end, 1, value)
// (06d84fc8); VC6 grows that by doubling. The block comes from ::operator new because
// ~EquipDescVector (Common 0x06269110) releases it with ::operator delete.
static void RpcEquipDescVectorPushBack(EquipDescVector& Vector, const EquipDesc& Item) {
    if (Vector.End == Vector.Cap) {
        const unsigned int Count = static_cast<unsigned int>(Vector.End - Vector.Begin);
        const unsigned int NewCap = (Count == 0u) ? 1u : Count * 2u;
        // VC6 allocator<EquipDesc>::allocate: a bare operator new, no test.
        EquipDesc* Block = static_cast<EquipDesc*>(
            ::operator new(sizeof(EquipDesc) * NewCap, std::nothrow));
        for (unsigned int Index = 0; Index < Count; ++Index) {
            new (&Block[Index]) EquipDesc(Vector.Begin[Index]);
        }
        ::operator delete(Vector.Begin);
        Vector.Begin = Block;
        Vector.End = Block + Count;
        Vector.Cap = Block + NewCap;
    }
    new (Vector.End) EquipDesc(Item);
    ++Vector.End;
}

// , inlined in the routine. For every node of the handle's list
// (the routine at 06d84f89; an absent list or an empty one skips the loop, 06d84f94 /
// ): a default EquipDesc (06d84fb8, EquipDesc::EquipDesc) is APPENDED to the
// record's vector (06d84fc8), and then the element just appended (`[EBX+0x78] - 0x20`,
// /06d84fd3) gets, in this order,
//   set_arch_id(rec+0x00)  06d84fd9      set_count(rec+0x04)    06d84fe5
//   set_id(rec+0x0c)       06d84ff4      set_equipped(rec+0x0e) 06d85002
//   set_status(rec+0x08)   06d8500e
// and then set_hardpoint(StringAlloc(rec+0x14, 0)) when the word at rec+0x10 is
// non-zero (06d85014 `CMP word ptr [EDI+0x10],0 ; JBE`), else make_internal() (06d8503f).
// Unlike the routine there is no null-record test here: 06d84fb1 loads the record and
// dereferences it.
static void RpcEquipDescVectorAppendFromHandle(EquipDescVector& Dest,
                                               const FRpcEquipWireHandle* Source) {
    const FRpcEquipWireListHeader* Header = RpcEquipHandleGetList(Source);
    if (Header == nullptr) {
        return;
    }
    const FRpcEquipWireNode* Dummy = Header->Head;
    for (const FRpcEquipWireNode* Node = Dummy->Next; Node != Dummy; Node = Node->Next) {
        const FRpcEquipWireRecord* Record = Node->Value;
        EquipDesc Blank;
        RpcEquipDescVectorPushBack(Dest, Blank);
        EquipDesc& Slot = *(Dest.End - 1);
        Slot.set_arch_id(Record->ArchId);
        Slot.set_count(Record->Count);
        Slot.set_id(Record->Id);
        Slot.set_equipped(Record->bEquipped);
        Slot.set_status(Record->Status);
        if (Record->HardpointSize != 0) {
            CacheString Hardpoint;
            Hardpoint.Value = StringAlloc(Record->Hardpoint, false);
            Slot.set_hardpoint(Hardpoint);
        } else {
            Slot.make_internal();
        }
    }
}

// ===========================================================================
// The create-object header (ch2 0x04 CREATESHIP, 0x42 CREATESOLAR)

// and the routine on the shared CEqObjCreateInfo; the wire side is laid
// out in rpc_containers.h.
// ===========================================================================

// The abstract base's constructor part, inlined into both packet constructors in the
// same order: CREATESHIP 06d9cca2..06d9ccd2, CREATESOLAR 06d9e66b..06d9e6a7.
//   packet+0x20/+0x24/+0x28/+0x2c = 0             06d9cca2..06d9ccab
// (packet+0x30): six dwords zeroed, LAST first (06d9cda4..06d9cdb3), i.e.
//     +0x44 (Ids), +0x40 (IdCount), +0x3c, +0x38, +0x34, +0x30 -- CREATESOLAR inlines the
//     same six stores at 06d9e67b..06d9e68a
//   packet+0x48/+0x4c/+0x50/+0x54 = 0             06d9ccb3..06d9ccc4
// (packet+0x58): the identity quaternion, 1.0f then three zeroes (06d931c4)
//   packet+0x68/+0x6c/+0x70 = 0                   06d9cccc..06d9ccd2
// Nothing is tested. The two Common.dll containers that follow are the derived
// FRpcCreateObjectWire's members and are built by their own constructors.
FRpcCreateObjectHead::FRpcCreateObjectHead() {
    Field_0x00 = 0;
    Field_0x04 = 0;
    Field_0x08 = 0;
    VibeGroupId = 0;
    Ids = nullptr;                                      // (+0x44)
    IdCount = 0;                                        // (+0x40)
    Field_0x1c = 0;                                     // (+0x3c)
    Field_0x18 = 0;                                     // (+0x38)
    Field_0x14 = 0;                                     // (+0x34)
    Field_0x10 = 0;                                     // (+0x30)
    Field_0x28 = 0;
    Field_0x2c = 0.0f;                                  // (integer 0 == +0.0f)
    Field_0x30 = 0.0f;
    Field_0x34 = 0.0f;
    Field_0x38 = 1.0f;                                  // MOV [EAX],0x3f800000
    Field_0x3c = 0.0f;
    Field_0x40 = 0.0f;
    Field_0x44 = 0.0f;
    Field_0x48 = 0;
    Field_0x4c = 0;
    Equipment.List = nullptr;
}

// The tail of the base part of the routine (== the routine from 06d9e81c): the handle at
// packet+0x70 through the routine (06d97c94 LEA ECX,[EBX+0x70] / 06d97ca2 CALL), then the
// id block at +0x44 through the routine's body inlined (06d97ca7..06d97cb1: TEST, JZ,
// operator delete, no nulling). The two containers above it in memory were destroyed
// first, by the derived FRpcCreateObjectWire's member destructors -- baseline's
// (DamageList) and 06d97c54 (list) -- which is also the EH order: this
// destructor is states 2 and 1 of FuncInfo_06db70a4 (Unwind@06db2873 the routine on +0x70,
// Unwind@06db2868 the routine on +0x30), run by the compiler if a container constructor
// throws after the head is built.
FRpcCreateObjectHead::~FRpcCreateObjectHead() {
    // The handle goes first, as at 06d97ca2; its own member destructor then finds it
    // null and does nothing.
    RpcEquipHandleDestroy(&Equipment);                  // / Unwind@06db2873
    if (Ids != nullptr) {                               // / 06d95b63
        ::operator delete(Ids);                         // / 06d95b68
    }
}

void RpcCreateObjectHeaderWrite(FRpcCreateObjectWire* Wire,
                                const CEqObjCreateInfo& Source) {
    Wire->Field_0x00 = Source.ObjId;                    // / 06d8c840
    Wire->Field_0x04 = Source.ArchetypeNickname;        // / 06d8c846
    Wire->Field_0x08 = Source.DockingBaseId;            // / 06d8c84c
    Wire->VibeGroupId = Source.VibeGroupId;             // / 06d8c852
    Wire->Field_0x10 = Source.CostumeHead;              // / 06d8c86a
    Wire->Field_0x14 = Source.CostumeBody;              // / 06d8c870
    Wire->Field_0x18 = Source.CostumeLeftHand;          // / 06d8c876
    Wire->Field_0x1c = Source.CostumeRightHand;         // / 06d8c87c

    // the accessory count, then operator new(count << 2) -- a bare
    // call, made unconditionally, a count of 0 included (06d8c88c) -- and the copy: a
    // signed `JLE` guard (06d8c8a0), then src+0x24 walked with no bound (06d8c8a5..
    // ). A Costume holds at most eight accessories (Costume::accessory[8]), so a
    // CEqObjCreateInfo built from one never carries a larger count.
    const int Count = Source.CostumeAccessoryCount;
    Wire->IdCount = Count;
    unsigned int* Block = static_cast<unsigned int*>(
        ::operator new(static_cast<unsigned int>(Count) << 2, std::nothrow));
    Wire->Ids = Block;
    const unsigned int* Accessory = &Source.CostumeAccessory[0];
    for (int i = 0; i < Count; ++i) {
        Block[i] = Accessory[i];
    }

    Wire->Field_0x28 = Source.VoiceId;                  // / 06d8c858
    Wire->Field_0x2c = Source.Position.x;               // / 06d8c8bb
    Wire->Field_0x30 = Source.Position.y;               // / 06d8c8c1
    Wire->Field_0x34 = Source.Position.z;               // / 06d8c8c7
    Wire->Field_0x38 = Source.Orientation.w;            // / 06d8c8cd
    Wire->Field_0x3c = Source.Orientation.x;            // / 06d8c8d3
    Wire->Field_0x40 = Source.Orientation.y;            // / 06d8c8d9
    Wire->Field_0x44 = Source.Orientation.z;            // / 06d8c8df
    Wire->Field_0x48 = static_cast<unsigned int>(Source.HitPts);  // / 06d8c85e
    Wire->Field_0x4c = Source.Field_0x6c;               // / 06d8c864

    RpcEquipHandleFromEquipDescVector(&Wire->Equipment, Source.Equipment);
    // std::list<CollisionGroupDesc>::operator= inlined -- overwrite the
    // nodes already there, erase the surplus, append the rest -- into the list the packet
    // constructor built (06d9cce2 / 06d9e6b7). The packet list is a real
    // std::list<CollisionGroupDesc> now, so this is that assignment.
    Wire->CollisionGroups = Source.CollisionGroups;
    // , one DamageList assignment: the node chain, then +0x0c as a BYTE
    // and +0x10/+0x14/+0x18 (06d8caae..06d8cac5) -- DamageList::operator=, into the
    // DamageList the packet constructor built (06d9ccf3 / 06d9e6c8).
    Wire->Damage = Source.Damage;
}

__declspec(dllexport) void RpcCreateObjectHeaderRead(CEqObjCreateInfo& Dest,
                               const FRpcCreateObjectWire* Wire) {
    Dest.ObjId = Wire->Field_0x00;                      // / 06d84ee0
    Dest.ArchetypeNickname = Wire->Field_0x04;          // / 06d84ee6
    Dest.DockingBaseId = Wire->Field_0x08;              // / 06d84eec
    Dest.VibeGroupId = Wire->VibeGroupId;               // / 06d84f37
    Dest.CostumeHead = Wire->Field_0x10;                // / 06d84ef8
    Dest.CostumeBody = Wire->Field_0x14;                // / 06d84ef2
    Dest.CostumeLeftHand = Wire->Field_0x18;            // / 06d84f05
    Dest.CostumeRightHand = Wire->Field_0x1c;           // / 06d84efe

    // . baseline's loop condition is unsigned (`CMP EAX,EDX ; JC`) after
    // a signed `JLE` guard, so a negative count copies nothing. No bound on dest+0x24 and
    // no test on the id block: the count and the block were produced together by
    // RpcCreateObjectHeaderWrite (at most eight, see there).
    const int Count = Wire->IdCount;
    Dest.CostumeAccessoryCount = Count;
    if (Count > 0) {
        unsigned int* Accessory = &Dest.CostumeAccessory[0];
        for (unsigned int i = 0; i < static_cast<unsigned int>(Wire->IdCount); ++i) {
            Accessory[i] = Wire->Ids[i];
        }
    }

    Dest.VoiceId = Wire->Field_0x28;                    // / 06d84f3d
    Dest.Position.x = Wire->Field_0x2c;                 // / 06d84f55
    Dest.Position.y = Wire->Field_0x30;                 // / 06d84f58
    Dest.Position.z = Wire->Field_0x34;                 // / 06d84f5b
    Dest.Orientation.w = Wire->Field_0x38;              // / 06d84f71
    Dest.Orientation.x = Wire->Field_0x3c;              // / 06d84f77
    Dest.Orientation.y = Wire->Field_0x40;              // / 06d84f7d
    Dest.Orientation.z = Wire->Field_0x44;              // / 06d84f83
    Dest.HitPts = static_cast<int>(Wire->Field_0x48);   // / 06d84f43
    Dest.Field_0x6c = Wire->Field_0x4c;                 // / 06d84f49

    // .
    RpcEquipDescVectorAppendFromHandle(Dest.Equipment, &Wire->Equipment);
    // the same list assignment in the other direction, into the
    // record's own list (built by its constructor, emptied by clear() since the last packet).
    Dest.CollisionGroups = Wire->CollisionGroups;
    // , the mirror of the write side's 06d8ca2c..06d8cac5:
    // DamageList::operator=, the four tail members at 06d851a9..06d851c1.
    Dest.Damage = Wire->Damage;
}

// The receiver statics' constructor (06d83df0), per-packet clear (06d83ed0 / 06d83fa0)
// and destructor (06d843b0) used to be hand-rolled here as RpcCreateObjectCommonInit /
// ::Release over the private mirror. They are now the shared type's own inline members
// in src/include/server_createinfo.h, which is what baseline compiled into this module.

// ===========================================================================
// ch2 0x03 CHARACTERINFO -- the record conversions and the wire list

// Sender 0x06d89890 / the routine, receiver IServerImpl::HandleCharacterInfo
// . The layout derivation is in rpc_containers.h.
// ===========================================================================

// "PUSH 0x18" and its array; 06d84782..06d847b7 back the other way.
// (BlockA; BlockB is the same run at 06d89c25..06d89d12).
// The inline block is a Costume (server_characterset.h): four part ids, eight accessory
// ids, the accessory count.
static void RpcIdBlockToWire(FRpcIdArrayBlockWire** Out, const Costume& Source) {
    // operator new(0x18); the new-expression's test (06d89b42) only skips the
    // six zero stores -- the pointer is stored and written through either way.
    FRpcIdArrayBlockWire* Block = static_cast<FRpcIdArrayBlockWire*>(
        ::operator new(sizeof(FRpcIdArrayBlockWire), std::nothrow));
    if (Block != nullptr) {
        Block->Ids = nullptr;
        Block->Count = 0;
        Block->Field_0x0c = 0;
        Block->Field_0x08 = 0;
        Block->Field_0x04 = 0;
        Block->Field_0x00 = 0;
    }
    *Out = Block;
    Block->Field_0x04 = Source.PartIds[1];            // <- src+0x278
    Block->Field_0x00 = Source.PartIds[0];            // <- src+0x274
    Block->Field_0x08 = Source.PartIds[2];
    Block->Field_0x0c = Source.PartIds[3];
    Block->Count = static_cast<int>(Source.AccessoryCount);   // <- src+0x2a4
    // `CMP EAX,EBX ; JBE` and 06d89c21 `JC`: the count is UNSIGNED and the copy
    // is not bounded by the eight-slot array -- it walks on from src+0x284.
    if ((unsigned int)Block->Count != 0) {
        Block->Ids = static_cast<unsigned int*>(
            ::operator new((unsigned int)Block->Count << 2, std::nothrow));
        const unsigned int* SourceIds = &Source.AccessoryIds[0];
        for (unsigned int i = 0; i < (unsigned int)Block->Count; ++i) {
            Block->Ids[i] = SourceIds[i];
        }
    }
}

static void RpcIdBlockFromWire(Costume& Dest, const FRpcIdArrayBlockWire* Block) {
    // , the record constructor's block init (06d845f8 / 06d84603): +0x00,
    // +0x04, +0x08, +0x0c and the count at +0x30. The eight ids are left alone there;
    // the port zeroes them too (heap garbage in baseline, see the uninitialised-memory rule).
    Dest.PartIds[0] = 0;
    Dest.PartIds[1] = 0;
    Dest.PartIds[2] = 0;
    Dest.PartIds[3] = 0;
    Dest.AccessoryCount = 0;
    for (unsigned int i = 0; i < 8u; ++i) {
        Dest.AccessoryIds[i] = 0;
    }
    if (Block == nullptr) {
        // / 06d847c1: a null block pointer skips the whole group.
        return;
    }
    Dest.PartIds[1] = Block->Field_0x04;
    Dest.PartIds[0] = Block->Field_0x00;
    Dest.PartIds[3] = Block->Field_0x0c;
    Dest.PartIds[2] = Block->Field_0x08;
    Dest.AccessoryCount = static_cast<unsigned int>(Block->Count);
    // `JBE` / 06d847b5 `JC`: unsigned count, no bound against the eight slots,
    // and the wire array is not tested -- exactly the sender's loop mirrored.
    unsigned int* DestIds = &Dest.AccessoryIds[0];
    for (unsigned int i = 0; i < (unsigned int)Block->Count; ++i) {
        DestIds[i] = Block->Ids[i];
    }
}

// the wire id block's destructor, whole:
// MOV EAX,[ECX+0x14] / 06d95b63 TEST EAX,EAX / 06d95b65 JZ -> RET
// PUSH EAX / 06d95b68 CALL the routine (free) / RET
// Called directly by the CHARACTERINFO packet destructor the routine (06d933b3, 06d933cd)
// and reached by three EH unwind funclets (06db280e, 06db286e, 06db290e) on an id block
// embedded at +0x30 of a half-built object; the port's constructors do not throw, so only
// the two direct calls exist here.
static void RpcIdBlockWireDestroy(FRpcIdArrayBlockWire* Block) {
    if (Block->Ids != nullptr) {
        ::operator delete(Block->Ids);
    }
}

// the scalar deleting destructor: the routine's body, then the block.
static void RpcIdBlockWireFree(FRpcIdArrayBlockWire* Block) {
    if (Block == nullptr) {
        return;
    }
    RpcIdBlockWireDestroy(Block);
    ::operator delete(Block);
}

void RpcCharacterInfoRecordToWire(FRpcCharacterInfoWireRecord* Wire,
                                  const CCharacterSet::CharacterInfo* Source) {
    // baseline's record constructor (06d89944..06d899d9) puts every owning field in a
    // releasable state before a single value is copied; the port does the same, because
    // the packet destructor can run after a half-built record.
    Wire->BlockA = nullptr;
    Wire->BlockB = nullptr;
    Wire->Equipment.List = nullptr;
    Wire->CollisionGroups.Allocator = 0;
    Wire->CollisionGroups.Pad_0x01[0] = 0;
    Wire->CollisionGroups.Pad_0x01[1] = 0;
    Wire->CollisionGroups.Pad_0x01[2] = 0;
    Wire->CollisionGroups.Head = nullptr;
    Wire->CollisionGroups.Size = 0;
    // the record constructor builds the std::list at +0x268 (COMMON.DLL's list
    // ctor, i.e. the dummy node); RpcCollisionGroupListInit stands in for it.
    RpcCollisionGroupListInit(&Wire->CollisionGroups);

    Wire->Name = Source->Id;                         // CHARACTER_ID::operator=
    Wire->Field_0x200 = Source->bIsRestart;
    Wire->Field_0x201 = Source->bIsAutoSave;
    Wire->TextA = Source->Name;                      // basic_string::assign
    Wire->TextB = Source->Description;
    Wire->Field_0x224 = Source->DescripStrId;
    Wire->Field_0x228 = Source->TimestampLow;
    Wire->Field_0x22c = Source->TimestampHigh;
    Wire->Field_0x230 = Source->ShipArchetype;
    Wire->Field_0x234 = static_cast<unsigned int>(Source->Money);
    // The permuted nine. Each line is one MOV pair in the sender and its inverse in the
    // receiver; the addresses are on the struct in rpc_containers.h.
    Wire->Field_0x238 = Source->SystemId;            // <- src+0x260
    Wire->Field_0x244 = Source->VoiceId;             // <- src+0x26c
    Wire->Field_0x248 = Source->Rank;                // <- src+0x270
    Wire->Field_0x23c = Source->BaseId;              // <- src+0x264
    Wire->Field_0x240 = Source->LocationId;          // <- src+0x268
    // the record ctor's zero is the ONLY write baseline ever makes to +0x24c.
    // It still travels -- see the note on the struct.
    Wire->Field_0x24c = 0;
    // src+0x244 is the float HullStatus; the sender moves its four bytes as a dword.
    std::memcpy(&Wire->Field_0x250, &Source->HullStatus, sizeof(Wire->Field_0x250));
    Wire->Field_0x254 = static_cast<unsigned int>(Source->NumKills);             // <- src+0x238
    Wire->Field_0x258 = static_cast<unsigned int>(Source->NumMissionFailures);   // <- src+0x240
    Wire->Field_0x25c = static_cast<unsigned int>(Source->NumMissionSuccesses);  // <- src+0x23c
    // hop one only zeroes it. The real value is written by the PACKET-BOUND
    // hop the routine at 06d959c6..06d959d2, which this port folds in below, once the
    // equipment list exists.
    Wire->Field_0x260 = 0;

    RpcIdBlockToWire(&Wire->BlockA, Source->ComCostume);   // <- src+0x274
    RpcIdBlockToWire(&Wire->BlockB, Source->BaseCostume);  // <- src+0x2a8

    // one operator new(0x18) wire record per EquipDesc. Unlike
    // this loop has NO is_internal gate -- it always copies the hardpoint
    // (06d89d78 strlen, 06d89d87 operator new, 06d89d97 the byte loop).
    for (std::list<EquipDesc>::const_iterator It = Source->Equipment.Items.begin();
         It != Source->Equipment.Items.end(); ++It) {
        // operator new(0x18); the test at 06d89d34 skips only the zero stores.
        FRpcEquipWireRecord* Record = static_cast<FRpcEquipWireRecord*>(
            ::operator new(sizeof(FRpcEquipWireRecord), std::nothrow));
        if (Record != nullptr) {
            RpcContainerInitRecord(Record);
        }
        Record->ArchId = It->ArchId;
        Record->Count = It->Count;
        Record->Status = It->Health;
        Record->Id = It->Id;
        Record->bEquipped = It->bEquipped;
        // "MOV DL,[EBP+0x20] ; MOV [EBX+0xf],DL". Node+0x20 is EquipDesc+0x18,
        // i.e. bMission -- and the receiver puts it straight back with 06d848fa/06d848fd
        // "MOV AL,[ESI+0xf] ; MOV [EDI+0x18],AL". That pair is what finally names
        // FRpcEquipWireRecord::Field_0x0f, which every other user of the record leaves
        // at zero.
        Record->Field_0x0f = (unsigned char)(It->bMission ? 1 : 0);
        const unsigned int Length = RpcContainerStringLength(It->Hardpoint);
        Record->Hardpoint = RpcContainerCopyString(It->Hardpoint, Length + 1u);
        Record->HardpointSize = (unsigned short)(Length + 1u);
        RpcEquipHandlePushBack(&Wire->Equipment, Record);             // the routine
    }

    // at 06d959c6..06d959da -- the packet-bound hop's own postscript, and
    // the only thing it does that the first hop does not:
    //     EAX = the routine(&dst+0x264)          ; *(header**)(rec+0x264)
    //     if (EAX) dst+0x260 = (byte)[EAX+0x8]    ; the list's Size, truncated
    //     else     dst+0x260 = 0
    // Folding the two hops into one, as this port does, dropped it -- and +0x260 is
    // exactly the count the wire codec emits and the decoder loops on (06d9528e /
    // ), so leaving it zero would have put every character's equipment list on
    // the wire as empty.
    {
        const FRpcEquipWireListHeader* Header = RpcEquipHandleGetList(&Wire->Equipment);
        Wire->Field_0x260 = (Header != nullptr) ? (unsigned char)(Header->Size & 0xffu) : 0;
    }

    // std::list<CollisionGroupDesc>::operator= of the caller's list
    // at src+0x254 onto rec+0x268 -- the self-assignment test (06d89de4), the overwrite
    // loop (06d89df8..06d89e0e), the range insert (06d89e18..06d89e3b) and the erase of
    // the surplus (06d89e46), each value moved as two raw dwords.
    RpcCollisionGroupListAssignRange(&Wire->CollisionGroups,
                                     Source->CollisionGroups.begin(),
                                     Source->CollisionGroups.end());
}

void RpcCharacterInfoRecordFromWire(CCharacterSet::CharacterInfo* Dest,
                                    const FRpcCharacterInfoWireRecord* Wire) {
    Dest->Id = Wire->Name;
    // "TEST CL,CL ; SETNZ DL" and 06d84647: the receiver re-normalises both
    // flags to 0/1 instead of copying the byte back verbatim.
    Dest->bIsRestart = (unsigned char)(Wire->Field_0x200 != 0 ? 1 : 0);
    Dest->bIsAutoSave = (unsigned char)(Wire->Field_0x201 != 0 ? 1 : 0);
    Dest->Name = Wire->TextA;
    Dest->Description = Wire->TextB;
    Dest->DescripStrId = Wire->Field_0x224;
    Dest->TimestampLow = Wire->Field_0x228;
    Dest->TimestampHigh = Wire->Field_0x22c;
    Dest->ShipArchetype = Wire->Field_0x230;
    Dest->Money = static_cast<int>(Wire->Field_0x234);
    Dest->VoiceId = Wire->Field_0x244;               // -> dst+0x26c
    Dest->Rank = Wire->Field_0x248;                  // -> dst+0x270
    Dest->BaseId = Wire->Field_0x23c;                // -> dst+0x264
    Dest->LocationId = Wire->Field_0x240;            // -> dst+0x268
    Dest->SystemId = Wire->Field_0x238;              // -> dst+0x260
    std::memcpy(&Dest->HullStatus, &Wire->Field_0x250, sizeof(Dest->HullStatus));  // -> dst+0x244
    Dest->NumKills = static_cast<int>(Wire->Field_0x254);              // -> dst+0x238
    Dest->NumMissionFailures = static_cast<int>(Wire->Field_0x258);    // -> dst+0x240
    Dest->NumMissionSuccesses = static_cast<int>(Wire->Field_0x25c);   // -> dst+0x23c

    RpcIdBlockFromWire(Dest->ComCostume, Wire->BlockA);   // -> dst+0x274
    RpcIdBlockFromWire(Dest->BaseCostume, Wire->BlockB);  // -> dst+0x2a8

    // one EquipDesc per wire record, through the real setters. The
    // hardpoint is unconditional here -- there is no make_internal branch, unlike
    // and bMission is written directly at EquipDesc+0x18 (06d848fd).
    Dest->Equipment.Items.clear();
    // the routine and 06d8484a JZ: only the list HEADER is tested; the dummy
    // (06d84850) and each node's record (06d84860) are used as they are.
    const FRpcEquipWireListHeader* Header = RpcEquipHandleGetList(&Wire->Equipment);
    if (Header != nullptr) {
        for (const FRpcEquipWireNode* Node = Header->Head->Next; Node != Header->Head;
             Node = Node->Next) {
            const FRpcEquipWireRecord* Record = Node->Value;
            EquipDesc Item;                                   // EquipDesc::EquipDesc()
            Item.set_arch_id(Record->ArchId);                 // set_arch_id
            Item.set_count(Record->Count);                    // set_count
            Item.set_id(Record->Id);                          // set_id
            Item.set_equipped(Record->bEquipped);        // set_equipped
            Item.set_status(Record->Status);                  // set_status
            Item.bMission = (Record->Field_0x0f != 0);        // , a direct store
            // `MOV ESI,[ESI+0x14] ; PUSH 0 ; PUSH ESI ; CALL
            // StringAlloc ; ... set_hardpoint`. Unlike the other two equipment receivers
            // this one has NO conditional and no make_internal branch -- it interns and
            // stores whatever the record holds. Interning is required here too, because
            // set_hardpoint only stores the pointer and the record's copy dies with the
            // packet.
            CacheString Hardpoint;
            Hardpoint.Value = StringAlloc(Record->Hardpoint, false);
            Item.set_hardpoint(Hardpoint);
            Dest->Equipment.Items.push_back(Item);
        }
    }

    // std::list operator= from rec+0x268 onto the fresh record's
    // +0x254 (06d84942 self test, overwrite loop, range insert, erase). The destination
    // is the record constructor's empty list, so it reduces to one append per node.
    Dest->CollisionGroups.clear();
    for (const FRpcCollisionGroupNode* Node = Wire->CollisionGroups.Head->Next;
         Node != Wire->CollisionGroups.Head; Node = Node->Next) {
        // The value moves as two raw dwords (06d8495d / 06d84963), pad included.
        CollisionGroupDesc Item;
        std::memcpy(&Item, &Node->Field_0x00, sizeof(Item));
        Dest->CollisionGroups.push_back(Item);
    }
}

// (decoder) and 06d95ad4..06d95afe (the routine): bare
// operator new(0xc), `if (Prev == 0) Prev = New`, splice before the dummy, and the value
// stored through the never-null `&node->value` test. The dummy is the one the packet
// constructor built (06d9d52f..06d9d549, FPacket_FLPACKET_SERVER_CHARACTERINFO), so it is
// read without a test, as baseline reads packet+0x28.
void RpcCharacterInfoWireListPushBack(FRpcCharacterInfoWireList* List,
                                      FRpcCharacterInfoWireRecord* Record) {
    FRpcCharacterInfoWireNode* Dummy = List->Head;
    FRpcCharacterInfoWireNode* Tail = Dummy->Prev;
    FRpcCharacterInfoWireNode* Node = static_cast<FRpcCharacterInfoWireNode*>(
        ::operator new(sizeof(FRpcCharacterInfoWireNode), std::nothrow));
    Node->Next = Dummy;
    if (Tail == nullptr) {
        Tail = Node;
    }
    Node->Prev = Tail;
    Dummy->Prev = Node;
    Node->Prev->Next = Node;
    Node->Value = Record;
    ++List->Size;
}

// , the CHARACTERINFO destructor's body, in baseline's two passes.
//   Pass 1, 06d9337c..06d9346c: for every node whose value is non-null (06d93396), the
//   record's owned members in baseline's own order, then the record; the nodes and their
//   value pointers are left alone.
//   Pass 2, 06d93472..06d934ab: every node through the routine (unlink, free,
//   --Size), then the sentinel; Head = 0, Size = 0.
void RpcCharacterInfoWireListRelease(FRpcCharacterInfoWireList* List) {
    FRpcCharacterInfoWireNode* Node = List->Head->Next;              // / 06d9337f
    while (Node != List->Head) {                                     // / 06d93465
        FRpcCharacterInfoWireRecord* Record = Node->Value;
        if (Record != nullptr) {
            // the routine (the id array at +0x14) and the block.
            if (Record->BlockA != nullptr) {
                RpcIdBlockWireDestroy(Record->BlockA);
                ::operator delete(Record->BlockA);
            }
            // the same for +0x278.
            if (Record->BlockB != nullptr) {
                RpcIdBlockWireDestroy(Record->BlockB);
                ::operator delete(Record->BlockB);
            }
            // COMMON.DLL's list<CollisionGroupDesc>::erase(begin, end),
            // the sentinel freed, Head = 0, Size = 0. No test: the record's constructor
            // always built the sentinel.
            FRpcCollisionGroupList& Groups = Record->CollisionGroups;
            FRpcCollisionGroupNode* Group = Groups.Head->Next;
            while (Group != Groups.Head) {
                FRpcCollisionGroupNode* Erased = Group;
                Group = Group->Next;
                Erased->Prev->Next = Erased->Next;
                Erased->Next->Prev = Erased->Prev;
                ::operator delete(Erased);
                --Groups.Size;
            }
            ::operator delete(Groups.Head);
            Groups.Head = nullptr;
            Groups.Size = 0;
            RpcEquipHandleDestroy(&Record->Equipment);               // the routine
            // / 06d9344c: the two basic_string destructors, +0x214 then +0x204,
            // and 06d93453 the record -- the member destructors run in that same order.
            delete Record;
        }
        Node = Node->Next;
    }
    FRpcCharacterInfoWireNode* Sentinel = List->Head;
    FRpcCharacterInfoWireNode* Erase = Sentinel->Next;
    while (Erase != Sentinel) {                                      // / 06d93494
        FRpcCharacterInfoWireNode* Erased = Erase;
        Erase = Erase->Next;
        Erased->Prev->Next = Erased->Next;
        Erased->Next->Prev = Erased->Prev;
        ::operator delete(Erased);
        --List->Size;
    }
    ::operator delete(List->Head);
    List->Head = nullptr;
    List->Size = 0;
}

// 's id-block copy, 06d94692..06d9476e for +0x274 and 06d94770..06d9484e for
// +0x278: only when the source has a block; operator new(0x18) whose new-expression test
// (06d946aa) skips just the six zero stores; the five scalars in baseline's order; then,
// for an UNSIGNED non-zero count (06d94724 JBE), a bare operator new(count * 4) and a
// dword copy whose bound re-reads the DESTINATION's count (06d94768 / 06d9476e JC).
static void RpcIdBlockWireCopy(FRpcIdArrayBlockWire** Out, const FRpcIdArrayBlockWire* Source) {
    if (Source == nullptr) {                                         // / 06d94770
        return;
    }
    FRpcIdArrayBlockWire* Block = static_cast<FRpcIdArrayBlockWire*>(
        ::operator new(sizeof(FRpcIdArrayBlockWire), std::nothrow));
    if (Block != nullptr) {
        Block->Ids = nullptr;
        Block->Count = 0;
        Block->Field_0x0c = 0;
        Block->Field_0x08 = 0;
        Block->Field_0x04 = 0;
        Block->Field_0x00 = 0;
    }
    *Out = Block;
    Block->Field_0x04 = Source->Field_0x04;
    Block->Field_0x00 = Source->Field_0x00;
    Block->Field_0x0c = Source->Field_0x0c;
    Block->Field_0x08 = Source->Field_0x08;
    Block->Count = Source->Count;
    if ((unsigned int)Block->Count != 0) {                           // / 06d94724
        Block->Ids = static_cast<unsigned int*>(
            ::operator new((unsigned int)Block->Count << 2));
        for (unsigned int i = 0; i < (unsigned int)Block->Count; ++i) {
            Block->Ids[i] = Source->Ids[i];                          // / 06d9475f
        }
    }
}

// IServerImpl::HandleCharacterInfo 06d849fe..06d84a73. Same members, same order as the
// packet destructor's pass 1 (RpcCharacterInfoWireListRelease), but inline in the
// receiver's loop and with the id blocks going through the routine (destroy + free).
void RpcCharacterInfoWireRecordFree(FRpcCharacterInfoWireRecord* Record) {
    if (Record->BlockA != nullptr) {
        RpcIdBlockWireFree(Record->BlockA);                          // the routine(1)
    }
    if (Record->BlockB != nullptr) {
        RpcIdBlockWireFree(Record->BlockB);                          // the routine(1)
    }
    // COMMON.DLL's std::list<CollisionGroupDesc> destructor on +0x268 -- every
    // node, then the dummy (built by the record constructor, so never null).
    FRpcCollisionGroupList& Groups = Record->CollisionGroups;
    FRpcCollisionGroupNode* Group = Groups.Head->Next;
    while (Group != Groups.Head) {
        FRpcCollisionGroupNode* Erased = Group;
        Group = Group->Next;
        Erased->Prev->Next = Erased->Next;
        Erased->Next->Prev = Erased->Prev;
        ::operator delete(Erased);
        --Groups.Size;
    }
    ::operator delete(Groups.Head);
    Groups.Head = nullptr;
    Groups.Size = 0;
    RpcEquipHandleDestroy(&Record->Equipment);                       // the routine
    // / 06d84a6c: the two basic_string _Tidy(1), +0x214 then +0x204, and 06d84a73
    // the record -- the member destructors run in that same order.
    delete Record;
}

// (`this` = the packet, argument = the destination header; RET 4).
void RpcCharacterInfoWireListCopy(FRpcCharacterInfoWireList* Dest,
                                  const FRpcCharacterInfoWireList* Source) {
    // the source dummy, read untested; an empty list returns at once.
    // The loop bound re-reads the source dummy every round (06d949ab).
    for (const FRpcCharacterInfoWireNode* Node = Source->Head->Next; Node != Source->Head;
         Node = Node->Next) {
        const FRpcCharacterInfoWireRecord* From = Node->Value;
        if (From == nullptr) {                                       // JZ 06d949a9
            continue;
        }
        // operator new(0x27c); the new-expression's test (06d944be) skips only
        // the constructor (06d9457b `XOR ESI,ESI`) -- the copy below runs on the result
        // either way, as in the sender. The constructor (06d944ca..06d94573) zeroes the
        // first name byte, +0x200/+0x201, +0x224, +0x228/+0x22c, +0x244..+0x25c, +0x260,
        // the handle at +0x264 and both block pointers, builds the two strings and the
        // collision list's dummy (06d94555, COMMON list ctor); +0x230..+0x240 it leaves
        // alone. The value-initialised record and RpcCollisionGroupListInit give the same
        // state, and every one of those fields is overwritten below.
        FRpcCharacterInfoWireRecord* To = new (std::nothrow) FRpcCharacterInfoWireRecord();
        RpcCollisionGroupListInit(&To->CollisionGroups);

        To->Name = From->Name;                                       // CHARACTER_ID::operator=
        To->Field_0x200 = From->Field_0x200;
        To->Field_0x201 = From->Field_0x201;
        To->TextA = From->TextA;                                     // basic_string::assign
        To->TextB = From->TextB;
        To->Field_0x224 = From->Field_0x224;
        To->Field_0x228 = From->Field_0x228;
        To->Field_0x22c = From->Field_0x22c;
        To->Field_0x230 = From->Field_0x230;
        To->Field_0x234 = From->Field_0x234;
        To->Field_0x238 = From->Field_0x238;
        To->Field_0x23c = From->Field_0x23c;
        To->Field_0x240 = From->Field_0x240;
        To->Field_0x244 = From->Field_0x244;
        To->Field_0x248 = From->Field_0x248;
        To->Field_0x24c = From->Field_0x24c;
        To->Field_0x250 = From->Field_0x250;
        To->Field_0x254 = From->Field_0x254;
        To->Field_0x258 = From->Field_0x258;
        To->Field_0x25c = From->Field_0x25c;

        RpcIdBlockWireCopy(&To->BlockA, From->BlockA);
        RpcIdBlockWireCopy(&To->BlockB, From->BlockB);

        // the routine: the equipment handle, deep.
        RpcEquipHandleCopy(&To->Equipment, &From->Equipment);
        // +0x260 is DERIVED from the copy, not copied -- the low byte
        // of the new list's Size, or 0 without a list (same postscript as the routine).
        {
            const FRpcEquipWireListHeader* Header = RpcEquipHandleGetList(&To->Equipment);
            To->Field_0x260 = (Header != nullptr) ? (unsigned char)(Header->Size & 0xffu) : 0;
        }

        // std::list<CollisionGroupDesc>::operator= inlined -- the
        // self-assignment test (06d9488c, never true here), the overwrite loop
        // (06d948b0..06d948cc), the erase of the surplus (06d948d4..06d94918) and the
        // appends (06d94928..06d94968), each value moved as two raw dwords.
        RpcCollisionGroupListAssignRange(&To->CollisionGroups,
                                         FRpcCollisionGroupNodeIterator(From->CollisionGroups.Head->Next),
                                         FRpcCollisionGroupNodeIterator(From->CollisionGroups.Head));

        // bare operator new(0xc), linked before Dest's dummy, value =
        // the record, ++Size.
        RpcCharacterInfoWireListPushBack(Dest, To);
    }
}

// ===========================================================================
// ch2 0x03 CHARACTERINFO -- the wire codec, packet vtable slots +0x04 / +0x08.

// (encode) and the routine (decode). Both walk the payload field
// by field with the one idiom FRpcMarshalStream implements:
//     Cursor += Width; if (Cursor > Limit) Error = code; if (Error) bail; access; Ptr += Width;
// -- see 06d94a23..06d94a56 on the encode side and 06d9352d..06d93554 on the decode
// side. Error is sticky and the codes differ by direction, 0x16 out and 0x17 back.

// FIELD ORDER, which is NOT the struct's order and is the thing that has to be right.
// Every line below names the encoder address that emits it; the decoder's mirror runs
// in exactly the same order.

//   header    u16   the routine (06d94a08), cursor starts at 2
//   count     u8    (byte)Records.Size, read from packet+0x2c        06d94a28
//   per record, walking packet+0x28's dummy-node ring:
//     NameLen u16 + NameLen bytes of the CHARACTER_ID at rec+0x000   06d94a92 / 06d94afe
//     u8    rec+0x200                                                06d94b3c
//     u8    rec+0x201                                                06d94b65
//     u16   TextA length in CHARACTERS + 2*length bytes              06d94b73 / 06d94bd5
//     u16   TextB length in CHARACTERS + 2*length bytes              06d94bf0 / 06d94c61
//     u32   rec+0x224, +0x22c, +0x228  (note the 22c/228 SWAP)       06d94ca1..06d94cfb
//     u32   rec+0x230, +0x234, +0x238, +0x23c, +0x240, +0x244        06d94d28..06d94e09
//     u32   rec+0x248, +0x24c, +0x250, +0x254, +0x258, +0x25c        06d94e36..06d94f18
//     BlockA: u8 present; then A+0x04, A+0x00, A+0x08, A+0x0c,
//             A+0x10 = Count, then Count*4 bytes from A+0x14         06d94f26..06d9508d
//     BlockB: the same, from rec+0x278                               06d950c6..06d9522b
//     u8    rec+0x260 = the equipment element count                  06d9528e
//     per equipment record E (the whole list at *(rec+0x264)):
//       u32 E+0x04, float E+0x08, u32 E+0x00, u16 E+0x0c,
//       u8 E+0x0e, u8 E+0x0f, u16 E+0x10 = byte count,
//       then that many bytes from E+0x14                             06d952f3..06d9542a
//     u32   rec+0x270 = the collision list's Size                    06d9545c
//     per collision node N (the ring at rec+0x26c):
//       u16 (low half of N.Field_0x00), float N.Field_0x04           06d954c3 / 06d954ec
//   tail      u32   packet+0x20                                      06d95546
//   trailer         the routine (06d9555c), a no-op

// The header is written with PutHeaderUnchecked, i.e. like the routine with no limit
// test (06da13a2). The name is memcpy'd into the 0x200-byte CHARACTER_ID with the full
// wire length and terminated at rec[NameLen], unbounded, as 06d936cb..06d936e0 do.
// ===========================================================================

// the decoder's own splice into rec+0x26c, which is the ordinary
// insert-before-the-dummy with baseline's `if (Prev == 0) Prev = the new node` quirk. The
// node is bare operator new(0x10) (06d9423f, no test); the value goes in through the
// never-null `&node->value` test at 06d94261. Returns the node: both decoders splice it
// in BEFORE reading the fields they then write into it.
static FRpcCollisionGroupNode* RpcCollisionGroupListPushBack(FRpcCollisionGroupList* List,
                                                             unsigned int Field_0x00,
                                                             unsigned int Field_0x04) {
    RpcCollisionGroupListInit(List);
    FRpcCollisionGroupNode* Dummy = List->Head;
    FRpcCollisionGroupNode* Tail = Dummy->Prev;
    FRpcCollisionGroupNode* New = static_cast<FRpcCollisionGroupNode*>(
        ::operator new(sizeof(FRpcCollisionGroupNode), std::nothrow));
    New->Next = Dummy;
    if (Tail == nullptr) {
        Tail = New;
    }
    New->Prev = Tail;
    Dummy->Prev = New;
    New->Prev->Next = New;
    New->Field_0x00 = Field_0x00;
    New->Field_0x04 = Field_0x04;
    ++List->Size;                                     // INC [EDI+0x270]
    return New;
}

// `OR ECX,-1 ; XOR EAX,EAX ; REPNE SCASB ; NOT ECX ; DEC ECX`, i.e. strlen.
// Bounded here by the buffer baseline is scanning, which is always NUL-terminated.
static unsigned int RpcCharacterIdLength(const CHARACTER_ID& Id) {
    unsigned int Length = 0;
    while (Length < (unsigned int)sizeof(Id.Buffer) && Id.Buffer[Length] != '\0') {
        ++Length;
    }
    return Length;
}

// The id block's five scalars and its array, in the encoder's order.
static void RpcEncodeIdBlock(FRpcMarshalStream& Stream, const FRpcIdArrayBlockWire* Block) {
    if (Block == nullptr) {
        Stream.PutByte(0);                            // / 06d95264
        return;
    }
    Stream.PutByte(1);                                // / 06d950f1
    Stream.PutDword(Block->Field_0x04);
    Stream.PutDword(Block->Field_0x00);
    Stream.PutDword(Block->Field_0x08);
    Stream.PutDword(Block->Field_0x0c);
    Stream.PutDword((unsigned int)Block->Count);
    // `SHL ECX,0x2` -- Count DWORDS, copied as Count*4 bytes from +0x14. The
    // encoder has no zero test here, and a zero-length run costs nothing.
    Stream.PutBytes(Block->Ids, (unsigned int)Block->Count * 4u);
}

static bool RpcDecodeIdBlock(FRpcMarshalStream& Stream, FRpcIdArrayBlockWire** Out) {
    *Out = nullptr;
    unsigned char bPresent = 0;
    if (!Stream.GetByte(bPresent)) {
        return false;
    }
    if (bPresent == 0) {                              // JZ
        return true;
    }
    // operator new(0x18). The new-expression's test (06d93bf3) only skips the
    // six zero stores: a null block is stored at 06d93c0e and written through at 06d93c44,
    // exactly as baseline does.
    FRpcIdArrayBlockWire* Block = static_cast<FRpcIdArrayBlockWire*>(
        ::operator new(sizeof(FRpcIdArrayBlockWire), std::nothrow));
    if (Block != nullptr) {
        Block->Ids = nullptr;
        Block->Count = 0;
        Block->Field_0x0c = 0;
        Block->Field_0x08 = 0;
        Block->Field_0x04 = 0;
        Block->Field_0x00 = 0;
    }
    *Out = Block;
    unsigned int Scratch = 0;
    if (!Stream.GetDword(Scratch)) { return false; }
    Block->Field_0x04 = Scratch;
    if (!Stream.GetDword(Scratch)) { return false; }
    Block->Field_0x00 = Scratch;
    if (!Stream.GetDword(Scratch)) { return false; }
    Block->Field_0x08 = Scratch;
    if (!Stream.GetDword(Scratch)) { return false; }
    Block->Field_0x0c = Scratch;
    if (!Stream.GetDword(Scratch)) { return false; }
    Block->Count = (int)Scratch;
    if ((unsigned int)Block->Count != 0) {            // TEST / JBE: unsigned
        const unsigned int Bytes = (unsigned int)Block->Count << 2;
        // bare operator new, stored at once; the stream bound is checked only
        // AFTER the allocation (06d93d63), then the copy (06d93da0).
        Block->Ids = static_cast<unsigned int*>(::operator new(Bytes, std::nothrow));
        if (!Stream.GetBytes(Block->Ids, Bytes)) {
            return false;
        }
    }
    return true;
}

unsigned int RpcEncodeServerCharacterInfo(FPacketBase& Packet, unsigned char** BufferHolder,
                                          unsigned int Capacity, unsigned int* OutLength) {
    const FPayloadServerCharacterInfo* Wire =
        reinterpret_cast<const FPayloadServerCharacterInfo*>(Packet.Payload());

    // / 06d949e6: the capacity is arg2, the cursor is *arg1.
    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));

    // `MOV CL,byte ptr [ESI+0x2c]` -- the LOW BYTE of the list's Size.
    const unsigned char Count = (unsigned char)(Wire->Records.Size & 0xffu);
    Stream.PutByte(Count);
    // and 06d94a6d: an empty count OR an empty ring skips straight to the tail.
    if (Count != 0 && Wire->Records.Head != nullptr) {
        for (const FRpcCharacterInfoWireNode* Node = Wire->Records.Head->Next;
             Node != Wire->Records.Head && !Stream.Failed(); Node = Node->Next) {
            const FRpcCharacterInfoWireRecord* Record = Node->Value;
            if (Record == nullptr) {
                continue;                                          // JZ
            }
            const unsigned int NameLen = RpcCharacterIdLength(Record->Name);
            Stream.PutWord((unsigned short)NameLen);
            Stream.PutBytes(Record->Name.Buffer, NameLen);
            Stream.PutByte(Record->Field_0x200);
            Stream.PutByte(Record->Field_0x201);
            // / 06d94ba8: the length is in CHARACTERS and the run is 2*that.
            const unsigned short LenA = (unsigned short)(Record->TextA.size() & 0xffffu);
            Stream.PutWord(LenA);
            Stream.PutBytes(LenA != 0 ? Record->TextA.data() : nullptr,
                            (unsigned int)LenA * 2u);
            const unsigned short LenB = (unsigned short)(Record->TextB.size() & 0xffffu);
            Stream.PutWord(LenB);
            Stream.PutBytes(LenB != 0 ? Record->TextB.data() : nullptr,
                            (unsigned int)LenB * 2u);
            Stream.PutDword(Record->Field_0x224);
            Stream.PutDword(Record->Field_0x22c);
            Stream.PutDword(Record->Field_0x228);
            Stream.PutDword(Record->Field_0x230);
            Stream.PutDword(Record->Field_0x234);
            Stream.PutDword(Record->Field_0x238);
            Stream.PutDword(Record->Field_0x23c);
            Stream.PutDword(Record->Field_0x240);
            Stream.PutDword(Record->Field_0x244);
            Stream.PutDword(Record->Field_0x248);
            Stream.PutDword(Record->Field_0x24c);
            Stream.PutDword(Record->Field_0x250);                   // FLD/FSTP
            Stream.PutDword(Record->Field_0x254);
            Stream.PutDword(Record->Field_0x258);
            Stream.PutDword(Record->Field_0x25c);
            RpcEncodeIdBlock(Stream, Record->BlockA);
            RpcEncodeIdBlock(Stream, Record->BlockB);
            // the stored count byte, then the WHOLE list. baseline trusts the
            // two to agree because the routine derives the byte from the list's Size;
            // reproduced rather than recomputed, so a caller that desynchronises them
            // gets baseline's behaviour and not a quieter one.
            Stream.PutByte(Record->Field_0x260);
            // the routine + 06d952ab JZ: the header is tested, the dummy
            // (06d952b1) and each node's record (06d952c8) are not.
            const FRpcEquipWireListHeader* Equip = RpcEquipHandleGetList(&Record->Equipment);
            if (Equip != nullptr) {
                for (const FRpcEquipWireNode* Item = Equip->Head->Next;
                     Item != Equip->Head && !Stream.Failed(); Item = Item->Next) {
                    const FRpcEquipWireRecord* E = Item->Value;
                    Stream.PutDword((unsigned int)E->Count);        // E+0x04
                    Stream.PutFloat(E->Status);                     // E+0x08
                    Stream.PutDword(E->ArchId);                     // E+0x00
                    Stream.PutWord(E->Id);                          // E+0x0c
                    Stream.PutByte(E->bEquipped);                   // E+0x0e
                    Stream.PutByte(E->Field_0x0f);                  // E+0x0f
                    Stream.PutWord(E->HardpointSize);               // E+0x10
                    // `MOVZX ECX,word [EDX+0x10]` with NO shift -- a BYTE count,
                    // unlike the two strings above.
                    Stream.PutBytes(E->Hardpoint, E->HardpointSize);
                }
            }
            Stream.PutDword(Record->CollisionGroups.Size);          // rec+0x270
            // `MOV ESI,[ESI+0x26c]`: the dummy of the record ctor's list, used
            // untested.
            for (const FRpcCollisionGroupNode* Group = Record->CollisionGroups.Head->Next;
                 Group != Record->CollisionGroups.Head && !Stream.Failed();
                 Group = Group->Next) {
                // `MOV AX,word ptr [ECX+0x8]` -- only the LOW HALF of the
                // first dword travels, which is why common_cobject.h's reading of
                // CollisionGroupDesc as { ushort Id; ushort Pad; float Health; } fits.
                Stream.PutWord((unsigned short)(Group->Field_0x00 & 0xffffu));
                Stream.PutDword(Group->Field_0x04);                 // FLD/FSTP
            }
        }
    }
    Stream.PutDword(Wire->Field_0x00);                              // packet+0x20
    // the routine -- `XOR EAX,EAX ; RET 0xc`, nothing to emit.
    *OutLength = Stream.Length();
    // The byte-statistics tail baseline runs on the SUCCESS PATH ONLY -- its error
    // exits jump straight past it. See FPacketWireStats.
    if (!Stream.Failed()) {
        RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    }
    return Stream.GetError();
}

unsigned int RpcDecodeServerCharacterInfo(FPacketBase& Packet, const unsigned char* Buffer,
                                          unsigned int Size, unsigned int* OutLength) {
    FPayloadServerCharacterInfo* Wire =
        reinterpret_cast<FPayloadServerCharacterInfo*>(Packet.Payload());

    // / 06d93506: the buffer pointer is taken DIRECTLY, no dereference, and
    // 's the routine reads no header.
    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);

    unsigned char Count = 0;
    if (!Stream.GetByte(Count)) {
        return Stream.GetError();
    }
    for (unsigned int Index = 0; Index < (unsigned int)Count; ++Index) {
        // operator new(0x27c). 06d9365e `TEST ESI,ESI ; JZ the branch`: a
        // failed allocation skips the record's whole body -- no bytes are read for it --
        // and the loop goes on with the next index.
        FRpcCharacterInfoWireRecord* Record = new (std::nothrow) FRpcCharacterInfoWireRecord();
        if (Record == nullptr) {
            continue;
        }
        // the record constructor's std::list at +0x268 (the dummy node).
        RpcCollisionGroupListInit(&Record->CollisionGroups);

        // Every failed read below returns the error at once, as baseline's jumps to
        // / 06d943b0 / 06d943b5 / 06d943ba -> the branch do: the record is
        // not in the packet list yet and baseline neither frees it nor reports a length
        // (the *OutLength store at 06d943c6 is on the success path only). The port leaks
        // it the same way.
        unsigned short Length = 0;
        unsigned int Scratch = 0;

        // the length word, then memcpy of the FULL wire length into
        // the 0x200-byte CHARACTER_ID and `rec[len] = 0` -- no bound, like baseline.
        if (!Stream.GetWord(Length)) {
            return Stream.GetError();
        }
        char* NameBytes = Record->Name.Buffer;
        if (!Stream.GetBytes(NameBytes, (unsigned int)Length)) {
            return Stream.GetError();
        }
        NameBytes[Length] = '\0';
        if (!Stream.GetByte(Record->Field_0x200)) { return Stream.GetError(); }
        if (!Stream.GetByte(Record->Field_0x201)) { return Stream.GetError(); }
        // the CHARACTER count, append(count, 0x20) through
        // , then 2*count bytes over it.
        if (!Stream.GetWord(Length)) {
            return Stream.GetError();
        }
        Record->TextA.append((size_t)Length, L' ');
        if (!Stream.GetBytes(Length != 0 ? &Record->TextA[0] : nullptr,
                             (unsigned int)Length * 2u)) {
            return Stream.GetError();
        }
        if (!Stream.GetWord(Length)) {
            return Stream.GetError();
        }
        Record->TextB.append((size_t)Length, L' ');
        if (!Stream.GetBytes(Length != 0 ? &Record->TextB[0] : nullptr,
                             (unsigned int)Length * 2u)) {
            return Stream.GetError();
        }
        if (!Stream.GetDword(Record->Field_0x224)) { return Stream.GetError(); }
        if (!Stream.GetDword(Record->Field_0x22c)) { return Stream.GetError(); }
        if (!Stream.GetDword(Record->Field_0x228)) { return Stream.GetError(); }
        if (!Stream.GetDword(Record->Field_0x230)) { return Stream.GetError(); }
        if (!Stream.GetDword(Record->Field_0x234)) { return Stream.GetError(); }
        if (!Stream.GetDword(Record->Field_0x238)) { return Stream.GetError(); }
        if (!Stream.GetDword(Record->Field_0x23c)) { return Stream.GetError(); }
        if (!Stream.GetDword(Record->Field_0x240)) { return Stream.GetError(); }
        if (!Stream.GetDword(Record->Field_0x244)) { return Stream.GetError(); }
        if (!Stream.GetDword(Record->Field_0x248)) { return Stream.GetError(); }
        if (!Stream.GetDword(Record->Field_0x24c)) { return Stream.GetError(); }
        if (!Stream.GetDword(Record->Field_0x250)) { return Stream.GetError(); }
        if (!Stream.GetDword(Record->Field_0x254)) { return Stream.GetError(); }
        if (!Stream.GetDword(Record->Field_0x258)) { return Stream.GetError(); }
        if (!Stream.GetDword(Record->Field_0x25c)) { return Stream.GetError(); }
        if (!RpcDecodeIdBlock(Stream, &Record->BlockA)) {
            return Stream.GetError();
        }
        if (!RpcDecodeIdBlock(Stream, &Record->BlockB)) {
            return Stream.GetError();
        }
        if (!Stream.GetByte(Record->Field_0x260)) {
            return Stream.GetError();
        }
        // `TEST AL,AL ; JBE` then a do/while over the byte just stored.
        for (unsigned int i = 0; i < (unsigned int)Record->Field_0x260; ++i) {
            // operator new(0x18); 06d94002 `TEST ESI,ESI ; JZ the branch`:
            // a failed allocation skips this element's reads and goes to the next index.
            FRpcEquipWireRecord* E = static_cast<FRpcEquipWireRecord*>(
                ::operator new(sizeof(FRpcEquipWireRecord), std::nothrow));
            if (E == nullptr) {
                continue;
            }
            RpcContainerInitRecord(E);
            if (!Stream.GetDword(Scratch)) { return Stream.GetError(); }   // E+0x04
            E->Count = (int)Scratch;
            if (!Stream.GetFloat(E->Status)) { return Stream.GetError(); } // E+0x08
            if (!Stream.GetDword(E->ArchId)) { return Stream.GetError(); } // E+0x00
            if (!Stream.GetWord(E->Id)) { return Stream.GetError(); }      // E+0x0c
            if (!Stream.GetByte(E->bEquipped)) { return Stream.GetError(); }
            if (!Stream.GetByte(E->Field_0x0f)) { return Stream.GetError(); }
            if (!Stream.GetWord(E->HardpointSize)) { return Stream.GetError(); }
            // operator new(HardpointSize) UNCONDITIONALLY -- a zero length gives
            // a zero-byte block -- untested, and stored before the stream bound check
            // (06d94176); then the copy (06d941a9).
            E->Hardpoint = static_cast<char*>(
                ::operator new((unsigned int)E->HardpointSize, std::nothrow));
            if (!Stream.GetBytes(E->Hardpoint, E->HardpointSize)) {
                return Stream.GetError();
            }
            RpcEquipHandlePushBack(&Record->Equipment, E);           // the routine
        }
        if (!Stream.GetDword(Scratch)) {                             // rec+0x270
            return Stream.GetError();
        }
        // `TEST ECX,ECX ; JBE` -- the count leads, the ring follows.
        for (unsigned int i = 0; i < Scratch; ++i) {
            // the node is spliced in FIRST, its value initialised
            // from the stack pair at [ESP+0x28] -- which holds the count just read
            // (06d94219) -- and [ESP+0x2c], stack residue that the dword read at 06d942eb
            // overwrites (0 here, per the uninitialised-memory rule). Then 06d942ae
            // overwrites only the low WORD of the first dword, so its high half keeps the
            // count's high half.
            FRpcCollisionGroupNode* Group =
                RpcCollisionGroupListPushBack(&Record->CollisionGroups, Scratch, 0u);
            unsigned short GroupId = 0;
            if (!Stream.GetWord(GroupId)) {
                return Stream.GetError();
            }
            Group->Field_0x00 = (Group->Field_0x00 & 0xffff0000u) | (unsigned int)GroupId;
            if (!Stream.GetDword(Group->Field_0x04)) {
                return Stream.GetError();
            }
        }
        // operator new(0xc), splice before the dummy, INC packet+0x2c.
        RpcCharacterInfoWireListPushBack(&Wire->Records, Record);
    }
    Stream.GetDword(Wire->Field_0x00);                              // packet+0x20
    // the routine -- the trailer hook, a no-op on this side too.
    *OutLength = Stream.Length();
    // The byte-statistics tail baseline runs on the SUCCESS PATH ONLY -- its error
    // exits jump straight past it. See FPacketWireStats.
    if (!Stream.Failed()) {
        RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    }
    return Stream.GetError();
}

// ===========================================================================
// The hardpoint-name dictionary -- the routine (build), the routine (name -> code),
// (code -> name).

// Only the CREATESHIP/CREATESOLAR equipment codec uses it, and it is a pure name
// compressor: rather than ship "HpRunningLight07" as a length byte plus sixteen bytes,
// the encoder ships one byte.

// The dictionary is NOT read from a file and CANNOT grow at runtime. It is a fixed,
// null-terminated array of char* in rpclocal's own .data at 06dbb770..06dbb814, with the
// terminating null dword at 06dbb818; every entry is transcribed below in table order
// with the .rdata address analysis gives its string.

// called once, from the serializer's construction at 06d95f07 -- walks
// that array and, per entry, inserts name -> index into the std::map at module storage
// and leaves the count in module storage (06da2942 `INC EDX` / 06da2959). Indices start
// at 1: 06da2856 seeds the counter with 1, and the routine indexes the field[code]
// (06da2a94 `MOV EAX,[EAX*0x4 + the field]`) where 06dbb76c + 4*1 == 06dbb770, the
// first entry. Code 0 therefore means "not in the dictionary".

// The port does the same: RpcBuildHardpointDictionary is the routine and runs from
// FPacketSerializer's constructor, name -> code goes through the map (the routine's
// find), and code -> name indexes the array bounded by the built count (the routine).

// The gap is baseline's, not a transcription slip: 06dbb7b0 is HpRunningLight07 and
// is HpRunningLight10 -- there is no 08 or 09.
// ===========================================================================
static const char* const GRpcHardpointNames[] = {
    nullptr,               // code 0 -- the field itself, never a valid hardpoint
    "HpCM01",              // -> 06db60d8
    "HpCargo01",           // -> 06db60cc
    "HpCargo02",           // -> 06db60c0
    "HpContrail01",        // -> 06db60b0
    "HpContrail02",        // -> 06db60a0
    "HpDockLight01",       // -> 06db6090
    "HpDockLight02",       // -> 06db6080
    "HpDockLight03",       // -> 06db6070
    "HpHeadlight",         // -> 06db6064
    "HpMine01",            // -> 06db6058
    "HpRunningLight01",    // -> 06db6044
    "HpRunningLight02",    // -> 06db6030
    "HpRunningLight03",    // -> 06db601c
    "HpRunningLight04",    // -> 06db6008
    "HpRunningLight05",    // -> 06db5ff4
    "HpRunningLight06",    // -> 06db5fe0
    "HpRunningLight07",    // -> 06db5fcc
    "HpRunningLight10",    // -> 06db5fb8   (no 08 / 09 in baseline)
    "HpRunningLight11",    // -> 06db5fa4
    "HpRunningLight12",    // -> 06db5f90
    "HpRunningLight13",    // -> 06db5f7c
    "HpShield01",          // -> 06db5f70
    "HpThruster01",        // -> 06db5f60
    "HpTurret01",          // -> 06db5f54
    "HpTurret02",          // -> 06db5f48
    "HpTurret03",          // -> 06db5f3c
    "HpTurret04",          // -> 06db5f30
    "HpTurret05",          // -> 06db5f24
    "HpTurret06",          // -> 06db5f18
    "HpTurret07",          // -> 06db5f0c
    "HpTurret08",          // -> 06db5f00
    "HpTurret09",          // -> 06db5ef4
    "HpTurret_U1_01",      // -> 06db5ee4
    "HpTurret_U1_02",      // -> 06db5ed4
    "HpTurret_U1_03",      // -> 06db5ec4
    "HpTurret_U1_04",      // -> 06db5eb4
    "HpTurret_U1_05",      // -> 06db5ea4
    "HpTurret_U3_01",      // -> 06db5e94
    "HpWeapon01",          // -> 06db5e88
    "HpWeapon02",          // -> 06db5e7c
    "HpWeapon03",          // -> 06db5e70
    "HpWeapon04",          // -> 06db5e64
    nullptr                // , the terminator the routine stops on (06da2943)
};

// , the std::map<std::string,int> the routine fills -- a CRT-initialised
// global, as in baseline. Its builder runs from the serializer's constructor, which
// (RpcGetPacketSerializer) reaches only from IServerImpl::Startup, IClient
// slot 10 or DumpPacketStats, i.e. long after static initialisation.
static std::map<std::string, int> GRpcHardpointMap;

// , the count the routine leaves: 42. Constant-initialised, so the
// builder's store survives whatever runs later.
static unsigned int GRpcHardpointCount = 0;

// , whole: count = 0 (06da2842); if the first entry is non-null (06da2848),
// then per entry `map[std::string(entry)] = code` (06da28e1 the routine, the store to
// node+0x1c at 06da28fe), code and count incremented together (06da2941 / 06da2942),
// until the next entry is null (06da2943 / 06da295f). Codes start at 1 (06da2856).
void RpcBuildHardpointDictionary() {
    GRpcHardpointCount = 0;
    int Code = 1;
    for (const char* const* Entry = &GRpcHardpointNames[1]; *Entry != nullptr; ++Entry) {
        GRpcHardpointMap[std::string(*Entry)] = Code;
        ++Code;
        ++GRpcHardpointCount;
    }
}

unsigned int RpcHardpointDictionaryCount() {
    return GRpcHardpointCount;
}

// . baseline builds a std::string from the argument (06da29b0 strlen,
// assign) and runs the map's find -- lower_bound (06da29e0 the routine) then
// `!(key < node)` (06da29fa) -- returning node+0x1c, the mapped int (06da2a69), or 0 on
// the end node (06da2a41 / 06da2a57).
unsigned int RpcHardpointNameToCode(const char* Name) {
    std::map<std::string, int>::const_iterator It = GRpcHardpointMap.find(std::string(Name));
    if (It == GRpcHardpointMap.end()) {
        return 0;
    }
    return static_cast<unsigned int>(It->second);
}

// , whole: `if (!code) return 0; if (code > count) return 0;
// return table[code];`. The upper bound is the UNSIGNED `JBE` at 06da2a8f against the
// BUILT count at module storage, so the valid range is [1, count].
const char* RpcHardpointCodeToName(unsigned int Code) {
    if (Code == 0) {
        return nullptr;
    }
    if (Code > GRpcHardpointCount) {
        return nullptr;
    }
    return GRpcHardpointNames[Code];
}

// ===========================================================================
// ch2 0x04 CREATESHIP / 0x42 CREATESOLAR -- the shared 0x7c-byte header codec

// Encoder the routine, decoder the routine. Neither is a whole packet codec: both are
// called from the concrete type's Encode/Decode slot (06d97d20 / 06d98250 for CREATESHIP,
// / 06daca80 for CREATESOLAR) AFTER the routing header has been dealt with, and
// each type's own tail follows on return. Every byte below therefore belongs to
// FRpcCreateObjectWire and stops at Wire+0x6c: the DamageList's four tail members
// (Damage.Field_0x0c/0x10/0x14/0x18 == packet+0x8c..0x98) are NOT touched by either
// function -- no [EBP + 0x8c] or higher appears anywhere in either listing. Only the
// node chain and the Size at packet+0x88 travel.

// `MOV EBP,ECX` at 06da3725 / 06da4025 is never reassigned, so every [EBP+0xNN] is a
// packet field and Wire+0xNN == packet+0x20+0xNN.

// ARITY: all 22 return sites of the encoder and both of the decoder are `RET 0xc`, i.e.
// three pushed dwords on top of the `this` in ECX -- and not one of the three is read.
// The routines run entirely off the packet's own four marshalling dwords, which the slot
// has already filled, so the port takes the FRpcMarshalStream view (which IS those four
// dwords) and nothing else.

// RETURN: 0, or the direction's sticky error -- 0x16 encode (06da373d `MOV EDI,0x16`),
// 0x17 decode (06da403d `MOV EDI,0x17`).
// ===========================================================================

// The per-equipment-record flag byte, built at 06da3ba3..06da3c0d and taken apart at
// . Every bit is a wire-width or presence switch.
static const unsigned char kEquipFlagEquipped      = 0x01; // -> 06da4481
static const unsigned char kEquipFlagMission       = 0x02; // -> 06da447b
static const unsigned char kEquipFlagWideCount     = 0x04; // -> 06da449b
static const unsigned char kEquipFlagWideId        = 0x08; // -> 06da4580
static const unsigned char kEquipFlagHardpointText = 0x10; // -> 06da45f3
static const unsigned char kEquipFlagNoHardpoint   = 0x20; // -> 06da45e7
static const unsigned char kEquipFlagUnitStatus    = 0x40; // -> 06da44f8
static const unsigned char kEquipFlagUnitCount     = 0x80; // -> 06da4490

// one equipment record.
static void RpcEncodeCreateObjectEquip(FRpcMarshalStream& Stream,
                                       const FRpcEquipWireRecord* Record) {
    unsigned char Flags = 0;
    if (Record->bEquipped != 0) {                        // / 06da3bb0
        Flags |= kEquipFlagEquipped;
    }
    if (Record->Field_0x0f != 0) {                       // / 06da3bb9
        Flags |= kEquipFlagMission;
    }
    // `CMP ECX,EAX ; JLE` with EAX == 0xff -- a SIGNED compare on the int Count.
    if (Record->Count > 255) {
        Flags |= kEquipFlagWideCount;
    }
    // `CMP word ptr [ESI+0xc],AX ; JBE` -- UNSIGNED, on the ushort Id.
    if (Record->Id > 255) {
        Flags |= kEquipFlagWideId;
    }
    // no hardpoint at all, a dictionary hit, or a literal string.
    unsigned char HardpointCode = 0;
    if (Record->Hardpoint == nullptr) {
        Flags |= kEquipFlagNoHardpoint;
    } else {
        HardpointCode =
            static_cast<unsigned char>(RpcHardpointNameToCode(Record->Hardpoint) & 0xffu);
        if (HardpointCode == 0) {
            Flags |= kEquipFlagHardpointText;
        }
    }
    if (Record->Count == 1) {                            // `CMP dword,0x1`
        Flags |= kEquipFlagUnitCount;
    }
    // `FLD [ESI+8] ; FCOMP the field` with
    // = 00 00 00 00 00 00 f0 3f == 1.0. TEST AH,0x44 + JP sets the bit
    // only on C3, i.e. only on equality, so an undamaged item ships no status byte.
    if (static_cast<double>(Record->Status) == 1.0) {
        Flags |= kEquipFlagUnitStatus;
    }

    Stream.PutByte(Flags);
    if ((Flags & kEquipFlagUnitCount) == 0) {            // JS
        if ((Flags & kEquipFlagWideCount) != 0) {
            Stream.PutDword(static_cast<unsigned int>(Record->Count));
        } else {
            Stream.PutByte(static_cast<unsigned char>(
                static_cast<unsigned int>(Record->Count) & 0xffu));
        }
    }
    if ((Flags & kEquipFlagUnitStatus) == 0) {
        // `FLD [ESI+8] ; FMUL the field (255.0) ; ftol ; MOV [ptr],AL`.
        Stream.PutQuantized(Record->Status, FRpcMarshalStream::kPercentScale);
    }
    // the archetype id narrows to a word exactly as the header's own did.
    Stream.PutWord(Archetype::LargeIDToSmallID(Record->ArchId));
    if ((Flags & kEquipFlagWideId) != 0) {
        Stream.PutWord(Record->Id);
    } else {
        Stream.PutByte(static_cast<unsigned char>(Record->Id & 0xffu));
    }
    if ((Flags & kEquipFlagNoHardpoint) != 0) {          // -> the branch
        return;
    }
    if ((Flags & kEquipFlagHardpointText) != 0) {
        // takes the LOW BYTE of HardpointSize for the length but 06da3da1
        // MOVZXes the whole WORD for the run, so a name of 256 bytes or more would ship
        // a truncated length with a full-length body. baseline's own names top out at
        // fifteen characters, and the quirk is reproduced rather than corrected.
        Stream.PutByte(static_cast<unsigned char>(Record->HardpointSize & 0xffu));
        Stream.PutBytes(Record->Hardpoint, Record->HardpointSize);
    } else {
        Stream.PutByte(HardpointCode);
    }
}

// the same record, read back.
static bool RpcDecodeCreateObjectEquip(FRpcMarshalStream& Stream,
                                       FRpcEquipWireRecord* Record) {
    unsigned char Flags = 0;
    if (!Stream.GetByte(Flags)) {
        return false;
    }
    // both bits land as a plain 0/1, not as the mask.
    Record->Field_0x0f = static_cast<unsigned char>((Flags >> 1) & 0x01u);
    Record->bEquipped = static_cast<unsigned char>(Flags & 0x01u);

    unsigned int Scratch = 0;
    if ((Flags & kEquipFlagUnitCount) != 0) {                            // JNS
        Record->Count = 1;
    } else if ((Flags & kEquipFlagWideCount) != 0) {
        if (!Stream.GetDword(Scratch)) {
            return false;
        }
        Record->Count = static_cast<int>(Scratch);
    } else {
        unsigned char Narrow = 0;
        if (!Stream.GetByte(Narrow)) {
            return false;
        }
        Record->Count = static_cast<int>(Narrow);                        // zero-ext
    }

    if ((Flags & kEquipFlagUnitStatus) != 0) {
        // `MOV dword ptr [EBX+0x8],0x3f800000` -- exactly 1.0f, which is the
        // same constant the encoder compared against.
        Record->Status = 1.0f;
    } else if (!Stream.GetQuantizedUnsigned(Record->Status,
                                            FRpcMarshalStream::kPercentInvScale)) {
        return false;
    }

    unsigned short Small = 0;
    if (!Stream.GetWord(Small)) {
        return false;
    }
    Record->ArchId = Archetype::SmallIDToLargeID(Small);                 // / 06da4583

    if ((Flags & kEquipFlagWideId) != 0) {
        if (!Stream.GetWord(Record->Id)) {
            return false;
        }
    } else {
        unsigned char Narrow = 0;
        if (!Stream.GetByte(Narrow)) {
            return false;
        }
        Record->Id = static_cast<unsigned short>(Narrow);                // zero-ext
    }

    if ((Flags & kEquipFlagNoHardpoint) != 0) {
        // both fields cleared again, though operator new(0x18) had already
        // been zeroed at 06da4435..06da444d.
        Record->HardpointSize = 0;
        Record->Hardpoint = nullptr;
        return true;
    }
    unsigned char Raw = 0;
    if ((Flags & kEquipFlagHardpointText) != 0) {
        if (!Stream.GetByte(Raw)) {
            return false;
        }
        Record->HardpointSize = static_cast<unsigned short>(Raw);        // zero-ext
        if (Record->HardpointSize == 0) {                                // / 06da462e
            return true;
        }
        // bare operator new, stored at once (06da4643), bound checked after
        // (06da4653), then the copy.
        Record->Hardpoint = static_cast<char*>(
            ::operator new(Record->HardpointSize, std::nothrow));
        return Stream.GetBytes(Record->Hardpoint, Record->HardpointSize);
    }
    if (!Stream.GetByte(Raw)) {
        return false;
    }
    // the routine: an out-of-range code simply leaves the record hardpointless
    // (06da46c9 `CMP ESI,EDI ; JZ`), which is how baseline survives a dictionary mismatch
    // between the two ends of the wire.
    const char* Name = RpcHardpointCodeToName(static_cast<unsigned int>(Raw));
    if (Name == nullptr) {
        return true;
    }
    // `OR ECX,-1 ; REPNE SCASB ; NOT ECX` is strlen+1, stored WITH
    // the terminator, and 06da46f0 copies the terminator too.
    unsigned int Length = 0;
    while (Name[Length] != '\0') {
        ++Length;
    }
    ++Length;
    Record->HardpointSize = static_cast<unsigned short>(Length & 0xffffu);
    // MOVZX / 06da46e0: bare operator new of the stored WORD, then the byte
    // loop 06da46f0..06da46f8 up to and including the NUL.
    Record->Hardpoint = static_cast<char*>(
        ::operator new(static_cast<unsigned int>(Record->HardpointSize), std::nothrow));
    unsigned int Index = 0;
    do {
        Record->Hardpoint[Index] = Name[Index];
    } while (Name[Index++] != '\0');
    return true;
}

// one collision-group node, appended before the dummy with
// baseline's `if (Prev == 0) Prev = the new node` quirk, exactly as the CHARACTERINFO
// decoder does it. RpcCollisionGroupListPushBack above already is that splice.

unsigned int RpcEncodeCreateObjectCommon(FRpcMarshalStream& Stream,
                                         const FRpcCreateObjectWire* Wire) {
    Stream.PutDword(Wire->Field_0x00);
    // the archetype id goes out narrowed to 16 bits and comes back through
    // SmallIDToLargeID at 06da4091.
    Stream.PutWord(Archetype::LargeIDToSmallID(Wire->Field_0x04));
    Stream.PutDword(Wire->Field_0x08);
    Stream.PutDword(static_cast<unsigned int>(Wire->VibeGroupId));
    // The swap is baseline's: packet+0x34 (Wire+0x14) goes out BEFORE packet+0x30.
    Stream.PutDword(Wire->Field_0x14);
    Stream.PutDword(Wire->Field_0x10);
    // reads the count as a BYTE off the int field, before the bounds check.
    Stream.PutByte(static_cast<unsigned char>(
        static_cast<unsigned int>(Wire->IdCount) & 0xffu));
    // `SHL EAX,0x2` -- IdCount DWORDS, checked and copied as IdCount*4 bytes.
    Stream.PutBytes(Wire->Ids, static_cast<unsigned int>(Wire->IdCount) * 4u);
    Stream.PutDword(Wire->Field_0x28);
    // checks 0xc at once and 06da398a hands the routine the three dwords
    // Wire+0x2c/0x30/0x34 in memory order; the routine is a plain three-dword copy
    // (06d8f514..06d8f521), not a quantizer.
    Stream.PutBytes(&Wire->Field_0x2c, 12u);
    // four ftol(f * 127.0) bytes. Memory order is w,x,y,z but the
    // wire order is x,y,z,w -- Field_0x38 goes LAST.
    Stream.PutQuantized(Wire->Field_0x3c, FRpcMarshalStream::kOrientationScale);
    Stream.PutQuantized(Wire->Field_0x40, FRpcMarshalStream::kOrientationScale);
    Stream.PutQuantized(Wire->Field_0x44, FRpcMarshalStream::kOrientationScale);
    Stream.PutQuantized(Wire->Field_0x38, FRpcMarshalStream::kOrientationScale);
    {
        // , the hull fraction:
        //   FILD  [EBP+0x68]            -- Field_0x48 read as a SIGNED int
        //   FSTP  float [local_14]      -- rounded to float before the divide
        //   FILD  GetArchMaxHitPts(Field_0x04)
        //   FDIVR float [local_14] ; FMUL 255.0 ; ftol ; store one byte
        const float HullPoints = static_cast<float>(static_cast<int>(Wire->Field_0x48));
        const int MaxHitPts = Archetype::GetArchMaxHitPts(Wire->Field_0x04);
        unsigned char Percent = 0;
        if (MaxHitPts != 0) {
            const double Scaled = (static_cast<double>(HullPoints) /
                                   static_cast<double>(MaxHitPts)) *
                                  FRpcMarshalStream::kPercentScale;
            Percent = static_cast<unsigned char>(
                static_cast<unsigned long>(static_cast<long>(Scaled)) & 0xffu);
        }
        // MaxHitPts == 0 cannot be reached through Common.dll (GetArchMaxHitPts defaults
        // to 1 on a miss). If it ever were, baseline's FDIVR would produce an infinity and
        // its ftol would return 0x80000000, whose low byte is 0 -- which is the byte the
        // branch above emits. So this is baseline's result without baseline's undefined cast.
        Stream.PutByte(Percent);
    }
    if (SinglePlayer()) {                                                // / 06da3b02
        Stream.PutDword(Wire->Field_0x4c);
    }
    // the routine -- the handle's list pointer, which is null until the first
    // push_back. A null list ships a zero COUNT and no records (the branch), which is
    // not the same code path as a present-but-empty list.
    const FRpcEquipWireListHeader* Equip = RpcEquipHandleGetList(&Wire->Equipment);
    if (Equip == nullptr) {
        Stream.PutWord(static_cast<unsigned short>(0));
    } else {
        // `MOV CX,word ptr [EAX+0x8]` -- the LOW WORD of the list's Size.
        Stream.PutWord(static_cast<unsigned short>(Equip->Size & 0xffffu));
        // `MOV EAX,[EDX+0x4] ; MOV EDI,[EAX]`: the dummy is used untested, and
        // so is each node's record (06da3ba3 reads [ESI+0xe] straight away -- unlike
        // CHARACTERINFO's encoder, which tests it at 06d94a80). The marshaller builds all
        // three lists (RpcCreateObjectHeaderWrite), so every dummy exists here.
        for (const FRpcEquipWireNode* Node = Equip->Head->Next;
             Node != Equip->Head && !Stream.Failed(); Node = Node->Next) {
            RpcEncodeCreateObjectEquip(Stream, Node->Value);
        }
    }
    // `MOV CL,byte ptr [EBP+0x7c]` -- the LOW BYTE of the list's _Size (packet
    // +0x7c); the collision-group count is ONE BYTE here, unlike CHARACTERINFO's dword at
    // .
    Stream.PutByte(static_cast<unsigned char>(Wire->CollisionGroups.size() & 0xffu));
    // `MOV ESI,[EBX]`: begin() off the list the packet constructor built.
    for (std::list<CollisionGroupDesc>::const_iterator Group = Wire->CollisionGroups.begin();
         Group != Wire->CollisionGroups.end() && !Stream.Failed(); ++Group) {
        // `MOV CL,byte ptr [ESI+0x8]` -- one byte of CollisionGroupDesc::Id,
        // where CHARACTERINFO sends a word (06d954c3).
        Stream.PutByte(static_cast<unsigned char>(Group->Id & 0xffu));
        // `FLD float [ESI+0xc] ; FMUL 255.0` -- CollisionGroupDesc::Health, a
        // FLOAT in [0,1]; CHARACTERINFO's raw dword at 06d954ec is the same field
        // unquantized.
        Stream.PutQuantized(Group->Health, FRpcMarshalStream::kPercentScale);
    }
    if (!SinglePlayer()) {                                               // / 06da3f24
        return Stream.GetError();
    }
    // the DamageList's list _Size (packet+0x88) as a whole dword.
    Stream.PutDword(static_cast<unsigned int>(Wire->Damage.Entries.size()));
    // `MOV EAX,[ECX]`: begin() off the DamageList the constructor built.
    for (std::list<DamageEntry>::const_iterator Entry = Wire->Damage.Entries.begin();
         Entry != Wire->Damage.Entries.end() && !Stream.Failed(); ++Entry) {
        // word SubObjId (value+0x00), 06da3fc6 dword Fate (value+0x08),
        // float HitPts (value+0x04) -- the middle field goes out LAST.
        Stream.PutWord(Entry->SubObjId);
        Stream.PutDword(static_cast<unsigned int>(Entry->Fate));
        Stream.PutFloat(Entry->HitPts);
    }
    return Stream.GetError();                                            // XOR EAX,EAX
}

unsigned int RpcDecodeCreateObjectCommon(FRpcMarshalStream& Stream,
                                         FRpcCreateObjectWire* Wire) {
    if (!Stream.GetDword(Wire->Field_0x00)) {
        return Stream.GetError();
    }
    unsigned short Small = 0;
    if (!Stream.GetWord(Small)) {
        return Stream.GetError();
    }
    Wire->Field_0x04 = Archetype::SmallIDToLargeID(Small);               // / 06da4097
    if (!Stream.GetDword(Wire->Field_0x08)) {
        return Stream.GetError();
    }
    unsigned int Scratch = 0;
    if (!Stream.GetDword(Scratch)) {
        return Stream.GetError();
    }
    Wire->VibeGroupId = static_cast<int>(Scratch);
    if (!Stream.GetDword(Wire->Field_0x14)) {
        return Stream.GetError();
    }
    if (!Stream.GetDword(Wire->Field_0x10)) {
        return Stream.GetError();
    }
    // / 06da4154: two fields the encoder never sends are cleared here, which is
    // the only thing in either direction that touches packet+0x38 / +0x3c.
    Wire->Field_0x18 = 0;
    Wire->Field_0x1c = 0;

    unsigned char Narrow = 0;
    if (!Stream.GetByte(Narrow)) {
        return Stream.GetError();
    }
    Wire->IdCount = static_cast<int>(Narrow);                            // zero-ext
    if (Wire->IdCount != 0) {                                            // JBE
        // bare operator new, stored unchecked at 06da4196.
        Wire->Ids = static_cast<unsigned int*>(
            ::operator new(static_cast<unsigned int>(Wire->IdCount) * 4u, std::nothrow));
        if (!Stream.GetBytes(Wire->Ids, static_cast<unsigned int>(Wire->IdCount) * 4u)) {
            return Stream.GetError();
        }
    }
    if (!Stream.GetDword(Wire->Field_0x28)) {
        return Stream.GetError();
    }
    // checks 0xc and 06da4242 hands the routine the stream cursor as the
    // SOURCE this time -- the arguments are the mirror of the encoder's.
    if (!Stream.GetBytes(&Wire->Field_0x2c, 12u)) {
        return Stream.GetError();
    }
    {
        // baseline reads all FOUR bytes first and only then runs the
        // conversions, so a short stream leaves none of the four fields written. Done the
        // same way here rather than field by field.
        unsigned char Raw[4] = { 0, 0, 0, 0 };
        for (unsigned int Index = 0; Index < 4u; ++Index) {
            if (!Stream.GetByte(Raw[Index])) {
                return Stream.GetError();
            }
        }
        // MOVSX at 06da42ed / 06da42f6 / 06da4307 / 06da430a, then FILD * (1/127).
        Wire->Field_0x3c = static_cast<float>(
            static_cast<double>(static_cast<signed char>(Raw[0])) *
            FRpcMarshalStream::kOrientationInvScale);
        Wire->Field_0x40 = static_cast<float>(
            static_cast<double>(static_cast<signed char>(Raw[1])) *
            FRpcMarshalStream::kOrientationInvScale);
        Wire->Field_0x44 = static_cast<float>(
            static_cast<double>(static_cast<signed char>(Raw[2])) *
            FRpcMarshalStream::kOrientationInvScale);
        Wire->Field_0x38 = static_cast<float>(
            static_cast<double>(static_cast<signed char>(Raw[3])) *
            FRpcMarshalStream::kOrientationInvScale);
    }
    {
        // , the hull fraction back to absolute hit points:
        //   byte -> ZERO-extended (06da4370 `XOR EDX,EDX ; MOV DL,BL`)
        //   FILD GetArchMaxHitPts(Field_0x04) ; FILD byte ; FMUL (1/255) ; FMULP ; ftol
        // Note the decoder multiplies by the reciprocal where the encoder divided, and
        // widens WITHOUT sign where the orientation quadruple above widened WITH it.
        unsigned char Percent = 0;
        if (!Stream.GetByte(Percent)) {
            return Stream.GetError();
        }
        const double MaxHitPts =
            static_cast<double>(Archetype::GetArchMaxHitPts(Wire->Field_0x04));
        const double Fraction = static_cast<double>(static_cast<unsigned int>(Percent)) *
                                FRpcMarshalStream::kPercentInvScale;
        Wire->Field_0x48 = static_cast<unsigned int>(
            static_cast<long>(MaxHitPts * Fraction));
    }
    if (SinglePlayer()) {                                                // / 06da439f
        if (!Stream.GetDword(Wire->Field_0x4c)) {
            return Stream.GetError();
        }
    }

    unsigned short Count = 0;
    if (!Stream.GetWord(Count)) {
        return Stream.GetError();
    }
    // `TEST CX,CX ; JBE` -- TEST clears CF, so this is a plain zero test.
    for (unsigned int Index = 0; Index < static_cast<unsigned int>(Count); ++Index) {
        FRpcEquipWireRecord* Record = static_cast<FRpcEquipWireRecord*>(
            ::operator new(sizeof(FRpcEquipWireRecord), std::nothrow));
        if (Record == nullptr) {
            // `CMP EBX,EDI ; JZ the branch` -- baseline SKIPS the record and
            // keeps looping, leaving the stream pointing at a record body it never reads.
            continue;
        }
        RpcContainerInitRecord(Record);
        if (!RpcDecodeCreateObjectEquip(Stream, Record)) {
            // A stream error: baseline returns straight out of the branch with the record
            // still in hand and not yet in the list -- leaked, and so it is here.
            return Stream.GetError();
        }
        RpcEquipHandlePushBack(&Wire->Equipment, Record);
    }

    unsigned char GroupCount = 0;
    if (!Stream.GetByte(GroupCount)) {
        return Stream.GetError();
    }
    for (unsigned int Index = 0; Index < static_cast<unsigned int>(GroupCount); ++Index) {
        // push_back on the packet's std::list<CollisionGroupDesc> --
        // the node is spliced in and _Size bumped FIRST, its value copied from an
        // unwritten stack pair (the routine from local_14 -- residue in baseline, 0 here
        // per the uninitialised-memory rule); the reads then fill it in place.
        CollisionGroupDesc Blank = {};
        Wire->CollisionGroups.push_back(Blank);
        CollisionGroupDesc& Group = Wire->CollisionGroups.back();
        unsigned char GroupId = 0;
        if (!Stream.GetByte(GroupId)) {
            return Stream.GetError();
        }
        // writes only the WORD at node+0x8 (a zero-extended byte): the Id.
        Group.Id = static_cast<unsigned short>(GroupId);
        float Health = 0.0f;
        if (!Stream.GetQuantizedUnsigned(Health,
                                         FRpcMarshalStream::kPercentInvScale)) {
            return Stream.GetError();
        }
        Group.Health = Health;                                           // FSTP
    }

    if (!SinglePlayer()) {                                               // / 06da4834
        return Stream.GetError();
    }
    unsigned int DamageCount = 0;
    if (!Stream.GetDword(DamageCount)) {
        return Stream.GetError();
    }
    // `CMP ECX,EBX ; JBE` with EBX zeroed at 06da4856 -- and it really is zeroed
    // there, even though the collision loop above left its own counter in EBX. A zero
    // count is simply the end of the packet.
    if (DamageCount == 0) {
        return Stream.GetError();
    }
    for (unsigned int Index = 0; Index < DamageCount; ++Index) {
        // the value is default constructed to { SUBOBJ_ID_NONE (a WORD
        // through the data import at [06db3100]), 0xbf800000 == -1.0f, 0 } -- which is
        // DamageEntry's own constructor -- then 06da4894 `PUSH 0x14` / 06da489b bare
        // operator new, the splice before the dummy, and 06da48d1 `INC dword ptr
        // [EBP+0x88]`: the list's _Size, which the port's DamageList also keeps as Count.
        // The reads below then fill the node in place, so a short stream leaves the
        // defaults in it, as baseline does.
        Wire->Damage.Entries.push_back(DamageEntry());
        ++Wire->Damage.Count;
        DamageEntry& Entry = Wire->Damage.Entries.back();
        if (!Stream.GetWord(Entry.SubObjId)) {                           // / 06da490b
            return Stream.GetError();
        }
        unsigned int Fate = 0;
        if (!Stream.GetDword(Fate)) {
            return Stream.GetError();
        }
        Entry.Fate = static_cast<DamageEntry::SubObjFate>(Fate);
        // four raw bytes into value+0x04, the float HitPts.
        if (!Stream.GetFloat(Entry.HitPts)) {
            return Stream.GetError();
        }
    }
    return Stream.GetError();                                            // XOR EAX,EAX
}

// ===========================================================================
// The four OUTER wire slots -- ch2 0x04 CREATESHIP and ch2 0x42 CREATESOLAR

// CREATESHIP  encode   0x06d98250 CREATESHIP  decode
// CREATESOLAR encode   0x06daca80 CREATESOLAR decode

// Each is the packet vtable's own +0x04 / +0x08 entry. Shape, identical in all four:
//   1. fill packet+0x10..0x1c -- Cursor = 0, Limit = arg2, Ptr = the buffer, Error = 0
//      (06d97d31 / 06d9825f / 06dac7ce / 06daca8e, always `LEA reg,[this+0x10]`);
//   2. the routing-header hook -- the routine out (two bytes, cursor := 2),
// back (a no-op, the dispatcher already ate them);
//   3. the shared 0x7c header, the routine / the routine, all three stack arguments
//      passed straight through;
//   4. this type's own tail, below;
//   5. the routine again as the trailer hook, in BOTH directions;
//   6. *arg3 = the cursor, then the per-type statistics record.

// CORRECTION, established here on the bytes: the DamageList's four tail members
// (FRpcCreateObjectWire::Damage.Field_0x0c..0x18 == packet+0x8c..0x98) do NOT travel and
// do NOT belong to these slots. Every packet offset the four listings mention is either
// the marshalling scratch (0x10..0x1c), the packet id (0x0c) or 0x9c and above -- the
// lowest payload byte any of them touches is payload+0x7c, exactly where the shared
// header ends. The shared codec stops below it too (its highest is packet+0x88, inside
// the damage list's own Size). So payload+0x6c..0x78 -- DamageList's bDestroyed, cause,
// inflictor id and inflictor owner -- exists only on the in-process the routine /
// path and is never serialized by anyone.

// ARGUMENT ASYMMETRY, re-proven on both pairs rather than inherited:
//   * `MOV EAX,[ESP+0x4]` then 06d97d37 `MOV ECX,dword ptr [EAX]` -- and
// / 06dac7d4 are the same two instructions -- so encode's arg1 is a
//     pointer TO the buffer pointer;
//   * `MOV EAX,[ESP+0x8]` (after the `PUSH ECX` at 06d98250) then 06d9826b
//     `MOV [EBP+0x8],EAX`, and 06daca80 / 06daca9a likewise, with no load in between --
//     decode's arg1 IS the buffer.

// baseline's epilogue also updates a per-type statistics record --
// (packet+0x0c) -> the routine on .data:the field, then count/total/
// max/average at record+0x00/0x18/0x10/0x08 for the encode side and +0x04/0x1c/0x14/
// 0x0c for the decode side (06d98211..06d98245, 06d985c3..06d985fe, 06daca45..06daca71,
// ). That is RpcRecordEncodedPacket / RpcRecordDecodedPacket on
// GRpcPacketStats (rpc_transport.cpp), which every codec's success path calls.
// Note that the decode side's maximum really is
// `if (Cursor > Max + 2) Max = Cursor + 2` (06d985ea `ADD EAX,0x2` against the
// unmodified cursor in EBP at 06d985ed), which is NOT `if (Cursor + 2 > Max)`; both
// decoders emit it identically, so it is baseline's and not a transcription slip.
// ===========================================================================

// Both create-object slots move packet+0xa0..0xa8 as ONE twelve-byte run (06d97dec
// checks 0xc in a single step), so the three members really must be adjacent with no
// padding between them. Checked, not assumed.

// The shared header is no longer baseline's 0x7c bytes (its two containers are this
// toolchain's std::list and DamageList, FRpcCreateObjectWire), so each tail is pinned
// RELATIVE to its first field, which follows the header directly: baseline offset minus
// 0x7c on the right-hand side.
static_assert(offsetof(FPayloadServerCreateShip, Field_0x7c) == sizeof(FRpcCreateObjectWire),
              "CREATESHIP's tail follows the shared header directly");
static_assert(offsetof(FPayloadServerCreateSolar, Field_0x7c) == sizeof(FRpcCreateObjectWire),
              "CREATESOLAR's tail follows the shared header directly");
static_assert(offsetof(FPayloadServerCreateShip, Field_0x80) -
                  offsetof(FPayloadServerCreateShip, Field_0x7c) == 0x80 - 0x7c,
              "the twelve-byte run starts at CREATESHIP payload+0x80");
static_assert(offsetof(FPayloadServerCreateShip, Field_0x84) -
                  offsetof(FPayloadServerCreateShip, Field_0x7c) == 0x84 - 0x7c,
              "module state moves packet+0xa0, +0xa4 and +0xa8 back to back");
static_assert(offsetof(FPayloadServerCreateShip, Field_0x88) -
                  offsetof(FPayloadServerCreateShip, Field_0x7c) == 0x88 - 0x7c,
              "module state moves packet+0xa0, +0xa4 and +0xa8 back to back");
static_assert(offsetof(FPayloadServerCreateShip, Field_0x98) -
                  offsetof(FPayloadServerCreateShip, Field_0x7c) == 0x98 - 0x7c,
              "the 127.0-quantized float is CREATESHIP payload+0x98");
static_assert(offsetof(FPayloadServerCreateShip, DisplayNameLength) -
                  offsetof(FPayloadServerCreateShip, Field_0x7c) == 0xb8 - 0x7c,
              "the wide display name is length at payload+0xb8, buffer at +0xbc");
static_assert(sizeof(FPayloadServerCreateShip) -
                  offsetof(FPayloadServerCreateShip, Field_0x7c) == 0xcc - 0x7c,
              "CREATESHIP's tail is 0x50 bytes (baseline payload 0xcc)");
static_assert(offsetof(FPayloadServerCreateSolar, NameASize) -
                  offsetof(FPayloadServerCreateSolar, Field_0x7c) == 0x80 - 0x7c,
              "module state is handed &payload+0x80 for CREATESOLAR's first name block");
static_assert(offsetof(FPayloadServerCreateSolar, NameBSize) -
                  offsetof(FPayloadServerCreateSolar, Field_0x7c) == 0x88 - 0x7c,
              "module state is handed &payload+0x88 for CREATESOLAR's second name block");
static_assert(sizeof(FPayloadServerCreateSolar) -
                  offsetof(FPayloadServerCreateSolar, Field_0x7c) == 0x98 - 0x7c,
              "CREATESOLAR's tail is 0x1c bytes (baseline payload 0x98)");

// The create-object encoders inline this pair twice each: a dword byte-count followed by
// that many raw bytes -- 06d9803b/06d9807e and 06d980d8/06d9811d for CREATESHIP,
// /06dac955 and 06dac9aa/06dac9ea for CREATESOLAR.
bool RpcPutSizedBlock(FRpcMarshalStream& Stream, unsigned int Size,
                      const void* Buffer) {
    if (!Stream.PutDword(Size)) {
        return false;
    }
    return Stream.PutBytes(Buffer, Size);
}

// , the decode-side counterpart, called with a pointer to the CONTIGUOUS
// {Size, Buffer} pair (06d9859e / 06d985ac, 06dacbb1 / 06dacbbf). Body at
// advance 4 and read the size into pair[0], operator_new(size) into
// pair[1], advance by size, then REP MOVS the bytes in. baseline allocates BEFORE the
// second bounds check, which is why the pointer is stored even when the read then fails
// -- the packet's destructor is what frees it either way.
bool RpcGetSizedBlock(FRpcMarshalStream& Stream, unsigned int& OutSize,
                      void*& OutBuffer) {
    if (!Stream.GetDword(OutSize)) {
        return false;
    }
    OutBuffer = ::operator new(OutSize, std::nothrow);                   // , untested
    return Stream.GetBytes(OutBuffer, OutSize);
}

// The CREATESHIP tail's one flag byte, built at 06d97d82..06d97daf and taken apart at
// . Bit 0x04 is a presence switch as well as a value bit: it decides
// between the wide display name and the two flattened FmtStrs.
static const unsigned char kCreateShipFlagField_0x7c = 0x01;  // -> 06d982da
static const unsigned char kCreateShipFlagField_0xc8 = 0x02;  // -> 06d982e5
static const unsigned char kCreateShipFlagField_0x9c = 0x04;  // -> 06d983b9

// CREATESHIP, packet vtable +0x04.
unsigned int RpcEncodeServerCreateShip(FPacketBase& Packet, unsigned char** BufferHolder,
                                       unsigned int Capacity, unsigned int* OutLength) {
    const FPayloadServerCreateShip* Wire =
        reinterpret_cast<const FPayloadServerCreateShip*>(Packet.Payload());

    // Cursor = 0, Limit = Capacity, Ptr = *BufferHolder, Error = 0.
    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    // the routine -- the two routing bytes, and the cursor starts at 2.
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));
    // the routine, the shared 0x7c header. baseline DISCARDS the return value
    // here (06d97d82 reads the next field straight away) and lets the sticky error stop
    // the first field below, so this port does not add a test baseline has not got.
    RpcEncodeCreateObjectCommon(Stream, &Wire->Common);

    unsigned char Flags = 0;
    if (Wire->Field_0x7c != 0) {                                         // byte test
        Flags |= kCreateShipFlagField_0x7c;
    }
    if (Wire->Field_0x9c != 0) {                                         // DWORD test
        Flags |= kCreateShipFlagField_0x9c;
    }
    if (Wire->Field_0xc8 != 0) {                                         // byte test
        Flags |= kCreateShipFlagField_0xc8;
    }
    if (!Stream.PutByte(Flags)) {                                        // / 06d97de7
        return Stream.GetError();
    }

    // checks 0xc in one go and 06d97e42 hands the routine a stack copy of the
    // three dwords at packet+0xa0..0xa8; the routine is a plain three-dword move, so
    // the twelve bytes go out in memory order.
    unsigned char VectorBytes[12];
    memcpy(VectorBytes, &Wire->Field_0x80, sizeof(VectorBytes));
    if (!Stream.PutBytes(VectorBytes, sizeof(VectorBytes))) {
        return Stream.GetError();
    }
    // `FLD float ptr [EBX+0xb8]` * (127.0), ftol, one byte.
    if (!Stream.PutQuantized(Wire->Field_0x98, FRpcMarshalStream::kOrientationScale)) {
        return Stream.GetError();
    }
    if (!Stream.PutWord(Wire->Field_0xa0)) {
        return Stream.GetError();
    }
    if (!Stream.PutByte(Wire->Field_0xa2)) {
        return Stream.GetError();
    }

    if ((Flags & kCreateShipFlagField_0x9c) != 0) {                      // TEST AL,0x4
        // Only the LOW BYTE of the dword travels (06d97f09 `MOV DL,byte [EBX+0xbc]`);
        // the decoder zero-extends it back into all four bytes.
        if (!Stream.PutByte((unsigned char)(Wire->Field_0x9c & 0xffu))) {
            return Stream.GetError();
        }
        // Field_0xa4 rides on this branch ONLY -- nothing writes it when the bit is
        // clear, and the decoder leaves the packet's zero in place.
        if (!Stream.PutWord(Wire->Field_0xa4)) {
            return Stream.GetError();
        }
        if (!SinglePlayer()) {                                           // / 06d97f73
            // writes the LOW BYTE of the dword length but 06d97fbf copies
            // `length * 2` bytes off the full dword. baseline's own desynchronisation for
            // a name of 128 characters or more; reproduced, not quietly repaired.
            if (!Stream.PutByte((unsigned char)(Wire->DisplayNameLength & 0xffu))) {
                return Stream.GetError();
            }
            if (!Stream.PutBytes(Wire->DisplayName, Wire->DisplayNameLength * 2u)) {
                return Stream.GetError();
            }
        }
    } else if (!SinglePlayer()) {                                        // / 06d98006
        if (!RpcPutSizedBlock(Stream, Wire->NameASize, Wire->NameABuffer)) {
            return Stream.GetError();
        }
        if (!RpcPutSizedBlock(Stream, Wire->NameBSize, Wire->NameBBuffer)) {
            return Stream.GetError();
        }
    }

    // baseline asks SinglePlayer() a second time here rather than reusing the
    // answer; it is a pure read of a Common.dll flag, so asking twice is the same.
    if (!SinglePlayer()) {
        if (!Stream.PutDword(Wire->Affiliation)) {
            return Stream.GetError();
        }
        // `FLD float ptr [EBX+0xe4]` * 127.0 -- the attitude, [-1,+1].
        if (!Stream.PutQuantized(Wire->Attitude, FRpcMarshalStream::kOrientationScale)) {
            return Stream.GetError();
        }
    }
    // the routine -- `XOR EAX,EAX ; RET 0xc`, nothing to emit.
    *OutLength = Stream.Length();
    // The byte-statistics tail baseline runs on the SUCCESS PATH ONLY -- its error
    // exits jump straight past it. See FPacketWireStats.
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// CREATESHIP, packet vtable +0x08.
unsigned int RpcDecodeServerCreateShip(FPacketBase& Packet, const unsigned char* Buffer,
                                       unsigned int Size, unsigned int* OutLength) {
    FPayloadServerCreateShip* Wire =
        reinterpret_cast<FPayloadServerCreateShip*>(Packet.Payload());

    // , and 06d9826b takes the buffer pointer with no dereference.
    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    // the routine -- the header hook on this side reads nothing.
    // the routine, the shared 0x7c header; its return value is discarded at
    // exactly as on the encode side.
    RpcDecodeCreateObjectCommon(Stream, &Wire->Common);

    unsigned char Flags = 0;
    if (!Stream.GetByte(Flags)) {                                        // / 06d982cb
        return Stream.GetError();
    }
    // `MOV AL,0x1` -- both bits store the literal 1, and a clear bit leaves the
    // field alone rather than zeroing it. The packet arrives zeroed, so it comes to the
    // same thing; transcribed as baseline has it.
    if ((Flags & kCreateShipFlagField_0x7c) != 0) {
        Wire->Field_0x7c = 1;
    }
    if ((Flags & kCreateShipFlagField_0xc8) != 0) {
        Wire->Field_0xc8 = 1;
    }

    // checks 0xc at once, 06d98318 hands the routine the cursor as the source
    // and &packet+0xa0 as the destination -- the same three dwords, read back in place.
    if (!Stream.GetBytes(&Wire->Field_0x80, 12u)) {
        return Stream.GetError();
    }
    // `MOVSX EAX,CL` then FILD * (1/127): SIGNED, which is what
    // makes the byte cover [-1,+1].
    if (!Stream.GetQuantizedSigned(Wire->Field_0x98,
                                   FRpcMarshalStream::kOrientationInvScale)) {
        return Stream.GetError();
    }
    if (!Stream.GetWord(Wire->Field_0xa0)) {
        return Stream.GetError();
    }
    if (!Stream.GetByte(Wire->Field_0xa2)) {
        return Stream.GetError();
    }

    if ((Flags & kCreateShipFlagField_0x9c) != 0) {
        unsigned char Narrow = 0;
        if (!Stream.GetByte(Narrow)) {
            return Stream.GetError();
        }
        // `XOR EAX,EAX ; MOV AL,CL` -- ZERO-extended into the whole dword.
        Wire->Field_0x9c = (unsigned int)Narrow;
        if (!Stream.GetWord(Wire->Field_0xa4)) {
            return Stream.GetError();
        }
        if (!SinglePlayer()) {                                           // / 06d98432
            unsigned char LengthByte = 0;
            if (!Stream.GetByte(LengthByte)) {
                return Stream.GetError();
            }
            // `MOVSX EAX,CL` -- baseline SIGN-extends this count and uses the
            // signed value for the stored length (06d98469), for `operator new(2*n + 2)`
            // (06d98464 / 06d9846f, bare, stored unchecked at 06d98474) and for the byte
            // run (06d98486 SHL, 06d984c3 REP MOVS). A byte of 0x80..0xff therefore
            // wraps the cursor backwards past the bound check and copies a wrapped
            // count -- reproduced as it is, like baseline.
            const unsigned int Length = (unsigned int)(int)(signed char)LengthByte;
            Wire->DisplayNameLength = Length;
            Wire->DisplayName = static_cast<wchar_t*>(
                ::operator new((Length * 2u) + 2u, std::nothrow));
            if (!Stream.GetBytes(Wire->DisplayName, Length * 2u)) {      // / 06d984c3
                return Stream.GetError();
            }
            Wire->DisplayName[Length] = L'\0';
        }
    } else if (!SinglePlayer()) {                                        // / 06d9858f
        if (!RpcGetSizedBlock(Stream, Wire->NameASize, Wire->NameABuffer)) {
            return Stream.GetError();
        }
        if (!RpcGetSizedBlock(Stream, Wire->NameBSize, Wire->NameBBuffer)) {
            return Stream.GetError();
        }
    }

    if (!SinglePlayer()) {                                               // / 06d984f4
        if (!Stream.GetDword(Wire->Affiliation)) {
            return Stream.GetError();
        }
        // `MOVSX EDX,CL` * 1/127 -- signed again, same as Field_0x98.
        if (!Stream.GetQuantizedSigned(Wire->Attitude,
                                       FRpcMarshalStream::kOrientationInvScale)) {
            return Stream.GetError();
        }
    }
    // the routine -- the trailer hook, a no-op on this side too.
    *OutLength = Stream.Length();
    // The byte-statistics tail baseline runs on the SUCCESS PATH ONLY -- its error
    // exits jump straight past it. See FPacketWireStats.
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// CREATESOLAR, packet vtable +0x04. Shorter than CREATESHIP and with NO
// flag byte at all: its two leading bytes go out unconditionally, and the reputation
// pair comes BEFORE the two name blocks rather than after them.
unsigned int RpcEncodeServerCreateSolar(FPacketBase& Packet, unsigned char** BufferHolder,
                                        unsigned int Capacity, unsigned int* OutLength) {
    const FPayloadServerCreateSolar* Wire =
        reinterpret_cast<const FPayloadServerCreateSolar*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));
    RpcEncodeCreateObjectCommon(Stream, &Wire->Common);
    if (!Stream.PutByte(Wire->Field_0x7c)) {
        return Stream.GetError();
    }
    if (!Stream.PutByte(Wire->Field_0x7d)) {
        return Stream.GetError();
    }
    if (!SinglePlayer()) {                                               // / 06dac88d
        if (!Stream.PutDword(Wire->Affiliation)) {
            return Stream.GetError();
        }
        // `FLD float ptr [EBP+0xb4]` * 127.0.
        if (!Stream.PutQuantized(Wire->Attitude, FRpcMarshalStream::kOrientationScale)) {
            return Stream.GetError();
        }
        if (!RpcPutSizedBlock(Stream, Wire->NameASize, Wire->NameABuffer)) {
            return Stream.GetError();
        }
        if (!RpcPutSizedBlock(Stream, Wire->NameBSize, Wire->NameBBuffer)) {
            return Stream.GetError();
        }
    }
    // the routine -- the trailer hook.
    *OutLength = Stream.Length();
    // The byte-statistics tail baseline runs on the SUCCESS PATH ONLY -- its error
    // exits jump straight past it. See FPacketWireStats.
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// CREATESOLAR, packet vtable +0x08.
unsigned int RpcDecodeServerCreateSolar(FPacketBase& Packet, const unsigned char* Buffer,
                                        unsigned int Size, unsigned int* OutLength) {
    FPayloadServerCreateSolar* Wire =
        reinterpret_cast<FPayloadServerCreateSolar*>(Packet.Payload());

    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    // the routine, then 06dacacf the routine with its result discarded.
    RpcDecodeCreateObjectCommon(Stream, &Wire->Common);
    if (!Stream.GetByte(Wire->Field_0x7c)) {
        return Stream.GetError();
    }
    if (!Stream.GetByte(Wire->Field_0x7d)) {
        return Stream.GetError();
    }
    if (!SinglePlayer()) {                                               // / 06dacb33
        if (!Stream.GetDword(Wire->Affiliation)) {
            return Stream.GetError();
        }
        // `MOVSX EDX,CL` * 1/127 -- signed, like every quantized field in this
        // family; the 255.0 pair belongs to the shared header codec, not to these tails.
        if (!Stream.GetQuantizedSigned(Wire->Attitude,
                                       FRpcMarshalStream::kOrientationInvScale)) {
            return Stream.GetError();
        }
        if (!RpcGetSizedBlock(Stream, Wire->NameASize, Wire->NameABuffer)) {
            return Stream.GetError();
        }
        if (!RpcGetSizedBlock(Stream, Wire->NameBSize, Wire->NameBBuffer)) {
            return Stream.GetError();
        }
    }
    // the routine -- the trailer hook.
    *OutLength = Stream.Length();
    // The byte-statistics tail baseline runs on the SUCCESS PATH ONLY -- its error
    // exits jump straight past it. See FPacketWireStats.
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// ===========================================================================
// ch1 0x02 FLPACKET_COMMON_FIREWEAPON -- encoder the branch (597 instructions),
// decoder the branch (557). The heaviest anonymous vtable pair left in the module,
// and the only codec so far that changes the WIDTH of a field depending on its value.

// WHOSE SLOTS THESE ARE, established before a line of it was ported. Both addresses
// appear exactly once in the whole listing, as module storage and module storage --
// consecutive entries of the four-word table the routine. The table's only
// reference is 06d9afe6 `MOV dword ptr [EAX],.rdata:the routine` inside
// , and that function's next instruction, 06d9afec
// `MOV dword ptr [EAX + 0xc],0x10002`, writes the routing header: channel 1, opcode 2,
// which rpc_packet_registry.cpp registers as FLPACKET_COMMON_FIREWEAPON (ctor 0x06d9afb0,
// factory 0x06d9af70, size 0x40). So the pair is +0x04 Encode / +0x08 Decode of the
// FIREWEAPON packet class, not of anything else, and the payload they walk is
// FPayloadCommonFireWeapon.

// The same constructor also settles the payload layout, which had been read off the
// IServer argument struct alone:
// [EAX+0x10] = 0          -- the marshal state, packet-wide
// [EAX+0x20] = 0          -- ObjectId
// [EAX+0x24] = 0x7f7fffff -- TargetPos.x, and +0x28 / +0x2c the same FLT_MAX
// [EAX+0x30] = the ctor's char argument
// [EAX+0x34] = [EAX+0x38] = [EAX+0x3c] = 0
// That +0x30 byte is NOT a field of its own: it is the allocator sub-object of the
// hardpoint vector, whose _First/_Last/_End follow at +0x34/+0x38/+0x3c. The compiler
// copies a byte of the incoming allocator into it and nothing ever reads it -- the same
// junk-allocator-copy this port already confirmed at 06d87ee8 on a std::list. Neither
// codec serialises it. FbaselineVector<unsigned short> at payload+0x10 reproduces exactly
// that, so FPayloadCommonFireWeapon needed no change.

// THE WIRE FORMAT. The first two bytes are the routing word (06da4f94 the routine),
// then the object id as a plain dword. What follows depends on COMMON.DLL::SinglePlayer
// (06da4fea / 06da55cb), which BOTH directions consult, so the two ends agree:

//   single player -- 06da4ff8..06da50d7 / 06da55df..06da5690
//       12 raw bytes of TargetPos (the routine is a plain three-dword move),
//       one count byte, then one word per hardpoint id. No flag byte at all.

//   multiplayer -- 06da50e8..06da54db / 06da56bd..06da5aee
//       one flag byte, then each of x, y, z in the width its flag bits chose, then the
//       count byte (unless bit 0x40 says there is exactly one id), then the ids.

// THE TIERS, from the .rdata constants rather than from the shape of the code:
// = 40 50 .. = 64.0        the small threshold   (06da50f3 FCOM)
// = 40 c0 .. = 8192.0      the medium threshold  (06da5106 FCOMP)
// = 3c800000 = 2^-6        (06da51b5 FMUL, the small tier's pre-scale)
// = 127.0                  (06da51bb FMUL, the small tier's scale)
// = 39000000 = 2^-13       (06da520b FMUL, the medium tier's pre-scale)
// = 40dfffc0.. = 32767.0   (06da5211 FMUL, the medium tier's scale)
// = 3fe02040.. = 64/127    (06da5712 FMUL, the small tier's inverse)
// = 3fd00020.. = 8192/32767(06da574f FMUL, the medium tier's inverse)
// The two inverses are not guesses: 64/127 is (1/2)(1 + 2^-7 + 2^-14 + ...) and
// 8192/32767 is (1/4)(1 + 2^-15 + 2^-30 + ...), and both mantissas in .rdata are exactly
// those repeating patterns (02 04 08 10 20 40 80 and 20 00 40 00 80).

// The port folds baseline's two FMULs into one constant. That is not an approximation:
// the first factor is an exact power of two, so `v * 2^-6` is exact in any precision and
// `(v * 2^-6) * 127.0` and `v * (127.0/64.0)` round identically.

// FOUR THINGS TO LEAVE ALONE, all of which read as mistakes and are not:

//  1. Z HAS NO MEDIUM TIER. The flag computation tests x against both thresholds
//     (06da50f3 and 06da5106) and y against both (06da511e and 06da5132), but z against
//     64.0 only -- 06da514b `FCOMP double ptr [the field]`, and then nothing. Bit 0x20
//     is therefore never set by any encoder. Both the encoder's writer (06da53c5
//     `TEST BL,0x20`) and the decoder (06da5859 `TEST DL,0x20`) still handle it, so the
//     branch is transcribed and left unreachable, exactly as baseline leaves it. Filling in
//     the missing tier "for symmetry" would put bytes on the wire that baseline's own
//     decoder would read back correctly and baseline's own encoder would never produce --
//     a divergence that no test would catch until two different builds talked.
//  2. THE TWO ONE-HARDPOINT TESTS ARE NOT THE SAME TEST. 06da516d compares the whole
//     int (`CMP EAX,0x1`) to decide bit 0x40; 06da5466 compares only the low byte
//     (`CMP AL,0x1`) to decide whether to leave the count byte off. They disagree at
//     exactly 257 hardpoints. Reproduced as found.
//  3. THE SINGLE-PLAYER BRANCH HAS NO 0x40 SHORTCUT. 06da5061 writes the count byte
//     unconditionally.
//  4. THE FLAG BYTE IS COMPUTED BEFORE IT IS WRITTEN, AND THE COUNT AFTER. baseline does
//     not cache the count across the two uses: it recomputes `(last-first)>>1` at
// for the flag and again at 06da545f for the write.

// The decoder's own quirk: on both branches it reads the count as an UNSIGNED byte and
// the loop counts in a BYTE register on both branches, so a count of 0 reads nothing and
// a count of 0xff reads 255 ids. The two branches do not share a register pair, and the
// note here used to give one branch's instruction text with the other branch's address:
//   single player  06da568c `INC BL` / 06da568e `CMP BL,AL`  / 06da5690 `JC the branch`
//   multiplayer    06da5ae6 `INC AL` / 06da5ae8 `CMP AL,CL`  / 06da5aee `JC the branch`
// (the MP counter is the byte at ESP+0x13 and its bound the byte at ESP+0x24; the
// that stood here is inside 06da5aea `MOV byte ptr [ESP + 0x13],AL`).
// ===========================================================================

// |v|, without pulling in <cmath>. baseline uses FABS; the two agree on NaN as well --
// baseline's FCOM leaves C0 and C2 both set for an unordered compare, TEST AH,0x5 then has
// even parity, the JP is taken, and the value falls through to the raw-float tier, which
// is what `!(NaN < limit)` does here too.
static double RpcFireWeaponAbs(double Value) {
    return Value < 0.0 ? -Value : Value;
}

// / 06da5106 and their y and z copies.
static const double kFireWeaponSmallLimit = 64.0;
static const double kFireWeaponMediumLimit = 8192.0;
// * and the field * , folded (see above).
static const double kFireWeaponSmallScale = 127.0 / 64.0;
static const double kFireWeaponMediumScale = 32767.0 / 8192.0;
static const double kFireWeaponSmallInvScale = 64.0 / 127.0;
static const double kFireWeaponMediumInvScale = 8192.0 / 32767.0;

static const unsigned char kFireWeaponXSmall = 0x01;   // MOV BL,0x1
static const unsigned char kFireWeaponXMedium = 0x02;  // MOV BL,0x2
static const unsigned char kFireWeaponYSmall = 0x04;   // OR BL,0x4
static const unsigned char kFireWeaponYMedium = 0x08;  // OR BL,0x8
static const unsigned char kFireWeaponZSmall = 0x10;   // OR BL,0x10
// Read at 06da53c5 and 06da5859, written nowhere. See note 1 above.
static const unsigned char kFireWeaponZMedium = 0x20;
static const unsigned char kFireWeaponOneHardpoint = 0x40;  // OR BL,0x40

// The per-component width decision, 06da50e8..06da515f. bMedium is false for z.
static unsigned char RpcFireWeaponPickTier(float Value, unsigned char SmallBit,
                                           unsigned char MediumBit, bool bHasMediumTier) {
    const double Abs = RpcFireWeaponAbs(static_cast<double>(Value));
    if (Abs < kFireWeaponSmallLimit) {
        return SmallBit;
    }
    if (bHasMediumTier && Abs < kFireWeaponMediumLimit) {
        return MediumBit;
    }
    return 0;
}

// / 06da5208 / 06da5282 and their y and z copies -- one body, three call sites,
// exactly as baseline's three copies differ only in the field and the bits.
static bool RpcFireWeaponPutComponent(FRpcMarshalStream& Stream, float Value,
                                      unsigned char Flags, unsigned char SmallBit,
                                      unsigned char MediumBit) {
    if ((Flags & SmallBit) != 0) {
        return Stream.PutQuantized(Value, kFireWeaponSmallScale);
    }
    if ((Flags & MediumBit) != 0) {
        return Stream.PutQuantizedWord(Value, kFireWeaponMediumScale);
    }
    // `FLD float ptr [EDI+0x24] ; FSTP float ptr [EAX]` -- the bits as they are.
    return Stream.PutFloat(Value);
}

// / 06da573a / 06da5772 and their y and z copies.
static bool RpcFireWeaponGetComponent(FRpcMarshalStream& Stream, float& OutValue,
                                      unsigned char Flags, unsigned char SmallBit,
                                      unsigned char MediumBit) {
    if ((Flags & SmallBit) != 0) {
        return Stream.GetQuantizedSigned(OutValue, kFireWeaponSmallInvScale);
    }
    if ((Flags & MediumBit) != 0) {
        return Stream.GetQuantizedWordSigned(OutValue, kFireWeaponMediumInvScale);
    }
    // `MOV ECX,[EAX] ; MOV [ESI+0x24],ECX` -- a dword move, not a conversion.
    return Stream.GetFloat(OutValue);
}

// FIREWEAPON, packet vtable +0x04.
unsigned int RpcEncodeCommonFireWeapon(FPacketBase& Packet, unsigned char** BufferHolder,
                                       unsigned int Capacity, unsigned int* OutLength) {
    const FPayloadCommonFireWeapon* Wire =
        reinterpret_cast<const FPayloadCommonFireWeapon*>(Packet.Payload());

    // Cursor = 0, Limit = Capacity (the SECOND argument), Ptr =
    // *BufferHolder (the FIRST, dereferenced), Error = 0.
    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    // the routine -- the two routing bytes.
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));
    // .
    if (!Stream.PutDword(Wire->ObjectId)) {
        return FRpcMarshalStream::kEncodeOverflow;
    }

    if (SinglePlayer()) {                                        // / 06da4ff2
        // baseline copies the three floats to its own stack and hands
        // that to the routine, a plain three-dword move, so twelve bytes go out in
        // memory order.
        unsigned char VectorBytes[12];
        memcpy(VectorBytes, &Wire->TargetPos, sizeof(VectorBytes));
        if (!Stream.PutBytes(VectorBytes, sizeof(VectorBytes))) {
            return FRpcMarshalStream::kEncodeOverflow;
        }
        // the count byte, unconditionally on this branch.
        const unsigned int SpCount = Wire->Hardpoints.Count();
        if (!Stream.PutByte((unsigned char)(SpCount & 0xffu))) {
            return FRpcMarshalStream::kEncodeOverflow;
        }
        // enters the loop only when _First != _Last, then walks by 2.
        for (unsigned int i = 0; i < SpCount; ++i) {
            if (!Stream.PutWord(Wire->Hardpoints.Begin[i])) {
                return FRpcMarshalStream::kEncodeOverflow;
            }
        }
    } else {
        unsigned char Flags = 0;                                 // XOR BL,BL
        Flags |= RpcFireWeaponPickTier(Wire->TargetPos.x, kFireWeaponXSmall,
                                       kFireWeaponXMedium, true);
        Flags |= RpcFireWeaponPickTier(Wire->TargetPos.y, kFireWeaponYSmall,
                                       kFireWeaponYMedium, true);
        // false: z really is tested against 64.0 only. See note 1 at the top.
        Flags |= RpcFireWeaponPickTier(Wire->TargetPos.z, kFireWeaponZSmall,
                                       kFireWeaponZMedium, false);
        // the full int is compared here, unlike 06da5466.
        if (Wire->Hardpoints.Begin != nullptr && Wire->Hardpoints.Count() == 1u) {
            Flags |= kFireWeaponOneHardpoint;
        }
        if (!Stream.PutByte(Flags)) {
            return FRpcMarshalStream::kEncodeOverflow;
        }

        if (!RpcFireWeaponPutComponent(Stream, Wire->TargetPos.x, Flags,
                                       kFireWeaponXSmall, kFireWeaponXMedium)) {
            return FRpcMarshalStream::kEncodeOverflow;
        }
        if (!RpcFireWeaponPutComponent(Stream, Wire->TargetPos.y, Flags,
                                       kFireWeaponYSmall, kFireWeaponYMedium)) {
            return FRpcMarshalStream::kEncodeOverflow;
        }
        if (!RpcFireWeaponPutComponent(Stream, Wire->TargetPos.z, Flags,
                                       kFireWeaponZSmall, kFireWeaponZMedium)) {
            return FRpcMarshalStream::kEncodeOverflow;
        }

        // the count is recomputed, and the byte is SKIPPED when its
        // low byte is 1 -- the decoder recovers that case from bit 0x40 instead.
        const unsigned int MpCount = Wire->Hardpoints.Count();
        const unsigned char CountByte = (unsigned char)(MpCount & 0xffu);
        if (!(Wire->Hardpoints.Begin != nullptr && CountByte == 1u)) {
            if (!Stream.PutByte(CountByte)) {
                return FRpcMarshalStream::kEncodeOverflow;
            }
        }
        // enters the loop only when _First != _Last.
        for (unsigned int i = 0; i < MpCount; ++i) {
            if (!Stream.PutWord(Wire->Hardpoints.Begin[i])) {
                return FRpcMarshalStream::kEncodeOverflow;
            }
        }
    }

    // the routine -- `XOR EAX,EAX ; RET 0xc`, the trailer hook, a no-op.
    *OutLength = Stream.Length();
    // The byte-statistics tail baseline runs on the SUCCESS PATH ONLY -- its error
    // exits jump straight past it. See FPacketWireStats.
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// FIREWEAPON, packet vtable +0x08.
unsigned int RpcDecodeCommonFireWeapon(FPacketBase& Packet, const unsigned char* Buffer,
                                       unsigned int Size, unsigned int* OutLength) {
    FPayloadCommonFireWeapon* Wire =
        reinterpret_cast<FPayloadCommonFireWeapon*>(Packet.Payload());

    // Limit = Size (the SECOND argument), Ptr = Buffer (the FIRST,
    // taken WITHOUT a dereference -- the decode side is handed the bytes, not a holder).
    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    // the routine -- the same no-op hook; the routing word is already gone by
    // the time the decoder is called, so the cursor starts at the object id.
    if (!Stream.GetDword(Wire->ObjectId)) {
        return FRpcMarshalStream::kDecodeOverrun;
    }

    // baseline rebuilds the hardpoint vector one element at a time, at _Last: with
    // COMMON.DLL::std::vector<unsigned short>::insert(_Last, 1, v) (IAT module storage,
    // ?insert@?$vector@GV?$allocator@G@std@@@std@@QAEXPAGIABG@Z) on the single-player
    // branch at 06da5682, and with the same routine INLINED on the multiplayer branch at
    // . RpcbaselineVectorAppend is that insert, capacity policy and all.
    // It APPENDS: a freshly constructed packet holds the three nulls the routine wrote.
    unsigned char Count = 0;
    if (SinglePlayer()) {                                        // / 06da55d9
        // twelve bytes straight into TargetPos.
        unsigned char VectorBytes[12];
        if (!Stream.GetBytes(VectorBytes, sizeof(VectorBytes))) {
            return FRpcMarshalStream::kDecodeOverrun;
        }
        memcpy(&Wire->TargetPos, VectorBytes, sizeof(VectorBytes));
        if (!Stream.GetByte(Count)) {
            return FRpcMarshalStream::kDecodeOverrun;
        }
    } else {
        unsigned char Flags = 0;
        if (!Stream.GetByte(Flags)) {
            return FRpcMarshalStream::kDecodeOverrun;
        }
        if (!RpcFireWeaponGetComponent(Stream, Wire->TargetPos.x, Flags,
                                       kFireWeaponXSmall, kFireWeaponXMedium)) {
            return FRpcMarshalStream::kDecodeOverrun;
        }
        if (!RpcFireWeaponGetComponent(Stream, Wire->TargetPos.y, Flags,
                                       kFireWeaponYSmall, kFireWeaponYMedium)) {
            return FRpcMarshalStream::kDecodeOverrun;
        }
        if (!RpcFireWeaponGetComponent(Stream, Wire->TargetPos.z, Flags,
                                       kFireWeaponZSmall, kFireWeaponZMedium)) {
            return FRpcMarshalStream::kDecodeOverrun;
        }
        // bit 0x40 stands in for a count byte of 1.
        if ((Flags & kFireWeaponOneHardpoint) != 0) {
            Count = 1;
        } else if (!Stream.GetByte(Count)) {
            return FRpcMarshalStream::kDecodeOverrun;
        }
    }

    // / 06da58f8: `TEST CL,CL ; JBE` -- an unsigned test, so 0 reads nothing.
    // / 06da5aee: the counter is a byte compared unsigned against Count.
    for (unsigned int Index = 0; Index < (unsigned int)Count; ++Index) {
        unsigned short Id = 0;
        if (!Stream.GetWord(Id)) {                               // / 06da5900
            return FRpcMarshalStream::kDecodeOverrun;
        }
        RpcbaselineVectorAppend(Wire->Hardpoints, Id);             // / 06da5945
    }

    // the routine again, and the same no-op.
    *OutLength = Stream.Length();                            // onward
    // The byte-statistics tail baseline runs on the SUCCESS PATH ONLY -- its error
    // exits jump straight past it. See FPacketWireStats.
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// ===========================================================================
// ch3 0x22 FLPACKET_CLIENT_DBGCREATESHIP -- the branch (+0x04, 597 instructions) and
// (+0x08, 495). The layout, and the two corrections to the note that
// identified them, are set out on FPayloadClientDbgCreateShip in rpc_payloads.h.

// WHOSE SLOTS THESE ARE, re-checked rather than inherited. Both addresses appear
// exactly once in the listing, at module storage and module storage, the second and
// third words of the routine. The table is published at 06da0e89
// `MOV dword ptr [EAX],.rdata:the routine` inside the routine, whose very next
// instruction 06da0e8f writes `dword ptr [EAX+0xc],0x30022` -- channel 3, opcode 0x22.
// The table's only other reference, 06db0508, is the same class's destructor
// restoring the vptr before it frees the four blocks, not another type.

// ARGUMENT ASYMMETRY, verified here on these two bodies and not carried over:
//   * encode  06db0560 `MOV EAX,[ESP+0x8]`  (arg2 = capacity) and 06db056b
//     `MOV ECX,[ESP+0x1c]` after SUB ESP,0x10 + two PUSHes = arg1, then 06db0576
//     `MOV EDX,dword ptr [ECX]` -- arg1 is a pointer TO the buffer pointer;
//   * decode  06db0b30 `MOV EAX,[ESP+0x8]` (arg2 = size) and 06db0b39
//     `MOV ECX,[ESP+0x10]` after three PUSHes = arg1, stored straight into the stream
//     at 06db0b43 `MOV [EBX+0x8],ECX` with no load between -- arg1 IS the buffer.
// Both end in `RET 0xc`, so three dword arguments each.

// NOT A QUANTIZER IN SIGHT. Neither body contains a single FLD/FMUL/ftol: every field
// travels at its natural width. The two Vectors and the Quaternion go out as raw
// dwords through the plain 3- and 4-dword copiers. So the "pick GetQuantizedSigned or
// GetQuantizedUnsigned per field from the bytes" rule has nothing to decide here, and
// inventing a quantizer by analogy with FIREWEAPON would have put bytes on the wire
// baseline never writes.

// THE ZERO-COUNT SHORTCUT IS REAL AND IS IN BOTH DIRECTIONS. Each of the four blocks
// is preceded by `TEST EAX,EAX ; JBE <past the block>` on the COUNT -- 06db07e2,
// , 06db08ff, 06db0a63 out and 06db0d3a, 06db0dc9, 06db0e58, 06db0f9a back --
// so a count of zero consumes no bytes and, on the decode side, allocates nothing and
// leaves the pointer as the constructor left it.

// TODO, the same open gap the create-object slots carry: baseline's epilogue also
// updates the per-type statistics record through the routine(packet+0x0c). The encode
// side does count/total/max/average at record+0x00/+0x18/+0x10/+0x08 (06db0aee..
// ) and the decode side at +0x04/+0x1c/+0x14/+0x0c (06db1035..06db106c),
// against a cursor that the decoder biases by two because its header hook reads
// nothing. No codec in this port reproduces it yet; it wants doing once for all of
// them together.
// ===========================================================================

// DBGCREATESHIP, packet vtable +0x04.
unsigned int RpcEncodeClientDbgCreateShip(FPacketBase& Packet, unsigned char** BufferHolder,
                                          unsigned int Capacity, unsigned int* OutLength) {
    const FPayloadClientDbgCreateShip* Wire =
        reinterpret_cast<const FPayloadClientDbgCreateShip*>(Packet.Payload());

    // Cursor = 0, Limit = Capacity, Ptr = *BufferHolder, Error = 0.
    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    // the routine -- the two routing bytes; the cursor starts at 2.
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));

    // , five plain dwords.
    if (!Stream.PutDword(Wire->Field_0x00) ||
        !Stream.PutDword(Wire->Field_0x04) ||
        !Stream.PutDword(Wire->Field_0x08) ||
        !Stream.PutDword(Wire->Field_0x0c) ||
        !Stream.PutDword(Wire->Field_0x10)) {
        return FRpcMarshalStream::kEncodeOverflow;
    }

    // two twelve-byte runs through the routine, a plain three-dword
    // move, so the floats go out in memory order.
    if (!Stream.PutBytes(&Wire->Field_0x14, (unsigned int)sizeof(Vector)) ||
        !Stream.PutBytes(&Wire->Field_0x20, (unsigned int)sizeof(Vector))) {
        return FRpcMarshalStream::kEncodeOverflow;
    }
    // sixteen bytes through the routine, the four-dword move.
    if (!Stream.PutBytes(&Wire->Orientation, (unsigned int)sizeof(Quaternion))) {
        return FRpcMarshalStream::kEncodeOverflow;
    }
    // .
    if (!Stream.PutDword(Wire->Field_0x3c)) {
        return FRpcMarshalStream::kEncodeOverflow;
    }

    // the wide name -- the CHARACTER count, then count*2 bytes.
    // baseline does not null-check the pointer before the REP MOVS and neither does this,
    // for the same reason the FIREWEAPON encoder does not check Hardpoints.Begin.
    if (!Stream.PutDword(Wire->NameCharCount)) {
        return FRpcMarshalStream::kEncodeOverflow;
    }
    if (Wire->NameCharCount != 0 &&
        !Stream.PutBytes(Wire->NameChars, Wire->NameCharCount * 2u)) {
        return FRpcMarshalStream::kEncodeOverflow;
    }

    // and 06db08c6..06db094c: two BYTE blocks, count then bytes.
    if (!Stream.PutDword(Wire->BlockASize)) {
        return FRpcMarshalStream::kEncodeOverflow;
    }
    if (Wire->BlockASize != 0 && !Stream.PutBytes(Wire->BlockA, Wire->BlockASize)) {
        return FRpcMarshalStream::kEncodeOverflow;
    }
    if (!Stream.PutDword(Wire->BlockBSize)) {
        return FRpcMarshalStream::kEncodeOverflow;
    }
    if (Wire->BlockBSize != 0 && !Stream.PutBytes(Wire->BlockB, Wire->BlockBSize)) {
        return FRpcMarshalStream::kEncodeOverflow;
    }

    // , four more plain dwords.
    if (!Stream.PutDword(Wire->Field_0x50) ||
        !Stream.PutDword(Wire->Field_0x54) ||
        !Stream.PutDword(Wire->Field_0x58) ||
        !Stream.PutDword(Wire->Field_0x5c)) {
        return FRpcMarshalStream::kEncodeOverflow;
    }

    // the dword array -- the ELEMENT count (06db0a6a `SHL EAX,0x2`
    // turns it into bytes), then count*4 bytes.
    if (!Stream.PutDword(Wire->DwordArrayCount)) {
        return FRpcMarshalStream::kEncodeOverflow;
    }
    if (Wire->DwordArrayCount != 0 &&
        !Stream.PutBytes(Wire->DwordArray, Wire->DwordArrayCount * 4u)) {
        return FRpcMarshalStream::kEncodeOverflow;
    }

    // the routine -- `XOR EAX,EAX ; RET 0xc`, the no-op trailer hook.
    *OutLength = Stream.Length();
    // The byte-statistics tail baseline runs on the SUCCESS PATH ONLY -- its error
    // exits jump straight past it. See FPacketWireStats.
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// DBGCREATESHIP, packet vtable +0x08.

// baseline allocates each block BEFORE the bounds check that guards the copy, and stores
// the pointer either way -- 06db0d3f / 06db0dcc / 06db0e5b / 06db0fa0 all write the
// packet field immediately after operator_new and only then advance the cursor. The
// packet's destructor the routine is what frees it, so a half-decoded packet still
// releases cleanly. The port keeps that order for the same reason.
unsigned int RpcDecodeClientDbgCreateShip(FPacketBase& Packet, const unsigned char* Buffer,
                                          unsigned int Size, unsigned int* OutLength) {
    FPayloadClientDbgCreateShip* Wire =
        reinterpret_cast<FPayloadClientDbgCreateShip*>(Packet.Payload());

    // Limit = Size, Ptr = Buffer taken WITHOUT a dereference,
    // Cursor = 0, Error = 0.
    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    // the routine -- the no-op header hook; the routing word is already gone.

    if (!Stream.GetDword(Wire->Field_0x00) ||
        !Stream.GetDword(Wire->Field_0x04) ||
        !Stream.GetDword(Wire->Field_0x08) ||
        !Stream.GetDword(Wire->Field_0x0c) ||
        !Stream.GetDword(Wire->Field_0x10)) {
        return FRpcMarshalStream::kDecodeOverrun;
    }

    if (!Stream.GetBytes(&Wire->Field_0x14, (unsigned int)sizeof(Vector)) ||
        !Stream.GetBytes(&Wire->Field_0x20, (unsigned int)sizeof(Vector))) {
        return FRpcMarshalStream::kDecodeOverrun;
    }
    if (!Stream.GetBytes(&Wire->Orientation, (unsigned int)sizeof(Quaternion))) {
        return FRpcMarshalStream::kDecodeOverrun;
    }
    if (!Stream.GetDword(Wire->Field_0x3c)) {
        return FRpcMarshalStream::kDecodeOverrun;
    }

    // the wide name.
    if (!Stream.GetDword(Wire->NameCharCount)) {
        return FRpcMarshalStream::kDecodeOverrun;
    }
    if (Wire->NameCharCount != 0) {
        const unsigned int NameBytes = Wire->NameCharCount * 2u;   // ADD EAX,EAX
        Wire->NameChars = static_cast<wchar_t*>(
            ::operator new(NameBytes, std::nothrow));              // , untested
        if (!Stream.GetBytes(Wire->NameChars, NameBytes)) {
            return FRpcMarshalStream::kDecodeOverrun;
        }
    }

    // and 06db0e28..06db0eaf: the two byte blocks.
    if (!Stream.GetDword(Wire->BlockASize)) {
        return FRpcMarshalStream::kDecodeOverrun;
    }
    if (Wire->BlockASize != 0) {
        Wire->BlockA = ::operator new(Wire->BlockASize, std::nothrow);  // , untested
        if (!Stream.GetBytes(Wire->BlockA, Wire->BlockASize)) {
            return FRpcMarshalStream::kDecodeOverrun;
        }
    }
    if (!Stream.GetDword(Wire->BlockBSize)) {
        return FRpcMarshalStream::kDecodeOverrun;
    }
    if (Wire->BlockBSize != 0) {
        Wire->BlockB = ::operator new(Wire->BlockBSize, std::nothrow);  // , untested
        if (!Stream.GetBytes(Wire->BlockB, Wire->BlockBSize)) {
            return FRpcMarshalStream::kDecodeOverrun;
        }
    }

    if (!Stream.GetDword(Wire->Field_0x50) ||
        !Stream.GetDword(Wire->Field_0x54) ||
        !Stream.GetDword(Wire->Field_0x58) ||
        !Stream.GetDword(Wire->Field_0x5c)) {
        return FRpcMarshalStream::kDecodeOverrun;
    }

    // the dword array.
    if (!Stream.GetDword(Wire->DwordArrayCount)) {
        return FRpcMarshalStream::kDecodeOverrun;
    }
    if (Wire->DwordArrayCount != 0) {
        const unsigned int ArrayBytes = Wire->DwordArrayCount * 4u;  // SHL EAX,2
        Wire->DwordArray = static_cast<unsigned int*>(
            ::operator new(ArrayBytes, std::nothrow));               // , untested
        if (!Stream.GetBytes(Wire->DwordArray, ArrayBytes)) {
            return FRpcMarshalStream::kDecodeOverrun;
        }
    }

    // the routine again, the same no-op.
    *OutLength = Stream.Length();
    // The byte-statistics tail baseline runs on the SUCCESS PATH ONLY -- its error
    // exits jump straight past it. See FPacketWireStats.
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}


// ===========================================================================
// ch1 0x12 FLPACKET_COMMON_PLAYER_TRADE -- the branch (+0x04, 481 instructions) and
// (+0x08, 483). One packet type carrying eight logical messages, picked by
// the sub-type byte the port already models as FPayloadCommonPlayerTrade::SubType.

// OWNERSHIP, on the bytes. Both addresses appear exactly once, at module storage and
// , the second and third words of the routine = { the routine,
// , the branch, IServerImpl::GetServerStats }. The table's only reference
// is 06d9bae4 `MOV dword ptr [EAX],.rdata:the routine` inside the constructor
// , and the next instruction, 06d9baea, writes 0x10012 to packet+0x0c --
// channel 1, opcode 0x12, registry size 0x44, payload 0x24.

// THE SWITCH IS A JUMP TABLE, NOT A BITMASK. `EAX = SubType ; DEC EAX ; CMP EAX,0x7f ;
// JA default ; MOVZX ECX,byte [EAX + 0x06da73b0] ; JMP [ECX*4 + 0x06da7390]`
// (06da70b5..06da70c9; the decoder's copy is at 06da74b0..06da74c4 with its own tables
// at 06da7818 / 06da77f8). The 0x80-byte index table maps only the eight powers of two
// to a case and everything else, including any COMBINATION of bits, to the default --
// which writes nothing beyond the sub-type byte. So a packet with two sub-type bits set
// travels as a bare header, not as two messages.

// The two index tables agree value for value (0x00->0, 0x01->1, 0x03->2, 0x07->3,
// 0x0f->4, and every other byte 7) but differ on the last three: the encoder's table has
// 0x1f->5, 0x3f->6, 0x7f->0 while the decoder's has 0x1f->0, 0x3f->5, 0x7f->6. That is
// only code sharing -- the compiler folded sub-type 0x80 onto sub-type 1's body in the
// encoder and sub-type 0x20 onto it in the decoder -- and the three bodies are the same
// three instructions either way. Not a divergence.

// THE MP FLAG IS READ THE OPPOSITE WAY ROUND IN THE TWO DIRECTIONS. This is the one
// thing in the pair that reads as a transcription slip and is not:
//   encoder 06da70ad  TEST AL,AL ; MOV DL,1 ; JNZ +2 ; MOV DL,BL   -> DL =  IsMPServer()
//   decoder 06da74a8  TEST AL,AL ; MOV DL,1 ; JZ  +2 ; MOV DL,BL   -> DL = !IsMPServer()
// One byte apart -- 75 against 74 -- and both bodies then do `TEST DL,DL ; JZ <past the
// reserve>`. So the trailing dword at payload+0x08 is accounted for in the cursor only
// when the ENCODING end is the MP server and only when the DECODING end is not. Between
// a server and a client those two conditions are the same condition, which is what makes
// the pair self-consistent; it is not self-consistent with itself. The dword is written
// by Encode and normally read by Decode (except Decode subtype 0x40, which writes
// the existing packet value into the input). Only the cursor bump, i.e. the bounds check and the reported
// length, is conditional. Transcribed exactly, with the two halves of a field access
// split apart (FRpcMarshalStream::Reserve / PutDwordUncounted / GetDwordUncounted),
// because there is no way to write this with a single Put/Get and stay honest.

// ONE MORE BASELINE SLIP, KEPT: the decoder's sub-type 0x40 bounds check stores the
// ENCODER's error code -- 06da7541 `MOV dword ptr [ESI+0xc],0x16` and 06da7552
// `MOV EAX,0x16` -- where all twelve of its other checks store 0x17. It is the only
// 0x16 in the decoder.

// FIELD WIDTHS, each read off the instruction rather than off the struct:
//   payload+0x00 SubType       one BYTE out (06da709d), read back ZERO-extended
//                              (06da7493 `XOR EDX,EDX ; MOV DL,CL`).
//   payload+0x04 Field_0x04    one BYTE out (06da7162), read back SIGN-extended
//                              (06da74f0 `MOVSX EDX,CL`). Sub-type 2 only, and sub-type
//                              2 sends nothing else -- not even payload+0x08.
//   payload+0x08 Field_0x08    dword, every sub-type except 2.
//   payload+0x0c Field_0x0c    dword, sub-type 4 only (06da719a / 06da7591).
//   payload+0x10 Field_0x10    one BYTE out (06da70fc), read back NORMALISED to 0/1
//                              (06da7522 `TEST CL,CL ; SETNZ AL`). Sub-type 0x40 only.
//   payload+0x14 EquipArchId   dword, sub-types 8 and 0x10.
//   payload+0x18 EquipCount    dword, sub-types 8 and 0x10.
//   payload+0x1c EquipStatus   FLOAT out (06da7251 `FLD float ptr [EDI+0x3c] ; FSTP
//                              float ptr [EAX]`), a plain dword move back (06da766e).
//                              Sub-type 8 ONLY -- sub-type 0x10 skips it, which is why
//                              its wire record is four bytes shorter.
//   payload+0x20 EquipId       WORD, sub-types 8 and 0x10.
// No quantizer appears anywhere in either body.
// ===========================================================================

// COMMON.DLL::IsMPServer, which baseline imports by name and calls at 06da70a7 and
// (`ff15 0431db06` -> the IAT slot at module storage).

// It is already declared for this translation unit -- common_goods.h above includes
// common_net.h, which is where COMMON_API bool __cdecl IsMPServer() lives, and that
// same chain is what makes the symbol exported from our Common.dll at all (the
// definition in src/Common/common_misc.cpp carries no macro of its own and relies on
// the declaration common_goods.h drags in). Restated here only because a reader of this
// codec should not have to trace four headers to find out where the call comes from;
// the signature is common_net.h's, verbatim, so the redeclaration is a no-op.
COMMON_API bool __cdecl IsMPServer();

static const unsigned int kPlayerTradeSubAccept = 0x01;
static const unsigned int kPlayerTradeSubCash = 0x02;
static const unsigned int kPlayerTradeSubOffer = 0x04;
static const unsigned int kPlayerTradeSubAddEquip = 0x08;
static const unsigned int kPlayerTradeSubRemoveEquip = 0x10;
static const unsigned int kPlayerTradeSubClear = 0x20;
static const unsigned int kPlayerTradeSubFlag = 0x40;
static const unsigned int kPlayerTradeSubEnd = 0x80;

// PLAYER_TRADE, packet vtable +0x04.
unsigned int RpcEncodeCommonPlayerTrade(FPacketBase& Packet, unsigned char** BufferHolder,
                                        unsigned int Capacity, unsigned int* OutLength) {
    const FPayloadCommonPlayerTrade* Wire =
        reinterpret_cast<const FPayloadCommonPlayerTrade*>(Packet.Payload());

    // Cursor = 0, Limit = Capacity, Ptr = *BufferHolder, Error = 0.
    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    // the routine -- the two routing bytes, cursor := 2.
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));
    // the sub-type goes out as ONE byte.
    if (!Stream.PutByte((unsigned char)(Wire->SubType & 0xffu))) {
        return FRpcMarshalStream::kEncodeOverflow;
    }
    // `MOV byte ptr [ESP+0x18],BL` clobbers the low byte of the incoming
    // capacity's own stack slot with zero. It is a dead store -- the capacity was read
    // into the stream at 06da7041 and is never read from the stack again -- so there is
    // nothing here to reproduce.
    const bool bAccountTrailer = IsMPServer();

    // The trailing dword, shared by every sub-type except 2. baseline reaches it through
    // three different labels (06da7310, 06da731a and the fall-through at 06da711d) that
    // all do the same two things.
    bool bNeedTrailer = true;

    switch (Wire->SubType) {
    case kPlayerTradeSubFlag:                        // 0x40, caseD_40 at 06da70d0
        if (!Stream.PutByte(Wire->Field_0x10)) {
            return FRpcMarshalStream::kEncodeOverflow;
        }
        break;
    case kPlayerTradeSubCash:                        // 0x02, caseD_2 at 06da7134
        // takes the LOW BYTE of payload+0x04 and 06da7165 jumps straight to the
        // pointer update, past every trailing-dword label. Sub-type 2 sends no more.
        if (!Stream.PutByte((unsigned char)(Wire->Field_0x04 & 0xffu))) {
            return FRpcMarshalStream::kEncodeOverflow;
        }
        bNeedTrailer = false;
        break;
    case kPlayerTradeSubOffer:                       // 0x04, caseD_4 at 06da716a
        if (!Stream.PutDword(Wire->Field_0x0c)) {
            return FRpcMarshalStream::kEncodeOverflow;
        }
        break;
    case kPlayerTradeSubAddEquip:                    // 0x08, caseD_8 at 06da71d4
        if (!Stream.PutDword(Wire->EquipArchId) ||
            !Stream.PutDword(Wire->EquipCount) ||
            !Stream.PutFloat(Wire->EquipStatus) ||   // FLD/FSTP
            !Stream.PutWord(Wire->EquipId)) {
            return FRpcMarshalStream::kEncodeOverflow;
        }
        break;
    case kPlayerTradeSubRemoveEquip:                 // 0x10, caseD_10 at 06da72ac
        // No status float on this branch -- 06da7303 goes straight from EquipCount to
        // the shared word writer at 06da7259.
        if (!Stream.PutDword(Wire->EquipArchId) ||
            !Stream.PutDword(Wire->EquipCount) ||
            !Stream.PutWord(Wire->EquipId)) {
            return FRpcMarshalStream::kEncodeOverflow;
        }
        break;
    case kPlayerTradeSubAccept:                      // 0x01 / 0x80, caseD_1 at 06da71a6
    case kPlayerTradeSubEnd:
    case kPlayerTradeSubClear:                       // 0x20, caseD_20 at 06da7106
        // These three ARE the unconditional reserve: their bodies are the four
        // instructions that bump the cursor by 4, with no IsMPServer test in front.
        Stream.Reserve(4u, FRpcMarshalStream::kEncodeOverflow);
        if (Stream.Failed()) {
            return FRpcMarshalStream::kEncodeOverflow;
        }
        if (!Stream.PutDwordUncounted(Wire->Field_0x08)) {
            return FRpcMarshalStream::kEncodeOverflow;
        }
        bNeedTrailer = false;
        break;
    default:
        // caseD_3: everything that is not one of the eight, including any combination
        // of bits, falls straight through to the trailer hook.
        bNeedTrailer = false;
        break;
    }

    if (bNeedTrailer) {
        // / 06da71a4 / 06da7282 -- `TEST DL,DL ; JZ <past the reserve>`.
        if (bAccountTrailer) {
            Stream.Reserve(4u, FRpcMarshalStream::kEncodeOverflow);
        }
        if (Stream.Failed()) {
            return FRpcMarshalStream::kEncodeOverflow;
        }
        if (!Stream.PutDwordUncounted(Wire->Field_0x08)) {
            return FRpcMarshalStream::kEncodeOverflow;
        }
    }

    // the routine -- the no-op trailer hook.
    *OutLength = Stream.Length();
    // The byte-statistics tail baseline runs on the SUCCESS PATH ONLY -- its error
    // exits jump straight past it. See FPacketWireStats.
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    // TODO, shared with every other codec in this file: 06da7354 also updates the
    // per-type statistics record through the routine.
    return 0;
}

// PLAYER_TRADE, packet vtable +0x08.
unsigned int RpcDecodeCommonPlayerTrade(FPacketBase& Packet, const unsigned char* Buffer,
                                        unsigned int Size, unsigned int* OutLength) {
    FPayloadCommonPlayerTrade* Wire =
        reinterpret_cast<FPayloadCommonPlayerTrade*>(Packet.Payload());

    // Limit = Size, Ptr = Buffer taken WITHOUT a dereference.
    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    // the routine -- the no-op header hook.

    unsigned char SubTypeByte = 0;
    if (!Stream.GetByte(SubTypeByte)) {
        return FRpcMarshalStream::kDecodeOverrun;
    }
    // `XOR EDX,EDX ; MOV DL,CL` -- zero-extended, unlike the sub-type 2 field
    // eight instructions later.
    Wire->SubType = (unsigned int)SubTypeByte;
    // clobbers the low byte of the incoming buffer pointer's stack slot; dead,
    // for the same reason as the encoder's.
    const bool bAccountTrailer = !IsMPServer();      // , INVERTED

    bool bNeedTrailer = true;

    switch (Wire->SubType) {
    case kPlayerTradeSubCash: {                      // 0x02, caseD_2 at 06da74cb
        unsigned char Value = 0;
        if (!Stream.GetByte(Value)) {
            return FRpcMarshalStream::kDecodeOverrun;
        }
        // MOVSX -- signed, where the sub-type byte above was not.
        Wire->Field_0x04 = (unsigned int)(int)(signed char)Value;
        bNeedTrailer = false;
        break;
    }
    case kPlayerTradeSubFlag: {                      // 0x40, caseD_40 at 06da74fc
        unsigned char Value = 0;
        if (!Stream.GetByte(Value)) {
            return FRpcMarshalStream::kDecodeOverrun;
        }
        // `TEST CL,CL ; SETNZ AL` -- any non-zero byte comes back as 1.
        Wire->Field_0x10 = (unsigned char)(Value != 0 ? 1 : 0);
        // the ONE place in this decoder that uses the encoder's
        // error code. Reproduced, not corrected.
        if (bAccountTrailer) {
            Stream.Reserve(4u, FRpcMarshalStream::kEncodeOverflow);
        }
        if (Stream.Failed()) {
            return FRpcMarshalStream::kEncodeOverflow;
        }
        // write the EXISTING packet target into the input.
        // The pointer advances four bytes at 06da75c3 even when MP omitted Reserve.
        if (!Stream.PutDwordUncounted(Wire->Field_0x08)) {
            return FRpcMarshalStream::kEncodeOverflow;
        }
        bNeedTrailer = false;
        break;
    }
    case kPlayerTradeSubOffer:                       // 0x04, caseD_4 at 06da7565
        if (!Stream.GetDword(Wire->Field_0x0c)) {
            return FRpcMarshalStream::kDecodeOverrun;
        }
        break;
    case kPlayerTradeSubAddEquip: {                  // 0x08, caseD_8 at 06da75ee
        unsigned int Status = 0;
        if (!Stream.GetDword(Wire->EquipArchId) ||
            !Stream.GetDword(Wire->EquipCount) ||
            // `MOV EBX,[EAX] ; MOV [EDI+0x3c],EBX` -- a dword move, not a
            // conversion, so the float's bits come back untouched.
            !Stream.GetDword(Status) ||
            !Stream.GetWord(Wire->EquipId)) {
            return FRpcMarshalStream::kDecodeOverrun;
        }
        memcpy(&Wire->EquipStatus, &Status, sizeof(float));
        break;
    }
    case kPlayerTradeSubRemoveEquip:                 // 0x10, caseD_10 at 06da76c9
        if (!Stream.GetDword(Wire->EquipArchId) ||
            !Stream.GetDword(Wire->EquipCount) ||
            !Stream.GetWord(Wire->EquipId)) {
            return FRpcMarshalStream::kDecodeOverrun;
        }
        break;
    case kPlayerTradeSubAccept:                      // 0x01 / 0x20, caseD_1 at 06da777a
    case kPlayerTradeSubClear:
    case kPlayerTradeSubEnd:                         // 0x80, caseD_80 at 06da7599
        Stream.Reserve(4u, FRpcMarshalStream::kDecodeOverrun);
        if (Stream.Failed()) {
            return Stream.GetError();
        }
        if (!Stream.GetDwordUncounted(Wire->Field_0x08)) {
            return Stream.GetError();
        }
        bNeedTrailer = false;
        break;
    default:                                         // caseD_3 at 06da75c9
        bNeedTrailer = false;
        break;
    }

    if (bNeedTrailer) {
        // / 06da76a7 / 06da7758, and the branch behind all three.
        if (bAccountTrailer) {
            Stream.Reserve(4u, FRpcMarshalStream::kDecodeOverrun);
        }
        if (Stream.Failed()) {
            return Stream.GetError();
        }
        if (!Stream.GetDwordUncounted(Wire->Field_0x08)) {
            return Stream.GetError();
        }
    }

    // the routine again.
    *OutLength = Stream.Length();
    // The byte-statistics tail baseline runs on the SUCCESS PATH ONLY -- its error
    // exits jump straight past it. See FPacketWireStats.
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// , the PLAYER_TRADE constructor. baseline zeroes packet+0x20..+0x3c with
// two exceptions: 06d9bafa writes 0xffffffff to packet+0x34 -- payload+0x14,
// EquipArchId, so the default archetype is "none", not 0 -- and 06d9bb0a writes
// payload+0x10 as a BYTE with CL, which is zero at that point (06d9bac7 XOR ECX,ECX),
// so it is a zero store and not a default. The 0xffffffff is now a store in
// FPacket_FLPACKET_COMMON_PLAYER_TRADE's constructor in
// rpc_packet_registry.cpp; this type no longer needs a factory of its own.

// ===========================================================================
// ch2 0x05 FLPACKET_SERVER_DAMAGEOBJECT -- the branch / the branch

// Constructor the routine:
// MOV dword ptr [ESI],.rdata:the routine
// MOV dword ptr [ESI + 0xc],0x20005
//   vtable 06db3cb0 = { dtor the routine, Encode the branch, Decode the branch,
//                       IServerImpl::GetServerStats }
// Each slot's XREF header names exactly one .rdata address, 06db3cb4 and 06db3cb8; the
// vtable's own second reference, 06d90235, is inside the destructor. One type, one pair.

// THE CONSTRUCTOR CONFIRMS THE PAYLOAD rpc_payloads.h ALREADY HAD, list and all:
// packet+0x1c = 0                 (the marshal error, not payload)
// MOV byte ptr [ESI+0x24],AL      payload+0x04 -- ONE BYTE, the VC6
//                                             std::allocator copied out of an
//                                             uninitialised stack temporary. Not a field.
// operator new(0x14) -> self-linked dummy node -> packet+0x28 (payload+0x08)
// packet+0x2c = 0                 payload+0x0c  the list's Size
// MOV byte ptr [ESI+0x30],BL      payload+0x10  a real byte field
// /32/35 packet+0x34/+0x38/+0x3c = 0  payload+0x14/+0x18/+0x1c
// i.e. a DamageList in place at payload+0x04 -- which is what FPayloadServerDamageObject
// holds now: Common.dll's own DamageList, so the fields below are its named members
// (+0x0c bDestroyed, +0x10 Cause, +0x14 InflictorId, +0x18 InflictorOwner).

// Wire layout, and THE ORDER IS NOT THE STRUCT ORDER -- the list's count travels FOURTH,
// after three fields that sit BEHIND it in memory:
//     dword  payload+0x00                       06d904f1 / 06d9072b
//     byte   payload+0x10  Damage.Field_0x0c    06d9051e / 06d90751
//     dword  payload+0x14  Damage.Field_0x10    06d9054b / 06d90777
//     dword  payload+0x18  Damage.Field_0x14    06d9057a / 06d9079f
//     word   payload+0x0c  Damage.Entries.Size  06d905ab / 06d907c7
//     per entry:
//       word   Value.Field_0x00 (low half)      06d905e8 / 06d9084b
//       byte   Value.Field_0x08 (low byte)      06d9060c / 06d90875
//       float  Value.Field_0x04                 06d90632 / 06d9089c

// PAYLOAD+0x1C NEVER TRAVELS. Neither slot touches packet+0x3c. It is a field the
// constructor zeroes and the wire ignores, not an omission here.

// THE PER-ENTRY ORDER IS A TRAP THE LISTING SETTLES AND THE ANALYSIS TOOL WOULD NOT. The
// encoder LOADS all three fields before it writes any of them -- 06d905c4
// `FLD float ptr [EAX + 0xc]`, 06d905c7 `MOV BP,word ptr [EAX + 0x8]`, 06d905cb
// `MOV DL,byte ptr [EAX + 0x10]` -- so the float is read FIRST and written LAST. Reading
// the loads as the wire order would have put the float in front of the word and the
// byte. The proof that the float really is still on the x87 stack across the two bounds
// checks is the error exit itself: the branch begins `POP EDI ; FSTP ST0`, a pop of a
// register the codec does not need, to unload the float the checks jumped over.

// The decoder reads them back word, byte, float (06d9084d, 06d90877, 06d9089e) into a
// stack temporary and only then allocates the node -- the opposite of the equipment
// decoder, which allocates first and leaks the record on a short buffer. This one leaks
// nothing.

// THE PADDING HALF OF THE FIRST DWORD. 06d90810..06d90831 seed
// that temporary with COMMON.DLL::SUBOBJ_ID_NONE, 0xbf800000 (-1.0f) and 0 -- and the
// first of those is a WORD store into a DWORD slot (06d9081d `MOV word ptr [ESP+0x18],CX`),
// as is the decoded value that replaces it (06d908a0). The upper half of that dword is
// never written, and 06d908f0 copies the whole dword into the node. baseline therefore
// stores stack residue in the top half of DamageEntry's first dword on every element.
// Those two bytes are padding behind the ushort SubObjId of common_misc.h's DamageEntry;
// the port's DamageEntry is built by its own ctor and nothing reads that half.

// THE DECODER APPENDS. It never clears the list the packet already holds, and the count
// it just read is used only as the loop bound (06d90909 `CMP EAX,ECX` / 06d90912 `JC`,
// unsigned) -- the size at payload+0x0c is incremented per element (06d908fc/06d9090b),
// not assigned.
// ===========================================================================

unsigned int RpcEncodeServerDamageObject(FPacketBase& Packet, unsigned char** BufferHolder,
                                         unsigned int Capacity, unsigned int* OutLength) {
    const FPayloadServerDamageObject* Wire =
        reinterpret_cast<const FPayloadServerDamageObject*>(Packet.Payload());
    const DamageList& Damage = Wire->Damage;

    // /06d90496: Limit = arg2, Ptr = *arg1.
    FRpcMarshalStream Stream(Packet, *BufferHolder, Capacity);
    Stream.PutHeaderUnchecked(RpcPacketHeaderWord(Packet.PacketId));   // the routine
    if (!Stream.PutDword(Wire->Field_0x00)) {
        return FRpcMarshalStream::kEncodeOverflow;
    }
    // DamageList+0x0c, moved as a byte.
    if (!Stream.PutByte(Damage.bDestroyed ? 1u : 0u)) {
        return FRpcMarshalStream::kEncodeOverflow;
    }
    if (!Stream.PutDword(static_cast<unsigned int>(Damage.Cause))) {
        return FRpcMarshalStream::kEncodeOverflow;
    }
    if (!Stream.PutDword(Damage.InflictorId)) {
        return FRpcMarshalStream::kEncodeOverflow;
    }
    // `MOV CX,word ptr [EBX+0x2c]` -- only the LOW WORD of the list's _Size.
    const unsigned int Count = static_cast<unsigned int>(Damage.Entries.size());
    if (!Stream.PutWord((unsigned short)(Count & 0xffffu))) {
        return FRpcMarshalStream::kEncodeOverflow;
    }
    // /06d905bc: the list the packet ctor built, walked untested; an empty list
    // skips the loop.
    for (std::list<DamageEntry>::const_iterator It = Damage.Entries.begin();
         It != Damage.Entries.end(); ++It) {                       // /06d9063b
        if (!Stream.PutWord(It->SubObjId)) {
            return FRpcMarshalStream::kEncodeOverflow;         // , the branch
        }
        // `MOV DL,byte ptr [EAX + 0x10]` -- the low byte of the fate.
        if (!Stream.PutByte((unsigned char)(static_cast<unsigned int>(It->Fate) & 0xffu))) {
            return FRpcMarshalStream::kEncodeOverflow;
        }
        // FSTP -- the float loaded back at 06d905c4, four bytes either way.
        if (!Stream.PutFloat(It->HitPts)) {
            return FRpcMarshalStream::kEncodeOverflow;
        }
    }
    // the routine -- nothing more to emit.
    *OutLength = Stream.Length();
    RpcRecordEncodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

unsigned int RpcDecodeServerDamageObject(FPacketBase& Packet, const unsigned char* Buffer,
                                         unsigned int Size, unsigned int* OutLength) {
    FPayloadServerDamageObject* Wire =
        reinterpret_cast<FPayloadServerDamageObject*>(Packet.Payload());
    DamageList& Damage = Wire->Damage;

    // /06d906dd: Limit = arg2, Ptr = arg1 directly.
    FRpcMarshalStream Stream(Packet, const_cast<unsigned char*>(Buffer), Size);
    // the routine -- no header consumed.

    if (!Stream.GetDword(Wire->Field_0x00)) {
        return Stream.GetError();
    }
    unsigned char Destroyed = 0;
    if (!Stream.GetByte(Destroyed)) {
        return Stream.GetError();
    }
    Damage.bDestroyed = (Destroyed != 0);
    unsigned int Cause = 0;
    if (!Stream.GetDword(Cause)) {
        return Stream.GetError();
    }
    Damage.Cause = static_cast<DamageCause>(Cause);
    if (!Stream.GetDword(Damage.InflictorId)) {
        return Stream.GetError();
    }
    unsigned short Count = 0;
    if (!Stream.GetWord(Count)) {
        return Stream.GetError();
    }
    // /06d907e4 `TEST EAX,EAX ; JA` on the zero-extended word.
    for (unsigned int Index = 0; Index < (unsigned int)Count; ++Index) {
        // seed the stack temporary with SUBOBJ_ID_NONE, -1.0f and 0;
        // all three are overwritten below before the node is made.
        DamageEntry Entry;
        if (!Stream.GetWord(Entry.SubObjId)) {
            return Stream.GetError();
        }
        unsigned char Fate = 0;
        if (!Stream.GetByte(Fate)) {
            return Stream.GetError();
        }
        Entry.Fate = static_cast<DamageEntry::SubObjFate>(Fate);
        if (!Stream.GetFloat(Entry.HitPts)) {                      // FLD/FSTP
            return Stream.GetError();
        }
        // the node is allocated and spliced only AFTER all three
        // fields are safely in hand, so a short buffer leaks nothing here. The list is
        // APPENDED to, never cleared, and its size is incremented per element
        // (06d908fc/06d9090b) -- which is also the port's DamageList::Count.
        Damage.Entries.push_back(Entry);
        ++Damage.Count;
    }
    // the routine -- nothing to consume.
    *OutLength = Stream.Length();
    RpcRecordDecodedPacket(Packet.PacketId, Stream.Length());
    return 0;
}

// ===========================================================================
// The flattened FmtStr (ch1 0x1b, ch2 0x29, 0x34, 0x35, 0x45)
// ===========================================================================

unsigned int RpcFlattenedFmtStrSize(const FmtStr& Source) {
    unsigned int Total = 6;
    for (const FmtStr::Val* Node = Source.Head; Node != nullptr; Node = Node->Next) {
        Total += Node->get_flattened_size();
    }
    return Total;
}

// Every baseline site stores the size, calls a bare operator new(size) and hands the
// result straight to FmtStr::flatten (06d88de7, 06d88e26, 06d89339, 06d89663, 06d8ae14,
// , 06d8b0a3, 06d8b1a3, 06d8b702 ...) -- no test between the two.
void* RpcFlattenFmtStr(const FmtStr& Source, unsigned int& OutSize) {
    const unsigned int Size = RpcFlattenedFmtStrSize(Source);
    OutSize = Size;
    void* Block = ::operator new(Size, std::nothrow);
    Source.flatten(Block, Size);
    return Block;
}

__declspec(dllexport) void RpcUnflattenFmtStr(FmtStr& Dest, void* Buffer, unsigned int Size) {
    Dest.unflatten(Buffer, Size);
}


// The two CHARACTERINFO list destructors (the routine / the routine) are
// ~MetaList / ~OwnerList of OwnerList<CCharacterSet::CharacterInfo>
// (src/include/common_metalist.h + server_characterset.h).
