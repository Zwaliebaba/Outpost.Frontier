// Author: Giperion.

#pragma once

// Common.dll-internal oriented-box helpers shared by the PathfindManager user-zone probe
// (common_pathfind.cpp, where both are defined) and the zone shapes (common_zone.cpp).
// Neither is exported by baseline. Moved here from common_stubs.h (CMN-SPLIT, 2026-10-14).

#include "common_ini.h"  // Vector
#include "common_csys.h" // Matrix

// (Common): Out = the 3DMathEngine's TransposeTransform
// (slot +0x38) of Src by M, bounced through a scratch Vector.
void MathTransposeTransformVia3DMathEngine(Vector* Out, Matrix const* M, Vector const* Src);

// (Common): `bool __thiscall(Box, Vector const* Src,
// Vector* Dst)` -- Dst = Src snapped onto the box surface (half-extents at Box+0x30), returns
// whether Src was outside. Box is an oriented box {Matrix +0x00, Vector +0x24, half-extents
// +0x30}.
bool ClampPointToBoxSurface(unsigned char* Box, Vector const* Src, Vector* Dst);
