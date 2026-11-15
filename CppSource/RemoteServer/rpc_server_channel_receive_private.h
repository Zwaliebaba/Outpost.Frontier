// OutpostFrontier is a reimplementation of the original 2003 game client and engine,
// faithful to the behaviour of the original game.
// This file was reconstructed by reverse engineering the original game's version 1.1 binaries,
// developed by Digital Anvil.
// Author: Giperion.

#pragma once

class FPacketBase;
class IClient;

// Internal channel-2 receiver. The caller retains packet ownership and releases it.
void RpcReceiveServerChannel(unsigned int ClientId, FPacketBase* Packet, IClient* Client);
