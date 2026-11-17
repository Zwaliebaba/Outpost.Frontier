// Oracle harness, work package P1.9 (net): RemoteCommon/remote_packet_codecs.cpp
// compiled into the harness as RemoteClient.dll compiles it. Oracle.vcxproj
// gives this file RemoteClient's preprocessor definitions. The source is
// included rather than listed so that its file-local quantisers and field
// helpers (anonymous namespace and static functions) can be called; nothing
// in it is changed. See oracle_net_codecs.h.

#if !defined(REMOTECLIENT_EXPORTS)
#error "Oracle.vcxproj compiles this file with RemoteClient.dll's definitions"
#endif

#include "../RemoteCommon/remote_packet_codecs.cpp"

#include "oracle_net_context.h"

#include <cstring>

namespace oracle_net
{
    WireContext* BeginWrite(unsigned char* Buffer, unsigned int Limit)
    {
        return new WireContext(Buffer, Limit);
    }

    WireContext* BeginRead(const unsigned char* Buffer, unsigned int Size)
    {
        return new WireContext(const_cast<unsigned char*>(Buffer), Size);
    }

    void End(WireContext* Context)
    {
        delete Context;
    }

    unsigned int Position(const WireContext* Context)
    {
        return Context->Packet.MarshalPtr - reinterpret_cast<unsigned int>(Context->Buffer);
    }

    unsigned int Put(WireContext* Context, const void* Data, unsigned int Width)
    {
        return Context->Stream.Put(Data, Width) ? 0u : Context->Packet.MarshalError;
    }

    unsigned int PutFloat(WireContext* Context, unsigned int Bits)
    {
        float Value;
        std::memcpy(&Value, &Bits, 4u);
        return Context->Stream.PutFloat(Value) ? 0u : Context->Packet.MarshalError;
    }

    unsigned int PutBlock(WireContext* Context, const void* Data, unsigned int Width)
    {
        return Context->Stream.PutBlock(Data, Width) ? 0u : Context->Packet.MarshalError;
    }

    unsigned int PutVector(WireContext* Context, const unsigned int Bits[3])
    {
        Vector Value;
        std::memcpy(&Value.x, &Bits[0], 4u);
        std::memcpy(&Value.y, &Bits[1], 4u);
        std::memcpy(&Value.z, &Bits[2], 4u);
        return RpcWirePutVector(Context->Stream, Value) ? 0u : Context->Packet.MarshalError;
    }

    unsigned int PutWideString(WireContext* Context, const wchar_t* Text)
    {
        return RpcEncodeWideString(Context->Packet, Text);
    }

    unsigned int PutFireComponent(WireContext* Context, unsigned int Bits, unsigned char Flags)
    {
        float Value;
        std::memcpy(&Value, &Bits, 4u);
        return RpcFirePutComponent(Context->Stream, Value, Flags, 0x01u, 0x02u)
                   ? 0u
                   : Context->Packet.MarshalError;
    }

    unsigned int Get(WireContext* Context, void* Data, unsigned int Width)
    {
        return Context->Stream.Get(Data, Width) ? 0u : Context->Packet.MarshalError;
    }

    unsigned int GetVector(WireContext* Context, unsigned int Bits[3])
    {
        Vector Value;
        if (!RpcWireGetVector(Context->Packet, Context->Stream, Value))
            return Context->Packet.MarshalError;
        std::memcpy(&Bits[0], &Value.x, 4u);
        std::memcpy(&Bits[1], &Value.y, 4u);
        std::memcpy(&Bits[2], &Value.z, 4u);
        return 0u;
    }

    unsigned int GetWideString(WireContext* Context, std::vector<unsigned short>& Units)
    {
        wchar_t* Text = nullptr;
        const unsigned int Result = RpcDecodeWideString(Context->Packet, &Text);
        Units.clear();
        if (Result == 0 && Text != nullptr)
        {
            for (const wchar_t* Unit = Text; *Unit != L'\0'; ++Unit)
                Units.push_back(static_cast<unsigned short>(*Unit));
        }
        ::operator delete(Text);
        return Result;
    }

    unsigned int GetOwnedBlock(WireContext* Context, std::vector<unsigned char>& Data)
    {
        unsigned int Size = 0;
        unsigned char* Block = nullptr;
        const unsigned int Result = RpcWireDecodeOwnedBlock(Context->Packet, Context->Stream, Size, Block);
        Data.clear();
        if (Result == 0 && Size != 0)
            Data.assign(Block, Block + Size);
        ::operator delete(Block);
        return Result;
    }

    unsigned int GetFireComponent(WireContext* Context, unsigned char Flags, unsigned int* Bits)
    {
        float Value = 0.0f;
        if (!RpcFireGetComponent(Context->Stream, Value, Flags, 0x01u, 0x02u, false, true))
            return Context->Packet.MarshalError;
        std::memcpy(Bits, &Value, 4u);
        return 0u;
    }

    unsigned char UpdateQuantize(unsigned int Bits)
    {
        float Value;
        std::memcpy(&Value, &Bits, 4u);
        return RpcUpdateQuantize(Value, nullptr, 0u);
    }

    unsigned int UpdateExpand(unsigned char Byte)
    {
        float Value = 0.0f;
        RpcUpdateExpand(static_cast<signed char>(Byte), Value);
        unsigned int Bits;
        std::memcpy(&Bits, &Value, 4u);
        return Bits;
    }

    unsigned char FirePickTier(unsigned int Bits, bool bMedium)
    {
        float Value;
        std::memcpy(&Value, &Bits, 4u);
        return RpcFirePickTier(Value, bMedium);
    }

    unsigned int FireQuantize(unsigned int Bits, bool bMedium)
    {
        float Value;
        std::memcpy(&Value, &Bits, 4u);
        return RpcFireQuantize(Value, bMedium);
    }

    unsigned int EncodeHeader(unsigned int PacketId, unsigned char* Out, unsigned int Capacity,
                              unsigned int* Length)
    {
        FPacketBase Packet(PacketId);
        unsigned char* Holder = Out;
        return Packet.Encode(&Holder, Capacity, Length);
    }

    unsigned int EncodeHullStatus(unsigned int Bits, unsigned char* Out, unsigned int Capacity,
                                  unsigned int* Length)
    {
        FRpcSetHullStatusPacket Packet;
        std::memcpy(&Packet.Value, &Bits, 4u);
        unsigned char* Holder = Out;
        return Packet.Encode(&Holder, Capacity, Length);
    }
}
