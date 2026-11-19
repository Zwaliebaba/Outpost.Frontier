// Author: Giperion.

#pragma once

// (Common, implementation tracker item 3304,
// ported by D102 2026-09-09 while surveying the D84-flagged unprocessed range
// ). A tiny multiplicative-congruential PRNG: a single
// 32-bit global seed (baseline the field) is multiplied by 75 (= 3*5*5, via
// three LEA-based multiplies -- Common) every call, the
// low 16 bits of the new seed are taken as the pseudo-random value, then
// scaled to a float via `float(x) * (1.0f/65536.0f)` (baseline the field ==
// == 2^-16, confirmed by reading its raw bytes) -- i.e. a float in
// [0, 1). No modulus beyond 32-bit register wraparound (baseline relies on
// natural overflow, not an explicit `% M`).

// Confirmed live caller context (Common/0x063498bf, outside
// this pass's assigned address range): IVP_Real_Object's per-contact
// friction/anomaly "cooldown" timer reset -- `word[core+0x118] = 15 -
// (int)(IVP_FastRandomFloat() * K)` once the previous cooldown counts down to
// 0 -- i.e. this feeds a randomized re-arm delay for a contact-pair anomaly
// limiter, not gameplay-visible randomness. Kept here as a small, reusable,
// self-contained utility rather than inlined into that (unported) caller.
float IVP_FastRandomFloat();

// Oracle instrumentation (P2.5, design/sim-records.md): the seed as it stands, which the sim
// oracle records at each PSI. Nothing in the game calls it.
unsigned IVP_FastRandomSeed();
