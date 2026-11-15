// Author: Giperion.

// HUD_Waypoint -- see hud_waypoint.h for the class, its vftables and the named gaps.

#include "hud_waypoint.h"

#include "best_path.h"
#include "character_select_equip.h" // GetPlayerStarSystemIdbaseline (the routine)
#include "client_gameobj.h"         // FClientGameobj, FClientSolarGameobj (the routine)
#include "client_objdb.h"           // ClientObjDb_RegisterGameobj (the routine)
#include "game_options.h"           // GetGameOption (the routine)
#include "hud_command_dispatch.h"   // FSpaceCommand, SpaceSendCommand (the routine)
#include "input_manager.h"          // GetClientMouseCursorDrawFlag (the routine)
#include "nn_messaging.h"           // NnVoiceEnqueue (the routine)
#include "player_behavior.h"        // GetPlayerObjRW (the routine)
#include "player_spawn.h"           // the player waypoint list, GetPlayerConnectionId
#include "star_sphere.h"            // Win3DIdentityRotation3x3 (the routine)

#include "common_archetype.h"
#include "common_cobject.h"
#include "common_iface.h"
#include "common_id.h"              // CreateID
#include "common_misc.h"            // SUBOBJ_ID_NONE
#include "common_reputation.h"      // Reputation::Vibe
#include "common_text.h"            // FmtStr
#include "common_universe.h"        // Archetype::GetSolarID

#include "neuroncomponents.h"                  // FDUMP

#include <math.h>
#include <stdio.h>
#include <string.h>

static_assert(sizeof(HUD_Waypoint) == 0x390, "baseline HUD_Waypoint is 0x390 bytes");

namespace
{
    // .
    HUD_Waypoint* G_HudWaypoint = nullptr;

    // (static initializer): the field = CreateID("objective_reached").
    unsigned const G_VoiceObjectiveReached = CreateID("objective_reached");

    // 15.0f, the field 100.0f, the field 0.01f, the field 150.0f,
    // 16.0f, the field 300.0f, the field 250.0f, the field pi / 180
    // (float), the field 2.0 (double).
    float const kScaleZeroDistance = 15.0f;
    float const kScaleFullDistance = 100.0f;
    float const kScalePerMeter = 0.01f;
    float const kReachedDistance = 150.0f;
    float const kBestPathMatchSq = 16.0f;
    float const kUserPickDistance = 300.0f;
    float const kSpinDegPerSecond = 250.0f;
    float const kDegToRad = 0.0174532924f;
    double const kSameSpotAxis = 2.0;

    // (Id, 0) -- the NN voice queue (nn_messaging.cpp).
    void QueueWaypointVoice(unsigned VoiceId)
    {
        NnVoiceEnqueue(VoiceId, 0);
    }

    IObjRW* WatchedObjRw(BaseWatcher const& Watcher)
    {
        return ObjRwFromWatchable(const_cast<Watchable*>(Watcher.target()));
    }

    // The client gameobj behind a watched IObjRW (baseline calls its extension slots straight
    // through the IObjRW vftable).
    FClientGameobj* AsGameobj(IObjRW* Obj)
    {
        return static_cast<FClientGameobj*>(static_cast<IObjInspectImpl*>(Obj));
    }

    // The release sequence the listing inlines (0x004f3f7a..0x004f3fb1, 0x004f40b2..0x004f40e9,
    // ): with a watched waypoint, its slot 87 (+0x15c) with 1, then the
    // watcher dropped.
    void ReleaseWaypointObject(BaseWatcher& Watcher)
    {
        IObjRW* const Obj = WatchedObjRw(Watcher);
        if (Obj == nullptr)
            return;
        AsGameobj(Obj)->Slot15c(true);
        Watcher.set(nullptr);
    }

    // Command 0x1c (target) on object ObjectId: a zeroed 0x28-byte record, +0x00 = 0x1c,
    // +0x10 = the id, +0x14 = SUBOBJ_ID_NONE, +0x26 = 1, through the routine.
    void SendTargetCommand(unsigned ObjectId)
    {
        FSpaceCommand Command = {};
        Command.Id = 0x1c;
        Command.ObjectId = ObjectId;
        Command.SubObjectId = SUBOBJ_ID_NONE;
        Command.Byte26 = 1;
        SpaceSendCommand(&Command);
    }

    // The FPU distance the listing builds from two positions: the three differences A - B,
    // then sqrt(dz*dz + dy*dy + dx*dx) in that order.
    double Distance(Vector const& A, Vector const& B)
    {
        double const Dx = static_cast<double>(A.x) - static_cast<double>(B.x);
        double const Dy = static_cast<double>(A.y) - static_cast<double>(B.y);
        double const Dz = static_cast<double>(A.z) - static_cast<double>(B.z);
        return sqrt(Dz * Dz + Dy * Dy + Dx * Dx);
    }

    // The same distance when the listing loads z first (SetWaypoint 0x004f480c..0x004f482c):
    // sqrt(dx*dx + dy*dy + dz*dz).
    double DistanceXFirst(Vector const& A, Vector const& B)
    {
        double const Dx = static_cast<double>(A.x) - static_cast<double>(B.x);
        double const Dy = static_cast<double>(A.y) - static_cast<double>(B.y);
        double const Dz = static_cast<double>(A.z) - static_cast<double>(B.z);
        return sqrt(Dx * Dx + Dy * Dy + Dz * Dz);
    }
}

HUD_Waypoint* GetHudWaypoint()
{
    return G_HudWaypoint;
}

HUD_Waypoint::HUD_Waypoint(INI_Reader& /*Reader*/)
    : SpawnCounter(0)
    , SpinAngle(0.0f)
    , bUser(0)
    , Index(0)
    , Scale(1.0f)
    , bReached(0)
    , Id(-1)
    , Field388(0)
    , bActive(0)
{
    // HUDItem inlined (0x004f3b0f..0x004f3b40), WinControl the routine
    // (0x004f3b50), the +0x364 watcher zeroed, the two vptrs, the field = this (0x004f3b91),
    // HUDItem's name "Waypoint", then Create's body inlined (SetOffset slot 31, SetPosition slot
    // 5 with +0x60, Win3D::Create the routine(0, 0) called directly, 0x004f3bb8..0x004f3c09),
    // +0x36c / +0x370 = 0, the Win3D name "HUD_Waypoint", +0x36c = +0x384 = -1, the class name,
    // +0x374 = 0, +0x37c = 1.0f, +0x380 = 0, +0x388 = 0, +0x38c = 0 (0x004f3c18..0x004f3c94).
    // +0x378 is not written (operator_new memory); the port starts it at 0.
    G_HudWaypoint = this;
    HUDItem::Name = "Waypoint";
    SetOffset(0.0f, 0.0f, -1.0f);
    SetPosition(Position);
    Win3D::Create(nullptr, nullptr);
    SpawnCounter = 0;
    SpinAngle = 0.0f;
    Win3D::SetName("HUD_Waypoint");
    SpawnCounter = -1;
    Id = -1;
    ClassName = "HUD_Waypoint";
}

HUD_Waypoint::~HUD_Waypoint()
{
    // the vptrs, the field = 0 (0x004f3d16), the +0x364 watcher cleared,
    // ~WinControl the routine, HUDItem's vptr and watcher -- the member and base destructors.
    G_HudWaypoint = nullptr;
}

bool HUD_Waypoint::Create(Win3D* NewParent, void* Data)
{
    // (this = the WinControl face).
    SetOffset(0.0f, 0.0f, -1.0f);   // slot 31 (+0x7c)
    SetPosition(Position);          // slot 5 (+0x14)
    return Win3D::Create(NewParent, Data);
}

void HUD_Waypoint::Update()
{
    // (this = the WinControl face).
    if (GetClientMouseCursorDrawFlag() != 0
        && GetGameOption(kGameOptInterfaceGroup) != 0      // (6)
        && GetGameOption(kGameOptHud) != 0                 // (0xa)
        && GetPlayerObjRW() != nullptr)
    {
        SetHiddenWithChildren(false);                      // (0)
        Win3D::Update();                                   // tail jump the routine
        return;
    }
    SetHiddenWithChildren(true);                           // (1)
}

void HUD_Waypoint::UpdateInactive()
{
    // (this = the WinControl face; the body HUD_CruiseProgress shares).
    if (GetClientMouseCursorDrawFlag() != 0 && GetPlayerObjRW() != nullptr)
    {
        SetHiddenWithChildren(false);             // (0)
    }
    Win3D::UpdateInactive();
}

void HUD_Waypoint::Render(FSpaceCamera* /*Camera*/, float DeltaSeconds)
{
    // (RET 8).
    IObjRW* const Player = static_cast<IObjRW*>(GetPlayerObjRW());
    if (Player == nullptr)
        return;
    Scale = 1.0f;

    if (WatchedObjRw(Waypoint) != nullptr && bActive != 0)
    {
        // the player must still target the waypoint Solar.
        IObjRW* Target = nullptr;
        Player->get_target(Target);                                 // slot 29 (+0x74)
        if (Target == nullptr)
        {
            bActive = 0;
        }
        else if (Target->get_id() != WatchedObjRw(Waypoint)->get_id()) // slot 8 (+0x20) both
        {
            bActive = 0;
        }
        else
        {
            bool bFound = false;
            if (bUser != 0)
            {
                // the entry with our id is still in the player's list.
                int const Count = GetPlayerWaypointCountbaseline();
                for (int i = 0; i < Count; ++i)
                {
                    if (GetPlayerWaypointbaseline(i)->AutoId == Id)   // , no null test
                    {
                        bFound = true;
                        break;
                    }
                }
            }
            else
            {
                // a best-path record in the player's system lies within
                // 4 m (squared 16) of the waypoint Solar.
                int const Count = BestPathGetWaypointCount();
                Vector const WaypointPos = WatchedObjRw(Waypoint)->get_position(); // slot 0
                for (int i = 0; i < Count; ++i)
                {
                    FBestPathWaypoint* const Record = BestPathGetWaypoint(i);
                    Vector RecordPos;
                    unsigned RecordSystem = 0;
                    if (!BestPathWaypointResolvePosition(Record, &RecordPos, &RecordSystem))
                        continue;
                    if (GetPlayerStarSystemIdbaseline() != RecordSystem)
                        continue;
                    // (Pos - WaypointPos), then dy^2 + dz^2 + dx^2 against 16
                    // (TEST AH,5 / JNP: strictly below).
                    float const Dx = RecordPos.x - WaypointPos.x;
                    float const Dy = RecordPos.y - WaypointPos.y;
                    float const Dz = RecordPos.z - WaypointPos.z;
                    double const Sq = static_cast<double>(Dy) * Dy + static_cast<double>(Dz) * Dz +
                                      static_cast<double>(Dx) * Dx;
                    if (Sq < kBestPathMatchSq)
                    {
                        bFound = true;
                        break;
                    }
                }
            }

            if (!bFound)
            {
                // drop the waypoint; a best-path one moves on to the
                // route's first point.
                ReleaseWaypointObject(Waypoint);
                if (bUser != 0)
                    return;
                HudWaypointSelectBestPath(0);                       // (0)
                return;
            }

            // the distance from the waypoint to the player sets the
            // scale -- 0 inside 15 m (TEST AH,5 / JP keeps >= or NaN), dist * 0.01 below 100 m.
            double const Dist = Distance(Player->get_position(), Target->get_position());
            if (Dist < kScaleZeroDistance)
            {
                Scale = 0.0f;
            }
            else if (Dist < kScaleFullDistance)
            {
                Scale = static_cast<float>(static_cast<double>(kScalePerMeter) * Dist);
            }

            // first time within 150 m -- reached.
            if (bReached == 0 && Dist < kReachedDistance)
            {
                bReached = 1;
                QueueWaypointVoice(G_VoiceObjectiveReached);
                if (bUser != 0)
                {
                    // Index + 1 removals of entry Index, i.e. this entry
                    // and every one before it.
                    if (Index >= 0)
                    {
                        for (int Left = Index + 1; Left != 0; --Left)
                            RemovePlayerWaypointbaseline(Index);
                    }
                    ReleaseWaypointObject(Waypoint);
                    // the entry now at Index becomes the waypoint.
                    FPlayerWaypointRecord* const Next = GetPlayerWaypointbaseline(Index);
                    if (Next != nullptr)
                        SetWaypoint(&Next->Pos, true, Index, Next->AutoId);
                }
            }
        }
    }
    else
    {
        // no live waypoint -- the first player waypoint within 300 m
        // counts as reached, and it and every entry before it are removed.
        int const Count = GetPlayerWaypointCountbaseline();
        for (int i = 0; i < Count; ++i)
        {
            FPlayerWaypointRecord* const Record = GetPlayerWaypointbaseline(i);
            if (Record == nullptr)
                continue;
            if (Distance(Player->get_position(), Record->Pos) < kUserPickDistance)
            {
                QueueWaypointVoice(G_VoiceObjectiveReached);
                for (int Remove = i; Remove >= 0; --Remove)
                    RemovePlayerWaypointbaseline(Remove);
                break;
            }
        }
    }

    // spin the waypoint Solar about its Y axis.
    if (WatchedObjRw(Waypoint) == nullptr)
        return;
    // FLD Dt, FMUL 250.0f, FADD +0x370, FST +0x370 (the sum stays unrounded on the stack),
    // FMUL pi / 180, FST a float copy; FCOS runs on the unrounded angle, FSIN on the float copy.
    double const Sum = static_cast<double>(DeltaSeconds) * kSpinDegPerSecond +
                       static_cast<double>(SpinAngle);
    SpinAngle = static_cast<float>(Sum);
    double const Radians = Sum * static_cast<double>(kDegToRad);
    float const RadiansF = static_cast<float>(Radians);
    float const Cos = static_cast<float>(cos(Radians));
    float const Sin = static_cast<float>(sin(static_cast<double>(RadiansF)));
    float Rotation[9] = {Cos, 0.0f, Sin, 0.0f, 1.0f, 0.0f, -Sin, 0.0f, Cos};
    HudMatrixRotateRows12(Rotation, 0.0f);                          // (0)
    HudMatrixRotateRows01(Rotation, 0.0f);                          // (0)

    // into the CObject's orientation (+0x08) when it is a CSimple
    // ((class & 3) == 3).
    IObjRW* const Obj = WatchedObjRw(Waypoint);
    if (Obj == nullptr)
        return;
    CObject* const Object = static_cast<CBase*>(static_cast<IObjInspectImpl*>(Obj))->Object;
    if (Object == nullptr)
        return;
    if ((static_cast<unsigned>(Object->ObjectClass) & 3u) != 3u)
        return;
    memcpy(&Object->Orientation, Rotation, sizeof(Rotation));
}

void HUD_Waypoint::SetWaypoint(Vector const* Pos, bool bNewUser, int NewIndex, int NewId)
{
    // (RET 0x10).

    // the same entry, and its Solar within 2 m on every axis (FABS,
    // TEST AH,5 / JP: below 2.0 only) -- just target it again.
    IObjRW* const Current = WatchedObjRw(Waypoint);
    if (Current != nullptr && static_cast<unsigned char>(bNewUser) == bUser && NewIndex == Index &&
        NewId == Id)
    {
        Vector const WaypointPos = Current->get_position();
        double const Dx = static_cast<double>(WaypointPos.x) - static_cast<double>(Pos->x);
        float const Dy = WaypointPos.y - Pos->y;
        float const Dz = WaypointPos.z - Pos->z;
        if (fabs(Dx) < kSameSpotAxis && fabs(static_cast<double>(Dy)) < kSameSpotAxis &&
            fabs(static_cast<double>(Dz)) < kSameSpotAxis)
        {
            bActive = 1;
            SendTargetCommand(WatchedObjRw(Waypoint)->get_id());
            return;
        }
    }

    // drop the old Solar, remember the entry.
    ReleaseWaypointObject(Waypoint);
    bUser = static_cast<unsigned char>(bNewUser);
    Index = NewIndex;
    Id = NewId;

    // the "waypoint" solar archetype, loaded, and its companion's
    // Load(0). baseline tests neither for null.
    ID_String ArchName;
    ArchName.Id = CreateID("waypoint");
    Archetype::Solar* const Arch = Archetype::GetSolar(Archetype::GetSolarID(ArchName));
    Arch->load();                                                   // Root slot 3 (+0x0c)
    Arch->Companion->Load(0);                                       // ICliObj slot 8 (+0x20)

    // CSolar::CreateParms on the stack. the routine (0x004f44d3) is
    // the CEqObj::CreateParms default constructor (EqObjCreateParmsDefaults). Then the CSolar
    // part: ID_String::clear on +0xb8 / +0xc0 / +0xcc and zeros through +0xe0, and the anim list.
    CSolar::CreateParms Parms;
    EqObjCreateParmsDefaults(Parms);
    Parms.NicknameSrc.clear();
    Parms.VisitValue = 0;
    Parms.ParentNicknameId = 0;                                     // ID_String::clear
    Parms.bDestructible = 0;
    Parms.bDynamic = 0;
    Parms.DestSystem = 0;
    Parms.DestGateId = 0;                                           // ID_String::clear
    Parms.PrevRing = 0;
    Parms.NextRing = 0;
    Parms.TradelaneSpaceNameId = 0;
    Parms.AtmosphereRange = 0.0f;
    Parms.DockWithBaseId = 0;

    // nickname "waypoint_target<old counter>", the counter incremented;
    // the id goes into Id, the nickname and the reputation group, which gets an entry and
    // affiliation -1.
    char Nickname[0x80];
    int const Suffix = SpawnCounter;
    // the field -> CreateParms+0x04 before CSolar::init.
    Parms.ObjDb = GetClientObjDb();
    SpawnCounter = Suffix + 1;
    sprintf(Nickname, "waypoint_target%d", Suffix);
    unsigned const NewObjectId = CreateID(Nickname);
    Parms.NicknameSrc.Id = NewObjectId;
    Parms.Id = NewObjectId;
    Parms.VibeId = NewObjectId;
    int const& RepGroup = reinterpret_cast<int const&>(Parms.VibeId);
    Reputation::Vibe::Alloc(RepGroup);
    Reputation::Vibe::SetAffiliation(RepGroup, 0xffffffffu, false);

    // the name is mad-lib ids 0x442 with no values, the second name
    // empty; no infocard.
    FmtStr Name;
    FmtStr Name2;
    Name.begin_mad_lib(0x442);
    Name.end_mad_lib();
    Reputation::Vibe::SetName(RepGroup, Name, Name2, nullptr);
    Reputation::Vibe::SetInfocard(RepGroup, 0);


    Parms.Pos = *Pos;

    // a client Solar (the routine), slot 90 create, CSolar::init on its
    // CObject when that is an EqObj ((class & 0x303) == 0x303; baseline calls init on whatever
    // that yields), slot 92, the routine(Solar, 0).
    FClientSolarGameobj* const Solar = new FClientSolarGameobj();
    Solar->create(Arch);
    CObject* const SolarObject = static_cast<CBase*>(Solar)->Object;
    CSolar* SolarCObject = nullptr;
    if (SolarObject != nullptr && (static_cast<unsigned>(SolarObject->ObjectClass) & 0x303u) == 0x303u)
        SolarCObject = static_cast<CSolar*>(SolarObject);
    SolarCObject->init(Parms);
    Solar->Slot170();
    ClientObjDb_RegisterGameobj(Solar, false);


    Arch->Release();
    SolarCObject->enable_collisions_r(false);

    // target it.
    SendTargetCommand(Solar->get_id());

    // watch it; live, not reached yet.
    Waypoint.set(static_cast<Watchable const*>(static_cast<IObjRW*>(Solar)));
    bActive = 1;
    bReached = 0;

    // already within 100 m (TEST AH,0x41 / JP: at most 100) counts as
    // reached.
    IObjRW* const Player = static_cast<IObjRW*>(GetPlayerObjRW());
    if (Player != nullptr)
    {
        Vector const& WaypointPos = WatchedObjRw(Waypoint)->get_position();
        if (DistanceXFirst(Player->get_position(), WaypointPos) <= kScaleFullDistance)
            bReached = 1;
    }
    // the two FmtStr and the CreateParms die here.
}

bool HUD_Waypoint::GetWaypointEntry(unsigned char* OutUser, int* OutIndex)
{
    // (RET 8).
    if (WatchedObjRw(Waypoint) == nullptr || bActive == 0)
        return false;
    *OutUser = bUser;
    *OutIndex = Index;
    return true;
}

bool HUD_Waypoint::IsWaypointAt(Vector const* Pos)
{
    // (RET 4): as SetWaypoint's same-spot test -- dx kept unrounded, dy / dz
    // stored as floats, each FABS below 2.0.
    if (WatchedObjRw(Waypoint) == nullptr || bActive == 0)
        return false;
    Vector const WaypointPos = WatchedObjRw(Waypoint)->get_position();
    double const Dx = static_cast<double>(WaypointPos.x) - static_cast<double>(Pos->x);
    float const Dy = WaypointPos.y - Pos->y;
    float const Dz = WaypointPos.z - Pos->z;
    if (!(fabs(Dx) < kSameSpotAxis))
        return false;
    if (!(fabs(static_cast<double>(Dy)) < kSameSpotAxis))
        return false;
    if (!(fabs(static_cast<double>(Dz)) < kSameSpotAxis))
        return false;
    return true;
}

float HUD_Waypoint::GetScale() const
{
    // .
    return Scale;
}

IObjRW* HUD_Waypoint::GetWaypointObject() const
{
    // .
    return WatchedObjRw(Waypoint);
}

bool HUD_Waypoint::IsActive() const
{
    // .
    return bActive != 0;
}

void HudWaypointOnPlayerWaypointsCleared()
{
    // .
    if (G_HudWaypoint == nullptr)
        return;
    IObjRW* const Player = static_cast<IObjRW*>(GetPlayerObjRW());
    if (Player == nullptr)
        return;
    if (WatchedObjRw(G_HudWaypoint->Waypoint) == nullptr)
        return;
    if (G_HudWaypoint->bActive == 0)
        return;
    // the IObjAffect face (+4), slot 18 (+0x48).
    static_cast<IObjAffect*>(Player)->set_target(nullptr, SUBOBJ_ID_NONE,
                                                 static_cast<int>(GetPlayerConnectionId()));
}

int HudWaypointGetObjectCount()
{
    // MOV ECX,[the field] / CALL the routine / NEG / SBB / NEG.
    return G_HudWaypoint->GetWaypointObject() != nullptr ? 1 : 0;
}

IObjRW* HudWaypointGetObject(int ObjectIndex)
{
    // a non-zero index gives 0; 0 tail-jumps to the routine.
    if (ObjectIndex != 0)
        return nullptr;
    return G_HudWaypoint->GetWaypointObject();
}

void EqObjCreateParmsDefaults(CEqObj::CreateParms& Parms)
{
    // , in the listing's store order.
    Parms.HitPts = -1.0f;                                           // +0xc 0xbf800000
    Parms.Id = 0;                                                   // +0x0
    Parms.ObjDb = nullptr;                                          // +0x4
    Parms.OwnerPlayer = 0;                                          // +0x8
    Parms.Pos.x = 0.0f;                                             // +0x10..+0x18
    Parms.Pos.y = 0.0f;
    Parms.Pos.z = 0.0f;
    Parms.Velocity.x = 0.0f;                                        // +0x40..+0x54
    Parms.Velocity.y = 0.0f;
    Parms.Velocity.z = 0.0f;
    Parms.AngularVelocity.x = 0.0f;
    Parms.AngularVelocity.y = 0.0f;
    Parms.AngularVelocity.z = 0.0f;
    Parms.Pad58 = 0;                                                // +0x58
    Win3DIdentityRotation3x3(reinterpret_cast<float*>(&Parms.Orient)); // on +0x1c
    Parms.DockingCount = 0;                                         // +0x5c
    Parms.VibeId = 0xffffffffu;                                     // +0x60
    Parms.SpaceCostume.PartIds[0] = 0;                              // +0x64..+0x70
    Parms.SpaceCostume.PartIds[1] = 0;
    Parms.SpaceCostume.PartIds[2] = 0;
    Parms.SpaceCostume.PartIds[3] = 0;
    Parms.SpaceCostume.AccessoryCount = 0;                          // +0x94
    Parms.VoiceId = 0;                                              // +0x98
    // +0x9c..+0xa8 (allocator byte, three null pointers) and +0xac..+0xb4 (allocator byte,
    // operator new(0x10) head linked to itself, size 0): Equipment and ArchGroupDescs, which
    // their std constructors already left empty. +0x74..+0x90 (AccessoryIds) are not written.
}

void HudWaypointSelectBestPath(int PointIndex)
{
    // (cdecl).
    if (PointIndex >= BestPathGetWaypointCount())                  // , signed
        return;
    FBestPathWaypoint* const Record = BestPathGetWaypoint(PointIndex);

    // a record that names an object -- target it.
    IObjRW* const Obj = BestPathWaypointResolveObject(Record);
    if (Obj != nullptr)
    {
        SendTargetCommand(Obj->get_id());
        return;
    }

    // otherwise its position becomes the waypoint (the system is not
    // read; the field is not tested).
    Vector Pos;
    unsigned System = 0;
    if (BestPathWaypointResolvePosition(Record, &Pos, &System))
        G_HudWaypoint->SetWaypoint(&Pos, false, PointIndex, PointIndex);
}

void HudWaypointSelectPlayerWaypoint(int Index)
{
    // (cdecl).
    if (Index >= GetPlayerWaypointCountbaseline())                   // , signed
        return;
    FPlayerWaypointRecord* const Record = GetPlayerWaypointbaseline(Index);

    // a waypoint on an object -- target it.
    if (Record->ObjectId != 0)
    {
        SendTargetCommand(Record->ObjectId);
        return;
    }

    // otherwise its position becomes the waypoint (the field is not
    // tested).
    G_HudWaypoint->SetWaypoint(&Record->Pos, true, Index, Record->AutoId);
}

namespace
{
    // The static HUDFACTORY<HUD_Waypoint> the field: XC entry the branch runs the ctor
    // ("Waypoint", appended to the factory chain by the routine) and registers
    // with atexit (-> the routine, the unlink). Its Create the routine is
    // operator new(0x390) + the routine, null when the allocation fails.
    HUDFACTORY<HUD_Waypoint> G_HudWaypointFactory("Waypoint");
}
