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
    std::vector<int> GRandModules;             // and the DLL that drew each
    std::map<unsigned int, std::string> GLastPersonality;

    // The DLLs whose rand import is replaced.
    enum ERandModule
    {
        RandCommon = 0,
        RandServer = 1,
        RandContent = 2,
        RandModuleCount = 3
    };
    const char* const kModuleNames[RandModuleCount] = {"Common.dll", "Server.dll", "Content.dll"};
    typedef int(__cdecl* FRandFn)();
    FRandFn GRealRand[RandModuleCount] = {};
    bool GRandHooked[RandModuleCount] = {};

    template <int Module>
    int __cdecl CountingRand()
    {
        const int Value = GRealRand[Module]();
        if (GOpen != 0)
        {
            GRandValues.push_back(Value);
            GRandModules.push_back(Module);
        }
        return Value;
    }

    // Replaces Module's import of Name (by name, from any DLL) with Replacement; the old
    // pointer goes to Original. The IAT is the module's own, so only its calls are seen.
    bool PatchImport(HMODULE Module, const char* Name, void* Replacement, void** Original)
    {
        if (Module == nullptr)
            return false;
        BYTE* const Base = reinterpret_cast<BYTE*>(Module);
        IMAGE_DOS_HEADER const* const Dos = reinterpret_cast<IMAGE_DOS_HEADER const*>(Base);
        if (Dos->e_magic != IMAGE_DOS_SIGNATURE)
            return false;
        IMAGE_NT_HEADERS const* const Nt = reinterpret_cast<IMAGE_NT_HEADERS const*>(Base + Dos->e_lfanew);
        IMAGE_DATA_DIRECTORY const& Dir = Nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (Dir.VirtualAddress == 0)
            return false;
        IMAGE_IMPORT_DESCRIPTOR const* Desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR const*>(Base + Dir.VirtualAddress);
        for (; Desc->Name != 0; ++Desc)
        {
            if (Desc->OriginalFirstThunk == 0)
                continue;
            IMAGE_THUNK_DATA const* Names = reinterpret_cast<IMAGE_THUNK_DATA const*>(Base + Desc->OriginalFirstThunk);
            IMAGE_THUNK_DATA* Slots = reinterpret_cast<IMAGE_THUNK_DATA*>(Base + Desc->FirstThunk);
            for (; Names->u1.AddressOfData != 0; ++Names, ++Slots)
            {
                if (IMAGE_SNAP_BY_ORDINAL(Names->u1.Ordinal))
                    continue;
                IMAGE_IMPORT_BY_NAME const* const ByName =
                    reinterpret_cast<IMAGE_IMPORT_BY_NAME const*>(Base + Names->u1.AddressOfData);
                if (strcmp(reinterpret_cast<const char*>(ByName->Name), Name) != 0)
                    continue;
                DWORD Old = 0;
                if (!VirtualProtect(&Slots->u1.Function, sizeof(Slots->u1.Function), PAGE_READWRITE, &Old))
                    return false;
                *Original = reinterpret_cast<void*>(Slots->u1.Function);
                Slots->u1.Function = reinterpret_cast<ULONG_PTR>(Replacement);
                VirtualProtect(&Slots->u1.Function, sizeof(Slots->u1.Function), Old, &Old);
                return true;
            }
        }
        return false;
    }

    void HookRand()
    {
        void* const Wrappers[RandModuleCount] = {reinterpret_cast<void*>(&CountingRand<RandCommon>),
                                                 reinterpret_cast<void*>(&CountingRand<RandServer>),
                                                 reinterpret_cast<void*>(&CountingRand<RandContent>)};
        for (int Index = 0; Index < RandModuleCount; ++Index)
        {
            void* Original = nullptr;
            GRandHooked[Index] = PatchImport(GetModuleHandleA(kModuleNames[Index]), "rand", Wrappers[Index], &Original);
            if (GRandHooked[Index])
                GRealRand[Index] = reinterpret_cast<FRandFn>(Original);
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
    };

    unsigned int IdOf(IObjRW const* Object)
    {
        return Object != nullptr ? Object->get_id() : 0;
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

    void WriteObject(IObjInspect const* O, std::vector<std::string> const& Roles)
    {
        if (O == nullptr)
            return;
        FAiObject Obj;
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
    }

    void BeginTick(IBehaviorManager const* Manager, float Dt)
    {
        GOpen = ++GSeq;
        GWritten.clear();
        GRandValues.clear();
        GRandModules.clear();
        FOracleAiManager M;
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
        for (IObjInspect const* O : Order)
            WriteObject(O, Roles[O]);
    }

    void EndTick(IBehaviorManager const* Manager)
    {
        FOracleAiManager M;
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
        for (int Module : GRandModules)
            J.Str(nullptr, kModuleNames[Module]);
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
        HookRand();
        GPreviousTick = GOracleSimHooks.OnBehaviorTick;
        GOracleSimHooks.OnBehaviorTick = OnAiBehaviorTick;
        GOracleAiHooks.OnQuery = OnAiQuery;

        FOracleJson J;
        J.Str("rec", "ai_start");
        J.Uint("version", kAiVersion);
        J.Float("near_radius", kNearRadius);
        J.BeginArray("rand_hooks");
        for (int Index = 0; Index < RandModuleCount; ++Index)
        {
            if (GRandHooked[Index])
                J.Str(nullptr, kModuleNames[Index]);
        }
        J.EndArray();
        Write(J);
        fflush(GFile);
        return true;
    }
}
