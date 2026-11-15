// Author: Giperion.

#pragma once

// Windowed limb arithmetic from movie.dll. Bodies are in MovieWindow.cpp.
// The baseline address is on each definition. Calling conventions match RET n.

// . RET 0x10. One limb. Returns squares plus multiplies.
int __stdcall Movie_ExponentiateLimb(unsigned int* Base, unsigned int Exponent, unsigned int* Dest, unsigned int* Context);

// . RET 0x8. Forwards the buffer to 0x06acca95 with two null pointers.
void __stdcall Movie_FillByteBuffer(unsigned char* Buffer, unsigned int ByteCount);

// . RET 0x18. The loop at 0x06aca8dd is the body, not a separate entry.
void __stdcall Movie_MultiplyWindows(unsigned int* Left, unsigned int* LeftTail, unsigned int* Right, unsigned int* RightTail, unsigned int* Product, unsigned int* Dispatch);

// . RET 0x18. Groups is the outer trip count (NEG, then count up to zero).
void __stdcall Movie_FoldPartialProducts(unsigned int* Base, unsigned int Count, unsigned int Groups, unsigned int Step, unsigned char* SignLeft, unsigned char* SignRight);

// . RET 0x1C. The loop at 0x06acaeb5 is the body, not a separate entry.
void __stdcall Movie_MultiplyWindowCross(unsigned int* Left, unsigned char* LeftMark, unsigned int* Right, unsigned char* RightMark, unsigned int* Middle, unsigned int* Product, unsigned int* Dispatch);

// . RET 0x14. Repeats is the outer trip count. Signs receive one byte per trip.
void __stdcall Movie_SubtractHalves(unsigned int* Limbs, unsigned int Count, unsigned int Repeats, unsigned int Step, unsigned char* Signs);

// . RET 0x14. Copies Count limbs, zero-fills to Dispatch[0], then subtracts halves.
void __stdcall Movie_EvaluateWindow(unsigned int* Source, unsigned int Count, unsigned int* Dest, unsigned char* DestTail, unsigned int* Dispatch);
