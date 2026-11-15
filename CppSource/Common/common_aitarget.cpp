// Author: Giperion.

// Common.dll FAiTarget (the 0x88-byte AI-target cache record): FAiTarget::Init
// (0x062d09b0), FAiTarget::RefreshPos (0x062c47c0) and GetAiTargetEffectiveRadius
// (0x062e6070). See src/include/common_aisteer.h for the layout.

// CMN-SPLIT stage 1 (2026-10-14): split out of common_aisteer.cpp, code unchanged.

#include "common_aisteer.h"

// , item 1470. Raw bytes at Common:
//   [ECX+0x80] = param_1; [ECX+0x84] = param_2;
//   [ECX+0x7d] = [ECX+0x7c] = [ECX+0x7e] = [ECX+0x7f] = 0;   RET 8
// baseline clears the four guards in the order 7d, 7c, 7e, 7f -- no observable
// difference, they are four independent bytes.
void FAiTarget::Init(IObjInspect const* NewAttitudeRef, IObjInspect const* NewIface)
{
    AttitudeRefIface = const_cast<IObjInspect*>(NewAttitudeRef);
    Iface = const_cast<IObjInspect*>(NewIface);
    bAttitudeValid = 0;
    bFlagsValid = 0;
    bPosValid = 0;
    bCornersValid = 0;
}

// , item 1283. Raw bytes at Common. baseline
// raises bPosValid BEFORE the vcall (062c47d3, ahead of the 062c47de CALL),
// unlike the get_type refresh below which raises it after -- reproduced as
// written, since a re-entrant call would see a different guard state.
// baseline dereferences `[EAX+0x84]`'s vtable unguarded, and so does the port.
void FAiTarget::RefreshPos()
{
    // no null test on Iface before `CALL [EDX+0x2c]`.
    if (bPosValid != 0)
        return;
    bPosValid = 1;
    Iface->get_physical_radius(CachedRadius, CachedPos);
}

// , item 1367. Raw bytes at Common: byte +0x7d zero ->
// [+0x84] slot 0x88 get_type(&this+0x08), then +0x7d = 1; return [+0x08]. No null test.
unsigned FAiTarget::GetFlags()
{
    if (bFlagsValid == 0)
    {
        Iface->get_type(CachedFlags);
        bFlagsValid = 1;
    }
    return CachedFlags;
}

// , item 1770. See common_aisteer.h for why this item's
// unaff_ESI/unaff_retaddr blocker was a false positive (baseline reuses its own
// incoming parameter slot as get_atmosphere_range's out-parameter).

// is FAiTarget::RefreshPos inlined verbatim, guard byte and
// argument order included. 062e60a5 pre-zeroes the out-slot, so a failing
// get_atmosphere_range yields 0.0f rather than garbage.
float GetAiTargetEffectiveRadius(FAiTarget* Target)
{
    Target->RefreshPos();

    // the radius is snapshotted into its own stack slot
    // BEFORE the second vtable call, so a get_atmosphere_range implementation
    // that somehow rewrote CachedRadius could not affect the comparison.
    float const Radius = Target->CachedRadius;

    // out-slot zeroed, then `CALL [EDX+0x140]` on Iface with
    // no null test (slot 80, get_atmosphere_range); the return value is ignored.
    float AtmosphereRange = 0.0f;
    Target->Iface->get_atmosphere_range(AtmosphereRange);

    // `FLD AtmosphereRange; FCOMP Radius; TEST AH,0x41; JNZ`
    // -- AH & 0x41 is zero only for a strict ST0 > src (fall-through, returns
    // the atmosphere range); equal AND unordered take the jump to the radius.
    return (AtmosphereRange > Radius) ? AtmosphereRange : Radius;
}
