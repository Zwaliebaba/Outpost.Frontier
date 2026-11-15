// Author: Giperion.

#include "common_reputation.h"
#include "common_reputation_internal.h"
#include "msvcrt_compat.h" // MsvcrtAtoi: baseline MSVCRT.DLL wraps mod 2^32, the UCRT saturates

#include "common_ini.h"
#include "common_misc.h"
#include "neuroncomponents.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "x87_trig.h" // X87Tan (DetailSwitchTable::adjust_distance, 062fed6a / 062fed77 FPTAN)

namespace
{
    const char kCharDescripSource[] = "Common\\CharDescrip.cpp";
    const unsigned kBodypartTypeCount = 4;
    const unsigned kMaxAccessories = 8;

    char const* const kBodypartTypeNames[kBodypartTypeCount] = {
        "Body",
        "Head",
        "LeftHand",
        "RightHand",
    };

    CostumeDescriptions* GCostumeDescriptions = nullptr;

    // and its inlined copies (062ff83a, 062ff91b, 062ff9fb, 062ffa79, 06300d01,
    // , 06300e8c): atol(Name), CreateID(Name) when that is 0. No null test.
    unsigned NicknameToCreateId(char const* Name)
    {
        long Parsed = MsvcrtAtoi(Name);
        if (Parsed == 0)
            return CreateID(Name);
        return (unsigned)Parsed;
    }

    // Common: the identical case-insensitive linear scan baseline hand-inlines three times
    // inside load_bodyparts -- module storage-06300754 ("[Animations]"/"anim"),
    // ("[Animations]"/"preload") and
    // ("[PetalAnimations]"/"anim"). Each walks the std::list nodes
    // (node+0x00 = next, node+0x0c = the string's character pointer) and substitutes MSVCP60's
    // shared empty string when that pointer is null (module storage / 063007bc / 063008ae),
    // which std::string::c_str() reproduces for free, then calls the NEURONCOMPONENTS.DLL stricmp import
    // as stricmp(value, entry). A hit means "already present, skip".
    bool ListContainsI(std::list<std::string> const& List, char const* Name)
    {
        std::list<std::string>::const_iterator It = List.begin();
        for (; It != List.end(); ++It)
        {
            if (CallNeuronComponentsStricmp(Name, It->c_str()) == 0)
                return true;
        }
        return false;
    }

    // The 0x104-byte {int Id; char Name[0x100]} table at module storage ({0,"Body"},
    // {1,"Head"}, {2,"LeftHand"}, {3,"RightHand"}), scanned with NEURONCOMPONENTS stricmp(entry, Name)
    // (06300a7a / 06300da9). Id == index, so the baseline `Id == -1` tests (06300af4, 06300e74)
    // can never fire; -1 here means "no entry matched".
    int BodypartTypeFromName(char const* Name)
    {
        unsigned Index = 0;
        for (; Index < kBodypartTypeCount; ++Index)
        {
            if (CallNeuronComponentsStricmp(kBodypartTypeNames[Index], Name) == 0)
                return (int)Index;
        }
        return -1;
    }


    CostumeDescriptions* EnsureCostumeDescriptions()
    {
        if (GCostumeDescriptions == nullptr)
            GCostumeDescriptions = new (std::nothrow) CostumeDescriptions();
        return GCostumeDescriptions;
    }

    void FillCostume(costume const* Desc, Costume& Out)
    {
        // head/body/left/right IDs from the body/head/left/right record.
        const unsigned TypeIds[4] = {1, 0, 2, 3};
        unsigned Index = 0;
        for (; Index < 4; ++Index)
        {
            bodypart const* const Part = Desc != nullptr ? Desc->Parts[TypeIds[Index]] : nullptr;
            Out.PartIds[Index] = Part != nullptr ? Part->Id : 0;
        }

        Out.AccessoryCount = Desc != nullptr ? Desc->AccessoryCount : 0;
        // baseline leaves the unused accessory slots untouched, including on a missing costume.
        for (Index = 0; Index < Out.AccessoryCount; ++Index)
            Out.AccessoryIds[Index] = Desc->Accessories[Index]->Id;
    }
}

// , 0x100 bytes: the one scratch buffer bodypart::get_name and
// accessory::get_name (a single ICF-folded body, 0x062ff140) format the id into.
static char GCostumeNameBuffer[0x100];

bodypart::bodypart()
    : Id(0)
    , Sex(0)
    , Switch(nullptr)
    , PetalAnimations(nullptr)
{
    // Id, the string, +0x14, +0x18, +0x1c all zeroed.
}

void bodypart::init(char const* Nickname, char const* InMesh, int InSex,
                    std::list<std::string>* InPetalAnimations, DetailSwitchTable* InSwitch)
{
    // Id = atol(nickname), CreateID(nickname) when that is 0; Mesh = InMesh;
    // [+0x14] = InSex, [+0x1c] = InPetalAnimations, [+0x18] = InSwitch. No null checks.
    Id = (unsigned long)MsvcrtAtoi(Nickname);
    if (Id == 0)
        Id = CreateID(Nickname);
    Mesh = InMesh;
    Sex = InSex;
    PetalAnimations = InPetalAnimations;
    Switch = InSwitch;
}

unsigned long bodypart::get_name_crc() const
{
    // MOV EAX,[ECX]
    return Id;
}

char const* bodypart::get_name() const
{
    // _ultoa([this], buffer, 10); return buffer.
    _ultoa(Id, GCostumeNameBuffer, 10);
    return GCostumeNameBuffer;
}

char const* bodypart::get_mesh() const
{
    // the string's buffer, or MSVCP60's empty string when it has none.
    return Mesh.c_str();
}

int bodypart::get_gender() const
{
    // MOV EAX,[ECX+0x14]
    return Sex;
}

DetailSwitchTable* bodypart::get_switch_table() const
{
    // MOV EAX,[ECX+0x18]
    return Switch;
}

std::list<std::string>* bodypart::get_petal_anims() const
{
    // MOV EAX,[ECX+0x1c]
    return PetalAnimations;
}

accessory::accessory()
    : Id(0)
{
    // three empty strings, an empty list, Id = 0.
}

void accessory::init(char const* Nickname, char const* InMesh, char const* InAccessoryHardpoint,
                     char const* InCharacterHardpoint, std::list<std::string> const& InMaterials)
{
    // Id as bodypart::init; the three strings assigned in order; the list
    // assigned only when it is not this accessory's own (062ff3f4 compares the addresses).
    Id = (unsigned long)MsvcrtAtoi(Nickname);
    if (Id == 0)
        Id = CreateID(Nickname);
    Mesh = InMesh;
    AccessoryHardpoint = InAccessoryHardpoint;
    CharacterHardpoint = InCharacterHardpoint;
    if (&Materials != &InMaterials)
        Materials = InMaterials;
}

unsigned long accessory::get_name_crc() const
{
    // MOV EAX,[ECX]
    return Id;
}

char const* accessory::get_name() const
{
    // (the same body as bodypart::get_name).
    _ultoa(Id, GCostumeNameBuffer, 10);
    return GCostumeNameBuffer;
}

char const* accessory::get_mesh() const
{
    // [+0x08], or the empty string.
    return Mesh.c_str();
}

char const* accessory::get_accessory_hardpoint() const
{
    // [+0x18], or the empty string.
    return AccessoryHardpoint.c_str();
}

char const* accessory::get_character_hardpoint() const
{
    // [+0x28], or the empty string.
    return CharacterHardpoint.c_str();
}

std::list<std::string> const& accessory::get_materials() const
{
    // LEA EAX,[ECX+0x34]
    return Materials;
}

CostumeDescriptions::CostumeDescriptions()
{
}

void CostumeDescriptions::warn(INI_Reader* Reader)
{
    // RET 4 -- an empty body (ICF-shared with a dozen other empty methods).
    (void)Reader;
}

CostumeDescriptions::~CostumeDescriptions()
{
    // nothing but the member destructors, in reverse declaration order -- costume
    // map (+0x94), accessory map (+0x80), the four bodypart maps (+0x30, `eh vector destructor
    // iterator`), then the lists at +0x24, +0x18, +0x0c and +0x00. The maps own their records
    // by value, so there is nothing else to free.
}

void DetailSwitchTable::add_table_entry(float Key, float Value)
{
    Table.emplace_back(Key, Value);
}

// Common:0x063011b0 -- std::vector<std::pair<float,float>>::reserve on the table's
// vector (unexported). Friend of DetailSwitchTable, not a member, so it is not exported.
void DetailSwitchTableReserve(DetailSwitchTable& Target, std::size_t Count)
{
    Target.Table.reserve(Count);
}

// , RET 4 (CMN-S2 AIS fix 2026-10-14). The keys run in descending
// order. No empty-table test: baseline reads Table[0] unconditionally (062feccf), so an empty
// table faults on its null begin pointer -- the former `Count == 0 -> 1.0f` guard was invented.
// Very strange, but baseline; the raw data() pointer keeps the read free of debug-iterator checks.
// Returns double: baseline leaves the result unrounded in ST0, and OutpostFrontier.exe's caller at
// compares that ST0 before any store.
double DetailSwitchTable::get_switch_distance(float X) const
{
    std::pair<float, float> const* const Rows = Table.data();
    // count = 0 when begin is null, else (end - begin) / 8.
    int const Count = (int)Table.size();

    // FCOMP [begin]; TEST AH,1; JNZ -- X >= Table[0].first (ordered)
    // returns Table[0].second.
    if (X >= Rows[0].first)
        return Rows[0].second;

    // TEST AH,0x41; JP -- X <= the last key (ordered) returns its value.
    if (X <= Rows[Count - 1].first)
        return Rows[Count - 1].second;

    // the first i < Count - 1 with Table[i].first >= X (ordered) and
    // Table[i + 1].first <= X (ordered) brackets X; none -> the 1.0f still in ST0.
    for (int Index = 0; Index < Count - 1; ++Index)
    {
        if (!(Rows[Index].first >= X))
            continue;
        if (!(Rows[Index + 1].first <= X))
            continue;

        // b + ((X - a) * (c - b)) / (d - a) on the x87 stack, returned
        // unrounded.
        double const KeyA = Rows[Index].first;
        double const ValueB = Rows[Index].second;
        double const ValueC = Rows[Index + 1].second;
        double const KeyD = Rows[Index + 1].first;
        return (((double)X - KeyA) * (ValueC - ValueB)) / (KeyD - KeyA) + ValueB;
    }
    // the 1.0f (the field) loaded first is still in ST0.
    return 1.0;
}

float DetailSwitchTable::adjust_distance(float Distance, float AngleDeg, float /*Unused*/) const
{
    // FLD AngleDeg / FMUL 0.017453277f (the field) / FPTAN, the same for
    // FovXHalf (+0x10), FDIVP, FMUL Distance -- all on the x87 stack. The only caller
    // (OutpostFrontier.exe 00445078) stores the result as a float straight away.
    double const DegToRad = (double)0.017453277f;
    double const TanAngle = X87Tan((double)AngleDeg * DegToRad);
    double const TanFov = X87Tan((double)FovXHalf * DegToRad);
    return (float)((TanAngle / TanFov) * (double)Distance);
}

void DetailSwitchTable::read_from_ini(INI_Reader* Reader)
{
    while (Reader->read_value())
    {
        if (Reader->is_value("switch"))
        {
            float Key = Reader->get_value_float(0);
            float Value = Reader->get_value_float(1);
            add_table_entry(Key, Value);
        }
        else if (Reader->is_value("fovx"))
        {
            FovXHalf = Reader->get_value_float(0) * 0.5f;
        }
        else if (Reader->is_value("sizex"))
        {
            SizeX = Reader->get_value_float(0);
        }
    }
}

// Common:0x062ffc50-0x062fff40,
// ?load_bodypart@CostumeDescriptions@@AAEXHHPAVDetailSwitchTable@@PAVINI_Reader@@@Z.
// Signature verified from the mangled export, not guessed: (int, int, DetailSwitchTable*,
// INI_Reader*). The call site in load_bodyparts pushes them at module storage-06300b0b.
void CostumeDescriptions::load_bodypart(int TypeId, int Sex, DetailSwitchTable* Switch,
                                        INI_Reader* Reader)
{
    std::string Nickname;
    std::string Mesh;

    while (Reader->read_value())
    {
        if (Reader->is_value("nickname"))
        {
            Nickname = Reader->get_value_string();
        }
        else if (Reader->is_value("mesh"))
        {
            Mesh = Reader->get_value_string();
        }
        else if (Reader->is_value("multilevel"))
        {
            // the NEURONCOMPONENTS FDUMP pointer is called directly.
            FDUMP(0x100002, "%s(%d) : *** WARNING: %s", kCharDescripSource, 0x2c9,
                  "multilevel is obsolete");
        }
    }

    if (Nickname.empty() || Mesh.empty())
        return;

    unsigned Id = NicknameToCreateId(Nickname.c_str());

    // . baseline does NOT receive the petal-animation list as a parameter;
    // it re-derives it here from its own registry: when the count at this+0x20 is non-zero it
    // takes `*(this+0x1c) -> +0x4` (the sentinel node's prev == the last node) and adds 8 (the
    // node's payload offset), i.e. &PetalAnimations.back(). Otherwise the pointer stays null.
    std::list<std::string>* PetalAnims = nullptr;
    if (!PetalAnimations.empty())
        PetalAnims = &PetalAnimations.back();

    // the record is built on the stack and inserted by value into
    // Bodyparts[TypeId] (the routine builds the pair, the routine is map::insert, which
    // leaves an existing entry with the same id untouched).
    bodypart Part;
    Part.init(Nickname.c_str(), Mesh.c_str(), Sex, PetalAnims, Switch);
    Bodyparts[TypeId].insert(std::map<unsigned, bodypart>::value_type(Id, Part));
}

void CostumeDescriptions::load_accessory(INI_Reader* Reader)
{
    std::string Nickname;
    std::string Mesh;
    std::string Hardpoint;
    std::string BodyHardpoint;
    std::list<std::string> MaterialLibrary;

    while (Reader->read_value())
    {
        if (Reader->is_value("nickname"))
            Nickname = Reader->get_value_string();
        else if (Reader->is_value("mesh"))
            Mesh = Reader->get_value_string();
        else if (Reader->is_value("hardpoint"))
            Hardpoint = Reader->get_value_string();
        else if (Reader->is_value("body_hardpoint"))
            BodyHardpoint = Reader->get_value_string();
        else if (Reader->is_value("material_library"))
            // Common (0x062fff60): baseline appends one entry per
            // repeated "material_library=" line into a persistent std::list<> member of
            // accessory (the routine list ctor, the routine node insert) instead of
            // overwriting a single slot like the four fields above.
            MaterialLibrary.push_back(std::string(Reader->get_value_string()));
    }

    // no nickname test -- an accessory without "nickname" still goes in,
    // under the routine("") (atol 0 -> CreateID of the empty string).
    unsigned Id = NicknameToCreateId(Nickname.c_str());

    // baseline (module storage-0630028c) fills a local accessory field by field rather than
    // through init: Id, then "mesh" -> +0x04, "hardpoint" -> +0x14, "body_hardpoint" -> +0x24,
    // each missing value as an empty string, then the material list. The local is inserted by
    // value into the accessory map (+0x80); map::insert keeps an existing entry with that id.
    accessory Acc;
    Acc.Id = Id;
    Acc.Mesh = Mesh;
    Acc.AccessoryHardpoint = Hardpoint;
    Acc.CharacterHardpoint = BodyHardpoint;
    Acc.Materials = std::move(MaterialLibrary);
    Accessories.insert(std::map<unsigned, accessory>::value_type(Id, Acc));
}

void CostumeDescriptions::load_bodyparts(char const* Path)
{
    // open(Path, false) with no null test; FDUMP is called directly
    // (06300b15-06300b30).
    INI_Reader Reader;
    if (!Reader.open(Path, false))
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: Cannot open bodyparts ini file: %s",
              kCharDescripSource, 0x3b0, Path);
        return;
    }

    // and module storage seed both loop-carried locals with EDI == 0. The
    // "[DetailSwitchTable]" pointer is sticky across sections; the sex is sticky too, except
    // that every "[Skeleton]" header resets it to 0 first (06300603).
    int CurrentSex = 0;
    DetailSwitchTable* CurrentSwitch = nullptr;

    while (Reader.read_header())
    {
        if (Reader.is_header("Skeleton"))
        {
            CurrentSex = 0;
            while (Reader.read_value())
            {
                if (Reader.is_value("nomotion"))
                {
                    FDUMP(0x100002, "%s(%d) : *** WARNING: %s", kCharDescripSource, 0x33b,
                          "nomotion is obsolete");
                }
                else if (Reader.is_value("sex"))
                {
                    // NEURONCOMPONENTS stricmp(value, "none" / "male" / "female").
                    char const* Sex = Reader.get_value_string();
                    if (CallNeuronComponentsStricmp(Sex, "none") == 0)
                        CurrentSex = 0;
                    else if (CallNeuronComponentsStricmp(Sex, "male") == 0)
                        CurrentSex = 1;
                    else if (CallNeuronComponentsStricmp(Sex, "female") == 0)
                        CurrentSex = 2;
                }
            }
        }
        else if (Reader.is_header("Animations"))
        {
            // . The old port drained this section into an empty loop under
            // a "read and discarded, deliberate" note. That note was wrong: baseline keeps two
            // persistent, de-duplicated name lists on the CostumeDescriptions itself.
            while (Reader.read_value())
            {
                if (Reader.is_value("anim"))
                {
                    char const* Name = Reader.get_value_string();
                    if (!ListContainsI(AnimationNames, Name))
                        AnimationNames.push_back(std::string(Name));
                }
                else if (Reader.is_value("preload"))
                {
                    char const* Name = Reader.get_value_string();
                    if (!ListContainsI(PreloadNames, Name))
                        PreloadNames.push_back(std::string(Name));
                }
            }
        }
        else if (Reader.is_header("PetalAnimations"))
        {
            // . baseline allocates a fresh std::list<std::string> on the
            // stack for this one section (operator new(0x18) for the self-linked sentinel node at
            // ), fills it with de-duplicated "anim=" values, appends the
            // whole list to the registry at this+0x18 (module storage, the routine) and then
            // destroys the stack copy (module storage, the routine == erase(begin,end), followed
            // by operator delete of the sentinel). Only the "anim" key is recognised here --
            // "preload" is NOT accepted under [PetalAnimations].
            std::list<std::string> Names;
            while (Reader.read_value())
            {
                if (Reader.is_value("anim"))
                {
                    char const* Name = Reader.get_value_string();
                    if (!ListContainsI(Names, Name))
                        Names.push_back(std::string(Name));
                }
            }
            PetalAnimations.push_back(Names);
        }
        else if (Reader.is_header("DetailSwitchTable"))
        {
            // . Stack-construct a table (zeroed vector, reserve(0x10),
            // FovXHalf = 20.0f, SizeX = 1280.0f), parse it, append it to the registry at
            // this+0x24 (module storage, the routine), then remember the address of that
            // just-appended element (module storage-06300a10) as the sticky pointer handed to
            // every following load_bodypart. Taking the address of the list element rather than
            // of the stack temporary matters: the temporary is destroyed at module storage.
            DetailSwitchTable Table;
            DetailSwitchTableReserve(Table, 0x10);
            Table.read_from_ini(&Reader);
            DetailSwitchTables.push_back(Table);
            CurrentSwitch = &DetailSwitchTables.back();
        }
        else if (Reader.is_header("Accessory"))
        {
            load_accessory(&Reader);
        }
        else
        {
            // . baseline walks a four-entry table of 0x104-byte records at
            // ({int Id; char Name[0x100]}): {0,"Body"}, {1,"Head"}, {2,"LeftHand"},
            // {3,"RightHand"} -- exactly kBodypartTypeNames, and the stored Id equals the index,
            // so BodypartTypeFromName is equivalent. The loop bound is `ESI < 0x063ed17c`
            // (module storage), i.e. base + 4*0x104; the "not found" sentinel is Id == -1
            // (module storage).
            int TypeId = BodypartTypeFromName(Reader.get_header_ptr());
            if (TypeId >= 0)
            {
                load_bodypart(TypeId, CurrentSex, CurrentSwitch, &Reader);
            }
            else
            {
                // the header pointer is fetched again and passed raw.
                FDUMP(0x100001, "%s(%d) : *** ERROR: Unknown header %s in bodyparts.ini\n",
                      kCharDescripSource, 0x3a8, Reader.get_header_ptr());
            }
        }
    }

    // . Reached both when read_header first returns false and when the loop ends.
    Reader.close();
}

void CostumeDescriptions::load_costumes(char const* Path)
{
    // open(Path, false) with no null test; FDUMP called directly
    // (06300f50-06300f6b).
    INI_Reader Reader;
    if (!Reader.open(Path, false))
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: Cannot open costume ini file: %s",
              kCharDescripSource, 0x415, Path);
        return;
    }

    // ESP+0xd8, 0x400 bytes: the costume's name as the warnings print it.
    char NicknameText[0x400];

    while (Reader.read_header())
    {
        if (!Reader.is_header("Costume"))
        {
            FDUMP(0x100002, "%s(%d) : *** WARNING: Unknown header %s in costumes.ini\n",
                  kCharDescripSource, 0x40e, Reader.get_header_ptr());
            continue;
        }

        // Id, Gender, Parts[0..3] and the count are zeroed. baseline leaves
        // Accessories[] as stack garbage (only the first Count entries are ever read); the
        // port zeroes the whole record.
        costume Scratch;
        std::memset(&Scratch, 0, sizeof(Scratch));

        while (Reader.read_value())
        {
            // the name buffer is zeroed at the top of EVERY value line and
            // only the nickname branch fills it, so the three warnings below always print an
            // empty costume name. Very strange, but baseline.
            memset(NicknameText, 0, sizeof(NicknameText));

            if (Reader.is_value("nickname"))
            {
                // byte copy into the buffer, then Id = atol / CreateID.
                char const* Value = Reader.get_value_string();
                unsigned Index = 0;
                do
                {
                    NicknameText[Index] = Value[Index];
                } while (Value[Index++] != '\0');
                Scratch.Id = NicknameToCreateId(Value);
            }
            else if (Reader.is_value("accessory"))
            {
                // find_accessory(atol / CreateID of the value).
                char const* AccName = Reader.get_value_string();
                accessory const* Acc = find_accessory((unsigned long)NicknameToCreateId(AccName));
                if (Acc != nullptr)
                {
                    // Accessories[Count++] = Acc with no bound. baseline UB: a
                    // 9th line writes the Count slot itself (+0x38) and corrupts the record;
                    // the port drops accessories past 8 instead.
                    if (Scratch.AccessoryCount < kMaxAccessories)
                    {
                        Scratch.Accessories[Scratch.AccessoryCount] = Acc;
                        Scratch.AccessoryCount += 1;
                    }
                }
                else
                {
                    FDUMP(0x100002,
                          "%s(%d) : *** WARNING: Cannot find accessory %s for costume %s\n",
                          kCharDescripSource, 0x3e4, AccName, NicknameText);
                }
            }
            else
            {
                // the key name against the body-part table.
                char const* KeyName = Reader.get_name_ptr();
                int TypeId = BodypartTypeFromName(KeyName);
                if (TypeId < 0)
                {
                    FDUMP(0x100002,
                          "%s(%d) : *** WARNING: Invalid part type %s for costume %s\n",
                          kCharDescripSource, 0x404, KeyName, NicknameText);
                    continue;
                }

                // find_bodypart(type, atol / CreateID of the value); a hit
                // fills Parts[type] and the first non-zero part sex becomes the gender.
                char const* PartName = Reader.get_value_string();
                bodypart const* Part =
                    find_bodypart(TypeId, (unsigned long)NicknameToCreateId(PartName));
                if (Part != nullptr)
                {
                    Scratch.Parts[TypeId] = Part;
                    if (Part->Sex != 0 && Scratch.Gender == 0)
                        Scratch.Gender = Part->Sex;
                }
                else
                {
                    // prints the key name as written in the ini.
                    FDUMP(0x100002,
                          "%s(%d) : *** WARNING: Cannot find bodypart %s (type %s) for costume %s\n",
                          kCharDescripSource, 0x3fc, PartName, KeyName, NicknameText);
                }
            }
        }

        // the pair {Id, record} is inserted (the routine, unique insert into
        // the +0x94 map) whether or not a nickname line was seen; an existing id keeps its record.
        Costumes.insert(std::map<unsigned, costume>::value_type(Scratch.Id, Scratch));
    }

    // explicit close after the header loop.
    Reader.close();
}

bodypart const* CostumeDescriptions::find_bodypart(int TypeId, unsigned long NicknameId) const
{
    // Bodyparts[TypeId] with no range check on TypeId; returns node+0x10, the
    // address of the mapped bodypart, or null at the end sentinel.
    std::map<unsigned, bodypart>::const_iterator It =
        Bodyparts[TypeId].find((unsigned)NicknameId);
    if (It == Bodyparts[TypeId].end())
        return nullptr;
    return &It->second;
}

bodypart const* CostumeDescriptions::find_bodypart(int TypeId, char const* Name) const
{
    return find_bodypart(TypeId, NicknameToCreateId(Name));
}

accessory const* CostumeDescriptions::find_accessory(unsigned long NicknameId) const
{
    // returns the address of the mapped accessory (node+0x10), or null.
    std::map<unsigned, accessory>::const_iterator It =
        Accessories.find((unsigned)NicknameId);
    if (It == Accessories.end())
        return nullptr;
    return &It->second;
}

accessory const* CostumeDescriptions::find_accessory(char const* Name) const
{
    return find_accessory(NicknameToCreateId(Name));
}

costume const* CostumeDescriptions::find_costume(unsigned long NicknameId) const
{
    // returns the address of the mapped costume (node+0x10), or null.
    std::map<unsigned, costume>::const_iterator It =
        Costumes.find((unsigned)NicknameId);
    if (It == Costumes.end())
        return nullptr;
    return &It->second;
}

costume const* CostumeDescriptions::find_costume(char const* Name) const
{
    return find_costume(NicknameToCreateId(Name));
}

unsigned long CostumeDescriptions::find_costume_id(char const* Name) const
{
    costume const* Desc = find_costume(Name);
    if (Desc == nullptr)
        return 0;
    return Desc->Id;
}

unsigned long CostumeDescriptions::find_part_id(int TypeId, char const* Name) const
{
    bodypart const* Part = find_bodypart(TypeId, Name);
    if (Part == nullptr)
        return 0;
    return Part->Id;
}

char const* CostumeDescriptions::get_part_name(int TypeId, unsigned long NicknameId) const
{
    bodypart const* Part = find_bodypart(TypeId, NicknameId);
    if (Part == nullptr)
        return nullptr;
    // baseline's accessory::get_name / bodypart::get_name (shared/ICF-folded body, one symbol
    // serving both classes -- Common, called from here at .c:88633 via a
    // bodypart*->accessory* cross-cast) does NOT return a nickname string: bodypart has no
    // stored nickname text at all. bodypart::init (Common) hashes/atol's the
    // nickname argument straight into a numeric `Id` at offset 0 and never keeps the text;
    // the getter just decimal-formats that Id into a shared static scratch buffer via
    // `_ultoa(*(ulong*)this, buf, 10)`. A previous pass guessed "returns the nickname text"
    // by analogy with get_part_mesh and used Part->Nickname here -- that was wrong; reproduced
    // faithfully now (single shared static buffer, non-reentrant, matches baseline exactly).
    return Part->get_name();
}

char const* CostumeDescriptions::get_part_mesh(int TypeId, unsigned long NicknameId) const
{
    bodypart const* Part = find_bodypart(TypeId, NicknameId);
    if (Part == nullptr)
        return nullptr;
    return Part->get_mesh();
}

unsigned long CostumeDescriptions::find_accessory_id(char const* Name) const
{
    accessory const* Acc = find_accessory(Name);
    if (Acc == nullptr)
        return 0;
    return Acc->Id;
}

void CostumeDescriptions::get_costume(char const* Name, Costume& Out) const
{
    get_costume((int)NicknameToCreateId(Name), Out);
}

void CostumeDescriptions::get_costume(int NicknameId, Costume& Out) const
{
    costume const* Desc = find_costume((unsigned long)NicknameId);
    if (Desc == nullptr)
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: Costume id %x does not exist\n",
              kCharDescripSource, 0x27b, NicknameId);
        FillCostume(nullptr, Out);
        return;
    }
    FillCostume(Desc, Out);
}

int CostumeDescriptions::get_costume_gender(Costume const& Cost) const
{
    // Costume is head/body/left/right; the maps are body/head/left/right.
    const unsigned PartIds[4] = {
        Cost.PartIds[1], Cost.PartIds[0], Cost.PartIds[2], Cost.PartIds[3],
    };
    int Gender = 0;
    int Slot = 0;
    for (; Slot < 4; ++Slot)
    {
        unsigned const PartId = PartIds[Slot];
        if (PartId == 0)
            continue;

        // dereferences the found part: a non-zero ID must exist in its type's map.
        bodypart const* const Part = find_bodypart(Slot, PartId);
        if (Part->Sex == 0)
            continue;

        if (Gender == 0)
        {
            Gender = Part->Sex;
            continue;
        }
        if (Gender == Part->Sex)
            continue;

        FDUMP(0x100001,
              "%s(%d) : *** ERROR: Costume body/head/lhand/rhand values %u/%u/%u/%u have "
              "inconsistent gender\n",
              kCharDescripSource, 0x2a1, Cost.PartIds[1], Cost.PartIds[0], Cost.PartIds[2],
              Cost.PartIds[3]);
    }
    return Gender;
}

int CostumeDescriptions::get_costume_gender(unsigned long NicknameId) const
{
    costume const* Desc = find_costume(NicknameId);
    if (Desc == nullptr)
        return 0;
    return Desc->Gender;
}

void __cdecl LoadBodypartDescriptions(char const* Path)
{
    // the field created on first use (operator new(0xa8), a null result
    // stored as is), then load_bodyparts on it with no null test.
    EnsureCostumeDescriptions()->load_bodyparts(Path);
}

bool __cdecl Costume_is_equal(Costume const& A, Costume const& B)
{
    // Common. Two quirks are baseline's, not slips:
    //  - the four body-part comparisons clear the result but do NOT return early (0630105a
    //    falls through to the accessory-count check), so the count mismatch below still runs;
    //  - only the first AccessoryCount entries are compared, not all eight, and the loop
    //    likewise records a mismatch without breaking out.
    bool bEqual = true;
    unsigned Part = 0;
    for (; Part < 4; ++Part)
    {
        if (A.PartIds[Part] != B.PartIds[Part])
        {
            bEqual = false;
            break;
        }
    }

    // a differing accessory count is the one early exit.
    if (A.AccessoryCount != B.AccessoryCount)
        return false;

    // tests the count as SIGNED (TEST EDI,EDI / JLE) and puts no upper bound on it --
    // a count above 8 would walk off the end of the array in baseline too. The costume loader is
    // what keeps it in range; the comparison does not re-check.
    int const Used = (int)A.AccessoryCount;
    int Slot = 0;
    for (; Slot < Used; ++Slot)
    {
        if (A.AccessoryIds[Slot] != B.AccessoryIds[Slot])
            bEqual = false;
    }
    return bEqual;
}

void __cdecl LoadCostumeDescriptions(char const* Path)
{
    // as LoadBodypartDescriptions, then load_costumes with no null test.
    EnsureCostumeDescriptions()->load_costumes(Path);
}

void __cdecl FreeCostumeDescriptions()
{
    delete GCostumeDescriptions;
    GCostumeDescriptions = nullptr;
}

CostumeDescriptions const* __cdecl GetCostumeDescriptions()
{
    return GCostumeDescriptions;
}

bool __cdecl CostumeGenderAdjust(int* gender, int adjustment)
{
    // Common CostumeGenderAdjust @ 0x9ebc0
    if (adjustment != 0) {
        if (*gender == 0) {
            *gender = adjustment;
            return true;
        }
        if (*gender != adjustment) {
            return false;
        }
    }
    return true;
}
