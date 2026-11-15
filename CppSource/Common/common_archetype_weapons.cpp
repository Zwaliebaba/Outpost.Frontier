// Author: Giperion.

// Archetype::Launcher, Gun, MineDropper, CounterMeasureDropper, Projectile, Munition, Mine,
// CounterMeasure -- split out of common_archetype.cpp (baseline Archetype.cpp, one TU) in CMN-SPLIT
// phase B.

#include "common_archetype.h"
#include "common_archetype_internal.h"

#include "common_cobject.h" // CollisionGroupDesc (get_undamaged_collision_group_list)
#include "common_fuses.h"
#include "common_ini.h"
#include "common_misc.h"
#include "common_phyarch.h"
#include "common_universe.h"
#include "NeuronRuntime.h"
#include "neuroncomponents.h"
#include "readfile.h"
#include "x87_trig.h" // X87CosF (Munition::read seeker_fov_deg, 062f723c FCOS; FSTP float)

#include <cstddef>
#include <list>
#include <map>
#include <math.h>
#include <memory>
#include <stdlib.h>
#include <string.h>
#include <vector>

namespace Archetype
{
    namespace
    {
        unsigned LookupHpType(char const* Name)
        {
            unsigned Value = 4;
            if (TryLookupHpType(Name, &Value))
                return Value;
            return 4;
        }
    }

    // Common 0x062f6f90: `EAX = [this+0x7c]; if (EAX == 0) EAX = this`.
    Equipment* Projectile::get_loot_appearance()
    {
        if (LootAppearance != nullptr)
            return LootAppearance;
        return this;
    }

    Launcher::Launcher(ICliObj* InCompanion)
        : AttachedEquipment(InCompanion)
        , DamagePerFire(0.0f)
        , PowerUsage(0.0f)
        , RefireDelay(0.0f)
        , MuzzleVelocity(0.0f)
    {
        ProjectileArchetype.clear();
        UseAnimation.clear();
    }

    bool Launcher::read(INI_Reader& Reader)
    {
        if (AttachedEquipment::read(Reader))
            return true;

        if (Reader.is_value("damage_per_fire"))
        {
            DamagePerFire = Reader.get_value_float(0);
            return true;
        }

        if (Reader.is_value("power_usage"))
        {
            PowerUsage = Reader.get_value_float(0);
            return true;
        }

        if (Reader.is_value("refire_delay"))
        {
            RefireDelay = Reader.get_value_float(0);
            return true;
        }

        if (Reader.is_value("muzzle_velocity"))
        {
            MuzzleVelocity = Reader.get_value_float(0);
            return true;
        }

        if (Reader.is_value("projectile_archetype"))
        {
            ProjectileArchetype.Id = CreateID(Reader.get_value_string());
            return true;
        }

        if (Reader.is_value("use_animation"))
        {
            UseAnimation.Value = StringAlloc(Reader.get_value_string(), false);
            return true;
        }

        return false;
    }

    void Launcher::redefine(Root const& Other)
    {
        AttachedEquipment::redefine(Other);
        Launcher const* Donor = (Launcher const*)&Other;
        if (Donor->PowerUsage != PowerUsage)
            PowerUsage = Donor->PowerUsage;
        if (Donor->MuzzleVelocity != MuzzleVelocity)
            MuzzleVelocity = Donor->MuzzleVelocity;
        if (Donor->ProjectileArchetype.Id != ProjectileArchetype.Id)
            ProjectileArchetype.Id = Donor->ProjectileArchetype.Id;
        if (Donor->RefireDelay != RefireDelay)
            RefireDelay = Donor->RefireDelay;
    }

    AClassType Launcher::get_class_type() const
    {
        return AClassLauncher;
    }

    // Root::operator=, the Equipment shape +0x5c..+0x6c, then dwords
    // +0x70..+0x9c (the AttachedEquipment six, then Launcher's six) inline. No self test.
    Launcher& Launcher::operator=(Launcher const& Other)
    {
        AttachedEquipment::operator=(Other);
        DamagePerFire = Other.DamagePerFire;
        PowerUsage = Other.PowerUsage;
        RefireDelay = Other.RefireDelay;
        MuzzleVelocity = Other.MuzzleVelocity;
        ProjectileArchetype = Other.ProjectileArchetype;
        UseAnimation = Other.UseAnimation;
        return *this;
    }

    Gun::Gun(ICliObj* InCompanion)
        : Launcher(InCompanion)
        , DispersionAngle(0.0f)
        , TurnRate(0.2617994f)  // stores 0x3e860a92 (15 degrees in radians)
        , AzimuthMin(0.0f)
        , AzimuthMax(0.0f)
        , ElevationMin(0.0f)
        , ElevationMax(0.0f)
        , bAzimuthValid(false)
        , bElevationValid(false)
        , bAutoTurret(false)
        , PadBb(0)
        , HpGunTypesBegin(nullptr)
        , HpGunTypesEnd(nullptr)
        , HpGunTypesCap(nullptr)
        , bRangeCached(false)
        , CachedMunitionRange(0.0f)
    {
        memset(PadBc, 0, sizeof(PadBc));
        PadCd[0] = 0;
        PadCd[1] = 0;
        PadCd[2] = 0;
    }

    Gun::Gun(Gun const& Other)
        : Launcher(Other)
        , DispersionAngle(Other.DispersionAngle)
        , TurnRate(Other.TurnRate)
        , AzimuthMin(Other.AzimuthMin)
        , AzimuthMax(Other.AzimuthMax)
        , ElevationMin(Other.ElevationMin)
        , ElevationMax(Other.ElevationMax)
        , bAzimuthValid(Other.bAzimuthValid)
        , bElevationValid(Other.bElevationValid)
        , bAutoTurret(Other.bAutoTurret)
        , PadBb(0)
        , bRangeCached(Other.bRangeCached)
        , CachedMunitionRange(Other.CachedMunitionRange)
    {
        // Root::Root(Other), +0x5c..+0xb4 and the bytes +0xb8/+0xb9/+0xba
        // copied (+0xbb is not), the allocator byte at +0xbc copied, the array copied, then the
        // byte +0xcc and +0xd0.
        memset(PadBc, 0, sizeof(PadBc));
        PadBc[0] = Other.PadBc[0];
        CopyConstructHpTypes(HpGunTypesBegin, HpGunTypesEnd, HpGunTypesCap,
                             Other.HpGunTypesBegin, Other.HpGunTypesEnd);
        PadCd[0] = 0;
        PadCd[1] = 0;
        PadCd[2] = 0;
    }

    Gun& Gun::operator=(Gun const& Other)
    {
        // Root::operator=, then +0x5c..+0xb4 and the bytes +0xb8..+0xba in
        // order (no self test), the array unless it is the same vector (module storage; the
        // allocator byte at +0xbc is not copied), then the byte +0xcc and +0xd0.
        Equipment::operator=(Other);
        SeparationExplosion = Other.SeparationExplosion;
        DebrisType = Other.DebrisType;
        ChildImpulse = Other.ChildImpulse;
        ParentImpulse = Other.ParentImpulse;
        HpChild = Other.HpChild;
        Toughness = Other.Toughness;
        DamagePerFire = Other.DamagePerFire;
        PowerUsage = Other.PowerUsage;
        RefireDelay = Other.RefireDelay;
        MuzzleVelocity = Other.MuzzleVelocity;
        ProjectileArchetype = Other.ProjectileArchetype;
        UseAnimation = Other.UseAnimation;
        DispersionAngle = Other.DispersionAngle;
        TurnRate = Other.TurnRate;
        AzimuthMin = Other.AzimuthMin;
        AzimuthMax = Other.AzimuthMax;
        ElevationMin = Other.ElevationMin;
        ElevationMax = Other.ElevationMax;
        bAzimuthValid = Other.bAzimuthValid;
        bElevationValid = Other.bElevationValid;
        bAutoTurret = Other.bAutoTurret;
        if (this != &Other)
        {
            AssignHpTypes(HpGunTypesBegin, HpGunTypesEnd, HpGunTypesCap,
                          Other.HpGunTypesBegin, Other.HpGunTypesEnd);
        }
        bRangeCached = Other.bRangeCached;
        CachedMunitionRange = Other.CachedMunitionRange;
        return *this;
    }

    Gun::~Gun()
    {
        // Common 0xee50: free the dynamic array living at +0xc0 before Root's base
        // destructor runs.
        free(HpGunTypesBegin);
        HpGunTypesBegin = nullptr;
        HpGunTypesEnd = nullptr;
        HpGunTypesCap = nullptr;
    }

    namespace
    {
        // Common 0x96550 hp_gun_type push. VC6 vector<unsigned> push_back: grow by
        // doubling (1 if empty/size<2) when full, else append in place. Mirrors
        // PushShieldHpType for ShieldGenerator::HpShieldTypes* above.
        void PushGunHpType(Gun& Weapon, unsigned Value)
        {
            if (Weapon.HpGunTypesEnd == Weapon.HpGunTypesCap)
            {
                unsigned OldSize = Weapon.HpGunTypesBegin != nullptr
                                       ? (unsigned)(Weapon.HpGunTypesEnd - Weapon.HpGunTypesBegin)
                                       : 0;
                unsigned Growth = (Weapon.HpGunTypesBegin == nullptr || OldSize < 2) ? 1 : OldSize;
                unsigned NewCount = OldSize + Growth;
                unsigned* NewBuf = (unsigned*)malloc(NewCount * sizeof(unsigned));
                if (Weapon.HpGunTypesBegin != nullptr)
                    memcpy(NewBuf, Weapon.HpGunTypesBegin, OldSize * sizeof(unsigned));
                NewBuf[OldSize] = Value;
                free(Weapon.HpGunTypesBegin);
                Weapon.HpGunTypesBegin = NewBuf;
                Weapon.HpGunTypesEnd = NewBuf + OldSize + 1;
                Weapon.HpGunTypesCap = NewBuf + NewCount;
            }
            else
            {
                *Weapon.HpGunTypesEnd = Value;
                Weapon.HpGunTypesEnd += 1;
            }
        }
    }

    bool Gun::read(INI_Reader& Reader)
    {
        if (Launcher::read(Reader))
            return true;

        if (Reader.is_value("dispersion_angle"))
        {
            // FMUL float [the field] on the unrounded get_value_float result
            // (value_num = the same read without the float rounding), one FSTP float.
            DispersionAngle = (float)(Reader.value_num(0) * (double)0.017453292f);
            return true;
        }

        if (Reader.is_value("auto_turret"))
        {
            bAutoTurret = Reader.get_value_bool(0);
            return true;
        }

        if (Reader.is_value("turn_rate"))
        {
            // same as dispersion_angle.
            TurnRate = (float)(Reader.value_num(0) * (double)0.017453292f);
            return true;
        }

        if (Reader.is_value("gun_azimuth"))
        {
            // the difference stays on the x87 stack and is compared with 0.0f (TEST AH,5 / JNP: below -> leave), 360.0
            // (TEST AH,0x41 / JZ: above -> leave) and 1.0f (below -> leave); anything else,
            // unordered included, sets the flag.
            // The max is stored with FST (rounded) but the FSUB float [min] runs on the
            // unrounded value still in ST0 (value_num = get_value_float without rounding).
            AzimuthMin = Reader.get_value_float(0);
            double const MaxRaw = Reader.value_num(1);
            AzimuthMax = (float)MaxRaw;
            double Delta = MaxRaw - AzimuthMin;
            if (!(Delta < 0.0) && !(Delta > 360.0) && !(Delta < 1.0))
                bAzimuthValid = true;
            return true;
        }

        if (Reader.is_value("gun_elevation"))
        {
            // same three tests as gun_azimuth.
            ElevationMin = Reader.get_value_float(0);
            double const MaxRaw = Reader.value_num(1);
            ElevationMax = (float)MaxRaw;
            double Delta = MaxRaw - ElevationMin;
            if (!(Delta < 0.0) && !(Delta > 360.0) && !(Delta < 1.0))
                bElevationValid = true;
            return true;
        }

        if (Reader.is_value("hp_gun_type"))
        {
            unsigned Index = 0;
            while (!Reader.is_value_empty(Index))
            {
                unsigned HpType = 0;
                if (TryLookupHpType(Reader.get_value_string(Index), &HpType))
                    PushGunHpType(*this, HpType);
                Index += 1;
            }
            return true;
        }

        return false;
    }

    void Gun::redefine(Root const& Other)
    {
        Launcher::redefine(Other);
        Gun const* Donor = (Gun const*)&Other;
        if (Donor->DispersionAngle != DispersionAngle)
            DispersionAngle = Donor->DispersionAngle;
        if (Donor->TurnRate != TurnRate)
            TurnRate = Donor->TurnRate;
    }

    // baseline 0x967f0. Turret guns report HpTypeTurret; otherwise, if a projectile archetype
    // is linked, look it up in GEquipmentDb and return the linked Munition's own HpType field
    // (+0xa8) -- this is a direct field read, not a vcall to Munition::get_hp_type(). Compiler
    // inlined ArchDB::Get's own FDUMP-on-miss (line 0x3c matches ArchDB::Get's error site).
    HpAttachmentType Gun::get_hp_type() const
    {
        if (bAutoTurret)
            return HpTypeTurret;
        if (ProjectileArchetype.IsEmpty())
            return HpTypeNone;
        // ArchDB::Get(unsigned) inlined (its miss dump at 0x3c). A hit reads
        // record->+0xa8 raw; a miss does XOR EAX,EAX / MOV EAX,[EAX+0xa8] (062f6877) -- the
        // same read through null, a deterministic fault, reproduced here (no null test).
        Equipment* Found = GEquipmentDb.Get(ProjectileArchetype.Id);
        return (HpAttachmentType)((Munition*)Found)->HpType;
    }

    HpAttachmentType Gun::get_hp_type_by_index(int Index) const
    {
        // — 24 dwords, Commonecc38. Empty list only.
        static HpAttachmentType const kEmptyGunHpTypes[24] = {
            HpTypeGun,            HpTypeGunSpecial1,    HpTypeGunSpecial2,    HpTypeGunSpecial3,
            HpTypeGunSpecial4,    HpTypeGunSpecial5,    HpTypeGunSpecial6,    HpTypeGunSpecial7,
            HpTypeGunSpecial8,    HpTypeGunSpecial9,    HpTypeGunSpecial10,   HpTypeTurretSpecial1,
            HpTypeTurretSpecial2, HpTypeTurretSpecial3, HpTypeTurretSpecial4, HpTypeTurretSpecial5,
            HpTypeTurretSpecial6, HpTypeTurretSpecial7, HpTypeTurretSpecial8, HpTypeTurretSpecial9,
            HpTypeTurretSpecial10, HpTypeTorpedoSpecial1, HpTypeTorpedoSpecial2,
            HpTypeNone};

        unsigned Count = (HpGunTypesBegin != nullptr)
                             ? (unsigned)(HpGunTypesEnd - HpGunTypesBegin)
                             : 0;
        if (Count == 0)
        {
            if (Index >= 0 && Index < 24)
                return kEmptyGunHpTypes[Index];
            return HpTypeNone;
        }
        // baseline UB (owner rule: deterministic in the port): 062f68f8 `CMP EDX,EAX; JG` accepts
        // Index == Count and reads the dword just past the array's end. The port answers 0 for
        // that index, as for every other out-of-range one; every in-range index is identical.
        if (Index >= 0 && (unsigned)Index < Count)
            return (HpAttachmentType)HpGunTypesBegin[Index];
        return HpTypeNone;
    }

    AClassType Gun::get_class_type() const
    {
        return AClassGun;
    }

    MineDropper::MineDropper(ICliObj* InCompanion)
        : Launcher(InCompanion)
    {
    }

    AClassType MineDropper::get_class_type() const
    {
        return AClassMineDropper;
    }

    // baseline 0xd450, folded with Power::get_class_type.
    HpAttachmentType MineDropper::get_hp_type() const
    {
        return HpTypeMineDropper;
    }

    CounterMeasureDropper::CounterMeasureDropper(ICliObj* InCompanion)
        : Launcher(InCompanion)
        , AiRangeSq(0.0f)
    {
    }

    // Common 0x96cd0.
    bool CounterMeasureDropper::read(INI_Reader& Reader)
    {
        if (Launcher::read(Reader))
            return true;

        if (!Reader.is_value("ai_range"))
            return false;

        // FLD ST0 / FMUL ST1 square the unrounded get_value_float result, then FSTP float.
        // value_num is the same read without the float rounding (text atof / BINI int are not
        // float-exact).
        double Range = Reader.value_num(0);
        AiRangeSq = (float)(Range * Range);
        return true;
    }

    // Common 0x96d20.
    void CounterMeasureDropper::redefine(Root const& Other)
    {
        Launcher::redefine(Other);
        CounterMeasureDropper const* Donor = (CounterMeasureDropper const*)&Other;
        if (Donor->AiRangeSq != AiRangeSq)
            AiRangeSq = Donor->AiRangeSq;
    }

    AClassType CounterMeasureDropper::get_class_type() const
    {
        return AClassCounterMeasureDropper;
    }

    // baseline 0xdb50, folded with Engine::get_class_type.
    HpAttachmentType CounterMeasureDropper::get_hp_type() const
    {
        return HpTypeCountermeasureDropper;
    }

    Projectile::Projectile(ICliObj* InCompanion)
        : Equipment(InCompanion)
        , Lifetime(0.0f)
        , OwnerSafeTime(-1.0f)
        , bRequiresAmmo(false)
        , bForceGunOri(false)
        , LootAppearance(nullptr)
    {
        Pad7a[0] = 0;
        Pad7a[1] = 0;
    }

    bool Projectile::read(INI_Reader& Reader)
    {
        if (Equipment::read(Reader))
            return true;

        if (Reader.is_value("lifetime"))
        {
            Lifetime = Reader.get_value_float(0);
            return true;
        }

        if (Reader.is_value("owner_safe_time"))
        {
            OwnerSafeTime = Reader.get_value_float(0);
            return true;
        }

        if (Reader.is_value("requires_ammo"))
        {
            bRequiresAmmo = Reader.get_value_bool(0);
            return true;
        }

        if (Reader.is_value("force_gun_ori"))
        {
            bForceGunOri = Reader.get_value_bool(0);
            return true;
        }

        return ReadLootAppearance(Reader, LootAppearance);
    }

    void Projectile::redefine(Root const& Other)
    {
        Equipment::redefine(Other);
        Projectile const* Donor = (Projectile const*)&Other;
        if (Donor->Lifetime != Lifetime)
            Lifetime = Donor->Lifetime;
        if (Donor->OwnerSafeTime != OwnerSafeTime)
            OwnerSafeTime = Donor->OwnerSafeTime;
        if (Donor->bRequiresAmmo != bRequiresAmmo)
            bRequiresAmmo = Donor->bRequiresAmmo;
        if (Donor->bForceGunOri != bForceGunOri)
            bForceGunOri = Donor->bForceGunOri;
    }

    AClassType Projectile::get_class_type() const
    {
        return AClassProjectile;
    }

    // baseline Archetype::Projectile::operator= (Common) -- part of the
    // "inherit=" copy family, though the routine itself never calls it directly (Munition/
    // Mine/CounterMeasure::operator= each inline the equivalent copy instead); see
    // Equipment::InheritFrom. baseline 0626f250: Root::operator=, the Equipment shape
    // +0x5c..+0x6c, +0x70, +0x74, bytes +0x78 / +0x79, +0x7c. No self test.
    Projectile& Projectile::operator=(Projectile const& Other)
    {
        Equipment::operator=(Other);
        Lifetime = Other.Lifetime;
        OwnerSafeTime = Other.OwnerSafeTime;
        bRequiresAmmo = Other.bRequiresAmmo;
        bForceGunOri = Other.bForceGunOri;
        LootAppearance = Other.LootAppearance;
        return *this;
    }

    Munition::Munition(ICliObj* InCompanion)
        : Projectile(InCompanion)
        , HullDamage(0.0f)
        , EnergyDamage(0.0f)
        , Seeker(0)
        , TimeToLock(0.0f)
        , SeekerRange(0.0f)
        , SeekerFovCos(1.0f)
        , MaxAngularVelocity(100.0f)
        , DetonationDistSq(0.0f)
        , HpType(4)
        , bCruiseDisruptor(false)
    {
        WeaponType.clear();
        Motor.clear();
        PadAd[0] = 0;
        PadAd[1] = 0;
        PadAd[2] = 0;
        ObjectType = 0x2000000;
    }

    bool Munition::read(INI_Reader& Reader)
    {
        if (Projectile::read(Reader))
            return true;

        if (Reader.is_value("damage") || Reader.is_value("hull_damage"))
        {
            HullDamage = Reader.get_value_float(0);
            return true;
        }

        if (Reader.is_value("energy_damage"))
        {
            EnergyDamage = Reader.get_value_float(0);
            return true;
        }

        if (Reader.is_value("weapon_type"))
        {
            WeaponType.Id = CreateID(Reader.get_value_string());
            return true;
        }

        if (Reader.is_value("motor"))
        {
            Motor.Id = CreateID(Reader.get_value_string());
            return true;
        }

        if (Reader.is_value("seeker"))
        {
            // NEURONCOMPONENTS.DLL stricmp("dumb", value) / stricmp("lock", value).
            char const* Kind = Reader.get_value_string();
            if (CallNeuronComponentsStricmp("dumb", Kind) == 0)
                Seeker = 1;
            else if (CallNeuronComponentsStricmp("lock", Kind) == 0)
                Seeker = 2;
            else
                Seeker = 0;

            if (Seeker != 0)
                ObjectType = 0x1000000;
            else
                ObjectType = 0x2000000;
            return true;
        }

        if (Reader.is_value("time_to_lock"))
        {
            TimeToLock = Reader.get_value_float(0);
            return true;
        }

        if (Reader.is_value("seeker_range"))
        {
            SeekerRange = Reader.get_value_float(0);
            return true;
        }

        if (Reader.is_value("seeker_fov_deg"))
        {
            // FMUL float [the field] (0x3c8efa35 = 0.017453292f) then FCOS on the
            // unrounded get_value_float result (value_num = same read without the float
            // rounding), FSTP float.
            double Deg = Reader.value_num(0);
            SeekerFovCos = X87CosF(Deg * (double)0.017453292f);
            return true;
        }

        if (Reader.is_value("max_angular_velocity"))
        {
            MaxAngularVelocity = Reader.get_value_float(0);
            return true;
        }

        if (Reader.is_value("detonation_dist"))
        {
            // FLD ST0 / FMUL ST1 on the unrounded get_value_float result.
            double Dist = Reader.value_num(0);
            DetonationDistSq = (float)(Dist * Dist);
            return true;
        }

        if (Reader.is_value("hp_type"))
        {
            HpType = LookupHpType(Reader.get_value_string());
            return true;
        }

        if (Reader.is_value("cruise_disruptor"))
        {
            bCruiseDisruptor = Reader.get_value_bool(0);
            return true;
        }

        return false;
    }

    long Munition::BEAM_ARCH_INDEX = -1;

    // Port-only names for code baseline inlines into its two callers (they need the protected
    // static): the LoadEquipment tail 062f930e..062f9347 and Archetype::Free 062fab7f..062fab98.
    void Munition::LoadBeamArchIdx()
    {
        // still -1 -> {"BEAM_ARCH", GetCRC32("BEAM_ARCH")} to IEngine slot 11
        // (+0x2c, CreateCompoundArchetype); the result is stored as is. No engine test.
        if (BEAM_ARCH_INDEX != -1)
            return;

        FEngineArchetypeName Name;
        Name.Text = "BEAM_ARCH";
        Name.Crc = NEURONCOMPONENTS_CRC::GetCRC32("BEAM_ARCH");
        BEAM_ARCH_INDEX = (long)NeuronRuntime::Engine->CreateCompoundArchetype(&Name);
    }

    void Munition::FreeBeamArchIdx()
    {
        // not -1 -> IEngine slot 15 (+0x3c, ReleaseHandle), then -1. No engine test.
        if (BEAM_ARCH_INDEX == -1)
            return;
        NeuronRuntime::Engine->ReleaseHandle((void*)BEAM_ARCH_INDEX);
        BEAM_ARCH_INDEX = -1;
    }

    bool Munition::load()
    {
        // no DA_archetype -> take the shared BEAM_ARCH handle and AddRef it through
        // IEngine slot 14 (+0x38) unconditionally (even while it is still -1), count the load,
        // true. Otherwise the inlined body of Root::load.
        if (DaArchetype.Value[0] == '\0')
        {
            EngineIndex = BEAM_ARCH_INDEX;
            NeuronRuntime::Engine->AddRefHandle((void*)EngineIndex);
            LoadCount += 1;
            return true;
        }
        return Root::load();
    }

    void Munition::free_resources()
    {
        // no DA_archetype -> release the BEAM_ARCH reference (IEngine slot 15, +0x3c)
        // when held, then the companion's slot 6. Otherwise the inlined body of
        // Root::free_resources.
        if (DaArchetype.Value[0] == '\0')
        {
            if (EngineIndex != -1)
            {
                NeuronRuntime::Engine->ReleaseHandle((void*)EngineIndex);
                EngineIndex = -1;
            }
            CliObjFreeResources(Companion);
            return;
        }
        Root::free_resources();
    }

    void Munition::redefine(Root const& Other)
    {
        Projectile::redefine(Other);
        Munition const* Donor = (Munition const*)&Other;
        if (Donor->HullDamage != HullDamage)
            HullDamage = Donor->HullDamage;
        if (Donor->EnergyDamage != EnergyDamage)
            EnergyDamage = Donor->EnergyDamage;
        if (Donor->WeaponType.Id != WeaponType.Id)
            WeaponType.Id = Donor->WeaponType.Id;
        if (Donor->Motor.Id != Motor.Id)
            Motor.Id = Donor->Motor.Id;
        if (Donor->TimeToLock != TimeToLock)
            TimeToLock = Donor->TimeToLock;
        if (Donor->SeekerRange != SeekerRange)
            SeekerRange = Donor->SeekerRange;
        if (Donor->SeekerFovCos != SeekerFovCos)
            SeekerFovCos = Donor->SeekerFovCos;
        if (Donor->MaxAngularVelocity != MaxAngularVelocity)
            MaxAngularVelocity = Donor->MaxAngularVelocity;
    }

    AClassType Munition::get_class_type() const
    {
        return AClassMunition;
    }

    // baseline 0xc9a0, folded with Ship::get_class_type / Mine::get_hp_type /
    // CounterMeasure::get_hp_type -- all return the same unnamed constant 3. NOTE: this is
    // distinct from the HpType field (+0xa8), which Gun::get_hp_type() reads directly off a
    // linked Munition record; this virtual is Munition's own (unrelated) attachment-type tag.
    HpAttachmentType Munition::get_hp_type() const
    {
        return HpTypeMunition;
    }

    // baseline Archetype::Munition::operator= (Common) -- one leaf of the
    // "inherit=" shape-copy family; see Equipment::InheritFrom. baseline 0626f3e0:
    // Root::operator=, the Equipment and Projectile shapes inline (+0x5c..+0x7c), dwords
    // +0x80..+0xa8, byte +0xac. No self test.
    Munition& Munition::operator=(Munition const& Other)
    {
        Projectile::operator=(Other);
        HullDamage = Other.HullDamage;
        EnergyDamage = Other.EnergyDamage;
        WeaponType = Other.WeaponType;
        Motor = Other.Motor;
        Seeker = Other.Seeker;
        TimeToLock = Other.TimeToLock;
        SeekerRange = Other.SeekerRange;
        SeekerFovCos = Other.SeekerFovCos;
        MaxAngularVelocity = Other.MaxAngularVelocity;
        DetonationDistSq = Other.DetonationDistSq;
        HpType = Other.HpType;
        bCruiseDisruptor = Other.bCruiseDisruptor;
        return *this;
    }

    Mine::Mine(ICliObj* InCompanion)
        : Projectile(InCompanion)
        , LinearDrag(0.0f)
        , DetonationDist(0.0f)
        , SeekDist(0.0f)
        , Acceleration(-1.0f)
        , TopSpeed(40.0f)
    {
        bRequiresAmmo = true;
        ObjectType = 0x4000000;
    }

    bool Mine::read(INI_Reader& Reader)
    {
        if (Projectile::read(Reader))
            return true;

        if (Reader.is_value("linear_drag"))
        {
            LinearDrag = Reader.get_value_float(0);
            return true;
        }

        if (Reader.is_value("detonation_dist"))
        {
            DetonationDist = Reader.get_value_float(0);
            return true;
        }

        if (Reader.is_value("seek_dist"))
        {
            SeekDist = Reader.get_value_float(0);
            return true;
        }

        if (Reader.is_value("acceleration"))
        {
            Acceleration = Reader.get_value_float(0);
            return true;
        }

        if (Reader.is_value("top_speed"))
        {
            TopSpeed = Reader.get_value_float(0);
            return true;
        }

        return false;
    }

    void Mine::redefine(Root const& Other)
    {
        Projectile::redefine(Other);
        Mine const* Donor = (Mine const*)&Other;
        if (Donor->LinearDrag != LinearDrag)
            LinearDrag = Donor->LinearDrag;
        if (Donor->DetonationDist != DetonationDist)
            DetonationDist = Donor->DetonationDist;
        if (Donor->SeekDist != SeekDist)
            SeekDist = Donor->SeekDist;
        if (Donor->TopSpeed != TopSpeed)
            TopSpeed = Donor->TopSpeed;
        if (Donor->Acceleration != Acceleration)
            Acceleration = Donor->Acceleration;
    }

    AClassType Mine::get_class_type() const
    {
        return AClassMine;
    }

    HpAttachmentType Mine::get_hp_type() const
    {
        return HpTypeMunition;
    }

    // baseline Archetype::Mine::operator= (Common) -- one leaf of the "inherit="
    // shape-copy family; see Equipment::InheritFrom. baseline 0626f5f0: Root::operator=,
    // +0x5c..+0x7c inline, dwords +0x80..+0x90. No self test.
    Mine& Mine::operator=(Mine const& Other)
    {
        Projectile::operator=(Other);
        LinearDrag = Other.LinearDrag;
        DetonationDist = Other.DetonationDist;
        SeekDist = Other.SeekDist;
        Acceleration = Other.Acceleration;
        TopSpeed = Other.TopSpeed;
        return *this;
    }

    CounterMeasure::CounterMeasure(ICliObj* InCompanion)
        : Projectile(InCompanion)
        , LinearDrag(0.0f)
        , Range(0.0f)
        , DiversionPctg(0)
    {
        bRequiresAmmo = true;
    }

    bool CounterMeasure::read(INI_Reader& Reader)
    {
        if (Projectile::read(Reader))
            return true;

        if (Reader.is_value("linear_drag"))
        {
            LinearDrag = Reader.get_value_float(0);
            return true;
        }

        if (Reader.is_value("range"))
        {
            Range = Reader.get_value_float(0);
            return true;
        }

        if (Reader.is_value("diversion_pctg"))
        {
            // _ftol on the unrounded get_value_float result (value_num = same read, no rounding).
            DiversionPctg = (int)Reader.value_num(0);
            return true;
        }

        return false;
    }

    void CounterMeasure::redefine(Root const& Other)
    {
        Projectile::redefine(Other);
        CounterMeasure const* Donor = (CounterMeasure const*)&Other;
        if (Donor->LinearDrag != LinearDrag)
            LinearDrag = Donor->LinearDrag;
        if (Donor->Range != Range)
            Range = Donor->Range;
        if (Donor->DiversionPctg != DiversionPctg)
            DiversionPctg = Donor->DiversionPctg;
    }

    AClassType CounterMeasure::get_class_type() const
    {
        return AClassCounterMeasure;
    }

    // baseline Archetype::CounterMeasure::operator= (Common) -- one leaf of the
    // "inherit=" shape-copy family; see Equipment::InheritFrom. baseline 0626f790:
    // Root::operator=, +0x5c..+0x7c inline, dwords +0x80..+0x88. No self test.
    CounterMeasure& CounterMeasure::operator=(CounterMeasure const& Other)
    {
        Projectile::operator=(Other);
        LinearDrag = Other.LinearDrag;
        Range = Other.Range;
        DiversionPctg = Other.DiversionPctg;
        return *this;
    }

    HpAttachmentType CounterMeasure::get_hp_type() const
    {
        return HpTypeMunition;
    }

    // Common 0x96910. Memoized in bRangeCached/CachedMunitionRange (Gun+0xcc/+0xd0).
    // No motor: range = Munition::Lifetime * MuzzleVelocity. With a motor: delay-phase coast
    // + accelerated burn distance + post-burn coast at the burned-up-to velocity.
    float Gun::get_munition_range() const
    {
        if (bRangeCached)
            return CachedMunitionRange;

        CachedMunitionRange = 0.0f;

        Equipment* Found = GEquipmentDb.Get(ProjectileArchetype.Id);
        if (Found == nullptr)
        {
            FDUMP(0x100001,
                  "%s(%d) : *** ERROR: Gun::get_munition_range() failed: Couldn't find [Munition] %s for [Gun] %s",
                  ArchetypeSource, 0xd2d, ProjectileArchetype.get_string(), Nickname.get_string());
            return CachedMunitionRange;
        }

        Munition* Muni = (Munition*)Found;
        if (Muni->Motor.IsEmpty())
        {
            CachedMunitionRange = Muni->Lifetime * MuzzleVelocity;
            bRangeCached = true;
            return CachedMunitionRange;
        }

        MotorData const* Motor = GMotorDb.Get(Muni->Motor.Id);
        if (Motor == nullptr)
        {
            FDUMP(0x100001,
                  "%s(%d) : *** ERROR: Gun::get_munition_range() failed: Couldn't find [Motor] %s for [Munition] %s for [Gun] %s",
                  ArchetypeSource, 0xd25, Muni->Motor.get_string(), ProjectileArchetype.get_string(),
                  Nickname.get_string());
            return CachedMunitionRange;
        }

        // the whole sum stays on the x87 stack (the intermediate FSTs into
        // +0xd0 are overwritten); only the final value is rounded to float. Same operation
        // order as baseline, intermediates in double:
        //   Sum  = Delay*Mv;  Sum += ((L*L)*Accel)*0.5;  Sum += Mv*L;
        //   +0xd0 = float(((MuniLifetime - L) * (L*Accel + Mv)) + Sum)
        double const Mv = MuzzleVelocity;
        double const BurnTime = Motor->Lifetime;
        double const Accel = Motor->Accel;
        double Sum = Motor->Delay * Mv;
        Sum = Sum + BurnTime * BurnTime * Accel * 0.5;
        Sum = Sum + Mv * BurnTime;
        double const PostBurn = (Muni->Lifetime - BurnTime) * (BurnTime * Accel + Mv);
        CachedMunitionRange = (float)(PostBurn + Sum);
        bRangeCached = true;
        return CachedMunitionRange;
    }

    int Gun::get_number_of_hp_types() const
    {
        // 0x968a0: falls back to 0x17 (23, the built-in kEmptyGunHpTypes table size used by
        // get_hp_type_by_index) when the custom hp_gun_type list is null or empty -- mirrors
        // ShieldGenerator::get_number_of_hp_types' 0x22 fallback below.
        int Count = (HpGunTypesBegin != nullptr)
                        ? (int)(HpGunTypesEnd - HpGunTypesBegin)
                        : 0;
        return Count != 0 ? Count : 0x17;
    }
}
