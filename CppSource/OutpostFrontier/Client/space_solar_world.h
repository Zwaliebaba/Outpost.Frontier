// Author: Giperion.

#pragma once

#include "common_ini.h"

// — system INI [Object] → real CSolar in client IObjDB.
// Sole become-real path for system objects (FSpaceSceneObject proxies removed).
// Runs during LoadSpaceSceneSlice (connect), before any BeginScene.

// Returns true when a FClientSolarGameobj was created, inited, and linked.
bool SpawnWorldSolarFromObjectSection(INI_Reader& Reader);

// (cdecl) -- push a planet-sphere detail level into every solar's ISphereMesh:
// T = clamp(2 * Detail - 1, 0, 1); Slot07(ftol(3T + 7)) and Slot03(float(ftol(3.5 - 2T))).
// Caller: the level_of_detail option apply the branch (perf_options.cpp).
void SolarSpheresApplyDetail(float Detail);
