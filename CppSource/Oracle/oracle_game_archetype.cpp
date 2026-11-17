// Data dumps of the goods, loadouts, factions and archetypes of Common (work
// package P2.1), written after oracle_game_data.cpp has run the [Data] list.
// crates/game-sim/tests/support/render.rs renders the same lines.
//
// Floats are written as their bit patterns, ids as eight hex digits, text
// through GameText (printable ASCII as it is except '"' and '\', every other
// byte as \u00xx).

#include "oracle_support.h"
#include "oracle_game.h"

#include <string.h>

#include <set>
#include <string>
#include <vector>

#include "common_ini.h"
#include "common_id.h"
#include "common_archetype.h"
#include "common_goods.h"
#include "common_reputation.h"

using namespace oracle;

std::string GameText(const char* Value)
{
    std::string Out;
    if (Value == nullptr)
        return Out;
    for (const unsigned char* P = (const unsigned char*)Value; *P != 0; ++P)
    {
        if (*P >= 0x20 && *P < 0x7f && *P != '"' && *P != '\\')
            Out += (char)*P;
        else
            Out += Format("\\u%04x", (unsigned int)*P);
    }
    return Out;
}

std::string GameF(float Value)
{
    return Format("%08x", FloatBits(Value));
}

std::string GameId(unsigned int Value)
{
    return Format("%08x", Value);
}

void FLine::Add(const char* Key, const std::string& Value)
{
    if (!Text.empty())
        Text += ",";
    Text += "\"";
    Text += Key;
    Text += "\":\"";
    Text += Value;
    Text += "\"";
}

std::string FLine::Done() const
{
    return "{" + Text + "}";
}

namespace
{
    std::string B(bool Value)
    {
        return Value ? "1" : "0";
    }

    std::string I(int Value)
    {
        return Format("%d", Value);
    }

    std::string U(unsigned int Value)
    {
        return Format("%u", Value);
    }

    std::string X(unsigned int Value)
    {
        return Format("%x", Value);
    }

    std::string F3(float A, float B, float C)
    {
        return GameF(A) + "," + GameF(B) + "," + GameF(C);
    }

    std::string Join(const std::vector<std::string>& Parts, const char* Separator)
    {
        std::string Out;
        for (size_t Index = 0; Index < Parts.size(); ++Index)
        {
            if (Index != 0)
                Out += Separator;
            Out += Parts[Index];
        }
        return Out;
    }

    // EquipDesc fields, as render.rs's equip_desc.
    void AddEquipDesc(FLine& L, const EquipDesc& D)
    {
        L.Add("id", Format("%04x", (unsigned int)D.Id));
        L.Add("arch", GameId(D.ArchId));
        L.Add("hp", GameText(D.Hardpoint));
        L.Add("internal", B(D.is_internal()));
        L.Add("equipped", U(D.bEquipped));
        L.Add("health", GameF(D.Health));
        L.Add("count", I(D.Count));
        L.Add("mission", B(D.bMission));
        L.Add("owner", U(D.Owner));
    }

    void AddRoot(FLine& L, const Archetype::Root& R)
    {
        L.Add("nick", GameId(R.Nickname.Id));
        L.Add("da", GameText(R.DaArchetype.Value));
        L.Add("type", GameId(R.ObjectType));
        L.Add("ids", I(R.IdsName) + "," + I(R.IdsInfo));
        L.Add("hit_pts", GameF(R.HitPts));
        L.Add("mass", GameF(R.Mass));
        L.Add("expl", GameId(R.ExplosionArch.Id) + "," + GameF(R.ExplosionResistance));
        L.Add("ri", F3(R.RotationInertiaX, R.RotationInertiaY, R.RotationInertiaZ));
        L.Add("has_ri", B(R.bHasRotationInertia));
        L.Add("phantom", B(R.bPhantomPhysics));
    }

    std::string FuseText(const Archetype::FGroupFuse& Fuse)
    {
        return GameId(Fuse.Fuse.Id) + "," + GameF(Fuse.Value1) + "," + GameF(Fuse.Value2);
    }

    void DumpGroup(Suite& Out, const Archetype::CollisionGroup& G)
    {
        FLine L;
        L.Add("group", Format("%04x", (unsigned int)G.Id));
        L.Add("obj", GameText(G.ObjName));
        L.Add("type", X(G.Type));
        L.Add("hit_pts", I(G.HitPts));
        L.Add("flags", B(G.bSeparable) + "," + B(G.bDestroyParent) + "," + B(G.bRootHealthProxy));
        L.Add("impulse", GameF(G.ParentImpulse) + "," + GameF(G.ChildImpulse));
        L.Add("mass", GameF(G.Mass));
        L.Add("has_ri", B(G.bHasRotationInertia));
        L.Add("ri", F3(G.RotationInertiaX, G.RotationInertiaY, G.RotationInertiaZ));
        L.Add("expl", GameId(G.SeparationExplosion.Id) + "," + GameId(G.DebrisType.Id) + "," +
                          GameF(G.ExplosionResistance));
        L.Add("linked", X(G.LinkedEquipType) + "," + GameF(G.LinkedEquipHealth));
        Out.Line(L.Done());
        for (const Archetype::DamageObjInfo& D : *G.DmgObjs.Items)
        {
            FLine M;
            M.Add("dmg", GameText(D.Hardpoint.Value) + "," + GameId(D.Arch.Id));
            Out.Line(M.Done());
        }
        for (const Archetype::DamageObjInfo& D : *G.GroupDmgObjs.Items)
        {
            FLine M;
            M.Add("group_dmg", GameText(D.Hardpoint.Value) + "," + GameId(D.Arch.Id));
            Out.Line(M.Done());
        }
        for (const Archetype::FGroupFuse& Fuse : *G.Fuses.Items)
        {
            FLine M;
            M.Add("group_fuse", FuseText(Fuse));
            Out.Line(M.Done());
        }
    }

    void AddEqObjHead(FLine& L, const Archetype::EqObj& E)
    {
        L.Add("shield_next", Format("%04x", (unsigned int)E.ShieldIdNext));
        L.Add("veilborn", B(E.bVeilborn));
        L.Add("dock_cam", B(E.bDockingCamera));
    }

    void DumpEqObjTail(Suite& Out, const Archetype::EqObj& E)
    {
        for (const Archetype::CollisionGroup* G = E.CollisionGroups; G != nullptr; G = G->Next)
            DumpGroup(Out, *G);
        for (const Archetype::ShieldLink& Link : *E.ShieldLinks)
        {
            std::vector<std::string> Gens;
            for (const CacheString& Hp : Link.GeneratorHps)
                Gens.push_back(GameText(Hp.Value));
            FLine L;
            L.Add("shield_link", GameId(Link.ShieldArch.Id));
            L.Add("id", Format("%04x", (unsigned int)Link.EquipId));
            L.Add("hp", GameText(Link.Hardpoint.Value));
            L.Add("gens", Join(Gens, ";"));
            Out.Line(L.Done());
        }
        for (const Archetype::FGroupFuse& Fuse : *E.Fuses)
        {
            FLine L;
            L.Add("fuse", FuseText(Fuse));
            Out.Line(L.Done());
        }
        for (const Archetype::EqObj::DockingSphere& D : E.DockingSpheres)
        {
            FLine L;
            L.Add("dock", U(D.Type));
            L.Add("hp", GameText(D.HardpointName.c_str()));
            L.Add("radius", GameF(D.Radius));
            L.Add("anim", GameText(D.AnimName.c_str()));
            Out.Line(L.Done());
        }
    }

    std::vector<std::string> AttachedTokens(const Archetype::AttachedEquipment& A)
    {
        std::vector<std::string> T;
        T.push_back(GameId(A.SeparationExplosion.Id));
        T.push_back(GameId(A.DebrisType.Id));
        T.push_back(GameF(A.ChildImpulse));
        T.push_back(GameF(A.ParentImpulse));
        T.push_back(GameText(A.HpChild.Value));
        T.push_back(I(A.Toughness));
        return T;
    }

    std::vector<std::string> LauncherTokens(const Archetype::Launcher& L)
    {
        std::vector<std::string> T = AttachedTokens(L);
        T.push_back(GameF(L.DamagePerFire));
        T.push_back(GameF(L.PowerUsage));
        T.push_back(GameF(L.RefireDelay));
        T.push_back(GameF(L.MuzzleVelocity));
        T.push_back(GameId(L.ProjectileArchetype.Id));
        T.push_back(GameText(L.UseAnimation.Value));
        return T;
    }

    std::string Appearance(const Archetype::Equipment* E)
    {
        return GameId(E != nullptr ? E->Nickname.Id : 0);
    }

    std::vector<std::string> ProjectileTokens(const Archetype::Projectile& P)
    {
        std::vector<std::string> T;
        T.push_back(GameF(P.Lifetime));
        T.push_back(GameF(P.OwnerSafeTime));
        T.push_back(B(P.bRequiresAmmo));
        T.push_back(B(P.bForceGunOri));
        T.push_back(Appearance(P.LootAppearance));
        return T;
    }

    std::vector<std::string> HpTypes(const unsigned* Begin, const unsigned* End)
    {
        std::vector<std::string> T;
        if (Begin == nullptr)
            return T;
        for (const unsigned* P = Begin; P != End; ++P)
            T.push_back(X(*P));
        return T;
    }

    // The data tokens, hp types and seeker cone of one equipment record, as
    // render.rs's kind().
    void EquipmentKind(const Archetype::Equipment& E, std::vector<std::string>& Data,
                       std::vector<std::string>& Types, std::string& FovCos)
    {
        using namespace Archetype;
        switch (E.get_class_type())
        {
        case AClassCommodity:
        {
            const Commodity& C = (const Commodity&)E;
            Data.push_back(GameF(C.DecayPerSecond));
            Data.push_back(Appearance(C.LootAppearance));
            Data.push_back(Appearance(C.PodAppearance));
            break;
        }
        case AClassPower:
        {
            const Power& P = (const Power&)E;
            Data.push_back(GameF(P.Capacity));
            Data.push_back(GameF(P.ChargeRate));
            Data.push_back(GameF(P.ThrustCapacity));
            Data.push_back(GameF(P.ThrustChargeRate));
            break;
        }
        case AClassEngine:
        {
            const Engine& P = (const Engine&)E;
            Data.push_back(GameF(P.ThrustMultiplier));
            Data.push_back(GameF(P.LinearDrag));
            Data.push_back(GameF(P.PowerDrawFactor));
            Data.push_back(GameF(P.CruisePowerUsage));
            Data.push_back(GameF(P.CruiseChargeTime));
            Data.push_back(B(P.bIndestructible));
            Data.push_back(GameF(P.ReverseFraction));
            break;
        }
        case AClassArmor:
            Data.push_back(GameF(((const Armor&)E).HitPtsScale));
            break;
        case AClassShield:
        case AClassCargoPod:
            Data = AttachedTokens((const AttachedEquipment&)E);
            break;
        case AClassShieldGenerator:
        {
            const ShieldGenerator& G = (const ShieldGenerator&)E;
            Data = AttachedTokens(G);
            Data.push_back(X(G.HpType));
            Data.push_back(GameId(G.ShieldType.Id));
            Data.push_back(GameF(G.RegenerationRate));
            Data.push_back(GameF(G.MaxCapacity));
            Data.push_back(GameF(G.ConstantPowerDraw));
            Data.push_back(GameF(G.RebuildPowerDraw));
            Data.push_back(GameF(G.OfflineThreshold));
            Data.push_back(GameF(G.OfflineRebuildTime));
            Types = HpTypes(G.HpShieldTypesBegin, G.HpShieldTypesEnd);
            break;
        }
        case AClassThruster:
        {
            const Thruster& T = (const Thruster&)E;
            Data = AttachedTokens(T);
            Data.push_back(GameF(T.PowerUsage));
            Data.push_back(GameF(T.MaxThrust));
            break;
        }
        case AClassGun:
        {
            const Gun& G = (const Gun&)E;
            Data = LauncherTokens(G);
            Data.push_back(GameF(G.DispersionAngle));
            Data.push_back(GameF(G.TurnRate));
            Data.push_back(GameF(G.AzimuthMin));
            Data.push_back(GameF(G.AzimuthMax));
            Data.push_back(GameF(G.ElevationMin));
            Data.push_back(GameF(G.ElevationMax));
            Data.push_back(B(G.bAzimuthValid));
            Data.push_back(B(G.bElevationValid));
            Data.push_back(B(G.bAutoTurret));
            Types = HpTypes(G.HpGunTypesBegin, G.HpGunTypesEnd);
            break;
        }
        case AClassMineDropper:
            Data = LauncherTokens((const Launcher&)E);
            break;
        case AClassCounterMeasureDropper:
        {
            const CounterMeasureDropper& D = (const CounterMeasureDropper&)E;
            Data = LauncherTokens(D);
            Data.push_back(GameF(D.AiRangeSq));
            break;
        }
        case AClassScanner:
        {
            const Scanner& S = (const Scanner&)E;
            Data.push_back(GameF(S.PowerThreshold));
            Data.push_back(GameF(S.RadarRange));
            Data.push_back(GameF(S.CargoScanRange));
            break;
        }
        case AClassLight:
        {
            const Light& S = (const Light&)E;
            Data.push_back(B(S.bAlwaysOn));
            Data.push_back(B(S.bDockingLight));
            break;
        }
        case AClassTractor:
        {
            const Tractor& S = (const Tractor&)E;
            Data.push_back(GameF(S.MaxLength));
            Data.push_back(GameF(S.ReachSpeed));
            break;
        }
        case AClassInternalFX:
            Data.push_back(GameText(((const InternalFXEquip&)E).UseAnimation.Value));
            break;
        case AClassRepairDroid:
            Data.push_back(GameF(((const RepairDroid&)E).RepairRate));
            break;
        case AClassRepairKit:
            Data.push_back(Appearance(((const RepairKit&)E).LootAppearance));
            break;
        case AClassShieldBattery:
            Data.push_back(Appearance(((const ShieldBattery&)E).LootAppearance));
            break;
        case AClassCloakingDevice:
        {
            const CloakingDevice& C = (const CloakingDevice&)E;
            Data = AttachedTokens(C);
            Data.push_back(GameF(C.PowerUsage));
            Data.push_back(GameF(C.CloakInTime));
            Data.push_back(GameF(C.CloakOutTime));
            Data.push_back(GameId(C.CloakInFx));
            Data.push_back(GameId(C.CloakOutFx));
            break;
        }
        case AClassMunition:
        {
            const Munition& M = (const Munition&)E;
            Data = ProjectileTokens(M);
            Data.push_back(GameF(M.HullDamage));
            Data.push_back(GameF(M.EnergyDamage));
            Data.push_back(GameId(M.WeaponType.Id));
            Data.push_back(GameId(M.Motor.Id));
            Data.push_back(U(M.Seeker));
            Data.push_back(GameF(M.TimeToLock));
            Data.push_back(GameF(M.SeekerRange));
            Data.push_back(GameF(M.MaxAngularVelocity));
            Data.push_back(GameF(M.DetonationDistSq));
            Data.push_back(X(M.HpType));
            Data.push_back(B(M.bCruiseDisruptor));
            FovCos = GameF(M.SeekerFovCos);
            break;
        }
        case AClassMine:
        {
            const Mine& M = (const Mine&)E;
            Data = ProjectileTokens(M);
            Data.push_back(GameF(M.LinearDrag));
            Data.push_back(GameF(M.DetonationDist));
            Data.push_back(GameF(M.SeekDist));
            Data.push_back(GameF(M.Acceleration));
            Data.push_back(GameF(M.TopSpeed));
            break;
        }
        case AClassCounterMeasure:
        {
            const CounterMeasure& M = (const CounterMeasure&)E;
            Data = ProjectileTokens(M);
            Data.push_back(GameF(M.LinearDrag));
            Data.push_back(GameF(M.Range));
            Data.push_back(I(M.DiversionPctg));
            break;
        }
        default:
            // LootCrate, AttachedFX, TradeLane: nothing beyond Equipment.
            break;
        }
    }

    // The ids of one class, in map order (Archetype::GetIdCount / GetId).
    std::vector<unsigned int> Ids(Archetype::AClassType Class)
    {
        std::vector<unsigned int> Out;
        const unsigned int Count = Archetype::GetIdCount(Class);
        for (unsigned int Index = 0; Index < Count; ++Index)
            Out.push_back(Archetype::GetId(Class, Index));
        return Out;
    }
}

void DumpGoods(FGameKind& K)
{
    Suite Out(K.Path("goods"));
    GoodInfoList* List = GoodList_get();
    const std::list<GoodInfo*>* Goods = List->get_list();
    for (const GoodInfo* G : *Goods)
    {
        FLine L;
        L.Add("good", GameId(G->Id));
        L.Add("cat", GameId(G->Category));
        L.Add("arch", GameId(G->ArchId));
        L.Add("ship", GameId(G->ShipArchId));
        L.Add("price", GameF(G->Price) + "," + GameF(G->GoodBuyPrice) + "," + GameF(G->BadBuyPrice) + "," +
                           GameF(G->GoodSellPrice) + "," + GameF(G->BadSellPrice));
        L.Add("jump", I(G->JumpDist));
        L.Add("comb", B(G->bCombinable));
        L.Add("shop", GameText(G->ShopArchetype.Value));
        L.Add("icon", GameText(G->ItemIcon.Value));
        L.Add("mat", GameText(G->MaterialLibrary.Value));
        L.Add("hp_child", GameText(G->HpChild.Value));
        L.Add("attach", GameText(G->AttachmentArchetype.Value));
        L.Add("ids", I(G->IdsName) + "," + I(G->IdsInfo));
        L.Add("hull", GameId(G->HullGoodId));
        L.Add("ammo", GameId(G->FreeAmmoArchId) + "," + I(G->FreeAmmoCount));
        L.Add("prefix", GameText(G->GetMsgIdPrefix().Data));
        L.Add("self_by_id", B(List->find_by_id(G->Id) == G));
        L.Add("self_by_arch", B(List->find_by_archetype(G->ArchId) == G));
        Out.Line(L.Done());
        const char* const Slots[] = {"low", "mid", "high"};
        const std::vector<EquipDesc>* Lists[] = {&G->AddonLow, &G->AddonMid, &G->AddonHigh};
        for (int Slot = 0; Slot < 3; ++Slot)
        {
            for (const EquipDesc& D : *Lists[Slot])
            {
                FLine A;
                A.Add("addon", Slots[Slot]);
                AddEquipDesc(A, D);
                Out.Line(A.Done());
            }
        }
    }
}

void DumpLoadouts(FGameKind& K, const std::set<unsigned int>& LoadoutIds)
{
    Suite Out(K.Path("loadouts"));
    for (unsigned int Id : LoadoutIds)
    {
        const Loadout::Map* Map = Loadout::Get(Id);
        if (Map == nullptr)
            continue;
        FLine L;
        L.Add("loadout", GameId(Map->NicknameId));
        L.Add("n", U((unsigned int)Map->Items.size()));
        Out.Line(L.Done());
        for (const EquipDesc& D : Map->Items)
        {
            FLine A;
            AddEquipDesc(A, D);
            Out.Line(A.Done());
        }
    }
}

namespace
{
    struct FGroupCollector : Reputation::RepGroupCB
    {
        std::vector<unsigned int> Ids;

        virtual char Execute(unsigned const* GroupId)
        {
            Ids.push_back(*GroupId);
            return 1;
        }
    };
}

void DumpReputation(FGameKind& K)
{
    Suite Out(K.Path("groups"));
    FGroupCollector Collector;
    Reputation::enumerate(&Collector);
    for (unsigned int Id : Collector.Ids)
    {
        TString<16> Nickname;
        Nickname.Length = 0;
        Nickname.Data[0] = '\0';
        Reputation::get_nickname(Nickname, Id);
        FLine L;
        L.Add("group", Format("%04x", Id));
        L.Add("nickname", GameText(Nickname.Data));
        L.Add("name", U(Reputation::get_name(Id)));
        L.Add("info", U(Reputation::get_info_card(Id)));
        L.Add("short", U(Reputation::get_short_name(Id)));
        Out.Line(L.Done());
    }
}

void DumpArchetypes(FGameKind& K, const std::set<unsigned int>& MotorIds,
                    const std::set<unsigned int>& ExplosionIds)
{
    using namespace Archetype;
    {
        Suite Out(K.Path("small_ids"));
        for (unsigned int Small = 1; Small < 0x10000; ++Small)
        {
            const unsigned int Large = SmallIDToLargeID((unsigned short)Small);
            if (Large == 0)
                break;
            FLine L;
            L.Add("small", U(Small));
            L.Add("large", GameId(Large));
            Out.Line(L.Done());
        }
    }
    {
        Suite Out(K.Path("simples"));
        for (unsigned int Id : Ids(AClassRoot))
        {
            FLine L;
            AddRoot(L, *GetSimple(Id));
            Out.Line(L.Done());
        }
    }
    {
        Suite Out(K.Path("ships"));
        for (unsigned int Id : Ids(AClassShip))
        {
            const Ship& S = *GetShip(Id);
            FLine L;
            AddRoot(L, S);
            AddEqObjHead(L, S);
            L.Add("prefix", GameText(S.GetMsgIdPrefix().Data));
            L.Add("hps", GameText(S.BayDoorAnim.Value) + "," + GameText(S.HpBaySurface.Value) + "," +
                             GameText(S.HpBayExternal.Value) + "," + GameText(S.HpTractorSource.Value));
            L.Add("ints", I(S.IdsInfo1) + "," + I(S.IdsInfo2) + "," + I(S.IdsInfo3) + "," + I(S.ShipClass) +
                              "," + I(S.NumExhaustNozzles) + "," + I(S.NanobotLimit) + "," +
                              I(S.ShieldBatteryLimit));
            L.Add("floats", GameF(S.HoldSize) + "," + GameF(S.LinearDrag) + "," + GameF(S.NudgeForce) + "," +
                                GameF(S.StrafeForce) + "," + GameF(S.StrafePowerUsage) + "," +
                                GameF(S.MaxBankAngle) + "," + F3(S.AngularDragX, S.AngularDragY, S.AngularDragZ));
            L.Add("torque", F3(S.SteeringTorqueX, S.SteeringTorqueY, S.SteeringTorqueZ));
            Out.Line(L.Done());
            DumpEqObjTail(Out, S);
            for (const LegalHpTypes& Legal : S.LegalHps)
            {
                std::vector<std::string> Hps;
                for (const CacheString& Hp : Legal.Hardpoints)
                    Hps.push_back(GameText(Hp.Value));
                FLine H;
                H.Add("hp_type", X((unsigned int)Legal.Type));
                H.Add("hps", Join(Hps, ";"));
                Out.Line(H.Done());
            }
        }
    }
    {
        Suite Out(K.Path("solars"));
        for (unsigned int Id : Ids(AClassSolar))
        {
            const Solar& S = *GetSolar(Id);
            FLine L;
            AddRoot(L, S);
            AddEqObjHead(L, S);
            L.Add("destructible", B(S.bDestructible));
            L.Add("radius", GameF(S.SolarRadius));
            L.Add("toughness", I(S.Toughness));
            L.Add("open_anim", GameText(S.OpenAnim.Value));
            L.Add("jump_out_hp", GameText(S.JumpOutHp.Value));
            L.Add("loadout", GameId(S.Loadout.Id));
            Out.Line(L.Done());
            DumpEqObjTail(Out, S);
        }
    }
    {
        Suite Out(K.Path("equipment"));
        for (unsigned int Id : Ids(AClassEquipment))
        {
            const Equipment& E = *GetEquipment(Id);
            std::vector<std::string> Data;
            std::vector<std::string> Types;
            std::string FovCos;
            EquipmentKind(E, Data, Types, FovCos);
            FLine L;
            AddRoot(L, E);
            L.Add("class", X((unsigned int)E.get_class_type()));
            L.Add("volume", GameF(E.Volume));
            L.Add("base", B(E.bUseCount) + "," + I(E.UnitsPerContainer) + "," + GameId(E.TractoredExplosion.Id) +
                              "," + B(E.bLootable));
            L.Add("data", Join(Data, ","));
            L.Add("hp_types", Join(Types, ","));
            L.Add("fov_cos", FovCos);
            Out.Line(L.Done());
        }
    }
    {
        Suite Out(K.Path("asteroids"));
        for (unsigned int Id : Ids(AClassAsteroid))
        {
            const Asteroid& A = *GetAsteroid(Id);
            FLine L;
            AddRoot(L, A);
            L.Add("detect", GameF(A.DetectRadius));
            L.Add("offset", GameF(A.ExplosionOffset));
            L.Add("recharge", GameF(A.RechargeTime));
            Out.Line(L.Done());
        }
    }
    {
        Suite Out(K.Path("dynamic_asteroids"));
        for (unsigned int Id : Ids(AClassDynamicAsteroid))
        {
            FLine L;
            AddRoot(L, *GetDynamicAsteroid(Id));
            Out.Line(L.Done());
        }
    }
    {
        Suite Out(K.Path("motors"));
        for (unsigned int Id : MotorIds)
        {
            const MotorData* M = GetMotor(Id);
            if (M == nullptr)
                continue;
            FLine L;
            L.Add("motor", GameId(M->Nickname.Id));
            L.Add("delay", GameF(M->Delay));
            L.Add("accel", GameF(M->Accel));
            L.Add("lifetime", GameF(M->Lifetime));
            Out.Line(L.Done());
        }
    }
    {
        Suite Out(K.Path("explosions"));
        for (unsigned int Id : ExplosionIds)
        {
            ID_String Name;
            Name.Id = Id;
            const Explosion* E = GetExplosion(Name);
            if (E == nullptr)
                continue;
            FLine L;
            L.Add("explosion", GameId(E->Nickname.Id));
            L.Add("damage", GameF(E->Radius) + "," + GameF(E->Impulse) + "," + GameF(E->HullDamage) + "," +
                                GameF(E->EnergyDamage));
            L.Add("process", U(E->Process));
            L.Add("life", GameF(E->Lifetime) + "," + GameF(E->LifetimeMax));
            L.Add("children", I(E->NumChildPieces));
            L.Add("debris_impulse", GameF(E->DebrisImpulse));
            L.Add("innards", F3(E->InnardsDebrisStartTime, E->InnardsDebrisNum, E->InnardsDebrisRadius));
            Out.Line(L.Done());
            for (const FExplosionEffect& Fx : E->Effects)
            {
                FLine M;
                M.Add("effect", GameId(Fx.Fx.Id) + "," + GameF(Fx.Strength));
                Out.Line(M.Done());
            }
            for (unsigned int Object : E->InnardsDebrisObjects)
            {
                FLine M;
                M.Add("innards_object", GameId(Object));
                Out.Line(M.Done());
            }
            for (const FExplosionDebrisType& Debris : E->DebrisTypes)
            {
                FLine M;
                M.Add("debris", GameId(Debris.Debris.Id) + "," + GameF(Debris.Count));
                Out.Line(M.Done());
            }
        }
    }
}
