// Author: Giperion.

// Common.dll ZONE -- Universe::IZone's implementation (RTTI .?AUZONE@@, vftable
// , 37 slots) and the zone shape geometry its slots call (Systems.cpp).

// Floating point: baseline evaluates on the x87 stack at 53-bit precision and rounds only where
// it stores a float (FSTP dword). The bodies below keep that split: a value baseline keeps on
// the stack is a double here, a value baseline stores to a float local is a float here, and
// the slots whose result other slots consume in-register (+0x00, +0x3c, +0x40, +0x48) return
// double, so the caller sees the unrounded value, as in baseline. A single
// float operation followed by a float store is written as plain float arithmetic (the 53-bit
// intermediate rounds to the same float).

// Comparison idioms (FCOM / FNSTSW AX / TEST AH,mask / Jcc), as used in the comments:
//   TEST 5 / JP     taken when ST >= src or unordered      TEST 5 / JNP   taken when ST < src
//   TEST 0x41 / JNZ taken when ST <= src or unordered      TEST 0x41 / JZ taken when ST > src
//   TEST 0x41 / JP  taken when ST > src or unordered       TEST 1 / JNZ   taken when ST < src or unordered
//   TEST 0x44 / JP  taken when ST != src or unordered

#include "common_zone.h"
#include "common_archetype.h" // CacheString

#include "common_csys.h"  // Acquire3DMathEngine (the routine)
#include "common_misc.h"  // Geometry::Frustum / Sphere, FrustumCull, SphereCull
#include "common_geometry_internal.h" // MathTransposeTransformVia3DMathEngine, ClampPointToBoxSurface
#include "x86math.h"

#include "NeuronRuntime.h"
#include "neuroncomponents.h"

#include "x87_trig.h" // baseline's inline FSIN / FCOS / FPTAN (CullQuatFromAxisAngle, FrustumCull)

#include <math.h>
#include <stdlib.h>

namespace
{
    char const kSystemsSource[] = "Common\\Systems.cpp";
    char const kZoneTypeNotSupported[] = "PCI - zone type not supported!\n";

    // 's first argument: a sphere {centre, radius}.
    struct FZoneSphere
    {
        Vector Center; // +0x00
        float Radius;  // +0x0c
    };
    static_assert(sizeof(FZoneSphere) == 0x10, "FZoneSphere is 0x10 bytes");

    // The segment every clip function takes: {start, end}.
    struct FZoneSegment
    {
        Vector Start; // +0x00
        Vector End;   // +0x0c
    };
    static_assert(sizeof(FZoneSegment) == 0x18, "FZoneSegment is 0x18 bytes");

    // The oriented frame the box / cylinder / ring code builds on the stack: the zone's
    // transform followed by three dimensions. ClampPointToBoxSurface (the routine) reads the
    // dimensions at +0x30 as half extents; the clip functions read them as their own sizes.
    struct FZoneFrame
    {
        Matrix Orientation; // +0x00
        Vector Pos;         // +0x24
        float Dim[3];       // +0x30
    };
    static_assert(sizeof(FZoneFrame) == 0x3c, "FZoneFrame is 0x3c bytes");

    // The ellipsoid object the routine / the routine / the routine work on.
    struct FZoneEllipsoid
    {
        Matrix Orientation; // +0x00
        Vector Pos;         // +0x24
        float Radius[3];    // +0x30
        Matrix Scaled;      // +0x3c
    };
    static_assert(sizeof(FZoneEllipsoid) == 0x60, "FZoneEllipsoid is 0x60 bytes");

    // .data:the field -- Systems.cpp's own 3DMathEngine pointer, fetched through
    // the first time a zone function needs it (every use site tests it first).
    I3DMathEngine* GZoneMathEngine = nullptr;

    I3DMathEngine* ZoneMathEngine()
    {
        if (GZoneMathEngine == nullptr)
            GZoneMathEngine = Acquire3DMathEngine();
        return GZoneMathEngine;
    }

    // this = {X, Y, Z}.
    Vector MakeVector(float X, float Y, float Z)
    {
        Vector Result;
        Result.x = X;
        Result.y = Y;
        Result.z = Z;
        return Result;
    }

    // Out = A - B, per component, float stores.
    Vector Vec3Sub(Vector const& A, Vector const& B)
    {
        return MakeVector(A.x - B.x, A.y - B.y, A.z - B.z);
    }

    // Out = A + B, per component, float stores.
    Vector Vec3Add(Vector const& A, Vector const& B)
    {
        return MakeVector(A.x + B.x, A.y + B.y, A.z + B.z);
    }

    // Out = V * S, per component, float stores.
    Vector Vec3Scale(Vector const& V, float S)
    {
        return MakeVector(S * V.x, S * V.y, S * V.z);
    }

    // sqrt((x*x + y*y) + z*z), left on the stack.
    double VecLength(Vector const& V)
    {
        return sqrt(((double)V.x * V.x + (double)(V.y) * V.y) +
                    (double)V.z * V.z);
    }

    // (x*x + y*y) + z*z, left on the stack.
    double VecLengthSq(Vector const& V)
    {
        return ((double)V.x * V.x + (double)(V.y) * V.y) +
               (double)V.z * V.z;
    }

    // (thiscall, RET 4): each component *= S, float stores.
    void VecScaleInPlace(Vector& V, float S)
    {
        V.x = S * V.x;
        V.y = S * V.y;
        V.z = S * V.z;
    }

    // 3DMathEngine vt+0x38 through the field: Dst = transpose(M) * Src.
    Vector EngineTransposeTransform(Matrix const& M, Vector const& Src)
    {
        Vector Dst;
        // Vector / Matrix and x86math's FVector / FMatrix share one layout.
        ZoneMathEngine()->TransposeTransform(*(FVector*)&Dst, *(FMatrix const*)&M,
                                             *(FVector const*)&Src);
        return Dst;
    }

    // 3DMathEngine vt+0x2c through the field: Dst = M * Src.
    Vector EngineTransformMatrix(Matrix const& M, Vector const& Src)
    {
        Vector Dst;
        ZoneMathEngine()->TransformMatrix(*(FVector*)&Dst, *(FMatrix const*)&M,
                                          *(FVector const*)&Src);
        return Dst;
    }

    // 3DMathEngine vt+0x28 through the field: Dst = Frame.Orientation * Src + Frame.Pos.
    Vector EngineTransformXform(FZoneFrame const& Frame, Vector const& Src)
    {
        Vector Dst;
        // The frame's first 0x30 bytes are the {Orientation, Pos} transform.
        ZoneMathEngine()->TransformXform(*(FVector*)&Dst, *(FTransform const*)&Frame,
                                         *(FVector const*)&Src);
        return Dst;
    }

    // Out = transpose(M) * Src through the shared 3DMathEngine.
    Vector MathTransposeTransform(Matrix const& M, Vector const& Src)
    {
        Vector Out;
        MathTransposeTransformVia3DMathEngine(&Out, &M, &Src);
        return Out;
    }

    // on a frame whose Dim holds the half extents.
    bool ClampToBoxSurface(FZoneFrame& Frame, Vector const& Src, Vector& Dst)
    {
        return ClampPointToBoxSurface((unsigned char*)&Frame, &Src, &Dst);
    }

    // The frame every shape builds from the zone: baseline first sets the matrix to identity
    // (the routine / the routine) and then copies the zone's 12 dwords at +0x0c over it
    // (REP MOVSD, ECX = 0xc), so only the copy survives.
    FZoneFrame ZoneFrame(Universe::IZone const& Zone)
    {
        FZoneFrame Frame;
        Frame.Orientation = Zone.Orientation;
        Frame.Pos = Zone.Pos;
        Frame.Dim[0] = 0.0f;
        Frame.Dim[1] = 0.0f;
        Frame.Dim[2] = 0.0f;
        return Frame;
    }

    void ZoneTypeNotSupported(int Line)
    {
        FDUMP(0x100002, "%s(%d) : *** WARNING: %s", kSystemsSource, Line, kZoneTypeNotSupported);
    }

    // (thiscall, RET 4). module storage-06339f31 copy the 9 matrix dwords to
    // +0x3c, module storage-06339f49 copy Radii to +0x30, then each column of the copy is
    // scaled by that axis's squared radius: column 0 (+0x3c/+0x48/+0x54) by r0*r0,
    // column 1 (+0x40/+0x4c/+0x58) by r1*r1, column 2 (+0x44/+0x50/+0x5c) by r2*r2.
    // Each product is FLD r / FLD ST0 / FMULP (r*r on the stack), FMUL [elem], FSTP [elem].
    void EllipsoidSetRadii(FZoneEllipsoid& Ellipsoid, Vector const& Radii)
    {
        Ellipsoid.Scaled = Ellipsoid.Orientation;
        Ellipsoid.Radius[0] = Radii.x;
        Ellipsoid.Radius[1] = Radii.y;
        Ellipsoid.Radius[2] = Radii.z;
        for (int Axis = 0; Axis < 3; ++Axis)
        {
            double const Square = (double)(Ellipsoid.Radius[Axis]) * Ellipsoid.Radius[Axis];
            for (int Row = 0; Row < 3; ++Row)
                Ellipsoid.Scaled.E[Row][Axis] =
                    (float)(Square * Ellipsoid.Scaled.E[Row][Axis]);
        }
    }

    // (thiscall). The smallest radius:
    //   FLD r0 / FCOMP r1 / TEST 5 / JP -> r0 >= r1 (or unordered): pick between r1 and r2;
    //   otherwise FLD r0 / FCOMP r2 / TEST 5 / JP -> r2 when r0 >= r2, else r0.
    // FLD r1 / FCOMP r2 / TEST 5 / JP -> r2 when r1 >= r2, else r1.
    float EllipsoidMinRadius(FZoneEllipsoid const& Ellipsoid)
    {
        float const R0 = Ellipsoid.Radius[0];
        float const R1 = Ellipsoid.Radius[1];
        float const R2 = Ellipsoid.Radius[2];
        if (R0 < R1)
        {
            if (R0 < R2)
                return R0;
            return R2;
        }
        if (R1 < R2)
            return R1;
        return R2;
    }

    // (thiscall, RET 4). The ellipsoid's radius along a unit local direction:
    //   A = r0*r0, B = r1*r1, C = r2*r2 (stack);
    //   S = (x*x*C*B + z*z*B*A) + y*y*C*A (each product left to right, stored as a double);
    //   P = (A * (C*B)) * S; FCOM 1e-4 (double module storage) / TEST 0x41 / JNZ -> 0.0f when
    //   P <= 1e-4 or unordered; else sqrt(P) / S.
    double EllipsoidRadiusAlong(FZoneEllipsoid const& Ellipsoid, Vector const& Dir)
    {
        double const A = (double)(Ellipsoid.Radius[0]) * Ellipsoid.Radius[0];
        double const B = (double)(Ellipsoid.Radius[1]) * Ellipsoid.Radius[1];
        double const C = (double)(Ellipsoid.Radius[2]) * Ellipsoid.Radius[2];
        double const X = Dir.x;
        double const Y = Dir.y;
        double const Z = Dir.z;
        double const Sum = ((((X * X) * C) * B) + (((Z * Z) * B) * A)) + (((Y * Y) * C) * A);
        double const Product = (A * (C * B)) * Sum;
        if (!(Product > 1.0e-4))
            return 0.0;
        return sqrt(Product) / Sum;
    }

    // The ring's surface scale along a unit tube direction W -- inlined twice in baseline, at
    // (slot +0x44) and module storage-0633a8f6 (slot +0x48):
    //   HalfH = Size.z * 0.5 (double 0.5); Cutoff = HalfH / sqrt(Size.x^2 + HalfH^2);
    //   FCOMPP |W.y| vs Cutoff / TEST 0x41 / JNZ -> the radial branch when |W.y| <= Cutoff or
    //   unordered; otherwise HalfH / |W.y|. Radial: ((Size.x - Size.y) * 0.5) /
    //   sqrt(W.z^2 + W.x^2).
    double RingSurfaceScale(Vector const& Size, Vector const& W)
    {
        double const HalfH = (double)Size.z * 0.5;
        double const AbsY = fabs((double)(W.y));
        double const Cutoff = HalfH / sqrt((double)Size.x * Size.x + HalfH * HalfH);
        if (AbsY > Cutoff)
            return HalfH / AbsY;
        return (((double)Size.x - Size.y) * 0.5) /
               sqrt((double)(W.z) * W.z + (double)(W.x) * W.x);
    }

    // The clip functions' shared tail (cylinder module storage, ring module storage, box
    // ): the hit point Start + (End - Start) * T. The x product is stored as a
    // float before the add, y stays on the stack, and z's difference is stored as a float
    // before the multiply.
    void WriteClipPoint(FZoneSegment const& Segment, float T, Vector& Out)
    {
        double const DeltaX = (double)(Segment.End.x) - Segment.Start.x;
        double const DeltaY = (double)(Segment.End.y) - Segment.Start.y;
        float const DeltaZ = (float)((double)(Segment.End.z) - Segment.Start.z);
        float const OffsetX = (float)(DeltaX * T);
        Out.x = OffsetX + Segment.Start.x;
        Out.y = (float)(DeltaY * T + Segment.Start.y);
        Out.z = (float)((double)(DeltaZ) * T + Segment.Start.z);
    }

    // (cdecl): where the segment enters the sphere.
    //   D = End - Start, F = Start - Center (the routine, float);
    //   A = float((Dz^2 + Dy^2) + Dx^2); B = (Dz*Fz + Dy*Fy) + Fx*Dx (stack);
    //   C0 = (Fz^2 + Fy^2) + Fx^2; Disc = float(B^2 - (C0 - R^2) * A).
    //   Disc vs 0.0 (double module storage): TEST 1 / JNZ -> false when < 0 or unordered;
    //   TEST 0x41 / JNZ -> the tangent case when <= 0 (so == 0 here).
    bool ClipSegmentSphere(FZoneSphere const& Sphere, FZoneSegment const& Segment, Vector& Out)
    {
        Vector const D = MakeVector(Segment.End.x - Segment.Start.x, Segment.End.y - Segment.Start.y,
                                    Segment.End.z - Segment.Start.z);
        Vector const F = MakeVector(Segment.Start.x - Sphere.Center.x, Segment.Start.y - Sphere.Center.y,
                                    Segment.Start.z - Sphere.Center.z);
        float const A = (float)(((double)(D.z) * D.z + (double)D.y * D.y) +
                                           (double)D.x * D.x);
        double const B = ((double)(D.z) * F.z + (double)D.y * F.y) +
                         (double)(F.x) * D.x;
        double const C0 = ((double)(F.z) * F.z + (double)(F.y) * F.y) +
                          (double)(F.x) * F.x;
        double const R = Sphere.Radius;
        float const Disc = (float)(B * B - (C0 - R * R) * A);

        if (!(Disc >= 0.0f))
            return false;

        if (!(Disc > 0.0f))
        {
            // T = -(B / A); FST float; T vs 0.0 (TEST 1 / JNZ -> false when < 0
            // or unordered); the float T vs 1.0 (TEST 0x41 / JP -> false when > 1 or
            // unordered). x and y are Delta * T + Start on the stack, z's product is stored.
            double const T = -(B / A);
            float const TF = (float)T;
            if (!(T >= 0.0))
                return false;
            if (!(TF <= 1.0f))
                return false;
            float const OffsetZ = D.z * TF;
            Out.x = (float)((double)D.x * TF + Segment.Start.x);
            Out.y = (float)((double)D.y * TF + Segment.Start.y);
            Out.z = OffsetZ + Segment.Start.z;
            return true;
        }

        // T1 = float((-B - sqrt) * (1/A)), T2 = float((sqrt - B) * (1/A)).
        double const Root = sqrt((double)(Disc));
        double const InvA = 1.0 / A;
        float const T1 = (float)((-B - Root) * InvA);
        float const T2 = (float)((Root - B) * InvA);
        // T1 vs 1.0: TEST 0x41 / JZ -> false when T1 > 1. T2 vs 0.0: TEST 5 / JNP -> false when
        // T2 < 0. T1 vs 0.0f: TEST 1 / JNZ -> T2 when T1 < 0 or unordered, else T1.
        if (T1 > 1.0f)
            return false;
        if (T2 < 0.0f)
            return false;
        float const T = (T1 >= 0.0f) ? T1 : T2;
        float const OffsetY = D.y * T;
        float const OffsetZ = D.z * T;
        Out.x = (float)((double)D.x * T + Segment.Start.x);
        Out.y = OffsetY + Segment.Start.y;
        Out.z = OffsetZ + Segment.Start.z;
        return true;
    }

    // (cdecl): the ellipsoid clip. Both ends go into the ellipsoid's local frame
    // (engine vt+0x38), are divided per axis by the radii, clipped against the unit sphere,
    // and a hit is scaled back, rotated (engine vt+0x2c) and moved to the ellipsoid.
    bool ClipSegmentEllipsoid(FZoneFrame const& Ellipsoid, FZoneSegment const& Segment, Vector& Out)
    {
        Vector const StartRel = Vec3Sub(Segment.Start, Ellipsoid.Pos);
        Vector const LocalStart = EngineTransposeTransform(Ellipsoid.Orientation, StartRel);
        Vector const EndRel = MakeVector(Segment.End.x - Ellipsoid.Pos.x, Segment.End.y - Ellipsoid.Pos.y,
                                         Segment.End.z - Ellipsoid.Pos.z);
        Vector const LocalEnd = EngineTransposeTransform(Ellipsoid.Orientation, EndRel);

        // FLD 1.0f / FDIV radius stays on the stack; the start
        // uses all three unrounded, the end's z uses the inverse after FST float.
        double const InvX = 1.0 / Ellipsoid.Dim[0];
        double const InvY = 1.0 / Ellipsoid.Dim[1];
        double const InvZ = 1.0 / Ellipsoid.Dim[2];
        float const InvZF = (float)(InvZ);

        FZoneSegment Unit;
        Unit.Start = MakeVector((float)(LocalStart.x * InvX), (float)(LocalStart.y * InvY),
                                (float)(LocalStart.z * InvZ));
        Unit.End = MakeVector((float)(LocalEnd.x * InvX), (float)(InvY * LocalEnd.y),
                              (float)((double)(InvZF) * LocalEnd.z));

        FZoneSphere UnitSphere;
        UnitSphere.Center = MakeVector(0.0f, 0.0f, 0.0f);
        UnitSphere.Radius = 1.0f;

        bool const bHit = ClipSegmentSphere(UnitSphere, Unit, Out);
        if (bHit)
        {
            // .
            Out = MakeVector(Out.x * Ellipsoid.Dim[0], Ellipsoid.Dim[1] * Out.y, Out.z * Ellipsoid.Dim[2]);
            Out = EngineTransformMatrix(Ellipsoid.Orientation, Out);
            Out = MakeVector(Ellipsoid.Pos.x + Out.x, Ellipsoid.Pos.y + Out.y, Ellipsoid.Pos.z + Out.z);
        }
        return bHit;
    }

    // One side-wall pass of the cylinder and ring clips: the roots of the circle equation in
    // the local XZ plane, kept only when inside the height band. TBest is the best parameter
    // so far (2.0f on entry to the first pass) and is also the upper limit here -- the
    // cylinder and the ring's outer pass compare against 2.0f (module storage) while TBest
    // is still 2.0f, the ring's inner pass against TBest itself.
    //   Disc > 0 (TEST 0x41 / JNZ not taken): T1 = float((-B - sqrt) / 2A),
    //     T2 = float((sqrt - B) / 2A). T1 counts when T1 >= 0.0 (TEST 1 / JNZ), T1 <= limit
    //     (TEST 0x41 / JP) and HalfH >= |Dy * T1 + LS.y| (TEST 1 / JNZ), Dy = LE.y - LS.y on
    //     the stack (stored as a float for the T2 test). Then TBest = T2 if T2 >= 0.0,
    //     T2 <= T1 and HalfH >= |DyF * T2 + LS.y|, else T1. When T1 does not count, T2 does
    //     under T2 >= 0.0, T2 <= limit and the height test with Dy recomputed.
    //   Disc == 0 (TEST 0x44 / JP not taken): T = -B / 2A, counts when T >= 0.0 (stack),
    //     float T <= limit and the height test.
    void ClipSideWall(float A, double B, double Disc, Vector const& LS, Vector const& LE, float HalfH,
                      float& TBest)
    {
        if (Disc > 0.0)
        {
            double const Root = sqrt(Disc);
            double const TwoA = (double)(A) + A;
            float const T1 = (float)((-B - Root) / TwoA);
            float const T2 = (float)((Root - B) / TwoA);

            if (T1 >= 0.0f && T1 <= TBest)
            {
                double const Dy = (double)LE.y - LS.y;
                float const DyF = (float)(Dy);
                if (HalfH >= fabs(Dy * T1 + LS.y))
                {
                    if (T2 >= 0.0f && T2 <= T1 && HalfH >= fabs((double)(DyF) * T2 + LS.y))
                        TBest = T2;
                    else
                        TBest = T1;
                    return;
                }
            }

            if (T2 >= 0.0f && T2 <= TBest &&
                HalfH >= fabs(((double)LE.y - LS.y) * T2 + LS.y))
                TBest = T2;
            return;
        }

        if (Disc == 0.0)
        {
            double const T = -B / ((double)(A) + A);
            float const TF = (float)T;
            if (T >= 0.0 && TF <= TBest && HalfH >= fabs(((double)LE.y - LS.y) * TF + LS.y))
                TBest = TF;
        }
    }

    // A cap plane of the cylinder / ring at local height where Num = +-HalfH - LS.y.
    // The plane counts when (Dir.y > 0 and Num > 0) or (Dir.y < 0 and Num < 0) (each TEST
    // 0x41 / JZ or TEST 5 / JP against 0.0f); then T = Num / Dir.y on the stack must be > 0
    // (TEST 0x41 / JNZ) and its float below TBest (TEST 5 / JP). The crossing is
    // X = LS.x + float(DxF * T), Z = LS.z + DzF * T (stack), S = Z^2 + X^2.
    bool CapCrossing(float Num, float DirY, Vector const& LS, float DxF, float DzF, float TBest, float& OutT,
                     double& OutRadialSq)
    {
        if (!((DirY > 0.0f && Num > 0.0f) || (DirY < 0.0f && Num < 0.0f)))
            return false;
        double const T = (double)(Num) / DirY;
        float const TF = (float)T;
        if (!(T > 0.0))
            return false;
        if (!(TF < TBest))
            return false;
        float const OffsetX = DxF * TF;
        double const X = (double)LS.x + OffsetX;
        double const Z = (double)LS.z + (double)(DzF) * TF;
        OutT = TF;
        OutRadialSq = Z * Z + X * X;
        return true;
    }

    // (cdecl): the cylinder clip, Dim = {radius, height}.
    bool ClipSegmentCylinder(FZoneFrame const& Cylinder, FZoneSegment const& Segment, Vector& Out)
    {
        Vector const StartRel = MakeVector(Segment.Start.x - Cylinder.Pos.x, Segment.Start.y - Cylinder.Pos.y,
                                           Segment.Start.z - Cylinder.Pos.z);
        Vector const LS = EngineTransposeTransform(Cylinder.Orientation, StartRel);
        Vector const EndRel = MakeVector(Segment.End.x - Cylinder.Pos.x, Segment.End.y - Cylinder.Pos.y,
                                         Segment.End.z - Cylinder.Pos.z);
        Vector const LE = EngineTransposeTransform(Cylinder.Orientation, EndRel);

        // .
        float const HalfH = (float)((double)(Cylinder.Dim[1]) * 0.5);
        float TBest = 2.0f;
        double const Dx = (double)(LE.x) - LS.x;
        float const DxF = (float)(Dx);
        double const Dz = (double)(LE.z) - LS.z;
        float const DzF = (float)(Dz);
        float const A = (float)(Dz * Dz + Dx * Dx);
        double B = (double)LS.z * Dz + (double)LS.x * Dx;
        B = B + B;
        double const Radius = Cylinder.Dim[0];
        double const C0 = (double)LS.z * LS.z + (double)LS.x * LS.x;
        double const Disc = B * B - ((C0 - Radius * Radius) * A) * 4.0f;

        ClipSideWall(A, B, Disc, LS, LE, HalfH, TBest);

        // the two caps, with the segment direction in local space.
        Vector const Along = MakeVector(Segment.End.x - Segment.Start.x, Segment.End.y - Segment.Start.y,
                                        Segment.End.z - Segment.Start.z);
        Vector const LD = EngineTransposeTransform(Cylinder.Orientation, Along);
        double const RadiusSq = Radius * Radius;

        float CapT = 0.0f;
        double RadialSq = 0.0;
        float const Num1 = (float)(-(double)HalfH - LS.y);
        if (CapCrossing(Num1, LD.y, LS, DxF, DzF, TBest, CapT, RadialSq) && RadiusSq > RadialSq)
            TBest = CapT;
        float const Num2 = (float)((double)HalfH - LS.y);
        if (CapCrossing(Num2, LD.y, LS, DxF, DzF, TBest, CapT, RadialSq) && RadiusSq > RadialSq)
            TBest = CapT;

        // TBest vs 0.0f: TEST 1 / JNZ -> false when < 0 or unordered; vs 1.0: TEST 0x41 / JP ->
        // false when > 1 or unordered.
        if (!(TBest >= 0.0f))
            return false;
        if (!(TBest <= 1.0f))
            return false;
        WriteClipPoint(Segment, TBest, Out);
        return true;
    }

    // (cdecl): the ring clip, Dim = {outer radius, inner radius, height}.
    bool ClipSegmentRing(FZoneFrame const& Ring, FZoneSegment const& Segment, Vector& Out)
    {
        Vector const StartRel = MakeVector(Segment.Start.x - Ring.Pos.x, Segment.Start.y - Ring.Pos.y,
                                           Segment.Start.z - Ring.Pos.z);
        Vector const LS = EngineTransposeTransform(Ring.Orientation, StartRel);
        Vector const EndRel = MakeVector(Segment.End.x - Ring.Pos.x, Segment.End.y - Ring.Pos.y,
                                         Segment.End.z - Ring.Pos.z);
        Vector const LE = EngineTransposeTransform(Ring.Orientation, EndRel);

        // the outer wall, as in the cylinder.
        float const HalfH = (float)((double)(Ring.Dim[2]) * 0.5);
        float TBest = 2.0f;
        double const Dx = (double)(LE.x) - LS.x;
        float const DxF = (float)(Dx);
        double const Dz = (double)(LE.z) - LS.z;
        float const DzF = (float)(Dz);
        float const A = (float)(Dz * Dz + Dx * Dx);
        double B = (double)LS.z * Dz + (double)LS.x * Dx;
        B = B + B;
        double const C0 = (double)LS.z * LS.z + (double)LS.x * LS.x;
        double const Outer = Ring.Dim[0];
        double const OuterDisc = B * B - ((C0 - Outer * Outer) * A) * 4.0f;
        ClipSideWall(A, B, OuterDisc, LS, LE, HalfH, TBest);

        // the inner wall with the same B and C0, limited by TBest.
        double const Inner = Ring.Dim[1];
        double const InnerDisc = B * B - ((C0 - Inner * Inner) * A) * 4.0f;
        ClipSideWall(A, B, InnerDisc, LS, LE, HalfH, TBest);

        // the caps count between the two radii: S < outer^2
        // (TEST 5 / JP) and S > inner^2 (TEST 0x41 / JNZ).
        Vector const Along = MakeVector(Segment.End.x - Segment.Start.x, Segment.End.y - Segment.Start.y,
                                        Segment.End.z - Segment.Start.z);
        Vector const LD = EngineTransposeTransform(Ring.Orientation, Along);
        double const OuterSq = Outer * Outer;
        double const InnerSq = Inner * Inner;

        float CapT = 0.0f;
        double RadialSq = 0.0;
        float const Num1 = (float)(-(double)HalfH - LS.y);
        if (CapCrossing(Num1, LD.y, LS, DxF, DzF, TBest, CapT, RadialSq) && RadialSq < OuterSq &&
            RadialSq > InnerSq)
            TBest = CapT;
        float const Num2 = (float)((double)HalfH - LS.y);
        if (CapCrossing(Num2, LD.y, LS, DxF, DzF, TBest, CapT, RadialSq) && RadialSq < OuterSq &&
            RadialSq > InnerSq)
            TBest = CapT;

        if (!(TBest >= 0.0f))
            return false;
        if (!(TBest <= 1.0f))
            return false;
        WriteClipPoint(Segment, TBest, Out);
        return true;
    }

    // The face point the box clip builds with three the routine calls: Q = LE - LS,
    // Q = Q * T, P = LS + Q, each a float store.
    Vector BoxFacePoint(Vector const& LS, Vector const& LE, float T)
    {
        Vector const Q = MakeVector(LE.x - LS.x, LE.y - LS.y, LE.z - LS.z);
        Vector const Scaled = MakeVector(Q.x * T, Q.y * T, Q.z * T);
        return MakeVector(LS.x + Scaled.x, LS.y + Scaled.y, LS.z + Scaled.z);
    }

    // (cdecl): the box clip, Dim = the full size.
    bool ClipSegmentBox(FZoneFrame const& Box, FZoneSegment const& Segment, Vector& Out)
    {
        Vector const StartRel = MakeVector(Segment.Start.x - Box.Pos.x, Segment.Start.y - Box.Pos.y,
                                           Segment.Start.z - Box.Pos.z);
        Vector const LS = EngineTransposeTransform(Box.Orientation, StartRel);
        Vector const EndRel = MakeVector(Segment.End.x - Box.Pos.x, Segment.End.y - Box.Pos.y,
                                         Segment.End.z - Box.Pos.z);
        Vector const LE = EngineTransposeTransform(Box.Orientation, EndRel);

        // a start inside the box (each FCOMPP / TEST 0x41 / JNZ,
        // half extent > |coordinate|; x compares the unrounded half) returns the start in the
        // box's LOCAL frame, not the world -- baseline writes the local vector at [ESP+0x14].
        double const HalfX = (double)(Box.Dim[0]) * 0.5;
        float const HalfXF = (float)(HalfX);
        float const HalfYF = (float)((double)(Box.Dim[1]) * 0.5);
        float const HalfZF = (float)((double)(Box.Dim[2]) * 0.5);
        if (HalfX > fabs((double)LS.x) && HalfYF > fabs(LS.y) && HalfZF > fabs(LS.z))
        {
            Out = LS;
            return true;
        }

        Vector const Along = MakeVector(Segment.End.x - Segment.Start.x, Segment.End.y - Segment.Start.y,
                                        Segment.End.z - Segment.Start.z);
        Vector const LD = EngineTransposeTransform(Box.Orientation, Along);
        float TBest = 2.0f;

        // X faces (module storage-06335e7b): LD.x > 0 (TEST 0x41 / JNZ) uses Num = -HalfX - LS.x
        // > 0, LD.x < 0 (TEST 5 / JP) uses Num = HalfX - LS.x < 0. T = float(Num / LD.x) is
        // NOT compared with TBest.
        {
            double Num = 0.0;
            bool bFace = false;
            if (LD.x > 0.0f)
            {
                Num = -(double)(HalfXF) - LS.x;
                bFace = Num > 0.0;
            }
            else if (LD.x < 0.0f)
            {
                Num = (double)(HalfXF) - LS.x;
                bFace = Num < 0.0;
            }
            if (bFace)
            {
                float const TF = (float)(Num / LD.x);
                Vector const P = BoxFacePoint(LS, LE, TF);
                if (HalfYF > fabs(P.y) && HalfZF > fabs(P.z))
                    TBest = TF;
            }
        }

        // Y faces (module storage-06336090): as X, but T on the stack must be below TBest
        // (TEST 5 / JP).
        {
            double Num = 0.0;
            bool bFace = false;
            if (LD.y > 0.0f)
            {
                Num = -(double)(HalfYF) - LS.y;
                bFace = Num > 0.0;
            }
            else if (LD.y < 0.0f)
            {
                Num = (double)(HalfYF) - LS.y;
                bFace = Num < 0.0;
            }
            if (bFace)
            {
                double const T = Num / LD.y;
                float const TF = (float)T;
                if (T < TBest)
                {
                    Vector const P = BoxFacePoint(LS, LE, TF);
                    if (HalfXF > fabs(P.x) && HalfZF > fabs(P.z))
                        TBest = TF;
                }
            }
        }

        // Z faces (module storage-0633624e). The +Z face adds the Q * T products on the
        // stack to LS and stores the point; the -Z face stores x and keeps y on the stack.
        if (LD.z > 0.0f)
        {
            double const Num = -(double)(HalfZF) - LS.z;
            if (Num > 0.0)
            {
                double const T = Num / LD.z;
                float const TF = (float)T;
                if (T < TBest)
                {
                    Vector const Q = MakeVector(LE.x - LS.x, LE.y - LS.y, LE.z - LS.z);
                    Vector const P = MakeVector((float)(LS.x + (double)(Q.x) * TF),
                                                (float)(LS.y + (double)(Q.y) * TF),
                                                (float)(LS.z + (double)(Q.z) * TF));
                    if (HalfXF > fabs(P.x) && HalfYF > fabs(P.y))
                        TBest = TF;
                }
            }
        }
        else if (LD.z < 0.0f)
        {
            double const Num = (double)(HalfZF) - LS.z;
            if (Num < 0.0)
            {
                double const T = Num / LD.z;
                float const TF = (float)T;
                if (T < TBest)
                {
                    Vector const Q = MakeVector(LE.x - LS.x, LE.y - LS.y, LE.z - LS.z);
                    float const PX = (float)(LS.x + (double)(Q.x) * TF);
                    double const PY = (double)(LS.y) + (double)(Q.y) * TF;
                    if (HalfXF > fabs(PX) && HalfYF > fabs(PY))
                        TBest = TF;
                }
            }
        }

        // only the upper bound (TEST 0x41 / JP -> false when > 1.0 or
        // unordered); there is no lower-bound test in the box clip.
        if (!(TBest <= 1.0f))
            return false;
        WriteClipPoint(Segment, TBest, Out);
        return true;
    }

    // ------------------------------------------------------------------------------------
    // Frustum / sphere culling: FrustumCull (0633c6d0), SphereCull (0633d770) and their
    // helpers 0633cb50 / 0633cc20 / 0633d040 / 0633d150 / 0633d1e0 / 0633d5d0 / 0633daa0 /
    // .
    // ------------------------------------------------------------------------------------

    float const kCullHalf = 0.5f;          // (float)
    float const kCullHalfPi = 1.5707964f;  // (float)
    float const kCullPi = 3.1415927f;      // (float)

    // .data:the field (int 5): the number of steps of FrustumCull's far-cap sweep. baseline
    // reads it again on every iteration (MOV EAX / FIDIV).
    int GFrustumCapSteps = 5;

    // The plane the routine builds: unit normal, then the offset along it.
    struct FCullPlane
    {
        Vector Normal; // +0x00
        float D;       // +0x0c
    };

    // The quaternion the routine fills (W first).
    struct FCullQuat
    {
        float W; // +0x00
        float X; // +0x04
        float Y; // +0x08
        float Z; // +0x0c
    };

    // / the routine / the routine: column 0 / 1 / 2 of the frustum matrix.
    Vector FrustumColumn(Geometry::Frustum const& Frustum, int Column)
    {
        Matrix const& Rotation = Frustum.Xform.Rotation;
        return MakeVector(Rotation.E[0][Column], Rotation.E[1][Column], Rotation.E[2][Column]);
    }

    // (thiscall, RET 4): each component negated.
    Vector VecNegate(Vector const& V)
    {
        return MakeVector(-V.x, -V.y, -V.z);
    }

    // (thiscall, RET 8): Normal = N, then scaled by 1.0f / sqrt((x*x + y*y) + z*z)
    // (FDIVR float 1.0); each scaled component is stored (FST) but stays on the stack, and
    // D = (Nz'*Pz + Ny'*Py) + Nx'*Px from those unrounded values. No guard: a zero normal
    // gives inf * 0, as in baseline.
    void CullPlaneSet(FCullPlane& Plane, Vector const& Point, Vector const& N)
    {
        Plane.Normal = N;
        double const Inverse = 1.0 / sqrt(((double)Plane.Normal.x * Plane.Normal.x +
                                           (double)(Plane.Normal.y) * Plane.Normal.y) +
                                          (double)(Plane.Normal.z) * Plane.Normal.z);
        double const NX = Inverse * Plane.Normal.x;
        Plane.Normal.x = (float)(NX);
        double const NY = Inverse * Plane.Normal.y;
        Plane.Normal.y = (float)(NY);
        double const NZ = Inverse * Plane.Normal.z;
        Plane.Normal.z = (float)(NZ);
        Plane.D = (float)((NZ * Point.z + NY * Point.y) + NX * Point.x);
    }

    // (thiscall, RET 8): h = Angle * 0.5f (on the stack);
    // k = sin(h) / sqrt((x*x + y*y) + z*z) stored as float; W = cos(h); X/Y/Z = k * axis.
    void CullQuatFromAxisAngle(FCullQuat& Quat, Vector const& Axis, float Angle)
    {
        double const Half = (double)(Angle) * kCullHalf;
        double const Length = sqrt(((double)(Axis.x) * Axis.x + (double)(Axis.y) * Axis.y) +
                                   (double)(Axis.z) * Axis.z);
        float const Scale = (float)(X87Sin(Half) / Length);
        Quat.W = X87CosF(Half);
        Quat.X = Scale * Axis.x;
        Quat.Y = Scale * Axis.y;
        Quat.Z = Scale * Axis.z;
    }

    // (cdecl, Out / Quat / V): V rotated by Quat,
    //   Out = (2W^2 - 1) V + 2 (q . V) q - 2W (V x q)
    // with baseline's float stores: (V x q).z, 2W, the x term of 2W (V x q), the y and z terms
    // of 2 (q . V) q, the three (2W^2 - 1) V products and the z sum are floats; the rest stays
    // on the stack.
    Vector CullQuatRotate(FCullQuat const& Quat, Vector const& V)
    {
        double const CrossX = (double)Quat.Z * V.y - (double)V.z * Quat.Y;
        double const CrossY = (double)V.z * Quat.X - (double)Quat.Z * V.x;
        float const CrossZ = (float)((double)V.x * Quat.Y - (double)Quat.X * V.y);
        float const TwoW = (float)((double)Quat.W + Quat.W);
        float const SpinX = (float)(CrossX * TwoW);
        double const SpinY = CrossY * TwoW;
        double const SpinZ = (double)(CrossZ) * TwoW;

        double const TwoDot = (((double)Quat.Z * V.z + (double)Quat.Y * V.y) +
                               (double)Quat.X * V.x) * 2.0;
        double const AlongX = TwoDot * Quat.X;
        float const AlongY = (float)(TwoDot * Quat.Y);
        float const AlongZ = (float)(TwoDot * Quat.Z);

        double const Cos2 = ((double)Quat.W * Quat.W) * 2.0 - 1.0;
        float const KeepX = (float)(Cos2 * V.x);
        float const KeepY = (float)(Cos2 * V.y);
        float const KeepZ = (float)(Cos2 * V.z);

        double const SumX = (double)(KeepX) + AlongX;
        double const SumY = (double)(KeepY) + AlongY;
        float const SumZ = (float)((double)(KeepZ) + AlongZ);

        return MakeVector((float)(SumX - SpinX), (float)(SumY - SpinY),
                          (float)((double)(SumZ) - SpinZ));
    }

    // (cdecl, Out / V / Axis / Angle): the routine and the routine in one body,
    // with its own float stores: the quaternion as above; the three 2W (V x q) terms, the three
    // 2 (q . V) q terms and the three (2W^2 - 1) V terms are each built through the Vector ctor
    // (the routine, floats); the x / y sums stay on the stack, the z sum is a float. The dot
    // is summed x, y, z here (the routine sums z, y, x).
    Vector CullRotateAboutAxis(Vector const& V, Vector const& Axis, float Angle)
    {
        FCullQuat Quat;
        CullQuatFromAxisAngle(Quat, Axis, Angle);

        double const CrossX = (double)Quat.Z * V.y - (double)Quat.Y * V.z;
        double const CrossY = (double)Quat.X * V.z - (double)Quat.Z * V.x;
        double const CrossZ = (double)Quat.Y * V.x - (double)Quat.X * V.y;
        float const TwoW = (float)((double)Quat.W + Quat.W);
        Vector const Spin = MakeVector((float)(CrossX * TwoW), (float)(CrossY * TwoW),
                                       (float)(CrossZ * TwoW));

        double const TwoDot = (((double)Quat.X * V.x + (double)Quat.Y * V.y) +
                               (double)Quat.Z * V.z) * 2.0;
        Vector const Along = MakeVector((float)(TwoDot * Quat.X), (float)(Quat.Y * TwoDot),
                                        (float)(Quat.Z * TwoDot));

        double const Cos2 = ((double)Quat.W * Quat.W) * 2.0 - 1.0;
        Vector const Keep = MakeVector((float)(Cos2 * V.x), (float)(Cos2 * V.y),
                                       (float)(Cos2 * V.z));

        double const SumX = (double)(Along.x) + Keep.x;
        double const SumY = (double)(Along.y) + Keep.y;
        float const SumZ = (float)((double)(Along.z) + Keep.z);

        return MakeVector((float)(SumX - Spin.x), (float)(SumY - Spin.y),
                          (float)((double)(SumZ) - Spin.z));
    }

    // (thiscall on the frustum, RET 8): side Index of the frustum as a plane.
    //   0: through the apex, normal -column 2;
    //   1: through apex - column 2 * Distance, normal column 2;
    //   2 / 3: through the apex, normal -column 2 turned about column 1 by
    //          AngleX * 0.5f - pi/2 / pi/2 - AngleX * 0.5f (the routine + the routine);
    //   4 / 5: the same about column 0 with AngleY;
    //   above 5 (CMP 5 / JA, unsigned): point and normal zero.
    // Every case ends in the routine.
    FCullPlane FrustumPlane(Geometry::Frustum const& Frustum, unsigned Index)
    {
        Vector const& Apex = Frustum.Xform.Translation;
        Vector Point;
        Vector Normal;
        switch (Index)
        {
        case 0:
            Point = Apex;
            Normal = VecNegate(FrustumColumn(Frustum, 2));
            break;
        case 1:
        {
            Vector const Forward = FrustumColumn(Frustum, 2);
            Point = Vec3Sub(Apex, Vec3Scale(Forward, Frustum.Distance));
            Normal = FrustumColumn(Frustum, 2);
            break;
        }
        case 2:
        case 3:
        case 4:
        case 5:
        {
            Point = Apex;
            bool const bHorizontal = (Index == 2 || Index == 3);
            Vector const Axis = FrustumColumn(Frustum, bHorizontal ? 1 : 0);
            Vector const Back = VecNegate(FrustumColumn(Frustum, 2));
            double const HalfAngle = (double)(bHorizontal ? Frustum.AngleX : Frustum.AngleY) * kCullHalf;
            // Cases 2 / 4: FSUB pi/2; cases 3 / 5: FSUBR pi/2.
            float const Angle = (Index == 2 || Index == 4)
                ? (float)(HalfAngle - kCullHalfPi)
                : (float)(kCullHalfPi - HalfAngle);
            FCullQuat Quat;
            CullQuatFromAxisAngle(Quat, Axis, Angle);
            Normal = CullQuatRotate(Quat, Back);
            break;
        }
        default:
            Point = MakeVector(0.0f, 0.0f, 0.0f);
            Normal = Point;
            break;
        }

        FCullPlane Plane;
        CullPlaneSet(Plane, Point, Normal);
        return Plane;
    }

    // (thiscall on the ellipsoid, RET 4): which side of Plane the ellipsoid lies.
    // The 3x3 built with the routine from the scaled matrix's columns is its transpose;
    // V = that * Normal (engine vt+0x2c through the field); Dist = (Py*Ny + Pz*Nz) +
    // Nx*Px - D; Q = (Vz*Nz + Vx*Nx) + Vy*Ny.
    //   FCOMPP Q vs Dist^2 / TEST 0x41 / JP -> 0 when Q > Dist^2 or unordered (it straddles);
    //   FCOMP Dist vs 0.0f / TEST 5 / JNP -> 2 when Dist < 0, otherwise 1.
    int EllipsoidPlaneSide(FZoneEllipsoid const& Ellipsoid, FCullPlane const& Plane)
    {
        Matrix Transposed;
        for (int Row = 0; Row < 3; ++Row)
        {
            for (int Column = 0; Column < 3; ++Column)
                Transposed.E[Row][Column] = Ellipsoid.Scaled.E[Column][Row];
        }
        Vector const Spread = EngineTransformMatrix(Transposed, Plane.Normal);

        double const Dist = (((double)(Ellipsoid.Pos.y) * Plane.Normal.y +
                              (double)(Ellipsoid.Pos.z) * Plane.Normal.z) +
                             (double)Plane.Normal.x * Ellipsoid.Pos.x) - Plane.D;
        double const Extent = ((double)(Spread.z) * Plane.Normal.z +
                               (double)(Spread.x) * Plane.Normal.x) +
                              (double)(Spread.y) * Plane.Normal.y;
        if (!(Extent <= Dist * Dist))
            return 0;
        if (Dist < 0.0f)
            return 2;
        return 1;
    }

    // (cdecl, Frustum / Sphere): true when the sphere lies wholly on the outer
    // side of one of the six sides. The sides are built as the routine builds them, except
    // that cases 2 / 3 / 5 turn the normal with the routine (case 4 keeps the routine +
    // ). Per side: Dist = ((Nx*Cx + Nz*Cz) + Ny*Cy) - D;
    //   FCOM Dist vs R / TEST 0x41 / JNZ -> 1 when Dist > R;
    //   else FCOMPP Dist vs -R / TEST 5 / JP -> 2 when Dist < -R, else 0;
    // the first 2 returns true. (baseline keeps the six results in a local array nobody reads.)
    bool FrustumCullsSphere(Geometry::Frustum const& Frustum, FZoneSphere const& Sphere)
    {
        Vector const& Apex = Frustum.Xform.Translation;
        for (unsigned Index = 0; Index < 6; ++Index)
        {
            Vector Point;
            Vector Normal;
            switch (Index)
            {
            case 0:
                Point = Apex;
                Normal = VecNegate(FrustumColumn(Frustum, 2));
                break;
            case 1:
                Point = Vec3Sub(Apex, Vec3Scale(FrustumColumn(Frustum, 2), Frustum.Distance));
                Normal = FrustumColumn(Frustum, 2);
                break;
            case 2:
                Point = Apex;
                Normal = CullRotateAboutAxis(VecNegate(FrustumColumn(Frustum, 2)), FrustumColumn(Frustum, 1),
                                             (float)((double)Frustum.AngleX * kCullHalf - kCullHalfPi));
                break;
            case 3:
                Point = Apex;
                Normal = CullRotateAboutAxis(VecNegate(FrustumColumn(Frustum, 2)), FrustumColumn(Frustum, 1),
                                             (float)(kCullHalfPi - (double)Frustum.AngleX * kCullHalf));
                break;
            case 4:
            {
                Point = Apex;
                FCullQuat Quat;
                CullQuatFromAxisAngle(Quat, FrustumColumn(Frustum, 0),
                                      (float)((double)Frustum.AngleY * kCullHalf - kCullHalfPi));
                Normal = CullQuatRotate(Quat, VecNegate(FrustumColumn(Frustum, 2)));
                break;
            }
            case 5:
                Point = Apex;
                Normal = CullRotateAboutAxis(VecNegate(FrustumColumn(Frustum, 2)), FrustumColumn(Frustum, 0),
                                             (float)(kCullHalfPi - (double)Frustum.AngleY * kCullHalf));
                break;
            default:
                Point = MakeVector(0.0f, 0.0f, 0.0f);
                Normal = Point;
                break;
            }

            FCullPlane Plane;
            CullPlaneSet(Plane, Point, Normal);

            double const Dist = (((double)Plane.Normal.x * Sphere.Center.x +
                                  (double)(Plane.Normal.z) * Sphere.Center.z) +
                                 (double)(Plane.Normal.y) * Sphere.Center.y) - Plane.D;
            double const Radius = Sphere.Radius;
            int Side = 0;
            if (Dist > Radius)
                Side = 1;
            else if (Dist < -Radius)
                Side = 2;
            if (Side == 2)
                return true;
        }
        return false;
    }

    // (thiscall on a frame whose Dim holds half extents, RET 4): signed squared
    // distance from Point to the box surface, negative inside. Delta = Point - Pos (floats),
    // Local = engine vt+0x38 (the field) transpose(Orientation) * Delta. Per axis
    // e = |L| - Dim (stored float); FCOMP 0.0f / TEST 1 / JNZ keeps L unless e >= 0, where the
    // closest coordinate becomes _copysign(Dim, L) and the point counts as outside. Inside on
    // all three axes, the axis with the largest e is snapped (each FCOMP / TEST 1 / JNZ moves
    // on when the left side is smaller or unordered). The z coordinate stays on the stack.
    double BoxSignedDistanceSq(FZoneFrame const& Frame, Vector const& Point)
    {
        Vector const Delta = MakeVector(Point.x - Frame.Pos.x, Point.y - Frame.Pos.y, Point.z - Frame.Pos.z);
        Vector const Local = EngineTransposeTransform(Frame.Orientation, Delta);

        bool bOutside = false;
        float ClosestX = Local.x;
        float ClosestY = Local.y;
        double ClosestZ = Local.z;

        double const RawX = fabs((double)Local.x) - Frame.Dim[0];
        float const ExcessX = (float)(RawX);
        if (RawX >= 0.0)
        {
            ClosestX = (float)(copysign((double)(Frame.Dim[0]), (double)Local.x));
            bOutside = true;
        }

        double const RawY = fabs((double)Local.y) - Frame.Dim[1];
        float const ExcessY = (float)(RawY);
        if (RawY >= 0.0)
        {
            ClosestY = (float)(copysign((double)(Frame.Dim[1]), (double)Local.y));
            bOutside = true;
        }

        double const RawZ = fabs((double)Local.z) - Frame.Dim[2];
        float const ExcessZ = (float)(RawZ);
        if (RawZ >= 0.0)
        {
            ClosestZ = copysign((double)Frame.Dim[2], (double)Local.z);
            bOutside = true;
        }
        else if (!bOutside)
        {
            if (ExcessX >= ExcessY)
            {
                if (ExcessX >= ExcessZ)
                    ClosestX = (float)(copysign((double)(Frame.Dim[0]), (double)Local.x));
                else
                    ClosestZ = copysign((double)Frame.Dim[2], (double)Local.z);
            }
            else
            {
                if (ExcessY >= ExcessZ)
                    ClosestY = (float)(copysign((double)(Frame.Dim[1]), (double)Local.y));
                else
                    ClosestZ = copysign((double)Frame.Dim[2], (double)Local.z);
            }
        }

        float const DX = (float)((double)Local.x - ClosestX);
        float const DY = (float)((double)Local.y - ClosestY);
        double const DZ = (double)Local.z - ClosestZ;
        double Value = ((DZ * DZ) + (double)(DY) * DY) + (double)(DX) * DX;
        if (!bOutside)
            Value = -Value;
        return Value;
    }

    // (thiscall on the ellipsoid, RET 4): Point's distance outside the surface.
    // dx and dz stay on the stack, dy is stored as float; Length = sqrt((dx^2 + dz^2) + dy^2),
    // stored as float. FCOMP 1e-4 (double module storage) / TEST 5 / JP -> continue when
    // >= or unordered; below it the result is -(smallest radius) (the the routine chain,
    // inlined). Otherwise the unit direction (1.0 / float length, double module storage) goes
    // through engine vt+0x38 (the field) and the result is Length - the routine(local).
    double EllipsoidSignedDistance(FZoneEllipsoid const& Ellipsoid, Vector const& Point)
    {
        double const DX = (double)(Point.x) - Ellipsoid.Pos.x;
        float const DY = (float)((double)(Point.y) - Ellipsoid.Pos.y);
        double const DZ = (double)(Point.z) - Ellipsoid.Pos.z;
        double const Length = sqrt((DX * DX + DZ * DZ) + (double)(DY) * DY);
        float const LengthF = (float)Length;
        if (Length < 1.0e-4)
            return -EllipsoidMinRadius(Ellipsoid);

        double const Inverse = 1.0 / LengthF;
        Vector const Dir = MakeVector((float)(DX * Inverse), (float)(DY * Inverse),
                                      (float)(DZ * Inverse));
        Vector const Local = EngineTransposeTransform(Ellipsoid.Orientation, Dir);
        return LengthF - EllipsoidRadiusAlong(Ellipsoid, Local);
    }

    // The ellipsoid FrustumCull and SphereCull build from a zone: identity / zero position
    // (the routine and three zero stores), GetSize (vt+0x28), the zone's 12 transform dwords
    // from vt+0x0c over it (REP MOVSD), then the routine with the size.
    FZoneEllipsoid CullEllipsoid(Universe::IZone const& Zone)
    {
        FZoneEllipsoid Ellipsoid;
        Vector const Size = Zone.GetSize();
        Transform const& Xform = Zone.GetTransform();
        Ellipsoid.Orientation = Xform.Rotation;
        Ellipsoid.Pos = Xform.Translation;
        EllipsoidSetRadii(Ellipsoid, Size);
        return Ellipsoid;
    }

    // One step of FrustumCull's far-cap sweep: a = Step * Sweep / steps (FILD / FMUL float /
    // FIDIV int); the plane through Center with normal Dir(a) (the routine + the routine);
    // true when the routine puts the ellipsoid on side 1.
    bool CapStepCulls(FZoneEllipsoid const& Ellipsoid, Vector const& Center, float Sweep, int Step, bool bHorizontal)
    {
        double const Angle = ((double)(Step) * Sweep) / GFrustumCapSteps;
        // FCOS; FCHS; FSTP dword and FSIN; FSTP dword: one rounding each (negation is exact).
        float const Sin = X87SinF(Angle);
        float const NegCos = -X87CosF(Angle);
        Vector const Dir = bHorizontal ? MakeVector(Sin, 0.0f, NegCos) : MakeVector(0.0f, Sin, NegCos);
        FCullPlane Plane;
        CullPlaneSet(Plane, Center, Dir);
        return EllipsoidPlaneSide(Ellipsoid, Plane) == 1;
    }
}

// module storage-06337710 builds the zone it parses into on the stack with
// these values before reading the [Zone] block: ID_String::clear at +0x04 / +0x08 / +0x6c,
// identity at +0x0c, pos 0, size 0 (the routine), CacheString::clear at +0x70, fog colour
// (1, 1, 1), the population's default constructor, the vftable, shape 0, mesh -1, edge 0,
// physical effect -1, drag 1.0, damage 0, interference 0, cached radius -1.0f, spacedust
// particles 0, loot 0, path label 0, visit 0x20, ids name / info 0, property flags 0.
ZONE::ZONE()
{
    NicknameId = 0;
    SystemId = 0;
    Orientation = Matrix(1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    Pos = MakeVector(0.0f, 0.0f, 0.0f);
    Shape = 0;
    PropertyFlags = 0;
    Size = MakeVector(0.0f, 0.0f, 0.0f);
    MeshHandle = -1;
    EdgeFraction = 0.0f;
    PhysicalEffect = -1;
    DragModifier = 1.0f;
    Damage = 0.0f;
    Interference = 0.0f;
    CachedRadius = -1.0f;
    MusicId = 0;
    ((CacheString*)&Spacedust)->clear(); // CacheString::clear (+0x70)
    SpacedustMaxParticles = 0;
    FogColor = MakeVector(1.0f, 1.0f, 1.0f);
    OwnedLootEntry = nullptr;
    bPathLabel = false;
    Visit = 0x20;
    IdsName = 0;
    IdsInfo = 0;
    // (module storage-06337676 also calls CreateID(0) and drops the result: no effect.)
}

// (thiscall, RET 4): the member-wise copy the universe loader makes of its stack
// ZONE twice per inserted zone (into the {id, ZONE} pair at module storage, and the pair into
// the zone map node by the routine). +0x04..+0x8c copied as they are (mesh handle and loot
// pointer included, so every copy's destructor releases them again, as in baseline); the
// population vector gets a new buffer of exactly its size; then the ZONE vftable.
ZONE::ZONE(ZONE const& Other) = default;

// .
ZONE::~ZONE()
{
    // a loaded mesh goes back to the engine (NeuronRuntime::Engine vt+0x3c,
    // ReleaseHandle) and the handle is reset.
    if (MeshHandle != -1)
    {
        NeuronRuntime::Engine->ReleaseHandle((void*)MeshHandle);
        MeshHandle = -1;
    }
    // the owned loot record (the routine is free()).
    if (OwnedLootEntry != nullptr)
        free(OwnedLootEntry);
    // the population buffer -- IZone::Population's destructor.
}

// .
double ZONE::GetImmersion(Vector const& Point) const
{
    // outside the bounding sphere (FCOMPP radius^2 vs distance^2,
    // TEST 5 / JP taken on >= or unordered) the answer is 0.0f.
    float const Dx = Point.x - Pos.x;
    float const Dy = Point.y - Pos.y;
    float const Dz = Point.z - Pos.z;
    double const Radius = GetRadius();
    double const DistSq = ((double)Dx * Dx + (double)(Dz) * Dz) +
                          (double)(Dy) * Dy;
    if (Radius * Radius < DistSq)
        return 0.0;

    // vt+0x48 on the point; FST float; vs 1.0 (double) with TEST 1 /
    // JNZ -> the ramp when below 1.0 or unordered, else 0.0f.
    double const Value = ClassifyPoint(Point);
    float const ValueF = (float)(Value);
    if (Value >= 1.0)
        return 0.0;

    // (1.0 - edge) vs the float value, TEST 0x41 / JZ -> 1.0f when
    // greater; edge vs 1e-4 (double), TEST 5 / JNP -> 1.0f when below; else (1 - v) / edge.
    if ((1.0 - EdgeFraction) > ValueF)
        return 1.0;
    if (EdgeFraction < 1.0e-4)
        return 1.0;
    return (1.0 - ValueF) / EdgeFraction; // left on the stack (module storage-0633acee)
}

// (ICF): MOV EAX,[ECX+0x4].
unsigned ZONE::GetId() const
{
    return NicknameId;
}

// (ICF): MOV EAX,[ECX+0x8].
unsigned ZONE::GetSystem() const
{
    return SystemId;
}

// (ICF, also slot +0x14): LEA EAX,[ECX+0xc] -- the orientation and position as one
// transform.
Transform const& ZONE::GetTransform() const
{
    return *(Transform const*)&Orientation;
}

// LEA EAX,[ECX+0x30].
Vector const& ZONE::GetPos() const
{
    return Pos;
}

// (ICF with slot +0x0c): LEA EAX,[ECX+0xc].
Matrix const& ZONE::GetOrientation() const
{
    return Orientation;
}

// (ICF): MOV EAX,[ECX+0x3c].
unsigned ZONE::GetShape() const
{
    return Shape;
}

// (ICF): MOV EAX,[ECX+0x40].
unsigned long ZONE::GetProperties() const
{
    return PropertyFlags;
}

// (flags & 0x18000) != 0 through NEG / SBB / NEG.
bool ZONE::Slot20() const
{
    return (PropertyFlags & 0x18000) != 0;
}

// LEA EAX,[ECX+0x78].
Vector const& ZONE::GetFogColor() const
{
    return FogColor;
}

// copies +0x44 through the hidden result pointer (RET 4).
Vector ZONE::GetSize() const
{
    return Size;
}

// (ICF): MOV EAX,[ECX+0x50].
long ZONE::GetMeshHandle() const
{
    return MeshHandle;
}

// .
float ZONE::GetRadius() const
{
    // FLD [this+0x68]; FCOMP 0.0f (module storage); TEST AH,5; JP ->
    // the cached value is returned unless it is below 0.0f (a NaN is returned too).
    if (!(CachedRadius < 0.0f))
        return CachedRadius;

    // switch (Shape - 1), five cases. The sums and square roots stay
    // on the stack and are rounded once when stored to +0x68.
    double const X = Size.x;
    double const Y = Size.y;
    double const Z = Size.z;
    switch (Shape)
    {
    case Universe::ZoneShapeSphere:
        // caseD_1 module storage.
        CachedRadius = Size.x;
        break;

    case Universe::ZoneShapeEllipsoid:
    {
        // caseD_2 module storage-0633916a: FLD x; FLD y; FCOMPP; TEST AH,5; JP -> y unless
        // y < x; then FCOM z; JP -> keep it unless it is < z.
        float Largest = (Size.y < Size.x) ? Size.x : Size.y;
        if (Largest < Size.z)
            Largest = Size.z;
        CachedRadius = Largest;
        break;
    }

    case Universe::ZoneShapeBox:
        // caseD_3 module storage-0633919a: sqrt((x*x + y*y) + z*z) * 0.5 (double module storage).
        CachedRadius = (float)(sqrt((X * X + Y * Y) + Z * Z) * 0.5);
        break;

    case Universe::ZoneShapeCylinder:
        // caseD_4 module storage -> the branch: sqrt(x*x + (y*y) * 0.25) (double module storage).
        CachedRadius = (float)(sqrt(X * X + (Y * Y) * 0.25));
        break;

    case Universe::ZoneShapeRing:
        // caseD_5 module storage -> the branch: the same with z (+0x4c).
        CachedRadius = (float)(sqrt(X * X + (Z * Z) * 0.25));
        break;

    default:
        // FDUMP line 0x35c, then baseline caches FLD [ESP+0x18] -- the
        // PUSH ECX slot, zeroed at module storage (C7 44 24 04 00000000) before the switch.
        ZoneTypeNotSupported(0x35c);
        CachedRadius = 0.0f;
        break;
    }
    // FLD [this+0x68].
    return CachedRadius;
}

// .
bool ZONE::InZone(Vector const& Point, float Margin) const
{
    // z's difference is FST'd to a float and multiplied on the stack
    // by that float; the squared distance vs 1e-4f (module storage) with TEST 5 / JP -> the
    // shape test when >= or unordered, else true.
    float const Dx = Point.x - Pos.x;
    float const Dy = Point.y - Pos.y;
    double const DzExact = (double)(Point.z) - Pos.z;
    float const Dz = (float)(DzExact);
    double const DistSq = (DzExact * Dz + (double)Dx * Dx) + (double)(Dy) * Dy;
    float const DistSqF = (float)(DistSq);
    if (DistSq < 1.0e-4f)
        return true;

    // vt+0x30 + Margin, squared; the stored distance vs it with
    // TEST 5 / JP -> false when >= or unordered.
    double const Reach = (double)(GetRadius()) + Margin;
    if (!(DistSqF < Reach * Reach))
        return false;

    // switch (Shape - 1), table module storage.
    switch (Shape)
    {
    case Universe::ZoneShapeSphere:
        // .
        return true;

    case Universe::ZoneShapeEllipsoid:
    {
        // the unit direction (1.0f / float length on the stack),
        // vt+0x40 on it plus Margin vs the float length, TEST 1 / JNZ -> false when below or
        // unordered.
        float const Length = (float)(sqrt((double)(DistSqF)));
        double const Inv = 1.0 / Length;
        Vector const Dir = MakeVector((float)(Dx * Inv), (float)(Dy * Inv),
                                      (float)(Dz * Inv));
        if (!((GetRadiusAlong(Dir) + Margin) >= Length))
            return false;
        return true;
    }

    case Universe::ZoneShapeBox:
    {
        // the box frame with half extents Size * 0.5f, the point in
        // its local frame (the routine), clamped to the surface (the routine); the squared
        // distance to the clamp, negated when the point was inside, vs Margin^2 with TEST 5 /
        // JP -> false when >= or unordered.
        FZoneFrame Frame = ZoneFrame(*this);
        Frame.Dim[0] = Size.x * 0.5f;
        Frame.Dim[1] = Size.y * 0.5f;
        Frame.Dim[2] = Size.z * 0.5f;
        Vector const Delta = Vec3Sub(Point, Frame.Pos);
        Vector const Local = MathTransposeTransform(Frame.Orientation, Delta);
        Vector Clamped;
        bool const bOutside = ClampToBoxSurface(Frame, Local, Clamped);
        Vector const Diff = MakeVector(Local.x - Clamped.x, Local.y - Clamped.y, Local.z - Clamped.z);
        double Value = ((double)Diff.x * Diff.x + (double)Diff.y * Diff.y) +
                       (double)Diff.z * Diff.z;
        if (!bOutside)
            Value = -Value;
        if (!(Value < (double)Margin * Margin))
            return false;
        return true;
    }

    case Universe::ZoneShapeCylinder:
    {
        // the point in the zone's frame (engine vt+0x38).
        //   Size.y * 0.5 vs |L.y| - Margin: TEST 1 / JNZ -> false when below or unordered.
        //   (L.x^2 + L.z^2) - ((2m + Size.x) * Size.x + m^2) vs 0.0f: TEST 5 / JP -> false
        //   when >= 0 or unordered.
        Vector const Delta = MakeVector(Point.x - Pos.x, Point.y - Pos.y, Point.z - Pos.z);
        Vector const Local = EngineTransposeTransform(Orientation, Delta);
        if (!((double)Size.y * 0.5 >= fabs((double)Local.y) - Margin))
            return false;
        double const RadialSq = (double)Local.x * Local.x + (double)Local.z * Local.z;
        double const Reach = ((double)Margin + Margin + Size.x) * Size.x +
                             (double)Margin * Margin;
        if (!(RadialSq - Reach < 0.0))
            return false;
        return true;
    }

    case Universe::ZoneShapeRing:
    {

        //   Inner = float(sqrt(Size.y^2 + Size.z^2 * 0.25));
        //   Size.z * 0.5 vs |L.y| - Margin: TEST 1 / JNZ -> false when below or unordered;
        //   S - ((2m + Size.x) * Size.x + m^2) vs 0.0f: TEST 0x41 / JP -> false when > 0 or
        //   unordered; S - ((2 Inner + Size.y) * Size.y + Inner^2) vs 0.0f: TEST 1 / JNZ ->
        //   false when < 0 or unordered.
        Vector const Delta = MakeVector(Point.x - Pos.x, Point.y - Pos.y, Point.z - Pos.z);
        Vector const Local = EngineTransposeTransform(Orientation, Delta);
        float const Inner = (float)(sqrt((double)Size.y * Size.y +
                                                    ((double)Size.z * Size.z) * 0.25));
        if (!((double)Size.z * 0.5 >= fabs((double)Local.y) - Margin))
            return false;
        double const RadialSq = (double)Local.x * Local.x + (double)Local.z * Local.z;
        double const OuterReach = ((double)Margin + Margin + Size.x) * Size.x +
                                  (double)Margin * Margin;
        if (!(RadialSq - OuterReach <= 0.0))
            return false;
        double const InnerReach = ((double)Inner + Inner + Size.y) * Size.y +
                                  (double)Inner * Inner;
        if (!(RadialSq - InnerReach >= 0.0))
            return false;
        return true;
    }

    case Universe::ZoneShapeMesh:
        // both outcomes of the mesh-handle test return false.
        return false;

    default:
        // FDUMP line 0x3bc, return false.
        ZoneTypeNotSupported(0x3bc);
        return false;
    }
}

// slot +0x34 in the zone's XZ plane -- the y difference is 0.0f throughout.
bool ZONE::InZoneXZ(Vector const& Point, float Margin) const
{
    // .
    float const Dx = Point.x - Pos.x;
    float const Dy = 0.0f;
    float const Dz = Point.z - Pos.z;
    double const DistSq = (double)Dx * Dx + (double)(Dz) * Dz;
    float const DistSqF = (float)(DistSq);
    if (DistSq < 1.0e-4f)
        return true;

    // .
    double const Reach = (double)(GetRadius()) + Margin;
    if (!(DistSqF < Reach * Reach))
        return false;

    switch (Shape)
    {
    case Universe::ZoneShapeSphere:
        // .
        return true;

    case Universe::ZoneShapeEllipsoid:
    {
        // .
        float const Length = (float)(sqrt((double)(DistSqF)));
        double const Inv = 1.0 / Length;
        Vector const Dir = MakeVector((float)(Dx * Inv), (float)(Dy * Inv),
                                      (float)(Inv * Dz));
        if (!((GetRadiusAlong(Dir) + Margin) >= Length))
            return false;
        return true;
    }

    case Universe::ZoneShapeBox:
    {
        // as slot +0x34, but the frame's position has y = 0
        // (module storage stores 0 at the copied transform's +0x28) and the squared distance
        // sums (dy^2 + dz^2) + dx^2.
        FZoneFrame Frame = ZoneFrame(*this);
        Frame.Pos.y = 0.0f;
        Frame.Dim[0] = Size.x * 0.5f;
        Frame.Dim[1] = Size.y * 0.5f;
        Frame.Dim[2] = Size.z * 0.5f;
        Vector const Delta = Vec3Sub(Point, Frame.Pos);
        Vector const Local = MathTransposeTransform(Frame.Orientation, Delta);
        Vector Clamped;
        bool const bOutside = ClampToBoxSurface(Frame, Local, Clamped);
        Vector const Diff = MakeVector(Local.x - Clamped.x, Local.y - Clamped.y, Local.z - Clamped.z);
        double Value = ((double)Diff.y * Diff.y + (double)Diff.z * Diff.z) +
                       (double)Diff.x * Diff.x;
        if (!bOutside)
            Value = -Value;
        if (!(Value < (double)Margin * Margin))
            return false;
        return true;
    }

    case Universe::ZoneShapeCylinder:
    {
        // the height test is |0.0| - Margin (FLD double 0.0 /
        // FABS / FSUB) -- the height never excludes the point.
        Vector const Delta = MakeVector(Point.x - Pos.x, Point.y - Pos.y, Point.z - Pos.z);
        Vector const Local = EngineTransposeTransform(Orientation, Delta);
        if (!((double)Size.y * 0.5 >= fabs(0.0) - Margin))
            return false;
        double const RadialSq = (double)Local.x * Local.x + (double)Local.z * Local.z;
        double const Reach = ((double)Margin + Margin + Size.x) * Size.x +
                             (double)Margin * Margin;
        if (!(RadialSq - Reach < 0.0))
            return false;
        return true;
    }

    case Universe::ZoneShapeRing:
    {
        // .
        Vector const Delta = MakeVector(Point.x - Pos.x, Point.y - Pos.y, Point.z - Pos.z);
        Vector const Local = EngineTransposeTransform(Orientation, Delta);
        float const Inner = (float)(sqrt((double)Size.y * Size.y +
                                                    ((double)Size.z * Size.z) * 0.25));
        if (!((double)Size.z * 0.5 >= fabs(0.0) - Margin))
            return false;
        double const RadialSq = (double)Local.x * Local.x + (double)Local.z * Local.z;
        double const OuterReach = ((double)Margin + Margin + Size.x) * Size.x +
                                  (double)Margin * Margin;
        if (!(RadialSq - OuterReach <= 0.0))
            return false;
        double const InnerReach = ((double)Inner + Inner + Size.y) * Size.y +
                                  (double)Inner * Inner;
        if (!(RadialSq - InnerReach >= 0.0))
            return false;
        return true;
    }

    case Universe::ZoneShapeMesh:
        // false either way.
        return false;

    default:
        // FDUMP line 0x421, return false.
        ZoneTypeNotSupported(0x421);
        return false;
    }
}

// switch (Shape - 1) over four cases, table module storage; every other shape
// (ring, mesh, invalid) takes the general path.
double ZONE::GetDistance(Vector const& Point) const
{
    switch (Shape)
    {
    case Universe::ZoneShapeSphere:
    {
        // sqrt((dz^2 + dy^2) + dx^2) - Size.x, left on the stack.
        Vector const Delta = Vec3Sub(Point, Pos);
        return sqrt(((double)Delta.z * Delta.z + (double)Delta.y * Delta.y) +
                    (double)Delta.x * Delta.x) -
               Size.x;
    }

    case Universe::ZoneShapeCylinder:
    case Universe::ZoneShapeBox:
    {
        // Cylinder module storage-06339c76 and box module storage-06339d88, joined at
        // . The cylinder is measured as a box of half extents
        // {Size.x, float(Size.y * 0.5), Size.x}; the box uses Size * 0.5f (the routine).
        // baseline runs the clamp twice: the first pass feeds engine vt+0x28 (TransformXform)
        // whose result the second pass's Vec3Sub overwrites unread.
        FZoneFrame Frame = ZoneFrame(*this);
        if (Shape == Universe::ZoneShapeCylinder)
        {
            Frame.Dim[0] = Size.x;
            Frame.Dim[1] = (float)((double)Size.y * 0.5);
            Frame.Dim[2] = Size.x;
        }
        else
        {
            Vector const Half = Vec3Scale(Size, 0.5f);
            Frame.Dim[0] = Half.x;
            Frame.Dim[1] = Half.y;
            Frame.Dim[2] = Half.z;
        }

        Vector const Delta = Vec3Sub(Point, Frame.Pos);
        Vector const Local = MathTransposeTransform(Frame.Orientation, Delta);
        Vector Clamped;
        ClampToBoxSurface(Frame, Local, Clamped);
        EngineTransformXform(Frame, Clamped);

        Vector const Delta2 = Vec3Sub(Point, Frame.Pos);
        Vector const Local2 = MathTransposeTransform(Frame.Orientation, Delta2);
        Vector Clamped2;
        bool const bOutside = ClampToBoxSurface(Frame, Local2, Clamped2);
        Vector const Diff = Vec3Sub(Local2, Clamped2);

        // the routine, negated when inside; FCOM 0.0f / TEST 5 / JP -> sqrt
        // when >= 0 or unordered, else -sqrt(-value).
        double Value = VecLengthSq(Diff);
        if (!bOutside)
            Value = -Value;
        if (Value < 0.0)
            return -sqrt(-Value);
        return sqrt(Value);
    }

    case Universe::ZoneShapeEllipsoid:
    {
        // .
        FZoneEllipsoid Ellipsoid;
        Ellipsoid.Orientation = Orientation;
        Ellipsoid.Pos = Pos;
        EllipsoidSetRadii(Ellipsoid, Size);
        Vector Delta = Vec3Sub(Point, Ellipsoid.Pos);
        double const Length = VecLength(Delta);
        float const LengthF = (float)Length;
        // FCOMP 1e-4 (double module storage) / TEST 5 / JP -> continue when >= or unordered.
        if (Length < 1.0e-4)
            return -EllipsoidMinRadius(Ellipsoid);
        VecScaleInPlace(Delta, (float)(1.0 / LengthF));
        Vector const Local = MathTransposeTransform(Ellipsoid.Orientation, Delta);
        return LengthF - EllipsoidRadiusAlong(Ellipsoid, Local); // FSUBR, left on the stack
    }

    default:
    {
        // the distance to vt+0x44's surface point, negative when
        // vt+0x34(point, 0.0f) says the point is inside.
        Vector const Surface = GetSurfacePoint(Point);
        Vector const Delta = Vec3Sub(Surface, Pos);
        float const Length = (float)(sqrt(((double)(Delta.z) * Delta.z +
                                                      (double)(Delta.y) * Delta.y) +
                                                     (double)(Delta.x) * Delta.x));
        if (InZone(Point, 0.0f))
            return -Length;
        return Length;
    }
    }
}

// switch (Shape - 1), table module storage. The radius along a world direction,
// left on the x87 stack (callers inside ZONE consume it in-register through vt+0x40).
double ZONE::GetRadiusAlong(Vector const& Direction) const
{
    switch (Shape)
    {
    case Universe::ZoneShapeSphere:
        // .
        return Size.x;

    case Universe::ZoneShapeEllipsoid:
    {
        // the ellipsoid from the zone (the routine), the direction
        // in the zone's frame (engine vt+0x38 on +0x0c), the routine.
        FZoneEllipsoid Ellipsoid;
        Ellipsoid.Orientation = Orientation;
        Ellipsoid.Pos = Pos;
        EllipsoidSetRadii(Ellipsoid, Size);
        Vector const Local = EngineTransposeTransform(Orientation, Direction);
        return EllipsoidRadiusAlong(Ellipsoid, Local);
    }

    case Universe::ZoneShapeBox:
    {
        // per axis, |L| > 1e-4f (FCOM / TEST 0x41 / JNZ -> 1e10f
        // when <= or unordered) gives Size / |L| -- x and z stored as floats, y left on the
        // stack. The smallest (A vs B: TEST 5 / JP keeps B when A >= B; then vs C: TEST 5 /
        // JNP keeps it when below C) times 0.5.
        Vector const Local = EngineTransposeTransform(Orientation, Direction);
        double const AbsX = fabs((double)Local.x);
        float const AlongX = (AbsX > 1.0e-4f) ? (float)(Size.x / AbsX) : 1.0e10f;
        double const AbsY = fabs((double)Local.y);
        double const AlongY = (AbsY > 1.0e-4f) ? (Size.y / AbsY) : (double)(1.0e10f);
        double const AbsZ = fabs((double)Local.z);
        float const AlongZ = (AbsZ > 1.0e-4f) ? (float)(Size.z / AbsZ) : 1.0e10f;
        double Smallest = (AlongX < AlongY) ? (double)(AlongX) : AlongY;
        if (!(Smallest < AlongZ))
            Smallest = AlongZ;
        return Smallest * 0.5;
    }

    case Universe::ZoneShapeCylinder:
    case Universe::ZoneShapeRing:
    {
        // Cylinder module storage-0633a1c0 (half height Size.y * 0.5), ring
        // (Size.z * 0.5), joined at the branch:
        //   Cutoff = HalfH / sqrt(Size.x^2 + HalfH^2); FCOMPP |L.y| vs Cutoff / TEST 0x41 /
        //   JNZ -> Size.x / sqrt(L.x^2 + L.z^2) when |L.y| <= Cutoff or unordered, else
        //   HalfH / |L.y|.
        Vector const Local = EngineTransposeTransform(Orientation, Direction);
        double const HalfH = (double)((Shape == Universe::ZoneShapeCylinder) ? Size.y : Size.z) * 0.5;
        double const AbsY = fabs((double)Local.y);
        double const Cutoff = HalfH / sqrt((double)Size.x * Size.x + HalfH * HalfH);
        if (AbsY > Cutoff)
            return HalfH / AbsY;
        return Size.x / sqrt((double)Local.x * Local.x + (double)Local.z * Local.z);
    }

    default:
        // FDUMP line 0x4be, then vt+0x30.
        ZoneTypeNotSupported(0x4be);
        return GetRadius();
    }
}

// (hidden result pointer, RET 8).
Vector ZONE::GetSurfacePoint(Vector const& Point) const
{
    if (Shape == Universe::ZoneShapeBox)
    {
        // the clamped local point back in the world (engine vt+0x28).
        FZoneFrame Frame = ZoneFrame(*this);
        Frame.Dim[0] = Size.x * 0.5f;
        Frame.Dim[1] = Size.y * 0.5f;
        Frame.Dim[2] = Size.z * 0.5f;
        Vector const Delta = Vec3Sub(Point, Frame.Pos);
        Vector const Local = MathTransposeTransform(Frame.Orientation, Delta);
        Vector Clamped;
        ClampToBoxSurface(Frame, Local, Clamped);
        return EngineTransformXform(Frame, Clamped);
    }

    if (Shape == Universe::ZoneShapeRing)
    {
        // the nearest point on the ring's middle circle (radius
        // float((Size.y + Size.x) * 0.5)), plus the tube direction scaled to the surface.
        Vector const Delta = Vec3Sub(Point, Pos);
        Vector const Local = EngineTransposeTransform(Orientation, Delta);
        Vector Rim = MakeVector(Local.x, 0.0f, Local.z);
        float const MidRadius = (float)(((double)Size.y + Size.x) * 0.5);
        double const Scale = MidRadius / VecLength(Rim);
        Rim = MakeVector((float)(Rim.x * Scale), (float)(0.0f * Scale),
                         (float)(Rim.z * Scale));
        Vector Tube = Vec3Sub(Local, Rim);
        double const TubeLength = VecLength(Tube);
        // FCOM 1e-4f / TEST 5 / JNP -> no normalisation when below.
        if (!(TubeLength < 1.0e-4f))
        {
            double const Inv = 1.0 / TubeLength;
            Tube = MakeVector((float)(Tube.x * Inv), (float)(Tube.y * Inv),
                              (float)(Inv * Tube.z));
        }
        double const Surface = RingSurfaceScale(Size, Tube);
        Vector const Offset = MakeVector((float)(Tube.x * Surface), (float)(Tube.y * Surface),
                                         (float)(Tube.z * Surface));
        Vector const LocalSurface = Vec3Add(Rim, Offset);
        Vector const World = EngineTransformMatrix(Orientation, LocalSurface);
        return Vec3Add(Pos, World);
    }

    // every other shape -- the centre when the point is within
    // 1e-4f of it (FCOM / TEST 5 / JNP), else centre + unit direction * float(vt+0x40).
    Vector Delta = MakeVector(Point.x - Pos.x, Point.y - Pos.y, Point.z - Pos.z);
    double const Length = VecLength(Delta);
    if (Length < 1.0e-4f)
        return Pos;
    double const Inv = 1.0 / Length;
    Delta = MakeVector((float)(Delta.x * Inv), (float)(Delta.y * Inv),
                       (float)(Delta.z * Inv));
    float const Radius = (float)GetRadiusAlong(Delta);
    Vector const Offset = Vec3Scale(Delta, Radius);
    return MakeVector(Offset.x + Pos.x, Offset.y + Pos.y, Offset.z + Pos.z);
}

// the box goes to the routine; the ring and every other shape end in
// Radius vs 1e-4f (FCOM / TEST 0x41 / JNZ -> the branch when <= or
// unordered), else float length / Radius. the branch loads [ESP+0x4] after its POP EBP,
// i.e. the local zeroed at module storage, not the float length: that tail returns 0.0.
double ZONE::ClassifyPoint(Vector const& Point) const
{
    if (Shape == Universe::ZoneShapeBox)
        return ClassifyBox(Point);

    float LengthF = 0.0f;
    double Radius = 0.0;
    if (Shape == Universe::ZoneShapeRing)
    {
        // the offset from the ring's middle circle (radius (Size.y + Size.x) *
        // 0.5 on the stack), its float length, normalised when not below 1e-4f, and the
        // ring's surface scale along it.
        Vector const Delta = Vec3Sub(Point, Pos);
        Vector const Local = EngineTransposeTransform(Orientation, Delta);
        double const MidRadius = ((double)Size.y + Size.x) * 0.5;
        double const Scale = MidRadius / sqrt((double)Local.z * Local.z +
                                              (double)Local.x * Local.x);
        float const RimX = (float)(Scale * Local.x);
        float const RimY = (float)(0.0f * Scale);
        double const RimZ = Local.z * Scale;
        Vector Tube = MakeVector(Local.x - RimX, Local.y - RimY,
                                 (float)((double)Local.z - RimZ));
        // sum (y^2 + z^2) + x^2 twice (the second as (z^2 + y^2) +
        // x^2, the same value): once stored as the float length, once kept for the test.
        double const TubeLength = sqrt(((double)(Tube.y) * Tube.y + (double)(Tube.z) * Tube.z) +
                                       (double)(Tube.x) * Tube.x);
        LengthF = (float)(TubeLength);
        if (!(TubeLength < 1.0e-4f))
            VecScaleInPlace(Tube, (float)(1.0 / TubeLength));
        Radius = RingSurfaceScale(Size, Tube);
    }
    else
    {
        // sqrt((dy^2 + dz^2) + dx^2) vs 1e-4f with TEST 5 / JP ->
        // 0.0f when below; else the unit direction (1.0 / float length) and vt+0x40.
        Vector Delta = Vec3Sub(Point, Pos);
        double const Length = sqrt(((double)(Delta.y) * Delta.y + (double)(Delta.z) * Delta.z) +
                                   (double)(Delta.x) * Delta.x);
        LengthF = (float)Length;
        if (Length < 1.0e-4f)
            return 0.0;
        double const Inv = 1.0 / LengthF;
        Delta = MakeVector((float)(Delta.x * Inv), (float)(Delta.y * Inv),
                           (float)(Delta.z * Inv));
        Radius = GetRadiusAlong(Delta);
    }

    if (!(Radius > 1.0e-4f))
        return 0.0;
    return LengthF / Radius;
}

// (a tail of slot +0x48 for the box): the squared distance to the surface
// (negative inside) as a float, then (v + r^2) / r^2 with r = vt+0x30.
double ZONE::ClassifyBox(Vector const& Point) const
{
    FZoneFrame Frame = ZoneFrame(*this);
    Frame.Dim[0] = Size.x * 0.5f;
    Frame.Dim[1] = Size.y * 0.5f;
    Frame.Dim[2] = Size.z * 0.5f;
    Vector const Delta = Vec3Sub(Point, Frame.Pos);
    Vector const Local = MathTransposeTransform(Frame.Orientation, Delta);
    Vector Clamped;
    bool const bOutside = ClampToBoxSurface(Frame, Local, Clamped);
    Vector const Diff = Vec3Sub(Local, Clamped);
    double Value = ((double)Diff.y * Diff.y + (double)Diff.z * Diff.z) +
                   (double)Diff.x * Diff.x;
    if (!bOutside)
        Value = -Value;
    float const ValueF = (float)(Value);
    double const Radius = GetRadius();
    double const RadiusSq = Radius * Radius;
    return (ValueF + RadiusSq) / RadiusSq;
}

// (RET 0xc): switch (Shape - 1), table module storage.
bool ZONE::Intersect(Vector const& Start, Vector const& End, Vector& OutHit) const
{
    FZoneSegment Segment;
    Segment.Start = Start;
    Segment.End = End;

    FZoneFrame Frame = ZoneFrame(*this);
    switch (Shape)
    {
    case Universe::ZoneShapeEllipsoid:
        // .
        Frame.Dim[0] = Size.x;
        Frame.Dim[1] = Size.y;
        Frame.Dim[2] = Size.z;
        return ClipSegmentEllipsoid(Frame, Segment, OutHit);

    case Universe::ZoneShapeBox:
        // .
        Frame.Dim[0] = Size.x;
        Frame.Dim[1] = Size.y;
        Frame.Dim[2] = Size.z;
        return ClipSegmentBox(Frame, Segment, OutHit);

    case Universe::ZoneShapeCylinder:
        // only the radius and the height are stored (the third
        // dimension is left as it was; the cylinder clip does not read it).
        Frame.Dim[0] = Size.x;
        Frame.Dim[1] = Size.y;
        return ClipSegmentCylinder(Frame, Segment, OutHit);

    case Universe::ZoneShapeRing:
        // .
        Frame.Dim[0] = Size.x;
        Frame.Dim[1] = Size.y;
        Frame.Dim[2] = Size.z;
        return ClipSegmentRing(Frame, Segment, OutHit);

    case Universe::ZoneShapeSphere:
        break;

    default:
        // FDUMP line 0x55a, then the sphere case.
        ZoneTypeNotSupported(0x55a);
        break;
    }

    // the bounding sphere {Pos, vt+0x30}.
    FZoneSphere Sphere;
    Sphere.Radius = GetRadius();
    Sphere.Center = Pos;
    return ClipSegmentSphere(Sphere, Segment, OutHit);
}

// FLD [ECX+0x54].
float ZONE::GetEdgeFraction() const
{
    return EdgeFraction;
}

// +0x58 is computed once -- 1 when drag < 1.0f (FCOMP / TEST 5 / JNP),
// interference > 0.0f or damage > 0.0f (FCOMP / TEST 0x41 / JZ), else 0.
bool ZONE::HasPhysicalEffect() const
{
    if (PhysicalEffect < 0)
    {
        if (DragModifier < 1.0f || Interference > 0.0f || Damage > 0.0f)
            PhysicalEffect = 1;
        else
        {
            PhysicalEffect = 0;
            return false;
        }
    }
    return PhysicalEffect != 0;
}

// (ICF with Archetype::Asteroid::get_detect_radius): FLD [ECX+0x5c].
float ZONE::GetDragModifier() const
{
    return DragModifier;
}

// (ICF with Archetype::Asteroid::get_offset_dist): FLD [ECX+0x60].
float ZONE::GetDamage() const
{
    return Damage;
}

// (ICF with Archetype::Asteroid::get_recharge_time): FLD [ECX+0x64].
float ZONE::GetInterference() const
{
    return Interference;
}

// 0 when the population buffer is null, else (last - first) >> 3.
int ZONE::GetPopulationCount() const
{
    if (Population.Items.data() == nullptr)
        return 0;
    return (int)Population.Items.size();
}

// (RET 0xc): Index < 0 (JL), a null buffer, or Index >= size (unsigned, JNC)
// -> false; else the pair's id and weight.
bool ZONE::GetPopulationEntry(int Index, unsigned& OutId, float& OutWeight) const
{
    if (Index < 0)
        return false;
    if (Population.Items.data() == nullptr)
        return false;
    if ((unsigned)Index >= Population.Items.size())
        return false;
    OutId = Population.Items[Index].first;
    OutWeight = Population.Items[Index].second;
    return true;
}

// LEA EAX,[ECX+0x8c].
weighted_vector<unsigned int> const& ZONE::GetPopulation() const
{
    return Population;
}

// MOV AL,[ECX+0x88].
bool ZONE::HasPathLabel() const
{
    return bPathLabel;
}

// *[ESP+4] = [ECX+0x6c], RET 4.
ID_String ZONE::GetMusic() const
{
    ID_String Result;
    Result.Id = MusicId;
    return Result;
}

// (RET 8): *arg0 = [+0x70], *arg1 = [+0x74], returns (flags >> 0x15) & 1.
bool ZONE::GetSpacedust(char const*& OutSpacedust, int& OutMaxParticles) const
{
    OutSpacedust = Spacedust;
    OutMaxParticles = SpacedustMaxParticles;
    return ((PropertyFlags >> 0x15) & 1) != 0;
}

// (ICF with File::tell): MOV EAX,[ECX+0x84].
CmnAsteroid::LootableZoneEntry* ZONE::GetLootableInfo() const
{
    return OwnedLootEntry;
}

// MOV AL,[ECX+0x9c].
unsigned char ZONE::GetVisit() const
{
    return Visit;
}

// MOV EAX,[ECX+0xa0].
int ZONE::GetName() const
{
    return IdsName;
}

// MOV EAX,[ECX+0xa4].
int ZONE::GetIdsInfo() const
{
    return IdsInfo;
}

// (RET 4): free the owned record (the routine) when there is one, then store
// the new one.
void ZONE::SetLootableInfo(CmnAsteroid::LootableZoneEntry* LootEntry)
{
    if (OwnedLootEntry != nullptr)
    {
        free(OwnedLootEntry);
        OwnedLootEntry = nullptr;
    }
    OwnedLootEntry = LootEntry;
}

// (RET 4): [+0x9c] = arg | 0x20.
void ZONE::SetVisit(unsigned char NewVisit)
{
    Visit = (unsigned char)(NewVisit | 0x20);
}

// ?FrustumCull@@YA_NABUFrustum@Geometry@@PBUIZone@Universe@@@Z, module storage-0633cb49.
// true when the zone lies outside the frustum. No zone -> false.
// Not an ellipsoid (vt+0x18 != 2): the sphere {vt+0x10 position, vt+0x30 radius} goes to
// . An ellipsoid is tested against the six sides (the routine + the routine):
// side 2 on any of them culls; then, unless side 1 (the far side) straddles, it stays. When
// exactly one of the left / right sides (2 / 3) straddles, planes turned step by step around
// the far edge on that side are tried (a side-1 result culls); the same for the top / bottom
// sides (4 / 5) afterwards. Center = apex + (-column 2) * Distance moved by
// tan(Angle * 0.5) * Distance along column 0 / 1 (FPTAN, FMUL double 0.5).
bool __cdecl FrustumCull(Geometry::Frustum const& Frustum, Universe::IZone const* Zone)
{
    if (Zone == nullptr)
        return false;

    if (Zone->GetShape() != Universe::ZoneShapeEllipsoid)
    {
        // .
        FZoneSphere Sphere;
        float const Radius = Zone->GetRadius();
        Sphere.Center = Zone->GetPos();
        Sphere.Radius = Radius;
        return FrustumCullsSphere(Frustum, Sphere);
    }

    FZoneEllipsoid const Ellipsoid = CullEllipsoid(*Zone);

    // the six sides, results kept at [ESP+0x40 + i*4].
    int Sides[6];
    for (unsigned Index = 0; Index < 6; ++Index)
    {
        FCullPlane const Plane = FrustumPlane(Frustum, Index);
        Sides[Index] = EllipsoidPlaneSide(Ellipsoid, Plane);
        if (Sides[Index] == 2)
            return true;
    }
    if (Sides[1] != 0)
        return false;

    // bit 0..3 = side 2..5 straddles (SETZ).
    unsigned const Straddle = (Sides[2] == 0 ? 1u : 0u) | (Sides[3] == 0 ? 2u : 0u) |
                              (Sides[4] == 0 ? 4u : 0u) | (Sides[5] == 0 ? 8u : 0u);
    float const Distance = Frustum.Distance;
    Vector const Back = VecNegate(FrustumColumn(Frustum, 2));

    unsigned const Horizontal = Straddle & 3u;
    if (Horizontal == 1u || Horizontal == 2u)
    {
        float const Spread = (float)(X87Tan((double)Frustum.AngleX * 0.5) * Distance);
        Vector const Right = FrustumColumn(Frustum, 0);
        Vector Center = Vec3Add(Frustum.Xform.Translation, Vec3Scale(Back, Distance));
        float Sweep;
        if ((Straddle & 1u) != 0)
        {
            Vector const Shift = Vec3Scale(Right, -Spread);
            Center = MakeVector(Shift.x + Center.x, Shift.y + Center.y, Shift.z + Center.z);
            Sweep = (float)(((double)Frustum.AngleX + kCullPi) * -0.5);
        }
        else
        {
            Vector const Shift = Vec3Scale(Right, Spread);
            Center = MakeVector(Shift.x + Center.x, Shift.y + Center.y, Shift.z + Center.z);
            Sweep = (float)(((double)Frustum.AngleX + kCullPi) * 0.5);
        }
        for (int Step = 1; Step < GFrustumCapSteps; ++Step)
        {
            if (CapStepCulls(Ellipsoid, Center, Sweep, Step, true))
                return true;
        }
    }

    unsigned const Vertical = Straddle & 0xcu;
    if (Vertical == 4u || Vertical == 8u)
    {
        float const Spread = (float)(X87Tan((double)Frustum.AngleY * 0.5) * Distance);
        Vector const Up = FrustumColumn(Frustum, 1);
        Vector Center = Vec3Add(Frustum.Xform.Translation, Vec3Scale(Back, Distance));
        float Sweep;
        if ((Straddle & 4u) != 0)
        {
            Vector const Shift = Vec3Scale(Up, Spread);
            Center = MakeVector(Shift.x + Center.x, Shift.y + Center.y, Shift.z + Center.z);
            Sweep = (float)(((double)Frustum.AngleY + kCullPi) * 0.5);
        }
        else
        {
            Vector const Shift = Vec3Scale(Up, -Spread);
            Center = MakeVector(Shift.x + Center.x, Shift.y + Center.y, Shift.z + Center.z);
            Sweep = (float)(((double)Frustum.AngleY + kCullPi) * -0.5);
        }
        for (int Step = 1; Step < GFrustumCapSteps; ++Step)
        {
            if (CapStepCulls(Ellipsoid, Center, Sweep, Step, false))
                return true;
        }
    }
    return false;
}

// ?SphereCull@@YA_NABUSphere@Geometry@@PBUIZone@Universe@@@Z, module storage-0633da96.
// true when the sphere does not reach the zone. No zone -> false. The shape (vt+0x18) is
// asked again before each test, as baseline does.
bool __cdecl SphereCull(Geometry::Sphere const& Sphere, Universe::IZone const* Zone)
{
    if (Zone == nullptr)
        return false;

    double const Radius = Sphere.Radius;
    if (Zone->GetShape() == Universe::ZoneShapeEllipsoid)
    {
        // ; FCOMP R / TEST 5 / JNP -> false when the distance is below R.
        FZoneEllipsoid const Ellipsoid = CullEllipsoid(*Zone);
        return !(EllipsoidSignedDistance(Ellipsoid, Sphere.Center) < Radius);
    }

    if (Zone->GetShape() == Universe::ZoneShapeBox)
    {
        // Half extents Size * 0.5f (the routine), the frame from vt+0x0c, the routine;
        // FCOMPP value vs R*R / TEST 5 / JNP -> false when below.
        Vector const Size = Zone->GetSize();
        Vector const Half = MakeVector(Size.x * kCullHalf, Size.y * kCullHalf, Size.z * kCullHalf);
        Transform const& Xform = Zone->GetTransform();
        FZoneFrame Frame;
        Frame.Orientation = Xform.Rotation;
        Frame.Pos = Xform.Translation;
        Frame.Dim[0] = Half.x;
        Frame.Dim[1] = Half.y;
        Frame.Dim[2] = Half.z;
        return !(BoxSignedDistanceSq(Frame, Sphere.Center) < Radius * Radius);
    }

    if (Zone->GetShape() == Universe::ZoneShapeCylinder)
    {
        // Three vt+0x28 calls: Dim = {Size.x, float(Size.y * 0.5 double), Size.x}. Local =
        // transpose(Orientation) * (Centre - Pos), the routine clamps it to the
        // box, the routine of the difference, negated when the clamp says inside; FCOMPP vs
        // R*R / TEST 5 / JNP -> false when below.
        float const DimX = Zone->GetSize().x;
        float const DimZ = Zone->GetSize().x;
        float const DimY = (float)((double)(Zone->GetSize().y) * 0.5);
        Transform const& Xform = Zone->GetTransform();
        FZoneFrame Frame;
        Frame.Orientation = Xform.Rotation;
        Frame.Pos = Xform.Translation;
        Frame.Dim[0] = DimX;
        Frame.Dim[1] = DimY;
        Frame.Dim[2] = DimZ;
        Vector const Delta = Vec3Sub(Sphere.Center, Frame.Pos);
        Vector const Local = MathTransposeTransform(Frame.Orientation, Delta);
        Vector Clamped;
        bool const bOutside = ClampToBoxSurface(Frame, Local, Clamped);
        Vector const Diff = Vec3Sub(Local, Clamped);
        double Value = VecLengthSq(Diff);
        if (!bOutside)
            Value = -Value;
        return !(Value < Radius * Radius);
    }

    // the zone's bounding sphere (vt+0x30 radius stored as float, vt+0x10
    // position). Sum = (dx^2 + dz^2) + dy^2 on the stack; FCOMPP (r + R)^2 vs Sum /
    // TEST 0x41 / JZ -> false when (r + R)^2 > Sum.
    float const ZoneRadius = Zone->GetRadius();
    Vector const ZonePos = Zone->GetPos();
    double const DX = (double)(Sphere.Center.x) - ZonePos.x;
    double const DY = (double)(Sphere.Center.y) - ZonePos.y;
    double const DZ = (double)(Sphere.Center.z) - ZonePos.z;
    double const Reach = (double)(ZoneRadius) + Sphere.Radius;
    double const Sum = (DX * DX + DZ * DZ) + DY * DY;
    return !(Reach * Reach > Sum);
}
