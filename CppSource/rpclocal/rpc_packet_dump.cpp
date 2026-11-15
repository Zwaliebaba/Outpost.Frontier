// Author: Giperion.

// rpc_packet_dump.cpp -- packet vtable slot +0x0c, the per-type ostream dump.

// WHAT BASELINE HAS. Every one of the 162 packet vtables is four slots wide
// { scalar dtor, encode, decode, dump }. Reading slot 3 out of .rdata for all 162
// constructors at once gives exactly twelve types with a body of their own and 150
// sharing one address:

// addr .text:IServerImpl::GetServerStats   <- 150 of them

// That name is an ICF fold and says nothing: IServerImpl::GetServerStats is itself a
// `RET 0x4` body (06d8eae0), and so is the dump no-op, so the linker gave them one
// address. Treating the name as evidence that the dump slot "is" GetServerStats is the
// mangled-name-read-for-more-than-it-says mistake; what the bytes say is only that
// those 150 types dump nothing.

// The twelve that do, with the vtable address the constructor publishes:

//   ch2 0x01 CONNECTRESPONSE      the branch   vtable 06db4fec, slot module storage
//   ch2 0x02 LOGINRESPONSE        the branch   vtable 06db4ffc, slot module storage
//   ch2 0x03 CHARACTERINFO        the branch   vtable 06db3dc4, slot module storage
//   ch2 0x07 LAUNCH               the routine   vtable 06db50ec, slot module storage
//   ch2 0x08 CHARSELECTVERIFIED   the branch   vtable 06db517c, slot module storage
//   ch2 0x29 SETREPUTATION        the routine   vtable 06db3d50, slot module storage
//   ch2 0x3c REPAIROBJECT         the routine   vtable 06db524c, slot module storage
//   ch3 0x01 LOGIN                the branch   vtable 06db5ca4, slot module storage
//   ch3 0x04 REQUESTLAUNCH        the branch   vtable 06db5c14, slot module storage
//   ch3 0x06 SELECTCHARACTER      the branch   vtable 06db5c94, slot module storage
//   ch3 0x3a DESTROYCHAR          the branch   vtable 06db5c84, slot module storage
//   ch3 0x39 CREATENEWCHAR        the branch   vtable 06db5c74, slot module storage

// Eleven bodies for twelve types: the branch really is one address in two vtables,
// the same way the branch / the branch are for the codec slots. That was read out
// of .rdata, not inferred from the bodies looking alike.

// The vtable column is the address the constructor stores into packet+0x00 -- the LAST
// such store, not the first. THIRTEEN of the 162 constructors store two, an intermediate
// base's table and then the real one, and taking the first picks the wrong table for
// every one of them. Counted over all 162 ctors, not sampled; the list is CHARACTERINFO
// , CREATESHIP 06d9cc60, DAMAGEOBJECT 06d9c8d0, CREATELOOT 06d9d140,
// SETREPUTATION 06d9d290, SETGROUPFEELINGS 06d9c540, CREATECOUNTER 06d9cea0,
// SETMISSIONOBJECTIVES 06d9da40, BURNFUSE 06d9e5a0, CREATESOLAR 06d9e630,
// SETCOLLISIONGROUPS 06d9e8b0, MUNCOLLISION 06da0790 and REQCOLLISIONGROUPS 06da10b0.
// ALL THIRTEEN name the same intermediate table, .rdata:the routine -- checked on
// each of the thirteen, not sampled; e.g. 06d9c575 then 06d9c5a5 (SETGROUPFEELINGS) and
// then 06da07cc (MUNCOLLISION).
// Three of them are in this file's twelve, so this is not a footnote.
// carries a FOURTEENTH reference that is NOT a constructor: 06da1370, the
// base destructor, `MOV dword ptr [param_1],.rdata:the routine` then
// `JMP the routine`. A plain grep of the listing misses it -- that row prints the
// operand truncated as ".rdata:PTR_FUN_06..." -- and analysis's XREF[14] header is what
// catches it. See the long note in rpc_wire_generic.cpp.

// WHICH OSTREAM OVERLOAD EACH FIELD USES. baseline imports six of them from MSVCP60 and
// picks a different one per field; the IAT slot and the ordinal comment next to it in
// the listing name each one exactly:

// ordinal 462  ??6std@@YAAAV?$basic_ostream@DU?$char_traits@D@std@@@0@AAV10@E@Z
//                    std::operator<<(ostream&, unsigned char)
// ordinal 432  ??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QAEAAV01@M@Z
//                    basic_ostream<char>::operator<<(float)
// ordinal 433  ...@N@Z    operator<<(double)
// ordinal 428  ...@H@Z    operator<<(int)
// ordinal 439  ...@PBX@Z  operator<<(void const*)
// ordinal 464  ??6std@@YAAAV?$basic_ostream@DU?$char_traits@D@std@@@0@AAV10@PBD@Z
//                    std::operator<<(ostream&, char const*)
// ordinal 429  ...@I@Z    operator<<(unsigned int)

// The label a comment describes is the entry DIRECTLY BELOW it -- checked on the two
// unambiguous rows in the same block, `;1056 ?assign@...` above PTR_assign_06db3178 and
// `;564 ??Mstd@@...` above PTR_operator<_06db3174.

// THE ONE THAT MATTERS: the three wide strings are streamed through ordinal 439,
// operator<<(void const*), not through any text overload. 06daf7b1 and 06daff71 are the
// only two call sites of module storage in the module. VC6's ostream has no
// wchar_t const* overload, so `out << (wchar_t*)p` decays to the void const* one and
// baseline's debug dump prints the ADDRESS of the account / character / pilot name, never
// its text. Reproduced, because printing the text would be a behaviour baseline has not
// got.

// The trailing newline is .rdata:the field, the two bytes `0a 00` -- a one-character
// C string, pushed to the char const* overload like any other literal, not a std::endl.

// NO CALLER. Nothing in rpclocal.dll calls slot +0x0c; baseline's callers live in the
// network transport. The slot exists here for the same reason it exists there.

#include "rpc_containers.h"
#include "rpc_payloads.h"

#include <ostream>
#include <cstring>

// . Spelled as a named constant so the twelve bodies below read the way the
// listing does -- every one of them pushes this same pointer.
static const char* const kRpcDumpEol = "\n";

// ---------------------------------------------------------------------------
// ch2 0x01 FLPACKET_SERVER_CONNECTRESPONSE -- the branch.
// MOV EAX,dword ptr [ECX + 0x20]   the only field
// PUSH .rdata:s_Client_ID:_06db6263+1  "   Client ID: "
// CALL operator<<(unsigned int)
// ---------------------------------------------------------------------------
void RpcDumpServerConnectResponse(const FPacketBase& Packet, std::ostream& Out) {
    const unsigned int* Fields = Packet.PayloadDwords();
    Out << "   Client ID: ";
    Out << Fields[0];                                         // packet+0x20
    Out << kRpcDumpEol;
}

// ---------------------------------------------------------------------------
// ch2 0x02 FLPACKET_SERVER_LOGINRESPONSE -- the branch.
// XOR EAX,EAX ; 06da84a2 MOV AL,byte ptr [ECX + 0x20]
// CALL module storage, std::operator<<(ostream&, unsigned char)
// The field is read as ONE BYTE and streamed through the unsigned char overload, so it
// prints as a character, not as a number. That is baseline's choice, kept.
// ---------------------------------------------------------------------------
void RpcDumpServerLoginResponse(const FPacketBase& Packet, std::ostream& Out) {
    const unsigned char Result = Packet.Payload()[0];         // packet+0x20
    Out << "   Result   : ";
    Out << Result;
    Out << kRpcDumpEol;
}

// ---------------------------------------------------------------------------
// ch2 0x03 FLPACKET_SERVER_CHARACTERINFO -- the branch. The only guarded dump:
// MOV EAX,dword ptr [ECX + 0x2c]
// TEST EAX,EAX
// JZ the branch        <- writes nothing at all when the count is zero
// ---------------------------------------------------------------------------
void RpcDumpServerCharacterInfo(const FPacketBase& Packet, std::ostream& Out) {
    const unsigned int Count = Packet.PayloadDwords()[3];     // packet+0x2c
    if (Count == 0) {
        return;
    }
    Out << "   Num Characters : ";
    Out << Count;
    Out << kRpcDumpEol;
}

// ---------------------------------------------------------------------------
// ch2 0x07 FLPACKET_SERVER_LAUNCH -- the routine. Three floats, each on its own line:
// [EDI+0x2c]  "   X: "   06daaf05 operator<<(float)
// [EDI+0x30]  "   Y: "   06daaf27
// [EDI+0x34]  "   Z: "   06daaf49
// packet+0x2c..+0x37 is FPayloadServerLaunch::Position, the Vector at payload+0x0c.
// ---------------------------------------------------------------------------
void RpcDumpServerLaunch(const FPacketBase& Packet, std::ostream& Out) {
    const FPayloadServerLaunch* Wire =
        reinterpret_cast<const FPayloadServerLaunch*>(Packet.Payload());
    Out << "   X: ";
    Out << Wire->Position.x;
    Out << kRpcDumpEol;
    Out << "   Y: ";
    Out << Wire->Position.y;
    Out << kRpcDumpEol;
    Out << "   Z: ";
    Out << Wire->Position.z;
    Out << kRpcDumpEol;
}

// ---------------------------------------------------------------------------
// ch2 0x08 FLPACKET_SERVER_CHARSELECTVERIFIED -- the branch.

// One line, two values: `"   RepInstance: " << (unsigned)[+0x20] << " time: "
// << (double)[+0x28] << "\n"`.

// The second value is a DOUBLE and it is eight bytes wide. 06dac023 pushes [ECX+0x28]
// and 06dac020 pushes [ECX+0x2c]; 06dac05c then calls module storage, ordinal 433
// `...QAEAAV01@N@Z`, whose one stack argument is 8 bytes, so the callee consumes both
// pushes as a single double at packet+0x28..+0x2f.

// FPayloadServerCharSelectVerified declares that range as Field_0x08 and Field_0x0c,
// two dwords, because the sender and the receiver both move it as two dwords. The two
// readings are not in conflict -- this is the only place in the module that treats the
// pair as one scalar -- so the struct is left alone and the double is assembled here.
// ---------------------------------------------------------------------------
void RpcDumpServerCharSelectVerified(const FPacketBase& Packet, std::ostream& Out) {
    const FPayloadServerCharSelectVerified* Wire =
        reinterpret_cast<const FPayloadServerCharSelectVerified*>(Packet.Payload());
    double Time = 0.0;
    ::memcpy(&Time, &Wire->Field_0x08, sizeof(Time));         // / 06dac023
    Out << "   RepInstance: ";
    Out << Wire->Field_0x00;                                  // packet+0x20
    Out << " time: ";
    Out << Time;
    Out << kRpcDumpEol;
}

// ---------------------------------------------------------------------------
// ch2 0x29 FLPACKET_SERVER_SETREPUTATION -- the routine.
// [EDI+0x58]  "   Target     : "   06d92885 operator<<(unsigned int)
// [EDI+0x5c]  "   Affiliation: "   06d928a7
// packet+0x58 / +0x5c are payload+0x38 / +0x3c of FPayloadServerSetReputation.
// ---------------------------------------------------------------------------
void RpcDumpServerSetReputation(const FPacketBase& Packet, std::ostream& Out) {
    const FPayloadServerSetReputation* Wire =
        reinterpret_cast<const FPayloadServerSetReputation*>(Packet.Payload());
    Out << "   Target     : ";
    Out << Wire->VibeHandle;                                  // packet+0x58
    Out << kRpcDumpEol;
    Out << "   Affiliation: ";
    Out << Wire->Field_0x3c;                                  // packet+0x5c
    Out << kRpcDumpEol;
}

// ---------------------------------------------------------------------------
// ch2 0x3c FLPACKET_SERVER_REPAIROBJECT -- the routine. Three lines, and the third
// uses a DIFFERENT overload from the first two:
// [EDI+0x20]  "   Object ID  : "  06dac2d5 operator<<(unsigned int)  module storage
// [EDI+0x24]  "   Equip ID   : "  06dac2f7 operator<<(unsigned int)  module storage
// [EDI+0x28]  "   Points     : "  06dac319 operator<<(int)           module storage
// So the third field is SIGNED where the first two are unsigned. Reproduced.
// ---------------------------------------------------------------------------
void RpcDumpServerRepairObject(const FPacketBase& Packet, std::ostream& Out) {
    const unsigned int* Fields = Packet.PayloadDwords();
    Out << "   Object ID  : ";
    Out << Fields[0];                                         // packet+0x20
    Out << kRpcDumpEol;
    Out << "   Equip ID   : ";
    Out << Fields[1];                                         // packet+0x24
    Out << kRpcDumpEol;
    Out << "   Points     : ";
    Out << (int)Fields[2];                                    // packet+0x28, signed
    Out << kRpcDumpEol;
}

// ---------------------------------------------------------------------------
// ch3 0x01 FLPACKET_CLIENT_LOGIN -- the branch.
// ADD ECX,0x20        -> &AccountName, the wide string at payload+0x000
// CALL module storage, operator<<(void const*)
// ---------------------------------------------------------------------------
void RpcDumpClientLogin(const FPacketBase& Packet, std::ostream& Out) {
    const void* Account = static_cast<const void*>(Packet.Payload());
    Out << "   Account: ";
    Out << Account;                                           // the ADDRESS
    Out << kRpcDumpEol;
}

// ---------------------------------------------------------------------------
// ch3 0x04 FLPACKET_CLIENT_REQUESTLAUNCH -- the branch.
// ---------------------------------------------------------------------------
void RpcDumpClientRequestLaunch(const FPacketBase& Packet, std::ostream& Out) {
    const unsigned int* Fields = Packet.PayloadDwords();
    Out << "   Object ID: ";
    Out << Fields[0];                                         // packet+0x20
    Out << kRpcDumpEol;
}

// ---------------------------------------------------------------------------
// ch3 0x06 SELECTCHARACTER and ch3 0x3a DESTROYCHAR -- the branch, ONE body in two
// vtables (module storage and module storage hold the same address in slot +0x0c).
// ---------------------------------------------------------------------------
void RpcDumpWireCharacterIdDump(const FPacketBase& Packet, std::ostream& Out) {
    const void* Character = static_cast<const void*>(Packet.Payload());
    Out << "   Character: ";
    Out << Character;                                         // the ADDRESS
    Out << kRpcDumpEol;
}

// ---------------------------------------------------------------------------
// ch3 0x39 FLPACKET_CLIENT_CREATENEWCHAR -- the branch. Five values on ONE line, and
// only one newline, at the very end. The push order settles the field order:

// MOV EAX,dword ptr [ECX + 0xac]
// MOV EDX,dword ptr [ECX + 0xa8]
// PUSH .rdata:the field          -- the newline
// PUSH EAX                          -- +0xac
// MOV EAX,dword ptr [ECX + 0xa4]
// PUSH .rdata:s_Pilot:_06db66d8
// PUSH EDX                          -- +0xa8
// MOV EDX,dword ptr [ECX + 0xa0]
// PUSH .rdata:s_Pkg:_06db66cc
// PUSH EAX                          -- +0xa4
// MOV EAX,dword ptr [ESP + 0x20]    -- the stream
// PUSH .rdata:s_Base:_06db66c0
// PUSH EDX                          -- +0xa0
// PUSH .rdata:s_Fac:_06db66b4
// ADD ECX,0x20
// PUSH ECX                          -- &Name
// PUSH .rdata:s_Name:_06db66a7+1
// PUSH EAX                          -- the stream

// so the chain unwinds Name, Fac, Base, Pkg, Pilot. The name goes through
// operator<<(void const*) at 06daf7b1; the four dwords through operator<<(unsigned int)
// at 06daf7bf / 06daf7cd / 06daf7db / 06daf7e9.
// ---------------------------------------------------------------------------
void RpcDumpClientCreateNewChar(const FPacketBase& Packet, std::ostream& Out) {
    const FPayloadClientCreateNewChar* Wire =
        reinterpret_cast<const FPayloadClientCreateNewChar*>(Packet.Payload());
    Out << "   Name: ";
    Out << static_cast<const void*>(Wire->Name);              // the ADDRESS
    Out << "   Fac: ";
    Out << Wire->Field_0x80;                                  // packet+0xa0
    Out << "   Base: ";
    Out << Wire->Field_0x84;                                  // packet+0xa4
    Out << "   Pkg: ";
    Out << Wire->Field_0x88;                                  // packet+0xa8
    Out << "   Pilot: ";
    Out << Wire->Field_0x8c;                                  // packet+0xac
    Out << kRpcDumpEol;
}
