// Author: Giperion.

#pragma once

// Public ABI of x86math.dll -- the NEURONCOMPONENTS 3DMathEngine component. Everything that does
// vector/matrix/quaternion work through the engine calls these methods; the interface is
// declared here and nowhere else (implementation notes).

#include "neuroncomponents.h"

// I3DMathEngine public vtable.
// Types are 3x3 row-major Matrix, Transform = Matrix + translation, Quaternion = (W,X,Y,Z).

// GR_INVALID_PARMS / GR_INTERFACE_UNSUPPORTED are NEURONCOMPONENTS-wide result codes, not part of this
// interface; thirteen modules still keep their own copy. TODO: one home in neuroncomponents.h.

struct FVector
{
    float X;
    float Y;
    float Z;
};

struct FMatrix
{
    float D[3][3];
};

struct FTransform
{
    FMatrix Rotation;
    FVector Translation;
};

struct FQuaternion
{
    float W;
    float X;
    float Y;
    float Z;
};

struct FMathDesc
{
    unsigned long Size;
    const char* InterfaceName;
    const char* Implementation;
};

static_assert(sizeof(FMathDesc) == 0xC, "baseline AGGDESC / DA3DMATHDESC size 0xC");

// baseline vtable slots 0–25. Overload grouping does not match reconstructed Helion 3dMath.h.
struct I3DMathEngine : INeuronComponent
{
    virtual GENRESULT NEURONCOMPONENTS_API Inverse(FMatrix& Dst, const FMatrix& Source) = 0;                 // +0x0C
    virtual GENRESULT NEURONCOMPONENTS_API Scale(FMatrix& Dst, const FMatrix& Source, float Scale) = 0;     // +0x10
    virtual float NEURONCOMPONENTS_API Det(const FMatrix& Source) = 0;                                      // +0x14
    virtual GENRESULT NEURONCOMPONENTS_API MulQuat(FQuaternion& Dst, const FQuaternion& A,
                                        const FQuaternion& B) = 0;                               // +0x18
    virtual GENRESULT NEURONCOMPONENTS_API MulTransform(FTransform& Dst, const FTransform& A,
                                             const FTransform& B) = 0;                           // +0x1C
    virtual GENRESULT NEURONCOMPONENTS_API MulMatrix(FMatrix& Dst, const FMatrix& A, const FMatrix& B) = 0; // +0x20
    virtual GENRESULT NEURONCOMPONENTS_API TransformQuat(FVector& Dst, const FQuaternion& Quat,
                                              const FVector& Source) = 0;                        // +0x24
    virtual GENRESULT NEURONCOMPONENTS_API TransformXform(FVector& Dst, const FTransform& Xform,
                                               const FVector& Source) = 0;                       // +0x28
    virtual GENRESULT NEURONCOMPONENTS_API TransformMatrix(FVector& Dst, const FMatrix& Matrix,
                                                const FVector& Source) = 0;                      // +0x2C
    virtual GENRESULT NEURONCOMPONENTS_API Rotate(FVector& Dst, const FTransform& Xform,
                                       const FVector& Source) = 0;                               // +0x30
    virtual GENRESULT NEURONCOMPONENTS_API InverseRotate(FVector& Dst, const FTransform& Xform,
                                              const FVector& Source) = 0;                        // +0x34
    virtual GENRESULT NEURONCOMPONENTS_API TransposeTransform(FVector& Dst, const FMatrix& Matrix,
                                                   const FVector& Source) = 0;                   // +0x38
    virtual GENRESULT NEURONCOMPONENTS_API InverseTransform(FVector& Dst, const FTransform& Xform,
                                                 const FVector& Source) = 0;                     // +0x3C
    virtual GENRESULT NEURONCOMPONENTS_API TransformListXform(FVector* Dst, const FTransform& Xform,
                                                   const FVector* Source, int Count) = 0;        // +0x40
    virtual GENRESULT NEURONCOMPONENTS_API TransformListMatrix(FVector* Dst, const FMatrix& Matrix,
                                                    const FVector* Source, int Count) = 0;       // +0x44
    virtual GENRESULT NEURONCOMPONENTS_API TransposeTransformList(FVector* Dst, const FMatrix& Matrix,
                                                       const FVector* Source, int Count) = 0;    // +0x48
    virtual GENRESULT NEURONCOMPONENTS_API InverseTransformList(FVector* Dst, const FTransform& Xform,
                                                     const FVector* Source, int Count) = 0;      // +0x4C
    virtual GENRESULT NEURONCOMPONENTS_API MatrixToQuaternion(FQuaternion& Dst, const FMatrix& Source) = 0; // +0x50
    virtual GENRESULT NEURONCOMPONENTS_API QuaternionToMatrix(FMatrix& Dst, const FQuaternion& Quat) = 0;   // +0x54
    virtual GENRESULT NEURONCOMPONENTS_API QuatSlerp(FQuaternion& Dst, const FQuaternion& A,
                                          const FQuaternion& B, float T) = 0;                    // +0x58
    virtual GENRESULT NEURONCOMPONENTS_API GeneralInverse(FTransform& Dst, float& W,
                                               const FTransform& Source) = 0;                    // +0x5C
    virtual GENRESULT NEURONCOMPONENTS_API InvSqrt(float& Dst, float X) = 0;                                // +0x60
    virtual GENRESULT NEURONCOMPONENTS_API Sqrt(float& Dst, float X) = 0;                                   // +0x64

    // Modern port-only raw-F32 transport; original baseline slots 0..25 are unchanged.
    // The input word is consumed at the kernel FMUL, after the quaternion dot test.
    virtual GENRESULT NEURONCOMPONENTS_API QuatSlerpValue(FQuaternion& Dst, const FQuaternion& A,
                                              const FQuaternion& B, const float* T) = 0;          // +0x68
};
