// Author: Giperion.

#include "common_newchar.h"

#include "common_ini.h"

extern "C"
{
    __declspec(dllimport) extern void(__cdecl* FDUMP)(int flags, const char* fmt, ...);
}

namespace
{
    const char kNewCharSource[] = "Common\\NewCharDB.cpp";

    NewChar::Database GNewCharDatabase;
}

NewChar::IDatabase* NewChar::TheDB = &GNewCharDatabase;

NewChar::Database::Database()
{
}

NewChar::Database::~Database()
{
    Shutdown();
}

bool NewChar::Database::Load(char const* Path)
{
    // (slot 14). Every section is parsed inline into a stack record that is
    // handed to its insert slot through the vtable (slots 6 / 7 / 8, and slot 9 for each
    // faction base).
    INI_Reader Reader;
    if (!Reader.open(Path, false))
    {
        // .
        FDUMP(0x100001, "%s(%d) : *** ERROR: failed to open configuration file (%s)\n",
              kNewCharSource, 0x1cd, Path);
        return false;
    }

    // The [Package] record's +0x04 / +0x08 / +0x2c ([ESP+0xa8] / [ESP+0xac] / [ESP+0xd0]) are
    // never initialised (module storage..06314df3 builds only the id and the two strings), and no
    // other section writes that stack range, so a [Package] missing strid_name / strid_desc /
    // money keeps the previous [Package]'s value. The first one's start value is whatever the
    // stack held; the port starts from 0.
    int PackageStridName = 0;
    int PackageStridDesc = 0;
    int PackageMoney = 0;

    while (Reader.read_header())
    {
        if (Reader.is_header("Faction"))
        {
            // the 0x50-byte record (ID_String::clear on the id, an
            // empty rep_group string, three empty sets), one branch per key, then slot 6.
            FNewCharFaction Faction;
            Faction.NicknameId = 0;
            while (Reader.read_value())
            {
                if (Reader.is_value("nickname"))
                {
                    // .
                    Faction.NicknameId = CreateID(Reader.get_value_string());
                }
                else if (Reader.is_value("rep_group"))
                {
                    // the string assigned (strlen, grow, copy).
                    Faction.RepGroup.assign(Reader.get_value_string());
                }
                else if (Reader.is_value("base"))
                {
                    // a {CreateID(base), base} record through slot 9
                    // (`CALL [EAX + 0x24]`); Server.dll's the routine finds it by the base id
                    // the client sends (slot 3) and writes its string as %%Home_Base%%.
                    FNewCharExtra Extra;
                    Extra.Id = CreateID(Reader.get_value_string());
                    Extra.Value.assign(Reader.get_value_string());
                    InsertExtra(Extra);
                    // then that record's id into the base set (+0x14).
                    Faction.Bases.insert(Extra.Id);
                }
                else if (Reader.is_value("package"))
                {
                    // -> the routine on +0x28.
                    Faction.Packages.insert(CreateID(Reader.get_value_string()));
                }
                else if (Reader.is_value("pilot"))
                {
                    // -> the routine on +0x3c.
                    Faction.Pilots.insert(CreateID(Reader.get_value_string()));
                }
            }
            // slot 6; its answer is not read.
            InsertFaction(Faction);
        }
        else if (Reader.is_header("Package"))
        {
            // the 0x30-byte record (see above for +0x04 / +0x08 /
            // +0x2c), one branch per key, then slot 7.
            FNewCharPackage Package;
            Package.NicknameId = 0;
            Package.StridName = PackageStridName;
            Package.StridDesc = PackageStridDesc;
            Package.Money = PackageMoney;
            while (Reader.read_value())
            {
                if (Reader.is_value("nickname"))
                {
                    // only the id is kept.
                    Package.NicknameId = CreateID(Reader.get_value_string());
                }
                else if (Reader.is_value("strid_name"))
                {
                    // ftol(get_value_float(0)).
                    Package.StridName = (int)Reader.get_value_float(0);
                }
                else if (Reader.is_value("strid_desc"))
                {
                    // .
                    Package.StridDesc = (int)Reader.get_value_float(0);
                }
                else if (Reader.is_value("ship"))
                {
                    // assign (strlen, grow, copy, eos) into +0x0c.
                    Package.Ship.assign(Reader.get_value_string());
                }
                else if (Reader.is_value("loadout"))
                {
                    // into +0x1c.
                    Package.Loadout.assign(Reader.get_value_string());
                }
                else if (Reader.is_value("money"))
                {
                    // .
                    Package.Money = (int)Reader.get_value_float(0);
                }
            }
            PackageStridName = Package.StridName;
            PackageStridDesc = Package.StridDesc;
            PackageMoney = Package.Money;
            // slot 7; its answer is not read.
            InsertPackage(Package);
        }
        else if (Reader.is_header("Pilot"))
        {
            // the 0x64-byte record (the id cleared, every string and
            // both vectors empty), one branch per key, then slot 8.
            FNewCharPilot Pilot;
            Pilot.NicknameId = 0;
            while (Reader.read_value())
            {
                if (Reader.is_value("nickname"))
                {
                    // only the id is kept.
                    Pilot.NicknameId = CreateID(Reader.get_value_string());
                }
                else if (Reader.is_value("body"))
                {
                    // assign into +0x04.
                    Pilot.Body.assign(Reader.get_value_string());
                }
                else if (Reader.is_value("body.anim"))
                {
                    // the vector emptied (the routine erase(begin,
                    // end)), then every parameter up to the first empty one appended through a
                    // temporary string (the routine insert at end).
                    Pilot.BodyAnims.clear();
                    unsigned ParamIndex = 0;
                    while (!Reader.is_value_empty(ParamIndex))
                    {
                        std::string const Anim(Reader.get_value_string(ParamIndex));
                        ParamIndex += 1;
                        Pilot.BodyAnims.push_back(Anim);
                    }
                }
                else if (Reader.is_value("comm"))
                {
                    // assign into +0x24.
                    Pilot.Comm.assign(Reader.get_value_string());
                }
                else if (Reader.is_value("comm.anim"))
                {
                    // as body.anim, on +0x34.
                    Pilot.CommAnims.clear();
                    unsigned ParamIndex = 0;
                    while (!Reader.is_value_empty(ParamIndex))
                    {
                        std::string const Anim(Reader.get_value_string(ParamIndex));
                        ParamIndex += 1;
                        Pilot.CommAnims.push_back(Anim);
                    }
                }
                else if (Reader.is_value("voice"))
                {
                    // assign into +0x44.
                    Pilot.Voice.assign(Reader.get_value_string());
                }
                else if (Reader.is_value("thumb"))
                {
                    // assign into +0x54.
                    Pilot.Thumb.assign(Reader.get_value_string());
                }
            }
            // slot 8; its answer is not read.
            InsertPilot(Pilot);
        }
    }

    return true;
}

bool NewChar::Database::Slot15(void* Arg)
{
    // baseline slot 15 (vcall +0x3c) folded by /OPT:ICF onto 0x0629ba10, whose
    // entire body is `XOR AL,AL ; RET 0x4` -- it unconditionally returns false
    // and ignores its one argument. That is the whole observable behaviour;
    // the slot's meaning is still unknown and the symbol the linker left on
    // that address (CEShield::GetEquipDesc) belongs to an unrelated class.
    // TODO: identify slot 15 from a caller outside Common.dll.
    (void)Arg;
    return false;
}

void NewChar::Database::Shutdown()
{
    // clears this+0x04 (module storage, Factions),
    // +0x18 (module storage, Packages), +0x2c (module storage, Pilots), then
    // the 4th collection (+0x40, module storage) in that order.
    Factions.clear();
    Packages.clear();
    Pilots.clear();
    Extras.clear();
}

FNewCharPilot const* NewChar::Database::FindPilot(unsigned NicknameId) const
{
    std::map<unsigned, FNewCharPilot>::const_iterator It = Pilots.find(NicknameId);
    if (It == Pilots.end())
        return nullptr;
    return &It->second;
}

FNewCharPackage const* NewChar::Database::FindPackage(unsigned NicknameId) const
{
    std::map<unsigned, FNewCharPackage>::const_iterator It = Packages.find(NicknameId);
    if (It == Packages.end())
        return nullptr;
    return &It->second;
}

FNewCharFaction const* NewChar::Database::FindFaction(unsigned NicknameId) const
{
    std::map<unsigned, FNewCharFaction>::const_iterator It = Factions.find(NicknameId);
    if (It == Factions.end())
        return nullptr;
    return &It->second;
}

bool NewChar::Database::ForEachFaction(bool(__cdecl* Callback)(unsigned, FNewCharFaction const*, void*), void* Context) const
{
    // (slot 4, vcall +0x10): in-order walk of the tree at
    // this+0x04 (`MOV EAX,[ECX+0x8]`), which Load fills from [Faction]
    // (slot 6) -- the Factions, not the Pilots (corrected 2026-09-24). std::map
    // already iterates in ascending key order, matching the baseline RB-tree walk
    // -- see common_newchar.h class comment. baseline calls the callback
    // (`CALL EBX`, module storage) with no null test; neither does the port.
    for (std::map<unsigned, FNewCharFaction>::const_iterator It = Factions.begin(); It != Factions.end(); ++It)
    {
        if (!Callback(It->first, &It->second, Context))
            return false;
    }
    return true;
}

bool NewChar::Database::RemovePilot(unsigned NicknameId)
{
    // (slot 12, vcall +0x30, tree this+0x2c): erase-by-key,
    // false if absent.
    return Pilots.erase(NicknameId) != 0;
}

bool NewChar::Database::RemovePackage(unsigned NicknameId)
{
    // (slot 11, vcall +0x2c): erase-by-key, false if absent.
    return Packages.erase(NicknameId) != 0;
}

bool NewChar::Database::RemoveFaction(unsigned NicknameId)
{
    // (slot 10, vcall +0x28, tree this+0x04): erase-by-key,
    // false if absent.
    return Factions.erase(NicknameId) != 0;
}

bool NewChar::Database::InsertExtra(FNewCharExtra const& Extra)
{
    // (slot 9, vcall +0x24, RET 4): the key (+0) found in the tree at
    // this+0x40 -> false; else the routine (operator[]) makes the node, the id is copied to
    // the record's +0 (module storage-0631471d) and the string (+4) assigned -> true.
    if (Extras.find(Extra.Id) != Extras.end())
        return false;
    FNewCharExtra& Record = Extras[Extra.Id];
    Record.Id = Extra.Id;
    Record.Value = Extra.Value;
    return true;
}

bool NewChar::Database::InsertFaction(FNewCharFaction const& Faction)
{
    // (slot 6, vcall +0x18, RET 4): the key (+0) found in the tree at
    // this+0x04 -> false; else a new node: the id, the string at +4 assigned, the three sets
    // (+0x14 / +0x28 / +0x3c) copied -> true.
    if (Factions.find(Faction.NicknameId) != Factions.end())
        return false;
    Factions[Faction.NicknameId] = Faction;
    return true;
}

bool NewChar::Database::InsertPackage(FNewCharPackage const& Package)
{
    // (slot 7, vcall +0x1c, RET 4): the routine lower bound in the tree at
    // this+0x18; not the end and the key not below the node's (JC) -> present -> false
    // (0x063144a7). Else the routine makes the node: +0x00 / +0x04 / +0x08 copied, the strings
    // at +0x0c / +0x1c assigned, +0x2c copied -> true.
    if (Packages.find(Package.NicknameId) != Packages.end())
        return false;
    Packages[Package.NicknameId] = Package;
    return true;
}

bool NewChar::Database::InsertPilot(FNewCharPilot const& Pilot)
{
    // (slot 8, vcall +0x20, RET 4): the same lower-bound test on the tree at
    // this+0x2c -> false (0x063146b2). Else the routine makes the node: +0x00 copied, the
    // strings at +0x04 / +0x24 / +0x44 / +0x54 assigned, the vectors at +0x14 (inline
    // operator=) and +0x34 (the routine) copied -> true.
    if (Pilots.find(Pilot.NicknameId) != Pilots.end())
        return false;
    Pilots[Pilot.NicknameId] = Pilot;
    return true;
}

bool NewChar::Database::RemoveExtra(unsigned Id)
{
    // (slot 13, vcall +0x34): erase-by-key, false if absent.
    return Extras.erase(Id) != 0;
}

FNewCharExtra const* NewChar::Database::FindExtra(unsigned Id) const
{
    // (slot 3, vcall +0x0c): find on the tree at this+0x40; miss -> 0,
    // hit -> node+0x10, the stored {Id, Value} record.
    std::map<unsigned, FNewCharExtra>::const_iterator It = Extras.find(Id);
    if (It == Extras.end())
        return nullptr;
    return &It->second;
}
