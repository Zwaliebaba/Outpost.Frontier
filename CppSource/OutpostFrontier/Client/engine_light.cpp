// Author: Giperion.

#include "engine_light.h"

#include "common_csys.h"
#include "common_ini.h"
#include "common_misc.h"
#include "game_options.h"
#include "NeuronRuntime.h"
#include "neuroncomponents.h"
#include "globals.h"
#include "room_camera.h" // LightsCameraMath (the field)
#include "x86math.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <map>
#include <new>
#include <windows.h>

namespace
{
    char const* const kEngineLightCpp = "Client\\cmn\\EngineLight.cpp";

    // .
    float const kPi = 3.1415927f;

    // — pipeline state 7, latched once on the first light. -1 means "not asked
    // yet"; baseline pre-sets 1 so a failed query leaves the "no hardware lights" answer.
    int G_PipelineLightState = -1;

    void QueryPipelineLightState()
    {
        // `if (the field == -1) { the field = 1;
        // Pipeline->slot6(7, &it); }` -- no test of the pipeline or its slot.
        if (G_PipelineLightState != -1)
            return;
        G_PipelineLightState = 1;
        NeuronRuntime::Pipeline->GetPipelineState(7, &G_PipelineLightState);
    }

    // The tail every setter shares: when the pipeline owns hardware lights and this one is
    // already registered, push the D3DLIGHT8 through slot 75.
    // The tail the routine / the routine and the other setters share (e.g. 0x00410f2c..
    // ): the field == 0 and registered (Flags bit 2) -> Pipeline vt+0x12c (slot 75)
    // with &D3d (+0xc). baseline tests nothing else.
    void PushLightUpdate(FEngineLight* Light)
    {
        if (G_PipelineLightState != 0 || (Light->Flags & 4u) == 0)
            return;
        NeuronRuntime::Pipeline->UpdateLight(&Light->D3d);
    }

    void StoreType(FEngineLight* Light, int LightType, bool bAlsoD3dType)
    {
        // switchD_00410d14: 1/2/3 also write the D3DLIGHT8 type; -2 keeps itself; the rest
        // collapse to -1.
        switch (LightType)
        {
        case 1:
        case 2:
        case 3:
            if (bAlsoD3dType)
                Light->D3d.Type = static_cast<unsigned long>(LightType);
            Light->Type = LightType;
            break;
        case -2:
            Light->Type = LightType;
            break;
        default:
            Light->Type = -1;
            break;
        }
    }

    // /e4/e8 — global intrusive list of registered lights (head/tail/count),
    // linked through FEngineLight::Field04 (next) / Field08 (prev). Insert via
    // LinkIntoRegisteredList (the routine bit4 arm).
    FEngineLight* G_LightListHead = nullptr;
    FEngineLight* G_LightListTail = nullptr;
    int G_LightListCount = 0;

    // — soft-light 3DMathEngine cache (the routine).
    I3DMathEngine* G_SoftLightMath = nullptr;

    FEngineLight* ListNext(FEngineLight const* L) { return reinterpret_cast<FEngineLight*>(static_cast<uintptr_t>(L->Field04)); }
    FEngineLight* ListPrev(FEngineLight const* L) { return reinterpret_cast<FEngineLight*>(static_cast<uintptr_t>(L->Field08)); }
    void SetListNext(FEngineLight* L, FEngineLight* V) { L->Field04 = static_cast<unsigned>(reinterpret_cast<uintptr_t>(V)); }
    void SetListPrev(FEngineLight* L, FEngineLight* V) { L->Field08 = static_cast<unsigned>(reinterpret_cast<uintptr_t>(V)); }

    I3DMathEngine* AcquireSoftLightMath()
    {
        if (G_SoftLightMath == nullptr)
            G_SoftLightMath = Acquire3DMathEngine();
        return G_SoftLightMath;
    }

    void SoftCameraRewriteLight(FEngineLight* Light, ICamera* Camera, float const* DirWorld)
    {
        // soft arm 0x004106ba..0x00410771 (the field != 0). Nothing is tested
        // but the type; each arm fetches the view transform (slot 16) and then the field
        // (filled by the routine when null).
        // not directional -> the D3D position is the light position
        // (+0x98) in camera space (TransformXform +0x28), z negated.
        if (Light->Type != 3)
        {
            FTransform const* const View =
                reinterpret_cast<FTransform const*>(Camera->GetViewTransform());
            I3DMathEngine* const Math = AcquireSoftLightMath();
            FVector CamSpace;
            Math->TransformXform(CamSpace, *View,
                                 *reinterpret_cast<FVector const*>(Light->Position));
            Light->D3d.Position[0] = CamSpace.X;
            Light->D3d.Position[1] = CamSpace.Y;
            Light->D3d.Position[2] = -CamSpace.Z;
        }

        // not a point light -> the D3D direction is the world direction
        // rotated into camera space (Rotate +0x30), z negated.
        if (Light->Type != 1)
        {
            FTransform const* const View =
                reinterpret_cast<FTransform const*>(Camera->GetViewTransform());
            I3DMathEngine* const Math = AcquireSoftLightMath();
            FVector CamDir;
            Math->Rotate(CamDir, *View, *reinterpret_cast<FVector const*>(DirWorld));
            Light->D3d.Direction[0] = CamDir.X;
            Light->D3d.Direction[1] = CamDir.Y;
            Light->D3d.Direction[2] = -CamDir.Z;
        }

        // pipeline slot 38 (+0x98, SetLight) with (&D3d, &D3d).
        NeuronRuntime::Pipeline->SetLight(&Light->D3d, (const D3DLIGHT8*)&Light->D3d);
    }

    // Shared by the destructor (the routine/ba0) and EngineLightUnregister (the routine):
    // unlink from the global light list and notify the pipeline (vtable slot 41 — a
    // RemoveLight-shaped call taking the D3DLIGHT8 pointer plus a trailing 0).
    void UnlinkFromList(FEngineLight* Light)
    {
        FEngineLight* const Next = ListNext(Light);
        FEngineLight* const Prev = ListPrev(Light);
        if (Prev == nullptr) G_LightListHead = Next; else SetListNext(Prev, Next);
        if (Next == nullptr) G_LightListTail = Prev; else SetListPrev(Next, Prev);
        --G_LightListCount;

        // PUSH 0, PUSH &D3d, PUSH this -- args are (Handle, Enable).
        // Unconditional in baseline: no pipeline/slot null test.
        NeuronRuntime::Pipeline->SetLightEnable(&Light->D3d, 0);
    }

    // Shared body of the routine (base dtor) and the routine (scalar deleting dtor, which is
    // this plus an optional operator-delete on a flag bit the port doesn't need since callers
    // always go through EngineLightDestroy).
    void DestructLightBody(FEngineLight* Light)
    {
        unsigned char const Was = Light->Flags;
        // / 00410bab: reset to the base vtable during destruction.
        if ((Was & 4u) != 0) // registered
        {
            if ((Was & 0x10u) != 0) // linked into the global list
            {
                Light->Flags = static_cast<unsigned char>(Light->Flags & 0xefu);
                UnlinkFromList(Light);
            }

            // Both branches take the D3DLIGHT8, pushed before the branch: 00410b71 LEA
            // EDX,[ESI+0xc]; 00410b74 PUSH EDX; 00410b75 PUSH EAX; 00410b76 JNZ; 00410b78 CALL
            // [ECX+0x128] (slot 74) / 00410b87 CALL [ECX+0x9c] (slot 39). No test of the
            // pipeline or of either slot.
            if (G_PipelineLightState == 0)
                NeuronRuntime::Pipeline->RemoveLight(&Light->D3d);
            else
                NeuronRuntime::Pipeline->DestroyLight(&Light->D3d);
            Light->Flags = static_cast<unsigned char>(Light->Flags & 0xfbu); // clear "registered"
        }
    }

    void SetColorFromBytes(FEngineLight* Light, unsigned char const RGB[3])
    {
        // / the routine — byte-identical bodies in baseline.
        Light->D3d.Diffuse[3] = 1.f;
        Light->Color[0] = static_cast<float>(RGB[0]) * (1.f / 255.f);
        Light->Color[1] = static_cast<float>(RGB[1]) * (1.f / 255.f);
        Light->Color[2] = static_cast<float>(RGB[2]) * (1.f / 255.f);
        Light->D3d.Diffuse[0] = Light->Color[0];
        Light->D3d.Diffuse[1] = Light->Color[1];
        Light->D3d.Diffuse[2] = Light->Color[2];
        PushLightUpdate(Light);
    }
}

void EngineLightInit(FEngineLight* Light, int LightType)
{
    ::new (Light) FEngineLight(LightType);
}

FEngineLight::FEngineLight(int LightType)
{
    // (thiscall, RET 4), listing-traced 0x00410ca0..0x00410db4. No null test, and
    // only the fields below are written: +0x04 / +0x08 (the list links) are left as they are.
    // the 3x3 at +0x74 made identity.
    this->Orient[7] = 0.f;
    this->Orient[6] = 0.f;
    this->Orient[5] = 0.f;
    this->Orient[3] = 0.f;
    this->Orient[2] = 0.f;
    this->Orient[1] = 0.f;
    this->Orient[8] = 1.f;
    this->Orient[4] = 1.f;
    this->Orient[0] = 1.f;
    // the position zeroed; the dword at +0xb4 zeroed, then its low
    // byte stored again with bit 0 cleared.
    this->Position[2] = 0.f;
    this->Position[1] = 0.f;
    this->Position[0] = 0.f;
    this->Flags = 0;
    this->PadB5[0] = 0;
    this->PadB5[1] = 0;
    this->PadB5[2] = 0;
    // the base vtable the routine.

    // the type switch (types 1..3 also write +0xc). It runs before the
    // STOSD below, which zeroes that +0xc write again; only +0xB0 survives.
    StoreType(this, LightType, true);

    // the field latched on the first light.
    QueryPipelineLightState();

    // +0x0c..+0x73 (the D3DLIGHT8, 0x1a dwords) zeroed; colour 1,1,1;
    // attenuation1 (+0x64) 1.0; the three curves and the period (+0xb8..+0xc4) zeroed.
    memset(&this->D3d, 0, sizeof(this->D3d));
    this->Color[0] = 1.f;
    this->Color[1] = 1.f;
    this->Color[2] = 1.f;
    this->D3d.Attenuation1 = 1.f;
    this->AmbientCurve = nullptr;
    this->ZoneCurve = nullptr;
    this->ColorCurve = nullptr;
    this->ColorCurvePeriod = 0.f;
}

void EngineLightSetLight(FEngineLight* Light, FD3dLight8 const* Source)
{
    // (module storage-004112d0), thiscall RET 4. No null tests.
    unsigned char const Was = Light->Flags;
    Light->Flags = static_cast<unsigned char>(Was | 0x20u);
    if ((Was & 4u) == 0)
    {
        // Not registered yet: the incoming type still gets to set both fields.
        Light->Flags = static_cast<unsigned char>((Was & 0xfeu) | 0x20u);
        StoreType(Light, static_cast<int>(Source->Type), true);
    }

    // src[1..0x0C] -> +0x10..+0x3C: Diffuse, Specular, Ambient.
    memcpy(Light->D3d.Diffuse, Source->Diffuse, sizeof(float) * 4);
    memcpy(Light->D3d.Specular, Source->Specular, sizeof(float) * 4);
    memcpy(Light->D3d.Ambient, Source->Ambient, sizeof(float) * 4);
    // src[0x0D..0x12] (Position / Direction) is skipped — the routine and the routine own it.
    // src[0x13..0x19] -> +0x58..+0x70.
    Light->D3d.Range = Source->Range;
    Light->D3d.Falloff = Source->Falloff;
    Light->D3d.Attenuation0 = Source->Attenuation0;
    Light->D3d.Attenuation1 = Source->Attenuation1;
    Light->D3d.Attenuation2 = Source->Attenuation2;
    Light->D3d.Theta = Source->Theta;
    Light->D3d.Phi = Source->Phi;

    Light->Color[2] = Light->D3d.Diffuse[2];
    Light->Color[0] = Light->D3d.Diffuse[0];
    Light->Color[1] = Light->D3d.Diffuse[1];

    if (Light->D3d.Type != 3)
    {
        // FCOMP the field (pi); TEST AH,0x41; JNZ skips when Phi <= pi or
        // unordered -- the fall-through (strictly greater) warns and clamps.
        if (Light->D3d.Phi > kPi)
        {
            FDUMP(0x100002, "%s(%d) : WARNING:General:%s", kEngineLightCpp, 0x23b,
                  "invalid light detected: phi > pi");
            Light->D3d.Phi = kPi;
        }
        // each attenuation FCOMP 0.0 (the field); TEST AH,0x41; JP
        // leaves when the parity is even, i.e. greater (C3=C0=0) or unordered (C3=C0=1).
        // The fall-through is less or equal, so the warning fires when all three are <= 0,
        // not only when all three are exactly zero.
        if (Light->D3d.Attenuation0 <= 0.f && Light->D3d.Attenuation1 <= 0.f &&
            Light->D3d.Attenuation2 <= 0.f)
        {
            FDUMP(0x100002, "%s(%d) : WARNING:General:%s", kEngineLightCpp, 0x241,
                  "invalid light detected: atten all zero");
            Light->D3d.Attenuation0 = 0.1f;
        }
    }

    PushLightUpdate(Light);
}

void EngineLightSetPosition(FEngineLight* Light, float const* Pos3)
{
    // (module storage-00410f52), thiscall RET 4: copy to +0x98; a non-directional
    // light (type +0xb0 != 3) also gets D3D Position (+0x40) = {x, y, -z}; then the pipeline
    // update. No null tests.
    Light->Position[0] = Pos3[0];
    Light->Position[1] = Pos3[1];
    Light->Position[2] = Pos3[2];

    // A directional light has no position; everything else gets it with Z negated.
    if (Light->Type != 3)
    {
        Light->D3d.Position[0] = Light->Position[0];
        Light->D3d.Position[1] = Light->Position[1];
        Light->D3d.Position[2] = -Light->Position[2];
    }

    PushLightUpdate(Light);
}

void EngineLightSetOrientation(FEngineLight* Light, float const* Orient9)
{
    // (module storage-00410fd2), thiscall RET 4: REP MOVSD 9 dwords to +0x74; for
    // anything but a point light (type +0xb0 != 1) the routine(&out, 0, 0) and D3D Direction
    // (+0x4c) = {out.x, out.y, -out.z}; then the pipeline update. No null tests.
    memcpy(Light->Orient, Orient9, sizeof(float) * 9);

    if (Light->Type != 1)
    {
        float Dir[3];
        EngineLightComputeDirection(Light, Dir, nullptr, nullptr);
        Light->D3d.Direction[0] = Dir[0];
        Light->D3d.Direction[1] = Dir[1];
        Light->D3d.Direction[2] = -Dir[2];
    }

    PushLightUpdate(Light);
}

bool EngineLightRegister(FEngineLight* Light)
{
    // (thiscall), listing-traced 0x00410dd0..0x00410e16: not yet registered (bit 2)
    // -> set it, and with the field == 0 pipeline slot 73 (+0x124) on the D3DLIGHT8; then
    // bit 3 set and returned ((flags >> 3) & 1, always 1). No test of the light or the pipeline.
    unsigned char const Was = Light->Flags;
    if ((Was & 4u) == 0)
    {
        Light->Flags = static_cast<unsigned char>(Was | 4u);
        if (G_PipelineLightState == 0)
            NeuronRuntime::Pipeline->AddLight(&Light->D3d);
    }
    Light->Flags = static_cast<unsigned char>(Light->Flags | 8u);
    return ((Light->Flags >> 3) & 1u) != 0;
}

FEngineLight* EngineLightCreate(float const* Frame12, FD3dLight8 const* Source,
                                float const* Direction3)
{
    // operator new(0xc8) and `PUSH -1; CALL the routine` -- the ctor
    // always gets -1, never the source type. No test of any argument anywhere below.
    FEngineLight* Light = static_cast<FEngineLight*>(::operator new(sizeof(FEngineLight)));
    EngineLightInit(Light, -1);

    // the caller's 0x1A dwords are copied to a local first, and a point
    // light has theta and phi forced to pi (0x40490fdb) before the routine sees them.
    FD3dLight8 Local = *Source;
    if (Local.Type == 1)
    {
        Local.Theta = kPi;
        Local.Phi = kPi;
    }

    // the routine; 0x0055708f the routine with the frame's position (+0x00).
    EngineLightSetLight(Light, &Local);
    EngineLightSetPosition(Light, Frame12);

    // the field tested and filled (the routine), then
    // TransformMatrix (+0x2c) turns Direction3 by the frame's orientation (Frame12 + 3, the 3x3
    // after the position); 0x005570be..0x005570f1: COMMON.DLL::LookMatrixYup on that vector and
    // with the result.
    I3DMathEngine* const Math = LightsCameraMath();
    FVector Rotated;
    Math->TransformMatrix(Rotated, *reinterpret_cast<FMatrix const*>(Frame12 + 3),
                          *reinterpret_cast<FVector const*>(Direction3));
    Vector Dir;
    Dir.x = Rotated.X;
    Dir.y = Rotated.Y;
    Dir.z = Rotated.Z;
    const Matrix Look = LookMatrixYup(Dir);
    EngineLightSetOrientation(Light, &Look.E[0][0]);

    // vtable slot 1 of the plain EngineLight table the routine,
    // i.e. the routine (register with the pipeline). This is not a GameLight: it is never on
    // the the field list and has no +0xc8..+0xd0 fields.
    Light->Enable();
    return Light;
}

// The two address computations the routine inlines (LEA of props + 0x28 / + 0x0c,
// ); no test.
FD3dLight8 const* SceneScriptLightPropsD3d(float const* Props36)
{
    return reinterpret_cast<FD3dLight8 const*>(reinterpret_cast<char const*>(Props36) + 0x28);
}

float const* SceneScriptLightPropsTarget(float const* Props36)
{
    return Props36 + 3; // +0x0C
}

void EngineLightDestroy(FEngineLight* Light)
{
    // calls vtable slot 0 with 1 (the deleting destructor, the routine); its
    // null test (0x0055713e) lives in the caller GfLightWrapperDestroy.
    EngineLightDelete(Light);
}

void EngineLightDelete(FEngineLight* Light)
{
    // via vtable slot 0, with the scalar-delete flag.
    delete Light;
}

void EngineLightUnregister(FEngineLight* Light)
{
    // (thiscall), listing-traced 0x00410e20..0x00410ebf: bit 3 cleared; linked
    // (bit 4) -> bit 4 cleared, unlinked (the relink arm behind the second TEST AL,0x10 is dead:
    // the bit was just cleared), count - 1, pipeline slot 41 (+0xa4) with (&D3D, 0). No null
    // test.
    unsigned char const Was = Light->Flags;
    Light->Flags = static_cast<unsigned char>(Was & 0xf7u); // clear "enabled"
    if ((Was & 0x10u) != 0)
    {
        Light->Flags = static_cast<unsigned char>(Light->Flags & 0xefu);
        UnlinkFromList(Light);
    }
}

void EngineLightPushUpdate(FEngineLight* Light)
{
    // (thiscall), 0x00410ec0..0x00410ee6: the field == 0 and bit 2 set ->
    // pipeline slot 75 (+0x12c) on the D3DLIGHT8. No null test.
    PushLightUpdate(Light);
}

void EngineLightSetTransform(FEngineLight* Light, float const* Orient9, float const* Pos3)
{
    // (module storage-004110b0), thiscall RET 4 on one {Orient[9], Pos[3]} block:
    // is the routine's body (Pos from block+0x24, D3D position unless
    // type 3, push), 0041104a..004110a4 is the routine's (REP MOVSD 9 dwords, direction via
    // (&out, 0, 0) unless type 1, push). Same order, same two pushes. No null tests.
    EngineLightSetPosition(Light, Pos3);
    EngineLightSetOrientation(Light, Orient9);
}

void EngineLightSetType(FEngineLight* Light, int LightType, bool Bit0)
{
    // (module storage-00411104), thiscall RET 8: `((f ^ b) & 1) ^ f` puts the
    // argument byte's bit 0 into Flags bit 0, then switchD_004110e7 (types 1/2/3 write +0xc and
    // +0xb0, -2 writes +0xb0, the rest +0xb0 = -1). No null test.
    Light->Flags = static_cast<unsigned char>((Light->Flags & 0xfeu) | (Bit0 ? 1u : 0u));
    StoreType(Light, LightType, true);
}

void EngineLightSetRangeRaw(FEngineLight* Light, float Range)
{
    Light->D3d.Range = Range; // MOV [ECX+0x58],EAX; RET 4. No push, no null test.
}

void EngineLightSetRadius(FEngineLight* Light, float Radius)
{
    // (module storage-00411350), thiscall RET 4: Range = r, Att0 = 1, Att1 = 0,
    // Att2 = 16 / (r * r) -- FLD/FMUL keep r*r on the FPU stack, FDIVR the field (16.0f),
    // one FSTP float at the end. Then the shared push. No null test.
    Light->D3d.Range = Radius;
    Light->D3d.Attenuation0 = 1.f;
    Light->D3d.Attenuation1 = 0.f;
    double const RadiusSq = static_cast<double>(Radius) * static_cast<double>(Radius);
    Light->D3d.Attenuation2 = static_cast<float>(16.0 / RadiusSq);
    PushLightUpdate(Light);
}

void EngineLightSetDirection(FEngineLight* Light, float const* Dir3)
{
    // (thiscall, RET 4), listing-traced 0x00411470..0x004114e6: non-point lights
    // (+0xb0 != 1) get the D3DLIGHT8 direction (x, y copied, z negated); every type gets
    // Orient[2] / [5] / [8] = -x / -y / -z; then the shared push. No null test.
    if (Light->Type != 1)
    {
        Light->D3d.Direction[0] = Dir3[0];
        Light->D3d.Direction[1] = Dir3[1];
        Light->D3d.Direction[2] = -Dir3[2];
    }
    Light->Orient[2] = -Dir3[0];
    Light->Orient[5] = -Dir3[1];
    Light->Orient[8] = -Dir3[2];

    PushLightUpdate(Light);
}

void EngineLightSetColorBytes(FEngineLight* Light, unsigned char const RGB[3])
{
    // (thiscall, RET 4): no test of the light or the byte pointer.
    SetColorFromBytes(Light, RGB);
}

void EngineLightSetColorBytesAlt(FEngineLight* Light, unsigned char const RGB[3])
{
    // (thiscall, RET 4): the same body; no tests.
    SetColorFromBytes(Light, RGB);
}

void EngineLightSetConeAngle(FEngineLight* Light, float DegreesFullAngle)
{
    // (thiscall, RET 4) — no test of the light, and deliberately no
    // PushLightUpdate call, matching baseline. Theta = Phi * 0.5 is exact either way.
    Light->D3d.Falloff = 1.f;
    Light->D3d.Phi = DegreesFullAngle * (kPi / 180.f);
    Light->D3d.Theta = Light->D3d.Phi * 0.5f;
}

double Vec3Length(float const* V)
{
    // (module storage-004101b0): FLD z, y, x; x*x + y*y, + z*z; FSQRT; the result
    // stays in ST0.
    return sqrt((static_cast<double>(V[0]) * V[0] + static_cast<double>(V[1]) * V[1]) +
                static_cast<double>(V[2]) * V[2]);
}

void Vec3Negate(float* Out, float const* In)
{
    // (module storage-0041136a), thiscall(In) RET 4 (Out): the three FCHS'd loads
    // happen before the stores; no null tests.
    Out[0] = -In[0];
    Out[1] = -In[1];
    Out[2] = -In[2];
}

void ScaleVec3(float* Out, float const* V, float Scalar)
{
    // three FMULs then the three stores, no null test (every caller passes
    // locals). Out may alias V, as texture_panels.cpp's second call does: all loads come first.
    Out[0] = Scalar * V[0];
    Out[1] = Scalar * V[1];
    Out[2] = Scalar * V[2];
}

void PackNibbleByte(unsigned int* Out, int Hi, unsigned int Lo)
{
    // (thiscall on Out, RET 8), 0x00411c50..0x00411c72: H = Hi << 4;
    // *Out = ((H ^ Lo) & 0xf) ^ H, i.e. (Lo & 0xf) ^ H (H's low nibble is 0). No null test.
    unsigned int const Shifted = static_cast<unsigned int>(Hi) << 4;
    *Out = ((Shifted ^ Lo) & 0xfu) ^ Shifted;
}

unsigned char* EngineLightPackColorBytes(unsigned char* Out4, float const* Rgb3)
{
    // — __thiscall on the 4-byte output, RET 4. Each channel is _ftol'd and clamped
    // to [0, 0xFF]; baseline converts index 2, then 1, then 0, and stores them back in place, so
    // Out4[i] comes from Rgb3[i]. Alpha is 0xFF.
    int Blue = static_cast<int>(Rgb3[2]);
    if (Blue < 0)
        Blue = 0;
    else if (Blue > 0xff)
        Blue = 0xff;
    int Green = static_cast<int>(Rgb3[1]);
    if (Green < 0)
        Green = 0;
    else if (Green > 0xff)
        Green = 0xff;
    int Red = static_cast<int>(Rgb3[0]);
    if (Red < 0)
        Red = 0;
    else if (Red > 0xff)
        Red = 0xff;
    Out4[0] = static_cast<unsigned char>(Red);
    Out4[2] = static_cast<unsigned char>(Blue);
    Out4[1] = static_cast<unsigned char>(Green);
    Out4[3] = 0xff;
    return Out4;
}

float* EngineLightUnpackColorBytes(unsigned char const* Bytes3, float* OutRgb3)
{
    // — __thiscall on the bytes, RET 4: FILD byte / FMUL the field (1/255f).
    OutRgb3[0] = static_cast<float>(Bytes3[0]) * (1.f / 255.f);
    OutRgb3[1] = static_cast<float>(Bytes3[1]) * (1.f / 255.f);
    OutRgb3[2] = static_cast<float>(Bytes3[2]) * (1.f / 255.f);
    return OutRgb3;
}

int EngineLightParseType(char const* Text)
{
    // .
    if (_stricmp(Text, "AMBIENT") == 0)
        return -2;
    if (_stricmp(Text, "POINT") == 0)
        return 1;
    if (_stricmp(Text, "CONE") == 0)
        return 2;
    if (_stricmp(Text, "DIRECTIONAL") == 0)
        return 3;

    FDUMP(0x100001, "%s(%d) : *** ERROR: invalid [LightSource] type = '%s'", kEngineLightCpp, 0x2ec,
          Text);
    return -1;
}

void MatrixSetColumn2(float* Matrix9, float const* Vec3)
{
    // — __thiscall on the 3x3, RET 4.
    Matrix9[2] = Vec3[0];
    Matrix9[5] = Vec3[1];
    Matrix9[8] = Vec3[2];
}

FLightCurveName* LightCurveNameInit(FLightCurveName* Name, char const* Source)
{
    // — __thiscall, RET 4.
    Name->Len = 0;
    Name->Text[0] = '\0';
    if (Source == nullptr)
    {
        Name->Len = 0;
        Name->Text[0] = '\0';
        return Name;
    }
    unsigned Len = static_cast<unsigned>(strlen(Source));
    if (Len >= 0x80)
        Len = 0x7f;
    memcpy(Name->Text, Source, Len);
    Name->Text[Len] = '\0';
    Name->Len = Len;
    return Name;
}

void EngineLightSetAttenCurve(FEngineLight* Light, char const* GraphName)
{
    // — __thiscall, RET 4.
    if (Light->Type == -2)
    {
        // An ambient light takes the graph as its ambient curve instead.
        if (Light->AmbientCurve != nullptr)
        {
            Light->AmbientCurve->Release();
            Light->AmbientCurve = nullptr;
        }
        IGraph* const Graph = GraphLibraryFind(GraphName);
        if (Graph != nullptr)
        {
            Light->AmbientCurve = Graph;
            Light->Flags = static_cast<unsigned char>(Light->Flags | 2u);
            return;
        }
        MessageBoxA(nullptr, GraphName, "Error: unknown graph name", 0);
        return;
    }

    if (Light->ZoneCurve != nullptr)
    {
        Light->ZoneCurve->Release();
        Light->ZoneCurve = nullptr;
    }
    IGraph* const Graph = GraphLibraryFind(GraphName);
    if (Graph == nullptr)
    {
        MessageBoxA(nullptr, GraphName, "Error: unknown graph name", 0);
        return;
    }
    if (Light->Type != 1 && Light->Type != 2 && Light->Type != 3)
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: Invalid dynamic light type (%s)", kEngineLightCpp,
              0x360, GraphName);
    }
    Light->ZoneCurve = Graph;
    Light->Flags = static_cast<unsigned char>(Light->Flags | 2u);
}

void EngineLightSetColorCurve(FEngineLight* Light, char const* GraphName, float Period)
{
    // — __thiscall, RET 8.
    if (Light->ColorCurve != nullptr)
    {
        Light->ColorCurve->Release();
        Light->ColorCurve = nullptr;
    }
    IGraph* const Graph = GraphLibraryFind(GraphName);
    if (Graph == nullptr)
    {
        MessageBoxA(nullptr, GraphName, "Error: unknown graph name", 0);
        return;
    }
    Light->ColorCurve = Graph;
    Light->ColorCurvePeriod = Period;
    Light->Flags = static_cast<unsigned char>(Light->Flags | 2u);
}

bool EngineLightReadValue(FEngineLight* Light, char const* Name, char const* Value)
{
    // — __thiscall, RET 8. Every recognised key returns true; ValueNum fields are
    // read highest index first, as baseline does.
    if (_stricmp(Name, "pos") == 0)
    {
        float Pos[3];
        Pos[2] = static_cast<float>(ValueNum(Value, 2));
        Pos[1] = static_cast<float>(ValueNum(Value, 1));
        Pos[0] = static_cast<float>(ValueNum(Value, 0));
        Light->Position[0] = Pos[0];
        Light->Position[1] = Pos[1];
        Light->Position[2] = Pos[2];
        if (Light->Type != 3)
        {
            Light->D3d.Position[0] = Light->Position[0];
            Light->D3d.Position[1] = Light->Position[1];
            Light->D3d.Position[2] = -Light->Position[2];
        }
        PushLightUpdate(Light);
        return true;
    }

    if (_stricmp(Name, "range") == 0)
    {
        Light->D3d.Range = static_cast<float>(ValueNum(Value, 0));
        return true;
    }

    if (_stricmp(Name, "color") == 0)
    {
        float Rgb[3];
        Rgb[2] = static_cast<float>(ValueNum(Value, 2));
        Rgb[1] = static_cast<float>(ValueNum(Value, 1));
        Rgb[0] = static_cast<float>(ValueNum(Value, 0));
        // through a clamped byte quad and back, so the colour is quantised
        // to whole 1/255 steps.
        unsigned char Packed[4];
        EngineLightPackColorBytes(Packed, Rgb);
        float Color[3];
        float const* const Unpacked = EngineLightUnpackColorBytes(Packed, Color);
        Light->Color[0] = Unpacked[0];
        Light->Color[1] = Unpacked[1];
        Light->Color[2] = Unpacked[2];
        Light->D3d.Diffuse[0] = Light->Color[0];
        Light->D3d.Diffuse[1] = Light->Color[1];
        Light->D3d.Diffuse[2] = Light->Color[2];
        Light->D3d.Diffuse[3] = 1.0f;
        PushLightUpdate(Light);
        return true;
    }

    if (_stricmp(Name, "type") == 0)
    {
        // the routine and the routine(type, false) inlined; the FDUMP
        // flag word there comes out of the routine(0x10000, 1) = 0x100001, the same constant.
        EngineLightSetType(Light, EngineLightParseType(Value), false);
        return true;
    }

    if (_stricmp(Name, "attenuation") == 0)
    {
        float const Atten2 = static_cast<float>(ValueNum(Value, 2));
        float const Atten1 = static_cast<float>(ValueNum(Value, 1));
        Light->D3d.Attenuation0 = static_cast<float>(ValueNum(Value, 0));
        Light->D3d.Attenuation1 = Atten1;
        Light->D3d.Attenuation2 = Atten2;
        return true;
    }

    if (_stricmp(Name, "direction") == 0)
    {
        float Dir[3];
        Dir[2] = static_cast<float>(ValueNum(Value, 2));
        Dir[1] = static_cast<float>(ValueNum(Value, 1));
        Dir[0] = static_cast<float>(ValueNum(Value, 0));
        // (FSQRT of x*x + y*y + z*z, unrounded), FDIVR 1.0, then each component
        // scaled and stored back as a float.
        double const Length = sqrt(static_cast<double>(Dir[0]) * Dir[0] +
                                   static_cast<double>(Dir[1]) * Dir[1] +
                                   static_cast<double>(Dir[2]) * Dir[2]);
        double const InvLength = 1.0 / Length;
        Dir[0] = static_cast<float>(Dir[0] * InvLength);
        Dir[1] = static_cast<float>(Dir[1] * InvLength);
        Dir[2] = static_cast<float>(Dir[2] * InvLength);
        if (Light->Type != 1)
        {
            Light->D3d.Direction[0] = Dir[0];
            Light->D3d.Direction[1] = Dir[1];
            Light->D3d.Direction[2] = -Dir[2];
        }
        float Negated[3];
        Vec3Negate(Negated, Dir);
        MatrixSetColumn2(Light->Orient, Negated);  // on light +0x74
        PushLightUpdate(Light);
        return true;
    }

    if (_stricmp(Name, "ambient_curve") == 0)
    {
        if (Light->AmbientCurve != nullptr)
        {
            Light->AmbientCurve->Release();
            Light->AmbientCurve = nullptr;
        }
        IGraph* const Graph = GraphLibraryFind(Value);
        if (Graph != nullptr)
        {
            Light->AmbientCurve = Graph;
            Light->Flags = static_cast<unsigned char>(Light->Flags | 2u);
            return true;
        }
        MessageBoxA(nullptr, Value, "Error: unknown graph name", 0);
        return true;
    }

    if (_stricmp(Name, "atten_curve") == 0)
    {
        // "atten_curve = graph[, type]": an optional light type first, then the graph.
        char const* const TypeName = StringIndex(Value, 1);
        if (TypeName != nullptr && TypeName[0] != '\0')
            EngineLightSetType(Light, EngineLightParseType(TypeName), false);
        EngineLightSetAttenCurve(Light, StringIndex(Value, 0));
        return true;
    }

    if (_stricmp(Name, "color_curve") == 0)
    {
        FLightCurveName CurveName;
        LightCurveNameInit(&CurveName, StringIndex(Value, 0));
        float const Period = static_cast<float>(ValueNum(Value, 1));
        EngineLightSetColorCurve(Light, CurveName.Text, Period);
        return true;
    }

    return false;
}

bool EngineLightAffectsPoint(FEngineLight const* Light, float const* Point3, float ExtraRadius)
{
    // (thiscall, RET 8), listing-traced 0x00410a40..0x00410aad. No null test.
    // not enabled (bit 3) -> 0.
    if ((Light->Flags & 8u) == 0)
        return false;

    // a directional light (+0xb0 == 3) without bit 1 -> 1.
    if (Light->Type == 3 && (Light->Flags & 2u) == 0)
        return true;

    // the differences and the reach range (+0x58) + ExtraRadius stay on
    // the FPU; FCOMPP of reach^2 against (dx^2 + dz^2) + dy^2, TEST AH,0x41 / JNZ: 1 only when
    // reach^2 is strictly greater (ordered).
    double const Dx = static_cast<double>(Point3[0]) - Light->Position[0];
    double const Dy = static_cast<double>(Point3[1]) - Light->Position[1];
    double const Dz = static_cast<double>(Point3[2]) - Light->Position[2];
    double const Reach = static_cast<double>(Light->D3d.Range) + ExtraRadius;
    double const DistSq = (Dx * Dx + Dz * Dz) + Dy * Dy;
    return Reach * Reach > DistSq;
}

bool EngineLightGetEffectiveColor(FEngineLight* Light, float Dist, float* OutColor3)
{
    // , a double.
    double const kInv255 = 1.0 / 255.0;

    OutColor3[0] = Light->Color[0];
    OutColor3[1] = Light->Color[1];
    OutColor3[2] = Light->Color[2];

    bool bModified = false;
    if (Light->ColorCurve != nullptr)
    {
        // range {wrap, 0, ColorCurvePeriod}; only the flag byte of the
        // stack block is written, as in baseline.
        FGraphRange Period;
        Period.bClamp = 0;
        Period.Min = 0.0f;
        Period.Max = Light->ColorCurvePeriod;
        float const Now = static_cast<float>(GetClientElapsedTime()); // , FSTP float
        unsigned const Packed = Light->ColorCurve->EvaluateColor(Now, &Period);
        // FILD channel / FMUL double 1/255 / FMUL float out / FSTP float.
        OutColor3[0] = static_cast<float>(((Packed >> 0x10) & 0xffu) * kInv255 * OutColor3[0]);
        OutColor3[1] = static_cast<float>(((Packed >> 8) & 0xffu) * kInv255 * OutColor3[1]);
        OutColor3[2] = static_cast<float>((Packed & 0xffu) * kInv255 * OutColor3[2]);
        bModified = true;
    }

    if (Light->ZoneCurve != nullptr)
    {
        // range {clamp, 0, D3d.Range}, evaluated at Dist.
        FGraphRange Reach;
        Reach.bClamp = 1;
        Reach.Min = 0.0f;
        Reach.Max = Light->D3d.Range;
        // the unrounded result multiplies each channel (FLD ST0 / FMUL).
        double const Scale = Light->ZoneCurve->Evaluate(Dist, &Reach);
        OutColor3[0] = static_cast<float>(Scale * OutColor3[0]);
        OutColor3[1] = static_cast<float>(Scale * OutColor3[1]);
        OutColor3[2] = static_cast<float>(Scale * OutColor3[2]);
        bModified = true;
    }

    return bModified;
}

float* EngineLightComputeDirection(FEngineLight* Light, float* OutDir3, float const* Point,
                                   float* OutDist)
{
    // (module storage-00411468), thiscall RET 0xc. No null tests.
    int const Type = Light->Type;
    bool const bTowardPoint =
        (Type == 1) || (((Light->Flags & 2u) != 0) && (Type == 3 || Type == 2));

    if (Point != nullptr && (bTowardPoint || OutDist != nullptr))
    {
        // dx and dz stay on the FPU, dy is stored as a float (local_8);
        // the sum is (dz*dz + dx*dx) + dy*dy.
        double const Dx = static_cast<double>(Point[0]) - Light->Position[0];
        float const Dy = Point[1] - Light->Position[1];
        double const Dz = static_cast<double>(Point[2]) - Light->Position[2];
        double const Len = sqrt((Dz * Dz + Dx * Dx) + static_cast<double>(Dy) * Dy);
        if (OutDist != nullptr)
            *OutDist = static_cast<float>(Len); // FST float [EDX]
        if (bTowardPoint)
        {
            // FDIVR 1.0 (double the field) -- no zero test.
            double const Inv = 1.0 / Len;
            OutDir3[0] = static_cast<float>(Dx * Inv);
            OutDir3[1] = static_cast<float>(static_cast<double>(Dy) * Inv);
            OutDir3[2] = static_cast<float>(Dz * Inv);
            return OutDir3;
        }
    }

    // Default: negate Orient column 2 (light+0x7c / +0x88 / +0x94).
    OutDir3[0] = -Light->Orient[2];
    OutDir3[1] = -Light->Orient[5];
    OutDir3[2] = -Light->Orient[8];
    return OutDir3;
}

bool EngineLightSample(FEngineLight* Light, float const* Point, float* OutDir3,
                       float* InOutAmbient3, float* OutColor3)
{
    // . 00410966: TEST byte [+0xb4],0x8 / JZ -> return false.
    if ((Light->Flags & 8u) == 0)
        return false;

    // the routine(&Dir, Point, &Dist) -- baseline reuses the Point argument
    // slot for the distance -- and the returned direction copied out.
    float Dir[3];
    float Dist;
    float const* const Result = EngineLightComputeDirection(Light, Dir, Point, &Dist);
    OutDir3[0] = Result[0];
    OutDir3[1] = Result[1];
    OutDir3[2] = Result[2];

    // the routine(Dist, OutColor3); the return value is not read.
    EngineLightGetEffectiveColor(Light, Dist, OutColor3);

    // CMP dword [+0xb0],-2 / JNZ -> return true.
    if (Light->Type == -2)
    {
        // no AmbientCurve -> 1.0f; otherwise slot 6 at Dist over
        // {clamp, 0, D3d.Range}, the unrounded result kept on the FPU.
        double Scale = 1.0;
        if (Light->AmbientCurve != nullptr)
        {
            FGraphRange Reach;
            Reach.bClamp = 1;
            Reach.Min = 0.0f;
            Reach.Max = Light->D3d.Range;
            Scale = Light->AmbientCurve->Evaluate(Dist, &Reach);
        }
        // the three products (G and B stored as floats), each added in.
        float const AddR = static_cast<float>(Scale * OutColor3[0]);
        float const AddG = static_cast<float>(Scale * OutColor3[1]);
        float const AddB = static_cast<float>(Scale * OutColor3[2]);
        InOutAmbient3[0] = AddR + InOutAmbient3[0];
        InOutAmbient3[1] = AddG + InOutAmbient3[1];
        InOutAmbient3[2] = AddB + InOutAmbient3[2];
    }
    return true;
}

void EngineLightGetAmbientRgbBytes(unsigned char* OutRgb)
{
    // .
    if (OutRgb == nullptr)
        return;
    int Packed;
    NeuronRuntime::Pipeline->GetRenderState(D3DRS_AMBIENT, &Packed);
    unsigned const Bits = static_cast<unsigned>(Packed);
    OutRgb[0] = static_cast<unsigned char>(Bits >> 0x10);
    OutRgb[1] = static_cast<unsigned char>(Bits >> 8);
    OutRgb[2] = static_cast<unsigned char>(Bits);
}

namespace
{
    void LinkIntoRegisteredList(FEngineLight* Light)
    {
        // list-insert arm (0041078e..004107c4): append at tail.
        FEngineLight* const OldTail = G_LightListTail;
        if (OldTail != nullptr)
            SetListNext(OldTail, Light);
        else
            G_LightListHead = Light;
        G_LightListTail = Light;
        SetListPrev(Light, OldTail);
        SetListNext(Light, nullptr);
        ++G_LightListCount;

        // vt+0xa4 SetLightEnable(&D3d, 1), unconditional in baseline.
        NeuronRuntime::Pipeline->SetLightEnable(&Light->D3d, 1);
    }
}

bool EngineLightApplyNearby(FEngineLight* Light, ICamera* Camera, float const* Pos3,
                            float* InOutAmbient)
{
    // (thiscall, RET 0xc: camera, point, ambient accumulator), listing-traced
    // . No argument is tested.
    bool bLit = false;
    float Dir[3] = {};
    if ((Light->Flags & 8u) != 0)
    {
        if ((Light->Flags & 0x20u) != 0)
        {
            EngineLightComputeDirection(Light, Dir, nullptr, nullptr);
            bLit = true;
        }
        else
        {
            float Dist = -1.0f;
            EngineLightComputeDirection(Light, Dir, Pos3, &Dist);

            if ((Light->Flags & 2u) != 0 && (Light->Type == 3 || Light->Type == 2))
            {
                if (Light->Type != 1)
                {
                    Light->D3d.Direction[0] = Dir[0];
                    Light->D3d.Direction[1] = Dir[1];
                    Light->D3d.Direction[2] = -Dir[2];
                }
                float Neg[3];
                Vec3Negate(Neg, Dir);
                Light->Orient[2] = Neg[0];
                Light->Orient[5] = Neg[1];
                Light->Orient[8] = Neg[2];
                EngineLightPushUpdate(Light);
            }

            // the routine(Dist, &colour) (it always writes the colour);
            // a modified colour becomes the diffuse (alpha 1.0), pushed through slot 75 when
            // == 0 and the light is registered (the shared tail, inlined).
            float Color[3];
            bool const bColorMod = EngineLightGetEffectiveColor(Light, Dist, Color);
            if (bColorMod)
            {
                Light->D3d.Diffuse[0] = Color[0];
                Light->D3d.Diffuse[1] = Color[1];
                Light->D3d.Diffuse[2] = Color[2];
                Light->D3d.Diffuse[3] = 1.0f;
                PushLightUpdate(Light);
            }

            // an ambient light (+0xb0 == -2) adds its colour, scaled by
            // its ambient curve (+0xb8) at Dist when it has one (1.0 otherwise), to the
            // accumulator.
            if (Light->Type == -2)
            {
                float Scale = 1.0f;
                if (Light->AmbientCurve != nullptr)
                {
                    // range {clamp, 0, D3d.Range}, evaluated at Dist.
                    FGraphRange Reach;
                    Reach.bClamp = 1;
                    Reach.Min = 0.0f;
                    Reach.Max = Light->D3d.Range;
                    Scale = static_cast<float>(Light->AmbientCurve->Evaluate(Dist, &Reach)); // FSTP float 0041064e
                }
                float Scaled[3];
                ScaleVec3(Scaled, Color, Scale);
                InOutAmbient[0] += Scaled[0];
                InOutAmbient[1] += Scaled[1];
                InOutAmbient[2] += Scaled[2];
            }

            // (c2 + c1) + c0 on the FPU against 1/255 (the field,
            // float); TEST AH,5 / JP: an ordered "less" gives AL = 1, SETZ -> unlit; greater,
            // equal and unordered are lit.
            double const Sum = (static_cast<double>(Color[2]) + Color[1]) + Color[0];
            bLit = !(Sum < static_cast<double>(1.0f / 255.0f));
        }

        // the field as it is (no query here) -- non-zero goes soft.
        if (G_PipelineLightState != 0)
            SoftCameraRewriteLight(Light, Camera, Dir);
    }

    // Bit4 list link / unlink vs bLit (00410772..).
    unsigned char const OldFlags = Light->Flags;
    bool const bWasLinked = (OldFlags & 0x10u) != 0;
    if (bWasLinked != bLit)
    {
        unsigned char const NewFlags = static_cast<unsigned char>(
            (((static_cast<unsigned>(bLit) << 4) ^ OldFlags) & 0x10u) ^ OldFlags);
        Light->Flags = NewFlags;
        if ((NewFlags & 0x10u) != 0)
        {
            LinkIntoRegisteredList(Light);
            return bLit;
        }
        UnlinkFromList(Light);
    }
    return bLit;
}

void EngineLightClearRegisteredList()
{
    // .
    FEngineLight* Light = G_LightListHead;
    while (Light != nullptr)
    {
        // vt+0xa4 SetLightEnable(&D3d, 0), unconditional in baseline.
        NeuronRuntime::Pipeline->SetLightEnable(&Light->D3d, 0);
        Light->Flags = static_cast<unsigned char>(Light->Flags & 0xefu);
        Light = ListNext(Light);
    }
    G_LightListHead = nullptr;
    G_LightListTail = nullptr;
    G_LightListCount = 0;
}

namespace
{
    // (cdecl): clamp an int to [0, 0xff].
    int ClampColorByte(int Value)
    {
        if (Value < 0)
            return 0;
        if (Value > 0xff)
            return 0xff;
        return Value;
    }
}

void EngineLightApplyList(FEngineLight* const* Lights, unsigned Count, ICamera* Camera,
                          float const* Pos3)
{
    // .
    if (GetGameOption(kGameOptLighting) == 0)
    {
        // the the routine walk inlined, then D3DRS_LIGHTING off.
        EngineLightClearRegisteredList();
        NeuronRuntime::Pipeline->SetRenderState(D3DRS_LIGHTING, 0);
        return;
    }

    // D3DRS_AMBIENT read back, each byte * 1/255 (the field, float).
    int Packed = 0;
    NeuronRuntime::Pipeline->GetRenderState(D3DRS_AMBIENT, &Packed);
    const float kInv255 = 1.0f / 255.0f;
    float Ambient[3];
    Ambient[0] = static_cast<float>((static_cast<unsigned>(Packed) >> 16) & 0xffu) * kInv255;
    Ambient[1] = static_cast<float>((static_cast<unsigned>(Packed) >> 8) & 0xffu) * kInv255;
    Ambient[2] = static_cast<float>(static_cast<unsigned>(Packed) & 0xffu) * kInv255;

    // the same inlined the routine walk.
    EngineLightClearRegisteredList();

    // unsigned index loop (JBE / JC).
    unsigned Index = 0;
    for (Index = 0; Index < Count; ++Index)
        EngineLightApplyNearby(Lights[Index], Camera, Pos3, Ambient);

    // each float * 255.0 (the field, double), truncated by ftol and
    // clamped; R inline, G and B through the routine; packed as ((R|0xffffff00)<<8|G)<<8|B.
    int const R = ClampColorByte(static_cast<int>(static_cast<double>(Ambient[0]) * 255.0));
    int const G = ClampColorByte(static_cast<int>(static_cast<double>(Ambient[1]) * 255.0));
    int const B = ClampColorByte(static_cast<int>(static_cast<double>(Ambient[2]) * 255.0));
    unsigned Out = static_cast<unsigned>(R) | 0xffffff00u;
    Out = (Out << 8) | static_cast<unsigned>(G);
    Out = (Out << 8) | static_cast<unsigned>(B);
    NeuronRuntime::Pipeline->SetRenderState(D3DRS_AMBIENT, static_cast<int>(Out));
}

namespace
{
    // / the field / the field -- the gamelit enable list: count, head, tail,
    // chained through FGameLight::ListNext (+0xc8) / ListPrev (+0xcc). All three are 0 in the PE
    // image and the XC initializer the branch stores 0 again.
    int G_GameLightCount = 0;
    FGameLight* G_GameLightHead = nullptr;
    FGameLight* G_GameLightTail = nullptr;

    // GameLight vtable slots as baseline dispatches them (CALL [EAX] / [EAX+4] / [EAX+8]).

    void GameLightCallDestroy(FGameLight* Light)
    {
        delete Light;
    }

    bool GameLightCallEnable(FGameLight* Light)
    {
        return Light->Enable();
    }

    void GameLightCallDisable(FGameLight* Light)
    {
        Light->Disable();
    }
}

// The GameLight base subobject is at +0; null remains null.
FEngineLight* EngineLightGameListHead()
{
    return reinterpret_cast<FEngineLight*>(G_GameLightHead);
}

FEngineLight* EngineLightGameListNext(FEngineLight* Light)
{
    // +0xc8 of the GameLight containing this EngineLight base.
    return reinterpret_cast<FEngineLight*>(reinterpret_cast<FGameLight*>(Light)->ListNext);
}

// (module storage-005376ea), thiscall on a GameLight.
bool GameLightEnable(FEngineLight* Light)
{
    FGameLight* const Game = reinterpret_cast<FGameLight*>(Light);
    // MOV AL,[+0xb4]; SHR AL,3; AND AL,1; JNZ -- already lit returns
    // that 1.
    if (((Light->Flags >> 3) & 1u) != 0)
        return true;
    // the routine; AL == 0 returns it.
    if (!EngineLightRegister(Light))
        return false;
    // append at the tail, count and reference.
    Game->ListPrev = G_GameLightTail;
    if (G_GameLightTail != nullptr)
        G_GameLightTail->ListNext = Game;
    else
        G_GameLightHead = Game;
    G_GameLightTail = Game;
    Game->ListNext = nullptr;
    ++G_GameLightCount;
    ++Game->RefCount;
    return true;
}

// (module storage-00537791), thiscall on a GameLight.
void GameLightDisable(FEngineLight* Light)
{
    FGameLight* const Game = reinterpret_cast<FGameLight*>(Light);
    // TEST [+0xb4],8 / JZ -- not lit does nothing.
    if ((Light->Flags & 8u) == 0)
        return;
    // the routine.
    EngineLightUnregister(Light);
    // unlink.
    FGameLight* const Prev = Game->ListPrev;
    if (Prev == nullptr)
    {
        G_GameLightHead = Game->ListNext;
        if (G_GameLightHead != nullptr)
            G_GameLightHead->ListPrev = nullptr;
        else
            G_GameLightTail = nullptr;
    }
    else if (Game->ListNext == nullptr)
    {
        G_GameLightTail = Prev;
        Prev->ListNext = nullptr;
    }
    else
    {
        Prev->ListNext = Game->ListNext;
        Game->ListNext->ListPrev = Game->ListPrev;
    }
    // clear the links, count and reference; slot 0 (1) at <= 0.
    Game->ListPrev = nullptr;
    Game->ListNext = nullptr;
    --G_GameLightCount;
    int const Remaining = --Game->RefCount;
    if (Remaining <= 0)
        GameLightCallDestroy(Game);
}

void EngineLightSetAmbientRgbBytes(unsigned char const Rgb[3])
{
    // — OR 0xffffff00 into R, then (R<<8|G)<<8|B, SetRenderState(0x8b).
    // baseline reads Rgb and NeuronRuntime::Pipeline unconditionally (0x41024e / 0x410244); every
    // port caller passes a stack or static array from a render path that runs after
    // NeuronRuntime::Startup.
    unsigned Packed = static_cast<unsigned>(Rgb[0]) | 0xffffff00u;
    Packed = (Packed << 8) | static_cast<unsigned>(Rgb[1]);
    Packed = (Packed << 8) | static_cast<unsigned>(Rgb[2]);
    NeuronRuntime::Pipeline->SetRenderState(D3DRS_AMBIENT, static_cast<int>(Packed));
}

namespace
{
    // vptr, Head the field, Tail the field, bUseHeap the field,
    // FreeList the field. the branch constructs it (the routine) and registers
    // with atexit -- this object's static constructor and destructor.
    OwnerList<Light::AnimationArch> G_LightAnimations;

    // the GameLight block pool {ElementSize 0x12C, FreeList 0, MaxFreeCount -1,
    // CurrentFreeCount 0}, stored by the static initializer the branch. the routine and
    // allocate from it, the routine returns every deleted GameLight to it, and
    // shrinks it.
    Heap G_GameLightHeap(0x12cu, -1);

}

FGameLight* GameLightConstruct(FGameLight* Light)
{
    return ::new (Light) FGameLight;
}

FGameLight::FGameLight() : FEngineLight(-1)
{
    // (thiscall), listing-traced 0x005374e0..0x00537594. No null test; the list
    // links +0xc8 / +0xcc are not written (GameLightEnable sets them).
    // the routine(-1).
    // refcount 1; anim keys / count / def 0, index -1, elapsed -1.0f,
    // range scale 0; engine index -1.
    this->RefCount = 1;
    memset(&this->Anim, 0, sizeof(this->Anim));
    this->Anim.CurrentIndex = -1;
    this->Anim.Elapsed = -1.0f;
    this->EngineIndex = 0xffffffffu;
    // the 12-float transform at +0xf0 zeroed with +0xf0 / +0x100 /
    // +0x110 (indices 0 / 4 / 8, the 3x3 diagonal) = 1.0.
    memset(this->LocalXform, 0, sizeof(this->LocalXform));
    this->LocalXform[8] = 1.0f;
    this->LocalXform[4] = 1.0f;
    this->LocalXform[0] = 1.0f;
    // +0x120..+0x128 zeroed, then the GameLight vtable.
    this->Field120 = 0.0f;
    this->Field124 = 0.0f;
    this->Field128 = 0.0f;
}

FGameLight* GameLightCreatePoint(float const* Pos3, unsigned char const Rgb[3], float Range)
{
    // — __cdecl (Pos3, Rgb, Range). The block comes straight from the pool
    // (Heap::Malloc on the field, not through the routine); a null block skips the
    // constructor and the setters then run on null, as in baseline.
    void* const Mem = G_GameLightHeap.Malloc();
    FGameLight* Light = nullptr;
    if (Mem != nullptr)
        Light = GameLightConstruct(static_cast<FGameLight*>(Mem));
    EngineLightSetType((FEngineLight*)Light, 1, true);           // (1, 1)
    EngineLightSetPosition((FEngineLight*)Light, Pos3);
    EngineLightSetColorBytesAlt((FEngineLight*)Light, Rgb);
    EngineLightSetRadius((FEngineLight*)Light, Range);
    if (Light->Enable()) // CALL [EAX+0x4]
        return Light;

    int const Remaining = --Light->RefCount;
    if (Remaining <= 0)
    {
        delete Light; // PUSH 1; CALL [EDX]
    }
    // baseline passes the float range as a double to a %d field (00537b62..00537b88).
    FDUMP(0x100002, "%s(%d) : *** WARNING: Could not load light of range %d",
          "Client\\space\\gamelit.cpp", 0x203,
          static_cast<double>(Range));
    return nullptr;
}

Light::AnimationArch::AnimationArch()
    : OwnerList<AnimFrame>(), NameLen(0), KeyBuffers(true)
{
    Name[0] = '\0';
}

Light::AnimationArch::~AnimationArch()
{
    // free() each spare copy and clear the slot; the member's and base's
    // destructors (~MetaList<void>, ~OwnerList<AnimFrame>) then run as in baseline.
    for (MetaNode<void>* Node = KeyBuffers.get_head(); Node != nullptr; Node = Node->Next)
    {
        ::free(Node->Data);
        Node->Data = nullptr;
    }
}

void LightAnimationLoad(char const* Path)
{
    INI_Reader Reader;
    if (Reader.open(Path, false) && Reader.read_header())
    {
        do
        {
            if (!Reader.is_header("LightAnim"))
                continue;

            Light::AnimationArch* const Anim = new Light::AnimationArch();
            Light::AnimFrame* LastFrame = nullptr; // ESI, cleared per section (00538bf5)
            if (Reader.read_value())
            {
                do
                {
                    if (Reader.is_value("name"))
                    {
                        char const* const Value = Reader.get_value_string();
                        if (Value == nullptr)
                        {
                            Anim->NameLen = 0;
                            Anim->Name[0] = '\0';
                        }
                        else
                        {
                            Anim->NameLen = CopyBoundedString(Anim->Name, 0x20, Value);
                        }
                    }
                    else if (Reader.is_value("frame"))
                    {
                        LastFrame = new Light::AnimFrame;
                        LastFrame->Time = Reader.get_value_float(0);
                        LastFrame->Rgb[0] = static_cast<unsigned char>(
                            static_cast<int>(Reader.get_value_float(1)));
                        LastFrame->Rgb[1] = static_cast<unsigned char>(
                            static_cast<int>(Reader.get_value_float(2)));
                        LastFrame->Rgb[2] = static_cast<unsigned char>(
                            static_cast<int>(Reader.get_value_float(3)));
                        LastFrame->Intensity = Reader.get_value_float(4);
                        // FCOMP 0.0 / TEST AH,0x41 / JP: only a positive (or unordered)
                        // intensity is kept.
                        if (LastFrame->Intensity <= 0.0f)
                            LastFrame->Intensity = 0.01f;
                        LastFrame->EndMarker = 0;
                        Anim->append(LastFrame);
                    }
                } while (Reader.read_value());
            }

            if (LastFrame != nullptr)
                LastFrame->EndMarker = -1;
            G_LightAnimations.append(Anim);
        } while (Reader.read_header());
    }
}

void LightAnimationFreeAll()
{
    // .
    G_LightAnimations.free();
}

Light::AnimationArch* GameLightFindAnimByName(char const* Name)
{
    // .
    for (MetaNode<Light::AnimationArch>* Node = G_LightAnimations.get_head(); Node != nullptr;
         Node = Node->Next)
    {
        if (_stricmp(Node->Data->Name, Name) == 0)
            return Node->Data;
    }
    return nullptr;
}

void GameLightBindAnimKeys(FGameLightAnimSlot* Slot, Light::AnimationArch* Anim, float RangeScale,
                           float IntensityScale, float TimeScale, float StartTime)
{
    // — thiscall on the slot, RET 0x14.
    if (Slot->Def != nullptr)
    {
        Slot->Def->KeyBuffers.append(static_cast<void*>(Slot->Keys));
        Slot->Keys = nullptr;
        Slot->Def = nullptr;
    }

    Slot->Def = Anim;
    MetaNode<void>* Spare = Anim->KeyBuffers.get_head();
    Light::AnimFrame* Buffer;
    if (Spare != nullptr)
        Buffer = static_cast<Light::AnimFrame*>(Spare->Data);
    else
        Buffer = static_cast<Light::AnimFrame*>(malloc(Anim->count() * sizeof(Light::AnimFrame)));
    Anim->KeyBuffers.remove(Spare); // ; a null node is a no-op
    Slot->Keys = Buffer;
    Slot->KeyCount = static_cast<int>(Slot->Def->count());

    int Index = 0;
    for (MetaNode<Light::AnimFrame>* Node = Slot->Def->get_head(); Node != nullptr;
         Node = Node->Next)
    {
        Slot->Keys[Index] = *Node->Data;
        Slot->Keys[Index].Time *= TimeScale;
        Slot->Keys[Index].Intensity *= IntensityScale;
        ++Index;
    }

    Slot->Elapsed = StartTime;
    Slot->RangeScale = RangeScale;
    Slot->CurrentIndex = 0;

    int const Count = Slot->KeyCount;
    Light::AnimFrame const* const Keys = Slot->Keys;
    // FCOMP / TEST AH,0x41 / JP: a last key at or before the start time (ordered) ends it here.
    if (Keys[Count - 1].Time <= StartTime)
    {
        Slot->CurrentIndex = -1;
        return;
    }
    for (int Key = 0; Key < Count; ++Key)
    {
        // TEST AH,5 / JP skips keys at or after the start time (and unordered ones).
        if (Keys[Key].Time < StartTime)
            Slot->CurrentIndex = Key;
    }
}

bool GameLightAttachAnim(FGameLight* Light, Light::AnimationArch* Anim, float IntensityScale,
                         float TimeScale, float StartTime)
{
    // .
    GameLightBindAnimKeys(&Light->Anim, Anim, Light->D3d.Range, IntensityScale, TimeScale,
                          StartTime);
    bool const bBound = Light->Anim.Keys != nullptr;
    if (bBound)
        ++Light->RefCount;
    return bBound;
}

void GameLightTickAnim(FGameLightAnimSlot* Slot, float Dt, FGameLight* Light)
{
    // — thiscall on the slot, RET 8.
    Light::AnimFrame* const Keys = Slot->Keys;
    if (Keys == nullptr)
        return;
    if (Slot->CurrentIndex < 0)
        return;

    int const Count = Slot->KeyCount;
    Light::AnimFrame const* const Last = &Keys[Count - 1];
    // FLD Dt / FADD Elapsed / FST Elapsed: the sum stays unrounded on the FPU for the compares.
    double const Elapsed = static_cast<double>(Dt) + Slot->Elapsed;
    Slot->Elapsed = static_cast<float>(Elapsed);

    // FCOM last / TEST AH,1 / JNZ: before the last key (or unordered) scans forward.
    if (!(Elapsed >= Last->Time))
    {
        for (int Index = Slot->CurrentIndex; Index < Count; ++Index)
        {
            // TEST AH,0x41 / JNZ: only a key strictly before Elapsed moves the index.
            if (Elapsed > Keys[Index].Time)
                Slot->CurrentIndex = Index;
        }
    }
    else
    {
        Slot->CurrentIndex = -1;
    }

    int const Current = Slot->CurrentIndex;
    if (Current != -1)
    {
        Light::AnimFrame const& From = Keys[Current];
        Light::AnimFrame const& To = Keys[Current + 1];
        // FDIVP then FSTP to a float local: the blend is rounded to float before use.
        float const Blend = static_cast<float>((Elapsed - From.Time) /
                                               (static_cast<double>(To.Time) - From.Time));

        // Per channel: FILD (to - from) / FMUL blend / FIADD from / _ftol, low byte kept.
        unsigned char Rgb[3];
        for (int Channel = 0; Channel < 3; ++Channel)
        {
            int const Delta =
                static_cast<int>(To.Rgb[Channel]) - static_cast<int>(From.Rgb[Channel]);
            Rgb[Channel] = static_cast<unsigned char>(
                static_cast<int>(Delta * static_cast<double>(Blend) + From.Rgb[Channel]));
        }
        EngineLightSetColorBytesAlt((FEngineLight*)Light, Rgb);

        double const Intensity =
            (static_cast<double>(To.Intensity) - From.Intensity) * Blend + From.Intensity;
        EngineLightSetRadius((FEngineLight*)Light, static_cast<float>(Intensity * Slot->RangeScale));
        return;
    }

    if (Last->EndMarker == -1)
    {
        // is the routine inlined.
        GameLightAnimDetach(Light);
        return;
    }

    EngineLightSetColorBytesAlt((FEngineLight*)Light, Last->Rgb);
    EngineLightSetRadius((FEngineLight*)Light, Last->Intensity * Slot->RangeScale);
}

I3DMathEngine* G_GameLightMath = nullptr;

void GameLightTick(FGameLight* Light, float Dt)
{
    // (thiscall, RET 4). No null test on the light: both callers (the list walk
    // through vtable slot 3 and the equip light's tick) hand a live one.
    // TEST byte [+0xb4],8 / JZ -> nothing at all.
    if ((Light->Flags & 8u) == 0)
        return;
    if (Light->EngineIndex == 0xffffffffu)
    {
        // the three products stay on the FPU; the z product is
        // spilled to a float local (FSTP 0x00537906) before its add, x and y are added from
        // the stack (FADD 0x0053790c / 0x00537916) and rounded once by their FSTP.
        double const Xv = static_cast<double>(Dt) * static_cast<double>(Light->Field120);
        double const Yv = static_cast<double>(Dt) * static_cast<double>(Light->Field124);
        float const Zv = static_cast<float>(static_cast<double>(Dt) * static_cast<double>(Light->Field128));
        float Pos[3];
        Pos[0] = static_cast<float>(Xv + static_cast<double>(Light->Position[0]));
        Pos[1] = static_cast<float>(Yv + static_cast<double>(Light->Position[1]));
        Pos[2] = static_cast<float>(static_cast<double>(Zv) + static_cast<double>(Light->Position[2]));
        // the routine.
        EngineLightSetPosition((FEngineLight*)Light, Pos);
    }
    else
    {
        // NeuronRuntime::Engine vt+0xa0 (GetInstanceTransform) on +0xec, no
        // null test on the engine or the result.
        float* const InstRaw = NeuronRuntime::Engine->GetInstanceTransform(
            reinterpret_cast<void*>(static_cast<intptr_t>(Light->EngineIndex)));
        // the field, the routine when still null.
        if (G_GameLightMath == nullptr)
            G_GameLightMath = Acquire3DMathEngine();
        // vt+0x1c MulTransform(temp, instance, +0xf0).
        FTransform World;
        G_GameLightMath->MulTransform(World, *reinterpret_cast<FTransform const*>(InstRaw),
                                      *reinterpret_cast<FTransform const*>(Light->LocalXform));
        // copied to a second local, the routine(this, &copy).
        FTransform const Copy = World;
        EngineLightSetTransform((FEngineLight*)Light, &Copy.Rotation.D[0][0], &Copy.Translation.X);
    }
    // +0xd4 non-null -> the routine(Dt, this) on the anim slot.
    if (Light->Anim.Keys != nullptr)
        GameLightTickAnim(&Light->Anim, Dt, Light);
}

void GameLightTickAll(float Dt)
{
    // (module storage-005383b4): walk the field; each light's +0xc8 is read
    // before its vtable slot 3 (+0xc) runs with Dt.
    FGameLight* Light = G_GameLightHead;
    while (Light != nullptr)
    {
        FGameLight* const Next = Light->ListNext;
        Light->Tick(Dt);
        Light = Next;
    }
}

void GameLightLeaveSystem()
{
    // (module storage-00538511).
    FGameLight* LocalHead = nullptr; // [ESP+0x18]
    FGameLight* LocalTail = nullptr; // EBP
    int LocalCount = 0;              // [ESP+0x14], never read
    FGameLight* Light = G_GameLightHead;
    while (Light != nullptr)
    {
        // next saved, ++RefCount.
        FGameLight* const Next = Light->ListNext;
        ++Light->RefCount;
        // the inlined the routine when Keys is set.
        if (Light->Anim.Keys != nullptr)
        {
            if (Light->Anim.Def != nullptr)
            {
                // on Def+0x38 with Keys.
                Light->Anim.Def->KeyBuffers.append(static_cast<void*>(Light->Anim.Keys));
                Light->Anim.Keys = nullptr;
                Light->Anim.Def = nullptr;
            }
            GameLightCallDisable(Light);
            int const AfterDetach = --Light->RefCount;
            if (AfterDetach <= 0)
                GameLightCallDestroy(Light);
        }
        // slot 2, --RefCount, slot 0 (1) at <= 0; exactly 0 is not
        // relinked (JZ 00538498), a negative count falls through into the relink.
        GameLightCallDisable(Light);
        int const Remaining = --Light->RefCount;
        bool bRelink = true;
        if (Remaining <= 0)
        {
            GameLightCallDestroy(Light);
            if (Remaining == 0)
                bRelink = false;
        }
        if (bRelink)
        {
            // append to the local list.
            Light->ListPrev = LocalTail;
            if (LocalTail != nullptr)
                LocalTail->ListNext = Light;
            else
                LocalHead = Light;
            ++LocalCount;
            LocalTail = Light;
            Light->ListNext = nullptr;
        }
        Light = Next;
    }
    (void)LocalCount;

    // each local entry unlinked from its neighbours, its links cleared,
    // then vtable slot 1 (enable).
    Light = LocalHead;
    while (Light != nullptr)
    {
        FGameLight* const Prev = Light->ListPrev;
        FGameLight* const Next = Light->ListNext;
        if (Prev == nullptr)
        {
            if (Next != nullptr)
                Next->ListPrev = nullptr;
        }
        else if (Next == nullptr)
        {
            Prev->ListNext = nullptr;
        }
        else
        {
            Prev->ListNext = Next;
            Next->ListPrev = Light->ListPrev;
        }
        Light->ListPrev = nullptr;
        Light->ListNext = nullptr;
        GameLightCallEnable(Light);
        Light = Next;
    }
}

void GameLightReleaseAll()
{
    // (module storage-00538556): next read first, --RefCount, slot 0 (1) at <= 0.
    // itself is not touched.
    FGameLight* Light = G_GameLightHead;
    while (Light != nullptr)
    {
        FGameLight* const Next = Light->ListNext;
        int const Remaining = --Light->RefCount;
        if (Remaining <= 0)
            GameLightCallDestroy(Light);
        Light = Next;
    }
}

void* GameLightHeapAlloc(size_t Size)
{
    // — GameLight::operator new: size==0x12c → Heap::Malloc(the field); else
    // malloc.
    if (Size != 0x12c)
        return malloc(Size);
    return G_GameLightHeap.Malloc();
}

void GameLightHeapShrink()
{
    // — MOV ECX,the field; JMP Heap::Shrink.
    G_GameLightHeap.Shrink();
}

void GameLightAddRef(FGameLight* Light)
{
    // (thiscall), 0x00537ac0..0x00537acd — *(this+0xd0)++. No null test (every
    // caller tests or dereferences the light first).
    Light->RefCount = Light->RefCount + 1;
}

int GameLightRelease(FGameLight* Light)
{
    // (thiscall), 0x00537ad0..0x00537aed: +0xd0 - 1 stored; not above 0 (JG) ->
    // vtable slot 0 with 1 (the deleting destructor, through the object's own table); the new
    // count returned. No null test.
    int const Next = Light->RefCount - 1;
    Light->RefCount = Next;
    if (Next <= 0)
    {
        delete Light;
    }
    return Next;
}

void GameLightAnimDetach(FGameLight* Light)
{
    // .
    if (Light->Anim.Keys == nullptr)
        return;
    if (Light->Anim.Def != nullptr)
    {
        Light->Anim.Def->KeyBuffers.append(static_cast<void*>(Light->Anim.Keys));
        Light->Anim.Keys = nullptr;
        Light->Anim.Def = nullptr;
    }

    Light->Disable(); // CALL [EDX+0x8]
    int const Remaining = --Light->RefCount;
    if (Remaining <= 0)
        delete Light; // PUSH 1; CALL [EAX]
}

void GameLightLoad(FGameLight* Light, INI_Reader* Reader)
{
    // — __thiscall, RET 4: every value of the [LightSource] section goes to
    // (value string fetched before the key name, as baseline does), then vtable
    // slot 1 (enable).
    while (Reader->read_value())
    {
        char const* const Value = Reader->get_value_string();
        char const* const Name = Reader->get_name_ptr();
        EngineLightReadValue((FEngineLight*)Light, Name, Value);
    }
    Light->Enable(); // CALL [EAX+0x4]
}

FEngineLight::~FEngineLight()
{
    DestructLightBody(this);
}

bool FEngineLight::Enable()
{
    return EngineLightRegister(this);
}

void FEngineLight::Disable()
{
    EngineLightUnregister(this);
}

FGameLight::~FGameLight()
{
    Light::AnimationArch* const Def = this->Anim.Def;
    if (Def != nullptr)
    {
        Def->KeyBuffers.append(static_cast<void*>(this->Anim.Keys));
        this->Anim.Keys = nullptr;
        this->Anim.Def = nullptr;
    }
}

void FGameLight::operator delete(void* Memory)
{
    G_GameLightHeap.Free(Memory);
}

bool FGameLight::Enable()
{
    return GameLightEnable(this);
}

void FGameLight::Disable()
{
    GameLightDisable(this);
}

void FGameLight::Tick(float Dt)
{
    GameLightTick(this, Dt);
}
