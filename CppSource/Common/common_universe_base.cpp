// Author: Giperion.

#include "common_universe.h"
#include "common_universe_internal.h"

#include "common_archetype.h"
#include "common_cobjdb.h"
#include "common_csys.h"
#include "common_id.h"
#include "common_ini.h"
#include "common_misc.h" // Geometry::Sphere / Frustum, SphereCull
#include "common_net.h"  // CommReferrable (SYSTEM's second base)
#include "common_reputation.h" // Reputation::Vibe static store (FreeStatic / Begin / EndStaticLoad)
#include "common_zone.h"   // ZONE, the concrete IZone

#include "NeuronRuntime.h"
#include "neuroncomponents.h"
#include "readfile.h"

#include <list>
#include <map>
#include <math.h>
#include <new>
#include <stdlib.h>
#include <string.h>
#include <vector>

// ============================================================================
// BASE -- Universe::IBase's implementation (vftable module storage)
// ============================================================================

namespace CmnUniverse
{
    // module storage-06338780 (the stack BASE's construction): ID_String::clear
    // on +0x04 / +0x14, CacheString::clear on +0x08 / +0x10, an empty string at +0x18, the
    // vftable, and zero in +0x0c, +0x28, +0x2c (byte), +0x30..+0x68 and +0x6c (byte).
    BASE::BASE()
    {
        NicknameId = 0;
        ((CacheString*)&Nickname)->clear();
        ((CacheString*)&File)->clear();
        SystemId = 0;
        StridName = 0;
        ShipSml[0] = 0;
        ShipSml[1] = 0;
        ShipSml[2] = 0;
        ShipMdm[0] = 0;
        ShipMdm[1] = 0;
        ShipMdm[2] = 0;
        ShipLrg[0] = 0;
        ShipLrg[1] = 0;
        ShipLrg[2] = 0;
        TerrainTiny = 0;
        TerrainSml = 0;
        TerrainMdm = 0;
        TerrainLrg = 0;
        TerrainDyna01 = 0;
        TerrainDyna02 = 0;
        bAutosaveForbidden = false;
        SpaceObjectId = 0;
        SpaceObjectVisit = 0;
    }

    unsigned BASE::GetId() const { return NicknameId; }             // [+0x04]
    char const* BASE::GetNickname() const { return Nickname; }      // [+0x08]
    int BASE::GetStridName() const { return StridName; }            // [+0x0c]
    char const* BASE::GetFile() const { return File; }              // [+0x10]
    unsigned BASE::GetSystem() const { return SystemId; }           // [+0x14]

    // the string's buffer ([+0x1c]), or the library's shared empty string when it
    // has none -- c_str().
    char const* BASE::GetBgcsBaseRunBy() const { return BgcsBaseRunBy.c_str(); }

    unsigned BASE::GetShipSml01() const { return ShipSml[0]; }      // [+0x30]
    unsigned BASE::GetShipSml02() const { return ShipSml[1]; }      // [+0x34]
    unsigned BASE::GetShipSml03() const { return ShipSml[2]; }      // [+0x38]
    unsigned BASE::GetShipMdm01() const { return ShipMdm[0]; }      // [+0x3c]
    unsigned BASE::GetShipMdm02() const { return ShipMdm[1]; }      // [+0x40]
    unsigned BASE::GetShipMdm03() const { return ShipMdm[2]; }      // [+0x44]
    unsigned BASE::GetShipLrg01() const { return ShipLrg[0]; }      // [+0x48]
    unsigned BASE::GetShipLrg02() const { return ShipLrg[1]; }      // [+0x4c]
    unsigned BASE::GetShipLrg03() const { return ShipLrg[2]; }      // [+0x50]
    unsigned BASE::GetTerrainTiny() const { return TerrainTiny; }   // [+0x54]
    unsigned BASE::GetTerrainSml() const { return TerrainSml; }     // [+0x58]
    unsigned BASE::GetTerrainMdm() const { return TerrainMdm; }     // [+0x5c]
    unsigned BASE::GetTerrainLrg() const { return TerrainLrg; }     // [+0x60]
    unsigned BASE::GetTerrainDyna01() const { return TerrainDyna01; } // [+0x64]
    unsigned BASE::GetTerrainDyna02() const { return TerrainDyna02; } // [+0x68]
    bool BASE::IsAutosaveForbidden() const { return bAutosaveForbidden; } // byte [+0x6c]
    void BASE::SetSystem(unsigned NewSystemId) { SystemId = NewSystemId; }
    unsigned BASE::GetSpaceObjectId() const { return SpaceObjectId; }          // [+0x28]
    unsigned char BASE::GetSpaceObjectVisit() const { return SpaceObjectVisit; } // byte [+0x2c]
    void BASE::SetSpaceObjectId(unsigned NewSpaceObjectId) { SpaceObjectId = NewSpaceObjectId; }
    void BASE::SetSpaceObjectVisit(unsigned char NewVisit) { SpaceObjectVisit = NewVisit; }
}
