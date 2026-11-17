// Oracle harness, work package P2.2 (protocol): the synthetic value source and
// the mode switch the protocol suites share. The suites write their vectors to
// <output>\game-sim\protocol_*.jsonl; crates/game-sim/tests/oracle_protocol.rs
// renders the same lines from the same values (crates/game-sim/tests/support/).
//
// oracle_protocol.cpp: FmtStr, binary RDL and MarshalBuffer through Common.dll.
// oracle_protocol_packets.cpp: every packet of RemoteCommon through its Encode,
// compiled with RemoteClient.dll's definitions like oracle_net_*_codecs.cpp.

#pragma once

#include <cstring>

namespace oracle_protocol
{
    // Marsaglia's 32-bit xorshift (13, 17, 5), as support::Gen in the Rust test.
    // Every value is drawn in its own statement: C++ leaves the order of
    // function arguments unspecified.
    class Gen
    {
    public:
        explicit Gen(unsigned int Seed) : State(Seed == 0 ? 1u : Seed) {}

        // The generator of case Case of packet Id.
        static Gen ForCase(unsigned int Id, unsigned int Case)
        {
            return Gen((Id * 2654435761u) ^ ((Case + 1u) * 0x9e3779b9u));
        }

        unsigned int U32()
        {
            unsigned int X = State;
            X ^= X << 13;
            X ^= X >> 17;
            X ^= X << 5;
            State = X;
            return X;
        }

        unsigned char U8() { return (unsigned char)(U32() & 0xffu); }
        unsigned short U16() { return (unsigned short)(U32() & 0xffffu); }
        int I32() { return (int)U32(); }

        // A finite float: exponent bit 30 cleared when the exponent is all ones.
        float F32()
        {
            unsigned int Bits = U32();
            if ((Bits & 0x7f800000u) == 0x7f800000u)
                Bits &= 0xbfffffffu;
            float Value;
            std::memcpy(&Value, &Bits, 4u);
            return Value;
        }

        // (next % 257 - 128) / 128, exact.
        float Unit()
        {
            const int Step = (int)(U32() % 257u) - 128;
            return (float)Step * 0.0078125f;
        }

        // 0x21 + next % 0x5e: printable, never 0.
        unsigned char TextByte() { return (unsigned char)(0x21u + U32() % 0x5eu); }
        // 0x41 + next % 58: 'A' to 'z', never 0.
        unsigned short TextUnit() { return (unsigned short)(0x41u + U32() % 58u); }

    private:
        unsigned int State;
    };

    // The C++ globals the codecs read: SetIsMPServer(true) ("mps"), a
    // multiplayer client ("mpc"), single player ("sp").
    enum Mode
    {
        ModeServer,
        ModeClient,
        ModeSingle
    };

    void SetMode(Mode Which);
    const char* ModeName(Mode Which);
    // Back to Common's defaults: single player, not a server.
    void RestoreMode();
}

// oracle_protocol_packets.cpp: the packet suites, called by RunProtocolSuites.
void RunProtocolPacketSuites();

// oracle_protocol.cpp: every protocol suite; called at the end of RunNetSuites.
void RunProtocolSuites();
