// Author: Giperion.

#pragma once

// Point hashing shared by the IVP surface builders (ledge soup weld path,
// pointsoup / polygon-convex chain). Bodies live in
// ivp_surface_builder_ledge_soup.cpp.

// (module storage-0636cfcf, stdcall, RET 4): CRC-32 of
// the first 12 bytes (x, y, z) of a point with the reflected table
// .data:the field (polynomial 0xEDB88320), seed 0xffffffff, no final
// inversion, then OR 0x80000000 (the IVP_VHash "occupied" bit).
// Callers: 0636b450, 0636b4c3, 0636c312, 0636c32b, 0636c652, 0636c6a5,
// , 0636dc31, 0637a3ee, 0637a416.
unsigned IVP_PointHashIndex(const void* Point);

// REPE CMPSD over 3 dwords; SETZ -> 1 when x, y and z
// match bit for bit, else 0 (stdcall, RET 8). Slot 0 (compare) of the point
// hash vtables module storage (ledge soup), 063a590c and 063a5918.
int IVP_PointHashCompare(const void* A, const void* B);
