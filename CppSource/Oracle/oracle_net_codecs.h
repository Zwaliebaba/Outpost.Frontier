// Oracle harness, work package P1.9 (net): calls into the RemoteCommon codecs.
//
// oracle_net_packet_codecs.cpp and oracle_net_server_codecs.cpp compile
// RemoteCommon/remote_packet_codecs.cpp and remote_server_channel_codecs.cpp
// into the harness as RemoteClient.dll compiles them, so that their
// file-local quantisers and field helpers can be called. This header is
// their interface for oracle_net.cpp and needs none of the codec headers.
// Floats cross it as bit patterns: under /arch:IA32 a float passed or
// returned by value goes through the x87 stack, which quiets signalling NaNs.

#pragma once

#include <vector>

namespace oracle_net
{
    // A packet base and its FRpcWireStream over a caller's buffer.
    struct WireContext;

    WireContext* BeginWrite(unsigned char* Buffer, unsigned int Limit);
    WireContext* BeginRead(const unsigned char* Buffer, unsigned int Size);
    void End(WireContext* Context);
    // The stream's moving pointer minus the buffer start.
    unsigned int Position(const WireContext* Context);

    // Encode primitives. Each returns 0, or the code the C++ returns: the
    // sticky MarshalError (0x16) for FRpcWireStream, the helper's own
    // result for RpcEncodeWideString.
    unsigned int Put(WireContext* Context, const void* Data, unsigned int Width);    // FRpcWireStream::Put
    unsigned int PutFloat(WireContext* Context, unsigned int Bits);                  // FRpcWireStream::PutFloat
    unsigned int PutBlock(WireContext* Context, const void* Data, unsigned int Width);
    unsigned int PutVector(WireContext* Context, const unsigned int Bits[3]);        // RpcWirePutVector
    unsigned int PutWideString(WireContext* Context, const wchar_t* Text);           // RpcEncodeWideString
    unsigned int PutWideText(WireContext* Context, const wchar_t* Units, unsigned int Count); // RpcServerPutWideText
    unsigned int PutFireComponent(WireContext* Context, unsigned int Bits, unsigned char Flags); // RpcFirePutComponent, x axis

    // Decode primitives; 0 or the C++ code (0x17 for an overrun).
    unsigned int Get(WireContext* Context, void* Data, unsigned int Width);          // FRpcWireStream::Get
    unsigned int GetVector(WireContext* Context, unsigned int Bits[3]);              // RpcWireGetVector
    unsigned int GetWideString(WireContext* Context, std::vector<unsigned short>& Units); // RpcDecodeWideString
    unsigned int GetWideText(WireContext* Context, std::vector<unsigned short>& Units);   // RpcServerGetWideText
    unsigned int GetOwnedBlock(WireContext* Context, std::vector<unsigned char>& Data);   // RpcWireDecodeOwnedBlock
    unsigned int GetFireComponent(WireContext* Context, unsigned char Flags, unsigned int* Bits); // RpcFireGetComponent, x axis

    // Quantisers and expanders.
    unsigned char Quantize127(unsigned int Bits);           // RpcServerQuantize127
    unsigned char UpdateQuantize(unsigned int Bits);        // RpcUpdateQuantize
    unsigned char Quantize255(unsigned int Bits);           // RpcServerQuantize255
    unsigned char SendCommQuantize(unsigned int Bits);      // RpcServerSendCommQuantize
    unsigned char QuantizeHull(int HitPts);                 // RpcServerQuantizeHull, unknown archetype
    unsigned char FirePickTier(unsigned int Bits, bool bMedium); // RpcFirePickTier
    unsigned int FireQuantize(unsigned int Bits, bool bMedium);  // RpcFireQuantize
    unsigned int Expand127(unsigned char Byte);             // RpcServerExpand127
    unsigned int UpdateExpand(unsigned char Byte);          // RpcUpdateExpand
    unsigned int Expand255(unsigned char Byte);             // RpcServerExpand255
    unsigned int SendCommExpand(unsigned char Byte);        // RpcServerSendCommExpand
    int ExpandHull(unsigned char Byte);                     // RpcServerExpandHull, unknown archetype

    // Whole packets through their Encode slot.
    unsigned int EncodeHeader(unsigned int PacketId, unsigned char* Out, unsigned int Capacity,
                              unsigned int* Length);        // FPacketBase::Encode
    unsigned int EncodeHullStatus(unsigned int Bits, unsigned char* Out, unsigned int Capacity,
                                  unsigned int* Length);    // FLPACKET_SERVER_SETHULLSTATUS
    unsigned int EncodeCharSelectVerified(unsigned int RepInstance, unsigned long long TimeBits,
                                          unsigned char* Out, unsigned int Capacity,
                                          unsigned int* Length); // FLPACKET_SERVER_CHARSELECTVERIFIED
}
