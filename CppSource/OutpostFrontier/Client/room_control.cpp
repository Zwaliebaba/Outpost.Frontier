// Author: Giperion.

#include "room_control.h"
#ifdef OUTPOST_FRONTIER_WALK_MODE
#include "base_walk.h"
#endif

#include "particlefx.h"
#include "anim_script_set.h"
#include "character_select_equip.h" // GetPlayerShipArchetypeIdbaseline / GetPlayerEquipDescListbaseline
#include "client_base_market.h"     // FClientMarket, GetClientLocationBaseMarket (the routine)
#include "fade_to_black.h"          // SetFadeImmediate (the routine), SetFadeTarget (the routine)
#include "flash_light.h"
#include "gameflow.h"               // HoldGameflowTime / ClearHoveredHotspot / UnlockHotspotInput
#include "input_manager.h"          // MouseSetCursor (the routine), SetClientMouseCursorDrawFlag
#include "space_letterbox.h"        // SetLetterboxTarget (the routine)
#include "object_prop.h"
#include "game_options.h"
#include "player_spawn.h"       // GetPlayerConnectionId (the field)
#include "server_connect.h"     // GetLocalServerInterface (the field)
#include "gf_script.h"
#include "gf_character_object.h"
#include "gf_object_ship.h"
#include "gf_script_object.h"
#include "menu_universe_base.h"
#include "mirror_property_bucket.h"
#include "nn_main_button.h"     // G_bNnKeepPanelOpen (the field)
#include "room_camera.h"
#include "room_control_hotspot.h" // RoomButton family (Client\Gf\RoomControlHotspot.cpp)
#include "client_cliobj.h"
#include "engine_light.h"
#include "perf_options.h"
#include "win3d_immediate.h"
#include "win3d_render.h"

#include "NeuronRuntime.h"
#include "common_archetype.h"   // Archetype::GetShip
#include "common_csys.h"
#include "x86math.h"            // I3DMathEngine (the room camera move's quaternions)
#include "common_goods.h"
#include "common_id.h"          // MakeLocationID (the routine's callee)
#include "common_iface.h"
#include "common_misc.h"
#include "common_newchar.h"
#include "common_server.h"      // IServer
#include "neuroncomponents.h"
#include "engine/sceneruntime.h"
#include "render/IFLAppearance.h"
#include "render/IRenderPipeline.h"
#include "render/rendcomp.h"
#include "render/shading.h"

#include <list>
#include <math.h>
#include <new>
#include <set>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

namespace
{
    // .rdata 0x005cbf24 -- the source path RoomControl.cpp hands FDUMP and the routine.
    char const kRoomControlCpp[] = "Client\\gf\\RoomControl.cpp";


    // (RoomObject init): the routine packs RoomData's camera
    // frame (+0x1d8 orientation, +0x1cc position) into one transform for the routine, then
    // / 556e00 / 556f10 take +0x1fc / +0x200 / +0x204 as they are.
    void InstallRoomDataCamera(FRoomCamera* Camera, RoomCameraInfo const& Info)
    {
        float Transform[12];
        memcpy(Transform, &Info.Frame.Orient, sizeof(float) * 9);
        memcpy(Transform + 9, &Info.Frame.Pos, sizeof(float) * 3);
        RoomCameraSetTransform(Camera, Transform);
        RoomCameraSetHalfFovX(Camera, Info.Fov);
        RoomCameraSetZNear(Camera, Info.NearPlane);
        RoomCameraSetZFar(Camera, Info.FarPlane);
    }

}

// ---------------------------------------------------------------------------
// RoomControl subobjects — listing-checked ctors only (pass 3).
// ---------------------------------------------------------------------------

// Runtime+0x4C is the named-script table FAnimScriptSet (anim_script_set.h, the routine ..
// ).

// Runtime+0x504 is the character placement vector (FRoomCharPlacement, room_control.h): a VC6
// std::vector of 8-byte elements, built empty by the routine (the allocator byte is an
// uninitialised stack byte there), filled by the routine and freed by the routine. The port
// keeps a std::vector, 0x10 bytes in the iterator-debug builds like baseline's.

// One ship pad of the room (0x10 bytes): a RoomData ship placement (+0x184, both
// [PlayerShipPlacement] and [ForSaleShipPlacement]) and the ship standing on it.
struct FRoomShipPad
{
    // (thiscall) -- each ship deleted through its slot 0 (with 1) and cleared.
    void Clear();
    // (thiscall) -- the player's ship (Gf ObjectShip, operator new(0xb0)) on this
    // pad: the pad's frame, shadow on, the pad's y, its launching script, the player's ship
    // archetype and equipment list (the routine / the routine).
    void CreatePlayerShip();
    // (thiscall, RET 4) -- a DealerShipObject (operator new(0x64)) of the ship
    // package GoodId on this pad: its hull's ship archetype and the package's addon list.
    void CreateForSaleShip(unsigned GoodId);

    FRoomRuntime* Room;        // +0x00
    ShipPlaceInfo* Place;      // +0x04
    FObjectShip* Ship;         // +0x08
    FDealerShipObject* ForSale; // +0x0c
};

// The room's ship-pad table at Runtime+0x1020 (0x18 bytes: a VC6 vector of FRoomShipPad, then
// two ints). Built by the routine at the room load, destroyed by the routine.
struct FRoomShipPadTable
{
    FRoomShipPadTable();
    // every pad cleared (the routine inlined), the vector freed. The runtime's
    // destructor calls it as the member destructor (0x0044965d, EH state 3); the routine is
    // the same teardown on the constructor's unwind path.
    ~FRoomShipPadTable();

    // +0x00 the allocator byte, +0x04..+0x10 = 0, +0x14 = -1.
    void Init();
    // (thiscall, RET 8) -- one pad per RoomData ship placement; the first free
    // player pad gets the player's ship (with a ship archetype and "player_ship" enabled); each
    // ship package the market sells (category 3) goes on the next free for-sale pad.
    void Build(FRoomRuntime* Runtime, FClientMarket* Market);
    // the player pad has a landing script.
    bool HasLandingScript() const;
    // with the field, a player pad, a landing script and a ship there: the
    // ship plays the landing script; true then.
    bool PlayLanding();

    std::vector<FRoomShipPad> Pads; // +0x00 allocator byte, begin +0x04, end +0x08, cap +0x0c
    int Count;                      // +0x10 the placement count
    int PlayerPad;                  // +0x14 the pad of the player's ship, -1 = none

private:
    FRoomShipPadTable(FRoomShipPadTable const&) = delete;
    FRoomShipPadTable& operator=(FRoomShipPadTable const&) = delete;
};

#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL != 0
static_assert(sizeof(FRoomShipPadTable) == 0x18, "FRoomShipPadTable sizeof 0x18");
#endif
static_assert(sizeof(FRoomShipPad) == 0x10, "FRoomShipPad sizeof 0x10");

// object (RoomControl). The RoomObject base (+0x00..+0x2f) is
// FRoomObject; RoomControl registers itself with a null owner (the routine(0) at 0x00449282),
// so no room's draw ever visits it. The Event::Handler face follows at +0x30: vftable
// , stored by the routine at 0x004492c5 over the base's 0x005cbed8 (0x0044928b);
// registered for id 0xf by the routine at 0x00449878; torn down by the routine at 0x00449686.
struct FRoomRuntime : public FRoomObject, public EventHandler
{
    FRoomRuntime();
    ~FRoomRuntime() override;

    // Slot 5 is the branch in RoomControl's vtable 0x005cbea0; slots 1-4 and 6-12 are the
    // RoomObject ones.
    void Update(double DeltaSeconds) override;

    // Event::Handler slot 0 of the +0x30 face (0x005cbe98).
    bool OnEvent(unsigned MsgId) override;

    unsigned BaseId;                   // +0x34 the routine 0x004496c8 (argument 1)
    unsigned RoomId;                    // +0x38 the routine 0x004496bf (argument 2)
    void* LocationBase;                 // +0x3C BaseData* of BaseId, the routine 0x004496d9 (the routine)
    RoomData* Room;                     // +0x40 the routine 0x004496e6 (the routine)
    // +0x44 / +0x48: -1 from the routine (0x004496e9 / 0x004496ec), then the routine takes
    // them from the prop whose RoomPropInfo +0x3f is set: its slot-6 engine instance and its
    // script table's +0x0c script.
    void* MainInstance;                 // +0x44 RoomData::fixup argument (0x004496fb)
    unsigned long MainScript;           // +0x48 the routine on +0x4c when not -1 (0x0044970c)
    FAnimScriptSet Scripts;             // +0x4C the routine (0x18) → +0x64
    // +0x64: the playback of each room animation (the routine), -1 when it has none.
    unsigned long Handles64[0x20];      // +0x64 STOSD 0xffffffff × 0x20
    // +0xE4: one the routine wrapper per RoomData+0x144 light (the routine 0x00449719..).
    FGfLightWrapper* RoomLights[0x80];
    int RoomLightCount;                 // +0x2E4
    // +0x2E8: the per-frame light list the routine builds (at most 0x80).
    FGfLightWrapper* FrameLights[0x80];
    int FrameLightCount;                // +0x4E8
    float FrameAmbient[3];              // +0x4EC RoomData+0xd8..+0xe0, or 0 in single-light mode
    int SingleLightIndex;               // +0x4F8 index into RoomLights for single-light mode
    unsigned char bSingleLight;         // +0x4FC
    unsigned char Pad4FD[3];            // +0x4FD .. +0x4FF
    FRoomCamera* Camera;                // +0x500 the routine
    std::vector<FRoomCharPlacement> Placements; // +0x504 the routine (0x10) → +0x514
    // +0x514 scene scripts started (the routine), +0x515 a start script runs, +0x516 the
    // character start script requested again (the routine), +0x517 a character script ran.
    unsigned char Flags514[4];          // +0x514 .. +0x517
    FCharacterObject* Slot518[0x40];    // +0x518 the room's characters (the routine)
    // +0x618: the room's props -- the routine appends one Gf::ObjectProp per enabled
    // RoomData+0x158 record, the routine appends one more, the routine finds one by name.
    FObjectProp* Props[0x80];           // +0x618
    int PropCount;                      // +0x818
    // +0x81C: the room's FlashLight objects (the routine fills them from the RoomData
    // FlashLightSet records); the routine hands them to the routine.
    FFlashLight* Flashlights[0x200];    // +0x81C
    int FlashlightCount;                // +0x101C
    FRoomShipPadTable ShipPads;         // +0x1020 the routine (0x18) → +0x1038
    // +0x1038 / +0x108c / +0x10e0: the room's "scene =" scripts the routine collects by type
    // (1 ambient, 2 traffic, 3 once), malloc'd paths, each with a flag byte (1 for the room's own
    // records) and a count. No bound check in the routine.
    char* SceneAmbient[0x10];           // +0x1038
    unsigned char SceneAmbientFlag[0x10]; // +0x1078
    int SceneAmbientCount;              // +0x1088
    char* SceneTraffic[0x10];           // +0x108C
    unsigned char SceneTrafficFlag[0x10]; // +0x10CC
    int SceneTrafficCount;              // +0x10DC
    char* SceneOnce[0x10];              // +0x10E0
    unsigned char SceneOnceFlag[0x10];  // +0x1120
    int SceneOnceCount;                 // +0x1130
    // +0x1134 the room buttons the routine builds from the RoomData hotspots (one per
    // hotspot whose behavior is 1..6), count +0x1334 (room_control_hotspot.h).
    RoomButton* Controllers[0x80];      // +0x1134
    int ControllerCount;                // +0x1334
    // The room camera move (the routine starts it, the routine runs it, the routine /
    // stop it). To* is where the camera goes (from a RoomData camera), From* where
    // it was when the move started.
    unsigned char bCameraMoving;        // +0x1338
    unsigned char Pad1339[3];
    float CameraMoveDuration;           // +0x133c 2.0f (0x0044baea)
    // +0x1340 -- NavBar stores it before a move (0x004417e1 / 0x0044185c / 0x004419a2 = 1,
    // = 0); the routine ends a move at once when it and the skip latch are set.
    unsigned char bCameraMoveSkippable; // +0x1340
    unsigned char Pad1341[3];
    float CameraMoveElapsed;            // +0x1344
    FQuaternion CameraToQuat;           // +0x1348
    float CameraToPos[3];               // +0x1358
    float CameraToHalfFovX;             // +0x1364
    float CameraToZNear;                // +0x1368
    float CameraToZFar;                 // +0x136c
    FQuaternion CameraFromQuat;         // +0x1370
    float CameraFromPos[3];             // +0x1380
    float CameraFromHalfFovX;           // +0x138c
    float CameraFromZNear;              // +0x1390
    float CameraFromZFar;               // +0x1394
    FRoomCameraDoneCallback CameraDone; // +0x1398
    void* CameraDoneA;                  // +0x139c
    void* CameraDoneB;                  // +0x13a0
    // +0x13a4 -- the routine's dword ("Camera_Track_%d" step; stored as a dword at 0x0044bff3
    // / 0x0044c08f / 0x0044c128). the routine is not ported (HOLE).
    int Field_0x13a4;                   // +0x13a4
    // +0x13a8 -- a dword the routine (0x0044cec8) and the routine (0x0044cef0) zero; no
    // [reg+0x13a8] read anywhere in the listing. TODO: meaning not recovered.
    int Field_0x13a8;                   // +0x13a8
};

static_assert(offsetof(FRoomObject, bNoUpdate) == 0x04, "RoomObject +0x04");
static_assert(offsetof(FRoomObject, Owner) == 0x08, "RoomObject owner +0x08");
static_assert(offsetof(FRoomObject, AmbientSource) == 0x0C, "RoomObject +0x0c");
static_assert(offsetof(FRoomObject, bVisible) == 0x10, "RoomObject +0x10");
static_assert(offsetof(FRoomObject, Layer) == 0x16, "RoomObject layer +0x16");
static_assert(offsetof(FRoomObject, RenderMode) == 0x18, "RoomObject +0x18");
static_assert(offsetof(FRoomObject, Float1C) == 0x1C, "RoomObject +0x1c");
static_assert(offsetof(FRoomObject, MaterialEntries) == 0x20, "RoomObject vector +0x20");

// The offsets below are baseline's. They hold when std::vector is 0x10 bytes (the iterator-debug
// builds, like baseline's VC6 vector with its allocator byte); a release build's 0x0c-byte vector
// shifts everything past +0x2c by four, which nothing in the port reads by raw offset.
#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL != 0
static_assert(sizeof(FRoomObject) == 0x30, "RoomObject sizeof 0x30");
static_assert(offsetof(FRoomRuntime, BaseId) == 0x34, "FRoomRuntime +0x34");
static_assert(offsetof(FRoomRuntime, MainInstance) == 0x44, "FRoomRuntime +0x44");
static_assert(offsetof(FRoomRuntime, MainScript) == 0x48, "FRoomRuntime +0x48");
static_assert(offsetof(FRoomRuntime, Scripts) == 0x4C, "FRoomRuntime +0x4C");
static_assert(offsetof(FRoomRuntime, Handles64) == 0x64, "FRoomRuntime +0x64");
static_assert(offsetof(FRoomRuntime, Room) == 0x40, "FRoomRuntime +0x40");
static_assert(offsetof(FRoomRuntime, RoomLights) == 0xE4, "FRoomRuntime +0xE4");
static_assert(offsetof(FRoomRuntime, RoomLightCount) == 0x2E4, "FRoomRuntime +0x2E4");
static_assert(offsetof(FRoomRuntime, FrameLights) == 0x2E8, "FRoomRuntime +0x2E8");
static_assert(offsetof(FRoomRuntime, FrameLightCount) == 0x4E8, "FRoomRuntime +0x4E8");
static_assert(offsetof(FRoomRuntime, FrameAmbient) == 0x4EC, "FRoomRuntime +0x4EC");
static_assert(offsetof(FRoomRuntime, SingleLightIndex) == 0x4F8, "FRoomRuntime +0x4F8");
static_assert(offsetof(FRoomRuntime, bSingleLight) == 0x4FC, "FRoomRuntime +0x4FC");
static_assert(offsetof(FRoomRuntime, Camera) == 0x500, "FRoomRuntime Camera +0x500");
static_assert(offsetof(FRoomRuntime, Placements) == 0x504, "FRoomRuntime +0x504");
static_assert(offsetof(FRoomRuntime, Slot518) == 0x518, "FRoomRuntime +0x518");
static_assert(offsetof(FRoomRuntime, Props) == 0x618, "FRoomRuntime +0x618");
static_assert(offsetof(FRoomRuntime, PropCount) == 0x818, "FRoomRuntime +0x818");
static_assert(offsetof(FRoomRuntime, Flashlights) == 0x81C, "FRoomRuntime +0x81C");
static_assert(offsetof(FRoomRuntime, FlashlightCount) == 0x101C, "FRoomRuntime +0x101C");
static_assert(offsetof(FRoomRuntime, ShipPads) == 0x1020, "FRoomRuntime +0x1020");
static_assert(offsetof(FRoomRuntime, SceneAmbient) == 0x1038, "FRoomRuntime +0x1038");
static_assert(offsetof(FRoomRuntime, SceneTraffic) == 0x108C, "FRoomRuntime +0x108C");
static_assert(offsetof(FRoomRuntime, SceneOnce) == 0x10E0, "FRoomRuntime +0x10E0");
static_assert(offsetof(FRoomRuntime, SceneOnceCount) == 0x1130, "FRoomRuntime +0x1130");
static_assert(offsetof(FRoomRuntime, ControllerCount) == 0x1334, "FRoomRuntime +0x1334");
static_assert(offsetof(FRoomRuntime, bCameraMoving) == 0x1338, "FRoomRuntime +0x1338");
static_assert(offsetof(FRoomRuntime, bCameraMoveSkippable) == 0x1340, "FRoomRuntime +0x1340");
static_assert(offsetof(FRoomRuntime, CameraToQuat) == 0x1348, "FRoomRuntime +0x1348");
static_assert(offsetof(FRoomRuntime, CameraFromQuat) == 0x1370, "FRoomRuntime +0x1370");
static_assert(offsetof(FRoomRuntime, CameraDone) == 0x1398, "FRoomRuntime +0x1398");
static_assert(offsetof(FRoomRuntime, Field_0x13a4) == 0x13a4, "FRoomRuntime +0x13a4");
static_assert(sizeof(FRoomRuntime) == 0x13AC, "FRoomRuntime sizeof 0x13AC");
#endif

// (list head the field, size the field) -- every RoomObject, in
// construction order.
namespace
{
    std::list<FRoomObject*> G_RoomObjects;
}

std::list<FRoomObject*>& GetRoomObjectList()
{
    // .
    return G_RoomObjects;
}

FRoomObject::FRoomObject(FRoomRuntime* InOwner)
    : bNoUpdate(0),
      Owner(InOwner),
      AmbientSource(nullptr),
      bVisible(1),
      bLit(1),
      bFog(1),
      bEnvMap(0),
      Flag14(0),
      LightGroup(0),
      Layer(0),
      RenderMode(0),
      Float1C(0.0f),
      MaterialEntries()
{
    // 0x005593f6..0x0055942f: the fields above (the vector's allocator byte at
    // +0x20 takes the argument's low byte, which only VC6's allocator stored). 0x00559432..
    // append to the field and bump the field.
    G_RoomObjects.push_back(this);
}

FRoomObject::~FRoomObject()
{
    // 0x005594dc..0x00559504: release every material entry through the routine,
    // re-reading the size each pass.
    size_t Index = 0;
    for (Index = 0; Index < MaterialEntries.size(); ++Index)
        Archetype::ReleaseMaterialEntrySlot(&MaterialEntries[Index]);
    MaterialEntries.clear();

    // the first node that holds this object is unlinked and freed.
    std::list<FRoomObject*>::iterator It = G_RoomObjects.begin();
    for (; It != G_RoomObjects.end(); ++It)
    {
        if (*It == this)
        {
            G_RoomObjects.erase(It);
            break;
        }
    }
    // the vector's buffer is freed by the member destructor.
}

bool FRoomObject::Slot1()
{
    // XOR AL,AL.
    return false;
}

bool FRoomObject::IsVisible()
{
    // .
    return bVisible != 0;
}

void FRoomObject::SetFloat1C(float Value)
{
    // .
    Float1C = Value;
}

float FRoomObject::GetFloat1C()
{
    // .
    return Float1C;
}

void* FRoomObject::GetEngineInstance()
{
    // OR EAX,-1.
    return kRoomObjectNoInstance;
}

void FRoomObject::DrawOpaque(ICamera* /*Camera*/)
{
    // IDPMsgHandler::OnAddHost (0x0053e4a0): RET 4.
}

void FRoomObject::DrawTranslucent(ICamera* /*Camera*/)
{
    // IDPMsgHandler::OnAddHost (0x0053e4a0): RET 4.
}

void FRoomObject::DrawEffects(ICamera* /*Camera*/)
{
    // IDPMsgHandler::OnAddHost (0x0053e4a0): RET 4.
}

void FRoomObject::Slot10(unsigned long /*Cookie*/, int const* /*Point*/)
{
    // IDPMsgHandler::OnReceive (0x0053aaa0): RET 8.
}

void FRoomObject::Slot11(unsigned long /*Cookie*/, int const* /*Point*/)
{
    // IDPMsgHandler::OnReceive (0x0053aaa0): RET 8.
}

void FRoomObject::Slot12(unsigned long /*Cookie*/, int const* /*Point*/)
{
    // IDPMsgHandler::OnReceive (0x0053aaa0): RET 8.
}

// ---------------------------------------------------------------------------
// The character placements (+0x504) and the characters (+0x518).
// ---------------------------------------------------------------------------

// (thiscall on the vector, RET 4) -- the room's placements whose posture is not 3
// (special), counted, the vector grown to hold them, then each appended {placement, null}.
static void BuildCharacterPlacements(std::vector<FRoomCharPlacement>& Placements,
                                     std::vector<CharPlaceInfo*> const& Source)
{
    // the count.
    int Wanted = 0;
    int Index = 0;
    for (Index = 0; Index < static_cast<int>(Source.size()); ++Index)
    {
        if (Source[Index]->Posture != 3)
            ++Wanted;
    }
    // capacity below the count -> reallocate to exactly that many.
    if (Placements.capacity() < static_cast<size_t>(Wanted))
        Placements.reserve(static_cast<size_t>(Wanted));
    // the appends (the grow paths the routine / the routine are
    // vector::insert's).
    for (Index = 0; Index < static_cast<int>(Source.size()); ++Index)
    {
        CharPlaceInfo* const Info = Source[Index];
        if (Info->Posture == 3)
            continue;
        FRoomCharPlacement Placement;
        Placement.Info = Info;
        Placement.Character = nullptr;
        Placements.push_back(Placement);
    }
}

// (cdecl) -- a costume worth drawing: any of the four part ids, or accessories.
static bool CostumeHasParts(Costume const& Cost)
{
    bool bHas = false;
    if (Cost.PartIds[0] != 0)
        bHas = true;
    if (Cost.PartIds[1] != 0)
        bHas = true;
    if (Cost.PartIds[2] != 0)
        bHas = true;
    if (Cost.PartIds[3] != 0)
        bHas = true;
    if (static_cast<int>(Cost.AccessoryCount) > 0)
        bHas = true;
    return bHas;
}

// (thiscall on the vector, RET 8) -- a character for Props on a free placement:
// the one named by the record (+0x18), else for a pc / rpc record (a foreign session makes it
// rpc, 2) the first of that char type (+0x34) and posture (+0x38 == record +0x20); a free
// placement found -> the routine with it (bScriptSpatial 0), stored in the element. A record
// of another kind gets a character with no placement (the routine: index -1, identity frame,
// bScriptSpatial 1). No placement found -> null.
static FCharacterObject* CreatePlacedCharacter(std::vector<FRoomCharPlacement>& Placements,
                                               CharacterProperties const& Props,
                                               FRoomRuntime* Room)
{
    // .
    int CharType = static_cast<int>(Props.CharType);
    if (Props.SessionId != 0 && Props.SessionId != GetPlayerConnectionId())
        CharType = 2;

    int Found = -1;
    int Index = 0;
    if (Props.NameAnsi != nullptr && Props.NameAnsi[0] != '\0')
    {
        // .
        for (Index = 0; Index < static_cast<int>(Placements.size()); ++Index)
        {
            if (Placements[Index].Character != nullptr)
                continue;
            if (Placements[Index].Info->is_named(Props.NameAnsi))
            {
                Found = Index;
                break;
            }
        }
    }
    else if (CharType == 1 || CharType == 2)
    {
        // .
        for (Index = 0; Index < static_cast<int>(Placements.size()); ++Index)
        {
            if (Placements[Index].Character != nullptr)
                continue;
            CharPlaceInfo const* const Info = Placements[Index].Info;
            if (CharType == Info->CharType && static_cast<int>(Props.Posture) == Info->Posture)
            {
                Found = Index;
                break;
            }
        }
    }
    else
    {
        // (a tail jump from 0x004359d7): no placement, the identity frame.
        Csys Frame;
        Frame.Pos.x = 0.0f;
        Frame.Pos.y = 0.0f;
        Frame.Pos.z = 0.0f;
        MatrixSetIdentity(Frame.Orient);
        return CreateCharacterObject(Props, nullptr, -1, Room, 1, Frame);
    }

    // (the -1 test is baseline's own).
    if (Found == -1)
        return nullptr;
    CharPlaceInfo* const Info = Placements[Found].Info;
    Csys const Frame = Info->Frame;
    FCharacterObject* const Character = CreateCharacterObject(Props, Info, Found, Room, 0, Frame);
    Placements[Found].Character = Character;
    return Character;
}

// (thiscall, RET 4) -- character Slot deleted: its placement freed
// (the routine on +0x504), the object through its deleting destructor, the slot cleared.
static void RoomControlDeleteCharacter(FRoomRuntime* Runtime, int Slot);

// (thiscall) -- the +0x1134 buttons (REP STOSD 0x80 dwords), +0x1334 and +0x13a8
// zeroed. Caller: the ctor the routine (0x0044938a).
static void RoomControlClearButtons(FRoomRuntime* Runtime)
{
    int Index = 0;
    for (Index = 0; Index < 0x80; ++Index)
        Runtime->Controllers[Index] = nullptr;
    Runtime->ControllerCount = 0;
    Runtime->Field_0x13a8 = 0;
}

// (thiscall) -- every non-null +0x1134 button deleted (slot 0 with 1) and its slot
// cleared; the count (+0x1334, re-read each pass) is left as it is. Caller: the dtor
// (0x004494e6).
static void RoomControlDeleteButtons(FRoomRuntime* Runtime)
{
    int Index = 0;
    for (Index = 0; Index < Runtime->ControllerCount; ++Index)
    {
        if (Runtime->Controllers[Index] != nullptr)
        {
            delete Runtime->Controllers[Index];
            Runtime->Controllers[Index] = nullptr;
        }
    }
}

FRoomRuntime::FRoomRuntime()
    : FRoomObject(nullptr)
{
    // . 0x0044927d..0x00449282: the RoomObject base with a null owner.
    // the routine / the routine (the Placements member) /
    // .
    Scripts.Init();
    ShipPads.Init();

    // +0x34..+0x40 = 0. +0x44 / +0x48 are not written here; the
    // zero-filled block CreateRoomRuntime hands in leaves them 0 until the routine sets -1.
    BaseId = 0;
    RoomId = 0;
    LocationBase = nullptr;
    Room = nullptr;

    // +0x64, 0x20 dwords of -1.
    unsigned Index = 0;
    for (Index = 0; Index < 0x20; ++Index)
        Handles64[Index] = 0xffffffffu;

    // both light arrays and their counts.
    RoomLightCount = 0;
    for (Index = 0; Index < 0x80; ++Index)
        RoomLights[Index] = nullptr;
    FrameLightCount = 0;
    for (Index = 0; Index < 0x80; ++Index)
        FrameLights[Index] = nullptr;

    // the frame ambient, then the routine (the room fog state).
    FrameAmbient[0] = 0.0f;
    FrameAmbient[1] = 0.0f;
    FrameAmbient[2] = 0.0f;
    GfFogReset();

    // .
    SingleLightIndex = 0;
    bSingleLight = 0;
    Camera = nullptr;
    Flags514[0] = 0;
    Flags514[1] = 0;
    Flags514[2] = 0;
    Flags514[3] = 0;
    for (Index = 0; Index < 0x40; ++Index)
        Slot518[Index] = nullptr;
    PropCount = 0;
    for (Index = 0; Index < 0x80; ++Index)
        Props[Index] = nullptr;
    for (Index = 0; Index < 0x200; ++Index)
        Flashlights[Index] = nullptr;
    FlashlightCount = 0;

    // the routine (+0x1134 x 0x80, +0x1334, +0x13a8 = 0).
    RoomControlClearButtons(this);

    // three 0x10-dword arrays (+0x1038 / +0x108c / +0x10e0) zeroed and
    // their three 0x10-byte flag arrays (+0x1078 / +0x10cc / +0x1120) set to 1, one index of
    // each per pass.
    for (Index = 0; Index < 0x10; ++Index)
    {
        SceneAmbient[Index] = nullptr;
        SceneTraffic[Index] = nullptr;
        SceneOnce[Index] = nullptr;
        SceneAmbientFlag[Index] = 1;
        SceneTrafficFlag[Index] = 1;
        SceneOnceFlag[Index] = 1;
    }
    // the three counts, +0x1338, +0x13a4, +0x1340.
    SceneAmbientCount = 0;
    SceneTrafficCount = 0;
    SceneOnceCount = 0;
    bCameraMoving = 0;
    Field_0x13a4 = 0;
    bCameraMoveSkippable = 0;
}

FRoomRuntime::~FRoomRuntime()
{
    // . 0x0044944f: the routine on the field -- every script sequence (of any
    // room) deleted. 0x00449461..0x004494e2: malloc_strfree over the three scene arrays, the
    // count re-read each pass and never written. 0x004494e6: the routine (the room buttons
    // deleted).
    G_GfScriptManager.DestroyAll();
    int SceneIndex = 0;
    for (SceneIndex = 0; SceneIndex < SceneAmbientCount; ++SceneIndex)
        malloc_strfree(&SceneAmbient[SceneIndex]);
    for (SceneIndex = 0; SceneIndex < SceneTrafficCount; ++SceneIndex)
        malloc_strfree(&SceneTraffic[SceneIndex]);
    for (SceneIndex = 0; SceneIndex < SceneOnceCount; ++SceneIndex)
        malloc_strfree(&SceneOnce[SceneIndex]);

    // the routine.
    RoomControlDeleteButtons(this);

    // with RoomData+0x1a4 (the FlashLightSet records) not empty, free
    // every +0x81c light and zero the count.
    if (!Room->FlashLightSets.empty())
    {
        int FlashIndex = 0;
        for (FlashIndex = 0; FlashIndex < FlashlightCount; ++FlashIndex)
        {
            if (Flashlights[FlashIndex] != nullptr)
                delete Flashlights[FlashIndex];
            Flashlights[FlashIndex] = nullptr;
        }
        FlashlightCount = 0;
    }

    // every prop through its deleting destructor; deleting the room's
    // main prop first sets +0x58 (the script table's +0x0c) and +0x48 to -1, because the
    // prop's own destructor closes that script.
    int PropIndex = 0;
    for (PropIndex = 0; PropIndex < PropCount; ++PropIndex)
    {
        if (Props[PropIndex]->bRoomMain == 1)
        {
            Scripts.Script = 0xffffffffu;
            MainScript = 0xffffffffu;
        }
        if (Props[PropIndex] != nullptr)
            delete Props[PropIndex];
        Props[PropIndex] = nullptr;
    }
    PropCount = 0;

    // the routine on the 0x40 +0x518 character slots.
    int CharacterSlot = 0;
    for (CharacterSlot = 0; CharacterSlot < 0x40; ++CharacterSlot)
        RoomControlDeleteCharacter(this, CharacterSlot);

    // the routine(+0x500), then +0x500 = 0.
    RoomCameraDestroy(Camera);
    Camera = nullptr;
    // the routine on each +0xe4 wrapper, then both counts to 0.
    int LightIndex = 0;
    for (LightIndex = 0; LightIndex < RoomLightCount; ++LightIndex)
        GfLightWrapperDestroy(&RoomLights[LightIndex]);
    RoomLightCount = 0;
    FrameLightCount = 0;

    // the routine on the +0x64 playback of every room animation.
    int AnimIndex = 0;
    for (AnimIndex = 0; AnimIndex < Room->AnimationCount; ++AnimIndex)
        AnimScriptSetStopPlayback(&Handles64[AnimIndex]);

    // the routine on +0x4c, +0x44 = +0x48 = -1, RoomData::unfixup.
    Scripts.Destroy();
    MainInstance = kRoomObjectNoInstance;
    MainScript = 0xffffffffu;
    Room->unfixup();

    // the routine(1.0f).
    SetFadeImmediate(1.0f);

    // the routine on +0x1020 is the ShipPads member's destructor (runs first of
    // the members, as baseline); 0x0044966d the routine on +0x504 is the Placements member's.
    // the routine (+0x30) is ~EventHandler, the base destroyed after the members
    // and before ~FRoomObject, as baseline. The thunk to
    // at 0x00449679 is the +0x4c member destructor (a second pass over the table
    // Scripts.Destroy already emptied). The RoomObject part (the routine) is ~FRoomObject.
}

// (thiscall, no arguments) -- one Gf::ObjectProp per RoomData+0x158 record whose
// priority the RtcSlider enables, its RoomObject flags copied from the record; the record
// marked as the room's main prop hands its engine instance and main script to +0x44 / +0x48.
static void RoomControlLoadProps(FRoomRuntime* Runtime)
{
    // .
    Runtime->PropCount = 0;
    unsigned Index = 0;
    for (Index = 0; Index < Runtime->Room->Props.size(); ++Index)
    {
        RoomPropInfo const* Record = Runtime->Room->Props[Index];
        // .
        if (!RtcSlider::is_enabled(static_cast<unsigned long>(Record->PriorityId)))
            continue;

        // the routine, then the description from the record.
        FPropData Data;
        Data.Template = Record->Template;
        Data.Name = Record->Name;
        Data.Type = Record->Category;
        Data.Pos = Record->Frame.Pos;
        Data.Orient = Record->Frame.Orient;
        Data.Loadout = Record->Loadout;

        // operator new(0x80), the routine(this, &Data) into +0x618.
        FObjectProp* Prop = new FObjectProp(Runtime, Data);
        Runtime->Props[Runtime->PropCount] = Prop;
        // .
        Prop->bVisible = Record->bVisible ? 1 : 0;
        Prop->bLit = Record->bLit ? 1 : 0;
        Prop->bFog = Record->bFog ? 1 : 0;
        Prop->bRoomMain = Record->bRoomMain ? 1 : 0;
        Prop->LightGroup = Record->LightGroup;
        Prop->Layer = Record->Layer;
        Prop->RenderMode = static_cast<int>(Record->RenderType);
        Prop->Flag14 = Record->bRunningLights ? 1 : 0;
        // the main prop's slot-6 instance and script table +0x0c.
        if (Record->bRoomMain)
        {
            Runtime->MainInstance = Prop->GetEngineInstance();
            Runtime->MainScript = Prop->GetMainScript();
        }
        // ++count, the routine on the description.
        ++Runtime->PropCount;
    }
}

FObjectProp* RoomControlFindProp(FRoomRuntime* Runtime, char const* Name)
{
    // the first prop whose +0x38 name (null -> "") equals Name byte for byte.
    int Index = 0;
    for (Index = 0; Index < Runtime->PropCount; ++Index)
    {
        if (strcmp(Runtime->Props[Index]->GetName(), Name) == 0)
            return Runtime->Props[Index];
    }
    return nullptr;
}

void RoomControlAddProp(FRoomRuntime* Runtime, FObjectProp* Prop)
{
    // no bound check.
    Runtime->Props[Runtime->PropCount] = Prop;
    ++Runtime->PropCount;
}

unsigned GetRoomRuntimeBaseId(FRoomRuntime* Runtime)
{
    // +0x34, read directly by the routine (0x00446294) through the prop's owner.
    return Runtime->BaseId;
}

RoomData* GetRoomRuntimeRoomData(FRoomRuntime* Runtime)
{
    // +0x40, read directly by the Gf script spawn (Script+0x30 -> +0x40, e.g. 0x0044d737).
    return Runtime->Room;
}

unsigned char RoomControlGetField1338(FRoomRuntime* Runtime)
{
    // MOV AL,[ECX+0x1338].
    return Runtime->bCameraMoving;
}

void RoomControlClearField1338(FRoomRuntime* Runtime)
{
    // XOR AL,AL; MOV [ECX+0x1338],AL; MOV [0x0066d359],AL; RET.
    Runtime->bCameraMoving = 0;
    G_bNnKeepPanelOpen = 0;
}

void RoomControlSetField1340(FRoomRuntime* Runtime, unsigned char Value)
{
    // NavBar's inline MOV byte ptr [EDI+0x1340],BL (e.g. 0x004417e1).
    Runtime->bCameraMoveSkippable = Value;
}

void RoomControlNotifyControllers(FRoomRuntime* Runtime, int /*A*/, int B, int C)
{
    // (RET 0xc). 0x0044d246..0x0044d253: C == -1 or B != 3 -> return.
    if (C == -1)
        return;
    if (B != 3)
        return;
    // per button Runtime+0x1134[i] (i < +0x1334, re-read each pass,
    // the entry not null-tested): +0x04 == C or +0x08 == C -> slot 1 (+0x04, no arguments).
    int Index = 0;
    for (Index = 0; Index < Runtime->ControllerCount; ++Index)
    {
        RoomButton* const Button = Runtime->Controllers[Index];
        if (static_cast<int>(Button->Location) == C || Button->Behavior == C)
            Button->Activate();
    }
}

namespace
{
    // the room camera move's lazily acquired 3DMathEngine (the routine on
    // first use, at every use site).
    I3DMathEngine* G_RoomCameraMath = nullptr;

    I3DMathEngine* RoomCameraMath()
    {
        if (G_RoomCameraMath == nullptr)
            G_RoomCameraMath = Acquire3DMathEngine();
        return G_RoomCameraMath;
    }

    // The RoomData +0x138 camera named CameraName (NEURONCOMPONENTS stricmp(CameraName, info +0x00), the
    // count (+0x13c - +0x138) / 4, 0 for a null vector), or null -- the loop the routine
    // (0x0044c2a5..0x0044c2f6) and the routine (0x0044ba73..0x0044bac5) both inline.
    RoomCameraInfo const* FindRoomDataCamera(FRoomRuntime* Runtime, char const* CameraName)
    {
        std::vector<RoomCameraInfo*> const& Cameras = Runtime->Room->Cameras;
        int const Count = static_cast<int>(Cameras.size());
        int Index = 0;
        for (Index = 0; Index < Count; ++Index)
        {
            RoomCameraInfo const* const Info = Cameras[Index];
            if (_stricmp(CameraName, Info->Name) == 0)
                return Info;
        }
        return nullptr;
    }

    // The part the routine (0x0044c304..0x0044c3df) and the routine (0x0044baf4..0x0044bbd1)
    // share: the destination from the RoomData camera (orientation as a quaternion through
    // slot 0x50, position +0x04, half fov / near / far +0x34 / +0x38 / +0x3c) and the start
    // from the +0x500 camera (slot 4 orientation -> slot 0x50, slot 3 position, slots 6 / 8 / 9).
    // The start's far plane is returned; the routine stores it at +0x1394 at once, the routine
    // after the callback's three arguments are read.
    float SetCameraMoveEnds(FRoomRuntime* Runtime, RoomCameraInfo const* Info)
    {
        RoomCameraMath()->MatrixToQuaternion(Runtime->CameraToQuat,
                                             *reinterpret_cast<FMatrix const*>(&Info->Frame.Orient));
        memcpy(Runtime->CameraToPos, &Info->Frame.Pos, sizeof(Runtime->CameraToPos));
        Runtime->CameraToHalfFovX = Info->Fov;
        Runtime->CameraToZNear = Info->NearPlane;
        Runtime->CameraToZFar = Info->FarPlane;

        FRoomCamera* const Camera = Runtime->Camera;
        float const* const Orient = Camera->GetOrientation();
        RoomCameraMath()->MatrixToQuaternion(Runtime->CameraFromQuat,
                                             *reinterpret_cast<FMatrix const*>(Orient));
        memcpy(Runtime->CameraFromPos, Camera->GetPosition(), sizeof(Runtime->CameraFromPos));
        Runtime->CameraFromHalfFovX = Camera->GetHalfFovXDeg();
        Runtime->CameraFromZNear = Camera->GetZNear();
        return Camera->GetZFar();
    }

    // on the +0x500 camera with {Orient, Pos} (orientation first, as baseline packs
    // the 0x30-byte frame on its stack), then the routine / 556e00 / 556f10.
    void ApplyRoomCameraFrame(FRoomRuntime* Runtime, FMatrix const& Orient, float const* Pos,
                              float HalfFovX, float ZNear, float ZFar)
    {
        float Transform[12];
        memcpy(Transform, &Orient, sizeof(float) * 9);
        memcpy(Transform + 9, Pos, sizeof(float) * 3);
        RoomCameraSetTransform(Runtime->Camera, Transform);
        RoomCameraSetHalfFovX(Runtime->Camera, HalfFovX);
        RoomCameraSetZNear(Runtime->Camera, ZNear);
        RoomCameraSetZFar(Runtime->Camera, ZFar);
    }

    // |A - B| < Eps with baseline's `FCOMP Eps; TEST AH,5; JP` -- not-less or unordered fails.
    bool CameraMoveNear(float A, float B, double Eps)
    {
        return fabs(static_cast<double>(A) - static_cast<double>(B)) < Eps;
    }

    // / the field / the field (doubles holding 1e-7f / 1e-4f / 1e-5f).
    double const kCameraPosEps = static_cast<double>(1e-7f);
    double const kCameraFovEps = static_cast<double>(1e-4f);
    double const kCameraQuatEps = static_cast<double>(1e-5f);

    // (thiscall, RET 4) -- true when a RoomData camera of that name exists and the
    // +0x500 camera already stands on it: the same ends and tolerances the routine tests, on
    // stack copies (nothing in the runtime is written). No camera of that name -> false.
    bool IsRoomCameraAt(FRoomRuntime* Runtime, char const* CameraName)
    {
        RoomCameraInfo const* const Info = FindRoomDataCamera(Runtime, CameraName);
        if (Info == nullptr)
            return false;
        FQuaternion ToQuat;
        RoomCameraMath()->MatrixToQuaternion(ToQuat,
                                             *reinterpret_cast<FMatrix const*>(&Info->Frame.Orient));
        FRoomCamera* const Camera = Runtime->Camera;
        FQuaternion FromQuat;
        RoomCameraMath()->MatrixToQuaternion(FromQuat,
                                             *reinterpret_cast<FMatrix const*>(
                                                 Camera->GetOrientation()));
        float FromPos[3];
        memcpy(FromPos, Camera->GetPosition(), sizeof(FromPos));
        float const FromHalfFovX = Camera->GetHalfFovXDeg();
        float const FromZNear = Camera->GetZNear();
        float const FromZFar = Camera->GetZFar();
        // .
        return CameraMoveNear(Info->Frame.Pos.x, FromPos[0], kCameraPosEps) &&
               CameraMoveNear(Info->Frame.Pos.y, FromPos[1], kCameraPosEps) &&
               CameraMoveNear(Info->Frame.Pos.z, FromPos[2], kCameraPosEps) &&
               CameraMoveNear(Info->Fov, FromHalfFovX, kCameraFovEps) &&
               CameraMoveNear(Info->NearPlane, FromZNear, kCameraPosEps) &&
               CameraMoveNear(Info->FarPlane, FromZFar, kCameraPosEps) &&
               CameraMoveNear(ToQuat.W, FromQuat.W, kCameraQuatEps) &&
               CameraMoveNear(ToQuat.X, FromQuat.X, kCameraQuatEps) &&
               CameraMoveNear(ToQuat.Y, FromQuat.Y, kCameraQuatEps) &&
               CameraMoveNear(ToQuat.Z, FromQuat.Z, kCameraQuatEps);
    }
}

bool RoomControlSnapCamera(FRoomRuntime* Runtime, char const* CameraName)
{
    // (thiscall, RET 4). 0x0044c297 / 0x0044c29e: any move stops (+0x1338 = 0,
    // = 0) before the lookup; no camera of that name -> false.
    Runtime->bCameraMoving = 0;
    G_bNnKeepPanelOpen = 0;
    RoomCameraInfo const* const Info = FindRoomDataCamera(Runtime, CameraName);
    if (Info == nullptr)
        return false;

    // both ends as the routine sets them (+0x1394 straight from slot 9).
    Runtime->CameraFromZFar = SetCameraMoveEnds(Runtime, Info);

    // the destination quaternion back to a matrix (slot 0x54) and the
    // destination frame, half fov, near and far installed on the camera.
    FMatrix Orient;
    RoomCameraMath()->QuaternionToMatrix(Orient, Runtime->CameraToQuat);
    ApplyRoomCameraFrame(Runtime, Orient, Runtime->CameraToPos, Runtime->CameraToHalfFovX,
                         Runtime->CameraToZNear, Runtime->CameraToZFar);
    return true;
}

bool RoomControlTrackCamera(FRoomRuntime* Runtime, char const* CameraName,
                            FRoomCameraDoneCallback Done, void* A, void* B)
{
    // (thiscall, RET 0x10). A move already running, or no camera of that name ->
    // false (0x0044ba68..0x0044bad0).
    if (Runtime->bCameraMoving != 0)
        return false;
    RoomCameraInfo const* const Info = FindRoomDataCamera(Runtime, CameraName);
    if (Info == nullptr)
        return false;

    // +0x1338 = the field = 1, +0x1344 = 0, +0x133c = 2.0f.
    Runtime->bCameraMoving = 1;
    G_bNnKeepPanelOpen = 1;
    Runtime->CameraMoveElapsed = 0.0f;
    Runtime->CameraMoveDuration = 2.0f;

    // both ends; +0x1394 = the start's far plane, +0x1398 / +0x139c /
    // +0x13a0 = Done, A, B.
    float const FromZFar = SetCameraMoveEnds(Runtime, Info);
    Runtime->CameraFromZFar = FromZFar;
    Runtime->CameraDone = Done;
    Runtime->CameraDoneA = A;
    Runtime->CameraDoneB = B;

    // when the camera is already there -- position components within
    // (1e-7f), half fov within the field (1e-4f), near and far within 1e-7f,
    // the four quaternion components within the field (1e-5f), each `TEST AH,5; JP` (a NaN
    // keeps the move) -- the move is dropped again (+0x1338 = the field = 0). The far test
    // uses the slot 9 value itself, before it went through +0x1394 (the same float).
    double const kPosEps = kCameraPosEps;
    double const kFovEps = kCameraFovEps;
    double const kQuatEps = kCameraQuatEps;
    if (CameraMoveNear(Runtime->CameraToPos[0], Runtime->CameraFromPos[0], kPosEps) &&
        CameraMoveNear(Runtime->CameraToPos[1], Runtime->CameraFromPos[1], kPosEps) &&
        CameraMoveNear(Runtime->CameraToPos[2], Runtime->CameraFromPos[2], kPosEps) &&
        CameraMoveNear(Runtime->CameraToHalfFovX, Runtime->CameraFromHalfFovX, kFovEps) &&
        CameraMoveNear(Runtime->CameraToZNear, Runtime->CameraFromZNear, kPosEps) &&
        CameraMoveNear(Runtime->CameraToZFar, FromZFar, kPosEps) &&
        CameraMoveNear(Runtime->CameraFromQuat.X, Runtime->CameraToQuat.X, kQuatEps) &&
        CameraMoveNear(Runtime->CameraFromQuat.Y, Runtime->CameraToQuat.Y, kQuatEps) &&
        CameraMoveNear(Runtime->CameraFromQuat.Z, Runtime->CameraToQuat.Z, kQuatEps) &&
        CameraMoveNear(Runtime->CameraFromQuat.W, Runtime->CameraToQuat.W, kQuatEps))
    {
        Runtime->bCameraMoving = 0;
        G_bNnKeepPanelOpen = 0;
    }

    // the answer is +0x1338 as it now stands.
    return Runtime->bCameraMoving != 0;
}

void RoomControlUpdateCameraMove(FRoomRuntime* Runtime, float Delta)
{
    // (thiscall, RET 4). No move -> nothing.
    if (Runtime->bCameraMoving == 0)
        return;

    // +0x1344 += Delta (stored as a float, the unrounded sum compared);
    // `TEST AH,0x41` + JNZ keeps BL clear for not-greater (or unordered): done only when the
    // sum is above +0x133c.
    double const Elapsed = static_cast<double>(Delta) +
                           static_cast<double>(Runtime->CameraMoveElapsed);
    Runtime->CameraMoveElapsed = static_cast<float>(Elapsed);
    bool const bDone = Elapsed > static_cast<double>(Runtime->CameraMoveDuration);

    // the skip latch (the routine) with +0x1340, or done -> the end.
    if ((GfScriptSequenceSkipLatch() && Runtime->bCameraMoveSkippable != 0) || bDone)
    {
        // +0x1338 = the field = 0; the destination installed.
        Runtime->bCameraMoving = 0;
        G_bNnKeepPanelOpen = 0;
        FMatrix Orient;
        RoomCameraMath()->QuaternionToMatrix(Orient, Runtime->CameraToQuat);
        ApplyRoomCameraFrame(Runtime, Orient, Runtime->CameraToPos, Runtime->CameraToHalfFovX,
                             Runtime->CameraToZNear, Runtime->CameraToZFar);
        // a callback -> Done(+0x139c, +0x13a0, 0) (cdecl).
        if (Runtime->CameraDone != nullptr)
            Runtime->CameraDone(Runtime->CameraDoneA, Runtime->CameraDoneB, 0);
        return;
    }

    // T = 1 - (cos(+0x1344 / +0x133c * pi) + 1) * 0.5 (the field pi,
    // 1.0, the field 0.5, all floats), stored as a float.
    double const Phase = static_cast<double>(Runtime->CameraMoveElapsed) /
                         static_cast<double>(Runtime->CameraMoveDuration) *
                         static_cast<double>(3.14159274101257324f);
    float const T = static_cast<float>(1.0 - (cos(Phase) + 1.0) * 0.5);

    // Pos = (To - From) * T + From. x's product and z's difference are
    // stored as floats on the way (FSTP), y stays on the FPU throughout.
    double const DeltaX = static_cast<double>(Runtime->CameraToPos[0]) -
                          static_cast<double>(Runtime->CameraFromPos[0]);
    double const DeltaY = static_cast<double>(Runtime->CameraToPos[1]) -
                          static_cast<double>(Runtime->CameraFromPos[1]);
    float const DeltaZ = static_cast<float>(static_cast<double>(Runtime->CameraToPos[2]) -
                                            static_cast<double>(Runtime->CameraFromPos[2]));
    float const ScaledX = static_cast<float>(DeltaX * static_cast<double>(T));
    float Pos[3];
    Pos[0] = static_cast<float>(static_cast<double>(ScaledX) +
                                static_cast<double>(Runtime->CameraFromPos[0]));
    Pos[1] = static_cast<float>(DeltaY * static_cast<double>(T) +
                                static_cast<double>(Runtime->CameraFromPos[1]));
    Pos[2] = static_cast<float>(static_cast<double>(DeltaZ) * static_cast<double>(T) +
                                static_cast<double>(Runtime->CameraFromPos[2]));

    // slerp From -> To by T (slot 0x58), back to a matrix (slot 0x54),
    // installed with Pos.
    FQuaternion Quat;
    RoomCameraMath()->QuatSlerp(Quat, Runtime->CameraFromQuat, Runtime->CameraToQuat, T);
    FMatrix Orient;
    RoomCameraMath()->QuaternionToMatrix(Orient, Quat);
    float Transform[12];
    memcpy(Transform, &Orient, sizeof(float) * 9);
    memcpy(Transform + 9, Pos, sizeof(float) * 3);
    RoomCameraSetTransform(Runtime->Camera, Transform);

    // half fov, near and far the same way, each (To - From) * T + From
    // rounded once.
    RoomCameraSetHalfFovX(Runtime->Camera,
                          static_cast<float>((static_cast<double>(Runtime->CameraToHalfFovX) -
                                              static_cast<double>(Runtime->CameraFromHalfFovX)) *
                                                 static_cast<double>(T) +
                                             static_cast<double>(Runtime->CameraFromHalfFovX)));
    RoomCameraSetZNear(Runtime->Camera,
                       static_cast<float>((static_cast<double>(Runtime->CameraToZNear) -
                                           static_cast<double>(Runtime->CameraFromZNear)) *
                                              static_cast<double>(T) +
                                          static_cast<double>(Runtime->CameraFromZNear)));
    RoomCameraSetZFar(Runtime->Camera,
                      static_cast<float>((static_cast<double>(Runtime->CameraToZFar) -
                                          static_cast<double>(Runtime->CameraFromZFar)) *
                                             static_cast<double>(T) +
                                         static_cast<double>(Runtime->CameraFromZFar)));
}

// (Event::Handler slot 0 of the +0x30 face, thiscall RET 4; the offsets below are
// the runtime's, the listing's are 0x30 lower because ECX is the face). Id 0xf with a camera move running finishes the move at once,
// as the routine's end does: +0x1338 = 0 and the field = 0 (0x0044ada1..0x0044adaf), the
// destination quaternion through the field slot 21 (lazily acquired, 0x0044ada8..0x0044add3),
// the +0x1358 position, then the routine / 556ce0 / 556e00 / 556f10 on +0x500 with the
// destination frame, half fov, near and far (0x0044ae0b..0x0044ae44); a callback at +0x1398 ->
// (+0x139c, +0x13a0, 0). True then; any other id, or no move, false.
bool FRoomRuntime::OnEvent(unsigned MsgId)
{
    if (MsgId != 0xf || bCameraMoving == 0)
        return false;
    bCameraMoving = 0;
    G_bNnKeepPanelOpen = 0;
    FMatrix Orient;
    RoomCameraMath()->QuaternionToMatrix(Orient, CameraToQuat);
    ApplyRoomCameraFrame(this, Orient, CameraToPos, CameraToHalfFovX, CameraToZNear,
                         CameraToZFar);
    if (CameraDone != nullptr)
        CameraDone(CameraDoneA, CameraDoneB, 0);
    return true;
}

bool RoomControlTrackScriptCamera(FRoomRuntime* Runtime, int Arg0,
                                  FRoomCameraDoneCallback Done, void* A, void* B,
                                  unsigned char bArg4)
{
    // (thiscall, RET 0x14) -- a step along the room's numbered "Camera_Track_%d"
    // cameras, +0x13a4 the current number (0 = none yet). Arg0 is a byte (TEST BL,BL): set steps
    // forward, clear backward. bArg4: when the camera already stands on the chosen one
    // (the routine), the step is repeated from there with bArg4 clear, and that answer is
    // returned. Otherwise the answer is the routine's move to it.
    bool const bForward = static_cast<unsigned char>(Arg0) != 0;
    char const* const kFirstTrack = "Camera_Track_1"; // s_Camera_Track_1_005cb893+1
    char Name[0x100];

    if (Runtime->Field_0x13a4 == 0)
    {
        // the first step goes to Camera_Track_1.
        Runtime->Field_0x13a4 = 1;
        if (bArg4 != 0 && IsRoomCameraAt(Runtime, kFirstTrack))
            return RoomControlTrackScriptCamera(Runtime, Arg0, Done, A, B, 0);
        return RoomControlTrackCamera(Runtime, kFirstTrack, Done, A, B);
    }

    if (bForward)
    {
        // the next number.
        Runtime->Field_0x13a4 += 1;
        sprintf(Name, "Camera_Track_%d", Runtime->Field_0x13a4);
        if (bArg4 != 0 && IsRoomCameraAt(Runtime, Name))
            return RoomControlTrackScriptCamera(Runtime, Arg0, Done, A, B, 0);
        // a move there -> true.
        if (RoomControlTrackCamera(Runtime, Name, Done, A, B))
            return true;
        // none (no such camera, or already there) -> back to 1.
        Runtime->Field_0x13a4 = 1;
        if (bArg4 != 0 && IsRoomCameraAt(Runtime, kFirstTrack))
            return RoomControlTrackScriptCamera(Runtime, Arg0, Done, A, B, 0);
        return RoomControlTrackCamera(Runtime, kFirstTrack, Done, A, B);
    }

    // the previous number.
    Runtime->Field_0x13a4 -= 1;
    if (Runtime->Field_0x13a4 == 0)
    {
        // below 1 wraps to the highest number whose camera exists
        // (counting up from 1 until a name has no RoomData camera; 0 when there is none).
        for (;;)
        {
            Runtime->Field_0x13a4 += 1;
            sprintf(Name, "Camera_Track_%d", Runtime->Field_0x13a4);
            if (FindRoomDataCamera(Runtime, Name) == nullptr)
                break;
        }
        Runtime->Field_0x13a4 -= 1;
    }

    // the chosen number; the repeat passes Arg0 = 0 (backward).
    sprintf(Name, "Camera_Track_%d", Runtime->Field_0x13a4);
    if (bArg4 != 0 && IsRoomCameraAt(Runtime, Name))
        return RoomControlTrackScriptCamera(Runtime, 0, Done, A, B, 0);
    return RoomControlTrackCamera(Runtime, Name, Done, A, B);
}

unsigned char RoomControlGetField517(FRoomRuntime* Runtime)
{
    // CMP byte ptr [EAX+0x517],BL.
    return Runtime->Flags514[3];
}

void RoomControlRequestCharacterScripts(FRoomRuntime* Runtime)
{
    // MOV byte ptr [ECX+0x516],0x1. Read by the room update the branch
    // (0x0044ab89), which then runs the room characters' start script once.
    Runtime->Flags514[2] = 1;
}

unsigned GetRoomRuntimeRoomId(FRoomRuntime* Runtime)
{
    // +0x38.
    return Runtime->RoomId;
}

FCharacterObject* RoomControlFindCharacterByKey(FRoomRuntime* Runtime, unsigned Key)
{
    // the first slot whose character's record key (+0x154) is Key.
    int Slot = 0;
    for (Slot = 0; Slot < 0x40; ++Slot)
    {
        FCharacterObject* const Character = Runtime->Slot518[Slot];
        if (Character != nullptr && Character->Props.Key == Key)
            return Character;
    }
    return nullptr;
}

FCharacterObject* RoomControlPlayerCharacter(FRoomRuntime* Runtime)
{
    // the first character on a placement (+0x1b8) whose char type (+0x34) is 1.
    int Slot = 0;
    for (Slot = 0; Slot < 0x40; ++Slot)
    {
        FCharacterObject* const Character = Runtime->Slot518[Slot];
        if (Character == nullptr)
            continue;
        CharPlaceInfo const* const Placement = Character->Placement;
        if (Placement != nullptr && Placement->CharType == 1)
            return Character;
    }
    return nullptr;
}

FCharacterObject* RoomControlFindCharacterByRole(FRoomRuntime* Runtime, int Role)
{
    // the first character whose role (+0x300) is Role.
    int Slot = 0;
    for (Slot = 0; Slot < 0x40; ++Slot)
    {
        FCharacterObject* const Character = Runtime->Slot518[Slot];
        if (Character != nullptr && Character->Role == Role)
            return Character;
    }
    return nullptr;
}

FCharacterObject* RoomControlFindCharacterByName(FRoomRuntime* Runtime, char const* Name)
{
    // the first character on a placement whose setpoint segment
    // (SetpointProperties::extract_from_name, +0x08) is Name (NEURONCOMPONENTS stricmp).
    int Slot = 0;
    for (Slot = 0; Slot < 0x40; ++Slot)
    {
        FCharacterObject* const Character = Runtime->Slot518[Slot];
        if (Character == nullptr)
            continue;
        CharPlaceInfo const* const Placement = Character->Placement;
        if (Placement == nullptr)
            continue;
        SetpointProperties Setpoint;
        Setpoint.extract_from_name(Placement->Name);
        if (_stricmp(Setpoint.Segment, Name) == 0)
            return Character;
    }
    return nullptr;
}

static void RoomControlDeleteCharacter(FRoomRuntime* Runtime, int Slot)
{
    // . 0x00449e18..0x00449e21: an empty slot, nothing.
    FCharacterObject* const Character = Runtime->Slot518[Slot];
    if (Character == nullptr)
        return;
    // its placement free again (the routine).
    if (Character->PlacementIndex != -1)
        Runtime->Placements[Character->PlacementIndex].Character = nullptr;
    // the deleting destructor (slot 0 with 1), the slot cleared.
    delete Runtime->Slot518[Slot];
    Runtime->Slot518[Slot] = nullptr;
}

void RoomControlUpdateCharacter(FRoomRuntime* Runtime, CharacterProperties const& Props)
{
    // . 0x00449e68..0x00449e84: this room's record, with a costume (the routine).
    if (Props.RoomId != Runtime->RoomId)
        return;
    if (!CostumeHasParts(Props.Cost))
        return;

    // the slot of the record's key.
    int Slot = 0;
    for (Slot = 0; Slot < 0x40; ++Slot)
    {
        FCharacterObject* const Character = Runtime->Slot518[Slot];
        if (Character != nullptr && Character->Props.Key == Props.Key)
            break;
    }
    if (Slot < 0x40)
    {
        // the character updated in place -- the record replaced
        // (CharacterProperties::destroy + operator=), the hotspot re-gated (the routine) and
        // the record's idle script made the own one (the routine with 0).
        FCharacterObject* const Character = Runtime->Slot518[Slot];
        Character->Props.destroy();
        Character->Props = Props;
        Character->UpdateHotspotEnabled();
        Runtime->Slot518[Slot]->SetOwnScript(Props.Script, 0);
        return;
    }

    // the first empty slot (-1 when there is none) takes a new
    // character (the routine). With all 0x40 slots taken baseline stores through slot -1, which
    // is the +0x514 flag dword just in front of the array; the port writes the same bytes.
    int Free = -1;
    for (Slot = 0; Slot < 0x40; ++Slot)
    {
        if (Runtime->Slot518[Slot] == nullptr)
        {
            Free = Slot;
            break;
        }
    }
    FCharacterObject* const Character = CreatePlacedCharacter(Runtime->Placements, Props, Runtime);
    if (Free == -1)
    {
        memcpy(Runtime->Flags514, &Character, sizeof(Runtime->Flags514));
        return;
    }
    Runtime->Slot518[Free] = Character;
}

namespace
{
    // 's three sequence loops (0x0044a800 / 0x0044a890 / 0x0044a920): a sequence
    // per path (findable only for the ambient ones), the script added with an identity frame
    // and no engine (loaded from the path), then the flags: +0x00 = 0, +0x04 loop (ambient
    // only), +0x06 = 0, +0x08 = 1, +0x09 = 0, +0x10 = 0, +0x0d = the path's flag byte; played.
    // The count is re-read from the room after every sequence (CMP EBP,[ESI+0x1088] at
    // , +0x10dc at 0x0044a8f7, +0x1130 at 0x0044a990), hence the reference.
    void PlayRoomSceneList(FRoomRuntime* Runtime, char* const* Paths,
                           unsigned char const* Flags, int const& Count, unsigned char bFindable,
                           int Kind, unsigned char bLoop)
    {
        int Index = 0;
        for (Index = 0; Index < Count; ++Index)
        {
            FGfScriptSequence* const Sequence =
                G_GfScriptManager.CreateSequence(Runtime, bFindable);
            Csys Frame;
            Frame.set_identity();
            Sequence->AddScript(Paths[Index], Kind, &Frame, nullptr);
            Sequence->BindMode = 0;
            Sequence->bLoop = bLoop;
            Sequence->bLetterbox = 0;
            Sequence->bApplySpatial = 1;
            Sequence->bRestoreCamera = 0;
            Sequence->Priority = 0;
            Sequence->bSoundsUseFade = Flags[Index];
            Sequence->Play();
        }
    }
}

// (thiscall, RET 8) -- the room character whose script behaviour is a start
// script. *OutCharacterKey = 0, *OutBehaviorKey = -1. The room state's characters (the routine)
// and behaviours (the routine) for +0x38; the characters counted (0x00449f90..0x00449fa0), then
// for each index a fresh walk to it (0x00449fb0..0x00449fd0; EDI stays 0 on a miss). A character
// whose +0x5c behaviour id is -1 is passed over; otherwise the first behaviour whose key (+0x00)
// equals it (0x00449fe0..0x00449ffe; EDX stays 0 when none does, and +0x09 is then read through
// null -- no guard). Its +0x09 == 1 -> the character's key (+0x00) and the behaviour's key out,
// answer true. No break: the last such character wins. Callers: the routine (0x0044a9a4),
// (0x0044ace2), the room update the branch (0x0044abbf).
static bool RoomControlFindStartScript(FRoomRuntime* Runtime, int* OutCharacterKey,
                                       int* OutBehaviorKey)
{
    *OutCharacterKey = 0;
    *OutBehaviorKey = -1;
    bool bFound = false;
    TMirrorPropertyBucket<CharacterProperties>* const Characters =
        GetClientLocationRoomCharacters(Runtime->RoomId);
    TMirrorPropertyBucket<ScriptBehavior>* const Behaviors =
        GetClientLocationRoomScriptBehaviors(Runtime->RoomId);

    int CharacterCount = 0;
    MetaNode<CharacterProperties>* CharacterCursor = nullptr;
    while (Characters->List.traverse(CharacterCursor, true))
        ++CharacterCount;

    int CharacterIndex = 0;
    for (CharacterIndex = 0; CharacterIndex < CharacterCount; ++CharacterIndex)
    {
        CharacterProperties const* Character = nullptr;
        int Step = 0;
        CharacterCursor = nullptr;
        while (Characters->List.traverse(CharacterCursor, true))
        {
            if (Step == CharacterIndex)
            {
                Character = CharacterCursor->Data;
                break;
            }
            ++Step;
        }
        unsigned const BehaviorId = Character->BehaviorId;
        if (BehaviorId == 0xffffffffu)
            continue;

        ScriptBehavior const* Behavior = nullptr;
        MetaNode<ScriptBehavior>* BehaviorCursor = nullptr;
        while (Behaviors->List.traverse(BehaviorCursor, true))
        {
            if (static_cast<unsigned>(BehaviorCursor->Data->BehaviorId) == BehaviorId)
            {
                Behavior = BehaviorCursor->Data;
                break;
            }
        }
        if (Behavior->Flag09 == 1)
        {
            *OutCharacterKey = static_cast<int>(Character->Key);
            *OutBehaviorKey = Behavior->BehaviorId;
            bFound = true;
        }
    }
    return bFound;
}

// The "found" tail both the routine (0x0044acf2..0x0044ad4c) and the branch
// (0x0044abc8..0x0044ac28) inline, instruction for instruction: the +0x518 slot whose object's
// +0x154 key is CharacterKey (none: EDI stays 0 and +0x1b8 is read through null -- no guard;
// the CMP EAX,-1 after a hit can never be taken), the routine(object, this, object +0x1b8,
// BehaviorKey, 0, 0), IServer slot 41 (+0xa4)(CharacterKey, the field), +0x517 = 1.
static void RoomControlRunStartScript(FRoomRuntime* Runtime, int CharacterKey, int BehaviorKey)
{
    FCharacterObject* Character = nullptr;
    int Slot = 0;
    for (Slot = 0; Slot < 0x40; ++Slot)
    {
        FCharacterObject* const Candidate = Runtime->Slot518[Slot];
        if (Candidate != nullptr && Candidate->Props.Key == static_cast<unsigned>(CharacterKey))
        {
            Character = Candidate;
            break;
        }
    }
    StartCharacterBehaviorScript(Character, Runtime, Character->Placement,
                                 static_cast<unsigned>(BehaviorKey), nullptr, nullptr);
    static_cast<IServer*>(GetLocalServerInterface())
        ->GFObjSelect(static_cast<unsigned>(CharacterKey), GetPlayerConnectionId());
    Runtime->Flags514[3] = 1;
}

// (thiscall, no arguments) -- start the room's scene scripts.
// every RoomData+0x1b8 "scene =" record whose priority the RtcSlider
//     enables: type 1 / 2 / 3 -> malloc_strcpy into the ambient / traffic / once array, flag 1,
//     count + 1 (no bound check).
// the same from the room state's ambient scripts (the routine on
//     the LocationController for +0x38): the nodes counted with the routine (forward), then
//     for each index a fresh forward walk to that node (0x0044a6c1..0x0044a709), its record's
//     +0x08 the type, +0x10 the path and +0x0c the flag byte.
// PlayRoomSceneList for the ambient (findable, kind 0, looping),
//     traffic (kind 5) and once (kind 0) lists.
// the routine -- a room character with a start script found ->
//     straight to 0x0044ab46 (no +0x1020, goods cart or character scripts).
// the routine on +0x1020 (the player's ship plays its landing
//     script) -> +0x515 = 1.
// the player's ship shown (the routine) and RoomData +0xb8 / +0xbc
//     (the goods-cart script and its priority) enabled -> a sequence with the flags +0x14 = 1,
//     +0x06 / +0x08 / +0x09 / +0x16 = 1, played; +0x515 = 1.
// each +0x518 character on a placement with a start script -> a
//     sequence of its script, bound to the character; +0x515 = 1.
// the routine(0).
static void RoomControlStartScenes(FRoomRuntime* Runtime)
{
    RoomData* const Room = Runtime->Room;
    int RecordIndex = 0;
    for (RecordIndex = 0; RecordIndex < static_cast<int>(Room->AmbientScripts.size());
         ++RecordIndex)
    {
        AmbientScriptInfo const& Record = *Room->AmbientScripts[RecordIndex];
        if (!RtcSlider::is_enabled(static_cast<unsigned long>(Record.Priority)))
            continue;
        if (Record.Type == 1)
        {
            malloc_strcpy(&Runtime->SceneAmbient[Runtime->SceneAmbientCount], Record.Path);
            Runtime->SceneAmbientFlag[Runtime->SceneAmbientCount] = 1;
            ++Runtime->SceneAmbientCount;
        }
        else if (Record.Type == 2)
        {
            malloc_strcpy(&Runtime->SceneTraffic[Runtime->SceneTrafficCount], Record.Path);
            Runtime->SceneTrafficFlag[Runtime->SceneTrafficCount] = 1;
            ++Runtime->SceneTrafficCount;
        }
        else if (Record.Type == 3)
        {
            malloc_strcpy(&Runtime->SceneOnce[Runtime->SceneOnceCount], Record.Path);
            Runtime->SceneOnceFlag[Runtime->SceneOnceCount] = 1;
            ++Runtime->SceneOnceCount;
        }
    }

    TMirrorPropertyBucket<AmbientScriptProperties>* const Scripts =
        GetClientLocationRoomAmbientScripts(Runtime->RoomId);
    int NodeCount = 0;
    MetaNode<AmbientScriptProperties>* Cursor = nullptr;
    while (Scripts->List.traverse(Cursor, true))
        ++NodeCount;
    int NodeIndex = 0;
    for (NodeIndex = 0; NodeIndex < NodeCount; ++NodeIndex)
    {
        // EDI starts at 0 and only a reached index sets it, so a walk
        // that ran out would read through null -- NodeIndex < NodeCount keeps it reachable.
        AmbientScriptProperties const* Script = nullptr;
        int Step = 0;
        Cursor = nullptr;
        while (Scripts->List.traverse(Cursor, true))
        {
            if (Step == NodeIndex)
            {
                Script = Cursor->Data;
                break;
            }
            ++Step;
        }
        if (Script->ScriptId == 1)
        {
            malloc_strcpy(&Runtime->SceneAmbient[Runtime->SceneAmbientCount], Script->Name);
            Runtime->SceneAmbientFlag[Runtime->SceneAmbientCount] =
                static_cast<unsigned char>(Script->bFlag);
            ++Runtime->SceneAmbientCount;
        }
        else if (Script->ScriptId == 2)
        {
            malloc_strcpy(&Runtime->SceneTraffic[Runtime->SceneTrafficCount], Script->Name);
            Runtime->SceneTrafficFlag[Runtime->SceneTrafficCount] =
                static_cast<unsigned char>(Script->bFlag);
            ++Runtime->SceneTrafficCount;
        }
        else if (Script->ScriptId == 3)
        {
            malloc_strcpy(&Runtime->SceneOnce[Runtime->SceneOnceCount], Script->Name);
            Runtime->SceneOnceFlag[Runtime->SceneOnceCount] =
                static_cast<unsigned char>(Script->bFlag);
            ++Runtime->SceneOnceCount;
        }
    }

    PlayRoomSceneList(Runtime, Runtime->SceneAmbient, Runtime->SceneAmbientFlag,
                      Runtime->SceneAmbientCount, 1, 0, 1);
    PlayRoomSceneList(Runtime, Runtime->SceneTraffic, Runtime->SceneTrafficFlag,
                      Runtime->SceneTrafficCount, 0, 5, 0);
    PlayRoomSceneList(Runtime, Runtime->SceneOnce, Runtime->SceneOnceFlag,
                      Runtime->SceneOnceCount, 0, 0, 0);

    int StartCharacterKey = 0;
    int StartBehaviorKey = -1;
    if (RoomControlFindStartScript(Runtime, &StartCharacterKey, &StartBehaviorKey))
    {
        RoomControlSetLandingPending(0);
        return;
    }

    // the routine.
    if (Runtime->ShipPads.PlayLanding())
        Runtime->Flags514[1] = 1;

    if (GfObjectShip() != nullptr && Room->GoodscartScript != nullptr &&
        RtcSlider::is_enabled(static_cast<unsigned long>(Room->GoodscartPriorityId)))
    {
        FGfScriptSequence* const Sequence = G_GfScriptManager.CreateSequence(Runtime, 0);
        Csys Frame;
        Frame.set_identity();
        Sequence->AddScript(Room->GoodscartScript, 0, &Frame, nullptr);
        Sequence->bPropsToRoom = 1;
        Sequence->BindMode = 0;
        Sequence->bLetterbox = 1;
        Sequence->bApplySpatial = 1;
        Sequence->bRestoreCamera = 1;
        Sequence->Priority = 0;
        Sequence->Field_0x16 = 1;
        Sequence->Play();
        Runtime->Flags514[1] = 1;
    }

    // every character on a placement with a start script (+0x40)
    // plays it in a sequence of its own, bound to it, letterboxed; +0x515 is set after each
    // one, even when no sequence could be made.
    int Slot = 0;
    for (Slot = 0; Slot < 0x40; ++Slot)
    {
        FCharacterObject* const Character = Runtime->Slot518[Slot];
        if (Character == nullptr)
            continue;
        if (Character->PlacementIndex == -1)
            continue;
        char const* const StartScript =
            Runtime->Placements[Character->PlacementIndex].Info->StartScript;
        if (StartScript == nullptr || StartScript[0] == '\0')
            continue;
        FGfScriptSequence* const Sequence = G_GfScriptManager.CreateSequence(Runtime, 0);
        if (Sequence != nullptr)
        {
            Csys Frame;
            Frame.Pos.x = 0.0f;
            Frame.Pos.y = 0.0f;
            Frame.Pos.z = 0.0f;
            MatrixSetIdentity(Frame.Orient);
            Sequence->AddScript(StartScript, 0, &Frame, nullptr);
            Sequence->Mirror.Add(Runtime->Slot518[Slot], nullptr);
            Sequence->BindMode = 1;
            Sequence->bLetterbox = 1;
            Sequence->bApplySpatial = 1;
            Sequence->bRestorePlayer = 0;
            Sequence->bRestoreCamera = 1;
            Sequence->Priority = 0;
            Sequence->bDuckAmbient = 0;
            Sequence->bDuckMusic = 0;
            Sequence->Field_0x16 = 1;
            Sequence->Play();
        }
        Runtime->Flags514[1] = 1;
    }

    RoomControlSetLandingPending(0);
}

void RoomControlPlayScenes(FRoomRuntime* Runtime)
{
    // . the routine; then, with neither +0x515 nor +0x514 set, the routine; a
    // character found -> 0x0044acf2..0x0044ad4c: the +0x518 slot whose object's +0x154 is its
    // key (none: EDI stays 0 and +0x1b8 is read through null), the routine(object, this,
    // object +0x1b8, behaviour key, 0, 0), IServer slot 41 (+0xa4)(character key, session
    // ), +0x517 = 1. Then +0x514 = 1.
    RoomControlStartScenes(Runtime);
    if (Runtime->Flags514[1] != 0)
        return;
    if (Runtime->Flags514[0] != 0)
        return;
    int CharacterKey = 0;
    int BehaviorKey = -1;
    if (RoomControlFindStartScript(Runtime, &CharacterKey, &BehaviorKey))
        RoomControlRunStartScript(Runtime, CharacterKey, BehaviorKey);
    Runtime->Flags514[0] = 1;
}

// (thiscall, RET 4) -- slot 2 (+0x08, Delta) on each +0x1134 room button
// (i < +0x1334, re-read each pass, the entry not null-tested).
static void RoomControlUpdateButtons(FRoomRuntime* Runtime, float Delta)
{
    int Index = 0;
    for (Index = 0; Index < Runtime->ControllerCount; ++Index)
        Runtime->Controllers[Index]->Update(Delta);
}

void FRoomRuntime::Update(double DeltaSeconds)
{
    // (RoomControl slot 5, thiscall, RET 8). Called every frame by the gameflow
    // RoomObject walk the routine (gameflow.cpp). The double is narrowed to float once
    // (FSTP float) and that float is what every callee below receives.
    float const Delta = static_cast<float>(DeltaSeconds);

    // the routine(Delta) -- the room camera move.
    RoomControlUpdateCameraMove(this, Delta);

    // a restarted character start script (+0x516, the routine) --
    // unless the scenes already ran (+0x514), the routine and, on a hit, the shared tail;
    // +0x514 set either way. +0x516 cleared whenever it was set.
    if (Flags514[2] != 0)
    {
        if (Flags514[0] == 0)
        {
            int CharacterKey = 0;
            int BehaviorKey = -1;
            if (RoomControlFindStartScript(this, &CharacterKey, &BehaviorKey))
                RoomControlRunStartScript(this, CharacterKey, BehaviorKey);
            Flags514[0] = 1;
        }
        Flags514[2] = 0;
    }

    // [EFFECTS] on and FLASHLIGHTS > 0 -> the routine(Delta) on each
    // +0x81c light, the count (+0x101c) re-read every iteration.
    if (GetGameOption(kGameOptEffectsGroup) != 0 && GetGameOption(kGameOptFlashlights) > 0)
    {
        int Index = 0;
        for (Index = 0; Index < FlashlightCount; ++Index)
            Flashlights[Index]->Update(Delta);
    }

    // the routine(Delta).
    RoomControlUpdateButtons(this, Delta);
}

namespace
{
    // the routine's byte: the player's ship plays its pad's landing script in
    // the next room that starts its scenes (the routine).
    unsigned char G_RoomControlLandingPending = 0;
}

void RoomControlSetLandingPending(unsigned char Value)
{
    // MOV AL,[ESP+4] / MOV [the field],AL.
    G_RoomControlLandingPending = Value;
}

// ---------------------------------------------------------------------------
// The ship-pad table (Runtime+0x1020).
// ---------------------------------------------------------------------------

void FRoomShipPad::Clear()
{
    // each non-null ship through its slot 0 with 1 (the deleting destructor),
    // then its field zeroed; +0x08 first, then +0x0c.
    if (Ship != nullptr)
    {
        delete Ship;
        Ship = nullptr;
    }
    if (ForSale != nullptr)
    {
        delete ForSale;
        ForSale = nullptr;
    }
}

void FRoomShipPad::CreatePlayerShip()
{
    // . 0x0044880d the routine (the description ctor).
    FObjectShipDesc Desc;
    // the pad's frame -- Place +0x04 the position, +0x10 the
    // orientation (MOVSD.REP 9 dwords).
    Desc.Pos = Place->Frame.Pos;
    Desc.Orient = Place->Frame.Orient;
    // shadow on. 0x00448843..0x00448846: +0x3c = Place +0x08, the pad's y.
    Desc.bShadow = 1;
    Desc.Float1C = Place->Frame.Pos.y;
    // malloc_strcpy(&Desc +0x34, Place->LaunchingScript (+0x3c)).
    malloc_strcpy(&Desc.LaunchScript, Place->LaunchingScript);
    // Archetype::GetShip(the routine()) into +0x00.
    Desc.Ship = Archetype::GetShip(GetPlayerShipArchetypeIdbaseline());
    // the routine, the player's equipment list, into +0x40.
    Desc.Equipment = &GetPlayerEquipDescListbaseline();
    // operator new(0xb0); non-null -> the routine(pad +0x00, &Desc);
    // the result into +0x08.
    Ship = new FObjectShip(Room, Desc);
    // the routine (the description dtor: malloc_strfree +0x34) at scope exit.
}

void FRoomShipPad::CreateForSaleShip(unsigned GoodId)
{
    // . 0x0044871d the routine (the description ctor).
    FDealerShipDesc Desc;
    // the pad's frame, as in the routine.
    Desc.Pos = Place->Frame.Pos;
    Desc.Orient = Place->Frame.Orient;
    // shadow on. 0x00448759..0x00448769: +0x38 = Place +0x08, the pad's y.
    Desc.bShadow = 1;
    Desc.Height = Place->Frame.Pos.y;
    // the package, its hull good (+0x90) and the hull's ship archetype
    // (+0x54); none of the three is null-tested.
    GoodInfo const* const Package = GoodList::find_by_id(GoodId);
    GoodInfo const* const Hull = GoodList::find_by_id(Package->HullGoodId);
    Desc.Ship = Archetype::GetShip(Hull->ShipArchId);
    // / 0x00448790: +0x3c = Package +0x94, the package's first addon list. baseline
    // hands the GoodInfo's own list; the port's GoodInfo keeps the addons in a vector
    // (common_goods.h), so they are copied into an EquipDescList first, as NN_Dealer does.
    EquipDescList Addons;
    Addons.Items.assign(Package->AddonLow.begin(), Package->AddonLow.end());
    Desc.Loadout = &Addons;
    // operator new(0x64); non-null -> the routine(pad +0x00, &Desc);
    // the result into +0x0c.
    ForSale = new FDealerShipObject(Room, Desc);
    // the description's (empty, ICF-shared) dtor at scope exit.
}

FRoomShipPadTable::FRoomShipPadTable()
    : Count(0), PlayerPad(-1)
{
    // The runtime's ctor calls the routine (Init) on this member; the vector itself is the
    // std::vector default state (the routine zeroes its three pointers).
}

void FRoomShipPadTable::Init()
{
    // +0x00 the allocator byte, +0x04 / +0x08 / +0x0c zero (the empty vector,
    // already so from its default construction), +0x10 = 0, +0x14 = -1.
    Count = 0;
    PlayerPad = -1;
}

FRoomShipPadTable::~FRoomShipPadTable()
{
    // erase(begin, end) -- nothing moves (0x00448910..0x00448946), each pad
    // destroyed through its element dtor the routine (inlined, 0x00448952..0x00448980), the
    // storage freed (the routine) and its three pointers zeroed. The port's pads have no
    // destructor of their own (the only resize, the routine, runs on an empty table), so the
    // element dtor runs here; std::vector frees the storage after this body.
    size_t Index = 0;
    for (Index = 0; Index < Pads.size(); ++Index)
        Pads[Index].Clear();
}

void FRoomShipPadTable::Build(FRoomRuntime* Runtime, FClientMarket* Market)
{
    // . 0x00448a7a..0x00448a94: +0x10 = RoomData +0x184's size (the routine).
    std::vector<ShipPlaceInfo*> const& Placements = Runtime->Room->ShipPlacements;
    Count = static_cast<int>(Placements.size());

    // resize to Count with a zeroed pad (the routine), then the
    // temporary's element dtor (the routine).
    FRoomShipPad Blank = {};
    Pads.resize(static_cast<size_t>(Count), Blank);
    Blank.Clear();

    // each pad gets the runtime and its placement.
    int Index = 0;
    for (Index = 0; Index < Count; ++Index)
    {
        Pads[Index].Room = Runtime;
        Pads[Index].Place = Placements[Index];
    }

    // +0x14 = the first pad with neither ship whose placement is the
    // player's (+0x34 == 1), else -1.
    int Found = -1;
    for (Index = 0; Index < Count; ++Index)
    {
        FRoomShipPad const& Pad = Pads[Index];
        if (Pad.Ship == nullptr && Pad.ForSale == nullptr && Pad.Place->bIsPlayerShip)
        {
            Found = Index;
            break;
        }
    }
    PlayerPad = Found;

    // with a player pad, a player ship archetype (the routine) and
    // "player_ship" enabled, the player's ship goes on it.
    if (PlayerPad != -1 && GetPlayerShipArchetypeIdbaseline() != 0 &&
        RtcSlider::is_enabled("player_ship"))
    {
        Pads[PlayerPad].CreatePlayerShip();
    }

    // the market's ship packages (category 3, the routine) into a
    // local list. the routine's result is passed on without a null test.
    std::list<FClientMarketGood const*> Packages;
    ClientMarketCollectCategory(Market, &Packages, 3);

    // each package on the first pad with neither ship whose placement
    // is for sale (+0x35 == 1), its GoodId (the entry's first dword) to the routine; no such
    // pad -> the package is skipped and the walk goes on.
    std::list<FClientMarketGood const*>::const_iterator It;
    for (It = Packages.begin(); It != Packages.end(); ++It)
    {
        int FreePad = -1;
        for (Index = 0; Index < Count; ++Index)
        {
            FRoomShipPad const& Pad = Pads[Index];
            if (Pad.Ship == nullptr && Pad.ForSale == nullptr && Pad.Place->bIsForSale)
            {
                FreePad = Index;
                break;
            }
        }
        if (FreePad != -1)
            Pads[FreePad].CreateForSaleShip((*It)->Info.GoodId);
    }
    // the list's nodes (the routine) and head freed -- ~list.
}

bool FRoomShipPadTable::HasLandingScript() const
{
    // +0x14 == -1 -> false; else the player pad's ShipPlaceInfo +0x38 non-null.
    if (PlayerPad == -1)
        return false;
    return Pads[PlayerPad].Place->LandingScript != nullptr;
}

bool FRoomShipPadTable::PlayLanding()
{
    // the field zero -> false; +0x14 == -1 -> false.
    if (G_RoomControlLandingPending == 0)
        return false;
    if (PlayerPad == -1)
        return false;
    // the player pad's landing script (+0x38) and ship (+0x08), both
    // non-null -> the routine(script) on the ship, true.
    FRoomShipPad const& Pad = Pads[PlayerPad];
    char const* const Script = Pad.Place->LandingScript;
    if (Script == nullptr)
        return false;
    if (Pad.Ship == nullptr)
        return false;
    Pad.Ship->PlayLandingScript(Script);
    return true;
}

bool RoomControlHasLandingScript(FRoomRuntime* Runtime)
{
    // ECX += 0x1020, JMP the routine.
    return Runtime->ShipPads.HasLandingScript();
}

void ClearRoomObjectList()
{
    // every node unlinked and freed, the size decremented per node.
    G_RoomObjects.clear();
}

// (thiscall, no arguments) -- start every RoomData animation found in the room's
// main script: play it on the main instance into +0x64 and evaluate it at -2 (looped with
// flag 2 when RoomData+0x94 marks it); FDUMP an error for a name the script does not have.
static void RoomControlStartAnimations(FRoomRuntime* Runtime)
{
    RoomData* Room = Runtime->Room;
    int Index = 0;
    for (Index = 0; Index < Room->AnimationCount; ++Index)
    {
        // the routine on the name.
        char const* Name = Room->Animations[Index];
        int const Script = Runtime->Scripts.Find(Name);
        if (Script == -1)
        {
            // .
            FDUMP(0x100001, "%s(%d) : *** ERROR: animation name %s is not present in room mesh",
                  kRoomControlCpp, 0xed, Name);
            continue;
        }
        // the routine on +0x44.
        unsigned long const Playback = Runtime->Scripts.Play(Runtime->MainInstance, Script);
        Runtime->Handles64[Index] = Playback;
        // Anim +0x2c (Playback, loop ? 2 : 0, -2.0, 1.0, 0, 1.0, 1.0,
        // 0).
        unsigned long const Flags = Room->AnimationLoop[Index] ? 2u : 0u;
        NeuronRuntime::Anim->EvalPlayback(Playback, Flags, -2.0f, 1.0f, AsOpaqueDword(0),
                                  AsOpaqueDword(1.0f), AsOpaqueDword(1.0f), AsOpaqueDword(0));
    }
}

namespace
{
    // .rdata 0x005cc01c -- the source path RoomControlHotspot.cpp hands FDUMP.
    char const kRoomControlHotspotCpp[] =
        "Client\\Gf\\RoomControlHotspot.cpp";
}

// (thiscall) -- one room button per RoomData hotspot (+0x40's vector at +0x194,
// its size re-read every pass) by the hotspot's behavior, appended at +0x1134 with its location
// (the routine = MakeLocationID(+0x34, room_switch), 0 without a room_switch) and behavior
// (the routine). No bound on the 0x80 slots, as in baseline. Caller: the routine (0x00449841).
static void RoomControlBuildButtons(FRoomRuntime* Runtime)
{
    // +0x13a8 = 0, +0x1334 = 0.
    Runtime->Field_0x13a8 = 0;
    Runtime->ControllerCount = 0;

    RoomData* const Room = Runtime->Room;
    size_t Index = 0;
    for (Index = 0; Index < Room->Hotspots.size(); ++Index)
    {
        RoomButtonInfo const* const Info = Room->Hotspots[Index];
        int const Behavior = Info->Behavior;
        // the next slot cleared before the type test.
        Runtime->Controllers[Runtime->ControllerCount] = nullptr;

        RoomButton* Button = nullptr;
        switch (Behavior)
        {
        case 1:
            // operator_new(0x18), the routine, vftable 0x005cc068,
            // +0x10 = +0x0c = 0.
            Button = new ExitDoorButton();
            break;
        case 2:
            // operator_new(0xc), the routine.
            Button = new NewsVendorButton();
            break;
        case 3:
            // operator_new(0xc), the routine.
            Button = new MissionVendorButton();
            break;
        case 4:
            // operator_new(0x18), the routine, +0x0c = 0, vftable
            // , +0x10 = 1.
            Button = new LaunchButton();
            break;
        case 5:
        case 6:
            // / 0x0044d0b2..0x0044d0ce: operator_new(0xc), the routine.
            Button = new RoomButton();
            break;
        case 7:
        case 8:
        case 9:
        case 0xa:
        case 0xb:
        case 0xc:
            // no button, the count unchanged.
            continue;
        default:
            // FDUMP(0x100002, ..., RoomControlHotspot.cpp, 0x5b, type).
            FDUMP(0x100002, "%s(%d) : *** WARNING: Unknown button behavior type [%d]",
                  kRoomControlHotspotCpp, 0x5b, Behavior);
            continue;
        }
        Runtime->Controllers[Runtime->ControllerCount] = Button;

        // the hotspot's room_switch (+0x04) as a location of this
        // room's base, 0 when it has none.
        unsigned Location = 0;
        if (Info->RoomSwitch != nullptr)
            Location = MakeLocationID(Runtime->BaseId, Info->RoomSwitch);
        // the routine(Location, Behavior) on the new slot.
        Runtime->Controllers[Runtime->ControllerCount]->Set(Location, Behavior);
        // +0x1334 + 1.
        ++Runtime->ControllerCount;
    }
}

// (thiscall, no arguments) -- one FlashLight per resolved frame of every RoomData
// FlashLightSet record (RoomData+0x1a4), appended to +0x81c / +0x101c with no bound check. The
// lights of one set share a period and are staggered by "gap" along it.
static void RoomControlLoadFlashlights(FRoomRuntime* Runtime)
{
    // nothing when the vector has no storage or is empty.
    // the size is re-read from +0x40 every iteration.
    int SetIndex = 0;
    for (SetIndex = 0; SetIndex < static_cast<int>(Runtime->Room->FlashLightSets.size());
         ++SetIndex)
    {
        FlashLightSetInfo* Set = Runtime->Room->FlashLightSets[SetIndex];
        // the resolved frame count (+0xe28), JLE skips the set.
        int const Count = static_cast<int>(Set->Cursor);
        if (Count <= 0)
            continue;

        // Period = (Count - 1) * gap (+0x10) + endpause (+0x18), FILD
        // then FMUL / FADD float, FSTP float.
        float const Period = static_cast<float>(
            static_cast<double>(Count - 1) * static_cast<double>(Set->Param4) +
            static_cast<double>(Set->Param6));
        // / 0x0044a442..0x0044a45a: Step 0; FCOMP double the field (0.0),
        // TEST AH,0x41 + JNZ skips when not greater -- a positive period gives
        // Step = gap / Period (FDIV float, FSTP float).
        float Step = 0.0f;
        if (Period > 0.0)
            Step = static_cast<float>(static_cast<double>(Set->Param4) /
                                      static_cast<double>(Period));

        // MinColor is a zeroed local.
        Vector const Black = {0.0f, 0.0f, 0.0f};
        // the count (+0xe28) is re-read after every light.
        int LightIndex = 0;
        for (LightIndex = 0; LightIndex < static_cast<int>(Set->Cursor); ++LightIndex)
        {
            // operator new(0x7c), the routine(&color (+0x00), &Black,
            // Period, -(LightIndex * Step), blink (+0x14),
            // (float)(scale (+0x0c) * (0.25)), scale).
            float const Phase =
                static_cast<float>(-(static_cast<double>(LightIndex) * static_cast<double>(Step)));
            float const BulbSize = static_cast<float>(static_cast<double>(Set->Param3) * 0.25);
            FFlashLight* Light = new FFlashLight(reinterpret_cast<Vector const*>(&Set->Param0),
                                                 &Black, Period, Phase, Set->Param5, BulbSize,
                                                 Set->Param3);
            // stored at +0x81c[+0x101c], the routine on the set's
            // resolved frame (+0xe2c + LightIndex * 0x30), then the count raised.
            Runtime->Flashlights[Runtime->FlashlightCount] = Light;
            Runtime->Flashlights[Runtime->FlashlightCount]->SetFrame(Set->FinalFrames[LightIndex]);
            ++Runtime->FlashlightCount;
        }
    }
}

FRoomRuntime* CreateRoomRuntime(RoomData* Room)
{
    // operator new(0x13ac) then the routine. baseline leaves the bytes the routine does not
    // write indeterminate; the port hands the ctor a zero-filled block instead.
    void* Memory = operator new(sizeof(FRoomRuntime));
    memset(Memory, 0, sizeof(FRoomRuntime));
    FRoomRuntime* Runtime = new (Memory) FRoomRuntime();

    // (BaseId, RoomId). 0x004496bf / 0x004496c8: +0x38 / +0x34.
    // the routine(BaseId) on the field into +0x3c.
    // +0x40 = the RoomData the routine(RoomId) finds -- the caller
    // already holds it.
    Runtime->RoomId = Room->RoomId;
    Runtime->BaseId = Room->BaseId;
    Runtime->LocationBase = LookupClientLocationBase(Runtime->BaseId);
    Runtime->Room = Room;
    // .
    Runtime->MainInstance = kRoomObjectNoInstance;
    Runtime->MainScript = 0xffffffffu;
    // the routine; 0x004496f4..0x004496fb RoomData::fixup(+0x44).
    RoomControlLoadProps(Runtime);
    Room->fixup(reinterpret_cast<long>(Runtime->MainInstance));
    // the routine on +0x4c with +0x48 unless -1.
    if (Runtime->MainScript != 0xffffffffu)
        Runtime->Scripts.Load(Runtime->MainScript);
    // the routine.
    RoomControlStartAnimations(Runtime);

    // 0x00449719..0x0044978b: one the routine wrapper per RoomData+0x144 light
    // (Frame, +0x3c D3DLIGHT8, +0x30 target, +0xa4 group) into +0xe4, no bound check; the
    // count is the vector's size.
    int LightIndex = 0;
    for (LightIndex = 0; LightIndex < static_cast<int>(Room->Lights.size()); ++LightIndex)
    {
        RoomLightInfo* Light = Room->Lights[LightIndex];
        Runtime->RoomLights[LightIndex] = GfLightWrapperCreate(
            &Light->Frame.Pos.x, reinterpret_cast<FD3dLight8 const*>(&Light->Mode),
            Light->Direction, Light->Type);
    }
    Runtime->RoomLightCount = static_cast<int>(Room->Lights.size());

    // 0x00449791: the routine into +0x500, then the RoomData camera
    // (0x0044979c..0x00449800).
    FRoomCamera* Cam = RoomCameraCreate();
    Runtime->Camera = Cam;
    InstallRoomDataCamera(Cam, Room->Camera);

    // the routine on +0x504 with RoomData+0x174.
    BuildCharacterPlacements(Runtime->Placements, Room->CharacterPlacements);

    // the routine -- the room FlashLights into +0x81c.
    RoomControlLoadFlashlights(Runtime);

    // the routine on +0x1020 with this and the market of the room's
    // base (the routine(+0x34) on the field, not null-tested).
    Runtime->ShipPads.Build(Runtime, GetClientLocationBaseMarket(Runtime->BaseId));

    // the routine -- the room buttons.
    RoomControlBuildButtons(Runtime);

    // the routine(0, (float)(1.0 / the field)) -- the fade opened
    // at the speed of its current duration (the field is 1.0, FDIVR double, FSTP float).
    SetFadeTarget(0.0f, static_cast<float>(1.0 / static_cast<double>(GetFadeDuration())));
    // the routine(0.0f, 3.0f) -- the letterbox bars away.
    SetLetterboxTarget(0.0f, 3.0f);
    // the routine(1) -- the software cursor drawn.
    SetClientMouseCursorDrawFlag(1);
    // the routine(0xf) on the +0x30 face.
    Runtime->RegisterEvent(0xf);
    // the routine("arrow", 0).
    MouseSetCursor("arrow", false);
    // / 0x00449891 / 0x00449896: the routine, the routine, the routine.
    HoldGameflowTime();
    ClearHoveredHotspot();
    UnlockHotspotInput();
    return Runtime;
}

void DestroyRoomRuntime(FRoomRuntime* Runtime)
{
#ifdef OUTPOST_FRONTIER_WALK_MODE
    BaseWalkRoomDestroyed(Runtime);
#endif
    // (the deleting destructor: the routine, then free). the routine is the
    // member destructor (this = ESI, no null test); the three callers in menu_universe_base.cpp
    // (UnloadLocationBase, the shutdown walk, and UnloadClientLocationRoomState = the routine)
    // test the pointer before the delete.
    delete Runtime;
}

ICamera* GetRoomRuntimeCamera(FRoomRuntime* Runtime)
{
    return Runtime != nullptr ? Runtime->Camera : nullptr;
}

FRoomCamera* GetRoomRuntimeRoomCamera(FRoomRuntime* Runtime)
{
    return Runtime != nullptr ? Runtime->Camera : nullptr;
}

unsigned GetRoomRuntimeByteSize()
{
    return static_cast<unsigned>(sizeof(FRoomRuntime));
}

// (thiscall, RET 4; the camera argument is never read). Rebuilds the per-frame
// light list at +0x2e8 when the LIGHTING option is on.
static void CollectRoomLights(FRoomRuntime* Runtime, ICamera* /*Camera*/)
{
    if (GetGameOption(kGameOptLighting) == 0)
        return;

    if (Runtime->bSingleLight != 0)
    {
        // no ambient, only the room light SingleLightIndex.
        Runtime->FrameAmbient[0] = 0.0f;
        Runtime->FrameAmbient[1] = 0.0f;
        Runtime->FrameAmbient[2] = 0.0f;
        Runtime->FrameLights[0] = Runtime->RoomLights[Runtime->SingleLightIndex];
        Runtime->FrameLightCount = 1;
        return;
    }

    // the room's ambient colour, RoomData +0xd8..+0xe0.
    Runtime->FrameAmbient[0] = Runtime->Room->AmbientColor.x;
    Runtime->FrameAmbient[1] = Runtime->Room->AmbientColor.y;
    Runtime->FrameAmbient[2] = Runtime->Room->AmbientColor.z;
    Runtime->FrameLightCount = 0;

    // the room's own lights that hold an engine light, up to 0x80.
    int Index = 0;
    for (Index = 0; Index < Runtime->RoomLightCount; ++Index)
    {
        FGfLightWrapper* Wrapper = Runtime->RoomLights[Index];
        if (Wrapper == nullptr || Wrapper->Light == nullptr)
            continue;
        if (Runtime->FrameLightCount >= 0x80)
            continue;
        Runtime->FrameLights[Runtime->FrameLightCount] = Wrapper;
        ++Runtime->FrameLightCount;
    }

    // then every enabled script light (the field), same filter.
    const int Count = GfLightRegistryCount();
    for (Index = 0; Index < Count; ++Index)
    {
        FGfLightWrapper* Wrapper = GfLightRegistryGet(Index);
        if (Wrapper == nullptr || Wrapper->Light == nullptr)
            continue;
        if (Runtime->FrameLightCount >= 0x80)
            continue;
        Runtime->FrameLights[Runtime->FrameLightCount] = Wrapper;
        ++Runtime->FrameLightCount;
    }
}

int const* FRoomObject::GetAmbientOverride() const
{
    // .
    if (AmbientSource == nullptr)
        return nullptr;
    return reinterpret_cast<int const*>(static_cast<unsigned char const*>(AmbientSource) + 0x6c);
}

// ---------------------------------------------------------------------------
// The material-batcher sink (Client\cmn\MaterialLib.cpp unit).
// ---------------------------------------------------------------------------

namespace
{
    // raised by the routine, dropped by the routine; nothing reads it.
    unsigned char G_RoomBatchActive = 0;

    // the one IMaterialBatcherEvents the exe implements: vptr 0x005c9230 and a
    // reference count the field, both stored by the static initializer the branch. While a
    // room's slot-7 pass runs it is the batcher's observer, and every queued record it flushes
    // relights its object (the routine) from the room and object stored with the record.
    struct FRoomBatcherSink final : public IMaterialBatcherEvents
    {
        unsigned long RefCount; // +0x04 the field

        FRoomBatcherSink()
            : RefCount(1)
        {
        }

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            // one entry in the table at 0x005c9268 -- "1.11_IMaterialBatcherEvents"
            // at offset 0 -- then the terminator.
            if (strcmp(Iid, Iids::MaterialBatcherEvents) == 0)
            {
                AddRef();
                *Out = static_cast<IMaterialBatcherEvents*>(this);
                return GR_OK;
            }
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }

        unsigned long NEURONCOMPONENTS_API AddRef() override
        {
            // .
            ++RefCount;
            return RefCount;
        }

        unsigned long NEURONCOMPONENTS_API Release() override
        {
            // (ICF-shared with FScriptCamSink::Release): decrement while above zero
            // (JBE); at zero reset the count to 1, free this (the routine) and return 0. baseline
            // runs the same body on this static (the field), whose count starts at 1 and only
            // sees the batcher's balanced AddRef / Release pairs (slot 14), so the free is never
            // reached there -- it is kept exactly as the listing has it, not dropped.
            if (RefCount > 0)
                --RefCount;
            if (RefCount != 0)
                return RefCount;
            RefCount = 1;
            free(this);
            return 0;
        }

        void NEURONCOMPONENTS_API BeginFlush(unsigned char /*Clear*/) override
        {
            // XOR EAX,EAX / RET 8.
        }

        void NEURONCOMPONENTS_API EndFlush(unsigned char /*Clear*/) override
        {
            // XOR EAX,EAX / RET 8.
        }

        void NEURONCOMPONENTS_API BeginRecord(void* /*Material*/, void* FieldA, void* FieldB) override;

        void NEURONCOMPONENTS_API EndRecord(void* /*Material*/, void* /*FieldA*/, void* /*FieldB*/) override
        {
            // XOR EAX,EAX / RET 0x10.
        }
    };

    FRoomBatcherSink G_RoomBatcherSink;

    // observer on, the room pass flag up.
    void RoomBatcherBegin()
    {
        IMaterialBatcher2* Batcher = NeuronRuntime::MaterialBatcher;
        if (Batcher == nullptr)
            return;
        G_RoomBatchActive = 1;
        Batcher->SetBinOrder(1, 1);
        Batcher->SetEvents(static_cast<IMaterialBatcherEvents*>(&G_RoomBatcherSink));
    }

    // flag down, flush every bin (so the sink sees each record), observer off.
    void RoomBatcherEnd()
    {
        IMaterialBatcher2* Batcher = NeuronRuntime::MaterialBatcher;
        if (Batcher == nullptr)
            return;
        G_RoomBatchActive = 0;
        Batcher->FlushAll(1);
        Batcher->SetEvents(nullptr);
        Batcher->SetBinOrder(1, -1);
    }

    // (room, object) -- tag the records queued from here on.
    void RoomBatcherTagObject(FRoomRuntime* Room, FRoomObject* Object)
    {
        IMaterialBatcher2* Batcher = NeuronRuntime::MaterialBatcher;
        if (Batcher == nullptr)
            return;
        Batcher->SetRecordTag(Object);
        Batcher->SetRecordTagContext(Room);
    }

    // untag.
    void RoomBatcherUntagObject()
    {
        IMaterialBatcher2* Batcher = NeuronRuntime::MaterialBatcher;
        if (Batcher == nullptr)
            return;
        Batcher->SetRecordTag(nullptr);
        Batcher->SetRecordTagContext(nullptr);
    }

    // (thiscall on the room, RET 4) -- fog enable, scene ambient and the lights
    // for one object.
    void ApplyObjectLighting(FRoomRuntime* Room, FRoomObject* Object)
    {
        // D3DRS_FOGENABLE = room fog active (the routine) and +0x12.
        int const bFog = (GfFogIsActive() && Object->bFog != 0) ? 1 : 0;
        NeuronRuntime::Pipeline->SetRenderState(D3DRS_FOGENABLE, bFog);

        // black when slot 2 says hidden, else the object's own ambient
        // (three ints * 1/255, the field) or the room's frame ambient +0x4ec.
        if (!Object->IsVisible())
        {
            GfSetSceneAmbient(0.0f, 0.0f, 0.0f);
        }
        else
        {
            int const* Override = Object->GetAmbientOverride();
            if (Override == nullptr)
            {
                GfSetSceneAmbient(Room->FrameAmbient[0], Room->FrameAmbient[1],
                                  Room->FrameAmbient[2]);
            }
            else
            {
                const double kInv255 = 1.0 / 255.0;
                GfSetSceneAmbient(static_cast<float>(static_cast<double>(Override[0]) * kInv255),
                                  static_cast<float>(static_cast<double>(Override[1]) * kInv255),
                                  static_cast<float>(static_cast<double>(Override[2]) * kInv255));
            }
        }

        // lit objects take the best four frame lights of their group
        // (the routine), unlit ones clear the light list (thunk the routine).
        if (Object->bLit != 0)
        {
            void* const Instance = Object->GetEngineInstance();
            GfLightsApplyNearest(Room->FrameLights, Room->FrameLightCount, Instance,
                                 Object->LightGroup, Room->Camera);
            return;
        }
        EngineLightClearRegisteredList();
    }

    void NEURONCOMPONENTS_API FRoomBatcherSink::BeginRecord(void* /*Material*/, void* FieldA, void* FieldB)
    {
        // fog off, then the routine(room = FieldB, object = FieldA) when an
        // object was tagged.
        NeuronRuntime::Pipeline->SetRenderState(D3DRS_FOGENABLE, 0);
        if (FieldA != nullptr)
            ApplyObjectLighting(static_cast<FRoomRuntime*>(FieldB),
                                static_cast<FRoomObject*>(FieldA));
    }

    // is DrawFlashlightFlares (flash_light.cpp).

    // The the routine layer walk: every object of Room in the field order whose layer byte
    // is Layer.
    template <typename FVisit>
    void ForEachRoomObjectInLayer(FRoomRuntime* Room, unsigned char Layer, FVisit Visit)
    {
        std::list<FRoomObject*>::iterator It = G_RoomObjects.begin();
        for (; It != G_RoomObjects.end(); ++It)
        {
            FRoomObject* Object = *It;
            if (Object->Owner != Room)
                continue;
            if (Object->Layer != Layer)
                continue;
            Visit(Object);
        }
    }
}

void RoomControlDrawObject(FRoomRuntime* Room, FRoomObject* Object)
{
    // . 0x0044af7e..0x0044af91: nothing without an engine instance.
    void* const Instance = Object->GetEngineInstance();
    if (Instance == kRoomObjectNoInstance)
        return;

    // the routine.
    ApplyObjectLighting(Room, Object);

    // with "shadow_glare" and the room's glare light (RoomData +0x10c)
    // the appearance gets the glare direction and colour through slot 6; otherwise slot 7(0).
    RoomData* Data = Room->Room;
    if (RtcSlider::is_enabled("shadow_glare") && Data->bGlareLight)
    {
        // Engine slot 36 (+0x90): the instance position.
        float const* InstancePos = NeuronRuntime::Engine->GetInstancePosition(Instance);
        float Pos[3] = {InstancePos[0], InstancePos[1], InstancePos[2]};
        float Direction[3];
        if (Data->bGlareLightWorldSpaceDir)
        {
            // +0x10d: the world-space direction (+0x11c) as it is.
            Direction[0] = Data->GlareLightDirection.x;
            Direction[1] = Data->GlareLightDirection.y;
            Direction[2] = Data->GlareLightDirection.z;
        }
        else
        {
            // (out, instance position, light position +0x110), the routine.
            Direction[0] = Pos[0] - Data->GlareLightPosition.x;
            Direction[1] = Pos[1] - Data->GlareLightPosition.y;
            Direction[2] = Pos[2] - Data->GlareLightPosition.z;
            RoomVec3Normalize(Direction);
        }
        float Color[3] = {Data->GlareLightColor.x, Data->GlareLightColor.y,
                          Data->GlareLightColor.z};
        IFLAppearance* Appearance = nullptr;
        if (NeuronRuntime::Engine->QueryInterface(Iids::FLAppearance,
                                          reinterpret_cast<void**>(&Appearance)) >= 0)
            Appearance->SetDirectionalLight(Direction, Color);
        // release and clear.
        if (Appearance != nullptr)
            Appearance->Release();
    }
    else
    {
        IFLAppearance* Appearance = nullptr;
        if (NeuronRuntime::Engine->QueryInterface(Iids::FLAppearance,
                                          reinterpret_cast<void**>(&Appearance)) >= 0)
            Appearance->SetDirectionalLightEnabled(0);
        if (Appearance != nullptr)
            Appearance->Release();
    }

    // the render mode at +0x18.
    int const Mode = Object->RenderMode;
    if (Mode == 0)
    {
        // envmap only with +0x13 and a positive perf slot 0
        // (the routine(0) > 0.0f, the field): appearance slot 8(4, &value) and, when that
        // succeeds, slot 3(instance, "envmapbasic").
        void* EnvValue = nullptr;
        if (Object->bEnvMap != 0 && GetPerfOptionSlot(0) > 0.0f)
        {
            IFLAppearance* Appearance = nullptr;
            bool bHaveEnv = false;
            if (NeuronRuntime::Engine->QueryInterface(Iids::FLAppearance,
                                              reinterpret_cast<void**>(&Appearance)) >= 0)
            {
                bHaveEnv = Appearance->GetOverrideMaterial(4, &EnvValue) >= 0;
                Appearance->Release();
                Appearance = nullptr;
                if (bHaveEnv)
                {
                    void* const EnvInstance = Object->GetEngineInstance();
                    if (NeuronRuntime::Engine->QueryInterface(Iids::FLAppearance,
                                                      reinterpret_cast<void**>(&Appearance)) >= 0)
                    {
                        Appearance->SetInstanceValue(static_cast<int>(reinterpret_cast<intptr_t>(EnvInstance)),
                                          const_cast<char*>("envmapbasic"));
                    }
                    if (Appearance != nullptr)
                        Appearance->Release();
                }
            }
        }
        // renderer slot 7 (camera, instance, 1.0f, 8, 0, env value).
        NeuronRuntime::Renderer->RenderInstance(Room->Camera, Instance, 1.0f, 8u, nullptr, EnvValue);
    }
    else if (Mode == 1)
    {
        // appearance slot 10 (+0x28) (camera, instance); the QI result
        // is tested for exactly 0 here.
        IFLAppearance* Appearance = nullptr;
        if (NeuronRuntime::Engine->QueryInterface(Iids::FLAppearance,
                                          reinterpret_cast<void**>(&Appearance)) == 0)
            Appearance->DrawStarsphere(Room->Camera, Instance);
        if (Appearance != nullptr)
            Appearance->Release();
    }
    else if (Mode == 2)
    {
        // appearance slot 9 (+0x24) (camera, instance).
        IFLAppearance* Appearance = nullptr;
        if (NeuronRuntime::Engine->QueryInterface(Iids::FLAppearance,
                                          reinterpret_cast<void**>(&Appearance)) == 0)
            Appearance->DrawBackdrop(Room->Camera, Instance);
        if (Appearance != nullptr)
            Appearance->Release();
    }
    else
    {
        // FDUMP 0x100002, RoomControl.cpp line 0x6d1.
        FDUMP(0x100002, "%s(%d) : *** WARNING: Invalid render type %d\n", kRoomControlCpp,
              0x6d1, Mode);
    }

    // appearance slot 7(0) again.
    IFLAppearance* Appearance = nullptr;
    if (NeuronRuntime::Engine->QueryInterface(Iids::FLAppearance,
                                      reinterpret_cast<void**>(&Appearance)) >= 0)
        Appearance->SetDirectionalLightEnabled(0);
    if (Appearance != nullptr)
        Appearance->Release();
}

void RoomControlDrawShadow(FRoomRuntime* Room, unsigned long A, unsigned long B,
                           unsigned char bFlag)
{
    // . 0x0044b389..0x0044b3be: the SHADOWS option (0x2b), "shadow_glare" and the
    // room's shadow light (RoomData +0xe4).
    if (GetGameOption(kGameOptShadows) == 0)
        return;
    if (!RtcSlider::is_enabled("shadow_glare"))
        return;
    if (!Room->Room->bShadowLight)
        return;
    IFLAppearance* Appearance = nullptr;
    if (NeuronRuntime::Engine->QueryInterface(Iids::FLAppearance,
                                      reinterpret_cast<void**>(&Appearance)) >= 0)
    {
        // {A, B} on the stack; slot 12 (+0x30) with bFlag, else
        // slot 11 (+0x2c), both (camera, &pair, 1, -1).
        unsigned long Pair[2] = {A, B};
        if (bFlag != 0)
            Appearance->DrawFixedShadows(Room->Camera, Pair, reinterpret_cast<void*>(1),
                               reinterpret_cast<void*>(static_cast<intptr_t>(-1)));
        else
            Appearance->DrawFadingShadows(Room->Camera, Pair, reinterpret_cast<void*>(1),
                               reinterpret_cast<void*>(static_cast<intptr_t>(-1)));
    }
    if (Appearance != nullptr)
        Appearance->Release();
}

void RoomControlLightObject(FRoomRuntime* Room, FRoomObject* Object)
{
    // (the body is ApplyObjectLighting above).
    ApplyObjectLighting(Room, Object);
}

void RoomControlDraw(FRoomRuntime* Runtime)
{
    // . 0x0044b58e: the routine.
    RoomBatcherBegin();

    // viewport (RoomControl.cpp line 0x748), the routine camera,
    // ZFUNC 2, depth test, depth write, cull, blend, SRCALPHA / INVSRCALPHA.
    Win3DSetViewport(Runtime->Camera, kRoomControlCpp, 0x748);
    Win3DApplyCamera(Runtime->Camera);
    Win3DSetDepthFunc(2);
    Win3DEnable(kWin3DDepthTest);
    Win3DSetDepthWrite(1);
    Win3DEnable(kWin3DCullFace);
    Win3DEnable(kWin3DBlend);
    Win3DSetBlendFunc(kWin3DBlendSrcAlpha, kWin3DBlendInvSrcAlpha);

    // the routine (frame lights), the routine (room fog).
    CollectRoomLights(Runtime, Runtime->Camera);
    GfFogApply();

    // collect layer bytes, ordered as signed chars.
    // uses CMP / SETL (0044c546..0044c54b), so negative background
    // layers precede layer 0 in all three draw passes.
    std::set<signed char> Layers;
    std::list<FRoomObject*>::iterator It = G_RoomObjects.begin();
    for (; It != G_RoomObjects.end(); ++It)
    {
        if ((*It)->Owner == Runtime)
            Layers.insert(static_cast<signed char>((*It)->Layer));
    }

    // slot 7, each object tagged into the batcher records.
    std::set<signed char>::iterator LayerIt = Layers.begin();
    for (; LayerIt != Layers.end(); ++LayerIt)
    {
        ForEachRoomObjectInLayer(Runtime, static_cast<unsigned char>(*LayerIt), [Runtime](FRoomObject* Object) {
            RoomBatcherTagObject(Runtime, Object);
            Object->DrawOpaque(Runtime->Camera);
            RoomBatcherUntagObject();
        });
    }
    // the routine.
    RoomBatcherEnd();

    // slot 8.
    for (LayerIt = Layers.begin(); LayerIt != Layers.end(); ++LayerIt)
    {
        ForEachRoomObjectInLayer(Runtime, static_cast<unsigned char>(*LayerIt), [Runtime](FRoomObject* Object) {
            Object->DrawTranslucent(Runtime->Camera);
        });
    }

    // EFFECTS on and FLASHLIGHTS > 0 -> the routine(+0x81c, +0x101c,
    // camera).
    if (GetGameOption(kGameOptEffectsGroup) != 0 && GetGameOption(kGameOptFlashlights) > 0)
        DrawFlashlightFlares(Runtime->Flashlights, Runtime->FlashlightCount, Runtime->Camera);

    // the same states again, full-white ambient (the routine),
    // every light unlinked (thunk the routine).
    Win3DSetDepthFunc(2);
    Win3DEnable(kWin3DDepthTest);
    Win3DSetDepthWrite(1);
    Win3DEnable(kWin3DCullFace);
    Win3DEnable(kWin3DBlend);
    Win3DSetBlendFunc(kWin3DBlendSrcAlpha, kWin3DBlendInvSrcAlpha);
    GfSetSceneAmbient(1.0f, 1.0f, 1.0f);
    EngineLightClearRegisteredList();

    // D3DRS_DITHERENABLE (0x1a) saved, then 0.
    IRenderPipeline* Pipe = NeuronRuntime::Pipeline;
    int SavedDither = 0;
    Pipe->GetRenderState(D3DRS_DITHERENABLE, &SavedDither);
    Pipe->SetRenderState(D3DRS_DITHERENABLE, 0);

    // ParticleFx slot 29 (+0x74) BeginDraw; slot 9 per layer, the cue
    // list the routine, slot 30 (+0x78) EndDraw.
    if (NeuronRuntime::ParticleFx->BeginDraw() >= 0)
    {
        for (LayerIt = Layers.begin(); LayerIt != Layers.end(); ++LayerIt)
        {
            ForEachRoomObjectInLayer(Runtime, static_cast<unsigned char>(*LayerIt), [Runtime](FRoomObject* Object) {
                Object->DrawEffects(Runtime->Camera);
            });
        }
        G_GfScriptManager.DrawEffects(Runtime, Runtime->Camera);
        NeuronRuntime::ParticleFx->EndDraw();
    }

    // dither restored; the set is freed on return.
    Pipe->SetRenderState(D3DRS_DITHERENABLE, SavedDither);
}
