// Author: Giperion.

// Common.dll room sub-records: RoomCameraInfo, SetpointProperties, SetpointInfo, CharPlaceInfo,
// ShipPlaceInfo, RoomButtonInfo, RoomLightInfo, RoomPropInfo and ExhaustNozzleInfo.
// Split out of common_goods.cpp.

#include "common_goods.h"
#include "common_misc.h" // malloc_strcpy / malloc_strfree
#include "neuroncomponents.h"

#include <cstdio>
#include <cstring>

// common_neuroncomponents_stricmp.cpp: NEURONCOMPONENTS.DLL!stricmp (Common IAT PTR 0639902c).
int CallNeuronComponentsStricmp(const char* Left, const char* Right);

// ---------------------------------------------------------------------------
// Room sub-structure implementations
// ---------------------------------------------------------------------------

RoomCameraInfo::RoomCameraInfo()
    : Name(nullptr)
    , Fov(35.0f)
    , NearPlane(0.25f)
    , FarPlane(1000000.0f)
{
    Frame.set_identity();
}

RoomCameraInfo::~RoomCameraInfo()
{
    // Common 0x13300: dtor is ICF-shared with destroy() -- frees Name before
    // resetting fields. Was previously an empty body, leaking every allocated Name.
    destroy();
}

// no self-assignment test. destroy() inlined (malloc_strfree(&Name), Name = 0,
// identity Frame, 35 / 0.25 / 1e6), then malloc_strcpy(&Name, Other.Name), the 0xc-dword
// Frame copy and the three floats. Assigning an object to itself therefore drops its Name
// and resets its Frame, as baseline.
RoomCameraInfo& RoomCameraInfo::operator=(RoomCameraInfo const& Other)
{
    destroy();
    malloc_strcpy(const_cast<char**>(&Name), Other.Name);
    Frame = Other.Frame;
    Fov = Other.Fov;
    NearPlane = Other.NearPlane;
    FarPlane = Other.FarPlane;
    return *this;
}

void RoomCameraInfo::clear()
{
    Name = nullptr;
    Frame.set_identity();
    Fov = 35.0f;
    NearPlane = 0.25f;
    FarPlane = 1000000.0f;
}

void RoomCameraInfo::destroy()
{
    // baseline 0x13300 (ICF-shared with dtor):
    // SUB ESP,0xc; PUSH ESI,EDI; MOV ESI,this;
    // PUSH ESI; CALL malloc_strfree (frees Name at +0x00);
    // LEA EAX,[ESI+0x4]; XOR ECX,ECX; MOV [ESI],ECX (Name=nullptr);
    // MOV EDI,ECX; MOV EDX,EAX; zero Frame rotation (9 dwords);
    // Set diagonal to 1.0f (0x3f800000) at +0x2c,+0x1c,+0x0c;
    // Zero remaining Frame fields; set Fov/NearPlane/FarPlane defaults.
    // Equivalent to malloc_strfree(&Name) + clear().
    malloc_strfree(const_cast<char**>(&Name));
    clear();
}

// SetpointProperties strings: +0x08 (Segment) is owned through malloc_strcpy /
// malloc_strfree in every baseline writer (0627b220, 0627b501, 0627b1d8), never StringAlloc.

// bytes +0/+1 = 0, +4 = -1, +8 = 0, +0xc = -1, byte +0x10 = 0, +0x20 = -1.
// +2/+3 and NameBuffer[1..15] are not written.
SetpointProperties::SetpointProperties()
    : bValid(0)
    , bZoneScript(0)
    , CharType(-1)
    , Segment(nullptr)
    , Zone(-1)
    , Posture(-1)
{
    NameBuffer[0] = '\0';
}

// (ICF-shared with destroy()): malloc_strfree(&+8), then the clear() stores.
SetpointProperties::~SetpointProperties()
{
    destroy();
}

// +0/+1 bytes, +4, malloc_strcpy(&+8, Other+8) (strdup, a null source stores null),
// +0xc, copy-until-NUL of +0x10, +0x20.
SetpointProperties::SetpointProperties(SetpointProperties const& Other)
    : bValid(Other.bValid)
    , bZoneScript(Other.bZoneScript)
    , CharType(Other.CharType)
    , Segment(nullptr)
{
    malloc_strcpy(const_cast<char**>(&Segment), Other.Segment);
    Zone = Other.Zone;
    strcpy(NameBuffer, Other.NameBuffer);
    Posture = Other.Posture;
}

// (??4SetpointProperties, ICF-folded with MissileUseStruct::operator=): a plain
// 9-dword block copy. +0x08 is copied as a pointer, so the two objects share Segment and the
// old one leaks -- baseline's own hazard, kept.
SetpointProperties& SetpointProperties::operator=(SetpointProperties const& Other)
{
    bValid = Other.bValid;
    bZoneScript = Other.bZoneScript;
    Pad02 = Other.Pad02;
    Pad03 = Other.Pad03;
    CharType = Other.CharType;
    Segment = Other.Segment;
    Zone = Other.Zone;
    memcpy(NameBuffer, Other.NameBuffer, sizeof(NameBuffer));
    Posture = Other.Posture;
    return *this;
}

// same stores as the ctor. +0x08 is overwritten with 0 WITHOUT being freed (baseline
// leaks it here; destroy() is the freeing variant).
void SetpointProperties::clear()
{
    bValid = 0;
    bZoneScript = 0;
    CharType = -1;
    Segment = nullptr;
    Zone = -1;
    NameBuffer[0] = '\0';
    Posture = -1;
}

// PUSH &+8; CALL malloc_strfree; then +8 = 0 and the clear() stores.
void SetpointProperties::destroy()
{
    malloc_strfree(const_cast<char**>(&Segment));
    clear();
}

// copy-until-NUL of NewSegment into +0x10 (no length check, no null check), then
// toupper over strlen(+0x10) bytes. The unbounded copy is baseline's (a source longer than 15
// characters runs into Posture at +0x20); reproduced as-is under the owner's UB ruling.
// toupper takes the byte through unsigned char: for every byte value the C-locale result is
// the one MSVCRT gives the sign-extended char baseline passes.
void SetpointProperties::set_segment(char const* NewSegment)
{
    strcpy(NameBuffer, NewSegment);
    size_t Length = strlen(NameBuffer);
    size_t Index = 0;
    for (Index = 0; Index < Length; ++Index)
        NameBuffer[Index] = (char)toupper((unsigned char)NameBuffer[Index]);
}

namespace
{
    const char kSetpointSource[] = "Common\\SetpointProperties.cpp";

    // combine_to_name's result buffer (0x400 bytes).
    char GSetpointNameBuffer[0x400];
}

// . Name convention "zs|zg / pc|npc|none|rpc / <+8> / <zone> / <+0x10> / posture".
// No reset of the object first: fields not reached keep their previous values.
void SetpointProperties::extract_from_name(char const* FullName)
{
    // a malloc_strcpy'd work copy, lowered byte by byte over strlen bytes
    // (tolower through unsigned char gives the C-locale result MSVCRT gives the sign-extended
    // char baseline passes). A null name crashes in the strlen, as baseline.
    char* Work = nullptr;
    malloc_strcpy(&Work, FullName);
    size_t Length = strlen(Work);
    size_t Index = 0;
    for (Index = 0; Index < Length; ++Index)
        Work[Index] = (char)tolower((unsigned char)Work[Index]);

    // six strtok calls on "/".
    char* PrefixToken = strtok(Work, "/");
    char* CharTypeToken = strtok(nullptr, "/");
    char* SegmentToken = strtok(nullptr, "/");
    char* ZoneToken = strtok(nullptr, "/");
    char* NameToken = strtok(nullptr, "/");
    char const* PostureToken = strtok(nullptr, "/");

    // the prefix is tested first (REPE CMPSB over 3 bytes = strcmp on the
    // lowered copy). Neither "zs" nor "zg": bValid = 0, free the work copy, return -- no dump.
    if (strcmp(PrefixToken, "zs") != 0 && strcmp(PrefixToken, "zg") != 0)
    {
        bValid = 0;
        malloc_strfree(&Work);
        return;
    }

    // a missing posture token becomes "" (module storage); bValid = 1;
    // a missing token 0..4 is reported, and parsing carries on regardless (a null token then
    // crashes in the compare/copy below, exactly as baseline does).
    if (PostureToken == nullptr)
        PostureToken = "";
    bValid = 1;
    if (PrefixToken == nullptr || CharTypeToken == nullptr || SegmentToken == nullptr ||
        ZoneToken == nullptr || NameToken == nullptr)
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: setpoint name %s does not match naming convention\n",
              kSetpointSource, 0x78, FullName);
    }


    if (strcmp(PrefixToken, "zs") == 0)
        bZoneScript = 1;
    else if (strcmp(PrefixToken, "zg") == 0)
        bZoneScript = 0;
    else
        FDUMP(0x100001, "%s(%d) : *** ERROR: setpoint name prefix %s (of %s) is not zs/ or zg/\n",
              kSetpointSource, 0x87, PrefixToken, FullName);


    if (strcmp(CharTypeToken, "pc") == 0)
        CharType = 1;
    else if (strcmp(CharTypeToken, "npc") == 0)
        CharType = 0;
    else if (strcmp(CharTypeToken, "none") == 0)
        CharType = -1;
    else if (strcmp(CharTypeToken, "rpc") == 0)
        CharType = 2;
    else
        FDUMP(0x100001,
              "%s(%d) : *** ERROR: setpoint character type %s (of %s) is not pc, npc, none, or rpc\n",
              kSetpointSource, 0x9e, CharTypeToken, FullName);

    // malloc_strcpy into +0x08 without freeing the previous value.
    malloc_strcpy(const_cast<char**>(&Segment), SegmentToken);


    char* ZoneEnd = nullptr;
    Zone = strtol(ZoneToken, &ZoneEnd, 10);
    if (ZoneEnd != nullptr && *ZoneEnd != '\0')
        FDUMP(0x100001, "%s(%d) : *** ERROR: setpoint zone number %s (of %s) is not a number\n",
              kSetpointSource, 0xac, ZoneToken, FullName);

    // set_segment inlined.
    set_segment(NameToken);


    if (strcmp(PostureToken, "stand") == 0)
        Posture = 0;
    else if (strcmp(PostureToken, "sitlow") == 0)
        Posture = 1;
    else if (strcmp(PostureToken, "sithigh") == 0)
        Posture = 2;
    else
    {
        if (bZoneScript == 0)
            FDUMP(0x100001,
                  "%s(%d) : *** ERROR: invalid posture %s in setpoint marker %s - if this is a special "
                  "relocation marker, mark it with Zs\n",
                  kSetpointSource, 0xc8, PostureToken, FullName);
        Posture = 3;
    }


    malloc_strfree(&Work);
}

// rebuilds the name into the shared buffer with unchecked appends.
char const* SetpointProperties::combine_to_name() const
{
    if (bValid == 0)
        return nullptr;

    GSetpointNameBuffer[0] = '\0';
    strcat(GSetpointNameBuffer, bZoneScript != 0 ? "Zs/" : "Zg/");

    if (CharType == 1)
        strcat(GSetpointNameBuffer, "pc/");
    else if (CharType == 2)
        strcat(GSetpointNameBuffer, "rpc/");
    else if (CharType == 0)
        strcat(GSetpointNameBuffer, "npc/");
    else
        FDUMP(0x100001, "%s(%d) : *** ERROR: %s", kSetpointSource, 0xfe,
              "Invalid occupancy value for setpoint\n");

    // +0x08 appended with no null test (a null Segment faults, as baseline).
    strcat(GSetpointNameBuffer, Segment);
    strcat(GSetpointNameBuffer, "/");

    char ZoneText[0x40];
    sprintf(ZoneText, "%02d/", Zone);
    strcat(GSetpointNameBuffer, ZoneText);

    strcat(GSetpointNameBuffer, NameBuffer);
    strcat(GSetpointNameBuffer, "/");

    if (Posture == 0)
        strcat(GSetpointNameBuffer, "stand");
    else if (Posture == 1)
        strcat(GSetpointNameBuffer, "sitlow");
    else if (Posture == 2)
        strcat(GSetpointNameBuffer, "sithigh");
    else if (Posture == 3)
        strcat(GSetpointNameBuffer, "special");
    else
        FDUMP(0x100001, "%s(%d) : *** ERROR: %s", kSetpointSource, 0x127,
              "Invalid posture value for setpoint\n");

    return GSetpointNameBuffer;
}

SetpointInfo::SetpointInfo()
    : Name(nullptr)
{
    Frame.set_identity();
}

SetpointInfo::SetpointInfo(SetpointInfo const& Other)
    : Name(Other.Name)
    , Frame(Other.Frame)
    , Props(Other.Props)
{
    // +0x00 Name copied as a pointer (no malloc_strcpy), the 0x0c-dword Csys
    // block copy at +0x04, then SetpointProperties' copy ctor on +0x34. The copy shares Name
    // with Other, as in baseline.
}

SetpointInfo::~SetpointInfo()
{
    destroy();
}

void SetpointInfo::clear()
{
    Name = nullptr;
    Frame.set_identity();
    Props.clear();
}

// malloc_strfree(&Name), Props.destroy() (CALL 0627b1d0), then Name = 0, Frame
// identity and a tail-jump into Props.clear() -- i.e. clear().
void SetpointInfo::destroy()
{
    malloc_strfree(const_cast<char**>(&Name));
    Props.destroy();
    clear();
}

CharPlaceInfo::CharPlaceInfo()
    : Name(nullptr)
    , CharType(-1)
    , Posture(-1)
    , bZoneScript(0)
    , StartScript(nullptr)
{
    Frame.set_identity();
}

CharPlaceInfo::~CharPlaceInfo()
{
    destroy();
}

void CharPlaceInfo::clear()
{
    Name = nullptr;
    Frame.set_identity();
    CharType = -1;
    Posture = -1;
    bZoneScript = 0;
    StartScript = nullptr;
}

// (ICF-shared with the dtor): malloc_strfree +0x00 and +0x40, then the clear() stores.
void CharPlaceInfo::destroy()
{
    malloc_strfree(const_cast<char**>(&Name));
    malloc_strfree(const_cast<char**>(&StartScript));
    clear();
}

// false for a null Name, else NEURONCOMPONENTS.DLL!stricmp(Name, TestName) == 0.
bool CharPlaceInfo::is_named(char const* TestName) const
{
    if (Name == nullptr)
        return false;
    return CallNeuronComponentsStricmp(Name, TestName) == 0;
}

ShipPlaceInfo::ShipPlaceInfo()
    : Nickname(nullptr)
    , bIsPlayerShip(false)
    , bIsForSale(false)
    , LandingScript(nullptr)
    , LaunchingScript(nullptr)
{
    Frame.set_identity();
}

ShipPlaceInfo::~ShipPlaceInfo()
{
    destroy();
}

void ShipPlaceInfo::clear()
{
    Nickname = nullptr;
    Frame.set_identity();
    bIsPlayerShip = false;
    bIsForSale = false;
    LandingScript = nullptr;
    LaunchingScript = nullptr;
}

void ShipPlaceInfo::destroy()
{
    // (ICF-shared with the dtor): malloc_strfree +0x00, +0x38, +0x3c, then the
    // clear() stores.
    malloc_strfree(const_cast<char**>(&Nickname));
    malloc_strfree(const_cast<char**>(&LandingScript));
    malloc_strfree(const_cast<char**>(&LaunchingScript));
    clear();
}

RoomButtonInfo::RoomButtonInfo()
    : Name(nullptr)
    , RoomSwitch(nullptr)
    , VirtualRoom(nullptr)
    , SetVirtualRoom(nullptr)
    , Behavior(-1)
    , StateSend(0)
    , StateRead(0)
    , StateTransition{nullptr, nullptr, nullptr, nullptr}
{
}

RoomButtonInfo::~RoomButtonInfo()
{
    destroy();
}

void RoomButtonInfo::clear()
{
    Name = nullptr;
    RoomSwitch = nullptr;
    VirtualRoom = nullptr;
    SetVirtualRoom = nullptr;
    Behavior = -1;
    StateSend = 0;
    StateRead = 0;
    StateTransition[0] = nullptr;
    StateTransition[1] = nullptr;
    StateTransition[2] = nullptr;
    StateTransition[3] = nullptr;
}

void RoomButtonInfo::destroy()
{
    // (ICF-shared with the dtor): malloc_strfree +0x00/+0x04/+0x08/+0x0c, then the
    // four +0x1c.. transitions in a counted loop, then the clear() stores.
    malloc_strfree(const_cast<char**>(&Name));
    malloc_strfree(const_cast<char**>(&RoomSwitch));
    malloc_strfree(const_cast<char**>(&VirtualRoom));
    malloc_strfree(const_cast<char**>(&SetVirtualRoom));
    int TransitionIndex = 0;
    for (TransitionIndex = 0; TransitionIndex < 4; ++TransitionIndex)
        malloc_strfree(const_cast<char**>(&StateTransition[TransitionIndex]));
    clear();
}

RoomLightInfo::RoomLightInfo()
    : Type(0)
{
    // Common 0x13410: Frame=identity, then this+0x30/+0x34/+0x38 = (0, 0, -1.0f),
    // NOT (1,1,1). The earlier note here guessed this field was a mislabeled direction; that
    // is now PROVEN -- read_set_script's named-light branch rotates and normalizes the very
    // same SceneScriptLightProperties+0xC triple (see RoomLightInfo::Direction in common_goods.h),
    // and the field has been renamed Color -> Direction accordingly.
    Frame.set_identity();
    Direction[0] = 0.0f;
    Direction[1] = 0.0f;
    Direction[2] = -1.0f;
    // baseline leaves everything from +0x3C to +0xA3 (Mode, Color and Opaque4C) uninitialized
    // here -- the ctor jumps straight from the +0x38 store to zeroing the +0xa4 Type byte.
    // Not zero-filled on purpose, to match baseline; add_set_script_light overwrites the whole
    // range before use.
}

RoomLightInfo::~RoomLightInfo()
{
    destroy();
}

void RoomLightInfo::clear()
{
    // Common 0x13490: byte-identical to the constructor above, including leaving
    // Mode/Color/Opaque4C untouched (see constructor's comment).
    Frame.set_identity();
    Direction[0] = 0.0f;
    Direction[1] = 0.0f;
    Direction[2] = -1.0f;
    Type = 0;
}

RoomLightInfo& RoomLightInfo::operator=(RoomLightInfo const& Other)
{
    // Common 0x1290: plain 0x2a-dword (0xA8-byte) memberwise copy, no self-assignment
    // check (unlike RoomButtonInfo/RoomPropInfo, RoomLightInfo owns no allocated memory).
    Frame = Other.Frame;
    Direction[0] = Other.Direction[0];
    Direction[1] = Other.Direction[1];
    Direction[2] = Other.Direction[2];
    Mode = Other.Mode;
    Color[0] = Other.Color[0];
    Color[1] = Other.Color[1];
    Color[2] = Other.Color[2];
    memcpy(Opaque4C, Other.Opaque4C, sizeof(Opaque4C));
    Type = Other.Type;
    return *this;
}

void RoomLightInfo::destroy()
{
    // baseline 0x48920 (ICF-folded): single RET instruction — empty function.
    // This is shared with ~18 other trivial functions via ICF folding.
}

RoomPropInfo::RoomPropInfo()
    : Template(nullptr)
    , Name(nullptr)
    , Category(-1)
    , bVisible(true)
    , bLit(true)
    , bFog(true)
    , bRoomMain(false)
    , LightGroup(0)
    , Layer(0)
    , RenderType(0)
    , Loadout(nullptr)
    , PriorityId(0)
    , bRunningLights(false)
{
    // Common 0x13510: Category defaults to -1 (0xffffffff) and bFog (+0x3e) defaults
    // to true -- both were wrong here (0 and false respectively) before this fix.
    Frame.set_identity();
}

RoomPropInfo::~RoomPropInfo()
{
    destroy();
}

RoomPropInfo& RoomPropInfo::operator=(RoomPropInfo const& Other)
{
    // `MOV ECX,0x15; MOVSD.REP` -- the whole 0x54-byte record is block-copied,
    // strings included as raw pointers; the old strings are not freed and there is no
    // self-assignment test (copying onto itself is a no-op anyway).
    Template = Other.Template;
    Name = Other.Name;
    Category = Other.Category;
    Frame = Other.Frame;
    bVisible = Other.bVisible;
    bLit = Other.bLit;
    bFog = Other.bFog;
    bRoomMain = Other.bRoomMain;
    LightGroup = Other.LightGroup;
    Layer = Other.Layer;
    RenderType = Other.RenderType;
    Loadout = Other.Loadout;
    PriorityId = Other.PriorityId;
    bRunningLights = Other.bRunningLights;
    return *this;
}

void RoomPropInfo::clear()
{
    // Common 0x135b0: Category=-1 and bFog=true (both were wrong here, same
    // bug as the constructor above). Matches baseline in not freeing Template/Name/
    // Loadout before nulling them (that happens in destroy(), not clear()).
    Template = nullptr;
    Name = nullptr;
    Category = -1;
    Frame.set_identity();
    bVisible = true;
    bLit = true;
    bFog = true;
    bRoomMain = false;
    LightGroup = 0;
    Layer = 0;
    RenderType = 0;
    Loadout = nullptr;
    PriorityId = 0;
    bRunningLights = false;
}

void RoomPropInfo::destroy()
{
    // (ICF-shared with the dtor): malloc_strfree +0x00, +0x04, +0x48 and nothing
    // else -- no clear(); the other fields keep their values.
    malloc_strfree(const_cast<char**>(&Template));
    malloc_strfree(const_cast<char**>(&Name));
    malloc_strfree(const_cast<char**>(&Loadout));
}

ExhaustNozzleInfo::ExhaustNozzleInfo()
    : SubObjId(SUBOBJ_ID_NONE)
    , Pad02(0)
    , Index(0)
    , ParentInstance(0)
{
    memset(&Offset, 0, sizeof(Offset));
}

ExhaustNozzleInfo& ExhaustNozzleInfo::operator=(ExhaustNozzleInfo const& Other)
{
    SubObjId = Other.SubObjId;
    Pad02 = Other.Pad02;
    Index = Other.Index;
    ParentInstance = Other.ParentInstance;
    Offset = Other.Offset;
    return *this;
}

bool ExhaustNozzleInfo::GetHardpointOffset_NS(Transform& Out) const
{
    // Common 0x35bf0 / Common (raw asm)
    // baseline: offset 0 is a ushort SubObjId compared against the SUBOBJ_ID_NONE global
    // sentinel (0x0000, read out of module storage -- confirmed live in the analysis, not
    // a hardcoded literal), not a char* HardpointName as previously modeled.
    if (SubObjId == SUBOBJ_ID_NONE)
    {
        return false;
    }
    Out = Offset;
    return true;
}
