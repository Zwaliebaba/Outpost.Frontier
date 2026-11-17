// Oracle harness, work package P1.9 (net): the WireContext of
// oracle_net_codecs.h. Included only after a RemoteCommon source, by
// oracle_net_packet_codecs.cpp and oracle_net_server_codecs.cpp.

#pragma once

#include "rpc_packet_codec.h"

#include "oracle_net_codecs.h"

struct oracle_net::WireContext
{
    // The packet ID only feeds the header and the wire statistics.
    WireContext(unsigned char* InBuffer, unsigned int Limit)
        : Packet(0x20049u), Stream(Packet, static_cast<void*>(InBuffer), Limit), Buffer(InBuffer)
    {
    }

    FPacketBase Packet;
    FRpcWireStream Stream;
    unsigned char* Buffer;

private:
    WireContext(const WireContext&);
    WireContext& operator=(const WireContext&);
};
