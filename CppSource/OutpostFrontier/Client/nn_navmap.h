// Author: Giperion.

#pragma once

// -------------------------------------------------------------------------------------------
// NeuroNetNavMap -- Client\Interface\NeuroNet\NavMap.cpp (baseline 0x0048dc10..0x004a3f90).
// The NeuroNet navigation map: the universe view (every system as a NAV_star) and the system
// view (solars, zones, trade lanes, patrol paths, waypoints, the best path), the filter / zoom /
// waypoint buttons along its frame, the info card and the known-bases list.

// RTTI .?AVNeuroNetNavMap@@ (0x00611cc4): WinControl @0 (IDataViewOwner @0x7c),
// Event::Handler @0x32c, ButtonMover @0x330. COLs 0x005eb5fc / 0x005eb610 / 0x005eb624 /
// . Size 0x100c (operator new at 0x004a2e5b).

// vftables (read out of the PE):
//   WinControl face 0x005d2f84 (45 slots) -- overrides 0 the routine (scalar deleting dtor
//     around the routine), 4 the branch HitTest, 5 the branch SetPosition, 9 the branch
//     Create, 11 the branch Update, 13 the branch Draw, 19 the branch MouseEvent,
//     23 the branch Slot23 (0xef01 show / 0xef02 close), 42 the branch SetProperty; every
//     other slot is WinControl's 0x005e5e44 entry.
//   IDataViewOwner face 0x005d2f74 -- WinControl's the branch / the routine.
//   Event::Handler 0x005d2f6c -- the branch OnEvent.
//   ButtonMover 0x005d2f60 -- the branch / the branch, i.e. ButtonMover's own bodies: no
//     override.

// Factory: WINFACTORY<NeuroNetNavMap> "NeuroNetNavMap", static init the branch ->
// (the WinFactory head, the routine, vftable 0x005d3fa0) + atexit; Create
// is operator new(0x100c) + the routine + slot 9. Built by the MissionBriefing
// state (0x0043d45c, parent "BriefingParentWindow") and by NavBar / NN_MainButton
// (0x0045cfb7, 0x0045d358).

// Member offsets in the comments are baseline's. The port holds the lists and vectors in
// current-STL containers (no baseline ABI, AGENTS.md), so from +0x3fc on the C++ offsets differ.

// Slot 11 Update (the branch) and its helpers are ported (nn_navmap_update.cpp /
// nn_navmap_view.cpp), slot 13 Draw (the branch) and its helpers too (nn_navmap_draw.cpp),
// slot 19 MouseEvent (the branch) and slot 42 SetProperty (the branch) with the helpers only
// they reach (nn_navmap_input.cpp). The info card's base sections are the routine
// (NnHelpAppendBaseGoods, nn_help.cpp). Remaining hole there: the story star reset
// (the routine), named in nn_navmap_input.cpp.
// -------------------------------------------------------------------------------------------

#include "button_mover.h"
#include "client_mesh_load.h"
#include "event_handler.h"
#include "space_camera.h"
#include "texture_shapes.h"
#include "wincontrol_base.h"
#include "common_tstring.h"

#include <list>
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

struct FFontString;
struct I3DMathEngine;
class IFont;
struct IRPDraw;
struct IRPIndexBuffer;
struct IRPVertexBuffer;
class RichTextWin;
class UiScroll;

namespace Universe
{
    struct ISystem;
    struct IZone;
}

// One map marker (a system on the universe view, a solar / zone / waypoint on the system
// view). baseline operator new(0x84) in the routine; destructor the routine, scalar deleting
// . Held by pointer in NeuroNetNavMap::Items.
struct FNavMapItem
{
    // 00491fd3..0049200d: the holder (the routine), its five tint floats
    // (the routine), the holder vftable / anim index, then +0x5c / +0x60 / +0x78 / +0x80 /
    // +0x81 cleared.
    FNavMapItem();

    ~FNavMapItem();

    int Kind;                   // +0x00 the routine argument 1
    int Type;                   // +0x04 argument 2 (bit 2 = a universe-view system)
    FClientMeshHolder Mesh;     // +0x08 "...\SpaceObjects\<name>.3db"
    float Position[3];          // +0x30 arguments 3..5
    float Size;                 // +0x3c argument 6 (the universe view passes a random
                                //       3000..4999): the marker's size -- the routine scales
                                //       the mesh by it, the routine picks within it;
                                // pulses it as RestSize + 1000 * wave
    float RestSize;             // +0x40 argument 6 again: the size a star pulses around
    float Heading;              // +0x44 argument 10: the marker's heading in degrees
                                //       (the routine the player, the routine moons /
                                //       planets facing the system centre; else 0)
    unsigned char bShown;       // +0x48 1 at creation; universe view: visit bit 0
    unsigned char Pad49[3];
    int StridName;              // +0x4c argument 8 (ISystem slot 3 / the object's ids_name)
    int IdsInfo;                // +0x50 argument 9
    int Variant;                // +0x54 0. A star (Kind 0): rand() * 7 >> 15, the the routine
                                //       tint scheme 0..6. A waypoint marker (Kind 3 / 4,
                                // ): the number it shows (0 none, -1 for a
                                //       best-path stop that is not a numbered one)
    int Ordinal;                // +0x58 0; a waypoint marker's slot + 1, then the waypoint's
                                //       AutoId (the routine)
    // +0x5c -- baseline {wchar_t* (msvcrt malloc), capacity}, freed with free by the routine.
    // Update (0x00494d67) shows it as a Kind 4 item's tool tip when it is non-null and not
    // empty (wcscmp against L"" at the field). No writer in NavMap.cpp (0x0048dc10..
    // ) sets it (only the routine's null); the port's empty string stands for both
    // baseline's null and L"", which Update treats alike. TODO: meaning / writer.
    std::wstring Field_0x5c;
    Universe::ISystem const* System; // +0x64 the universe view's system
    float PulseRate;            // +0x68 universe view: (rand() * 100 >> 15 + 1) * 0.01
    float PulsePhase;           // +0x6c universe view: (rand() * 900 >> 15 + 100) * 0.001;
                                // advances it by dt * PulseRate, wraps > 1
    unsigned Id;                // +0x70 argument 11
    unsigned char Visit;        // +0x74 argument 12
    unsigned char Pad75[3];
    FFontString* Label;         // +0x78 heap Font::String (operator new(0x34))
    float LabelWidth;           // +0x7c not written by the ctor; the routine: the label's
                                //       pixel width (IFont slot 8)
    unsigned char bLabelStarred; // +0x80 0; the routine: the label text carries the "*"
                                //       prefix (copied from bLabelCrowded when it changes)
    unsigned char bLabelCrowded; // +0x81 0; the routine sets it on a placed label another one
                                //       could not fit beside, the routine clears every one in
                                //       the zoomed view
    unsigned char Pad82[2];

private:
    FNavMapItem(FNavMapItem const&) = delete;
    FNavMapItem& operator=(FNavMapItem const&) = delete;
};

// One trade lane segment of the system view, 0x18 bytes (the routine 0x004982c7..0x0049836b):
// ring to next ring, both already scaled by SystemScale.
struct FNavMapSegment
{
    float Start[3]; // +0x00
    float End[3];   // +0x0c
};

// One trade lane ring of the system view, 0x7c bytes (the routine 0x00491906..0x004919a0):
// the [Object]'s nickname, position, archetype and ring links.
struct FNavMapRing
{
    TString<32> Nickname;  // +0x00 (the routine); the routine empties it when chained
    float Pos[3];          // +0x24 unscaled
    unsigned ArchetypeId;  // +0x30
    TString<32> NextRing;  // +0x34 next_ring (the routine)
    TString<32> PrevRing;  // +0x58 prev_ring (the routine, capacity 0x20)
};

// One placed label (the routine 0x0049a535..0x0049a569): its rect in screen fractions (the
// width and height grown by 1.4) and the item it belongs to.
struct FNavMapLabelRect
{
    float X;           // +0x00
    float Y;           // +0x04
    float W;           // +0x08 W * 1.4 (the field)
    float H;           // +0x0c H * 1.4
    FNavMapItem* Item; // +0x10
};

// One row of the known-bases list, 0x18 bytes (the routine 0x004a25f6..0x004a2655): the
// system's id, the base's [Object] position (unscaled) and the indices of its name in BaseNames
// and its system's name in SystemNames.
struct FNavMapBaseEntry
{
    unsigned SystemId; // +0x00 ISystem slot 0
    float Pos[3];      // +0x04 FSystemObject::Pos
    int BaseIndex;     // +0x10
    int SystemIndex;   // +0x14
};
static_assert(sizeof(FNavMapBaseEntry) == 0x18, "FNavMapBaseEntry is 0x18 bytes");

// One jump link of the universe view (the routine: operator new(0x10)). Held by pointer in
// NeuroNetNavMap::JumpLinks; the list is emptied without freeing them (the routine,
// 's second list erase), exactly as baseline does.
struct FNavMapJumpLink
{
    FNavMapItem* From;          // +0x00
    FNavMapItem* To;            // +0x04
    unsigned char Field_0x08;   // +0x08 1 at creation. TODO: meaning
    unsigned char bBothWays;    // +0x09 set when the reverse pair is added
    unsigned char Pad0a[2];
    int Kind;                   // +0x0c 0; 1 = both ends on the waypoint route, 2 = both on
                                //       the best path (or the player's system)
};

class NeuroNetNavMap : public WinControl, public EventHandler, public ButtonMover
{
public:

    NeuroNetNavMap();
    // (scalar deleting the routine)
    ~NeuroNetNavMap() override;

    // --- WinControl / Win3D overrides ---
    // slot 4 the branch (RET 4)
    bool HitTest(float const* Ray) override;
    // slot 5 the branch (RET 4)
    void SetPosition(float const* NewPosition) override;
    // slot 9 the branch (RET 8)
    bool Create(Win3D* NewParent, void* Data) override;
    // slot 23 the branch (RET 0xc) -- 0xef01 show, 0xef02 close and delete.
    bool Slot23(unsigned Msg, unsigned A, unsigned B) override;

    // --- Event::Handler ---
    // (RET 4) -- 0xf while briefing: tell the MissionBriefing state to continue
    // (the routine) and close through slot 23 (0xef02).
    bool OnEvent(unsigned MsgId) override;

    // --- Input (nn_navmap_input.cpp) ---
    // slot 19 the branch (RET 4) -- the map's mouse: arm a drag (zoomed in), the info card on
    // a right click, the debug beam or the object tools (the routine) on a release.
    bool MouseEvent(FWin3DMouseEvent const* Event) override;
    // slot 42 the branch (RET 0xc) -- the children's notifications: 0x26 clicks, 0x2c / 0x2d
    // hover over an objective row, 0x3e scroll moves (else a click); 0 for anything else.
    intptr_t SetProperty(int Msg, uintptr_t A, uintptr_t B) override;
    // (RET 4) -- the base of the current system's object whose nickname hashes to
    // ObjectId; 0 when none.
    unsigned ObjectBaseId(unsigned ObjectId);
    // (RET 0x10) -- open the info card (filter mode 6) on the name / info ids.
    void ShowInfoCard(int IdsName, int IdsInfo, unsigned char Visit, unsigned BaseId);
    // ask whether to abort the mission (the yes / no buttons).
    void AskAbortMission();
    // abort the first route's mission (IServer::AbortMission) and leave.
    void ConfirmAbortMission();
    // (RET 8) -- the cursor in map-frame fractions (0..1); false outside ScreenRect.
    bool CursorMapFraction(float* OutU, float* OutV);
    // open the object tools around the cursor for the item / zone under it.
    void ShowObjectTools();
    // once (bBaseListBuilt): every visited base of every visited system.
    void BuildBaseList();
    // (RET 4) -- bubble sort BaseEntries by base name (bByBase) or by system name
    // then base name.
    void SortBaseList(unsigned char bByBase);
    // the fifteen base-list rows from BaseListFirstRow and the scroll bar.
    void RefreshBaseList();

    // once (+0x834): the three map meshes, NavMapTextures.ini and the terrain /
    // waypath / trade-lane shapes.
    void LoadMapResources();
    // once (+0x408): LoadMapResources, then the system (or universe) view, the
    // map rectangles and the filter buttons; ends in Update (slot 11).
    void InitialShow();
    // the universe view: one NAV_star item per system, then the jump links.
    void BuildUniverseItems();
    // (RET 0x30) -- create an item and push it on Items; null when the mesh file
    // does not exist.
    FNavMapItem* AddItem(int Kind, int Type, float X, float Y, float Z, float Arg6,
                         char const* MeshName, int StridName, int IdsInfo, float Heading,
                         unsigned Id, unsigned char Visit);
    // universe view only: rebuild JumpLinks from every shown system's
    // connections.
    void BuildJumpLinks();
    // (RET 8) -- add the From -> To link unless it is there; the reverse pair only
    // gets bBothWays.
    void AddJumpLink(FNavMapItem* From, FNavMapItem* To);
    // (RET 4) -- the star system best-path waypoint Index names ('S', 'N', or the
    // system of the 'B' base), null when none does.
    Universe::ISystem const* BestPathWaypointSystem(int Index);
    // the info title / text from the best path (or blanks) and the abort button.
    void RefreshMissionInfo();
    // hide the zoom / info / waypoint / jump / select buttons and the option
    // bridge; +0x92c = 0.
    void HideObjectTools();
    // the three objective rows from the player's waypoints or the best path.
    void RefreshObjectives();
    // (RET 4) -- switch the filter mode (the field) and the controls with it.
    void SetFilterMode(int Mode);
    // rebuild MissionZoneIds from the best path's 'Z' tokens and show / hide
    // the mission filter.
    void RefreshMissionZones();
    // the universe stars' pulse: PulseTimer / PulseIndex, then every shown
    // Kind 0 item's phase, +0x3c and tint by Variant.
    void AnimateItems();
    // (RET 0xc) -- the position of the system object that docks with BaseId
    // (cached in CachedObjectId / CachedObjectPosition), else a mission-created solar's
    // (uncached), else zero. Returns Out.
    float* CachedBasePosition(float* Out, unsigned BaseId, Universe::ISystem const* System);
    // the system view: the extents, the item per shown [Object] (lane rings
    // gathered on the way), then the lane segments, the map zones, and no lanes when nothing
    // in the system has been visited.
    void BuildSystemItems();
    // the player marker (PlayerItem) and the ten waypoint meshes (+0x930).
    void AddPlayerMarker();
    // chain LaneRings from each lane's first ring into LaneSegments; the rings
    // are dropped afterwards.
    void BuildLaneSegments();
    // MapZones in three passes over the system's zones (the second pass sorted
    // by its type bits).
    void BuildMapZones();
    // a visited object (not a lane ring or sun) or a visited map zone exists.
    bool HasVisitedContent();
    // Not a baseline function: the SkipSystemIds test NavMap.cpp inlines at every use.
    bool IsSkippedSystem(Universe::ISystem const* System) const;
    // (RET 8) -- the player's marker: the universe view's system point, or the
    // system view's position (the docked base's solar) and heading in degrees, scaled by
    // SystemScale.
    void PlayerMapPosition(float* OutPos, float* OutHeading);

    // --- The per-frame work (nn_navmap_update.cpp / nn_navmap_view.cpp) ---
    // slot 11 the branch -- see nn_navmap_update.cpp. Win3D's face: ButtonMover::Update
    // shares the name, so callers name the Win3D face explicitly.
    void Update() override;
    // every item's map transform (position, heading, size by view), the edge
    // clamp of Kind 2 items (the off-screen arrow mesh), and the visit-driven bShown.
    void PlaceItems();
    // (RET 8) -- the first shown item whose pick circle holds MapPos (Kind 3 / 4
    // skipped when bSkipWaypoints); null when none.
    FNavMapItem* ItemAt(float const* MapPos, unsigned char bSkipWaypoints);
    // (RET 4) -- the cursor's point on the map in map units; false when the cursor
    // is outside ScreenRect or the view is not 0 / 1 / 2.
    bool MapPointUnderCursor(float* OutMapPos);
    // (RET 4) -- the cursor's point on the Win3D camera's near plane (in the map's
    // screen space), false outside ScreenRect.
    bool CursorRayPoint(float* OutPoint);
    // (RET 4) -- the reputation id of the group member whose marker holds MapPos
    // in the current system; -1 when none or in single player.
    int GroupMemberAt(float const* MapPos);
    // (RET 8) -- the zoomed map mesh (View 2) or the flat one: scale, pan and
    // alpha.
    void SetMapMeshTransform(int View, float Alpha);
    // the grid letters / numbers along the map's edges, following the pan /
    // zoom (and the transition), hidden outside the frame.
    void PlaceGridLabels();
    // zoom in on (ClickU, ClickV): a view-2 transition.
    void ZoomToClick();
    // rebuild the system view (ViewMode 1) and its filter row.
    void EnterSystemView();
    // rebuild the universe view (ViewMode 0) and hide the filter row.
    void EnterUniverseView();
    // the waypoint / best-path marker items follow the player's waypoints and
    // the best path; true when a marker moved.
    bool RefreshWaypointItems();
    // (RET 0xc) -- best-path waypoint Index's position and system from its 'S' /
    // 'B' / 'N' tokens (cached by BestPathWaypointHash).
    bool ResolveBestPathWaypoint(int Index, float* OutPos, unsigned* OutSystem);
    // open / close the side panels with the visibility of their controls.
    void SyncSidePanels();
    // (RET 4) -- slide the side panels in (bOpen) or out over 0.4 s.
    void SlideSidePanels(unsigned char bOpen);

    // --- The frame's drawing (nn_navmap_draw.cpp) ---
    // slot 13 the branch -- see nn_navmap_draw.cpp. Sets bDrawnOnce (0x004932f5).
    void Draw() override;
    // the universe view's jump links as gradient bands (the routine).
    void DrawJumpLinks();
    // the waypoint and best-path routes between their marker items as textured
    // bands (the routine), once Draw has run and not on the universe view.
    void DrawRouteLines();
    // the system objects' name labels (three passes by type), off in filter
    // modes 4 / 6 / 7.
    void DrawLabels();
    // (RET 8) -- make (or refresh on a bLabelCrowded change) the item's label text
    // and cache its width in LabelWidth.
    void EnsureItemLabel(IFont* Font, FNavMapItem* Item);
    // (RET 8) -- place and draw one item's label (or, when it does not fit, mark the
    // labels it overlaps).
    void DrawItemLabel(IFont* Font, FNavMapItem* Item);
    // (RET 0x14) -- find a free spot for a W x H label at (*X, *Y), nudging it by up
    // to 14 pixels in eight directions; on success record it in LabelRects.
    bool PlaceLabel(float* X, float* Y, float W, float H, FNavMapItem* Item);
    // (RET 0x10) -- the label rect (X * 0.9, Y, W * 1.2, H) lies inside the frame
    // and clear of every recorded one.
    bool LabelRectFits(float X, float Y, float W, float H);
    // the group members' markers (NAV_playership.3db) in the current system, in
    // multiplayer.
    void DrawGroupMarkers();
    // the filter-mode shading of the system's zones (the vertex / index arrays
    // BuildZoneShading fills) through the pipeline's vertex / index buffers.
    void DrawZoneShading();
    // rebuild Shade* when the view, system or filter changed (or once a
    // second when the count of qualifying zones changed); true when rebuilt.
    bool BuildZoneShading();
    // the terrain patches of the visited map zones (the arrays
    // BuildTerrainPatches fills), one textured draw per patch.
    void DrawTerrainPatches();
    // rebuild Terrain* when the view or system changed (or once a second when
    // the count of visited terrain zones changed); true when rebuilt.
    bool BuildTerrainPatches();
    // the trade lanes (LaneSegments) as animated textured bands, one of the 16
    // TradeLaneShapes frames every 1/15 s.
    void DrawTradeLanes();
    // rebuild TradeLane* when the view, system or segment count changed; true
    // when rebuilt.
    bool BuildTradeLanes();
    // the zones' outlines as lines; the whole body runs only with
    // bDrawZoneOutlines, which nothing in the exe sets (see nn_navmap_draw.cpp).
    void DrawZoneOutlines();

    // --- Members (baseline offsets) ---
    FClientMeshHolder ZoomedMapMesh;    // +0x350 zoomedmap_helion.3db, rotation * 1.5
    FClientMeshHolder ZoomedMapMesh2;   // +0x378 zoomedmap_helion.3db
    FClientMeshHolder PrettyMapMesh;    // +0x3a0 NAV_prettymap.3db, [0] * 1.175, [4] * 1.5
    float Field_0x3c8[3];               // +0x3c8 SetPosition: position + (0.2, -0.25, 0). TODO
    float Field_0x3d4[9];               // +0x3d4 ctor identity. TODO
    float ZoomScale;                    // +0x3f8 ctor 1.0f; the map's zoom (MapExtentX /
                                        //        (ZoomDistance + PanOffset z) at the end of a
                                        //        transition, 8.6 zoomed in)
    // +0x3fc -- baseline VC6 std::list<FNavMapItem*> (allocator byte, head +0x400, size +0x404).
    std::list<FNavMapItem*> Items;
    unsigned char bInitialShowDone;     // +0x408 the routine's latch
    unsigned char Pad409[3];
    float MapOrigin[3];                 // +0x40c ctor {-0.234, -0.234, 0}
    float MapSize[3];                   // +0x418 ctor {0.468, 0.468, 0}
    float ScreenRectMin[3];             // +0x424 ctor zero; Update: the map frame's top-left in
                                        //        pixels (Win3D camera projection, floored)
    float ScreenRectSize[3];            // +0x430 ctor {1, 1, 0}; Update: its size in pixels
    float MapCorner0[3];                // +0x43c InitialShow: MapOrigin
    float MapCorner1[3];                // +0x448 InitialShow: MapOrigin + (MapSize.x, 0, 0)
    float MapCorner2[3];                // +0x454 InitialShow: MapOrigin + MapSize
    float MapCorner3[3];                // +0x460 InitialShow: MapOrigin + (0, MapSize.y, 0)
    float PanOffset[3];                 // +0x46c InitialShow: zero. The map's pan in map-frame
                                        //        units (x / y; Update's drag, clamped to half
                                        //        MapSize) and, during a zoom, the distance
                                        //        offset (z)
    unsigned char bTransition;          // +0x478 ctor 0; a view transition runs
    unsigned char Pad479[3];
    float TransitionEase;               // +0x47c Update: 0.5 - 0.5 cos(pi t / T)
    float TransitionTime;               // +0x480 t; Slot23 show: = TransitionDuration
    float TransitionDuration;           // +0x484 T (0.4 s a view switch, 1.5 s a zoom)
    float TransitionDelta[3];           // +0x488 the routine: the pan (and distance) to add
    float TransitionStart[3];           // +0x494 the routine: PanOffset at the start
    float ZoomDistance;                 // +0x4a0 ctor 1.0f; the routine: MapExtentX / ZoomScale
    int TransitionView;                 // +0x4a4 the view the transition ends in (0 / 1 / 2)
    float MapExtentX;                   // +0x4a8 ctor / universe view 112000.0f; the map's half
                                        //        width in map units
    float MapExtentZ;                   // +0x4ac ctor / universe view 112000.0f; half height
    unsigned SolarTypeMask;             // +0x4b0 ctor 0x2020bdff -- the routine adds a system
                                        //        object only when its solar ObjectType has a
                                        //        bit in here (0x20000000 alone never)
    FNavMapItem* PlayerItem;            // +0x4b4 ctor 0; the routine's player marker
    // +0x4b8 / +0x644 -- the waypoint (Kind 3) and best-path (Kind 4) marker items of
    // , one slot per waypoint; ctor / dtor / the view rebuilds zero them (the items
    // themselves live in Items).
    FNavMapItem* WaypointItems[0x63];
    FNavMapItem* BestPathItems[0x63];
    int ViewMode;                       // +0x7d0 ctor 1: 0 the universe, 1 a system, 2 a system
                                        //        zoomed in (the routine)
    unsigned char bMouseDown;           // +0x7d4 ctor 0; MouseEvent arms a drag
    unsigned char bDragging;            // +0x7d5 ctor 0; the cursor moved > 4 px since then
    unsigned char Pad7d6[2];
    int DragMouseX;                     // +0x7d8 the cursor when the drag last moved
    int DragMouseY;                     // +0x7dc
    Universe::ISystem const* CurrentSystem; // +0x7e0 ctor 0
    float ShipMarkerSize;               // +0x7e4 ctor 5000.0f; the player marker's size
                                        //        (the routine) and a group member's pick radius
                                        //        (the routine)
    float WaypointMarkerSize;           // +0x7e8 ctor 4500.0f; the routine's markers use twice
                                        //        this
    WinControl* UniverseButton;         // +0x7ec "NavMapUniverseButton"
    WinControl* LabelButton;            // +0x7f0 "NavMapLabelButton"
    WinControl* SetWaypointButton;      // +0x7f4 "NavMapSetWaypointButton"
    WinControl* BestPathButton;         // +0x7f8 "NavMapBestPathButton"
    WinControl* JumpGateButton;         // +0x7fc "NavMapJumpGateButton"
    WinControl* SelectObjectButton;     // +0x800 "NavMapSelectObjectButton"
    WinControl* ShipButton;             // +0x804 "NavMapShipButton"
    WinControl* CloseButton;            // +0x808 "NavMapCloseButton"
    // +0x80c -- baseline VC6 std::list<FNavMapJumpLink*> (head +0x810, size +0x814).
    std::list<FNavMapJumpLink*> JumpLinks;
    unsigned char Field_0x818;          // +0x818 ctor 0; shows "NavMapDebugText" when clear.
                                        //        MouseEvent (0x00497fbe): while set, a release
                                        //        in a system view beams the player ship to the
                                        //        cursor and clears it. TODO: the writer of 1
    unsigned char Pad819[3];
    float MeshPosition[3];              // +0x81c ctor {0, 0, -0.71}: the three map meshes
    float Field_0x828[3];               // +0x828 ctor {0, 0, -0.55}. TODO
    unsigned char bResourcesLoaded;     // +0x834 the routine's latch
    unsigned char Pad835[3];
    unsigned CachedObjectId;            // +0x838 ctor -1; the routine's cache key
    float CachedObjectPosition[3];      // +0x83c the routine's cached position
    // +0x848 / +0x858 / +0x868 -- baseline VC6 vectors. Element types: TODO (see the users).
    // +0x848 -- the trade lane segments the routine chains out of LaneRings (VC6 vector of
    // 0x18-byte records, erase the routine, insert the routine).
    std::vector<FNavMapSegment> LaneSegments;
    // +0x858 -- the system's trade lane rings, gathered by the routine and consumed (emptied)
    // by the routine (VC6 vector of 0x7c-byte records, insert the routine).
    std::vector<FNavMapRing> LaneRings;
    // +0x868 -- the label rects placed this frame (VC6 vector of 0x14-byte records: the routine
    // clears it, the routine appends through the routine, the routine / the routine read it).
    std::vector<FNavMapLabelRect> LabelRects;
    // +0x878 ctor 0 -- the system Update switches to (from the universe view through a zoom
    // on its star, else at once); kept until it is the current one. Written by SetProperty
    // (the ship / jump gate buttons, an objective row).
    Universe::ISystem const* PendingSystem;
    unsigned char bSystemViewPending;   // +0x87c ctor 0; InitialShow 1: Update builds the system
                                        //        view once Draw has run
    unsigned char bDrawnOnce;           // +0x87d ctor 0; InitialShow 0; Draw sets it (0x004932f5)
    unsigned char Field_0x87e;          // +0x87e ctor 0; forces every universe item shown. TODO
    unsigned char Pad87f;
    FShapeDB Shapes;                    // +0x880 NavMapTextures.ini (baseline 0x18 bytes)
    unsigned char bDrawZoneOutlines;    // +0x898 ctor 0 (0x0048e337), its only writer in the
                                        //        exe; the routine draws only with it set
    unsigned char Pad899[3];
    FClientMeshHolder GroupMarkerMesh;  // +0x89c NAV_playership.3db, loaded by the routine
    unsigned char Field_0x8c4;          // +0x8c4 ctor 0. TODO
    unsigned char Pad8c5[3];
    WinControl* SystemNameText;         // +0x8c8 "NavMapSystemName"
    WinControl* DebugText;              // +0x8cc "NavMapDebugText"
    WinControl* NoBestPathText;         // +0x8d0 "NavMapNoBestPathText"
    unsigned char bViewSwitch;          // +0x8d4 ctor 0; the transition switches between the
                                        //        universe and a system (text fades, no pan)
    unsigned char Pad8d5[3];
    WinControl* PhysicalFilter;         // +0x8d8 "NavMapPhysicalFilter"
    WinControl* PoliticalFilter;        // +0x8dc "NavMapPoliticalFilter"
    WinControl* InZoomButton;           // +0x8e0 "NavMapInZoom"
    WinControl* OutZoomButton;          // +0x8e4 "NavMapOutZoom"
    WinControl* InfoButton;             // +0x8e8 "NavMapInfo"
    WinControl* ClearWaypointButton;    // +0x8ec "NavMapClearWaypointButton"
    WinControl* InfoTitle;              // +0x8f0 "NavMapInfoTitle"
    RichTextWin* InfoText;              // +0x8f4 "NavMapInfoText"
    int Field_0x8f8;                    // +0x8f8 TODO
    WinControl* ClearAllWaypointsButton; // +0x8fc "NavMapClearAllWaypointsButton"
    WinControl* ClearAllWaypointsText;  // +0x900 "NavMapClearAllWaypointsText"
    WinControl* AbortMissionButton;     // +0x904 "NavMapAbortMissionButton"
    WinControl* AbortMissionText;       // +0x908 "NavMapAbortMissionText"
    // +0x90c..+0x92b -- the map click the object tools opened on (MouseEvent 0x0049edab..
    // ): the picked point in map units, the cursor's ray point then, and the click in
    // map-frame fractions (Update also sets the two fractions from G_NavMapRequestedPos).
    float ClickMapPos[3];               // +0x90c
    float ClickRayPos[3];               // +0x918
    float ClickU;                       // +0x924
    float ClickV;                       // +0x928
    unsigned char bObjectToolsShown;    // +0x92c ctor 0; the routine 0
    unsigned char Pad92d[3];
    // +0x930 the routine(10, 0x28, the routine): the digits 0..9 the routine loads, drawn
    // over the waypoint markers by Draw (0x00492e2d..0x00492f93).
    FClientMeshHolder DigitMeshes[10];
    WinControl* AbortYesButton;         // +0xac0 "NavMapAbortYesButton"
    WinControl* AbortYesText;           // +0xac4 "NavMapAbortYesText"
    WinControl* AbortNoButton;          // +0xac8 "NavMapAbortNoButton"
    WinControl* AbortNoText;            // +0xacc "NavMapAbortNoText"
    WinControl* OptionBridge;           // +0xad0 "NavMapOptionBridge"
    WinControl* MissionObjective[3];    // +0xad4 "NavMapMissionObjective1..3"
    UiScroll* ObjectiveScroll;          // +0xae0 "NavMapMissionObjectiveScroll"
    WinControl* ObjectiveText[3];       // +0xae4 "NavMapMissionObjectiveText1..3"
    int HoveredObjective;               // +0xaf0 ctor -1; SetProperty 0x2c / 0x2d: the objective
                                        //        row under the cursor (Draw draws it last)
    int ObjectiveFirstRow;              // +0xaf4 ctor 0; the first objective shown
    WinControl* FactionLabels[4];       // +0xaf8 Helion / Vespera / Orsena / Dornmark
    WinControl* GridLetters[8];         // +0xb08 "NavMapGridLetter%c" A..H
    WinControl* GridNumbers[8];         // +0xb28 "NavMapGridNumber%d" 1..8
    unsigned char Field_0xb48;          // +0xb48 ctor 0. TODO
    unsigned char Padb49[3];
    WinControl* PatrolFilter;           // +0xb4c "NavMapPatrolFilter"
    WinControl* MissionFilter;          // +0xb50 "NavMapMissionFilter"
    WinControl* FilterTitle;            // +0xb54 "NavMapFilterTitle"
    // +0xb58 -- baseline VC6 vector<unsigned>: the zone ids of the best path's 'Z' tokens
    // (the routine).
    std::vector<unsigned> MissionZoneIds;
    FTexShape* TerrainShapes[9];        // +0xb68 the the field names
    // +0xb8c -- the zones the routine picks for the map, in three passes (VC6 vector of
    // IZone pointers, erase the routine, insert the routine).
    std::vector<Universe::IZone const*> MapZones;
    // +0xb9c..+0xbac -- the routine's cache: the waypoint's BestPathWaypointHash and what it
    // resolved to.
    unsigned CachedWaypointHash;        // +0xb9c ctor 0
    float CachedWaypointPos[3];         // +0xba0
    unsigned CachedWaypointSystem;      // +0xbac
    float BlinkTimer;                   // +0xbb0 ctor 0; Update: wraps every second
    unsigned char bBlink;               // +0xbb4 ctor 0; toggled on each wrap
    unsigned char Padbb5[3];
    WinControl* RightWindow;            // +0xbb8 "NavMapRightWindow"
    WinControl* LeftWindow;             // +0xbbc "NavMapLeftWindow"
    unsigned char bSidePanelsOpen;      // +0xbc0 ctor 0; the routine; HitTest's two extra boxes
    unsigned char Padbc1[3];
    float PulseTimer;                   // +0xbc4 ctor 0; the routine counts it down, then
                                        //        re-arms it to 0.1..2.099 s
    int PulseIndex;                     // +0xbc8 ctor -1; the routine: the item (list index)
                                        //        whose alpha pulses until the next re-arm
                                        //        (every other star is opaque)
    unsigned char bBestPathObjectives;  // +0xbcc ctor 1: objectives from the best path (0 =
                                        //        the player's waypoints)
    unsigned char Padbcd[3];
    FTexShape* WaypathShape;            // +0xbd0 "nav_waypathtexture"
    FTexShape* TradeLaneShapes[16];     // +0xbd4 "TLRanimated_01".."_16"
    WinControl* LegendFilter;           // +0xc14 "NavMapLegendFilter"
    WinControl* BaseListFilter;         // +0xc18 "NavMapBaseListFilter"
    FClientMeshHolder LegendMesh;       // +0xc1c NAV_legend.3db, loaded by Draw in mode 4
    WinControl* LegendLabels[5];        // +0xc44 Hostile / Friendly / Neutral / PlayerShip /
                                        //        MissionWaypoints
    WinControl* MineableFilter;         // +0xc58 "NavMapmineableFilter"
    WinControl* AllFilter;              // +0xc5c "NavMapAllFilter"
    WinControl* HostileFilter;          // +0xc60 "NavMapHostileFilter"
    WinControl* FriendlyFilter;         // +0xc64 "NavMapFriendlyFilter"
    unsigned char bItemClamped;         // +0xc68 ctor 0; the routine's scratch: the last
                                        //        Kind 2 item was clamped to the map edge
    unsigned char Padc69[3];
    float NoBestPathTimer;              // +0xc6c ctor 0; Update: NoBestPathText stays 8 s
    float GroupPollTimer;               // +0xc70 ctor 0; Update: group positions every 30 s
    int TradeLaneFrame;                 // +0xc74 ctor 0; the routine: the TradeLaneShapes
                                        //        frame, 0..15
    float TradeLaneFrameTimer;          // +0xc78 ctor 0; the routine steps the frame each
                                        //        1/15 s (the field)
    int LastWaypointCount;              // +0xc7c ctor -1; the routine's last
                                        //        GetPlayerWaypointCountbaseline
    FSpaceCamera Camera;                // +0xc80 the routine (0xc0 bytes)
    // +0xd40..+0xd68 -- the routine's trade lane mesh (8 vertices / 12 indices a segment,
    // FVF 0x142 {pos, diffuse, uv}, 0x18 bytes a vertex) and the view it was built for;
    // draws it through G_NavMapBufferHandles[2].
    unsigned char* TradeLaneVertices;   // +0xd40 operator new(0x6000)
    unsigned short* TradeLaneIndices;   // +0xd44 operator new(0x1800)
    int TradeLaneVertexCount;           // +0xd48 ctor 0
    int TradeLaneIndexCount;            // +0xd4c ctor 0
    unsigned TradeLaneVbOffset;         // +0xd50 CopyVertices' out offset (the routine)
    float TradeLaneCachePan[3];         // +0xd54 ctor zero; PanOffset when built
    float TradeLaneCacheZoom;           // +0xd60 ctor -1.0f; ZoomScale when built
    unsigned TradeLaneCacheSystem;      // +0xd64 ctor 0; the system's id when built
    int TradeLaneCacheSegments;         // +0xd68 ctor 0; LaneSegments' count when built
    // +0xd6c..+0xedc -- the routine's terrain patches (33 vertices / 96 indices a zone, FVF
    // 0x142, 0x18 bytes a vertex), grouped into patches by terrain shape; the routine draws
    // them through G_NavMapBufferHandles[0].
    unsigned char* TerrainVertices;     // +0xd6c operator new(0x9000)
    unsigned short* TerrainIndices;     // +0xd70 operator new(0x2400)
    int TerrainVertexCount;             // +0xd74 ctor 0
    int TerrainMinIndex[16];            // +0xd78 a patch's first vertex
    int TerrainNumVertices[16];         // +0xdb8 its vertex count
    int TerrainStartIndex[16];          // +0xdf8 its first index
    int TerrainPatchIndexCount[16];     // +0xe38 its index count
    int TerrainPatchShape[16];          // +0xe78 its TerrainShapes index
    int TerrainPatchCount;              // +0xeb8
    int TerrainIndexCount;              // +0xebc ctor 0
    float TerrainRecheckTimer;          // +0xec0 ctor / the routine 1.0f (the field)
    int TerrainZoneCount;               // +0xec4 ctor / the routine 0; visited terrain zones
                                        //        counted at the last recheck
    unsigned TerrainVbOffset;           // +0xec8 CopyVertices' out offset (the routine)
    float TerrainCachePan[3];           // +0xecc ctor zero
    float TerrainCacheZoom;             // +0xed8 ctor -1.0f
    unsigned TerrainCacheSystem;        // +0xedc ctor 0
    // +0xee0..+0xf14 -- the routine's zone shading (up to 6000 vertices, FVF 0x42 {pos,
    // diffuse}, 0x10 bytes a vertex) and the view / filter it was built for; the routine
    // draws it through G_NavMapBufferHandles[1].
    unsigned char* ShadeVertices;       // +0xee0 operator new(0x17700)
    unsigned short* ShadeIndices;       // +0xee4 operator new(0x8ca0)
    int ShadeVertexCount;               // +0xee8 ctor 0
    int ShadeIndexCount;                // +0xeec ctor 0
    float ShadeCachePan[3];             // +0xef0 ctor zero
    float ShadeCacheZoom;               // +0xefc ctor -1.0f
    unsigned ShadeCacheSystem;          // +0xf00 ctor 0
    int ShadeCacheZoneFilter;           // +0xf04 ctor -1; G_NavMapZoneFilter when built
    int ShadeCacheFilterMode;           // +0xf08 ctor -1; G_NavMapFilterMode when built
    float ShadeRecheckTimer;            // +0xf0c ctor / the routine 1.0f (the field)
    int ShadeZoneCount;                 // +0xf10 ctor 0; qualifying zones at the last recheck
    unsigned ShadeVbOffset;             // +0xf14 CopyVertices' out offset (the routine)
    WinControl* MissionBriefingText;    // +0xf18 "MissionBriefingText" (briefing only)
    WinControl* ContinueButton;         // +0xf1c "NavMapContinueButton" (briefing only)
    WinControl* ContinueText;           // +0xf20 "NavMapContinueText" (briefing only)
    unsigned char bBriefing;            // +0xf24 Create: the routine
    unsigned char Padf25[3];
    unsigned SkipSystemIds[4];          // +0xf28 Ep02 / Ep03 / Ep03b / Ep02c
    RichTextWin* InfoCard;              // +0xf38 "NavMapInfoCard"
    WinControl* CloseInfocardButton;    // +0xf3c "NavMapCloseInfocardButton"
    WinControl* CloseInfocardText;      // +0xf40 "NavMapCloseInfocardText"
    int FilterModeBeforeInfoCard;       // +0xf44 ctor 0; the filter mode 0x00497c5a saves
                                        //        before the info card (mode 6) opens
    float SystemScale;                  // +0xf48 ctor / universe view 1.0f; the NavMapScale
    WinControl* BaseTexts[15];          // +0xf4c "NavMapBaseText%d"
    WinControl* BaseSystemTexts[15];    // +0xf88 "NavMapBaseSystemText%d"
    UiScroll* BaseListScroll;           // +0xfc4 "NavMapBaseListScroll"
    WinControl* BaseTitleText;          // +0xfc8 "NavMapBaseTitleText"
    WinControl* SystemTitleText;        // +0xfcc "NavMapSystemTitleText"
    unsigned char bBaseListByBase;      // +0xfd0 ctor 1; the routine's order (1 by base name,
                                        //        0 by system then base) -- Draw colours the
                                        //        sorted column's title
    unsigned char Padfd1[3];
    // +0xfd4 -- baseline VC6 vector of 0x18-byte FNavMapBaseEntry (erase the routine, insert
    // ), filled by the routine.
    std::vector<FNavMapBaseEntry> BaseEntries;
    // +0xfe4 / +0xff4 -- baseline VC6 vectors of 8-byte wide strings ({wchar_t*, capacity},
    // element dtor the routine, copy the routine, erase the routine, insert the routine):
    // the base names and the system names the routine collects, indexed by an entry's
    // BaseIndex / SystemIndex.
    std::vector<std::wstring> BaseNames;
    std::vector<std::wstring> SystemNames;
    unsigned char bBaseListBuilt;       // +0x1004 ctor 0; the routine's latch
    unsigned char Pad1005[3];
    int BaseListFirstRow;               // +0x1008 ctor 0; the scroll bar's row (0x3e)

private:
    NeuroNetNavMap(NeuroNetNavMap const&) = delete;
    NeuroNetNavMap& operator=(NeuroNetNavMap const&) = delete;
};

// --- NavMap.cpp globals (baseline .bss/.data) ---
extern int G_NavMapFilterMode;              // 0..7 (the the routine modes)
extern int G_NavMapZoneFilter;              // 0 all / 1 hostile / 2 friendly
extern unsigned char G_bNavMapNoBestPath;   // ctor 0; IClient slot 93
                                            // (0x0053c37d) stores the best-path answer's byte;
                                            // Update shows NoBestPathText 8 s and clears it
extern unsigned G_NavMapRequestedSystem;    // the system InitialShow opens
extern float G_NavMapRequestedPos[3];       // ..8c -- the routine's focus point
extern unsigned char G_bNavMapRequestedPos; // set with it; Update consumes it
extern float G_NavMapGlobal_67203c[3];      // ..44 -- the branch {0.005, 0.048,
                                            // 0}; Update reads [0] / [1]. TODO: meaning
extern unsigned char G_bNavMapRouteChanged; // the routine 1, RefreshObjectives 0
extern unsigned char G_bNavMapShowLabels;   // .data 1; LabelButton's toggle
// the I3DMathEngine every NavMap.cpp function acquires on first use
// (the routine).
extern I3DMathEngine* G_NavMapMath;

// Not a baseline function: the sequence NavMap.cpp inlines wherever it rescales a mesh instance
// (the tool buttons * 1.7, the waypoint markers * 0.6) -- copy the instance transform, scale
// its rotation through I3DMathEngine::Scale, write it back.
void NavMapScaleMeshRotation(FMeshHolder* Holder, float Factor);

// (cdecl) -- the routine without the cache, the system looked up by id. Returns
// Out.
float* NavMapBasePosition(float* Out, unsigned BaseId, unsigned SystemId);

// (cdecl) -- the smallest zone of System's zone tree (ISystem slot 7, a 0.1 query
// sphere at Pos) that holds Pos in its XZ plane (IZone slot 0x38): with bNamed only zones with
// an ids_name, with bInfo only ones with an ids_info; unless bShowAll, only visited ones (the
// visit map | the zone's own visit byte, bit 0; with bInfo also bit 3). Null when none.
// Callers: NeuroNetNavMap::Update (0x004955dc), MouseEvent (0x0049ed8d).
Universe::IZone const* NavMapZoneAt(Universe::ISystem const* System, float const* Pos,
                                    unsigned char bNamed, unsigned char bInfo,
                                    unsigned char bShowAll);

// (cdecl) -- with a pending G_NavMapRequestedSystem, post command 0x63
// (the routine(0x63, 0)). Tail-called by NN_MainButton's update (0x0045cacf).
void NavMapPostPendingOpen();

// One group member's map position (0x18 bytes), IClient slot 96 REQUEST_GROUP_POSITIONS.
struct FNavMapGroupMember
{
    float Pos[3];      // +0x00 the record's own, or the docked base's (the routine)
    unsigned BaseId;   // +0x0c the base it is docked at (0 in space)
    unsigned SystemId; // +0x10
    int RepId;         // +0x14 its reputation id (Reputation::Vibe::GetName)
};

// the group members' positions (VC6 vector: allocator byte, first +0x04,
// last +0x08, end +0x0c; static init the branch, atexit the branch frees it).
extern std::vector<FNavMapGroupMember> G_NavMapGroupPositions;

// (cdecl) -- replace G_NavMapGroupPositions from the server's list: {int Count,
// Count x FNavMapGroupMember}. A docked member's position is its base's; a member whose position
// lies within 128 of the origin on all three axes is dropped. baseline never advances its source
// pointer, so it stores the FIRST record Count times (kept). Called by IClient slot 96
// (the branch) with its first argument; the second (the size) is not passed on.
void NavMapSetGroupPositions(void const* Data);

// (cdecl) -- find Id and make it the nav map's pending focus. bZone: a zone of any
// system with that id (G_NavMapRequestedPos = its position). Otherwise a system with that id
// (system only), else a system [Object] whose nickname hashes to Id (its position). A hit sets
// G_NavMapRequestedSystem (and, with a position, G_NavMapRequestedPos /
// G_bNavMapRequestedPos). Callers: 0x00455225, NnCommodityByBase (0x0046f6b4).
void NavMapRequestFocus(unsigned Id, unsigned char bZone);
