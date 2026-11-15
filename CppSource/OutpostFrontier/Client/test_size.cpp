// OutpostFrontier is a reimplementation of the original 2003 game client and engine,
// faithful to the behaviour of the original game.
// This file was reconstructed by reverse engineering the original game's version 1.1 binaries,
// developed by Digital Anvil.
// Author: Giperion.

#include <cstddef>
#include <stdio.h>
struct FCockpitMeshHolder { void* a; long b; void* c; void* d; };
struct FWatchableStub { void* Self; void* Next; };
struct FPlayerShipManager {
    void* VtablePrimary;
    void* VtableSecondary;
    FWatchableStub Watchable;
    void* ShipObject;
    unsigned char PadTo1c[0x08];
    int ManagerKind;
    unsigned char PadTo104[0xE4];
    FCockpitMeshHolder CockpitMeshHolder;
    unsigned char PadTo400[0x2EC];
};
int main() { printf("sizeof holder=%zu manager=%zu cockpit_off=%zu\n", sizeof(FCockpitMeshHolder), sizeof(FPlayerShipManager), offsetof(FPlayerShipManager, CockpitMeshHolder)); return 0; }
