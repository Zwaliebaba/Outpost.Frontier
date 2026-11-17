// Oracle suites of work package P2.3 (physics): the exact pieces of Common's physics glue,
// called through Common.dll's exports.
//
//   physics_damage  Collision::Load, GetImpactDamageFromType and both CalcCollisionDamage
//                   overloads (COMMON_API, common_physys.h; COLLISION_DAMAGE_FACTOR is
//                   COMMON_API data, common_misc.h)
//   physics_drv     CDeadReckonedVector (SHARED_INLINE_API, common_cobject.h)
//   physics_remote  CRemotePhysicsSimulation (SHARED_INLINE_API, common_server.h); its
//                   RemoteUpdate, Doit and CheckForSync reach x86math.dll's 3DMathEngine
//                   through Common's Acquire3DMathEngine, so the suite loads it first
//                   (OracleLoadMathEngine, oracle_scene.cpp)
//
// Only the out-of-line members (Common only) and the header-inline ones are called; the
// objects live on the stack, so nothing is freed across a CRT boundary. Inputs are in
// physics/ under the fixture directory, written by make_physics_fixtures.py; the Rust test
// crates/neuron-core/tests/oracle_physics.rs renders the same lines. Numbers are read and
// written as bit patterns with memcpy, never through the FPU, and every NaN is written as
// one pattern (NaN payloads are not part of the port).
//
// Two harness choices mirror the port where the reference is undefined (divergence D94):
// CRemotePhysicsSimulation's constructor leaves m_Orientation (and m_bLogging) unset, so
// the suite sets the identity (and false) after construction.

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "oracle_support.h"

#include "neuroncomponents.h"
#include "common_archetype.h"
#include "common_cequip.h"
#include "common_cobject.h"            // CDeadReckonedVector, common_physys.h (Collision)
#include "common_csys.h"
#include "common_ini.h"                // INI_Reader, Vector
#include "common_misc.h"               // COLLISION_DAMAGE_FACTOR
#include "common_server.h"             // CRemotePhysicsSimulation

using namespace oracle;

namespace
{
    std::vector<std::string> Split(const std::string& Text)
    {
        std::vector<std::string> Parts;
        size_t Start = 0;
        for (;;)
        {
            const size_t Space = Text.find(' ', Start);
            Parts.push_back(Text.substr(Start, Space == std::string::npos ? std::string::npos : Space - Start));
            if (Space == std::string::npos)
                return Parts;
            Start = Space + 1;
        }
    }

    bool Expect(const std::string& Line, const std::vector<std::string>& Words, size_t Count)
    {
        if (Words.size() == Count)
            return true;
        fprintf(stderr, "physics: bad input: %s\n", Line.c_str());
        GFailed = true;
        return false;
    }

    unsigned int U32In(const std::string& Text)
    {
        return (unsigned int)strtoul(Text.c_str(), nullptr, 16);
    }

    void F32In(float* Out, const std::string& Text)
    {
        const unsigned int Bits = U32In(Text);
        memcpy(Out, &Bits, sizeof(Bits));
    }

    void F64In(double* Out, const std::string& Text)
    {
        const unsigned long long Bits = _strtoui64(Text.c_str(), nullptr, 16);
        memcpy(Out, &Bits, sizeof(Bits));
    }

    // Three f32 words from Words[At].
    void VecIn(Vector* Out, const std::vector<std::string>& Words, size_t At)
    {
        F32In(&Out->x, Words[At]);
        F32In(&Out->y, Words[At + 1]);
        F32In(&Out->z, Words[At + 2]);
    }

    // Four f32 words (W X Y Z) from Words[At].
    void QuatIn(Quaternion* Out, const std::vector<std::string>& Words, size_t At)
    {
        F32In(&Out->w, Words[At]);
        F32In(&Out->x, Words[At + 1]);
        F32In(&Out->y, Words[At + 2]);
        F32In(&Out->z, Words[At + 3]);
    }

    // An f32 in memory as eight hex digits; every NaN as 7fc00000.
    std::string H32(const float* Value)
    {
        unsigned int Bits;
        memcpy(&Bits, Value, sizeof(Bits));
        if ((Bits & 0x7F800000u) == 0x7F800000u && (Bits & 0x007FFFFFu) != 0)
            Bits = 0x7FC00000u;
        return Format("%08x", Bits);
    }

    // An f64 in memory as sixteen hex digits; every NaN as 7ff8000000000000.
    std::string H64(const double* Value)
    {
        unsigned long long Bits;
        memcpy(&Bits, Value, sizeof(Bits));
        if ((Bits & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (Bits & 0x000FFFFFFFFFFFFFull) != 0)
            Bits = 0x7FF8000000000000ull;
        return Format("%016llx", Bits);
    }

    std::string HV(const Vector& Value)
    {
        return H32(&Value.x) + H32(&Value.y) + H32(&Value.z);
    }

    std::string HQ(const Quaternion& Value)
    {
        return H32(&Value.w) + H32(&Value.x) + H32(&Value.y) + H32(&Value.z);
    }

    // The lines of an input file cut into sequences, each starting at a "seq" line.
    std::vector<std::vector<std::string> > Sequences(const char* Name)
    {
        std::vector<std::vector<std::string> > Out;
        for (const std::string& Line : InputLines(Name))
        {
            if (Line.compare(0, 3, "seq") == 0)
                Out.push_back(std::vector<std::string>());
            if (Out.empty())
            {
                fprintf(stderr, "physics: %s: a call before the first seq\n", Name);
                GFailed = true;
                continue;
            }
            Out.back().push_back(Line);
        }
        return Out;
    }

    // --- physics_damage ---------------------------------------------------------------

    void DamageSuite()
    {
        Suite Out("physics_damage.jsonl");
        const float SavedFactor = COLLISION_DAMAGE_FACTOR;
        for (const std::string& Line : InputLines("physics/damage.txt"))
        {
            const std::vector<std::string> Words = Split(Line);
            std::string Result;
            if (Words[0] == "load")
            {
                if (!Expect(Line, Words, 2))
                    continue;
                Collision::Free();
                Bytes Contents;
                if (!ReadWholeFile(Words[1], Contents))
                {
                    fprintf(stderr, "physics: cannot read %s\n", Words[1].c_str());
                    GFailed = true;
                    continue;
                }
                std::string Ok;
                INI_Reader* Reader = new INI_Reader();
                if (Reader->open_memory(Contents.empty() ? nullptr : (const char*)Contents.data(),
                                        (unsigned int)Contents.size()))
                {
                    while (Reader->read_header())
                        Ok += Collision::Load(*Reader) ? "1" : "0";
                }
                delete Reader;
                Result = "\"ok\":\"" + Ok + "\"";
            }
            else if (Words[0] == "type")
            {
                if (!Expect(Line, Words, 2))
                    continue;
                const float Damage = Collision::GetImpactDamageFromType(U32In(Words[1]));
                Result = "\"damage\":\"" + H32(&Damage) + "\"";
            }
            else if (Words[0] == "calc")
            {
                if (!Expect(Line, Words, 8))
                    continue;
                Vector RelVel;
                Vector Normal;
                VecIn(&RelVel, Words, 2);
                VecIn(&Normal, Words, 5);
                const float Damage = Collision::CalcCollisionDamage(U32In(Words[1]), RelVel, Normal);
                Result = "\"damage\":\"" + H32(&Damage) + "\"";
            }
            else if (Words[0] == "scaled")
            {
                if (!Expect(Line, Words, 5))
                    continue;
                float Factor;
                float DamageFactor;
                float MaxSpeed;
                float ActualSpeed;
                F32In(&Factor, Words[1]);
                F32In(&DamageFactor, Words[2]);
                F32In(&MaxSpeed, Words[3]);
                F32In(&ActualSpeed, Words[4]);
                COLLISION_DAMAGE_FACTOR = Factor;
                const float Damage = Collision::CalcCollisionDamage(DamageFactor, MaxSpeed, ActualSpeed);
                Result = "\"damage\":\"" + H32(&Damage) + "\"";
            }
            else
            {
                fprintf(stderr, "physics: bad damage input: %s\n", Line.c_str());
                GFailed = true;
                continue;
            }
            Out.Line("{\"in\":\"" + Line + "\"," + Result + "}");
        }
        COLLISION_DAMAGE_FACTOR = SavedFactor;
        Collision::Free();
    }

    // --- physics_drv ------------------------------------------------------------------

    std::string DrvLine(const std::string& Line, CDeadReckonedVector& Drv, double ProbeT,
                        const Vector& ProbeP)
    {
        std::string Samples;
        std::string Stamps;
        for (int Index = 0; Index < 3; ++Index)
        {
            Samples += HV(Drv.Samples[Index]);
            Stamps += H64(&Drv.Timestamps[Index]);
        }
        const Vector Projected = Drv.ProjectVelocity(ProbeT);
        const double Error = Drv.CalcError(ProbeP);
        const double Window = Drv.ConvergeTime();
        return "{\"in\":\"" + Line + "\",\"samples\":\"" + Samples + "\",\"stamps\":\"" + Stamps +
               "\",\"base\":\"" + HV(Drv.BasePosition) + "\",\"blend_base\":\"" + HV(Drv.BlendBasePosition) +
               "\",\"projected\":\"" + HV(Drv.ProjectedPosition) + "\",\"tracked\":\"" + HV(Drv.TrackedPosition) +
               "\",\"velocity\":\"" + HV(Drv.Velocity) + "\",\"velocity2\":\"" + HV(Drv.Velocity2) +
               "\",\"converged\":\"" + HV(Drv.ConvergedVelocity) + "\",\"accel\":\"" + HV(Drv.Acceleration) +
               "\",\"accel2\":\"" + HV(Drv.Acceleration2) + "\",\"count\":\"" +
               Format("%08x", (unsigned int)Drv.SampleCount) + "\",\"window\":\"" + H64(&Window) +
               "\",\"elapsed\":\"" + H64(&Drv.ElapsedTime) + "\",\"max_corrective\":\"" +
               H64(&Drv.MaxCorrectiveTime) + "\",\"project_velocity\":\"" + HV(Projected) + "\",\"error\":\"" +
               H64(&Error) + "\"}";
    }

    void DrvSuite()
    {
        Suite Out("physics_drv.jsonl");
        const std::vector<std::vector<std::string> > All = Sequences("physics/drv.txt");
        for (size_t Seq = 0; Seq < All.size(); ++Seq)
        {
            const std::vector<std::string>& Calls = All[Seq];
            CDeadReckonedVector Drv;
            double ProbeT = 0.0;
            Vector ProbeP;
            ProbeP.x = 0.0f;
            ProbeP.y = 0.0f;
            ProbeP.z = 0.0f;
            for (size_t Index = 0; Index < Calls.size(); ++Index)
            {
                const std::string& Line = Calls[Index];
                const std::vector<std::string> Words = Split(Line);
                if (Words[0] == "seq")
                {
                    if (!Expect(Line, Words, 5))
                        continue;
                    F64In(&ProbeT, Words[1]);
                    VecIn(&ProbeP, Words, 2);
                }
                else if (Words[0] == "add")
                {
                    if (!Expect(Line, Words, 5))
                        continue;
                    Vector Pos;
                    double Timestamp;
                    VecIn(&Pos, Words, 1);
                    F64In(&Timestamp, Words[4]);
                    Drv.AddSample(Pos, Timestamp);
                }
                else if (Words[0] == "doit" || Words[0] == "maxc")
                {
                    if (!Expect(Line, Words, 2))
                        continue;
                    double Value;
                    F64In(&Value, Words[1]);
                    if (Words[0] == "doit")
                        Drv.Doit(Value);
                    else
                        Drv.SetMaxCorrectiveConvergeTime(Value);
                }
                else if (Words[0] == "reset")
                {
                    Drv.Reset();
                }
                else if (Words[0] == "normp")
                {
                    Drv.NormalizeProjectedPosition();
                }
                else if (Words[0] == "normt")
                {
                    Drv.NormalizeTrackedPosition();
                }
                else
                {
                    fprintf(stderr, "physics: bad drv input: %s\n", Line.c_str());
                    GFailed = true;
                    continue;
                }
                Out.Line(DrvLine(Line, Drv, ProbeT, ProbeP));
            }
        }
    }

    // --- physics_remote ---------------------------------------------------------------

    void RemoteSuite()
    {
        Suite Out("physics_remote.jsonl");
        if (!OracleLoadMathEngine())
        {
            fprintf(stderr, "physics: no 3DMathEngine, physics_remote skipped\n");
            GFailed = true;
            return;
        }
        const std::vector<std::vector<std::string> > All = Sequences("physics/remote.txt");
        for (size_t Seq = 0; Seq < All.size(); ++Seq)
        {
            const std::vector<std::string>& Calls = All[Seq];
            CRemotePhysicsSimulation Sim;
            // Divergence D94: the port starts at the identity; the constructor leaves both unset.
            Sim.m_Orientation.w = 1.0f;
            Sim.m_Orientation.x = 0.0f;
            Sim.m_Orientation.y = 0.0f;
            Sim.m_Orientation.z = 0.0f;
            Sim.m_bLogging = false;
            for (size_t Index = 0; Index < Calls.size(); ++Index)
            {
                const std::string& Line = Calls[Index];
                const std::vector<std::string> Words = Split(Line);
                const char* SyncResult = "-";
                if (Words[0] == "seq")
                {
                    if (!Expect(Line, Words, 1))
                        continue;
                }
                else if (Words[0] == "update")
                {
                    if (!Expect(Line, Words, 9))
                        continue;
                    Vector Pos;
                    Quaternion Orient;
                    double Timestamp;
                    VecIn(&Pos, Words, 1);
                    QuatIn(&Orient, Words, 4);
                    F64In(&Timestamp, Words[8]);
                    Sim.RemoteUpdate(Pos, Orient, Timestamp);
                }
                else if (Words[0] == "doit")
                {
                    if (!Expect(Line, Words, 2))
                        continue;
                    double Dt;
                    F64In(&Dt, Words[1]);
                    Sim.Doit(Dt);
                }
                else if (Words[0] == "sync")
                {
                    if (!Expect(Line, Words, 11))
                        continue;
                    Vector P1;
                    Vector P2;
                    Quaternion Orient;
                    VecIn(&P1, Words, 1);
                    VecIn(&P2, Words, 4);
                    QuatIn(&Orient, Words, 7);
                    SyncResult = Sim.CheckForSync(P1, P2, Orient) ? "1" : "0";
                }
                else if (Words[0] == "reset")
                {
                    Sim.Reset();
                }
                else
                {
                    fprintf(stderr, "physics: bad remote input: %s\n", Line.c_str());
                    GFailed = true;
                    continue;
                }
                Out.Line("{\"in\":\"" + Line + "\",\"velocity\":\"" + HV(Sim.m_Velocity) + "\",\"blend_from\":\"" +
                         HV(Sim.m_BlendFromVelocity) + "\",\"blend_to\":\"" + HV(Sim.m_BlendToVelocity) +
                         "\",\"orientation\":\"" + HQ(Sim.m_Orientation) + "\",\"projected\":\"" +
                         HV(Sim.m_ProjectedPosition) + "\",\"count\":\"" +
                         Format("%08x", (unsigned int)Sim.m_SampleCount) + "\",\"time\":\"" +
                         H64(&Sim.m_SimulationTime) + "\",\"window\":\"" + H64(&Sim.m_ConvergeWindow) +
                         "\",\"backoff\":\"" + H64(&Sim.m_SyncBackoff) + "\",\"pos_tracked\":\"" +
                         HV(Sim.m_PositionDRV.TrackedPosition) + "\",\"x_tracked\":\"" +
                         HV(Sim.m_OrientAxisXDRV.TrackedPosition) + "\",\"y_tracked\":\"" +
                         HV(Sim.m_OrientAxisYDRV.TrackedPosition) + "\",\"sync\":\"" + SyncResult + "\"}");
            }
        }
    }
}

void RunPhysicsSuites()
{
    DamageSuite();
    DrvSuite();
    RemoteSuite();
}
