// Author: Giperion.

// IVP friction pass -- Common.dll module storage-0635558b and
// (2026-10-08, claude-code:cmn-phy2).

// IVP_Friction_Solver (the stack object built by the pass) and the friction
// system's own methods (IVP_ListManager = Ipion's IVP_Friction_System). The
// linear algebra they drive is ivp_great_matrix.cpp (Gauss sub-solver and the
// LCP solver, module storage-063682f2).

// x87: baseline keeps intermediates on the FPU stack (53-bit precision under the
// VC6 runtime). Every value kept on the stack is carried in `double` here and
// cast to float exactly where baseline does FST/FSTP float; every comparison
// states the FNSTSW mask, the jump and the side it takes, so NaN takes the same
// branch as in baseline.

#include "ivp_friction_solver.h"
#include "ivp_environment.h"
#include "ivp_controller.h"
#include "ivp_mindist_settings.h"
#include "ivp_simulation_unit.h"
#include "../ivp_utility/ivp_core_trap.h"
#include "../ivp_collision/ivp_listmanager_topology.h" // IVP_UnlinkPairRecordFromObjects (the routine), IVP_UpdatePairRecordContactFrame

#include <cmath>
#include <cstdint>
#include <cstring>

namespace
{
    // incremented once per friction pass (module storage `INC dword ptr`),
    // its only reference in the listing.
    int32_t G_IVP_FrictionPassCount = 0;

    unsigned IVP_FrictionKey(const IVP_ListManager* FrictionSystem)
    {
        return static_cast<unsigned>(reinterpret_cast<uintptr_t>(FrictionSystem));
    }

    // (this = A; B): (A.z*B.z + A.y*B.y) + A.x*B.x, left in ST0.
    double IVP_Dot(const Vector& A, const Vector& B)
    {
        const double Z = (double)(B.z) * (double)(A.z);
        const double Y = (double)(B.y) * (double)(A.y);
        const double X = (double)(B.x) * (double)(A.x);
        return (Z + Y) + X;
    }

    // (this = Dst; Src, Scale): Dst = Src * Scale (Scale is a float argument).
    void IVP_SetMultiple(Vector& Dst, const Vector& Src, float Scale)
    {
        const float Y = (float)((double)(Scale) * (double)(Src.y));
        const float Z = (float)((double)(Scale) * (double)(Src.z));
        const float X = (float)((double)(Scale) * (double)(Src.x));
        Dst.x = X;
        Dst.y = Y;
        Dst.z = Z;
    }

    // (this = Dst; A, B, Scale): Dst = A + B * Scale, all three
    // components loaded before the first store.
    void IVP_AddMultiple(Vector& Dst, const Vector& A, const Vector& B, float Scale)
    {
        const float Y = (float)((double)(Scale) * (double)(B.y) + (double)(A.y));
        const float Z = (float)((double)(Scale) * (double)(B.z) + (double)(A.z));
        const float X = (float)((double)(Scale) * (double)(B.x) + (double)(A.x));
        Dst.x = X;
        Dst.y = Y;
        Dst.z = Z;
    }

    // (this = Out; A, B, RET 8): Out = A (x) B componentwise.
    void IVP_SetPairwiseMult(Vector& Out, const Vector& A, const Vector& B)
    {
        const float Y = (float)((double)(A.y) * (double)(B.y));
        const float Z = (float)((double)(A.z) * (double)(B.z));
        const float X = (float)((double)(A.x) * (double)(B.x));
        Out.z = Z;
        Out.x = X;
        Out.y = Y;
    }

    // (this = Dst; V): Dst = -V, all loads before the stores.
    void IVP_SetNegative(Vector& Dst, const Vector& V)
    {
        const float Y = -V.y;
        const float Z = -V.z;
        const float X = -V.x;
        Dst.x = X;
        Dst.z = Z;
        Dst.y = Y;
    }

    // (cdecl, 4 args): dot(LongTerm->CoordOSCrossNormal[Side], RotArm)
    // + dot(LongTerm->ContactNormalWS, LinArm), in baseline's association order:
    //   ((Cz*Rz + Cy*Ry) + Cx*Rx) + ((Nz*Lz + Ny*Ly) + Nx*Lx)
    double IVP_CrossAndNormalDot(const IVP_Impact_Solver_Long_Term* LongTerm, int Side, const Vector& RotArm, const Vector& LinArm)
    {
        const Vector& Cross = LongTerm->CoordOSCrossNormal[Side];
        const Vector& Normal = LongTerm->ContactNormalWS;
        const double CrossPart = ((double)(Cross.z) * (double)(RotArm.z) +
                                  (double)(Cross.y) * (double)(RotArm.y)) +
                                 (double)(Cross.x) * (double)(RotArm.x);
        const double NormalPart = ((double)(Normal.z) * (double)(LinArm.z) +
                                   (double)(Normal.y) * (double)(LinArm.y)) +
                                  (double)(Normal.x) * (double)(LinArm.x);
        return CrossPart + NormalPart;
    }
}

// ============================================================================
// IVP_Friction_Solver
// ============================================================================

// (RET 8; SEH frame guards the inline vector at +0x24).
IVP_Friction_Solver::IVP_Friction_Solver(IVP_ListManager* FrictionSystem, const IVP_Event_Sim* EventSimIn)
{
    // DistChangeMat ctor (06366400); 063553a4-063553b2: DistsOfSystem
    // (elems = this+0x2c, memsize 0x200, n_elems 0) -- both member constructors.
    Environment = FrictionSystem->Environment;
    EventSim = EventSimIn;

    // MOVSX both counts.
    const int Columns = static_cast<int>(static_cast<int16_t>(FrictionSystem->CtxListCount)) -
                        static_cast<int>(FrictionSystem->ComplexNotNecessaryNumber);
    DistChangeMat.Columns = Columns;
    DistChangeMat.AlignedRowLen = (Columns + 3) & ~3;

    // , 063553f0-06355471: three arena allocations, in this order.
    IVP_U_Memory* Mem = Environment->vector_cores_active;
    DistChangeMat.MatrixValues = static_cast<float*>(Mem->GetMem(DistChangeMat.Columns * DistChangeMat.AlignedRowLen * 4 + 0xc));
    DistChangeMat.DesiredVector = static_cast<float*>(Mem->GetMem(DistChangeMat.AlignedRowLen * 4));
    DistChangeMat.ResultVector = static_cast<float*>(Mem->GetMem(DistChangeMat.AlignedRowLen * 4));
    DistChangeMat.AlignMatrixValues(); // 063661a0
}

// (RET 4)
void IVP_Friction_Solver::SetupCoordsMindists(IVP_ListManager* FrictionSystem)
{
    int Position = 0;   // EAX, spilled into the argument slot
    int MatrixIndex = 0; // local_8

    for (IVP_Contact_Point* Dist = FrictionSystem->CtxListHead; Dist != nullptr; Dist = Dist->ListManagerListNext)
    {
        IVP_Impact_Solver_Long_Term* LongTerm = Dist->Owner;
        // MOVSX [fs+0x40]; CMP EAX,ECX; JGE -> setup. The skip
        // branch does not advance Position (baseline jumps straight to the next node).
        if (Position < static_cast<int>(FrictionSystem->ComplexNotNecessaryNumber))
        {
            LongTerm->IndexInFs = -1; // MOV word ptr [EDI+0x50],0xffff
            continue;
        }

        ++Position;
        LongTerm->FrictionInfos[0] = static_cast<IVP_Friction_Info_For_Core*>(
            Dist->Object1->core->find_object_hash_entry(IVP_FrictionKey(FrictionSystem)));
        LongTerm->FrictionInfos[1] = static_cast<IVP_Friction_Info_For_Core*>(
            Dist->Object2->core->find_object_hash_entry(IVP_FrictionKey(FrictionSystem)));
        LongTerm->GapDistance = Dist->GapDistance;
        LongTerm->HasNegativePullSince = Dist->HasNegativePullSince;
        DistsOfSystem.Add(LongTerm);                                                       // (063441a0 when full)
        LongTerm->IndexInFs = static_cast<int16_t>(MatrixIndex);
        ++MatrixIndex;
    }
}

// (RET 8)
int IVP_Friction_Solver::CalcSolverPSI(IVP_ListManager* FrictionSystem, int* ActiveIsAtPos)
{
    // zero Columns * AlignedRowLen floats, from the last down.
    for (int K = DistChangeMat.Columns * DistChangeMat.AlignedRowLen - 1; K >= 0; --K)
    {
        DistChangeMat.MatrixValues[K] = 0.0f;
    }

    int TotalActives = 0; // local_5c
    for (int I = 0; I < static_cast<int>(DistsOfSystem.n_elems); ++I) // word compare, JBE/JL
    {
        IVP_Impact_Solver_Long_Term* LongTerm = DistsOfSystem.elems[I];

        // relative normal speed of the two cores at the contact.
        float RelSpeed = 0.0f; // local_64
        IVP_Core* CoreA = LongTerm->ChildCandidate[0];
        if (CoreA != nullptr)
        {
            const float RotPart = (float)(IVP_Dot(CoreA->RotSpeed, LongTerm->CoordOSCrossNormal[0])); // FSTP local_54
            RelSpeed = (float)(IVP_Dot(CoreA->Speed, LongTerm->ContactNormalWS) + (double)(RotPart));
        }
        IVP_Core* CoreB = LongTerm->ChildCandidate[1];
        if (CoreB != nullptr)
        {
            const float RotPart = (float)(IVP_Dot(CoreB->RotSpeed, LongTerm->CoordOSCrossNormal[1]));
            const double Part = IVP_Dot(CoreB->Speed, LongTerm->ContactNormalWS) + (double)(RotPart);
            RelSpeed = (float)((double)(RelSpeed) - Part); // FSUBR local_64
        }

        // Desired[I] = Factor * (Settings+0x10c - Gap) + RelSpeed,
        // Factor 1.0f, or 20.0f (the field) when the difference is below 0.0f
        // (FCOMP 0.0; TEST AH,0x1; JZ keeps 1.0 -- C0 set, i.e. less or unordered,
        // takes 20.0).
        const double GapError = (double)(G_IVP_MindistSettings.friction_dist) -
                                (double)(LongTerm->GapDistance);
        double Factor = (double)(1.0f);
        if (!(GapError >= 0.0))
        {
            Factor = (double)(20.0f);
        }
        DistChangeMat.DesiredVector[I] = (float)(Factor * GapError + (double)(RelSpeed));


        if (LongTerm->HasNegativePullSince != 0)
        {
            *ActiveIsAtPos = I;
            ++ActiveIsAtPos;
            ++TotalActives;
        }

        // core A's column entries, pushed with the negated arms.
        CoreA = LongTerm->ChildCandidate[0];
        if (CoreA != nullptr)
        {
            Vector RotArm; // local_40
            IVP_SetPairwiseMult(RotArm, LongTerm->CoordOSCrossNormal[0], CoreA->InvRotInertia);
            IVP_SetNegative(RotArm, RotArm);
            Vector LinArm; // local_30
            const float LinScale = (float)((double)(CoreA->InvMass) * (double)(-1.0f));
            IVP_SetMultiple(LinArm, LongTerm->ContactNormalWS, LinScale);

            IVP_Friction_Info_For_Core* Info = LongTerm->FrictionInfos[0]; // local_44
            IVP_Core* ThisCore = LongTerm->ChildCandidate[0];               // local_4c
            for (int J = 0; J < static_cast<int>(Info->FrDists.n_elems); ++J)
            {
                const int Row = static_cast<int>(Info->FrDists.elems[J]->Owner->IndexInFs); // MOVSX
                if (Row < 0)
                {
                    continue;
                }
                IVP_Impact_Solver_Long_Term* Other = DistsOfSystem.elems[Row];
                int Side = 1;
                float Sign = 1.0f;
                if (Other->ChildCandidate[0] == ThisCore)
                {
                    Side = 0;
                    Sign = -1.0f;
                }
                if (Other->ChildCandidate[Side] == nullptr)
                {
                    continue;
                }
                const float RotDot = (float)(IVP_Dot(RotArm, Other->CoordOSCrossNormal[Side])); // FSTP local_48
                const double Sum = IVP_Dot(LinArm, Other->ContactNormalWS) + (double)(RotDot);
                float* Cell = &DistChangeMat.MatrixValues[Row * DistChangeMat.AlignedRowLen + I];
                *Cell = (float)(Sum * (double)(Sign) + (double)(*Cell));
            }
        }

        // core B's column entries.
        CoreB = LongTerm->ChildCandidate[1];
        if (CoreB != nullptr)
        {
            Vector RotArm; // local_10
            IVP_SetPairwiseMult(RotArm, LongTerm->CoordOSCrossNormal[1], CoreB->InvRotInertia);
            Vector LinArm; // local_20
            IVP_SetMultiple(LinArm, LongTerm->ContactNormalWS, CoreB->InvMass);

            IVP_Friction_Info_For_Core* Info = LongTerm->FrictionInfos[1];
            IVP_Core* ThisCore = LongTerm->ChildCandidate[1]; // local_44
            for (int J = 0; J < static_cast<int>(Info->FrDists.n_elems); ++J)
            {
                const int Row = static_cast<int>(Info->FrDists.elems[J]->Owner->IndexInFs);
                if (Row < 0)
                {
                    continue;
                }
                IVP_Impact_Solver_Long_Term* Other = DistsOfSystem.elems[Row];
                int Side = 1;
                float Sign = 1.0f;
                if (Other->ChildCandidate[0] == ThisCore)
                {
                    Side = 0;
                    Sign = -1.0f;
                }
                if (Other->ChildCandidate[Side] == nullptr)
                {
                    continue;
                }
                const double Value = IVP_CrossAndNormalDot(Other, Side, RotArm, LinArm);
                float* Cell = &DistChangeMat.MatrixValues[Row * DistChangeMat.AlignedRowLen + I];
                *Cell = (float)(Value * (double)(Sign) + (double)(*Cell));
            }
        }
    }

    // every contact's matrix index must equal its list position.
    int Position = 0;
    for (IVP_Contact_Point* Dist = FrictionSystem->CtxListHead; Dist != nullptr; Dist = Dist->ListManagerListNext)
    {
        const int16_t Index = Dist->Owner->IndexInFs;
        if (Index >= 0 && static_cast<int>(Index) != Position)
        {
            IVP_CoreTrap(); // MOV dword ptr [0],0
        }
        ++Position;
    }
    return TotalActives;
}


void IVP_Friction_Solver::NormizeConstraintEqu()
{
    const int Count = DistChangeMat.Columns; // EBP
    const int Last = Count - 1;              // ESI

    // largest diagonal element (FCOM local; TEST AH,0x41; JNZ
    // keeps the old value -- only a strictly greater, ordered value replaces it).
    float MaxDiag = 0.0f;
    double DiagScale = (double)(1.0f);
    if (Last >= 0)
    {
        const float* Diag = &DistChangeMat.MatrixValues[(DistChangeMat.AlignedRowLen + 1) * Last];
        const int Stride = -1 - DistChangeMat.AlignedRowLen;
        for (int K = Last + 1; K != 0; --K)
        {
            if (*Diag > MaxDiag)
            {
                MaxDiag = *Diag;
            }
            Diag += Stride;
        }
        // FCOMP 1e-10f (the field); TEST AH,0x41; JNZ -> 1.0 (not greater or unordered).
        if (MaxDiag > 1.000000013351432e-10f)
        {
            DiagScale = (double)(1.0f) / (double)(MaxDiag);
        }
    }
    ImpulseScale = (float)(DiagScale); // FST (overwritten below)

    // largest |rhs| (each candidate rounded to float through local_4).
    float RhsScale = 1.0f;                   // local_4 when the max is too small
    double BackScale = (double)(1.0f);
    if (Last >= 0)
    {
        double MaxRhs = (double)(0.0f);
        for (int I = Last; I >= 0; --I)
        {
            const float AbsValue = std::fabs(DistChangeMat.DesiredVector[I]);
            if ((double)(AbsValue) > MaxRhs) // FCOMP; TEST AH,0x41; JNZ keeps
            {
                MaxRhs = (double)(AbsValue);
            }
        }
        // FCOM 1e-10f; TEST AH,0x41; JNZ -> 1.0 path.
        if (MaxRhs > (double)(1.000000013351432e-10f))
        {
            RhsScale = (float)((double)(1.0f) / MaxRhs); // FDIV ST0,ST1; FSTP local_4
            BackScale = MaxRhs;                                                 // stays in ST0
        }
    }

    // ImpulseScale = DiagScale * BackScale.
    ImpulseScale = (float)(DiagScale * BackScale);

    // every row, whole aligned width, times DiagScale.
    for (int Row = 0; Row < DistChangeMat.Columns; ++Row)
    {
        float* RowValues = &DistChangeMat.MatrixValues[DistChangeMat.AlignedRowLen * Row];
        for (int Col = 0; Col < DistChangeMat.AlignedRowLen; ++Col)
        {
            RowValues[Col] = (float)(DiagScale * (double)(RowValues[Col]));
        }
    }

    // right-hand side times RhsScale, last to first.
    for (int I = DistChangeMat.Columns - 1; I >= 0; --I)
    {
        DistChangeMat.DesiredVector[I] = (float)((double)(RhsScale) *
                                                            (double)(DistChangeMat.DesiredVector[I]));
    }
}

// (RET 0x10)
int IVP_Friction_Solver::TestGaussSolutionSuggestion(const float* Values, const int* Indices, int Count, IVP_U_Memory* Mem)
{
    // one int per column from the arena, zeroed.
    int* Visited = static_cast<int*>(Mem->GetMem(DistChangeMat.Columns * 4));
    std::memset(Visited, 0, static_cast<size_t>(DistChangeMat.Columns * 4));

    int bPushTooSmall = 0; // the reused argument slot
    for (int I = 0; I < Count; ++I)
    {
        const int Index = Indices[I];
        const double Value = (double)(Values[I]);
        const double Threshold = (double)(Environment->field_c4) * (double)(0.01f);
        const double Push = (double)(DistsOfSystem.elems[Index]->EffectiveMassDenom) *
                            (double)(ImpulseScale) * Value;
        // FCOMPP; TEST AH,0x1; JZ skips -- C0 (Push below Threshold, or unordered) flags it.
        if (!(Push >= Threshold))
        {
            bPushTooSmall = 1;
        }
        Visited[Index] = 1;
        DistChangeMat.ResultVector[Index] = (float)(Value);
    }

    // every row the Gauss sub-solution did not cover must hold.
    for (int J = 0; J < DistChangeMat.Columns; ++J)
    {
        if (Visited[J] == 0 && DistChangeMat.TestResultRow(J) == 0)
        {
            return 0;
        }
    }
    return bPushTooSmall == 0 ? 1 : 0;
}


void IVP_Friction_Solver::ScaleResultVector()
{
    for (int I = DistChangeMat.Columns - 1; I >= 0; --I)
    {
        DistChangeMat.ResultVector[I] = (float)((double)(ImpulseScale) *
                                                           (double)(DistChangeMat.ResultVector[I]));
    }
}

// (RET 0x10)
void IVP_Friction_Solver::SolveLinearEquationAndPush(IVP_ListManager* FrictionSystem, int* ActiveIsAtPos, int TotalActives,
                                                     IVP_U_Memory* Mem)
{
    // the actives-only matrix on the stack (local_118).
    IVP_Great_Matrix_Many_Zero ActiveMat;
    ActiveMat.Columns = TotalActives;
    ActiveMat.AlignedRowLen = (TotalActives + 3) & ~3;

    NormizeConstraintEqu();

    // rhs, result, matrix -- in this order.
    ActiveMat.DesiredVector = static_cast<float*>(Mem->GetMem(ActiveMat.AlignedRowLen * 4));
    ActiveMat.ResultVector = static_cast<float*>(Mem->GetMem(ActiveMat.AlignedRowLen * 4));
    ActiveMat.MatrixValues = static_cast<float*>(Mem->GetMem(ActiveMat.AlignedRowLen * TotalActives * 4 + 0xc));
    ActiveMat.AlignMatrixValues();

    // clear the full result vector (aligned width).
    std::memset(DistChangeMat.ResultVector, 0, static_cast<size_t>(DistChangeMat.AlignedRowLen * 4));

    ActiveMat.FillFromBiggerMatrix(&DistChangeMat, ActiveIsAtPos, TotalActives);

    // the LCP solver's member matrices (+0x98/+0xb0/+0xc8/+0xe0).
    IVP_Linear_Constraint_Solver LinearSolver;

    // diagonal += 1e-5f (the field).
    for (int I = 0; I < DistChangeMat.Columns; ++I)
    {
        float* Diag = &DistChangeMat.MatrixValues[(DistChangeMat.AlignedRowLen + 1) * I];
        *Diag = (float)((double)(*Diag) + (double)(9.99999974737875e-06f));
    }

    // try the Gauss solution of the actives first.
    bool bSolved = false;
    if (ActiveMat.SolveGreatMatrixManyZero() == 1 &&
        TestGaussSolutionSuggestion(ActiveMat.ResultVector, ActiveIsAtPos, TotalActives, Mem) == 1)
    {
        bSolved = true;
    }
    if (!bSolved)
    {
        // full LCP.
        const int NumActives = FrictionSystem->GetNumSupposedActiveFrDists();
        if (LinearSolver.InitAndSolveLc(DistChangeMat.MatrixValues, DistChangeMat.DesiredVector, DistChangeMat.ResultVector,
                                        DistChangeMat.Columns, NumActives, Mem) != 1)
        {
            return;
        }
    }


    ScaleResultVector();
    const float EnergyBefore = (float)(FrictionSystem->KineticEnergyOfSystem()); // FSTP local_fc
    DoResultingPushes(FrictionSystem);
    const float EnergyAfter = (float)(FrictionSystem->KineticEnergyOfSystem());  // FSTP local_100
    const double Allowed = FrictionSystem->MaxEnergyGain() + (double)(EnergyBefore);
    // FCOMP local_100; TEST AH,0x1; JNZ -> undo when Allowed is below EnergyAfter (or unordered).
    if (!(Allowed >= (double)(EnergyAfter)))
    {
        FrictionSystem->UndoComplexPushes();
        return;
    }
    FrictionSystem->ConfirmComplexPushes();
}

// (RET 4)
void IVP_Friction_Solver::DoResultingPushes(IVP_ListManager* FrictionSystem)
{
    int Index = 0; // EBP: list position, also the ResultVector index
    for (IVP_Contact_Point* Dist = FrictionSystem->CtxListHead; Dist != nullptr; Dist = Dist->ListManagerListNext)
    {
        double Pressure = (double)(0.0f); // what the tail multiplies by i_delta_time
        if (Index >= static_cast<int>(FrictionSystem->ComplexNotNecessaryNumber))
        {
            const float ImpulseF = DistChangeMat.ResultVector[Index];
            const double Impulse = (double)(ImpulseF);
            bool bPush = true;

            // FCOM 0.0; TEST AH,0x41; JNZ: only a strictly positive, ordered impulse falls through.
            if (Impulse > 0.0)
            {

                if (Dist->HasNegativePullSince >= 0)
                {
                    Dist->HasNegativePullSince = -1;
                }
                else
                {
                    --Dist->HasNegativePullSince;
                }
            }
            // FCOM 0.0; TEST AH,0x40; JZ: C3 (equal or unordered) is the zero branch.
            else if (!(Impulse < 0.0))
            {
                Dist->HasNegativePullSince = 0; // , then straight to the 0.0 tail
                bPush = false;
            }
            else
            {

                if (Dist->HasNegativePullSince < 0)
                {
                    Dist->HasNegativePullSince = 0;
                }
                const int Count = Dist->HasNegativePullSince + 1;
                Dist->HasNegativePullSince = Count;
                if (Count > 9)
                {
                    Dist->HasNegativePullSince = 0;
                }
            }

            if (bPush)
            {
                IVP_Impact_Solver_Long_Term* LongTerm = Dist->Owner;

                // core A gets the negative push.
                IVP_Core* CoreA = LongTerm->ChildCandidate[0];
                if (CoreA != nullptr)
                {
                    Vector RotArm; // local_20
                    RotArm.x = (float)((double)(LongTerm->CoordOSCrossNormal[0].x) * (double)(CoreA->InvRotInertia.x));
                    RotArm.y = (float)((double)(LongTerm->CoordOSCrossNormal[0].y) * (double)(CoreA->InvRotInertia.y));
                    RotArm.z = (float)((double)(LongTerm->CoordOSCrossNormal[0].z) * (double)(CoreA->InvRotInertia.z));
                    IVP_AddMultiple(CoreA->RotSpeedChange, CoreA->RotSpeedChange, RotArm, (float)(-Impulse));
                    const float LinScale = (float)(-((double)(CoreA->InvMass) * (double)(ImpulseF)));
                    IVP_AddMultiple(CoreA->SpeedChange, CoreA->SpeedChange, LongTerm->ContactNormalWS, LinScale);
                }

                // core B gets the positive push, inline. The x
                // product stays on the stack unrounded; y and z go through float locals.
                IVP_Core* CoreB = LongTerm->ChildCandidate[1];
                if (CoreB != nullptr)
                {
                    const double ArmX = (double)(LongTerm->CoordOSCrossNormal[1].x) * (double)(CoreB->InvRotInertia.x);
                    const float ArmY = (float)((double)(LongTerm->CoordOSCrossNormal[1].y) * (double)(CoreB->InvRotInertia.y)); // local_c
                    const float ArmZ = (float)((double)(LongTerm->CoordOSCrossNormal[1].z) * (double)(CoreB->InvRotInertia.z)); // local_8
                    CoreB->RotSpeedChange.x = (float)(ArmX * Impulse + (double)(CoreB->RotSpeedChange.x));
                    CoreB->RotSpeedChange.y = (float)((double)(ArmY) * Impulse + (double)(CoreB->RotSpeedChange.y));
                    CoreB->RotSpeedChange.z = (float)((double)(ArmZ) * Impulse + (double)(CoreB->RotSpeedChange.z));

                    const double MassImpulse = (double)(CoreB->InvMass) * Impulse;
                    const double NewY = MassImpulse * (double)(LongTerm->ContactNormalWS.y) + (double)(CoreB->SpeedChange.y);
                    const float NewZ = (float)(MassImpulse * (double)(LongTerm->ContactNormalWS.z) + (double)(CoreB->SpeedChange.z));
                    const double NewX = MassImpulse * (double)(LongTerm->ContactNormalWS.x);
                    CoreB->SpeedChange.z = NewZ;
                    CoreB->SpeedChange.x = (float)(NewX + (double)(CoreB->SpeedChange.x));
                    CoreB->SpeedChange.y = (float)(NewY);
                }

                // FCOM 0.0; TEST AH,0x1; JZ keeps a non-negative
                // impulse, anything below zero becomes 0.0.
                Pressure = (Impulse >= 0.0) ? Impulse : (double)(0.0f);
            }
        }

        // Dist+0x54 = Pressure * i_delta_time.
        Dist->NowFrictionPressure = (float)(Pressure * (double)(EventSim->i_delta_time));
        ++Index;
    }
}

// ============================================================================
// IVP_ListManager friction methods (Ipion's IVP_Friction_System)
// ============================================================================

// (RET 4)
void IVP_ListManager::RemoveDistFromSystem(IVP_Contact_Point* Dist)
{
    IVP_Core* Core1 = Dist->Object1->core; // EDI
    IVP_Core* Core2 = Dist->Object2->core; // EBX
    Core1->update_time_of_last_motion();   // 063470e0
    Core2->update_time_of_last_motion();
    RemoveCtx(Dist);                       // 0634b8d0
    if (RetirePairRecordBookkeeping(Dist) == 1) // 0634b850
    {
        union_find_necessary = 1;
    }

    IVP_Friction_Info_For_Core* Info1 = static_cast<IVP_Friction_Info_For_Core*>(
        Core1->find_object_hash_entry(IVP_FrictionKey(this)));
    IVP_Friction_Info_For_Core* Info2 = static_cast<IVP_Friction_Info_For_Core*>(
        Core2->find_object_hash_entry(IVP_FrictionKey(this)));

    Info1->FrDists.RemoveElement(Dist); // 06355310
    if (Info1->FrDists.n_elems == 0)    // 06355370
    {
        Core1->release_object_hash_entry(Info1); // 06347d50
        UnregisterCore(Core1);                    // 0634ba50
        IVP_Simulation_Unit* Unit = Core1->OwningSimUnit;
        Unit->State = (Unit->State & 0xfffffdffu) | 0x100u;
    }

    Info2->FrDists.RemoveElement(Dist);
    if (Info2->FrDists.n_elems == 0)
    {
        Core2->release_object_hash_entry(Info2);
        UnregisterCore(Core2);
        IVP_Simulation_Unit* Unit = Core2->OwningSimUnit;
        Unit->State = (Unit->State & 0xfffffdffu) | 0x100u;
    }

    // destructor the routine, then operator delete.
    if (Dist != nullptr)
    {
        IVP_UnlinkPairRecordFromObjects(Dist);
        delete Dist;
    }
}


int IVP_ListManager::GetNumSupposedActiveFrDists() const
{
    int Count = 0;
    for (const IVP_Contact_Point* Dist = CtxListHead; Dist != nullptr; Dist = Dist->ListManagerListNext)
    {
        if (Dist->HasNegativePullSince >= 0) // JGE -> return
        {
            break;
        }
        ++Count;
    }
    return Count;
}

// (RET 8). baseline argument order: First = [ESP+4], Second = [ESP+8];
// pushes the later node first, so First is always the earlier one.
void IVP_ListManager::ExchangeFrDists(IVP_Contact_Point* First, IVP_Contact_Point* Second)
{
    if (CtxListHead == First)
    {
        CtxListHead = Second;
    }
    if (First->ListManagerListPrev != nullptr)
    {
        First->ListManagerListPrev->ListManagerListNext = Second;
    }
    if (Second->ListManagerListNext != nullptr)
    {
        Second->ListManagerListNext->ListManagerListPrev = First;
    }
    First->ListManagerListNext = Second->ListManagerListNext;
    Second->ListManagerListPrev = First->ListManagerListPrev;
    First->ListManagerListPrev = Second;
    Second->ListManagerListNext = First;
}


void IVP_ListManager::ReorderMindistsForComplex()
{
    IVP_Contact_Point* Current = CtxListHead; // EDI
    while (Current != nullptr)
    {
        IVP_Contact_Point* Next = Current->ListManagerListNext; // ESI
        if (Next == nullptr)
        {
            return;
        }
        if (Current->HasNegativePullSince > Next->HasNegativePullSince) // CMP; JLE skips
        {
            // sink Next backwards past every larger key.
            ExchangeFrDists(Current, Next);
            for (;;)
            {
                IVP_Contact_Point* Before = Next->ListManagerListPrev;
                if (Before == nullptr || Before->HasNegativePullSince <= Next->HasNegativePullSince)
                {
                    break;
                }
                ExchangeFrDists(Before, Next);
            }
            Next = Current; // continue after the node that was overtaken
        }
        Current = Next;
    }
}


void IVP_ListManager::RecheckFrDistsBeforeSolve()
{
    IVP_Contact_Point* Dist = CtxListHead;
    while (Dist != nullptr)
    {
        IVP_Contact_Point* Next = Dist->ListManagerListNext; // EBX, read first

        // FCOMP Settings+0x11c; TEST AH,0x1; JZ -> remove when the
        // gap is not below it (ordered). Below it (or unordered), remove only when the
        // contact record's +0x54 dword has (bits & 0x300) == 0x100.
        bool bRemove = true;
        if (!(Dist->GapDistance >= G_IVP_MindistSettings.max_distance_for_friction))
        {
            bRemove = (Dist->Owner->FrictionIsBroken & 0x3) == 0x1;
        }

        if (bRemove)
        {
            RemoveDistFromSystem(Dist); // 06355490
        }
        else
        {
            // FLD gap; FLD Settings+0x118; FADD Settings+0x10c; FCOMPP;
            // TEST AH,0x1; JZ skips -- acts when the sum is below the gap (or unordered).
            const double Limit = (double)(G_IVP_MindistSettings.distance_keepers_safety) +
                                 (double)(G_IVP_MindistSettings.friction_dist);
            if (!(Limit >= (double)(Dist->GapDistance)))
            {
                // both original cores share Flags bit 0 or 1 -> move to the head.
                const unsigned Shared = Dist->Object2->original_core->Flags & Dist->Object1->original_core->Flags;
                if ((Shared & 0x3) != 0)
                {
                    RemoveCtx(Dist);
                    InsertCtx(Dist);
                }
            }
        }
        Dist = Next;
    }
    ComplexNotNecessaryNumber = 0; // (BX is the null next pointer) / 06368e10
}

// (RET 4)
int IVP_ListManager::CoreIsInAtMostOneDynamicPair(IVP_Core* Core) const
{
    int Count = 0;
    for (int I = static_cast<int>(CorePairEntries.n_elems) - 1; I >= 0; --I)
    {
        const IVP_ListManager_CorePairEntry* Pair = CorePairEntries.elems[I];
        if (Pair->CoreA == Core || Pair->CoreB == Core)
        {
            if (((Pair->CoreB->Flags | Pair->CoreA->Flags) & 0xc) == 0)
            {
                ++Count;
            }
        }
    }
    return Count > 1 ? 0 : 1;
}


void IVP_ListManager::StaticFrOversizedMatrixPanic()
{
    for (int I = static_cast<int>(RegisteredDynamicCores.n_elems) - 1; I >= 0; --I)
    {
        IVP_Core* Core = RegisteredDynamicCores.elems[I];
        Core->Flags = (Core->Flags & 0xfffffffdu) | 1u;
        if (CoreIsInAtMostOneDynamicPair(Core) == 0)
        {
            // Speed then RotSpeed.
            Core->Speed.x = 0.0f;
            Core->Speed.y = 0.0f;
            Core->Speed.z = 0.0f;
            Core->RotSpeed.x = 0.0f;
            Core->RotSpeed.y = 0.0f;
            Core->RotSpeed.z = 0.0f;
        }
    }
}

// (RET 4)
void IVP_ListManager::DoPushesDistanceKeepers(const IVP_Event_Sim* EventSim)
{
    IVP_U_Memory* Mem = Environment->vector_cores_active;
    ++Mem->State;                                         // transaction depth (word +0x10)
    ++G_IVP_FrictionPassCount;

    ReorderMindistsForComplex();
    RecheckFrDistsBeforeSolve();

    // MOVSX both counts; more than 0x96 dists -> fallback.
    if (static_cast<int>(static_cast<int16_t>(CtxListCount)) - static_cast<int>(ComplexNotNecessaryNumber) > 0x96)
    {
        StaticFrOversizedMatrixPanic();
        --Mem->State;
        Mem->reset_chunks(); // 06344070
        return;
    }

    IVP_Friction_Solver Solver(this, EventSim);
    Solver.SetupCoordsMindists(this);
    int* ActiveIsAtPos = static_cast<int*>(Mem->GetMem(static_cast<int>(static_cast<int16_t>(CtxListCount)) * 4));
    const int TotalActives = Solver.CalcSolverPSI(this, ActiveIsAtPos);
    Solver.SolveLinearEquationAndPush(this, ActiveIsAtPos, TotalActives, Mem);
    --Mem->State;
    Mem->reset_chunks();
    // Solver's vector frees its heap buffer on scope exit.
}


double IVP_ListManager::KineticEnergyOfSystem() const
{
    float Sum = 0.0f; // local_28
    for (int I = static_cast<int>(RegisteredCores.n_elems) - 1; I >= 0; --I)
    {
        const IVP_Core* Core = RegisteredCores.elems[I];
        Vector Rot; // local_20
        Rot.x = Core->RotSpeed.x + Core->RotSpeedChange.x;
        Rot.y = Core->RotSpeed.y + Core->RotSpeedChange.y;
        Rot.z = Core->RotSpeed.z + Core->RotSpeedChange.z;
        Vector Lin; // local_10
        Lin.x = Core->Speed.x + Core->SpeedChange.x;
        Lin.y = Core->Speed.y + Core->SpeedChange.y;
        Lin.z = Core->Speed.z + Core->SpeedChange.z;
        Sum = (float)(Core->calc_kinetic_energy(&Lin, &Rot) + (double)(Sum));
    }
    return (double)(Sum); // FLD local_28
}


void IVP_ListManager::ConfirmComplexPushes()
{
    for (int I = static_cast<int>(RegisteredDynamicCores.n_elems) - 1; I >= 0; --I)
    {
        RegisteredDynamicCores.elems[I]->commit_speed_changes();
    }
}


void IVP_ListManager::UndoComplexPushes()
{
    for (int I = static_cast<int>(RegisteredDynamicCores.n_elems) - 1; I >= 0; --I)
    {
        RegisteredDynamicCores.elems[I]->clear_speed_change();
    }
}


double IVP_ListManager::MaxEnergyGain() const
{
    double Sum = (double)(0.0f); // FLD 0.0, kept on the stack
    for (int I = static_cast<int>(RegisteredCores.n_elems) - 1; I >= 0; --I)
    {
        const IVP_Core* Core = RegisteredCores.elems[I];
        if ((Core->Flags & 0xc) == 0) // TEST byte ptr [ECX],0xc
        {
            Sum = (double)(Core->Mass) * (double)(9.81000041961670f) * (double)(0.1f) + Sum;
        }
    }
    return Sum;
}

// ============================================================================
// The friction system as a controller, and its two handles
// (2026-10-08, claude-code:cmn-phy3). Vtables and layout: ivp_mindist.h,
// above IVP_ListManager.
// ============================================================================

// (module storage-06347d70, RET 4), IVP_Core's member, defined
// here because it needs IVP_Friction_Info_For_Core complete (see ivp_core.cpp).
// unregister_object_hash_entry(Candidate) (the routine).
// `TEST ESI,ESI; JZ` -> null stops here.
// the routine on Candidate (its FrDists vector destructor).
// the routine = operator delete.
// `delete` is that pair. Until 2026-10-08 the port used Clear() + free() on
// records allocated with `new`.
void IVP_Core::release_object_hash_entry(IVP_Friction_Info_For_Core* Candidate)
{
    unregister_object_hash_entry(Candidate);
    if (Candidate != nullptr)
    {
        delete Candidate;
    }
}

// (RET 8), slot 3: rebase every dist's IVP_Time stamp (+0x68 /
// +0x6c) by Delta, pair by pair and dist by dist, both backward.
// pair index = zero-extended word [this+0x36] - 1.
// / 06355bca: EBX = Delta.time_offset, EBP = Delta.time_base.
// pair re-read from [this+0x38] every iteration.
// dist index = zero-extended word [pair+2] - 1.
// the routine(this=dist, time_base, time_offset)
//     (PUSH EBX then PUSH EBP: time_base is the first stack argument).
void IVP_ListManager::reset_time(IVP_Time Delta)
{
    for (int I = static_cast<int>(CorePairEntries.n_elems) - 1; I >= 0; --I)
    {
        IVP_ListManager_CorePairEntry* Pair = CorePairEntries.elems[I];
        for (int J = static_cast<int>(Pair->AttachedCtx.n_elems) - 1; J >= 0; --J)
        {
            IVP_Contact_Point* Dist = Pair->AttachedCtx.elems[J];
            Dist->ShiftTimeStamp(Delta.time_base, Delta.time_offset);
        }
    }
}

// (module storage-0634b84f, no arguments).
// pair index = zero-extended word [this+0x36] - 1; JS.
// pair re-read from [this+0x38] each iteration.
// dist index = zero-extended word [pair+2] - 1;
//     local_c (the float sum) = 0 before the dist loop.
// dist re-read from [pair+4]; local_4 = dist +0x58 (the
//     gap) BEFORE the recompute.
// the routine on the dist.
// `FLD [ESI+0x58]; FSUBR local_4; FMUL [ESI+0x54];
//     FADD local_c; FSTP local_c` -> sum += (old gap - new gap) * pressure,
//     rounded to float once per dist.
// `FCOMP 0.0f; TEST AH,0x41; JNZ skip` -- taken when the
//     sum is < 0, == 0 or unordered; the fall-through (sum > 0) adds it:
// `FLD local_c; FADD [EBX+0x24]; FSTP [EBX+0x24]`.
void IVP_ListManager::RecalcAllContactPoints()
{
    for (int I = static_cast<int>(CorePairEntries.n_elems) - 1; I >= 0; --I)
    {
        IVP_ListManager_CorePairEntry* Pair = CorePairEntries.elems[I];
        float Sum = 0.0f;
        for (int J = static_cast<int>(Pair->AttachedCtx.n_elems) - 1; J >= 0; --J)
        {
            IVP_Contact_Point* Dist = Pair->AttachedCtx.elems[J];
            const float OldGap = Dist->GapDistance;
            IVP_UpdatePairRecordContactFrame(Dist);
            Sum = (float)(((double)(OldGap) - (double)(Dist->GapDistance)) *
                                         (double)(Dist->NowFrictionPressure) +
                                     (double)(Sum));
        }
        if (Sum > 0.0f)
        {
            Pair->_field24 = (float)((double)(Sum) + (double)(Pair->_field24));
        }
    }
}

// (RET 4), the static handle's slot 0: a core is being deleted.
// pair index = zero-extended word [Fs+0x36] - 1 (Fs =
//     [this+4], re-read every time it is used).
// pair = [Fs+0x38][i]; skip unless pair->CoreA (+0x28)
//     or pair->CoreB (+0x2c) is Core.
// dist index = zero-extended word [pair+2] - 1; each dist
//     (re-read from [pair+4]) goes through the routine on Fs.
// `CMP word [Fs+0x3e],0; JNZ; TEST ECX,ECX; JZ;
//     vtbl[6](1)` -> `delete Fs` once the system holds no dist. The null test
//     is the compiler's own around a virtual delete.
// Removing a pair's last dist deletes that pair (the routine); that only
// happens on its index-0 dist, after which the inner loop ends without
// touching the pair again -- as in baseline.
void IVP_Friction_Sys_Static::core_is_going_to_be_deleted_event(IVP_Core* Core)
{
    for (int I = static_cast<int>(FrictionSystem->CorePairEntries.n_elems) - 1; I >= 0; --I)
    {
        IVP_ListManager_CorePairEntry* Pair = FrictionSystem->CorePairEntries.elems[I];
        if (Pair->CoreA == Core || Pair->CoreB == Core)
        {
            for (int J = static_cast<int>(Pair->AttachedCtx.n_elems) - 1; J >= 0; --J)
            {
                FrictionSystem->RemoveDistFromSystem(Pair->AttachedCtx.elems[J]);
            }
        }
    }

    IVP_ListManager* Fs = FrictionSystem;
    if (static_cast<int16_t>(Fs->CtxListCount) == 0)
    {
        delete Fs;
    }
}

// (RET 8), the static handle's do_simulation_controller.
// `CMP word [Fs+0x3e],1; JG` (signed) -- at most one dist
//     -> the routine on this handle; more -> the routine on Fs
//     (DoPushesDistanceKeepers). Both get the event.
// no dist left (`CMP word [Fs+0x3e],0; JNZ`):
// this->FrictionSystem = 0; 063565eb-063565fa: delete Fs (null
//     tested); 063565fd-0635660f: EventSim->sim_unit->State =
//     (State & ~0x200) | 0x100. Return.
// otherwise, only when the low byte of Fs+0x44 is set:
// clear that byte;
// Core = the routine(Fs) (a core cut off from the first one);
// none -> return;
// the routine(Fs, Core) splits its group off into a new system;
// Core->OwningSimUnit->State = (State & ~0x200) | 0x100.
// `this` is a member of Fs: nothing touches it after `delete Fs`, and the split
// may delete Fs as well (the routine's underfull-Manager branch), after which
// only Core is used -- as in baseline.
void IVP_Friction_Sys_Static::do_simulation_controller(IVP_Event_Sim* EventSim, IVP_U_Vector<IVP_Core>* /*CoreList*/)
{
    if (static_cast<int16_t>(FrictionSystem->CtxListCount) <= 1)
    {
        DoSimulationSingleFriction(EventSim);
    }
    else
    {
        FrictionSystem->DoPushesDistanceKeepers(EventSim);
    }

    IVP_ListManager* Fs = FrictionSystem;
    if (static_cast<int16_t>(Fs->CtxListCount) == 0)
    {
        FrictionSystem = nullptr;
        delete Fs;
        IVP_Simulation_Unit* Unit = EventSim->sim_unit;
        Unit->State = (Unit->State & 0xfffffdffu) | 0x100u;
        return;
    }

    if (Fs->union_find_necessary == 0)
    {
        return;
    }
    Fs->union_find_necessary = 0;

    IVP_Core* SplitCore = IVP_ListManager_FindDisconnectedCore(FrictionSystem);
    if (SplitCore == nullptr)
    {
        return;
    }
    IVP_ListManager_SplitOffCoreGroup(FrictionSystem, SplitCore);
    IVP_Simulation_Unit* Unit = SplitCore->OwningSimUnit;
    Unit->State = (Unit->State & 0xfffffdffu) | 0x100u;
}

// (RET 4, thiscall on the static handle).
// Dist = Fs->CtxListHead (+0x20).
// the routine(this=Dist, EventSim, the field, 1.0f)
//     -- pushes 1.0f, then the global, then the event: the event is the first
//     stack argument. the field is IVP_Mindist_Settings+0x10c.
// `FLD [Dist+0x58]; FCOMP the field; TEST AH,0x1; JZ`
//     -- C0 clear (gap >= Settings+0x11c, ordered) jumps straight to removal.
// otherwise (gap below, or unordered) removal only when
//     ([Dist->Owner+0x54] & 0x300) == 0x100; any other value returns.
// the routine on Fs.
void IVP_Friction_Sys_Static::DoSimulationSingleFriction(IVP_Event_Sim* EventSim)
{
    IVP_Contact_Point* Dist = FrictionSystem->CtxListHead;
    Dist->SolveNormalImpulse(EventSim, G_IVP_MindistSettings.friction_dist, 1.0f);

    if (!(Dist->GapDistance >= G_IVP_MindistSettings.max_distance_for_friction))
    {
        const uint32_t OwnerState = *reinterpret_cast<const uint32_t*>(
            reinterpret_cast<const char*>(Dist->Owner) + offsetof(IVP_Impact_Solver_Long_Term, StateProcessed));
        if ((OwnerState & 0x300u) != 0x100u)
        {
            return;
        }
    }
    FrictionSystem->RemoveDistFromSystem(Dist);
}

// (RET 8), the energy handle's do_simulation_controller.
// `CMP word [Fs+0x3e],1; JG` (signed).
// at most one dist -> the routine on Fs->CtxListHead,
//     return. (With no dist the head is null; baseline calls anyway -- the
//     static handle, priority 0, deletes an empty system before this one,
//     priority 2000, runs.)
// the routine on Fs (RecalcAllContactPoints).
// `MOV EAX,[EventSim+0xc]; MOV ECX,[EAX]; TEST CH,0x30;
//     JZ` -> sim_unit->State & 0x3000 set: the routine on Fs (zero every
//     pair's _field24).
// `TEST AH,0xc; JNZ` -> State & 0x0c00 clear:
// on Fs (tick every pair's _field24 budget).
void IVP_Friction_Sys_Energy::do_simulation_controller(IVP_Event_Sim* EventSim, IVP_U_Vector<IVP_Core>* /*CoreList*/)
{
    IVP_ListManager* Fs = FrictionSystem;
    if (static_cast<int16_t>(Fs->CtxListCount) <= 1)
    {
        IVP_UpdatePairRecordContactFrame(Fs->CtxListHead);
        return;
    }

    Fs->RecalcAllContactPoints();
    if ((EventSim->sim_unit->State & 0x3000u) != 0)
    {
        IVP_ListManager_ClearAllCorePairBudgets(FrictionSystem);
    }
    if ((EventSim->sim_unit->State & 0x0c00u) == 0)
    {
        IVP_ListManager_TickAllCorePairBudgets(FrictionSystem);
    }
}
