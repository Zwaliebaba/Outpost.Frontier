// Author: Giperion.

#pragma once

// baseline "Client/Space" object family: OutpostFrontier.exe .text 0x00520000-0x0053f890 (approximate
// boundaries; see per-class notes below). Discovered while auditing this range (implementation tracker
// agent claude-sonnet-OutpostFrontier-batch48b), continuing on from tradelane_fx.h's TradeLaneRing
// class which ends around 0x00520000. Unlike the tradelane/space_env_fx ranges (which turned out
// to be one or two classes), this range is a whole run of distinct baseline Client/Space/*.cpp
// files, one after another, each the "client-side" (rendering/hardpoint-fx/network-activation)
// counterpart of an already-ported COMMON.DLL object class (CObject/CSimple/CEquip/Archetype).
// None of these client-side wrapper classes exist in the port. This header documents the file
// boundaries and identifying evidence found so a future pass has named anchors instead of
// re-deriving them; it intentionally does NOT attempt struct layouts for these (no
// individually-confirmed field yet justifies one), following the same "stub before simplified"
// spirit as tradelane_fx.h/space_env_fx.h.

// Boundaries were derived by finding, for each embedded "Client\..." path
// string, the nearest preceding function definition in OutpostFrontier -- i.e. each class's
// range starts at the first function that embeds its file's path (or, where a shared static
// registry precedes the class as in CliAsteroid's case, at that registry's first function).

// ----------------------------------------------------------------------------------------------
// 1) CliAsteroid.cpp -- OutpostFrontier.exe 0x005200b0-0x0052c7d0
// Confirmed via "Client\\Space\\CliAsteroid.cpp" embedded at many
// FDUMP call sites throughout this range (OutpostFrontier lines ~181906-185289, offsets 0x11e
// through 0x988 into the file). This is baseline's client-side CAsteroid rendering/field-member
// class (asteroid band/field visuals, mining/damage FX, debris spawn on destruction).
// /the routine/the routine/the routine/the routine (just before the first path
// string) walk a static doubly-linked list (the field head/the field count) calling
// /the routine on each node -- almost certainly this class's static
// registry/free-list (create/destroy-all/count accessors), so folded into this class's range
// rather than left attached to tradelane.cpp.
// No CAsteroid client-visual class exists in the port; src/ has no CliAsteroid/CAsteroid hits
// beyond CmnAsteroid::CAsteroidSystem/CAsteroidField in common_casteroid (server/Common field
// sim). 2026-09-22: space_scene_load ProcessAsteroidsSection now wires the Common-facing
// slice (Load/CreateAsteroidField/set_zone/CAsteroidField::load) without the
// 0x238 CliAsteroid object or the routine client-only keys — those remain this header's gap.

// ----------------------------------------------------------------------------------------------
// 2) CliLauncher.cpp -- OutpostFrontier.exe 0x0052c830-0x0052ec30
// Confirmed via "Client\\Space\\CliLauncher.cpp" (OutpostFrontier
// lines ~190509-190896). baseline's client-side missile/countermeasure launcher visual class
// (muzzle FX, launch-tube hardpoint resolution, fired-projectile handoff). Not ported.

// ----------------------------------------------------------------------------------------------
// 3) CliThruster.cpp -- OutpostFrontier.exe 0x0052ec40-0x00530a10
// Confirmed via "Client\\Space\\CliThruster.cpp" (OutpostFrontier line
// ~192069). the routine is this class's constructor: calls the routine (base-class ctor,
// likely the shared client-fx-object base used across this whole family), sets its vtable to
// , resolves a "thruster hardpoint" via FindHardpoint_PS using an ID_String key
// at archetype+0x54/+0x58, and on success spawns a thruster-glow effect via the routine +
// (particle/FX-instance creation helpers). On failure it FDUMPs "Thruster hardpoint
// not found: %s". Not ported.

// ----------------------------------------------------------------------------------------------
// 4) Debris.cpp -- OutpostFrontier.exe 0x005300f0-0x00531032 (the Debris gameobj, RTTI .?AVDebris@@,
// and the [Debris] table the field with its loader the routine). Ported in
// client_debris.h/.cpp (FClientDebrisGameobj, FDebrisArch, LoadDebrisArchFile, FindDebrisArch).

// ----------------------------------------------------------------------------------------------
// 5) EquippedObj.cpp -- OutpostFrontier.exe 0x00533d10-0x00536800
// Confirmed via "Client\\Space\\EquippedObj.cpp" (OutpostFrontier
// lines ~196012-196293). the routine (the file's first function) takes a CObject* and two
// ID_String* params -- consistent with baseline's per-hardpoint "equipped object" wrapper that
// binds a mounted equipment archetype's client visuals (guns/shields/thrusters/countermeasures
// external models) to a parent ship's hardpoint. Not ported (the port's equivalent concept,
// space_equipment_spawn.cpp, is a different/simplified spawn-only helper, not this class).

// ----------------------------------------------------------------------------------------------
// 6) explosion.cpp -- OutpostFrontier.exe 0x00536b50-0x00537ae0
// Confirmed via "Client\\space\\explosion.cpp" (OutpostFrontier lines
// ~197959-198171). Client-side explosion FX class (fireball/shockwave/debris-scatter visuals
// triggered by Gameobj.cpp's destruction path -- see the routine below, which calls
// Archetype::GetExplosion and the routine in this same range). Action_Explode
// (the routine..the routine, vtable 0x005de584) and the routine are ported in
// client_explosion.h/.cpp; the rest of this range is still to be checked item by item.

// ----------------------------------------------------------------------------------------------
// 7) gamelit.cpp -- OutpostFrontier.exe 0x00537af0-0x00539730
// Confirmed via "Client\\space\\gamelit.cpp" (OutpostFrontier line
// ~198765, inside `the routine(undefined4,undefined4,float)`). Client-side dynamic-light
// object class (per-object point/spot lights: engine glow, weapon muzzle flash, explosion
// flicker, registered with the D3D lighting pipeline). Not ported.

// ----------------------------------------------------------------------------------------------
// 8) Gameobj.cpp -- OutpostFrontier.exe 0x00539740-0x0053d930
// Confirmed via "Client\\space\\Gameobj.cpp" (OutpostFrontier lines
// ~200408-201055). This is the client-side per-CObject wrapper/dispatcher that ties a COMMON.DLL
// CObject/CSimple instance to its client-only rendering/FX state (explosion.cpp, gamelit.cpp,
// Debris.cpp, EquippedObj.cpp above all appear to be attached components of it). the routine
// (destruction handler): reads the owning CObject's archetype explosion-arch ID_String, resolves
// it via Archetype::GetExplosion, and either instantiates the explosion FX (the routine, this
// file's own explosion.cpp helper) or FDUMPs "Archtype(%s) explosion_arch(%s) not found" --
// then invokes a virtual method at *param_1+0x88 (likely the base client-fx-object's per-frame
// or on-destroy hook) and conditionally calls the already-ported-elsewhere the routine.
// forwards to the already-ported CSimple::SetOwnerPlayer, confirming this class
// holds/wraps a CSimple* member. This is architecturally the most important unported class in
// this whole range -- effectively "CGameObj", the client-side base every other class here
// (CliAsteroid/CliLauncher/CliThruster/EquippedObj) likely derives from or attaches to via
// (the shared base-class constructor referenced by CliThruster above). Not ported;
// no CGameObj-equivalent class exists in src/.

// ----------------------------------------------------------------------------------------------
// 9) IClientS.cpp -- OutpostFrontier.exe 0x0053d940-0x0053f880 (continues to ~0x005437d0 where
// baseline's ship.cpp begins, per batch47a's tradelane_fx.h note -- out of this range's scope).
// Confirmed via "Client\\space\\IClientS.cpp" (OutpostFrontier lines
// ~201697-201708). the routine dispatches on an activation-result code from the routine:
// mode 2 -> the routine (normal activation), mode 1 -> FDUMP "Combat mode no longer supported!
// (Ignorable)", failure -> FDUMP "SPObjActivate failed... probably jump-gate in old system
// closing?". This is baseline's "IClientSide" interface implementation: the per-space-object
// network activation/deactivation entry point (jump-gate transitions, zone-in/zone-out). Not
// ported; no IClientSide/IClientSpaceObj-equivalent exists in src/ (the unrelated IClientS*
// substrings that do exist -- common_server.h's EOT_CliAsteroid enum member, common_net.h's
// CliAsteroid-exclusion-distance helpers -- are server-side networking concepts, not this
// client interface).

// This IClientS.cpp range transitions directly into the already-triaged CDeadReckonedVector /
// CRemotePhysicsSimulation cluster at 0x0053f890 (see implementation tracker notes on those items --
// those two classes DO have a partial port in src/Common/common_cobject.h/.cpp and
// src/include/common_server.h + src/Common/common_physys.cpp; CDeadReckonedVector's layout
// and quadratic dead-reckoning algorithm were re-derived and fixed, but CRemotePhysicsSimulation's
// own Doit/RemoteUpdate/CheckForSync are still unported), then into ship.cpp beyond 0x00540000.

// CORRECTION (batch49a, auditing 0x00540000-0x00560000): ship.cpp does begin at ~0x00540930
// as guessed, but it is NOT the only file in that range -- it is one of EIGHT distinct
// baseline files packed back to back (ship.cpp/CShip, space.cpp connect+disconnect lifecycle,
// SpaceDust.cpp, SpaceTerrainRenderComp.cpp, StarSphere.h/StarSys.cpp, Hotspot.cpp,
// LightsCamera.cpp, Support.cpp). See src/OutpostFrontier/Client/ship_space_supt_fx.h for the
// full evidence chain and boundaries.
