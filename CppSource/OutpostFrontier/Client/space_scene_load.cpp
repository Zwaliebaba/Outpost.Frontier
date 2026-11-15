// Author: Giperion.

#include "space_scene_load.h"
#include "jump_effect.h"
#include "star_sphere.h"

#include "common_ini.h"

// 2026-09-25 (EXE-W2-E): the system INI load that used to live here -- a hand-rolled reading
// of [System]/[Archetype]/[Asteroids]/[Object]/[Nebula] with an invented "deferred spawn
// queue" standing in for the [Archetype] preload list, a fake current-system id
// (CreateID("He01")) and a second copy of the field -- is gone. baseline's owner of all of
// it is StarSys.cpp: the routine / the routine in star_sys.cpp, reached from the connect
// slice the routine (player_spawn.cpp) with the real id from the routine.
// The scaffold that hung off it (space_scene_spawn.*, space_scene_objects.*,
// space_equipment_spawn.*, the external draw vtable and the world-position lookups) had no
// caller left and was removed with it.

// What remains here is the part of the routine that precedes the system enter:
// (jump-gate FX INI) and the routine (solar star archetypes).

bool LoadSpaceSceneSlice()
{
    // the routine -- the jump-gate FX INI (JumpGateEffect / JumpShipEffect
    // sections).
    JumpEffectIniLoad();
    ::LoadSolarStarArchIni();
    RefreshLoadingProgress();
    return true;
}
