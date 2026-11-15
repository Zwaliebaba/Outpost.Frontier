// Author: Giperion.

#pragma once

// Content.dll TU58 (06f71fc0-06f802b0): the per-system information table SystemInfo
// (the field, map<system id, SystemInfo>), plus the pieces of TU30 (the zone
// population block) and TU64 (the zone enumerator) that fill its zones.
// Lane CNT-POP (implementation notes, TU map).

#include "common_net.h"     // CommReferrable
#include "common_id.h"      // ID_String
#include "common_ini.h"     // Vector
#include "common_server.h"  // pub::System::ZoneEnumerator
#include "x86math.h"        // FMatrix
#include "PatrolPaths.h"    // FPatrolPath (FSystemInfo +0x50)
#include "NpcCreate.h"      // FSolarNpc (FSystemInfo +0x88)
#include "EncounterParams.h" // FZoneEncounter (zone +0x38), FEncounterParameters (+0x120)

#include <cstdlib>
#include <list>
#include <map>
#include <utility>
#include <vector>

class INI_Reader;
class FContentNpcShip;              // NpcShip.h (TU43)
class FContentPlayerShip;           // ContentPlayerState.h (TU46)
struct FContentNpcShipParams;       // NpcShip.h (TU43)
struct FSpawnRequest;               // Populators.h (the SpacePop population request)
struct FSystemInfo;
struct FSystemZone;

// ---------------------------------------------------------------------------------------
// TU30 -- the zone population block (zone +0x18, baseline 0x44 bytes).
// ---------------------------------------------------------------------------------------

// "density_restriction = count, type" list element (8 bytes; the routine copies it).
struct FDensityRestriction
{
    int MaxCount;               // +0x00 get_value_int(0)
    ID_String Type;             // +0x04 CreateID(get_value_string(1))
};

// Ctor the routine, value parser the routine, validation the routine.
struct FZonePopulation
{
    int Density = 0;                                    // +0x00 zone "density" (the routine)
    float RepopTime = 10.0f;                            // +0x04 zone "repop_time"
    bool bValid = false;                                // +0x08 the routine result
    bool bHasPopulationKeys = false;                    // +0x09 set by every key the routine takes
    float TradelaneAttack = 0.0f;                       // +0x0c "tradelane_attack" * 0.01
    std::list<FDensityRestriction> DensityRestrictions; // +0x10 "density_restriction"
    float Field_0x1c = 3.0f;                            // +0x1c TODO: reader not ported yet
    // +0x20 "encounter" (EncounterParams.h); element dtor the routine (zone dtor
    // 06f30863) / its twin the routine, insert the routine.
    std::vector<FZoneEncounter> Encounters;
    float Field_0x30 = -1.0f;                           // +0x30 TODO: reader not ported yet
    bool bPopulationAdditive = true;                    // +0x34 "population_additive"
    int MaxBattleSize = 0;                              // +0x38 "max_battle_size"
    float ReliefTime = 15.0f;                           // +0x3c "relief_time"
    FSystemInfo* System = nullptr;                      // +0x40 written by the routine

    // (RET 0xc): one [Zone] value; false when the key is not a population key.
    bool ParseValue(INI_Reader* Reader, FSystemInfo* InSystem, FSystemZone* Zone);
    // bValid = every encounter passes the routine(0).
    bool Validate();

    // (RET 0x10; SpacePop the routine): every encounter refreshes at *Time
    // (failures are erased; bValid drops when none is left) and spawns its starred
    // formations; then, when RollSpawnChance allows, encounters are drawn by chance (rand)
    // until one spawns. Returns the number of ships created.
    int Populate(FSpawnRequest const* Request, float const* Time, float const* DensityScale,
                 unsigned int const* MaxDensity);
    // (RET 0xc): the chance of a spawn -- clamp((MaxDensity * Scale - ships
    // within the density radius) / (RepopTime / Scale), 0, 1) -- against rand(). MaxDensity -1
    // means the zone's own density. False (without rand) when the zone is already full.
    bool RollSpawnChance(FSpaceObjPlacement const* Placement, float const* DensityScale,
                         unsigned int const* MaxDensity);
    // (RET 0xc; SpacePop the routine): as Populate without the chance roll; the
    // draw repeats (a successful encounter stays in the pool) until the ships created reach
    // *MaxCount or the pool empties.
    int PopulateUpTo(FSpawnRequest const* Request, float const* Time, unsigned int const* MaxCount);
};

// TU30 static-init globals (the branch / 8f50 / 8f70 / 8f80).
extern float GEncounterDensityRadiusSP;     // = 3750 (single-player density radius)
extern float GEncounterDensityRadiusMP;     // = 850 * 2 + 7500 = 9200
extern float GEncounterRestrictionCone;     // = 3750 (density_restriction cone range)
// = 2700: read by TU53 06f65a0a / 06f65a36 / 06f65c21 (SpawnPlacement.cpp): the
// search range of the non-arrival spawn placement points.
extern float GEncounterPlacementRange;

// ---------------------------------------------------------------------------------------
// TU64 -- zones.
// ---------------------------------------------------------------------------------------

// SystemInfo +0x60 vector element, baseline 0x5c bytes. Built on the stack by the zone
// enumerator the routine and parsed by the routine; operator= the routine, dtor
// (both member-wise).
struct FSystemZone
{
    unsigned int ZoneId = 0;            // +0x00 the enumerated zone
    ID_String Nickname;                 // +0x04 CreateID("nickname")
    FSystemInfo* System = nullptr;      // +0x08 the owning system
    FPatrolPath* Path = nullptr;        // +0x0c the path of the zone's path_label (the routine)
    // +0x10: 1 by default; on an encounter load, 1 only when the zone has neither
    // population keys nor a path_label. Such a zone with no mission_type is dropped.
    bool bUnpopulated = true;
    bool bTradelane = false;            // +0x11 "Tradelane" ring zone, 10x longer than wide
    bool bLawful = false;               // +0x12 mission_type lawful, or a "vignette" nickname
    bool bUnlawful = false;             // +0x13 mission_type unlawful
    unsigned int VignetteType = 1;      // +0x14 1 default, 2 "field", 4 "exclusion"
    FZonePopulation Population;         // +0x18

    FSystemZone() = default;
    // (cdecl construct-in-place used by the zone vector): every member, +0x14
    // included -- the implicit copy.
    FSystemZone(FSystemZone const& Other) = default;
    // (RET 4): member-wise, EXCEPT VignetteType (+0x14), which keeps the
    // destination's value. baseline only reaches it through the zone vector's shifting copies,
    // which neither the loader's push_back at the end nor the full clear performs.
    FSystemZone& operator=(FSystemZone const& Other);

    // (RET 8): the zone's centre moved by +/- half its y size along its own y
    // axis (pub::Zone::GetSize / GetPos / GetOrientation, I3DMathEngine::TransformXform).
    // Always true.
    bool GetPathEnds(Vector* OutFirst, Vector* OutSecond);
    // (RET 4).
    void SetPath(FPatrolPath* InPath);
    // (ICF-folded "return [this + 0xc]" getter).
    FPatrolPath* GetPath();
    // pub::Zone::GetPopulation(ZoneId) -- the zone's faction weights (the
    // result code is ignored). Callers TU58 06f779da / 06f77a78, RM 06f808a5 / 06f825ef.
    weighted_vector<unsigned int> const* GetPopulationFactions();
    // (x87 result): 0 for an unpopulated zone, else Population.TradelaneAttack.
    // Callers TU58 06f779fe / 06f77a9c.
    float GetTradelaneAttack() const;
};

// "path_label" record the enumerator collects for the routine (baseline 0x38 bytes; the
// loader's local vector, element dtor = the vector at +0x08).
struct FZonePathLabel
{
    unsigned int ZoneId = 0;            // +0x00 0 = the zone had no path_label
    ID_String Label;                    // +0x04 CreateID(value 0)
    std::vector<int> Indices;           // +0x08 values 1.., unique, in first-seen order
    // +0x18 / +0x24: the zone's two path ends, written by the FPatrolPath ctor through
    // FSystemZone::GetPathEnds; the enumerator leaves them unset (uninitialised in baseline).
    Vector Start = {};
    Vector End = {};
    unsigned int Usage = 0;             // +0x30 "usage": trade 0x10, patrol 0x01
    bool bMissionEligible = false;      // +0x34 "mission_eligible"
};

// The ZoneEnumerator the routine hands to pub::System::EnumerateZones (06f75358); vtable
// , slot 0 = the routine.
struct FSystemZoneEnumerator : public pub::System::ZoneEnumerator
{
    FSystemInfo* System;                        // +0x04
    bool bLoadEncounters;                       // +0x08
    std::vector<FSystemZone>* Zones;            // +0x0c
    std::vector<FZonePathLabel>* PathLabels;    // +0x10

    // (RET 8).
    void Slot00(unsigned int const* ZoneId, INI_Reader* Reader) override;
};

// (RET 0xc): parse one [Zone] section into Zone / PathLabel; false = drop.
bool ParseSystemZone(FSystemZone* Zone, INI_Reader* Reader, FZonePathLabel* PathLabel,
                     bool bLoadEncounters);

// ---------------------------------------------------------------------------------------
// TU58 -- SystemInfo.
// ---------------------------------------------------------------------------------------

// A jump gate / jump hole of the system ([Object] archetype jumpgate / jumphole), +0x10c
// vector element, baseline 0x1c bytes (plain copy).
struct FSystemJump
{
    unsigned int ObjectId;      // +0x00 pub::GetNicknameId(nickname)
    int Type;                   // +0x04 1 = jumpgate, 2 = jumphole
    Vector Position;            // +0x08 "pos"
    unsigned int GotoSystem;    // +0x14 "goto" value 0 (pub::GetSystemID)
    unsigned int GotoObject;    // +0x18 "goto" value 1 (pub::GetNicknameId)
};

// A Trade_Lane_Ring [Object], value of the loader's local map keyed by the ring nickname
// (baseline 0x4c bytes); consumed by the routine.
struct FTradeLaneRingInfo
{
    unsigned int ObjectId;          // +0x00
    Vector Position;                // +0x04 "pos"
    // +0x10: 0x30-byte transform copied whole (the routine). Rotation = "rotate" (identity
    // when absent); the loader zeroes the translation (06f74de3-06f74dd5) and never sets it.
    FTransform Placement;
    unsigned int PrevRing;          // +0x40 "prev_ring"
    unsigned int NextRing;          // +0x44 "next_ring"
    unsigned int Reputation;        // +0x48 "reputation" (uninitialised when absent)
};

// One ring of a trade lane, baseline 0x44 bytes (member-wise copy the routine). Built by
// from the loader's ring map and by the routine from the live ring objects.
struct FTradeLaneRing
{
    unsigned int ObjectId;          // +0x00
    Vector Position;                // +0x04
    FTransform Placement;           // +0x10 translation always zero
    unsigned int Reputation;        // +0x40 ring "reputation"; -1 from the routine
};

// +0x88 element of FTradeLane, baseline 0x18 bytes (dtor the routine): the zones around one
// ring whose population can attack the lane. Only the routine writes it (06f777a2-06f777d1),
// from the routine.
struct FTradeLaneRingCandidates
{
    int RingIndex = 0;                                         // +0x00 index into Rings
    // +0x04: {patrol zone, its tradelane_attack} for the zones near the ring with a patrol
    // path whose population is partly hostile to the ring's faction.
    std::vector<std::pair<FSystemZone*, float>> Candidates;
    float MaxWeight = 0.0f;                                    // +0x14 max of .second
};

// A trade lane of the system, SystemInfo +0xe8 element, baseline 0x98 bytes: copy ctor
// , dtor the routine (member-wise). Default values from the routine /
// (06f76b70-06f76bf2).
struct FTradeLane
{
    std::vector<FTradeLaneRing> Rings;          // +0x00 prev_ring -> next_ring order
    Vector StartPosition;                       // +0x10 ring with no prev_ring (else unset)
    Vector EndPosition;                         // +0x1c ring with no next_ring (else unset)
    FTransform Placement;                       // +0x28 rotation of the first ring taken
    // +0x58 / +0x68 (copy the routine): the ids of the ships travelling the lane forward
    // (towards the last ring) / backward. the routine appends (GameSupport msg 0x16 06efee51 /
    // ), the routine removes (GameSupport msg 0x17 06efed4b, SpaceSupport 06f5d96a).
    std::vector<unsigned int> ForwardShips;
    std::vector<unsigned int> BackwardShips;
    // +0x78 / +0x7c: the game time a ship last left ForwardShips / BackwardShips
    // (the routine); -1 until then.
    float LastForwardLeaveTime = -1.0f;
    float LastBackwardLeaveTime = -1.0f;
    float MaxWeight = 0.0f;                     // +0x80 the routine: max over RingCandidates
    // +0x84: the game time of the last trade-lane attack roll (the routine); 0 = never.
    float LastAttackTime = 0.0f;
    std::vector<FTradeLaneRingCandidates> RingCandidates;  // +0x88 the routine

    FTradeLane();
};

// TU53 the routine (cdecl): squared distance from Point to the segment Start-End;
// OutClosest receives the nearest point of the segment. Lives here because the trade-lane
// queries of SystemInfo need it; SpaceSupport calls this one too.
// baseline returns the x87 value (float10); double keeps the extended compare of the callers.
double __cdecl DistanceSquaredToSegment(Vector const* Point, Vector const* Start,
                                        Vector const* End, Vector* OutClosest);

// TU53 the routine (cdecl; callers TU58 06f730ff, 06f73ca3): true when Point is no farther
// than *MaxDistance from the viewer and inside the cone of full angle *ConeAngle degrees
// around the viewer's -z axis (the third orientation column, negated). Both lengths use the
// reciprocal square root with one Newton step, as baseline does. Lives here with
// its only callers (orchestrator decision 2026-09-30).
bool __cdecl IsPointInViewCone(Vector const* Point, FSpaceObjPlacement const* Viewer,
                               float const* MaxDistance, float const* ConeAngle);

// The weighted draw baseline inlines over a range (TU58 the routine 06f78418 / 06f785bb /
// , BasePop the routine 06eb2ef3): the x87 sum of the weights is compared with 0
// before its float store; Target = float(rand() * 2^-15 (the field) * float(Sum)); the
// first element whose running x87 sum is not below Target (FCOMP, TEST AH,0x41 JNP: below or
// equal) is taken. An empty range, a zero sum or no match give End, which the baseline callers
// dereference as it is.
template <typename T, typename WeightFn>
T* PickByWeight(T* Begin, T* End, WeightFn GetWeight)
{
    double Sum = 0.0;
    for (T* It = Begin; It != End; ++It)
        Sum += GetWeight(*It);
    if (Begin == End || Sum == 0.0)
        return End;
    float const SumStored = static_cast<float>(Sum);
    float const Target = static_cast<float>(
        static_cast<double>(rand()) * (1.0 / 32768.0) * static_cast<double>(SumStored));
    double Running = 0.0;
    for (T* It = Begin; It != End; ++It)
    {
        Running += GetWeight(*It);
        if (static_cast<double>(Target) <= Running)
            return It;
    }
    return End;
}

// Mapped value of FSystemInfo +0xc8. The tree node is 0x20 bytes (links +0/+4/+8, a
// 16-byte value_type at +0xc, colour +0x1c, isnil +0x1d); the value_type is copied as four
// dwords by the routine (copy the routine) and has no destructor (the routine frees the
// nodes only). No baseline code inserts into, looks up or iterates this map -- every
// non-stack +0xc8..+0xd4 operand of the listing was checked: the only FSystemInfo sites are
// the ctor (06f722b8), dtor (06f75de3), copy ctor (06f79f75) and the pair<id, FSystemInfo>
// copy the routine (06f2f4b9) -- so the key/mapped split and the field types are not
// derivable. TODO(CNT-POP): the 4-byte key + three dwords are an assumption.
struct FSystemInfoField0xc8Value
{
    unsigned int Field_0x0 = 0;
    unsigned int Field_0x4 = 0;
    unsigned int Field_0x8 = 0;
};

// baseline class (0x124 bytes) derived from CommReferrable without overriding any slot:
// vtable 06fb93c4 = the three CommReferrable import thunks 06fa8f0c/06fa8f06/06fa8f00.
// Ctor the routine, copy ctor the routine and dtor the routine are member-wise.
struct FSystemInfo : public CommReferrable
{
    unsigned int SystemId;                          // +0x48
    int LoadCount = 0;                              // +0x4c the routine / the routine
    // +0x50: the zone paths, one per path_label (the routine); the routine keeps them.
    std::vector<FPatrolPath> Paths;
    std::vector<FSystemZone> Zones;                 // +0x60
    std::map<unsigned int, FSystemZone*> ZoneById;  // +0x70 points into Zones
    unsigned int LocalFaction = 0;                  // +0x84 [System] local_faction
    // +0x88: the solar NPCs of the system's [Object] sections, only filled by the full
    // load (the routine) and emptied by the full unload (the routine).
    std::vector<FSolarNpc> SolarNpcs;
    // +0x98: space object ids the mission scripts created in this system (the routine,
    // callers TU38 06f16f70 and TU61 06f92d42); the routine drops one (and frees its
    // vibe), the routine / the routine destroy them all.
    std::vector<unsigned int> MissionObjects;
    // +0xa8: ids flagged by RM the routine (06f8a9a3, flag = 1); dropped together with
    // +0x98. TODO(CNT-POP): no reader besides the removers found yet -- meaning unknown.
    std::vector<unsigned int> Field_0xa8;
    // +0xb8: the player states in the system, added uniquely by the routine (SpaceSupport
    // ); searched through the player's ship (+0x10) by the routine / the routine.
    std::vector<FContentPlayerState*> Players;
    // +0xc8: a map only ever constructed (the routine at 06f722cb), copied (the routine)
    // and destroyed (the routine); see FSystemInfoField0xc8Value.
    std::map<unsigned int, FSystemInfoField0xc8Value> Field_0xc8;
    // +0xdc: the space objects (NPC ships) of the system: the routine appends and sets
    // their system, the routine removes; node value = the object.
    std::list<FContentSpaceObj*> SpaceObjs;
    std::vector<FTradeLane> TradeLanes;             // +0xe8 the routine / the routine
    // +0xf8: ring id -> its lane in TradeLanes (the routine 06f7768e; read by
    // / the routine / the routine).
    std::map<unsigned int, FTradeLane*> TradeLaneByRing;
    std::vector<FSystemJump> Jumps;                 // +0x10c
    bool bRpopSolarDetection = true;                // +0x11c
    // +0x120: only non-null while an encounter load runs (the routine creates it and
    // deletes it with every value after the zones are read).
    std::map<unsigned int, FEncounterParameters*>* EncounterParameters = nullptr;

    // (RET 4).
    explicit FSystemInfo(unsigned int const* InSystemId);

    // (RET 4): LoadFromReader(nullptr, bLoadEncounters).
    void Load(bool bLoadEncounters);
    // (RET 8): reference-counted; only the first call reads the system.
    void LoadFromReader(INI_Reader* Reader, bool bLoadEncounters);
    // the last release clears the jumps and the zones.
    void Unload();
    // (RET 4): [Object] sections -> SolarNpcs, [SystemInfo]
    // rpop_solar_detection; ends with Reader->reset().
    void LoadSolarNpcs(INI_Reader* Reader);
    // the full load (solar NPCs, then LoadFromReader with encounters).
    // Caller SpaceSupport 06f5e356.
    void LoadFull();
    // the full unload. Caller SpaceSupport 06f5e30c.
    void UnloadFull();

    // (RET 4): append Player unless it is already listed.
    void AddPlayer(FContentPlayerState* Player);
    // (RET 4): drop Player from Players (when listed); in single player then
    // DestroyObjectsIfUnoccupied. Caller SpaceSupport 06f5defd.
    void RemovePlayer(FContentPlayerState* Player);
    // (RET 4, the argument is not read): in single player,
    // DestroyObjectsIfUnoccupied. Caller SpaceSupport 06f5e0f6 (passes the routine(&id)).
    void OnPlayerChanged(FContentPlayerState* Unused);
    // when no player of Players has a ship (+0x10), destroys the space
    // objects of SpaceObjs (the routine) and the mission objects (Destroy +
    // RemoveMissionObject). Callers 06f72c81, 06f72c9f, UnloadFull 06f75bc7.
    void DestroyObjectsIfUnoccupied();

    // (RET 4): push_back into MissionObjects.
    void AddMissionObject(unsigned int SpaceObjId);
    // (RET 4): erase SpaceObjId from MissionObjects -- when it was there, its
    // reputation (pub::SpaceObj::GetRep) is freed on success -- then from Field_0xa8.
    void RemoveMissionObject(unsigned int SpaceObjId);
    // (RET 8): bFlag adds SpaceObjId to Field_0xa8 unless present, !bFlag
    // erases it.
    void SetObjectFlag(unsigned int SpaceObjId, bool bFlag);
    // &MissionObjects. Caller TU56 06f7065a.
    std::vector<unsigned int>* GetMissionObjects();
    // Destroy(id, DESTROY_NORMAL) + the RemoveMissionObject body (inlined)
    // for every id of a copy of MissionObjects. Caller TU38 06f171cf (tail jump).
    void DestroyMissionObjects();
    // (RET 4): append Object to SpaceObjs and set its system to this
    // (the routine). Callers TU43 06f348c5, TU53 06f60b05.
    void AddSpaceObj(FContentSpaceObj* Object);
    // (RET 8): unlink the first object with this space object id; deleted
    // through its virtual dtor when bDelete. Callers TU43 06f34814, TU53 06f5db3a.
    void RemoveSpaceObj(unsigned int SpaceObjId, bool bDelete);
    // (RET 4): the ship (+0x10) of the first player in Players whose ship has
    // this space object id, else the first object of SpaceObjs with it, else null.
    FContentSpaceObj* FindShip(unsigned int SpaceObjId);
    // (RET 4): as FindShip; on a miss, the LAST solar NPC with this id (the
    // loop over SolarNpcs does not stop at a match), else null.
    FContentSpaceObj* FindShipOrSolar(unsigned int SpaceObjId);
    // (RET 4): the first object of SpaceObjs with this space object id, or
    // null (21 callers).
    FContentSpaceObj* FindSpaceObj(unsigned int SpaceObjId);
    // (RET 4): the first object of SpaceObjs whose nickname (+0xd8) equals
    // Nickname (baseline takes a TString and compares its chars at +4), or null. Callers
    // TU40 06f28444 / 06f28945.
    FContentSpaceObj* FindSpaceObjByNickname(char const* Nickname);
    // (RET 0xc): every object of SpaceObjs, every solar NPC and every player's
    // ship gets SendPendingComm(Arg1, Key, 1). Caller SpaceSupport 06f5dc40.
    void SendPendingComms(unsigned int Arg1, unsigned int Unused2, unsigned int Key);

    // (RET 0xc): true when the ship of a player of Players is within *Radius
    // of Point (squared distance <= Radius^2, compared against the float square); then
    // *OutDistanceSq = -1. Otherwise false and *OutDistanceSq = the smallest squared
    // distance to a player's ship (-1 when no player has a ship).
    bool IsPlayerShipNear(Vector const* Point, float const* Radius, float* OutDistanceSq);
    // (RET 0xc; caller SpaceSupport 06f5d0c3): Out is cleared, then receives
    // every player of Players whose ship is within *Radius of Point.
    void FindPlayersNear(Vector const* Point, float const* Radius,
                         std::vector<FContentPlayerState*>* Out);
    // (RET 0xc; callers TU53 06f62c68 / 06f62d62 / 06f62e7b): some player's
    // ship sees Point (IsPointInViewCone with the ship's placement).
    bool IsSeenByPlayerShip(Vector const* Point, float const* MaxDistance,
                            float const* ConeAngle);
    // (RET 8): some player's ship has a +0x120 pair (HasField120) and the
    // space object of its +0x124 id is within *Radius of Point.
    bool IsNearPlayerField124Object(Vector const* Point, float const* Radius);
    // (RET 8): some player's ship has a +0x120 pair and Point is within *Radius
    // of the segment between the space objects of its +0x120 and +0x124 ids.
    bool IsNearPlayerField120Segment(Vector const* Point, float const* Radius);
    // (RET 0xc): creates one NPC ship in this system from a copy of *Params
    // (System = this). Without a dock target: new FContentNpcShip, *OutShip = it (when
    // OutShip), appended to SpaceObjs, controller 0 gets message 0x1010 with the ship;
    // returns 0. With one (DockTarget & 0x3fffffff): the result of the dock launch queue
    // TU53 the routine (QueueDockLaunch, DockQueues.h).
    int CreateNpcShip(FContentNpcShipParams const* Params, unsigned int DockTarget,
                      FContentNpcShip** OutShip);
    // (RET 0xc; callers TU29 06ef5879 / 06ef588c, TU31 06efc1b9, TU38
    // , TU59 06f83a66, TU61 06f92033): CreateNpcShip for every block of ParamsList;
    // each created ship goes to OutShips (when both are non-null). A non-zero result of a
    // block owned by SpacePop (ControllerId == GSpacePopControllerId) stops the loop and is
    // returned; otherwise 0.
    int CreateNpcShips(std::list<FContentNpcShipParams> const* ParamsList,
                       unsigned int DockTarget, std::list<FContentNpcShip*>* OutShips);
    // (RET 8; caller TU38 06f14b9e): the AddSpaceObj body inlined, then
    // pub::SpaceObj::JumpIn(ship, GateId) (result ignored).
    void AddJumpingInSpaceObj(FContentSpaceObj* Object, unsigned int GateId);

    // (RET 4; callers TU46 06f477f4 with 1 / 06f47826 with 0 when the player
    // ship's +0x11c changes): every object of SpaceObjs, taken as an NPC ship, gets
    // SetField1b1(!bClear).
    void SetNpcShipsField1b1(bool bClear);
    // (RET 4, x87 result): how fast Ship's remaining life (+0x1c8) runs down.
    // -1 when a player is close: a player ship within 2500, in single player one whose
    // 70 degree view cone reaches it within 4000, or a player ship's +0x124 object within
    // 4000 / +0x120..+0x124 segment within 2500 of it; also -1 when the ship itself has a
    // +0x120 pair. Otherwise min(15, (distance to the nearest player ship - 2500) * 0.01 + 1),
    // 15 when that distance is at most 2500 or no player has a ship.
    double GetNpcShipLifeRate(FContentNpcShip* Ship);
    // (RET 4, the argument is not read; caller SpaceSupport 06f5d699 passes the
    // tick the field): the ids of Field_0xa8 with no player ship within 10000 are
    // destroyed (DESTROY_NORMAL) and removed (the RemoveMissionObject body inlined).
    void DestroyFlaggedObjectsAwayFromPlayers(float Unused);
    // (RET 4; caller SpaceSupport 06f5d64c with the tick the field): runs
    // down the remaining life of the NPC ships of SpaceObjs and destroys the expired ones
    // (whole formations at once) that no player targets.
    void UpdateNpcShipLives(float DeltaTime);

    // (RET 8): *Out = a copy of (*EncounterParameters)[Id] (the routine
    // copies the value found by lower_bound + key test; baseline reads the map's head node
    // for an unknown id). Caller: the zone "encounter" key (TU30 06ef909c).
    void GetEncounterParameters(unsigned int Id, FEncounterParameters* Out);

    // (RET 4): the zone with this id, or null.
    FSystemZone* FindZone(unsigned int ZoneId);
    // (RET 4): of the zones containing Position, the one with the smallest
    // radius, or null.
    FSystemZone* FindSmallestZoneAt(Vector const* Position);

    // (RET 4): sorts the zones' path labels, builds one FPatrolPath per label
    // (emptying Labels), points each zone at its path and registers the system's objects
    // near the paths. Returns the path count.
    int BuildPatrolPaths(std::vector<FZonePathLabel>* Labels);

    // &TradeLanes.
    std::vector<FTradeLane>* GetTradeLanes();
    // (RET 0xc): Out = the lanes whose start-end segment passes closer than
    // Radius to Position (every lane when Radius == -1), each once.
    void FindTradeLanesNear(Vector const* Position, float Radius,
                            std::vector<FTradeLane*>* Out);
    // (RET 0xc): as FindTradeLanesNear, but Out receives copies of the lanes'
    // ring lists, unique by their first ring.
    void FindTradeLaneRingListsNear(Vector const* Position, float Radius,
                                    std::vector<std::vector<FTradeLaneRing>>* Out);
    // (RET 8; callers GameSupport 06efed91, SpacePop 06f5c523, TU53 06f63f2d):
    // *OutLane = the lane of the ring *RingId (TradeLaneByRing), true; null and false when
    // the ring is unknown.
    bool FindTradeLaneByRing(unsigned int const* RingId, FTradeLane** OutLane);
    // (RET 8; callers TU37 goto guide 06f08881 / 06f089cc; added by CNT-AI6):
    // *OutRings = the rings of the lane of the ring *RingId (TradeLaneByRing), true; false
    // (OutRings untouched) when the ring is unknown.
    bool GetTradeLaneRingsByRing(unsigned int const* RingId,
                                 std::vector<FTradeLaneRing>* OutRings);
    // (RET 4): rebuilds TradeLanes from the loader's ring map by following
    // prev_ring / next_ring; empties Rings. Returns the lane count.
    int BuildTradeLanes(std::map<unsigned int, FTradeLaneRingInfo>* Rings);
    // (caller LoadFull 06f759cb): TradeLanes rebuilt from the live trade lane
    // rings of the system (ScanObjects type 0x80, chained with GetTradelaneNextAndPrev);
    // then every ring enters TradeLaneByRing (the map is not cleared first) and gets its
    // attack candidates (the routine); each lane's MaxWeight = the largest candidate
    // weight. Returns the lane count.
    int RebuildTradeLanes();
    // (RET 0x10; Ring is not read): the patrol zones at / near Position
    // (CollectSpacePopZones) whose faction population is partly hostile to *Affiliation and
    // whose tradelane_attack is positive go to Out->Candidates with that weight.
    void CollectRingCandidates(FTradeLaneRing const* Ring, Vector const* Position,
                               unsigned int const* Affiliation, FTradeLaneRingCandidates* Out);
    // (RET 0x10; callers SpacePop msg 0x16 06f5c5e2 / 06f5c64c): a player ship
    // (PlayerShip) entered *Lane at ring EntryIndex travelling *Direction (1 / -1). Unless the
    // player's flags hold 8, at most every 180 s and with the lane's MaxWeight as chance, a ring
    // ahead (beyond the next one) with no player ship within 5000 is drawn by weight, then one
    // of its attack zones, then an encounter of that zone with factions hostile to the ring's
    // owner: a "TLParams" random encounter of that encounter's first formation is spawned at
    // the ring (Spawn(false)). No destroyed ring (relative health 0) may be on the lane.
    void SpawnTradeLaneAttack(FContentPlayerShip* PlayerShip, int const* Direction,
                              int EntryIndex, FTradeLane** Lane);
    // (RET 0xc; callers GameSupport msg 0x16 06efee51 / 06efee8e): *ShipId joins
    // the ForwardShips (*Direction == 1) or BackwardShips of the lane of ring *RingId. baseline
    // reads the map's head node for an unknown ring.
    void AddTradeLaneShip(unsigned int const* RingId, int const* Direction,
                          unsigned int const* ShipId);
    // (RET 8; callers GameSupport msg 0x17 06efed4b, SpaceSupport 06f5d96a):
    // *ShipId leaves the lane of ring *RingId (every lane when *RingId is 0): the first match
    // in ForwardShips (LastForwardLeaveTime = now), and -- until some lane matched -- in
    // BackwardShips (LastBackwardLeaveTime = now).
    void RemoveTradeLaneShip(unsigned int const* ShipId, unsigned int const* RingId);
    // (6 callers): &Players.
    std::vector<FContentPlayerState*>* GetPlayers();

    // (RET 0x1c): one-line comm through SendSpaceComm (TextCount 1, mood 0,
    // interrupt). Returns pub::SpaceObj::SendComm's result.
    int SendComm(unsigned int SenderId, unsigned int TargetId, unsigned int TextId,
                 unsigned int VoiceId, unsigned int CommType, Costume const* CommCostume,
                 float Duration);

    // (RET 8; added by CNT-AI5 for the TU40 directives, other callers TU21
    // , TU27 06ef26fe / 06ef27b5 / 06ef288a): the space object id a mission name
    // stands for. Empty -> 0; "Player" -> the ship of the player whose live mission is
    // ControllerId (FindContentPlayerStateByLiveMission), else in single player the first
    // player's ship; else the first SpaceObjs entry with this nickname; else
    // pub::GetNicknameId(name) when IsSpaceObjTypeSet accepts it, 0 otherwise.
    unsigned int FindObjectIdByName(TString<0x30> const* Name, unsigned int ControllerId);

    // --- Spawn placement helpers (added by CNT-GS9 for TU53 SpawnPlacement.cpp) ---
    // (RET 0x20; caller the routine): Out gets every object of the system
    // within Range of *Position (ScanObjects, 32 at most, type *TypeMask) that is not
    // *ExcludeId, not a gate locked for NPCs, a dock target ships of ClassIds could use
    // (CanUseDockTarget with ShipId 0) and -- unless its type has a bit of 0xc40 -- a base of
    // the group (*ShipRep, or *Group when it is -1) whose docking target is set. Returns
    // Out's size.
    int CollectDockTargets(std::vector<unsigned int> const* ClassIds, int const* ShipRep,
                           unsigned int const* Group, unsigned int const* ExcludeId,
                           Vector const* Position, float Range, unsigned int const* TypeMask,
                           std::vector<unsigned int>* Out);
    // (RET 0x1c; callers TU53 the routine, the routine): a random one of
    // CollectDockTargets, 0 when there is none.
    unsigned int PickDockTarget(std::vector<unsigned int> const* ClassIds, int const* ShipRep,
                                unsigned int const* Group, unsigned int const* ExcludeId,
                                Vector const* Position, float Range,
                                unsigned int const* TypeMask);
    // (RET 0x24; caller TU53 the routine x3): as PickDockTarget over the
    // objects of Path's points First..Last (FPatrolPath::FindObjectsInRange with *TypeMask),
    // each farther than MinDistance (compared with the squared distance) from *ExcludeId's
    // position, without the docking-target test; 0 when there is none.
    unsigned int PickPathDockTarget(std::vector<unsigned int> const* ClassIds,
                                    int const* ShipRep, unsigned int const* Group,
                                    unsigned int const* ExcludeId, FPatrolPath* Path,
                                    int First, int Last, float MinDistance,
                                    unsigned int const* TypeMask);
    // (RET 8; caller TU53 the routine): 0 when either ring id is 0; else 1
    // when *SecondRing is the ring right after *FirstRing on *FirstRing's lane, -1 otherwise.
    // baseline dereferences the null lane of an unknown ring.
    int GetTradeLaneRingOrder(unsigned int const* FirstRing, unsigned int const* SecondRing);
    // (RET 0x10; caller TU53 the routine x2): *OutDistance = the distance from
    // *Position to the nearest ship travelling ring *RingId's lane in *Direction (1 forward),
    // negative when *Position lies behind it along the lane's z axis; false (untouched) when
    // no ship travels that way.
    bool GetLaneShipDistance(int const* Direction, unsigned int const* RingId,
                             Vector const* Position, float* OutDistance);
    // (RET 8, x87 result; caller TU53 the routine x2): the game time since a
    // ship last left ring *RingId's lane in *Direction, -1 when none has.
    double GetTimeSinceLaneLeave(int const* Direction, unsigned int const* RingId);
};

// TU53 the routine (cdecl, 11 args): forwards to pub::SpaceObj::SendComm. The first
// argument (the caller's SystemInfo) is not read. Lives here with the SystemInfo wrapper;
// SpaceSupport calls this one too.
int __cdecl SendSpaceComm(void const* Unused, unsigned int SenderId, unsigned int TargetId,
                          unsigned int* TextIds, int const* TextCount, unsigned int VoiceId,
                          unsigned int CommType, Costume const* CommCostume, unsigned int Mood,
                          float Duration, bool bInterrupt);

// static init the branch, atexit the routine.
extern std::map<unsigned int, FSystemInfo> GSystemInfos;

std::map<unsigned int, FSystemInfo>* __cdecl GetSystemInfos();
FSystemInfo* __cdecl FindSystemInfo(unsigned int const* SystemId);
void __cdecl BuildSystemInfos();
void __cdecl ClearSystemInfos();
void __cdecl LoadDynSysInfo(INI_Reader* Reader);
void __cdecl VisitSystemInfos();
