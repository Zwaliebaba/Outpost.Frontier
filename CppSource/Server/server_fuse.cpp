// Author: Giperion.

// Server.dll fuse database and fuse-action classes -- baseline module storage-06cffc97.
// See src/include/server_fuse.h for what is and is not ported.

#include "../include/server_fuse.h"

#include "../include/common_ini.h"
#include "../include/common_cequip.h"      // CEquipTraverser, CExternalEquip (DestroyHardpoint)
#include "../include/common_goods.h"       // FindHardpoint_PS / HardpointToWorld (ImpulseAction)
#include "../include/server_gameobject.h"  // EqObj / GameObject slots every Trigger calls
#include "../include/server_sysdb.h"       // StarSystem::find_object (the routine)
#include "../include/server_createinfo.h"  // Server_Acquire3DMathEngine (the routine)
#include "../include/x86math.h"            // I3DMathEngine::Rotate (TumbleAction)

#include <cstdlib> // rand (baseline: MSVCRT.DLL::rand)
#include <cstring> // _stricmp (baseline: NEURONCOMPONENTS.DLL::stricmp)

namespace
{
// .data:the field -- Fuse.cpp's own lazily acquired "3DMathEngine" (the routine), read by
// TumbleAction::Trigger (module storage) and ImpulseAction::Trigger (module storage). baseline
// never tests the engine for null.
I3DMathEngine* GFuseMathEngine = nullptr;

// The DamageList every damage Trigger builds on its stack. baseline inlines the DamageList
// constructor (e.g. the branch module storage-06cfe1d0: list head, Count = 0,
// bDestroyed = 0, InflictorOwner = 0) and then fills it in the same order everywhere
// (module storage-06cfe233):
//   Cause = 0x15, InflictorId = SourceId;
//   if (Owner != 0 && Owner->OwnerSystem (+0x28) != 0)
//       if (Source = the routine(OwnerSystem, SourceId, 0x7f0000))
//           InflictorOwner = Source's CSimple (+0x10 under the `& 3 == 3` guard)->+0xb4;
// The CSimple read has no null test: a source whose object is not a CSimple faults in baseline
// (`XOR EAX,EAX` then `MOV reg,[EAX+0xb4]`), and here.
void InitFuseDamageList(DamageList& Damage, EqObj* Owner, unsigned SourceId)
{
    unsigned int InflictorOwner = 0;
    Damage.Cause = static_cast<DamageCause>(0x15);
    Damage.InflictorId = SourceId;
    if (Owner != nullptr && Owner->OwnerSystem != nullptr)
    {
        GameObject* Source = Owner->OwnerSystem->find_object(SourceId, 0x7f0000u);
        if (Source != nullptr)
            InflictorOwner = Source->csimple()->OwnerPlayer;
    }
    Damage.InflictorOwner = InflictorOwner;
}

// TumbleAction's random interpolation, four times inline (module storage-06cff7b4 and its
// copies): FILD rand(); FMUL [module storage] (0x38000000 = 2^-15); FLD High; FSUB Low;
// FMULP; FADD Low; FSTP float. Evaluated in double, the x87 precision MSVC runs with.
float RandomBetween(int Roll, float Low, float High)
{
    double const Scaled = static_cast<double>(Roll) * (1.0 / 32768.0);
    return static_cast<float>(Scaled * (static_cast<double>(High) - static_cast<double>(Low)) +
                              static_cast<double>(Low));
}
} // namespace

// ---------------------------------------------------------------------------
// ServerFuse
// ---------------------------------------------------------------------------

ServerFuse::ServerFuse(ID_String const& Nickname, float Lifetime)
    : Fuse(Nickname, Lifetime)
    , Owner(nullptr)
    , bHasDestroyRoot(false)
{
}

ServerFuse::ServerFuse(ServerFuse const& Other)
    : Fuse(Other)
    , Owner(Other.Owner)
    , bHasDestroyRoot(Other.bHasDestroyRoot)
{
}

ServerFuse::~ServerFuse()
{
    // Fuse::~Fuse, then operator delete when bit 0 of the flag is set.
}

bool ServerFuse::Read(INI_Reader& Reader)
{
    // .
    if (!ReadFuseValues(Reader))
        return false; // -> the branch
    if (!Reader.read_header())
        return true;  // -> the branch

    for (;;)
    {
        // The headers are tested in this order (module storage .. 06cfcb0a).
        ServerFuseAction* Action = nullptr;
        if (Reader.is_header("ignite_fuse"))
        {
            Action = new IgniteFuseAction();
        }
        else if (Reader.is_header("destroy_root"))
        {
            // `MOV byte ptr [EAX + 0x38],0x1` -- before the allocation.
            bHasDestroyRoot = true;
            Action = new DestroyRootAction();
        }
        else if (Reader.is_header("damage_root"))
        {
            Action = new DamageRootAction();
        }
        else if (Reader.is_header("destroy_group"))
        {
            Action = new DestroyGroupAction();
        }
        else if (Reader.is_header("damage_group"))
        {
            Action = new DamageGroupAction();
        }
        else if (Reader.is_header("destroy_hp_attachment"))
        {
            Action = new DestroyHardpointAttachmentAction();
        }
        else if (Reader.is_header("damage_hp_attachment"))
        {
            Action = new DamageHardpointAttachmentAction();
        }
        else if (Reader.is_header("impulse"))
        {
            Action = new ImpulseAction();
        }
        else if (Reader.is_header("tumble"))
        {
            Action = new TumbleAction();
        }
        else if (Reader.is_header("make_invincible"))
        {
            Action = new MakeInvincibleAction();
        }
        else if (Reader.is_header("dump_cargo"))
        {
            Action = new DumpCargoAction();
        }
        else if (Reader.is_header("fuse"))
        {
            // the next fuse starts here; the caller reads it.
            return true;
        }

        if (Action != nullptr)
        {
            // vt+8. Unlike the exe reader (the routine), a failed read does NOT
            // end the fuse: module storage `JZ the branch` skips the insert and goes on to the
            // next header, leaking the action (baseline).
            if (Action->Slot08(Reader))
            {
                // FLD [action+4] -> key, the routine multimap insert.
                Actions.insert(std::multimap<float, FuseAction*>::value_type(
                    Action->GetTriggerTime(), Action));
                // vt+0x24.
                if (Action->ShouldRandomize())
                    bNeedsRandomize = true;
            }
        }

        // .
        if (!Reader.read_header())
            return true;
    }
}

void ServerFuse::SetOwner(EqObj* NewOwner)
{
    // +0x34 = owner, then every action's +0x24 = owner (walks the action tree
    // with the routine, the _Tree iterator increment).
    Owner = NewOwner;
    for (std::multimap<float, FuseAction*>::iterator It = Actions.begin(); It != Actions.end(); ++It)
        static_cast<ServerFuseAction*>(It->second)->Owner = NewOwner;
}

// ---------------------------------------------------------------------------
// ServerFuseAction
// ---------------------------------------------------------------------------

ServerFuseAction::ServerFuseAction()
    : Owner(nullptr)
{
}

ServerFuseAction::~ServerFuseAction()
{
    // is ~FuseAction inlined; the base destructor does the same here.
}

// ---------------------------------------------------------------------------
// IgniteFuseAction
// ---------------------------------------------------------------------------

IgniteFuseAction::IgniteFuseAction()
    : FuseT(0.0f)
{
    // / 06cfcf01-06cfcf0f: ID_String::clear on +0x28, +0x2c = 0.
    FuseId.clear();
}

FuseAction* IgniteFuseAction::Slot04() const
{
    // new 0x30 object, then its slot 12 copies *this.
    IgniteFuseAction* Copy = new IgniteFuseAction();
    Copy->Slot30(*this);
    return Copy;
}

bool IgniteFuseAction::Slot08(INI_Reader& Reader)
{
    // .
    while (Reader.read_value())
    {
        if (ReadFuseActionValue(Reader))
            continue;
        if (Reader.is_value("fuse"))
            FuseId.Id = CreateID(Reader.get_value_string());
        else if (Reader.is_value("fuse_t"))
            FuseT = (float)Reader.get_value_float(0);
    }
    return true;
}

void IgniteFuseAction::Slot30(IgniteFuseAction const& Other)
{
    // (shared by DestroyGroupAction and DestroyHardpointAttachmentAction):
    // CopyArchProperties, then the dwords at +0x28 and +0x2c.
    FuseAction::CopyArchProperties(Other);
    FuseId = Other.FuseId;
    FuseT = Other.FuseT;
}

void IgniteFuseAction::Trigger(unsigned SourceId, unsigned short SubObjId)
{
    // , RET 8.
    // CALL [EAX + 0x18]        ; IsTriggered() (slot 6)
    // PUSH 0xbf800000          ; Lifetime = -1.0f
    // PUSH [ESI + 0x2c]        ; FuseT
    // PUSH EDI                 ; SubObjId
    // PUSH &[ESI + 0x28]       ; FuseId
    // PUSH SourceId
    // CALL [EDX + 0x1e4]       ; Owner->light_fuse, result unused
    if (!IsTriggered())
        Owner->light_fuse(SourceId, FuseId, SubObjId, FuseT, -1.0f);
    // FuseAction::Trigger (module storage) inlined.
    FuseAction::Trigger(SourceId, SubObjId);
}

void IgniteFuseAction::UnTrigger(unsigned short SubObjId)
{
    // , RET 4.
    // find SubObjId in [+0x18, +0x1c) and erase it (shift the tail
    //                            down, _Last -= 2) -- FuseAction::UnTrigger (module storage)
    //                            inlined; nothing happens when it is absent.
    // bTriggered = 0 -- the base body leaves the flag alone.
    // Owner->unlight_fuse(&FuseId, SubObjId, FuseT), CALL
    //                            [EDX + 0x1e8], result unused -- on both paths.
    FuseAction::UnTrigger(SubObjId);
    bTriggered = false;
    Owner->unlight_fuse(FuseId, SubObjId, FuseT);
}

// ---------------------------------------------------------------------------
// DestroyRootAction
// ---------------------------------------------------------------------------

DestroyRootAction::DestroyRootAction()
{
    // FuseAction(), +0x24 = 0, vftable. No fields of its own.
}

FuseAction* DestroyRootAction::Slot04() const
{
    // new 0x28 object (FuseAction constructor inlined), then vt+0x2c
    // (CopyArchProperties, slot 11) on it with *this.
    DestroyRootAction* Copy = new DestroyRootAction();
    Copy->CopyArchProperties(*this);
    return Copy;
}

bool DestroyRootAction::Slot08(INI_Reader& Reader)
{
    // every value goes to ReadFuseActionValue; its result is not tested.
    while (Reader.read_value())
        ReadFuseActionValue(Reader);
    return true;
}

void DestroyRootAction::Trigger(unsigned SourceId, unsigned short SubObjId)
{
    // , RET 8.
    // CALL [EAX + 0x18]        ; IsTriggered() (slot 6)
    // CALL [EDX + 0x158]       ; Owner->destroy(1, SourceId)
    if (!IsTriggered())
        Owner->destroy(1, SourceId);
    // is FuseAction::Trigger (module storage) inlined: bTriggered = 1,
    // append SubObjId through vector::insert(end, 1, id) unless present, then a side-effect-free
    // second find.
    FuseAction::Trigger(SourceId, SubObjId);
}

// ---------------------------------------------------------------------------
// DamageRootAction
// ---------------------------------------------------------------------------

DamageRootAction::DamageRootAction()
    : Hitpoints(0)
    , bCanKill(true)
{
    // / 06cfcbcb-06cfcbce: +0x28 = 0, +0x2c = 1.
}

FuseAction* DamageRootAction::Slot04() const
{
    // .
    DamageRootAction* Copy = new DamageRootAction();
    Copy->Slot30(*this);
    return Copy;
}

bool DamageRootAction::Slot08(INI_Reader& Reader)
{
    // .
    while (Reader.read_value())
    {
        if (ReadFuseActionValue(Reader))
            continue;
        if (Reader.is_value("damage_type"))
        {
            // NEURONCOMPONENTS stricmp against "absolute".
            if (_stricmp(Reader.get_value_string(), "absolute") == 0)
                bCanKill = false;
            else
                bCanKill = true;
        }
        else if (Reader.is_value("hitpoints"))
        {
            Hitpoints = Reader.get_value_int(0);
        }
    }
    return true;
}

void DamageRootAction::Slot30(DamageRootAction const& Other)
{
    // CopyArchProperties, dword +0x28, byte +0x2c.
    FuseAction::CopyArchProperties(Other);
    Hitpoints = Other.Hitpoints;
    bCanKill = Other.bCanKill;
}

void DamageRootAction::Trigger(unsigned SourceId, unsigned short SubObjId)
{
    // , RET 8.
    // CALL [EAX + 0x18]        ; IsTriggered() (slot 6)
    // ; the stack DamageList (InitFuseDamageList)
    // FILD [ESI + 0x28]        ; (float)Hitpoints
    // MOV DL,[ESI + 0x2c]      ; bCanKill
    // CALL [EAX + 0x1c0]       ; Owner->inflict_hull_damage(hp, bCanKill, DL)
    // CALL [EAX + 0x188]       ; Owner->apply_damage_list(DL)
    // ; ~DamageList inlined
    if (!IsTriggered())
    {
        DamageList Damage;
        InitFuseDamageList(Damage, Owner, SourceId);
        Owner->inflict_hull_damage(static_cast<float>(Hitpoints), bCanKill, Damage);
        Owner->apply_damage_list(Damage);
    }
    // FuseAction::Trigger (module storage) inlined.
    FuseAction::Trigger(SourceId, SubObjId);
}

// ---------------------------------------------------------------------------
// DestroyGroupAction
// ---------------------------------------------------------------------------

DestroyGroupAction::DestroyGroupAction()
    : Fate(0)
{
    // / 06cfcc61-06cfcc6f: CacheString::clear on +0x28, +0x2c = 0.
    GroupName.clear();
}

FuseAction* DestroyGroupAction::Slot04() const
{
    // .
    DestroyGroupAction* Copy = new DestroyGroupAction();
    Copy->Slot30(*this);
    return Copy;
}

bool DestroyGroupAction::Slot08(INI_Reader& Reader)
{
    // .
    while (Reader.read_value())
    {
        if (ReadFuseActionValue(Reader))
            continue;
        if (Reader.is_value("group_name"))
        {
            GroupName.Value = StringAlloc(Reader.get_value_string(), false);
        }
        else if (Reader.is_value("fate"))
        {
            // the value is copied into a stack TString<32> (null ->
            // empty, else clamped to 31 characters) and compared byte-exactly (REPE CMPSB over
            // the literal and its terminator). Any other string leaves Fate unchanged.
            TString<32> FateName;
            FateName = Reader.get_value_string();
            if (FateName == "debris")
                Fate = 2;
            else if (FateName == "disappear")
                Fate = 3;
        }
    }
    return true;
}

void DestroyGroupAction::Slot30(DestroyGroupAction const& Other)
{
    // (ICF with IgniteFuseAction's): CopyArchProperties, dwords +0x28 and +0x2c.
    FuseAction::CopyArchProperties(Other);
    GroupName = Other.GroupName;
    Fate = Other.Fate;
}

void DestroyGroupAction::Trigger(unsigned SourceId, unsigned short SubObjId)
{
    // , RET 8.
    if (!IsTriggered()) // CALL [EAX + 0x18]
    {
        // stricmp(GroupName, the field "random").
        if (_stricmp(GroupName.Value, GRandomGroupName) == 0)
        {
            // the routine (ceqobj, no null test), module storage LEA EBP,[+0x144].
            CArchGroupManager& Groups = Owner->ceqobj()->ArchGroups;
            int const Living = Groups.NumLiving();
            if (Living > 0)
            {
                // rand() * Living, signed divide by 0x8000.
                int const Target = (rand() * Living) / 0x8000;
                // / 06cfe522: counter = 0, traverser "at start" = 1.
                int Counter = 0;
                CArchGrpTraverser Walk;
                CArchGroup* Group = Groups.Traverse(Walk);
                while (Group != nullptr)
                {
                    // dead groups (State 2 / 3) are skipped; a living
                    // one bumps the counter BEFORE the compare, so the first living group is
                    // number 1 and a Target of 0 never matches -- the name then stays "random".
                    if (!Group->IsDead())
                    {
                        ++Counter;
                        if (Counter == Target)
                        {
                            // CArchGroup::Name, raw pointer copy into
                            // +0x28 (CacheString has no assignment operator in baseline).
                            GroupName = Group->Name();
                            break;
                        }
                    }
                    Group = Groups.Traverse(Walk);
                }
            }
            else
            {
                GroupName.clear(); // CacheString::clear on +0x28
            }
        }

        // an empty name skips the damage.
        if (GroupName.Value[0] != '\0')
        {
            // the stack DamageList (InitFuseDamageList).
            DamageList Damage;
            InitFuseDamageList(Damage, Owner, SourceId);
            // CALL [EDX + 0x1c8]: destroy_group_by_name(name, Fate, DL), result
            // unused; module storage CALL [EAX + 0x188]: apply_damage_list(DL).
            Owner->destroy_group_by_name(GroupName.Value,
                                         static_cast<DamageEntry::SubObjFate>(Fate), Damage);
            Owner->apply_damage_list(Damage);
        }
    }
    // FuseAction::Trigger (module storage) inlined.
    FuseAction::Trigger(SourceId, SubObjId);
}

// ---------------------------------------------------------------------------
// DamageGroupAction
// ---------------------------------------------------------------------------

DamageGroupAction::DamageGroupAction()
    : Hitpoints(0)
    , bCanKill(true)
{
    // / 06cfcd01-06cfcd12: CacheString::clear on +0x28, +0x2c = 0,
    // +0x30 = 1.
    GroupName.clear();
}

FuseAction* DamageGroupAction::Slot04() const
{
    // .
    DamageGroupAction* Copy = new DamageGroupAction();
    Copy->Slot30(*this);
    return Copy;
}

bool DamageGroupAction::Slot08(INI_Reader& Reader)
{
    // .
    while (Reader.read_value())
    {
        if (ReadFuseActionValue(Reader))
            continue;
        if (Reader.is_value("group_name"))
        {
            GroupName.Value = StringAlloc(Reader.get_value_string(), false);
        }
        else if (Reader.is_value("damage_type"))
        {
            // TString<32> + byte-exact compare (NOT the stricmp
            // DamageRootAction uses).
            TString<32> DamageType;
            DamageType = Reader.get_value_string();
            if (DamageType == "absolute")
                bCanKill = false;
            else
                bCanKill = true;
        }
        else if (Reader.is_value("hitpoints"))
        {
            Hitpoints = Reader.get_value_int(0);
        }
    }
    return true;
}

void DamageGroupAction::Slot30(DamageGroupAction const& Other)
{
    // CopyArchProperties, dwords +0x28 / +0x2c, byte +0x30.
    FuseAction::CopyArchProperties(Other);
    GroupName = Other.GroupName;
    Hitpoints = Other.Hitpoints;
    bCanKill = Other.bCanKill;
}

void DamageGroupAction::Trigger(unsigned SourceId, unsigned short SubObjId)
{
    // , RET 8. No "random" pick here -- the name goes out as read.
    // CALL [EAX + 0x18]        ; IsTriggered() (slot 6)
    // ; the stack DamageList (InitFuseDamageList)
    // FILD [ESI + 0x2c]        ; (float)Hitpoints
    // MOV BL,[ESI + 0x30]      ; bCanKill
    // CALL [EDX + 0x1c4]       ; inflict_group_damage(name, hp, bCanKill, DL)
    // CALL [EAX + 0x188]       ; apply_damage_list(DL)
    if (!IsTriggered())
    {
        DamageList Damage;
        InitFuseDamageList(Damage, Owner, SourceId);
        Owner->inflict_group_damage(GroupName.Value, static_cast<float>(Hitpoints), bCanKill,
                                    Damage);
        Owner->apply_damage_list(Damage);
    }
    // FuseAction::Trigger (module storage) inlined.
    FuseAction::Trigger(SourceId, SubObjId);
}

// ---------------------------------------------------------------------------
// DestroyHardpointAttachmentAction
// ---------------------------------------------------------------------------

DestroyHardpointAttachmentAction::DestroyHardpointAttachmentAction()
    : Fate(0)
{
    // / 06cfcdb1-06cfcdbf: CacheString::clear on +0x28, +0x2c = 0.
    Hardpoint.clear();
}

FuseAction* DestroyHardpointAttachmentAction::Slot04() const
{
    // .
    DestroyHardpointAttachmentAction* Copy = new DestroyHardpointAttachmentAction();
    Copy->Slot30(*this);
    return Copy;
}

bool DestroyHardpointAttachmentAction::Slot08(INI_Reader& Reader)
{
    // .
    while (Reader.read_value())
    {
        if (ReadFuseActionValue(Reader))
            continue;
        if (Reader.is_value("hardpoint"))
        {
            Hardpoint.Value = StringAlloc(Reader.get_value_string(), false);
        }
        else if (Reader.is_value("fate"))
        {
            // TString<32> + byte-exact compares; "loot" is
            // .
            TString<32> FateName;
            FateName = Reader.get_value_string();
            if (FateName == "debris")
                Fate = 2;
            else if (FateName == "disappear")
                Fate = 3;
            else if (FateName == "loot")
                Fate = 5;
        }
    }
    return true;
}

void DestroyHardpointAttachmentAction::Slot30(DestroyHardpointAttachmentAction const& Other)
{
    // (ICF): CopyArchProperties, dwords +0x28 and +0x2c.
    FuseAction::CopyArchProperties(Other);
    Hardpoint = Other.Hardpoint;
    Fate = Other.Fate;
}

void DestroyHardpointAttachmentAction::Trigger(unsigned SourceId, unsigned short SubObjId)
{
    // , RET 8.
    if (!IsTriggered()) // CALL [EAX + 0x18]
    {
        // stricmp(Hardpoint, the field "random").
        if (_stricmp(Hardpoint.Value, GRandomHardpointName) == 0)
        {
            // the routine (ceqobj, no null test); module storage ADD ESI,0xe4.
            CEquipManager& Equips = Owner->ceqobj()->EquipManager;
            // counter = 0, CEquipTraverser(0x1fe0) -- attached equips.
            int Count = 0;
            CEquipTraverser Walk(0x1fe0);
            // , first pass: count the equips whose slot 2 (+0x08,
            // IsDestroyed) says false.
            CEquip* Equip = Equips.Traverse(Walk);
            while (Equip != nullptr)
            {
                if (!Equip->IsDestroyed())
                    ++Count;
                Equip = Equips.Traverse(Walk);
            }

            if (Count != 0) // TEST EBX,EBX
            {
                // rand() * Count, signed divide by 0x8000.
                int const Target = (rand() * Count) / 0x8000;
                // counter = 0; module storage traverser +0x08 = 0 (Restart
                // inlined).
                Count = 0;
                Walk.Restart();
                // , second pass: EVERY equip bumps the counter -- the
                // destroyed ones the first pass skipped included -- before the compare, so a
                // Target of 0 never matches and the name then stays "random".
                Equip = Equips.Traverse(Walk);
                while (Equip != nullptr)
                {
                    ++Count;
                    if (Count == Target)
                    {
                        // slot +0x60 (CExternalEquip::GetParentHPName,
                        // CacheString returned through a hidden pointer), raw pointer copy into
                        // +0x28. The 0x1fe0 traverser only yields attached equips.
                        Hardpoint = static_cast<CExternalEquip*>(Equip)->GetParentHPName();
                        break;
                    }
                    Equip = Equips.Traverse(Walk);
                }
            }
            else
            {
                Hardpoint.clear(); // CacheString::clear on +0x28
            }
        }

        // an empty name skips the damage.
        if (Hardpoint.Value[0] != '\0')
        {
            // the stack DamageList (InitFuseDamageList).
            DamageList Damage;
            InitFuseDamageList(Damage, Owner, SourceId);
            // CALL [EDX + 0x1d0]: destroy_equip_by_hardpoint(hp, Fate, DL), result
            // unused; module storage CALL [EAX + 0x188]: apply_damage_list(DL).
            Owner->destroy_equip_by_hardpoint(Hardpoint.Value,
                                              static_cast<DamageEntry::SubObjFate>(Fate), Damage);
            Owner->apply_damage_list(Damage);
        }
    }
    // FuseAction::Trigger (module storage) inlined.
    FuseAction::Trigger(SourceId, SubObjId);
}

// ---------------------------------------------------------------------------
// DamageHardpointAttachmentAction
// ---------------------------------------------------------------------------

DamageHardpointAttachmentAction::DamageHardpointAttachmentAction()
    : Hitpoints(0)
    , bCanKill(true)
{
    // / 06cfce51-06cfce62: CacheString::clear on +0x28, +0x2c = 0,
    // +0x30 = 1.
    Hardpoint.clear();
}

FuseAction* DamageHardpointAttachmentAction::Slot04() const
{
    // .
    DamageHardpointAttachmentAction* Copy = new DamageHardpointAttachmentAction();
    Copy->Slot30(*this);
    return Copy;
}

bool DamageHardpointAttachmentAction::Slot08(INI_Reader& Reader)
{
    // .
    while (Reader.read_value())
    {
        if (ReadFuseActionValue(Reader))
            continue;
        if (Reader.is_value("hardpoint"))
        {
            Hardpoint.Value = StringAlloc(Reader.get_value_string(), false);
        }
        else if (Reader.is_value("damage_type"))
        {
            // TString<32> + byte-exact compare.
            TString<32> DamageType;
            DamageType = Reader.get_value_string();
            if (DamageType == "absolute")
                bCanKill = false;
            else
                bCanKill = true;
        }
        else if (Reader.is_value("hitpoints"))
        {
            Hitpoints = Reader.get_value_int(0);
        }
    }
    return true;
}

void DamageHardpointAttachmentAction::Slot30(DamageHardpointAttachmentAction const& Other)
{
    // (ICF with DamageGroupAction's): CopyArchProperties, +0x28, +0x2c, +0x30.
    FuseAction::CopyArchProperties(Other);
    Hardpoint = Other.Hardpoint;
    Hitpoints = Other.Hitpoints;
    bCanKill = Other.bCanKill;
}

void DamageHardpointAttachmentAction::Trigger(unsigned SourceId, unsigned short SubObjId)
{
    // , RET 8 -- byte-for-byte DamageGroupAction's body (the branch) except the
    // owner slot: +0x1cc instead of +0x1c4 (module storage). No "random" pick.
    // FILD [ESI + 0x2c]        ; (float)Hitpoints
    // MOV BL,[ESI + 0x30]      ; bCanKill
    // CALL [EDX + 0x1cc]       ; inflict_equip_damage(hp name, hp, bCanKill, DL)
    // CALL [EAX + 0x188]       ; apply_damage_list(DL)
    if (!IsTriggered())
    {
        DamageList Damage;
        InitFuseDamageList(Damage, Owner, SourceId);
        Owner->inflict_equip_damage(Hardpoint.Value, static_cast<float>(Hitpoints), bCanKill,
                                    Damage);
        Owner->apply_damage_list(Damage);
    }
    // FuseAction::Trigger (module storage) inlined.
    FuseAction::Trigger(SourceId, SubObjId);
}

// ---------------------------------------------------------------------------
// ImpulseAction
// ---------------------------------------------------------------------------

ImpulseAction::ImpulseAction()
    : Force(0.0f)
    , Radius(0.0f)
    , Damage(0.0f)
{
    // / 06cfcfc1-06cfcfde: CacheString::clear on +0x28, +0x2c/+0x30/
    // +0x34 = 0, Vector(0, 0, 0) at +0x38 (the routine in the reader, three stores in the
    // clone).
    Hardpoint.clear();
    PosOffset.x = 0.0f;
    PosOffset.y = 0.0f;
    PosOffset.z = 0.0f;
}

FuseAction* ImpulseAction::Slot04() const
{
    // .
    ImpulseAction* Copy = new ImpulseAction();
    Copy->Slot30(*this);
    return Copy;
}

bool ImpulseAction::Slot08(INI_Reader& Reader)
{
    // .
    while (Reader.read_value())
    {
        if (ReadFuseActionValue(Reader))
            continue;
        if (Reader.is_value("hardpoint"))
        {
            Hardpoint.Value = StringAlloc(Reader.get_value_string(), false);
        }
        else if (Reader.is_value("force"))
        {
            Force = (float)Reader.get_value_float(0);
        }
        else if (Reader.is_value("radius"))
        {
            Radius = (float)Reader.get_value_float(0);
        }
        else if (Reader.is_value("damage"))
        {
            Damage = (float)Reader.get_value_float(0);
        }
        else if (Reader.is_value("pos_offset"))
        {
            // values read 0, 1, 2 into temporaries, then stored.
            float const X = (float)Reader.get_value_float(0);
            float const Y = (float)Reader.get_value_float(1);
            float const Z = (float)Reader.get_value_float(2);
            PosOffset.x = X;
            PosOffset.y = Y;
            PosOffset.z = Z;
        }
    }
    return true;
}

void ImpulseAction::Slot30(ImpulseAction const& Other)
{
    // CopyArchProperties, then +0x28 .. +0x40 -- the hardpoint IS copied here
    // (unlike the exe-side impulse action class).
    FuseAction::CopyArchProperties(Other);
    Hardpoint = Other.Hardpoint;
    Force = Other.Force;
    Radius = Other.Radius;
    Damage = Other.Damage;
    PosOffset = Other.PosOffset;
}

void ImpulseAction::Trigger(unsigned SourceId, unsigned short SubObjId)
{
    // (vftable module storage +0x1c, RET 8). Every early-out jumps to the inlined
    // base tail the branch. SourceId is not read by the body -- the blast is the owner's own.
    if (!IsTriggered()) // CALL [EAX + 0x18]
    {
        // the routine on the owner's id (IObjInspect slot 8); the
        // lookup failing or the StarSystem out-param being null bails. The object out-param
        // ([ESP+0x4c]) is never read.
        unsigned int OwnerId = Owner->get_id();
        GameObject* Found = nullptr;
        StarSystem* System = nullptr;
        if (Server_FindObject(OwnerId, Found, System) && System != nullptr)
        {
            // FindHardpoint_PS(owner get_index (slot 7), the Hardpoint
            // string, handle out preset to -1, info out); false bails.
            long HpHandle = -1;
            HardpointInfo HpInfo;
            if (FindHardpoint_PS(Owner->get_index(), Hardpoint.Value, HpHandle, HpInfo))
            {
                // HardpointToWorld(get_index() again, info).
                HardpointToWorld(Owner->get_index(), HpInfo);

                // the field, lazily the routine, then
                // I3DMathEngine +0x38 (engine, &Offset, &HpInfo.Orient (+0x10), &PosOffset).
                if (GFuseMathEngine == nullptr)
                    GFuseMathEngine = Server_Acquire3DMathEngine();
                FVector Offset;
                GFuseMathEngine->TransposeTransform(Offset,
                                                    reinterpret_cast<FMatrix const&>(HpInfo.Orient),
                                                    reinterpret_cast<FVector const&>(PosOffset));

                // Offset + HpInfo.Position (+0x04), component-wise
                // (z, y, x evaluated; stored x, y, z). A float sum rounded from the 53-bit x87
                // register equals the plain float sum.
                Vector BlastPos;
                BlastPos.x = static_cast<float>(static_cast<double>(Offset.X) + HpInfo.Position.x);
                BlastPos.y = static_cast<float>(static_cast<double>(Offset.Y) + HpInfo.Position.y);
                BlastPos.z = static_cast<float>(static_cast<double>(Offset.Z) + HpInfo.Position.z);

                // Archetype::Explosion(ID_String{CreateID("FuseImpulse")})
                // on the stack, then Radius (+0x04) = +0x30, Impulse (+0x08) = Force (+0x2c),
                // HullDamage (+0x0c) = Damage (+0x34).
                Archetype::Explosion Blast(ID_String{CreateID("FuseImpulse")});
                Blast.Radius = Radius;
                Blast.Impulse = Force;
                Blast.HullDamage = Damage;

                // the event at [ESP+0x2c]. baseline zeroes both ids,
                // stores cause 4 and zeroes +0x18/+0x1c, calls get_id (slot 8) once more, then
                // writes both ids, the position, cause 0x15 (over the 4) and the explosion.
                ExplosionDamageEvent Event;
                Event.ExplodingObjId = 0;
                Event.InflictorId = 0;
                Event.Cause = static_cast<DamageCause>(4);
                Event.Explosion = nullptr;
                Event.SkipCollisionGroup = nullptr;
                unsigned int const BlastOwnerId = Owner->get_id();
                Event.ExplodingObjId = BlastOwnerId;
                Event.InflictorId = BlastOwnerId;
                Event.Position = BlastPos;
                Event.Cause = static_cast<DamageCause>(0x15);
                Event.Explosion = &Blast;

                // the routine on the StarSystem the lookup returned.
                System->explode(Event);
                // ~Explosion inlined (the two vectors and the effect
                // list, the routine) -- the scope exit of Blast.
            }
        }
    }
    // FuseAction::Trigger (module storage) inlined.
    FuseAction::Trigger(SourceId, SubObjId);
}

// ---------------------------------------------------------------------------
// TumbleAction
// ---------------------------------------------------------------------------

TumbleAction::TumbleAction()
    : AngDragScale(1.0f)
    , ThrottleLow(0.0f)
    , ThrottleHigh(0.0f)
    , bEngineKill(false)
{
    // / 06cfd049-06cfd06b: +0x28 = 1.0f, Vector(0, 0, 0) at +0x2c,
    // Vector(1, 1, 1) at +0x38, +0x4c = 0, +0x48 = 0, +0x44 = 0.
    TurnThrottleLow.x = 0.0f;
    TurnThrottleLow.y = 0.0f;
    TurnThrottleLow.z = 0.0f;
    TurnThrottleHigh.x = 1.0f;
    TurnThrottleHigh.y = 1.0f;
    TurnThrottleHigh.z = 1.0f;
}

FuseAction* TumbleAction::Slot04() const
{
    // .
    TumbleAction* Copy = new TumbleAction();
    Copy->Slot30(*this);
    return Copy;
}

bool TumbleAction::Slot08(INI_Reader& Reader)
{
    // .
    while (Reader.read_value())
    {
        if (ReadFuseActionValue(Reader))
            continue;
        if (Reader.is_value("ang_drag_scale"))
        {
            AngDragScale = (float)Reader.get_value_float(0);
        }
        else if (Reader.is_value("turn_throttle_x"))
        {
            TurnThrottleLow.x = (float)Reader.get_value_float(0);
            TurnThrottleHigh.x = (float)Reader.get_value_float(1);
        }
        else if (Reader.is_value("turn_throttle_y"))
        {
            TurnThrottleLow.y = (float)Reader.get_value_float(0);
            TurnThrottleHigh.y = (float)Reader.get_value_float(1);
        }
        else if (Reader.is_value("turn_throttle_z"))
        {
            TurnThrottleLow.z = (float)Reader.get_value_float(0);
            TurnThrottleHigh.z = (float)Reader.get_value_float(1);
        }
        else if (Reader.is_value("throttle"))
        {
            ThrottleLow = (float)Reader.get_value_float(0);
            ThrottleHigh = (float)Reader.get_value_float(1);
        }
        else if (Reader.is_value("engine_kill"))
        {
            bEngineKill = Reader.get_value_bool(0);
        }
    }
    return true;
}

void TumbleAction::Slot30(TumbleAction const& Other)
{
    // CopyArchProperties, +0x28, +0x44, +0x48 and +0x4c ONLY. The two turn
    // throttle vectors (+0x2c .. +0x40) are NOT copied, so a clone keeps the constructor's
    // (0,0,0) / (1,1,1) -- baseline.
    FuseAction::CopyArchProperties(Other);
    AngDragScale = Other.AngDragScale;
    ThrottleLow = Other.ThrottleLow;
    ThrottleHigh = Other.ThrottleHigh;
    bEngineKill = Other.bEngineKill;
}

void TumbleAction::Trigger(unsigned SourceId, unsigned short SubObjId)
{
    // , RET 8. Every early-out below jumps to the same inlined base tail.
    // The `TEST ECX,ECX` / `LEA EAX,[ECX+0xc]; TEST EAX,EAX` pairs at module storage-06cff706 and
    // are the compiler's null-preserving conversion of Owner to its CBase
    // sub-object; Owner has already been dereferenced at module storage by then, so they are not
    // guards and have no counterpart here.
    if (!IsTriggered()) // CALL [EAX + 0x18]
    {
        // the owner's CSimple (+0x10 under the `& 3 == 3` guard, no null
        // test on the result) must have no owner player (+0xb4) -- players never tumble.
        if (Owner->csimple()->OwnerPlayer == 0)
        {
            // the object must be a CShip ((ObjectClass & 0x503) ==
            // 0x503). cobj()'s `& 3 == 3` guard is implied by that mask.
            CObject* Object = Owner->cobj();
            if (Object != nullptr &&
                (static_cast<unsigned>(Object->ObjectClass) & 0x503u) == 0x503u)
            {
                // the routine (ceqobj); module storage CEqObj::
                // get_behavior_interface -- a ship driven by a behaviour manager is left alone.
                CEqObj* Cq = Owner->ceqobj();
                if (Cq->get_behavior_interface() == nullptr)
                {
                    // the same CShip cast again, then
                    // CShip+0x2c0 = AngDragScale.
                    CShip* Ship = static_cast<CShip*>(Owner->cobj());
                    Ship->AngularDragFactor = AngDragScale;

                    // three random turn throttles in x, y, z order.
                    FVector Axis;
                    Axis.X = RandomBetween(rand(), TurnThrottleLow.x, TurnThrottleHigh.x);
                    Axis.Y = RandomBetween(rand(), TurnThrottleLow.y, TurnThrottleHigh.y);
                    Axis.Z = RandomBetween(rand(), TurnThrottleLow.z, TurnThrottleHigh.z);

                    // CALL [EDX + 0x10]: IObjInspect::get_orientation.
                    Matrix const& Orientation = Owner->get_orientation();
                    // the field, lazily the routine, then
                    // I3DMathEngine +0x30 Rotate(engine, &Rotated, orientation, &Axis).
                    if (GFuseMathEngine == nullptr)
                        GFuseMathEngine = Server_Acquire3DMathEngine();
                    FVector Rotated;
                    GFuseMathEngine->Rotate(Rotated,
                                            reinterpret_cast<FTransform const&>(Orientation),
                                            Axis);

                    // copied into a Vector, IObjAffect (Owner+4) slot
                    // 1, set_axis_throttle. Result unused.
                    Vector AxisThrottle;
                    AxisThrottle.x = Rotated.X;
                    AxisThrottle.y = Rotated.Y;
                    AxisThrottle.z = Rotated.Z;
                    static_cast<IObjAffect*>(Owner)->set_axis_throttle(AxisThrottle);

                    // a random throttle, IObjAffect slot 0,
                    // set_throttle. Result unused.
                    float const Throttle = RandomBetween(rand(), ThrottleLow, ThrottleHigh);
                    static_cast<IObjAffect*>(Owner)->set_throttle(Throttle);

                    // engine_kill -- the first engine (type mask
                    // 0x20000) of the CEqObj, no null test; its sub-object id (word +0x08)
                    // zero-extended into a dword local and passed by address to IObjAffect
                    // slot 19 (+0x4c) with (false, 0): deactivate. Result unused.
                    if (bEngineKill)
                    {
                        unsigned int EngineId = Cq->EquipManager.FindFirst(0x20000u)->Id;
                        unsigned short const* EngineIdPtr =
                            reinterpret_cast<unsigned short const*>(&EngineId);
                        static_cast<IObjAffect*>(Owner)->slot_4c(EngineIdPtr, false, 0);
                    }
                }
            }
        }
    }
    // FuseAction::Trigger (module storage) inlined.
    FuseAction::Trigger(SourceId, SubObjId);
}

// ---------------------------------------------------------------------------
// MakeInvincibleAction
// ---------------------------------------------------------------------------

MakeInvincibleAction::MakeInvincibleAction()
    : bTurnOn(true)
{
    // / 06cfd0db: +0x28 = 1.
}

FuseAction* MakeInvincibleAction::Slot04() const
{
    // .
    MakeInvincibleAction* Copy = new MakeInvincibleAction();
    Copy->Slot30(*this);
    return Copy;
}

bool MakeInvincibleAction::Slot08(INI_Reader& Reader)
{
    // .
    while (Reader.read_value())
    {
        if (ReadFuseActionValue(Reader))
            continue;
        if (Reader.is_value("turn_on"))
            bTurnOn = Reader.get_value_bool(0);
    }
    return true;
}

void MakeInvincibleAction::Slot30(MakeInvincibleAction const& Other)
{
    // CopyArchProperties, byte +0x28.
    FuseAction::CopyArchProperties(Other);
    bTurnOn = Other.bTurnOn;
}

void MakeInvincibleAction::Trigger(unsigned SourceId, unsigned short SubObjId)
{
    // , RET 8.
    // CALL [EAX + 0x18]        ; IsTriggered() (slot 6)
    // [ESP+4]  = 4             ; FInvincibility::Type (the list key)
    // [ESP+8]  = bTurnOn       ; bProtect
    // [ESP+9]  = bTurnOn       ; bFromPlayers
    // [ESP+0xc] = 0.0f, 1.0f when !bTurnOn (module storage) ; MaxHpLoss
    // CALL [EDX + 0x194]       ; Owner->set_invincible(&block)
    // Bytes +0x0a/+0x0b of the block are never written by baseline (stack residue); zeroed here.
    if (!IsTriggered())
    {
        FInvincibility Command;
        Command.Type = 4;
        Command.bProtect = bTurnOn;
        Command.bFromPlayers = bTurnOn;
        Command.Pad06[0] = 0;
        Command.Pad06[1] = 0;
        Command.MaxHpLoss = bTurnOn ? 0.0f : 1.0f;
        Owner->set_invincible(Command);
    }
    // FuseAction::Trigger (module storage) inlined.
    FuseAction::Trigger(SourceId, SubObjId);
}

// ---------------------------------------------------------------------------
// DumpCargoAction
// ---------------------------------------------------------------------------

DumpCargoAction::DumpCargoAction()
{
    // / 06cfd170-06cfd178: CacheString::clear on +0x28.
    OriginHardpoint.clear();
}

FuseAction* DumpCargoAction::Slot04() const
{
    // .
    DumpCargoAction* Copy = new DumpCargoAction();
    Copy->Slot30(*this);
    return Copy;
}

bool DumpCargoAction::Slot08(INI_Reader& Reader)
{
    // .
    while (Reader.read_value())
    {
        if (ReadFuseActionValue(Reader))
            continue;
        if (Reader.is_value("origin_hardpoint"))
            OriginHardpoint.Value = StringAlloc(Reader.get_value_string(0), false);
    }
    return true;
}

void DumpCargoAction::Slot30(DumpCargoAction const& Other)
{
    // CopyArchProperties, dword +0x28.
    FuseAction::CopyArchProperties(Other);
    OriginHardpoint = Other.OriginHardpoint;
}

void DumpCargoAction::Trigger(unsigned SourceId, unsigned short SubObjId)
{
    // , RET 8.
    // CALL [EAX + 0x18]        ; IsTriggered() (slot 6)
    // ; the stack DamageList (InitFuseDamageList)
    // CALL [EDX + 0x1d4]       ; dump_cargo(OriginHardpoint, DL), result unused
    // CALL [EAX + 0x188]       ; apply_damage_list(DL)
    if (!IsTriggered())
    {
        DamageList Damage;
        InitFuseDamageList(Damage, Owner, SourceId);
        Owner->dump_cargo(OriginHardpoint.Value, Damage);
        Owner->apply_damage_list(Damage);
    }
    // FuseAction::Trigger (module storage) inlined.
    FuseAction::Trigger(SourceId, SubObjId);
}

// ---------------------------------------------------------------------------
// "random" -- the branch / the branch, CRT dynamic initializers:
// `PUSH 0; PUSH "random"; CALL StringAlloc; MOV [DAT], EAX`.
// ---------------------------------------------------------------------------

char const* GRandomGroupName = StringAlloc("random", false);
char const* GRandomHardpointName = StringAlloc("random", false);

// ---------------------------------------------------------------------------
// ServerFuseDB
// ---------------------------------------------------------------------------

// . Its dynamic initializer (the branch -> the routine tree ctor, atexit
// tree dtor guarded by bit 0 of the field) is the std::map's own construction
// and destruction; neither deletes the ServerFuse values.
std::map<unsigned, ServerFuse*> ServerFuseDB::Fuses;

std::map<unsigned, ServerFuse*>& ServerFuseDB::Get()
{
    // `MOV EAX,the field`.
    return Fuses;
}

void ServerFuseDB::Clear()
{
    // virtual-delete (vt[0](1)) every non-null prototype in key order, then erase
    // all nodes (the whole-tree fast path when begin() is the leftmost node, node by node
    // otherwise -- same result).
    for (std::map<unsigned, ServerFuse*>::iterator It = Fuses.begin(); It != Fuses.end(); ++It)
    {
        if (It->second != nullptr)
            delete It->second;
    }
    Fuses.clear();
}

void ServerFuseDB::LoadAll()
{
    // one the routine per path in FuseDB::m_FuseINIFiles (node+0xc = the
    // TString's characters).
    for (std::list<TString<260>>::iterator It = m_FuseINIFiles.begin(); It != m_FuseINIFiles.end(); ++It)
        LoadFile(It->Data);
}

void ServerFuseDB::LoadFile(char const* Path)
{
    // .
    INI_Reader Reader;
    if (!Reader.open(Path, false))
        return;
    Reader.read_header(); // result unused (module storage)
    while (!Reader.is_end())
    {
        if (Reader.is_header("fuse"))
        {
            ServerFuse* NewFuse = new ServerFuse(ID_String{CreateID("")}, -1.0f);
            // . A failed Read, or a nickname already in the map (the routine
            // lower_bound + key compare, the routine hinted insert), leaks NewFuse (baseline).
            if (NewFuse->Read(Reader))
                Fuses.insert(std::map<unsigned, ServerFuse*>::value_type(NewFuse->Name().Id, NewFuse));
        }
        else
        {
            Reader.read_header();
        }
    }
}
