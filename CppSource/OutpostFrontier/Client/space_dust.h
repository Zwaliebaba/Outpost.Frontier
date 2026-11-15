// Author: Giperion.

#pragma once

// SpaceDust -- baseline Client\Space\SpaceDust.cpp (OutpostFrontier.exe 0x0054bda0..0x0054c361).
// Three ParticleEffects follow the camera: the system's own dust ([Dust] spacedust= of the
// system INI) and, while the camera sits in a PROP_DUST_ZONE zone, that zone's dust, with the
// previous zone's dust fading out. All state is file-level (.bss 0x00679790..0x006797c0).

#include "common_archetype.h" // CacheString
#include "common_universe.h"

// (cdecl) -- StarSysLoadIni's [Dust] section (00553d6d): remember the system dust
// name (pointer copy into the field) and reset the zone-dust bookkeeping. No effect is
// created here; the first SpaceDustUpdate does that.
void SpaceDustSetSystemDust(CacheString const* Dust);

// StarSysLeave (00554c0b): release all three effects, clear both names.
void SpaceDustShutdown();

// (cdecl) -- StarSysUpdate (00554ecb) with the field and the active camera.
void SpaceDustUpdate(Universe::ISystem const* System, void* Camera);
