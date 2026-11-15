// Author: Giperion.

#pragma once

// Common.dll-internal helpers shared by the Archetype translation units: common_archetype.cpp
// (where they are defined), common_archetype_ships.cpp, common_archetype_equipment.cpp,
// common_archetype_weapons.cpp and common_archetype_misc.cpp. baseline keeps all of this in one
// TU (Common\Archetype.cpp); none of these helpers is exported. Split out
// of common_archetype.cpp's anonymous namespaces (CMN-SPLIT phase B, 2026-10-14).

#include "common_archetype.h"
#include "common_ini.h"     // INI_Reader
#include "NeuronRuntime.h"          // FEngineName (via engine/sceneruntime.h; NeuronRuntime.h must come first, it
                            // declares the distinct IEngine2 before sceneruntime.h is seen)

int CallNeuronComponentsStricmp(const char* Left, const char* Right);  // common_neuroncomponents_stricmp.cpp

namespace Archetype
{
    namespace CmnArchetype
    {
        // FEngineArchetypeName was a field-for-field copy of sceneruntime.h's FEngineName; the two
        // are the same 8-byte {const char*, CRC32} record IEngine takes at slots 11/12/17.
        typedef FEngineName FEngineArchetypeName;

        // baseline __FILE__ of Archetype.cpp, the first argument of every FDUMP here.
        const char ArchetypeSource[] = "Common\\Archetype.cpp";

        // GetEquipmentByName(CreateID(Name)).
        Equipment* LookupLootAppearance(char const* Name);

        // NEURONCOMPONENTS stricmp walk over the 63-entry hp-type table at module storage; first match
        // stores its value and returns true, a miss returns false with OutValue untouched.
        bool TryLookupHpType(char const* Name, unsigned* OutValue);

        // "loot_appearance" key: Out = LookupLootAppearance(value), true; any other key false.
        bool ReadLootAppearance(INI_Reader& Reader, Equipment*& Out);

        // The VC6 vector<unsigned> copy constructor / operator= of the ShieldGenerator (+0xa8)
        // and Gun (+0xbc) hp-type arrays, kept as malloc'd {Begin, End, Cap} blocks.
        void CopyConstructHpTypes(unsigned*& Begin, unsigned*& End, unsigned*& Cap,
                                  unsigned const* SrcBegin, unsigned const* SrcEnd);
        void AssignHpTypes(unsigned*& Begin, unsigned*& End, unsigned*& Cap,
                           unsigned const* SrcBegin, unsigned const* SrcEnd);

        // ICliObj slot 0 / slot 1 on a companion; null skips.
        void CliObjAddRef(ICliObj* Companion);
        void CliObjRelease(ICliObj* Companion);
    }

    using namespace CmnArchetype;
}
