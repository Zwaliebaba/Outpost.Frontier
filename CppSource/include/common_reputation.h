// Author: Giperion.

#pragma once

#include "common_exports.h"

// Common.dll Reputation + CostumeDescriptions (bodyparts / costumes).
// Linked into OutpostFrontier.exe. Not a Common.dll project. Not MSVCP60 STL.
// See implementation notes (reputation, costumes).

#include "common_cobjdb.h"
#include "common_id.h"
#include "common_text.h"

#include <cstddef>
#include <cstring>
#include <list>
#include <map>
#include <string>
#include <utility>
#include <vector>

class INI_Reader;
// The save writer Reputation::Save writes through. Declared once, in server_savefile.h
// (Server implements it); baseline's export spells it `struct ISave` at global scope.
struct ISave;

// Per-bodypart level-of-detail switch-distance curve, read from the "DetailSwitchTable" INI
// section nested under a bodypart definition (Common 0x062fecb0-0x062fef70ish). baseline
// keeps a plain vector<pair<Key,Value>> of "switch=" entries (appended in file order via
// add_table_entry, a hand-inlined vector push_back -- confirmed COMDAT-folded generic STL vector
// insert, see the routine) plus two derived camera constants ("fovx" halved, and "sizex"),
// used only by adjust_distance. get_switch_distance's bracket search only makes sense if the ini
// data is written with Key descending (confirmed by its own comparison logic, not assumed).

// The old "not currently wired up anywhere / consumed and discarded" note that used to sit here
// was DISPROVEN and has been removed: CostumeDescriptions::load_bodyparts (0x06300570) really
// does construct one of these per "[DetailSwitchTable]" section, fill it via read_from_ini
// (module storage), push it into a CostumeDescriptions-owned std::list (module storage,
// ), and thread the address of the just-pushed element
// (`*(this+0x28) -> +4 -> +8`, module storage-06300a10) into every subsequent
// load_bodypart call as its 3rd argument -- see
// `?load_bodypart@CostumeDescriptions@@AAEXHHPAVDetailSwitchTable@@PAVINI_Reader@@@Z`
// (0x062ffc50) and `?init@bodypart@@QAEXPBD0HPAV?$list@...@std@@PAVDetailSwitchTable@@@Z`
// (0x062ff060, tail module storage-062ff136 stores it at bodypart+0x18). Both are modelled now.
// baseline's default field values (0x41a00000 == 20.0f, 0x44a00000 == 1280.0f) are written at
// /063009c2, matching this constructor.
class COMMON_API DetailSwitchTable
{
public:
    // zeroed vector, operator new(0x80) = room for 16 pairs, then 20.0f / 1280.0f.
    DetailSwitchTable() : FovXHalf(20.0f), SizeX(1280.0f) { Table.reserve(0x10); }

    // Common:0x063011b0, called from load_bodyparts at module storage-063009b5 with
    // Count == 0x10 right after the vector is zero-initialised. the routine is a plain
    // std::vector<std::pair<float,float>>::reserve: capacity is ([+0xc]-[+4])/8, it returns
    // early when that already covers Count (module storage), otherwise it allocates Count*8
    // bytes, copies the live range, frees the old block and sets _End = _First + Count.
    // baseline exports no such member (it is a std::vector instantiation, not a
    // DetailSwitchTable method), so the port's helper is a friend free function defined in
    // common_costume.cpp rather than a member of this exported class.
    friend void DetailSwitchTableReserve(DetailSwitchTable& Target, std::size_t Count);
    // baseline hand-inlines a vector-of-pairs copy/assign; std::vector<std::pair<float,float>>
    // already reproduces that value-for-value, so the special members can be defaulted.
    DetailSwitchTable(DetailSwitchTable const&) = default;
    DetailSwitchTable& operator=(DetailSwitchTable const&) = default;
    ~DetailSwitchTable() = default;

    // Common:0x062fecb0. Interpolates between the entries bracketing X (keys descending);
    // 1.0 when X falls between the end keys without a bracket. baseline reads Table[0] without an
    // empty test. Returns double: baseline leaves the value unrounded in ST0 and OutpostFrontier.exe
    // compares that ST0.
    double get_switch_distance(float X) const;
    // Common:0x062fed60. tan(AngleDeg in degrees)/tan(FovXHalf) * Distance, computed in
    // double (baseline x87) and rounded once on return. The third
    // parameter is genuinely unused in baseline (confirmed against the raw analysis: `RET 0xc`
    // pops three stack floats but only the first two, plus the this-relative FovXHalf, are ever
    // read) -- kept only for call-signature parity.
    float adjust_distance(float Distance, float AngleDeg, float Unused) const;
    // Common:0x062fef70. Parses "switch"/"fovx"/"sizex" values until the next header.
    // baseline takes the reader by pointer: ?read_from_ini@DetailSwitchTable@@QAEXPAVINI_Reader@@@Z.
    void read_from_ini(INI_Reader* Reader);

private:
    // Common:0x062fed90. Always appends -- baseline relies on the ini's "switch=" lines
    // already being written in the order get_switch_distance needs.
    void add_table_entry(float Key, float Value);

    std::vector<std::pair<float, float>> Table;
    float FovXHalf; // baseline +0x10, half of "fovx" in degrees
    float SizeX;    // baseline +0x14, "sizex" -- stored but never read by any traced method
};

// TString<N> (baseline length-prefixed fixed string) lives in common_tstring.h.
#include "common_tstring.h"

// bodypart -- one [Head]/[Body]/[LeftHand]/[RightHand] entry of bodyparts.ini. baseline layout,
// read off ??0bodypart@@QAE@XZ (0x062ff030) and ?init@bodypart@@... (0x062ff060):
//   +0x00 unsigned long Id     (atol(nickname), else CreateID(nickname) -- module storage-062ff08d)
//   +0x04 std::string Mesh     (init's param_2 -- module storage/062ff0fe-062ff11f)
//   +0x14 int Sex              (module storage)
//   +0x18 DetailSwitchTable*   (module storage)
//   +0x1c std::list<std::string>* PetalAnimations (module storage)
// baseline never stores the nickname text: get_name (0x062ff140) _ultoa's the Id into a static
// buffer. Copy ctor 0x062b6640, operator= 0x062b6790 and the non-virtual dtor 0x06315710 are
// memberwise; the port lets the compiler define them.
struct COMMON_API bodypart
{
    bodypart();
    bodypart(bodypart const& Other) = default;
    ~bodypart() = default;
    bodypart& operator=(bodypart const& Other) = default;


    void init(char const* Nickname, char const* InMesh, int InSex,
              std::list<std::string>* InPetalAnimations, DetailSwitchTable* InSwitch);

    unsigned long get_name_crc() const;             // [+0x00]
    char const* get_name() const;                   // (shared with accessory)
    char const* get_mesh() const;                   // [+0x04].c_str()
    int get_gender() const;                         // [+0x14]
    DetailSwitchTable* get_switch_table() const;    // [+0x18]
    std::list<std::string>* get_petal_anims() const; // [+0x1c]

    unsigned long Id;                               // +0x00
    std::string Mesh;                               // +0x04
    int Sex;                                        // +0x14
    // Borrowed, non-owning pointer into CostumeDescriptions::DetailSwitchTables (node-stable
    // std::list, as in baseline). Null when no "[DetailSwitchTable]" preceded this bodypart.
    DetailSwitchTable* Switch;                      // +0x18
    // Borrowed, non-owning pointer into CostumeDescriptions::PetalAnimations (the most
    // recently parsed "[PetalAnimations]" block, module storage-062ffd8c).
    std::list<std::string>* PetalAnimations;        // +0x1c
};

// accessory -- one [Accessory] entry of bodyparts.ini. baseline layout from ??0accessory@@QAE@XZ
// (0x062ff160), init (0x062ff2f0) and the local one load_accessory builds (module storage-
// ):
//   +0x00 unsigned long Id
//   +0x04 std::string Mesh                 ("mesh")
//   +0x14 std::string AccessoryHardpoint   ("hardpoint")      get_accessory_hardpoint
//   +0x24 std::string CharacterHardpoint   ("body_hardpoint") get_character_hardpoint
//   +0x34 std::list<std::string> Materials ("material_library", one entry per line)
struct COMMON_API accessory
{
    accessory();
    accessory(accessory const& Other) = default;
    ~accessory() = default;
    accessory& operator=(accessory const& Other) = default;


    void init(char const* Nickname, char const* InMesh, char const* InAccessoryHardpoint,
              char const* InCharacterHardpoint, std::list<std::string> const& InMaterials);

    unsigned long get_name_crc() const;             // [+0x00]
    char const* get_name() const;                   // (shared with bodypart)
    char const* get_mesh() const;                   // [+0x04].c_str()
    char const* get_accessory_hardpoint() const;    // [+0x14].c_str()
    char const* get_character_hardpoint() const;    // [+0x24].c_str()
    std::list<std::string> const& get_materials() const; // [+0x34]

    unsigned long Id;                               // +0x00
    std::string Mesh;                               // +0x04
    std::string AccessoryHardpoint;                 // +0x14
    std::string CharacterHardpoint;                 // +0x24
    std::list<std::string> Materials;               // +0x34
};

// costume -- the 0x3c-byte value of the costume map, as CostumeDescriptions::load_costumes
// builds it on the stack (06300c80-06300c9a, pair copy 06300e27-06300e39) and get_costume /
// get_costume_gender(id) read it (062ffaea-062ffb2c, 062b6cb1). baseline stores no nickname.
struct costume
{
    unsigned Id;                        // +0x00
    int Gender;                         // +0x04, the first non-zero part sex
    bodypart const* Parts[4];           // +0x08, indexed by body-part type (body/head/lhand/rhand)
    accessory const* Accessories[8];    // +0x18
    unsigned AccessoryCount;            // +0x38
};

// Shared runtime costume: head/body/left hand/right hand IDs. Bodypart TypeIds are
// body/head/left hand/right hand, so TypeIds 0/1 must be swapped when converting.
struct Costume
{
    unsigned PartIds[4];
    unsigned AccessoryIds[8];
    unsigned AccessoryCount;
};

class COMMON_API CostumeDescriptions
{
public:
    CostumeDescriptions();
    ~CostumeDescriptions();

    // baseline ??0CostumeDescriptions@@QAE@ABV0@@Z (ord 209, 0x062b6d70) and
    // ??4CostumeDescriptions@@QAEAAV0@ABV0@@Z (ord 755, 0x062b6f60) are the compiler's
    // memberwise copies: the four lists, then the four bodypart maps, then the accessory and
    // costume maps, each copied by value. Borrowed pointers inside the copied records
    // (bodypart::Switch/PetalAnimations, costume::Parts/Accessories) keep pointing into the
    // source object, as in baseline.
    CostumeDescriptions(CostumeDescriptions const&) = default;
    CostumeDescriptions& operator=(CostumeDescriptions const&) = default;

    void load_bodyparts(char const* Path);
    void load_costumes(char const* Path);

    bodypart const* find_bodypart(int TypeId, unsigned long NicknameId) const;
    bodypart const* find_bodypart(int TypeId, char const* Name) const;
    accessory const* find_accessory(unsigned long NicknameId) const;
    accessory const* find_accessory(char const* Name) const;
    costume const* find_costume(unsigned long NicknameId) const;
    costume const* find_costume(char const* Name) const;
    unsigned long find_costume_id(char const* Name) const;
    unsigned long find_accessory_id(char const* Name) const;
    unsigned long find_part_id(int TypeId, char const* Name) const;
    char const* get_part_name(int TypeId, unsigned long NicknameId) const;
    char const* get_part_mesh(int TypeId, unsigned long NicknameId) const;

    void get_costume(char const* Name, Costume& Out) const;
    void get_costume(int NicknameId, Costume& Out) const;
    int get_costume_gender(Costume const& Cost) const;
    // Commonb6ca0. Distinct baseline overload: reads the stored Gender straight off
    // the costume record by id, instead of re-deriving it from the four bodypart maps.
    int get_costume_gender(unsigned long NicknameId) const;

private:
    // ?load_bodypart@CostumeDescriptions@@AAEXHHPAVDetailSwitchTable@@PAVINI_Reader@@@Z
    // (0x062ffc50): baseline takes FOUR arguments, not three. The 3rd is the sticky
    // "[DetailSwitchTable]" pointer threaded in by load_bodyparts (module storage).
    // Both take the reader by pointer, and the table pointer is non-const (PAV...).
    void load_bodypart(int TypeId, int Sex, DetailSwitchTable* Switch, INI_Reader* Reader);
    // ?load_accessory@CostumeDescriptions@@AAEXPAVINI_Reader@@@Z
    void load_accessory(INI_Reader* Reader);
    // ?warn@CostumeDescriptions@@AAEXPAVINI_Reader@@@Z: an empty body (RET 4).
    void warn(INI_Reader* Reader);

public:
    // The two name lists are read in place by OutpostFrontier.exe's gf\CharacterInfo.cpp: +0x00 by
    // (0x00435133, [descs+4] = the sentinel) and +0x0c by the routine
    // (0x004355ea, [descs+0x10]). There is no accessor export, so the members are public; the
    // declaration order (and with it the layout) is unchanged.
    // baseline member order, from the base-relative accesses in load_bodyparts (0x06300570) and
    // load_bodypart (0x062ffc50). Each of the four is an MSVCP60 std::list (0x0c bytes:
    // allocator byte, sentinel node pointer, count), which is why the port keeps std::list here
    // too -- both DetailSwitchTables and PetalAnimations hand out interior pointers that must
    // survive later insertions.
    //   +0x00 "[Animations]" / "anim="   -- scanned at module storage, pushed at module storage
    std::list<std::string> AnimationNames;
    //   +0x0c "[Animations]" / "preload=" -- scanned at module storage, pushed at module storage
    std::list<std::string> PreloadNames;

private:
    //   +0x18 one entry per "[PetalAnimations]" section -- pushed at module storage
    //         (the routine); the local per-section list is built on the stack at
    // and copied in.
    std::list<std::list<std::string>> PetalAnimations;
    //   +0x24 one entry per "[DetailSwitchTable]" section -- pushed at module storage
    //         (the routine)
    std::list<DetailSwitchTable> DetailSwitchTables;
    //   +0x30 the four bodypart maps (load_bodypart indexes them with
    //         `EBP + TypeId*0x14 + 0x30` at module storage-062ffe6b; 0x14 == sizeof(map)).
    //   +0x80 accessories, +0x94 costumes (find_accessory / find_costume, and the ctor
    // / 062ff5ac). Every map holds its record BY VALUE: the finds return
    //         node+0x10, the address of the mapped value (module storage, 062ff9d8), and the
    //         dtor (0x062ff600) only runs the member destructors -- there is no owning side list.
    std::map<unsigned, bodypart> Bodyparts[4];
    std::map<unsigned, accessory> Accessories;
    std::map<unsigned, costume> Costumes;
};

COMMON_API void __cdecl LoadBodypartDescriptions(char const* Path);
// Common, ?Costume_is_equal@@YA_NABUCostume@@0@Z. Compares the four body
// part ids, the accessory count, and only the accessories the count claims are in use.
COMMON_API bool __cdecl Costume_is_equal(Costume const& A, Costume const& B);

COMMON_API void __cdecl LoadCostumeDescriptions(char const* Path);
COMMON_API void __cdecl FreeCostumeDescriptions();
COMMON_API CostumeDescriptions const* __cdecl GetCostumeDescriptions();

COMMON_API bool __cdecl CostumeGenderAdjust(int* gender, int adjustment);

namespace Reputation
{
    struct Relation
    {
        unsigned GroupId;
        float Feeling;
    };

    struct RepGroupCB
    {
        virtual char Execute(unsigned const* GroupId) = 0;
    };

    COMMON_API bool __cdecl Startup(char const* Path);
    COMMON_API void __cdecl Shutdown();
    COMMON_API void __cdecl Save(ISave* Writer);
    COMMON_API void __cdecl LoadFeelings(INI_Reader& Reader);
    COMMON_API void __cdecl FreeFeelings();

    COMMON_API unsigned __cdecl get_id(TString<16> const& Name);
    COMMON_API int __cdecl get_nickname(TString<16>& Out, unsigned const& GroupId);
    COMMON_API unsigned __cdecl get_name(unsigned GroupId);
    COMMON_API unsigned __cdecl get_short_name(unsigned GroupId);
    COMMON_API unsigned __cdecl get_info_card(unsigned GroupId);
    COMMON_API unsigned __cdecl group_count();
    COMMON_API void __cdecl enumerate(RepGroupCB* Callback);
    COMMON_API int __cdecl get_feelings_towards(unsigned FromId, unsigned ToId, float& Out);
    COMMON_API int __cdecl affect_relation(unsigned const& FromId, unsigned const& ToId, float Delta);
    COMMON_API bool __cdecl IsStoryFaction(unsigned GroupId);
    // ?set@Reputation@@YAXABU?$TString@$0BA@@@QAURelation@1@I@Z -- Relation* const, i.e. the
    // pointer is const and the relations are NOT. Top-level const on a parameter changes
    // nothing for callers but MSVC still encodes it (Q instead of P).
    COMMON_API void __cdecl set(TString<16> const& Name, Relation* const Rels, unsigned Count);

    // . The changed-affiliation map (baseline global the field): client id ->
    // that client's previous affiliation, filled by Vibe::SetAffiliation (MP, Notify) and by
    // the auto-affiliation pass after Vibe::Set / Vibe::SetGroupFeelingsTowards (MP). Returns
    // null while the map is empty. Common never clears it: the consumers do (Server's
    // per-system update module storage, the client's reputation notice module storage).
    COMMON_API std::map<unsigned int, unsigned int>* __cdecl GetChangedAffiliationClientMap();

    // ============================================================================
    // Reputation::Vibe -- per-GroupId *dynamic/runtime* reputation record: a name override,
    // custom infocard, rank, per-player attitude, and per-other-group feeling overrides, layered
    // on top of the static Relation matrix above (GetGroupFeelingsTowards/AttitudeTowards fall
    // back to Reputation::get_feelings_towards when a group has no override of its own).

    // baseline keeps these records in TWO stores, and so does the port (common_vibe.cpp):
    //  - the static store, a std::vector of {int key; VibeValue} slots (the field), filled
    //    while the universe loads (BeginStaticLoad .. EndStaticLoad: Alloc appends, lookups
    //    scan linearly), then sorted by key and binary-searched, with a one-slot cache
    //    (the field); FreeDynamic keeps it, FreeStatic empties it;
    //  - the dynamic store, a std::map<int, VibeValue> (the field) with a last-node cache
    //    (the field); Alloc inserts here outside the universe load, FreeDynamic empties it.
    // Every accessor looks in the dynamic store first and then in the static one, except
    // SetClientID / GetClientID, which only see the dynamic store; Free only frees from the
    // dynamic store.
    namespace Vibe
    {
        // Per-other-group feeling override. Common confirms this is a real
        // std::vector<GroupFeeling> (begin/end/cap at VibeValue+0x7c/+0x80/+0x84), read by
        // GetGroupFeelingsTowards and grown by SetGroupFeelingsTowards (the routine).
        struct GroupFeeling
        {
            unsigned GroupId;
            float Feeling;
        };

        // Node of the VibeValue+0x88 singly-linked attitude-delta list. AdjustAttitude
        // (0x06321af0) op-news one of these (0xc bytes) per distinct target GroupId and prepends
        // it; VibeValue's own copy-ctor (the routine, 0x0631fad0) deep-copies the whole chain.
        struct AttitudeDelta
        {
            AttitudeDelta* Next;
            unsigned GroupId;
            float Delta;
        };

        // Real per-GroupId dynamic reputation record. Field offsets below are cross-checked
        // against baseline across three independent sources: the destructor (the routine,
        // Common), the copy-ctor tail shared by 17 call sites (the routine,
        // Common), and every Get/Set accessor's own direct offset access.
        // Byte-for-byte layout preserved (static_asserts below) even though this port's storage
        // engine no longer needs it, both as a documented cross-check of that analysis and
        // because +0x00.. +0x88 is what every accessor below actually reads/writes.
        // Internal to Common.dll: baseline exports nothing of VibeValue (no ctor, dtor,
        // operator= or CopyFieldsFrom), so the struct is not COMMON_API.
        struct VibeValue
        {
            unsigned Affiliation;              // +0x00, default 0xffffffff ("no affiliation")
            float AttitudeTowardsPlayer;        // +0x04
            unsigned ClientID;                   // +0x08, doubles as "is player-owned" (!=0)
            // +0x0c. Earlier passes called this "QueueLink"; it is not a queue link. baseline
            // stores here the address of the Affiliation group's record inside the group
            // registry the field (node+0x10 of that tree, the port's GGroups value):
            // SetAffiliation (module storage/06321498) and Set (module storage) write it, the
            // auto-affiliation pass the routine and SetAffiliation's MP-notify path clear it
            // (module storage/0631ff24, module storage), and the attitude blend the routine
            // reads its +0x20 (the group's feelings map) and treats null as "no base relation"
            // (module storage-0631ffaa). Opaque here because the group record type is private
            // to Common (CmnReputation::FGroup, src/Common/common_reputation_internal.h).
            void const* AffiliationGroup;          // +0x0c
            FmtStr Name1;                          // +0x10
            FmtStr Name2;                           // +0x28
            unsigned Infocard;                       // +0x40
            wchar_t DisplayName[0x18];                // +0x44, wcsncpy-filled, NUL-terminated
            unsigned Rank;                              // +0x74
            unsigned Reserved78;                         // +0x78: in baseline the allocator byte of
                                                           // the VC6 std::vector that follows
                                                           // (the routine 0631fa0c); no data.
            AttitudeDelta* Deltas;                          // baseline +0x88, head of the delta list
            std::vector<GroupFeeling> GroupFeelings;       // baseline +0x7c

            VibeValue();
            VibeValue(VibeValue const& Other);
            VibeValue& operator=(VibeValue const& Other);
            ~VibeValue();

            // baseline's shared copy/assign tail (this = destination). Public
            // because Reputation::Vibe::Alloc applies it to an existing entry directly
            // (module storage / module storage), not through a constructor or operator=.
            void CopyFieldsFrom(VibeValue const& Other);
        };
        // NOTE: GroupFeelings/Deltas are declared in the REVERSE of their baseline +0x7c/+0x88
        // order (see comments above) and no static_assert pins either's offset. This build's
        // std::vector is 16 bytes (MSVC Debug adds a debug-iterator proxy pointer on top of
        // baseline's raw 12-byte begin/end/cap), so embedding GroupFeelings before a fixed-offset
        // Deltas can't reproduce baseline's byte layout here regardless of field order -- same
        // constraint already hit by EqObj::DockingSpheres (common_archetype.h), which resolves
        // it the same way: put the vector last, assert nothing after it. Harmless: every access
        // to both fields in common_vibe.cpp goes through the named C++ members, never raw
        // offset arithmetic, so the in-memory order carries no behavioral meaning.
        static_assert(offsetof(VibeValue, Affiliation) == 0x00, "VibeValue::Affiliation at +0x00");
        static_assert(offsetof(VibeValue, AttitudeTowardsPlayer) == 0x04, "VibeValue::AttitudeTowardsPlayer at +0x04");
        static_assert(offsetof(VibeValue, ClientID) == 0x08, "VibeValue::ClientID at +0x08");
        static_assert(offsetof(VibeValue, AffiliationGroup) == 0x0c, "VibeValue::AffiliationGroup at +0x0c");
        static_assert(offsetof(VibeValue, Name1) == 0x10, "VibeValue::Name1 at +0x10");
        static_assert(offsetof(VibeValue, Name2) == 0x28, "VibeValue::Name2 at +0x28");
        static_assert(offsetof(VibeValue, Infocard) == 0x40, "VibeValue::Infocard at +0x40");
        static_assert(offsetof(VibeValue, DisplayName) == 0x44, "VibeValue::DisplayName at +0x44");
        static_assert(offsetof(VibeValue, Rank) == 0x74, "VibeValue::Rank at +0x74");
        static_assert(offsetof(VibeValue, Deltas) == 0x7c, "VibeValue::Deltas");

        COMMON_API unsigned __cdecl Count();
        COMMON_API bool __cdecl Verify(int GroupId);
        // . Returns -1 for GroupId -1; otherwise makes sure GroupId has an entry
        // and RESETS it to the default record through the copy tail (so an existing entry
        // loses its affiliation, names, rank, feelings and client id -- but keeps its
        // attitude-delta list, which the copy tail only prepends to), and returns 0. During
        // the universe load the entry lives in the static store, otherwise in the dynamic one.
        COMMON_API int __cdecl Alloc(int const& GroupId);
        // Calls Alloc only when GroupId has no entry yet. Always returns 0; matches baseline
        // (Common).
        COMMON_API int __cdecl EnsureExists(int const& GroupId);
        // . Frees GroupId's DYNAMIC entry (-2 when it has none) and drops every
        // other entry's attitude deltas towards it.
        COMMON_API int __cdecl Free(int const& GroupId);
        COMMON_API void __cdecl FreeDynamic();

        // Common.dll-internal (not exported by baseline), called by the universe load:
        // static-store loading mode on, capacity at least 0x1000 slots;
        void __cdecl BeginStaticLoad();
        // loading mode off, the static store sorted by key;
        void __cdecl EndStaticLoad();
        // (via the thunk 062b8e20) -- the static store emptied.
        void __cdecl FreeStatic();

        // bool ExcludeIfPlayerOwned: when true and the group's Vibe is player-owned (ClientID
        // != 0), Out is forced to 0xffffffff instead of the real Affiliation (Common
        // ). Returns -2 if GroupId has no Vibe entry (Out left at its
        // pre-set default of 0xffffffff either way).
        COMMON_API int __cdecl GetAffiliation(int const& GroupId, unsigned& Out, bool ExcludeIfPlayerOwned);
        // . -2 if GroupId has no entry, -3 if Affiliation is not a registered
        // group, 0 otherwise. On a real change outside SinglePlayer() with Notify set, the
        // client's previous affiliation is recorded in the changed-affiliation map (see
        // GetChangedAffiliationClientMap) keyed by the entry's ClientID.
        COMMON_API int __cdecl SetAffiliation(int const& GroupId, unsigned Affiliation, bool Notify);
        // / 06320f10: the DYNAMIC store only -- an entry that lives in the static
        // store is not found (-2 / 0). Returns 0/current ClientID, or -2/0.
        COMMON_API int __cdecl SetClientID(int GroupId, unsigned ClientID);
        COMMON_API unsigned __cdecl GetClientID(int GroupId);
        COMMON_API int __cdecl GetInfocard(int const& GroupId, unsigned& Out);
        COMMON_API int __cdecl SetInfocard(int const& GroupId, unsigned Infocard);
        COMMON_API int __cdecl GetRank(int const& GroupId, unsigned& Out);
        COMMON_API int __cdecl SetRank(int const& GroupId, unsigned Rank);
        // *DisplayName receives a pointer straight into the found entry's internal buffer
        // (Commone6), matching baseline's own aliasing.
        COMMON_API int __cdecl GetName(int const& GroupId, FmtStr& Name1Out, FmtStr& Name2Out, wchar_t const*& DisplayNameOut);
        // DisplayName may be null (clears the stored display name); otherwise copied via
        // wcsncpy(..., 0x17) with a forced NUL at DisplayName[0x17], matching baseline exactly.
        COMMON_API int __cdecl SetName(int const& GroupId, FmtStr const& Name1, FmtStr const& Name2, wchar_t const* DisplayName);
        // -2 if GroupId has no entry, -3 if get_feelings_towards(Other,
        // Affiliation) fails, else 0 with Out = clamp(override + base).
        COMMON_API int __cdecl GetGroupFeelingsTowards(int const& GroupId, unsigned const& OtherGroupId, float& Out);
        COMMON_API int __cdecl SetGroupFeelingsTowards(int const& GroupId, unsigned const& OtherGroupId, float Feeling);
        // Direct field write, no blending -- fully confirmed (Common).
        COMMON_API int __cdecl SetAttitudeTowardsPlayer(int GroupId, float Attitude);

        // Common:0x06320bc0 (?Get@Vibe@Reputation@@YAHABHAAI1AAEQAURelation@2@AAUFmtStr@@4AAPBG@Z).
        // Bulk getter combining Affiliation/Rank/GroupFeelings(as a Relation[] snapshot)/Name1/
        // Name2/DisplayName in one call -- everything it returns is a plain field read against
        // the VibeValue layout (the lookup is the usual dynamic-then-static one). Returns -2
        // if GroupId has no Vibe entry. CountOut is a single byte counter exactly like baseline
        // (Commonc46-06320c74) -- callers must supply a Relations buffer large
        // enough for every GroupFeelings entry (baseline's own call sites always pass a fixed
        // 0xff-capacity buffer); more than 255 entries wraps the byte counter and overwrites from
        // the start, matching baseline exactly, not fixed up here. *DisplayNameOut aliases straight
        // into the found entry's internal buffer, same aliasing as GetName.
        COMMON_API int __cdecl Get(int const& GroupId, unsigned& AffiliationOut, unsigned& RankOut, unsigned char& CountOut, Relation* const Relations, FmtStr& Name1Out, FmtStr& Name2Out, wchar_t const*& DisplayNameOut);
        // Common:0x06320cb0 (?Set@Vibe@Reputation@@YAHABHIIEQBURelation@2@ABUFmtStr@@2PBG@Z).
        // Bulk setter: replaces Affiliation/Rank/GroupFeelings(from Relations[0..Count))/Name1/
        // Name2/DisplayName in one call. Returns -1 immediately if GroupId==-1, -2 if no Vibe
        // entry exists yet (does not create one -- caller must EnsureExists first, same contract
        // as every other Set*). Traced against Commoncb0-06320e99:
        //  - an NPC entry (ClientID == 0) caches its new affiliation's group record in
        //    AffiliationGroup (null when the group is not registered);
        //  - only Relations entries whose GroupId is a registered group are kept
        //    (module storage-06320e57);
        //  - unlike SetName, DisplayName is copied with NO null check (module storage) --
        //    a null DisplayName is not a safe argument, exactly as in baseline;
        //  - the call ends with the auto-affiliation pass the routine (module storage).
        // , the list that pass reads, is the story-faction id list shared with
        //    IsStoryFaction (built from the fc_*_grp names); its elements are MakeId values
        //    truncated to 16 bits, not IObjInspect* as analysis's ICF-folded name suggests.
        COMMON_API int __cdecl Set(int const& GroupId, unsigned Affiliation, unsigned Rank, unsigned char Count, Relation const* const Relations, FmtStr const& Name1, FmtStr const& Name2, wchar_t const* DisplayName);

        // AttitudeTowards (module storage) and AttitudeTowardsSymmetrical (module storage) are
        // both the routine (AttitudeTowardsCore in common_vibe.cpp): -2 if From has no
        // entry, -3 if To has none (Out1 = 0 first; Out2 untouched on failure), otherwise the
        // blend the routine per direction, clamped to [-1, 1] and rounded to hundredths
        // (the field / the field, static initialisers the branch / the branch).
        // AdjustAttitude (module storage): -2 / -3 the same way; stores in From's delta list
        // for To the value `Delta - clamp(override + base)` (or `Delta - base` when the base
        // relation cannot be computed), so Delta acts as a target attitude, not an increment.
        COMMON_API int __cdecl AttitudeTowards(int const& FromGroupId, int const& ToGroupId, float& Out);
        COMMON_API int __cdecl AttitudeTowardsSymmetrical(int const& FromGroupId, int const& ToGroupId, float& Out1, float& Out2);
        COMMON_API int __cdecl AdjustAttitude(int const& FromGroupId, int const& ToGroupId, float Delta);
    }
}
