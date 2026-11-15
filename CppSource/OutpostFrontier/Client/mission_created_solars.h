// Author: Giperion.

#pragma once

// Universe\MissionCreatedSolars.ini -- the positions and fieldguidetext of the bases that missions
// create at run time (they have no [Object] in a system file). Loaded once, on first use, into
// the vector the field (static init the branch, atexit the branch).

//   [MissionCreatedSolar]
//   base     = <base nickname>
//   ids_info = <ids>
//   pos      = x, y, z

// Readers: the nav map's base / solar position lookups (the routine, the routine) and the
// NN base-info code at 0x0047503c.

#include <vector>

// One record, 0x14 bytes (the routine 0x00476dd5..0x00476e03).
struct FMissionCreatedSolar
{
    unsigned BaseId; // +0x00 CreateID(base)
    float Pos[3];    // +0x04 pos
    int IdsInfo;     // +0x10 ftol(ids_info)
};

// (allocator byte) / the field.. (first, last, end).
extern std::vector<FMissionCreatedSolar> G_MissionCreatedSolars;
// the load latch (set before the file is opened, so a missing file is not
// retried).
extern unsigned char G_bMissionCreatedSolarsLoaded;

// once: read MissionCreatedSolars.ini; a record is pushed as soon as a section
// has given all three keys (and the three flags start over).
void LoadMissionCreatedSolars();

// (cdecl) -- load, then the first record for BaseId: its position into OutPos
// (3 floats) and its ids_info into OutIdsInfo. False (nothing written) when there is none.
bool FindMissionCreatedSolar(unsigned BaseId, float* OutPos, int* OutIdsInfo);
