// Author: Giperion.

#pragma once

// Limb arithmetic from movie.dll between the field helpers and SHA-1 init.
// Each body is in MovieWide.cpp. The baseline address is on the definition.

// . RET 0x10. Dest[i] = Source[i] * Digit. Returns the high carry.
unsigned int __stdcall Movie_StoreScaledLimbs(int Source, unsigned int Digit, unsigned int* Dest, int Count);

// . RET 0x10. Low Count limbs of Left[0..Count) * Right[0..Count).
void __stdcall Movie_MultiplyLowLimbs(unsigned int* Left, unsigned int* Right, unsigned int* Dest, unsigned int Count);

// . RET 0x14. Subtract RightCount limbs, then the borrow through Left.
// Returns the borrow. Same arity as AddLimbs.
long __stdcall Movie_SubtractLimbsWide(unsigned int* Left, int LeftCount, unsigned int* Right, int RightCount, int Sum);

// . RET 0x14. Sum of two limb vectors. The return value counts the carry limb.
unsigned int __stdcall Movie_AddLimbsLength(unsigned int* Left, unsigned int LeftCount, unsigned int* Right, unsigned int RightCount, int Dest);

// . RET 0x14. Schoolbook product. A zero short count clears the long count of Dest.
void __stdcall Movie_MultiplySchoolbook(unsigned int* Left, unsigned int LeftCount, unsigned int* Right, unsigned int RightCount, unsigned int* Dest);

// . RET 0x20. Extended Euclid on the significant limbs.
// Gcd receives the gcd. Coefficient receives CountB limbs of one Bézout coefficient.
// QuotientOut and Product may be null. Returns the gcd length.
unsigned int __stdcall Movie_ExtendedEuclidLimbs(unsigned int* OperandA, unsigned int CountA, unsigned int* OperandB, unsigned int CountB, unsigned int* Coefficient, unsigned int* QuotientOut, unsigned int* Gcd, unsigned int* Product);
