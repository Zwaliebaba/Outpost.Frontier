// Oracle suite of work package P2.4 (the shared remainder): the turret joints and gun
// pointing of CEGun (Common/common_cegun.cpp) over a stand-in engine. Run by
// `Oracle.exe game-sim` after the AI suites; the Rust test is object_turret in
// crates/game-sim/tests/oracle_vectors.rs, the input object_turret.list (written by
// make_game_sim_fixtures.py).
//
// CEGun reads the engine instance tree through NeuronRuntime::Engine: instance transforms,
// joint payloads (GetInstanceRelative), joint angles (Get/SetJointParams), parents and
// children. FOracleEngine answers those from a table of three instances (1 the connector,
// 2 the gun's root, 3 its child) and nothing else; it stores joint angles as given
// (the engine's limit clamp is the scene suites' business, and the Rust world's). FOracleGun
// is a CEGun whose root instance and destroyed state the suite sets, and which makes the
// protected joint walk and frame functions callable. The gun is never destroyed: its
// projectile archetype is missing, and ~CELauncher releases it without a test.

#include <windows.h>

#include "oracle_support.h"
#include "oracle_game.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "NeuronRuntime.h"
#include "neuroncomponents.h"
#include "common_archetype.h"
#include "common_cequip.h"

using namespace oracle;

namespace
{
    const int kInstances = 4;   // 0 unused, 1 connector, 2 root, 3 child
    const int kPayload = 23;    // G_EngineJointPayloadDwords

    struct FOracleInstance
    {
        float Xform[12];             // rotation rows, then the translation
        unsigned long Relative[kPayload];
        long Parent;
        float Joint;
    };

    // The stand-in engine. Every slot the gun does not use answers 0.
    struct FOracleEngine : IEngine
    {
        FOracleInstance Inst[kInstances];
        FOracleInstance Scratch;

        FOracleInstance* At(void* Instance)
        {
            const long Id = (long)(intptr_t)Instance;
            if (Id >= 1 && Id < kInstances)
                return &Inst[Id];
            return &Scratch;
        }

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            if (Out != nullptr)
                *Out = nullptr;
            return GR_GENERIC;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }

        GENRESULT NEURONCOMPONENTS_API LoadEngineComponents(INeuronComponent*) override { return 0; }
        unsigned long NEURONCOMPONENTS_API EnumerateEngineComponents(int(__cdecl*)(const char*, void*)) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API NotifyEngineComponents(void*) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API SetSearchPath(const char*) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API QuerySearchPath(char*, unsigned long) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API SetFilesystem(INeuronComponent*) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API GetSearchPath(void**) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API CreateFileSystem(const char*, void**, void*) override { return 0; }
        void* NEURONCOMPONENTS_API CreateCompoundArchetype(FEngineName*) override { return nullptr; }
        void* NEURONCOMPONENTS_API CreateArchetypeHandle(FEngineName*, void*) override { return nullptr; }
        void* NEURONCOMPONENTS_API DuplicateArchetype(void*, FEngineName*) override { return nullptr; }
        unsigned long NEURONCOMPONENTS_API AddRefHandle(void*) override { return 0; }
        unsigned long NEURONCOMPONENTS_API ReleaseHandle(void*) override { return 0; }
        FEngineName* NEURONCOMPONENTS_API GetArchetypeName(FEngineName*, void*) override { return nullptr; }
        void* NEURONCOMPONENTS_API FindArchetype(FEngineName*) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API QueryEngineComponent(int, void*, void**) override { return 0; }
        unsigned long NEURONCOMPONENTS_API IsCompoundArchetype(void*) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API EnumerateArchetypeConstraints(void*, int(__cdecl*)(void*, void*, void*, void*), void*) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API EnumerateArchetypeChildren(void*, int(__cdecl*)(void*, void*, int, void*), void*) override { return 0; }
        void* NEURONCOMPONENTS_API CreateInstanceFromName(FEngineName*, void*, void*) override { return nullptr; }
        void* NEURONCOMPONENTS_API CreateInstance(void*, void*) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API DestroyInstance(void*) override { return 0; }
        void* NEURONCOMPONENTS_API GetInstanceArchetype(void*) override { return nullptr; }
        FEngineName* NEURONCOMPONENTS_API GetInstanceName(FEngineName*, void*) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API UpdateInstance(void*, unsigned long, void*) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API SetInstanceTransform(void*, void*) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API GetInstanceTransformObject(void*, void**) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API QueryInstance(void*, const char*, void**) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API SetInstanceUserData(void*, void*) override { return 0; }
        void* NEURONCOMPONENTS_API GetInstanceUserData(void*) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API SetInstanceExtent(void*, unsigned long, float, const float*) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API GetInstanceExtent(void*, unsigned long, float*, float*) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API SetInstancePosition(void*, void*) override { return 0; }
        float* NEURONCOMPONENTS_API GetInstancePosition(void* Instance) override { return At(Instance)->Xform + 9; }
        GENRESULT NEURONCOMPONENTS_API SetInstanceOrientation(void*, void*) override { return 0; }
        float* NEURONCOMPONENTS_API GetInstanceOrientation(void* Instance) override { return At(Instance)->Xform; }
        GENRESULT NEURONCOMPONENTS_API SetInstanceMatrix(void*, void*) override { return 0; }
        float* NEURONCOMPONENTS_API GetInstanceTransform(void* Instance) override { return At(Instance)->Xform; }
        GENRESULT NEURONCOMPONENTS_API NoOp41(void*, void*) override { return 0; }
        float* NEURONCOMPONENTS_API GetIdentityMatrix(void*) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API NoOp43(void*, void*) override { return 0; }
        float* NEURONCOMPONENTS_API GetIdentityMatrix2(void*) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API SetTransformUser(void*, unsigned long) override { return 0; }
        unsigned long NEURONCOMPONENTS_API GetTransformUser(void*) override { return 0; }
        unsigned long NEURONCOMPONENTS_API CreateJoint(void*, void*, const unsigned long*) override { return 0; }
        unsigned long NEURONCOMPONENTS_API DestroyJoint(void*, void*) override { return 0; }
        unsigned long* NEURONCOMPONENTS_API GetInstanceRelative(void* Instance) override { return At(Instance)->Relative; }
        unsigned long NEURONCOMPONENTS_API SetJointParams(void* Instance, unsigned long, const float* Values) override
        {
            At(Instance)->Joint = Values[0];
            return 1;
        }
        unsigned long NEURONCOMPONENTS_API GetJointParams(void* Instance, unsigned long, float* Out) override
        {
            Out[0] = At(Instance)->Joint;
            return 1;
        }
        void* NEURONCOMPONENTS_API FindMappedInstance(void*) override { return (void*)(intptr_t)-1; }
        void* NEURONCOMPONENTS_API GetInstanceParent(void* Instance) override
        {
            return (void*)(intptr_t)At(Instance)->Parent;
        }
        void* NEURONCOMPONENTS_API GetRelatedInstance(void* From, unsigned long, void* Cursor) override
        {
            // The children of an instance, in id order (flag 1 is all the gun asks for).
            const long Parent = (long)(intptr_t)From;
            long Id = (long)(intptr_t)Cursor;
            Id = (Id < 1) ? 1 : Id + 1;
            for (; Id < kInstances; ++Id)
            {
                if (Inst[Id].Parent == Parent)
                    return (void*)(intptr_t)Id;
            }
            return (void*)(intptr_t)-1;
        }
        void* NEURONCOMPONENTS_API GetInstanceRoot(void* Instance) override { return Instance; }
        void* NEURONCOMPONENTS_API FindArchetypeChild(void*, void*) override { return nullptr; }
        unsigned long NEURONCOMPONENTS_API RejectOperation(void*, void*) override { return 0; }
    };

    struct FOracleGun : public CEGun
    {
        explicit FOracleGun(Archetype::Gun const* GunArchetype)
            : CEGun(nullptr, 0x22, GunArchetype, false), Root(-1), bOracleDestroyed(false)
        {
        }

        virtual bool IsDestroyed() const override { return bOracleDestroyed; }
        virtual long GetRootIndex() const override { return Root; }

        using CEGun::ComputeTurretFrame;
        using CEGun::DecomposeJointMotion;
        using CEGun::LocateJoints;

        long Root;
        bool bOracleDestroyed;
    };

    std::vector<std::string> Words(const std::string& Line)
    {
        std::vector<std::string> Out;
        size_t Start = 0;
        while (Start <= Line.size())
        {
            const size_t End = Line.find(' ', Start);
            const std::string Word = Line.substr(Start, End == std::string::npos ? std::string::npos : End - Start);
            if (!Word.empty())
                Out.push_back(Word);
            if (End == std::string::npos)
                break;
            Start = End + 1;
        }
        return Out;
    }

    std::string Fl(const float* Values, int Count)
    {
        std::string Out;
        for (int Index = 0; Index < Count; ++Index)
        {
            if (Index != 0)
                Out += ",";
            Out += GameF(Values[Index]);
        }
        return Out;
    }

    // One instance's words: type, transform (12), axis (3), rotation (9), min, max, child
    // position (3), joint angle; 31 words.
    void ReadInstance(FOracleInstance& I, const unsigned* W, long Parent)
    {
        memset(&I, 0, sizeof(I));
        I.Relative[0] = W[0];
        for (int Index = 0; Index < 12; ++Index)
            I.Xform[Index] = FloatFromBits(W[1 + Index]);
        for (int Index = 0; Index < 3; ++Index)
            I.Relative[16 + Index] = W[13 + Index];
        for (int Index = 0; Index < 9; ++Index)
            I.Relative[7 + Index] = W[16 + Index];
        I.Relative[19] = W[25];
        I.Relative[20] = W[26];
        for (int Index = 0; Index < 3; ++Index)
            I.Relative[4 + Index] = W[27 + Index];
        I.Joint = FloatFromBits(W[30]);
        I.Parent = Parent;
    }

    std::string JointWords(CEGun::JointChannel const& J)
    {
        if (J.EngineIndex == -1)
            return "-";
        return Format("%d,%d,", J.EngineIndex, J.bLimited) + GameF(J.Current);
    }

    std::string TickWords(CEGun::JointChannel& J, float Desired, float Dt)
    {
        if (J.EngineIndex == -1)
            return "-";
        J.Desired = Desired;
        J.Tick(Dt);
        return GameF(J.Current) + Format(",%d", J.bSettled);
    }

    void TurretSuite()
    {
        Suite Out("object_turret.jsonl");
        static FOracleEngine Engine;
        IEngine* const Saved = NeuronRuntime::Engine;
        NeuronRuntime::Engine = &Engine;

        int Case = 0;
        for (const std::string& Line : InputLines("object_turret.list"))
        {
            const std::vector<std::string> Parts = Words(Line);
            if (Parts.size() != 82)
            {
                fprintf(stderr, "object_turret.list: a line without 82 words\n");
                GFailed = true;
                break;
            }
            unsigned W[82];
            for (int Index = 0; Index < 82; ++Index)
                W[Index] = (unsigned)strtoul(Parts[Index].c_str(), nullptr, 16);

            // 1 the connector (only its orientation), 2 the root on it, 3 the root's child.
            memset(&Engine.Inst[1], 0, sizeof(Engine.Inst[1]));
            for (int Index = 0; Index < 9; ++Index)
                Engine.Inst[1].Xform[Index] = FloatFromBits(W[Index]);
            Engine.Inst[1].Parent = 0;
            ReadInstance(Engine.Inst[2], W + 9, 1);
            ReadInstance(Engine.Inst[3], W + 40, 2);
            memset(&Engine.Scratch, 0, sizeof(Engine.Scratch));
            Engine.Scratch.Parent = -2;

            Vector Barrel;
            Barrel.x = FloatFromBits(W[71]);
            Barrel.y = FloatFromBits(W[72]);
            Barrel.z = FloatFromBits(W[73]);
            Vector Point;
            Point.x = FloatFromBits(W[74]);
            Point.y = FloatFromBits(W[75]);
            Point.z = FloatFromBits(W[76]);
            const float Angle = FloatFromBits(W[77]);
            const float TurnRate = FloatFromBits(W[78]);
            const float Dt = FloatFromBits(W[79]);
            const float DesiredYaw = FloatFromBits(W[80]);
            const float DesiredPitch = FloatFromBits(W[81]);

            Archetype::Gun* const Arch = new Archetype::Gun();
            Arch->EngineIndex = 0;   // load() then has no file to read
            Arch->TurnRate = TurnRate;
            FOracleGun* const Gun = new FOracleGun(Arch);
            Gun->ConnectionIndex = 1;
            Gun->Root = 2;
            Gun->Barrels[0].EngineIndex = 3;
            Gun->Barrels[0].Position = Barrel;
            Gun->Barrels[0].Orient = Matrix(1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f);
            Gun->LocateJoints();

            FLine L;
            L.Add("case", Format("%d", Case++));
            L.Add("yaw", JointWords(Gun->Yaw));
            L.Add("pitch", JointWords(Gun->Pitch));
            L.Add("frame", Fl(&Gun->TurretFrame.E[0][0], 9));
            const Vector Origin = Gun->GetTurretOrigin();
            const float OriginWords[3] = {Origin.x, Origin.y, Origin.z};
            L.Add("origin", Fl(OriginWords, 3));
            Vector Dir;
            Dir.x = Point.x - Origin.x;
            Dir.y = Point.y - Origin.y;
            Dir.z = Point.z - Origin.z;
            float Yaw = 0.0f;
            float Pitch = 0.0f;
            Gun->DecomposeJointMotion(Dir, Yaw, Pitch);
            const float Angles[2] = {Yaw, Pitch};
            L.Add("angles", Fl(Angles, 2));
            L.Add("point", Gun->CanPointAt(Point, Angle) ? "1" : "0");
            L.Add("see", Gun->CanSeeTargetPoint(Point, Angle) ? "1" : "0");
            L.Add("tick", TickWords(Gun->Yaw, DesiredYaw, Dt) + "|" + TickWords(Gun->Pitch, DesiredPitch, Dt));
            Gun->bOracleDestroyed = true;
            L.Add("destroyed", Gun->CanPointAt(Point, Angle) ? "1" : "0");
            Out.Line(L.Done());
            // The gun and its archetype are left alive (see the file comment).
        }

        NeuronRuntime::Engine = Saved;
    }

    typedef void (*FTurretSuite)();

    DWORD GuardedTurretSuiteCall(FTurretSuite Run)
    {
        __try
        {
            Run();
            return 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return GetExceptionCode();
        }
    }
}

void RunTurretSuites()
{
    // CEGun multiplies and transforms through the 3DMathEngine (Acquire3DMathEngine).
    if (!OracleLoadMathEngine())
    {
        fprintf(stderr, "game-sim: no 3DMathEngine for the turret suite\n");
        GFailed = true;
        return;
    }
    const DWORD Code = GuardedTurretSuiteCall(TurretSuite);
    fflush(stdout);
    if (Code != 0)
    {
        fprintf(stderr, "game-sim object_turret: exception %08lx\n", static_cast<unsigned long>(Code));
        GFailed = true;
    }
}
