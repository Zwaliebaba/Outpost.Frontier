// Oracle harness, work package P1.9 (net): RemoteCommon/remote_server_channel_codecs.cpp
// compiled into the harness as RemoteClient.dll compiles it. Oracle.vcxproj
// gives this file RemoteClient's preprocessor definitions. The source is
// included rather than listed so that its static quantisers and wide-text
// helpers can be called; nothing in it is changed. See oracle_net_codecs.h.

#if !defined(REMOTECLIENT_EXPORTS)
#error "Oracle.vcxproj compiles this file with RemoteClient.dll's definitions"
#endif

#include "../RemoteCommon/remote_server_channel_codecs.cpp"

#include "oracle_net_context.h"

#include <cstring>
#include <string>

namespace oracle_net
{
    namespace
    {
        // No archetype is loaded in the harness: Archetype::GetArchMaxHitPts
        // reports 1 for every ID (after an ArchDB miss dump).
        const unsigned int UnknownArchetype = 0;

        unsigned int BitsOf(const float& Value)
        {
            unsigned int Bits;
            std::memcpy(&Bits, &Value, 4u);
            return Bits;
        }
    }

    unsigned char Quantize127(unsigned int Bits)
    {
        float Value;
        std::memcpy(&Value, &Bits, 4u);
        return RpcServerQuantize127(&Value);
    }

    unsigned int Expand127(unsigned char Byte)
    {
        float Value = 0.0f;
        RpcServerExpand127(Byte, &Value);
        return BitsOf(Value);
    }

    unsigned char Quantize255(unsigned int Bits)
    {
        float Value;
        std::memcpy(&Value, &Bits, 4u);
        return RpcServerQuantize255(&Value);
    }

    unsigned int Expand255(unsigned char Byte)
    {
        float Value = 0.0f;
        RpcServerExpand255(Byte, &Value);
        return BitsOf(Value);
    }

    unsigned char SendCommQuantize(unsigned int Bits)
    {
        float Value;
        std::memcpy(&Value, &Bits, 4u);
        return RpcServerSendCommQuantize(&Value);
    }

    unsigned int SendCommExpand(unsigned char Byte)
    {
        float Value = 0.0f;
        RpcServerSendCommExpand(Byte, &Value);
        return BitsOf(Value);
    }

    unsigned char QuantizeHull(int HitPts)
    {
        return RpcServerQuantizeHull(HitPts, UnknownArchetype);
    }

    int ExpandHull(unsigned char Byte)
    {
        return RpcServerExpandHull(Byte, UnknownArchetype);
    }

    unsigned int PutWideText(WireContext* Context, const wchar_t* Units, unsigned int Count)
    {
        const std::wstring Text(Units, Count);
        return RpcServerPutWideText(Context->Stream, Text) ? 0u : Context->Packet.MarshalError;
    }

    unsigned int GetWideText(WireContext* Context, std::vector<unsigned short>& Units)
    {
        std::wstring Text;
        if (!RpcServerGetWideText(Context->Stream, Text))
            return Context->Packet.MarshalError;
        Units.clear();
        for (std::wstring::const_iterator It = Text.begin(); It != Text.end(); ++It)
            Units.push_back(static_cast<unsigned short>(*It));
        return 0u;
    }

    unsigned int EncodeCharSelectVerified(unsigned int RepInstance, unsigned long long TimeBits,
                                          unsigned char* Out, unsigned int Capacity,
                                          unsigned int* Length)
    {
        FRpcServerCharSelectVerifiedPacket Packet;
        Packet.RepInstance = RepInstance;
        std::memcpy(&Packet.Time, &TimeBits, 8u);
        unsigned char* Holder = Out;
        return Packet.Encode(&Holder, Capacity, Length);
    }
}
