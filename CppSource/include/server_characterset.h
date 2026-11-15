// Author: Giperion.

#pragma once

// ===========================================================================
// server_characterset.h -- the character-select list: the record Server.dll
// builds for every saved character, the list IClient slot 13 (ch2 0x03
// FLPACKET_SERVER_CHARACTERINFO) carries, and the ONE declaration Server.dll,
// rpclocal.dll and OutpostFrontier.exe all read it through.

// NAMES. baseline's own, from RTTI. Server.dll's IServerImpl::CharacterInfoReq
// stamps module storage on its 0x18-byte local (module storage) and, in the
// teardown, module storage (module storage). The locators behind them:
// -> 06d70910, type module storage
//       -> module storage ".?AV?$OwnerList@UCharacterInfo@CCharacterSet@@@@"
// -> 06d70954, type module storage
//       -> module storage ".?AV?$MetaList@UCharacterInfo@CCharacterSet@@@@"
// and 06d70910's base list names MetaList<> as OwnerList<>'s base
// (module storage). OutpostFrontier.exe carries the same two descriptors
// (module storage / 00615b5c). So the record is `struct
// CCharacterSet::CharacterInfo` and the list is OwnerList<> of it -- the
// template in common_metalist.h, whose MetaList<GoodDesc> bodies are
// instruction-for-instruction what this instantiation compiled to:
//   * CharacterInfoReq's teardown (module storage .. module storage) is
//     OwnerList::free (record destructor the routine + free, Data = 0, then
//     the nodes through the routine = free_node) followed by MetaList's
//     destructor (the MetaList vptr, then the pool drain);
//   * the builder's the routine / the routine / the routine are prepend /
//     append / alloc_node, and its inline link at module storage .. 06d49ef1
//     is insert_after;
//   * rpclocal's the routine / the routine (vtables module storage /
// ) are ~MetaList / ~OwnerList of the same instantiation;
//   * OutpostFrontier.exe's the routine is append.

// WHAT THE 0x18 OBJECT IS. The OwnerList sits at +0x00 with ITS OWN vptr as
// the complete-object vptr (the locator at 06d70910 has offset 0), so whatever
// encloses it adds no virtual function; the only other field is the dword at
// +0x14. That the enclosing type is CCharacterSet itself is the natural reading
// of the RTTI (CharacterInfo is nested in it) but is NOT proven by a descriptor
// of its own -- CCharacterSet has none, because it is not polymorphic.

// MODERN TYPES. baseline's two strings are MSVCP60 basic_string<unsigned short>
// (VC6 spelled wchar_t that way) and its two lists are VC6 std::list; the port
// uses std::wstring and std::list, so the record is not 0x2dc bytes here and no
// offset below is a byte offset of the port's struct. They are baseline's record
// offsets, kept because every reader in every module is written against them.
// Only the ORDER and the VALUES are baseline's.
// ===========================================================================

#include "common_server_types.h"   // CHARACTER_ID
#include "common_goods.h"          // EquipDesc, CollisionGroupDesc, Costume, OwnerList<T>

#include <list>
#include <string>

class CCharacterSet
{
public:
    struct CharacterInfo;

    // +0x00 .. +0x13. bUseHeap is false on every constructor in all three
    // modules (CharacterInfoReq's inline stores module storage .. 06d45284;
    // rpclocal the routine with `PUSH EBP` = 0 at module storage).
    OwnerList<CharacterInfo> Characters;

    // +0x14. Server fills it from PlayerData::save_failure_count (+0x344) and
    // then zeroes the counter (module storage .. module storage); rpclocal
    // carries it as the payload's first dword (06d89e9d / 06d84a93);
    // OutpostFrontier.exe copies it to its connect context +0x1cc (module storage).
    // CharacterInfoReq never writes it when its bool is true (the local is
    // uninitialised there); the port's constructor zeroes it.
    unsigned int SaveFailureCount;

    // Defined below, after OwnerListDeleteData's specialisation for the record:
    // the constructor is what instantiates OwnerList<CharacterInfo>'s vtable,
    // and with it free() and the element delete.
    CCharacterSet();
};

// The record. Server.dll the routine builds it from a PlayerData::FSavedCharacter
// (the per-field source is named on each member); OutpostFrontier.exe the routine
// copies it member by member (module storage .. module storage) and the routine
// reads it at character select.
struct CCharacterSet::CharacterInfo
{
    CHARACTER_ID Id;                 // +0x000 the saved_characters key (module storage)
    // +0x200 / +0x201: stricmp(FileName, "Restart.fl") == 0 and
    // stricmp(FileName, "AutoSave.fl") == 0 (module storage / module storage,
    // `NEG / SBB / INC`). Also the first two keys of the list's sort order.
    unsigned char bIsRestart;        // +0x200
    unsigned char bIsAutoSave;       // +0x201
    std::wstring Name;               // +0x204 <- FSavedCharacter::Name
    std::wstring Description;        // +0x214 <- ::Description; L"" for the autosave
    unsigned int DescripStrId;       // +0x224 <- ::DescripStrId; 0x65c for the autosave
    // +0x228 / +0x22c: a FILETIME -- CompareFileTime reads it at module storage.
    unsigned int TimestampLow;       // +0x228 <- ::TimestampLow  (dwLowDateTime)
    unsigned int TimestampHigh;      // +0x22c <- ::TimestampHigh (dwHighDateTime)
    unsigned int ShipArchetype;      // +0x230 <- ::ShipArchetype
    int Money;                       // +0x234 <- ::Money
    int NumKills;                    // +0x238 <- ::NumKills
    int NumMissionSuccesses;         // +0x23c <- ::NumMissionSuccesses
    int NumMissionFailures;          // +0x240 <- ::NumMissionFailures
    float HullStatus;                // +0x244 <- ::HullStatus
    // +0x248 is a COMMON.DLL EquipDescList: the inlined constructor calls
    // ??0EquipDescList@@QAE@XZ on it (Server module storage via module storage,
    // rpclocal module storage via module storage), and rpclocal's EH map destroys it
    // with the imported ~EquipDescList (Unwind@06db2349, ECX = record+0x248). In
    // OutpostFrontier.exe the same member's ctor/operator= are expanded to the underlying
    // std::list<EquipDesc> imports (module storage, module storage).
    EquipDescList Equipment;                     // +0x248 <- ::Equipment
    std::list<CollisionGroupDesc> CollisionGroups; // +0x254 <- ::CollisionGroups
    unsigned int SystemId;           // +0x260 <- ::SystemId   (module storage)
    unsigned int BaseId;             // +0x264 <- ::BaseId     (module storage)
    unsigned int LocationId;         // +0x268 <- ::LocationId (module storage)
    unsigned int VoiceId;            // +0x26c <- CreateID(::Voice's text) (module storage)
    unsigned int Rank;               // +0x270 <- ::Rank       (module storage)
    Costume ComCostume;              // +0x274 <- ::ComCostume  (module storage)
    Costume BaseCostume;             // +0x2a8 <- ::BaseCostume (module storage)

    // The constructor both Server (module storage .. module storage) and
    // OutpostFrontier.exe (module storage .. module storage) inline: the key's first
    // byte, both flags and every dword are zeroed, the two Costumes go through
    // their constructor (the routine / the routine: the four parts and the
    // count, not the accessory slots). Each costume copy reads accessory ids
    // only up to AccessoryCount; the remaining slots stay unwritten.
    CharacterInfo()
        : bIsRestart(0), bIsAutoSave(0), DescripStrId(0), TimestampLow(0), TimestampHigh(0),
          ShipArchetype(0), Money(0), NumKills(0), NumMissionSuccesses(0),
          NumMissionFailures(0), HullStatus(0.0f), SystemId(0), BaseId(0), LocationId(0),
          VoiceId(0), Rank(0)
    {
        Id.Buffer[0] = '\0';
        // RS 06bd4eb0 / Server 06d0a060 / game 004cdc10: ComCostume, then BaseCostume.
        ComCostume.PartIds[0] = 0;
        ComCostume.PartIds[1] = 0;
        ComCostume.PartIds[2] = 0;
        ComCostume.PartIds[3] = 0;
        ComCostume.AccessoryCount = 0;
        BaseCostume.PartIds[0] = 0;
        BaseCostume.PartIds[1] = 0;
        BaseCostume.PartIds[2] = 0;
        BaseCostume.PartIds[3] = 0;
        BaseCostume.AccessoryCount = 0;
    }
};

// OwnerList<T>::free deletes its elements through OwnerListDeleteData. This
// record has real members, and baseline's free runs its destructor before the
// free: Server the routine + the routine (module storage / module storage),
// rpclocal the routine's record pass (the two lists, the two strings, then the
// block, module storage .. module storage).
template <>
inline void OwnerListDeleteData<CCharacterSet::CharacterInfo>(CCharacterSet::CharacterInfo* Data)
{
    delete Data;
}

// The baseline Costume assignment every CharacterInfo copy uses (Server inline at
// .. module storage, OutpostFrontier.exe the routine): the four
// parts, the count, then only `count` accessory ids, with a SIGNED count test
// (`JLE` / `JL`).
inline void CharacterInfoCopyCostume(Costume& Dest, Costume const& Source)
{
    Dest.PartIds[0] = Source.PartIds[0];
    Dest.PartIds[1] = Source.PartIds[1];
    Dest.PartIds[2] = Source.PartIds[2];
    Dest.PartIds[3] = Source.PartIds[3];
    Dest.AccessoryCount = Source.AccessoryCount;
    for (int Index = 0; Index < static_cast<int>(Dest.AccessoryCount); ++Index)
        Dest.AccessoryIds[Index] = Source.AccessoryIds[Index];
}

inline CCharacterSet::CCharacterSet()
    : Characters(), SaveFailureCount(0)
{
}
