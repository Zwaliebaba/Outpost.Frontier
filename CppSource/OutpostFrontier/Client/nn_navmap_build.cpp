// Author: Giperion.

// NeuroNetNavMap -- building the system view: its items, trade lanes, zones and the player /
// waypoint markers. See nn_navmap.h. baseline Client\Interface\NeuroNet\NavMap.cpp.

#include "nn_navmap.h"
#include "character_select_equip.h" // GetPlayerStarSystemIdbaseline, GetPlayerVisitFlagsbaseline
#include "client_cliobj.h"          // Archetype::CliSolarShapeName (companion +0x60)
#include "nn_ui_pulse.h"            // G_NnPaletteArgb_679ba4
#include "common_archetype.h"       // Archetype::GetSolar, Solar
#include "common_id.h"              // CreateID
#include "common_universe.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace
{
    // / the field.
    float const kNavMapRadToDeg = 57.2957763671875f;
    float const kNavMapFullTurn = 360.0f;
    // the system view's map extents.
    float const kNavMapSystemExtent = 112000.0f;
    // a sun's alpha.
    float const kNavMapSunAlpha = 0.99f;
    // the waypoint markers' scale.
    float const kNavMapWaypointScale = 0.6f;

    // The 0x118-byte [Object] record, its four TStrings emptied the way every NavMap.cpp
    // caller does before ISystem slot 16 (length 0, first char 0).
    void ClearSystemObject(Universe::FSystemObject* Object)
    {
        Object->Nickname.Length = 0;
        Object->Nickname.Data[0] = '\0';
        Object->Reputation.Length = 0;
        Object->Reputation.Data[0] = '\0';
        Object->NextRing.Length = 0;
        Object->NextRing.Data[0] = '\0';
        Object->PrevRing.Length = 0;
        Object->PrevRing.Data[0] = '\0';
    }
}

bool NeuroNetNavMap::IsSkippedSystem(Universe::ISystem const* System) const
{
    // Inlined in baseline (e.g. 0x0049187b..0x00491892, 0x00491b44..0x00491b5b): the system's id
    // against the four SkipSystemIds.
    unsigned const Id = System->GetId();
    for (int Index = 0; Index < 4; ++Index)
    {
        if (SkipSystemIds[Index] == Id)
            return true;
    }
    return false;
}

void NeuroNetNavMap::BuildSystemItems()
{
    // .
    MapExtentX = kNavMapSystemExtent;
    MapExtentZ = kNavMapSystemExtent;
    LaneSegments.clear(); // (first, last)
    LaneRings.clear();    // (last, last, first), then last = first

    Universe::FSystemObject Object;
    ClearSystemObject(&Object);

    // the object count (read before the null test, as in baseline),
    // none for a null or a skipped system; SystemScale = the system's NavMapScale.
    unsigned Count = static_cast<unsigned>(CurrentSystem->GetObjectCount());
    if (CurrentSystem == nullptr || IsSkippedSystem(CurrentSystem))
        Count = 0;
    SystemScale = CurrentSystem->GetNavMapScale();

    for (unsigned Index = 0; Index < Count; ++Index)
    {
        if (!CurrentSystem->GetSystemObject(Index, &Object))
            continue;
        Archetype::Solar const* const Arch = Archetype::GetSolar(Object.ArchetypeId);
        if (Arch == nullptr)
            continue;

        unsigned Type = Arch->ObjectType;
        if ((Type & 0x80) != 0)
        {
            // a trade lane ring is remembered; one in the middle of a
            // lane (both links set) gets no item of its own.
            FNavMapRing Ring;
            Ring.Nickname.Length = 0;
            Ring.Nickname.Data[0] = '\0';
            Ring.NextRing.Length = 0;
            Ring.NextRing.Data[0] = '\0';
            Ring.PrevRing.Length = 0;
            Ring.PrevRing.Data[0] = '\0';
            Ring.Nickname = Object.Nickname.Data;
            Ring.Pos[0] = Object.Pos.x;
            Ring.Pos[1] = Object.Pos.y;
            Ring.Pos[2] = Object.Pos.z;
            Ring.ArchetypeId = Object.ArchetypeId;
            Ring.NextRing = Object.NextRing.Data;
            Ring.PrevRing = Object.PrevRing.Data; // (data, 0x20, prev_ring)
            LaneRings.push_back(Ring);            // (last, 1, &Ring)
            if (Object.PrevRing.Data[0] != '\0' && Object.NextRing.Data[0] != '\0')
                Type = 0;
        }

        if ((SolarTypeMask & Type) == 0 || Type == 0x20000000)
            continue;

        // moons and planets face the system centre.
        float Heading = 0.0f;
        Archetype::ICliObj* const Companion = Arch->Companion;
        if ((Type & 3) != 0)
        {
            double const A = -static_cast<double>(Object.Pos.x);
            double const B = -static_cast<double>(Object.Pos.z);
            double const Length = sqrt(B * B + A * A);
            // TEST AH,0x41 + JNE: only "greater than 0" computes a heading.
            if (Length > 0.0)
            {
                double const Inv = 1.0 / Length;
                float const CosAngle = static_cast<float>(Inv * A);
                float const SinAngle = static_cast<float>(Inv * B);
                Heading = static_cast<float>(acos(static_cast<double>(CosAngle)) *
                                             static_cast<double>(kNavMapRadToDeg));
                if (SinAngle > 0.0f)
                    Heading = static_cast<float>(static_cast<double>(kNavMapFullTurn) -
                                                 static_cast<double>(Heading));
            }
        }

        // the item, placed at the scaled position, with the solar's
        // radius, the companion's shape_name mesh, ids_name / ids_info, the heading, the
        // nickname id and the visit byte.
        unsigned const Id = CreateID(Object.Nickname.Data);
        double const Scale = static_cast<double>(SystemScale);
        float const X = static_cast<float>(static_cast<double>(Object.Pos.x) * Scale);
        float const Y = static_cast<float>(static_cast<double>(Object.Pos.y) * Scale);
        float const Z = static_cast<float>(Scale * static_cast<double>(Object.Pos.z));
        FNavMapItem* const Item = AddItem(
            0, static_cast<int>(Type), X, Y, Z, Arch->SolarRadius,
            Archetype::CliSolarShapeName(Companion), Object.IdsName, Object.IdsInfo, Heading, Id,
            Object.Visit);

        // the item is used without a null test, as in baseline.
        if ((Type & 4) != 0)
            Item->Mesh.UserFloats[4] = kNavMapSunAlpha;
        if (Object.Goto != 0)
            Item->System = Universe::get_system(Object.Goto);
    }

    // .
    BuildLaneSegments();
    if (CurrentSystem == nullptr || IsSkippedSystem(CurrentSystem))
        MapZones.clear(); // the element-wise erase(first, last) at 0x00491b9c..0x00491bce
    else
        BuildMapZones();
    if (!HasVisitedContent())
        LaneSegments.clear(); // (last, last, first), then last = first
}

void NeuroNetNavMap::AddPlayerMarker()
{
    // .
    float Pos[3];
    float Heading;
    PlayerMapPosition(Pos, &Heading);
    PlayerItem = AddItem(2, 0, Pos[0], Pos[1], Pos[2], ShipMarkerSize, "NNM_SM_INFO_POSITION",
                         0x219a, 0, Heading, 0, 0);
    // no null test, as in baseline.
    PlayerItem->PulsePhase = -1.0f;
    if (ViewMode != 0 && CurrentSystem->GetId() != GetPlayerStarSystemIdbaseline())
        PlayerItem->bShown = 0;

    // the ten waypoint markers, tinted with the pulse palette
    // colour (the the routine sequence inline) and scaled to 0.6.
    for (int Index = 0; Index < 10; ++Index)
    {
        char Path[0x200];
        sprintf(Path, "Interface\\NeuroNet\\NavMap\\NewNavMap\\waypoint%d.3db", Index);
        FClientMeshHolder* const Holder = &DigitMeshes[Index];
        ClientMeshLoadFromPath(Holder, Path, 0, true); // (path, 0, 1)
        ClientMeshHolderSetColor(Holder,
                                 reinterpret_cast<unsigned char const*>(&G_NnPaletteArgb_679ba4));
        NavMapScaleMeshRotation(Holder, kNavMapWaypointScale);
    }
}

void NeuroNetNavMap::BuildLaneSegments()
{
    // .
    int const Count = static_cast<int>(LaneRings.size());
    for (int Chain = 0; Chain < Count; ++Chain)
    {
        // a lane's first ring (no prev_ring, a next_ring) starts a
        // chain; it is marked used by emptying its nickname.
        FNavMapRing& First = LaneRings[Chain];
        if (First.PrevRing.Data[0] != '\0' || First.NextRing.Data[0] == '\0')
            continue;
        First.Nickname.Data[0] = '\0';
        TString<32> Name = First.NextRing; // REP MOVSD 9 dwords
        float Pos[3] = {First.Pos[0], First.Pos[1], First.Pos[2]};

        for (;;)
        {
            // the unused ring named Name (NEURONCOMPONENTS stricmp). baseline
            // starts the scan over, without end, when there is none.
            FNavMapRing* Next = nullptr;
            while (Next == nullptr)
            {
                for (int Index = 0; Index < Count; ++Index)
                {
                    FNavMapRing& Ring = LaneRings[Index];
                    if (Ring.Nickname.Data[0] == '\0')
                        continue;
                    if (_stricmp(Name.Data, Ring.Nickname.Data) == 0)
                    {
                        Next = &Ring;
                        break;
                    }
                }
            }

            // one segment, both ends scaled by SystemScale.
            FNavMapSegment Segment;
            double const EndScale = static_cast<double>(SystemScale);
            float const EndX = static_cast<float>(static_cast<double>(Next->Pos[0]) * EndScale);
            float const EndY = static_cast<float>(static_cast<double>(Next->Pos[1]) * EndScale);
            float const EndZ = static_cast<float>(static_cast<double>(Next->Pos[2]) * EndScale);
            double const StartScale = static_cast<double>(SystemScale);
            Segment.Start[0] = static_cast<float>(static_cast<double>(Pos[0]) * StartScale);
            Segment.Start[1] = static_cast<float>(static_cast<double>(Pos[1]) * StartScale);
            Segment.Start[2] = static_cast<float>(static_cast<double>(Pos[2]) * StartScale);
            Segment.End[0] = EndX;
            Segment.End[1] = EndY;
            Segment.End[2] = EndZ;
            LaneSegments.push_back(Segment); // (last, 1, &Segment)

            // the ring is used; follow its next_ring.
            Next->Nickname.Data[0] = '\0';
            Pos[0] = Next->Pos[0];
            Pos[1] = Next->Pos[1];
            Pos[2] = Next->Pos[2];
            if (Next->NextRing.Data[0] == '\0')
                break;
            Name = Next->NextRing.Data; // strlen, clamped to 0x1f, copied, NUL
        }
    }

    // the rings are dropped (CMP EBX,EBX: an erase of the whole
    // range, last = first).
    LaneRings.clear();
}

void NeuroNetNavMap::BuildMapZones()
{
    // .
    TerrainZoneCount = 0;
    TerrainRecheckTimer = 1.0f;
    MapZones.clear();   // (first, last)

    // three passes over the zone list; each records where it
    // started in MapZones.
    int PassStart[3];
    for (int Pass = 0; Pass < 3; ++Pass)
    {
        PassStart[Pass] = static_cast<int>(MapZones.size());
        std::list<Universe::FSystemZone> const* const Zones = CurrentSystem->GetZones();
        for (std::list<Universe::FSystemZone>::const_iterator It = Zones->begin();
             It != Zones->end(); ++It)
        {
            // baseline copies the 0xc0c record to the stack and reads the zone from the copy.
            Universe::IZone const* const Zone = It->Zone;
            unsigned long const Props = Zone->GetProperties();
            bool bAdd = false;
            if (Pass == 0)
                bAdd = (Props & 0x8000) != 0 && (Props & 0x30000) == 0;
            else if (Pass == 1)
                bAdd = (Props & 0x38000) == 0 && (Props & 0x7fc0) != 0;
            else
                bAdd = (Props & 0x30000) != 0;
            if (bAdd)
                MapZones.push_back(Zone); // (last, 1, &zone)
        }
    }

    // the second pass's zones bubble-sorted by (properties & 0x7fc0),
    // ascending, until a sweep swaps nothing.
    int const First = PassStart[1];
    int const Last = PassStart[2] - 1;
    for (;;)
    {
        if (First >= Last)
            break;
        bool bSorted = true;
        for (int Index = First; Index < Last; ++Index)
        {
            Universe::IZone const* const Right = MapZones[Index + 1];
            Universe::IZone const* const Left = MapZones[Index];
            unsigned long const RightKey = Right->GetProperties() & 0x7fc0;
            unsigned long const LeftKey = Left->GetProperties() & 0x7fc0;
            if (LeftKey > RightKey)
            {
                MapZones[Index] = Right;
                MapZones[Index + 1] = Left;
                bSorted = false;
            }
        }
        if (bSorted)
            break;
    }
}

bool NeuroNetNavMap::HasVisitedContent()
{
    // .
    Universe::FSystemObject Object;
    ClearSystemObject(&Object);

    // an object that is neither a lane ring nor a sun (0x84) -- or
    // has no solar archetype -- and is visited (the visit map's flags OR its own, bit 0).
    unsigned const Count = static_cast<unsigned>(CurrentSystem->GetObjectCount());
    for (unsigned Index = 0; Index < Count; ++Index)
    {
        if (!CurrentSystem->GetSystemObject(Index, &Object))
            continue;
        Archetype::Solar const* const Arch = Archetype::GetSolar(Object.ArchetypeId);
        if (Arch != nullptr && (Arch->ObjectType & 0x84) != 0)
            continue;
        unsigned char Visit = 0;
        GetPlayerVisitFlagsbaseline(CreateID(Object.Nickname.Data), &Visit, 0, 0);
        if (((Visit | Object.Visit) & 1) != 0)
            return true;
    }

    // a visited map zone (the visit map OR the zone's own byte,
    // IZone slot 32).
    int const ZoneCount = static_cast<int>(MapZones.size());
    for (int Index = 0; Index < ZoneCount; ++Index)
    {
        Universe::IZone const* const Zone = MapZones[Index];
        unsigned char Visit = 0;
        GetPlayerVisitFlagsbaseline(Zone->GetId(), &Visit, 1, 0);
        if (((Visit | Zone->GetVisit()) & 1) != 0)
            return true;
    }
    return false;
}
