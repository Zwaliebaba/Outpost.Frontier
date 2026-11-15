// Author: Giperion.

#pragma once

#include "common_exports.h"
#include <stdint.h>

// Shared switch for the client and Common/IVP. Disabled unless explicitly enabled.
#ifndef OUTPOST_FRONTIER_ENABLE_TICK_PROFILE
#define OUTPOST_FRONTIER_ENABLE_TICK_PROFILE 0
#endif

// Port-only physics diagnostics requested by Giperion (CLIENT-BETA-LABEL).
// QPC ticks, inclusive and summed across all substeps of one client physics block.
enum class EPhysicsTickSection
{
    Simulate,
    PsiPrepare,
    SimUnits,
    CommitHulls,
    Contacts,
    SyncObjects,
    Count
};

struct FPhysicsTickProfile
{
    int64_t Ticks[static_cast<int>(EPhysicsTickSection::Count)] = {};
};

namespace PhySys
{
    // Diagnostic extension to the port's export surface, absent in baseline.
    // Borrowed on this thread only; caller restores the previous sink before destruction.
    COMMON_API FPhysicsTickProfile* __cdecl SetTickProfile(FPhysicsTickProfile* Profile);
}
