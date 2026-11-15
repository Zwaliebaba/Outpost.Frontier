// Author: Giperion.

// NeuroNetNavMap -- the side panels (objectives, mission info, filters) and the universe
// view's jump links. See nn_navmap.h. baseline Client\Interface\NeuroNet\NavMap.cpp.

#include "nn_navmap.h"
#include "best_path.h"               // BestPathGetWaypointCount / BestPathGetWaypoint / First
#include "character_select_equip.h"  // GetPlayerStarSystemIdbaseline, GetPlayerVisitFlagsbaseline
#include "hud_target.h"              // NavMapSectorName (the routine)
#include "ids_rdl.h"                 // AppendTextMaybeXmlRdl (the routine)
#include "ids_strings.h"             // GetIdsStringWWithRawFallback (the routine)
#include "misc_util.h"               // FmtStrSprintf (the routine)
#include "nn_text_scratch.h"         // / the field / the field
#include "nn_ui_pulse.h"             // G_NnPaletteArgb_679bb0
#include "player_spawn.h"            // GetPlayerWaypointCountbaseline / GetPlayerWaypointbaseline
#include "rich_text_win.h"
#include "ui_scroll.h"
#include "common_text.h"             // FmtStr, RenderDisplayList, StyleNode
#include "common_universe.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

namespace
{
    // the filter title per mode (the routine 0x004a054e).
    unsigned const kNavMapFilterTitleIds[8] = {0x53a, 0x53b, 0x53c, 0x53d,
                                               0x550, 0x551, 0x1,   0x3c8};
}

// ----------------------------------------------------------------------------------------
// Jump links (universe view)
// ----------------------------------------------------------------------------------------

void NeuroNetNavMap::BuildJumpLinks()
{
    // .
    if (ViewMode != 0)
        return;

    // the routine erases every node; the records are NOT freed (baseline
    // leaks them here -- only the destructor frees what is left).
    JumpLinks.clear();

    for (std::list<FNavMapItem*>::iterator It = Items.begin(); It != Items.end(); ++It)
    {
        FNavMapItem* const From = *It;
        if (From->bShown == 0 || (From->Type & 4) == 0)
            continue;

        Universe::ISystem const* const System = From->System;
        int const ConnectionCount = System->GetConnectionCount();
        for (int Index = 0; Index < ConnectionCount; ++Index)
        {
            // the connection, its object's visit flags OR'd with its own.
            Universe::FSystemConnection Connection;
            System->GetConnection(static_cast<unsigned>(Index), &Connection);
            unsigned char VisitFlags = 0;
            GetPlayerVisitFlagsbaseline(Connection.ObjectId, &VisitFlags, 0, 0); // 0x0049764d
            unsigned char const Flags = static_cast<unsigned char>(VisitFlags | Connection.Visit);
            if ((Flags & 1) == 0)
                continue;

            // every shown system item the connection leads to.
            for (std::list<FNavMapItem*>::iterator ToIt = Items.begin(); ToIt != Items.end();
                 ++ToIt)
            {
                FNavMapItem* const To = *ToIt;
                if (To->bShown == 0 || (To->Type & 4) == 0)
                    continue;
                if (To->System->GetId() == Connection.GotoSystemId)
                    AddJumpLink(From, To);
            }
        }
    }
}

void NeuroNetNavMap::AddJumpLink(FNavMapItem* From, FNavMapItem* To)
{
    // .
    // the same pair is ignored; the reverse pair becomes two-way.
    for (std::list<FNavMapJumpLink*>::iterator It = JumpLinks.begin(); It != JumpLinks.end();
         ++It)
    {
        FNavMapJumpLink* const Existing = *It;
        if (Existing->From == From && Existing->To == To)
            return;
        if (Existing->From == To && Existing->To == From)
        {
            Existing->bBothWays = 1;
            return;
        }
    }


    FNavMapJumpLink* const Link = static_cast<FNavMapJumpLink*>(operator new(0x10));
    Link->From = From;
    Link->To = To;
    Link->Field_0x08 = 1;
    Link->bBothWays = 0;
    Link->Kind = 0;

    // both ends on the best path (a best-path waypoint's system, or the
    // player's own system) -> kind 2.
    bool bFromOnPath = GetPlayerStarSystemIdbaseline() == From->System->GetId();
    if (!bFromOnPath)
    {
        int const Count = BestPathGetWaypointCount();
        for (int Index = 0; Index < Count; ++Index)
        {
            if (BestPathWaypointSystem(Index) == From->System)
            {
                bFromOnPath = true;
                break;
            }
        }
    }
    bool bKindSet = false;
    if (bFromOnPath)
    {
        bool bToOnPath = GetPlayerStarSystemIdbaseline() == To->System->GetId();
        if (!bToOnPath)
        {
            int const Count = BestPathGetWaypointCount();
            for (int Index = 0; Index < Count; ++Index)
            {
                if (BestPathWaypointSystem(Index) == To->System)
                {
                    bToOnPath = true;
                    break;
                }
            }
        }
        if (bToOnPath)
        {
            Link->Kind = 2;
            bKindSet = true;
        }
    }

    // otherwise both ends on the player's waypoint route (a waypoint in
    // the system, or the player's own system) -> kind 1.
    if (!bKindSet)
    {
        bool bFromOnRoute = GetPlayerStarSystemIdbaseline() == From->System->GetId();
        if (!bFromOnRoute)
        {
            int const Count = GetPlayerWaypointCountbaseline();
            for (int Index = 0; Index < Count; ++Index)
            {
                FPlayerWaypointRecord const* const Record = GetPlayerWaypointbaseline(Index);
                if (Record->SystemId == From->System->GetId())
                {
                    bFromOnRoute = true;
                    break;
                }
            }
        }
        if (bFromOnRoute)
        {
            bool bToOnRoute = GetPlayerStarSystemIdbaseline() == To->System->GetId();
            if (!bToOnRoute)
            {
                int const Count = GetPlayerWaypointCountbaseline();
                for (int Index = 0; Index < Count; ++Index)
                {
                    FPlayerWaypointRecord const* const Record = GetPlayerWaypointbaseline(Index);
                    if (Record->SystemId == To->System->GetId())
                    {
                        bToOnRoute = true;
                        break;
                    }
                }
            }
            if (bToOnRoute)
                Link->Kind = 1;
        }
    }

    // push_back.
    JumpLinks.push_back(Link);
}

Universe::ISystem const* NeuroNetNavMap::BestPathWaypointSystem(int Index)
{
    // . The record is dereferenced without a null test, as in baseline (every
    // caller stays below BestPathGetWaypointCount).
    FmtStr::Val const* Token = BestPathGetWaypoint(Index)->Marker.Head;
    while (Token != nullptr)
    {
        unsigned short const Tag = Token->TypeTag;
        if (Tag == 0x42)
        {
            // 'B' -- the system one of whose objects docks with the base.
            unsigned const BaseId = Token->Value;
            for (Universe::ISystem* System = Universe::GetFirstSystem(); System != nullptr;
                 System = Universe::GetNextSystem())
            {
                Universe::FSystemObject Object;
                unsigned const Count = static_cast<unsigned>(System->GetObjectCount());
                for (unsigned ObjectIndex = 0; ObjectIndex < Count; ++ObjectIndex)
                {
                    if (System->GetSystemObject(ObjectIndex, &Object) && Object.DockWith == BaseId)
                        return System;
                }
            }
        }
        else if (Tag == 0x4e)
        {
            // 'N' -- the data block's first dword.
            return Universe::get_system(Token->Marker->Data[0]);
        }
        else if (Tag == 0x53)
        {
            // 'S' -- the id itself.
            return Universe::get_system(Token->Value);
        }
        Token = Token->Next;
    }
    return nullptr;
}

// ----------------------------------------------------------------------------------------
// Panels
// ----------------------------------------------------------------------------------------

void NeuroNetNavMap::RefreshMissionInfo()
{
    // .
    FBestPath* const Path = G_BestPaths.empty() ? nullptr : &G_BestPaths.begin()->second;
    if (Path == nullptr)
    {
        // no route -- no abort button, a blank title and text.
        AbortMissionButton->SetProperty(1, 1, 0);
        AbortMissionText->SetProperty(1, 1, 0);
        wcscpy(G_NnTextScratchB, L" ");
        InfoTitle->SetProperty(0x1d, reinterpret_cast<uintptr_t>(G_NnTextScratchB), 0);
        InfoText->Flags = static_cast<unsigned char>(InfoText->Flags & 0xfd);

        RenderDisplayList Text;
        {
            StyleNode Style(0x9ca4);
            RdlAppendNode(Text, Style.Clone());
        }
        AppendTextMaybeXmlRdl(G_NnTextScratchB,
                              static_cast<unsigned>(wcslen(G_NnTextScratchB)), Text, false);
        InfoText->ClearContent();
        InfoText->AppendContent(Text);
        InfoText->SetScrollOrigin(0, 0);
        return;
    }

    // the route's title and text (node +0x20 / +0x38).
    FmtStrSprintf(&Path->Title, G_NnTextScratchB, G_NnTextScratchCapacity);
    InfoTitle->SetProperty(0x1d, reinterpret_cast<uintptr_t>(G_NnTextScratchB), 0);
    FmtStrSprintf(&Path->Text, G_NnTextScratchB, G_NnTextScratchCapacity);
    InfoText->Flags = static_cast<unsigned char>(InfoText->Flags | 2);


    {
        RenderDisplayList Text;
        {
            StyleNode Style(0x9ca4);
            RdlAppendNode(Text, Style.Clone());
        }
        AppendTextMaybeXmlRdl(G_NnTextScratchB,
                              static_cast<unsigned>(wcslen(G_NnTextScratchB)), Text, false);
        InfoText->ClearContent();
        InfoText->AppendContent(Text);
        InfoText->SetScrollOrigin(0, 0);
    }

    // the abort button is hidden for routes of kind 7 and 8.
    unsigned char bHideAbort = 1;
    FBestPath const* const First = BestPathGetFirst();
    if (First != nullptr && First->Field_0x50 != 7 && First->Field_0x50 != 8)
        bHideAbort = 0;
    AbortMissionButton->SetProperty(1, bHideAbort, 0);
    AbortMissionText->SetProperty(1, bHideAbort, 0);
}

void NeuroNetNavMap::HideObjectTools()
{
    // .
    InZoomButton->SetProperty(1, 1, 0);
    OutZoomButton->SetProperty(1, 1, 0);
    InfoButton->SetProperty(1, 1, 0);
    SetWaypointButton->SetProperty(1, 1, 0);
    BestPathButton->SetProperty(1, 1, 0);
    JumpGateButton->SetProperty(1, 1, 0);
    SelectObjectButton->SetProperty(1, 1, 0);
    ClearWaypointButton->SetProperty(1, 1, 0);
    OptionBridge->SetProperty(1, 1, 0);
    bObjectToolsShown = 0;
}

void NeuroNetNavMap::RefreshObjectives()
{
    // .
    G_bNavMapRouteChanged = 0;
    int Row = 0;

    if (bBestPathObjectives == 0)
    {
        // the player's waypoints -- the scroll range, and the first row
        // back to 0 when it ran past the end.
        int const Count = GetPlayerWaypointCountbaseline();
        int FirstScroll = Count - 3;
        if (FirstScroll < 0)
            FirstScroll = 0;
        ObjectiveScroll->SetProperty(0x37, 0, static_cast<uintptr_t>(Count - 1));
        if (FirstScroll == 0)
            ObjectiveScroll->SetProperty(2, 1, 0);
        else
            ObjectiveScroll->SetProperty(2, 0, 0);
        if (ObjectiveFirstRow >= Count)
            ObjectiveFirstRow = 0;

        // up to three rows from ObjectiveFirstRow.
        for (int Index = ObjectiveFirstRow; Index < Count; ++Index)
        {
            G_NnTextScratchB[0] = L'\0';
            FPlayerWaypointRecord const* const Record = GetPlayerWaypointbaseline(Index);
            if (Record != nullptr)
            {
                // "Waypoint %d", the system line ("\n" + ids 0x54c formatted with the system
                // name) and the sector ("\n" + the routine).
                GetIdsStringWWithRawFallback(0x54b, G_NnTextScratchA, G_NnTextScratchCapacity);
                _swprintf(G_NnTextScratchB, G_NnTextScratchA, Record->AutoId);
                wchar_t Sector[0x100];
                NavMapSectorName(&Record->Pos, Sector);
                Universe::ISystem const* const System =
                    Universe::get_system(Record->SystemId);
                if (System != nullptr)
                {
                    GetIdsStringWWithRawFallback(0x54c, G_NnTextScratchA,
                                                 G_NnTextScratchCapacity);
                    wcscat(G_NnTextScratchB, L"\n");
                    wchar_t SystemName[0x100];
                    GetIdsStringWWithRawFallback(static_cast<unsigned>(System->GetStridName()),
                                                 SystemName, 0x100);
                    _swprintf(G_NnTextScratchB + wcslen(G_NnTextScratchB), G_NnTextScratchA,
                              SystemName);
                }
                wcscat(G_NnTextScratchB, L"\n");
                wcscat(G_NnTextScratchB, Sector);
            }

            // the row's text in the idle colour, both shown.
            ObjectiveText[Row]->SetProperty(0x1d, reinterpret_cast<uintptr_t>(G_NnTextScratchB),
                                            0);
            ObjectiveText[Row]->SetProperty(0x33,
                                            reinterpret_cast<uintptr_t>(&G_NnPaletteArgb_679bb0),
                                            1);
            ObjectiveText[Row]->SetProperty(1, 0, 0);
            MissionObjective[Row]->SetProperty(1, 0, 0);
            ++Row;
            if (Row >= 3)
                return;
        }

        // the rows left, hidden.
        for (; Row < 3; ++Row)
        {
            ObjectiveText[Row]->SetProperty(1, 1, 0);
            MissionObjective[Row]->SetProperty(1, 1, 0);
        }
        return;
    }

    // the best path. The loop runs over five times the waypoint count
    // (baseline's bound; the records past the end are null and skipped). Kind 9 is not listed
    // and not counted; the rows before ObjectiveFirstRow only advance the stop number.
    int const Total = BestPathGetWaypointCount() * 5;
    int Listed = 0;
    int StopNumber = 1;
    for (int Index = 0; Index < Total; ++Index)
    {
        FBestPathWaypoint* const Waypoint = BestPathGetWaypoint(Index);
        if (Waypoint == nullptr)
            continue;
        unsigned const Kind = Waypoint->Field_0x18 & 0xf;
        if (Kind != 9)
            ++Listed;
        if (Index < ObjectiveFirstRow)
        {
            if (Kind == 0xa)
                ++StopNumber;
            continue;
        }
        if (Kind == 9 || Row >= 3)
            continue;


        if (Kind == 0xa)
        {
            GetIdsStringWWithRawFallback(0x4df, G_NnTextScratchA, G_NnTextScratchCapacity);
            _swprintf(G_NnTextScratchB, G_NnTextScratchA, StopNumber);
            ++StopNumber;
        }
        else
        {
            GetIdsStringWWithRawFallback(0x331c, G_NnTextScratchB, G_NnTextScratchCapacity);
        }
        FmtStrSprintf(&Waypoint->Marker, G_NnTextScratchA, G_NnTextScratchCapacity);
        wcscat(G_NnTextScratchB, G_NnTextScratchA);
        ObjectiveText[Row]->SetProperty(0x1d, reinterpret_cast<uintptr_t>(G_NnTextScratchB), 0);
        ObjectiveText[Row]->SetProperty(1, 0, 0);
        MissionObjective[Row]->SetProperty(1, 0, 0);
        ++Row;
    }


    for (; Row < 3; ++Row)
    {
        ObjectiveText[Row]->SetProperty(1, 1, 0);
        MissionObjective[Row]->SetProperty(1, 1, 0);
    }

    // the scroll range over the listed records.
    int FirstScroll = Listed - 3;
    if (FirstScroll < 0)
        FirstScroll = 0;
    ObjectiveScroll->SetProperty(0x37, 0, static_cast<uintptr_t>(Listed - 1));
    if (FirstScroll == 0)
        ObjectiveScroll->SetProperty(2, 1, 0);
    else
        ObjectiveScroll->SetProperty(2, 0, 0);
    if (ObjectiveFirstRow >= Listed)
        ObjectiveFirstRow = 0;
}

void NeuroNetNavMap::SetFilterMode(int Mode)
{
    // .
    // the known-base rows (and their titles) take property 0x24 = mode 7.
    uintptr_t const bBaseList = Mode == 7 ? 1 : 0;
    for (int Index = 0; Index < 15; ++Index)
    {
        BaseTexts[Index]->SetProperty(0x24, bBaseList, 0);
        BaseSystemTexts[Index]->SetProperty(0x24, bBaseList, 0);
    }
    BaseTitleText->SetProperty(0x24, bBaseList, 0);
    SystemTitleText->SetProperty(0x24, bBaseList, 0);

    // leaving an info-card mode (6 / 7) closes the card and brings back
    // what it covered.
    if (G_NavMapFilterMode == 6 || G_NavMapFilterMode == 7)
    {
        InfoCard->Flags = static_cast<unsigned char>(InfoCard->Flags & 0xfc);
        CloseInfocardButton->Flags = static_cast<unsigned char>(CloseInfocardButton->Flags & 0xfc);
        CloseInfocardText->Flags = static_cast<unsigned char>(CloseInfocardText->Flags & 0xfc);
        if (ViewMode == 0)
        {
            for (int Index = 0; Index < 4; ++Index)
                FactionLabels[Index]->SetProperty(1, 0, 0);
        }
        else
        {
            PhysicalFilter->Flags = static_cast<unsigned char>(PhysicalFilter->Flags | 3);
            PoliticalFilter->Flags = static_cast<unsigned char>(PoliticalFilter->Flags | 3);
            PatrolFilter->Flags = static_cast<unsigned char>(PatrolFilter->Flags | 3);
            LegendFilter->Flags = static_cast<unsigned char>(LegendFilter->Flags | 3);
            BaseListFilter->Flags = static_cast<unsigned char>(BaseListFilter->Flags | 3);
            MineableFilter->Flags = static_cast<unsigned char>(MineableFilter->Flags | 3);
            LabelButton->Flags = static_cast<unsigned char>(LabelButton->Flags | 3);
            if (!MissionZoneIds.empty())
                MissionFilter->Flags = static_cast<unsigned char>(MissionFilter->Flags | 3);
        }
    }

    // the new mode and the toggles' pressed state.
    G_NavMapFilterMode = Mode;
    PhysicalFilter->SetProperty(0xc, G_NavMapFilterMode == 0 ? 1 : 0, 0);
    PoliticalFilter->SetProperty(0xc, G_NavMapFilterMode == 1 ? 1 : 0, 0);
    PatrolFilter->SetProperty(0xc, G_NavMapFilterMode == 2 ? 1 : 0, 0);
    MissionFilter->SetProperty(0xc, G_NavMapFilterMode == 3 ? 1 : 0, 0);
    LegendFilter->SetProperty(0xc, G_NavMapFilterMode == 4 ? 1 : 0, 0);
    BaseListFilter->SetProperty(0xc, G_NavMapFilterMode == 7 ? 1 : 0, 0);
    MineableFilter->SetProperty(0xc, G_NavMapFilterMode == 5 ? 1 : 0, 0);

    // the zone filters only in the political / patrol modes.
    if (G_NavMapFilterMode == 1 || G_NavMapFilterMode == 2)
    {
        AllFilter->SetProperty(1, 0, 0);
        HostileFilter->SetProperty(1, 0, 0);
        FriendlyFilter->SetProperty(1, 0, 0);
    }
    else
    {
        AllFilter->SetProperty(1, 1, 0);
        HostileFilter->SetProperty(1, 1, 0);
        FriendlyFilter->SetProperty(1, 1, 0);
    }

    // mode 6 (the info card) hides the filter row.
    if (G_NavMapFilterMode == 6)
    {
        PhysicalFilter->Flags = static_cast<unsigned char>(PhysicalFilter->Flags & 0xfc);
        PoliticalFilter->Flags = static_cast<unsigned char>(PoliticalFilter->Flags & 0xfc);
        PatrolFilter->Flags = static_cast<unsigned char>(PatrolFilter->Flags & 0xfc);
        MissionFilter->Flags = static_cast<unsigned char>(MissionFilter->Flags & 0xfc);
        LegendFilter->Flags = static_cast<unsigned char>(LegendFilter->Flags & 0xfc);
        BaseListFilter->Flags = static_cast<unsigned char>(BaseListFilter->Flags & 0xfc);
        MineableFilter->Flags = static_cast<unsigned char>(MineableFilter->Flags & 0xfc);
        LabelButton->Flags = static_cast<unsigned char>(LabelButton->Flags & 0xfc);
    }

    // the title (the mode indexes the field unchecked, as in baseline).
    FilterTitle->SetProperty(0x1e, kNavMapFilterTitleIds[G_NavMapFilterMode], 0);
}

void NeuroNetNavMap::RefreshMissionZones()
{
    // .
    ShadeRecheckTimer = 1.0f;

    // the vector emptied (a self-copy of [last, last) then last = first).
    MissionZoneIds.clear();

    // every 'Z' token of every best-path waypoint, appended. The record is
    // dereferenced without a null test (the index stays below the count).
    int const Count = BestPathGetWaypointCount();
    for (int Index = 0; Index < Count; ++Index)
    {
        for (FmtStr::Val const* Token = BestPathGetWaypoint(Index)->Marker.Head;
             Token != nullptr; Token = Token->Next)
        {
            if (Token->TypeTag == 0x5a)
                MissionZoneIds.push_back(Token->Value);
        }
    }

    // in the system view (and with no transition running) the mission
    // filter shows only with zones; the mission mode falls back to physical without them.
    if (ViewMode != 0 && bTransition == 0)
    {
        unsigned char const bNoZones = MissionZoneIds.empty() ? 1 : 0;
        MissionFilter->SetProperty(1, bNoZones, 0);
        if (bNoZones != 0 && G_NavMapFilterMode == 3)
            SetFilterMode(0);
    }
}
