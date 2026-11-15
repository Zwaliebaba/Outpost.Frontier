// Author: Giperion.

#pragma once

class FPacketBase;
class IClient;

// Internal channel-2 receiver. The caller retains packet ownership and releases it.
void RpcReceiveServerChannel(unsigned int ClientId, FPacketBase* Packet, IClient* Client);
