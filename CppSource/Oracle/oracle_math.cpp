// Oracle suites for work package P1.4: neuron-core::math against the
// 3DMathEngine component of x86math.dll.
//
// The oracle does not link x86math.dll (it has no exports). Loading it runs
// its DllMain, which builds the InvSqrt table and registers the factory with
// neuroncomponents; the engine is then created through ICOManager as
// dx8renderer does. Everything runs in main()'s 53-bit precision.
//
// Inputs are crates/neuron-core/tests/fixtures/oracle/math/*.hex (lines of
// f32 bit patterns); the Rust test crates/neuron-core/tests/oracle_math.rs
// renders the same lines. Bits are copied with memcpy, never through the
// FPU, so the harness itself cannot change a word.

#include "oracle_support.h"

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "neuroncomponents.h"
#include "x86math.h"

using namespace oracle;

namespace
{
    typedef std::vector<unsigned int> Words;

    I3DMathEngine* AcquireMath()
    {
        if (LoadLibraryA("x86math.dll") == nullptr)
        {
            fprintf(stderr, "cannot load x86math.dll (%lu)\n", GetLastError());
            GFailed = true;
            return nullptr;
        }
        FMathDesc Desc;
        Desc.Size = sizeof(FMathDesc);
        Desc.InterfaceName = "3DMathEngine";
        Desc.Implementation = nullptr;
        void* Instance = nullptr;
        if (NEURONCOMPONENTS_Acquire()->CreateInstance((AGGDESC*)(&Desc), &Instance) != GR_OK ||
            Instance == nullptr)
        {
            fprintf(stderr, "no 3DMathEngine\n");
            GFailed = true;
            return nullptr;
        }
        return (I3DMathEngine*)(Instance);
    }

    Words ParseWords(const std::string& Line)
    {
        Words Out;
        const char* Cursor = Line.c_str();
        while (*Cursor != '\0')
        {
            char* End = nullptr;
            Out.push_back((unsigned int)strtoul(Cursor, &End, 16));
            if (End == Cursor)
                break;
            Cursor = End;
            while (*Cursor == ' ')
                ++Cursor;
        }
        return Out;
    }

    bool Expect(const std::string& Line, const Words& W, size_t Count)
    {
        if (W.size() == Count)
            return true;
        fprintf(stderr, "bad math input: %s\n", Line.c_str());
        GFailed = true;
        return false;
    }

    // Count words from W[At] into Out, bit for bit.
    template <typename T>
    void Load(T& Out, const Words& W, size_t At)
    {
        memcpy(&Out, &W[At], sizeof(T));
    }

    // The f32 words of a value, each as eight lower-case hex digits. Every
    // NaN is written as 7fc00000: the suites are Tier B, any NaN matches any
    // NaN, and x87 NaN sign propagation is not part of the port.
    std::string HexWords(const void* Data, size_t Bytes)
    {
        std::string Out;
        for (size_t Offset = 0; Offset < Bytes; Offset += 4)
        {
            unsigned int Bits;
            memcpy(&Bits, (const unsigned char*)(Data) + Offset, sizeof(Bits));
            if ((Bits & 0x7F800000u) == 0x7F800000u && (Bits & 0x007FFFFFu) != 0)
                Bits = 0x7FC00000u;
            Out += Format("%08x", Bits);
        }
        return Out;
    }

    template <typename T>
    std::string HexOf(const T& Value)
    {
        return HexWords(&Value, sizeof(T));
    }

    std::string HexList(const std::vector<FVector>& List)
    {
        return List.empty() ? std::string() : HexWords(&List[0], List.size() * sizeof(FVector));
    }

    const char* Flag(GENRESULT Result)
    {
        return Result == GR_OK ? "1" : "0";
    }

    class Record
    {
    public:
        explicit Record(const std::string& Input) : Text("{\"in\":\"" + Input + "\"") {}

        Record& Field(const char* Key, const std::string& Value)
        {
            Text += ",\"";
            Text += Key;
            Text += "\":\"";
            Text += Value;
            Text += "\"";
            return *this;
        }

        std::string Done() const { return Text + "}"; }

    private:
        std::string Text;
    };

    void MatrixSuite(I3DMathEngine* Math)
    {
        Suite Out("math_matrix.jsonl");
        for (const std::string& Line : InputLines("math/matrix.hex"))
        {
            const Words W = ParseWords(Line);
            if (!Expect(Line, W, 19))
                continue;
            FMatrix A;
            FMatrix B;
            float S;
            Load(A, W, 0);
            Load(B, W, 9);
            Load(S, W, 18);

            const float Det = Math->Det(A);
            FMatrix Inv;
            const GENRESULT InvResult = Math->Inverse(Inv, A);
            FMatrix InvAlias = A;
            const GENRESULT InvAliasResult = Math->Inverse(InvAlias, InvAlias);
            FMatrix Scaled;
            Math->Scale(Scaled, A, S);
            FMatrix Mul;
            Math->MulMatrix(Mul, A, B);
            FMatrix MulA = A;
            Math->MulMatrix(MulA, MulA, B);
            FMatrix MulB = B;
            Math->MulMatrix(MulB, A, MulB);
            FQuaternion Quat;
            Math->MatrixToQuaternion(Quat, A);

            Out.Line(Record(Line)
                         .Field("det", HexOf(Det))
                         .Field("inv", HexOf(Inv))
                         .Field("inv_ok", Flag(InvResult))
                         .Field("inv_alias", HexOf(InvAlias))
                         .Field("inv_alias_ok", Flag(InvAliasResult))
                         .Field("scale", HexOf(Scaled))
                         .Field("mul", HexOf(Mul))
                         .Field("mul_a", HexOf(MulA))
                         .Field("mul_b", HexOf(MulB))
                         .Field("quat", HexOf(Quat))
                         .Done());
        }
    }

    void TransformSuite(I3DMathEngine* Math)
    {
        Suite Out("math_transform.jsonl");
        for (const std::string& Line : InputLines("math/transform.hex"))
        {
            const Words W = ParseWords(Line);
            if (!Expect(Line, W, 24))
                continue;
            FTransform A;
            FTransform B;
            Load(A, W, 0);
            Load(B, W, 12);

            FTransform Mul;
            Math->MulTransform(Mul, A, B);
            FTransform MulA = A;
            Math->MulTransform(MulA, MulA, B);
            FTransform MulB = B;
            Math->MulTransform(MulB, A, MulB);
            FTransform Inv;
            float InvW = 0.0f;
            const GENRESULT InvResult = Math->GeneralInverse(Inv, InvW, A);
            FTransform Alias = A;
            float AliasW = 0.0f;
            const GENRESULT AliasResult = Math->GeneralInverse(Alias, AliasW, Alias);

            Out.Line(Record(Line)
                         .Field("mul", HexOf(Mul))
                         .Field("mul_a", HexOf(MulA))
                         .Field("mul_b", HexOf(MulB))
                         .Field("ginv", HexOf(Inv))
                         .Field("ginv_w", HexOf(InvW))
                         .Field("ginv_ok", Flag(InvResult))
                         .Field("ginv_alias", HexOf(Alias))
                         .Field("ginv_alias_w", HexOf(AliasW))
                         .Field("ginv_alias_ok", Flag(AliasResult))
                         .Done());
        }
    }

    void VectorSuite(I3DMathEngine* Math)
    {
        Suite Out("math_vector.jsonl");
        for (const std::string& Line : InputLines("math/vector.hex"))
        {
            const Words W = ParseWords(Line);
            if (!Expect(Line, W, 15))
                continue;
            FTransform X;
            FVector V;
            Load(X, W, 0);
            Load(V, W, 12);

            FVector Xform, ByMatrix, Rotated, InvRotated, Transposed, InvXform;
            FVector XformAlias = V, MatrixAlias = V, RotatedAlias = V, InvRotatedAlias = V,
                    TransposedAlias = V, InvXformAlias = V;
            Math->TransformXform(Xform, X, V);
            Math->TransformXform(XformAlias, X, XformAlias);
            Math->TransformMatrix(ByMatrix, X.Rotation, V);
            Math->TransformMatrix(MatrixAlias, X.Rotation, MatrixAlias);
            Math->Rotate(Rotated, X, V);
            Math->Rotate(RotatedAlias, X, RotatedAlias);
            Math->InverseRotate(InvRotated, X, V);
            Math->InverseRotate(InvRotatedAlias, X, InvRotatedAlias);
            Math->TransposeTransform(Transposed, X.Rotation, V);
            Math->TransposeTransform(TransposedAlias, X.Rotation, TransposedAlias);
            Math->InverseTransform(InvXform, X, V);
            Math->InverseTransform(InvXformAlias, X, InvXformAlias);

            Out.Line(Record(Line)
                         .Field("xform", HexOf(Xform))
                         .Field("xform_alias", HexOf(XformAlias))
                         .Field("matrix", HexOf(ByMatrix))
                         .Field("matrix_alias", HexOf(MatrixAlias))
                         .Field("rotate", HexOf(Rotated))
                         .Field("rotate_alias", HexOf(RotatedAlias))
                         .Field("inv_rotate", HexOf(InvRotated))
                         .Field("inv_rotate_alias", HexOf(InvRotatedAlias))
                         .Field("transpose", HexOf(Transposed))
                         .Field("transpose_alias", HexOf(TransposedAlias))
                         .Field("inv_xform", HexOf(InvXform))
                         .Field("inv_xform_alias", HexOf(InvXformAlias))
                         .Done());
        }
    }

    void QuatSuite(I3DMathEngine* Math)
    {
        Suite Out("math_quat.jsonl");
        for (const std::string& Line : InputLines("math/quat.hex"))
        {
            const Words W = ParseWords(Line);
            if (!Expect(Line, W, 12))
                continue;
            FQuaternion Q;
            FQuaternion P;
            FVector V;
            float T;
            Load(Q, W, 0);
            Load(P, W, 4);
            Load(V, W, 8);
            Load(T, W, 11);

            FQuaternion Mul;
            Math->MulQuat(Mul, Q, P);
            FQuaternion MulA = Q;
            Math->MulQuat(MulA, MulA, P);
            FQuaternion MulB = P;
            Math->MulQuat(MulB, Q, MulB);
            FVector Rotated;
            Math->TransformQuat(Rotated, Q, V);
            FMatrix Rotation;
            Math->QuaternionToMatrix(Rotation, Q);
            // QuatSlerpValue passes t's word through untouched.
            FQuaternion Slerp;
            Math->QuatSlerpValue(Slerp, Q, P, &T);
            FQuaternion SlerpA = Q;
            Math->QuatSlerpValue(SlerpA, SlerpA, P, &T);
            FQuaternion SlerpB = P;
            Math->QuatSlerpValue(SlerpB, Q, SlerpB, &T);

            Out.Line(Record(Line)
                         .Field("mul", HexOf(Mul))
                         .Field("mul_a", HexOf(MulA))
                         .Field("mul_b", HexOf(MulB))
                         .Field("rot", HexOf(Rotated))
                         .Field("matrix", HexOf(Rotation))
                         .Field("slerp", HexOf(Slerp))
                         .Field("slerp_a", HexOf(SlerpA))
                         .Field("slerp_b", HexOf(SlerpB))
                         .Done());
        }
    }

    void ListSuite(I3DMathEngine* Math)
    {
        Suite Out("math_list.jsonl");
        for (const std::string& Line : InputLines("math/list.hex"))
        {
            const Words W = ParseWords(Line);
            if (W.size() < 15 || (W.size() - 12) % 3 != 0)
            {
                fprintf(stderr, "bad math input: %s\n", Line.c_str());
                GFailed = true;
                continue;
            }
            FTransform X;
            Load(X, W, 0);
            const int Count = (int)((W.size() - 12) / 3);
            std::vector<FVector> Source(Count);
            for (int Index = 0; Index < Count; ++Index)
                Load(Source[Index], W, 12 + 3 * Index);

            std::vector<FVector> Xform(Count), ByMatrix(Count), Transposed(Count), Inverse(Count);
            std::vector<FVector> XformAlias(Source), MatrixAlias(Source), TransposedAlias(Source),
                InverseAlias(Source);
            Math->TransformListXform(&Xform[0], X, &Source[0], Count);
            Math->TransformListXform(&XformAlias[0], X, &XformAlias[0], Count);
            Math->TransformListMatrix(&ByMatrix[0], X.Rotation, &Source[0], Count);
            Math->TransformListMatrix(&MatrixAlias[0], X.Rotation, &MatrixAlias[0], Count);
            Math->TransposeTransformList(&Transposed[0], X.Rotation, &Source[0], Count);
            Math->TransposeTransformList(&TransposedAlias[0], X.Rotation, &TransposedAlias[0], Count);
            Math->InverseTransformList(&Inverse[0], X, &Source[0], Count);
            Math->InverseTransformList(&InverseAlias[0], X, &InverseAlias[0], Count);

            Out.Line(Record(Line)
                         .Field("xform", HexList(Xform))
                         .Field("xform_alias", HexList(XformAlias))
                         .Field("matrix", HexList(ByMatrix))
                         .Field("matrix_alias", HexList(MatrixAlias))
                         .Field("transpose", HexList(Transposed))
                         .Field("transpose_alias", HexList(TransposedAlias))
                         .Field("inverse", HexList(Inverse))
                         .Field("inverse_alias", HexList(InverseAlias))
                         .Done());
        }
    }

    void SqrtSuite(I3DMathEngine* Math)
    {
        Suite Out("math_sqrt.jsonl");
        for (const std::string& Line : InputLines("math/sqrt.hex"))
        {
            const Words W = ParseWords(Line);
            if (!Expect(Line, W, 1))
                continue;
            float X;
            Load(X, W, 0);
            float InvRoot = 0.0f;
            float Root = 0.0f;
            Math->InvSqrt(InvRoot, X);
            Math->Sqrt(Root, X);
            Out.Line(Record(Line).Field("inv_sqrt", HexOf(InvRoot)).Field("sqrt", HexOf(Root)).Done());
        }
    }
}

void RunMathSuites()
{
    I3DMathEngine* Math = AcquireMath();
    if (Math == nullptr)
        return;
    MatrixSuite(Math);
    TransformSuite(Math);
    VectorSuite(Math);
    QuatSuite(Math);
    ListSuite(Math);
    SqrtSuite(Math);
}
