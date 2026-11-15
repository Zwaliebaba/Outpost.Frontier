// Author: Giperion.

#pragma once

// Field-record and exponent helpers from movie.dll. Bodies are in MovieMod.cpp.
// The baseline address is on each definition. Calling conventions match the listing RET n.

// . RET 0x4. One step of the record state machine at stride 0x410.
void __stdcall Movie_AdvanceFieldRecordStates(int* Context);

// . RET 0x10. Depth at byte +0x11158 must be 0 or 1.
// *Context == 0 returns Slot and writes nothing. Every other path returns 0 or 1.
long __stdcall Movie_CommitFieldSlot(unsigned int* Limbs, int Slot, unsigned int* Context, int Sign);

// . RET 0x10. A zero high half returns that test's EAX.
// A high half equal to Field+0x12c commits the slot. Otherwise queues a state-1 record.
long __stdcall Movie_QueueOrCommitFieldLimbs(unsigned int* Limbs, unsigned int Slot, unsigned int* Context, unsigned int Sign);

// . RET 0x4. Returns the record index, then increments Context[2].
// While that count is 0x14, flushes through Movie_AdvanceFieldRecordStates.
int __stdcall Movie_AllocateFieldRecord(int* Context);

// . RET 0x14. Seeds Scratch + 5*n from Value, then multiplies Left by that seed.
long __stdcall Movie_SeedAndMultiplyLimbs(unsigned int* Left, unsigned int* Value, unsigned int* Dest, unsigned int* FieldHolder, unsigned int* Scratch);

// . RET 0x14. Product of two values stored as limb pairs (base, base+n).
long __stdcall Movie_MultiplyPairedLimbs(unsigned int* Left, unsigned int* Right, unsigned int* Dest, unsigned int* FieldHolder, unsigned int* Scratch);

// . RET 0x14. Sliding-window exponentiation. Returns squares + multiplies.
int __stdcall Movie_ExponentiateByWindow(unsigned int* Base, unsigned int* Exponent, unsigned int ExponentLimbs, unsigned int* Dest, unsigned int* Context);
