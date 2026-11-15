// Author: Giperion.

// The client's mission objectives map the field -- see best_path.h.

#include "best_path.h"

#include "character_select_equip.h" // GetPlayerStarSystemIdbaseline (the routine -> the field)
#include "client_gameobj.h"         // FClientGameobj
#include "client_objdb.h"           // ClientObjDb_GetKindHead / ClientObjDb_FindEqObjByVibeId
#include "common_cobject.h"         // CObject, CEqObj::DockingBaseId
#include "common_goods.h"           // BaseWatcher
#include "common_iface.h"           // IObjRW, ObjRwFromWatchable
#include "common_universe.h"        // Universe::get_base
#include "neuroncomponents.h"                  // FDUMP
#include "hud_contact_list.h"       // G_bHudContactListRefreshRequested (the field)
#include "nn_navmap.h"              // G_bNavMapRouteChanged (the field)
#include "nn_story_star.h"          // StoryStarReset / StoryStarAddHistory / StoryStarPushPending
#include "player_spawn.h"           // GetPlayerWaypointCountbaseline / ResetPlayerWaypointsbaseline
#include "rpc_iclient_args.h"       // the slot 71 / 72 / 73 argument records

#include <cstring>
#include <utility>

// ; static init the branch (the routine, the empty map), teardown the branch.
std::map<unsigned, FBestPath> G_BestPaths;

namespace
{
    // the docking solar of the last base a record named (static init
    // the routine(0), the BaseWatcher(IObjRW*) ctor with null; teardown
    // -> the routine, ~BaseWatcher).
    BaseWatcher G_BestPathBaseWatcher;
    // the star system of G_BestPathBaseId (0 until get_base answers).
    unsigned G_BestPathBaseSystem = 0;
    // the base id the watcher was resolved for.
    unsigned G_BestPathBaseId = 0;
    // ObjDB bucket 2 was searched for G_BestPathBaseId's solar.
    unsigned char G_bBestPathBaseSearched = 0;

    // (cdecl; also inlined at 0x004c51b6 and 0x004c5240): mark the search done,
    // then the first solar of ObjDB bucket 2 whose CEqObj +0x160 docking base is
    // G_BestPathBaseId becomes the watched object (BaseWatcher::set_pointer behind a compare
    // with the current target -- BaseWatcher::set).
    void SearchBaseSolar()
    {
        G_bBestPathBaseSearched = 1;
        int const SolarKind = 2;
        for (FClientGameobj* Go = ClientObjDb_GetKindHead(&SolarKind); Go != nullptr;
             Go = Go->ListNext)
        {
            // the CObject (+0x10) narrowed to a CSolar by
            // (ObjectClass & 0x303) == 0x303, else null -- read unguarded, as in baseline
            // (bucket 2 only holds solars).
            CObject* const Obj = Go->Object;
            CEqObj* Solar = nullptr;
            if (Obj != nullptr && (static_cast<unsigned>(Obj->ObjectClass) & 0x303u) == 0x303u)
                Solar = static_cast<CEqObj*>(Obj);
            if (Solar->DockingBaseId == G_BestPathBaseId)
            {
                G_BestPathBaseWatcher.set(static_cast<Watchable const*>(static_cast<IObjRW*>(Go)));
                return;
            }
        }
    }

    // The 'B' branch of the routine (0x004c514c..0x004c52c8) and of the routine
    // (0x004c532d..0x004c541c). The two spell it differently (the routine inlines the search
    // twice and guards set_pointer(0) with a null test; the routine calls the routine and
    // BaseWatcher::set) but take the same paths:
    //  - a new base id: remember it, system 0, drop the watched object, not searched; then
    //    Universe::get_base -- with a base, its system (IBase vt+0x10); a system other than the
    //    player's returns the (now empty) watcher at once, the player's searches bucket 2;
    //  - then (also for the same id, or no base): the base's system is the player's and bucket
    //    2 was not searched yet -> search it;
    //  - the watched object, IObjRW (Watchable - 8), or null.
    IObjRW* ResolveBaseObject(unsigned BaseId)
    {
        if (G_BestPathBaseId != BaseId)
        {
            G_BestPathBaseId = BaseId;
            G_BestPathBaseSystem = 0;
            G_BestPathBaseWatcher.set(nullptr);
            G_bBestPathBaseSearched = 0;
            Universe::IBase* const Base = Universe::get_base(BaseId);
            if (Base != nullptr)
            {
                G_BestPathBaseSystem = Base->SystemId;
                if (G_BestPathBaseSystem != GetPlayerStarSystemIdbaseline())
                    return ObjRwFromWatchable(const_cast<Watchable*>(G_BestPathBaseWatcher.target()));
                SearchBaseSolar();
            }
        }
        if (G_BestPathBaseSystem == GetPlayerStarSystemIdbaseline() && G_bBestPathBaseSearched == 0)
            SearchBaseSolar();
        return ObjRwFromWatchable(const_cast<Watchable*>(G_BestPathBaseWatcher.target()));
    }

    // The first 'B' (0x42) or 'R' (0x52) token of the record's chain, as the routine
    // (0x004c5120..0x004c5132) and the routine (0x004c52e0..0x004c52f2) walk it.
    FmtStr::Val const* FindObjectToken(FBestPathWaypoint const* Waypoint)
    {
        for (FmtStr::Val const* Token = Waypoint->Marker.Head; Token != nullptr; Token = Token->Next)
        {
            if (Token->TypeTag == 0x42 || Token->TypeTag == 0x52)
                return Token;
        }
        return nullptr;
    }

    // The object a 'B' / 'R' token names: 'R' through the routine(data, 0) (0x004c513a /
    // ), 'B' through ResolveBaseObject.
    IObjRW* ResolveObjectToken(FmtStr::Val const* Token)
    {
        unsigned const Data = Token->Value;
        if (Token->TypeTag == 0x52)
            return ClientObjDb_FindEqObjByVibeId(Data, 0);
        return ResolveBaseObject(Data);
    }
}

int BestPathGetWaypointCount()
{
    // an empty map (head->_Left == head) or a null vector start gives 0;
    // otherwise (last - first) / 0x1c of the first entry.
    if (G_BestPaths.empty())
        return 0;
    FBestPath const& Path = G_BestPaths.begin()->second;
    if (Path.Waypoints.empty())
        return 0;
    return static_cast<int>(Path.Waypoints.size());
}

FBestPathWaypoint* BestPathGetWaypoint(int Index)
{
    // 0x004c4ffb empty map, 0x004c5003 Index < 0, 0x004c500a null vector
    // start, 0x004c5026 (unsigned) Index >= count -> null; else first + Index * 0x1c.
    if (G_BestPaths.empty())
        return nullptr;
    if (Index < 0)
        return nullptr;
    FBestPath& Path = G_BestPaths.begin()->second;
    if (Path.Waypoints.empty())
        return nullptr;
    if (static_cast<unsigned>(Index) >= static_cast<unsigned>(Path.Waypoints.size()))
        return nullptr;
    return &Path.Waypoints[static_cast<size_t>(Index)];
}

FBestPath* BestPathGetFirst()
{
    // head->_Left == head -> 0; else node + 0x10 (see best_path.h).
    if (G_BestPaths.empty())
        return nullptr;
    return &G_BestPaths.begin()->second;
}

int BestPathFirstWaypointNotKind3()
{
    // an empty map (0x004c549d), a null vector start (0x004c54a4) or no
    // waypoints (0x004c54c0) is -1. Then each index through the the routine lookup inline
    // (0x004c54d0..0x004c5504): the first record whose +0x18 dword is not 3 answers its index;
    // all of them 3 is -1. The lookup cannot fail for Index < count, so its null (which baseline
    // would dereference at 0x004c5508) is never reached.
    int const Count = BestPathGetWaypointCount();
    for (int Index = 0; Index < Count; ++Index)
    {
        FBestPathWaypoint const* const Waypoint = BestPathGetWaypoint(Index);
        if (Waypoint->Field_0x18 != 3)
            return Index;
    }
    return -1;
}

bool BestPathHasText()
{
    // an empty map, a null vector start or no waypoints (0x004c58ed..
    // ) is false; then node +0x3c -- the Text FmtStr's MadLibId -- must be neither 1
    // nor 0 (0x004c591f..0x004c5929).
    if (G_BestPaths.empty())
        return false;
    FBestPath const& Path = G_BestPaths.begin()->second;
    if (Path.Waypoints.empty())
        return false;
    unsigned const MadLibId = Path.Text.MadLibId;
    return MadLibId != 1 && MadLibId != 0;
}

unsigned BestPathWaypointHash(int Index)
{
    // the the routine lookup inline (0x004c5530..0x004c5572) -- 0 for no
    // record. Then from the record's MadLibId, every token rotates the hash left by one and
    // adds its data, except 'R' (0x52) and 'N' (0x4e) tokens, which only rotate.
    FBestPathWaypoint* const Waypoint = BestPathGetWaypoint(Index);
    if (Waypoint == nullptr)
        return 0;
    unsigned Hash = Waypoint->Marker.MadLibId;
    for (FmtStr::Val const* Token = Waypoint->Marker.Head; Token != nullptr; Token = Token->Next)
    {
        Hash = (Hash << 1) | (Hash >> 31);
        if (Token->TypeTag != 0x52 && Token->TypeTag != 0x4e)
            Hash += Token->Value; // the raw payload dword, whatever the tag (0x21 adds the pointer)
    }
    return Hash;
}

IObjRW* BestPathWaypointResolveObject(FBestPathWaypoint const* Waypoint)
{
    // the first 'B' / 'R' token decides; none is null (0x004c5134).
    FmtStr::Val const* const Token = FindObjectToken(Waypoint);
    if (Token == nullptr)
        return nullptr;
    return ResolveObjectToken(Token);
}

bool BestPathWaypointResolvePosition(FBestPathWaypoint const* Waypoint, Vector* OutPos,
                                     unsigned* OutSystem)
{
    // (RET 8). 0x004c541e: an object from the first 'B' / 'R' token -> the
    // player's system (the field) and the object's IObjInspect slot 0 position.
    FmtStr::Val const* const ObjectToken = FindObjectToken(Waypoint);
    if (ObjectToken != nullptr)
    {
        IObjRW* const Object = ResolveObjectToken(ObjectToken);
        if (Object != nullptr)
        {
            *OutSystem = GetPlayerStarSystemIdbaseline();
            *OutPos = Object->get_position();
            return true;
        }
    }

    // / 0x004c5453: otherwise the first 'N' (0x4e) token's data block
    // {+0x00 system, +0x08 position}; no such token is false.
    for (FmtStr::Val const* Token = Waypoint->Marker.Head; Token != nullptr; Token = Token->Next)
    {
        if (Token->TypeTag == 0x4e)
        {
            FmtStr::NavMarker const* const Block = Token->Marker;
            *OutSystem = Block->Data[0];
            std::memcpy(OutPos, &Block->Data[2], sizeof(Vector)); // words 2..4: the float triple
            return true;
        }
    }
    return false;
}

void BestPathObjectivesChanged()
{
    // (cdecl; caller slot 72, 0x005a5e1a).
    G_bNavMapRouteChanged = 1;
    G_bHudContactListRefreshRequested = 1;
    if (G_BestPaths.empty())
        return;

    ResetPlayerWaypointsbaseline();
    int const Count = BestPathGetWaypointCount();
    for (int Index = 0; Index < Count; ++Index)
    {
        // cannot fail for Index < Count; baseline would read +0x18 through the null
        // at 0x004bcd06 anyway.
        FBestPathWaypoint* const Waypoint = BestPathGetWaypoint(Index);
        if ((Waypoint->Field_0x18 & 0x80u) != 0) // TEST AL,AL / JNS
            StoryStarReset();                    // , inlined at 0x004bcc4e..0x004bcce6
        if ((Waypoint->Field_0x18 & 0x20u) != 0)
            StoryStarAddHistory(&Waypoint->Marker);  // (0x004bccef)
        if ((Waypoint->Field_0x18 & 0x10u) != 0)
            StoryStarPushPending(&Waypoint->Marker); // (0x004bccfe)
        Waypoint->Field_0x18 &= 0xfu;
    }
}

void BestPathReplaceObjective(FServerReplaceMissionObjectiveInfo const* Info)
{
    // (RET 8): the routine (operator[]) on the key, then record (byte) +0x20 of
    // its vector -- unchecked -- gets FmtStr::operator=(+0x04) and +0x18 = +0x1c.
    FBestPath& Path = G_BestPaths[Info->Field_0x00];
    FBestPathWaypoint* const Records = Path.Waypoints.data();
    Records[Info->State].Marker = Info->Text;
    Records[Info->State].Field_0x18 = Info->ObjectiveType;
}

void BestPathSetObjectives(FServerSetMissionObjectivesInfo const* Info)
{
    // (RET 8). 0x005a5bef: the routine erase(begin, end).
    G_BestPaths.clear();

    // a title of mad-lib 0 or 1 while the player has waypoints
    // (the routine > 0) and a +0x3c of 7 or 8 returns at once -- without the routine.
    unsigned const TitleMadLib = Info->TextA.MadLibId;
    if ((TitleMadLib == 0 || TitleMadLib == 1) && GetPlayerWaypointCountbaseline() > 0 &&
        (Info->Field_0x3c == 7 || Info->Field_0x3c == 8))
        return;

    // no records -> straight to the routine with the map left empty.
    if (Info->Count != 0)
    {
        // a default value (the routine vector, two FmtStr
        // ) paired with the key (the routine) and inserted (the routine).
        std::pair<std::map<unsigned, FBestPath>::iterator, bool> const Inserted =
            G_BestPaths.insert(std::make_pair(Info->Field_0x00, FBestPath()));
        FBestPath& Path = Inserted.first->second;

        // reserve(count) -- a new block when the capacity is short,
        // copies, the routine destroys, the old block freed.
        Path.Waypoints.reserve(Info->Count);

        // per record a temporary {FmtStr = entry text, +0x18 = entry
        // state}, appended with the routine (insert(end, 1, value)).
        for (unsigned Index = 0; Index < Info->Count; ++Index)
        {
            FBestPathWaypoint Record;
            Record.Marker = Info->Entries[Index].Text;
            Record.Field_0x18 = Info->Entries[Index].State;
            Path.Waypoints.push_back(Record);
        }

        // Title = +0x0c, byte +0x40 = +0x3c, Text = +0x24.
        Path.Title = Info->TextA;
        Path.Field_0x50 = Info->Field_0x3c;
        Path.Text = Info->TextB;
    }

    BestPathObjectivesChanged(); // (0x005a5e1a)
}

void BestPathSetObjectiveState(FServerMissionObjectiveStateInfo const* Info)
{
    // (RET 8): the routine lower_bound, a hit when not end and not key < node
    // key -- find(); then record (byte) +0x04, unchecked, gets +0x18 = the dword at +0x08.
    std::map<unsigned, FBestPath>::iterator const It = G_BestPaths.find(Info->Field_0x00);
    if (It == G_BestPaths.end())
        return;
    It->second.Waypoints.data()[Info->Field_0x04].Field_0x18 = Info->Field_0x08;
}
