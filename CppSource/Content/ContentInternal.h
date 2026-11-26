// Author: Giperion.

#pragma once

#include "content.h"
#include "x86math.h"

#include <cstdio>
#include <windows.h>

// Internal Content.dll helpers. Not a NEURONCOMPONENTS surface.

extern HMODULE G_ContentModule;

// (.data, initial 1): module-wide id generator, post-incremented by
// the timer/handler-id writers of TU21, TU25, TU36, TU38, TU47 and TU53. Content.cpp.
extern unsigned int GContentNextUniqueId;

// (TU33, GameSupport.cpp): FLD double the field -- the game time
// accumulated from GameSupport heartbeats.
double __cdecl GetContentGameTime();
// Oracle instrumentation (P3.7b, OracleDumpRmSession): the game time GameSupport's heartbeat adds
// to (GameSupport.cpp:598), advanced by Dt without a heartbeat. No game module calls it.
void __cdecl OracleAddContentGameTime(double Dt);

// (TU33, GameSupport.cpp): |*A - *B|, or the .rdata default
// when either time is the -1.0 sentinel. Callers: GCS, TU54, TU57.
double __cdecl GetContentTimeDistance(double const* A, double const* B);

class Vector;   // common_ini.h

// (cdecl, COMDAT emitted in TU25; Populators.cpp): A - B returned by
// value -- FLD A.z FSUB B.z, FLD A.y FSUB B.y, FLD A.x FSUB B.x, each FSTP'd as float.
// 34 CALL sites (raw listing grep): 06eeab75 (TU25), 06f06b81/06f06bd1/06f091c0/06f0b4a5
// (TU37), 06f27cdb/06f27fcb (TU40), TU45 06f37400/06f37416 (PatrolPaths), TU46 06f475fa,
// TU52 06f5ac81, TU53 06f5eb00 .. 06f674f4 (18), TU54 06f6be9a, TU58 06f7274b,
// TU61 06f88d6e/06f8906e/06f8cb92.
Vector __cdecl SubtractVectors(Vector const& A, Vector const& B);

// (cdecl, TU52; Populators.cpp): y = bits 0x5f3759df - (bits(Value)
// SAR 1); returns (1.5f - ((y * y) * Value) * 0.5f) * y on the x87 stack (double here).
// CALL sites: TU52 06f5acab, TU53 06f65105/06f651c4/06f6751e/06f675c0/06f675d2.
double __cdecl FastReciprocalSqrt(float Value);

// (TU53, SpaceSupport.cpp): the SpaceSupport controller id, written by
// its ctor the routine; timers of the message router are set on it.
extern unsigned int GSpaceSupportControllerId;

// TU36 message router singleton (the field); class in ContentMessageRouter.h.
class FContentMessageRouter;
extern FContentMessageRouter* GMessageRouter;

I3DMathEngine* __cdecl ContentAcquire3DMathEngine();
void __cdecl ContentSetGcsClampEnabled(unsigned char EnabledByte);
bool __cdecl ContentIsGcsClampEnabled();
void __cdecl SpacePopSetGcsClampEnabled(unsigned char EnabledByte);

class INI_Reader;

// (ContentDataPath.cpp): pub::GetDataPath() + _makepath + INI_Reader::open(path, false).
bool __cdecl OpenContentDataIni(char const* RelativePath, INI_Reader* Reader);

// (ContentDataPath.cpp; callers TU31 06efc589, TU38 06f16339): Out's position is
// Anchor * (cos Heading * Range, 0, -sin Heading * Range) and its rotation faces away from
// Anchor's position (the z column points from the anchor to Out, y stays in the vertical plane).
void __cdecl PlaceFacingAnchor(FTransform const* Anchor, float const* Heading, float const* Range,
                               FTransform* Out);

IDLL* CreateGameSupport(char const* Name, unsigned int ControllerId, void const* Parms);
IDLL* CreateSpaceSupport(char const* Name, unsigned int ControllerId, void const* Parms);
IDLL* CreateBaseSupport(char const* Name, unsigned int ControllerId, void const* Parms);
IDLL* CreateSpacePop(char const* Name, unsigned int ControllerId, void const* Parms);
IDLL* CreateBasePop(char const* Name, unsigned int ControllerId, void const* Parms);
IDLL* CreateStoryLauncher(char const* Name, unsigned int ControllerId, void const* Parms);
IDLL* CreateStory(char const* Name, unsigned int ControllerId, void const* Parms);
IDLL* CreateDestroyMission(char const* Name, unsigned int ControllerId, void const* Parms);
// StoryMissions.cpp: one class per CreateInstance name (the routine table).
IDLL* CreateGenericMission(char const* Name, unsigned int ControllerId, void const* Parms);
IDLL* CreateMission01a(char const* Name, unsigned int ControllerId, void const* Parms);
IDLL* CreateMission01b(char const* Name, unsigned int ControllerId, void const* Parms);
IDLL* CreateMission02(char const* Name, unsigned int ControllerId, void const* Parms);
IDLL* CreateMission03(char const* Name, unsigned int ControllerId, void const* Parms);
IDLL* CreateMission04(char const* Name, unsigned int ControllerId, void const* Parms);
IDLL* CreateMission05(char const* Name, unsigned int ControllerId, void const* Parms);
IDLL* CreateMission06(char const* Name, unsigned int ControllerId, void const* Parms);
IDLL* CreateMission07(char const* Name, unsigned int ControllerId, void const* Parms);
IDLL* CreateMission08(char const* Name, unsigned int ControllerId, void const* Parms);
IDLL* CreateMission09(char const* Name, unsigned int ControllerId, void const* Parms);
IDLL* CreateMission10(char const* Name, unsigned int ControllerId, void const* Parms);
IDLL* CreateMission11(char const* Name, unsigned int ControllerId, void const* Parms);
IDLL* CreateMission12(char const* Name, unsigned int ControllerId, void const* Parms);
IDLL* CreateMission13(char const* Name, unsigned int ControllerId, void const* Parms);

// SpacePop exclusion spheres (baseline the field/5c/60).
void __cdecl SpacePopSetPopulationEnabled(unsigned char EnabledByte);
unsigned char __cdecl SpacePopGetPopulationEnabled();
void __cdecl SpacePopSetEncounterEnabled(unsigned char EnabledByte);
unsigned char __cdecl SpacePopGetEncounterEnabled();
void __cdecl SpacePopSetAttackClamp(unsigned char EnabledByte);
bool __cdecl SpacePopPositionIsExcluded(float const* Position);
void __cdecl SpacePopAddExclusionSphere(float X, float Y, float Z, float Radius);
void __cdecl SpacePopRemoveExclusionSphere(float X, float Y, float Z, float Radius);
void __cdecl SpacePopClearExclusionSpheres();
