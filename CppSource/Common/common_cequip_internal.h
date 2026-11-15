// Author: Giperion.

#pragma once

// Common-internal helpers shared by the TUs split out of common_cequip.cpp (CMN-B2, plan 3.3).
// They were file-local (anonymous namespace) in common_cequip.cpp; the ones used by two or more
// of the new TUs live here in namespace CmnCequip. The constants keep internal linkage (const at
// namespace scope); the functions are defined in common_cequip.cpp. Not part of the public ABI.

// baseline calls the IEngine slots directly at every site (no validity / null wrappers); the former
// IsValidInstance / GetInstanceUserData / GetEngineInstanceOrientation / OwnerVelocity wrappers
// added guards baseline does not have and were removed (cmn-s2-eqa, 2026-10-14).

#include "common_cequip.h"
#include "common_cobject.h"   // CEqObj
#include "common_goods.h"     // BaseWatcher (WatchedTarget)
#include "NeuronRuntime.h"            // NeuronRuntime::Engine, G_EngineJointPayloadDwords (FJointRelative)

// NEURONCOMPONENTS.DLL!stricmp (Common IAT 0639902c), defined in common_neuroncomponents_stricmp.cpp.
int CallNeuronComponentsStricmp(const char* Left, const char* Right);

namespace CmnCequip
{
    const char kCAttachedEquipSource[] = "Common\\CAttachedEquip.cpp";
    const unsigned kAttachedEquipMask = 0x1fe0u;
    const long kInvalidEngineInstance = -1;

    void* WatchedTarget(BaseWatcher const& Watcher);

    // An engine joint block as Common reads and builds it: what IEngine::GetInstanceRelative
    // (+0xc4) returns and IEngine::CreateJoint (+0xbc) copies (G_EngineJointPayloadDwords
    // dwords; SceneRuntime owns the storage). Layout from the routine, which builds a revolute one
    // (joint at ESP+0x30, the next local at ESP+0x8c), and from CEGun's readers.
    struct FJointRelative
    {
        unsigned long Type;   // +0x00  2 = revolute
        Vector ParentPos;     // +0x04  the parent hardpoint position
        Vector ChildPos;      // +0x10  the child hardpoint position
        Matrix Orient;        // +0x1c  parent * child hardpoint orientation
        Vector Axis;          // +0x40  the rotation axis in that frame
        float Min;            // +0x4c  angle limits
        float Max;            // +0x50
        // TODO: +0x54/+0x58 are never written by the routine (stack garbage in baseline); a
        // revolute joint does not use them.
        unsigned long Field_0x54;
        unsigned long Field_0x58;
    };
    static_assert(sizeof(FJointRelative) == G_EngineJointPayloadDwords * sizeof(unsigned long),
                  "FJointRelative is the whole IEngine joint block");

    // IEngine+0xc4 for an instance id (baseline never tests the result for null where it reads it).
    inline FJointRelative const* InstanceRelative(long Instance)
    {
        return (FJointRelative const*)NeuronRuntime::Engine->GetInstanceRelative((void*)Instance);
    }
}
