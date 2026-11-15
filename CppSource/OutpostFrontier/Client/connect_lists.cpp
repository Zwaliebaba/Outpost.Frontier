// Author: Giperion.

#include "connect_lists.h"

#include "best_path.h"              // G_BestPaths -- the field
#include "character_select_equip.h" // the player-state lists (the field / the field / ...)
#include "common_goods.h"           // EquipDescList
#include "neuroncomponents.h"                  // FDUMP
#include "nn_story_star.h"          // StoryStarReset (the routine), StoryStarSetRankUp (the routine)
#include "player_spawn.h"           // ResetPlayerWaypointsbaseline (the routine)

#include <list>

namespace
{
    // The steps the routine and the routine share, in listing order.
    void ClearPlayerConnectLists()
    {
        // / 0x004c4bc5..0x004c4be8: every node of the collision-group
        // list the field erased (Common's list erase, one node at a time).
        GetPlayerCollisionGroupListbaseline().clear();
        // / 0x004c4bea..0x004c4c0d: every node of the EquipDescList
        // erased. Neither revision counter is touched.
        GetPlayerEquipDescListbaseline().Items.clear();
        // / 0x004c4c0f..0x004c4c22: the routine erases the objective map
        // from its first node to its head -- the whole map (G_BestPaths,
        // best_path.h).
        G_BestPaths.clear();
        // / 0x004c4c27..0x004c4ca2: the visit map the field cleared
        // (the whole tree at once when the erase range is begin..end, else node by node).
        ClearPlayerVisitMapbaseline();
        // / 0x004c4ca4..0x004c4cab: the NN_StoryStar data model --
        // () (the pending objectives and the banner) and the routine(0) (no rank-up
        // pending).
        StoryStarReset();
        StoryStarSetRankUp(0);
    }
}

void PreConnectReset()
{
    // . 0x004c4a8a / 0x004c4a94: the field = 0, the field = 0.
    ClearPlayerShipArchAndHealthbaseline();
    // / 0x004c4a9e..0x004c4aaa: a positive waypoint count the field goes to 0 and
    // (HudWaypointOnPlayerWaypointsCleared) runs -- the routine's body, inlined.
    ResetPlayerWaypointsbaseline();
    ClearPlayerConnectLists();
}

void TeardownConnectLists()
{
    // (callers 0x005a80e8, 0x005a82b9, 0x005a8825, 0x005a891a): the routine
    // without the two stores and the waypoint reset.
    ClearPlayerConnectLists();
}
