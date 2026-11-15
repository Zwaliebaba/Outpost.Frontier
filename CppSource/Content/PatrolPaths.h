// Author: Giperion.

#pragma once

// Content.dll: the zone path of a system -- the zones sharing one "path_label", chained in
// path-index order into a polyline of points. The code lives in the COMDAT run at the head
// of TU45 (06f37110-06f38e00); SystemInfo (TU58) owns the paths (FSystemInfo +0x50, built by
// ) and every zone of a path points at it (FSystemZone +0x0c, TU64 the routine).
// baseline's own class name is not in the binary (no RTTI). Lane CNT-POP.

#include "common_id.h"      // ID_String
#include "common_ini.h"     // Vector

#include <map>
#include <vector>

struct FSystemInfo;
struct FZonePathLabel;

// +0x00 vector element, baseline 0x14 bytes (element copy the routine, dtor the routine): a
// space object near the path and the indices of the path points it is near.
struct FPatrolPathObject
{
    unsigned int ObjectId = 0;          // +0x00
    std::vector<int> PointIndices;      // +0x04 indices into FPatrolPath::Points
};

// One element of FPatrolPath::GetZoneSegments, baseline 8 bytes (copy the routine): the two
// points of FPatrolPath::Points one path step spans.
struct FPatrolPathSegment
{
    int FirstPoint;                     // +0x00 step index
    int SecondPoint;                    // +0x04 step index + 1
};

// baseline 0x44 bytes. Ctor the routine; copy and dtor are member-wise (the ctor's unwind
// funclets destroy +0x24 the routine, +0x14 and +0x00).
struct FPatrolPath
{
    std::vector<FPatrolPathObject> Objects;         // +0x00 RegisterObject the routine
    ID_String Label;                                // +0x10 the shared "path_label"
    std::vector<Vector> Points;                     // +0x14 the path polyline
    // +0x24: zone id -> the steps (indices of the path's zones in path-index order) of that
    // zone; step i spans Points[i]..Points[i + 1].
    std::map<unsigned int, std::vector<int>> ZoneSteps;
    FSystemInfo* System = nullptr;                  // +0x38
    bool bLoop = false;                             // +0x3c the last point meets the first
    bool bMissionEligible = false;                  // +0x3d first label's "mission_eligible"
    unsigned int Usage = 0;                         // +0x40 first label's "usage" bits

    // (RET 0xc). baseline takes pointers to the two iterators of the label range
    // [First, Last) of one path_label; the labels' Start / End are written here.
    FPatrolPath(FZonePathLabel* const* First, FZonePathLabel* const* Last,
                FSystemInfo* InSystem);

    // (RET 8): records ObjectId against every point closer than 5000 to
    // Position; true when at least one point was.
    bool RegisterObject(unsigned int ObjectId, Vector const* Position);
    // (RET 0x10): Out = copies of the objects (each once) of type TypeMask near
    // a point with index in [First, Last] (-1 = the path's first / last point); when
    // First > Last the range wraps around the end of the path.
    void FindObjectsInRange(std::vector<FPatrolPathObject>* Out, int First, int Last,
                            unsigned int TypeMask);
    // (RET 8): Out = the point indices of ObjectId, empty when it is not
    // registered.
    void GetObjectPointIndices(unsigned int const* ObjectId, std::vector<int>* Out);
    // (RET 8): Out = the segments of ZoneId's steps; false (Out empty) when the
    // zone is not on the path.
    bool GetZoneSegments(unsigned int ZoneId, std::vector<FPatrolPathSegment>* Out);
    // (RET 4): Out = the zone ids of the path in ascending order; returns the
    // count.
    int GetZoneIds(std::vector<unsigned int>* Out);
};
