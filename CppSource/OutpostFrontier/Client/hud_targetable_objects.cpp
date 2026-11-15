// Author: Giperion.

#include "hud_targetable_objects.h"

#include "best_path.h"        // BestPathGetWaypointCount / BestPathGetWaypoint / resolvers
#include "cargo_bar_control.h" // ToolTipHoverInit
#include "common_archetype.h" // Archetype::Munition
#include "neuronruntime_net.h"        // CDPClient::GetPingDelay
#include "game_options.h"     // GetGameOption (the routine)
#include "menu_ui_init.h"     // PlayOrRewindUiSoundSlot (the routine)
#include "nn_messaging.h"     // NnVoiceEnqueue (the routine)
#include "nn_tooltip.h"       // NnToolTipWasFedThisFrame (the routine)
#include "tooltip_hover.h"    // FToolTipHoverInfo, NnToolTipFeedHover (the routine)
#include "character_select_equip.h" // GetPlayerStarSystemIdbaseline (the routine)
#include "client_gameobj.h"   // FClientGameobj::Slot158 (player slot +0x158)
#include "client_mesh_load.h"
#include "client_objdb.h"     // GetClientObjDb (the field)
#include "common_id.h"        // CreateID
#include "common_universe.h"  // Universe::ISystem
#include "neuroncomponents.h"            // FDUMP
#include "NeuronRuntime.h"            // NeuronRuntime::Pipeline
#include "engine_light.h"     // EngineLightClearRegisteredList (the routine)
#include "common_cequip.h"
#include "common_csys.h"      // Acquire3DMathEngine
#include "common_iface.h"     // IObjRW, IObjInspect, ObjRwFromWatchable
#include "common_misc.h"      // ICamera, SUBOBJ_ID_NONE, SUBOBJ_ID_ROOT
#include "common_physys.h"    // PhySys::FindRayCollisions, PhySys::RayHit
#include "common_reputation.h" // Reputation::Vibe::GetAffiliation, Reputation::get_short_name
#include "font_imp.h"         // FindMenuFontImp (the routine)
#include "globals.h"          // G_RenderSurfaceWidth / G_RenderSurfaceHeight
#include "widescreen.h"   // port-only (OUTPOST_FRONTIER_WIDESCREEN_SUPPORT)
#include "hud_comm.h"         // HudCommIsWatchedObjectId (the routine)
#include "hud_command_dispatch.h" // HudCommandRecord, FSpaceCommand, SpaceSendCommand
#include "hud_status_core.h"  // HudSampleShieldHitPoints (the routine)
#include "hud_target.h"       // G_HudObjectsMath, HudIsMousePickBlocked
#include "hud_waypoint.h"     // HudWaypointSelectPlayerWaypoint / HudWaypointSelectBestPath
#include "ids_strings.h"      // GetIdsStringWWithRawFallback (the routine)
#include "input_manager.h"    // GetClientSoftwareCursor (the field / the field)
#include "menu_sound_arch.h"  // GetClientSoundListenerObject (the routine)
#include "nn_montage_draw.h"  // FindButtonMontageTile (the routine)
#include "nn_text_control.h"  // NnTextControl (+0x334 Font::String, the routine / the routine)
#include "nn_ui_pulse.h"      // G_NnPaletteArgb_679ba8
#include "player_behavior.h"  // GetPlayerObjRW, IsMouseFlightHoldThresholdReached, IsMouseFlightLocked
#include "player_spawn.h"     // GetPlayerWaypointCountbaseline / GetPlayerWaypointbaseline
#include "space_camera.h"     // FSpaceCamera, AsCamera
#include "space_letterbox.h"  // IsLetterboxVisible (the routine)
#include "star_sphere.h"      // Win3DBindModelview (the routine)
#include "star_sys.h"         // StarSysGetActiveSystemId (the routine), GetAttitudeClass
#include "win3d.h"            // Win3DGetHover
#include "win3d_immediate.h"  // Win3DEnable / Win3DDisable / Win3DSetBlendFunc
#include "win3d_render.h"     // Win3DApplyCamera (the routine)
#include "win3d_renderables.h" // Win3DImmedDrawLine (the routine)
#include "x86math.h"

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <limits>
#include <string>
#include <tuple>
#include <utility>

namespace
{
    // rdata (EXE bytes verified):
    constexpr float kProjectScaleMul = 10.0f;
    constexpr float kProjectNumerFar = 3000.0f;
    constexpr float kProjectScaleGate = 100.0f;
    constexpr double kProjectNumerNearMul = 10.0;
    constexpr double kProjectNumerNearAdd = 2000.0;
    constexpr float kProjectRadiusMin = 10.0f;
    constexpr float kProjectRadiusMax = 100.0f;

    // The inlined `+0xc (the CBase face) -> +4 (CBase::Object)` chain on an IObjRW / gameobj.
    CObject* CObjectOfObjRW(IObjRW* ObjRW)
    {
        CBase const* const Base = static_cast<IObjInspectImpl*>(ObjRW);
        return Base->cobject();
    }

    CObject* CObjectOfGameobj(FClientGameobj* Gameobj)
    {
        CBase const* const Base = static_cast<IObjInspectImpl*>(Gameobj);
        return Base->cobject();
    }

    // The ctor's IDS fetch (0x004ea505..0x004ea527 / 0x004ea569..0x004ea58a): the routine into
    // a 0x80-wchar stack buffer; a zero result leaves the buffer as the empty string.
    void LoadLabelIds(unsigned Ids, wchar_t* Buffer)
    {
        if (GetIdsStringWWithRawFallback(Ids, Buffer, 0x80) == 0)
            Buffer[0] = L'\0';
    }

    // The ctor's alert block (0x004e98d4..0x004e9bca): the routine with a 0.018 square mouse
    // box, then SetProperty(0x18, red, red); SetProperty(3, Ids, 0) unless Ids is 0 (the lag
    // alert has none); SetProperty(1, 1, 0).
    WinControl* CreateAlert(WinControl* Parent, char const* Name, char const* MeshPath,
                            float const* Position, unsigned Ids)
    {
        float const Zero[3] = {0.0f, 0.0f, 0.0f};
        float const HalfExtents[3] = {0.018f, 0.018f, 0.0f};
        WinControl* const Alert = Parent->CreateStaticControl(
            Name, MeshPath, Position, nullptr, HalfExtents, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, Zero);
        uintptr_t const Red = reinterpret_cast<uintptr_t>(&G_NnPaletteArgb_679ba8);
        Alert->SetProperty(0x18, Red, Red);
        if (Ids != 0)
            Alert->SetProperty(3, Ids, 0);
        Alert->SetProperty(1, 1, 0);
        return Alert;
    }
}

HudTargetableObjects* G_HudTargetableObjects = nullptr;

HudTargetableObjects::HudTargetableObjects(INI_Reader& /*Reader*/)
{
    // . HUDItem inlined, WinControl the routine, the handler vptr, the contact
    // vector (0x004e906b..0x004e907d).

    // both standalone records zeroed with +0x2f = 1.
    std::memset(&WaypointRecord, 0, sizeof(WaypointRecord));
    WaypointRecord.bValid = 1;
    std::memset(&BestPathRecord, 0, sizeof(BestPathRecord));
    BestPathRecord.bValid = 1;
    // +0x3e8 / +0x3ec are not written by the ctor (operator_new memory); the routine sets both
    // before any read. The port starts them at 0.
    HotIndex = 0;
    HotScreenRadius = 0.0f;
    // .
    RayHit.Object = nullptr;
    RayHit.Instance = -1;
    RayHit.Point.x = 0.0f;
    RayHit.Point.y = 0.0f;
    RayHit.Point.z = 0.0f;
    RayHit.Distance = 0.0f;

    // the 19 holders (the routine, the five floats 1.0f, anim -1).
    FClientMeshHolder* const Holders[19] = {
        &TargetArrow, &TargetArrowHat, &TargetArrowPlayer, &TargetHealth, &TargetShields,
        &TargetArrowQuotes, &NonselectedArrow, &NonselectedArrowHat, &NonselectedArrowQuotes,
        &NonselectedPlayerArrow, &LootArrow, &Reticle, &ReticleQuotes, &ReticleArrows,
        &ReticleHealth, &ReticleShields, &WaypointReticle, &SelectedWaypoint,
        &NonselectedWaypoint,
    };
    for (int Index = 0; Index < 19; ++Index)
        ClientMeshHolderConstruct(Holders[Index]);

    // the five Font::Strings (the routine), the map (the routine) and
    // the list (a fresh head node) are the members' own constructors.

    // the vptrs, the field, HUDItem's name, then Create's body
    // inlined -- SetPosition(own position) through slot 5, Win3D::Create the routine(0, 0)
    // called directly, RegisterEvent(6) on success -- then the Win3D name and the class name.
    G_HudTargetableObjects = this;
    HUDItem::Name = "Targetable_Objects";
    SetPosition(Position);
    if (Win3D::Create(nullptr, nullptr))
        RegisterEvent(6);
    Win3D::SetName("Targetable_Objects");
    ClassName = "Targetable_Objects";

    // .
    WaypointRecord.bIsWaypoint = 1;   // +0x3b1
    BestPathRecord.bIsBestPath = 1;   // +0x3e4
    Scanner = nullptr;
    ScanEntries = nullptr;
    RayObject = nullptr;
    WaypointRecord.TypeFlags = 0x200;
    BestPathRecord.TypeFlags = 0x200;
    MissileBlinkTimer = 0.1f;
    MissileBlinkPeriod = 0.1f;
    bMissileBlinkOn = 1;
    TrailBlinkTimer = 0.5f;
    TrailBlinkPeriod = 0.5f;
    bTrailBlinkOn = 1;
    RadiationBlinkTimer = 0.7f;
    bRadiationBlinkOn = 1;
    LagTimer = 1.0f;
    bLagBlinkOn = 0;
    PickSubObjId = SUBOBJ_ID_NONE;
    PickId = 0;

    // the range read-out under the reticle.
    float const RangePos[3] = {0.003f, 0.0f, 0.0f};
    ReticleRangeText = CreateTextControl("ReticleRangeText", "HudSmall", RangePos, nullptr,
                                         0.171f /* 0x3e2f1aa0 */, 0.03f  ,
                                         1.0f, 0x453, 1, 0, 0);
    ReticleRangeText->SetProperty(1, 1, 0);
    unsigned char const White[4] = {0xff, 0xff, 0xff, 0xff};
    ReticleRangeText->SetProperty(0x33, reinterpret_cast<uintptr_t>(White), 1);

    // the four alert icons.
    float const MissilePos[3] = {0.126f, 0.213f, -0.6f};     // 0x3e5a1cac 0xbf19999a
    MissileAlert = CreateAlert(this, "MissileAlert", "Interface\\hud\\HUD_missilealert.3db",
                               MissilePos, 0x4d0);
    float const TrailPos[3] = {-0.126f, 0.213f, -0.6f};
    TrailAlert = CreateAlert(this, "TrailAlert", "Interface\\hud\\HUD_trailalert.3db", TrailPos,
                             0x616);
    float const RadiationPos[3] = {0.126f, 0.177f, -0.6f};
    RadiationAlert = CreateAlert(this, "RadiationAlert", "Interface\\hud\\HUD_radiationalert.3db",
                                 RadiationPos, 0x617);
    float const LagPos[3] = {-0.126f, 0.177f, -0.6f};
    LagAlert = CreateAlert(this, "LagAlert", "Interface\\multiplayer\\MULTI_lag.3db", LagPos, 0);

    // the routine(path, 0, 1), in this order.
    ClientMeshLoadFromPath(&TargetArrow, "interface\\hud\\hud_targetarrow.3db", 0, true);
    ClientMeshLoadFromPath(&TargetArrowHat, "interface\\hud\\hud_targetarrowhat.3db", 0, true);
    ClientMeshLoadFromPath(&TargetArrowPlayer, "interface\\hud\\hud_targetarrowplayer.3db", 0,
                           true);
    ClientMeshLoadFromPath(&TargetHealth, "interface\\hud\\hud_targethealth.3db", 0, true);
    ClientMeshLoadFromPath(&TargetShields, "interface\\hud\\hud_targetshields.3db", 0, true);
    ClientMeshLoadFromPath(&TargetArrowQuotes, "interface\\hud\\hud_targetarrowquotes.3db", 0,
                           true);
    ClientMeshLoadFromPath(&NonselectedArrow, "interface\\hud\\hud_nonselectedarrow.3db", 0, true);
    ClientMeshLoadFromPath(&NonselectedArrowHat, "interface\\hud\\hud_nonselectedarrowhat.3db", 0,
                           true);
    ClientMeshLoadFromPath(&NonselectedPlayerArrow,
                           "interface\\hud\\hud_nonselectedplayerarrow.3db", 0, true);
    ClientMeshLoadFromPath(&LootArrow, "interface\\hud\\hud_lootarrow.3db", 0, true);
    ClientMeshLoadFromPath(&NonselectedArrowQuotes,
                           "interface\\hud\\hud_nonselectedarrowquotes.3db", 0, true);

    // the bar extents. the routine fills {maxX, minX, maxY, minY,
    // maxZ, minZ} into one stack buffer that every query below reuses; a query on a holder with
    // no instance leaves it as the previous query left it.
    FClientMeshAabb Extents;
    ClientMeshQueryAabb(&TargetHealth, &Extents);
    HealthBarRect.Left = Extents.Values[1];
    HealthBarRect.Width = Extents.Values[0] - Extents.Values[1];
    HealthBarRect.Top = Extents.Values[4];
    HealthBarRect.Bottom = Extents.Values[5];
    ClientMeshQueryAabb(&TargetShields, &Extents);
    ShieldBarRect.Width = Extents.Values[0] - Extents.Values[1];
    ShieldBarRect.Bottom = Extents.Values[5];
    ShieldBarRect.Left = Extents.Values[1];
    ShieldBarRect.Top = Extents.Values[4];

    // .
    ClientMeshLoadFromPath(&Reticle, "interface\\hud\\hud_reticle.3db", 0, true);
    ClientMeshLoadFromPath(&ReticleQuotes, "interface\\hud\\hud_reticle_quotes.3db", 0, true);
    ClientMeshLoadFromPath(&ReticleArrows, "interface\\hud\\hud_reticle_arrows.3db", 0, true);
    ClientMeshLoadFromPath(&ReticleHealth, "interface\\hud\\hud_reticle_health.3db", 0, true);
    ClientMeshLoadFromPath(&ReticleShields, "interface\\hud\\hud_reticle_shields.3db", 0, true);
    ClientMeshLoadFromPath(&WaypointReticle, "interface\\hud\\hud_waypointreticle.3db", 0, true);
    ClientMeshLoadFromPath(&SelectedWaypoint, "interface\\hud\\hud_selectedwaypoint.3db", 0, true);
    ClientMeshLoadFromPath(&NonselectedWaypoint, "interface\\hud\\hud_nonselectedwaypoint.3db", 0,
                           true);

    // .
    ClientMeshQueryAabb(&WaypointReticle, &Extents);
    WaypointReticleWidth = Extents.Values[0] - Extents.Values[1];
    WaypointReticleHeight = Extents.Values[2] - Extents.Values[3];
    ClientMeshQueryAabb(&Reticle, &Extents);
    ReticleRect.Left = Extents.Values[1];
    ReticleRect.Top = Extents.Values[2] - Extents.Values[3];
    ReticleRect.Bottom = Extents.Values[3];
    ReticleRect.Width = Extents.Values[0] - Extents.Values[1];
    ClientMeshQueryAabb(&ReticleHealth, &Extents);
    ReticleHealthRect.Left = Extents.Values[1];
    ReticleHealthRect.Width = Extents.Values[0] - Extents.Values[1];
    ReticleHealthRect.Top = Extents.Values[2];
    ReticleHealthRect.Bottom = Extents.Values[3];
    ClientMeshQueryAabb(&ReticleShields, &Extents);
    ReticleShieldsRect.Width = Extents.Values[0] - Extents.Values[1];
    ReticleShieldsRect.Left = Extents.Values[1];
    ReticleShieldsRect.Bottom = Extents.Values[3];
    ReticleShieldsRect.Top = Extents.Values[2];

    // the ButtonMontage tiles (the routine), in baseline's order.
    bMissileWarned = 0;
    CurrentTargetCorner = FindButtonMontageTile(std::string("CurrentTargetCorner"));
    CurrentTargetHat = FindButtonMontageTile(std::string("CurrentTargetHat"));
    WaypointTarget = FindButtonMontageTile(std::string("WaypointTarget"));
    WaypointArrow = FindButtonMontageTile(std::string("WaypointArrow"));
    LootArrowTile = FindButtonMontageTile(std::string("LootArrow"));
    NSTargetArrow = FindButtonMontageTile(std::string("NSTargetArrow"));
    NSTargetHat = FindButtonMontageTile(std::string("NSTargetHat"));
    PlayerArrow = FindButtonMontageTile(std::string("PlayerArrow"));
    CurrentTargetArrow = FindButtonMontageTile(std::string("CurrentTargetArrow"));
    CurrentTargetArrowOutline = FindButtonMontageTile(std::string("CurrentTargetArrowOutline"));
    CurrentTargetArrowHat = FindButtonMontageTile(std::string("CurrentTargetArrowHat"));
    NSArrowBackground = FindButtonMontageTile(std::string("NSArrowBackground"));
    LootArrowBackground = FindButtonMontageTile(std::string("LootArrowBackground"));
    WaypointArrowBackground = FindButtonMontageTile(std::string("WaypointArrowBackground"));
    PlayerArrowBackground = FindButtonMontageTile(std::string("PlayerArrowBackground"));

    // .
    SmallFont = FindMenuFontImp("HudSmall");
    PlayerNameFont = FindMenuFontImp("PlayerName");

    // the two waypoint captions, scaled to twice the reticle width.
    wchar_t Caption[0x80];
    LoadLabelIds(0x442, Caption);
    FontStringSetFont(&WaypointLabel, SmallFont);
    FontStringSetScale(&WaypointLabel, WaypointReticleWidth + WaypointReticleWidth, -1.0f);
    FontStringSetText(&WaypointLabel, Caption);
    LoadLabelIds(0x443, Caption);
    FontStringSetFont(&BestPathLabel, SmallFont);                 // inlined the routine
    FontStringSetScale(&BestPathLabel, WaypointReticleWidth + WaypointReticleWidth, -1.0f);
    FontStringSetText(&BestPathLabel, Caption);

    // .
    NameCacheSimpleId = 0;
    NameCacheKey848 = -1;
    NameCacheKey84c = -1;
    TargetInfoKey = -1;
    NameCacheTimer = 0.0f;   // dword 0
    AimPoint.z = 0.0f;
    AimPoint.y = 0.0f;
    AimPoint.x = 0.0f;
    bFireAttitude = 0;
    FireAttitude = 0;
    LastTargetId = 0;
    SelectPulse = 0.0f;
    bPickPending = 0;
}

HudTargetableObjects::~HudTargetableObjects()
{
    // . 0x004ea7f3..0x004ea82d: the five Font::Strings emptied (the routine).
    FontStringClearContents(&WaypointLabel);
    FontStringClearContents(&BestPathLabel);
    FontStringClearContents(&TargetNameText);
    FontStringClearContents(&TargetInfoText);
    FontStringClearContents(&RangeText);

    // every name label emptied, then the whole map erased (the routine;
    // each node's Font::String destructor runs there).
    for (std::map<unsigned, FHudNameLabel>::iterator It = NameLabels.begin();
         It != NameLabels.end(); ++It)
    {
        FontStringClearContents(&It->second.Text);
    }
    NameLabels.clear();

    // the routine on each holder, in this order.
    ClientUiMeshHolderRelease(&TargetArrow);
    ClientUiMeshHolderRelease(&TargetArrowHat);
    ClientUiMeshHolderRelease(&TargetArrowPlayer);
    ClientUiMeshHolderRelease(&TargetHealth);
    ClientUiMeshHolderRelease(&TargetShields);
    ClientUiMeshHolderRelease(&TargetArrowQuotes);
    ClientUiMeshHolderRelease(&NonselectedArrow);
    ClientUiMeshHolderRelease(&NonselectedPlayerArrow);
    ClientUiMeshHolderRelease(&LootArrow);
    ClientUiMeshHolderRelease(&NonselectedArrowHat);
    ClientUiMeshHolderRelease(&NonselectedArrowQuotes);
    ClientUiMeshHolderRelease(&Reticle);
    ClientUiMeshHolderRelease(&ReticleQuotes);
    ClientUiMeshHolderRelease(&ReticleArrows);
    ClientUiMeshHolderRelease(&ReticleHealth);
    ClientUiMeshHolderRelease(&ReticleShields);
    ClientUiMeshHolderRelease(&WaypointReticle);
    ClientUiMeshHolderRelease(&SelectedWaypoint);
    ClientUiMeshHolderRelease(&NonselectedWaypoint);

    // .
    G_HudTargetableObjects = nullptr;
    LeadMarkers.clear();

    // the member destructors of the Font::Strings (the routine), last
    // declared first (the list and the map free their head nodes in their own destructors).
    // Declared after the holders, so their member dtors would run first anyway; ~FFontString
    // then runs on empty strings.
    FontStringDestroy(&RangeText);
    FontStringDestroy(&TargetInfoText);
    FontStringDestroy(&TargetNameText);
    FontStringDestroy(&BestPathLabel);
    FontStringDestroy(&WaypointLabel);

    // the holders' destructor the routine, last declared first
    // (NonselectedWaypoint .. TargetArrow) -- ~FClientMeshHolder as member dtors after the
    // strings above; the same release again, which finds nothing bound.
    // Then the contact vector's buffer, Event::Handler the routine, ~WinControl the routine
    // and HUDItem's watcher -- the base and member destructors.
}

void HudTargetableObjects::SetTarget(IObjRW* Target)
{
    // .
    HUDItem::SetTarget(Target);
    Scanner = nullptr;

    Watchable const* const Watched = HUDItem::Target.target();
    IObjRW* const ObjRW = ObjRwFromWatchable(const_cast<Watchable*>(Watched));
    if (ObjRW == nullptr)
        return;

    // -8, +0xc (the CBase face), +4 (CBase::Object), a CShip class test;
    // a miss anywhere leaves EAX = 0.
    CShip* Ship = nullptr;
    CBase const* const Base = static_cast<IObjInspectImpl*>(ObjRW);
    if (Base != nullptr)
    {
        CObject* const Object = Base->cobject();
        if (Object != nullptr &&
            (static_cast<unsigned>(Object->ObjectClass) & 0x503u) == 0x503u)
        {
            Ship = static_cast<CShip*>(Object);
        }
    }
    // baseline runs FindFirst on `EAX + 0xe4` without testing EAX, so a
    // watched object that is not a ship reaches CEquipManager::FindFirst with this = 0xe4. The
    // only broadcaster (the routine from 0x0051cbea) hands every HUD item the player's ship.
    CEquip const* const Equip = Ship->EquipManager.FindFirst(EquipTypeScanner);
    Scanner = CEScanner::cast(const_cast<CEquip*>(Equip));
}

bool HudTargetableObjects::Create(Win3D* NewParent, void* Data)
{
    // (this = the WinControl face).
    SetPosition(Position);   // slot 5 (+0x14) with +0x60
    if (!Win3D::Create(NewParent, Data))
        return false;
    RegisterEvent(6);        // on the handler at +0x32c of the face
    return true;
}

bool HudTargetableObjects::OnEvent(unsigned MsgId)
{
    // (this = the Event::Handler face, +0x364).
    if (HudIsMousePickBlocked())
        return false;
    if (Win3DGetHover() != nullptr)
        return false;
    if (MsgId != 6)
        return false;
    // mouse flight held past its threshold and not locked -> no pick.
    if (IsMouseFlightHoldThresholdReached() && !IsMouseFlightLocked())
        return false;

    // a key-down record re-picks quietly and remembers the result.
    if (HudCommandRecord()->Byte18 != 0)
    {
        bPickPending = PickUnderCursor(true) ? 1 : 0;
        return false;
    }
    // .
    if (HudCommandRecord()->Byte1A != 0)
        return false;

    // .
    if (PickId == 0 && bPickPending == 0)
        PickUnderCursor(false);

    // "select target" (0x1c) for the picked object.
    bPickPending = 0;
    if (PickId == 0)
        return false;
    FSpaceCommand Command;
    std::memset(&Command, 0, sizeof(Command));
    Command.Id = 0x1c;
    Command.ObjectId = PickId;
    Command.SubObjectId = PickSubObjId;
    Command.Byte26 = 1;
    SpaceSendCommand(&Command);
    return true;
}

Vector* HudTargetableObjects::GetAimPoint(Vector* Out) const
{
    // the three dwords at +0x91c; returns Out.
    *Out = AimPoint;
    return Out;
}

void HudTargetableObjects::HideAlerts()
{
    // 's body: AND 0xfc on Win3D +0x6c of the four alerts.
    MissileAlert->Flags &= 0xfc;
    TrailAlert->Flags &= 0xfc;
    RadiationAlert->Flags &= 0xfc;
    LagAlert->Flags &= 0xfc;
}

void HudTargetableObjectsHideAlertsInLetterbox()
{
    // .
    if (G_HudTargetableObjects == nullptr)
        return;
    if (!IsLetterboxVisible())
        return;
    G_HudTargetableObjects->HideAlerts();
}

bool HudTargetableObjects::ProjectContact(Vector const* World, float Radius, Vector* OutScreen,
                                          float* OutScreenRadius, float* OutDistance)
{
    // (RET 0x14; ECX is not read). The listener camera (the routine).
    ICamera* const Camera = static_cast<ICamera*>(GetClientSoundListenerObject());
    float const* const View = Camera->GetViewTransform();   // slot 16 (+0x40)
    if (G_HudObjectsMath == nullptr)
        G_HudObjectsMath = Acquire3DMathEngine();
    // math slot 0x28 TransformXform(out, view, world).
    FVector ViewLocal;
    FVector const WorldPoint = {World->x, World->y, World->z};
    G_HudObjectsMath->TransformXform(ViewLocal, *reinterpret_cast<FTransform const*>(View),
                                     WorldPoint);

    // the out vector sits at [ESP+0x10] (LEA EDX,[ESP+0x18] after two
    // PUSHes), so FLD [ESP+0x18] reads its Z. -viewZ is parked (FSTP [ESP+0x44]), then ST0 =
    // ZNear is FCOMPed against it: TEST AH,0x41 / JP skips the projection when C3 and C0 are both
    // clear (ZNear > -viewZ) or both set (unordered); it falls through on C0 (ZNear < -viewZ) or
    // C3 (equal) -- projection is tried only for an ordered ZNear <= -viewZ.
    float const NegViewZ = -ViewLocal.Z;
    float const ZNear = Camera->GetZNear();   // slot 8 (+0x20)
    bool bOnScreen = false;
    if (ZNear <= NegViewZ)
    {
        // slot 17 (+0x44) ProjectVec straight into OutScreen.
        if (Camera->ProjectVec(&OutScreen->x, &World->x) != 0)
        {
            // clamp x and y into the surface; any clamp means "off
            // screen". z is zeroed either way.
            int const MaxX = static_cast<int>(G_RenderSurfaceWidth) - 1;
            int const MaxY = static_cast<int>(G_RenderSurfaceHeight) - 1;
            bool bClamped = false;
            if (OutScreen->x < 0.0f)
            {
                OutScreen->x = 0.0f;
                bClamped = true;
            }
            else if (static_cast<float>(MaxX) < OutScreen->x)
            {
                OutScreen->x = static_cast<float>(MaxX);
                bClamped = true;
            }
            if (OutScreen->y < 0.0f)
            {
                OutScreen->y = 0.0f;
                bClamped = true;
            }
            else if (static_cast<float>(MaxY) < OutScreen->y)
            {
                OutScreen->y = static_cast<float>(MaxY);
                bClamped = true;
            }
            bOnScreen = !bClamped;
            OutScreen->z = 0.0f;
        }
    }

    // the distance to the player (the routine, slot 0 get_position),
    // summed as x*x + z*z + y*y on the x87 stack and stored every time.
    IObjRW* const Player = static_cast<IObjRW*>(GetPlayerObjRW());
    Vector const& PlayerPos = Player->get_position();
    double const Dx = static_cast<double>(PlayerPos.x) - static_cast<double>(World->x);
    double const Dy = static_cast<double>(PlayerPos.y) - static_cast<double>(World->y);
    double const Dz = static_cast<double>(PlayerPos.z) - static_cast<double>(World->z);
    double const Distance = std::sqrt(Dx * Dx + Dz * Dz + Dy * Dy);
    *OutDistance = static_cast<float>(Distance);
    if (!bOnScreen)
        return false;

    // Scale = Distance / Radius * 10; Numer = Scale * 10 + 2000 when
    // Scale < 100 (ordered), else 3000; R = Numer / Scale, stored rounded.
    double const Scale = Distance / static_cast<double>(Radius) * static_cast<double>(kProjectScaleMul);
    double Numer = static_cast<double>(kProjectNumerFar);
    if (Scale < static_cast<double>(kProjectScaleGate))
        Numer = kProjectNumerNearMul * Scale + kProjectNumerNearAdd;
    double const ScreenRadius = Numer / Scale;
    *OutScreenRadius = static_cast<float>(ScreenRadius);
    // the unrounded value is clamped into [10, 100].
    if (ScreenRadius < static_cast<double>(kProjectRadiusMin))
        *OutScreenRadius = kProjectRadiusMin;
    else if (ScreenRadius > static_cast<double>(kProjectRadiusMax))
        *OutScreenRadius = kProjectRadiusMax;
    return true;
}

void HudTargetableObjects::RebuildContacts()
{
    // . 0x004ed18a..0x004ed1da: +0x36c = Scanner + 0x24 (no null test -- Render only
    // runs with the player's ship, which always carries a scanner), then the vector emptied.
    ScanEntries = &Scanner->Scans;
    Contacts.clear();

    // the player (kept for the tail) and its ship.
    IObjRW* const Player = static_cast<IObjRW*>(GetPlayerObjRW());
    CShip* PlayerShip = nullptr;
    if (Player != nullptr)
    {
        CObject* const Object = CObjectOfObjRW(Player);
        if (Object != nullptr && (static_cast<unsigned>(Object->ObjectClass) & 0x503u) == 0x503u)
            PlayerShip = static_cast<CShip*>(Object);
    }

    // the nav waypoint -- the player's first waypoint, when it lies in the
    // current system. A waypoint on an object takes that object's position and radius.
    WaypointRecord.bOnScreen = 0;
    WaypointRecord.bValid = 0;
    WaypointRecord.ObjectId = 0;
    if (GetPlayerWaypointCountbaseline() > 0)
    {
        FPlayerWaypointRecord const* const Waypoint = GetPlayerWaypointbaseline(0);
        if (Waypoint != nullptr && Waypoint->SystemId == GetPlayerStarSystemIdbaseline())
        {
            Vector Pos = Waypoint->Pos;
            float Radius = 40.0f;
            if (Waypoint->ObjectId != 0)
            {
                FClientGameobj* const Gameobj = ClientObjDb_FindObject(Waypoint->ObjectId, 0);
                if (Gameobj != nullptr)
                {
                    WaypointRecord.ObjectId = Waypoint->ObjectId;
                    CObject const* const Object = CObjectOfGameobj(Gameobj);   // slot 0x150
                    Radius = Object->Radius;                             // +0x38
                    Pos = Object->WorldPosition;                         // +0x2c
                }
            }
            WaypointRecord.World = Pos;
            WaypointRecord.bValid = 1;
            WaypointRecord.bOnScreen =
                ProjectContact(&Pos, Radius, &WaypointRecord.Screen, &WaypointRecord.ScreenRadius,
                               &WaypointRecord.Distance) ? 1 : 0;
            // TEST AH,5 / JP: only an ordered < 30.0f (the field) is raised.
            if (WaypointRecord.ScreenRadius < 30.0f)
                WaypointRecord.ScreenRadius = 30.0f;
        }
    }

    // the best-path waypoint -- the first waypoint of the first route,
    // when it resolves to a position in the current system.
    BestPathRecord.bOnScreen = 0;
    BestPathRecord.bValid = 0;
    BestPathRecord.ObjectId = 0;
    if (BestPathGetWaypointCount() > 0)
    {
        FBestPathWaypoint* const Waypoint = BestPathGetWaypoint(0);
        if (Waypoint != nullptr)
        {
            Vector Pos;
            unsigned System = 0;
            if (BestPathWaypointResolvePosition(Waypoint, &Pos, &System) &&
                System == GetPlayerStarSystemIdbaseline())
            {
                BestPathRecord.World = Pos;
                BestPathRecord.bValid = 1;
                IObjRW* const Object = BestPathWaypointResolveObject(Waypoint);
                BestPathRecord.ObjectId = (Object != nullptr) ? Object->get_id() : 0;
                BestPathRecord.bOnScreen =
                    ProjectContact(&Pos, 40.0f, &BestPathRecord.Screen,
                                   &BestPathRecord.ScreenRadius, &BestPathRecord.Distance) ? 1 : 0;
                if (BestPathRecord.ScreenRadius < 30.0f)
                    BestPathRecord.ScreenRadius = 30.0f;
            }
        }
    }

    // .
    IObjRW* HotObject = nullptr;   // [ESP+0x44]
    float HotDistance = 0.0f;      // [ESP+0x14]
    HotScreenRadius = 1.0e8f;
    HotIndex = -1;
    RayObject = nullptr;
    GetClientSoundListenerObject();   // ; the result is not used

    int CursorX = 0;
    int CursorY = 0;
    int CursorZ = 0;
    GetClientSoftwareCursor(&CursorX, &CursorY, &CursorZ);   // / the field

    // one record per scanned object.
    for (int Index = 0; Index < ScanEntries->Count; ++Index)
    {
        Watchable const* const Watched = ScanEntries->Entries[Index].target();
        if (Watched == nullptr)
            continue;
        IObjRW* const Object = ObjRwFromWatchable(const_cast<Watchable*>(Watched));
        if (Object == nullptr)
            continue;

        // the routine inserts a zeroed record with +0x2f = 1 at end().
        FHudContactRecord Blank;
        std::memset(&Blank, 0, sizeof(Blank));
        Blank.bValid = 1;
        Contacts.push_back(Blank);
        FHudContactRecord& Record = Contacts.back();
        Record.TypeFlags = 0;
        Record.bHot = 0;
        Record.bWatchListed = 0;
        if (PlayerShip == nullptr)
            continue;

        // .
        Record.World = Object->get_position();
        Record.bIsBestPath =
            (BestPathRecord.bValid != 0 && Object->get_id() == BestPathRecord.ObjectId) ? 1 : 0;
        Record.bIsWaypoint =
            (WaypointRecord.bValid != 0 && Object->get_id() == WaypointRecord.ObjectId) ? 1 : 0;
        Record.ObjectId = Object->get_id();

        // .
        unsigned Type = 0;
        if (Object->get_type(Type) != 0)
            continue;
        CObject const* const Target = Object->cobject();
        if (Target != nullptr && (static_cast<unsigned>(Target->ObjectClass) & 0x503u) == 0x503u &&
            static_cast<CShip const*>(Target)->bIsPlayer != 0)
        {
            Record.bPlayerShip = 1;
        }
        if ((Type & 0x20004000u) != 0)
            continue;

        // the client gameobj's +0xa8 byte, looked up in the bucket of
        // the type (1 ships, 2 other simples, 7 0x10000000).
        int Kind = 0;
        if ((Type & 0x7f0000u) != 0)
            Kind = 1;
        else if ((Type & 0xfdffu) != 0)
            Kind = 2;
        else if ((Type & 0x10000000u) != 0)
            Kind = 7;
        if (Kind != 0)
        {
            FClientGameobj* const Gameobj = ClientObjDb_FindObject(Object->get_id(), Kind);
            if (Gameobj != nullptr)
                Record.bWatchListed = Gameobj->bWatchListed;
        }

        // slot 5 get_center_of_mass, the CObject's slot 0x64 radius.
        Vector const Center = Object->get_center_of_mass();
        Vector PartCenter;
        float const Radius = Object->cobject()->get_physical_radius_r(PartCenter);
        Vector Screen;
        float ScreenRadius = 0.0f;
        float Distance = 0.0f;
        if (!ProjectContact(&Center, Radius, &Screen, &ScreenRadius, &Distance))
        {
            // .
            Record.TypeFlags = Type;
            Record.bOnScreen = 0;
            Record.Distance = Distance;
            continue;
        }

        // .
        Record.TypeFlags = Type;
        Record.Screen = Screen;
        Record.ScreenRadius = ScreenRadius;
        Record.Distance = Distance;
        Record.bOnScreen = 1;

        // the cursor inside the bracket box, edges included (FILD of
        // the cursor, the float edges exact on the x87 stack).
        double const MouseX = static_cast<double>(CursorX);
        double const MouseY = static_cast<double>(CursorY);
        if (!(MouseX >= static_cast<double>(Screen.x) - static_cast<double>(ScreenRadius)))
            continue;
        if (!(MouseX <= static_cast<double>(Screen.x) + static_cast<double>(ScreenRadius)))
            continue;
        if (!(MouseY >= static_cast<double>(Screen.y) - static_cast<double>(ScreenRadius)))
            continue;
        if (!(MouseY <= static_cast<double>(Screen.y) + static_cast<double>(ScreenRadius)))
            continue;

        // with a candidate, a larger (or unordered) radius takes it
        // (TEST AH,0x41 / JP); then any radius but an equal one takes it (TEST AH,0x44 / JP);
        // an equal radius goes to the one not farther away (TEST AH,0x41 / JZ skips only an
        // ordered >).
        bool bTake = false;
        if (HotObject != nullptr && !(ScreenRadius < HotScreenRadius) &&
            !(ScreenRadius == HotScreenRadius))
        {
            bTake = true;
        }
        else if (!(ScreenRadius == HotScreenRadius))
        {
            bTake = true;
        }
        else if (!(Distance > HotDistance))
        {
            bTake = true;
        }
        if (bTake)
        {
            // .
            HotIndex = Index;
            HotObject = Object;
            HotDistance = Distance;
            HotScreenRadius = ScreenRadius;
        }
    }

    // . The scan index doubles as the vector index (baseline does the same,
    // though a skipped null watcher leaves the two out of step).
    if (HotIndex >= 0)
    {
        Contacts.data()[HotIndex].bHot = 1;
        return;
    }

    // nothing bracketed under the cursor -- fall back to the object the
    // camera ray hit (+0x3f0), when it is a simple, not of type 0x20004000, and closer than 45
    // of its radii.
    if (PlayerShip == nullptr)
        return;
    CObject* const RayTarget = RayHit.Object;
    if (RayTarget == nullptr)
        return;
    if ((static_cast<unsigned>(RayTarget->ObjectClass) & 3u) != 3u)
        return;
    CSimple* Pick = static_cast<CSimple*>(RayTarget);
    if ((Pick->TypeBits & 0x20004000u) != 0)
        return;
    Vector RayCenter;
    float const RayRadius = RayTarget->get_physical_radius_r(RayCenter);   // slot 0x64
    Vector const& PlayerPos = Player->get_position();
    double const Dx = static_cast<double>(RayCenter.x) - static_cast<double>(PlayerPos.x);
    double const Dy = static_cast<double>(RayCenter.y) - static_cast<double>(PlayerPos.y);
    double const Dz = static_cast<double>(RayCenter.z) - static_cast<double>(PlayerPos.z);
    double const Ratio = std::sqrt(Dx * Dx + Dz * Dz + Dy * Dy) / static_cast<double>(RayRadius);
    // TEST AH,5 / JP: only an ordered ratio < 45.0f (the field) goes on.
    if (!(Ratio < 45.0))
        return;

    // a solar with a parent picks the parent, when the parent's client
    // object is known; a known parent that is not a solar ends the pick.
    if ((static_cast<unsigned>(RayTarget->ObjectClass) & 0x303u) == 0x303u)
    {
        ID_String const& Parent = static_cast<CSolar*>(RayTarget)->GetParentNickname();
        if (!Parent.IsEmpty())
        {
            FClientGameobj* const Gameobj = ClientObjDb_FindObject(Parent.Id, 2);
            if (Gameobj != nullptr)
            {
                CObject* const ParentObject = CObjectOfGameobj(Gameobj);
                if (ParentObject == nullptr)
                    return;
                if ((static_cast<unsigned>(ParentObject->ObjectClass) & 0x303u) != 0x303u)
                    return;
                Pick = static_cast<CSimple*>(ParentObject);
            }
        }
    }

    // remember it, and mark its record hot when it is also scanned.
    RayObject = Pick;
    unsigned const PickObjectId = Pick->Id;   // +0xb0
    for (int Index = 0; Index < ScanEntries->Count; ++Index)
    {
        Watchable const* const Watched = ScanEntries->Entries[Index].target();
        if (Watched == nullptr)
            continue;
        IObjRW* const Object = ObjRwFromWatchable(const_cast<Watchable*>(Watched));
        if (Object == nullptr)
            continue;
        if (Object->get_id() == PickObjectId)
        {
            HotIndex = Index;
            Contacts.data()[Index].bHot = 1;
            return;
        }
    }
}

namespace
{
    // .data 0x00612434 -- the radar range assumed without a scanner (0x451c4000; only read).
    constexpr float kDefaultRadarRange = 2500.0f;

    // an object id the Render tree forces as the pick (written 0x004f1182;
    // cleared 0x004ee29c, 0x004f08dc, 0x004f1097). Read by the routine only.
    unsigned G_HudPickOverrideId = 0;
    // the waypoint record the Render tree found under the cursor (written
    // , 0x004f1091; cleared by Render 0x004ec4af and 0x004f1187). Read by
    // only.
    FHudContactRecord const* G_HudHoverRecord = nullptr;

    // The inlined bracket test of the routine (0x004edad2..0x004edb38 / 0x004edb53..
    // ) and the routine (0x004ee1b8..0x004ee20e): FILD of the cursor against the
    // edges, which stay exact on the x87 stack. Each edge fails when unordered (TEST AH,1 / JNZ
    // on the low side, TEST AH,0x41 / JP on the high side).
    bool CursorInBracket(Vector const& Screen, float Radius, int CursorX, int CursorY)
    {
        double const MouseX = static_cast<double>(CursorX);
        double const MouseY = static_cast<double>(CursorY);
        if (!(MouseX >= static_cast<double>(Screen.x) - static_cast<double>(Radius)))
            return false;
        if (!(MouseX <= static_cast<double>(Radius) + static_cast<double>(Screen.x)))
            return false;
        if (!(MouseY >= static_cast<double>(Screen.y) - static_cast<double>(Radius)))
            return false;
        if (!(MouseY <= static_cast<double>(Radius) + static_cast<double>(Screen.y)))
            return false;
        return true;
    }
}

FCameraRayHit* CameraRayPickTo(FCameraRayHit* Out, ICamera* Camera, Vector const* To,
                               CObject const* Ignore, bool bSkipFireBlocked)
{
    // . 0x0051ddf1..0x0051de0d: the result starts as "no hit".
    FCameraRayHit Result;
    Result.Object = nullptr;
    Result.Instance = -1;
    Result.Point.x = 0.0f;
    Result.Point.y = 0.0f;
    Result.Point.z = 0.0f;
    Result.Distance = 0.0f;

    // slot 3 GetPosition first, then the active system (the routine),
    // then at most eight hits into the stack array.
    Vector const* const From = reinterpret_cast<Vector const*>(Camera->GetPosition());
    unsigned const SystemId = StarSysGetActiveSystemId();
    PhySys::RayHit Hits[8];
    int const HitCount = PhySys::FindRayCollisions(SystemId, *From, *To, Hits, 8);

    // the first hit that is not Ignore (itself or through its root),
    // not a shield part, and -- with bSkipFireBlocked -- not on a ship whose CEqObj+0x140 is at
    // least 1.0 (the field; FCOMP / TEST AH,1 / JNZ keeps it only below 1.0 or unordered).
    for (int Index = 0; Index < HitCount; ++Index)
    {
        PhySys::RayHit const& Hit = Hits[Index];
        if (Hit.Object == Ignore)
            continue;
        CObject* const HitRoot = GetRoot(Hit.Object);
        if (HitRoot == Ignore)
            continue;
        // the inlined CObject::is_shield_part scan.
        if (Hit.Object->is_shield_part(Hit.PartId))
            continue;
        if (bSkipFireBlocked && HitRoot != nullptr &&
            (static_cast<unsigned>(HitRoot->ObjectClass) & 0x503u) == 0x503u &&
            static_cast<CEqObj const*>(HitRoot)->CloakPercent >= 1.0f)
        {
            continue;
        }

        // the part's engine instance -- the inlined
        // CObject::part_to_inst, where -1 (and an unmapped part) falls back to the object's own
        // instance (+4).
        long Instance = Hit.Object->part_to_inst(Hit.PartId);
        if (Instance == -1)
            Instance = Hit.Object->EngineInstanceId;

        // the root, the instance, the point and its distance from the
        // camera (slot 3 again), summed z, y, x.
        Result.Object = GetRoot(Hit.Object);
        Result.Instance = Instance;
        Vector const* const CameraPos = reinterpret_cast<Vector const*>(Camera->GetPosition());
        Result.Point = Hit.Point;
        double const Dx = static_cast<double>(Hit.Point.x) - static_cast<double>(CameraPos->x);
        double const Dy = static_cast<double>(Hit.Point.y) - static_cast<double>(CameraPos->y);
        double const Dz = static_cast<double>(Hit.Point.z) - static_cast<double>(CameraPos->z);
        Result.Distance = static_cast<float>(std::sqrt(Dz * Dz + Dy * Dy + Dx * Dx));
        break;
    }

    // REP MOVSD of the six dwords into Out.
    *Out = Result;
    return Out;
}

FCameraRayHit* CameraRayPick(FCameraRayHit* Out, ICamera* Camera, float ScreenX, float ScreenY,
                             CObject const* Ignore, bool bSkipFireBlocked)
{
    // . 0x0051dd23..0x0051dd3a: slot 14 Unproject into the stack vector.
    Vector Direction;
    Camera->Unproject(&Direction.x, ScreenX, ScreenY);

    // stretched to 1e7 (the field); the length sums y, z, x.
    double const Length = std::sqrt(static_cast<double>(Direction.y) * Direction.y +
                                    static_cast<double>(Direction.z) * Direction.z +
                                    static_cast<double>(Direction.x) * Direction.x);
    double const Scale = 1.0e7 / Length;
    Direction.x = static_cast<float>(static_cast<double>(Direction.x) * Scale);
    Direction.y = static_cast<float>(static_cast<double>(Direction.y) * Scale);
    Direction.z = static_cast<float>(static_cast<double>(Direction.z) * Scale);

    // the far end is the camera position (slot 3) plus that.
    float const* const From = Camera->GetPosition();
    Vector To;
    To.x = Direction.x + From[0];
    To.y = Direction.y + From[1];
    To.z = Direction.z + From[2];
    CameraRayPickTo(Out, Camera, &To, Ignore, bSkipFireBlocked);
    return Out;
}

bool HudTargetableObjects::PickUnderCursor(bool bQuiet)
{
    // . 0x004ed9f3..0x004eda18.
    PickSubObjId = SUBOBJ_ID_NONE;
    PickId = 0;

    // nothing without the player's ship.
    IObjRW* const Player = static_cast<IObjRW*>(GetPlayerObjRW());
    if (Player == nullptr)
        return false;
    CObject* const PlayerShip = CObjectOfObjRW(Player);
    if (PlayerShip == nullptr)
        return false;
    if ((static_cast<unsigned>(PlayerShip->ObjectClass) & 0x503u) != 0x503u)
        return false;

    // an id the Render tree forced.
    if (G_HudPickOverrideId != 0)
    {
        PickId = G_HudPickOverrideId;
        PickSubObjId = SUBOBJ_ID_NONE;
        return true;
    }

    // , the branch / the branch: the cursor on a waypoint marker --
    // the record itself, or a contact standing for it (+0x31 / +0x30).
    FHudContactRecord const* const Hover = G_HudHoverRecord;
    if (Hover == &WaypointRecord || (Hover != nullptr && Hover->bIsWaypoint != 0))
    {
        PickId = 0;
        PickSubObjId = SUBOBJ_ID_NONE;
        if (bQuiet)
            return false;
        HudWaypointSelectPlayerWaypoint(0);
        return true;
    }
    if (Hover == &BestPathRecord || (Hover != nullptr && Hover->bIsBestPath != 0))
    {
        PickId = 0;
        PickSubObjId = SUBOBJ_ID_NONE;
        if (bQuiet)
            return false;
        HudWaypointSelectBestPath(0);
        return true;
    }

    // rebuild the contacts, then the nav waypoint's bracket, else the
    // best-path waypoint's, under the cursor (the field / the field).
    RebuildContacts();
    int CursorX = 0;
    int CursorY = 0;
    int CursorZ = 0;
    GetClientSoftwareCursor(&CursorX, &CursorY, &CursorZ);
    FHudContactRecord const* Record = nullptr;
    if (WaypointRecord.bOnScreen != 0 &&
        CursorInBracket(WaypointRecord.Screen, WaypointRecord.ScreenRadius, CursorX, CursorY))
    {
        Record = &WaypointRecord;
    }
    else if (BestPathRecord.bOnScreen != 0 &&
             CursorInBracket(BestPathRecord.Screen, BestPathRecord.ScreenRadius, CursorX,
                             CursorY))
    {
        Record = &BestPathRecord;
    }

    // the waypoint wins unless a bracketed contact is there too whose
    // bracket is not larger (FCOMP / TEST AH,0x41 / JNZ: only an ordered HotScreenRadius >
    // Record->ScreenRadius keeps the waypoint). The result is "not quiet".
    if (Record != nullptr && (HotIndex < 0 || HotScreenRadius > Record->ScreenRadius))
    {
        if (Record == &WaypointRecord)
        {
            if (!bQuiet)
                HudWaypointSelectPlayerWaypoint(0);
        }
        else if (!bQuiet)
        {
            HudWaypointSelectBestPath(0);
        }
        PickId = 0;
        PickSubObjId = SUBOBJ_ID_NONE;
        return !bQuiet;
    }

    // the bracketed contact; a solar with a known parent picks the
    // parent's id.
    if (HotIndex >= 0)
    {
        IObjRW* const Hot =
            ObjRwFromWatchable(const_cast<Watchable*>(ScanEntries->Entries[HotIndex].target()));
        if (Hot != nullptr)
        {
            PickId = Hot->get_id();
            PickSubObjId = SUBOBJ_ID_NONE;
            CObject const* const HotObject = Hot->cobject();   // slot 0x150
            if (HotObject != nullptr &&
                (static_cast<unsigned>(HotObject->ObjectClass) & 0x303u) == 0x303u)
            {
                CSolar const* const Solar = static_cast<CSolar const*>(HotObject);
                if (!Solar->GetParentNickname().IsEmpty())
                {
                    FClientGameobj* const Parent =
                        ClientObjDb_FindObject(Solar->GetParentNickname().Id, 2);
                    if (Parent != nullptr)
                        PickId = static_cast<IObjRW*>(Parent)->get_id();
                }
            }
            return PickId != 0;
        }
    }

    // the camera ray from the cursor, past the player's ship, passing
    // over ships blocked by CEqObj+0x140 (the routine with 1), resolved by the routine.
    ICamera* const Camera = static_cast<ICamera*>(GetClientSoundListenerObject());
    FCameraRayHit Hit;
    CameraRayPick(&Hit, Camera, static_cast<float>(CursorX), static_cast<float>(CursorY),
                  PlayerShip, true);
    PickRayTarget(&Hit, &PickId, &PickSubObjId);

    // the squared distance a hostile contact has to beat, kept in the
    // argument's own stack slot ([ESP+0x3c]). With a ray pick it is FLT_MAX squared (the
    // at local_4 times itself), which overflows to +inf when stored as a float. With
    // the nearest pickable object it is that object's squared distance from the camera, summed
    // z, x, y. With neither, the slot still holds the argument's dword -- both callers push 1 or
    // 0 (0x004ed94d / 0x004ed989) -- read as a float: the smallest denormal or +0.
    float HostileLimit = 0.0f;
    if (PickId != 0)
    {
        HostileLimit = std::numeric_limits<float>::infinity();
    }
    else
    {
        unsigned const ArgumentDword = bQuiet ? 1u : 0u;
        std::memcpy(&HostileLimit, &ArgumentDword, sizeof(HostileLimit));
        FClientGameobj* const Near = FindPickableNearCursor();
        if (Near != nullptr)
        {
            IObjRW* const NearObject = Near;
            PickId = NearObject->get_id();
            Vector const* const CameraPos = reinterpret_cast<Vector const*>(Camera->GetPosition());
            Vector const& NearPos = NearObject->get_position();
            double const Dx = static_cast<double>(NearPos.x) - static_cast<double>(CameraPos->x);
            double const Dy = static_cast<double>(NearPos.y) - static_cast<double>(CameraPos->y);
            double const Dz = static_cast<double>(NearPos.z) - static_cast<double>(CameraPos->z);
            HostileLimit = static_cast<float>(Dz * Dz + Dx * Dx + Dy * Dy);
        }
    }

    // a hostile contact under the cursor takes over when it is nearer
    // the camera (summed z, y, x; TEST AH,5 / JP: only an ordered <).
    IObjRW* const Hostile = FindHostileContactNearCursor();
    if (Hostile != nullptr)
    {
        Vector const* const CameraPos = reinterpret_cast<Vector const*>(Camera->GetPosition());
        Vector const& HostilePos = Hostile->get_position();
        double const Dx = static_cast<double>(HostilePos.x) - static_cast<double>(CameraPos->x);
        double const Dy = static_cast<double>(HostilePos.y) - static_cast<double>(CameraPos->y);
        double const Dz = static_cast<double>(HostilePos.z) - static_cast<double>(CameraPos->z);
        if (Dz * Dz + Dy * Dy + Dx * Dx < static_cast<double>(HostileLimit))
        {
            PickId = Hostile->get_id();
            FDUMP(0x100002, "%s(%d) : *** WARNING: %s",
                  "Client\\Space\\HUD_Targetable_Objects.cpp", 0x9ae,
                  "Used Hostile Pick Assistance!");
        }
    }
    return PickId != 0;
}

void HudTargetableObjects::PickRayTarget(FCameraRayHit const* Hit, unsigned* OutId,
                                         unsigned short* OutSubObjId)
{
    // . 0x004edeb0..0x004eded0.
    *OutId = 0;
    *OutSubObjId = SUBOBJ_ID_NONE;

    // the player's ship.
    IObjRW* const Player = static_cast<IObjRW*>(GetPlayerObjRW());
    if (Player == nullptr)
        return;
    CObject* const PlayerShip = CObjectOfObjRW(Player);
    if (PlayerShip == nullptr)
        return;
    if ((static_cast<unsigned>(PlayerShip->ObjectClass) & 0x503u) != 0x503u)
        return;

    // a simple under the ray.
    CObject* const Object = Hit->Object;
    if (Object == nullptr)
        return;
    if ((static_cast<unsigned>(Object->ObjectClass) & 3u) != 3u)
        return;
    CSimple const* const Simple = static_cast<CSimple const*>(Object);

    // with a radar range under 2500 (TEST AH,5 / JP: ordered <), the
    // object's surface -- its centre's distance from the ship (the routine stores the offset as
    // three floats; summed y, z, x) less its radius -- must lie within it (FABS; only an
    // ordered > is out of range).
    float Range = kDefaultRadarRange;
    if (Scanner != nullptr)
        Range = Scanner->GetRadarRange();
    bool bOutOfRange = false;
    if (Range < kDefaultRadarRange)
    {
        Vector Center;
        float const Radius = Object->get_physical_radius_r(Center);   // slot 0x64
        Vector Offset;
        Offset.x = Center.x - PlayerShip->WorldPosition.x;
        Offset.y = Center.y - PlayerShip->WorldPosition.y;
        Offset.z = Center.z - PlayerShip->WorldPosition.z;
        double const Length = std::sqrt(static_cast<double>(Offset.y) * Offset.y +
                                        static_cast<double>(Offset.z) * Offset.z +
                                        static_cast<double>(Offset.x) * Offset.x);
        if (std::fabs(Length - static_cast<double>(Radius)) > static_cast<double>(Range))
            bOutOfRange = true;
    }

    // in range, the object's id -- at once for one with any of the type
    // bits 7, otherwise only within 45 of its radii (the field; summed z, y, x; ordered <).
    // Type 0x20004000 is never picked.
    if (!bOutOfRange)
    {
        unsigned const Type = Simple->TypeBits;   // +0xe0
        if ((Type & 0x20004000u) == 0)
        {
            if ((Type & 7u) != 0)
            {
                *OutId = Simple->Id;   // +0xb0
            }
            else
            {
                Vector Center;
                float const Radius = Object->get_physical_radius_r(Center);   // slot 0x64
                double const Dx = static_cast<double>(Center.x) -
                                  static_cast<double>(PlayerShip->WorldPosition.x);
                double const Dy = static_cast<double>(Center.y) -
                                  static_cast<double>(PlayerShip->WorldPosition.y);
                double const Dz = static_cast<double>(Center.z) -
                                  static_cast<double>(PlayerShip->WorldPosition.z);
                double const Ratio =
                    std::sqrt(Dz * Dz + Dy * Dy + Dx * Dx) / static_cast<double>(Radius);
                if (Ratio < 45.0)
                    *OutId = Simple->Id;
            }
        }
    }

    // the sub-object only for an equipped object (class 0x103) in range.
    CObject* const Equipped = Hit->Object;
    if (Equipped == nullptr)
        return;
    if ((static_cast<unsigned>(Equipped->ObjectClass) & 0x103u) != 0x103u)
        return;
    if (bOutOfRange)
        return;

    // a solar with a known parent picks the parent, whole.
    if ((static_cast<unsigned>(Equipped->ObjectClass) & 0x303u) == 0x303u)
    {
        CSolar const* const Solar = static_cast<CSolar const*>(Equipped);
        if (!Solar->GetParentNickname().IsEmpty())
        {
            FClientGameobj* const Parent =
                ClientObjDb_FindObject(Solar->GetParentNickname().Id, 2);
            if (Parent != nullptr)
            {
                *OutId = static_cast<IObjRW*>(Parent)->get_id();
                *OutSubObjId = SUBOBJ_ID_NONE;
            }
        }
    }

    // then the hit instance's sub-object (slot 0xf4), with the root
    // reported as none. This overwrites the parent's SUBOBJ_ID_NONE above too.
    *OutSubObjId = static_cast<CEqObj const*>(Equipped)->inst_to_subobj_id(Hit->Instance);
    if (*OutSubObjId == SUBOBJ_ID_ROOT)
        *OutSubObjId = SUBOBJ_ID_NONE;
}

FClientGameobj* HudTargetableObjects::FindPickableNearCursor()
{
    // . 0x004ee106..0x004ee12b: bucket 2 of the client ObjDB (the routine), the
    // best distance starting at 1e20 (0x60ad78ec).
    FClientGameobj* Best = nullptr;
    float BestDistance = 1.0e20f;
    int const Kind = 2;
    for (FClientGameobj* Gameobj = ClientObjDb_GetKindHead(&Kind); Gameobj != nullptr;
         Gameobj = Gameobj->ListNext)   // +0x14
    {
        // a simple whose +0xe0 low byte has bit 7 (TEST CL,CL / JNS).
        IObjRW* const Object = Gameobj;
        CObject const* const Target = Object->cobject();   // slot 0x150
        if (Target == nullptr)
            continue;
        if ((static_cast<unsigned>(Target->ObjectClass) & 3u) != 3u)
            continue;
        if ((static_cast<CSimple const*>(Target)->TypeBits & 0x80u) == 0)
            continue;

        // slot 5's centre, the CObject's slot 0x64 radius, projected.
        Vector const Center = Object->get_center_of_mass();
        Vector PartCenter;
        float const Radius = Object->cobject()->get_physical_radius_r(PartCenter);
        Vector Screen;
        float ScreenRadius = 0.0f;
        float Distance = 0.0f;
        if (!ProjectContact(&Center, Radius, &Screen, &ScreenRadius, &Distance))
            continue;

        // the doubled bracket (stored as a float) holds the cursor.
        ScreenRadius = ScreenRadius + ScreenRadius;
        int CursorX = 0;
        int CursorY = 0;
        int CursorZ = 0;
        GetClientSoftwareCursor(&CursorX, &CursorY, &CursorZ);   // / the field
        if (!CursorInBracket(Screen, ScreenRadius, CursorX, CursorY))
            continue;

        // the nearest (TEST AH,5 / JP: ordered <).
        if (Distance < BestDistance)
        {
            BestDistance = Distance;
            Best = Gameobj;
        }
    }

    // only under 11000 (the field; ordered <).
    if (BestDistance < 11000.0f)
        return Best;
    return nullptr;
}

IObjRW* HudTargetableObjects::FindHostileContactNearCursor()
{
    // . 0x004f1c27..0x004f1c3d.
    IObjRW* const Player = static_cast<IObjRW*>(GetPlayerObjRW());
    if (Player == nullptr)
        return nullptr;

    // FLT_MAX (0x7f7fffff); the routine's result is not used. The
    // cursor as a fraction of the surface from its centre: (mouse - size * 0.5) / size, the
    // half size (the field) and the fraction stored as floats.
    IObjRW* Best = nullptr;
    float BestDistance = FLT_MAX;
    GetClientSoundListenerObject();
    int CursorX = 0;
    int CursorY = 0;
    int CursorZ = 0;
    GetClientSoftwareCursor(&CursorX, &CursorY, &CursorZ);   // / the field
    double const Width = static_cast<double>(static_cast<int>(G_RenderSurfaceWidth));    // FILD the field
    double const Height = static_cast<double>(static_cast<int>(G_RenderSurfaceHeight));  // FILD the field
    float const HalfWidth = static_cast<float>(Width * 0.5);
    float const CursorFracX =
        static_cast<float>((static_cast<double>(CursorX) - static_cast<double>(HalfWidth)) / Width);
    float const HalfHeight = static_cast<float>(Height * 0.5);
    float const CursorFracY =
        static_cast<float>((static_cast<double>(CursorY) - static_cast<double>(HalfHeight)) / Height);

    // the scan list and the contact vector walked by one index.
    for (int Index = 0; Index < ScanEntries->Count; ++Index)
    {
        IObjRW* const Object =
            ObjRwFromWatchable(const_cast<Watchable*>(ScanEntries->Entries[Index].target()));
        if (Object == nullptr)
            continue;
        FHudContactRecord const& Record = Contacts.data()[Index];
        if (Record.bOnScreen == 0)
            continue;
        if ((Record.TypeFlags & 0x10000u) == 0)
            continue;
        // the routine(player, object) -- hostile only.
        if (GetAttitudeClass(Player, Object) != -1)
            continue;

        // the contact's circle, in the same units, holds the cursor:
        // dy^2 + dx^2 < r^2 + 0.003 (the field; TEST AH,0x41 / JNZ, ordered >).
        double const Dx = static_cast<double>(CursorFracX) -
                          (static_cast<double>(Record.Screen.x) - static_cast<double>(HalfWidth)) / Width;
        double const Dy = static_cast<double>(CursorFracY) -
                          (static_cast<double>(Record.Screen.y) - static_cast<double>(HalfHeight)) / Height;
        double const R = static_cast<double>(Record.ScreenRadius) / Width;
        if (!(R * R + static_cast<double>(0.003f) > Dy * Dy + Dx * Dx))
            continue;

        // the nearest (TEST AH,5 / JP: ordered <).
        if (Record.Distance < BestDistance)
        {
            Best = Object;
            BestDistance = Record.Distance;
        }
    }
    return Best;
}

namespace
{
    // (Id, 0) -- defined with the hazard warning below (NnVoiceEnqueue).
    void QueueHazardVoiceLine(unsigned VoiceId);

    // / the field -- the static initialisers the branch / the branch.
    unsigned const G_VoiceIncomingTorpedo = CreateID("incoming_torpedo");
    unsigned const G_VoiceIncomingMissile = CreateID("incoming_missile");

    // / the field -- Render's half-second toggle (0x004ec53b..0x004ec57f). The
    // byte is written here and read nowhere else in the PE.
    float G_HudTargetableBlinkTimer = 0.0f;           // (.bss)
    unsigned char G_bHudTargetableBlinkPhase = 1;

    // The x87 test every alert timer uses: FST, FCOM Limit, TEST AH,5 / JNP -- only an ordered
    // "below" stops; above, equal or unordered goes on.
    bool NotBelow(double Value, float Limit)
    {
        return !(Value < static_cast<double>(Limit));
    }

    // The `+0xc (CBase face) -> +4 (Object)` chain of the routine's object, kept only when the
    // class has all of Mask (the inlined player-ship lookups).
    CObject* PlayerObjectOfClass(IObjRW* Player, unsigned Mask)
    {
        if (Player == nullptr)
            return nullptr;
        CObject* const Object = CObjectOfObjRW(Player);
        if (Object == nullptr)
            return nullptr;
        if ((static_cast<unsigned>(Object->ObjectClass) & Mask) != Mask)
            return nullptr;
        return Object;
    }

    // Render's tooltip feed for one object (0x004ecb72..0x004ecc04 / 0x004ece70..0x004ecee8):
    // an unvisited solar shows its type caption (the routine) under UnvisitedIds; anything else
    // its caption (the routine, mode 2 without an owner player at +0xb4, else 1, with the
    // faction short name) under its id, with its ids_name (the routine).
    void FeedObjectToolTip(FToolTipHoverInfo* Info, CSimple const* Object, int UnvisitedIds)
    {
        if (HudIsUnvisitedSolar(Object))
        {
            Info->IdsAlt = static_cast<int>(HudObjectTypeIds(Object));
            if (Info->IdsAlt == 0)
                return;
            Info->Ids = UnvisitedIds;
            NnToolTipFeedHover(Info);
            return;
        }
        int const Mode = (Object->OwnerPlayer == 0) ? 2 : 1;
        if (!HudBuildObjectCaption(Object, reinterpret_cast<FHudWideText*>(&Info->CaptionW), Mode,
                                   true))
        {
            return;
        }
        Info->Ids = static_cast<int>(Object->Id);
        Info->IdsAlt = static_cast<int>(GetObjectIdsName(Object));
        NnToolTipFeedHover(Info);
    }
}

void HudTargetableObjects::Render(FSpaceCamera* /*Camera*/, float DeltaSeconds)
{
    // (HUDItem slot 6, RET 8). 0x004ec46f..0x004ec4a9 and the branch: with the
    // interface group (option 6) or the HUD (option 0xa) off, the cursor not drawn
    // (the routine) or no player, the four alerts are hidden and nothing else happens.
    if (GetGameOption(kGameOptInterfaceGroup) == 0 || GetGameOption(kGameOptHud) == 0 ||
        GetClientMouseCursorDrawFlag() == 0 || GetPlayerObjRW() == nullptr)
    {
        HideAlerts();
        return;
    }

    // the camera ray under the cursor, past the player's object (any
    // class with bit 0), with ships blocked by CEqObj+0x140 passed over, into +0x3f0.
    G_HudHoverRecord = nullptr;
    IObjRW* const Player = static_cast<IObjRW*>(GetPlayerObjRW());   // [ESP+0x18]
    CObject* const IgnoreObject = PlayerObjectOfClass(Player, 1u);
    int CursorX = 0;
    int CursorY = 0;
    int CursorZ = 0;
    GetClientSoftwareCursor(&CursorX, &CursorY, &CursorZ);   // / the field
    ICamera* const Listener = static_cast<ICamera*>(GetClientSoundListenerObject());
    CameraRayPick(&RayHit, Listener, static_cast<float>(CursorX), static_cast<float>(CursorY),
                  IgnoreObject, true);

    // the routine erases the whole lead-marker list.
    LeadMarkers.clear();

    // the half-second toggle (the field): only above 0.5 (TEST
    // AH,0x41 / JNZ) is it wound down, flipping the phase once per 0.5 taken off.
    double BlinkTimer =
        static_cast<double>(G_HudTargetableBlinkTimer) + static_cast<double>(DeltaSeconds);
    G_HudTargetableBlinkTimer = static_cast<float>(BlinkTimer);
    if (BlinkTimer > 0.5)
    {
        unsigned char Phase = G_bHudTargetableBlinkPhase;
        do
        {
            BlinkTimer -= 0.5;
            Phase = (Phase == 0) ? 1 : 0;
        } while (BlinkTimer > 0.5);
        G_HudTargetableBlinkTimer = static_cast<float>(BlinkTimer);
        G_bHudTargetableBlinkPhase = Phase;
    }

    // .
    RebuildContacts();
    DrawContactMarkers();
    DrawTargetReticle();
    CollectLeadMarkers();

    // the lead markers only for a player ship neither in a trade lane nor
    // in cruise.
    CShip* const PlayerShip = static_cast<CShip*>(PlayerObjectOfClass(Player, 0x503u));
    if (PlayerShip != nullptr && !PlayerShip->is_using_tradelane() &&
        !PlayerShip->is_cruise_active())
    {
        DrawLeadMarkers();
    }

    // .
    UpdateAimPoint();
    UpdateMissileAlert();
    UpdateTrailAlert();

    // the radiation alert blinks every 0.7 s (the field) while the
    // zone warning is on (the routine): shown when the warning is on and the phase is off.
    double const RadiationTimer =
        GetClientFrameTimeCapped() + static_cast<double>(RadiationBlinkTimer);
    RadiationBlinkTimer = static_cast<float>(RadiationTimer);
    if (NotBelow(RadiationTimer, 0.7f))
    {
        RadiationBlinkTimer = static_cast<float>(RadiationTimer - static_cast<double>(0.7f));
        bRadiationBlinkOn = (bRadiationBlinkOn == 0) ? 1 : 0;
        uintptr_t const bHide =
            (IsHudRadiationZoneWarningOn() && bRadiationBlinkOn == 0) ? 0u : 1u;
        RadiationAlert->SetProperty(1, bHide, 0);   // slot 42
    }

    UpdateLagAlert();

    // the radar range, when it is under 2500 (.data 0x00612434), limits
    // the ray object's cursor and tooltip below; otherwise the limit (1e30, 0x7149f2ca) is never
    // read.
    float Range = kDefaultRadarRange;
    if (Scanner != nullptr && Player != nullptr)
        Range = Scanner->GetRadarRange();
    bool bRangeLimited = false;   // [ESP+0x13]
    float RangeLimit = 1.0e30f;   // [ESP+0x1c]
    if (Range < kDefaultRadarRange)
    {
        RangeLimit = Range;
        bRangeLimited = true;
    }

    // the cursor. Over a Win3D control it is the arrow; over the ray
    // object or the bracketed contact it shows the attitude towards it; otherwise the cross --
    // and with the fire attitude latched (+0x928) the fire cursor of +0x92c instead.
    Win3D* const Hover = Win3DGetHover();   // , [ESP+0x14]
    int Attitude = 0;                       // EDI
    bool bUseAttitude = false;
    if (Player == nullptr || Hover != nullptr)
    {
        if (Hover != nullptr)
            MouseSetCursor("Arrow", false);   // , the routine
    }
    else if (HotIndex < 0)
    {
        // the ray object.
        CSimple* const Ray = RayObject;
        if (Ray != nullptr)
        {
            bool bNoAttitude = true;   // BL
            FClientGameobj* const RayGameobj = ClientObjDb_FindObject(Ray->Id, 0);
            if (RayGameobj != nullptr)
            {
                // (player, object): the object's attitude towards the player, the
                // player passed only when its gameobj sits in ObjDB bucket 1 (+0x1c; SETNZ / DEC /
                // AND at 0x004ec70a..0x004ec717).
                IObjRW* const PlayerOrNull =
                    (static_cast<FClientGameobj*>(Player)->ManagerKind == 1) ? Player : nullptr;
                Attitude = GetAttitudeClass(PlayerOrNull, RayGameobj);
                bNoAttitude = false;
            }
            if (bRangeLimited)
            {
                // the surface out of radar range (summed x, y, z).
                Vector Center;
                float const Radius = Ray->get_physical_radius_r(Center);   // slot 0x64
                Vector const& PlayerPos = Player->get_position();
                Vector Offset;
                Offset.x = Center.x - PlayerPos.x;
                Offset.y = Center.y - PlayerPos.y;
                Offset.z = Center.z - PlayerPos.z;
                double const Length = std::sqrt(static_cast<double>(Offset.x) * Offset.x +
                                                static_cast<double>(Offset.y) * Offset.y +
                                                static_cast<double>(Offset.z) * Offset.z);
                if (std::fabs(Length - static_cast<double>(Radius)) > static_cast<double>(RangeLimit))
                    bNoAttitude = true;
            }
            // an equipped object blocked at +0x140 (>= 1.0, ordered).
            bool const bBlocked =
                (static_cast<unsigned>(Ray->ObjectClass) & 0x103u) == 0x103u &&
                static_cast<CEqObj const*>(static_cast<CObject const*>(Ray))->CloakPercent >= 1.0f;
            if (!bBlocked && !bNoAttitude)
                bUseAttitude = true;
        }
    }
    else
    {
        // the bracketed contact (a solar's known parent stands in).
        IObjRW* Contact =
            ObjRwFromWatchable(const_cast<Watchable*>(ScanEntries->Entries[HotIndex].target()));
        if (Contact != nullptr)
        {
            CObject const* const ContactObject = Contact->cobject();   // slot 0x150
            if (ContactObject != nullptr &&
                (static_cast<unsigned>(ContactObject->ObjectClass) & 0x303u) == 0x303u)
            {
                CSolar const* const Solar = static_cast<CSolar const*>(ContactObject);
                if (!Solar->GetParentNickname().IsEmpty())
                {
                    FClientGameobj* const Parent =
                        ClientObjDb_FindObject(Solar->GetParentNickname().Id, 2);
                    if (Parent != nullptr)
                        Contact = Parent;
                }
            }
            // as above, the player only from ObjDB bucket 1.
            IObjRW* const PlayerOrNull =
                (static_cast<FClientGameobj*>(Player)->ManagerKind == 1) ? Player : nullptr;
            Attitude = GetAttitudeClass(PlayerOrNull, Contact);
            CObject const* const Target = Contact->cobject();
            bool const bBlocked =
                Target != nullptr && (static_cast<unsigned>(Target->ObjectClass) & 0x103u) == 0x103u &&
                static_cast<CEqObj const*>(Target)->CloakPercent >= 1.0f;
            if (!bBlocked)
                bUseAttitude = true;
        }
    }
    if (Player != nullptr && Hover == nullptr)
    {
        bool bFireCursor = false;
        if (bUseAttitude)
        {
            // .
            if (Attitude == -1)
                MouseSetCursor("hostile", false);
            else if (Attitude == 0)
                MouseSetCursor("neutral", false);
            else if (Attitude == 1)
                MouseSetCursor("friendly", false);
            bFireCursor = bFireAttitude != 0;
        }
        else if (bFireAttitude != 0)
        {
            bFireCursor = true;
        }
        else
        {
            MouseSetCursor("Cross", false);
        }
        if (bFireCursor)
        {
            // (the field is "fire").
            if (FireAttitude == -1)
                MouseSetCursor("fire", false);
            else if (FireAttitude == 0)
                MouseSetCursor("fire_neutral", false);
            else if (FireAttitude == 1)
                MouseSetCursor("fire_friendly", false);
        }
    }
    else if (Player == nullptr && Hover == nullptr)
    {
        // -> 0x004ec8f1: no player and no control under the cursor.
        if (bFireAttitude != 0)
        {
            if (FireAttitude == -1)
                MouseSetCursor("fire", false);
            else if (FireAttitude == 0)
                MouseSetCursor("fire_neutral", false);
            else if (FireAttitude == 1)
                MouseSetCursor("fire_friendly", false);
        }
        else
        {
            MouseSetCursor("Cross", false);
        }
    }

    // the object tooltip -- not over a fed tooltip (the routine) or a
    // control, not while picking is blocked (the routine), only with mouse-flight unlocked
    // (the routine) and before its hold engages (the routine).
    if (NnToolTipWasFedThisFrame())
        return;
    if (Hover != nullptr)
        return;
    if (HudIsMousePickBlocked())
        return;
    if (!IsMouseFlightUnlocked())
        return;
    if (IsMouseFlightHoldThresholdReached())
        return;

    // the stack hover record, Ids -1.
    FToolTipHoverInfo Info;
    ToolTipHoverInit(&Info);
    if (HotIndex < 0)
    {
        // the ray object, within radar range, not blocked; unvisited
        // solars under ~Id.
        CSimple* const Ray = RayObject;
        if (Ray != nullptr)
        {
            bool bShow = true;
            if (bRangeLimited && Player != nullptr)
            {
                Vector Center;
                float const Radius = Ray->get_physical_radius_r(Center);   // slot 0x64
                Vector const& PlayerPos = Player->get_position();
                Vector Offset;
                Offset.x = Center.x - PlayerPos.x;
                Offset.y = Center.y - PlayerPos.y;
                Offset.z = Center.z - PlayerPos.z;
                double const Length = std::sqrt(static_cast<double>(Offset.x) * Offset.x +
                                                static_cast<double>(Offset.y) * Offset.y +
                                                static_cast<double>(Offset.z) * Offset.z);
                if (std::fabs(Length - static_cast<double>(Radius)) > static_cast<double>(RangeLimit))
                    bShow = false;
            }
            if (bShow)
            {
                bool const bBlocked =
                    (static_cast<unsigned>(Ray->ObjectClass) & 0x103u) == 0x103u &&
                    static_cast<CEqObj const*>(static_cast<CObject const*>(Ray))->CloakPercent >= 1.0f;
                if (!bBlocked)
                    FeedObjectToolTip(&Info, Ray, static_cast<int>(~Ray->Id));
            }
        }
    }
    else
    {
        // the bracketed contact (a simple); a solar's known parent
        // stands in, and must itself be a solar; not blocked; unvisited solars under their id.
        IObjRW* const Contact =
            ObjRwFromWatchable(const_cast<Watchable*>(ScanEntries->Entries[HotIndex].target()));
        CObject const* Target = (Contact != nullptr) ? Contact->cobject() : nullptr;
        bool bShow = Target != nullptr && (static_cast<unsigned>(Target->ObjectClass) & 3u) == 3u;
        if (bShow && (static_cast<unsigned>(Target->ObjectClass) & 0x303u) == 0x303u)
        {
            CSolar const* const Solar = static_cast<CSolar const*>(Target);
            if (!Solar->GetParentNickname().IsEmpty())
            {
                FClientGameobj* const Parent =
                    ClientObjDb_FindObject(Solar->GetParentNickname().Id, 2);
                if (Parent != nullptr)
                {
                    CObject* const ParentObject = CObjectOfGameobj(Parent);
                    if (ParentObject == nullptr ||
                        (static_cast<unsigned>(ParentObject->ObjectClass) & 0x303u) != 0x303u)
                    {
                        bShow = false;
                    }
                    else
                    {
                        Target = ParentObject;
                    }
                }
            }
        }
        if (bShow)
        {
            bool const bBlocked =
                (static_cast<unsigned>(Target->ObjectClass) & 0x103u) == 0x103u &&
                static_cast<CEqObj const*>(Target)->CloakPercent >= 1.0f;
            if (!bBlocked)
            {
                CSimple const* const Simple = static_cast<CSimple const*>(Target);
                FeedObjectToolTip(&Info, Simple, static_cast<int>(Simple->Id));
            }
        }
    }
    // The record's teardown: ~FToolTipHoverInfo (the routine).
}

namespace
{
    // 's corner cut: Corner + (Toward - Corner) * Scale. The difference is rounded to
    // floats (the routine / the the routine arguments); x and y are scaled and added on the x87
    // stack with one rounding at the store, z is scaled into a float first (FSTP) and then added.
    Vector BracketCutPoint(Vector const& Corner, Vector const& Toward, float Scale)
    {
        float const DeltaX = Toward.x - Corner.x;
        float const DeltaY = Toward.y - Corner.y;
        float const DeltaZ = Toward.z - Corner.z;
        Vector Out;
        Out.x = static_cast<float>(static_cast<double>(Corner.x) +
                                   static_cast<double>(DeltaX) * static_cast<double>(Scale));
        Out.y = static_cast<float>(static_cast<double>(Corner.y) +
                                   static_cast<double>(DeltaY) * static_cast<double>(Scale));
        float const ScaledZ = static_cast<float>(static_cast<double>(DeltaZ) * static_cast<double>(Scale));
        Out.z = Corner.z + ScaledZ;
        return Out;
    }

    // The one cut (0x004eafc0..0x004eb004) that stores all three scaled components as floats and
    // adds the corner afterwards (the routine, Vector::operator+=).
    Vector BracketCutPointRounded(Vector const& Corner, Vector const& Toward, float Scale)
    {
        float const DeltaX = Toward.x - Corner.x;
        float const DeltaY = Toward.y - Corner.y;
        float const DeltaZ = Toward.z - Corner.z;
        Vector Out;
        Out.x = static_cast<float>(static_cast<double>(DeltaX) * static_cast<double>(Scale));
        Out.y = static_cast<float>(static_cast<double>(DeltaY) * static_cast<double>(Scale));
        Out.z = static_cast<float>(static_cast<double>(DeltaZ) * static_cast<double>(Scale));
        Out.x = Corner.x + Out.x;
        Out.y = Corner.y + Out.y;
        Out.z = Corner.z + Out.z;
        return Out;
    }

    Vector MakeVector(float X, float Y, float Z)
    {
        Vector Out;
        Out.x = X;
        Out.y = Y;
        Out.z = Z;
        return Out;
    }

    // Every the routine call in the routine passes width 2.0f (0x40000000).
    void DrawBracketLine(Vector const& A, Vector const& B, unsigned long Color,
                         unsigned long EdgeColor)
    {
        Win3DImmedDrawLine(&A.x, &B.x, Color, EdgeColor, 2.0f);
    }
}

void HudTargetableObjects::DrawContactBracket(FHudContactRecord const* Record,
                                              unsigned long const* Color, bool bHostile,
                                              bool bWatchListed)
{
    // . 0x004eadae..0x004eadc7: nothing for a radius that equals 0.0 (FCOMP double
    // / TEST AH,0x44 / JNP -- an unordered radius goes on).
    float const Radius = Record->ScreenRadius;   // local_80
    if (static_cast<double>(Radius) == 0.0)
        return;

    // the four corners, Screen + (+-R, +-R, 0) through the routine,
    // named by the signs of their x and y offsets.
    float const NegRadius = -Radius;   // local_6c
    Vector const& Screen = Record->Screen;   // local_2c
    Vector const CornerNP = MakeVector(Screen.x + NegRadius, Screen.y + Radius, Screen.z + 0.0f);    // local_44
    Vector const CornerPP = MakeVector(Screen.x + Radius, Screen.y + Radius, Screen.z + 0.0f);       // local_50
    Vector const CornerPN = MakeVector(Screen.x + Radius, Screen.y + NegRadius, Screen.z + 0.0f);    // local_5c
    Vector const CornerNN = MakeVector(Screen.x + NegRadius, Screen.y + NegRadius, Screen.z + 0.0f); // local_68

    // the arm fraction, 0.2 at a 10-pixel radius falling by 0.15 over
    // the next 90 (the field 10, the field 0.15, the field 100, the field 0.2; FDIVP
    // DE F9 divides the scaled term by 90).
    double const Falloff = (static_cast<double>(Radius) - static_cast<double>(10.0f)) *
                           static_cast<double>(0.15f) /
                           (static_cast<double>(100.0f) - static_cast<double>(10.0f));
    float Scale = static_cast<float>(static_cast<double>(0.2f) - Falloff);   // local_a8

    // the RGBA bytes as A:R:G:B (ESI) and the same with alpha 0 (EDI).
    unsigned char const* const Rgba = reinterpret_cast<unsigned char const*>(Color);
    unsigned long EdgeColor = (static_cast<unsigned long>(Rgba[0]) << 16) |
                              (static_cast<unsigned long>(Rgba[1]) << 8) |
                              static_cast<unsigned long>(Rgba[2]);
    unsigned long LineColor = (static_cast<unsigned long>(Rgba[3]) << 24) | EdgeColor;

    if (bHostile)
    {
        // the four corners cut off diagonally.
        Vector const CutA = BracketCutPointRounded(CornerNN, CornerPN, Scale);   // local_98
        Vector const CutB = BracketCutPoint(CornerNN, CornerNP, Scale);          // local_a4
        DrawBracketLine(CutA, CutB, LineColor, EdgeColor);
        Vector const LeftLow = CutB;                                             // local_c

        Vector const CutC = BracketCutPoint(CornerPP, CornerNP, Scale);
        Vector const CutD = BracketCutPoint(CornerPP, CornerPN, Scale);
        DrawBracketLine(CutC, CutD, LineColor, EdgeColor);
        Vector const RightHigh = MakeVector(CutD.x, CutD.y + 1.0f, CutD.z);      // local_8c (the field)

        Vector const CutE = BracketCutPoint(CornerPN, CornerNN, Scale);
        Vector const CutF = BracketCutPoint(CornerPN, CornerPP, Scale);
        DrawBracketLine(CutE, CutF, LineColor, EdgeColor);
        Vector const RightLow = CutF;                                            // local_38

        Vector const CutG = BracketCutPoint(CornerNP, CornerNN, Scale);
        Vector const CutH = BracketCutPoint(CornerNP, CornerPP, Scale);
        Vector const LeftHigh = MakeVector(CutG.x, CutG.y + 1.0f, CutG.z);       // local_1c
        DrawBracketLine(CutG, CutH, LineColor, EdgeColor);

        // a watch-listed hostile also gets the two side bars between
        // the cuts.
        if (bWatchListed)
        {
            DrawBracketLine(LeftLow, LeftHigh, LineColor, EdgeColor);
            DrawBracketLine(RightLow, RightHigh, LineColor, EdgeColor);
        }
    }
    else
    {
        // an angle at each corner -- the arms run from the cut into the
        // corner or from the corner out to the cut, in baseline's order.
        DrawBracketLine(BracketCutPoint(CornerNN, CornerPN, Scale), CornerNN, LineColor, EdgeColor);
        DrawBracketLine(CornerNN, BracketCutPoint(CornerNN, CornerNP, Scale), LineColor, EdgeColor);
        DrawBracketLine(BracketCutPoint(CornerPP, CornerNP, Scale), CornerPP, LineColor, EdgeColor);
        DrawBracketLine(CornerPP, BracketCutPoint(CornerPP, CornerPN, Scale), LineColor, EdgeColor);
        DrawBracketLine(BracketCutPoint(CornerPN, CornerNN, Scale), CornerPN, LineColor, EdgeColor);
        DrawBracketLine(CornerPN, BracketCutPoint(CornerPN, CornerPP, Scale), LineColor, EdgeColor);
        DrawBracketLine(BracketCutPoint(CornerNP, CornerNN, Scale), CornerNP, LineColor, EdgeColor);
        DrawBracketLine(CornerNP, BracketCutPoint(CornerNP, CornerPP, Scale), LineColor, EdgeColor);
    }

    // the side marks.
    if (Record->bPlayerShip != 0)
    {
        // watch-listed, they are blue (A:00:00:FF, edge 000000FF).
        if (bWatchListed)
        {
            LineColor = (static_cast<unsigned long>(Rgba[3]) << 24) | 0xffu;
            EdgeColor = 0xffu;
        }

        // Arm = Scale * R (kept as a float); the mark is 2 * Arm wide and
        // its inner ends close in by a quarter of that (the field 0.25).
        double const ArmExact = static_cast<double>(Scale) * static_cast<double>(Radius);
        Scale = static_cast<float>(ArmExact);                                        // local_a8
        float Width = static_cast<float>(ArmExact + ArmExact);                       // local_7c
        float const Inset = static_cast<float>((ArmExact + ArmExact) * 0.25);        // local_20
        float const NegArm = -Scale;                                                 // local_6c / local_94

        // the left mark, bar then its two slants.
        Vector const LeftBottom = MakeVector(Screen.x + NegRadius, NegArm + Screen.y, Screen.z);   // local_98
        Vector const LeftTop = MakeVector(Screen.x + NegRadius, Screen.y + Scale, Screen.z);       // local_a4
        DrawBracketLine(LeftBottom, LeftTop, LineColor, EdgeColor);
        DrawBracketLine(LeftBottom, MakeVector(LeftBottom.x + Width, LeftBottom.y + Inset, Screen.z),
                        LineColor, EdgeColor);
        float const NegInset = -Inset;                                               // local_10
        DrawBracketLine(LeftTop, MakeVector(LeftTop.x + Width, LeftTop.y + NegInset, Screen.z),
                        LineColor, EdgeColor);

        // the right mark, mirrored.
        Vector const RightTop = MakeVector(Screen.x + Radius, Screen.y + Scale, Screen.z);         // local_a4
        Vector const RightBottom = MakeVector(Screen.x + Radius, NegArm + Screen.y, Screen.z);     // local_98
        DrawBracketLine(RightBottom, RightTop, LineColor, EdgeColor);
        Width = -Width;                                                              // local_7c
        DrawBracketLine(RightBottom, MakeVector(Width + RightBottom.x, RightBottom.y + Inset, Screen.z),
                        LineColor, EdgeColor);
        DrawBracketLine(RightTop, MakeVector(Width + RightTop.x, RightTop.y + NegInset, Screen.z),
                        LineColor, EdgeColor);
    }
    else if (bWatchListed)
    {
        // a watch-listed object's marks close to a point: the slants end
        // Arm in from each bar end, 2 * Arm across (the colours are rebuilt from Color, the same
        // values as above).
        double const ArmExact = static_cast<double>(Scale) * static_cast<double>(Radius);
        Scale = static_cast<float>(ArmExact);                                        // local_a8
        float const Width = static_cast<float>(ArmExact + ArmExact);                 // local_20
        float const NegArm = -Scale;                                                 // local_7c / local_94

        Vector const LeftTop = MakeVector(NegRadius + Screen.x, Screen.y + Scale, Screen.z);       // local_a4
        Vector const LeftBottom = MakeVector(NegRadius + Screen.x, NegArm + Screen.y, Screen.z);   // local_98
        DrawBracketLine(LeftBottom, LeftTop, LineColor, EdgeColor);
        DrawBracketLine(LeftBottom, MakeVector(LeftBottom.x + Width, LeftBottom.y + Scale, Screen.z),
                        LineColor, EdgeColor);
        DrawBracketLine(LeftTop, MakeVector(LeftTop.x + Width, LeftTop.y + NegArm, Screen.z),
                        LineColor, EdgeColor);

        Vector const RightTop = MakeVector(Screen.x + Radius, Screen.y + Scale, Screen.z);         // local_a4
        Vector const RightBottom = MakeVector(Screen.x + Radius, NegArm + Screen.y, Screen.z);     // local_98
        DrawBracketLine(RightBottom, RightTop, LineColor, EdgeColor);
        float const NegWidth = -Width;                                               // local_10
        DrawBracketLine(RightBottom, MakeVector(NegWidth + RightBottom.x, RightBottom.y + Scale, Screen.z),
                        LineColor, EdgeColor);
        DrawBracketLine(RightTop, MakeVector(NegWidth + RightTop.x, RightTop.y + NegArm, Screen.z),
                        LineColor, EdgeColor);
    }
}

void HudTargetableObjects::DrawContactMarkers()
{
    // . 0x004ebf90..0x004ebfd8: a scanner, a player whose object is a ship
    // (0x503); nothing otherwise.
    IObjRW* const Player = static_cast<IObjRW*>(GetPlayerObjRW());   // , [ESP+0x20]
    if (Scanner == nullptr || Player == nullptr)
        return;
    CShip* const PlayerShip = static_cast<CShip*>(PlayerObjectOfClass(Player, 0x503u));   // [ESP+0x38]
    if (PlayerShip == nullptr)
        return;

    // the id of the ship's target (CShip::get_target, slot 8), 0 without.
    unsigned TargetId = 0;   // [ESP+0x24]
    IObjRW* const ShipTarget = PlayerShip->get_target();
    if (ShipTarget != nullptr)
        TargetId = ShipTarget->get_id();

    // flat, unlit, blended 2D state.
    Win3DDisable(kWin3DLightingCap);   // (0xb50)
    Win3DDisable(kWin3DTexture2D);     // 0xde1
    Win3DDisable(kWin3DCullFace);      // 0xb44
    Win3DDisable(kWin3DDepthTest);     // 0xb71
    Win3DDisable(kWin3DFog);           // 0xb60
    EngineLightClearRegisteredList();
    NeuronRuntime::Pipeline->SetRenderState(D3DRS_AMBIENT, -1);   // slot 60: D3DRS_AMBIENT white
    NeuronRuntime::Pipeline->SetRenderState(D3DRS_CULLMODE, 1);    // D3DRS_CULLMODE none
    Win3DEnable(kWin3DBlend);                    // (0xbe2)
    Win3DSetBlendFunc(kWin3DBlendSrcAlpha, kWin3DBlendInvSrcAlpha);   // (5, 6)

    // every scanner entry with a record (the count re-read each pass).
    for (int Index = 0; Index < ScanEntries->Count; ++Index)
    {
        // a watched object, a record with type bits, not the ship's
        // target and not the best-path waypoint's object.
        Watchable const* const Watched = ScanEntries->Entries[Index].target();
        if (Watched == nullptr)
            continue;
        IObjRW* const Contact = ObjRwFromWatchable(const_cast<Watchable*>(Watched));   // EBP
        if (Contact == nullptr)
            continue;
        FHudContactRecord const& Record = Contacts.data()[Index];   // ESI = +0x374 + i * 0x34
        if (Record.TypeFlags == 0)
            continue;
        if (Contact->get_id() == TargetId)
            continue;
        if (Record.bIsBestPath != 0)
            continue;

        // the idle cyan (the field); with the player in ObjDB bucket 1
        // its reputation colour (the routine) and whether the contact is hostile (the routine
        // == -1).
        unsigned long Color = G_NnPaletteArgb_679bb0;   // [ESP+0x10]
        bool bHostile = false;                           // [ESP+0x1c]
        if (static_cast<FClientGameobj*>(Player)->ManagerKind == 1)
        {
            unsigned long Reputation = 0;
            HudReputationColor(&Reputation, Player, Contact);
            Color = Reputation;
            if (GetAttitudeClass(Player, Contact) == -1)
                bHostile = true;
        }

        // type 0x10000000 is white.
        unsigned const TypeFlags = Record.TypeFlags;   // [ESP+0x34]
        unsigned const WhiteTypeBit = TypeFlags & 0x10000000u;   // EBX from here on
        if (WhiteTypeBit != 0)
            Color = 0xffffffffu;

        unsigned char* const ColorBytes = reinterpret_cast<unsigned char*>(&Color);
        if (Record.bHot != 0)
        {
            // the contact under the mouse pulses, unless picking is
            // blocked (the routine).
            if (!HudIsMousePickBlocked())
            {
                unsigned long Pulsed = 0;
                NnUiPulseBlendFromArgb(&Pulsed, Color);
                Color = Pulsed;
            }
        }
        else
        {
            // one of HUD_Comm's watched objects is amber (the field).
            if (HudCommIsWatchedObjectId(Contact->get_id()))
                Color = G_NnPaletteArgb_679ba4;

            // the alpha fades with distance: 1.25 - D * 0.0004
            // (the field, the field), halved for type 0x10000000 (the field), clamped to
            // [0, 1] -- below 0 ordered (TEST AH,5 / JP) is 0, above 1 ordered (TEST AH,0x41 /
            // JNZ) is 1, unordered stays -- and the alpha byte scaled by it (ftol truncates).
            double Fade = static_cast<double>(1.25f) -
                          static_cast<double>(Record.Distance) * static_cast<double>(0.0004f);
            if (WhiteTypeBit != 0)
                Fade *= static_cast<double>(0.5f);
            if (Fade < 0.0)
                Fade = 0.0;
            else if (Fade > 1.0)
                Fade = 1.0;
            ColorBytes[3] = static_cast<unsigned char>(
                static_cast<int>(static_cast<double>(static_cast<int>(ColorBytes[3])) * Fade));
        }

        // the bracket for types 0x107f0000, the best-path object and
        // watch-listed objects.
        if ((TypeFlags & 0x107f0000u) != 0 || Record.bIsBestPath != 0 || Record.bWatchListed != 0)
            DrawContactBracket(&Record, &Color, bHostile, Record.bWatchListed != 0);

        // an on-screen contact the ship may lead: of types 0x7f0000 only
        // when hostile; of type 0x1000000 only when its own target is the player; of type
        // always; and only when CShip::get_tgt_lead_fire_pos finds a position.
        if (Record.bOnScreen == 0)
            continue;
        if ((TypeFlags & 0x7f0000u) != 0)
        {
            if (!bHostile)
                continue;
        }
        else if ((TypeFlags & 0x1000000u) != 0)
        {
            IObjRW* ContactTarget = nullptr;
            Contact->get_target(ContactTarget);   // slot 29
            if (ContactTarget == nullptr)
                continue;
            if (ContactTarget->get_id() != Player->get_id())
                continue;
        }
        else if ((TypeFlags & 0x4000000u) == 0)
        {
            continue;
        }
        Vector Lead;   // [ESP+0x44]
        if (!PlayerShip->get_tgt_lead_fire_pos(Contact, Lead))
            continue;

        // the marker -- a watcher on the contact (baseline fills a stack
        // watcher with set_pointer, copies it into the node with BaseWatcher::set and releases
        // the stack one), the lead position and its projection through the listener camera
        // (slot 17), appended to +0x910. baseline projects into an uninitialised stack vector and
        // does not test the result (a failed Project, 0x0040ffd7, writes nothing); the port
        // starts it at zero.
        Vector LeadScreen;   // [ESP+0x64]
        LeadScreen.x = 0.0f;
        LeadScreen.y = 0.0f;
        LeadScreen.z = 0.0f;
        ICamera* const Camera = static_cast<ICamera*>(GetClientSoundListenerObject());
        Camera->ProjectVec(&LeadScreen.x, &Lead.x);
        LeadMarkers.emplace_back();
        FHudLeadMarker& Marker = LeadMarkers.back();
        Marker.Target.set(Contact);
        Marker.LeadWorld = Lead;
        Marker.LeadScreen = LeadScreen;
    }
}

namespace
{
    // 's label point (0x004ee40f..0x004ee4a7 and its two copies): a screen point
    // onto the Win3D camera's near plane (the field: +0x68 / +0x6c centre, +0x78 / +0x7c
    // scales, +0xa4 near; each coordinate stored as a float), then scaled by -1.0 / z
    // (the field) to depth 1. ScreenY arrives unrounded from the x87 stack.
    Vector ReticleLabelPoint(double ScreenX, double ScreenY)
    {
        FSpaceCamera const* const Ui = Win3DGetCamera();
        float const X = static_cast<float>(
            (ScreenX - static_cast<double>(Ui->ScreenCenterX)) *
            (static_cast<double>(Ui->ZNear) / static_cast<double>(Ui->XScale)));
        float const Y = static_cast<float>(
            (ScreenY - static_cast<double>(Ui->ScreenCenterY)) *
            (static_cast<double>(Ui->ZNear) / static_cast<double>(Ui->YScale)));
        float const Z = -Ui->ZNear;
        double const Scale = -1.0 / static_cast<double>(Z);
        Vector Out;
        Out.x = static_cast<float>(static_cast<double>(X) * Scale);
        Out.y = static_cast<float>(static_cast<double>(Y) * Scale);
        Out.z = static_cast<float>(Scale * static_cast<double>(Z));
        return Out;
    }

    // The label draw the routine inlines three times: the range text control's Font::String
    // (+0x334, the routine) swapped for Label (the routine), SetProperty(5, point) (slot 42)
    // and Draw (slot 13), then the old string put back.
    void DrawReticleLabel(WinControl* TextControl, FFontString* Label, Vector const& Point)
    {
        NnTextControl* const Text = static_cast<NnTextControl*>(TextControl);
        FFontString* const Saved = Text->FontString;
        Text->FontString = Label;
        Text->SetProperty(5, reinterpret_cast<uintptr_t>(&Point), 0);
        Text->Draw();
        Text->FontString = Saved;
    }

    // The FCOMP / TEST AH,0x44 / JP triple: all three components equal (ordered).
    bool SameWorldPosition(Vector const& A, Vector const& B)
    {
        return A.x == B.x && A.y == B.y && A.z == B.z;
    }
}

void HudTargetableObjects::DrawWaypointMarker(FHudContactRecord const* Record,
                                              unsigned long const* Color, int /*Unused*/)
{
    // . 0x004ebde3..0x004ebedd: the midpoints of the four edges (float adds of the
    // screen position and +-R).
    float const Radius = Record->ScreenRadius;
    Vector const& Screen = Record->Screen;
    float const NegRadius = -Radius;
    Vector const Top = MakeVector(Screen.x, Screen.y + Radius, Screen.z);        // local_30
    Vector const Right = MakeVector(Screen.x + Radius, Screen.y, Screen.z);      // local_24
    Vector const Bottom = MakeVector(Screen.x, NegRadius + Screen.y, Screen.z);  // local_18
    Vector const Left = MakeVector(NegRadius + Screen.x, Screen.y, Screen.z);    // local_c

    // RGBA -> A:R:G:B and R:G:B, four 2-wide lines.
    unsigned char const* const Rgba = reinterpret_cast<unsigned char const*>(Color);
    unsigned long const EdgeColor = (static_cast<unsigned long>(Rgba[0]) << 16) |
                                    (static_cast<unsigned long>(Rgba[1]) << 8) |
                                    static_cast<unsigned long>(Rgba[2]);
    unsigned long const LineColor = (static_cast<unsigned long>(Rgba[3]) << 24) | EdgeColor;
    DrawBracketLine(Top, Right, LineColor, EdgeColor);
    DrawBracketLine(Right, Bottom, LineColor, EdgeColor);
    DrawBracketLine(Bottom, Left, LineColor, EdgeColor);
    DrawBracketLine(Left, Top, LineColor, EdgeColor);
}

void HudTargetableObjects::DrawTargetReticle()
{
    // . 0x004ee27f..0x004ee2b6: the range text shown (property 1 = 0) for the draws
    // below, the forced pick cleared, the Win3D camera applied (the routine(&the field)).
    ReticleRangeText->SetProperty(1, 0, 0);   // slot 42
    G_HudPickOverrideId = 0;
    Win3DApplyCamera(AsCamera(Win3DGetCamera()));
    bool bTargetIsBestPath = false;   // [ESP+0x13]
    bool bTargetSeen = false;         // [ESP+0x12]
    bool bTargetIsWaypoint = false;   // [ESP+0x11] (either waypoint)

    // without a scanner or a player the text is hidden again; without a
    // player ship (0x503) nothing else happens -- not even that.
    IObjRW* const Player = static_cast<IObjRW*>(GetPlayerObjRW());
    if (Scanner == nullptr || Player == nullptr)
    {
        ReticleRangeText->SetProperty(1, 1, 0);
        return;
    }
    CShip* const PlayerShip = static_cast<CShip*>(PlayerObjectOfClass(Player, 0x503u));   // [ESP+0x30]
    if (PlayerShip == nullptr)
        return;

    // the ship's target (its id restarts the select pulse when it
    // changes), and whether it sits exactly on the nav waypoint or the best-path waypoint.
    unsigned TargetId = 0;   // [ESP+0x4c]
    bool bWaypointDone = false;   // BL
    IObjRW* const Target = PlayerShip->get_target();
    if (Target != nullptr)
    {
        TargetId = Target->get_id();
        if (LastTargetId != TargetId)
        {
            SelectPulse = 0.0f;
            LastTargetId = TargetId;
        }
        if (WaypointRecord.bValid != 0 &&
            SameWorldPosition(Target->get_position(), WaypointRecord.World))
        {
            bWaypointDone = true;
            bTargetSeen = true;
            bTargetIsWaypoint = true;
        }
        if (BestPathRecord.bValid != 0 &&
            SameWorldPosition(Target->get_position(), BestPathRecord.World))
        {
            bTargetIsBestPath = true;
            bTargetSeen = true;
            bTargetIsWaypoint = true;
        }
    }
    else
    {
        LastTargetId = 0;
    }

    // the nav waypoint (unless it is the target): on screen its diamond
    // (the field) and label 1.1 radii above, else its edge arrow.
    if (!bWaypointDone && WaypointRecord.bValid != 0)
    {
        if (WaypointRecord.bOnScreen != 0)
        {
            DrawWaypointMarker(&WaypointRecord, &G_NnPaletteArgb_679b80, 0);
            double const LabelY = static_cast<double>(WaypointRecord.Screen.y) -
                                  static_cast<double>(WaypointRecord.ScreenRadius) * 1.1;
            Vector const Point =
                ReticleLabelPoint(static_cast<double>(WaypointRecord.Screen.x), LabelY);
            DrawReticleLabel(ReticleRangeText, &WaypointLabel, Point);
        }
        else
        {
            DrawWaypointArrow(&WaypointRecord);
        }
    }

    // the best-path waypoint the same way (unless it is the target).
    if (!bTargetIsBestPath && BestPathRecord.bValid != 0)
    {
        if (BestPathRecord.bOnScreen != 0)
        {
            DrawWaypointMarker(&BestPathRecord, &G_NnPaletteArgb_679b80, 1);
            double const LabelY = static_cast<double>(BestPathRecord.Screen.y) -
                                  static_cast<double>(BestPathRecord.ScreenRadius) * 1.1;
            Vector const Point =
                ReticleLabelPoint(static_cast<double>(BestPathRecord.Screen.x), LabelY);
            DrawReticleLabel(ReticleRangeText, &BestPathLabel, Point);
        }
        else
        {
            DrawWaypointArrow(&BestPathRecord);
        }
    }

    // every scanner entry (the count re-read each pass).
    for (int Index = 0; Index < ScanEntries->Count; ++Index)
    {
        Watchable const* const Watched = ScanEntries->Entries[Index].target();
        IObjRW* const Contact =
            (Watched != nullptr) ? ObjRwFromWatchable(const_cast<Watchable*>(Watched)) : nullptr;
        FHudContactRecord const& Record = Contacts.data()[Index];   // EBP
        if (Contact == nullptr)
            continue;
        bool const bIsTarget = Contact->get_id() == TargetId;
        if (bIsTarget)
            bTargetSeen = true;

        // off screen, its edge arrow (the id compared again).
        if (Record.bOnScreen == 0)
        {
            DrawOffScreenContact(Contact, &Record, Contact->get_id() == TargetId);
            continue;
        }
        // the target on screen gets its reticle, unless it is a waypoint
        // (drawn after the loop).
        if (bIsTarget)
        {
            if (!bTargetIsWaypoint)
                DrawOnScreenTarget(Contact, &Record);
            continue;
        }

        // a player ship nearer than 3000 (the field; TEST AH,5 / JP
        // -- ordered "below" only) gets its name label 2 pixels above its bracket.
        if (Record.bPlayerShip == 0)
            continue;
        if (!(Record.Distance < 3000.0f))
            continue;
        double const LabelY = static_cast<double>(Record.Screen.y) -
                              static_cast<double>(Record.ScreenRadius) -
                              static_cast<double>(2.0f);
        Vector const Point = ReticleLabelPoint(static_cast<double>(Record.Screen.x), LabelY);
        CObject const* const Object = Contact->cobject();   // slot 0x150
        if (Object == nullptr || (static_cast<unsigned>(Object->ObjectClass) & 0x503u) != 0x503u)
            continue;
        CShip const* const Ship = static_cast<CShip const*>(Object);

        // the label cache (+0x8fc, keyed by the ship's +0xb0 id). A new
        // entry gets "<faction short name> <pilot name>" in the PlayerName font at 0.171.
        unsigned const Key = Ship->Id;   // [ESP+0x34]
        std::map<unsigned, FHudNameLabel>::iterator It = NameLabels.find(Key);
        if (It == NameLabels.end())
        {
            // baseline builds the pair from a stack Font::String
            // (the routine) through the copy constructor the routine, inserts it (the routine),
            // tears the temporaries down and looks the key up again (the routine + the key
            // test). The node's string is a copy of a fresh one: the port constructs it fresh.
            NameLabels.emplace(std::piecewise_construct, std::forward_as_tuple(Key),
                               std::forward_as_tuple());
            It = NameLabels.find(Key);
            FFontString* const Label = &It->second.Text;   // EDI

            // the faction short name of the ship's vibe (+0x104), unless
            // the affiliation query fails or the ids is 0, -1, 0x30386 or 0x303b8; a lone space
            // is dropped, anything else is followed by " - ".
            unsigned ShortNameIds = 0;   // EBP
            int const Vibe = static_cast<int>(static_cast<CEqObj const*>(Ship)->VibeId);   // [ESP+0x90]
            unsigned Affiliation = 0;
            if (Reputation::Vibe::GetAffiliation(Vibe, Affiliation, false) == 0)
                ShortNameIds = Reputation::get_short_name(Affiliation);
            wchar_t Text[0x100];   // [ESP+0x228]
            Text[0] = 0;
            if (ShortNameIds != 0 && ShortNameIds != 0xffffffffu && ShortNameIds != 0x30386u &&
                ShortNameIds != 0x303b8u)
            {
                wchar_t Faction[0x80];   // [ESP+0x128]
                if (GetIdsStringWWithRawFallback(ShortNameIds, Faction, 0x80) == 0)
                    Faction[0] = 0;
                if (wcslen(Faction) > 1 || Faction[0] != L' ')
                {
                    wcscat(Text, Faction);
                    wcscat(Text, L" - "); // the field
                }
            }
            wcscat(Text, Ship->get_pilot_name());

            // the PlayerName font (the inlined the routine), the scale
            // (the routine, the field 0.171f = 0x3e2f1aa0, -1.0) and the text (the routine).
            FontStringSetFont(Label, PlayerNameFont);
            FontStringSetScale(Label, 0.171f, -1.0f);
            FontStringSetText(Label, Text);
        }

        // .
        DrawReticleLabel(ReticleRangeText, &It->second.Text, Point);
    }

    // the target is drawn here unless the scan already drew it (seen and
    // not a waypoint).
    if (bTargetSeen && !bTargetIsWaypoint)
    {
        ReticleRangeText->SetProperty(1, 1, 0);
        return;
    }

    // a fresh record for the ship's target: bValid set, the rest zero;
    // its type (slot 34), world position (slot 0), watch-list byte (client gameobj +0xa8, for
    // types 0x107ffdff) and player-ship flag (CShip +0x2bd).
    IObjRW* const ShipTarget = PlayerShip->get_target();
    if (ShipTarget == nullptr)
    {
        ReticleRangeText->SetProperty(1, 1, 0);
        return;
    }
    FHudContactRecord TargetRecord;   // [ESP+0x50]
    std::memset(&TargetRecord, 0, sizeof(TargetRecord));
    TargetRecord.bValid = 1;
    ShipTarget->get_type(TargetRecord.TypeFlags);   // slot 34
    TargetRecord.World = ShipTarget->get_position();
    if ((TargetRecord.TypeFlags & 0x107ffdffu) != 0)
    {
        FClientGameobj* const Gameobj = ClientObjDb_FindObject(ShipTarget->get_id(), 0);
        if (Gameobj != nullptr)
            TargetRecord.bWatchListed = Gameobj->bWatchListed;
    }
    CObject const* const TargetObject = ShipTarget->cobject();
    if (TargetObject != nullptr &&
        (static_cast<unsigned>(TargetObject->ObjectClass) & 0x503u) == 0x503u &&
        static_cast<CShip const*>(TargetObject)->bIsPlayer != 0)
    {
        TargetRecord.bPlayerShip = 1;
    }

    // projected from slot 5's centre with the CObject's slot 0x64 radius.
    // The screen radius and distance land in stack words that are copied into the record only on
    // the paths below (baseline reuses the radius's own slot and the loop counter's).
    Vector const Center = ShipTarget->get_center_of_mass();
    Vector PartCenter;
    float ProjectedRadius = ShipTarget->cobject()->get_physical_radius_r(PartCenter);   // [ESP+0x30]
    float Distance = 0.0f;   // [ESP+0x2c]
    Vector Screen;           // [ESP+0x98]
    bool const bOnScreen =
        ProjectContact(&Center, ProjectedRadius, &Screen, &ProjectedRadius, &Distance);

    // / 0x004eed20..0x004eedd4: on screen the record takes the projection,
    // radius and distance, off screen only the distance; both mark whether the target is the
    // best-path (+0x30) or nav (+0x31) waypoint, then the routine / the routine(.., 1).
    if (bOnScreen)
    {
        TargetRecord.Screen = Screen;
        TargetRecord.ScreenRadius = ProjectedRadius;
    }
    TargetRecord.Distance = Distance;
    TargetRecord.bIsBestPath = static_cast<unsigned char>(
        (BestPathRecord.bValid != 0 &&
         SameWorldPosition(ShipTarget->get_position(), BestPathRecord.World)) ? 1 : 0);
    TargetRecord.bIsWaypoint = static_cast<unsigned char>(
        (WaypointRecord.bValid != 0 &&
         SameWorldPosition(ShipTarget->get_position(), WaypointRecord.World)) ? 1 : 0);
    if (bOnScreen)
        DrawOnScreenTarget(ShipTarget, &TargetRecord);
    else
        DrawOffScreenContact(ShipTarget, &TargetRecord, true);

    // the range text hidden again.
    ReticleRangeText->SetProperty(1, 1, 0);
}

void HudTargetableObjects::DrawWaypointArrow(FHudContactRecord const* Record)
{
    // . 0x004f062b: the routine's result is not used.
    GetPlayerObjRW();

    // the waypoint in the listener camera's frame -- its orientation and
    // position (slot 4, this + 4, read as a transform) inverted onto World by the field.
    ICamera* const Listener = static_cast<ICamera*>(GetClientSoundListenerObject());
    float const* const Frame = Listener->GetOrientation();
    if (G_HudObjectsMath == nullptr)
        G_HudObjectsMath = Acquire3DMathEngine();
    FVector Local;   // [ESP+0x14]
    G_HudObjectsMath->InverseTransform(Local, *reinterpret_cast<FTransform const*>(Frame),
                                       *reinterpret_cast<FVector const*>(&Record->World));

    // baseline also builds RotateMatrix(0, 0, 180) (0x43340000) and
    // LookMatrix(x, y, 0), multiplies them (slot 8) and copies the product next to the arrow
    // position; nothing reads that copy.
    Vector Flat;
    Flat.x = Local.X;
    Flat.y = Local.Y;
    Flat.z = 0.0f;
    Vector Spin;
    Spin.x = 0.0f;
    Spin.y = 0.0f;
    Spin.z = 180.0f;
    Matrix const Rotation = RotateMatrix(Spin);
    Matrix const Look = LookMatrix(Flat);
    if (G_HudObjectsMath == nullptr)
        G_HudObjectsMath = Acquire3DMathEngine();
    FMatrix ArrowOrientation;
    G_HudObjectsMath->MulMatrix(ArrowOrientation, *reinterpret_cast<FMatrix const*>(&Look),
                                *reinterpret_cast<FMatrix const*>(&Rotation));

    // the arrow sits where the direction leaves the view, at depth -1.3
    // (the field): through a side edge when |y| * aspect is below |x| (ordered; TEST AH,5 /
    // JP), at x = +-tan(HalfFovX deg) * 1.3 * 0.945 (the field deg to rad, the field)
    // with y on the line (negative unless x > 0, TEST AH,0x41 / JZ); otherwise through the top
    // or bottom at y = +-tan(FovY deg) * 1.3 * 0.945 with x on the line.
    FSpaceCamera const* const Ui = Win3DGetCamera();
    double const AbsX = std::fabs(static_cast<double>(Local.X));
    double const AbsYAspect =
        std::fabs(static_cast<double>(Local.Y)) * static_cast<double>(Ui->Aspect);
    Vector ArrowPos;   // [ESP+0x74]; the copy at [ESP+0x34] is the one projected below
    ArrowPos.z = -1.3f;
    if (AbsYAspect < AbsX)
    {
        double Edge = std::tan(static_cast<double>(Ui->HalfFovXDeg) *
                               static_cast<double>(0.0174533f)) *
                      static_cast<double>(-(-1.3f)) * static_cast<double>(0.945f);
#ifdef OUTPOST_FRONTIER_WIDESCREEN_SUPPORT
        // Port-only Hor+: +0x98 is the 4:3 angle; the screen edge is the widened one.
        Edge *= SpaceCameraHorPlusScale(Ui);
#endif
        if (!(Local.X > 0.0f))
            Edge = -Edge;
        ArrowPos.y = static_cast<float>(
            (static_cast<double>(Local.Y) / static_cast<double>(Local.X)) * Edge);
        ArrowPos.x = static_cast<float>(Edge);
    }
    else
    {
        double Edge = std::tan(static_cast<double>(Ui->FovYDeg) *
                               static_cast<double>(0.0174533f)) *
                      static_cast<double>(-(-1.3f)) * static_cast<double>(0.945f);
        if (!(Local.Y > 0.0f))
            Edge = -Edge;
        ArrowPos.y = static_cast<float>(Edge);
        ArrowPos.x = static_cast<float>(
            static_cast<double>(ArrowPos.y) /
            (static_cast<double>(Local.Y) / static_cast<double>(Local.X)));
    }

    // with both waypoint tiles, the background (bytes 0c 3e 7a ff) and
    // the arrow (the field) at 0.06 (0x3d75c28f), mode 0, angle 0 (the routine).
    if (WaypointArrow != nullptr && WaypointArrowBackground != nullptr)
    {
        unsigned char const BackgroundRgba[4] = {0x0c, 0x3e, 0x7a, 0xff};
        unsigned long const ArrowRgba = G_NnPaletteArgb_679b80;
        NnMontageDrawTileAt(WaypointArrowBackground, &ArrowPos.x, BackgroundRgba, 0.06f, 0, 0.0f);
        NnMontageDrawTileAt(WaypointArrow, &ArrowPos.x,
                            reinterpret_cast<unsigned char const*>(&ArrowRgba), 0.06f, 0, 0.0f);
    }

    // the arrow's point through the Win3D camera (slot 13); when the
    // cursor lies within (-1 / -1.3) * 0.02733 / near * near * XScale of it (the field,
    // ; TEST AH,1 / JNZ leaves on "farther" or unordered), the record becomes the
    // hover pick (the field) and the forced pick is cleared.
    float ScreenX = 0.0f;
    float ScreenY = 0.0f;
    float ScreenZ = 0.0f;
    if ((AsCamera(Win3DGetCamera())->Project(&ScreenX, &ScreenY, &ScreenZ, &ArrowPos.x) & 0xff) == 0)
        return;
    int CursorX = 0;
    int CursorY = 0;
    int CursorZ = 0;
    GetClientSoftwareCursor(&CursorX, &CursorY, &CursorZ);   // / the field
    float const ScreenZCopy = ScreenZ;
    float const DeltaX = static_cast<float>(static_cast<double>(ScreenX) - CursorX);
    double const DeltaY = static_cast<double>(ScreenY) - CursorY;
    double const DeltaZ = static_cast<double>(ScreenZ) - static_cast<double>(ScreenZCopy);
    double const CursorDistance =
        std::sqrt(DeltaZ * DeltaZ + DeltaY * DeltaY +
                  static_cast<double>(DeltaX) * static_cast<double>(DeltaX));
    double const PickRadius = static_cast<double>(-1.0f) / static_cast<double>(-1.3f) *
                              static_cast<double>(0.02733f) / static_cast<double>(Ui->ZNear) *
                              static_cast<double>(Ui->ZNear) * static_cast<double>(Ui->XScale);
    if (!(PickRadius >= CursorDistance))
        return;
    G_HudHoverRecord = Record;
    G_HudPickOverrideId = 0;
}

namespace
{
    // ..the field -- a zero vector (static initialiser the branch). Its only
    // readers are the routine's two the routine calls, which pass its address as an argument
    // never reads.
    Vector const G_HudZeroVector = MakeVector(0.0f, 0.0f, 0.0f);

    // One reticle bar (0x004ef81e..0x004ef88a and its seven copies): four vertices in the
    // reticle's x / y plane at z = 0 -- Left/Bottom, Right/Bottom, Right/Top, Left/Top, Right
    // being Fraction * Width + Left for a fill (bFill), else Left + Width.
    void EmitReticleBarQuad(FHudMeshRect const& Rect, float Fraction, bool bFill)
    {
        float Right;
        if (bFill)
            Right = static_cast<float>(static_cast<double>(Fraction) * static_cast<double>(Rect.Width) +
                                       static_cast<double>(Rect.Left));
        else
            Right = static_cast<float>(static_cast<double>(Rect.Left) +
                                       static_cast<double>(Rect.Width));
        Win3DVertex3f(Rect.Left, Rect.Bottom, 0.0f);
        Win3DVertex3f(Right, Rect.Bottom, 0.0f);
        Win3DVertex3f(Right, Rect.Top, 0.0f);
        Win3DVertex3f(Rect.Left, Rect.Top, 0.0f);
    }

    // with a palette colour's R, G, B bytes (whole, or each * 3 / 4 truncated --
    // CDQ / AND 3 / SAR 2) and alpha 0xff.
    void SetReticleBarColor(unsigned long Argb, bool bDimmed)
    {
        unsigned char const* const Bytes = reinterpret_cast<unsigned char const*>(&Argb);
        if (bDimmed)
        {
            Win3DColor4ub(static_cast<unsigned char>((static_cast<int>(Bytes[0]) * 3) / 4),
                          static_cast<unsigned char>((static_cast<int>(Bytes[1]) * 3) / 4),
                          static_cast<unsigned char>((static_cast<int>(Bytes[2]) * 3) / 4), 0xff);
        }
        else
        {
            Win3DColor4ub(Bytes[0], Bytes[1], Bytes[2], 0xff);
        }
    }

    // A copy of P with its y replaced -- the label points of the routine all keep x / z.
    Vector WithY(Vector const& P, float Y)
    {
        return MakeVector(P.x, Y, P.z);
    }
}

void HudTargetableObjects::DrawRangeLabel(void const* /*Unused1*/, float Radius,
                                          FHudContactRecord const* /*Unused3*/, int Distance,
                                          Vector const* Point)
{
    // . 0x004eef50..0x004eef94: with Radius > 0 (TEST AH,0x41 / JNZ) the distance
    // to the surface, ftol(float(Distance) - Radius), a negative one read as 0.
    int Range = Distance;
    if (Radius > 0.0f)
    {
        Range = static_cast<int>(static_cast<double>(Distance) - static_cast<double>(Radius));
        if (Range < 0)
            Range = 0;
    }

    // metres up to 2000, else kilometres with one decimal (signed
    // divisions by 1000 and 100 through the reciprocal multiplies).
    char Text[0x100];   // local_100
    if (Range <= 2000)
        sprintf(Text, "%d M", Range);
    else
        sprintf(Text, "%d.%d K", Range / 1000, (Range % 1000) / 100); // s_%d.%d_K_005d9620

    // the routine(Text) on the range text control -- TextControl's
    // number-string setter (FontStringSetScale, then the routine: the wide text dropped, child
    // 0 a Font::NumberString, Text through its slot 12 as a printf format).
    static_cast<NnTextControl*>(ReticleRangeText)->SetNumberText(Text);

    // placed at Point (slot 42, property 5) and drawn (slot 13).
    ReticleRangeText->SetProperty(5, reinterpret_cast<uintptr_t>(Point), 0);
    ReticleRangeText->Draw();
}

void HudTargetableObjects::DrawOnScreenTarget(IObjRW* Object, FHudContactRecord const* Record)
{
    // . 0x004ef050..0x004ef0ae: a ship-typed record (0x7f0000) needs a ship CObject
    // (class & 0x503) whose vibe (CEqObj +0x104) has an affiliation (GetAffiliation == 0).
    unsigned TypeBits = Record->TypeFlags;   // EBX
    if ((TypeBits & 0x7f0000u) != 0)
    {
        CObject const* const ShipObject = Object->cobject();   // slot 0x150
        if (ShipObject == nullptr)
            return;
        if ((static_cast<unsigned>(ShipObject->ObjectClass) & 0x503u) != 0x503u)
            return;
        int const Vibe = static_cast<int>(static_cast<CEqObj const*>(ShipObject)->VibeId);   // [ESP+0x20]
        unsigned Affiliation = 0;   // [ESP+0x74]
        if (Reputation::Vibe::GetAffiliation(Vibe, Affiliation, false) != 0)
            return;
    }

    // the HUD waypoint's object counts as a waypoint (type 0x200); the
    // player (the routine), the HUD_Comm speaker test (the routine), the hat condition.
    bool bWaypointObject = false;   // [ESP+0x2b]
    if (HudIsWaypointObjectId(Object->get_id()))
    {
        TypeBits = 0x200;
        bWaypointObject = true;
    }
    IObjRW* const Player = static_cast<IObjRW*>(GetPlayerObjRW());   // [ESP+0x74]
    bool const bWatched = HudCommIsWatchedObjectId(Object->get_id());   // [ESP+0x12]
    bool const bHat = Record->bWatchListed != 0 || Record->bIsBestPath != 0;   // [ESP+0x11]

    // the reticle scale from the bracket size, 1 - (1 - r / 100) * 0.25
    // (the field, the field, the field); the corner offset (0.08, 0.05, 0) and the hat
    // offset (0.091, 0, 0) at rest.
    float Scale = static_cast<float>(
        1.0 - (1.0 - static_cast<double>(Record->ScreenRadius) / static_cast<double>(100.0f)) * 0.25);   // [ESP+0x24]
    float CornerX = 0.08f;   // [ESP+0x60] 0x3da3d70a
    float CornerY = 0.05f;   // [ESP+0x64] 0x3d4ccccd
    float CornerZ = 0.0f;    // [ESP+0x68]
    float HatX = 0.091f;     // [ESP+0x30] 0x3dba5e35
    float HatY = 0.0f;       // [ESP+0x34]
    float HatZ = 0.0f;       // [ESP+0x38]

    // for 0.3 s after a target change (the field; TEST AH,5 / JP --
    // ordered "below") the select pulse: the lock sound on its first frame (== 0, TEST AH,0x44 /
    // JP; the routine(0x24)), the pulse advanced by the frame time (the routine); while the
    // unrounded fraction is below 1 the scale eases in from 0.25 (only when above it; TEST AH,0x41
    // / JNZ) and both offsets grow with the fraction (the field 0.08, the field 0.05,
    // 0.091; the zero components become 0 * fraction).
    if (SelectPulse < 0.3f)
    {
        if (SelectPulse == 0.0f)
            PlayOrRewindUiSoundSlot(0x24);
        double const Pulse = GetClientFrameTimeCapped() + static_cast<double>(SelectPulse);
        SelectPulse = static_cast<float>(Pulse);
        double const Fraction = Pulse / static_cast<double>(0.3f);
        if (Fraction < 1.0)
        {
            if (Scale > 0.25f)
                Scale = static_cast<float>((static_cast<double>(Scale) - static_cast<double>(0.25f)) *
                                               Fraction +
                                           static_cast<double>(0.25f));
            CornerX = static_cast<float>(static_cast<double>(0.08f) * Fraction);
            CornerY = static_cast<float>(static_cast<double>(0.05f) * Fraction);
            float const Zero = static_cast<float>(static_cast<double>(0.0f) * Fraction);
            CornerZ = Zero;
            HatX = static_cast<float>(Fraction * static_cast<double>(0.091f));
            HatY = Zero;
            HatZ = Zero;
        }
    }

    // the bracket centre on the Win3D camera's near plane, scaled to
    // depth 1 -- the routine's label point.
    Vector const P = ReticleLabelPoint(static_cast<double>(Record->Screen.x),
                                       static_cast<double>(Record->Screen.y));   // [ESP+0x14]

    // the reticle transform -- the identity scaled by Scale (slot 4
    // Scale, in place) at P.
    FTransform Reticle;   // [ESP+0xa8]
    for (int Row = 0; Row < 3; ++Row)
    {
        for (int Column = 0; Column < 3; ++Column)
            Reticle.Rotation.D[Row][Column] = (Row == Column) ? 1.0f : 0.0f;
    }
    if (G_HudObjectsMath == nullptr)
        G_HudObjectsMath = Acquire3DMathEngine();
    G_HudObjectsMath->Scale(Reticle.Rotation, Reticle.Rotation, Scale);
    Reticle.Translation.X = P.x;
    Reticle.Translation.Y = P.y;
    Reticle.Translation.Z = P.z;

    // violet for a waypoint or the best-path object, else the marker
    // colour (the routine); the HUD_Comm speaker amber (the field) over either.
    bool const bWaypoint = (TypeBits & 0x200u) != 0;   // [ESP+0x48]
    unsigned long Color;   // [ESP+0x5c]
    if (bWaypoint || Record->bIsBestPath != 0)
    {
        Color = G_NnPaletteArgb_679b80;
    }
    else
    {
        unsigned long Marker = 0;   // [ESP+0x6c]
        Color = *HudTargetMarkerColor(&Marker, Object);
    }
    if (bWatched)
        Color = G_NnPaletteArgb_679ba4;
    unsigned char const* const ColorRgba = reinterpret_cast<unsigned char const*>(&Color);

    if (!bWaypoint)
    {
        // the four corners at Scale * 0.04 (the field), unturned:
        // +x+y mirrored (mode 1), -x-y flipped (2), -x+y plain (0), +x-y both (3).
        if (CurrentTargetCorner != nullptr)
        {
            float const Size = static_cast<float>(static_cast<double>(Scale) * static_cast<double>(0.04f));
            float const NegCornerX = -CornerX;   // [ESP+0x3c]
            Vector const TopRight = MakeVector(CornerX + P.x, CornerY + P.y, CornerZ + P.z);
            NnMontageDrawTileAt(CurrentTargetCorner, &TopRight.x, ColorRgba, Size, 1, 0.0f);
            Vector const BottomLeft = MakeVector(P.x - CornerX, P.y - CornerY, P.z - CornerZ);
            NnMontageDrawTileAt(CurrentTargetCorner, &BottomLeft.x, ColorRgba, Size, 2, 0.0f);
            Vector const TopLeft = MakeVector(P.x + NegCornerX, P.y + CornerY, P.z + CornerZ);
            NnMontageDrawTileAt(CurrentTargetCorner, &TopLeft.x, ColorRgba, Size, 0, 0.0f);
            Vector const BottomRight = MakeVector(P.x - NegCornerX, P.y - CornerY, P.z - CornerZ);
            NnMontageDrawTileAt(CurrentTargetCorner, &BottomRight.x, ColorRgba, Size, 3, 0.0f);
        }
    }
    else if (WaypointTarget != nullptr)
    {
        // the waypoint tile at Scale * 0.25, its alpha from the centre
        // distance: f = ((y^2 + x^2) * 4 - 0.01) * 3.44828 (the field, the field,
        // ), 1 above 1 (TEST AH,0x41 / JNZ keeps <= and NaN), else 0 below 0
        // (TEST AH,5 / JP); alpha = ftol((1 - f) * 150 + f * 255) (the field, the field).
        float const Size = static_cast<float>(static_cast<double>(Scale) * static_cast<double>(0.25f));   // [ESP+0x2c]
        double Fade = (static_cast<double>(P.y) * static_cast<double>(P.y) +
                       static_cast<double>(P.x) * static_cast<double>(P.x)) *
                          static_cast<double>(4.0f) -
                      static_cast<double>(0.01f);
        Fade = Fade * static_cast<double>(3.44828f);
        if (Fade > 1.0)
            Fade = 1.0;
        else if (Fade < 0.0)
            Fade = 0.0;
        int const Alpha = static_cast<int>((1.0 - Fade) * 150.0 + Fade * static_cast<double>(255.0f));
        unsigned long WaypointColor = Color;   // [ESP+0x20]
        reinterpret_cast<unsigned char*>(&WaypointColor)[3] = static_cast<unsigned char>(Alpha);
        NnMontageDrawTileAt(WaypointTarget, &P.x,
                            reinterpret_cast<unsigned char const*>(&WaypointColor), Size, 0, 0.0f);
    }

    // the hat for a watch-listed / best-path non-waypoint, both halves
    // at 0.04 (0x3d23d70a): P - H plain, H + P mirrored. The tile is not tested here
    // (the routine answers false for a null one).
    if (bHat && !bWaypoint)
    {
        Vector const HatLeft = MakeVector(P.x - HatX, P.y - HatY, P.z - HatZ);
        NnMontageDrawTileAt(CurrentTargetHat, &HatLeft.x, ColorRgba, 0.04f, 0, 0.0f);
        Vector const HatRight = MakeVector(HatX + P.x, HatY + P.y, HatZ + P.z);
        NnMontageDrawTileAt(CurrentTargetHat, &HatRight.x, ColorRgba, 0.04f, 1, 0.0f);
    }

    // the health bar for a CSimple (class & 3) unless it is a solar with
    // a non-destructible archetype (+0x88 -> +0x94 byte 0) or its TypeBits (+0xe0) have 1 / 2.
    CObject const* const BarObject = Object->cobject();
    bool bHealthBar = false;   // BL
    if (BarObject != nullptr && (static_cast<unsigned>(BarObject->ObjectClass) & 3u) == 3u)
    {
        unsigned const Class = static_cast<unsigned>(BarObject->ObjectClass);
        bool bIndestructible = false;
        if ((Class & 0x303u) == 0x303u && BarObject->Archetype != nullptr &&
            !static_cast<Archetype::Solar const*>(BarObject->Archetype)->bDestructible)
        {
            bIndestructible = true;
        }
        if (!bIndestructible && (static_cast<CSimple const*>(BarObject)->TypeBits & 3u) == 0)
            bHealthBar = true;
    }

    // relative health, and for a CEqObj (class & 0x103) the shield
    // (the routine) with its fraction only above a positive maximum (TEST AH,0x41 / JNZ).
    bool bShieldActive = false;   // [ESP+0x13], cleared first
    CSimple const* Simple = static_cast<CSimple const*>(Object->cobject());   // EDI
    if (Simple != nullptr && (static_cast<unsigned>(Simple->ObjectClass) & 3u) != 3u)
        Simple = nullptr;
    float Health = 0.0f;           // [ESP+0x4c]
    float ShieldFraction = 0.0f;   // [ESP+0x2c]
    if (Simple != nullptr)
    {
        Health = Simple->get_relative_health();
        if ((static_cast<unsigned>(Simple->ObjectClass) & 0x103u) == 0x103u)
        {
            float ShieldMax = 0.0f;   // [ESP+0x20]
            float ShieldCur = 0.0f;   // [ESP+0x70]
            HudSampleShieldHitPoints(const_cast<CEqObj*>(static_cast<CEqObj const*>(Simple)),
                                     &ShieldMax, &ShieldCur, &bShieldActive);
            if (ShieldMax > 0.0f)
                ShieldFraction = static_cast<float>(static_cast<double>(ShieldCur) /
                                                    static_cast<double>(ShieldMax));
        }
    }

    // a waypoint's bars sit 0.003 right (the field) and 0.04 down.
    if (bWaypoint)
    {
        Reticle.Translation.X = static_cast<float>(static_cast<double>(Reticle.Translation.X) +
                                                   static_cast<double>(0.003f));
        Reticle.Translation.Y = static_cast<float>(static_cast<double>(Reticle.Translation.Y) -
                                                   static_cast<double>(0.04f));
    }

    // the reticle transform times the Win3D camera's (slot 7,
    // ), copied (the routine) and bound as the model-view (the routine); quads,
    // opaque black.
    if (G_HudObjectsMath == nullptr)
        G_HudObjectsMath = Acquire3DMathEngine();
    FTransform Combined;   // [ESP+0x220]
    G_HudObjectsMath->MulTransform(
        Combined, Reticle, *reinterpret_cast<FTransform const*>(&Win3DGetCamera()->World[0]));
    FTransform const ModelView = Combined;   // [ESP+0x1f0]
    Win3DBindModelview(nullptr, reinterpret_cast<float const*>(&ModelView));
    Win3DBegin(kWin3DQuads);          // (8)
    Win3DColor4ub(0, 0, 0, 0xff);

    // the black backs.
    if (bHealthBar)
        EmitReticleBarQuad(ReticleHealthRect, 0.0f, false);
    if (bShieldActive)
        EmitReticleBarQuad(ReticleShieldsRect, 0.0f, false);

    // the fills, health red (the field) and shield blue
    // (the field), each only above 0 (TEST AH,0x41 / JNZ).
    if (bHealthBar && Health > 0.0f)
    {
        SetReticleBarColor(G_NnPaletteArgb_679ba8, false);
        EmitReticleBarQuad(ReticleHealthRect, Health, true);
    }
    if (bShieldActive && ShieldFraction > 0.0f)
    {
        SetReticleBarColor(G_NnPaletteArgb_679b98, false);
        EmitReticleBarQuad(ReticleShieldsRect, ShieldFraction, true);
    }
    Win3DEnd();

    // each bar's frame, a line loop at 3/4 of its colour.
    if (bHealthBar)
    {
        Win3DBegin(kWin3DLineLoop);   // (7)
        SetReticleBarColor(G_NnPaletteArgb_679ba8, true);
        EmitReticleBarQuad(ReticleHealthRect, 0.0f, false);
        Win3DEnd();
    }
    if (bShieldActive)
    {
        Win3DBegin(kWin3DLineLoop);
        SetReticleBarColor(G_NnPaletteArgb_679b98, true);
        EmitReticleBarQuad(ReticleShieldsRect, 0.0f, false);
        Win3DEnd();
    }

    // a waypoint record (the record's own type 0x200) gets the range at
    // 0.4 of the reticle height (the field) and the waypoint / best-path caption at 0.8
    // (the field), both scaled, above P -- and nothing else.
    unsigned const RecordType = Record->TypeFlags;   // [ESP+0x6c]
    double const ReticleHeight = static_cast<double>(ReticleRect.Top);   // +0x77c
    double const ReticleWidth = static_cast<double>(ReticleRect.Width);  // +0x778
    if ((RecordType & 0x200u) != 0)
    {
        Vector const RangePoint = WithY(
            P, static_cast<float>(ReticleHeight * static_cast<double>(0.4f) * static_cast<double>(Scale) +
                                  static_cast<double>(P.y)));   // [ESP+0x30]
        DrawRangeLabel(&G_HudZeroVector, 0.0f, Record, static_cast<int>(Record->Distance),
                       &RangePoint);
        Vector const CaptionPoint = WithY(
            P, static_cast<float>(ReticleHeight * static_cast<double>(0.8f) * static_cast<double>(Scale) +
                                  static_cast<double>(P.y)));   // [ESP+0x3c]
        DrawReticleLabel(ReticleRangeText,
                         (Record->bIsWaypoint != 0) ? &WaypointLabel : &BestPathLabel, CaptionPoint);
        return;
    }

    // the range below P at (0.5 - 0.1) of the height (the field,
    // ), measured to the surface (the CObject's slot 0x64 radius, 0 without one).
    Vector const RangePoint = WithY(
        P, static_cast<float>((ReticleHeight * static_cast<double>(0.5f) - ReticleHeight * 0.1) *
                                  static_cast<double>(Scale) +
                              static_cast<double>(P.y)));   // [ESP+0x9c]
    float Radius = 0.0f;   // [ESP+0x20]
    if (Object->cobject() != nullptr)
    {
        Vector RadiusCenter;   // [ESP+0xe4]
        Radius = Object->cobject()->get_physical_radius_r(RadiusCenter);
    }
    DrawRangeLabel(&G_HudZeroVector, Radius, Record, static_cast<int>(Record->Distance),
                   &RangePoint);

    // the ship behind the object (class & 0x503) and whether a player
    // owns it (CSimple +0xb4).
    CObject const* const ShipObject = Object->cobject();
    CShip const* Ship = nullptr;   // [ESP+0x20]
    if (ShipObject != nullptr && (static_cast<unsigned>(ShipObject->ObjectClass) & 0x503u) == 0x503u)
        Ship = static_cast<CShip const*>(ShipObject);
    FHudWideText Caption;   // [ESP+0x60]; the SEH-guarded buffer freed on the way out
    Caption.Text = nullptr;
    Caption.Capacity = 0;
    bool const bPlayerOwned =
        Ship != nullptr && static_cast<CSimple const*>(Ship)->OwnerPlayer != 0;   // [ESP+0x11]
    FFontString* const CaptionText = bPlayerOwned ? &TargetInfoText : &TargetNameText;

    if (Simple != nullptr)
    {
        // the cache keys -- loot units (1 unless CLoot, 0x803), visited
        // (0 for an unvisited solar, the routine) -- and the TypeBits 0x80 / 0x40 tests.
        int LootUnits = 1;   // [ESP+0x2c]
        if ((static_cast<unsigned>(Simple->ObjectClass) & 0x803u) == 0x803u)
            LootUnits = static_cast<int>(static_cast<CLoot const*>(Simple)->Units);
        int Visited = 1;     // ESI, [ESP+0x4c]
        if (HudIsUnvisitedSolar(Simple))
            Visited = 0;
        bool const bType80 = (Simple->TypeBits & 0x80u) != 0;   // [ESP+0x12]
        bool const bType40 = (Simple->TypeBits & 0x40u) != 0;   // CL

        // rebuild the caption unless the cache matches.
        if (!(NameCacheSimpleId == Simple->Id && NameCacheKey848 == LootUnits &&
              NameCacheKey84c == Visited))
        {
            // mode 1 (with the location line) for a ship, a docked
            // CEqObj (+0x160), or TypeBits 0x80 / 0x40; the routine without the short name.
            bool const bDocked = (static_cast<unsigned>(Simple->ObjectClass) & 0x103u) == 0x103u &&
                                 static_cast<CEqObj const*>(Simple)->DockingBaseId != 0;
            int const Mode = (Ship != nullptr || bDocked || bType80 || bType40) ? 1 : 0;
            if (HudBuildObjectCaption(Simple, &Caption, Mode, false))
            {
                // an unvisited non-ship shows its type caption
                // (the routine); a visited one its caption; a ship "<faction short name> "
                // (as the routine's labels) and its caption.
                wchar_t Text[0xf8];   // [ESP+0x250]
                Text[0] = 0;
                if (Ship == nullptr)
                {
                    if (Visited == 0)
                    {
                        wchar_t TypeText[0x80];   // [ESP+0xf0]
                        if (GetIdsStringWWithRawFallback(HudObjectTypeIds(Simple), TypeText, 0x80) == 0)
                            TypeText[0] = 0;
                        wcscpy(Text, TypeText);
                    }
                    else
                    {
                        wcscat(Text, Caption.Text);
                    }
                }
                else
                {
                    int const Vibe = static_cast<int>(static_cast<CEqObj const*>(Ship)->VibeId);   // [ESP+0x48]
                    unsigned Affiliation = 0;   // [ESP+0x70]
                    if (Reputation::Vibe::GetAffiliation(Vibe, Affiliation, false) == 0)
                    {
                        unsigned const ShortNameIds = Reputation::get_short_name(Affiliation);
                        if (ShortNameIds != 0 && ShortNameIds != 0xffffffffu &&
                            ShortNameIds != 0x30386u && ShortNameIds != 0x303b8u)
                        {
                            wchar_t Faction[0x80];   // [ESP+0xf0]
                            if (GetIdsStringWWithRawFallback(ShortNameIds, Faction, 0x80) == 0)
                                Faction[0] = 0;
                            if (wcslen(Faction) > 1 || Faction[0] != L' ')
                            {
                                wcscat(Text, Faction);
                                wcscat(Text, L" - "); // the field
                            }
                        }
                    }
                    wcscat(Text, Caption.Text);
                }

                // the keys stored, the timer reset, the caption string
                // in the PlayerName (player-owned) or HudSmall font at 0.171 (the field),
                // -1.0 (the inlined the routine, the routine, the routine).
                NameCacheKey84c = Visited;
                NameCacheSimpleId = Simple->Id;
                NameCacheKey848 = LootUnits;
                NameCacheTimer = 0.0f;
                FontStringSetFont(CaptionText, bPlayerOwned ? PlayerNameFont : SmallFont);
                FontStringSetScale(CaptionText, 0.171f, -1.0f);
                FontStringSetText(CaptionText, Text);
            }
            else
            {
                // no caption -- the keys invalidated, the string emptied
                // (the routine).
                NameCacheKey848 = -1;
                NameCacheKey84c = -1;
                NameCacheSimpleId = 0;
                NameCacheTimer = 0.0f;
                FontStringClearContents(CaptionText);
            }
        }

        // a TypeBits 0x80 object's cache expires after 10 s
        // (the field; TEST AH,0x41 / JNZ keeps it at or below) -- baseline advances the timer
        // twice per frame.
        if (bType80)
        {
            for (int Pass = 0; Pass < 2; ++Pass)
            {
                double const Timer = GetClientFrameTimeCapped() + static_cast<double>(NameCacheTimer);
                NameCacheTimer = static_cast<float>(Timer);
                if (Timer > static_cast<double>(10.0f))
                    NameCacheSimpleId = 0;
            }
        }

        // the caption above P at (0.05 + 0.5) of the height
        // (the field, the field), 0.03 higher (the field) for the waypoint's object.
        float CaptionY = static_cast<float>(
            (ReticleHeight * 0.05 + ReticleHeight * static_cast<double>(0.5f)) *
                static_cast<double>(Scale) +
            static_cast<double>(P.y));   // [ESP+0x34]
        if (bWaypointObject)
            CaptionY = static_cast<float>(static_cast<double>(CaptionY) + static_cast<double>(0.03f));
        DrawReticleLabel(ReticleRangeText, CaptionText, WithY(P, CaptionY));

        // the best-path object also gets the best-path caption, at
        // (0.2 + 0.5) of the height (the field) or, for the waypoint's object, at the height.
        if (Record->bIsBestPath != 0)
        {
            float BestPathY;
            if (!bWaypointObject)
                BestPathY = static_cast<float>(
                    (ReticleHeight * 0.2 + ReticleHeight * static_cast<double>(0.5f)) *
                        static_cast<double>(Scale) +
                    static_cast<double>(P.y));
            else
                BestPathY = static_cast<float>(ReticleHeight * static_cast<double>(Scale) +
                                               static_cast<double>(P.y));
            DrawReticleLabel(ReticleRangeText, &BestPathLabel, WithY(P, BestPathY));
        }
    }

    // a ship's toughness (slot 81) at the reticle's lower left
    // ((0.5 - 0.13) of the width, the field; (0.5 - 0.16) of the height, the field).
    // The string is rebuilt when the value changes: "%d" (the field) for 0..9999,
    // otherwise empty and the key left as it was.
    if (Ship != nullptr)
    {
        int const Toughness = Object->get_toughness();
        if (Toughness != TargetInfoKey)
        {
            wchar_t ToughnessText[0x80];   // [ESP+0xf0]
            ToughnessText[0] = 0;
            if (Toughness >= 0 && Toughness < 10000)
            {
                swprintf(ToughnessText, 0x80, L"%d", Toughness);
                TargetInfoKey = Toughness;
            }
            FontStringSetFont(&RangeText, SmallFont);
            FontStringSetScale(&RangeText, 0.171f, -1.0f);
            FontStringSetText(&RangeText, ToughnessText);
        }
        float const ToughnessY = static_cast<float>(
            (ReticleHeight * static_cast<double>(0.5f) - ReticleHeight * 0.16) *
                static_cast<double>(Scale) +
            static_cast<double>(P.y));   // [ESP+0x40]
        float const ToughnessX = static_cast<float>(
            (ReticleWidth * static_cast<double>(0.5f) - ReticleWidth * 0.13) *
                static_cast<double>(Scale) +
            static_cast<double>(P.x));   // [ESP+0x3c]
        DrawReticleLabel(ReticleRangeText, &RangeText, MakeVector(ToughnessX, ToughnessY, P.z));
    }

    // with no type bits in 0xfdff, the lead-fire marker: the player's
    // CShip (the inlined +0xc / +4 chain with the 0x503 test; baseline calls
    // CShip::get_tgt_lead_fire_pos on the result without a null test) finds the lead position;
    // the node (watcher on Object, the lead and its listener-camera projection, slot 17) is
    // appended to +0x910. baseline projects into an uninitialised stack vector; the port starts it
    // at zero.
    if ((RecordType & 0xfdffu) == 0)
    {
        CShip* const PlayerShip = static_cast<CShip*>(PlayerObjectOfClass(Player, 0x503u));
        Vector Lead;   // [ESP+0xd8]
        if (PlayerShip->get_tgt_lead_fire_pos(Object, Lead))
        {
            Vector LeadScreen = MakeVector(0.0f, 0.0f, 0.0f);   // [ESP+0x8c]
            ICamera* const Camera = static_cast<ICamera*>(GetClientSoundListenerObject());
            Camera->ProjectVec(&LeadScreen.x, &Lead.x);
            LeadMarkers.emplace_back();
            FHudLeadMarker& Marker = LeadMarkers.back();
            Marker.Target.set(Object);
            Marker.LeadWorld = Lead;
            Marker.LeadScreen = LeadScreen;
        }
    }

    // the caption buffer freed.
    if (Caption.Text != nullptr)
        free(Caption.Text);
}

unsigned long* HudTargetMarkerColor(unsigned long* OutArgb, IObjRW* Object)
{
    // . 0x004eeea2: the idle cyan unless a rule below replaces it.
    unsigned long Color = G_NnPaletteArgb_679bb0;
    unsigned Type = 0;   // baseline reuses the Object argument's stack slot
    if (Object->get_type(Type) == 0)   // slot 34 (+0x88); a failure keeps the cyan
    {
        if ((Type & 0x200u) != 0)
        {
            Color = G_NnPaletteArgb_679b80;
        }
        else if ((Type & 0x10000000u) != 0)
        {
            Color = 0xffffffffu;              // four 0xff bytes
        }
        else if ((Type & 0x5000000u) != 0)
        {
            Color = G_NnPaletteArgb_679ba8;
        }
        else
        {
            // the player (the routine) in ObjDB bucket 1 -- its
            // reputation colour of Object (the routine).
            void* const Player = GetPlayerObjRW();
            if (Player != nullptr && static_cast<FClientGameobj*>(Player)->ManagerKind == 1)
            {
                unsigned long Reputation = 0;
                HudReputationColor(&Reputation, static_cast<IObjRW*>(Player), Object);
                Color = Reputation;
            }
        }
    }
    *OutArgb = Color;
    return OutArgb;
}

bool HudIsWaypointObjectId(unsigned ObjectId)
{
    // . 0x004d4225..0x004d4267: the first player waypoint (the routine /
    // (0)) -- its position is the live waypoint (the routine on the field, which
    // baseline does not null-test), or its +0x10 id is ObjectId.
    if (GetPlayerWaypointCountbaseline() > 0)
    {
        FPlayerWaypointRecord* const Waypoint = GetPlayerWaypointbaseline(0);
        if (GetHudWaypoint()->IsWaypointAt(&Waypoint->Pos))
            return true;
        if (Waypoint->ObjectId != 0 && Waypoint->ObjectId == ObjectId)
            return true;
    }

    // the first best-path point (the routine / the routine(0)) -- its
    // resolved position (the routine; the system lands in the argument's stack slot, the id was
    // already read into EDI) is the live waypoint, or its resolved object (the routine) has id
    // ObjectId (slot 8).
    if (BestPathGetWaypointCount() <= 0)
        return false;
    FBestPathWaypoint* const Point = BestPathGetWaypoint(0);
    Vector Position;          // local_c
    unsigned System = 0;      // the ObjectId argument's slot
    if (BestPathWaypointResolvePosition(Point, &Position, &System) &&
        GetHudWaypoint()->IsWaypointAt(&Position))
    {
        return true;
    }
    IObjRW* const Resolved = BestPathWaypointResolveObject(Point);
    if (Resolved == nullptr)
        return false;
    return Resolved->get_id() == ObjectId;
}

namespace
{
    // The cursor test of the routine (0x004f0fe0..0x004f1088 and 0x004f10a9..0x004f116f), the
    // same shape as the routine's: ArrowPos through the Win3D camera (slot 13; a failure is
    // "not under the cursor"), then its distance to the cursor (the field / the field,
    // z against itself) compared with (-1 / -1.3) * Scale / near * near * x-scale
    // (the field, the field, the field, the field). TEST AH,1 / JNZ: an unordered or
    // "radius below distance" compare is a miss.
    bool IsCursorOverEdgeArrow(Vector const& ArrowPos, float Scale)
    {
        float ScreenX = 0.0f;
        float ScreenY = 0.0f;
        float ScreenZ = 0.0f;
        Vector Point = ArrowPos;   // the copy at [ESP+0x44] (the first site passes P)
        if ((AsCamera(Win3DGetCamera())->Project(&ScreenX, &ScreenY, &ScreenZ, &Point.x) & 0xff) == 0)
            return false;
        int CursorX = 0;
        int CursorY = 0;
        int CursorZ = 0;
        GetClientSoftwareCursor(&CursorX, &CursorY, &CursorZ);   // / the field
        float const ScreenZCopy = ScreenZ;   // [ESP+0x9c]
        float const DeltaX = static_cast<float>(static_cast<double>(ScreenX) - CursorX);
        double const DeltaY = static_cast<double>(ScreenY) - CursorY;
        double const DeltaZ = static_cast<double>(ScreenZ) - static_cast<double>(ScreenZCopy);
        double const CursorDistance =
            std::sqrt(DeltaZ * DeltaZ + DeltaY * DeltaY +
                      static_cast<double>(DeltaX) * static_cast<double>(DeltaX));
        FSpaceCamera const* const Ui = Win3DGetCamera();
        double const PickRadius = static_cast<double>(-1.0f) / static_cast<double>(-1.3f) *
                                  static_cast<double>(Scale) / static_cast<double>(Ui->ZNear) *
                                  static_cast<double>(Ui->ZNear) * static_cast<double>(Ui->XScale);
        return PickRadius >= CursorDistance;
    }

    // One bar of the off-screen target (0x004f13da..0x004f1446 and the three copies): four
    // vertices Left/Bottom, Right/Bottom, Right/Top, Left/Top at y = 0, Right being
    // Fraction * Width + Left for a fill (bFill), else Left + Width (Fraction not read).
    void EmitBarQuad(FHudMeshRect const& Rect, float Fraction, bool bFill)
    {
        float Right;
        if (bFill)
            Right = static_cast<float>(static_cast<double>(Fraction) * static_cast<double>(Rect.Width) +
                                       static_cast<double>(Rect.Left));
        else
            Right = static_cast<float>(static_cast<double>(Rect.Left) +
                                       static_cast<double>(Rect.Width));
        Win3DVertex3f(Rect.Left, 0.0f, Rect.Bottom);
        Win3DVertex3f(Right, 0.0f, Rect.Bottom);
        Win3DVertex3f(Right, 0.0f, Rect.Top);
        Win3DVertex3f(Rect.Left, 0.0f, Rect.Top);
    }

    // / 0x004f16f9..0x004f1735: a palette colour at three quarters (each
    // byte * 3 / 4, CDQ / AND 3 / SAR 2 -- truncating), through the routine (R, G, B).
    void SetDimmedPaletteColor(unsigned long Argb)
    {
        unsigned char const* const Bytes = reinterpret_cast<unsigned char const*>(&Argb);
        Win3DColor3ub(static_cast<unsigned char>((static_cast<int>(Bytes[0]) * 3) / 4),
                      static_cast<unsigned char>((static_cast<int>(Bytes[1]) * 3) / 4),
                      static_cast<unsigned char>((static_cast<int>(Bytes[2]) * 3) / 4));
    }
}

void HudTargetableObjects::DrawOffScreenContact(IObjRW* Object, FHudContactRecord const* Record,
                                                bool bIsTarget)
{
    // . 0x004f0918..0x004f096a: a ship-typed record (0x7f0000) needs a ship CObject
    // (class & 0x503) whose vibe (CEqObj +0x104) has an affiliation (GetAffiliation == 0).
    unsigned Affiliation = 0;   // [ESP+0x50]
    if ((Record->TypeFlags & 0x7f0000u) != 0)
    {
        CObject const* const ShipObject = Object->cobject();   // slot 0x150
        if (ShipObject == nullptr)
            return;
        if ((static_cast<unsigned>(ShipObject->ObjectClass) & 0x503u) != 0x503u)
            return;
        int const Vibe = static_cast<int>(static_cast<CEqObj const*>(ShipObject)->VibeId);
        if (Reputation::Vibe::GetAffiliation(Vibe, Affiliation, false) != 0)
            return;
    }

    // the type bits the arrow depends on.
    unsigned const TypeFlags = Record->TypeFlags;
    bool const bLoot = ((TypeFlags >> 28) & 1u) != 0;       // [ESP+0x23] 0x10000000
    bool const bDockingRing = ((TypeFlags >> 5) & 1u) != 0; // [ESP+0x42] 0x20
    bool const bJumpGate = ((TypeFlags >> 6) & 1u) != 0;   // [ESP+0x43] 0x40
    bool const bPlayerShip = Record->bPlayerShip != 0;      // [ESP+0x1b]
    bool const bPlanet = ((TypeFlags >> 1) & 1u) != 0;      // BL 0x2
    bool const bWatched = HudCommIsWatchedObjectId(Object->get_id());   // , [ESP+0x3b]
    bool const bHat = Record->bWatchListed != 0 || Record->bIsBestPath != 0;   // [ESP+0x12]
    bool bWaypoint = ((TypeFlags >> 9) & 1u) != 0;          // [ESP+0x13] 0x200

    // the contact's attitude towards the player (slot 26), 0 without one.
    float Attitude = 0.0f;   // [ESP+0x54]
    if (GetPlayerObjRW() != nullptr)
        Object->get_attitude_towards(Attitude, static_cast<IObjRW*>(GetPlayerObjRW()));

    if (!bIsTarget)
    {
        // without the hat or a player ship, only a hostile (attitude
        // <= -0.6, the field; TEST AH,0x41 / JP leaves on "above" and unordered) is shown.
        if (!bHat && !bPlayerShip && !(Attitude <= -0.6f))
            return;
        // planets, docking rings, jump gates and waypoint records never.
        if (bPlanet || bDockingRing || bJumpGate || Record->bIsBestPath != 0 ||
            Record->bIsWaypoint != 0)
        {
            return;
        }
    }
    else
    {
        // the target counts as a waypoint when the HUD waypoint points at
        // it (the routine); its select pulse restarts.
        if (HudIsWaypointObjectId(Object->get_id()))
            bWaypoint = true;
        SelectPulse = 0.0f;
    }

    // the alpha. A non-target fades from 500 (loot, the field) or
    // 1500 (the field) out by 0.001 per unit (the field; TEST AH,0x41 / JNZ keeps 1 at or
    // below the start and for NaN); then below 0 (ordered, TEST AH,5 / JP) is 0, above 1
    // (TEST AH,0x41 / JNZ) is 1, times 255 truncated (ftol).
    double Fade = 1.0;
    if (!bIsTarget)
    {
        float const Start = bLoot ? 500.0f : 1500.0f;
        if (Record->Distance > Start)
            Fade = 1.0 - (static_cast<double>(Record->Distance) - static_cast<double>(Start)) * 0.001;
    }
    if (Fade < 0.0)
        Fade = 0.0;
    if (Fade > 1.0)
        Fade = 1.0;
    int const Alpha = static_cast<int>(Fade * static_cast<double>(255.0f));   // EBX, [ESP+0x3c]
    unsigned char const BackgroundRgba[4] = {0x0c, 0x3e, 0x7a,
                                             static_cast<unsigned char>(Alpha)};   // [ESP+0x28]

    // blended, the light list cleared; the routine's result is not used.
    Win3DEnable(kWin3DBlend);                                         // (0xbe2)
    Win3DSetBlendFunc(kWin3DBlendSrcAlpha, kWin3DBlendInvSrcAlpha);   // (5, 6)
    EngineLightClearRegisteredList();
    GetPlayerObjRW();

    // violet for a waypoint, else the marker colour (the routine); the
    // HUD_Comm speaker amber (the field) over either; the alpha byte replaced.
    unsigned long Color;   // [ESP+0x24]
    if (bWaypoint)
    {
        Color = G_NnPaletteArgb_679b80;
    }
    else
    {
        unsigned long Marker = 0;   // [ESP+0x50]
        Color = *HudTargetMarkerColor(&Marker, Object);
    }
    if (bWatched)
        Color = G_NnPaletteArgb_679ba4;
    reinterpret_cast<unsigned char*>(&Color)[3] = static_cast<unsigned char>(Alpha);
    unsigned char const* const ColorRgba = reinterpret_cast<unsigned char const*>(&Color);

    // the contact's centre (slot 5) in the listener camera's frame
    // (the routine, slot 4 read as a transform) through the field's InverseTransform.
    ICamera* const Listener = static_cast<ICamera*>(GetClientSoundListenerObject());
    Vector const Center = Object->get_center_of_mass();   // [ESP+0x94]
    float const* const Frame = Listener->GetOrientation();
    if (G_HudObjectsMath == nullptr)
        G_HudObjectsMath = Acquire3DMathEngine();
    FVector Local;   // [ESP+0x2c]
    G_HudObjectsMath->InverseTransform(Local, *reinterpret_cast<FTransform const*>(Frame),
                                       *reinterpret_cast<FVector const*>(&Center));

    // LookMatrix(x, y, 0) * RotateMatrix(0, 0, 180) (slot 8) -- the
    // arrow's orientation; copied to [ESP+0x64], the rotation of the transform whose position is
    // the arrow point below ([ESP+0x88]).
    Vector Flat;
    Flat.x = Local.X;
    Flat.y = Local.Y;
    Flat.z = 0.0f;
    Vector Spin;
    Spin.x = 0.0f;
    Spin.y = 0.0f;
    Spin.z = 180.0f;
    Matrix const Rotation = RotateMatrix(Spin);
    Matrix const Look = LookMatrix(Flat);
    if (G_HudObjectsMath == nullptr)
        G_HudObjectsMath = Acquire3DMathEngine();
    FTransform ArrowXform;   // [ESP+0x64]
    G_HudObjectsMath->MulMatrix(ArrowXform.Rotation, *reinterpret_cast<FMatrix const*>(&Look),
                                *reinterpret_cast<FMatrix const*>(&Rotation));

    // the arrow point at depth -1.3 (the field), as the routine but
    // at 0.89 of the side edge (the field) or 0.855 of the top / bottom (the field): the
    // side when |y| * aspect is below |x| (TEST AH,5 / JP), its sign from x > 0 (TEST AH,0x41 /
    // JZ), y on the line; otherwise y at the edge (sign from y > 0) and x on the line.
    FSpaceCamera const* const Ui = Win3DGetCamera();
    double const AbsX = std::fabs(static_cast<double>(Local.X));
    double const AbsYAspect =
        std::fabs(static_cast<double>(Local.Y)) * static_cast<double>(Ui->Aspect);
    Vector ArrowPos;   // [ESP+0x88]
    ArrowPos.z = -1.3f;
    if (AbsYAspect < AbsX)
    {
        double Edge = std::tan(static_cast<double>(Ui->HalfFovXDeg) *
                               static_cast<double>(0.0174533f)) *   // , the field
                      static_cast<double>(-(-1.3f)) * static_cast<double>(0.89f);
#ifdef OUTPOST_FRONTIER_WIDESCREEN_SUPPORT
        // Port-only Hor+: +0x98 is the 4:3 angle; the screen edge is the widened one.
        Edge *= SpaceCameraHorPlusScale(Ui);
#endif
        if (!(Local.X > 0.0f))
            Edge = -Edge;
        ArrowPos.y = static_cast<float>(
            (static_cast<double>(Local.Y) / static_cast<double>(Local.X)) * Edge);
        ArrowPos.x = static_cast<float>(Edge);
    }
    else
    {
        double Edge = std::tan(static_cast<double>(Ui->FovYDeg) *
                               static_cast<double>(0.0174533f)) *
                      static_cast<double>(-(-1.3f)) * static_cast<double>(0.855f);
        if (!(Local.Y > 0.0f))
            Edge = -Edge;
        ArrowPos.y = static_cast<float>(Edge);
        ArrowPos.x = static_cast<float>(
            static_cast<double>(ArrowPos.y) /
            (static_cast<double>(Local.Y) / static_cast<double>(Local.X)));
    }
    ArrowXform.Translation.X = ArrowPos.x;
    ArrowXform.Translation.Y = ArrowPos.y;
    ArrowXform.Translation.Z = ArrowPos.z;

    // the arrow's roll -- 0 for a waypoint, else the routine of the
    // orientation's third column (rows 0 and 1). Every tile below except the waypoint pair
    // turns by roll + pi - pi/4 (the field float, the field double 0x3fe921fb60000000).
    float Roll = 0.0f;   // [ESP+0x1c]
    if (!bWaypoint)
        Roll = HudVectorAngle(ArrowXform.Rotation.D[0][2], ArrowXform.Rotation.D[1][2]);
    float const Angle = static_cast<float>(static_cast<double>(Roll) +
                                           static_cast<double>(3.14159265f) -
                                           static_cast<double>(0.7853982f));
    float const* const Point = &ArrowPos.x;

    // a non-target's arrow at 0.05 (0x3d4ccccd), mode 0: the background
    // then the arrow in Color -- loot tiles for loot (then the player tiles for a player ship),
    // the player tiles for a player ship, else (and not a waypoint) the NS tiles.
    if (!bIsTarget)
    {
        bool bPlayerTiles = false;
        if (bLoot)
        {
            if (LootArrowTile != nullptr && LootArrowBackground != nullptr)
            {
                NnMontageDrawTileAt(LootArrowBackground, Point, BackgroundRgba, 0.05f, 0, Angle);
                NnMontageDrawTileAt(LootArrowTile, Point, ColorRgba, 0.05f, 0, Angle);
            }
            bPlayerTiles = bPlayerShip;
        }
        else if (bWaypoint)
        {
            bPlayerTiles = bPlayerShip;   // -> 0x004f0df0
        }
        else if (bPlayerShip)
        {
            bPlayerTiles = true;          // -> 0x004f0df8
        }
        else if (NSTargetArrow != nullptr && NSArrowBackground != nullptr)
        {
            NnMontageDrawTileAt(NSArrowBackground, Point, BackgroundRgba, 0.05f, 0, Angle);
            NnMontageDrawTileAt(NSTargetArrow, Point, ColorRgba, 0.05f, 0, Angle);
        }
        if (bPlayerTiles && PlayerArrow != nullptr && PlayerArrowBackground != nullptr)
        {
            NnMontageDrawTileAt(PlayerArrowBackground, Point, BackgroundRgba, 0.05f, 0, Angle);
            NnMontageDrawTileAt(PlayerArrow, Point, ColorRgba, 0.05f, 0, Angle);
        }
    }

    if (bWaypoint)
    {
        // the waypoint pair, unturned, at 0.1 (0x3dcccccd) for the target
        // or 0.05; the cursor over it (0.02733, the field) makes the record the hover pick.
        // The target stops here.
        if (WaypointArrow != nullptr && WaypointArrowBackground != nullptr)
        {
            float const Size = bIsTarget ? 0.1f : 0.05f;
            NnMontageDrawTileAt(WaypointArrowBackground, Point, BackgroundRgba, Size, 0, 0.0f);
            NnMontageDrawTileAt(WaypointArrow, Point, ColorRgba, Size, 0, 0.0f);
            if (IsCursorOverEdgeArrow(ArrowPos, 0.02733f))
            {
                G_HudHoverRecord = Record;
                G_HudPickOverrideId = 0;
            }
        }
        if (bIsTarget)
            return;
    }
    else if (bIsTarget)
    {
        // with all four target tiles, the hat (Color with the alpha
        // byte) when watch-listed or best path, then the background and the arrow at 0.1 --
        // the player arrow twice for a player ship, else the target arrow and its outline.
        if (CurrentTargetArrow != nullptr && CurrentTargetArrowOutline != nullptr &&
            PlayerArrow != nullptr && CurrentTargetArrowHat != nullptr)
        {
            if (bHat)
            {
                unsigned long HatColor = Color;   // [ESP+0x14]
                reinterpret_cast<unsigned char*>(&HatColor)[3] = static_cast<unsigned char>(Alpha);
                NnMontageDrawTileAt(CurrentTargetArrowHat, Point,
                                    reinterpret_cast<unsigned char const*>(&HatColor), 0.1f, 0,
                                    Angle);
            }
            FButtonMontageTile const* Second;
            if (bPlayerShip)
            {
                NnMontageDrawTileAt(PlayerArrow, Point, BackgroundRgba, 0.1f, 0, Angle);
                Second = PlayerArrow;
            }
            else
            {
                NnMontageDrawTileAt(CurrentTargetArrow, Point, BackgroundRgba, 0.1f, 0, Angle);
                Second = CurrentTargetArrowOutline;
            }
            NnMontageDrawTileAt(Second, Point, ColorRgba, 0.1f, 0, Angle);
        }

        // the health bars for a CSimple (class & 3) unless it is a solar
        // with a non-destructible archetype (+0x94 byte 0), or its type bits (+0xe0) have 1 / 2.
        CObject const* const BarObject = Object->cobject();
        bool bHealthBar = false;   // BL
        if (BarObject != nullptr && (static_cast<unsigned>(BarObject->ObjectClass) & 3u) == 3u)
        {
            unsigned const Class = static_cast<unsigned>(BarObject->ObjectClass);
            bool bIndestructible = false;
            if ((Class & 0x303u) == 0x303u && BarObject->Archetype != nullptr &&
                !static_cast<Archetype::Solar const*>(BarObject->Archetype)->bDestructible)
            {
                bIndestructible = true;
            }
            if (!bIndestructible &&
                (static_cast<CSimple const*>(BarObject)->TypeBits & 3u) == 0)
            {
                bHealthBar = true;
            }
        }

        // relative health; for a CEqObj (class & 0x103) the shield
        // (the routine), its fraction only above a positive maximum (TEST AH,0x41 / JNZ).
        bool bShieldActive = false;   // [ESP+0x12], cleared first
        CObject const* HealthObject = Object->cobject();
        if (HealthObject != nullptr && (static_cast<unsigned>(HealthObject->ObjectClass) & 3u) != 3u)
            HealthObject = nullptr;
        float Health = 0.0f;       // [ESP+0x1c]
        float ShieldFraction = 0.0f;   // [ESP+0x14]
        if (HealthObject != nullptr)
        {
            Health = static_cast<CSimple const*>(HealthObject)->get_relative_health();
            if ((static_cast<unsigned>(HealthObject->ObjectClass) & 0x103u) == 0x103u)
            {
                float ShieldMax = 0.0f;   // [ESP+0x3c]
                float ShieldCur = 0.0f;   // [ESP+0x50]
                HudSampleShieldHitPoints(
                    const_cast<CEqObj*>(static_cast<CEqObj const*>(HealthObject)), &ShieldMax,
                    &ShieldCur, &bShieldActive);
                if (ShieldMax > 0.0f)
                    ShieldFraction = static_cast<float>(static_cast<double>(ShieldCur) /
                                                        static_cast<double>(ShieldMax));
            }
        }

        // the arrow transform times the Win3D camera's (slot 7,
        // ) as the model-view (the routine), quads in black.
        if (G_HudObjectsMath == nullptr)
            G_HudObjectsMath = Acquire3DMathEngine();
        FTransform BarXform;   // [ESP+0xc4], copied to [ESP+0xf4]
        G_HudObjectsMath->MulTransform(
            BarXform, ArrowXform,
            *reinterpret_cast<FTransform const*>(&Win3DGetCamera()->World[0]));
        Win3DBindModelview(nullptr, reinterpret_cast<float const*>(&BarXform));
        Win3DBegin(kWin3DQuads);   // (8)
        Win3DColor3ub(0, 0, 0);

        // the black backs of both bars.
        if (bHealthBar)
            EmitBarQuad(HealthBarRect, 0.0f, false);
        if (bShieldActive)
            EmitBarQuad(ShieldBarRect, 0.0f, false);

        // the fills -- health in red (the field), shield in blue
        // (the field) -- each only above 0 (TEST AH,0x41 / JNZ).
        if (bHealthBar && Health > 0.0f)
        {
            unsigned char const* const Red =
                reinterpret_cast<unsigned char const*>(&G_NnPaletteArgb_679ba8);
            Win3DColor3ub(Red[0], Red[1], Red[2]);
            EmitBarQuad(HealthBarRect, Health, true);
        }
        if (bShieldActive && ShieldFraction > 0.0f)
        {
            unsigned char const* const Blue =
                reinterpret_cast<unsigned char const*>(&G_NnPaletteArgb_679b98);
            Win3DColor3ub(Blue[0], Blue[1], Blue[2]);
            EmitBarQuad(ShieldBarRect, ShieldFraction, true);
        }
        Win3DEnd();

        // each bar's frame as a line loop in its colour at 3/4.
        if (bHealthBar)
        {
            Win3DBegin(kWin3DLineLoop);   // (7)
            SetDimmedPaletteColor(G_NnPaletteArgb_679ba8);
            EmitBarQuad(HealthBarRect, 0.0f, false);
            Win3DEnd();
        }
        if (bShieldActive)
        {
            Win3DBegin(kWin3DLineLoop);
            SetDimmedPaletteColor(G_NnPaletteArgb_679b98);
            EmitBarQuad(ShieldBarRect, 0.0f, false);
            Win3DEnd();
        }
        return;
    }
    else if (bHat && NSTargetHat != nullptr)
    {
        // a watch-listed / best-path non-target's hat, Color with the
        // alpha byte, at 0.05.
        unsigned long HatColor = Color;   // [ESP+0x14]
        reinterpret_cast<unsigned char*>(&HatColor)[3] = static_cast<unsigned char>(Alpha);
        NnMontageDrawTileAt(NSTargetHat, Point, reinterpret_cast<unsigned char const*>(&HatColor),
                            0.05f, 0, Angle);
    }

    // the cursor over the arrow (0.03, the field) makes the object
    // the forced pick (the field) and clears the hover record.
    if (IsCursorOverEdgeArrow(ArrowPos, 0.03f))
    {
        G_HudPickOverrideId = Record->ObjectId;
        G_HudHoverRecord = nullptr;
    }
}

void HudTargetableObjects::CollectLeadMarkers()
{
    // . 0x004f1df0..0x004f1e21: the player, the listener camera, the surface's last
    // pixel column and row.
    IObjRW* const Player = static_cast<IObjRW*>(GetPlayerObjRW());
    if (Player == nullptr)
        return;
    ICamera* const Camera = static_cast<ICamera*>(GetClientSoundListenerObject());
    int const LastColumn = static_cast<int>(G_RenderSurfaceWidth) - 1;   // - 1
    int const LastRow = static_cast<int>(G_RenderSurfaceHeight) - 1;     // - 1

    // every gameobj of ObjDB bucket 9 (the routine, +0x14 next).
    int const Kind = 9;
    for (FClientGameobj* Gameobj = ClientObjDb_GetKindHead(&Kind); Gameobj != nullptr;
         Gameobj = Gameobj->ListNext)
    {
        // not being reaped (slot 86), with a collision state (the
        // object is passed only when its class has bit 0).
        if (Gameobj->Slot158())
            continue;
        CObject* const Object = CObjectOfGameobj(Gameobj);
        CObject const* const Engine =
            (Object != nullptr && (static_cast<unsigned>(Object->ObjectClass) & 1u) == 1u) ? Object
                                                                                           : nullptr;
        if (!PhySys::GetCollisionState(Engine))
            continue;

        // projected (slot 17) and inside the surface on both axes; a
        // point that had to be clamped (below 0 ordered, or above the last pixel) is skipped.
        IObjRW* const Object_ = Gameobj;
        Vector Screen;
        Screen.x = 0.0f;
        Screen.y = 0.0f;
        Screen.z = 0.0f;
        if ((Camera->ProjectVec(&Screen.x, &Object_->get_position().x) & 0xff) == 0)
            continue;
        bool bClamped = false;
        if (Screen.x < 0.0f)
        {
            Screen.x = 0.0f;
            bClamped = true;
        }
        else if (static_cast<double>(Screen.x) > static_cast<double>(LastColumn))
        {
            Screen.x = static_cast<float>(LastColumn);
            bClamped = true;
        }
        if (Screen.y < 0.0f)
        {
            Screen.y = 0.0f;
            bClamped = true;
        }
        else if (static_cast<double>(Screen.y) > static_cast<double>(LastRow))
        {
            Screen.y = static_cast<float>(LastRow);
            bClamped = true;
        }
        if (bClamped)
            continue;

        // the lead position from the player's ship. baseline makes the
        // thiscall on whatever the 0x503 test left, null included.
        CShip* const PlayerShip = static_cast<CShip*>(PlayerObjectOfClass(Player, 0x503u));
        Vector Lead;
        if (!PlayerShip->get_tgt_lead_fire_pos(Object_, Lead))
            continue;

        // the marker -- a watcher on the gameobj, the lead position and
        // its projection through the listener camera (slot 17; its result is not tested) --
        // appended to +0x910.
        LeadMarkers.emplace_back();
        FHudLeadMarker& Marker = LeadMarkers.back();
        Marker.Target.set(Gameobj);
        Marker.LeadWorld = Lead;
        Marker.LeadScreen.x = 0.0f;
        Marker.LeadScreen.y = 0.0f;
        Marker.LeadScreen.z = 0.0f;
        ICamera* const LeadCamera = static_cast<ICamera*>(GetClientSoundListenerObject());
        LeadCamera->ProjectVec(&Marker.LeadScreen.x, &Marker.LeadWorld.x);
    }
}

void HudTargetableObjects::DrawLeadMarkers()
{
    // . 0x004f2079..0x004f2094: the routine's result is not used; the player's
    // target (slot 29 into a zeroed local; the player is not null-tested -- Render calls this
    // only for a player ship).
    GetClientSoundListenerObject();
    IObjRW* const Player = static_cast<IObjRW*>(GetPlayerObjRW());
    IObjRW* PlayerTarget = nullptr;
    Player->get_target(PlayerTarget);

    // the red of the field (bytes b0..b3) as b3:b0:b1:b2, once with
    // its alpha and once with alpha 0 for the edges.
    unsigned char const* const Red = reinterpret_cast<unsigned char const*>(&G_NnPaletteArgb_679ba8);
    unsigned long const EdgeColor = (static_cast<unsigned long>(Red[0]) << 16) |
                                    (static_cast<unsigned long>(Red[1]) << 8) |
                                    static_cast<unsigned long>(Red[2]);
    unsigned long const Color = (static_cast<unsigned long>(Red[3]) << 24) | EdgeColor;

    // a cross on every marker whose object is still there: 6 pixels
    // of the 640-wide unit (the field 0.0015625 * 6) on the player's target,
    // 2 on any object of type 0x55f0000, none otherwise; a zero size draws nothing (TEST AH,0x44
    // / JNP). Each arm is a 2-wide line (0x40000000).
    for (std::list<FHudLeadMarker>::iterator It = LeadMarkers.begin(); It != LeadMarkers.end();
         ++It)
    {
        FHudLeadMarker const& Marker = *It;
        IObjRW* const Object = ObjRwFromWatchable(const_cast<Watchable*>(Marker.Target.target()));
        if (Object == nullptr)
            continue;
        unsigned Type = 0;
        Object->get_type(Type);   // slot 34
        float Size = 0.0f;
#ifdef OUTPOST_FRONTIER_WIDESCREEN_SUPPORT
        // Port-only Hor+: marker size follows the height-scaled interface.
        double const Unit = WidescreenEquivalentSurfaceWidth() * static_cast<double>(0.0015625f);
#else
        double const Unit = static_cast<double>(static_cast<int>(G_RenderSurfaceWidth)) *
                            static_cast<double>(0.0015625f);   // FILD the field
#endif
        if (Object == PlayerTarget)
            Size = static_cast<float>(Unit * 6.0);
        else if ((Type & 0x55f0000u) != 0)
            Size = static_cast<float>(Unit + Unit);
        else
            continue;
        if (Size == 0.0f)
            continue;

        float const X = Marker.LeadScreen.x;
        float const Y = Marker.LeadScreen.y;
        float const LeftEnd[3] = {X - Size, Y, 0.0f};
        float const RightEnd[3] = {Size + X, Y, 0.0f};
        Win3DImmedDrawLine(LeftEnd, RightEnd, Color, EdgeColor, 2.0f);
        float const TopEnd[3] = {X, Y - Size, 0.0f};
        float const BottomEnd[3] = {X, Size + Y, 0.0f};
        Win3DImmedDrawLine(TopEnd, BottomEnd, Color, EdgeColor, 2.0f);
    }
}

void HudTargetableObjects::UpdateAimPoint()
{
    // . 0x004f222c..0x004f226c: the player (nothing without one), the fire attitude
    // cleared, the id of the player's target (slot 29, then slot 8).
    IObjRW* const Player = static_cast<IObjRW*>(GetPlayerObjRW());   // , [ESP+0x2c]
    if (Player == nullptr)
        return;
    FireAttitude = 0;
    unsigned PlayerTargetId = 0;   // [ESP+0x14]
    IObjRW* PlayerTarget = nullptr;
    Player->get_target(PlayerTarget);
    if (PlayerTarget != nullptr)
        PlayerTargetId = PlayerTarget->get_id();

    // a simple under the camera ray moving slower than 5 (|v|^2 < 25,
    // ; summed x, y, z) with any of the type bits 0x107ffdff counts; its client
    // object's attitude towards the player becomes the fire attitude.
    bool bRayUsed = false;   // [ESP+0xf]
    CObject* const RayTarget = RayHit.Object;
    if (RayTarget != nullptr && (static_cast<unsigned>(RayTarget->ObjectClass) & 3u) == 3u)
    {
        Vector const Velocity = RayTarget->get_velocity();   // slot 0x54
        double const SpeedSq = static_cast<double>(Velocity.x) * Velocity.x +
                               static_cast<double>(Velocity.y) * Velocity.y +
                               static_cast<double>(Velocity.z) * Velocity.z;
        CSimple const* const Simple = static_cast<CSimple const*>(RayTarget);
        if (SpeedSq < 25.0 && (Simple->TypeBits & 0x107ffdffu) != 0)
        {
            bRayUsed = true;
            FClientGameobj* const Gameobj = ClientObjDb_FindObject(Simple->Id, 0);
            if (Gameobj != nullptr)
                FireAttitude = GetAttitudeClass(Player, Gameobj);
        }
    }

    // the listener camera, the cursor as a fraction of the surface from
    // its centre (as the routine), and the depth to beat: FLT_MAX, or the ray hit's distance
    // squared when the ray object counted.
    ICamera* const Camera = static_cast<ICamera*>(GetClientSoundListenerObject());
    int CursorX = 0;
    int CursorY = 0;
    int CursorZ = 0;
    GetClientSoftwareCursor(&CursorX, &CursorY, &CursorZ);   // / the field
    double const Width = static_cast<double>(static_cast<int>(G_RenderSurfaceWidth));
    double const Height = static_cast<double>(static_cast<int>(G_RenderSurfaceHeight));
    float BestScreenDistSq = FLT_MAX;   // [ESP+0x2c]
    float BestDepthSq = FLT_MAX;        // [ESP+0x14]
    int BestAttitude = 0;               // [ESP+0x28]
    float const HalfWidth = static_cast<float>(Width * 0.5);
    float const CursorFracX =
        static_cast<float>((static_cast<double>(CursorX) - static_cast<double>(HalfWidth)) / Width);
    float const HalfHeight = static_cast<float>(Height * 0.5);
    float const CursorFracY =
        static_cast<float>((static_cast<double>(CursorY) - static_cast<double>(HalfHeight)) / Height);
    if (bRayUsed)
        BestDepthSq = static_cast<float>(static_cast<double>(RayHit.Distance) * RayHit.Distance);

    // the lead marker nearest the camera among those whose screen
    // position lies within a radius of the cursor (0.003 for the player's target, else 0.002 --
    // compared against the squared distance, as baseline does), nearer than the depth to beat.
    // The marker's watcher is not null-tested before its slot 8 call, as baseline.
    FHudLeadMarker const* Best = nullptr;   // EBX
    for (std::list<FHudLeadMarker>::iterator It = LeadMarkers.begin(); It != LeadMarkers.end();
         ++It)
    {
        FHudLeadMarker const& Marker = *It;
        IObjRW* const Object = ObjRwFromWatchable(const_cast<Watchable*>(Marker.Target.target()));
        float const Radius = (Object->get_id() == PlayerTargetId) ? 0.003f : 0.002f;
        double const Dx = static_cast<double>(CursorFracX) -
                          (static_cast<double>(Marker.LeadScreen.x) - static_cast<double>(HalfWidth)) / Width;
        double const Dy = static_cast<double>(CursorFracY) -
                          (static_cast<double>(Marker.LeadScreen.y) - static_cast<double>(HalfHeight)) / Height;
        double const ScreenDistSq = Dy * Dy + Dx * Dx;
        if (!(ScreenDistSq < static_cast<double>(Radius)))
            continue;
        Vector const* const CameraPos = reinterpret_cast<Vector const*>(Camera->GetPosition());
        Vector Offset;
        Offset.x = Marker.LeadWorld.x - CameraPos->x;
        Offset.y = Marker.LeadWorld.y - CameraPos->y;
        Offset.z = Marker.LeadWorld.z - CameraPos->z;
        double const DepthSq = static_cast<double>(Offset.z) * Offset.z +
                               static_cast<double>(Offset.y) * Offset.y +
                               static_cast<double>(Offset.x) * Offset.x;
        if (!(DepthSq < static_cast<double>(BestDepthSq)))
            continue;
        BestScreenDistSq = static_cast<float>(ScreenDistSq);
        BestDepthSq = static_cast<float>(DepthSq);
        Best = &Marker;
        BestAttitude = GetAttitudeClass(Player, Object);
    }

    // a marker latches the fire attitude; unless auto-aim is set to 5
    // (options 0x35 > 0 and 0x36 == 5), a marker within 0.001 (the field) of the cursor puts
    // the aim point on its lead position.
    bFireAttitude = 0;
    if (Best != nullptr)
    {
        bFireAttitude = 1;
        FireAttitude = BestAttitude;
        bool const bAutoAim5 =
            GetGameOption(kGameOptPlayerGroup) > 0 && GetGameOption(kGameOptAutoAim) == 5;
        if (!bAutoAim5 && BestScreenDistSq < 0.001f)
        {
            AimPoint = Best->LeadWorld;
            return;
        }

        // otherwise the cursor ray (slot 14, normalised: summed y, z, x)
        // out to the marker's distance from the camera (summed z, y, x).
        Vector Direction;
        Camera->Unproject(&Direction.x, static_cast<float>(CursorX), static_cast<float>(CursorY));
        double const InvLength = 1.0 / std::sqrt(static_cast<double>(Direction.y) * Direction.y +
                                                 static_cast<double>(Direction.z) * Direction.z +
                                                 static_cast<double>(Direction.x) * Direction.x);
        Direction.x = static_cast<float>(Direction.x * InvLength);
        Direction.y = static_cast<float>(Direction.y * InvLength);
        Direction.z = static_cast<float>(Direction.z * InvLength);
        Vector const* const CameraPos = reinterpret_cast<Vector const*>(Camera->GetPosition());
        double const Dx = static_cast<double>(Best->LeadWorld.x) - static_cast<double>(CameraPos->x);
        double const Dy = static_cast<double>(Best->LeadWorld.y) - static_cast<double>(CameraPos->y);
        double const Dz = static_cast<double>(Best->LeadWorld.z) - static_cast<double>(CameraPos->z);
        double const Distance = std::sqrt(Dz * Dz + Dy * Dy + Dx * Dx);
        Vector Reach;
        Reach.x = static_cast<float>(Direction.x * Distance);
        Reach.y = static_cast<float>(Direction.y * Distance);
        Reach.z = static_cast<float>(Direction.z * Distance);
        Vector const* const Origin = reinterpret_cast<Vector const*>(Camera->GetPosition());
        AimPoint.x = Reach.x + Origin->x;
        AimPoint.y = Reach.y + Origin->y;
        AimPoint.z = Reach.z + Origin->z;
        return;
    }

    // no marker -- the cursor ray (summed y, z, x) out to the farthest
    // active gun's range (CShip::get_farthest_active_gun_range, called on whatever the 0x503 test
    // left, as baseline), or 1e10 (0x501502f9) when that fails.
    Vector Direction;
    Camera->Unproject(&Direction.x, static_cast<float>(CursorX), static_cast<float>(CursorY));
    double const InvLength = 1.0 / std::sqrt(static_cast<double>(Direction.y) * Direction.y +
                                             static_cast<double>(Direction.z) * Direction.z +
                                             static_cast<double>(Direction.x) * Direction.x);
    Direction.x = static_cast<float>(Direction.x * InvLength);
    Direction.y = static_cast<float>(Direction.y * InvLength);
    Direction.z = static_cast<float>(Direction.z * InvLength);
    float GunRange = 0.0f;   // [ESP+0x10]
    CShip* const PlayerShip = static_cast<CShip*>(PlayerObjectOfClass(Player, 0x503u));
    if (!PlayerShip->get_farthest_active_gun_range(GunRange))
        GunRange = 1.0e10f;

    // a counted ray hit nearer than that range (ordered <) sets the
    // reach instead.
    float Reach = GunRange;
    if (bRayUsed && RayHit.Distance < GunRange)
        Reach = RayHit.Distance;
    Vector Scaled;
    Scaled.x = static_cast<float>(static_cast<double>(Direction.x) * Reach);
    Scaled.y = static_cast<float>(static_cast<double>(Direction.y) * Reach);
    Scaled.z = static_cast<float>(static_cast<double>(Direction.z) * Reach);
    Vector const* const Origin = reinterpret_cast<Vector const*>(Camera->GetPosition());
    AimPoint.x = Scaled.x + Origin->x;
    AimPoint.y = Scaled.y + Origin->y;
    AimPoint.z = Scaled.z + Origin->z;
}

void HudTargetableObjects::UpdateMissileAlert()
{
    // . 0x004f18d6..0x004f1925: the blink timer (0.1, the field) -- below it
    // nothing happens; at or past the period the phase flips and the timer restarts.
    bool bToggled = false;   // [ESP+0x7]
    double const Timer =
        GetClientFrameTimeCapped() + static_cast<double>(MissileBlinkTimer);
    MissileBlinkTimer = static_cast<float>(Timer);
    if (!NotBelow(Timer, 0.1f))
        return;
    if (!(Timer < static_cast<double>(MissileBlinkPeriod)))   // TEST AH,1 / JNZ keeps < or unordered
    {
        bMissileBlinkOn = (bMissileBlinkOn == 0) ? 1 : 0;
        MissileBlinkTimer = 0.0f;
        bToggled = true;
    }

    // the nearest scanned missile (type 0x1000000) targeting the player,
    // by squared distance (summed z, y, x; ordered <), starting at 1e12 (0x5368d4a5). The first
    // one found since the alert went dark queues the voice line: "incoming_torpedo" for a
    // munition of hp type 0xa (hp_torpedo), else "incoming_missile".
    float Nearest = 1.0e12f;   // [ESP+0x18]
    bool bAny = false;         // [ESP+0x12]
    IObjRW* const Player = static_cast<IObjRW*>(GetPlayerObjRW());
    if (Player != nullptr)
    {
        for (int Index = 0; Index < ScanEntries->Count; ++Index)
        {
            IObjRW* const Missile =
                ObjRwFromWatchable(const_cast<Watchable*>(ScanEntries->Entries[Index].target()));
            if (Missile == nullptr)
                continue;
            if ((Contacts.data()[Index].TypeFlags & 0x1000000u) == 0)
                continue;
            IObjRW* MissileTarget = nullptr;
            if (Missile->get_target(MissileTarget) != 0)   // slot 29 (+0x74)
                continue;
            if (MissileTarget == nullptr)
                continue;
            if (Player->get_id() != MissileTarget->get_id())
                continue;
            Vector const& MissilePos = Missile->get_position();
            Vector const& PlayerPos = Player->get_position();
            double const Dx = static_cast<double>(PlayerPos.x) - static_cast<double>(MissilePos.x);
            double const Dy = static_cast<double>(PlayerPos.y) - static_cast<double>(MissilePos.y);
            double const Dz = static_cast<double>(PlayerPos.z) - static_cast<double>(MissilePos.z);
            double const DistanceSq = Dz * Dz + Dy * Dy + Dx * Dx;
            if (!(DistanceSq < static_cast<double>(Nearest)))
                continue;
            Nearest = static_cast<float>(DistanceSq);
            bAny = true;
            if (bMissileWarned != 0)
                continue;
            bMissileWarned = 1;
            Archetype::Root const* const Arch = Missile->cobject()->get_archetype();
            if (Arch != nullptr &&
                static_cast<Archetype::Munition const*>(Arch)->HpType == 0xa)
            {
                QueueHazardVoiceLine(G_VoiceIncomingTorpedo);   // (the field, 0)
            }
            else
            {
                QueueHazardVoiceLine(G_VoiceIncomingMissile);   // (the field, 0)
            }
        }
    }

    // shown while a missile is there and the phase is off; the warned
    // latch follows "any". With none, the period drops back to 0.1; with one, a flip rings UI
    // sound 0x19 ("ui_incoming_missile") and the period becomes sqrt(nearest) * 0.001 (a double,
    // ), at least 0.1.
    uintptr_t const bHide = (bAny && bMissileBlinkOn == 0) ? 0u : 1u;
    MissileAlert->SetProperty(1, bHide, 0);   // slot 42
    bMissileWarned = bAny ? 1 : 0;
    if (!bAny)
    {
        MissileBlinkPeriod = 0.1f;
        return;
    }
    if (bToggled)
        PlayOrRewindUiSoundSlot(0x19);
    double const Period = std::sqrt(static_cast<double>(Nearest)) * 0.001;
    MissileBlinkPeriod = static_cast<float>(Period);
    if (Period < static_cast<double>(0.1f))
        MissileBlinkPeriod = 0.1f;
}

void HudTargetableObjects::UpdateTrailAlert()
{
    // . 0x004f1803..0x004f1858: the blink timer (0.5, the field) -- below it
    // nothing happens; at or past the period (TEST AH,1 / JNZ keeps < or unordered) it restarts
    // with the period 0.5 and the phase flips.
    double const Timer =
        GetClientFrameTimeCapped() + static_cast<double>(TrailBlinkTimer);
    TrailBlinkTimer = static_cast<float>(Timer);
    if (!NotBelow(Timer, 0.5f))
        return;
    if (!(Timer < static_cast<double>(TrailBlinkPeriod)))
    {
        TrailBlinkTimer = 0.0f;
        TrailBlinkPeriod = 0.5f;
        bTrailBlinkOn = (bTrailBlinkOn == 0) ? 1 : 0;
    }

    // shown while the player's ship has a behaviour manager whose
    // closest trailing ship is nearer than 500 (the field; ordered <) and the phase is off.
    uintptr_t bHide = 1;
    CEqObj* const PlayerShip =
        static_cast<CEqObj*>(PlayerObjectOfClass(static_cast<IObjRW*>(GetPlayerObjRW()), 0x503u));
    if (PlayerShip != nullptr)
    {
        IBehaviorManager* const Behavior = PlayerShip->get_behavior_interface();
        if (Behavior != nullptr && Behavior->get_closest_trailing_ship() < 500.0f &&
            bTrailBlinkOn == 0)
        {
            bHide = 0;
        }
    }
    TrailAlert->SetProperty(1, bHide, 0);   // slot 42
}

void HudTargetableObjects::UpdateLagAlert()
{
    // . Single player: nothing. 0x004f1b51..0x004f1b7a: once a second (the field;
    // below it nothing happens) the timer loses a second.
    if (SinglePlayer())
        return;
    double const Timer =
        GetClientFrameTimeCapped() + static_cast<double>(LagTimer);
    LagTimer = static_cast<float>(Timer);
    if (!NotBelow(Timer, 1.0f))
        return;
    LagTimer = static_cast<float>(Timer - 1.0);

    // lit above 600 ms; above 1200 ms red (the field) and blinking,
    // otherwise amber (the field) and steady.
    bool const bSlow = CDPClient::GetPingDelay() > 0x258;
    if (CDPClient::GetPingDelay() > 0x4b0)
    {
        bLagBlinkOn = (bLagBlinkOn == 0) ? 1 : 0;
        uintptr_t const Red = reinterpret_cast<uintptr_t>(&G_NnPaletteArgb_679ba8);
        LagAlert->SetProperty(0x18, Red, Red);
    }
    else
    {
        uintptr_t const Amber = reinterpret_cast<uintptr_t>(&G_NnPaletteArgb_679ba4);
        bLagBlinkOn = 0;
        LagAlert->SetProperty(0x18, Amber, Amber);
    }
    // .
    uintptr_t const bHide = (bSlow && bLagBlinkOn == 0) ? 0u : 1u;
    LagAlert->SetProperty(1, bHide, 0);
}

namespace
{
    // The static HUDFACTORY<HUD_Targetable_Objects> built by the routine (XC entry 0x004f27aa);
    // its Create the routine is HUDFACTORY<T>::Create (operator new(0x93c) + the routine).
    HUDFACTORY<HudTargetableObjects> GTargetableFactory("Targetable_Objects");
}

namespace
{
    // ..the field -- the voice-line ids the static initialisers the branch ..
    // hash with CreateID.
    unsigned const G_VoiceAnomalyDetected = CreateID("anomaly_detected");
    unsigned const G_VoiceDanger = CreateID("danger");
    unsigned const G_VoiceRadiationDamage = CreateID("radiation_damage_detected");
    unsigned const G_VoiceMinesDetected = CreateID("mines_detected");
    unsigned const G_VoiceVolatileGases = CreateID("volatile_gases_detected");
    unsigned const G_VoiceWarning = CreateID("warning");
    unsigned const G_VoiceSensorEfficiency = CreateID("sensor_efficiency_reduced");
    unsigned const G_VoiceIncreasedDrag = CreateID("increased_drag_detected");

    // , static initialiser the branch: {0, 0, 2.0f (the field)}.
    FHudHazardWarning G_HudHazardWarning = {0, 0, 2.0f};

    // .
    bool G_bHudRadiationZoneWarning = false;

    // (Id, 0) -- the NN voice queue (nn_messaging.cpp).
    void QueueHazardVoiceLine(unsigned VoiceId)
    {
        NnVoiceEnqueue(VoiceId, 0);
    }

    // The inlined `+0xc (CBase) -> +4 (Object)` chain with a class test, null anywhere -> null.
    CObject* HazardObjectOfClass(FClientGameobj* Gameobj, unsigned Mask)
    {
        CBase* const Base = static_cast<IObjInspectImpl*>(Gameobj);
        if (Base == nullptr)
            return nullptr;
        CObject* const Object = Base->cobject();
        if (Object == nullptr)
            return nullptr;
        if ((static_cast<unsigned>(Object->ObjectClass) & Mask) != Mask)
            return nullptr;
        return Object;
    }

}

void FHudHazardWarning::Update(float DeltaSeconds)
{
    // .
    FClientGameobj* const Player = static_cast<FClientGameobj*>(GetPlayerObjRW());
    // no player, a player whose slot +0x158 answers true, or a CSimple
    // (class & 3) at zero relative health (FCOMP the field = 0.0) -> reset.
    bool bReset = (Player == nullptr);
    if (!bReset && Player->Slot158())
        bReset = true;
    if (!bReset)
    {
        CSimple const* const Simple = static_cast<CSimple const*>(HazardObjectOfClass(Player, 3));
        if (Simple->get_relative_health() == 0.0f)
            bReset = true;
    }
    if (bReset)
    {
        // .
        MineId = 0;
        ZoneFlags = 0;
        Timer = 2.0f;
        G_bHudRadiationZoneWarning = false;
        return;
    }

    // the player's position (slot 0).
    Vector const& ShipPos = Player->get_position();

    // the ship's scanner (CEqObj +0xe4 FindFirst(0x80000), used as the
    // CEScanner it is, with no cast check). Any scanned object of type 0x800 closer than 2000 m
    // (length squared < the field = 4000000.0f) counts; the first new such id is announced.
    bool bMineInRange = false;
    CShip* const ShipForScan = static_cast<CShip*>(HazardObjectOfClass(Player, 0x503));
    CEScanner const* const Scanner = static_cast<CEScanner const*>(
        ShipForScan->EquipManager.FindFirst(EquipTypeScanner));
    if (Scanner != nullptr)
    {
        for (int Index = 0; Index < Scanner->Scans.Count; ++Index)
        {
            Watchable const* const Watched = Scanner->Scans.Entries[Index].target();
            if (Watched == nullptr)
                continue;
            IObjRW* const Candidate =
                ObjRwFromWatchable(const_cast<void*>(static_cast<void const*>(Watched)));
            if (Candidate == nullptr)
                continue;
            unsigned Type = 0;
            if (Candidate->get_type(Type) != 0 || Type != 0x800u)
                continue;
            Vector const& Pos = Candidate->get_position();
            // the float differences, then the routine builds the vector.
            float const Dx = Pos.x - ShipPos.x;
            float const Dy = Pos.y - ShipPos.y;
            float const Dz = Pos.z - ShipPos.z;
            // z*z + y*y + x*x on the x87 stack.
            double const LengthSq = static_cast<double>(Dz) * Dz + static_cast<double>(Dy) * Dy +
                                    static_cast<double>(Dx) * Dx;
            if (!(LengthSq < 4000000.0))
                continue;
            bMineInRange = true;
            unsigned const Id = Candidate->get_id();
            if (MineId != Id)
            {
                MineId = Id;
                QueueHazardVoiceLine(G_VoiceAnomalyDetected);
            }
        }
    }
    if (!bMineInRange)
        MineId = 0;

    // nothing more while the ship rides a trade lane (no reset either).
    CShip* const Ship = static_cast<CShip*>(HazardObjectOfClass(Player, 0x503));
    if (Ship->is_using_tradelane())
        return;

    // the zone sample runs every 2 seconds (the field). FST stores the
    // rounded sum; FCOMP tests the unrounded one, and only an ordered >= 2.0 goes on.
    double const Elapsed = static_cast<double>(DeltaSeconds) + static_cast<double>(Timer);
    Timer = static_cast<float>(Elapsed);
    if (!(Elapsed >= 2.0))
        return;

    // the zone mask of a 25 m sphere (the field) about the ship, from the
    // client ObjDB's zone database (the field vt+8, then ISystem vt+0x20).
    Universe::ISystem const* const Zones =
        static_cast<Universe::ISystem const*>(GetClientObjDb()->GetZoneDb());
    Geometry::Sphere Query;
    Query.Center = ShipPos;
    Query.Radius = 25.0f;
    unsigned const NewFlags = Zones->GetSphereProperties(&Query);

    if (NewFlags != ZoneFlags)
    {
        // CShip +0x218: the atmosphere the ship is inside (CShip::AtmosphereSolar).
        bool const bShipHullOk = (Ship != nullptr && Ship->AtmosphereSolar == nullptr);
        // .
        unsigned const Radiation = NewFlags & 0x40000u;
        if (Radiation != 0)
        {
            if (bShipHullOk)
                G_bHudRadiationZoneWarning = true;
        }
        else
        {
            G_bHudRadiationZoneWarning = false;
        }

        // one announcement per newly entered hazard, first match wins.
        if (Radiation != 0 && (ZoneFlags & 0x40000u) == 0)
        {
            if (bShipHullOk)
            {
                QueueHazardVoiceLine(G_VoiceDanger);
                QueueHazardVoiceLine(G_VoiceRadiationDamage);
            }
        }
        else if ((NewFlags & 0x1000u) != 0 && (ZoneFlags & 0x1000u) == 0)
        {
            QueueHazardVoiceLine(G_VoiceDanger);
            QueueHazardVoiceLine(G_VoiceMinesDetected);
        }
        else if ((NewFlags & 0x4000u) != 0 && (ZoneFlags & 0x4000u) == 0)
        {
            QueueHazardVoiceLine(G_VoiceDanger);
            QueueHazardVoiceLine(G_VoiceVolatileGases);
        }
        else if ((NewFlags & 0x100000u) != 0 && (ZoneFlags & 0x100000u) == 0)
        {
            QueueHazardVoiceLine(G_VoiceWarning);
            QueueHazardVoiceLine(G_VoiceSensorEfficiency);
        }
        else if ((NewFlags & 0x80000u) != 0 && (ZoneFlags & 0x80000u) == 0)
        {
            QueueHazardVoiceLine(G_VoiceWarning);
            QueueHazardVoiceLine(G_VoiceIncreasedDrag);
        }
    }

    // .
    ZoneFlags = NewFlags;
    Timer = 0.0f;
}

void UpdateHudHazardWarning(float DeltaSeconds)
{
    // .
    G_HudHazardWarning.Update(DeltaSeconds);
}

bool IsHudRadiationZoneWarningOn()
{
    // .
    return G_bHudRadiationZoneWarning;
}
