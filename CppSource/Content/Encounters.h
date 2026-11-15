// Author: Giperion.

#pragma once

#include "common_id.h"
#include "x86math.h"

#include <map>
#include <vector>

class INI_Reader;

// Content.dll lane CNT-POP (implementation notes, TU map): encounters and the static game tables.
//   TU31 tail 06efc9b0-06efe100  missions\formations.ini database (the field)
//   TU32      06efe100-06efe8e0  LoadContentGameTables / UnloadContentGameTables

// ---------------------------------------------------------------------------------------
// TU32 -- the static game tables, loaded by the GameSupport ctor the routine (06efeb73)
// and released by its dtor the routine (06efebdb).
// ---------------------------------------------------------------------------------------
void __cdecl LoadContentGameTables();
void __cdecl UnloadContentGameTables();

// ---------------------------------------------------------------------------------------
// TU31 tail -- formations (missions\formations.ini, [Formation] blocks).
// ---------------------------------------------------------------------------------------

// (thiscall on the matrix, no return): the 3x3 identity.
void SetMatrixIdentity(FMatrix* Matrix);

struct FFormationSlot           // baseline 0x34 bytes
{
    FVector Position;           // +0x00 "pos" / "pl_pos" x, y, z
    FMatrix Orientation;        // +0x0c the loader always stores the identity
    bool bPlayerSlot;           // +0x30 1 for "pl_pos", 0 for "pos"
};

struct FFormation               // map value, baseline 0x44 bytes
{
    ID_String Nickname;         // +0x00 CreateID("nickname")
    FVector Position;           // +0x04 the loader leaves it zero
    FMatrix Orientation;        // +0x10 the loader leaves it identity
    std::vector<FFormationSlot> Slots;   // +0x34 (alloc byte) / +0x38..+0x40

    int CountPlayerSlots() const;
    int CountShipSlots() const;
    // Slot position in world space: Orientation * slot + Position; zero vector when
    // the slot vector is unallocated or Index is out of range.
    FVector GetSlotPosition(unsigned int Index) const;
    // Position as above plus Orientation * slot orientation; false (outputs untouched)
    // when the slot vector is unallocated or Index is out of range.
    bool GetSlotTransform(unsigned int Index, FVector* OutPosition,
                          FMatrix* OutOrientation) const;
    // No range check, exactly as baseline.
    bool IsPlayerSlot(int Index) const;
};

// map<formation nickname id, formation>; static init the branch, atexit
// .
extern std::map<unsigned int, FFormation> GFormations;

void __cdecl LoadFormation(INI_Reader* Reader);
void __cdecl ClearFormations();
FFormation* __cdecl FindFormation(unsigned int const* FormationId);
