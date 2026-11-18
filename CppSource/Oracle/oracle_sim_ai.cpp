// Oracle.exe sim: the behaviour-tick recorder of work package P2.5 (oracle_sim_ai.h). For
// every IBehaviorManager::UpdateBehaviorTick it writes to ai.jsonl the manager's state on
// entry and exit, the personality, a snapshot of every object the tick can read (its own
// ship, target, formation and follow partners, dock target, the target list, the object in
// its sights and the objects within kNearRadius), the PhySys queries the tick made with the
// objects they found, and the rand() values the game DLLs drew. The layout is in
// design/sim-records.md (`ai.jsonl`). The IObjAffect commands of the tick are already in
// calls.jsonl (`affect`, P2.3), keyed by the same `seq`.
//
// Everything here is inert unless OracleSimAiStart runs, which only Oracle.exe sim does.

#include <windows.h>

#include <stdio.h>
#include <string.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "oracle_sim_ai.h"
#include "oracle_sim_rand.h"
#include "../OutpostFrontier/Client/oracle_json.h"

#include "common_cequip.h"   // StrafeDir
#include "common_cobjdb.h"   // IObjDB
#include "common_cobject.h"  // CObject
#include "common_iface.h"    // IObjInspect, IObjRW, IBehaviorManager, EquipStatus
#include "common_misc.h"     // SUBOBJ_ID_NONE
#include "common_ini.h"      // Vector
#include "common_csys.h"     // Matrix
#include "oracle_ai.h"
#include "oracle_sim_hooks.h"

namespace
{
    // The record layout version (`ai_start`).
    const unsigned int kAiVersion = 1;
    // Objects within this distance of the ship (m) are snapshotted with role "near".
    const float kNearRadius = 5000.0f;
    // The object database walk mask: ships, solars and loot (StarSystem::GetNext).
    const unsigned int kWalkMask = 0x107ffdffu;
    // At most this many formation followers are read.
    const unsigned int kMaxFollowers = 16;
    // At most this many of the own ship's items are read (find_equipment's buffer of the
    // gun groups is 0x40 per type).
    const int kMaxEquip = 64;
    // The equipment types the system manager finds: guns and launchers, mine droppers,
    // countermeasure droppers, shields.
    const unsigned int kEquipTypes[4] = {0x80u, 0x20u, 0x40u, 0x100u};
    // The angle the fire control asks is_pointing_at with for guns (pi/60, 0x3d567750).
    const float kGunPointingAngle = 0.05235988f;
    // The angle the auto-turret scan asks can_point_at with (0x3e99999a).
    const float kTurretPointAngle = 0.3f;
    // At most this many target-list candidates are asked can_point_at for each auto turret.
    const int kMaxCandidates = 64;

    FILE* GFile = nullptr;
    unsigned int const* GTick = nullptr;
    oracle::FSimNameFn GNameOf = nullptr;
    void(__cdecl* GPreviousTick)(IBehaviorManager const*, float, int) = nullptr;

    // The behaviour tick in progress.
    unsigned int GSeq = 0;
    unsigned int GOpen = 0;
    IObjDB* GOpenDb = nullptr;
    IObjRW* GOpenSelf = nullptr;
    std::set<unsigned int> GWritten;           // objects snapshotted in this tick
    std::vector<int> GRandValues;              // rand() values drawn in this tick
    std::vector<std::string> GRandModules;     // and the DLL that drew each
    std::map<unsigned int, std::string> GLastPersonality;
    // The tick's target list and the gun groups' two missile angles, for the own ship's
    // equipment reads.
    IObjInspect const* GCandidates[kMaxCandidates] = {};
    int GCandidateCount = 0;
    float GWideConeAngle = 0.0f;
    float GMissileConeAngle = 0.0f;

    // The rand() recorder's observer (oracle_sim_rand.cpp): every draw while a behaviour
    // tick is open, with the DLL that drew it.
    std::vector<std::string> GRandHooks;

    void __cdecl OnRand(int Value, const char* Module)
    {
        if (GOpen != 0)
        {
            GRandValues.push_back(Value);
            GRandModules.push_back(Module);
        }
    }

    void Write(FOracleJson& J)
    {
        if (GFile == nullptr)
            return;
        fputs(J.Finish().c_str(), GFile);
        fputc('\n', GFile);
    }

    unsigned int Tick()
    {
        return GTick != nullptr ? *GTick : 0;
    }

    void IdAndName(FOracleJson& J, unsigned int Id)
    {
        J.Uint("id", Id);
        const std::string Name = GNameOf != nullptr ? GNameOf(Id) : std::string();
        if (!Name.empty() && Name[0] != '#')
            J.Str("obj", Name.c_str());
        else
            J.Null("obj");
    }

    void VectorJ(FOracleJson& J, const char* Name, Vector const& V)
    {
        const float Values[3] = {V.x, V.y, V.z};
        J.FloatArray(Name, Values, 3);
    }

    Vector ToVector3(float const* In)
    {
        Vector Out;
        Out.x = In[0];
        Out.y = In[1];
        Out.z = In[2];
        return Out;
    }

    // One of the own ship's items as the fire control reads it.
    struct FAiEquip
    {
        unsigned int Id;
        unsigned int Type;          // the find_equipment mask that found it
        int StatusResult;
        unsigned char Flags;
        unsigned int EquipFlags;
        int ValResult[5];           // get_equipment_val: munition kind, auto turret, seeker,
        unsigned int Val[5];        // range, target id (the value's four bytes)
        int BarrelResult;
        float BarrelPos[3];
        float BarrelDir[3];
        int LeadResult;             // get_tgt_lead_fire_pos(Id) (CEGun::ComputeTgtLeadPosition)
        float Lead[3];
        bool Pointing[3];           // is_pointing_at at the gun angle, the wide and the missile
                                    // cone angles
        int CanPointCount;          // can_point_at(candidate position, 0.3) for an auto turret
        unsigned int CanPointIds[kMaxCandidates];
        bool CanPoint[kMaxCandidates];
    };

    // One object as the recorder reads it through IObjInspect: plain data only, so that the
    // read can run under SEH (ReadObjectGuarded).
    struct FAiObject
    {
        unsigned int Id;
        unsigned int Class;
        unsigned int Type;
        int TypeResult;
        Vector Pos;
        Matrix Orient;
        Vector Vel;
        Vector AngVel;
        float Radius;
        Vector Center;
        int ExtentsResult;
        Vector ExtentsMin;
        Vector ExtentsMax;
        float Mass;
        float Hp;
        float HpMax;
        int ShieldResult;
        float Shield;
        float ShieldMax;
        bool bShieldActive;
        int ThrottleResult;
        float Throttle;
        int AxisResult;
        Vector Axis;
        int NudgeResult;
        Vector Nudge;
        int StrafeResult;
        int Strafe;
        int CruiseResult;
        bool bCruise;
        bool bTradeLane;
        int TargetResult;
        unsigned int TargetId;
        int SubtargetResult;
        unsigned short Subtarget;
        int LeaderResult;
        unsigned int LeaderId;
        int FollowResult;
        unsigned int FollowId;
        int FollowerCount;
        unsigned int Followers[kMaxFollowers];
        int DockResult;
        unsigned int DockTarget;
        int BaseResult;
        unsigned int Base;
        int RankResult;
        unsigned int Rank;
        int AffiliationResult;
        unsigned int Affiliation;
        int BehaviorId;
        int Toughness;
        bool bPlayer;
        bool bDying;
        bool bTargetable;
        int AttitudeResult;
        float Attitude;
        float ReverseAttitude;
        // P2.5 goto/face: what the thrust manager reads before it commands the engine and
        // the thrusters (find_equipment(engine) + get_equipment_status,
        // are_thrusters_active), and get_atmosphere_range.
        int EngineCount;
        unsigned short EngineId;
        int EngineStatusResult;
        unsigned char EngineFlags;
        int ThrustersResult;
        bool bThrusters;
        int AtmosphereResult;
        float AtmosphereRange;
        float MaxBankAngle;
        // P2.5 combat: CShip::get_targeted_enemy_count (-1 for an object that is not a ship).
        int TargetedEnemies;
        // The own ship only: what the fire control reads of its items, and the ship's lead.
        int bOwn;
        int LeadResult;
        float Lead[3];
        int EquipCount;
        FAiEquip Equip[kMaxEquip];
    };

    unsigned int IdOf(IObjRW const* Object)
    {
        return Object != nullptr ? Object->get_id() : 0;
    }

    void ReadOwnEquipment(IObjInspect const* O, FAiObject& Out)
    {
        Out.bOwn = 1;
        Out.LeadResult = OracleAiShipLeadFirePos(O, Out.Lead);
        static const EquipmentValType kVals[5] = {EquipValMunitionKind, EquipValAutoTurret, EquipValSeeker,
                                                  EquipValRange, EquipValTargetId};
        for (unsigned int Type : kEquipTypes)
        {
            // find_equipment answers 0 and ends the ids with a 0 when they do not fill the
            // buffer (IObjInspectImpl::find_equipment).
            unsigned short Ids[kMaxEquip] = {};
            if (O->find_equipment(Ids, kMaxEquip, Type) != 0)
                continue;
            for (int Index = 0; Index < kMaxEquip && Out.EquipCount < kMaxEquip; ++Index)
            {
                if (Ids[Index] == 0)
                    break;
                FAiEquip& E = Out.Equip[Out.EquipCount++];
                E.Id = Ids[Index];
                E.Type = Type;
                EquipStatus Status{};
                E.StatusResult = O->get_equipment_status(Status, Ids[Index]);
                E.Flags = Status.Flags;
                E.EquipFlags = Status.EquipFlags;
                for (int V = 0; V < 5; ++V)
                {
                    EquipmentVal Val{};
                    E.ValResult[V] = O->get_equipment_val(Val, Ids[Index], kVals[V], 0.0f);
                    memcpy(&E.Val[V], &Val.Value, sizeof(E.Val[V]));
                }
                E.BarrelResult = OracleAiLauncherBarrel(O, Ids[Index], E.BarrelPos, E.BarrelDir);
                Vector GunLead;
                GunLead.x = GunLead.y = GunLead.z = 0.0f;
                E.LeadResult = O->get_tgt_lead_fire_pos(Ids[Index], GunLead);
                E.Lead[0] = GunLead.x;
                E.Lead[1] = GunLead.y;
                E.Lead[2] = GunLead.z;
                const float Angles[3] = {kGunPointingAngle, GWideConeAngle, GMissileConeAngle};
                for (int A = 0; A < 3; ++A)
                {
                    bool bPointing = false;
                    O->is_pointing_at(bPointing, Ids[Index], Angles[A]);
                    E.Pointing[A] = bPointing;
                }
                // EquipValAutoTurret's byte.
                if (Type == 0x80u && E.ValResult[1] == 0 && (E.Val[1] & 0xffu) != 0)
                {
                    for (int C = 0; C < GCandidateCount; ++C)
                    {
                        if (GCandidates[C] == nullptr)
                            continue;
                        bool bCanPoint = false;
                        O->can_point_at(bCanPoint, Ids[Index], GCandidates[C]->get_position(), kTurretPointAngle);
                        E.CanPointIds[E.CanPointCount] = GCandidates[C]->get_id();
                        E.CanPoint[E.CanPointCount] = bCanPoint;
                        ++E.CanPointCount;
                    }
                }
            }
        }
    }

    void ReadObject(IObjInspect const* O, IObjInspect const* Self, FAiObject& Out)
    {
        Out.Id = O->get_id();
        CObject const* const C = O->cobject();
        Out.Class = C != nullptr ? static_cast<unsigned int>(C->ObjectClass) : 0;
        Out.TypeResult = O->get_type(Out.Type);
        Out.Pos = O->get_position();
        Out.Orient = O->get_orientation();
        Out.Vel = O->get_velocity();
        Out.AngVel = O->get_angular_velocity();
        O->get_physical_radius(Out.Radius, Out.Center);
        Out.ExtentsResult = O->get_archetype_extents(Out.ExtentsMin, Out.ExtentsMax);
        Out.Mass = O->get_mass();
        O->get_status(Out.Hp, Out.HpMax);
        Out.ShieldResult = O->get_shield_status(Out.Shield, Out.ShieldMax, Out.bShieldActive);
        Out.ThrottleResult = O->get_throttle(Out.Throttle);
        Out.AxisResult = O->get_axis_throttle(Out.Axis);
        Out.NudgeResult = O->get_nudge_vec(Out.Nudge);
        StrafeDir Strafe = StrafeNone;
        Out.StrafeResult = O->get_strafe_dir(Strafe);
        Out.Strafe = static_cast<int>(Strafe);
        Out.CruiseResult = O->is_cruise_active(Out.bCruise, Out.bTradeLane);
        IObjRW* Link = nullptr;
        Out.TargetResult = O->get_target(Link);
        Out.TargetId = IdOf(Link);
        Out.SubtargetResult = O->get_subtarget(Out.Subtarget);
        Link = nullptr;
        Out.LeaderResult = O->get_formation_leader(Link);
        Out.LeaderId = IdOf(Link);
        Link = nullptr;
        Out.FollowResult = O->get_follow_leader(Link);
        Out.FollowId = IdOf(Link);
        IObjRW* Followers[kMaxFollowers] = {};
        const int Count = O->get_formation_followers(Followers, kMaxFollowers);
        Out.FollowerCount = (Count < 0) ? 0 : ((Count > (int)kMaxFollowers) ? (int)kMaxFollowers : Count);
        for (int Index = 0; Index < Out.FollowerCount; ++Index)
            Out.Followers[Index] = IdOf(Followers[Index]);
        Out.DockResult = O->get_dock_target(Out.DockTarget);
        Out.BaseResult = O->get_base(Out.Base);
        Out.RankResult = O->get_rank(Out.Rank);
        Out.AffiliationResult = O->get_affiliation(Out.Affiliation);
        Out.BehaviorId = O->get_behavior_id();
        Out.Toughness = O->get_toughness();
        Out.bPlayer = O->is_player();
        Out.bDying = O->is_dying();
        Out.bTargetable = O->is_targetable();
        Out.EngineId = SUBOBJ_ID_NONE;
        Out.EngineCount = O->find_equipment(&Out.EngineId, 1, EquipTypeEngine);
        Out.EngineStatusResult = -1;
        if (Out.EngineId != SUBOBJ_ID_NONE)
        {
            EquipStatus Status{};
            Out.EngineStatusResult = O->get_equipment_status(Status, Out.EngineId);
            Out.EngineFlags = Status.Flags;
        }
        Out.ThrustersResult = O->are_thrusters_active(Out.bThrusters);
        Out.AtmosphereResult = O->get_atmosphere_range(Out.AtmosphereRange);
        Out.MaxBankAngle = O->get_max_bank_angle();
        Out.TargetedEnemies = OracleAiTargetedEnemyCount(O);
        if (Self == nullptr)
            ReadOwnEquipment(O, Out);
        Out.AttitudeResult = -1;
        if (Self != nullptr)
            Out.AttitudeResult = Self->get_attitude_towards_symmetrical(Out.Attitude, O, Out.ReverseAttitude);
    }

    // ReadObject under SEH; no C++ object with a destructor lives in this frame.
    DWORD ReadObjectGuarded(IObjInspect const* O, IObjInspect const* Self, FAiObject* Out)
    {
        __try
        {
            ReadObject(O, Self, *Out);
            return 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return GetExceptionCode();
        }
    }

    // `x|null`: the value when the slot answered 0.
    void OptFloat(FOracleJson& J, const char* Name, int Result, float Value)
    {
        if (Result == 0)
            J.Float(Name, Value);
        else
            J.Null(Name);
    }

    void OptVector(FOracleJson& J, const char* Name, int Result, Vector const& Value)
    {
        if (Result == 0)
            VectorJ(J, Name, Value);
        else
            J.Null(Name);
    }

    void OptUint(FOracleJson& J, const char* Name, int Result, unsigned int Value)
    {
        if (Result == 0)
            J.Uint(Name, Value);
        else
            J.Null(Name);
    }

    void WriteEquip(FOracleJson& J, FAiEquip const& E)
    {
        static const char* const kValNames[5] = {"munition", "auto_turret", "seeker", "range", "target"};
        J.BeginObject(nullptr);
        J.Uint("id", E.Id);
        J.Uint("type", E.Type);
        if (E.StatusResult == 0)
        {
            J.BeginArray("status");
            J.Uint(nullptr, E.Flags);
            J.Uint(nullptr, E.EquipFlags);
            J.EndArray();
        }
        else
        {
            J.Int("status", E.StatusResult);
        }
        for (int V = 0; V < 5; ++V)
        {
            if (E.ValResult[V] == 0)
                J.Uint(kValNames[V], E.Val[V]);
            else
                J.Null(kValNames[V]);
        }
        if (E.BarrelResult == 1)
        {
            const float Barrel[6] = {E.BarrelPos[0], E.BarrelPos[1], E.BarrelPos[2],
                                     E.BarrelDir[0], E.BarrelDir[1], E.BarrelDir[2]};
            J.FloatArray("barrel", Barrel, 6);
        }
        else
        {
            J.Null("barrel");
        }
        if (E.LeadResult == 0)
            J.FloatArray("lead", E.Lead, 3);
        else
            J.Null("lead");
        J.BeginArray("pointing");
        for (int A = 0; A < 3; ++A)
            J.Bool(nullptr, E.Pointing[A]);
        J.EndArray();
        J.BeginArray("can_point");
        for (int C = 0; C < E.CanPointCount; ++C)
        {
            J.BeginArray(nullptr);
            J.Uint(nullptr, E.CanPointIds[C]);
            J.Bool(nullptr, E.CanPoint[C]);
            J.EndArray();
        }
        J.EndArray();
        J.EndObject();
    }

    void WriteObject(IObjInspect const* O, std::vector<std::string> const& Roles)
    {
        if (O == nullptr)
            return;
        static FAiObject Obj;
        memset(&Obj, 0, sizeof(Obj));
        IObjInspect const* const Self = GOpenSelf;
        const DWORD Fault = ReadObjectGuarded(O, Self == O ? nullptr : Self, &Obj);
        GWritten.insert(Obj.Id);

        FOracleJson J;
        J.Str("rec", "ai_obj");
        J.Uint("tick", Tick());
        J.Uint("seq", GOpen);
        IdAndName(J, Obj.Id);
        J.BeginArray("roles");
        for (std::string const& Role : Roles)
            J.Str(nullptr, Role.c_str());
        J.EndArray();
        if (Fault != 0)
        {
            J.Uint("fault", (unsigned int)Fault);
            Write(J);
            return;
        }
        J.Uint("class", Obj.Class);
        OptUint(J, "type", Obj.TypeResult, Obj.Type);
        VectorJ(J, "pos", Obj.Pos);
        J.FloatArray("orient", &Obj.Orient.E[0][0], 9);
        VectorJ(J, "vel", Obj.Vel);
        VectorJ(J, "angvel", Obj.AngVel);
        J.Float("radius", Obj.Radius);
        VectorJ(J, "center", Obj.Center);
        if (Obj.ExtentsResult == 0)
        {
            const float Extents[6] = {Obj.ExtentsMin.x, Obj.ExtentsMin.y, Obj.ExtentsMin.z,
                                      Obj.ExtentsMax.x, Obj.ExtentsMax.y, Obj.ExtentsMax.z};
            J.FloatArray("extents", Extents, 6);
        }
        else
        {
            J.Null("extents");
        }
        J.Float("mass", Obj.Mass);
        J.Float("hp", Obj.Hp);
        J.Float("hp_max", Obj.HpMax);
        if (Obj.ShieldResult == 0)
        {
            J.BeginArray("shield");
            J.Float(nullptr, Obj.Shield);
            J.Float(nullptr, Obj.ShieldMax);
            J.Bool(nullptr, Obj.bShieldActive);
            J.EndArray();
        }
        else
        {
            J.Null("shield");
        }
        OptFloat(J, "throttle", Obj.ThrottleResult, Obj.Throttle);
        OptVector(J, "axis", Obj.AxisResult, Obj.Axis);
        OptVector(J, "nudge", Obj.NudgeResult, Obj.Nudge);
        if (Obj.StrafeResult == 0)
            J.Int("strafe", Obj.Strafe);
        else
            J.Null("strafe");
        if (Obj.CruiseResult == 0)
        {
            J.BeginArray("cruise");
            J.Bool(nullptr, Obj.bCruise);
            J.Bool(nullptr, Obj.bTradeLane);
            J.EndArray();
        }
        else
        {
            J.Null("cruise");
        }
        OptUint(J, "target", Obj.TargetResult == 0 && Obj.TargetId != 0 ? 0 : -1, Obj.TargetId);
        OptUint(J, "subtarget", Obj.SubtargetResult, Obj.Subtarget);
        OptUint(J, "formation_leader", Obj.LeaderResult == 0 && Obj.LeaderId != 0 ? 0 : -1, Obj.LeaderId);
        OptUint(J, "follow_leader", Obj.FollowResult == 0 && Obj.FollowId != 0 ? 0 : -1, Obj.FollowId);
        J.BeginArray("followers");
        for (int Index = 0; Index < Obj.FollowerCount; ++Index)
            J.Uint(nullptr, Obj.Followers[Index]);
        J.EndArray();
        OptUint(J, "dock_target", Obj.DockResult, Obj.DockTarget);
        OptUint(J, "base", Obj.BaseResult, Obj.Base);
        OptUint(J, "rank", Obj.RankResult, Obj.Rank);
        OptUint(J, "affiliation", Obj.AffiliationResult, Obj.Affiliation);
        J.Int("behavior_id", Obj.BehaviorId);
        J.Int("toughness", Obj.Toughness);
        J.Bool("player", Obj.bPlayer);
        J.Bool("dying", Obj.bDying);
        J.Bool("targetable", Obj.bTargetable);
        if (Obj.EngineId != SUBOBJ_ID_NONE && Obj.EngineStatusResult == 0)
        {
            J.BeginArray("engine");
            J.Uint(nullptr, Obj.EngineId);
            J.Bool(nullptr, (Obj.EngineFlags & EquipStatusFlagActive) != 0);
            J.Bool(nullptr, (Obj.EngineFlags & EquipStatusFlagDestroyed) != 0);
            J.EndArray();
        }
        else
        {
            J.Null("engine");
        }
        if (Obj.ThrustersResult == 0)
            J.Bool("thrusters", Obj.bThrusters);
        else
            J.Null("thrusters");
        OptFloat(J, "atmosphere", Obj.AtmosphereResult, Obj.AtmosphereRange);
        J.Float("max_bank", Obj.MaxBankAngle);
        if (Obj.TargetedEnemies >= 0)
            J.Int("targeted_enemies", Obj.TargetedEnemies);
        else
            J.Null("targeted_enemies");
        if (Obj.bOwn != 0)
        {
            if (Obj.LeadResult == 1)
                VectorJ(J, "lead", ToVector3(Obj.Lead));
            else
                J.Null("lead");
            J.BeginArray("equip");
            for (int Index = 0; Index < Obj.EquipCount; ++Index)
                WriteEquip(J, Obj.Equip[Index]);
            J.EndArray();
        }
        if (Obj.AttitudeResult == 0)
        {
            J.BeginArray("attitude");
            J.Float(nullptr, Obj.Attitude);
            J.Float(nullptr, Obj.ReverseAttitude);
            J.EndArray();
        }
        else
        {
            J.Null("attitude");
        }
        Write(J);
    }

    IObjRW* FindById(unsigned int Id)
    {
        if (GOpenDb == nullptr || Id == 0)
            return nullptr;
        return GOpenDb->FindById(&Id);
    }

    void SlotJ(FOracleJson& J, FOracleAiGunSlot const& S)
    {
        J.BeginArray(nullptr);
        J.Uint(nullptr, S.Id);
        J.Bool(nullptr, S.bAutoTurret != 0);
        J.Int(nullptr, S.Seeker);
        J.Bool(nullptr, S.bFireTick != 0);
        J.Int(nullptr, S.Kind);
        J.Float(nullptr, S.BurstTime);
        J.Float(nullptr, S.NoBurstTime);
        J.Float(nullptr, S.FireInterval);
        J.Float(nullptr, S.Phase);
        J.EndArray();
    }

    void SysFields(FOracleJson& J, FOracleAiManager const& M)
    {
        J.BeginObject("sys");
        FOracleAiTargeting const& T = M.Targeting;
        J.BeginObject("targeting");
        J.Int("attack_state", T.AttackState);
        J.Float("aggression", T.AggressionRatio);
        J.Float("subtarget_dist_sq", T.SubtargetDistanceSq);
        J.Uint("subtarget", T.SubtargetId);
        J.BeginArray("forced");
        for (int Index = 0; Index < T.NumForcedTargets && Index < 8; ++Index)
            J.Uint(nullptr, T.ForcedTargets[Index]);
        J.EndArray();
        J.FloatArray("anchor", T.Anchor, 3);
        J.Bool("committed", T.bCommitted != 0);
        J.Float("commit_timer", T.CommitTimer);
        J.Int("priority", T.SelectionPriority);
        J.Int("candidates_a", T.NumCandidatesA);
        J.Int("candidates_b", T.NumCandidatesB);
        J.Int("selected", T.NumSelected);
        J.Bool("selected_is_player", T.bSelectedIsPlayer != 0);
        J.Int("targeted_enemies", T.TargetedEnemyCount);
        J.Bool("over_clamp_kept", T.bOverClampKept != 0);
        J.Bool("over_clamp_dropped", T.bOverClampDropped != 0);
        J.EndObject();

        FOracleAiTimerTable const& Tt = M.Timers;
        J.BeginObject("timers");
        J.Uint("scanner", Tt.ScannerId);
        J.Float("decay_rate", Tt.DecayRate);
        J.Float("level", Tt.Level);
        J.BeginArray("timers");
        for (int Index = 0; Index < 11; ++Index)
        {
            J.BeginArray(nullptr);
            J.Bool(nullptr, Tt.TimerActive[Index] != 0);
            J.Float(nullptr, Tt.TimerValues[Index][0]);
            J.Float(nullptr, Tt.TimerValues[Index][1]);
            J.Float(nullptr, Tt.TimerValues[Index][2]);
            J.EndArray();
        }
        J.EndArray();
        J.Float("decay_scale", Tt.DecayScale);
        J.BeginArray("missiles");
        for (int Index = 0; Index < Tt.MissileCount && Index < 16; ++Index)
            J.Uint(nullptr, Tt.Missiles[Index]);
        J.EndArray();
        J.Uint("best_missile", Tt.BestMissile);
        J.Float("best_time", Tt.BestTime);
        J.EndObject();

        FOracleAiGunGroups const& G = M.Guns;
        J.BeginObject("guns");
        J.Int("last_result", G.LastGunFireResult);
        J.Uint("target", G.TargetId);
        J.Uint("hold", G.HoldMask);
        J.FloatArray("aim_offset", G.AimOffset, 3);
        J.Float("aim_timer", G.AimRefreshTimer);
        J.Float("cruise_held", G.CruiseHeldTime);
        J.Float("scan_timer", G.ScanTimer);
        J.Float("scan_period", G.ScanPeriod);
        J.BeginArray("slots");
        for (int Index = 0; Index < G.GunCount && Index < 64; ++Index)
            SlotJ(J, G.Guns[Index]);
        J.EndArray();
        J.BeginArray("mines");
        for (int Index = 0; Index < G.MineDropperCount && Index < 64; ++Index)
            SlotJ(J, G.MineDroppers[Index]);
        J.EndArray();
        J.FloatArray("params", G.Params, 19);
        J.Float("wide_cone", G.WideConeAngle);
        J.Bool("fire_all", G.bFireAllGuns != 0);
        J.Bool("enabled", G.bEnabled != 0);
        J.BeginArray("line_of_fire");
        for (int Index = 0; Index < 16; ++Index)
        {
            FOracleAiLineOfFire const& L = G.LineOfFire[Index];
            J.BeginArray(nullptr);
            J.Bool(nullptr, L.bInUse != 0);
            J.Uint(nullptr, L.TargetId);
            J.Uint(nullptr, L.Id);
            J.Float(nullptr, L.Age);
            J.Float(nullptr, L.RefreshTimer);
            J.Bool(nullptr, L.bNeedsRefresh != 0);
            J.Bool(nullptr, L.bBlocked != 0);
            J.EndArray();
        }
        J.EndArray();
        J.BeginArray("cm");
        J.Float(nullptr, G.CmTimer);
        J.Bool(nullptr, G.bCmActive != 0);
        J.BeginArray(nullptr);
        for (int Index = 0; Index < G.CmCount && Index < 8; ++Index)
            J.Uint(nullptr, G.CmIds[Index]);
        J.EndArray();
        J.EndArray();
        J.EndObject();
        J.EndObject();
    }

    void FaceFields(FOracleJson& J, FOracleAiFace const& F)
    {
        J.BeginObject("face");
        J.Int("mode", F.Mode);
        J.Int("result", F.Result);
        J.Float("dt", F.DeltaTime);
        J.Uint("flags", static_cast<unsigned int>(F.Flags28));
        J.Uint("flag29", static_cast<unsigned int>(F.Flag29));
        J.Int("kind", F.Kind);
        J.Uint("watched", F.WatchedId);
        J.FloatArray("orient", F.Orient, 9);
        J.FloatArray("target", F.Target, 3);
        J.Uint("flag_1b4", static_cast<unsigned int>(F.Flag1b4));
        J.FloatArray("steer", F.Steer, 3);
        J.Uint("flag_1c4", static_cast<unsigned int>(F.Flag1c4));
        J.Uint("flag_1c5", static_cast<unsigned int>(F.Flag1c5));
        J.Float("field_1c8", F.Field1c8);
        J.Float("field_1cc", F.Field1cc);
        J.Float("field_1d0", F.Field1d0);
        J.Uint("flag_1d4", static_cast<unsigned int>(F.Flag1d4));
        J.Float("field_1d8", F.Field1d8);
        J.EndObject();
    }

    void ManagerFields(FOracleJson& J, FOracleAiManager const& M)
    {
        J.Int("behavior", M.BehaviorIndex);
        J.Int("state_graph", M.StateGraphId);
        J.BeginArray("directive");
        for (unsigned int Word : M.Directive)
            J.Uint(nullptr, Word);
        J.EndArray();
        J.Uint("maneuver_mask", M.ManeuverMask);
        J.Bool("lock", M.bLockManeuvers != 0);
        J.Bool("dirty", M.bDirty != 0);
        J.Bool("suppressed", M.bSuppressed != 0);
        const float Timers[5] = {M.TimerA, M.TimerB, M.TimerC, M.AttackOverrideTimer, M.AttackOverridePeriod};
        J.FloatArray("timers", Timers, 5);
        J.Uint("in_sights", M.InSightsTargetId);
        J.Int("f118", M.Field118);
        J.Int("f11c", M.Field11c);
        J.Uint("f120", M.Field120);
        J.FloatArray("score_w", M.ScoreWeights, 21);
        J.BeginArray("score_b");
        for (int Behavior : M.ScoreBehaviors)
            J.Int(nullptr, Behavior);
        J.EndArray();
        // P2.5 goto/face: the pathfinder and the thrust manager's flags.
        if (M.bPathfind != 0)
        {
            J.BeginObject("pathfind");
            J.FloatArray("timers", M.PathfindTimers, 3);
            J.FloatArray("periods", M.PathfindPeriods, 3);
            J.Int("record", M.PathfindRecord);
            J.FloatArray("goal", M.PathfindGoal, 3);
            J.Float("cap", M.PathfindCap);
            J.Int("kind", M.PathfindKind);
            J.FloatArray("target", M.PathfindTarget, 3);
            J.Uint("target_id", M.PathfindTargetId);
            J.Int("reaction", M.PathfindReaction);
            J.Bool("probe_active", M.bProbeActive != 0);
            J.Bool("probe_contact", M.bProbeContact != 0);
            J.EndObject();
        }
        else
        {
            J.Null("pathfind");
        }
        J.Int("thrust", M.ThrustFlags);
        J.Int("flags_1c8", M.Flags1c8);
        if (M.bSys != 0)
            SysFields(J, M);
        else
            J.Null("sys");
        if (M.bFace != 0)
            FaceFields(J, M.Face);
        else
            J.Null("face");
    }

    void BeginTick(IBehaviorManager const* Manager, float Dt)
    {
        GOpen = ++GSeq;
        GWritten.clear();
        GRandValues.clear();
        GRandModules.clear();
        static FOracleAiManager M;
        OracleAiReadManager(Manager, M);
        GOpenDb = M.ObjectDb;
        GOpenSelf = M.Object;

        std::vector<unsigned int> TargetIds;
        for (int Index = 0; Index < M.TargetCount && Index < 64; ++Index)
            TargetIds.push_back(M.Targets[Index] != nullptr ? M.Targets[Index]->get_id() : 0);

        FOracleJson J;
        J.Str("rec", "ai_in");
        J.Uint("tick", Tick());
        J.Uint("seq", GOpen);
        IdAndName(J, M.ObjectId);
        J.Float("dt", Dt);
        ManagerFields(J, M);
        J.BeginArray("targets");
        for (unsigned int Id : TargetIds)
            J.Uint(nullptr, Id);
        J.EndArray();
        Write(J);

        if (M.bPersonality != 0)
        {
            const std::string Bytes(reinterpret_cast<const char*>(M.Personality), sizeof(M.Personality));
            std::string& Last = GLastPersonality[M.ObjectId];
            if (Last != Bytes)
            {
                Last = Bytes;
                FOracleJson P;
                P.Str("rec", "personality");
                P.Uint("tick", Tick());
                P.Uint("seq", GOpen);
                IdAndName(P, M.ObjectId);
                P.Bytes("bytes", M.Personality, sizeof(M.Personality));
                Write(P);
            }
        }

        if (M.Object == nullptr)
            return;

        // The objects the tick can read, each once, with every role it has.
        std::vector<IObjInspect const*> Order;
        std::map<IObjInspect const*, std::vector<std::string>> Roles;
        auto Add = [&](IObjInspect const* O, const char* Role) {
            if (O == nullptr)
                return;
            if (Roles.find(O) == Roles.end())
                Order.push_back(O);
            Roles[O].push_back(Role);
        };
        IObjRW* const Self = M.Object;
        Add(Self, "self");
        IObjRW* Link = nullptr;
        if (Self->get_target(Link) == 0)
            Add(Link, "target");
        Link = nullptr;
        if (Self->get_formation_leader(Link) == 0)
            Add(Link, "formation_leader");
        Link = nullptr;
        if (Self->get_follow_leader(Link) == 0)
            Add(Link, "follow_leader");
        IObjRW* Followers[kMaxFollowers] = {};
        const int Count = Self->get_formation_followers(Followers, kMaxFollowers);
        for (int Index = 0; Index < Count && Index < (int)kMaxFollowers; ++Index)
            Add(Followers[Index], "follower");
        unsigned int DockId = 0;
        if (Self->get_dock_target(DockId) == 0)
            Add(FindById(DockId), "dock_target");
        Add(FindById(M.InSightsTargetId), "in_sights");
        for (int Index = 0; Index < M.TargetCount && Index < 64; ++Index)
            Add(M.Targets[Index], "candidate");
        // P2.5 combat: the incoming missiles of the timer table (the near walk's mask leaves
        // munitions out), which UpdateSystemManagerCandidateScan and the evade behaviour read.
        if (M.bSys)
        {
            for (int Index = 0; Index < M.Timers.MissileCount && Index < 16; ++Index)
                Add(FindById(M.Timers.Missiles[Index]), "missile");
            if (M.Timers.BestMissile != 0)
                Add(FindById(M.Timers.BestMissile), "missile");
        }

        if (GOpenDb != nullptr)
        {
            Vector const SelfPos = Self->get_position();
            struct
            {
                unsigned int Cursor0;
                unsigned int Cursor1;
                unsigned int TypeMask;
            } Walk = {0, 0, kWalkMask};
            for (IObjRW* O = GOpenDb->GetNext(&Walk); O != nullptr; O = GOpenDb->GetNext(&Walk))
            {
                Vector const P = O->get_position();
                const double Dx = (double)P.x - SelfPos.x;
                const double Dy = (double)P.y - SelfPos.y;
                const double Dz = (double)P.z - SelfPos.z;
                if (Dx * Dx + Dy * Dy + Dz * Dz <= (double)kNearRadius * kNearRadius)
                    Add(O, "near");
            }
        }
        GCandidateCount = 0;
        for (int Index = 0; Index < M.TargetCount && Index < kMaxCandidates; ++Index)
            GCandidates[GCandidateCount++] = M.Targets[Index];
        GWideConeAngle = M.Guns.WideConeAngle;
        GMissileConeAngle = M.Guns.Params[11];  // +0x1068 MissileLaunchConeAngle
        for (IObjInspect const* O : Order)
            WriteObject(O, Roles[O]);
    }

    void EndTick(IBehaviorManager const* Manager)
    {
        static FOracleAiManager M;
        OracleAiReadManager(Manager, M);
        FOracleJson J;
        J.Str("rec", "ai_out");
        J.Uint("tick", Tick());
        J.Uint("seq", GOpen);
        IdAndName(J, M.ObjectId);
        ManagerFields(J, M);
        J.BeginArray("rand");
        for (int Value : GRandValues)
            J.Int(nullptr, Value);
        J.EndArray();
        J.BeginArray("rand_mod");
        for (std::string const& Module : GRandModules)
            J.Str(nullptr, Module.c_str());
        J.EndArray();
        Write(J);
        if (GFile != nullptr)
            fflush(GFile);
        GOpen = 0;
        GOpenDb = nullptr;
        GOpenSelf = nullptr;
    }

    void __cdecl OnAiBehaviorTick(IBehaviorManager const* Manager, float Dt, int Phase)
    {
        if (Phase == 0)
        {
            if (GPreviousTick != nullptr)
                GPreviousTick(Manager, Dt, Phase);
            BeginTick(Manager, Dt);
        }
        else
        {
            EndTick(Manager);
            if (GPreviousTick != nullptr)
                GPreviousTick(Manager, Dt, Phase);
        }
    }

    const char* QueryName(int Kind)
    {
        switch (Kind)
        {
        case OracleAiQueryRay:
            return "ray";
        case OracleAiQueryRayObject:
            return "ray_object";
        case OracleAiQuerySphere:
            return "sphere";
        default:
            return "unknown";
        }
    }

    void __cdecl OnAiQuery(int Kind, unsigned int SystemId, CObject const* Object, float const* From, float const* To,
                           float Radius, int MaxHits, int HitCount, FOracleAiHit const* Hits)
    {
        if (GOpen == 0)
            return;
        FOracleJson J;
        J.Str("rec", "ai_query");
        J.Uint("tick", Tick());
        J.Uint("seq", GOpen);
        J.Str("kind", QueryName(Kind));
        J.Uint("system", SystemId);
        if (Object != nullptr)
            J.Uint("object", OracleAiObjectId(Object));
        else
            J.Null("object");
        J.FloatArray("from", From, 3);
        J.FloatArray("to", To, 3);
        J.Float("radius", Radius);
        J.Int("max", MaxHits);
        J.BeginArray("hits");
        for (int Index = 0; Index < HitCount; ++Index)
        {
            J.BeginArray(nullptr);
            J.Uint(nullptr, Hits[Index].ObjectId);
            if (Kind != OracleAiQuerySphere)
            {
                J.FloatArray(nullptr, Hits[Index].Point, 3);
                J.FloatArray(nullptr, Hits[Index].Normal, 3);
                J.Uint(nullptr, Hits[Index].PartId);
            }
            J.EndArray();
        }
        J.EndArray();
        Write(J);

        // Objects the query found and the tick has not snapshotted yet.
        for (int Index = 0; Index < HitCount; ++Index)
        {
            const unsigned int Id = Hits[Index].ObjectId;
            if (Id == 0 || GWritten.count(Id) != 0)
                continue;
            IObjRW* const Found = FindById(Id);
            if (Found != nullptr)
                WriteObject(Found, std::vector<std::string>(1, "query"));
        }
    }
}

namespace oracle
{
    bool OracleSimAiStart(std::string const& OutDir, unsigned int const* Tick, FSimNameFn NameOf)
    {
        const std::string Path = OutDir + "\\ai.jsonl";
        GFile = fopen(Path.c_str(), "wb");
        if (GFile == nullptr)
            return false;
        GTick = Tick;
        GNameOf = NameOf;
        oracle::OracleSimRandHook(GRandHooks);
        oracle::OracleSimRandSetObserver(OnRand);
        GPreviousTick = GOracleSimHooks.OnBehaviorTick;
        GOracleSimHooks.OnBehaviorTick = OnAiBehaviorTick;
        GOracleAiHooks.OnQuery = OnAiQuery;

        FOracleJson J;
        J.Str("rec", "ai_start");
        J.Uint("version", kAiVersion);
        J.Float("near_radius", kNearRadius);
        J.BeginArray("rand_hooks");
        for (std::string const& Name : GRandHooks)
            J.Str(nullptr, Name.c_str());
        J.EndArray();
        Write(J);
        fflush(GFile);
        return true;
    }
}
