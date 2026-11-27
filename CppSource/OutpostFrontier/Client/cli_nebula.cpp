// Author: Giperion.

#include "cli_nebula.h"
#include "oracle_mode.h" // Oracle instrumentation (P0.5)

#include "cli_projectile_fx.h" // CliEffectCompanionArmStop / CliEffectCompanionClear (+0xe4)
#include "client_mesh_load.h"
#include "common_csys.h"
#include "common_id.h"
#include "common_ini.h"
#include "common_misc.h"
#include "common_net.h"
#include "common_universe.h"
#include "effect_base.h"    // FEffectBase (the effect at Nebula +0xec)
#include "engine_light.h"
#ifdef OUTPOST_FRONTIER_DX9RENDERER
#include "nebula_volume.h" // port-only Direct3D 9 volumetric nebula gates
#endif
#include "pixel_format.h"   // PixelFormatFromD3D / PixelFormatGetDesc (the routine / 005b7030)
#include "space_camera.h"
#include "star_sys.h"
#include "texture_panels.h"
#include "texture_shapes.h"
#include "win3d_immediate.h"
#include "win3d_render.h"

#include "NeuronRuntime.h"
#include "engine/sceneruntime.h"
#include "render/IRenderPipeline.h"
#include "render/rendcomp.h"
#include "render/shading.h"
#include "x86math.h"

#include <list>
#include <map>
#include <math.h>
#include <new>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

I3DMathEngine* G_CloudsMath = nullptr; // (cli_nebula.h)

// The zone_shell mesh an [Exclusion Zones] section loads (0x00505c78: operator new(0x38)).
// +0x00 the ObjectHandle (the routine); 0x00505c9b sets the seven floats +0x10..+0x28 to
// 1.0f; +0x10 is handed to the engine as the instance's user data (0x00505d1f), and the
// exclusion-shell draw copies the tint into +0x10..+0x18 and the faded alpha into +0x20
// (0x00508ff6..0x00509017). +0x24 shell_scalar (raised to 1), +0x28 max_alpha (capped at 1),
// +0x2c..+0x34 exclusion_tint (each capped at 1) -- the tint is never initialised when the
// key is absent, exactly as baseline leaves it.
struct FNebulaZoneShell
{
    FMeshHolder Handle;   // +0x00
    float UserData[5];    // +0x10 colour r, g, b; +0x1c; +0x20 alpha
    float ShellScalar;    // +0x24
    float MaxAlpha;       // +0x28
    float Tint[3];        // +0x2c
};
static_assert(sizeof(FNebulaZoneShell) == 0x38, "zone_shell mesh is 0x38");
static_assert(offsetof(FNebulaZoneShell, ShellScalar) == 0x24, "zone_shell +0x24");
static_assert(offsetof(FNebulaZoneShell, Tint) == 0x2c, "zone_shell +0x2c");


namespace
{
    // / +0xe0/+0xe4/+0xe8 — PE .data defaults (exe RVA 0x2132dc…).
    float G_NebulaDetail = 1.0f;
    float G_NebulaDetailEase = 0.707f;
    float G_NebulaDetailMix = 0.35f;
    float G_NebulaDetailFog = 0.5f;

    float BitsToFloat(unsigned Bits)
    {
        float V = 0.0f;
        memcpy(&V, &Bits, sizeof(float));
        return V;
    }

    // The "color" keys of the routine and the routine: three values, each * 1/255 (float
    // ) and stored as float, into Out[0..2].
    void ReadColor255(INI_Reader& Reader, float* Out)
    {
        float const R = Reader.get_value_float(0);
        float const G = Reader.get_value_float(1);
        float const Bl = Reader.get_value_float(2);
        Out[0] = R * (1.0f / 255.0f);
        Out[1] = G * (1.0f / 255.0f);
        Out[2] = Bl * (1.0f / 255.0f);
    }

    I3DMathEngine* AcquireNebulaMath()
    {
        // — lazy 3DMathEngine (the routine).
        static I3DMathEngine* G_NebulaMath = nullptr;
        if (G_NebulaMath == nullptr)
            G_NebulaMath = Acquire3DMathEngine();
        return G_NebulaMath;
    }

    // Nebula +0x38: std::map<unsigned (CreateID of the mesh name), zone_shell*>. baseline's VC6
    // map (the routine ctor, lower_bound the routine / the routine, insert the routine,
    // erase the routine); the port keeps a real std::map in the same bytes.
    typedef std::map<unsigned, FNebulaZoneShell*> FNebulaZoneShellMap;
    static_assert(sizeof(FNebulaZoneShellMap) < 0x14,
                  "std::map must fit in the baseline tree's 0x14 bytes");

    FNebulaZoneShellMap& NebulaZoneShells(FNebulaEffect* Nebula)
    {
        return Nebula->ZoneShells;
    }

    // The two-step find every reader inlines (0x00505c38..0x00505c65, 0x005071f6..0x0050721b):
    // lower_bound, then end unless !(Key < node key).
    FNebulaZoneShellMap::iterator NebulaFindZoneShell(FNebulaZoneShellMap& Map, unsigned Key)
    {
        FNebulaZoneShellMap::iterator It = Map.lower_bound(Key);
        if (It == Map.end() || Key < It->first)
            return Map.end();
        return It;
    }

    // The nebula's ShapeDB lives in the baseline bytes at +0x04 (the routine builds it there).
    FShapeDB* NebulaShapes(FNebulaEffect* Nebula)
    {
        return &Nebula->Shapes;
    }

    // (thiscall) -- the zone_shell's destructor: the routine (instance and both
    // material slots released), then ~ObjectHandle the routine (vptr reset only). The caller
    // frees the memory.
    void NebulaZoneShellDestroy(FNebulaZoneShell* Shell)
    {
        ClientMeshHolderRelease(&Shell->Handle);
    }

    // /60/64 is not a nebula listener list: it is the TexturePanels instance list
    // (the routine links into it, the routine hands it to the StarSys tick). Its walks
    // / the routine live with the panels in texture_panels.cpp.

    int ClampByte255(int V)
    {
        if (V < 0)
            return 0;
        if (V > 255)
            return 255;
        return V;
    }

} // namespace

// (cdecl, 3 args) -- the one port of it; space_scene_obj_draw.cpp had a second
// copy, now removed. Each colour channel: FLD / FMUL 255.0f (the field) / ftol, clamped
// to [0,0xff] (00422449..004224c6). The float*float product is exact in double, as it is in
// the x87 register. No null tests: baseline dereferences Rgb3 and NeuronRuntime::Pipeline directly.
void FogApplyColorAndRange(float const* Rgb3, float FogStart, float FogEnd)
{
    int R = static_cast<int>(static_cast<double>(Rgb3[0]) * 255.0);
    if (R < 0)
        R = 0;
    else if (R > 0xff)
        R = 0xff;
    int G = static_cast<int>(static_cast<double>(Rgb3[1]) * 255.0);
    if (G < 0)
        G = 0;
    else if (G > 0xff)
        G = 0xff;
    int B = static_cast<int>(static_cast<double>(Rgb3[2]) * 255.0);
    if (B < 0)
        B = 0;
    else if (B > 0xff)
        B = 0xff;

    // Pipeline vt+0x20 queries device ability 0xd (W-fog),
    // preset to 0. GetPipelineState is vt+0x18 and reads an unrelated setting.
    unsigned long WFogAbility = 0;
    NeuronRuntime::Pipeline->QueryDeviceAbility(0xd, &WFogAbility);
    if (WFogAbility == 0)
    {
        NeuronRuntime::Pipeline->SetRenderState(D3DRS_FOGTABLEMODE, 0); // D3DRS_FOGTABLEMODE
        NeuronRuntime::Pipeline->SetRenderState(D3DRS_FOGVERTEXMODE, 3); // D3DRS_FOGVERTEXMODE
    }
    else
    {
        NeuronRuntime::Pipeline->SetRenderState(D3DRS_FOGTABLEMODE, 3);
        NeuronRuntime::Pipeline->SetRenderState(D3DRS_FOGVERTEXMODE, 0);
        NeuronRuntime::Pipeline->SetRenderState(D3DRS_RANGEFOGENABLE, 0); // D3DRS_RANGEFOGENABLE
    }

    // ((R|0xffffff00)<<8|G)<<8|B -> D3DRS_FOGCOLOR.
    unsigned Packed = static_cast<unsigned>(R) | 0xffffff00u;
    Packed = (Packed << 8) | static_cast<unsigned>(G);
    Packed = (Packed << 8) | static_cast<unsigned>(B);
    NeuronRuntime::Pipeline->SetRenderState(D3DRS_FOGCOLOR, static_cast<int>(Packed));

    // the raw float bits of arg 2 / arg 3, then D3DRS_FOGENABLE = 1.
    int FogStartBits = 0;
    int FogEndBits = 0;
    memcpy(&FogStartBits, &FogStart, sizeof(float));
    memcpy(&FogEndBits, &FogEnd, sizeof(float));
    NeuronRuntime::Pipeline->SetRenderState(D3DRS_FOGSTART, FogStartBits); // D3DRS_FOGSTART
    NeuronRuntime::Pipeline->SetRenderState(D3DRS_FOGEND, FogEndBits);   // D3DRS_FOGEND
    NeuronRuntime::Pipeline->SetRenderState(D3DRS_FOGENABLE, 1);
}

namespace
{

    // NeuronRuntime::MaterialBatcher slot +0x20. baseline reads the import and calls through it with no
    // null test at every site (e.g. 0x00508e67, 0x00508ed1).
    void SetMaterialBatchBinOrder(unsigned long Index, long Key)
    {
        NeuronRuntime::MaterialBatcher->SetBinOrder(Index, Key);
    }

    // — ambient GameLight at +0x270: SetPosition, flicker accum, colour/enable.
    // is thiscall (RET 4) with the camera position as the stack argument;
    // caller 0x00507309 (NebulaUpdate). No test of `this` or of the position pointer.
    void NebulaAmbientTick(FNebulaEffect* Nebula, float const* CamPos3)
    {
        unsigned char* const B = reinterpret_cast<unsigned char*>(Nebula);
        // the light at +0x270 is the one pointer baseline tests.
        FGameLight* const GLight = *reinterpret_cast<FGameLight**>(B + 0x270);
        if (GLight == nullptr)
            return;
#ifdef OUTPOST_FRONTIER_DX9RENDERER
        // Port-only: while the Direct3D 9 volumetric nebula's lightning strikes drive this light
        // (direction from the strike, its flicker; nebula_volume.cpp), the random flash below
        // stays out. Never true on DX8RENDERER or with the volume off.
        if (NebulaVolume::DrivesNebulaLight(Nebula))
            return;
#endif
        FEngineLight* const Light = (FEngineLight*)GLight;
        // +0x278 FCOMP 0.0f, TEST AH,0x41 / JNP -- return when <= 0
        // (a NaN period goes on).
        float const Period = *reinterpret_cast<float*>(B + 0x278);
        if (Period <= 0.0f)
            return;

        // the routine on the light.
        EngineLightSetPosition(Light, CamPos3);

        // the old +0x27c vs 0.0f; +0x27c = dt (+0x1c) + old, FST float,
        // with the unrounded sum kept on the FPU.
        float const Dt = *reinterpret_cast<float*>(B + 0x1c);
        float const AccPrev = *reinterpret_cast<float*>(B + 0x27c);
        double const Acc = static_cast<double>(Dt) + AccPrev;
        *reinterpret_cast<float*>(B + 0x27c) = static_cast<float>(Acc);

        // TEST AH,0x41 / JNZ: only an old value strictly above 0 falls through;
        // then returns unless the new sum is strictly above 0.
        if (!(AccPrev > 0.0f))
        {
            if (!(Acc > 0.0))
                return;
        }
        // FCOMP [+0x278], TEST AH,0x41 / JNZ -- only a sum strictly
        // above the period re-arms; anything else goes to the colour path (the branch).
        else if (Acc > Period)
        {
            // +0x27c = float((rand * 3.0517578125e-05f * double(0.7f)
            // - 1.5) * +0x274).
            int const RArm = rand();
            *reinterpret_cast<float*>(B + 0x27c) = static_cast<float>(
                (static_cast<double>(RArm) * 3.0517578125e-05f * static_cast<double>(0.7f) - 1.5) *
                *reinterpret_cast<float*>(B + 0x274));

            // three rands, each r * 3.0517578125e-05f * 2 - 1.0 (exact).
            double const First = static_cast<double>(rand()) * 3.0517578125e-05f;
            double const Second = static_cast<double>(rand()) * 3.0517578125e-05f;
            double const Third = static_cast<double>(rand()) * 3.0517578125e-05f;
            double const Z = (Third + Third) - 1.0;
            double const Y = (Second + Second) - 1.0;
            double const X = (First + First) - 1.0;
            // Inv = float(1.0f / sqrt(X^2 + Y^2 + Z^2)); no zero test.
            float const Inv = static_cast<float>(1.0f / sqrt(X * X + Y * Y + Z * Z));
            // the out vector is {Z, Y, X} * Inv -- the THIRD rand lands
            // in [0], the first in [2].
            float Dir[3];
            Dir[0] = static_cast<float>(Z * Inv);
            Dir[1] = static_cast<float>(Y * Inv);
            Dir[2] = static_cast<float>(static_cast<float>(X) * static_cast<double>(Inv));
            // the routine on the light.
            EngineLightSetDirection(Light, Dir);
            // lit (+0xb4 bit 3) -> GameLight vt+8 (disable).
            if ((Light->Flags & 0x08) != 0)
                GameLightVirtualDisable(GLight);
            return;
        }

        // intensity = +0x28c, plus +0x290 (stored as float) when +0x26c is set.
        float Intensity = *reinterpret_cast<float*>(B + 0x28c);
        if (B[0x26c] != 0)
            Intensity = Intensity + *reinterpret_cast<float*>(B + 0x290);

        // U0 = float(rand * c); Flick = rand * U0 * Intensity * c, kept
        // on the FPU.
        float const U0 = static_cast<float>(static_cast<double>(rand()) * 3.0517578125e-05f);
        int const RFlick = rand();
        double const Flick =
            static_cast<double>(RFlick) * U0 * Intensity * 3.0517578125e-05f;
        // red and green are stored as float, blue stays on the FPU.
        float Cr = static_cast<float>(Flick * *reinterpret_cast<float*>(B + 0x280));
        float Cg = static_cast<float>(Flick * *reinterpret_cast<float*>(B + 0x284));
        double Cb = Flick * *reinterpret_cast<float*>(B + 0x288);
        // FCOMP / FCOM 1.0f, TEST AH,5 / JNP -- keep only a value below
        // 1.0; anything else (>=, NaN) becomes 1.0.
        if (!(Cr < 1.0f))
            Cr = 1.0f;
        if (!(Cg < 1.0f))
            Cg = 1.0f;
        if (!(Cb < 1.0))
            Cb = 1.0;

        // red and green * 255.0f stored as float, then truncated
        // (ftol); blue * 255.0f is truncated from the FPU. Each is clamped to 0..255.
        float const Red255 = Cr * 255.0f;
        float const Green255 = Cg * 255.0f;
        int const Blue = ClampByte255(static_cast<int>(Cb * 255.0f));
        int const Green = ClampByte255(static_cast<int>(Green255));
        int const Red = ClampByte255(static_cast<int>(Red255));

        // {R, G, B, 0xff} built in the argument slot, then the routine.
        unsigned char Rgba[4];
        Rgba[0] = static_cast<unsigned char>(Red);
        Rgba[1] = static_cast<unsigned char>(Green);
        Rgba[2] = static_cast<unsigned char>(Blue);
        Rgba[3] = 0xff;
        EngineLightSetColorBytes(Light, Rgba);
        // not lit -> GameLight vt+4 (enable).
        if ((Light->Flags & 0x08) == 0)
            GameLightEnable(Light);
    }


    float const kCloudsRandScale = 3.0517578125e-05f; // = 1/32768

    void LocalNebulaCloudsConstruct(FNebulaClouds* Clouds)
    {
        // (thiscall): only these fields; the rest of the 0x12c block keeps what
        // operator new left there until the [Clouds] parse. The zeroed local vector at
        // [ESP+4..0xc] is an inlined empty ctor that nothing reads.
        unsigned char* const R = Clouds->Raw;
        *reinterpret_cast<float*>(R + 0x8) = 1.0f;
        *reinterpret_cast<float*>(R + 0xac) = 1.0f;
        *reinterpret_cast<int*>(R + 0xf4) = 0;
        *reinterpret_cast<int*>(R + 0xf8) = 0;
        *reinterpret_cast<int*>(R + 0xfc) = 0;
        *reinterpret_cast<int*>(R + 0x0) = 0;
        *reinterpret_cast<int*>(R + 0x4) = 0;
        *reinterpret_cast<int*>(R + 0xc) = 0;
        *reinterpret_cast<int*>(R + 0xc4) = 0;
        R[0xec] = 0;
        *reinterpret_cast<int*>(R + 0x128) = 0;
    }

    // The detail pair both the routine and the routine compute, on the FPU: a = 1.0f - d;
    // +0x08 = float(0.5f * a + d); +0xac = float((1.0f - e) * 0.707f + e), e = 1.0f - a * a
    // (0.707f is the field).
    void CloudsDetailFactors(FNebulaClouds* Clouds, float Detail)
    {
        double const A = 1.0f - static_cast<double>(Detail);
        *reinterpret_cast<float*>(Clouds->Raw + 0x8) = static_cast<float>(0.5f * A + Detail);
        double const Ease = 1.0f - A * A;
        *reinterpret_cast<float*>(Clouds->Raw + 0xac) =
            static_cast<float>((1.0f - Ease) * 0.707f + Ease);
    }

    // (thiscall, RET 4). No test of `this`.
    void LocalNebulaCloudsSetDetail(FNebulaClouds* Clouds, float Detail)
    {
        CloudsDetailFactors(Clouds, Detail);
    }

    // (thiscall): free (the routine) every non-null puff of the table (+0x00,
    // count +0x0c re-read each step) and null its slot, then free the table itself and null
    // +0x00. No test of the table pointer; +0x04 / +0xec are left alone.
    void LocalNebulaCloudsFreePuffTable(FNebulaClouds* Clouds)
    {
        unsigned char* const R = Clouds->Raw;
        for (int I = 0; I < *reinterpret_cast<int*>(R + 0xc); ++I)
        {
            void** const Table = *reinterpret_cast<void***>(R);
            if (Table[I] != nullptr)
                operator delete(Table[I]);
            (*reinterpret_cast<void***>(R))[I] = nullptr;
        }
        operator delete(*reinterpret_cast<void**>(R));
        *reinterpret_cast<void**>(R) = nullptr;
    }

    // (thiscall): with +0xec set, the the routine body, then +0x04 = 0 and
    // +0xec = 0.
    void LocalNebulaCloudsReleaseBits(FNebulaClouds* Clouds)
    {
        if (Clouds->Raw[0xec] == 0)
            return;
        LocalNebulaCloudsFreePuffTable(Clouds);
        *reinterpret_cast<int*>(Clouds->Raw + 4) = 0;
        Clouds->Raw[0xec] = 0;
    }

    // (thiscall on the puff, RET 0x18): Sample, Scale, Min, Max, Weights, Count.
    void LocalNebulaCloudsFillPuff(unsigned char* Puff, Vector const& Sample, float Scale,
                                   Vector const& MinCorner, Vector const& MaxCorner,
                                   float const* Weights, int WeightCount)
    {
        // the position.
        *reinterpret_cast<float*>(Puff + 0x0) = Sample.x;
        *reinterpret_cast<float*>(Puff + 0x4) = Sample.y;
        *reinterpret_cast<float*>(Puff + 0x8) = Sample.z;

        // +0x18 = float((rand * c * 0.40000003576278687 (double
        // ) + double(0.8f) (0x005e0bd0)) * Scale).
        int const RSize = rand();
        *reinterpret_cast<float*>(Puff + 0x18) = static_cast<float>(
            (static_cast<double>(RSize) * kCloudsRandScale * 0.40000003576278687 +
             0.800000011920929) *
            Scale);

        // D = Max - Min (floats); T = rand * c on the FPU; x = float(
        // D.x * T + Min.x) in one go, y and z add Min to the float product.
        float const Dx = MaxCorner.x - MinCorner.x;
        float const Dy = MaxCorner.y - MinCorner.y;
        float const Dz = MaxCorner.z - MinCorner.z;
        int const RTint = rand();
        double const T = static_cast<double>(RTint) * kCloudsRandScale;
        float const Py = static_cast<float>(Dy * T);
        float const Pz = static_cast<float>(Dz * T);
        *reinterpret_cast<float*>(Puff + 0xc) = static_cast<float>(Dx * T + MinCorner.x);
        *reinterpret_cast<float*>(Puff + 0x10) = Py + MinCorner.y;
        *reinterpret_cast<float*>(Puff + 0x14) = Pz + MinCorner.z;

        // +0x1c = (rand * count) / 32768, truncated toward zero.
        int const RIndex = rand();
        *reinterpret_cast<int*>(Puff + 0x1c) = (RIndex * WeightCount) / 32768;

        // pick = float(rand * c); the weights accumulate on the FPU and
        // the first running sum strictly above the pick (TEST AH,5 / JNP) replaces +0x1c.
        int const RPick = rand();
        float const Pick = static_cast<float>(static_cast<double>(RPick) * kCloudsRandScale);
        double Running = 0.0f;
        for (int I = 0; I < WeightCount; ++I)
        {
            Running += Weights[I];
            if (Pick < Running)
            {
                *reinterpret_cast<int*>(Puff + 0x1c) = I;
                break;
            }
        }

        // +0x54 = 0, +0x38 = -1.0f.
        *reinterpret_cast<int*>(Puff + 0x54) = 0;
        *reinterpret_cast<float*>(Puff + 0x38) = -1.0f;
    }

    // (thiscall): rebuild the puff table while +0xec is set.
    void LocalNebulaCloudsRebuildBits(FNebulaClouds* Clouds)
    {
        unsigned char* const R = Clouds->Raw;
        if (R[0xec] == 0)
            return;

        // an existing table (the one pointer tested here) is freed as
        // in the routine.
        if (*reinterpret_cast<void**>(R) != nullptr)
            LocalNebulaCloudsFreePuffTable(Clouds);

        // +0x0c = ftol(+0x04 * +0x08 + 0.5f); +0x00 = operator new
        // (count << 2), not tested.
        int const Count = static_cast<int>(
            static_cast<double>(*reinterpret_cast<int*>(R + 4)) * *reinterpret_cast<float*>(R + 8) +
            0.5f);
        *reinterpret_cast<int*>(R + 0xc) = Count;
        void** const Table =
            static_cast<void**>(operator new(static_cast<unsigned>(Count) << 2));
        *reinterpret_cast<void***>(R) = Table;

        // per puff (the count re-read every step).
        for (int I = 0; I < *reinterpret_cast<int*>(R + 0xc); ++I)
        {
            // a 0x58 block, +0x50 / +0x51 = 0 only on a non-null one.
            unsigned char* Puff = static_cast<unsigned char*>(operator new(0x58));
            if (Puff != nullptr)
            {
                Puff[0x50] = 0;
                Puff[0x51] = 0;
            }
            Table[I] = Puff;

            // Common random_inside_sphere(+0x100), then the routine
            // with float(+0xa8 * +0xac), the box +0x78 / +0x84, the weights +0x48 and +0x68.
            Vector const Sample = random_inside_sphere(*reinterpret_cast<float*>(R + 0x100));
            float const Scale = *reinterpret_cast<float*>(R + 0xa8) * *reinterpret_cast<float*>(R + 0xac);
            LocalNebulaCloudsFillPuff(Puff, Sample, Scale, *reinterpret_cast<Vector*>(R + 0x78),
                                      *reinterpret_cast<Vector*>(R + 0x84),
                                      reinterpret_cast<float*>(R + 0x48),
                                      *reinterpret_cast<int*>(R + 0x68));

            // the drift, random_inside_sphere(+0xb0), at +0x44.
            Vector const Drift = random_inside_sphere(*reinterpret_cast<float*>(R + 0xb0));
            *reinterpret_cast<float*>(Puff + 0x44) = Drift.x;
            *reinterpret_cast<float*>(Puff + 0x48) = Drift.y;
            *reinterpret_cast<float*>(Puff + 0x4c) = Drift.z;

            // +0xa0 FCOMP 0.0f, TEST AH,0x41 / JNZ -- <= 0 or unordered
            // gives +0x38 = -1.0f; otherwise float((double(-0.8f) (0x005e0df8) - rand * c *
            // double(0.7f) (0x005db0b8)) * +0x9c).
            if (!(*reinterpret_cast<float*>(R + 0xa0) > 0.0f))
            {
                *reinterpret_cast<float*>(Puff + 0x38) = -1.0f;
            }
            else
            {
                int const RPhase = rand();
                *reinterpret_cast<float*>(Puff + 0x38) = static_cast<float>(
                    (-0.800000011920929 -
                     static_cast<double>(RPhase) * kCloudsRandScale * 0.699999988079071) *
                    *reinterpret_cast<float*>(R + 0x9c));
            }
            // +0x50 = 1.
            Puff[0x50] = 1;
        }
    }

    // (thiscall, RET 4): the the routine body (no +0xec test), the the routine
    // factors inline, then the routine (which tests +0xec itself). No test of `this`.
    void LocalNebulaCloudsApplyDetail(FNebulaClouds* Clouds, float Detail)
    {
        LocalNebulaCloudsFreePuffTable(Clouds);
        CloudsDetailFactors(Clouds, Detail);
        LocalNebulaCloudsRebuildBits(Clouds);
    }

    // (.data, initialised 1, only reader 00550f51): puff sort direction.
    // Non-zero sorts the table by descending +0x54, zero by ascending.
    int G_CloudsSortDescending = 1;

    // (G_CloudsMath, cli_nebula.h) -- this TU's lazy 3DMathEngine (the routine),
    // shared with 0054cb06, 0055156e, 005519f3 and 00551b94.
    I3DMathEngine* AcquireCloudsMath()
    {
        if (G_CloudsMath == nullptr)
            G_CloudsMath = Acquire3DMathEngine();
        return G_CloudsMath;
    }

    // .rdata constants read by the routine (values from the PE).
    float const kCloudsCullSlack = 1.0199999809265137f;
    float const kCloudsExclusionDepth = 0.8999999761581421f;
    double const kCloudsDriftSpan = 0.699999988079071;
    double const kCloudsDriftBase = -0.800000011920929;
    double const kCloudsTwoPi = 6.2831854820251465;
    float const kCloudsDriftSpeed = 5.0f;

    // (__thiscall, RET 0x10): (camera, dt, visibility, exclusion manager).
    // Callers: the nebula update 005072e4 (visibility = nebula +0x294, manager = nebula +0x34)
    // and the TexturePanels update 0050b2b1 (visibility = panels +0x70, manager = null).
    // Puff (0x58): +0x00 position, +0x20 drift anchor, +0x2c drift direction, +0x38 drift
    // timer, +0x3c/+0x40 drift randoms, +0x44 velocity, +0x50 bActive, +0x51 bHidden,
    // +0x54 sort key.
    void LocalNebulaCloudsUpdate(FNebulaClouds* Clouds, void* Camera, float Dt, float Visibility,
                                 Exclusion::NebulaExclusionZoneMgr* ExclusionMgr)
    {
        unsigned char* const C = Clouds->Raw;
        // nothing to do until the puff table was built.
        if (C[0xec] == 0)
            return;

        // budget = ftol(visibility / +0x108 * count), clamped to [0, count].
        int const PuffCount = *reinterpret_cast<int*>(C + 0xc);
        int Budget = static_cast<int>(static_cast<double>(Visibility) /
                                      static_cast<double>(*reinterpret_cast<float*>(C + 0x108)) *
                                      static_cast<double>(PuffCount));
        *reinterpret_cast<int*>(C + 0x10) = Budget;
        if (Budget < 0)
            *reinterpret_cast<int*>(C + 0x10) = 0;
        else if (Budget > PuffCount)
            *reinterpret_cast<int*>(C + 0x10) = PuffCount;

        // fade. FCOMP +0x104 / TEST AH,0x41 / JNZ: only visibility above
        // +0x104 gives 1.0; then FCOMP 0.0 / TEST AH,0x41 / JP: an ordered visibility <= 0
        // gives 0.0; everything else (including unordered) divides.
        float const FullVisibility = *reinterpret_cast<float*>(C + 0x104);
        if (Visibility > FullVisibility)
            *reinterpret_cast<float*>(C + 0xe0) = 1.0f;
        else if (Visibility <= 0.0f)
            *reinterpret_cast<float*>(C + 0xe0) = 0.0f;
        else
            *reinterpret_cast<float*>(C + 0xe0) = Visibility / FullVisibility;

        // +0xb8 = +0xbc + R (R = +0x100), the cull threshold R*R*1.02,
        // and the local vector (0, 0, -(+0xbc + R)).
        ICamera* const Cam = static_cast<ICamera*>(Camera);
        float const Radius = *reinterpret_cast<float*>(C + 0x100);
        float const Ahead = *reinterpret_cast<float*>(C + 0xbc) + Radius;
        *reinterpret_cast<float*>(C + 0xb8) = Ahead;
        float CullDistSq = static_cast<float>(static_cast<double>(Radius) * Radius *
                                              static_cast<double>(kCloudsCullSlack));
        FVector AheadLocal;
        AheadLocal.X = 0.0f;
        AheadLocal.Y = 0.0f;
        AheadLocal.Z = -Ahead;

        // the cloud centre +0xf4 = camera transform (vt+0x10) applied to
        // that vector through 3DMathEngine vt+0x28 (TransformXform).
        FTransform const* const CamXf = reinterpret_cast<FTransform const*>(Cam->GetOrientation());
        FVector Centre;
        AcquireCloudsMath()->TransformXform(Centre, *CamXf, AheadLocal);
        *reinterpret_cast<float*>(C + 0xf4) = Centre.X;
        *reinterpret_cast<float*>(C + 0xf8) = Centre.Y;
        *reinterpret_cast<float*>(C + 0xfc) = Centre.Z;

        unsigned char** const Table = *reinterpret_cast<unsigned char***>(C);

        // pass 1 over every puff.
        int Active = 0;
        for (int I = 0; I < *reinterpret_cast<int*>(C + 0xc); ++I)
        {
            unsigned char* const Puff = Table[I];
            // FCOMP 0.0 / TEST AH,0x41 / JP -- an ordered fade <= 0
            // deactivates every puff.
            if (*reinterpret_cast<float*>(C + 0xe0) <= 0.0f)
            {
                Puff[0x50] = 0;
                Puff[0x51] = 1;
            }
            if (Puff[0x50] == 0)
                continue;
            // an active puff further than the cull radius from the centre
            // is deactivated (TEST AH,0x41 / JNZ keeps <= and unordered), the rest are counted.
            double const Dx = static_cast<double>(*reinterpret_cast<float*>(C + 0xf4)) -
                              *reinterpret_cast<float*>(Puff + 0x0);
            double const Dy = static_cast<double>(*reinterpret_cast<float*>(C + 0xf8)) -
                              *reinterpret_cast<float*>(Puff + 0x4);
            double const Dz = static_cast<double>(*reinterpret_cast<float*>(C + 0xfc)) -
                              *reinterpret_cast<float*>(Puff + 0x8);
            double const DistSq = Dz * Dz + Dy * Dy + Dx * Dx;
            if (DistSq > CullDistSq)
            {
                Puff[0x50] = 0;
                Puff[0x51] = 1;
            }
            else
                ++Active;
        }

        // the same local now holds +0xc8 squared; the drift pass below
        // lowers it to the nearest anchor distance. Nothing reads the result.
        float NearestDistSq = static_cast<float>(
            static_cast<double>(*reinterpret_cast<float*>(C + 0xc8)) *
            *reinterpret_cast<float*>(C + 0xc8));

        // bubble sort of the table by the float at +0x54, repeated until a
        // pass makes no swap. the field != 0: swap when left < right (TEST AH,0x5 / JP
        // skips >=, == and unordered); == 0: swap when left > right (TEST AH,0x41 / JNZ).
        for (;;)
        {
            if (*reinterpret_cast<int*>(C + 0xc) <= 1)
                break;
            int Swaps = 0;
            for (int J = 1; J < *reinterpret_cast<int*>(C + 0xc); ++J)
            {
                unsigned char* const Left = Table[J - 1];
                unsigned char* const Right = Table[J];
                float const L = *reinterpret_cast<float*>(Left + 0x54);
                float const R = *reinterpret_cast<float*>(Right + 0x54);
                bool const bSwap = G_CloudsSortDescending != 0 ? (L < R) : (L > R);
                if (bSwap)
                {
                    Table[J - 1] = Right;
                    ++Swaps;
                    Table[J] = Left;
                }
            }
            if (Swaps <= 0)
                break;
        }

        // pass 2 -- respawn, drift, move.
        for (int I = 0; I < *reinterpret_cast<int*>(C + 0xc); ++I)
        {
            // an inactive puff respawns while fewer than the budget are
            // active (signed JGE).
            if (Table[I][0x50] == 0 && Active < *reinterpret_cast<int*>(C + 0x10))
            {
                // on the sphere of radius R around the centre.
                float const SpawnRadius = *reinterpret_cast<float*>(C + 0x100);
                Vector const OnSphere = random_on_unit_sphere();
                float const Sz = static_cast<float>(static_cast<double>(SpawnRadius) * OnSphere.z);
                float* const Pos = reinterpret_cast<float*>(Table[I]);
                Pos[0] = static_cast<float>(static_cast<double>(SpawnRadius) * OnSphere.x +
                                            *reinterpret_cast<float*>(C + 0xf4));
                Pos[1] = static_cast<float>(static_cast<double>(SpawnRadius) * OnSphere.y +
                                            *reinterpret_cast<float*>(C + 0xf8));
                Pos[2] = static_cast<float>(static_cast<double>(Sz) +
                                            *reinterpret_cast<float*>(C + 0xfc));

                // velocity random_inside_sphere(+0xb0) -> +0x44.
                Vector const Vel = random_inside_sphere(*reinterpret_cast<float*>(C + 0xb0));
                *reinterpret_cast<float*>(Table[I] + 0x44) = Vel.x;
                *reinterpret_cast<float*>(Table[I] + 0x48) = Vel.y;
                *reinterpret_cast<float*>(Table[I] + 0x4c) = Vel.z;

                // FCOMP +0x10c / TEST AH,0x1 / JZ -- visibility not below
                // +0x10c accepts outright; otherwise the owner (+0xc4, null rejects) is asked
                // for the density at the new position (vt[0], __thiscall), and only a density
                // above 0.0 (TEST AH,0x41 / JNZ) accepts.
                bool bAccept = true;
                if (!(Visibility >= *reinterpret_cast<float*>(C + 0x10c)))
                {
                    Universe::IZoneImmersion const* const Owner = *(Universe::IZoneImmersion const**)(C + 0xc4);
                    if (Owner == nullptr)
                        bAccept = false;
                    else
                    {
                        using FDensity = float(__thiscall*)(void*, float const*);
                        float const Density =
                            (float)Owner->GetImmersion(*(Vector const*)Pos);
                        if (!(static_cast<double>(Density) > 0.0))
                            bAccept = false;
                    }
                }

                if (bAccept)
                {
                    // activate; inside an exclusion zone deeper than 0.9
                    // (TEST AH,0x41 / JNZ keeps <= and unordered) it stays hidden.
                    ++Active;
                    Table[I][0x50] = 1;
                    Table[I][0x51] = 0;
                    if (ExclusionMgr != nullptr)
                    {
                        Exclusion::NebulaExclusionZone* HitZone;
                        float Depth = -1.0f;
                        if (ExclusionMgr->is_in_exclusion_zone(
                                *reinterpret_cast<Vector const*>(Table[I]), &HitZone, &Depth,
                                true) &&
                            Depth > kCloudsExclusionDepth)
                            Table[I][0x51] = 1;
                    }
                }
            }

            // only active, visible puffs drift and move.
            unsigned char* const Puff = Table[I];
            if (Puff[0x50] == 0 || Puff[0x51] != 0)
                continue;

            float* const Timer = reinterpret_cast<float*>(Puff + 0x38);
            float const Period = *reinterpret_cast<float*>(C + 0xa0);
            // TEST AH,0x41 / JNZ -- no drift unless the period is above 0.
            if (Period > 0.0f)
            {
                // the timer advances by dt; the branch is taken on the OLD
                // value (FCOMP before the FADD; TEST AH,0x1 / JNZ sends < 0 and unordered to
                // ).
                float const OldTimer = *Timer;
                *Timer = static_cast<float>(static_cast<double>(Dt) + OldTimer);
                if (OldTimer >= 0.0f)
                {
                    // with +0xc8 above 0, the squared distance of the drift
                    // anchor (+0x20) from the camera lowers NearestDistSq (TEST AH,0x5 / JP
                    // keeps >= and unordered).
                    if (*reinterpret_cast<float*>(C + 0xc8) > 0.0f)
                    {
                        float const* const CamPos = Cam->GetPosition();
                        double const Ax = static_cast<double>(*reinterpret_cast<float*>(Puff + 0x20)) - CamPos[0];
                        double const Ay = static_cast<double>(*reinterpret_cast<float*>(Puff + 0x24)) - CamPos[1];
                        double const Az = static_cast<double>(*reinterpret_cast<float*>(Puff + 0x28)) - CamPos[2];
                        double const AnchorDistSq = Az * Az + Ay * Ay + Ax * Ax;
                        if (AnchorDistSq < NearestDistSq)
                            NearestDistSq = static_cast<float>(AnchorDistSq);
                    }
                    // past the period (TEST AH,0x41 / JNZ keeps <=), the
                    // timer restarts negative: (-0.8 - rand/32768 * 0.7) * +0x9c.
                    if (*Timer > Period)
                    {
                        int const R = rand();
                        *Timer = static_cast<float>(
                            (kCloudsDriftBase -
                             static_cast<double>(R) * kCloudsRandScale * kCloudsDriftSpan) *
                            *reinterpret_cast<float*>(C + 0x9c));
                    }
                }
                else if (*Timer >= 0.0f)
                {
                    // the timer just crossed zero. Anchor = position; the
                    // drift direction is a random in-plane direction (cos, sin, 0) of
                    // LookMatrix(anchor - camera), via 3DMathEngine vt+0x2c, scaled to length
                    // 5.0; +0x40 = rand/32768.
                    *reinterpret_cast<float*>(Puff + 0x20) = *reinterpret_cast<float*>(Puff + 0x0);
                    *reinterpret_cast<float*>(Puff + 0x24) = *reinterpret_cast<float*>(Puff + 0x4);
                    *reinterpret_cast<float*>(Puff + 0x28) = *reinterpret_cast<float*>(Puff + 0x8);
                    float const* const CamPos = Cam->GetPosition();
                    Vector ToAnchor;
                    ToAnchor.x = *reinterpret_cast<float*>(Puff + 0x20) - CamPos[0];
                    ToAnchor.y = *reinterpret_cast<float*>(Puff + 0x24) - CamPos[1];
                    ToAnchor.z = *reinterpret_cast<float*>(Puff + 0x28) - CamPos[2];
                    Matrix const Look = LookMatrix(ToAnchor);

                    int const RAngle = rand();
                    double const Angle = static_cast<double>(RAngle) * kCloudsRandScale * kCloudsTwoPi;
                    FVector InPlane;
                    InPlane.X = static_cast<float>(cos(Angle));
                    InPlane.Y = static_cast<float>(sin(Angle));
                    InPlane.Z = 0.0f;
                    FVector Dir;
                    AcquireCloudsMath()->TransformMatrix(
                        Dir, *reinterpret_cast<FMatrix const*>(&Look), InPlane);
                    float* const DriftDir = reinterpret_cast<float*>(Puff + 0x2c);
                    DriftDir[0] = Dir.X;
                    DriftDir[1] = Dir.Y;
                    DriftDir[2] = Dir.Z;
                    double const Len = sqrt(static_cast<double>(DriftDir[0]) * DriftDir[0] +
                                            static_cast<double>(DriftDir[1]) * DriftDir[1] +
                                            static_cast<double>(DriftDir[2]) * DriftDir[2]);
                    double const Scale = static_cast<double>(kCloudsDriftSpeed) / Len;
                    DriftDir[0] = static_cast<float>(Scale * DriftDir[0]);
                    DriftDir[1] = static_cast<float>(Scale * DriftDir[1]);
                    DriftDir[2] = static_cast<float>(Scale * DriftDir[2]);

                    int const RPhase = rand();
                    *reinterpret_cast<float*>(Puff + 0x40) =
                        static_cast<float>(static_cast<double>(RPhase) * kCloudsRandScale);
                }

                // while the timer is not negative (TEST AH,0x1 / JNZ skips
                // < and unordered), +0x3c = rand * (rand/32768) / 32768.
                if (*Timer >= 0.0f)
                {
                    int const RA = rand();
                    float const A = static_cast<float>(static_cast<double>(RA) * kCloudsRandScale);
                    int const RB = rand();
                    *reinterpret_cast<float*>(Puff + 0x3c) = static_cast<float>(
                        static_cast<double>(RB) * A * kCloudsRandScale);
                }
            }

            // position += dt * velocity.
            float* const Pos = reinterpret_cast<float*>(Puff);
            float const StepZ = static_cast<float>(
                static_cast<double>(Dt) * *reinterpret_cast<float*>(Puff + 0x4c));
            Pos[0] = static_cast<float>(
                static_cast<double>(Dt) * *reinterpret_cast<float*>(Puff + 0x44) + Pos[0]);
            Pos[1] = static_cast<float>(
                static_cast<double>(Dt) * *reinterpret_cast<float*>(Puff + 0x48) + Pos[1]);
            Pos[2] = static_cast<float>(static_cast<double>(StepZ) + Pos[2]);
        }

        // .
        *reinterpret_cast<int*>(C + 0x14) = Active;
    }

    // (thiscall, RET 4, arg the nebula's exclusion manager): re-seed every puff of
    // the table around the clouds centre (+0xf4..+0xfc). No test of `this`, the table or a puff.
    void LocalNebulaCloudsRegenExcluded(FNebulaClouds* Clouds, void* ExclusionCtx)
    {
        unsigned char* const R = Clouds->Raw;
        Exclusion::NebulaExclusionZoneMgr* const ExclMgr =
            static_cast<Exclusion::NebulaExclusionZoneMgr*>(ExclusionCtx);
        // for i < +0x0c (re-read each step).
        for (int I = 0; I < *reinterpret_cast<int*>(R + 0xc); ++I)
        {
            unsigned char* const Puff = static_cast<unsigned char*>((*reinterpret_cast<void***>(R))[I]);
            // / the branch: past the active count (+0x10) -> hidden, flagged.
            if (I >= *reinterpret_cast<int*>(R + 0x10))
            {
                Puff[0x50] = 0;
                Puff[0x51] = 1;
                continue;
            }

            // position = centre + random_inside_sphere(+0x100) (float
            // sums), drift = random_inside_sphere(+0xb0).
            Vector const Sample = random_inside_sphere(*reinterpret_cast<float*>(R + 0x100));
            *reinterpret_cast<float*>(Puff + 0x0) = *reinterpret_cast<float*>(R + 0xf4) + Sample.x;
            *reinterpret_cast<float*>(Puff + 0x4) = *reinterpret_cast<float*>(R + 0xf8) + Sample.y;
            *reinterpret_cast<float*>(Puff + 0x8) = *reinterpret_cast<float*>(R + 0xfc) + Sample.z;
            Vector const Drift = random_inside_sphere(*reinterpret_cast<float*>(R + 0xb0));
            *reinterpret_cast<float*>(Puff + 0x44) = Drift.x;
            *reinterpret_cast<float*>(Puff + 0x48) = Drift.y;
            *reinterpret_cast<float*>(Puff + 0x4c) = Drift.z;

            // the zone (+0xc4; null counts as outside) slot 0 on the
            // position, FCOMP 0.0, TEST AH,0x41 / JNZ -- only strictly above 0 is inside.
            Universe::IZone const* const Zone = *reinterpret_cast<Universe::IZone**>(R + 0xc4);
            if (Zone == nullptr ||
                !(Zone->GetImmersion(*reinterpret_cast<Vector const*>(Puff)) > 0.0))
            {
                // .
                Puff[0x50] = 0;
                Puff[0x51] = 1;
                continue;
            }

            // shown and not flagged; with a manager, a hit whose distance
            // is strictly above 0.9f (0x005e0e00; TEST AH,0x41 / JNZ) flags it again.
            Puff[0x50] = 1;
            Puff[0x51] = 0;
            if (ExclMgr != nullptr)
            {
                Exclusion::NebulaExclusionZone* HitZone = nullptr;
                float HitDist = -1.0f;
                if (ExclMgr->is_in_exclusion_zone(*reinterpret_cast<Vector*>(Puff), &HitZone,
                                                  &HitDist, true) &&
                    HitDist > 0.9f)
                {
                    Puff[0x51] = 1;
                }
            }
        }
    }

    // (thiscall, RET 4): the routine, then free when bit 0 of the flags is set.
    // No test of `this`.
    void LocalNebulaCloudsDestroy(FNebulaClouds* Clouds, unsigned char Flags)
    {
        LocalNebulaCloudsReleaseBits(Clouds);
        if ((Flags & 1u) != 0)
            operator delete(Clouds);
    }

    // (thiscall, RET 8) -- the [Clouds] section: (reader, the owner's ShapeDB --
    // nebula +0x04 at 0x0050545c, TexturePanels +0x04 at 0x0050a75d). No test of `this`.
    void LocalNebulaCloudsParseIni(FNebulaClouds* Clouds, INI_Reader& Reader, FShapeDB* Shapes)
    {
        unsigned char* const R = Clouds->Raw;

        // an existing puff table is freed (the routine), +0x04 and
        // +0xec cleared.
        if (R[0xec] != 0)
        {
            LocalNebulaCloudsFreePuffTable(Clouds);
            *reinterpret_cast<int*>(R + 4) = 0;
            R[0xec] = 0;
        }

        // the defaults (+0x11c, +0x128, +0x90..+0x98 and +0x18..+0x44
        // are left as they are).
        *reinterpret_cast<int*>(R + 0x114) = 0;
        *reinterpret_cast<int*>(R + 0x110) = 0;
        *reinterpret_cast<int*>(R + 0x124) = 0;
        *reinterpret_cast<int*>(R + 0x120) = 0;
        *reinterpret_cast<int*>(R + 0x118) = 0;
        *reinterpret_cast<int*>(R + 0x74) = 0;
        *reinterpret_cast<float*>(R + 0x6c) = 1.0f;
        *reinterpret_cast<float*>(R + 0x70) = 1.0f;
        {
            // (+0x6c, +0x70, +0x74) normalised -- 1.0f / sqrt((x^2 + y^2)
            // + z^2) on the FPU, no zero test.
            float* const N = reinterpret_cast<float*>(R + 0x6c);
            double const Inv = 1.0f / sqrt((static_cast<double>(N[0]) * N[0] +
                                            static_cast<double>(N[1]) * N[1]) +
                                           static_cast<double>(N[2]) * N[2]);
            N[0] = static_cast<float>(Inv * N[0]);
            N[1] = static_cast<float>(Inv * N[1]);
            N[2] = static_cast<float>(Inv * N[2]);
        }
        *reinterpret_cast<float*>(R + 0xa4) = 1.0f;
        *reinterpret_cast<float*>(R + 0xdc) = 1.0f;
        *reinterpret_cast<float*>(R + 0xa8) = 50.0f;
        *reinterpret_cast<float*>(R + 0xb4) = 0.75f;
        *reinterpret_cast<int*>(R + 0xa0) = 0;
        *reinterpret_cast<int*>(R + 0xb0) = 0;
        *reinterpret_cast<float*>(R + 0xc0) = 1500.0f;
        *reinterpret_cast<int*>(R + 0xc8) = 0;
        *reinterpret_cast<int*>(R + 0x68) = 0;
        for (int I = 0; I < 8; ++I)
            reinterpret_cast<float*>(R + 0x48)[I] = 1.0f; // REP STOSD with 0x3f800000
        *reinterpret_cast<float*>(R + 0xe4) = 50.0f;
        *reinterpret_cast<float*>(R + 0xe8) = 100.0f;
        *reinterpret_cast<float*>(R + 0x104) = BitsToFloat(0x3dcccccd);
        *reinterpret_cast<float*>(R + 0x108) = BitsToFloat(0x3e99999a);
        *reinterpret_cast<float*>(R + 0x10c) = BitsToFloat(0x3e99999a);
        *reinterpret_cast<int*>(R + 0xf0) = 0;

        // [ESP+0x14]: how many puff_colora / puff_colorb keys were taken (at most two).
        int ColorCount = 0;
        while (Reader.read_value())
        {
            if (Reader.is_value("puff_count"))
            {
                *reinterpret_cast<int*>(R + 4) = static_cast<int>(Reader.get_value_float(0));
            }
            else if (Reader.is_value("puff_radius"))
            {
                *reinterpret_cast<float*>(R + 0xa8) = Reader.get_value_float(0);
            }
            else if (Reader.is_value("puff_colora") || Reader.is_value("puff_colorb"))
            {
                // either name fills the next of the two colour slots (+0x78,
                // +0x84) in the order the keys come, as v / 255; the first one also seeds the
                // second slot. A third is ignored.
                if (ColorCount < 2)
                {
                    float Color[3];
                    ReadColor255(Reader, Color);
                    float* const Dest = reinterpret_cast<float*>(R + 0x78 + ColorCount * 0xc);
                    Dest[0] = Color[0];
                    Dest[1] = Color[1];
                    Dest[2] = Color[2];
                    if (ColorCount == 0)
                    {
                        float* const Second = reinterpret_cast<float*>(R + 0x84);
                        Second[0] = Dest[0];
                        Second[1] = Dest[1];
                        Second[2] = Dest[2];
                    }
                    ++ColorCount;
                }
            }
            else if (Reader.is_value("puff_shape"))
            {
                // at most eight (+0x68). A miss is the ShapeDB warning
                // and then this file's own error (SpaceTerrainRenderComp.cpp:0x523, the value
                // string re-read and the reader's file name); the null shape is stored anyway.
                if (*reinterpret_cast<int*>(R + 0x68) < 8)
                {
                    char const* const Name = Reader.get_value_string();
                    FTexShape* const Shape = Shapes->Find(Name);
                    if (Shape == nullptr)
                    {
                        char const* const File = Reader.get_file_name();
                        FDUMP(0x100001,
                              "%s(%d) : *** ERROR: Texture Shape (%s) not found for [Clouds] in "
                              "file (%s)",
                              "Client\\Space\\SpaceTerrainRenderComp.cpp",
                              0x523, Reader.get_value_string(), File);
                    }
                    reinterpret_cast<FTexShape**>(R + 0x28)[*reinterpret_cast<int*>(R + 0x68)] =
                        Shape;
                    ++*reinterpret_cast<int*>(R + 0x68);
                }
            }
            else if (Reader.is_value("puff_weights"))
            {
                for (int I = 0; I < *reinterpret_cast<int*>(R + 0x68); ++I)
                    reinterpret_cast<float*>(R + 0x48)[I] = Reader.get_value_float(I);
            }
            else if (Reader.is_value("puff_opacity"))
            {
                // recognised and ignored.
            }
            else if (Reader.is_value("puff_max_alpha"))
            {
                *reinterpret_cast<float*>(R + 0xb4) = Reader.get_value_float(0);
            }
            else if (Reader.is_value("puff_drift"))
            {
                *reinterpret_cast<float*>(R + 0xb0) = Reader.get_value_float(0);
            }
            else if (Reader.is_value("lightning_shape"))
            {
                // four shapes (+0x18..+0x24), value i copied into a
                // 32-byte buffer (the routine, at most 31 characters; a null value is empty)
                // and found through the cached ShapeDB find; a miss stores null.
                for (int I = 0; I < 4; ++I)
                {
                    char Buf[0x20];
                    Buf[0] = '\0';
                    char const* const Src = Reader.get_value_string(static_cast<unsigned>(I));
                    if (Src != nullptr)
                    {
                        size_t Len = strlen(Src);
                        if (Len >= sizeof(Buf))
                            Len = sizeof(Buf) - 1;
                        memcpy(Buf, Src, Len);
                        Buf[Len] = '\0';
                    }
                    reinterpret_cast<FTexShape**>(R + 0x18)[I] = Shapes->Find(Buf);
                }
            }
            else if (Reader.is_value("lightning_color"))
            {
                ReadColor255(Reader, reinterpret_cast<float*>(R + 0x90));
            }
            else if (Reader.is_value("lightning_gap"))
            {
                *reinterpret_cast<float*>(R + 0x9c) = Reader.get_value_float(0);
            }
            else if (Reader.is_value("lightning_duration"))
            {
                *reinterpret_cast<float*>(R + 0xa0) = Reader.get_value_float(0);
            }
            else if (Reader.is_value("lightning_size"))
            {
                *reinterpret_cast<float*>(R + 0xa4) = Reader.get_value_float(0);
            }
            else if (Reader.is_value("llight_range"))
            {
                *reinterpret_cast<float*>(R + 0xc8) = Reader.get_value_float(0);
            }
            else if (Reader.is_value("llight_color"))
            {
                ReadColor255(Reader, reinterpret_cast<float*>(R + 0xcc));
            }
            else if (Reader.is_value("lightning_intensity"))
            {
                *reinterpret_cast<float*>(R + 0xdc) = Reader.get_value_float(0);
            }
            else if (Reader.is_value("max_distance"))
            {
                *reinterpret_cast<float*>(R + 0xc0) = Reader.get_value_float(0);
            }
            else if (Reader.is_value("near_fade_distance"))
            {
                // +0xe4 / +0xe8; a second value strictly below the first
                // (FCOMP, TEST AH,5 / JP) warns and swaps the two.
                *reinterpret_cast<float*>(R + 0xe4) = Reader.get_value_float(0);
                *reinterpret_cast<float*>(R + 0xe8) = Reader.get_value_float(1);
                if (*reinterpret_cast<float*>(R + 0xe8) < *reinterpret_cast<float*>(R + 0xe4))
                {
                    FDUMP(0x100002, "%s(%d) : *** WARNING: %s",
                          "Client\\Space\\SpaceTerrainRenderComp.cpp",
                          0x555, "near_fade_distances are flipped");
                    float const Far = *reinterpret_cast<float*>(R + 0xe8);
                    *reinterpret_cast<float*>(R + 0xe8) = *reinterpret_cast<float*>(R + 0xe4);
                    *reinterpret_cast<float*>(R + 0xe4) = Far;
                }
            }
            else if (Reader.is_value("no_cloud_blend"))
            {
                *reinterpret_cast<int*>(R + 0x110) = static_cast<int>(Reader.get_value_float(0));
            }
            else if (Reader.is_value("zwrite_clouds"))
            {
                *reinterpret_cast<int*>(R + 0x114) = static_cast<int>(Reader.get_value_float(0));
            }
            else if (Reader.is_value("disable_clouds"))
            {
                *reinterpret_cast<int*>(R + 0x118) = static_cast<int>(Reader.get_value_float(0));
            }
            else if (Reader.is_value("disable_wisps"))
            {
                *reinterpret_cast<int*>(R + 0x11c) = static_cast<int>(Reader.get_value_float(0));
            }
            else if (Reader.is_value("disable_lightning_flash"))
            {
                *reinterpret_cast<int*>(R + 0x124) = static_cast<int>(Reader.get_value_float(0));
            }
            else if (Reader.is_value("disable_lightning_bolts"))
            {
                *reinterpret_cast<int*>(R + 0x120) = static_cast<int>(Reader.get_value_float(0));
            }
            else if (Reader.is_value("disable_batcher"))
            {
                *reinterpret_cast<int*>(R + 0x128) = static_cast<int>(Reader.get_value_float(0));
            }
        }

        // +0xec = 1; the weights normalised -- the sum on the FPU, each
        // = float((1.0f / sum) * weight), no zero test, the count re-read each step.
        R[0xec] = 1;
        {
            float* const Weights = reinterpret_cast<float*>(R + 0x48);
            int const WeightCount = *reinterpret_cast<int*>(R + 0x68);
            double Sum = 0.0f;
            for (int I = 0; I < WeightCount; ++I)
                Sum += Weights[I];
            if (WeightCount > 0)
            {
                double const InvSum = 1.0f / Sum;
                for (int I = 0; I < *reinterpret_cast<int*>(R + 0x68); ++I)
                    Weights[I] = static_cast<float>(InvSum * Weights[I]);
            }
        }

        // +0x0c = +0x10 = +0x04; +0x100 = float(+0xc0 * 0.5f + +0xa8);
        // .
        *reinterpret_cast<int*>(R + 0xc) = *reinterpret_cast<int*>(R + 4);
        *reinterpret_cast<int*>(R + 0x10) = *reinterpret_cast<int*>(R + 4);
        *reinterpret_cast<float*>(R + 0x100) = static_cast<float>(
            static_cast<double>(*reinterpret_cast<float*>(R + 0xc0)) * 0.5f +
            *reinterpret_cast<float*>(R + 0xa8));
        LocalNebulaCloudsRebuildBits(Clouds);

        // +0xbc, the near-fade reach. Only a +0xe4 strictly above 0
        // (TEST AH,0x41 / JNZ) computes it; otherwise +0xbc = 0.
        float const NearFade = *reinterpret_cast<float*>(R + 0xe4);
        if (!(NearFade > 0.0f))
        {
            *reinterpret_cast<int*>(R + 0xbc) = 0;
            return;
        }
        // T = tan(double 0.6108652551968893 (0x005e0be0)); A = float(+0x100 * 0.8f / T -
        // +0xe4); B = (T * +0xe4 + +0xa8 * 1.2f) * 1.25f on the FPU.
        double const Tan = tan(0.6108652551968893);
        double const Reach = *reinterpret_cast<float*>(R + 0x100);
        float const A = static_cast<float>(Reach * 0.8f / Tan - NearFade);
        double const BReach =
            (Tan * NearFade + static_cast<double>(*reinterpret_cast<float*>(R + 0xa8)) * 1.2f) *
            1.25f;
        // FCOM +0x100, TEST AH,0x41 / JNZ: B strictly above the reach takes A; otherwise
        // sqrt(reach^2 - B^2).
        double X;
        if (BReach > Reach)
            X = A;
        else
            X = sqrt(Reach * Reach - BReach * BReach);
        // FCOM A, TEST AH,0x41 / JNZ: X strictly above A becomes A.
        if (X > A)
            X = A;
        // +0xbc = float(X + +0xe4 - +0x100); FCOMP 0.0, TEST AH,5 / JP: an ordered "less"
        // becomes 0.
        double const Result = (X + NearFade) - Reach;
        *reinterpret_cast<float*>(R + 0xbc) = static_cast<float>(Result);
        if (Result < 0.0)
            *reinterpret_cast<int*>(R + 0xbc) = 0;
    }

    // 's clouds-list insert, 0x00505477..0x005054ac: the routine buys a node linked
    // in front of the sentinel (+0xf8), the routine stores the value and the size (+0xfc) goes
    // up. No tests.
    void CloudsListPush(FNebulaEffect* Nebula, FNebulaClouds* Clouds)
    {
        FNebulaCloudsListNode* const Sentinel = Nebula->Clouds.Sentinel;
        FNebulaCloudsListNode* const Node =
            static_cast<FNebulaCloudsListNode*>(operator new(sizeof(FNebulaCloudsListNode)));
        Node->Next = Sentinel;
        Node->Prev = Sentinel->Prev;
        Sentinel->Prev = Node;
        Node->Prev->Next = Node;
        Node->Clouds = Clouds;
        ++Nebula->Clouds.Count;
    }

    // 's [Exterior] section, 0x00504fd3..0x005053ff (the branch is a label inside
    // the bit_radius_random_variation clamp).
    void ParseExteriorSection(FNebulaEffect* Nebula, INI_Reader& Reader)
    {
        unsigned char* const B = reinterpret_cast<unsigned char*>(Nebula);
        while (Reader.read_value())
        {
            if (Reader.is_value("bit_radius"))
            {
                *reinterpret_cast<float*>(B + 0xa4) = Reader.get_value_float(0);
            }
            else if (Reader.is_value("color"))
            {
                // +0xa8..+0xb0.
                ReadColor255(Reader, reinterpret_cast<float*>(B + 0xa8));
            }
            else if (Reader.is_value("shape"))
            {
                // at most eight shapes (+0x60[+0xa0]); the ShapeDB (+0x04)
                // find warns and yields null on a miss; the count goes up either way.
                if (*reinterpret_cast<int*>(B + 0xa0) < 8)
                {
                    char const* const Name = Reader.get_value_string();
                    FTexShape* const Shape = NebulaShapes(Nebula)->Find(Name);
                    reinterpret_cast<FTexShape**>(B + 0x60)[*reinterpret_cast<int*>(B + 0xa0)] =
                        Shape;
                    ++*reinterpret_cast<int*>(B + 0xa0);
                }
            }
            else if (Reader.is_value("shape_weights"))
            {
                // one weight per shape read so far (+0x80[i]).
                for (int I = 0; I < *reinterpret_cast<int*>(B + 0xa0); ++I)
                    reinterpret_cast<float*>(B + 0x80)[I] = Reader.get_value_float(I);
            }
            else if (Reader.is_value("fill_shape"))
            {
                // the fill shape at +0x5c, the same find.
                char const* const Name = Reader.get_value_string();
                *reinterpret_cast<FTexShape**>(B + 0x5c) = NebulaShapes(Nebula)->Find(Name);
            }
            else if (Reader.is_value("plane_slices"))
            {
                *reinterpret_cast<int*>(B + 0xbc) = static_cast<int>(Reader.get_value_float(0));
            }
            else if (Reader.is_value("min_bits"))
            {
                *reinterpret_cast<int*>(B + 0xc0) = static_cast<int>(Reader.get_value_float(0));
            }
            else if (Reader.is_value("max_bits"))
            {
                *reinterpret_cast<int*>(B + 0xc4) = static_cast<int>(Reader.get_value_float(0));
            }
            else if (Reader.is_value("bit_radius_random_variation"))
            {
                // FCOM double 0.9999 (0x005dafa8), TEST AH,0x41 / JNZ --
                // only a value strictly above it becomes 0.9999f (0x005dafa4); then FCOM 0.0,
                // TEST AH,5 / JP -- only an ordered "less" becomes 0.0f. +0xc8 = 1 - v,
                // +0xcc = v + 1.
                float V = Reader.get_value_float(0);
                if (static_cast<double>(V) > 0.9999)
                    V = 0.9999f;
                if (static_cast<double>(V) < 0.0)
                    V = 0.0f;
                *reinterpret_cast<float*>(B + 0xc8) = 1.0f - V;
                *reinterpret_cast<float*>(B + 0xcc) = V + 1.0f;
            }
            else if (Reader.is_value("equator_bias"))
            {
                // FSUBR double 1.0.
                *reinterpret_cast<float*>(B + 0xd0) =
                    static_cast<float>(1.0 - static_cast<double>(Reader.get_value_float(0)));
            }
            else if (Reader.is_value("move_bit_percent"))
            {
                *reinterpret_cast<float*>(B + 0xd4) = Reader.get_value_float(0);
            }
            else if (Reader.is_value("opacity"))
            {
                float const V = Reader.get_value_float(0);
                *reinterpret_cast<float*>(B + 0xb4) = V;
                *reinterpret_cast<float*>(B + 0xb8) = V;
            }
        }
    }

    // 's [Fog] section, 0x005054ed..0x00505652 ([ObjectFog] is recognised and skipped
    // before it).
    void ParseFogSection(FNebulaEffect* Nebula, INI_Reader& Reader)
    {
        unsigned char* const B = reinterpret_cast<unsigned char*>(Nebula);
        while (Reader.read_value())
        {
            if (Reader.is_value("distance"))
            {
                Nebula->FogDistance = Reader.get_value_float(0);
            }
            else if (Reader.is_value("near"))
            {
                Nebula->FogNear = Reader.get_value_float(0);
            }
            else if (Reader.is_value("color"))
            {
                // +0x2a0..+0x2a8, then each FCOMP 1.0f, TEST AH,5 / JP --
                // only a value below 1.0 is kept, anything else (>=, NaN) becomes 1.0f.
                float* const Color = reinterpret_cast<float*>(B + 0x2a0);
                ReadColor255(Reader, Color);
                for (int I = 0; I < 3; ++I)
                    Color[I] = (Color[I] < 1.0f) ? Color[I] : 1.0f;
            }
        }
    }

    // 's [BackgroundLightning] section, 0x00505712..0x0050585e.
    void ParseBackgroundLightningSection(FNebulaEffect* Nebula, INI_Reader& Reader)
    {
        unsigned char* const B = reinterpret_cast<unsigned char*>(Nebula);
        while (Reader.read_value())
        {
            if (Reader.is_value("gap"))
            {
                // +0x250 = gap, +0x258 = -gap.
                float const Gap = Reader.get_value_float(0);
                *reinterpret_cast<float*>(B + 0x250) = Gap;
                *reinterpret_cast<float*>(B + 0x258) = -Gap;
            }
            else if (Reader.is_value("duration"))
            {
                *reinterpret_cast<float*>(B + 0x254) = Reader.get_value_float(0);
            }
            else if (Reader.is_value("color"))
            {
                ReadColor255(Reader, reinterpret_cast<float*>(B + 0x25c));
            }
            else if (Reader.is_value("intensity"))
            {
                *reinterpret_cast<float*>(B + 0x268) = Reader.get_value_float(0);
            }
        }
    }

    // 's [DynamicLightning] section, 0x00505879..0x005059c9.
    void ParseDynamicLightningSection(FNebulaEffect* Nebula, INI_Reader& Reader)
    {
        unsigned char* const B = reinterpret_cast<unsigned char*>(Nebula);
        while (Reader.read_value())
        {
            if (Reader.is_value("gap"))
                *reinterpret_cast<float*>(B + 0x274) = Reader.get_value_float(0);
            else if (Reader.is_value("duration"))
                *reinterpret_cast<float*>(B + 0x278) = Reader.get_value_float(0);
            else if (Reader.is_value("color"))
                ReadColor255(Reader, reinterpret_cast<float*>(B + 0x280));
            else if (Reader.is_value("ambient_intensity"))
                *reinterpret_cast<float*>(B + 0x28c) = Reader.get_value_float(0);
            else if (Reader.is_value("intensity_increase"))
                *reinterpret_cast<float*>(B + 0x290) = Reader.get_value_float(0);
        }
    }

    // 's [NebulaLight] section, 0x005059e4..0x00505abc.
    void ParseNebulaLightSection(FNebulaEffect* Nebula, INI_Reader& Reader)
    {
        unsigned char* const B = reinterpret_cast<unsigned char*>(Nebula);
        while (Reader.read_value())
        {
            if (Reader.is_value("ambient"))
            {
                // +0x2cc = 1, then the three values truncated (ftol)
                // into the bytes +0x2cd..+0x2cf.
                B[0x2cc] = 1;
                B[0x2cd] = static_cast<unsigned char>(static_cast<int>(Reader.get_value_float(0)));
                B[0x2ce] = static_cast<unsigned char>(static_cast<int>(Reader.get_value_float(1)));
                B[0x2cf] = static_cast<unsigned char>(static_cast<int>(Reader.get_value_float(2)));
            }
            else if (Reader.is_value("sun_burnthrough_intensity"))
            {
                *reinterpret_cast<float*>(B + 0x2c4) = Reader.get_value_float(0);
            }
            else if (Reader.is_value("sun_burnthrough_scaler"))
            {
                *reinterpret_cast<float*>(B + 0x2c8) = Reader.get_value_float(0);
            }
        }
    }

    // 's [Exclusion Zones] section, 0x00505ac1..0x00505ff0. Two locals carry across
    // the keys: the number of zones this section added (EBX, reset at 0x00505b08) gates every
    // key but "exclusion", and the current zone_shell (ESI, reset there too) receives
    // shell_scalar / max_alpha / exclusion_tint.
    void ParseExclusionZonesSection(FNebulaEffect* Nebula, INI_Reader& Reader, unsigned SystemId)
    {
        // the manager is created once per nebula.
        if (Nebula->ExclusionMgr == nullptr)
            Nebula->ExclusionMgr = new Exclusion::NebulaExclusionZoneMgr();

        int ZonesAdded = 0;
        FNebulaZoneShell* Shell = nullptr;
        while (Reader.read_value())
        {
            if (Reader.is_value("exclusion"))
            {
                // an unknown zone is an FDUMP and nothing else.
                char const* const Nick = Reader.get_value_string();
                Universe::IZone const* const Z = Universe::get_zone(SystemId, Nick);
                if (Z == nullptr)
                {
                    FDUMP(0x100001,
                          "%s(%d) : *** ERROR: AsteroidField linked to missing Exclusion Zone (%s)",
                          "Client\\Space\\Nebula.cpp", 0x2b2, Nick);
                    continue;
                }
                Nebula->ExclusionMgr->push_back(Z, Nebula->Zone);
                ++ZonesAdded;
            }
            else if (Reader.is_value("fog_far"))
            {
                // FSTP [back + 0x8].
                if (ZonesAdded == 0)
                    continue;
                Exclusion::NebulaExclusionZone* const Back = Nebula->ExclusionMgr->back();
                Back->FogFar = Reader.get_value_float(0);
            }
            else if (Reader.is_value("zone_shell"))
            {
                // the current shell is dropped first, whatever follows.
                Shell = nullptr;
                if (ZonesAdded == 0)
                    continue;
                Exclusion::NebulaExclusionZone* const Back = Nebula->ExclusionMgr->back();
                char const* const Name = Reader.get_value_string();
                Back->set_zone_shell_name(Name);
                unsigned const Key = CreateID(Name);

                // a mesh this nebula already loaded is reused.
                FNebulaZoneShellMap& Shells = NebulaZoneShells(Nebula);
                FNebulaZoneShellMap::iterator const Found = NebulaFindZoneShell(Shells, Key);
                if (Found != Shells.end())
                {
                    Shell = Found->second;
                    continue;
                }

                // a new 0x38 shell, the ObjectHandle ctor and seven
                // 1.0f.
                FNebulaZoneShell* const NewShell = new FNebulaZoneShell;
                for (int I = 0; I < 5; ++I)
                    NewShell->UserData[I] = 1.0f;
                NewShell->ShellScalar = 1.0f;
                NewShell->MaxAlpha = 1.0f;

                // the routine(Name, 0); on failure the shell is
                // destroyed (the routine, the routine, free) and there is no current shell.
                if (!ClientMeshHolderLoad(&NewShell->Handle, Name, nullptr))
                {
                    NebulaZoneShellDestroy(NewShell);
                    delete NewShell;
                    continue;
                }

                // +0x10 becomes the instance's user data, and its first
                // three floats are set to 1.0f again.
                NeuronRuntime::Engine->SetInstanceUserData(NewShell->Handle.EngineInstance,
                                                   &NewShell->UserData[0]);
                NewShell->UserData[0] = 1.0f;
                NewShell->UserData[1] = 1.0f;
                NewShell->UserData[2] = 1.0f;

                // a unique insert keyed by the zone's shell id (the
                // same CreateID), result discarded.
                Shells.insert(FNebulaZoneShellMap::value_type(Back->ZoneShellName.Id, NewShell));
                Shell = NewShell;
            }
            else if (Reader.is_value("shell_scalar"))
            {
                // FST +0x24; TEST AH,5 / JP -- raised to 1.0 only on an
                // ordered "less".
                if (ZonesAdded == 0 || Shell == nullptr)
                    continue;
                Shell->ShellScalar = Reader.get_value_float(0);
                if (Shell->ShellScalar < 1.0f)
                    Shell->ShellScalar = 1.0f;
            }
            else if (Reader.is_value("max_alpha"))
            {
                // FST +0x28; TEST AH,0x41 / JNZ -- capped only when
                // strictly greater.
                if (ZonesAdded == 0 || Shell == nullptr)
                    continue;
                Shell->MaxAlpha = Reader.get_value_float(0);
                if (Shell->MaxAlpha > 1.0f)
                    Shell->MaxAlpha = 1.0f;
            }
            else if (Reader.is_value("exclusion_tint"))
            {
                // three bytes / 255 (the field); each is kept only
                // on an ordered "less" than 1.0 (TEST AH,5 / JNP), anything else is 1.0.
                if (ZonesAdded == 0 || Shell == nullptr)
                    continue;
                float const R = Reader.get_value_float(0);
                float const G = Reader.get_value_float(1);
                float const Bl = Reader.get_value_float(2);
                float const ScaledB = static_cast<float>(Bl * (1.0f / 255.0f));
                float const ScaledG = static_cast<float>(G * (1.0f / 255.0f));
                float const ScaledR = static_cast<float>(R * (1.0f / 255.0f));
                Shell->Tint[0] = (ScaledR < 1.0f) ? ScaledR : 1.0f;
                Shell->Tint[1] = (ScaledG < 1.0f) ? ScaledG : 1.0f;
                Shell->Tint[2] = (ScaledB < 1.0f) ? ScaledB : 1.0f;
            }
        }
    }

    // (MOV EAX,[ECX+0x20]; RET), shared with the JumpShipEffect colour getter
    // (JumpShipEffectColor, space_scene_obj_draw.cpp). No test of `this`.


    // / the branch / the branch / the branch / the branch: each is
    // FLD float [ECX+off]; RET -- no test of `this`.


    // (thiscall, RET 4) — copy fog RGB float3 from +0x2a0; no tests.
    // loads EAX = [ESP+4] first and never changes it, so the slot returns Out; the
    // space scene's lighting pass (0x0055564c CALL [EDX+0x30]; PUSH EAX) uses that result.


    // (thiscall, RET 4) — fog RGB flicker into Out (or copy +0x2a0 when idle).
    // No test of `this` or of the out pointer.
    // Both RET 4 exits (0x0050963a / 0x00509637) leave EAX = Out (MOV EAX,[ESP+0x2c]).


    // — the [NebulaLight] ambient bytes (+0x2cd..+0x2cf, written by Load's
    // ) when their flag +0x2cc is set, else null; no test of `this`.
    // Reader: the space scene's ambient lerp (0x00555588).


    // (thiscall, RET 4) — Out = Zone(+0x20)->GetPos() (IZone vt+0x10), then
    // return Zone->GetRadius() (vt+0x30). No tests of `this`, the out pointer or the zone.


    // — exterior distance factor vs Zone@+0x20 (not +0x24).


    // — fog factor; +0x24 = NebulaExclusionZone*.


    // (thiscall, RET 4). The one stack argument is never read; the draw pass
    // (0x0051c3c0) pushes the field there. No test of `this` in baseline.


    // — CloudsList@+0xf8 → the routine (TexturePanelDrawClouds).


    // — exclusion-shell mesh draw when flag@+0x2c and holder@+0x30 set.


    // (thiscall, RET 4) — [Lightning] INI into the member at Nebula+0x100.
    // ; no test of `this`.
    void LightningLoadIni(float* Lightning, INI_Reader& Reader)
    {
        unsigned char* const Flag = reinterpret_cast<unsigned char*>(Lightning) + 0x14c;
        if (*Flag != 0)
            *Flag = 0;
        Lightning[0x52] = 0.0f;
        while (Reader.read_value())
        {
            if (Reader.is_value("gap"))
                Lightning[10] = Reader.get_value_float(0);
            else if (Reader.is_value("color"))
            {
                Lightning[7] = Reader.get_value_float(0) * (1.0f / 255.0f);
                Lightning[8] = Reader.get_value_float(1) * (1.0f / 255.0f);
                Lightning[9] = Reader.get_value_float(2) * (1.0f / 255.0f);
            }
            else if (Reader.is_value("size"))
                Lightning[11] = Reader.get_value_float(0);
            else if (Reader.is_value("duration"))
                Lightning[12] = Reader.get_value_float(0);
        }
        Lightning[6] = 0.0f;
        Lightning[14] = 0.0f;
        Lightning[15] = 0.0f;
        Lightning[16] = 0.0f;
        Lightning[17] = 0.0f;
        // the first flash is a negative countdown,
        // (-0.8 - rand/32768 * 0.7) * gap, on the FPU with the two double constants
        // (the field = double(-0.8f), the field = double(0.7f)). This is the only write
        // of +0x00 in OutpostFrontier.exe, so the draw's "+0x00 >= 0" test keeps a lightning with a
        // positive gap dark for good.
        int const R = rand();
        *Flag = 1;
        Lightning[0] = static_cast<float>(
            (static_cast<double>(-0.8f) -
             static_cast<double>(R) * 3.0517578125e-05f * static_cast<double>(0.7f)) *
            Lightning[10]);
    }

    // (thiscall) — clear the lightning's active flag (+0x14c) when it is set.
    void LightningDisable(float* Lightning)
    {
        unsigned char* const Flag = reinterpret_cast<unsigned char*>(Lightning) + 0x14c;
        if (*Flag != 0)
            *Flag = 0;
    }

    // (stdcall, RET 8) — one lightning bolt segment as a camera-facing quad.
    // Seg layout (0x1c): float3 A, float3 B, float* UVs (+8/+c/+10/+14). Eye = the camera
    // position (ICamera vt+0xc, read by the caller). The vector helpers are the routine
    // (out = a - b), the routine (out = a + b), the routine (out = v * s), each storing float.
    void LightningDrawSegment(float* Seg, float const* Eye)
    {
        float const* const A = Seg;
        float const* const B = Seg + 3;

        // / 0x00552713: Eye - A, B - A.
        float EyeMinusA[3];
        EyeMinusA[0] = Eye[0] - A[0];
        EyeMinusA[1] = Eye[1] - A[1];
        EyeMinusA[2] = Eye[2] - A[2];

        float BA[3];
        BA[0] = B[0] - A[0];
        BA[1] = B[1] - A[1];
        BA[2] = B[2] - A[2];

        // Cross = (Eye-A) x (B-A); both products and the difference stay
        // on the FPU, one float store (a float*float product is exact in double).
        float Cross[3];
        Cross[0] = static_cast<float>(static_cast<double>(EyeMinusA[2]) * BA[1] -
                                      static_cast<double>(BA[2]) * EyeMinusA[1]);
        Cross[1] = static_cast<float>(static_cast<double>(BA[2]) * EyeMinusA[0] -
                                      static_cast<double>(EyeMinusA[2]) * BA[0]);
        Cross[2] = static_cast<float>(static_cast<double>(EyeMinusA[1]) * BA[0] -
                                      static_cast<double>(BA[1]) * EyeMinusA[0]);

        // / 0x00552775: Mid = (A + B) * 0.5 (sum stored to float first).
        float Mid[3];
        Mid[0] = (A[0] + B[0]) * 0.5f;
        Mid[1] = (A[1] + B[1]) * 0.5f;
        Mid[2] = (A[2] + B[2]) * 0.5f;

        // HalfChord = Mid - A.
        float HalfChord[3];
        HalfChord[0] = Mid[0] - A[0];
        HalfChord[1] = Mid[1] - A[1];
        HalfChord[2] = Mid[2] - A[2];

        // Cross *= |HalfChord| / |Cross|. Both lengths and the quotient
        // stay on the FPU (no zero test in baseline); each component is stored back as float.
        double const LenHalf = sqrt(static_cast<double>(HalfChord[0]) * HalfChord[0] +
                                    static_cast<double>(HalfChord[1]) * HalfChord[1] +
                                    static_cast<double>(HalfChord[2]) * HalfChord[2]);
        double const LenCross = sqrt(static_cast<double>(Cross[0]) * Cross[0] +
                                     static_cast<double>(Cross[1]) * Cross[1] +
                                     static_cast<double>(Cross[2]) * Cross[2]);
        double const Scale = LenHalf / LenCross;
        Cross[0] = static_cast<float>(Cross[0] * Scale);
        Cross[1] = static_cast<float>(Cross[1] * Scale);
        Cross[2] = static_cast<float>(Cross[2] * Scale);

        // Edge = A - Mid; 0x005527f8..0x00552860: Edge and Cross times 1.15
        // (the field), stored through the routine.
        float const K = 1.15f;
        float Edge[3];
        Edge[0] = A[0] - Mid[0];
        Edge[1] = A[1] - Mid[1];
        Edge[2] = A[2] - Mid[2];
        Edge[0] *= K;
        Edge[1] *= K;
        Edge[2] *= K;
        Cross[0] *= K;
        Cross[1] *= K;
        Cross[2] *= K;

        // the UV block (+0x18) is read without a null test.
        float const* const UvBase = *reinterpret_cast<float* const*>(Seg + 6);

        // Quad: Mid+Edge, Mid+Cross, Mid-Edge, Mid-Cross with UV corners from shape.
        Win3DTexCoord2f(UvBase[2], UvBase[3]); // +8, +0xc
        Win3DVertex3f(Mid[0] + Edge[0], Mid[1] + Edge[1], Mid[2] + Edge[2]);
        Win3DTexCoord2f(UvBase[4], UvBase[3]); // +0x10, +0xc
        Win3DVertex3f(Mid[0] + Cross[0], Mid[1] + Cross[1], Mid[2] + Cross[2]);
        Win3DTexCoord2f(UvBase[4], UvBase[5]); // +0x10, +0x14
        Win3DVertex3f(Mid[0] - Edge[0], Mid[1] - Edge[1], Mid[2] - Edge[2]);
        Win3DTexCoord2f(UvBase[2], UvBase[5]); // +8, +0x14
        Win3DVertex3f(Mid[0] - Cross[0], Mid[1] - Cross[1], Mid[2] - Cross[2]);
    }

    // (thiscall, RET 4) — the lightning flash draw, 0x005529b0..0x00552b00.
    void LightningDraw(float* Lightning, void* Camera)
    {
        // nothing unless active (+0x14c) and the countdown +0x00 is not
        // below zero (TEST AH,5 / JNP -- only an ordered "less" returns).
        unsigned char const Flag =
            *reinterpret_cast<unsigned char*>(reinterpret_cast<unsigned char*>(Lightning) + 0x14c);
        if (Flag == 0 || Lightning[0] < 0.0f)
            return;

        // culling off, blending on, blend (2, 2), depth write off.
        Win3DDisable(kWin3DCullFace);
        Win3DEnable(kWin3DBlend);
        Win3DSetBlendFunc(2, 2);
        Win3DSetDepthWrite(0);

        // colour (+0x1c..+0x24) * rand/32768 * intensity (+0x14). The
        // red product stays on the FPU between the two multiplies; green and blue are rounded to
        // float after the first.
        double const Jitter = static_cast<double>(rand()) * 3.0517578125e-05f;
        float const Intensity = Lightning[5];
        float const Red =
            static_cast<float>(static_cast<double>(Lightning[7]) * Jitter * Intensity);
        float const Green = static_cast<float>(
            static_cast<double>(static_cast<float>(Lightning[8] * Jitter)) * Intensity);
        float const Blue = static_cast<float>(
            static_cast<double>(static_cast<float>(Lightning[9] * Jitter)) * Intensity);

        // the routine -- the camera's projection and view.
        ICamera* const Cam = static_cast<ICamera*>(Camera);
        Win3DApplyCamera(Cam);

        // the texture of the reference at +0x38 (the routine, frame 0).
        FTextureRef const* const TexRef = *reinterpret_cast<FTextureRef* const*>(Lightning + 14);
        Win3DBindTexture(TexShapeGetGpuHandle(TexRef, 0));

        // Color3f, then quads (the routine(8)).
        Win3DColor3f(Red, Green, Blue);
        Win3DBegin(kWin3DQuads);

        // eight 0x1c-byte segments from +0x48, each drawn while its
        // counter (+0x128 + 4*i) is below 3; the eye is re-read (ICamera vt+0x0c) per segment.
        int* const Counters = reinterpret_cast<int*>(Lightning + 0x4a);
        float* Seg = Lightning + 0x12;
        for (int I = 0; I < 8; ++I)
        {
            if (Counters[I] < 3)
                LightningDrawSegment(Seg, Cam->GetPosition());
            Seg += 7;
        }

        // End, depth write on, blending off, culling on.
        Win3DEnd();
        Win3DSetDepthWrite(1);
        Win3DDisable(kWin3DBlend);
        Win3DEnable(kWin3DCullFace);
    }

    // — screen fog fade overlay (+ Lightning@+0x100).


    // — fog shell + NebulaUpdateBoltCamera under MaterialBatcher/depth gate.


    // (module storage..00508f19) — fog RS + NebulaDrawBolts under MaterialBatcher/
    // depth gate. No null tests on This, the Pipeline or the MaterialBatcher, and no guard on a
    // zero +0x298 (baseline FDIVs straight through).


    // (thiscall, RET 4) -- vt+0x58: the routine, then free (the routine) when
    // bit 0 of the flags is set; returns this. No test of `this`.


    // (thiscall, RET 8) -- vt+0x00 Load(file, zone), 0x00504e60..0x0050629x. Caller:
    // StarSysLoadIni's [Nebula] block. No test of `this` or of either string.

} // anonymous namespace

void NebulaCloudsConstruct(FNebulaClouds* Clouds)
{
    LocalNebulaCloudsConstruct(Clouds);
}

void NebulaCloudsSetDetail(FNebulaClouds* Clouds, float Detail)
{
    LocalNebulaCloudsSetDetail(Clouds, Detail);
}

void NebulaCloudsApplyDetail(FNebulaClouds* Clouds, float Detail)
{
    LocalNebulaCloudsApplyDetail(Clouds, Detail);
}

void NebulaCloudsReleaseBits(FNebulaClouds* Clouds)
{
    LocalNebulaCloudsReleaseBits(Clouds);
}

void NebulaCloudsRebuildBits(FNebulaClouds* Clouds)
{
    LocalNebulaCloudsRebuildBits(Clouds);
}

void NebulaCloudsRegenExcluded(FNebulaClouds* Clouds, void* ExclusionCtx)
{
    LocalNebulaCloudsRegenExcluded(Clouds, ExclusionCtx);
}

void NebulaCloudsUpdate(FNebulaClouds* Clouds, void* Camera, float Dt, float Visibility,
                        Exclusion::NebulaExclusionZoneMgr* ExclusionMgr)
{
    LocalNebulaCloudsUpdate(Clouds, Camera, Dt, Visibility, ExclusionMgr);
}

void NebulaCloudsDestroy(FNebulaClouds* Clouds, unsigned char Flags)
{
    LocalNebulaCloudsDestroy(Clouds, Flags);
}

void NebulaCloudsParseIni(FNebulaClouds* Clouds, INI_Reader& Reader, FShapeDB* Shapes)
{
    LocalNebulaCloudsParseIni(Clouds, Reader, Shapes);
}

void FNebulaEffect::Load(char const* FilePath, char const* ZoneName)
{
    FNebulaEffect* const This = this;
    unsigned char* const B = reinterpret_cast<unsigned char*>(This);

    // a loaded nebula is shut down first (vt+0x04); +0x4c = 1.
    if (This->FlagLoaded != 0)
        This->Shutdown();
    B[0x4c] = 1;

    // the zone from the active system (the routine); a miss is an
    // FDUMP error and nothing else -- the zone's GetShape (vt+0x18) is called regardless.
    unsigned const SystemId = StarSysGetActiveSystemId();
    This->Zone = const_cast<Universe::IZone*>(Universe::get_zone(SystemId, ZoneName));
    if (This->Zone == nullptr)
        FDUMP(0x100001, "%s(%d) : *** ERROR: Nebula linked to missing Zone (%s)",
              "Client\\Space\\Nebula.cpp", 0x1ee, ZoneName);
    (void)This->Zone->GetShape();

    // the defaults.
    *reinterpret_cast<float*>(B + 0xb8) = 0.75f;
    *reinterpret_cast<float*>(B + 0xb4) = 0.75f;
    *reinterpret_cast<int*>(B + 0xa0) = 0;
    for (int I = 0; I < 8; ++I)
        reinterpret_cast<float*>(B + 0x80)[I] = 1.0f;
    This->FogDistance = 10000.0f;
    *reinterpret_cast<int*>(B + 0x29c) = 0;
    *reinterpret_cast<int*>(B + 0x24) = 0;
    *reinterpret_cast<int*>(B + 0x28) = 0;
    B[0x2c] = 0;
    *reinterpret_cast<int*>(B + 0x30) = 0;
    *reinterpret_cast<float*>(B + 0x258) = -1.0f;
    *reinterpret_cast<int*>(B + 0xbc) = 1;

    // the reader, and the file resolved against NeuronRuntime::Data.
    INI_Reader Reader;
    char FullPath[MAX_PATH];
    GetFileSysPath(FullPath, FilePath, NeuronRuntime::Data);
    if (!Reader.open(FullPath, false))
    {
        // an error, then the rebuild and the light as usual.
        FDUMP(0x100001, "%s(%d) : *** ERROR: unable to open nebula file %s",
              "Client\\Space\\Nebula.cpp", 0x312, FullPath);
        OracleOnNebulaRead(FilePath, This, false); // Oracle instrumentation (P0.5)
    }
    else
    {
        // the section loop; a header nobody names is skipped.
        while (Reader.read_header())
        {
            if (Reader.is_header("Exterior"))
            {
                ParseExteriorSection(This, Reader);
            }
            else if (Reader.is_header("Clouds"))
            {
                // a 0x12c clouds object (the ctor the routine only
                // on a non-null block), the routine(detail), the routine(reader, ShapeDB
                // +0x04), +0xc4 = the zone, then the list insert. Nothing else is tested.
                void* const Mem = operator new(sizeof(FNebulaClouds));
                FNebulaClouds* Clouds = nullptr;
                if (Mem != nullptr)
                {
                    Clouds = static_cast<FNebulaClouds*>(Mem);
                    LocalNebulaCloudsConstruct(Clouds);
                }
                LocalNebulaCloudsSetDetail(Clouds, G_NebulaDetail);
                LocalNebulaCloudsParseIni(Clouds, Reader, NebulaShapes(This));
                *reinterpret_cast<Universe::IZone**>(Clouds->Raw + 0xc4) = This->Zone;
                CloudsListPush(This, Clouds);
            }
            else if (Reader.is_header("ObjectFog"))
            {
                // recognised and skipped -- its keys are never read.
            }
            else if (Reader.is_header("Fog"))
            {
                ParseFogSection(This, Reader);
            }
            else if (Reader.is_header("TexturePanels"))
            {
                // every file= goes through GetFullFilename into the
                // ShapeDB (+0x04, the routine with no base directory).
                while (Reader.read_value())
                {
                    if (Reader.is_value("file"))
                        NebulaShapes(This)->Load(GetFullFilename(Reader.get_value_string()),
                                                 nullptr);
                }
            }
            else if (Reader.is_header("Lightning"))
            {
                // the routine on +0x100, then +0x248 = the zone.
                LightningLoadIni(reinterpret_cast<float*>(B + 0x100), Reader);
                *reinterpret_cast<Universe::IZone**>(B + 0x248) = This->Zone;
            }
            else if (Reader.is_header("BackgroundLightning"))
            {
                ParseBackgroundLightningSection(This, Reader);
            }
            else if (Reader.is_header("DynamicLightning"))
            {
                ParseDynamicLightningSection(This, Reader);
            }
            else if (Reader.is_header("NebulaLight"))
            {
                ParseNebulaLightSection(This, Reader);
            }
            else if (Reader.is_header("Exclusion Zones"))
            {
                // the only spelling; the system id is the one read at
                // .
                ParseExclusionZonesSection(This, Reader, SystemId);
            }
        }
        OracleOnNebulaRead(FilePath, This, true); // Oracle instrumentation (P0.5)
    }

    // the routine.
    NebulaRebuild(This);

    // — ambient GameLight when period@+0x278 > 0.
    {
        unsigned char* const LB = reinterpret_cast<unsigned char*>(This);
        // FCOMP 0.0 / TEST AH,0x41 / JNZ -- only a strictly positive period
        // builds the light. There is no else: +0x270 keeps whatever it held (null from the
        // ctor, or the pointer the last Shutdown released -- baseline never clears it there).
        float const Period = *reinterpret_cast<float*>(LB + 0x278);
        if (Period > 0.0f)
        {
            // the routine(0x12c), the ctor only on a non-null
            // block; the result is stored and used with no null test.
            void* Mem = GameLightHeapAlloc(sizeof(FGameLight));
            FGameLight* Light = nullptr;
            if (Mem != nullptr)
                Light = GameLightConstruct(static_cast<FGameLight*>(Mem));
            *reinterpret_cast<FGameLight**>(LB + 0x270) = Light;
            // (type=3, bit0=0) — directional.
            EngineLightSetType((FEngineLight*)Light, 3, false);
            float Zero[3] = {0.0f, 0.0f, 0.0f};
            EngineLightSetPosition((FEngineLight*)Light, Zero);
            unsigned char White[3] = {0xff, 0xff, 0xff};
            EngineLightSetColorBytesAlt((FEngineLight*)Light, White);
            // = 500000.0f — the routine raw Range.
            EngineLightSetRangeRaw((FEngineLight*)Light, 500000.0f);
            // vt+4 (enable); failure releases the light, warns and
            // clears +0x270; success turns it straight off again when its flag bit 3 is set
            // (the routine's flicker enables it).
            if (!GameLightEnable((FEngineLight*)Light))
            {
                GameLightRelease(Light);
                // baseline passes the double 500000.0 to a "%d".
                FDUMP(0x100002,
                      "%s(%d) : *** WARNING: Could not create nebula light of range %d",
                      "Client\\Space\\Nebula.cpp", 0x32a, 500000.0);
                *reinterpret_cast<FGameLight**>(LB + 0x270) = nullptr;
            }
            else if ((Light->Flags & 0x08) != 0)
            {
                GameLightVirtualDisable(Light);
            }
        }
    }

    // loaded.
    This->FlagLoaded = 1;
}

void FNebulaEffect::DrawFogOverlay(void* Camera)
{
    FNebulaEffect* const This = this;
    // (thiscall, RET 4), 0x00508b00..0x00508ce6. No test of `this` or Camera.
    if (This->FlagLoaded == 0)
        return;
    unsigned char* const B = reinterpret_cast<unsigned char*>(This);
    ICamera* const Cam = static_cast<ICamera*>(Camera);

    // / 0x00508b21: the zone's GetPos (IZone vt+0x10) and the camera's
    // GetOrientation (vt+0x10) are called and their results dropped.
    (void)This->Zone->GetPos();
    (void)Cam->GetOrientation();

    float const* const FadePtr = reinterpret_cast<float*>(B + 0x294);
    bool bDrawOverlay = true;
    // FCOMP 1.0f, TEST AH,5 / JP -- only an ordered "less" clears +0x2b8.
    if (*FadePtr < 1.0f)
        B[0x2b8] = 0;
    // JNP skips on "less"; otherwise a set +0x2b8 goes straight to the lightning.
    if (!(*FadePtr < 1.0f) && B[0x2b8] != 0)
        bDrawOverlay = false;
    // FCOMP 0.0f, TEST AH,0x41 / JNZ -- <= 0 or unordered skips to the lightning.
    else if (!(*FadePtr > 0.0f))
        bDrawOverlay = false;

    if (bDrawOverlay)
    {
        // FCOMP 1.0f, TEST AH,1 / JNZ -- only >= 1.0 sets +0x2b8.
        if (*FadePtr >= 1.0f)
            B[0x2b8] = 1;
    }

#ifdef OUTPOST_FRONTIER_DX9RENDERER
    // Port-only: while the Direct3D 9 volumetric nebula draws this nebula's inside
    // (nebula_volume.cpp), it replaces the fullscreen fog quad; the flag above and the
    // lightning below stay as baseline.
    if (bDrawOverlay && !NebulaVolume::ReplacesFogOverlay(This))
#else
    if (bDrawOverlay)
#endif
    {
        // the alpha is the raw +0x294 dword, copied before the state calls.
        float const Alpha = *FadePtr;
        // the routine(Camera); Disable 0xb50; DepthWrite 0;
        // Disable 0xb71; Disable 0xde1; Enable 0xbe2; Disable 0xb44; BlendFunc(5, 6);
        // Begin(8). the routine is Enable and the routine is Disable.
        Win3DSetOrtho2DForCamera(static_cast<FSpaceCamera*>(Camera));
        Win3DDisable(kWin3DLightingCap);
        Win3DSetDepthWrite(0);
        Win3DDisable(kWin3DDepthTest);
        Win3DDisable(0xde1); // GL_TEXTURE_2D
        Win3DEnable(kWin3DBlend);
        Win3DDisable(kWin3DCullFace);
        Win3DSetBlendFunc(5, 6);
        Win3DBegin(kWin3DQuads);
        // Color4f(+0x2a0, +0x2a4, +0x2a8, alpha).
        float const* Rgb = reinterpret_cast<float*>(B + 0x2a0);
        Win3DColor4f(Rgb[0], Rgb[1], Rgb[2], Alpha);
        // the camera's viewport rect (vt+0x14), no null test; the four corners
        // go (0,0), (W,0), (W,H), (0,H), each width / height an int difference FILD'd to float.
        int const* const Vp = Cam->GetViewport();
        Win3DVertex3f(0.0f, 0.0f, 0.0f);
        Win3DVertex3f(static_cast<float>(Vp[2] - Vp[0]), 0.0f, 0.0f);
        Win3DVertex3f(static_cast<float>(Vp[2] - Vp[0]), static_cast<float>(Vp[3] - Vp[1]),
                      0.0f);
        Win3DVertex3f(0.0f, static_cast<float>(Vp[3] - Vp[1]), 0.0f);
        // End; DepthWrite 1; Enable 0xb71; Disable 0xbe2;
        // Enable 0xb44; the routine(Camera).
        Win3DEnd();
        Win3DSetDepthWrite(1);
        Win3DEnable(kWin3DDepthTest);
        Win3DDisable(kWin3DBlend);
        Win3DEnable(kWin3DCullFace);
        Win3DApplyCameraPerspective(Cam);
    }

    // +0x294 re-read, FCOMP 0.0f, TEST AH,0x41 / JNZ -- only a value strictly
    // above 0 draws the lightning (the routine(this+0x100, Camera)).
    if (*FadePtr > 0.0f)
        LightningDraw(reinterpret_cast<float*>(B + 0x100), Camera);
}

void FNebulaEffect::DrawFogShell(void* Camera)
{
    FNebulaEffect* const This = this;
    // (thiscall, RET 4), 0x00508cf0..0x00508dfa. No test of `this`, Camera,
    // the MaterialBatcher or the Pipeline.
    if (This->FlagLoaded == 0)
        return;
    unsigned char* const B = reinterpret_cast<unsigned char*>(This);
    // FCOMP 1.0f, TEST AH,5 / JP -- only an ordered "less" goes on.
    float const Fade = *reinterpret_cast<float*>(B + 0x294);
    if (!(Fade < 1.0f))
        return;

    // batcher slot8(1, -1); RS 0x89 = 0; DepthWrite 0; Disable 0xb71;
    // ; RS 0x8b = -1.
    SetMaterialBatchBinOrder(1u, -1);
    NeuronRuntime::Pipeline->SetRenderState(D3DRS_LIGHTING, 0);
    Win3DSetDepthWrite(0);
    Win3DDisable(kWin3DDepthTest);
    EngineLightClearRegisteredList();
    NeuronRuntime::Pipeline->SetRenderState(D3DRS_AMBIENT, -1);

    // the camera's GetOrientation (vt+0x10) block, 12 dwords (the
    // 3x3 axes and, at +0x24, the position) copied to the stack.
    ICamera* const Cam = static_cast<ICamera*>(Camera);
    float Xform[12];
    memcpy(Xform, Cam->GetOrientation(), sizeof(Xform));
    // the routine(Camera) -- the passed camera, not the active space camera.
    Win3DApplyCamera(Cam);
    // the routine(this, copy+0x24, copy+0x28, copy+0x2c).
#ifdef OUTPOST_FRONTIER_DX9RENDERER
    // Port-only: while the Direct3D 9 volumetric nebula draws this nebula at full strength
    // (nebula_volume.cpp) the two fill sheets are left out -- the path the routine itself
    // takes without a fill shape; the states around it stay as baseline.
    if (!NebulaVolume::ReplacesFillSheets(This))
#endif
        NebulaUpdateBoltCamera(This, Xform[9], Xform[10], Xform[11]);

    // batcher slot8(1, 1); RS 0x89 = 1; DepthWrite 1; Enable 0xb71.
    SetMaterialBatchBinOrder(1u, 1);
    NeuronRuntime::Pipeline->SetRenderState(D3DRS_LIGHTING, 1);
    Win3DSetDepthWrite(1);
    Win3DEnable(kWin3DDepthTest);
}

void FNebulaEffect::DrawBoltsPass(void* Camera)
{
    FNebulaEffect* const This = this;
    if (This->FlagLoaded == 0)
        return;
    unsigned char* const B = reinterpret_cast<unsigned char*>(This);
    float const FarZ = *reinterpret_cast<float*>(B + 0x2ac);
    float const Near = *reinterpret_cast<float*>(B + 0x29c);
    float const FogDist = *reinterpret_cast<float*>(B + 0x298);
    // ((FarZ - Near) * 20.0 / FogDist), clamped at 0 when below it,
    // then (T + 1.0) * FogDist + FarZ, stored as float.
    double T = (static_cast<double>(FarZ) - Near) * 20.0 / FogDist;
    if (T < 0.0)
        T = 0.0;
    float const FogEnd = static_cast<float>((T + 1.0) * FogDist + FarZ);
    FogApplyColorAndRange(reinterpret_cast<float*>(B + 0x2a0), FarZ, FogEnd);

    SetMaterialBatchBinOrder(1u, -1);
    NeuronRuntime::Pipeline->SetRenderState(D3DRS_LIGHTING, 0);
    Win3DSetDepthWrite(0);
    Win3DDisable(kWin3DDepthTest);
    EngineLightClearRegisteredList();
    NeuronRuntime::Pipeline->SetRenderState(D3DRS_AMBIENT, -1);

#ifdef OUTPOST_FRONTIER_DX9RENDERER
    // Port-only: while the Direct3D 9 volumetric nebula draws this nebula at full strength
    // (nebula_volume.cpp) the exterior billboards are left out -- the path the routine itself
    // takes with the zone out of view; the states around it stay as baseline.
    if (!NebulaVolume::ReplacesExteriorBits(This))
#endif
        NebulaDrawBolts(This, Camera);

    SetMaterialBatchBinOrder(1u, 1);
    NeuronRuntime::Pipeline->SetRenderState(D3DRS_LIGHTING, 1);
    Win3DSetDepthWrite(1);
    Win3DEnable(kWin3DDepthTest);
    Win3DDisable(0xb60); // GL_FOG — Win3D may warn; listing keeps the call
}

void FNebulaEffect::DrawExclusionShell(void* Camera)
{
    FNebulaEffect* const This = this;
    // (thiscall, RET 4), 0x00508f20..0x00509192. Drawn only while loaded and
    // while Update found a shell for the current exclusion zone (+0x2c / +0x30).
    if (This->FlagLoaded == 0)
        return;
    unsigned char* const B = reinterpret_cast<unsigned char*>(This);
    if (B[0x2c] == 0)
        return;

    // batcher slot 8 (1, -1); RS 0x89 = 0; every registered light
    // off (the routine); RS 0x8b saved into a zeroed local, then -1; depth write off and
    // fog off (the routine(0xb60)) -- depth test and culling are left alone.
    SetMaterialBatchBinOrder(1u, -1);
    NeuronRuntime::Pipeline->SetRenderState(D3DRS_LIGHTING, 0);
    EngineLightClearRegisteredList();
    int SavedRs8b = 0;
    NeuronRuntime::Pipeline->GetRenderState(D3DRS_AMBIENT, &SavedRs8b);
    NeuronRuntime::Pipeline->SetRenderState(D3DRS_AMBIENT, -1);
    Win3DSetDepthWrite(0);
    Win3DDisable(kWin3DFog);

    // (immersion +0x28 - 0.2f) * 1.25f, kept on the FPU; below
    // 0.0 (TEST AH,5 / JP) -> 0.0f, else above 1.0 (TEST AH,0x41 / JNZ) -> 1.0f.
    double Alpha = (static_cast<double>(*reinterpret_cast<float*>(B + 0x28)) - 0.2f) * 1.25f;
    if (Alpha < 0.0)
        Alpha = 0.0f;
    else if (Alpha > 1.0)
        Alpha = 1.0f;

    // the shell's user data gets the tint and alpha * max_alpha.
    FNebulaZoneShell* const Shell = *reinterpret_cast<FNebulaZoneShell**>(B + 0x30);
    Shell->UserData[0] = Shell->Tint[0];
    Shell->UserData[1] = Shell->Tint[1];
    Shell->UserData[2] = Shell->Tint[2];
    Shell->UserData[4] = static_cast<float>(Alpha * Shell->MaxAlpha);

    // a scale transform, identity rotation times
    // zone radius (vt+0x30) * shell_scalar * 0.001f on the diagonal, zero translation.
    Exclusion::NebulaExclusionZone* const Excl =
        *reinterpret_cast<Exclusion::NebulaExclusionZone**>(B + 0x24);
    Universe::IZone const& Zone = *Excl->ZonePtr;
    float const Scale = static_cast<float>(
        static_cast<double>(Zone.GetRadius()) * Shell->ShellScalar * 0.001f);
    FTransform ScaleXf;
    memset(&ScaleXf, 0, sizeof(ScaleXf));
    ScaleXf.Rotation.D[0][0] = Scale;
    ScaleXf.Rotation.D[1][1] = Scale;
    ScaleXf.Rotation.D[2][2] = Scale;

    // the zone's transform (vt+0x0c) times the scale through
    // MulTransform (+0x1c), copied out and set as the instance matrix
    // (IEngine +0x9c).
    FTransform const& ZoneXf = reinterpret_cast<FTransform const&>(Zone.GetTransform());
    FTransform Scaled;
    AcquireNebulaMath()->MulTransform(Scaled, ZoneXf, ScaleXf);
    NeuronRuntime::Engine->SetInstanceMatrix(Shell->Handle.EngineInstance, &Scaled);

    // rendered (IRenderer +0x1c) only with a live instance.
    if (Shell->Handle.EngineInstance != reinterpret_cast<void*>(-1))
    {
        NeuronRuntime::Renderer->RenderInstance(Camera, Shell->Handle.EngineInstance, 1.0f, 0x108u, nullptr,
                              nullptr);
    }

    // batcher slot 8 (1, 1); RS 0x16 = 2, 0x89 = 1, 0x8b restored;
    // depth write on.
    SetMaterialBatchBinOrder(1u, 1);
    NeuronRuntime::Pipeline->SetRenderState(D3DRS_CULLMODE, 2);
    NeuronRuntime::Pipeline->SetRenderState(D3DRS_LIGHTING, 1);
    NeuronRuntime::Pipeline->SetRenderState(D3DRS_AMBIENT, SavedRs8b);
    Win3DSetDepthWrite(1);
}

void FNebulaEffect::DrawCloudsPanels(void* Camera)
{
    FNebulaEffect* const This = this;
    NebulaDrawCloudsPanels(This, Camera);
}

float FNebulaEffect::ZoneExteriorFactor(float const* CamPos3)
{
    FNebulaEffect* const This = this;
    // the offset from the zone's vt+0x10 position, as floats.
    Universe::IZone const& Zone = *This->Zone;
    Vector const& ZonePos = Zone.GetPos();
    Vector Dir;
    Dir.x = CamPos3[0] - ZonePos.x;
    Dir.y = CamPos3[1] - ZonePos.y;
    Dir.z = CamPos3[2] - ZonePos.z;

    // vt+0x30 squared on the stack vs the float (dx^2 + dz^2) +
    // dy^2; TEST 0x41 / JNZ -> go on when the distance is <= or unordered, else 0.0f.
    double const Radius = Zone.GetRadius();
    float const DistSq = static_cast<float>(
        (static_cast<double>(Dir.x) * Dir.x + static_cast<double>(Dir.z) * Dir.z) +
        static_cast<double>(Dir.y) * Dir.y);
    if (DistSq > Radius * Radius)
        return 0.0f;

    // vs 0.1 (double module storage), TEST 5 / JP -> go on when
    // >= or unordered, else 1.0f.
    if (DistSq < 0.1)
        return 1.0f;

    // the direction scaled by 1.0 / sqrt (double, unrounded),
    // stored as floats, then vt+0x40.
    double const Dist = sqrt(static_cast<double>(DistSq));
    float const DistF = static_cast<float>(Dist);
    double const Inv = 1.0 / Dist;
    Dir.x = static_cast<float>(Dir.x * Inv);
    Dir.y = static_cast<float>(Dir.y * Inv);
    Dir.z = static_cast<float>(Dir.z * Inv);

    // Edge = vt+0x40 - float distance, FST float, the stack
    // value vs 0.0f with TEST 5 / JP -> go on when >= or unordered, else 0.0f.
    double const Edge = static_cast<double>(Zone.GetRadiusAlong(Dir)) - DistF;
    float const EdgeF = static_cast<float>(Edge);
    if (Edge < 0.0)
        return 0.0f;

    // the stored edge vs FogDistance (+0x298), TEST 0x41 / JNZ ->
    // divide when <= or unordered, else 1.0f.
    float const FogDist = This->FogDistance;
    if (EdgeF > FogDist)
        return 1.0f;
    return EdgeF / FogDist;
}

float FNebulaEffect::GetFloat294()
{
    FNebulaEffect* const This = this;
    return *reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(This) + 0x294);
}

float FNebulaEffect::GetFloat2C4()
{
    FNebulaEffect* const This = this;
    return *reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(This) + 0x2c4);
}

float FNebulaEffect::GetFloat2C8()
{
    FNebulaEffect* const This = this;
    return *reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(This) + 0x2c8);
}

float* FNebulaEffect::CopyFogRgb(float* OutRgb3)
{
    FNebulaEffect* const This = this;
    float const* Src = reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(This) + 0x2a0);
    OutRgb3[0] = Src[0];
    OutRgb3[1] = Src[1];
    OutRgb3[2] = Src[2];
    return OutRgb3;
}

float* FNebulaEffect::FillFogRgb(float* OutRgb3)
{
    FNebulaEffect* const This = this;
    unsigned char* const B = reinterpret_cast<unsigned char*>(This);
    // +0x254 FCOMP 0.0f, TEST AH,0x41 / JNP -> copy when the period
    // is <= 0 (a NaN period goes on).
    float const Period = *reinterpret_cast<float*>(B + 0x254);
    if (Period <= 0.0f)
    {
        float const* Src = reinterpret_cast<float*>(B + 0x2a0);
        OutRgb3[0] = Src[0];
        OutRgb3[1] = Src[1];
        OutRgb3[2] = Src[2];
        return OutRgb3;
    }

    // the old +0x258 vs 0.0f, +0x26c = 0, then +0x258 = dt (+0x1c) +
    // old, FST float with the unrounded sum kept on the FPU for the compares below.
    float const AccPrev = *reinterpret_cast<float*>(B + 0x258);
    B[0x26c] = 0;
    float const Dt = *reinterpret_cast<float*>(B + 0x1c);
    double const Acc = static_cast<double>(Dt) + AccPrev;
    *reinterpret_cast<float*>(B + 0x258) = static_cast<float>(Acc);

    // TEST AH,0x41 / JNZ: the old value strictly greater than 0 falls through.
    if (AccPrev > 0.0f)
    {
        // +0x26c = 1; the sum strictly above the period re-arms
        // +0x258 = (rand * 3.0517578125e-05f * double(0.7f) - 1.5) * +0x250.
        B[0x26c] = 1;
        if (Acc > Period)
        {
            int const R = rand();
            *reinterpret_cast<float*>(B + 0x258) = static_cast<float>(
                (static_cast<double>(R) * 3.0517578125e-05f * static_cast<double>(0.7f) - 1.5) *
                *reinterpret_cast<float*>(B + 0x250));
        }
    }
    else if (Acc > 0.0)
    {
        // the sum strictly above 0 sets +0x26c.
        B[0x26c] = 1;
    }

    if (B[0x26c] == 0)
    {
        float const* Src = reinterpret_cast<float*>(B + 0x2a0);
        OutRgb3[0] = Src[0];
        OutRgb3[1] = Src[1];
        OutRgb3[2] = Src[2];
        return OutRgb3;
    }

    // U0 = float(rand * 3.0517578125e-05f); Scale = rand * +0x268 *
    // U0 * 3.0517578125e-05f, kept on the FPU.
    float const U0 = static_cast<float>(static_cast<double>(rand()) * 3.0517578125e-05f);
    int const Rand2 = rand();
    double const Scale = static_cast<double>(Rand2) * *reinterpret_cast<float*>(B + 0x268) * U0 *
                         3.0517578125e-05f;
    // the green and blue products are stored as float before the
    // base is added; the red product stays on the FPU. Each sum is stored as float (green's
    // unrounded sum is the one compared below, which clamps to the same value).
    float const GreenProduct = static_cast<float>(Scale * *reinterpret_cast<float*>(B + 0x260));
    float const BlueProduct = static_cast<float>(Scale * *reinterpret_cast<float*>(B + 0x264));
    float R = static_cast<float>(Scale * *reinterpret_cast<float*>(B + 0x25c) +
                                 *reinterpret_cast<float*>(B + 0x2a0));
    float G = GreenProduct + *reinterpret_cast<float*>(B + 0x2a4);
    float Bv = BlueProduct + *reinterpret_cast<float*>(B + 0x2a8);
    // FCOMP 1.0f, TEST AH,5 / JNP -- keep only a value below 1.0,
    // anything else (>=, NaN) becomes 1.0f.
    if (!(R < 1.0f))
        R = 1.0f;
    if (!(G < 1.0f))
        G = 1.0f;
    if (!(Bv < 1.0f))
        Bv = 1.0f;
    OutRgb3[0] = R;
    OutRgb3[1] = G;
    OutRgb3[2] = Bv;
    return OutRgb3;
}

float FNebulaEffect::GetFloat2AC()
{
    FNebulaEffect* const This = this;
    return *reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(This) + 0x2ac);
}

float FNebulaEffect::FogFactor(float const* CamPos3)
{
    FNebulaEffect* const This = this;
    unsigned char* const B = reinterpret_cast<unsigned char*>(This);
    float const FarZ = *reinterpret_cast<float*>(B + 0x2b0);
    Exclusion::NebulaExclusionZone* const Excl =
        *reinterpret_cast<Exclusion::NebulaExclusionZone**>(B + 0x24);
    if (Excl != nullptr)
    {
        // the exclusion zone's slot +0x00 on the camera position
        // (kept on the stack), blended between FarZ and FogNear + FogDistance.
        double const Inside = Excl->ZonePtr->GetImmersion(*reinterpret_cast<Vector const*>(CamPos3));
        float const Span = This->FogNear + This->FogDistance;
        return static_cast<float>((1.0f - Inside) * Span + Inside * FarZ);
    }

    // vt+0x20 (ZoneExteriorFactor). FCOM 1.0f / TEST 5 / JP -> FarZ when
    // >= 1 or unordered; else FarZ / max(F^2, 1e-4f) (FCOM / TEST 5 / JP keeps F^2 when
    // >= the floor).
    double F = This->ZoneExteriorFactor(CamPos3);
    if (!(F < 1.0f))
        return FarZ;
    F = F * F;
    if (F < 0.0001f)
        F = 0.0001f;
    return static_cast<float>(FarZ / F);
}

float FNebulaEffect::GetFloat2B0()
{
    FNebulaEffect* const This = this;
    return *reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(This) + 0x2b0);
}

float FNebulaEffect::FogFarClamp(float Unused)
{
    FNebulaEffect* const This = this;
    (void)Unused;
    unsigned char* const B = reinterpret_cast<unsigned char*>(This);
    // vt+0x40 (GetFloat2B0) + 500.0f (.rdata 0x005d9668), stored
    // rounded to [ESP+4]; the compare below uses the unrounded sum.
    typedef float(__thiscall* FGetFloatFn)(FNebulaEffect* Self);
    const double Sum =
        static_cast<double>(This->GetFloat2B0()) +
        500.0f;
    float V = static_cast<float>(Sum);
    // FCOMP [+0x2bc], TEST AH,5 / JP -- keep the sum when it is
    // >= +0x2bc or unordered; only a strictly smaller sum is replaced by +0x2bc.
    const float Floor = *reinterpret_cast<float*>(B + 0x2bc);
    if (Sum < Floor)
        V = Floor;
    // with an exclusion zone at +0x24 (only that pointer is tested),
    // R = its zone's GetRadius (IZone vt+0x30) * 2 + 500.0 (double .rdata 0x005db0c0).
    // FCOMP V against R, TEST AH,0x41 / JNZ: V <= R or unordered returns R, else V.
    Exclusion::NebulaExclusionZone* const Excl =
        *reinterpret_cast<Exclusion::NebulaExclusionZone**>(B + 0x24);
    if (Excl != nullptr)
    {
        const double Radius = static_cast<double>(Excl->ZonePtr->GetRadius());
        const double R = (Radius + Radius) + 500.0;
        if (!(V > R))
            return static_cast<float>(R);
    }
    return V;
}

unsigned char const* FNebulaEffect::GetAmbientBytes()
{
    FNebulaEffect* const This = this;
    unsigned char* const B = reinterpret_cast<unsigned char*>(This);
    if (B[0x2cc] == 0)
        return nullptr;
    return B + 0x2cd;
}

Universe::IZone* FNebulaEffect::GetZone()
{
    FNebulaEffect* const This = this;
    return This->Zone;
}

float FNebulaEffect::GetZoneCenterRadius(float* OutPos3)
{
    FNebulaEffect* const This = this;
    Vector const& Pos = This->Zone->GetPos();
    OutPos3[0] = Pos.x;
    OutPos3[1] = Pos.y;
    OutPos3[2] = Pos.z;
    return This->Zone->GetRadius();
}

FNebulaEffectCompanion::FNebulaEffectCompanion()
{
    Values[0] = nullptr;
    Values[1] = nullptr;
    Values[2] = nullptr;
    Values[3] = nullptr;
}

FNebulaEffectCompanion::~FNebulaEffectCompanion()
{
    CliEffectCompanionClear(Values);
}

FNebulaCloudsListStorage::FNebulaCloudsListStorage()
{
    void* ListSent = operator new(0xc);
    *static_cast<void**>(ListSent) = ListSent;
    *reinterpret_cast<void**>(static_cast<unsigned char*>(ListSent) + 4) = ListSent;
    Sentinel = static_cast<FNebulaCloudsListNode*>(ListSent);
    Count = 0;
}

FNebulaCloudsListStorage::~FNebulaCloudsListStorage()
{
    FNebulaCloudsListNode* Node = Sentinel->Next;
    while (Node != Sentinel)
    {
        FNebulaCloudsListNode* const Next = Node->Next;
        operator delete(Node);
        Node = Next;
    }
    operator delete(Sentinel);
    Sentinel = nullptr;
    Count = 0;
}

FNebulaEffect::FNebulaEffect()
    : Field24(0), Field28(0), Flag2C(0), Field30(0)
{
    FNebulaEffect* const This = this;
    // member construction registers the native cleanup order for exceptions.
    // Remaining stores deliberately leave every other byte untouched.
    unsigned char* const B = reinterpret_cast<unsigned char*>(This);

    // The compiler installs the derived vptr; +0x24c retains its explicit zero store.
    B[0x24c] = 0;

    // .
    *reinterpret_cast<unsigned*>(B + 0xa0) = 0;
    *reinterpret_cast<unsigned*>(B + 0xdc) = 0;
    *reinterpret_cast<unsigned*>(B + 0xe0) = 0;
    This->Zone = nullptr;
    This->FlagLoaded = 0;
    B[0x2cc] = 0;
    *reinterpret_cast<unsigned*>(B + 0xd0) = 0;
    *reinterpret_cast<unsigned*>(B + 0x294) = 0;
    *reinterpret_cast<unsigned*>(B + 0x2b4) = 0;
    *reinterpret_cast<unsigned*>(B + 0x5c) = 0;
    This->ExclusionMgr = nullptr;

    // the routine, erase(begin, end) of the (empty) zone_shell map.
    NebulaZoneShells(This).clear();

    // , 0x00504b87.
    B[0x26c] = 0;
    *reinterpret_cast<unsigned*>(B + 0x270) = 0;
    *reinterpret_cast<unsigned*>(B + 0x1c) = 0;

    // Constants of 0x00504a77..0x00504b8a.
    *reinterpret_cast<unsigned*>(reinterpret_cast<unsigned char*>(This) + 0xc0) = 4;
    *reinterpret_cast<unsigned*>(reinterpret_cast<unsigned char*>(This) + 0xc4) = 8;
    *reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(This) + 0xc8) = 1.0f;
    *reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(This) + 0xcc) = 1.0f;
    *reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(This) + 0xd4) =
        BitsToFloat(0x3f400000);
    *reinterpret_cast<unsigned*>(reinterpret_cast<unsigned char*>(This) + 0xd8) = 1;

    *reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(This) + 0x50) =
        BitsToFloat(0x3dcccccd);
    *reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(This) + 0x54) =
        BitsToFloat(0x41200000);
    *reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(This) + 0x58) =
        BitsToFloat(0x3e4ccccd);

    *reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(This) + 0x250) =
        BitsToFloat(0x41200000);
    *reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(This) + 0x254) =
        BitsToFloat(0x3f000000);
    *reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(This) + 0x258) =
        BitsToFloat(0xbf800000);
    *reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(This) + 0x25c) =
        BitsToFloat(0x3e800000);
    *reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(This) + 0x260) =
        BitsToFloat(0x3e800000);
    *reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(This) + 0x264) =
        BitsToFloat(0x3e800000);
    *reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(This) + 0x268) = 1.0f;

    *reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(This) + 0x274) =
        BitsToFloat(0x40400000);
    *reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(This) + 0x278) =
        BitsToFloat(0x3f000000);
    *reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(This) + 0x27c) =
        BitsToFloat(0xbf800000);
    *reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(This) + 0x280) = 1.0f;
    *reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(This) + 0x284) = 1.0f;
    *reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(This) + 0x288) = 1.0f;
    *reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(This) + 0x28c) =
        BitsToFloat(0x3e800000);
    *reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(This) + 0x290) =
        BitsToFloat(0x3f400000);

    *reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(This) + 0x2c4) =
        BitsToFloat(0x3f000000);
    *reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(This) + 0x2c8) =
        BitsToFloat(0x3fe00000);
    *reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(This) + 0x2bc) =
        BitsToFloat(0x459c4000);

}

FNebulaEffect* NebulaConstruct(FNebulaEffect* This)
{
    return ::new (This) FNebulaEffect();
}

FNebulaEffect* NebulaCreate()
{
    FNebulaEffect* const Nebula = new FNebulaEffect;
    Nebula->Detail(G_NebulaDetail);
    return Nebula;
}


void NebulaDestroy(FNebulaEffect* Nebula)
{
    // (cdecl): a non-null nebula gets vt+0x58 (the deleting dtor) with 1.
    if (Nebula == nullptr)
        return;
#ifdef OUTPOST_FRONTIER_DX9RENDERER
    NebulaVolume::UnregisterNebula(Nebula); // port-only bookkeeping (nebula_volume.cpp)
#endif
    using FDel = void*(__thiscall*)(FNebulaEffect*, unsigned char);
    delete Nebula;
}

void FNebulaEffect::Detail(float Detail)
{
    FNebulaEffect* const Nebula = this;
    // (thiscall, RET 4) -- vt+0x54: the routine(detail) on every clouds object of
    // the list (+0xf8), then the routine. The global detail is not written here (the options
    // callback the routine does that); no test of `this` or of a node.
    FNebulaCloudsListNode* const Sentinel = Nebula->Clouds.Sentinel;
    for (FNebulaCloudsListNode* N = Sentinel->Next; N != Sentinel; N = N->Next)
        LocalNebulaCloudsApplyDetail(N->Clouds, Detail);
    NebulaRebuild(Nebula);
}

void NebulaSetDetail(FNebulaEffect* Nebula, float Detail)
{
    Nebula->FNebulaEffect::Detail(Detail);
}

void NebulaApplyGlobalDetail(float Detail)
{
    // — PerfOptions Nebula callback.
    RaiseActiveSpaceCameraCut(); // the routine, before the store
    G_NebulaDetail = Detail;
    // every nebula on the StarSys list gets slot 21 (+0x54) Detail.
    std::list<FNebulaEffect*>& Nebulae = StarSysNebulaList();
    for (std::list<FNebulaEffect*>::iterator It = Nebulae.begin(); It != Nebulae.end(); ++It)
    {
        FNebulaEffect* const Nebula = *It;
        using FDetail = void(__thiscall*)(FNebulaEffect*, float);
        Nebula->Detail(Detail);
    }
    // the routine, the TexturePanels detail walk.
    TexturePanelsApplyDetailAll(Detail);
}

void NebulaOrientBolts(FNebulaEffect* Nebula, float Tx, float Ty, float Tz)
{
    // (thiscall, RET 0xc), 0x00507340..0x005074d9: re-aim each bolt anchor's
    // axis (+0x14..+0x1c) square to the direction from the anchor to the target. No test of
    // `this`, the zone or the buffer; no zero-length test.
    unsigned char* const B = reinterpret_cast<unsigned char*>(Nebula);

    // the target relative to the zone position (GetPos, vt+0x10), as
    // floats.
    Vector const ZonePos = Nebula->Zone->GetPos();
    float const RelX = Tx - ZonePos.x;
    float const RelY = Ty - ZonePos.y;
    float const RelZ = Tz - ZonePos.z;

    float* const Segs = *reinterpret_cast<float**>(B + 0xe0);
    for (int I = 0; I < *reinterpret_cast<int*>(B + 0xdc); ++I)
    {
        float* const Seg = Segs + I * 8;
        // D = normalize(Rel - anchor). The z difference is stored as a
        // float and squared as unrounded * float; Inv = float(1.0f / sqrt((z2 + y2) + x2)).
        // Dx and Dz are stored as floats, Dy stays on the FPU; the dot below uses the
        // unrounded Dz, the cross the stored one.
        double const A = static_cast<double>(RelX) - Seg[0];
        double const Bv = static_cast<double>(RelY) - Seg[1];
        double const C = static_cast<double>(RelZ) - Seg[2];
        float const CF = static_cast<float>(C);
        float const Inv = static_cast<float>(1.0f / sqrt((C * CF + Bv * Bv) + A * A));
        float const Dx = static_cast<float>(A * Inv);
        double const Dy = Bv * Inv;
        double const DzExt = static_cast<double>(CF) * Inv;
        float const DzF = static_cast<float>(DzExt);

        // |(Dz * o7 + Dy * o6) + Dx * o5| vs double 0.25; TEST AH,0x41 /
        // JNZ -- only strictly above re-aims.
        double const Dot =
            fabs((DzExt * Seg[7] + Dy * Seg[6]) + static_cast<double>(Dx) * Seg[5]);
        if (Dot > 0.25)
        {
            // N = D x O, on the FPU (Dz the stored float).
            double const Cx = Dy * Seg[7] - static_cast<double>(DzF) * Seg[6];
            double const Cy = static_cast<double>(DzF) * Seg[5] - static_cast<double>(Dx) * Seg[7];
            double const Cz = static_cast<double>(Dx) * Seg[6] - Dy * Seg[5];
            // the same normalisation (Cz stored as float first).
            float const CzF = static_cast<float>(Cz);
            float const CInv = static_cast<float>(1.0f / sqrt((Cz * CzF + Cy * Cy) + Cx * Cx));
            float const Ncx = static_cast<float>(Cx * CInv);
            double const Ncy = Cy * CInv;
            double const Ncz = static_cast<double>(CzF) * CInv;
            // O = N x D, each component stored as float.
            Seg[5] = static_cast<float>(Ncy * DzF - Ncz * Dy);
            Seg[6] = static_cast<float>(Ncz * Dx - static_cast<double>(DzF) * Ncx);
            Seg[7] = static_cast<float>(Dy * Ncx - Ncy * Dx);
        }
    }
}

namespace
{
    // 's projection step (0x00506a84..0x00506ad7, and the two caps): Seg xyz =
    // Zone->GetSurfacePoint(Point) (IZone vt+0x44) minus the zone position, each difference a float.
    void NebulaBoltProject(Universe::IZone* Zone, Vector const& Point, Vector const& ZonePos,
                           float* Seg)
    {
        Vector const Surface = Zone->GetSurfacePoint(Point);
        Seg[0] = Surface.x - ZonePos.x;
        Seg[1] = Surface.y - ZonePos.y;
        Seg[2] = Surface.z - ZonePos.z;
    }

    // 's pull-in step (0x00506afa..0x00506b21 and the caps): Seg xyz *=
    // (Len - +0xd4 * +0xa4) / Len, the factor on the FPU and no zero test. Len is the caller's
    // sqrt; the sites differ in their summation order.
    void NebulaBoltShrink(FNebulaEffect* Nebula, float* Seg, double Len)
    {
        unsigned char* const B = reinterpret_cast<unsigned char*>(Nebula);
        double const Margin = static_cast<double>(*reinterpret_cast<float*>(B + 0xd4)) *
                              *reinterpret_cast<float*>(B + 0xa4);
        double const Shrink = (Len - Margin) / Len;
        Seg[0] = static_cast<float>(Shrink * Seg[0]);
        Seg[1] = static_cast<float>(Shrink * Seg[1]);
        Seg[2] = static_cast<float>(Shrink * Seg[2]);
    }
}

void NebulaRebuild(FNebulaEffect* Nebula)
{
    // (thiscall, no arguments), 0x00506600..0x00506cb2: the bolt-anchor buffer
    // (+0xe0, count +0xdc, 0x20 bytes each) rebuilt on the zone's surface. No test of `this`.
    unsigned char* const B = reinterpret_cast<unsigned char*>(Nebula);

    // the routine (operator delete -> free) on the old buffer, then
    // +0xe0 = +0xdc = 0.
    operator delete(*reinterpret_cast<void**>(B + 0xe0));
    *reinterpret_cast<void**>(B + 0xe0) = nullptr;
    *reinterpret_cast<int*>(B + 0xdc) = 0;

    // the detail the field FCOMP double 0.05 (0x005d5b58), TEST AH,5
    // / JNP -- only an ordered "less" returns; then the zone (+0x20) is the one pointer tested.
    if (static_cast<double>(G_NebulaDetail) < 0.05 || Nebula->Zone == nullptr)
        return;

    // the zone's GetPos (vt+0x10), copied to the stack.
    Vector const ZonePos = Nebula->Zone->GetPos();

    // , on the FPU: Ease = 1.0f - (1.0 - detail)^2;
    // Scale = float((the field * (1 - Ease) + Ease) * +0xa4);
    // Density = float((1.0f - detail) * + detail);
    // +0xb8 = float(((1 - Ease) * + Ease) * +0xb4).
    double const OneMinusDetail = 1.0 - static_cast<double>(G_NebulaDetail);
    double const Ease = 1.0f - OneMinusDetail * OneMinusDetail;
    double const OneMinusEase = 1.0f - Ease;
    float const Scale = static_cast<float>((G_NebulaDetailEase * OneMinusEase + Ease) *
                                           *reinterpret_cast<float*>(B + 0xa4));
    float const Density = static_cast<float>(
        (1.0 - static_cast<double>(G_NebulaDetail)) * G_NebulaDetailMix + G_NebulaDetail);
    *reinterpret_cast<float*>(B + 0xb8) = static_cast<float>(
        (OneMinusEase * G_NebulaDetailFog + Ease) * *reinterpret_cast<float*>(B + 0xb4));

    // the two per-slice bit counts scaled by Density, + 0.5f, truncated
    // (ftol): +0xc4 is the count at the middle slice ([ESP+0x1c]), +0xc0 the one at the ends
    // ([ESP+0x24]).
    int const MidCount = static_cast<int>(
        static_cast<double>(*reinterpret_cast<int*>(B + 0xc4)) * Density + 0.5f);
    int const EndCount = static_cast<int>(
        static_cast<double>(*reinterpret_cast<int*>(B + 0xc0)) * Density + 0.5f);

    // no slices (+0xbc) -> nothing more.
    int const PlaneSlices = *reinterpret_cast<int*>(B + 0xbc);
    if (PlaneSlices <= 0)
        return;

    // the shape weights (+0x80, count +0xa0) normalised -- the sum on
    // the FPU, then each weight = float(weight * (1.0f / sum)) with no zero test; the count is
    // re-read every step.
    {
        float* const Weights = reinterpret_cast<float*>(B + 0x80);
        int const WeightCount = *reinterpret_cast<int*>(B + 0xa0);
        double Sum = 0.0f;
        for (int I = 0; I < WeightCount; ++I)
            Sum += Weights[I];
        if (WeightCount > 0)
        {
            double const InvSum = 1.0f / Sum;
            for (int I = 0; I < *reinterpret_cast<int*>(B + 0xa0); ++I)
                Weights[I] = static_cast<float>(InvSum * Weights[I]);
        }
    }

    // Mid = float((slices - 1) * 0.5). One slice takes MidCount;
    // otherwise each slice adds floor((1 - W) * EndCount + MidCount * W + 0.5), W rising
    // T / Mid to the middle and falling 1 - (T - Mid) / Mid after it (here through a float
    // 1.0f / Mid; FCOM / TEST AH,0x41 / JNZ -- only T strictly above Mid falls).
    float const Mid = static_cast<float>(static_cast<double>(PlaneSlices - 1) * 0.5);
    int SegTotal = 0;
    if (PlaneSlices == 1)
    {
        SegTotal = MidCount;
    }
    else
    {
        float const InvMid = static_cast<float>(1.0f / static_cast<double>(Mid));
        double const EndCountD = EndCount;
        float const MidCountF = static_cast<float>(MidCount);
        for (int I = 0; I < PlaneSlices; ++I)
        {
            double const T = I;
            double W;
            if (T > Mid)
                W = 1.0f - (T - Mid) * InvMid;
            else
                W = T * InvMid;
            SegTotal += static_cast<int>(floor((1.0 - W) * EndCountD + MidCountF * W + 0.5));
        }
    }

    // +0xdc = total + 2 (the two polar caps); +0xe0 = operator new
    // (count << 5), neither tested nor cleared.
    int const SegCount = SegTotal + 2;
    *reinterpret_cast<int*>(B + 0xdc) = SegCount;
    float* const Segs =
        static_cast<float*>(operator new(static_cast<unsigned>(SegCount) << 5));
    *reinterpret_cast<float**>(B + 0xe0) = Segs;

    // per segment (the count re-read every step): +0xc radius,
    // +0x14/+0x18 sin/cos of a random angle, +0x1c = 0, +0x10 a weighted random shape.
    for (int I = 0; I < *reinterpret_cast<int*>(B + 0xdc); ++I)
    {
        float* const S = Segs + I * 8;
        // +0xc = float((rand * c * (+0xcc - +0xc8) + +0xc8) * Scale).
        float const RadiusLo = *reinterpret_cast<float*>(B + 0xc8);
        float const RadiusHi = *reinterpret_cast<float*>(B + 0xcc);
        int const RRadius = rand();
        S[3] = static_cast<float>(
            (static_cast<double>(RRadius) * 3.0517578125e-05f *
                 (static_cast<double>(RadiusHi) - RadiusLo) +
             RadiusLo) *
            Scale);
        // angle = rand * c * 2pi (double 0x005c97f8) on the FPU.
        int const RAngle = rand();
        double const Angle =
            static_cast<double>(RAngle) * 3.0517578125e-05f * 6.2831854820251465;
        S[5] = static_cast<float>(sin(Angle));
        S[6] = static_cast<float>(cos(Angle));
        S[7] = 0.0f;
        // pick = float(rand * c); the weights accumulate on the FPU and
        // the first running sum strictly above the pick (FCOMP, TEST AH,5 / JNP) stores that
        // index's shape (+0x60[j]) at +0x10. No match leaves +0x10 as it was.
        int const RPick = rand();
        float const Pick = static_cast<float>(static_cast<double>(RPick) * 3.0517578125e-05f);
        int const WeightCount = *reinterpret_cast<int*>(B + 0xa0);
        double Running = 0.0f;
        for (int J = 0; J < WeightCount; ++J)
        {
            Running += reinterpret_cast<float*>(B + 0x80)[J];
            if (Pick < Running)
            {
                memcpy(&S[4], B + 0x60 + J * 4, sizeof(float));
                break;
            }
        }
    }

    // the latitude rings (the slice count re-read every step). The
    // phase ([ESP+0x28], 0 at the start) carries over between slices.
    float* Seg = Segs;
    float Phase = 0.0f;
    for (int Slice = 0; Slice < *reinterpret_cast<int*>(B + 0xbc); ++Slice)
    {
        int const Slices = *reinterpret_cast<int*>(B + 0xbc);
        int Bits;
        if (Slices == 1)
        {
            // one slice takes MidCount.
            Bits = MidCount;
        }
        else
        {
            // W as above but with a true division by Mid; then past the
            // middle (FCOMP Mid, TEST AH,0x41 / JNZ -- strictly above only) the phase becomes
            // float(pi (0x005c8a5c) / Bits), no zero test.
            float const T = static_cast<float>(Slice);
            double W;
            if (T > Mid)
                W = 1.0f - (static_cast<double>(T) - Mid) / Mid;
            else
                W = static_cast<double>(T) / Mid;
            Bits = static_cast<int>(
                floor((1.0 - W) * EndCount + static_cast<double>(MidCount) * W + 0.5));
            if (T > Mid)
                Phase = static_cast<float>(3.1415927f / static_cast<double>(Bits));
        }

        // Lat = acos((((slice + 1.0f) / (slices + 1.0f)) * 2 - 1.0f)
        // * +0xd0), on the FPU.
        double const Lat =
            acos((((static_cast<double>(Slice) + 1.0f) / (static_cast<double>(Slices) + 1.0f)) *
                      2.0 -
                  1.0f) *
                 *reinterpret_cast<float*>(B + 0xd0));

        if (Bits > 0)
        {
            // Bits as a double; cos(Lat) stored as float, sin(Lat) as a
            // double; the point's y = float(zone y + that float cos).
            double const BitsD = Bits;
            float const CosLat = static_cast<float>(cos(Lat));
            double const SinLat = sin(Lat);
            float const PointY = ZonePos.y + CosLat;
            for (int Bit = 0; Bit < Bits; ++Bit)
            {
                // angle = bit * 2pi / Bits + phase; x = cos * sinLat
                // (stored, and kept unrounded for the point), z = float(sin * sinLat); y is the
                // float cos (overwritten by the projection below).
                Seg[1] = CosLat;
                double const Angle =
                    static_cast<double>(Bit) * 6.2831854820251465 / BitsD + Phase;
                double const DirX = cos(Angle) * SinLat;
                Seg[0] = static_cast<float>(DirX);
                float const DirZ = static_cast<float>(sin(Angle) * SinLat);
                Seg[2] = DirZ;
                Vector Point;
                Point.x = static_cast<float>(ZonePos.x + DirX);
                Point.y = PointY;
                Point.z = ZonePos.z + DirZ;
                // the surface point minus the zone position.
                NebulaBoltProject(Nebula->Zone, Point, ZonePos, Seg);
                // Len = sqrt((z^2 + y^2) + x^2), then the pull-in.
                double const Len = sqrt((static_cast<double>(Seg[2]) * Seg[2] +
                                         static_cast<double>(Seg[1]) * Seg[1]) +
                                        static_cast<double>(Seg[0]) * Seg[0]);
                NebulaBoltShrink(Nebula, Seg, Len);
                Seg += 8;
            }
        }
    }

    // the +Y cap, zone position + (0, 1.0f, 0); same length order.
    Vector Point;
    Point.x = ZonePos.x;
    Point.y = ZonePos.y + 1.0f;
    Point.z = ZonePos.z;
    NebulaBoltProject(Nebula->Zone, Point, ZonePos, Seg);
    double const TopLen = sqrt((static_cast<double>(Seg[2]) * Seg[2] +
                                static_cast<double>(Seg[1]) * Seg[1]) +
                               static_cast<double>(Seg[0]) * Seg[0]);
    NebulaBoltShrink(Nebula, Seg, TopLen);
    Seg += 8;

    // the -Y cap; this length sums (x^2 + y^2) + z^2.
    Point.y = ZonePos.y - 1.0f;
    NebulaBoltProject(Nebula->Zone, Point, ZonePos, Seg);
    double const BottomLen = sqrt((static_cast<double>(Seg[0]) * Seg[0] +
                                   static_cast<double>(Seg[1]) * Seg[1]) +
                                  static_cast<double>(Seg[2]) * Seg[2]);
    NebulaBoltShrink(Nebula, Seg, BottomLen);
}

void FNebulaEffect::Update(void* Camera, float Dt)
{
    FNebulaEffect* const Nebula = this;
    // — vtable +0x08 (__thiscall, RET 8). 506f20 MOV EAX,[ESP+8] /
    // MOV [ESI+0x1c],EAX: the SECOND argument (frame dt) lands at +0x1c; the first is the
    // camera (506f9c).
    unsigned char* const B = reinterpret_cast<unsigned char*>(Nebula);
    *reinterpret_cast<float*>(B + 0x1c) = Dt;

    // after a (re)load (+0x4c), +0x2b4 = the largest clouds reach
    // (+0xc0 * 0.5 + +0xa8; FCOM / TEST AH,0x41 / JNZ keeps the old value on <= and
    // unordered). The exclusion manager gets its square at +0x14 -- the listing inlines what
    // NebulaExclusionZoneMgr::set_max_distance does (Common module storage, the same store).
    if (B[0x4c] != 0)
    {
        *reinterpret_cast<float*>(B + 0x2b4) = 0.0f;
        FNebulaCloudsListNode* const Sentinel = Nebula->Clouds.Sentinel;
        for (FNebulaCloudsListNode* N = Sentinel->Next; N != Sentinel; N = N->Next)
        {
            unsigned char const* const C = N->Clouds->Raw;
            double const Reach =
                static_cast<double>(*reinterpret_cast<float const*>(C + 0xc0)) * 0.5f +
                *reinterpret_cast<float const*>(C + 0xa8);
            if (Reach > *reinterpret_cast<float*>(B + 0x2b4))
                *reinterpret_cast<float*>(B + 0x2b4) = static_cast<float>(Reach);
        }
        if (Nebula->ExclusionMgr != nullptr)
            Nebula->ExclusionMgr->set_max_distance(*reinterpret_cast<float*>(B + 0x2b4));
        B[0x4c] = 0;
    }

    // camera vt+0x0c (__stdcall), copied to a local.
    ICamera* const Cam = static_cast<ICamera*>(Camera);
    float const* const CamPosPtr = Cam->GetPosition();
    Vector CamPos;
    CamPos.x = CamPosPtr[0];
    CamPos.y = CamPosPtr[1];
    CamPos.z = CamPosPtr[2];
    // The camera's cut flag, byte +0xbc of the camera object (FSpaceCamera::FlagBC, raised by
    // when the view jumps). Read raw at 00506fe1 and 0050701d.
    unsigned char const CameraCut = static_cast<unsigned char const*>(Camera)[0xbc];

    // not yet fully inside (+0x294 < 1.0; TEST AH,0x5 / JP skips >= and
    // unordered): re-orient the bolts (the routine) when the +0x58 timer is above 1.0 or the
    // camera cut, then restart that timer.
    if (*reinterpret_cast<float*>(B + 0x294) < 1.0)
    {
        if (*reinterpret_cast<float*>(B + 0x58) > 1.0f || CameraCut != 0)
        {
            NebulaOrientBolts(Nebula, CamPos.x, CamPos.y, CamPos.z);
            *reinterpret_cast<float*>(B + 0x58) = 0.0f;
        }
    }

    // partly inside (+0x294 > 0), the exclusion manager is sorted around
    // the camera (vt+0x08, sort) on a camera cut, or when +0x50 is above 0.25 and +0x58 is not
    // zero (TEST AH,0x44 / JNP skips only an ordered equal).
    if (*reinterpret_cast<float*>(B + 0x294) > 0.0)
    {
        bool bSort = CameraCut != 0;
        if (!bSort)
            bSort = *reinterpret_cast<float*>(B + 0x294) > 0.0 &&
                    *reinterpret_cast<float*>(B + 0x50) > 0.25f &&
                    !(*reinterpret_cast<float*>(B + 0x58) == 0.0f);
        if (bSort && Nebula->ExclusionMgr != nullptr)
            Nebula->ExclusionMgr->sort(&CamPos);
    }

    // depth into the zone. Dir = camera - zone position (the routine, the
    // Vector(x, y, z) ctor); zone vt+0x30 is called and its result dropped; Len = |Dir|; Dir is
    // scaled by 1 / Len and zone vt+0x40 gives the surface distance along it. D = Len - that:
    // positive outside. +0x294 = 0 for D > 0 (TEST AH,0x41 / JNZ), else -D / (+0x298 + +0x29c)
    // capped at 1.0 (-D above the span gives 1.0; <= and unordered divide).
    Universe::IZone const* const Zone = Nebula->Zone;
    Vector const& ZonePos = Zone->GetPos();
    Vector Dir;
    Dir.x = CamPos.x - ZonePos.x;
    Dir.y = CamPos.y - ZonePos.y;
    Dir.z = CamPos.z - ZonePos.z;
    (void)Zone->GetRadius();
    float const Len = static_cast<float>(sqrt(static_cast<double>(Dir.y) * Dir.y +
                                              static_cast<double>(Dir.z) * Dir.z +
                                              static_cast<double>(Dir.x) * Dir.x));
    float const FogSpan = Nebula->FogDistance + Nebula->FogNear;
    double const InvLen = 1.0 / Len;
    Dir.x = static_cast<float>(Dir.x * InvLen);
    Dir.y = static_cast<float>(Dir.y * InvLen);
    Dir.z = static_cast<float>(Dir.z * InvLen);
    float const Depth = static_cast<float>(static_cast<double>(Len) - Zone->GetRadiusAlong(Dir));
    if (Depth > 0.0f)
        *reinterpret_cast<float*>(B + 0x294) = 0.0f;
    else
    {
        double const Inside = -static_cast<double>(Depth);
        if (Inside > FogSpan)
            *reinterpret_cast<float*>(B + 0x294) = 1.0f;
        else
            *reinterpret_cast<float*>(B + 0x294) = static_cast<float>(Inside / FogSpan);
    }

    if (*reinterpret_cast<float*>(B + 0x294) > 0.0)
    {
        // fog far = +0x298 + +0x29c, fog near = +0x29c.
        *reinterpret_cast<float*>(B + 0x2b0) = Nebula->FogDistance + Nebula->FogNear;
        *reinterpret_cast<float*>(B + 0x2ac) = Nebula->FogNear;

        // the current exclusion zone (+0x24) is dropped when the camera's
        // immersion in its zone (IZone vt+0x00) falls below 1e-4 (TEST AH,0x5 / JP keeps >=
        // and unordered); +0x28 keeps that immersion.
        Exclusion::NebulaExclusionZone** const ExclSlot =
            reinterpret_cast<Exclusion::NebulaExclusionZone**>(B + 0x24);
        if (*ExclSlot != nullptr)
        {
            float const Immersion = (*ExclSlot)->ZonePtr->GetImmersion(CamPos);
            *reinterpret_cast<float*>(B + 0x28) = Immersion;
            if (Immersion < 9.999999747378752e-05f)
            {
                *ExclSlot = nullptr;
                *reinterpret_cast<unsigned*>(B + 0x28) = 0;
                B[0x2c] = 0;
                *reinterpret_cast<void**>(B + 0x30) = nullptr;
            }
        }

        // with none, ask the manager straight into +0x24/+0x28 (its bool
        // is ignored); a zone found there looks its shell id (+0x0c) up in the +0x38 map
        // (the routine lower_bound, then !(key < node key)) and, when present, raises +0x2c
        // and stores the mapped value at +0x30.
        if (*ExclSlot == nullptr && Nebula->ExclusionMgr != nullptr)
        {
            Nebula->ExclusionMgr->is_in_exclusion_zone(
                CamPos, ExclSlot, reinterpret_cast<float*>(B + 0x28), true);
            if (*ExclSlot != nullptr)
            {
                unsigned const Key = (*ExclSlot)->ZoneShellName.Id;
                FNebulaZoneShellMap& Shells = NebulaZoneShells(Nebula);
                FNebulaZoneShellMap::iterator const Found = NebulaFindZoneShell(Shells, Key);
                if (Found != Shells.end())
                {
                    B[0x2c] = 1;
                    *reinterpret_cast<FNebulaZoneShell**>(B + 0x30) = Found->second;
                }
            }
        }

        // inside a zone with immersion above 0 (TEST AH,0x41 / JNZ) whose
        // fog_far (+0x08) is beyond the near value, the far distance blends toward it:
        // +0x2b0 = (1 - f) * +0x2b0 + fog_far * f.
        Exclusion::NebulaExclusionZone* const Excl = *ExclSlot;
        if (Excl != nullptr && *reinterpret_cast<float*>(B + 0x28) > 0.0f)
        {
            float const FogFar = Excl->FogFar;
            if (FogFar > *reinterpret_cast<float*>(B + 0x2ac))
            {
                float const F = *reinterpret_cast<float*>(B + 0x28);
                *reinterpret_cast<float*>(B + 0x2b0) = static_cast<float>(
                    (1.0f - static_cast<double>(F)) * *reinterpret_cast<float*>(B + 0x2b0) +
                    static_cast<double>(FogFar) * F);
            }
        }
    }
    else
    {
        // outside, the fog range follows the depth: near = D + +0x29c,
        // far = near + +0x298; an enabled ambient light (+0x270, Flags bit 3) is switched off
        // through its vt+0x08 (GameLight disable, the routine).
        double const Near = static_cast<double>(Depth) + Nebula->FogNear;
        *reinterpret_cast<float*>(B + 0x2ac) = static_cast<float>(Near);
        *reinterpret_cast<float*>(B + 0x2b0) = static_cast<float>(Near + Nebula->FogDistance);
        FGameLight* const Light = *reinterpret_cast<FGameLight**>(B + 0x270);
        if (Light != nullptr && (Light->Flags & 0x08) != 0)
            GameLightVirtualDisable(Light);
    }

    // within 300 of the surface (TEST AH,0x5 / JP skips >= and
    // unordered), every Clouds gets the routine(camera, dt, +0x294, exclusion manager).
    if (Depth < 300.0)
    {
        FNebulaCloudsListNode* const CloudSent = Nebula->Clouds.Sentinel;
        for (FNebulaCloudsListNode* N = CloudSent->Next; N != CloudSent; N = N->Next)
            NebulaCloudsUpdate(N->Clouds, Camera, Dt, *reinterpret_cast<float*>(B + 0x294),
                               Nebula->ExclusionMgr);
    }

    // fully inside (TEST AH,0x1 / JNZ skips < and unordered), the ambient
    // light tick the routine with the camera position.
    if (*reinterpret_cast<float*>(B + 0x294) >= 1.0f)
    {
        float const CamCopy[3] = {CamPos.x, CamPos.y, CamPos.z};
        NebulaAmbientTick(Nebula, CamCopy);
    }

    // the three timers advance by dt.
    *reinterpret_cast<float*>(B + 0x58) = Dt + *reinterpret_cast<float*>(B + 0x58);
    *reinterpret_cast<float*>(B + 0x50) = Dt + *reinterpret_cast<float*>(B + 0x50);
    *reinterpret_cast<float*>(B + 0x54) = Dt + *reinterpret_cast<float*>(B + 0x54);
}

void NebulaUpdate(FNebulaEffect* Nebula, void* Camera, float Dt)
{
    Nebula->FNebulaEffect::Update(Camera, Dt);
}

namespace
{
    // (cdecl, one float): the estimate Y = 0x5f3759df - (bits >> 1) and one Newton
    // step (1.5f - Y * Y * X * 0.5f) * Y, returned on the FPU. the routine also inlines the
    // same sequence twice.
    double NebulaInvSqrt(float X)
    {
        int XBits = 0;
        memcpy(&XBits, &X, sizeof(XBits));
        int const YBits = 0x5f3759df - (XBits >> 1);
        float Y = 0.0f;
        memcpy(&Y, &YBits, sizeof(Y));
        return (1.5f - static_cast<double>(Y) * Y * X * 0.5f) * Y;
    }

    // (thiscall on the vector, RET 4): each component = float(S * component).
    void NebulaScaleVec3(float* V, float S)
    {
        V[0] = S * V[0];
        V[1] = S * V[1];
        V[2] = S * V[2];
    }

    // The the routine fog-sheet edge: normalize (the the routine sequence on the float
    // (x^2 + z^2) + y^2), then scale to float(Zone->GetRadiusAlong(edge) * 1.15f) / Vec3Length(edge)
    // (the routine), both factors on the FPU.
    void NebulaSheetEdge(Universe::IZone* Zone, float* Edge)
    {
        float const LenSq = static_cast<float>(
            (static_cast<double>(Edge[0]) * Edge[0] + static_cast<double>(Edge[2]) * Edge[2]) +
            static_cast<double>(Edge[1]) * Edge[1]);
        double const Inv = NebulaInvSqrt(LenSq);
        Edge[0] = static_cast<float>(Edge[0] * Inv);
        Edge[1] = static_cast<float>(Edge[1] * Inv);
        Edge[2] = static_cast<float>(Edge[2] * Inv);
        Vector Dir;
        Dir.x = Edge[0];
        Dir.y = Edge[1];
        Dir.z = Edge[2];
        float const Reach = Zone->GetRadiusAlong(Dir) * 1.15f;
        double const Scale = Reach / Vec3Length(Edge);
        Edge[0] = static_cast<float>(Edge[0] * Scale);
        Edge[1] = static_cast<float>(Edge[1] * Scale);
        Edge[2] = static_cast<float>(Edge[2] * Scale);
    }
}

void NebulaUpdateBoltCamera(FNebulaEffect* Nebula, float CamX, float CamY, float CamZ)
{
    // (thiscall, RET 0xc), 0x005074e0..0x00507e63: two crossing fog sheets through
    // the zone, textured with the fill shape (+0x5c). Only +0x5c is tested; the arguments are the
    // camera position.
    unsigned char* const B = reinterpret_cast<unsigned char*>(Nebula);
    FTexShape* const Shape = *reinterpret_cast<FTexShape**>(B + 0x5c);
    if (Shape == nullptr)
        return;

    // GetSize (vt+0x28), GetPos (vt+0x10) and the GetOrientation block
    // (vt+0x14, nine dwords), split into its columns.
    Universe::IZone* const Zone = Nebula->Zone;
    Vector const Size = Zone->GetSize();
    Vector const ZonePos = Zone->GetPos();
    Matrix const Orient = Zone->GetOrientation();
    float Col0[3] = {Orient.E[0][0], Orient.E[1][0], Orient.E[2][0]};
    float Col1[3] = {Orient.E[0][1], Orient.E[1][1], Orient.E[2][1]};
    float Col2[3] = {Orient.E[0][2], Orient.E[1][2], Orient.E[2][2]};

    // FCOMP Size.z, TEST AH,0x41 / JNZ -- only Size.x strictly above
    // Size.z makes column 0 the long axis (A) with Size.x; otherwise column 2 with Size.z. The
    // other of the two is B, used only for the second sheet's fade.
    float AxisA[3];
    float AxisB[3];
    float ExtA;
    if (Size.x > Size.z)
    {
        AxisA[0] = Col0[0];
        AxisA[1] = Col0[1];
        AxisA[2] = Col0[2];
        ExtA = Size.x;
        AxisB[0] = Col2[0];
        AxisB[1] = Col2[1];
        AxisB[2] = Col2[2];
    }
    else
    {
        AxisA[0] = Col2[0];
        AxisA[1] = Col2[1];
        AxisA[2] = Col2[2];
        ExtA = Size.z;
        AxisB[0] = Col0[0];
        AxisB[1] = Col0[1];
        AxisB[2] = Col0[2];
    }

    // Look = normalize(camera - zone position) (the routine, then the
    // inline the routine sequence on the float (y^2 + x^2) + z^2).
    float Look[3];
    Look[0] = CamX - ZonePos.x;
    Look[1] = CamY - ZonePos.y;
    Look[2] = CamZ - ZonePos.z;
    float const LookLenSq = static_cast<float>(
        (static_cast<double>(Look[1]) * Look[1] + static_cast<double>(Look[0]) * Look[0]) +
        static_cast<double>(Look[2]) * Look[2]);
    double const LookInv = NebulaInvSqrt(LookLenSq);
    Look[0] = static_cast<float>(Look[0] * LookInv);
    Look[1] = static_cast<float>(Look[1] * LookInv);
    Look[2] = static_cast<float>(Look[2] * LookInv);

    // FaceA = ((1.0 - |(ay*ly + ax*lx) + az*lz|) - 0.0001f) * 204.08163f,
    // stored as float. FCOMP double 1.0 on the unrounded value, TEST AH,0x41 / JNZ: strictly
    // above -> 1.0f; otherwise the stored float FCOMP 0.0, TEST AH,5 / JP: an ordered "less"
    // -> 0.0f.
    double const DotA = fabs((static_cast<double>(AxisA[1]) * Look[1] +
                              static_cast<double>(AxisA[0]) * Look[0]) +
                             static_cast<double>(AxisA[2]) * Look[2]);
    double const FaceARaw = ((1.0 - DotA) - static_cast<double>(0.0001f)) * 204.08163f;
    float FaceA = static_cast<float>(FaceARaw);
    if (FaceARaw > 1.0)
        FaceA = 1.0f;
    else if (FaceA < 0.0)
        FaceA = 0.0f;

    // EdgeA = Look x AxisA (each component a float store).
    float EdgeA[3];
    EdgeA[0] = static_cast<float>(static_cast<double>(AxisA[2]) * Look[1] -
                                  static_cast<double>(Look[2]) * AxisA[1]);
    EdgeA[1] = static_cast<float>(static_cast<double>(Look[2]) * AxisA[0] -
                                  static_cast<double>(AxisA[2]) * Look[0]);
    EdgeA[2] = static_cast<float>(static_cast<double>(AxisA[1]) * Look[0] -
                                  static_cast<double>(Look[1]) * AxisA[0]);
    // normalised and scaled to the zone's reach.
    NebulaSheetEdge(Zone, EdgeA);

    // AxisA *= ExtA * 1.15f (the factor on the FPU).
    double const AxisScale = static_cast<double>(ExtA) * 1.15f;
    AxisA[0] = static_cast<float>(AxisA[0] * AxisScale);
    AxisA[1] = static_cast<float>(AxisA[1] * AxisScale);
    AxisA[2] = static_cast<float>(AxisA[2] * AxisScale);

    // FaceB from column 1 with 0.2f / 5.0f, the same two clamps.
    double const DotY = fabs((static_cast<double>(Col1[1]) * Look[1] +
                              static_cast<double>(Col1[0]) * Look[0]) +
                             static_cast<double>(Col1[2]) * Look[2]);
    double const FaceBRaw = ((1.0 - DotY) - static_cast<double>(0.2f)) * 5.0f;
    float FaceB = static_cast<float>(FaceBRaw);
    if (FaceBRaw > 1.0)
        FaceB = 1.0f;
    else if (FaceB < 0.0)
        FaceB = 0.0f;
    // only a FaceB strictly above 0 (TEST AH,0x41 / JNZ) is refined by
    // axis B: FaceB = float(((1.0 - |(by*ly + bx*lx) + bz*lz|) - 0.2f) * FaceB * 5.0f), the
    // unrounded value below 0 (TEST AH,5 / JP) -> 0.0f.
    if (FaceB > 0.0)
    {
        double const DotB = fabs((static_cast<double>(AxisB[1]) * Look[1] +
                                  static_cast<double>(AxisB[0]) * Look[0]) +
                                 static_cast<double>(AxisB[2]) * Look[2]);
        double const Refined = ((1.0 - DotB) - static_cast<double>(0.2f)) * FaceB * 5.0f;
        FaceB = static_cast<float>(Refined);
        if (Refined < 0.0)
            FaceB = 0.0f;
    }

    // EdgeB = Look x column 1.
    float EdgeB[3];
    EdgeB[0] = static_cast<float>(static_cast<double>(Col1[2]) * Look[1] -
                                  static_cast<double>(Col1[1]) * Look[2]);
    EdgeB[1] = static_cast<float>(static_cast<double>(Look[2]) * Col1[0] -
                                  static_cast<double>(Col1[2]) * Look[0]);
    EdgeB[2] = static_cast<float>(static_cast<double>(Col1[1]) * Look[0] -
                                  static_cast<double>(Look[1]) * Col1[0]);
    // normalised (the routine proper) and scaled to the zone's reach.
    NebulaSheetEdge(Zone, EdgeB);
    // column 1 *= float(Size.y * 1.15f) (the routine).
    NebulaScaleVec3(Col1, Size.y * 1.15f);

    // lighting off, culling off, depth write off, depth test off,
    // blending on, the fill shape's texture (the routine, frame 0), blend (5, 6), quads.
    Win3DDisable(kWin3DLightingCap);
    Win3DDisable(kWin3DCullFace);
    Win3DSetDepthWrite(0);
    Win3DDisable(kWin3DDepthTest);
    Win3DEnable(kWin3DBlend);
    Win3DBindTexture(TexShapeGetGpuHandle(Shape, 0));
    Win3DSetBlendFunc(5, 6);
    Win3DBegin(kWin3DQuads);

    float const* const Fog = reinterpret_cast<float*>(B + 0x2a0);
    float V[3];
    float T[3];

    // sheet A, only when FaceA is strictly above 0 (TEST AH,0x41 / JNZ).
    // Corners (zone +/- EdgeA) +/- AxisA through the routine / the routine (float stores).
    if (FaceA > 0.0)
    {
        Win3DColor4f(Fog[0], Fog[1], Fog[2], FaceA);
        Win3DTexCoord2f(Shape->U0, Shape->V0);
        T[0] = ZonePos.x + EdgeA[0];
        T[1] = ZonePos.y + EdgeA[1];
        T[2] = ZonePos.z + EdgeA[2];
        Win3DVertex3f(T[0] - AxisA[0], T[1] - AxisA[1], T[2] - AxisA[2]);
        Win3DTexCoord2f(Shape->U1, Shape->V0);
        Win3DVertex3f(T[0] + AxisA[0], T[1] + AxisA[1], T[2] + AxisA[2]);
        Win3DTexCoord2f(Shape->U1, Shape->V1);
        T[0] = ZonePos.x - EdgeA[0];
        T[1] = ZonePos.y - EdgeA[1];
        T[2] = ZonePos.z - EdgeA[2];
        Win3DVertex3f(T[0] + AxisA[0], T[1] + AxisA[1], T[2] + AxisA[2]);
        Win3DTexCoord2f(Shape->U0, Shape->V1);
        Win3DVertex3f(T[0] - AxisA[0], T[1] - AxisA[1], T[2] - AxisA[2]);
    }

    // sheet B, only when FaceB is strictly above 0. Corners
    // (zone +/- EdgeB) +/- scaled column 1. The last corner keeps x and y on the FPU across both
    // subtractions; its z is rounded in between.
    if (FaceB > 0.0)
    {
        Win3DColor4f(Fog[0], Fog[1], Fog[2], FaceB);
        Win3DTexCoord2f(Shape->U0, Shape->V0);
        T[0] = ZonePos.x + EdgeB[0];
        T[1] = EdgeB[1] + ZonePos.y;
        T[2] = EdgeB[2] + ZonePos.z;
        Win3DVertex3f(T[0] - Col1[0], T[1] - Col1[1], T[2] - Col1[2]);
        Win3DTexCoord2f(Shape->U1, Shape->V0);
        Win3DVertex3f(T[0] + Col1[0], T[1] + Col1[1], T[2] + Col1[2]);
        Win3DTexCoord2f(Shape->U1, Shape->V1);
        T[0] = ZonePos.x - EdgeB[0];
        T[1] = ZonePos.y - EdgeB[1];
        T[2] = ZonePos.z - EdgeB[2];
        V[0] = T[0] + Col1[0];
        V[1] = T[1] + Col1[1];
        V[2] = T[2] + Col1[2];
        Win3DVertex3f(V[0], V[1], V[2]);
        Win3DTexCoord2f(Shape->U0, Shape->V1);
        V[0] = static_cast<float>(
            (static_cast<double>(ZonePos.x) - EdgeB[0]) - static_cast<double>(Col1[0]));
        V[1] = static_cast<float>(
            (static_cast<double>(ZonePos.y) - EdgeB[1]) - static_cast<double>(Col1[1]));
        V[2] = (ZonePos.z - EdgeB[2]) - Col1[2];
        Win3DVertex3f(V[0], V[1], V[2]);
    }

    // End, depth test on, depth write on, blending off, culling on
    // (lighting is left off).
    Win3DEnd();
    Win3DEnable(kWin3DDepthTest);
    Win3DSetDepthWrite(1);
    Win3DDisable(kWin3DBlend);
    Win3DEnable(kWin3DCullFace);
}

void NebulaDrawBolts(FNebulaEffect* Nebula, void* Camera)
{
    // (__thiscall, RET 4), traced end to end against module storage..00508aef.
    unsigned char* const B = reinterpret_cast<unsigned char*>(Nebula);

    // nothing without segments (+0xdc, signed JLE), nothing once +0x294
    // is not below 1.0 (`TEST AH,0x5; JP` also exits when unordered).
    if (*reinterpret_cast<int*>(B + 0xdc) <= 0)
        return;
    if (!(*reinterpret_cast<float*>(B + 0x294) < 1.0))
        return;

    // the camera's slot +0x10 (orientation followed by the position:
    // twelve dwords copied with REP MOVSD) and the zone's slot +0x10 position.
    ICamera* const Cam = static_cast<ICamera*>(Camera);
    FTransform View;
    memcpy(&View, Cam->GetOrientation(), sizeof(View));
    Vector const CamPos = { View.Translation.X, View.Translation.Y, View.Translation.Z };
    Vector const ZonePos = Nebula->Zone->GetPos();

    // zone centre into view space (the field slot +0x3c), zone radius
    // (slot +0x30), camera ClassifySphere (slot +0x3c); `CMP AX,3; JC` -- below 3 draws nothing.
    {
        FVector ZoneWorld;
        ZoneWorld.X = ZonePos.x;
        ZoneWorld.Y = ZonePos.y;
        ZoneWorld.Z = ZonePos.z;
        FVector ZoneView = {};
        AcquireNebulaMath()->InverseTransform(ZoneView, View, ZoneWorld);
        float const Center[3] = { ZoneView.X, ZoneView.Y, ZoneView.Z };
        float const ZoneRadius = Nebula->Zone->GetRadius();
        if (static_cast<unsigned short>(Cam->ClassifySphere(Center, ZoneRadius)) < 3)
            return;
    }

    // back-to-front bubble sort while +0x54 is above 1.15f, by squared
    // distance to the camera (world point minus camera position). A pair swaps only when the
    // later one is farther by more than 10. The previous distance is cached (byte [ESP+0xf])
    // only right after a swap -- the swapped-down record keeps its value; a pair left alone
    // inside the 10 band keeps the flag as it was (baseline quirk, kept). +0x54 is zeroed after
    // the sort, including when there is only one segment.
    if (*reinterpret_cast<float*>(B + 0x54) > 1.15f)
    {
        int Swapped = 0;
        do
        {
            Swapped = 0;
            if (*reinterpret_cast<int*>(B + 0xdc) <= 1)
                break;
            bool bPrevCached = false;
            double PrevDistSq = 0.0;
            for (int I = 1; I < *reinterpret_cast<int*>(B + 0xdc); ++I)
            {
                float* const Segs = *reinterpret_cast<float**>(B + 0xe0);
                float* const Prev = Segs + (I - 1) * 8;
                float* const Cur = Segs + I * 8;
                if (!bPrevCached)
                {
                    float const Dx = (ZonePos.x + Prev[0]) - CamPos.x;
                    float const Dy = (ZonePos.y + Prev[1]) - CamPos.y;
                    float const Dz = (ZonePos.z + Prev[2]) - CamPos.z;
                    PrevDistSq = static_cast<double>(Dx) * Dx + static_cast<double>(Dy) * Dy +
                                 static_cast<double>(Dz) * Dz;
                }
                float const Cx = (ZonePos.x + Cur[0]) - CamPos.x;
                float const Cy = (ZonePos.y + Cur[1]) - CamPos.y;
                float const Cz = (ZonePos.z + Cur[2]) - CamPos.z;
                double const CurDistSq = static_cast<double>(Cx) * Cx +
                                         static_cast<double>(Cy) * Cy +
                                         static_cast<double>(Cz) * Cz;
                if (!(PrevDistSq < CurDistSq))
                {
                    bPrevCached = false;
                    continue;
                }
                if (!(fabs(PrevDistSq - CurDistSq) > 10.0))
                    continue;
                ++Swapped;
                float Tmp[8];
                memcpy(Tmp, Prev, sizeof(Tmp));
                memcpy(Prev, Cur, sizeof(Tmp));
                memcpy(Cur, Tmp, sizeof(Tmp));
                bPrevCached = true;
            }
        } while (Swapped > 0);
        *reinterpret_cast<float*>(B + 0x54) = 0.0f;
    }

    // lighting and culling off, no depth write, depth test OFF
    // (the routine 0xb71), blending on, then the routine with the camera.
    Win3DDisable(kWin3DLightingCap);
    Win3DDisable(kWin3DCullFace);
    Win3DSetDepthWrite(0);
    Win3DDisable(kWin3DDepthTest);
    Win3DEnable(kWin3DBlend);
    Win3DApplyCamera(Cam);
    // the routine(+0x60 shape, frame 0) bound through the routine.
    FTexShape* const BoundShape = *reinterpret_cast<FTexShape**>(B + 0x60);
    Win3DBindTexture(TexShapeGetGpuHandle(BoundShape, 0));
    // the shape's alpha cache (+0x04), probed while it is negative:
    // (0) seeds the format id, TextureLib slot 23 (+0x5c, GetGroupPixelFormat) reads
    // the shape record's, then the routine's body inline -- ids 10..14 have alpha, any other id
    // takes its descriptor's +0x48 byte (the routine).
    if (BoundShape->Field04 < 0)
    {
        int Format = PixelFormatFromD3D(0);
        NeuronRuntime::TextureLib->GetGroupPixelFormat(BoundShape->Texture, nullptr, &Format);
        unsigned char bHasAlpha = 1;
        if (Format < 10 || Format > 14)
            bHasAlpha = static_cast<unsigned char>(PixelFormatGetDesc(Format)->HasAlpha & 0xffu);
        BoundShape->Field04 = bHasAlpha;
    }
    // the routine(5, 6) with alpha, (2, 2) without. The SETNZ result
    // is kept ([ESP+0xf]) and picks the colour call per bolt below.
    bool const bHasAlpha = BoundShape->Field04 != 0;
    if (bHasAlpha)
        Win3DSetBlendFunc(kWin3DBlendSrcAlpha, kWin3DBlendInvSrcAlpha);
    else
        Win3DSetBlendFunc(kWin3DBlendOne, kWin3DBlendOne);

    // Pipeline render state 0x18 saved into a zeroed local (slot +0xf4),
    // then set to 4 (slot +0xf0). No null test on the Pipeline.
    int SavedRs18 = 0;
    NeuronRuntime::Pipeline->GetRenderState(D3DRS_ALPHAREF, &SavedRs18);
    NeuronRuntime::Pipeline->SetRenderState(D3DRS_ALPHAREF, 4);

    // base alpha +0xb8, faded out over the second half of +0x294.
    float const Fade = *reinterpret_cast<float*>(B + 0x294);
    float BaseAlpha = *reinterpret_cast<float*>(B + 0xb8);
    if (Fade > 0.5f)
        BaseAlpha = static_cast<float>((1.0 - ((static_cast<double>(Fade) - 0.5) +
                                               (static_cast<double>(Fade) - 0.5))) * BaseAlpha);

    // edge fade. `TEST AH,0x41; JP` against 0.0f takes the zero branch
    // when +0x294 is above zero (or unordered): only a non-positive +0x294 gets the span ramp.
    // Span = +0x2ac - +0x29c; below 7000 -> 0, from 10000 up -> 1, linear in between.
    float EdgeFade = 1.0f;
    if (!(Fade <= 0.0f))
        EdgeFade = 0.0f;
    else
    {
        float const Span = *reinterpret_cast<float*>(B + 0x2ac) - Nebula->FogNear;
        if (Span < 7000.0f)
            EdgeFade = 0.0f;
        else if (Span < 10000.0f)
            EdgeFade = static_cast<float>(1.0 - (10000.0f - Span) * 0.00033333333f);
    }

    // the routine(8).
    Win3DBegin(kWin3DQuads);

    // the unit vector from the zone centre to the camera.
    float ToCam[3] = { CamPos.x - ZonePos.x, CamPos.y - ZonePos.y, CamPos.z - ZonePos.z };
    {
        float const InvLen =
            1.0f / sqrtf(ToCam[0] * ToCam[0] + ToCam[2] * ToCam[2] + ToCam[1] * ToCam[1]);
        ToCam[0] *= InvLen;
        ToCam[1] *= InvLen;
        ToCam[2] *= InvLen;
    }

    // the segment array is read once; the count (+0xdc) is re-read per iteration.
    // Segment record (0x20): +0x00 position relative to the zone, +0x0c radius, +0x10 FTexShape*,
    // +0x14 bolt axis.
    float* Seg = *reinterpret_cast<float**>(B + 0xe0);
    for (int I = 0; I < *reinterpret_cast<int*>(B + 0xdc); ++I, Seg += 8)
    {
        float A = BaseAlpha;

        // world point P and the camera-to-point vector V.
        float const P[3] = { ZonePos.x + Seg[0], ZonePos.y + Seg[1], ZonePos.z + Seg[2] };
        float const V[3] = { P[0] - CamPos.x, P[1] - CamPos.y, P[2] - CamPos.z };

        // view depth through InverseTransform (slot +0x3c); a point on
        // or behind the eye plane is skipped (`TEST AH,0x41; JNP`: less or equal).
        FVector PWorld;
        PWorld.X = P[0];
        PWorld.Y = P[1];
        PWorld.Z = P[2];
        FVector PView = {};
        AcquireNebulaMath()->InverseTransform(PView, View, PWorld);
        float const Depth = -PView.Z;
        if (Depth <= 0.0f)
            continue;

        // facing fade, entered when +0xd8 is set or EdgeFade < 1.
        // Dot = ToCam . normalize(segment position) -- the segment's offset from the zone
        // centre, not its axis.
        int const Mode = *reinterpret_cast<int*>(B + 0xd8);
        if (Mode != 0 || EdgeFade < 1.0f)
        {
            float const Sx = Seg[0];
            float const Sy = Seg[1];
            float const Sz = Seg[2];
            float const Inv = 1.0f / sqrtf(Sx * Sx + Sz * Sz + Sy * Sy);
            float const Dot = ToCam[0] * (Sx * Inv) + ToCam[2] * (Sz * Inv) + ToCam[1] * (Sy * Inv);
            if (Mode == 0)
            {
                if (Dot < 0.1f)
                    A = EdgeFade * BaseAlpha;
            }
            else
            {
                if (Dot < 0.0f)
                    continue;
                if (Dot < 0.05)
                    A = static_cast<float>(Dot * BaseAlpha * 20.0);
            }
        }

        // Q = V x axis and H = Q x V, each scaled to the radius +0x0c.
        float const R = Seg[3];
        float const* const Axis = Seg + 5;
        float Qx = V[1] * Axis[2] - V[2] * Axis[1];
        float Qy = V[2] * Axis[0] - V[0] * Axis[2];
        float Qz = V[0] * Axis[1] - V[1] * Axis[0];
        float const QScale = R / sqrtf(Qx * Qx + Qz * Qz + Qy * Qy);
        Qx *= QScale;
        Qy *= QScale;
        Qz *= QScale;
        float Hx = Qy * V[2] - Qz * V[1];
        float Hy = Qz * V[0] - V[2] * Qx;
        float Hz = V[1] * Qx - Qy * V[0];
        float const HScale = R / sqrtf(Hz * Hz + Hy * Hy + Hx * Hx);
        Hx *= HScale;
        Hy *= HScale;
        Hz *= HScale;

        // nearer than the radius -> alpha scaled by Depth / R; below
        // 0.01 the bolt is skipped.
        if (Depth < R)
            A = (Depth / R) * A;
        if (A < 0.01)
            continue;

        // colour +0xa8..+0xb0. With an alpha texture the routine
        // (Color4f); without, the additive path premultiplies through the routine (Color3f).
        float const Cr = *reinterpret_cast<float*>(B + 0xa8);
        float const Cg = *reinterpret_cast<float*>(B + 0xac);
        float const Cb = *reinterpret_cast<float*>(B + 0xb0);
        if (bHasAlpha)
            Win3DColor4f(Cr, Cg, Cb, A);
        else
            Win3DColor3f(Cr * A, Cg * A, Cb * A);

        // four corners, P+Q+H, P+Q-H, P-Q-H, P-Q+H.
        FTexShape const* const Shape = *reinterpret_cast<FTexShape* const*>(Seg + 4);
        Win3DTexCoord2f(Shape->U0, Shape->V0);
        Win3DVertex3f((P[0] + Qx) + Hx, (P[1] + Qy) + Hy, (P[2] + Qz) + Hz);
        Win3DTexCoord2f(Shape->U1, Shape->V0);
        Win3DVertex3f((P[0] + Qx) - Hx, (P[1] + Qy) - Hy, (P[2] + Qz) - Hz);
        Win3DTexCoord2f(Shape->U1, Shape->V1);
        Win3DVertex3f((P[0] - Qx) - Hx, (P[1] - Qy) - Hy, (P[2] - Qz) - Hz);
        Win3DTexCoord2f(Shape->U0, Shape->V1);
        Win3DVertex3f((P[0] - Qx) + Hx, (P[1] - Qy) + Hy, (P[2] - Qz) + Hz);
    }

    // End, render state 0x18 restored, then depth test ON, depth write
    // on, blending off, culling on.
    Win3DEnd();
    NeuronRuntime::Pipeline->SetRenderState(D3DRS_ALPHAREF, SavedRs18);
    Win3DEnable(kWin3DDepthTest);
    Win3DSetDepthWrite(1);
    Win3DDisable(kWin3DBlend);
    Win3DEnable(kWin3DCullFace);

    // the effect at +0xec (released by NebulaShutdown's the routine at
    // ), its vtable slot +0x0c with 1 - +0x294.
    FEffectBase* const Effect = *reinterpret_cast<FEffectBase**>(B + 0xec);
    if (Effect != nullptr)
    {
        using FEffectSlot0C = void(__thiscall*)(FEffectBase*, float);
        Effect->SetIntensity(static_cast<float>(1.0 - *reinterpret_cast<float*>(B + 0x294)));
    }
}


void FNebulaEffect::Shutdown()
{
    FNebulaEffect* const Nebula = this;
    // (thiscall), 0x00506cc0..0x00506eaa. Nothing unless loaded (+0x2c0).
    if (Nebula->FlagLoaded == 0)
        return;

    unsigned char* const B = reinterpret_cast<unsigned char*>(Nebula);
    *reinterpret_cast<unsigned*>(B + 0xa0) = 0;

    void* Bolt = *reinterpret_cast<void**>(B + 0xe0);
    if (Bolt != nullptr)
        operator delete(Bolt);
    *reinterpret_cast<void**>(B + 0xe0) = nullptr;
    *reinterpret_cast<unsigned*>(B + 0xdc) = 0;

    // every Clouds of the list is destroyed and freed (the routine(1),
    // skipped for a null entry), then the list is cleared (the routine: each node unlinked,
    // freed, count decremented).
    FNebulaCloudsListNode* const Sentinel = Nebula->Clouds.Sentinel;
    FNebulaCloudsListNode* N = Sentinel->Next;
    while (N != Sentinel)
    {
        if (N->Clouds != nullptr)
            LocalNebulaCloudsDestroy(N->Clouds, 1);
        N = N->Next;
    }
    N = Sentinel->Next;
    while (N != Sentinel)
    {
        FNebulaCloudsListNode* const Next = N->Next;
        N->Prev->Next = N->Next;
        N->Next->Prev = N->Prev;
        operator delete(N);
        --Nebula->Clouds.Count;
        N = Next;
    }

    if (Nebula->ExclusionMgr != nullptr)
    {
        delete Nebula->ExclusionMgr;
        Nebula->ExclusionMgr = nullptr;
    }

    // every zone_shell of the +0x38 map is destroyed (the routine)
    // and freed, then the whole map is erased (the routine over [begin, end)).
    {
        FNebulaZoneShellMap& Shells = NebulaZoneShells(Nebula);
        for (FNebulaZoneShellMap::iterator It = Shells.begin(); It != Shells.end(); ++It)
        {
            FNebulaZoneShell* const Shell = It->second;
            if (Shell != nullptr)
            {
                NebulaZoneShellDestroy(Shell);
                delete Shell;
            }
        }
        Shells.clear();
    }

    // the routine on the lightning member at +0x100.
    LightningDisable(reinterpret_cast<float*>(B + 0x100));

    // the effect / sound / light companion at +0xe4 is stopped (the
    // inlined the routine prelude: effect vt+4, sound pose (-1, identity) + vt+0x60, light
    // + vt+8) and released (the inlined the routine). Nothing in Nebula.cpp ever
    // fills it (only the ctor, this function and the dtor touch +0xe4..+0xf0).
    CliEffectCompanionArmStop(B + 0xe4);
    CliEffectCompanionClear(B + 0xe4);

    // the ambient light is switched off when bit 3 is set and
    // released. +0x270 is NOT cleared -- only a later Load with a positive period rewrites it.
    FGameLight* const Ambient = *reinterpret_cast<FGameLight**>(B + 0x270);
    if (Ambient != nullptr)
    {
        if ((Ambient->Flags & 0x08) != 0)
            GameLightVirtualDisable(Ambient);
        GameLightRelease(*reinterpret_cast<FGameLight**>(B + 0x270));
    }

    // the routine on the ShapeDB (+0x04), zone and loaded flag
    // cleared.
    NebulaShapes(Nebula)->Clear();
    Nebula->Zone = nullptr;
    Nebula->FlagLoaded = 0;
}

void NebulaShutdown(FNebulaEffect* Nebula)
{
    Nebula->FNebulaEffect::Shutdown();
}

FNebulaEffect::~FNebulaEffect()
{
    FNebulaEffect* const Nebula = this;
    // (thiscall), 0x00504cf0..0x00504e5d: the member destructors in reverse order
    // after Shutdown.
    NebulaShutdown(Nebula);

    unsigned char* const B = reinterpret_cast<unsigned char*>(Nebula);

    // ~lightning (+0x100) is the routine.
    LightningDisable(reinterpret_cast<float*>(B + 0x100));

    // Clouds, companion, zone-shell map and ShapeDB unwind in reverse member order.
}

void NebulaDestruct(FNebulaEffect* Nebula)
{
    Nebula->FNebulaEffect::~FNebulaEffect();
}
