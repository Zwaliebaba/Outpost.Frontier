// P4.14: the run time of SceneScript.dll -- param curves (CppSource/SceneScript/ParamCurve.cpp),
// spline paths (SplinePath.cpp, SplineMath.h) and scene playback (SceneScriptScene.cpp) --
// compiled into the oracle and driven through their interfaces.
//
// Suites (fixtures in crates/neuron-client/tests/fixtures/oracle, Rust side
// crates/neuron-client/tests/support/scene_script_runtime.rs):
//   scene_script_curve    one curve per line, created by factory name, then operations;
//   scene_script_spline   one spline path per line, then operations;
//   scene_script_playback one script per line, loaded through the parser's two passes
//                         (SceneScriptOracleLoad, oracle_scene_script_build.cpp) into an
//                         FSceneScriptScene as FScriptEngine::Slot4Load does, with recording
//                         engine objects attached to entities, then a program of timeline calls.
//
// Line formats. A word is 8 hex digits of its bits; an x87 result is the 10 bytes of the
// extended value, most significant first (20 hex digits), or "nan".
//   curve:    {"case":"<n>","curve":"<name>"}
//             {"case":"<n>","op":"<k>","add"|"replace"|"delete":"<int>"}
//             {"case":"<n>","op":"<k>","eval"|"wrap":"<x87>"}
//             {"case":"<n>","op":"<k>","class":"<str>","display":"<str>","count":"<n>","keys":"<id>:<4 words>,..."}
//   spline:   {"case":"<n>","op":"<k>","f":"<letter>","r":"<int>","v":"<tokens>"}
//   playback: {"case":"<n>","load":"<status>"[,"duration":..,"entities":..,"events":..]}
//             {"case":"<n>","event":"<i>","type":..,"start":..,"primary":..,"secondary":..}
//             {"case":"<n>","attach":"<handle>","r":"<int>"}
//             {"c":"<handle>","f":"<call>","a":"<tokens>"}   each control call, in order
//             {"case":"<n>","op":"<text>","r":"<int>","clock":..,"state":..}
//             {"case":"<n>","entity":"<handle>","pos":"<3 words>","orient":"<9 words>","world":"<12 words>"}
// An event descriptor handed to a control is its common words without the id (type, start,
// flags, event flags, field 10, duration, primary, secondary, period, curve 0/1), " |", and
// the payload as the build suite writes it; StopSound gets the common words only.

#include "../SceneScript/SceneScriptScene.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "oracle_support.h"
#include "x86math.h"

namespace scene_script_build
{
    std::string Words(const void* Data, size_t Count);
    std::string Str(const char* Text);
    std::string EventPayload(const void* Desc);
}

GENRESULT SceneScriptOracleLoad(const unsigned char* Data, size_t Size, IScriptEngine* Engine,
                                unsigned int* Duration);

// SceneScript.cpp (not compiled in) defines these for ParamCurve.cpp, SplinePath.cpp and
// SceneScriptScene.cpp.
bool SceneScriptNameEqual(const char* Left, const char* Right)
{
    if (Left == nullptr || Right == nullptr)
        return false;
    return _stricmp(Left, Right) == 0;
}

// The math engine through the component manager, as SceneScript.cpp creates it;
// x86math.dll registers it when it loads (OracleLoadMathEngine).
I3DMathEngine* SceneScriptAcquire3DMathEngine()
{
    static bool bLoaded = false;
    if (!bLoaded)
    {
        bLoaded = true;
        if (!OracleLoadMathEngine())
        {
            fprintf(stderr, "scene script runtime: no 3DMathEngine\n");
            oracle::GFailed = true;
        }
    }
    FMathDesc Desc;
    Desc.Size = sizeof(FMathDesc);
    Desc.InterfaceName = "3DMathEngine";
    Desc.Implementation = nullptr;
    void* Instance = nullptr;
    NEURONCOMPONENTS_Acquire()->CreateInstance(reinterpret_cast<AGGDESC*>(&Desc), &Instance);
    return static_cast<I3DMathEngine*>(Instance);
}

namespace scene_script_runtime
{
    using oracle::Format;

    std::string W(const void* Data)
    {
        unsigned int Bits;
        std::memcpy(&Bits, Data, sizeof(Bits));
        return Format("%08x", Bits);
    }

    std::string Ws(const void* Data, size_t Count)
    {
        return scene_script_build::Words(Data, Count);
    }

    std::string I(int Value)
    {
        return W(&Value);
    }

    std::string X87(const FSceneScriptExtendedProgress& Value)
    {
        const unsigned char* B = Value.Bytes;
        const unsigned int Exponent = (static_cast<unsigned int>(B[9] & 0x7f) << 8) | B[8];
        bool bInteger = (B[7] & 0x80) != 0;
        bool bFraction = (B[7] & 0x7f) != 0;
        for (int Index = 0; Index < 7; ++Index)
            bFraction = bFraction || B[Index] != 0;
        if (Exponent == 0x7fff && (bFraction || !bInteger))
            return "nan";
        std::string Text;
        for (int Index = 9; Index >= 0; --Index)
            Text += Format("%02x", B[Index]);
        return Text;
    }

    unsigned int ParseHex(const std::string& Text)
    {
        return static_cast<unsigned int>(std::strtoul(Text.c_str(), nullptr, 16));
    }

    float HexFloat(const std::string& Text)
    {
        const unsigned int Bits = ParseHex(Text);
        float Value;
        std::memcpy(&Value, &Bits, sizeof(Value));
        return Value;
    }

    std::vector<std::string> Split(const std::string& Text, char Separator)
    {
        std::vector<std::string> Parts;
        size_t Start = 0;
        while (true)
        {
            const size_t End = Text.find(Separator, Start);
            Parts.push_back(Text.substr(Start, End == std::string::npos ? std::string::npos : End - Start));
            if (End == std::string::npos)
                return Parts;
            Start = End + 1;
        }
    }

    std::string CommonWords(const FSceneScriptEventDesc* Desc)
    {
        const int Curve = Desc->ParamCurve != nullptr ? 1 : 0;
        const int Words[10] = {Desc->Type,         Desc->StartMs,       Desc->Flags,
                               Desc->EventFlags,   Desc->Field10,       Desc->DurationMs,
                               Desc->PrimaryIndex, Desc->SecondaryIndex, Desc->Field28,
                               Curve};
        return Ws(Words, 10);
    }

    std::string DescText(const void* Desc)
    {
        return CommonWords(static_cast<const FSceneScriptEventDesc*>(Desc)) + " | " +
               scene_script_build::EventPayload(Desc);
    }

    // ---------------------------------------------------------------- curves

    void CurveSuite()
    {
        oracle::Suite Out("scene_script_curve.jsonl");
        unsigned int Case = 0;
        for (const std::string& Line : oracle::InputLines("scene_script_curve.list"))
        {
            const std::vector<std::string> Tokens = Split(Line, ' ');
            const std::string& Name = Tokens[0];
            INeuronComponent* Object = SceneScriptCreateParamCurve(Name.c_str(), Name == "LinearPCurve");
            IParamCurve* Curve = nullptr;
            if (Object != nullptr)
                Object->QueryInterface(Iids::ParamCurve, reinterpret_cast<void**>(&Curve));
            Out.Line(Format("{\"case\":\"%u\",\"curve\":\"%s\"}", Case, Curve != nullptr ? Name.c_str() : "null"));
            for (size_t Op = 1; Curve != nullptr && Op < Tokens.size(); ++Op)
            {
                const std::string& Token = Tokens[Op];
                const std::vector<std::string> Args = Split(Token.substr(1), ',');
                const std::string Head = Format("{\"case\":\"%u\",\"op\":\"%u\",", Case, static_cast<unsigned int>(Op));
                FSceneScriptExtendedProgress Result;
                switch (Token[0])
                {
                case 'a':
                    Out.Line(Head + Format("\"add\":\"%d\"}", Curve->AddKey(HexFloat(Args[0]), HexFloat(Args[1]),
                                                                            HexFloat(Args[2]), HexFloat(Args[3]))));
                    break;
                case 'r':
                    Out.Line(Head + Format("\"replace\":\"%d\"}",
                                           Curve->Slot4Replace(std::atoi(Args[0].c_str()), HexFloat(Args[1]),
                                                               HexFloat(Args[2]), HexFloat(Args[3]),
                                                               HexFloat(Args[4]))));
                    break;
                case 'd':
                    Out.Line(Head + Format("\"delete\":\"%d\"}", Curve->Slot7DeleteKey(std::atoi(Args[0].c_str()))));
                    break;
                case 'e':
                {
                    const float T = HexFloat(Args[0]);
                    Curve->EvaluateToExtended(&T, &Result);
                    Out.Line(Head + "\"eval\":\"" + X87(Result) + "\"}");
                    break;
                }
                case 'w':
                    Curve->WrapEvaluateToExtended(std::atoi(Args[0].c_str()), std::atoi(Args[1].c_str()), &Result);
                    Out.Line(Head + "\"wrap\":\"" + X87(Result) + "\"}");
                    break;
                case 's':
                {
                    std::string Keys;
                    for (int Index = 0;; ++Index)
                    {
                        const int Id = Curve->Slot9IdByIndex(Index);
                        if (Id == -1)
                            break;
                        float K[4] = {0, 0, 0, 0};
                        Curve->Slot5ReadKey(Id, &K[0], &K[1], &K[2], &K[3]);
                        if (!Keys.empty())
                            Keys += ",";
                        Keys += Format("%d:", Id) + Ws(K, 4);
                    }
                    Out.Line(Head + "\"class\":\"" + scene_script_build::Str(Curve->Slot10Clsid()) +
                             "\",\"display\":\"" + scene_script_build::Str(Curve->Slot11Display()) +
                             Format("\",\"count\":\"%u\",\"keys\":\"", Curve->GetKeyCount()) + Keys + "\"}");
                    break;
                }
                default:
                    fprintf(stderr, "scene_script_curve: bad op %s\n", Token.c_str());
                    oracle::GFailed = true;
                    break;
                }
            }
            ++Case;
        }
    }

    // ---------------------------------------------------------------- splines

    const unsigned int Sentinel = 0xdeadbeefu;

    void FillSentinel(float* Out, size_t Count)
    {
        for (size_t Index = 0; Index < Count; ++Index)
            std::memcpy(Out + Index, &Sentinel, sizeof(Sentinel));
    }

    void SplineSuite()
    {
        oracle::Suite Out("scene_script_spline.jsonl");
        unsigned int Case = 0;
        for (const std::string& Line : oracle::InputLines("scene_script_spline.list"))
        {
            const std::vector<std::string> Tokens = Split(Line, ' ');
            const bool bOrientation = Tokens[0] == "o";
            INeuronComponent* Object = SceneScriptCreateSplinePath(
                bOrientation ? "CV_CROrientationSplinePath" : "CV_CRSplinePath", bOrientation, nullptr, nullptr);
            ICRSpline* Spline = nullptr;
            IMotionPath* Motion = nullptr;
            IPersistString* Persist = nullptr;
            Object->QueryInterface(Iids::CRSpline, reinterpret_cast<void**>(&Spline));
            Object->QueryInterface(Iids::MotionPath, reinterpret_cast<void**>(&Motion));
            Object->QueryInterface(Iids::PersistString, reinterpret_cast<void**>(&Persist));
            for (size_t Op = 1; Op < Tokens.size(); ++Op)
            {
                const std::string& Token = Tokens[Op];
                const std::vector<std::string> Args = Split(Token.substr(1), ',');
                std::vector<float> F;
                for (const std::string& Arg : Args)
                    F.push_back(Arg.empty() ? 0.0f : HexFloat(Arg));
                int R = 0;
                std::string V;
                float Buffer[12];
                FillSentinel(Buffer, 12);
                switch (Token[0])
                {
                case 'P':
                {
                    std::vector<float> Points;
                    if (Token.size() > 1)
                        Points = F;
                    R = Spline->Slot4LoadPoints(Points.empty() ? nullptr : Points.data(),
                                                static_cast<unsigned long>(Points.size() / 3));
                    break;
                }
                case 'L':
                    if (Token == "L!")
                        R = Persist->Slot3Load(nullptr);
                    else
                    {
                        const oracle::Bytes Text = oracle::Unhex(Token.substr(1));
                        R = Persist->Slot3Load(reinterpret_cast<const char*>(Text.data()));
                    }
                    break;
                case 'C':
                    R = Spline->Slot8(static_cast<unsigned char>(std::atoi(Args[0].c_str())));
                    break;
                case 'S':
                    R = Spline->Slot3(static_cast<unsigned int>(std::atoi(Args[0].c_str())), &F[1]);
                    break;
                case 'Q':
                    R = Spline->Slot10(static_cast<unsigned int>(std::atoi(Args[0].c_str())), &F[1]);
                    break;
                case 'E':
                    R = Motion->Slot4Evaluate(F[0], Buffer);
                    V = Ws(Buffer, 3);
                    break;
                case 'K':
                    R = Motion->Slot6LookAt(F[0], Buffer);
                    V = Ws(Buffer, 3);
                    break;
                case 'O':
                    R = Motion->Slot5(F[0], Buffer);
                    V = Ws(Buffer, 4);
                    break;
                case 'T':
                    std::memcpy(Buffer, &F[1], 12 * sizeof(float));
                    R = Motion->Slot3Transform(F[0], Buffer);
                    V = Ws(Buffer, 12);
                    break;
                case 'N':
                    R = Motion->Slot7Length(Buffer);
                    V = Ws(Buffer, 1);
                    break;
                case 'G':
                {
                    const unsigned int Index = static_cast<unsigned int>(std::atoi(Args[0].c_str()));
                    float Point[3], Time[1], Quat[4], Xform[12];
                    FillSentinel(Point, 3);
                    FillSentinel(Time, 1);
                    FillSentinel(Quat, 4);
                    FillSentinel(Xform, 12);
                    const int R6 = Spline->Slot6(Index, Point);
                    const int R7 = Spline->Slot7(Index, Time);
                    const int R12 = Spline->Slot12(Index, Quat);
                    const int R15 = Spline->Slot15(Index, Xform);
                    V = I(R6) + " " + Ws(Point, 3) + " " + I(R7) + " " + Ws(Time, 1) + " " + I(R12) + " " +
                        Ws(Quat, 4) + " " + I(R15) + " " + Ws(Xform, 12);
                    break;
                }
                case 'Z':
                {
                    const unsigned long Size = static_cast<unsigned long>(std::atoi(Args[0].c_str()));
                    std::vector<char> Text(Size + 4096, 0);
                    R = Persist->Slot4Save(Text.data(), Size);
                    V = R == GR_OK ? "s" + oracle::HexC(Text.data()) : std::string("-");
                    break;
                }
                case 'I':
                {
                    const unsigned long Live = Spline->Slot5();
                    const int Closed = Spline->Slot9();
                    const unsigned long Estimate = Persist->Slot5EstimateSize();
                    V = Format("%08lx %08x %08lx", Live, static_cast<unsigned int>(Closed), Estimate);
                    break;
                }
                default:
                    fprintf(stderr, "scene_script_spline: bad op %s\n", Token.c_str());
                    oracle::GFailed = true;
                    break;
                }
                Out.Line(Format("{\"case\":\"%u\",\"op\":\"%u\",\"f\":\"%c\",\"r\":\"%d\",\"v\":\"", Case,
                                static_cast<unsigned int>(Op), Token[0], R) +
                         V + "\"}");
            }
            ++Case;
        }
    }

    // ---------------------------------------------------------------- playback

    // Control calls of the current operation, in order.
    std::vector<std::string> GCalls;

    // The interfaces a recording engine object answers (the Rust side's
    // playback::interface bits).
    enum : unsigned int
    {
        BitGeo = 0x1,
        BitCamera = 0x2,
        BitCompound = 0x4,
        BitIk = 0x8,
        BitLight = 0x10,
        BitD3dLight = 0x20,
        BitSound = 0x40,
        BitUserEvent = 0x80,
        BitSubtitle = 0x100,
        BitFog = 0x200,
        BitReverb = 0x400,
        BitPsys = 0x800,
        BitMotionPath = 0x1000,
        BitScript = 0x2000,
    };

    // The state one recording engine object keeps: what the getters answer and
    // the setters change.
    struct FStubState
    {
        int Handle;
        unsigned int Mask;
        float Pos[3];
        float Orient[9];
        float Camera[4]; // fov, near, far, aspect
        float Floor;
        float Sparam;
        unsigned int Reverb[4];
        unsigned int Fog[8];
        float Audio[8];
        int LightColor[3];
        float LightDirection[3];
        float LightRange;
        float LightCutoff;
        int LightSlot13;
        int LightSlot14;
        int D3dOn;
        unsigned int D3dLight[26];
        float D3dDirection[3];
    };

    float Bits(unsigned int Word)
    {
        float Value;
        std::memcpy(&Value, &Word, sizeof(Value));
        return Value;
    }

    void InitStub(FStubState& S, int Handle, unsigned int Mask)
    {
        std::memset(&S, 0, sizeof(S));
        S.Handle = Handle;
        S.Mask = Mask;
        S.Orient[0] = S.Orient[4] = S.Orient[8] = 1.0f;
        S.Camera[0] = 1.0f;
        S.Camera[1] = 0.5f;
        S.Camera[2] = 2000.0f;
        S.Camera[3] = 1.25f;
        S.Floor = 0.25f;
        S.Sparam = 0.5f;
        const float Reverb[3] = {0.5f, 1.5f, 0.75f};
        S.Reverb[0] = 3;
        std::memcpy(&S.Reverb[1], Reverb, sizeof(Reverb));
        const float FogRange[3] = {5.0f, 50.0f, 0.01f};
        S.Fog[0] = 0x0101;
        S.Fog[1] = 10;
        S.Fog[2] = 20;
        S.Fog[3] = 30;
        S.Fog[4] = 1;
        std::memcpy(&S.Fog[5], FogRange, sizeof(FogRange));
        const float Audio[8] = {-3.0f, 0.25f, 2.0f, 500.0f, 0.0f, 0.0f, -1.0f, 0.5f};
        std::memcpy(S.Audio, Audio, sizeof(Audio));
        const int Cones[2] = {180, 270};
        std::memcpy(&S.Audio[4], Cones, sizeof(Cones));
        S.LightColor[0] = 200;
        S.LightColor[1] = 100;
        S.LightColor[2] = 50;
        S.LightDirection[2] = -1.0f;
        S.LightRange = 100.0f;
        S.LightCutoff = 0.5f;
        S.LightSlot13 = 1;
        S.LightSlot14 = 0;
        S.D3dOn = 1;
        // D3DLIGHT8: type, diffuse, specular, ambient, position, direction, range,
        // falloff, attenuation, theta, phi.
        const float Light[26] = {0.0f, 0.5f, 0.5f, 0.5f, 1.0f, 0, 0, 0, 0, 0, 0, 0, 0,
                                 0,    0,    0,    0,    -1.0f, 0, 50.0f, 1.0f, 1.0f, 0, 0, 0.25f, 0.5f};
        std::memcpy(S.D3dLight, Light, sizeof(Light));
        S.D3dLight[0] = 3;
        S.D3dDirection[1] = -1.0f;
    }

    void Call(const FStubState& S, const char* Name, const std::string& Args = std::string())
    {
        GCalls.push_back(Format("{\"c\":\"%d\",\"f\":\"%s\",\"a\":\"", S.Handle, Name) + Args + "\"}");
    }

    // The scene never asks a facet for another interface.
#define STUB_UNKNOWN                                                                                \
    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override                 \
    {                                                                                               \
        *Out = nullptr;                                                                             \
        return GR_INTERFACE_UNSUPPORTED;                                                            \
    }                                                                                               \
    unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }                              \
    unsigned long NEURONCOMPONENTS_API Release() override { return 1; }

    struct FGeo final : IGeoTransformControl
    {
        FStubState* S;
        STUB_UNKNOWN
        GENRESULT NEURONCOMPONENTS_API SetPosition(float* P) override
        {
            Call(*S, "SetPosition", Ws(P, 3));
            std::memcpy(S->Pos, P, sizeof(S->Pos));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetOrientation(float* M) override
        {
            Call(*S, "SetOrientation", Ws(M, 9));
            std::memcpy(S->Orient, M, sizeof(S->Orient));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetPosition(float* Out) override
        {
            Call(*S, "GetPosition");
            std::memcpy(Out, S->Pos, sizeof(S->Pos));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetOrientation(float* Out) override
        {
            Call(*S, "GetOrientation");
            std::memcpy(Out, S->Orient, sizeof(S->Orient));
            return GR_OK;
        }
    };

    struct FCamera final : ICameraControl
    {
        FStubState* S;
        STUB_UNKNOWN
        GENRESULT Set(const char* Name, int Index, const float* V)
        {
            Call(*S, Name, W(V));
            S->Camera[Index] = *V;
            return GR_OK;
        }
        GENRESULT Get(const char* Name, int Index, float* Out)
        {
            Call(*S, Name);
            *Out = S->Camera[Index];
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetFov(const float* V) override { return Set("SetFov", 0, V); }
        GENRESULT NEURONCOMPONENTS_API CameraSlot4(void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetAspect(const float* V) override { return Set("SetAspect", 3, V); }
        GENRESULT NEURONCOMPONENTS_API SetNear(const float* V) override { return Set("SetNear", 1, V); }
        GENRESULT NEURONCOMPONENTS_API SetFar(const float* V) override { return Set("SetFar", 2, V); }
        GENRESULT NEURONCOMPONENTS_API GetFov(float* Out) override { return Get("GetFov", 0, Out); }
        GENRESULT NEURONCOMPONENTS_API CameraSlot9(void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetAspect(float* Out) override { return Get("GetAspect", 3, Out); }
        GENRESULT NEURONCOMPONENTS_API GetNear(float* Out) override { return Get("GetNear", 1, Out); }
        GENRESULT NEURONCOMPONENTS_API GetFar(float* Out) override { return Get("GetFar", 2, Out); }
    };

    struct FCompound final : ICompoundControl
    {
        FStubState* S;
        STUB_UNKNOWN
        GENRESULT NEURONCOMPONENTS_API StartMotion(void* Desc) override
        {
            Call(*S, "StartMotion", DescText(Desc));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API StopMotion(void* Desc) override
        {
            Call(*S, "StopMotion", DescText(Desc));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetFloorHeight(float Height) override
        {
            Call(*S, "SetFloorHeight", W(&Height));
            S->Floor = Height;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API AdvanceTime(int Delta) override
        {
            Call(*S, "AdvanceTime", I(Delta));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API ConnectHardpoints(void* Desc) override
        {
            Call(*S, "ConnectHardpoints", DescText(Desc));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API DisconnectHardpoints(void* Desc) override
        {
            Call(*S, "DisconnectHardpoints", DescText(Desc));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetHardpointXform(int Type, char const* Name, float* Out) override
        {
            Call(*S, "GetHardpointXform", I(Type) + " " + scene_script_build::Str(Name));
            const float Xform[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1,
                                     static_cast<float>(Name != nullptr ? std::strlen(Name) : 0),
                                     static_cast<float>(Type), 1.5f};
            std::memcpy(Out, Xform, sizeof(Xform));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetFloorHeight(float* Out) override
        {
            Call(*S, "GetFloorHeight");
            *Out = S->Floor;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetTransform(float const*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTransform(float*) override { return GR_GENERIC; }
    };

    struct FIk final : IIKControl
    {
        FStubState* S;
        STUB_UNKNOWN
        GENRESULT NEURONCOMPONENTS_API StartIk(void* Desc, float* P, float* M) override
        {
            Call(*S, "StartIk", DescText(Desc) + " | " + Ws(P, 3) + " " + Ws(M, 9));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API StopIk(void* Desc) override
        {
            Call(*S, "StopIk", DescText(Desc));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API UpdateIk(void* Desc, float* P, float* M) override
        {
            Call(*S, "UpdateIk", DescText(Desc) + " | " + Ws(P, 3) + " " + Ws(M, 9));
            return GR_OK;
        }
    };

    struct FLight final : ILightControl
    {
        FStubState* S;
        STUB_UNKNOWN
        GENRESULT NEURONCOMPONENTS_API SetColor(int* Rgb) override
        {
            Call(*S, "LSetColor", Ws(Rgb, 3));
            std::memcpy(S->LightColor, Rgb, sizeof(S->LightColor));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetDirection(float* D) override
        {
            Call(*S, "LSetDirection", Ws(D, 3));
            std::memcpy(S->LightDirection, D, sizeof(S->LightDirection));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetRange(float V) override
        {
            Call(*S, "LSetRange", W(&V));
            S->LightRange = V;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetCutoff(float V) override
        {
            Call(*S, "LSetCutoff", W(&V));
            S->LightCutoff = V;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetOn(int V) override
        {
            Call(*S, "LSetOn", I(V));
            S->LightSlot13 = V;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetIsSpot(int V) override
        {
            Call(*S, "LSetIsSpot", I(V));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetColor(float* Out) override
        {
            Call(*S, "LGetColor");
            std::memcpy(Out, S->LightColor, sizeof(S->LightColor));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetDirection(float* Out) override
        {
            Call(*S, "LGetDirection");
            std::memcpy(Out, S->LightDirection, sizeof(S->LightDirection));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetRange(float* Out) override
        {
            Call(*S, "LGetRange");
            *Out = S->LightRange;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetCutoff(float* Out) override
        {
            Call(*S, "LGetCutoff");
            *Out = S->LightCutoff;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetSlot13(float* Out) override
        {
            Call(*S, "LGetSlot13");
            std::memcpy(Out, &S->LightSlot13, sizeof(int));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetSlot14(float* Out) override
        {
            Call(*S, "LGetSlot14");
            std::memcpy(Out, &S->LightSlot14, sizeof(int));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API Slot15() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot16(void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot17(void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot18(void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot19(void*) override { return GR_GENERIC; }
    };

    struct FD3dLight final : ID3DLightControl
    {
        FStubState* S;
        STUB_UNKNOWN
        GENRESULT NEURONCOMPONENTS_API SetOn(int V) override
        {
            Call(*S, "DSetOn", I(V));
            S->D3dOn = V;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetLight(void const* Light) override
        {
            Call(*S, "DSetLight", Ws(Light, 26));
            std::memcpy(S->D3dLight, Light, sizeof(S->D3dLight));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetDirection(float* D) override
        {
            Call(*S, "DSetDirection", Ws(D, 3));
            std::memcpy(S->D3dDirection, D, sizeof(S->D3dDirection));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetOn(int* Out) override
        {
            Call(*S, "DGetOn");
            *Out = S->D3dOn;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetLight(void* Out) override
        {
            Call(*S, "DGetLight");
            std::memcpy(Out, S->D3dLight, sizeof(S->D3dLight));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetDirection(float* Out) override
        {
            Call(*S, "DGetDirection");
            std::memcpy(Out, S->D3dDirection, sizeof(S->D3dDirection));
            return GR_OK;
        }
    };

    struct FFog final : IFogControl
    {
        FStubState* S;
        STUB_UNKNOWN
        GENRESULT NEURONCOMPONENTS_API SetOn(unsigned char On) override
        {
            Call(*S, "FSetOn", I(On));
            S->Fog[0] = (S->Fog[0] & ~0xffu) | On;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetOn(unsigned char*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetColor(int const* Rgb) override
        {
            Call(*S, "FSetColor", Ws(Rgb, 3));
            std::memcpy(&S->Fog[1], Rgb, 3 * sizeof(int));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetColor(int*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetRange(float, float, float) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetRange(float*, float*, float*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetTableMode(unsigned char Table, unsigned int Mode) override
        {
            Call(*S, "FSetTableMode", I(Table) + " " + I(static_cast<int>(Mode)));
            S->Fog[0] = (S->Fog[0] & ~0xff00u) | (static_cast<unsigned int>(Table) << 8);
            S->Fog[4] = Mode;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetTableMode(unsigned char*, unsigned int*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetPacked(unsigned char const*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetPacked(unsigned char* Out) override
        {
            Call(*S, "FGetPacked");
            std::memcpy(Out, S->Fog, sizeof(S->Fog));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetRangeValue(const float* Start, const float* End, const float* Density) override
        {
            Call(*S, "FSetRangeValue", W(Start) + " " + W(End) + " " + W(Density));
            std::memcpy(&S->Fog[5], Start, 4);
            std::memcpy(&S->Fog[6], End, 4);
            std::memcpy(&S->Fog[7], Density, 4);
            return GR_OK;
        }
    };

    struct FReverb final : IReverbControl
    {
        FStubState* S;
        STUB_UNKNOWN
        GENRESULT NEURONCOMPONENTS_API SetReverb(unsigned int Environment, float Volume, float Decay, float Damping) override
        {
            Call(*S, "SetReverb", I(static_cast<int>(Environment)) + " " + W(&Volume) + " " + W(&Decay) + " " + W(&Damping));
            S->Reverb[0] = Environment;
            std::memcpy(&S->Reverb[1], &Volume, 4);
            std::memcpy(&S->Reverb[2], &Decay, 4);
            std::memcpy(&S->Reverb[3], &Damping, 4);
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetReverb(float* Environment, float* Volume, float* Decay, float* Damping) override
        {
            Call(*S, "GetReverb");
            std::memcpy(Environment, &S->Reverb[0], 4);
            std::memcpy(Volume, &S->Reverb[1], 4);
            std::memcpy(Decay, &S->Reverb[2], 4);
            std::memcpy(Damping, &S->Reverb[3], 4);
            return GR_OK;
        }
    };

    struct FPsys final : IPSysControl
    {
        FStubState* S;
        STUB_UNKNOWN
        GENRESULT NEURONCOMPONENTS_API Start() override
        {
            Call(*S, "PStart");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API StopEffect() override
        {
            Call(*S, "PStop");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API Tick(int Delta) override
        {
            Call(*S, "PTick", I(Delta));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetSparam(float V) override
        {
            Call(*S, "PSetSparam", W(&V));
            S->Sparam = V;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetSparam(float* Out) override
        {
            Call(*S, "PGetSparam");
            *Out = S->Sparam;
            return GR_OK;
        }
    };

    struct FSound final : ISoundControl
    {
        FStubState* S;
        STUB_UNKNOWN
        GENRESULT NEURONCOMPONENTS_API StartSound(void* Desc, int Time) override
        {
            Call(*S, "StartSound", DescText(Desc) + " | " + I(Time));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API StopSound(void* Desc) override
        {
            Call(*S, "StopSound", CommonWords(static_cast<const FSceneScriptEventDesc*>(Desc)));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetAttenuation(float) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetPan(float) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetAttenuation(float*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetPan(float*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetAudioProps(float const* Props, int Mask) override
        {
            Call(*S, "SetAudioProps", Ws(Props, 8) + " " + I(Mask));
            std::memcpy(S->Audio, Props, sizeof(S->Audio));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetAudioProps(float* Props, int Mask) override
        {
            Call(*S, "GetAudioProps", I(Mask));
            std::memcpy(Props, S->Audio, sizeof(S->Audio));
            return GR_OK;
        }
    };

    struct FSubtitle final : ISubtitleControl
    {
        FStubState* S;
        STUB_UNKNOWN
        GENRESULT NEURONCOMPONENTS_API ShowSubtitle(int Id) override
        {
            Call(*S, "ShowSubtitle", I(Id));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API HideSubtitle(int Id) override
        {
            Call(*S, "HideSubtitle", I(Id));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API Slot5() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot6() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot7(void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot8(void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot9(void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot10(void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot11(void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot12(void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot13(void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot14(void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot15(void*) override { return GR_GENERIC; }
    };

    struct FUserEvent final : IUserEventControl
    {
        FStubState* S;
        STUB_UNKNOWN
        GENRESULT NEURONCOMPONENTS_API Slot3(void* Desc) override
        {
            Call(*S, "UStart", DescText(Desc));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API Slot4(void* Desc) override
        {
            Call(*S, "UStop", DescText(Desc));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API Slot5() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot6() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot7(void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot8(void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot9(void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot10(void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot11(void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot12(void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot13(void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot14(void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot15(void*) override { return GR_GENERIC; }
    };

    struct FScript final : IScriptControl
    {
        FStubState* S;
        STUB_UNKNOWN
        GENRESULT NEURONCOMPONENTS_API Slot3Start(int Arg) override
        {
            Call(*S, "SStart", I(Arg));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API Slot4Tick(int Delta) override
        {
            Call(*S, "STick", I(Delta));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API Slot5Stop() override
        {
            Call(*S, "SStop");
            return GR_OK;
        }
        int NEURONCOMPONENTS_API Slot6GetDuration() override { return 0; }
    };

    struct FMotion final : IMotionPath
    {
        FStubState* S;
        STUB_UNKNOWN
        GENRESULT NEURONCOMPONENTS_API Slot3Transform(float, float*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot4Evaluate(float T, float* Out) override
        {
            Call(*S, "MEvaluate", W(&T));
            Out[0] = T;
            Out[1] = 2.0f * T;
            Out[2] = 3.0f * T;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API Slot5(float T, float* Out) override
        {
            Call(*S, "MOrient", W(&T));
            Out[0] = 1.0f;
            Out[1] = Out[2] = Out[3] = 0.0f;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API Slot6LookAt(float T, float* Out) override
        {
            Call(*S, "MLookAt", W(&T));
            Out[0] = Out[1] = 0.0f;
            Out[2] = 1.0f;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API Slot7Length(float*) override { return GR_GENERIC; }
    };

    // A recording engine object: one facet per control interface, answered when
    // its bit is in the mask.
    struct FStubObject final : INeuronComponent
    {
        FStubState S;
        FGeo Geo;
        FCamera Camera;
        FCompound Compound;
        FIk Ik;
        FLight Light;
        FD3dLight D3dLight;
        FFog Fog;
        FReverb Reverb;
        FPsys Psys;
        FSound Sound;
        FSubtitle Subtitle;
        FUserEvent UserEvent;
        FScript Script;
        FMotion Motion;

        FStubObject(int Handle, unsigned int Mask)
        {
            InitStub(S, Handle, Mask);
            Bind(Geo);
            Bind(Camera);
            Bind(Compound);
            Bind(Ik);
            Bind(Light);
            Bind(D3dLight);
            Bind(Fog);
            Bind(Reverb);
            Bind(Psys);
            Bind(Sound);
            Bind(Subtitle);
            Bind(UserEvent);
            Bind(Script);
            Bind(Motion);
        }

        template <class TFacet>
        void Bind(TFacet& Facet)
        {
            Facet.S = &S;
        }

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            struct FEntry
            {
                const char* Iid;
                unsigned int Bit;
                void* Facet;
            };
            const FEntry Table[] = {
                {Iids::GeoTransformControl, BitGeo, static_cast<IGeoTransformControl*>(&Geo)},
                {Iids::CameraControl, BitCamera, static_cast<ICameraControl*>(&Camera)},
                {Iids::CompoundControl, BitCompound, static_cast<ICompoundControl*>(&Compound)},
                {Iids::IKControl, BitIk, static_cast<IIKControl*>(&Ik)},
                {Iids::LightControl, BitLight, static_cast<ILightControl*>(&Light)},
                {Iids::D3DLightControl, BitD3dLight, static_cast<ID3DLightControl*>(&D3dLight)},
                {Iids::SoundControl, BitSound, static_cast<ISoundControl*>(&Sound)},
                {Iids::UserEventControl, BitUserEvent, static_cast<IUserEventControl*>(&UserEvent)},
                {Iids::SubtitleControl, BitSubtitle, static_cast<ISubtitleControl*>(&Subtitle)},
                {Iids::FogControl, BitFog, static_cast<IFogControl*>(&Fog)},
                {Iids::ReverbControl, BitReverb, static_cast<IReverbControl*>(&Reverb)},
                {Iids::PSysControl, BitPsys, static_cast<IPSysControl*>(&Psys)},
                {Iids::MotionPath, BitMotionPath, static_cast<IMotionPath*>(&Motion)},
                {Iids::ScriptControl, BitScript, static_cast<IScriptControl*>(&Script)},
            };
            for (const FEntry& Entry : Table)
            {
                if (std::strcmp(Iid, Entry.Iid) == 0 && (S.Mask & Entry.Bit) != 0)
                {
                    *Out = Entry.Facet;
                    return GR_OK;
                }
            }
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
    };

    // FScriptEngine's slots the parser's second pass calls, on a scene.
    class FPlaybackEngine final : public IScriptEngine
    {
    public:
        explicit FPlaybackEngine(FSceneScriptScene& Scene) : Scene(Scene) {}

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API Slot3(void*, void*) override { return GR_OK; }
        GENRESULT NEURONCOMPONENTS_API Slot4Load(void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot5Save(void*) override { return GR_GENERIC; }
        unsigned long NEURONCOMPONENTS_API Slot6EntityCount() override { return Scene.LiveEntityCount(); }
        void* NEURONCOMPONENTS_API Slot7EntityHandle(int Index) override { return Scene.EntityHandle(Index); }
        void* NEURONCOMPONENTS_API Slot8(int Index) override { return Scene.EntitySnapInorder(Index); }
        void* NEURONCOMPONENTS_API Slot9Entity(int Index) override { return Scene.EntitySnap(Index); }
        GENRESULT NEURONCOMPONENTS_API Slot10GetPos(int Index, float* Out) override { return Scene.GetEntityPos(Index, Out); }
        GENRESULT NEURONCOMPONENTS_API Slot11GetOrient(int Index, float* Out) override { return Scene.GetEntityOrient(Index, Out); }
        GENRESULT NEURONCOMPONENTS_API Slot12Attach(int, void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot13Detach(int) override { return GR_GENERIC; }
        unsigned long NEURONCOMPONENTS_API Slot14EventCount() override { return Scene.EventCount(); }
        void* NEURONCOMPONENTS_API Slot15EventByIndex(int Index) override { return Scene.EventDescByIndex(Index); }
        void* NEURONCOMPONENTS_API Slot16EventById(int Id) override { return Scene.EventDescById(Id); }
        int NEURONCOMPONENTS_API Slot17GetDuration() override { return Scene.DurationMs(); }
        GENRESULT NEURONCOMPONENTS_API Slot18SetDuration(int Ms) override
        {
            Scene.SetDuration(Ms);
            return GR_OK;
        }
        int NEURONCOMPONENTS_API Slot19GetClock() override { return Scene.ClockMs(); }
        GENRESULT __thiscall Slot20SetLoop(unsigned char Value) override
        {
            Scene.SetLoop(Value);
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API Slot21GetState() override { return Scene.PlaybackState(); }
        GENRESULT NEURONCOMPONENTS_API Slot22Start(int Arg) override { return Scene.StartPlayback(Arg); }
        GENRESULT NEURONCOMPONENTS_API Slot23Stop() override { return Scene.StopPlayback(); }
        GENRESULT NEURONCOMPONENTS_API Slot24Pause() override { return Scene.PausePlayback(); }
        GENRESULT NEURONCOMPONENTS_API Slot25Start(int Arg) override { return Scene.StartPlayback(Arg); }
        GENRESULT NEURONCOMPONENTS_API Slot26Tick(int Dt) override { return Scene.Tick(Dt); }
        GENRESULT NEURONCOMPONENTS_API Slot27(void*) override { return GR_OK; }
        GENRESULT NEURONCOMPONENTS_API Slot28(void*, void*) override { return GR_OK; }
        GENRESULT NEURONCOMPONENTS_API Slot29(void*, void*, void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot30(void*, void*, void*) override { return GR_GENERIC; }
        unsigned long NEURONCOMPONENTS_API Slot31(int Index) override { return Scene.UserPropCount(Index); }
        const char* NEURONCOMPONENTS_API Slot32(int Index, int N) override { return Scene.UserPropKey(Index, N); }
        GENRESULT NEURONCOMPONENTS_API Slot33UserProp(void* Handle, const char* Key, char** Out) override
        {
            return Scene.UserProp(Handle, Key, Out);
        }
        GENRESULT NEURONCOMPONENTS_API Slot34SetUserProp(int Index, const char* Key, const char* Value) override
        {
            return Scene.SetUserProp(Index, Key, Value);
        }
        GENRESULT NEURONCOMPONENTS_API Slot35AddEntity(void* Desc, int* OutIndex) override
        {
            return Scene.AddEntityFromDesc(Desc, OutIndex);
        }
        GENRESULT NEURONCOMPONENTS_API Slot36RemoveEntity(int Index) override { return Scene.RemoveEntityAt(Index); }
        GENRESULT NEURONCOMPONENTS_API Slot37UpdateEntity(int Index, void* Desc) override
        {
            return Scene.UpdateEntityFromDesc(Index, Desc);
        }
        GENRESULT NEURONCOMPONENTS_API Slot38AddEvent(const void* Desc, int* OutId) override
        {
            return Scene.AddEventFromDesc(Desc, OutId);
        }
        GENRESULT NEURONCOMPONENTS_API Slot39RemoveEvent(int Id) override { return Scene.RemoveEventById(Id); }
        GENRESULT NEURONCOMPONENTS_API Slot40SetCom(ISceneScriptComContext*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot41SetLoadParser(ISceneScriptLoadParser*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot42SetSaveParser(ISceneScriptSaveParser*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot43LoadPath(const char*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot44SavePath(const char*) override { return GR_GENERIC; }

    private:
        FSceneScriptScene& Scene;
    };

    void Flush(oracle::Suite& Out)
    {
        for (const std::string& Line : GCalls)
            Out.Line(Line);
        GCalls.clear();
    }

    // FScriptEngine::Slot4Load after the parser: state, duration and sort, then the
    // default SET_CAMERA event for one monitor and one camera.
    void FinishLoad(FSceneScriptScene& Scene, FPlaybackEngine& Engine, unsigned int Duration)
    {
        Scene.MarkLoaded();
        Scene.FinalizeLoad(static_cast<int>(Duration));
        int MonitorCount = 0, CameraCount = 0, MonitorIndex = 0, CameraIndex = 0;
        for (unsigned long Index = 0; Index < Scene.EntityCount(); ++Index)
        {
            const int Handle = static_cast<int>(reinterpret_cast<INT_PTR>(Engine.Slot7EntityHandle(static_cast<int>(Index))));
            const FSceneScriptEntity* Entity = Scene.EntityAt(Handle);
            if (Entity->Snap.Type == kSceneScriptEntityMonitor)
            {
                ++MonitorCount;
                MonitorIndex = static_cast<int>(Index);
            }
            else if (Entity->Snap.Type == kSceneScriptEntityCamera)
            {
                ++CameraCount;
                CameraIndex = static_cast<int>(Index);
            }
        }
        if (MonitorCount == 1 && CameraCount == 1 && !Scene.HasEventType(kSceneScriptTypeSetCamera))
        {
            int Desc[11] = {};
            Desc[0] = kSceneScriptTypeSetCamera;
            Desc[5] = -1;
            Desc[7] = MonitorIndex;
            Desc[8] = CameraIndex;
            FSceneScriptEvent* Temporary = new FSceneScriptEvent(kSceneScriptTypeSetCamera, Desc);
            int EventId;
            Engine.Slot38AddEvent(&Temporary->SetCameraDesc, &EventId);
        }
    }

    void PlaybackSuite()
    {
        oracle::Suite Out("scene_script_playback.jsonl");
        unsigned int Case = 0;
        std::vector<FStubObject*> Stubs;
        for (const std::string& Line : oracle::InputLines("scene_script_playback.list"))
        {
            const std::vector<std::string> Fields = Split(Line, ' ');
            const oracle::Bytes Chunk = oracle::Unhex(Fields[2]);
            FSceneScriptScene* Scene = new FSceneScriptScene();
            FPlaybackEngine Engine(*Scene);
            unsigned int Duration = 0;
            const GENRESULT Status = SceneScriptOracleLoad(Chunk.data(), Fields[2].size() / 2, &Engine, &Duration);
            if (Status < 0)
            {
                Out.Line(Format("{\"case\":\"%u\",\"load\":\"-1\"}", Case));
                ++Case;
                continue;
            }
            FinishLoad(*Scene, Engine, Duration);
            Out.Line(Format("{\"case\":\"%u\",\"load\":\"0\",\"duration\":\"%d\",\"entities\":\"%lu\",\"events\":\"%lu\"}",
                            Case, Scene->DurationMs(), Scene->LiveEntityCount(), Scene->EventCount()));
            for (unsigned long Index = 0; Index < Scene->EventCount(); ++Index)
            {
                const FSceneScriptEventDesc* Desc =
                    static_cast<const FSceneScriptEventDesc*>(Scene->EventDescByIndex(static_cast<int>(Index)));
                Out.Line(Format("{\"case\":\"%u\",\"event\":\"%lu\",\"type\":\"%d\",\"start\":\"%d\",\"primary\":\"%d\","
                                "\"secondary\":\"%d\"}",
                                Case, Index, Desc->Type, Desc->StartMs, Desc->PrimaryIndex, Desc->SecondaryIndex));
            }
            // FScriptEngine's cached inverse of the scene frame: the identity at
            // +0 until the frame first moves.
            float CachedInverse[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
            auto AttachOne = [&](const std::string& Spec) {
                const std::vector<std::string> Parts = Split(Spec, ':');
                const int Handle = std::atoi(Parts[0].c_str());
                FStubObject* Stub = new FStubObject(Handle, ParseHex(Parts[1]));
                Stubs.push_back(Stub);
                const GENRESULT R = Scene->Attach(Handle, static_cast<INeuronComponent*>(Stub));
                return R;
            };
            if (Fields[0] != "-")
            {
                for (const std::string& Spec : Split(Fields[0], ','))
                {
                    const GENRESULT R = AttachOne(Spec);
                    Out.Line(Format("{\"case\":\"%u\",\"attach\":\"%d\",\"r\":\"%d\"}", Case,
                                    std::atoi(Spec.c_str()), R));
                }
            }
            for (const std::string& Op : Split(Fields[1], ','))
            {
                GENRESULT R = GR_OK;
                const std::string Arg = Op.substr(1);
                switch (Op[0])
                {
                case 's': R = Scene->StartPlayback(std::atoi(Arg.c_str())); break;
                case 't': R = Scene->Tick(std::atoi(Arg.c_str())); break;
                case 'x': R = Scene->StopPlayback(); break;
                case 'p': R = Scene->PausePlayback(); break;
                case 'l': Scene->SetLoop(static_cast<unsigned char>(std::atoi(Arg.c_str()))); break;
                case 'd': R = Scene->Detach(std::atoi(Arg.c_str())); break;
                case 'a': R = AttachOne(Arg); break;
                case 'r': R = Scene->RemoveEntityAt(std::atoi(Arg.c_str())); break;
                case 'P':
                case 'R':
                {
                    // FScriptEngine's IGeoTransformControl: the new frame, its cached
                    // inverse, then the bound entities follow.
                    std::vector<float> Words;
                    for (const std::string& Word : Split(Arg, '/'))
                        Words.push_back(HexFloat(Word));
                    FTransform Old;
                    std::memcpy(&Old, CachedInverse, sizeof(Old));
                    if (Op[0] == 'P')
                        std::memcpy(Scene->ForwardCsys().Pos, Words.data(), 3 * sizeof(float));
                    else
                        std::memcpy(Scene->ForwardCsys().Orient, Words.data(), 9 * sizeof(float));
                    Scene->BuildInverseCsys(CachedInverse);
                    Scene->PushBoundGeoFromScene(Old, Op[0] == 'R');
                    break;
                }
                default:
                    fprintf(stderr, "scene_script_playback: bad op %s\n", Op.c_str());
                    oracle::GFailed = true;
                    break;
                }
                Flush(Out);
                Out.Line(Format("{\"case\":\"%u\",\"op\":\"%s\",\"r\":\"%d\",\"clock\":\"%d\",\"state\":\"%d\"}", Case,
                                Op.c_str(), R, Scene->ClockMs(), Scene->PlaybackState()));
            }
            for (unsigned long Ordinal = 0;; ++Ordinal)
            {
                const int Handle = static_cast<int>(reinterpret_cast<INT_PTR>(Scene->EntityHandle(static_cast<int>(Ordinal))));
                if (Handle == -1)
                    break;
                const FSceneScriptEntitySnap* Snap = Scene->EntitySnap(Handle);
                float World[12];
                Scene->GetEntityPos(Handle, World + 9);
                Scene->GetEntityOrient(Handle, World);
                Out.Line(Format("{\"case\":\"%u\",\"entity\":\"%d\",\"pos\":\"", Case, Handle) + Ws(Snap->Pos, 3) +
                         "\",\"orient\":\"" + Ws(Snap->Orient, 9) + "\",\"world\":\"" + Ws(World + 9, 3) + " " +
                         Ws(World, 9) + "\"}");
            }
            Flush(Out);
            ++Case;
        }
    }
}

void SceneScriptRuntimeSuites()
{
    scene_script_runtime::CurveSuite();
    scene_script_runtime::SplineSuite();
    scene_script_runtime::PlaybackSuite();
}
