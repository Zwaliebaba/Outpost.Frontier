// Author: Giperion.

#include "gf_character_object.h"
#ifdef OUTPOST_FRONTIER_WALK_MODE
#include "base_walk.h"
#endif

#include "cargo_bar_control.h" // ToolTipHoverInit / ToolTipHoverSetCaption
#include "client_cliobj.h"     // PushMaterialPathPair, ResolveMaterialEntry, ReleaseMaterialEntrySlot
#include "game_options.h"      // GetGameOption (the routine)
#include "gf_script_object.h"  // G_GfScriptManager (the field), FGfScriptSequence
#include "globals.h"           // GetClientFrameTimeCapped (the routine), G_RenderSurfaceWidth
#include "character_select_equip.h" // GetPlayerVisitFlagsbaseline / SetPlayerVisitFlagsbaseline
#include "input_manager.h"     // MouseSetCursor (the routine)
#include "menu_universe_base.h" // GetClientLocationRoomScriptBehaviors (the routine)
#include "mirror_property_bucket.h"
#include "misc_util.h"         // MiscUtilLoadEngineInstance, MiscUtilLoadIdsCopy, MiscUtilSphereInView
#include "nav_bar.h"           // NavBarCharacterClicked (the routine), NavBarOpenButton (the routine)
#include "nn_tooltip.h"        // NnToolTipHideGlobal (the routine)
#include "player_spawn.h"      // GetPlayerConnectionId (the field)
#include "server_connect.h"    // GetLocalServerInterface (the field)
#include "space_camera.h"      // FSpaceCamera (the field)
#include "tooltip_hover.h"     // FToolTipHoverInfo / NnToolTipFeedHover (the routine)
#include "widescreen.h"      // port-only Hor+ (OUTPOST_FRONTIER_WIDESCREEN_SUPPORT)
#include "win3d.h"

#include "NeuronRuntime.h"
#include "common_cobject.h"    // CompoundInstanceFindFirstName
#include "common_iface.h"      // ScriptBehavior
#include "common_ini.h"        // StringAlloc, RefreshLoadingProgress
#include "common_misc.h"       // malloc_strcpy / malloc_strfree / malloc_wstrcpy / get_projected_bounding_sphere
#include "common_newchar.h"    // RtcSlider
#include "common_reputation.h" // CostumeDescriptions, DetailSwitchTable
#include "common_server.h"     // IServer
#include "common_universe.h"   // Universe::get_base (the routine)
#include "neuroncomponents.h"             // FDUMP, NEURONCOMPONENTS_CRC
#include "engine/sceneruntime.h"
#include "x86math.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

namespace
{
    char const kObjectDeformableCpp[] = "Client\\gf\\ObjectDeformable.cpp";

    // (0x00668848..0x00668947, bss) -- the routine's result buffer; every exit
    // returns its address. That function is its only reader and writer.
    char G_BgcsContextBuffer[256];

    // (0x124 bytes) -- the body every "pc" character shares. The static
    // initializer 0x00443e40 zeroes the instance and both four-entry arrays and registers an
    // empty atexit (the field is a bare RET); zero initialization here is that.
    FCharacterLodSet G_SharedCharacterLod;

    void* const kNoInstance = reinterpret_cast<void*>(static_cast<intptr_t>(-1));

    // (thiscall on Set, RET 4) -- build Set's deformable from the four bodyparts
    // (0 body, 1 head, 2 left hand, 3 right hand) and read back its LOD switch table.
    void BuildCharacterDeformable(FCharacterLodSet* Set, bodypart const* const* BodyParts)
    {
        // "low_character_lod" sets Flags bit 1 (Deformable2 skips LOD 0)
        // and raises the LOD limit of the non-first parts from 2 to 3.
        unsigned long PartFlags = 0;
        unsigned long ChildLodLimit = 2;
        if (RtcSlider::is_enabled("low_character_lod"))
        {
            PartFlags = 2;
            ChildLodLimit = 3;
        }

        // the four entries {0, 0, 0, 0, 0, -1}.
        FDeformPartDesc Entries[4];
        int Index = 0;
        for (Index = 0; Index < 4; ++Index)
        {
            Entries[Index].LodCount = 0;
            Entries[Index].Lods = nullptr;
            Entries[Index].Field08 = 0;
            Entries[Index].Path = nullptr;
            Entries[Index].Flags = 0;
            Entries[Index].LodLimit = 0xFFFFFFFFu;
        }

        // one entry per bodypart with a mesh: its full file name, the
        // sixteen LOD slots with FileSystem and Name zeroed (Deformable2 then opens the part by
        // Path), LodCount 16.
        FDeformLodDesc LodSlots[4][16];
        char Paths[4][0x400];
        int Count = 0;
        for (Index = 0; Index < 4; ++Index)
        {
            bodypart const* const Part = BodyParts[Index];
            if (Part == nullptr || Part->Mesh.empty())
                continue;
            strcpy(Paths[Count], GetFullFilename(Part->Mesh.c_str()));
            int Lod = 0;
            for (Lod = 0; Lod < 16; ++Lod)
            {
                LodSlots[Count][Lod].FileSystem = nullptr;
                LodSlots[Count][Lod].Name = nullptr;
            }
            Entries[Count].LodCount = 16;
            Entries[Count].Lods = LodSlots[Count];
            Entries[Count].Field08 = 0;
            Entries[Count].Path = Paths[Count];
            Entries[Count].Flags = PartFlags;
            Entries[Count].LodLimit = (Index == 0) ? 0xFFFFFFFFu : ChildLodLimit;
            ++Count;
        }

        // the table header {Count, Entries}.
        struct FDeformPartTable
        {
            int Count;
            FDeformPartDesc* Entries;
        };
        FDeformPartTable Table;
        Table.Count = Count;
        Table.Entries = Entries;
        RefreshLoadingProgress();

        // a material-library reference per bodypart mesh
        // (the routine(mesh, 1, -1)) while the body loads.
        Archetype::FCliMaterialEntry* Materials[4];
        for (Index = 0; Index < 4; ++Index)
        {
            bodypart const* const Part = BodyParts[Index];
            if (Part == nullptr || Part->Mesh.empty())
                continue;
            Materials[Index] = Archetype::ResolveMaterialEntry(Part->Mesh.c_str(), true,
                                                               0xffffffffu);
        }
        RefreshLoadingProgress();

        // IDeformable slot 3 into Set +0x00.
        NeuronRuntime::Deform->Create(&Set->Instance, reinterpret_cast<int*>(&Table), nullptr, nullptr,
                              nullptr);
        RefreshLoadingProgress();

        // the references released (the routine).
        for (Index = 0; Index < 4; ++Index)
        {
            bodypart const* const Part = BodyParts[Index];
            if (Part == nullptr || Part->Mesh.empty())
                continue;
            Archetype::ReleaseMaterialEntrySlot(&Materials[Index]);
        }
        RefreshLoadingProgress();

        // per bodypart, the LOD count the part came back with and each
        // LOD's switch distance (DetailSwitchTable::get_switch_distance(fraction * 100.0)); the
        // last one is the table's distance at 0. A part without a mesh gets 0 and no table.
        int EntryIndex = 0;
        for (Index = 0; Index < 4; ++Index)
        {
            bodypart const* const Part = BodyParts[Index];
            if (Part == nullptr || Part->Mesh.empty())
            {
                Set->Counts[Index] = 0;
                Set->Tables[Index] = nullptr;
                continue;
            }
            int const LodCount = Entries[EntryIndex].LodCount;
            Set->Counts[Index] = LodCount;
            int Lod = 0;
            for (Lod = 0; Lod < LodCount; ++Lod)
            {
                DetailSwitchTable* const Switch = Part->Switch;
                float const Fraction = static_cast<float>(
                    static_cast<double>(Entries[EntryIndex].Lods[Lod].Fraction) * 100.0);
                Set->Switch[Index][Lod] = Switch->get_switch_distance(Fraction);
                Set->Tables[Index] = Switch;
            }
            if (Set->Counts[Index] > 0)
                Set->Switch[Index][Set->Counts[Index] - 1] = Part->Switch->get_switch_distance(0.0f);
            ++EntryIndex;
        }
    }

    // The dealer screen a role opens (0x00445ce1..0x00445d1e and the branch): 2 -> 5,
    // 3 -> 7, 4 -> 8 (NavBarOpenButton, the routine); the bartender (1) and no role open
    // nothing.
    void OpenRoleDealer(int Role)
    {
        if (Role == 1)
            return;
        if (Role == 2)
            NavBarOpenButton(5);
        else if (Role == 3)
            NavBarOpenButton(7);
        else if (Role == 4)
            NavBarOpenButton(8);
    }

    // (cdecl) -- the stop callback of a click's behaviour script: A is the
    // character (the sequence's +0x1c).
    void __cdecl OnBehaviorScriptStopped(void* A, void* /*B*/, unsigned char /*bArg*/)
    {
        OpenRoleDealer(static_cast<FCharacterObject*>(A)->Role);
    }

    // (thiscall on the bucket, RET 4) -- the first node whose behaviour has Key,
    // or null.
    MetaNode<ScriptBehavior>* FindBehaviorNode(TMirrorPropertyBucket<ScriptBehavior>* Bucket,
                                                unsigned Key)
    {
        MetaNode<ScriptBehavior>* Cursor = nullptr;
        while (Bucket->List.traverse(Cursor, true))
        {
            if (static_cast<unsigned>(Cursor->Data->BehaviorId) == Key)
                return Cursor;
        }
        return nullptr;
    }
}

// ============================================================================================
// FCharacterDesc
// ============================================================================================

FCharacterDesc::FCharacterDesc()
    : Placement(nullptr)
    , PlacementIndex(-1)
    , bScriptSpatial(0)
{
    // CharacterProperties constructed (the member), +0x64 = 0, +0x6c = 0,
    // +0x68 = -1, +0x70..+0x78 = 0, +0x7c / +0x8c / +0x9c = 1.0 and the rest of the matrix 0.
    Pos.x = 0.0f;
    Pos.y = 0.0f;
    Pos.z = 0.0f;
    MatrixSetIdentity(Orient);
}

FCharacterDesc::~FCharacterDesc()
{
    // CharacterProperties::destroy, then ~CharacterProperties (the member).
    Props.destroy();
}

// ============================================================================================
// FCharacterHotspot
// ============================================================================================

FCharacterHotspot::FCharacterHotspot()
    : Character(nullptr)
    , bOffscreen(0)
{
}

FCharacterHotspot::~FCharacterHotspot()
{
    // vptr, then JMP the routine (~FHotspot).
}

void FCharacterHotspot::UpdateScreenRect(ICamera* Camera)
{
    // . 0x00555f66..0x00555f96: the owner's engine instance position.
    float const* const Where = NeuronRuntime::Engine->GetInstancePosition(Character->GetEngineInstance());
    Vector Center;
    Center.x = Where[0];
    Center.y = Where[1];
    Center.z = Where[2];

    // get_projected_bounding_sphere(Camera, Center, 1.0, x, y, r, z).
    float ScreenX = 0.0f;
    float ScreenY = 0.0f;
    float Radius = 0.0f;
    float Depth = 0.0f;
    if (get_projected_bounding_sphere(Camera, Center, 1.0f, ScreenX, ScreenY, Radius, Depth) == 0)
    {
        // .
        bOffscreen = 1;
        ScreenRect.Left = 0;
        ScreenRect.Top = 0;
        ScreenRect.Width = 0;
        ScreenRect.Height = 0;
        return;
    }

    // floor(v + 0.5) of x - 0.6r (the 0.6f constant widened, 0x005e1220),
    // y - r, r * 1.2f (0x005c9800) and r + r.
    bOffscreen = 0;
    int const Left = static_cast<int>(floor(static_cast<double>(ScreenX) -
                                            static_cast<double>(Radius) *
                                                static_cast<double>(0.6f) +
                                            0.5));
    int const Top = static_cast<int>(
        floor(static_cast<double>(ScreenY) - static_cast<double>(Radius) + 0.5));
    int const Width = static_cast<int>(
        floor(static_cast<double>(Radius) * static_cast<double>(1.2f) + 0.5));
    int const Height = static_cast<int>(
        floor(static_cast<double>(Radius) + static_cast<double>(Radius) + 0.5));
    ScreenRect.Left = Left;
    ScreenRect.Top = Top;
    ScreenRect.Width = Width;
    ScreenRect.Height = Height;
}

// ============================================================================================
// FCharacterObject
// ============================================================================================

FCharacterObject::FPartCacheEntry::FPartCacheEntry()
    : Name(nullptr)
    , Part(-1)
    , bUsed(0)
{
    // .
}

FCharacterObject::FPartCacheEntry::~FPartCacheEntry()
{
    // +0x04 = -1, the name freed (not cleared), +0x08 = 0.
    Part = -1;
    if (Name != nullptr)
        free(Name);
    bUsed = 0;
}

FCharacterObject::FCharacterObject(FRoomRuntime* InOwner, FCharacterDesc const& Desc)
    : FRoomObject(InOwner)
{
    // . 0x00443f00..0x00443f1c: +0x30 = 0, the four counts and the four tables 0.
    Lod.Instance = nullptr;
    int Index = 0;
    for (Index = 0; Index < 4; ++Index)
    {
        Lod.Counts[Index] = 0;
        Lod.Tables[Index] = nullptr;
    }
    // CharacterProperties, the eight HardpointSummaries, +0x304 and the
    // five part slots (the members). 0x00443f7f..0x00443fab: +0x2f4 = +0x300 = +0x1b8 = 0,
    // +0x1c0..+0x1cc = -1.
    Hotspot = nullptr;
    Role = 0;
    Placement = nullptr;
    for (Index = 0; Index < 4; ++Index)
        LodIds[Index] = -1;
    // each accessory -1 and its summary cleared.
    for (Index = 0; Index < kMaxAccessories; ++Index)
    {
        AccessoryInstances[Index] = -1;
        AccessoryHardpoints[Index].clear();
    }
    // .
    AccessoryCount = 0;
    bHovered = 0;
    DisplayName = nullptr;
    Hardpoints.clear();
    OwnScript = nullptr;
    OwnScriptName = nullptr;
    bOwnScriptPlaying = 0;
    ScriptState = 0;
    ActiveMotions = 0;
    ActiveAims = 0;
    bPoseDirty = 0;
    bHasLastPose = 0;
    // .
    Load(Desc);
}

FCharacterObject::~FCharacterObject()
{
#ifdef OUTPOST_FRONTIER_WALK_MODE
    BaseWalkCharacterChanged(this, true);
#endif
    // . 0x004440e6..0x00444118: still taken over by a script.
    if (ScriptState == 1)
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: %s", kObjectDeformableCpp, 0x175,
              "Character is being destroyed while still scripted, GAME IS ABOUT TO CRASH!!!\n");
    }
    // the own script forgotten (not destroyed) and its name freed.
    if (OwnScript != nullptr)
    {
        OwnScript = nullptr;
        bOwnScriptPlaying = 0;
        malloc_strfree(&OwnScriptName);
    }
    // every accessory instance destroyed (IEngine slot 24) and its
    // summary destroyed.
    int Index = 0;
    for (Index = 0; Index < AccessoryCount; ++Index)
    {
        if (AccessoryInstances[Index] != -1)
        {
            NeuronRuntime::Engine->DestroyInstance(reinterpret_cast<void*>(AccessoryInstances[Index]));
            AccessoryInstances[Index] = -1;
        }
        AccessoryHardpoints[Index].destroy();
    }
    AccessoryCount = 0;
    // .
    malloc_wstrfree(&DisplayName);
    Hardpoints.destroy();
    Props.destroy();
    // an own body is destroyed (IDeformable slot 6); the shared one
    // is left alone.
    if (bSharedDeform == 0)
    {
        if (Lod.Instance != nullptr)
            NeuronRuntime::Deform->Destroy(Lod.Instance);
        Lod.Instance = nullptr;
        for (Index = 0; Index < 4; ++Index)
        {
            Lod.Counts[Index] = 0;
            Lod.Tables[Index] = nullptr;
        }
    }
    // the hotspot through its deleting destructor.
    if (Hotspot != nullptr)
    {
        delete Hotspot;
        Hotspot = nullptr;
    }
    // the members (the routine x5, ~HardpointSummary, x8,
    // ~CharacterProperties) and the routine.
}

void FCharacterObject::Load(FCharacterDesc const& Desc)
{
#ifdef OUTPOST_FRONTIER_WALK_MODE
    BaseWalkCharacterChanged(this, false);
#endif
    // . 0x004442ae..0x004442fa: the record, placement and index; a "pc" record
    // (+0x1c == 1) shares the one body.
    RefreshLoadingProgress();
    Props = Desc.Props;
    Placement = Desc.Placement;
    PlacementIndex = Desc.PlacementIndex;
    bSharedDeform = (Props.CharType == 1) ? 1 : 0;
    Role = 0;

    // the role from the placement's setpoint segment.
    if (Placement != nullptr)
    {
        SetpointProperties Setpoint;
        Setpoint.extract_from_name(Placement->Name);
        if (_stricmp(Setpoint.Segment, "bartender") == 0) // NEURONCOMPONENTS stricmp
            Role = 1;
        else if (_stricmp(Setpoint.Segment, "trader") == 0)
            Role = 2;
        else if (_stricmp(Setpoint.Segment, "shipdealer") == 0)
            Role = 3;
        else if (_stricmp(Setpoint.Segment, "equipment") == 0)
            Role = 4;
    }

    // the four bodyparts (the head is costume part 0, the body part
    // 1) and the accessories.
    CostumeDescriptions const* const Descs = GetCostumeDescriptions();
    bodypart const* BodyParts[4];
    BodyParts[1] = Descs->find_bodypart(1, Props.Cost.PartIds[0]);
    BodyParts[0] = Descs->find_bodypart(0, Props.Cost.PartIds[1]);
    BodyParts[2] = Descs->find_bodypart(2, Props.Cost.PartIds[2]);
    BodyParts[3] = Descs->find_bodypart(3, Props.Cost.PartIds[3]);
    accessory const* Accessories[kMaxAccessories];
    int Index = 0;
    for (Index = 0; Index < static_cast<int>(Props.Cost.AccessoryCount); ++Index)
        Accessories[Index] = Descs->find_accessory(Props.Cost.AccessoryIds[Index]);

    // the body -- built for this character, or the shared one (built
    // the first time) copied in.
    if (bSharedDeform == 0)
    {
        BuildCharacterDeformable(&Lod, BodyParts);
    }
    else
    {
        if (G_SharedCharacterLod.Instance == nullptr)
            BuildCharacterDeformable(&G_SharedCharacterLod, BodyParts);
        Lod.Instance = G_SharedCharacterLod.Instance;
        for (Index = 0; Index < 4; ++Index)
        {
            Lod.Counts[Index] = G_SharedCharacterLod.Counts[Index];
            Lod.Tables[Index] = G_SharedCharacterLod.Tables[Index];
            int Lod16 = 0;
            for (Lod16 = 0; Lod16 < 16; ++Lod16)
                Lod.Switch[Index][Lod16] = G_SharedCharacterLod.Switch[Index][Lod16];
        }
    }

    // with a root bone, the pose (IDeformable slots 19 / 20) and the
    // height (slot 3, with the position's y).
    int Bone = -1;
    NeuronRuntime::Deform->GetFirstBone(Lod.Instance, &Bone);
    if (Bone != -1)
    {
        NeuronRuntime::Deform->SetPosePos(Lod.Instance, const_cast<float*>(&Desc.Pos.x));
        SetFloat1C(Desc.Pos.y);
        NeuronRuntime::Deform->SetPoseOri(Lod.Instance, const_cast<Matrix*>(&Desc.Orient));
    }
    // .
    Hardpoints.analyze_instance(Bone);
    RefreshLoadingProgress();

    // each accessory with a mesh: its material libraries as {name, -1}
    // pairs, the mesh loaded (the routine, the material entries into RoomObject +0x20), its
    // hardpoints analysed, and its AccessoryHardpoint connected to the body's
    // CharacterHardpoint (IHardpoint slot 6) when both exist.
    for (Index = 0; Index < static_cast<int>(Props.Cost.AccessoryCount); ++Index)
    {
        accessory const* const Found = Accessories[Index];
        if (Found == nullptr || Found->Mesh.empty())
            continue;

        Archetype::FCliVc6Vector<Archetype::FCliMaterialPathPair> Paths;
        Paths.Cookie = 0;
        Paths.Begin = nullptr;
        Paths.End = nullptr;
        Paths.Capacity = nullptr;
        std::list<std::string>::const_iterator It = Found->Materials.begin();
        for (; It != Found->Materials.end(); ++It)
        {
            CacheString Name;
            Name.clear();
            Name.Value = StringAlloc(It->c_str(), true);
            CacheString Pair;
            Pair.clear();
            Archetype::PushMaterialPathPair(&Paths, Name.Value, 0xffffffffu);
        }

        void* const Instance = MiscUtilLoadEngineInstance(Found->Mesh.c_str(), NeuronRuntime::Data,
                                                          nullptr, nullptr, &Paths,
                                                          &MaterialEntries, nullptr);
        AccessoryInstances[Index] = reinterpret_cast<long>(Instance);
        AccessoryHardpoints[Index].analyze_instance(AccessoryInstances[Index]);
        if (AccessoryInstances[Index] != -1)
        {
            char const* const CharacterHp = Found->CharacterHardpoint.c_str();
            char const* const AccessoryHp = Found->AccessoryHardpoint.c_str();
            int const CharacterIndex = Hardpoints.find_name(CharacterHp);
            int const AccessoryIndex = AccessoryHardpoints[Index].find_name(AccessoryHp);
            if (CharacterIndex != -1 && AccessoryIndex != -1)
            {
                FHardpointKey AccessoryKey;
                AccessoryKey.Name = AccessoryHp;
                AccessoryKey.Crc = NEURONCOMPONENTS_CRC::GetCRC32(AccessoryHp);
                FHardpointKey CharacterKey;
                CharacterKey.Name = CharacterHp;
                CharacterKey.Crc = NEURONCOMPONENTS_CRC::GetCRC32(CharacterHp);
                void* const AccessoryPart = reinterpret_cast<void*>(
                    AccessoryHardpoints[Index].get_instance(AccessoryIndex));
                void* const CharacterPart =
                    reinterpret_cast<void*>(Hardpoints.get_instance(CharacterIndex));
                NeuronRuntime::Hardpoint->ConnectHardpoints(CharacterPart, &CharacterKey, AccessoryPart,
                                                    &AccessoryKey);
            }
        }
        // the pair list's buffer.
        delete[] Paths.Begin;
    }

    // .
    AccessoryCount = static_cast<int>(Props.Cost.AccessoryCount);
    RefreshLoadingProgress();

    // the hotspot, owned by and answering to this character; enabled
    // only with a behaviour or a role (the routine, inlined).
    Hotspot = new FCharacterHotspot();
    Hotspot->Character = this;
    Hotspot->bEnabled = 1;
    Hotspot->Owner = this;
    Hotspot->Cookie = 0;
    UpdateHotspotEnabled();

    // the name -- the record's ids (the routine), else its wide name.
    DisplayName = nullptr;
    if (Props.NameIds != 0)
        MiscUtilLoadIdsCopy(&DisplayName, Props.NameIds);
    else
        malloc_wstrcpy(&DisplayName, Props.NameWide);

    // no self-update until the room starts it; the idle script
    // (+0x1ac) becomes the own script unless it already is.
    bNoUpdate = 1;
    unsigned char const bSpatial = Desc.bScriptSpatial;
    char const* const IdleScript = Props.Script;
    bool bSame = false;
    if (IdleScript != nullptr && OwnScriptName != nullptr &&
        _stricmp(IdleScript, OwnScriptName) == 0) // NEURONCOMPONENTS stricmp
    {
        bSame = true;
    }
    if (!bSame)
    {
        if (OwnScript != nullptr)
        {
            G_GfScriptManager.DestroySequence(OwnScript);
            OwnScript = nullptr;
            bOwnScriptPlaying = 0;
            malloc_strfree(&OwnScriptName);
        }
        malloc_strcpy(&OwnScriptName, IdleScript);
        OwnScript = CreateOwnScript(IdleScript, bSpatial);
        if (ScriptState == 0 && OwnScript != nullptr)
        {
            OwnScript->Play();
            bOwnScriptPlaying = 1;
        }
    }

    // the script run on by one full step, then by a random fraction
    // (rand() / 32768, the field) when that is above 0 (`TEST AH,0x41` + JNZ: not greater
    // skips).
    if (OwnScript != nullptr)
        OwnScript->AdvanceCurrent(1.0f);
    float const Random = static_cast<float>(rand()) * 3.0517578125e-05f;
    if (OwnScript != nullptr && Random > 0.0f)
        OwnScript->AdvanceCurrent(Random);

    // .
    RenderMode = 0;
    LightGroup = 0;
    Layer = 0;
}

void* FCharacterObject::GetEngineInstance()
{
    // IDeformable slot 18 into an int that starts at -1.
    int Bone = -1;
    NeuronRuntime::Deform->GetFirstBone(Lod.Instance, &Bone);
    return reinterpret_cast<void*>(static_cast<intptr_t>(Bone));
}

void FCharacterObject::SetFloat1C(float Value)
{
    // IDeformable slot 24 with the value, +0x1c = value, +0x378 = 1.
    NeuronRuntime::Deform->SetHeight(Lod.Instance, Value);
    Float1C = Value;
    bPoseDirty = 1;
}

void FCharacterObject::Update(double DeltaSeconds)
{
    // . 0x004449ef..0x00444a0f: the OBJECTS and CHARACTERS options.
    if (GetGameOption(kGameOptObjectsGroup) == 0)
        return;
    if (GetGameOption(kGameOptCharacters) == 0)
        return;

    // a script let go of the character -- its own script again, and
    // no self-update.
    if (ScriptState == 2)
    {
        ScriptState = 0;
        RestartOwnScript();
        bNoUpdate = 1;
    }

    // the instance's pose; a pose other than last frame's (compared
    // dword by dword, CMPSD) marks the pose dirty.
    float Pos[3];
    memcpy(Pos, NeuronRuntime::Engine->GetInstancePosition(GetEngineInstance()), sizeof(Pos));
    float Orient[9];
    memcpy(Orient, NeuronRuntime::Engine->GetInstanceOrientation(GetEngineInstance()), sizeof(Orient));
    if (bHasLastPose == 0 || memcmp(LastPos, Pos, sizeof(Pos)) != 0 ||
        memcmp(LastOrient, Orient, sizeof(Orient)) != 0)
    {
        bPoseDirty = 1;
    }
    memcpy(LastPos, Pos, sizeof(Pos));
    memcpy(LastOrient, Orient, sizeof(Orient));
    bHasLastPose = 1;

    // with motions, aims or a dirty pose, the mixer walked
    // (IAnimation2 slot 27) and the instance updated (IEngine slot 27) by the frame time.
    if (ActiveMotions != 0 || ActiveAims != 0 || bPoseDirty != 0)
    {
        float const Time = static_cast<float>(DeltaSeconds);
        NeuronRuntime::Anim->MixerWalk(static_cast<unsigned long>(
                                   reinterpret_cast<uintptr_t>(GetEngineInstance())),
                               Time);
        void* TimeArg = nullptr;
        memcpy(&TimeArg, &Time, sizeof(Time));
        NeuronRuntime::Engine->UpdateInstance(GetEngineInstance(), 0, TimeArg);
    }

    // over a 3D window the arrow cursor, and no name.
    if (Win3DGetHover() != nullptr)
    {
        MouseSetCursor("arrow", false);
        bHovered = 0;
    }

    // hovered with a name -- the name as a tooltip keyed on this
    // object, pointing along the view ray through the hotspot's top-left corner 30 pixels up.
    if (bHovered == 0)
        return;
    if (DisplayName == nullptr)
        return;

    FToolTipHoverInfo Info;
    ToolTipHoverInit(&Info);
    ToolTipHoverSetCaption(&Info, DisplayName);
    Info.Ids = static_cast<int>(reinterpret_cast<intptr_t>(this));

    // the corner on the Win3D camera's near plane (the field /
    // centre, 0067dc70 / 0067dc74 scale, 0067dc9c znear), turned by its orientation
    // (the routine).
    FSpaceCamera* const Cam = Win3DGetCamera();
    FVector Local;
    Win3DScreenToNearPlane(Cam, Local, static_cast<float>(Hotspot->ScreenRect.Left),
                           static_cast<float>(Hotspot->ScreenRect.Top + -0x1e));
    FMatrix CamOrientation;
    memcpy(&CamOrientation, Cam->World, sizeof(CamOrientation));
    FVector World;
    Win3DTransformByMatrix(World, CamOrientation, Local);

    // normalised ((z^2 + x^2) + y^2, 1 / sqrt, the field) into the
    // world position; zero half extents; live.
    double const X = World.X;
    double const Y = World.Y;
    double const Z = World.Z;
    double const Inv = 1.0 / sqrt(Z * Z + X * X + Y * Y);
    Info.WorldPos[0] = static_cast<float>(X * Inv);
    Info.WorldPos[1] = static_cast<float>(Y * Inv);
    Info.WorldPos[2] = static_cast<float>(Z * Inv);
    Info.HalfExtents[0] = 0.0f;
    Info.HalfExtents[1] = 0.0f;
    Info.HalfExtents[2] = 0.0f;
    Info.bLive = 1;
    NnToolTipFeedHover(&Info);

    // the record torn down (~FToolTipHoverInfo).
}

void FCharacterObject::PickLods(ICamera* Camera)
{
    // . 0x00444f9b..0x0044501f: the camera distance, 1.0 without an instance.
    float Distance = 1.0f;
    void* const Instance = GetEngineInstance();
    if (Instance != kNoInstance)
    {
        float const* const Where = NeuronRuntime::Engine->GetInstancePosition(Instance);
        float const X = Where[0];
        float const Y = Where[1];
        float const Z = Where[2];
        float const* const Eye = Camera->GetPosition();
        double const Dx = static_cast<double>(X) - Eye[0];
        double const Dy = static_cast<double>(Y) - Eye[1];
        double const Dz = static_cast<double>(Z) - Eye[2];
        Distance = static_cast<float>(sqrt(Dx * Dx + Dy * Dy + Dz * Dz));
    }
    // the half field of view (ICamera slot 6) and the surface width.
    float const HalfFov = Camera->GetHalfFovXDeg();
#ifdef OUTPOST_FRONTIER_WIDESCREEN_SUPPORT
    // Port-only Hor+: slot 6 is the angle on a 4:3 surface, so the LOD pairs it with the width
    // of that surface and picks what a 4:3 screen of this height would.
    float const SurfaceWidth = static_cast<float>(WidescreenEquivalentSurfaceWidth());
#else
    float const SurfaceWidth = static_cast<float>(static_cast<int>(G_RenderSurfaceWidth));
#endif

    // per part with LODs, the first LOD whose switch distance is not
    // below the adjusted distance (`TEST AH,1` + JZ: not less is the hit); none -> -1. With
    // "low_character_lod" and the adjusted distance beyond the 100% switch distance (`TEST
    // AH,5` + JP: only C0 alone falls through), one LOD coarser, or past the last: the first
    // part keeps its last LOD, the others none. The picks are packed.
    int Written = 0;
    int Part = 0;
    for (Part = 0; Part < 4; ++Part)
    {
        int const Count = Lod.Counts[Part];
        if (Count <= 0)
            continue;
        float const Adjusted =
            Lod.Tables[Part]->adjust_distance(Distance, HalfFov, SurfaceWidth);
        int Pick = -1;
        int Index = 0;
        for (Index = 0; Index < Count; ++Index)
        {
            if (!(Lod.Switch[Part][Index] < Adjusted))
                break;
        }
        if (Index < Count)
        {
            Pick = Index;
            if (RtcSlider::is_enabled("low_character_lod"))
            {
                float const Full = Lod.Tables[Part]->get_switch_distance(100.0f);
                if (Full < Adjusted)
                {
                    if (Index + 1 < Count)
                        Pick = Index + 1;
                    else if (Part == 0)
                        Pick = Lod.Counts[0] - 1;
                    else
                        Pick = -1;
                }
            }
        }
        LodIds[Written] = Pick;
        ++Written;
    }
    // the rest -1.
    for (; Written < 4; ++Written)
        LodIds[Written] = -1;
}

void FCharacterObject::DrawOpaque(ICamera* Camera)
{
    // . 0x00445156..0x00445188: OBJECTS and CHARACTERS on, LIKE_PETAL off.
    if (GetGameOption(kGameOptObjectsGroup) == 0)
        return;
    if (GetGameOption(kGameOptCharacters) == 0)
        return;
    if (GetGameOption(kGameOptLikePetal) != 0)
        return;

    // the hotspot follows the room camera; the body's bounding sphere
    // (IDeformable slot 38) must touch the room camera's view (the routine).
    ICamera* const RoomCamera = GetRoomRuntimeCamera(Owner);
    Hotspot->UpdateScreenRect(RoomCamera);
    float Radius = 0.0f;
    float Center[3];
    NeuronRuntime::Deform->GetBoundSphere(Lod.Instance, &Radius, Center);
    if (!MiscUtilSphereInView(RoomCamera, Center, Radius))
        return;
    if (Camera == nullptr)
        return;
    void* const Instance = GetEngineInstance();
    if (Instance == kNoInstance)
        return;

    // the LODs, then IDeformable slot 15 with the frame time.
    PickLods(Camera);
    float const FrameTime = static_cast<float>(GetClientFrameTimeCapped());
    NeuronRuntime::Deform->SetLod(Lod.Instance, FrameTime, LodIds);

    // the shadow (the routine with the instance, the height and 1).
    float const Height = GetFloat1C();
    unsigned long HeightBits = 0;
    memcpy(&HeightBits, &Height, sizeof(HeightBits));
    RoomControlDrawShadow(Owner, static_cast<unsigned long>(reinterpret_cast<uintptr_t>(Instance)),
                          HeightBits, 1);
    // the routine.
    RoomControlLightObject(Owner, this);
    // IDeformable slot 16 (camera, LODs, 0, &out, 0). The out int is
    // an uninitialised stack slot in baseline.
    int DrawnLod = 0;
    NeuronRuntime::Deform->Draw(Lod.Instance, Camera, LodIds, nullptr, &DrawnLod, 0);
}

void FCharacterObject::RestartOwnScript()
{
    // . 0x00445283..0x0044529e: a live sequence restarts (the routine) when it
    // played before, else plays.
    if (OwnScript != nullptr)
    {
        if (bOwnScriptPlaying != 0)
        {
            OwnScript->RestartCurrent();
            return;
        }
        OwnScript->Play();
        bOwnScriptPlaying = 1;
        return;
    }
    // the idle script, unless it already is the own one.
    char const* const IdleScript = Props.Script;
    if (IdleScript != nullptr && OwnScriptName != nullptr &&
        _stricmp(IdleScript, OwnScriptName) == 0) // NEURONCOMPONENTS stricmp
    {
        return;
    }
    if (OwnScript != nullptr)
    {
        G_GfScriptManager.DestroySequence(OwnScript);
        OwnScript = nullptr;
        bOwnScriptPlaying = 0;
        malloc_strfree(&OwnScriptName);
    }
    malloc_strcpy(&OwnScriptName, IdleScript);
    OwnScript = CreateOwnScript(IdleScript, 0);
    if (ScriptState != 0)
        return;
    if (OwnScript == nullptr)
        return;
    OwnScript->Play();
    bOwnScriptPlaying = 1;
}

FGfScriptSequence* FCharacterObject::CreateOwnScript(char const* ScriptName, unsigned char bSpatial)
{
    // . 0x0044535a..0x00445362: no name, no script.
    if (ScriptName == nullptr || ScriptName[0] == '\0')
        return nullptr;

    // a sequence of the room's.
    FGfScriptSequence* const Sequence = G_GfScriptManager.CreateSequence(Owner, 0);
    if (Sequence == nullptr)
    {
        // .
        FDUMP(0x100002, "%s(%d) : *** WARNING: %s", kObjectDeformableCpp, 0x453,
              "Cannot allocate scenescript script\n");
        return Sequence;
    }

    // the script and its own frame; the play frame starts at the
    // identity.
    IScriptEngine* const Engine = SceneProgramLoad(ScriptName);
    Csys const ScriptFrame = SceneProgramGetCsys(Engine, nullptr);
    Csys Frame;
    Frame.Pos.x = 0.0f;
    Frame.Pos.y = 0.0f;
    Frame.Pos.z = 0.0f;
    MatrixSetIdentity(Frame.Orient);

    // on a placement, the placement's frame taken back through the
    // script's own (the routine: Placement * ScriptFrame^-1), and its name.
    char const* PlacementName = nullptr;
    if (Placement != nullptr)
    {
        Csys const PlacementFrame = Placement->Frame;
        PlacementName = Placement->Name;
        Frame = PlacementFrame * ScriptFrame.get_inverse();
    }

    // the script, this character in the mirror, and the sequence's
    // flags: bind the mirror's first, loop, +0x05 set, no letterbox, spatial as asked.
    Sequence->AddScript(ScriptName, 0, &Frame, Engine);
    Sequence->Mirror.Add(this, nullptr);
    Sequence->BindMode = 1;
    Sequence->Field_0x05 = 1;
    Sequence->bLoop = 1;
    Sequence->bLetterbox = 0;
    Sequence->bApplySpatial = bSpatial;
    Sequence->bRestoreCamera = 0;
    Sequence->bHoldTime = 0;
    Sequence->Priority = 0;
    Sequence->Callback = nullptr;
    Sequence->CallbackA = nullptr;
    Sequence->CallbackB = nullptr;
    // named after the placement.
    if (PlacementName != nullptr)
        Sequence->SetName(PlacementName);
    return Sequence;
}

void FCharacterObject::SetOwnScript(char const* ScriptName, unsigned char bSpatial)
{
    // . 0x00445586..0x004455a3: already the own script.
    if (ScriptName != nullptr && OwnScriptName != nullptr &&
        _stricmp(ScriptName, OwnScriptName) == 0) // NEURONCOMPONENTS stricmp
    {
        return;
    }
    // .
    if (OwnScript != nullptr)
    {
        G_GfScriptManager.DestroySequence(OwnScript);
        OwnScript = nullptr;
        bOwnScriptPlaying = 0;
        malloc_strfree(&OwnScriptName);
    }
    // .
    malloc_strcpy(&OwnScriptName, ScriptName);
    OwnScript = CreateOwnScript(ScriptName, bSpatial);
    if (ScriptState != 0)
        return;
    if (OwnScript == nullptr)
        return;
    OwnScript->Play();
    bOwnScriptPlaying = 1;
}

void FCharacterObject::UpdateHotspotEnabled()
{
    // no behaviour (+0x1b0 == -1) and no role -> disabled.
    if (Props.BehaviorId == 0xffffffffu && Role == 0)
    {
        Hotspot->bEnabled = 0;
        return;
    }
    Hotspot->bEnabled = 1;
}

void FCharacterObject::Slot10(unsigned long /*Cookie*/, int const* /*Point*/)
{
    // (RET 8). Not while a window holds the mouse.
    if (Win3DAnyCaptured())
        return;
    // the name shows; the cursor by role.
    bHovered = 1;
    char const* Cursor = "talk_blowoff";
    if (Role == 2)
        Cursor = "talk_commodity_dealer";
    else if (Role == 3)
        Cursor = "talk_ship_dealer";
    else if (Role == 4)
        Cursor = "talk_equip_dealer";

    // with a behaviour the room knows, the cursor by its kind: none
    // shown (+0x0a clear) "talk_story"; else by +0x220 blowoff / bribe / rumor / mission /
    // info / story, another kind keeping the role's cursor.
    unsigned const BehaviorId = Props.BehaviorId;
    if (BehaviorId != 0xffffffffu)
    {
        MetaNode<ScriptBehavior>* const Node =
            FindBehaviorNode(GetClientLocationRoomScriptBehaviors(GetRoomRuntimeRoomId(Owner)),
                             BehaviorId);
        if (Node != nullptr && Node->Data != nullptr)
        {
            ScriptBehavior const* const Behavior = Node->Data;
            if (Behavior->Flag0a == 0)
            {
                Cursor = "talk_story";
            }
            else
            {
                unsigned const Kind = Behavior->Field220;
                if (Kind == 0)
                    Cursor = "talk_blowoff";
                else if (Kind == 1)
                    Cursor = "talk_bribe";
                else if (Kind == 2)
                    Cursor = "talk_rumor";
                else if (Kind == 3)
                    Cursor = "talk_mission";
                else if (Kind == 4)
                    Cursor = "talk_info";
                else if (Kind == 5)
                    Cursor = "talk_story";
            }
        }
    }
    MouseSetCursor(Cursor, false);
}

void FCharacterObject::Slot11(unsigned long /*Cookie*/, int const* /*Point*/)
{
    // (RET 8): unless a window holds the mouse, the arrow, no name, the tooltip
    // hidden (the routine).
    if (Win3DAnyCaptured())
        return;
    MouseSetCursor("arrow", false);
    bHovered = 0;
    NnToolTipHideGlobal();
}

void FCharacterObject::Slot12(unsigned long Cookie, int const* Point)
{
#ifdef OUTPOST_FRONTIER_WALK_MODE
    BaseWalkBeforeInteraction();
#endif
    // (RET 8). Not while a window holds the mouse.
    if (Win3DAnyCaptured())
        return;
    // the leave handling first (slot 11).
    Slot11(Cookie, Point);

    // the character's faction (+0x164) marked met in the visit map
    // (flags | 0x41, the server told) unless bit 0 is already set.
    unsigned const Faction = Props.FactionId;
    if (Faction != 0xffffffffu)
    {
        unsigned char Flags = 0;
        GetPlayerVisitFlagsbaseline(Faction, &Flags, 0, 0);
        if ((Flags & 1) == 0)
            SetPlayerVisitFlagsbaseline(Faction, static_cast<unsigned char>(Flags | 0x41), true);
    }

    // the NavBar may take the click; a character under a script's
    // control does nothing.
    if (NavBarCharacterClicked(this, Role))
        return;
    if (ScriptState != 0)
        return;

    // with a behaviour, its script (the routine, the dealer opened
    // when it stops) and the server told (IServer slot 41 GFObjSelect).
    if (Props.BehaviorId != 0xffffffffu)
    {
        StartCharacterBehaviorScript(this, Owner, Placement, Props.BehaviorId,
                                     &OnBehaviorScriptStopped, this);
        static_cast<IServer*>(GetLocalServerInterface())->GFObjSelect(Props.Key,
                                                                      GetPlayerConnectionId());
        return;
    }
    // else the role's dealer.
    OpenRoleDealer(Role);
}

void FCharacterObject::TakeOverForScript(unsigned char bKeepOwnScript)
{
#ifdef OUTPOST_FRONTIER_WALK_MODE
    BaseWalkCharacterScriptStarting(this);
#endif
    // . 0x004454e9 `JNZ` on a set argument goes straight to +0x04 = 1 (0x00445533).
    // Clear: a non-null +0x360 is handed to the routine on the field (0x004454fb), +0x360 =
    // 0, +0x368 = 0, malloc_strfree(&+0x364) -- all inside the non-null branch; then +0x36c = 1
    // and +0x04 = 1 (0x00445521..0x0044552c) either way.
    if (bKeepOwnScript == 0)
    {
        if (OwnScript != nullptr)
        {
            G_GfScriptManager.DestroySequence(OwnScript);
            OwnScript = nullptr;
            bOwnScriptPlaying = 0;
            malloc_strfree(&OwnScriptName);
        }
        ScriptState = 1;
    }
    bNoUpdate = 1;
}

void FCharacterObject::ResetScriptState(unsigned char bRelease)
{
    // . 0x00445548 writes +0x04 before the branch; 0x0044554b `JZ` on a zero
    // argument takes the +0x36c = 2 side.
    bNoUpdate = 0;
    if (bRelease == 0)
    {
        ScriptState = 2;
        return;
    }
    OwnScript = nullptr;
    bOwnScriptPlaying = 0;
    malloc_strfree(&OwnScriptName);
}

long FCharacterObject::FindPart(char const* PartName)
{
    // . The scan (0x00445631..0x0044566d) returns a used entry whose name matches
    // (NEURONCOMPONENTS stricmp) and whose part is not -1; on the way it remembers the first unused entry
    // (0x00445659 `TEST EBX,EBX; JGE`). None unused -> entry 0 (0x00445671). The lookup runs on
    // slot 6's instance; a hit replaces the entry's part and name (CRT free / _strdup, not the
    // malloc_str* pair) and marks it used. The result is returned either way.
    int Unused = -1;
    int Index = 0;
    for (; Index < kPartCacheEntries; ++Index)
    {
        FPartCacheEntry const& Entry = Parts[Index];
        if (Entry.bUsed != 0 && Entry.Name != nullptr && _stricmp(Entry.Name, PartName) == 0 &&
            Entry.Part != -1)
            return Entry.Part;
        if (Unused < 0 && Entry.bUsed == 0)
            Unused = Index;
    }
    if (Unused < 0)
        Unused = 0;

    long const Part = CompoundInstanceFindFirstName(
        static_cast<long>(reinterpret_cast<intptr_t>(GetEngineInstance())), PartName);
    if (Part != -1)
    {
        FPartCacheEntry& Entry = Parts[Unused];
        Entry.Part = Part;
        if (Entry.Name != nullptr)
        {
            std::free(Entry.Name);
            Entry.Name = nullptr;
        }
        Entry.Name = _strdup(PartName);
        Entry.bUsed = 1;
    }
    return Part;
}

void FCharacterObject::AddMotionHandle(int Handle)
{
    // .
    if (Handle != -1)
        ++ActiveMotions;
}

void FCharacterObject::RemoveMotionHandle(int Handle)
{
    // .
    if (Handle != -1)
        --ActiveMotions;
}

void FCharacterObject::AddAimHandle(void* Handle)
{
    // .
    if (reinterpret_cast<intptr_t>(Handle) != -1)
        ++ActiveAims;
}

void FCharacterObject::RemoveAimHandle(void* Handle)
{
    // .
    if (reinterpret_cast<intptr_t>(Handle) != -1)
        --ActiveAims;
}

char const* FCharacterObject::ResolveBgcsContextToken(char const* TemplateName) const
{
    // . Every exit returns the field (0x00445a44 / 0x00445923 / 0x0044599a, and
    // EAX loaded before the 0x00445871 branch). baseline's Universe::IBase calls go through its
    // vtable (+0x04 nickname, +0x14 BGCS_base_run_by); the port's IBase publishes the same two
    // strings as fields.

    // GetCostumeDescriptions()->get_costume_gender(this +0x178):
    // 1 -> "gcs_ml_" (the field), 2 -> "gcs_fm_" (the field), else "" (the field) and
    // the FDUMP at ObjectDeformable.cpp:0x51f.
    int const Gender = GetCostumeDescriptions()->get_costume_gender(Props.Cost);
    if (Gender == 1)
        strcpy(G_BgcsContextBuffer, "gcs_ml_");
    else if (Gender == 2)
        strcpy(G_BgcsContextBuffer, "gcs_fm_");
    else
    {
        G_BgcsContextBuffer[0] = '\0';
        FDUMP(0x100001, "%s(%d) : *** ERROR: %s", kObjectDeformableCpp, 0x51f,
              "Invalid gender for character, cannot find GCS voice profile message name\n");
    }

    unsigned const BaseId = GetRoomRuntimeBaseId(Owner); // [[this+8]+0x34]

    if (_stricmp(TemplateName, "$BGCS_Base_Name") == 0)
    {
        // append "W02aF_" (the field..0e), then the base nickname,
        // then drop a trailing "_base" (s__base_005cbb00) when the whole buffer is longer.
        strcat(G_BgcsContextBuffer, "W02aF_");
        strcat(G_BgcsContextBuffer, Universe::get_base(BaseId)->Nickname);
        int const Length = static_cast<int>(strlen(G_BgcsContextBuffer));
        int const SuffixLength = static_cast<int>(strlen("_base"));
        if (Length <= SuffixLength)
            return G_BgcsContextBuffer;
        char* const Suffix = G_BgcsContextBuffer + (Length - SuffixLength);
        if (_stricmp(Suffix, "_base") != 0)
            return G_BgcsContextBuffer;
        *Suffix = '\0';
        return G_BgcsContextBuffer;
    }

    if (_stricmp(TemplateName, "$BGCS_Base_Run_By") == 0)
    {
        // append the base's BGCS_base_run_by; an empty one is reported
        // with a second get_base for the nickname (ObjectDeformable.cpp:0x54a).
        // get_base, then IBase vt+0x14 (the BGCS string's c_str).
        char const* const RunBy = Universe::get_base(BaseId)->GetBgcsBaseRunBy();
        strcat(G_BgcsContextBuffer, RunBy);
        if (RunBy[0] != '\0')
            return G_BgcsContextBuffer;
        FDUMP(0x100001,
              "%s(%d) : *** ERROR: No BGCS_base_run_by specified for base %s, cannot resolve "
              "voice profile message %s\n",
              kObjectDeformableCpp, 0x54a, Universe::get_base(BaseId)->Nickname, TemplateName);
        return G_BgcsContextBuffer;
    }

    // My_Faction ML/FM -> "D06aG_" (the field..42), Faction_Rebuff
    // ML/FM -> "A03aG_" (the field..4a); anything else is ObjectDeformable.cpp:0x571.
    char const* Prefix = nullptr;
    if (_stricmp(TemplateName, "$BGCS_My_Faction_ML") == 0 ||
        _stricmp(TemplateName, "$BGCS_My_Faction_FM") == 0)
    {
        Prefix = "D06aG_";
    }
    else if (_stricmp(TemplateName, "$BGCS_Faction_Rebuff_ML") == 0 ||
             _stricmp(TemplateName, "$BGCS_Faction_Rebuff_FM") == 0)
    {
        Prefix = "A03aG_";
    }
    else
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: Unknown GCS generic voice profile message %s\n",
              kObjectDeformableCpp, 0x571, TemplateName);
        return G_BgcsContextBuffer;
    }

    // append the prefix, then Reputation::get_nickname of +0x164 into a
    // zeroed TString<16> (0x00445a02 / 0x00445a07) and append its text.
    strcat(G_BgcsContextBuffer, Prefix);
    TString<16> Nickname;
    Nickname.Length = 0;
    Nickname.Data[0] = '\0';
    Reputation::get_nickname(Nickname, Props.FactionId);
    strcat(G_BgcsContextBuffer, Nickname.Data);
    return G_BgcsContextBuffer;
}

// ============================================================================================
// Free functions
// ============================================================================================

void ReleaseSharedCharacterLod()
{
    // IDeformable slot 6 on a live instance, then the instance and both
    // four-entry arrays zeroed.
    if (G_SharedCharacterLod.Instance != nullptr)
        NeuronRuntime::Deform->Destroy(G_SharedCharacterLod.Instance);
    G_SharedCharacterLod.Instance = nullptr;
    int Index = 0;
    for (Index = 0; Index < 4; ++Index)
    {
        G_SharedCharacterLod.Counts[Index] = 0;
        G_SharedCharacterLod.Tables[Index] = nullptr;
    }
}

FCharacterObject* CreateCharacterObject(CharacterProperties const& Props, CharPlaceInfo* Placement,
                                        int PlacementIndex, FRoomRuntime* Owner,
                                        unsigned char bScriptSpatial, Csys const& Frame)
{
    // . 0x00435b9d..0x00435c23: the description (the routine) filled from the
    // arguments.
    FCharacterDesc Desc;
    Desc.Props = Props;
    Desc.Placement = Placement;
    Desc.PlacementIndex = PlacementIndex;
    Desc.bScriptSpatial = bScriptSpatial;
    Desc.Pos = Frame.Pos;
    Desc.Orient = Frame.Orient;
    // operator new(0x3b0) + the routine(Owner, &Desc); a failed
    // allocation gives null. 0x00435c66: the description's destructor (the routine).
    FCharacterObject* const Character = new (std::nothrow) FCharacterObject(Owner, Desc);
    return Character;
}

void StartCharacterBehaviorScript(FCharacterObject* Character, FRoomRuntime* Room,
                                  CharPlaceInfo const* Placement, unsigned BehaviorKey,
                                  void(__cdecl* Callback)(void*, void*, unsigned char),
                                  void* CallbackA)
{
    // . 0x00435dd5..0x00435e1b: the room's behaviour of that key (the routine +
    // ); no node or no record, nothing.
    unsigned const RoomId = (Room != nullptr) ? GetRoomRuntimeRoomId(Room) : 0;
    char const* const PlacementName = (Placement != nullptr) ? Placement->Name : nullptr;
    MetaNode<ScriptBehavior>* const Node =
        FindBehaviorNode(GetClientLocationRoomScriptBehaviors(RoomId), BehaviorKey);
    if (Node == nullptr)
        return;
    ScriptBehavior const* const Behavior = Node->Data;
    if (Behavior == nullptr)
        return;

    // .
    FGfScriptSequence* const Sequence = G_GfScriptManager.CreateSequence(Room, 0);
    if (Sequence == nullptr)
        return;

    // each of the behaviour's scripts with a file, at the identity.
    Csys Frame;
    Frame.Pos.x = 0.0f;
    Frame.Pos.y = 0.0f;
    Frame.Pos.z = 0.0f;
    MatrixSetIdentity(Frame.Orient);
    int Index = 0;
    for (Index = 0; Index < Behavior->StringArray2Count; ++Index)
    {
        char const* const File = Behavior->StringArray2[Index];
        if (File == nullptr || File[0] == '\0')
            continue;
        Sequence->AddScript(File, static_cast<int>(Behavior->StringArray2Dword[Index]), &Frame,
                            nullptr);
    }

    // the dialog (+0x220 / +0x224 / +0x228 and the two texts).
    Sequence->SetDialog(static_cast<int>(Behavior->Field220), static_cast<int>(Behavior->Field224),
                        static_cast<int>(Behavior->Field228), Behavior->NameFmtStr,
                        Behavior->DescFmtStr);

    // +0x14 set -- this character alone, bound as the mirror's first;
    // clear -- the listed characters by key under their names, bound by name.
    int BindMode = 0;
    if (Behavior->Flag14 != 0)
    {
        Sequence->Mirror.Add(Character, nullptr);
        BindMode = 1;
    }
    else
    {
        BindMode = 2;
        for (Index = 0; Index < Behavior->IdArrayCount; ++Index)
        {
            char const* const Name = Behavior->StringArray[Index];
            FCharacterObject* const Listed =
                RoomControlFindCharacterByKey(Room, Behavior->IdArray[Index]);
            Sequence->Mirror.Add(Listed, Name);
        }
    }

    // letterboxed, named after the placement, the stop callback, the
    // ambience and music ducked, the behaviour's location / skip / server fields, +0x16 for a
    // dealer role; then played.
    Sequence->BindMode = BindMode;
    Sequence->bLetterbox = 1;
    Sequence->bApplySpatial = Behavior->Flag08;
    Sequence->SetName(PlacementName);
    Sequence->Priority = 0;
    Sequence->Callback = Callback;
    Sequence->CallbackB = nullptr;
    Sequence->bDuckAmbient = 1;
    Sequence->bDuckMusic = 1;
    Sequence->CallbackA = CallbackA;
    Sequence->Location = Behavior->Field0c;
    Sequence->Field_0x15 = Behavior->Flag0a;
    Sequence->RtcId = Behavior->Field25c;
    int const Role = Character->Role;
    Sequence->Field_0x16 = (Role == 2 || Role == 3 || Role == 4) ? 1 : 0;
    Sequence->Play();
}

void PlacePlayerAtSetpoint(char const* SetpointName)
{
    // . 0x00435cf7..0x00435d1b: the current room runtime (the routine) and the
    // current location's RoomData (the routine on the field); neither is null-tested.
    FRoomRuntime* const Runtime = static_cast<FRoomRuntime*>(GetClientCurrentLocationRuntime());
    RoomData* const Room = LookupClientLocationRoom(GetClientLocationCurrentRoom());

    // the first setpoint (+0x164, the size re-read every pass) whose
    // name stricmp-equals SetpointName; none -> return.
    SetpointInfo const* Found = nullptr;
    size_t Index = 0;
    for (Index = 0; Index < Room->Setpoints.size(); ++Index)
    {
        SetpointInfo const* const Setpoint = Room->Setpoints[Index];
        if (_stricmp(Setpoint->Name, SetpointName) == 0) // NEURONCOMPONENTS stricmp
        {
            Found = Setpoint;
            break;
        }
    }
    if (Found == nullptr)
        return;

    // the frame copied out (position, then the nine-float orientation).
    Vector Pos = Found->Frame.Pos;
    Matrix Orient = Found->Frame.Orient;

    // the room's player character (the routine); with one,
    // IDeformable slot 19 (position) and slot 20 (orientation) on its +0x30 instance, then its
    // slot 3 (+0x0c) with the position's Y.
    FCharacterObject* const Player = RoomControlPlayerCharacter(Runtime);
    if (Player == nullptr)
        return;
    void* const Instance = Player->Lod.Instance;
    NeuronRuntime::Deform->SetPosePos(Instance, &Pos.x);
    NeuronRuntime::Deform->SetPoseOri(Instance, &Orient);
    Player->SetFloat1C(Pos.y);
}
