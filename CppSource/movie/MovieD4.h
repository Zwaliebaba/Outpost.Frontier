// Author: Giperion.

#pragma once

// Limb-pair multiply table the field. Slot 1 is the dword at 0x06add4d0.
// Each call multiplies Count separate unsigned integers and writes the products
// back to back. A product is twice as wide as one input. Left == Right selects
// the square body. The baseline address is on each definition.

// . RET 0x10. One 32-bit limb -> two limbs.
void __stdcall MultiplyOneLimbPairs(unsigned int* Left, unsigned int* Right, unsigned int* Product, unsigned int Count);

// . RET 0x10. Two limbs -> four limbs.
void __stdcall MultiplyTwoLimbPairs(unsigned int* Left, unsigned int* Right, unsigned int* Product, unsigned int Count);

// . RET 0x10. Three limbs -> six limbs.
void __stdcall MultiplyThreeLimbPairs(unsigned int* Left, unsigned int* Right, unsigned int* Product, unsigned int Count);

// . RET 0x10. Four limbs -> eight limbs.
// Count 0 and Count 0x80000000 both return without writing: the prologue adds Count to itself.
void __stdcall MultiplyFourLimbPairs(unsigned int* Left, unsigned int* Right, unsigned int* Product, unsigned int Count);

// . RET 0x10. Six limbs -> twelve limbs.
// IMUL EAX,EAX,0x30 / TEST EAX,EAX / JE returns when that byte count wraps to 0.
void __stdcall MultiplySixLimbPairs(unsigned int* Left, unsigned int* Right, unsigned int* Product, unsigned int Count);

// . RET 0x10. Eight limbs -> sixteen limbs.
// IMUL EAX,EAX,0x40 / TEST EAX,EAX / JE returns when that byte count wraps to 0.
void __stdcall MultiplyEightLimbPairs(unsigned int* Left, unsigned int* Right, unsigned int* Product, unsigned int Count);

// . RET 0x10. Twelve limbs -> twenty-four limbs.
// IMUL EAX,EAX,0x60 / TEST EAX,EAX / JE returns when that byte count wraps to 0.
void __stdcall MultiplyTwelveLimbPairs(unsigned int* Left, unsigned int* Right, unsigned int* Product, unsigned int Count);
