// Author: Giperion.

// StarSphere / SunEffect — the routine.
// Listing: the routine / 00510ef0 / 00510f10 / the branch / the routine / 0050d420 /
// / the routine / 00422690 / the routine.

#include "star_sphere.h"

#include "NeuronRuntime.h"
#include "cli_asteroid.h"
#include "cli_nebula.h"
#include "client_objdb.h"
#include "common_cobject.h"
#include "common_csys.h"
#include "common_iface.h" // IObjRW::is_using_tradelane (the routine)
#include "common_ini.h"
#include "common_misc.h" // ICamera
#include "common_physys.h"
#include "common_universe.h"
#include "neuroncomponents.h"
#include "engine_light.h"
#include "game_options.h"
#include "globals.h"
#ifdef OUTPOST_FRONTIER_DX9RENDERER
#include "nebula_volume.h" // NebulaVolume::NoteStarGlow (port-only Direct3D 9 nebula)
#endif
#include "hud_widescreen.h"   // port-only (OUTPOST_FRONTIER_WIDESCREEN_SUPPORT)
#include "pixel_format.h" // PixelFormatFromD3D / PixelFormatGetDesc (the routine / 005b7030)
#include "player_behavior.h"
#include "render/IRenderPipeline.h"
#include "render/RenderPipeline.h"
#include "render/shading.h"
#include "space_camera.h"
#include "space_scene_obj_draw.h"
#include "star_sys.h"
#include "texture_shapes.h"
#include "win3d_immediate.h"
#include "win3d_render.h"
#include "x86math.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>

constexpr int kPipelineStateViewspace = 7;
constexpr float kPi = 3.14159274f;
constexpr float kTinyFade = 1.0e-7f; // — Load clamp for zero fades
constexpr float kHalf = 0.5f;
constexpr float kSpineBlend = 0.8f;
constexpr int kCoronaRingCount = 13;
constexpr int kCoronaOuterCount = 12;
constexpr int kCoronaOuterFanEnable = 1;
constexpr int kCoronaOuterFanCount = 12;

// — written by the branch (SunEffect perf apply).
float G_SunEffectQuality = 0.0f; // .data past raw size -> zero-initialised in the PE

// / 00613471 / 00613472 / 00613473 — enable gates (PE defaults 1).
static unsigned char G_StarBillboardEnabled = 1;
static unsigned char G_StarSpinesEnabled = 1;
static unsigned char G_StarCoronaDrawEnabled = 1;
static unsigned char G_StarLensFlareEnabled = 1;

// — corona "draw without blending" byte: the routine turns GL_BLEND (0xbe2) off
// before the corona and back on after it when set. BSS, and nothing in the image writes it
// (its only references are the two reads 0x0050e822 / 0x0050f206), so it stays 0.
static unsigned char G_SunCoronaNoBlend = 0;

// — LOD≥3 outer-ring walk start index (PE default 0).
static int G_CoronaLod3Start = 0;

// — the routine call counter; zeroed by the Draw at 00511d7a, read nowhere.
static unsigned G_SunRayQueryCount = 0;

// — the routine's static 6-entry RayHit buffer (once-init guard the field,
// trivial atexit destructor the field = RET).
static PhySys::RayHit G_SunRayHits[6];

// — lazy 3DMathEngine (the routine) shared by the routine and the Draw.
static I3DMathEngine* G_SunRayMath = nullptr;

// nebula arm (00511bcd..00511c4c): walk the StarSys nebula list; each nebula's
// slot 19 (+0x4c, GetZone) gives its zone, and the zone's +0x4c Intersect(cam, sun, &hit)
// decides. Stops at the first hit.
static bool StarSysNebulaSegmentHits(float const* From3, float const* To3)
{
    Vector Start{};
    Start.x = From3[0];
    Start.y = From3[1];
    Start.z = From3[2];
    Vector End{};
    End.x = To3[0];
    End.y = To3[1];
    End.z = To3[2];
    Vector Hit{};

    std::list<FNebulaEffect*>& Nebulae = StarSysNebulaList();
    for (std::list<FNebulaEffect*>::iterator It = Nebulae.begin(); It != Nebulae.end(); ++It)
    {
        FNebulaEffect* const Nebula = *It;
        Universe::IZone* const Zone = Nebula->GetZone();
        if (Zone->Intersect(Start, End, Hit))
            return true;
    }
    return false;
}

// The star_glow / star_center record's inline constructor, as the StarSphere ctor
// (0x00510e17..0x00510e92) and Load's star_center arm (0x005111ae..0x00511208) both write it:
// +0x00 = 0 and name byte +0x04 = 0, then the empty string (the field) assigned (length 0,
// terminator at +0x04; the routine / the routine), +0x28..+0x30 = 1, +0x34..+0x3c = 1, +0x24 =
// 1, +0x40 = 1, +0x44 = 0, byte +0x4c = 0, +0x48 = 0. The name bytes after the terminator and
// +0x4d..+0x4f are not written.
static void InitGlowDescDefaults(FStarGlowDesc* Desc, unsigned char OwnerTag)
    {
        (void)OwnerTag;
        Desc->ShapeLen = 0;
        Desc->ShapeName[0] = 0;
        Desc->InnerColor[0] = 1.0f;
        Desc->InnerColor[1] = 1.0f;
        Desc->InnerColor[2] = 1.0f;
        Desc->OuterColor[0] = 1.0f;
        Desc->OuterColor[1] = 1.0f;
        Desc->OuterColor[2] = 1.0f;
        Desc->Scale = 1.0f;
        Desc->Alpha = 1.0f;
        Desc->Field44 = 0;
        Desc->Field4C = 0;
        Desc->Field48 = 0;
    }

// The lens-flare / spines 0x40 shell, inlined twice in the StarSphere ctor (0x00510d61..
// , 0x00510da2..0x00510ddc): operator new(0x40), and on success the allocator byte
// +0x00 (a stack byte in baseline), +0x04..+0x0c (the element vector) = 0, +0x10 = 0 and name
// byte +0x14 = 0 (both written twice), the cached shape +0x34 = 0, then the two clamp bounds
// +0x38 / +0x3c. The rest of the inline name (+0x15..+0x33) is not written.
static FStarFxChild40* AllocChild40(float Field38, float Field3C)
    {
        FStarFxChild40* Child = static_cast<FStarFxChild40*>(operator new(0x40));
        if (Child == nullptr)
            return nullptr;
        unsigned char* const Raw = Child->Raw;
        Raw[0] = 0;
        *reinterpret_cast<unsigned*>(Raw + 0x04) = 0;
        *reinterpret_cast<unsigned*>(Raw + 0x08) = 0;
        *reinterpret_cast<unsigned*>(Raw + 0x0c) = 0;
        *reinterpret_cast<unsigned*>(Raw + 0x10) = 0;
        Raw[0x14] = 0;
        *reinterpret_cast<unsigned*>(Raw + 0x34) = 0;
        *reinterpret_cast<float*>(Raw + 0x38) = Field38;
        *reinterpret_cast<float*>(Raw + 0x3c) = Field3C;
        return Child;
    }

    // ------------------------------------------------------------------ helpers

    void* ResolveShapeTextureHandle(FTexShape const* Shape)
    {
        // — Slot22(Shape->Texture, frame, out); return out[0] GPU handle.
        return TexShapeGetGpuHandle(Shape, 0);
    }

    // The alpha flag both probe sites write (the routine 0x0050e793..0x0050e7f7 into +0x2e4,
    // 0x0051175e..0x005117bd into the glow desc +0x4c): cleared, the id seeded with
    // (0); with a shape, Pipeline slot 50 (+0xc8, GetTextureFormat) on its
    // handle, and only a 0 result sets the flag -- ids 10..14 have alpha
    // (CMP 0xa JL / CMP 0xe JG), any other id takes the routine(id)'s +0x48 byte.
    unsigned char ProbeShapeAlpha(FTexShape const* Shape)
    {
        unsigned char bHasAlpha = 0;
        int Format = PixelFormatFromD3D(0);
        if (Shape != nullptr)
        {
            IRenderPipeline* const Pipe = NeuronRuntime::Pipeline;
            if (Pipe->GetTextureFormat(ResolveShapeTextureHandle(Shape), &Format) == 0)
            {
                if (Format >= 10 && Format <= 14)
                    bHasAlpha = 1;
                else
                    bHasAlpha = static_cast<unsigned char>(PixelFormatGetDesc(Format)->HasAlpha & 0xffu);
            }
        }
        return bHasAlpha;
    }

    FTexShape const* ResolveNamedShape(char const* Name, FTexShape const** CacheSlot,
                                       unsigned char* AlphaFlagSlot)
    {
        // ShapeDB path used by UpdateIntensity / 50e6f0 / 50f850 / 510160: every site tests only
        // the cache slot, then keys the find on the owner's inline name buffer as it is -- no
        // null or empty-name test (0x0050e6f8, 0x0050f857, 0x00510175, 0x005116f7, 0x005117cb),
        // so an empty name still reaches the ShapeDB.h:0xf2 miss warning.
        // G_StarSphereShapeDB.Find(Name) → record; probe format alpha into *AlphaFlagSlot.
        if (*CacheSlot != nullptr)
            return *CacheSlot;
        FTexShape const* const Shape = G_StarSphereShapeDB.Find(Name);
        *CacheSlot = Shape;
        if (AlphaFlagSlot != nullptr)
            *AlphaFlagSlot = ProbeShapeAlpha(Shape);
        return Shape;
    }

    void ResolveGlowDescShape(FStarGlowDesc* Desc)
    {
        // mid-body, once per descriptor: when +0x48 is still 0, look +0x04 up,
        // store the shape at +0x48 and the alpha probe at +0x4c. The caller decides which
        // descriptors get here (StarGlow untested, StarCenter behind a null test).
        ResolveNamedShape(Desc->ShapeName, reinterpret_cast<FTexShape const**>(&Desc->Field48),
                          &Desc->Field4C);
    }

    float ApproachFloat(float Current, float Target, float StepUp, float StepDown)
    {
        // Shared approach used by UpdateIntensity / Draw ZoneOcclusion / the routine.
        if (Target > Current)
        {
            Current += StepUp;
            if (Current > Target)
                Current = Target;
        }
        else if (Target < Current)
        {
            Current -= StepDown;
            if (Current < Target)
                Current = Target;
        }
        return Current;
    }

    // — per-corona-slot alpha approach toward (1-param1)*param4.
    void CoronaSlotApproachAlpha(unsigned char* SlotBase, float Param1, float StepUp, float StepDown,
                                 float Param4)
    {
        SlotBase[0x14] = 1;
        float* const Alpha = reinterpret_cast<float*>(SlotBase + 0x10);
        float Target = (1.0f - Param1) * Param4;
        *Alpha = ApproachFloat(*Alpha, Target, StepUp, StepDown);
        if (*Alpha < 0.0f)
            *Alpha = 0.0f;
        if (*Alpha > 1.0f)
            *Alpha = 1.0f;
    }

    // The Draw's inlined corona-slot fade toward 0 (the branch at 0x00512112..0x00512238 and
    // ): 13 slots from Body+0, then 12 from Body+0x138, stride 0x18. Per
    // slot the flag byte +0x14 is set, the alpha +0x10 steps toward 0 (StepUp while below,
    // StepDown while above) and snaps to 0 on overshoot; otherwise it is clamped to [0, 1].
    // The steps stay on the FPU (double) except where the caller spilled one to a float.
    void CoronaSlotFadeToZero(unsigned char* Slot, double StepUp, double StepDown)
    {
        Slot[0x14] = 1;
        float* const Alpha = reinterpret_cast<float*>(Slot + 0x10);
        // FLD 0 / FCOMP alpha, TEST AH,0x41 / JNZ -- the fall-through
        // is 0 > alpha (ordered).
        if (0.0f > *Alpha)
        {
            // alpha += StepUp; FCOMP 0, TEST AH,0x41 / JNZ goes to the
            // clamp when the sum <= 0 or unordered, otherwise the slot becomes 0 and skips it.
            double const Next = StepUp + *Alpha;
            *Alpha = static_cast<float>(Next);
            if (Next > 0.0)
            {
                *Alpha = 0.0f;
                return;
            }
        }
        // FLD 0 / FCOMP alpha, TEST AH,5 / JP -- proceeds only on an
        // ordered 0 < alpha.
        else if (0.0f < *Alpha)
        {
            // alpha -= StepDown; TEST AH,5 / JP goes to the clamp unless
            // the difference < 0, which makes the slot 0 and skips it.
            double const Next = *Alpha - StepDown;
            *Alpha = static_cast<float>(Next);
            if (Next < 0.0)
            {
                *Alpha = 0.0f;
                return;
            }
        }
        // alpha < 0 -> 0 (TEST AH,5 / JP), else alpha > 1 -> 1
        // (TEST AH,0x41 / JNZ skips on <= 1 or unordered).
        if (*Alpha < 0.0f)
        {
            *Alpha = 0.0f;
            return;
        }
        if (*Alpha > 1.0f)
            *Alpha = 1.0f;
    }

    void FadeCoronaSlotsToZero(FSunEffectBody* Body, double StepUp, double StepDown)
    {
        unsigned char* Slot = Body->Raw;
        for (int I = 0; I < 13; ++I)
        {
            CoronaSlotFadeToZero(Slot, StepUp, StepDown);
            Slot += 0x18;
        }
        Slot = Body->Raw + 0x138;
        for (int I = 0; I < 12; ++I)
        {
            CoronaSlotFadeToZero(Slot, StepUp, StepDown);
            Slot += 0x18;
        }
    }

    // — weighted corona alpha → Draw stores 1−result at Field38.
    float SunBodyComputeCoronaFactor(FSunEffectBody* Body)
    {
        if (Body == nullptr)
            return 0.0f;
        unsigned char* const Base = Body->Raw;
        int const Lod = *reinterpret_cast<int*>(Base + 0x2b0);
        float const Center = *reinterpret_cast<float*>(Base + 0x10);
        if (Lod == 0)
            return Center;
        float Sum = 0.0f;
        float* Pf = reinterpret_cast<float*>(Base + 0x28);
        for (int I = 0; I < 12; ++I)
        {
            Sum += *Pf;
            Pf += 6;
        }
        if (Lod < 3)
            return Center * 0.2f + Sum * (1.0f / 12.0f) * 0.8f;
        Pf = reinterpret_cast<float*>(Base + 0x148);
        for (int I = 0; I < 12; ++I)
        {
            Sum += *Pf;
            Pf += 6;
        }
        return Center * 0.1f + Sum * (1.0f / 24.0f) * 0.9f;
    }

    // is StarSphereMaxGlowScale below (public; its caller is the Solar slot 94).

    float const* OwnerGetWorldPos(IObjInspect* Owner);

    // (thiscall on the sphere, RET 4; caller the Draw at 0x00511cfd) — zone
    // occlusion factor, 0 or 1. The player's tradelane state first, then the segment camera ->
    // sun against every nebula zone and every asteroid field zone; the first hit gives 1.
    // baseline tests nothing for null: the player object is the only thing checked (0x00511ba5).
    float StarZoneOcclusionFactor(FStarSphere* This, void* Cam)
    {
        // the routine; when set, IObjInspect slot 77 (+0x134)
        // is_using_tradelane(&flag) -- a true byte returns 1.0f (the field).
        IObjRW* const PlayerRw = static_cast<IObjRW*>(GetPlayerObjRW());
        if (PlayerRw != nullptr)
        {
            bool bUsingTradelane = false;
            PlayerRw->is_using_tradelane(&bUsingTradelane);
            if (bUsingTradelane)
                return 1.0f;
        }

        // camera vt+0x0c (__stdcall, one argument) position copied.
        float const* const CamPosPtr = ((ICamera*)Cam)->GetPosition();
        float CamPos[3] = {CamPosPtr[0], CamPosPtr[1], CamPosPtr[2]};

        // owner (+0x04) vt[0] (thiscall, no argument) position copied.
        float const* const SunPosPtr = (float const*)&This->Owner->get_position();
        float SunPos[3] = {SunPosPtr[0], SunPosPtr[1], SunPosPtr[2]};

        // the nebula arm; a hit skips the asteroid arm.
        if (StarSysNebulaSegmentHits(CamPos, SunPos))
            return 1.0f;

        // the routine's field list; each field's zone (+0x1c) slot 19
        // (+0x4c) Intersect(cam, sun, &hit), read unchecked. The walk stops at the first hit.
        struct FAsteroidHitCtx
        {
            float const* From;
            float const* To;
            bool bHit;
        };
        FAsteroidHitCtx Ctx = {CamPos, SunPos, false};
        CliAsteroidForEach(
            [](FCliAsteroid* Field, void* Raw) {
                FAsteroidHitCtx* HitCtx = static_cast<FAsteroidHitCtx*>(Raw);
                if (HitCtx->bHit)
                    return;
                Vector Start{};
                Start.x = HitCtx->From[0];
                Start.y = HitCtx->From[1];
                Start.z = HitCtx->From[2];
                Vector End{};
                End.x = HitCtx->To[0];
                End.y = HitCtx->To[1];
                End.z = HitCtx->To[2];
                Vector Hit{};
                if (Field->Zone->Intersect(Start, End, Hit))
                    HitCtx->bHit = true;
            },
            &Ctx);
        if (Ctx.bHit)
            return 1.0f;
        return 0.0f;
    }

    void AddVec3Local(float* Out, float const* A, float const* B)
    {
        Out[0] = A[0] + B[0];
        Out[1] = A[1] + B[1];
        Out[2] = A[2] + B[2];
    }

    void SubVec3Local(float* Out, float const* A, float const* B)
    {
        Out[0] = A[0] - B[0];
        Out[1] = A[1] - B[1];
        Out[2] = A[2] - B[2];
    }

    float const* OwnerGetWorldPos(IObjInspect* Owner)
    {
        // MOV ECX,[ESI+4]; MOV EAX,[ECX]; CALL [EAX] -- the owner's
        // slot 0 (thiscall, no argument) returns its position. baseline tests neither the owner
        // nor its vtable nor the result.
        return (float const*)&Owner->get_position();
    }

    // The CObject an owner record exposes through +0x0c -> +0x04 (baseline's inlined chain at
    // and 0050c7a5..0050c7ba: load +4 of the record, add 0xc, load +4).
    CObject* SunRayOwnerObject(void* Record)
    {
        if (Record == nullptr)
            return nullptr;
        unsigned char* const Inner = static_cast<unsigned char*>(Record) + 0xc;
        return *reinterpret_cast<CObject**>(Inner + 0x4);
    }

    // — is the ray From -> To blocked by something big enough on screen?
    // cdecl, four arguments (callers 0050dd84 / 0050e085 / 0050e2ea all ADD ESP,0x10).
    unsigned char SunRayIsOccluded(FStarSphere* Sphere, Vector const* From, Vector const* To,
                                   void* Cam)
    {
        // the field++ (reset by the Draw at 00511d7a, never read).
        ++G_SunRayQueryCount;

        // the sun's own solar is excluded when its class carries every
        // bit of 0x303 (ClassSolar).
        CObject* ExcludeOwn = nullptr;
        {
            CObject* const Own = SunRayOwnerObject(Sphere->Owner);
            if (Own != nullptr && (static_cast<unsigned>(Own->ObjectClass) & 0x303u) == 0x303u)
                ExcludeOwn = Own;
        }

        // from the cockpit (the routine) the player's ship is excluded
        // too, when its class has bit 0 set (AND 1 / CMP DL,1).
        CObject* ExcludePlayer = nullptr;
        if (IsFirstPersonCameraCurrent())
        {
            CObject* const Player = SunRayOwnerObject(GetPlayerObjRW());
            if (Player != nullptr && (static_cast<unsigned>(Player->ObjectClass) & 1u) == 1u)
                ExcludePlayer = Player;
        }

        // FindRayCollisions(the routine(), *From, *To, the field, 6).
        int const Count =
            PhySys::FindRayCollisions(StarSysGetActiveSystemId(), *From, *To, G_SunRayHits, 6);

        // threshold = Sphere+0x88 * 0.65f (the field).
        float const Threshold = Sphere->PixelScale * 0.65f;

        for (int Index = 0; Index < Count; ++Index)
        {
            PhySys::RayHit const& Hit = G_SunRayHits[Index];
            CObject* const Obj = Hit.Object;

            // skip a hit on a shield part. The hit's +0x1c is the part id
            // (compared to PartMap[0] in 12-byte entries, byte +8 = is_shield_part) -- the
            // same scan CObject::is_shield_part does, inlined in baseline.
            unsigned const PartId = Hit.PartId;
            if (Obj->is_shield_part(PartId))
                continue;

            // .
            if (Obj == ExcludeOwn || Obj == ExcludePlayer)
                continue;

            // the object's centre (+0x2c) into view space: the camera's
            // slot +0x40 transform through the lazily acquired math engine (the field,
            // ) slot +0x28, TransformXform.
            float const Radius = Obj->Radius;
            ICamera* const Camera = (ICamera*)Cam;
            FTransform const* const View = (FTransform const*)Camera->GetViewTransform();
            if (G_SunRayMath == nullptr)
                G_SunRayMath = Acquire3DMathEngine();
            FVector ViewPos;
            G_SunRayMath->TransformXform(ViewPos, *View,
                                         *reinterpret_cast<FVector const*>(&Obj->WorldPosition));

            // FCOMP -0.1f (the field), TEST AH,0x41 / JNZ keeps z when
            // C0 or C3 is set (z <= -0.1f, or unordered); only an ordered z > -0.1f becomes
            // -0.1f (0xbdcccccd).
            float Z = ViewPos.Z;
            if (Z > -0.1f)
                Z = -0.1f;

            // size = slot+0x20(cam) * slot+0x2c(cam) * (-1.0f / z) * radius
            // (the field = -1.0f). TEST AH,0x41 / JZ: occluded only on an ordered
            // size > threshold.
            float const Term2c = (float)Camera->GetXScaleOverNear();
            float const Term20 = Camera->GetZNear();
            float const Size = Term20 * Term2c * (-1.0f / Z) * Radius;
            if (Size > Threshold)
                return 1;
        }
        return 0;
    }

    void ApproachSlotAlpha(float* Alpha, unsigned char* Flag, float Target, float StepUp,
                           float StepDown)
    {
        *Flag = 1;
        *Alpha = ApproachFloat(*Alpha, Target, StepUp, StepDown);
        if (*Alpha < 0.0f)
            *Alpha = 0.0f;
        if (*Alpha > 1.0f)
            *Alpha = 1.0f;
    }

    // 's four-test screen box (0x0050dc51..0x0050dcac and its three twins): the
    // sample is on screen when X > -R, W + R > X, Y > -R and H + R > Y, each an ordered
    // "greater" (FCOMP, TEST AH,0x41 / JNZ to the off-screen arm). W and H are FILD'd ints.
    bool SunSampleOnScreen(float Sx, float Sy, float Margin, float NegMargin, int Width,
                           int Height)
    {
        return Sx > NegMargin && static_cast<double>(Width) + Margin > Sx && Sy > NegMargin &&
               static_cast<double>(Height) + Margin > Sy;
    }

    // 's per-sample ray (0x0050dcb2..0x0050dd89 and its two twins): camera slot 14
    // Unproject(&dir, x, y) into the stack local, the direction normalised in place (1/len, no
    // zero test) and spilled to floats, To = cam + dir * dist (the z product spilled to a float
    // first, 0x0050dd2d), then the routine(sphere, &cam, &To, cam).
    unsigned char SunSampleRayBlocked(FStarSphere* Sphere, ICamera* Camera, Vector const& CamPos,
                                      float Dist, float Sx, float Sy)
    {
        float Dir[3];
        Camera->Unproject(Dir, Sx, Sy);
        double const Inv =
            1.0 / std::sqrt(static_cast<double>(Dir[0]) * Dir[0] +
                            static_cast<double>(Dir[1]) * Dir[1] +
                            static_cast<double>(Dir[2]) * Dir[2]);
        Dir[0] = static_cast<float>(Dir[0] * Inv);
        Dir[1] = static_cast<float>(Dir[1] * Inv);
        Dir[2] = static_cast<float>(Dir[2] * Inv);
        float const ZReach = static_cast<float>(static_cast<double>(Dir[2]) * Dist);
        Vector To{};
        To.x = static_cast<float>(static_cast<double>(Dir[0]) * Dist + CamPos.x);
        To.y = static_cast<float>(static_cast<double>(Dir[1]) * Dist + CamPos.y);
        To.z = static_cast<float>(static_cast<double>(ZReach) + CamPos.z);
        return SunRayIsOccluded(Sphere, &CamPos, &To, Camera);
    }

    // 's 0.3/0.7 pair blend (0x0050e541..0x0050e574, 0x0050e64b..0x0050e684): FLD
    // own / FCOMP neighbour, TEST AH,0x41 / JNZ -- own <= neighbour or unordered puts 0.3 on the
    // neighbour and 0.7 on its own value, an ordered own > neighbour the other way round
    // (the field = 0.3, the field = 0.7, both double).
    float CoronaBlendPair(float Own, float Neighbour)
    {
        if (!(Own > Neighbour))
            return static_cast<float>(Neighbour * 0.3 + Own * 0.7);
        return static_cast<float>(Neighbour * 0.7 + Own * 0.3);
    }

    // (__thiscall on the lens-glow body, RET 0xc; its only caller the Draw at
    // ): per-frame occlusion of the corona samples. baseline tests nothing for null.
    // Slots are 0x18 bytes: +0 / +4 the sample's offset, +0x10 alpha, +0x14 touched flag; the
    // main slot 0 and the inner ring 1..12 start at +0, the outer ring 0..11 at +0x138.
    void SunBodyUpdateOcclusion(FSunEffectBody* Body, void* Cam, FStarSphere* Sphere, float Dt)
    {
        unsigned char* const Base = Body->Raw;
        ICamera* const Camera = static_cast<ICamera*>(Cam);

        // both steps spilled to float locals.
        float const StepUp = Dt / *reinterpret_cast<float*>(Base + 0x2e8);
        float const StepDown = Dt / *reinterpret_cast<float*>(Base + 0x2ec);

        // viewport (slot 5) W = r - l + 1, H = b - t + 1; the sphere's
        // screen X/Y (+0x7c/+0x80) and pixel scale (+0x88) copied.
        int const* const Vp = Camera->GetViewport();
        int const Width = Vp[2] - Vp[0] + 1;
        int const Height = Vp[3] - Vp[1] + 1;
        float const ScreenX = Sphere->ScreenX;
        float const ScreenY = Sphere->ScreenY;
        float const PixelScale = Sphere->PixelScale;

        // touched flags cleared -- +0x14 + i*0x18 and +0x14c + i*0x18
        // for i = 0..11, then +0x134 (main/inner slot 12).
        for (int I = 0; I < 12; ++I)
        {
            Base[0x14 + I * 0x18] = 0;
            Base[0x14c + I * 0x18] = 0;
        }
        Base[0x134] = 0;

        // LOD = the first k whose int threshold (+0x2f8 + 4k, FILD) is
        // an ordered "greater" than the pixel scale, else 5.
        int const* const Thresholds = reinterpret_cast<int const*>(Base + 0x2f8);
        int Lod = 5;
        if (static_cast<double>(Thresholds[0]) > PixelScale)
            Lod = 0;
        else if (static_cast<double>(Thresholds[1]) > PixelScale)
            Lod = 1;
        else if (static_cast<double>(Thresholds[2]) > PixelScale)
            Lod = 2;
        else if (static_cast<double>(Thresholds[3]) > PixelScale)
            Lod = 3;
        else if (static_cast<double>(Thresholds[4]) > PixelScale)
            Lod = 4;
        // minus +0x2b8 (0 when below it, JGE signed), capped at +0x2b4.
        int* const LodSlot = reinterpret_cast<int*>(Base + 0x2b0);
        int const LodMin = *reinterpret_cast<int*>(Base + 0x2b8);
        *LodSlot = (Lod >= LodMin) ? Lod - LodMin : 0;
        int const LodCap = *reinterpret_cast<int*>(Base + 0x2b4);
        if (*LodSlot > LodCap)
            *LodSlot = LodCap;

        // margin R = +0x88 * +0x2ac, stored as float; the unrounded
        // value against 50.0f (the field), only an ordered R < 50 becomes 50.
        double const MarginExt =
            static_cast<double>(PixelScale) * *reinterpret_cast<float*>(Base + 0x2ac);
        float Margin = static_cast<float>(MarginExt);
        if (MarginExt < 50.0)
            Margin = 50.0f;

        // camera slot 3 position and the owner's slot 0 position
        // copied; distance = sqrt((dx*dx + dz*dz) + dy*dy) stored as float.
        float const* const CamPosPtr = Camera->GetPosition();
        Vector CamPos{};
        CamPos.x = CamPosPtr[0];
        CamPos.y = CamPosPtr[1];
        CamPos.z = CamPosPtr[2];
        float const* const SunPtr = OwnerGetWorldPos(Sphere->Owner);
        double const Dx = static_cast<double>(SunPtr[0]) - CamPos.x;
        double const Dy = static_cast<double>(SunPtr[1]) - CamPos.y;
        double const Dz = static_cast<double>(SunPtr[2]) - CamPos.z;
        float const Dist = static_cast<float>(std::sqrt((Dx * Dx + Dz * Dz) + Dy * Dy));

        // the main sample at (+0x88 * slot[0] + X, +0x88 * slot[4] + Y).
        float const NegMargin = -Margin;
        float* const MainAlpha = reinterpret_cast<float*>(Base + 0x10);
        {
            float const Sx = static_cast<float>(
                static_cast<double>(PixelScale) * *reinterpret_cast<float*>(Base + 0x00) + ScreenX);
            float const Sy = static_cast<float>(
                static_cast<double>(PixelScale) * *reinterpret_cast<float*>(Base + 0x04) + ScreenY);
            if (SunSampleOnScreen(Sx, Sy, Margin, NegMargin, Width, Height))
            {
                // target 1 - (blocked ? 1 : 0), approached with the
                // two steps, then the [0, 1] clamp.
                unsigned char const bBlocked =
                    SunSampleRayBlocked(Sphere, Camera, CamPos, Dist, Sx, Sy);
                float const Target = 1.0f - (bBlocked != 0 ? 1.0f : 0.0f);
                ApproachSlotAlpha(MainAlpha, Base + 0x14, Target, StepUp, StepDown);
            }
            else
            {
                // the same approach toward 0, then the clamp.
                ApproachSlotAlpha(MainAlpha, Base + 0x14, 0.0f, StepUp, StepDown);
            }
        }

        // sample strides by LOD (switch 0x0050e6dc); both default to 14.
        int const LodNow = *LodSlot;
        int StrideInner = 14;
        int StrideOuter = 14;
        switch (LodNow)
        {
        case 1:
            StrideInner = 4;
            break;
        case 2:
            StrideInner = 2;
            break;
        case 3:
            StrideInner = 2;
            StrideOuter = 2;
            break;
        case 4:
            StrideInner = 1;
            StrideOuter = 2;
            break;
        case 5:
            StrideInner = 1;
            StrideOuter = 1;
            break;
        default:
            break;
        }

        // LOD > 2 -- the outer ring from slot 0, every StrideOuter-th,
        // placed at (+0x88 * slot[0] * +0x2f4 + X, ...); the target is float((main + 1) * 0.5)
        // times (1 - blocked), or times 0.0f off screen (so a non-finite factor stays NaN).
        if (LodNow > 2)
        {
            float const Mul = static_cast<float>((static_cast<double>(*MainAlpha) + 1.0) * 0.5);
            float const Spread = *reinterpret_cast<float*>(Base + 0x2f4);
            unsigned char* Slot = Base + 0x138;
            for (int I = 0; I < 12; I += StrideOuter)
            {
                float const Sx = static_cast<float>(static_cast<double>(PixelScale) *
                                                        *reinterpret_cast<float*>(Slot + 0x00) *
                                                        Spread +
                                                    ScreenX);
                float const Sy = static_cast<float>(static_cast<double>(PixelScale) *
                                                        *reinterpret_cast<float*>(Slot + 0x04) *
                                                        Spread +
                                                    ScreenY);
                float* const Alpha = reinterpret_cast<float*>(Slot + 0x10);
                if (SunSampleOnScreen(Sx, Sy, Margin, NegMargin, Width, Height))
                {
                    unsigned char const bBlocked =
                        SunSampleRayBlocked(Sphere, Camera, CamPos, Dist, Sx, Sy);
                    float const Target = (1.0f - (bBlocked != 0 ? 1.0f : 0.0f)) * Mul;
                    ApproachSlotAlpha(Alpha, Slot + 0x14, Target, StepUp, StepDown);
                }
                else
                {
                    ApproachSlotAlpha(Alpha, Slot + 0x14, Mul * 0.0f, StepUp, StepDown);
                }
                Slot += StrideOuter * 0x18;
            }
        }

        // LOD > 0 -- the inner ring from slot 1, every StrideInner-th,
        // at (+0x88 * slot[0] + X, ...). The factor starts as float((main + 1) * 0.5); at
        // LOD > 2 each sample replaces it with float((outer[i-1] alpha + 1) * 0.5) (+0x120 from
        // the inner alpha), and the replacement persists.
        if (*LodSlot > 0)
        {
            float Mul = static_cast<float>((static_cast<double>(*MainAlpha) + 1.0) * 0.5);
            unsigned char* Slot = Base + 0x18;
            for (int I = 1; I < 13; I += StrideInner)
            {
                float const Sx = static_cast<float>(
                    static_cast<double>(PixelScale) * *reinterpret_cast<float*>(Slot + 0x00) +
                    ScreenX);
                if (*LodSlot > 2)
                    Mul = static_cast<float>(
                        (static_cast<double>(*reinterpret_cast<float*>(Slot + 0x10 + 0x120)) + 1.0) *
                        0.5);
                float const Sy = static_cast<float>(
                    static_cast<double>(PixelScale) * *reinterpret_cast<float*>(Slot + 0x04) +
                    ScreenY);
                float* const Alpha = reinterpret_cast<float*>(Slot + 0x10);
                if (SunSampleOnScreen(Sx, Sy, Margin, NegMargin, Width, Height))
                {
                    unsigned char const bBlocked =
                        SunSampleRayBlocked(Sphere, Camera, CamPos, Dist, Sx, Sy);
                    float const Target = (1.0f - (bBlocked != 0 ? 1.0f : 0.0f)) * Mul;
                    ApproachSlotAlpha(Alpha, Slot + 0x14, Target, StepUp, StepDown);
                }
                else
                {
                    ApproachSlotAlpha(Alpha, Slot + 0x14, Mul * 0.0f, StepUp, StepDown);
                }
                Slot += StrideInner * 0x18;
            }
        }

        // / 0x0050e6b7: LOD 0 copies the main alpha into all 12 inner
        // slots and returns; a LOD outside 1..4 returns.
        int const LodPost = *LodSlot;
        if (LodPost == 0)
        {
            float const Main = *MainAlpha;
            unsigned char* Slot = Base + 0x18;
            for (int I = 0; I < 12; ++I)
            {
                *reinterpret_cast<float*>(Slot + 0x10) = Main;
                Slot += 0x18;
            }
            return;
        }
        if (LodPost < 0 || LodPost > 4)
            return;

        // while the inner stride > 1, the skipped inner samples are
        // filled halfway: dst slot 1 + half (half = ftol(stride * 0.5f)), src slot 1, the
        // neighbour slot src + stride wrapped past 12; the stride then halves.
        if (StrideInner > 1)
        {
            int Stride = StrideInner;
            do
            {
                int const Half = static_cast<int>(static_cast<double>(Stride) * 0.5);
                float* Dst = reinterpret_cast<float*>(Base + 0x28 + Half * 0x18);
                float const* Src = reinterpret_cast<float const*>(Base + 0x28);
                for (int I = 1; I < 13;)
                {
                    int const Next = I + Stride;
                    int Offset = Next * 0x18;
                    if (Next > 12)
                        Offset -= 0x120;
                    float const Neighbour = *reinterpret_cast<float const*>(Base + Offset + 0x10);
                    *Dst = CoronaBlendPair(*Src, Neighbour);
                    Dst += Stride * 6;
                    Src += Stride * 6;
                    I = Next;
                }
                Stride = Half;
            } while (Stride > 1);
        }

        // LOD 2 -- outer[2j] = float((inner[2j+1] + main) * 0.5) for
        // j = 0..5. (baseline's wrap test on 2j+1 > 12 cannot fire: 2j+1 <= 11.)
        if (*LodSlot == 2)
        {
            for (int J = 0; J < 12; J += 2)
            {
                float const Inner = *reinterpret_cast<float const*>(Base + 0x10 + (J + 1) * 0x18);
                *reinterpret_cast<float*>(Base + 0x148 + J * 0x18) =
                    static_cast<float>((static_cast<double>(Inner) + *MainAlpha) * 0.5);
            }
        }

        // only LOD 3/4 with an outer stride > 1 go on; the outer ring's
        // skipped samples are filled like the inner ones, from slot 0, wrap past 11.
        if (*LodSlot <= 2)
            return;
        if (StrideOuter <= 1)
            return;
        int Stride = StrideOuter;
        do
        {
            int const Half = static_cast<int>(static_cast<double>(Stride) * 0.5);
            float* Dst = reinterpret_cast<float*>(Base + 0x148 + Half * 0x18);
            float const* Src = reinterpret_cast<float const*>(Base + 0x148);
            for (int I = 0; I < 12;)
            {
                int const Next = I + Stride;
                int Offset = Next * 0x18;
                if (Next > 11)
                    Offset -= 0x120;
                float const Neighbour = *reinterpret_cast<float const*>(Base + Offset + 0x148);
                *Dst = CoronaBlendPair(*Src, Neighbour);
                Dst += Stride * 6;
                Src += Stride * 6;
                I = Next;
            }
            Stride = Half;
        } while (Stride > 1);
    }
    int CoronaAlphaSignBand(float V)
    {
        if (V > 0.999f)
            return 1;
        if (V < 0.001f)
            return -1;
        return 0;
    }

    // (thiscall on one corona slot {x, y, u, v, alpha}, RET 0x24: screen position
    // (3 floats by value), scale, colour (3 floats by value), a flag byte it never reads,
    // intensity), listing-traced 0x0050f230..0x0050f292. Color(rgb, 0.5 (the field) * alpha
    // * intensity); TexCoord(u, v); Vertex(scale * x + sx, scale * y + sy, 0), each product and
    // sum on the FPU and rounded once. the routine inlines the same sequence for every other
    // corona vertex, so it uses this too.
    void CoronaEmitVertex(float const* Slot, float Sx, float Sy, float Scale, float const* Rgb,
                          float IntensityMul)
    {
        Win3DColor4f(Rgb[0], Rgb[1], Rgb[2],
                     static_cast<float>(static_cast<double>(kHalf) * Slot[4] * IntensityMul));
        Win3DTexCoord2f(Slot[2], Slot[3]);
        float const Y = static_cast<float>(static_cast<double>(Scale) * Slot[1] + Sy);
        float const X = static_cast<float>(static_cast<double>(Scale) * Slot[0] + Sx);
        Win3DVertex3f(X, Y, 0.0f);
    }

    // — corona/glow via Win3D. LOD<3 fan + LOD≥3 triangle adjacency + outer fan.
    void SunBodyDrawCorona(FSunEffectBody* Body, FStarSphere* Sphere, float IntensityMul)
    {
        // `this` (ESI) is used at once -- baseline tests neither it nor the sphere.
        unsigned char* const Base = Body->Raw;
        FTexShape const** ShapeSlot = reinterpret_cast<FTexShape const**>(Base + 0x2e0);
        unsigned char* AlphaFlag = Base + 0x2e4;
        char const* ShapeName = reinterpret_cast<char const*>(Base + 0x2c0);
        FTexShape const* Shape = ResolveNamedShape(ShapeName, ShapeSlot, AlphaFlag);
        if (Shape == nullptr)
            return;

        int const BlendSrc = (*AlphaFlag == 0) ? 2 : 5;
        Win3DSetBlendFunc(BlendSrc, 2);
        // the routine(0xbe2) -- glDisable(GL_BLEND).
        if (G_SunCoronaNoBlend != 0)
            Win3DDisable(kWin3DBlend);

        // scale = (float)(+0x2ac * sphere +0x88); the sun's screen
        // position (sphere +0x7c..+0x84) copied; the lod +0x2b0.
        float const Scale = static_cast<float>(
            static_cast<double>(*reinterpret_cast<float*>(Base + 0x2ac)) * Sphere->PixelScale);
        float const Cx = Sphere->ScreenX;
        float const Cy = Sphere->ScreenY;
        int const Lod = *reinterpret_cast<int*>(Base + 0x2b0);
        float const* const CenterRgb = reinterpret_cast<float*>(Base + 0x288);
        float const* const MidRgb = reinterpret_cast<float*>(Base + 0x294);
        float const* const OuterRgb = reinterpret_cast<float*>(Base + 0x2a0);
        // Slot {x, y, u, v, alpha} (stride 0x18): the centre at +0x00, the ring slots k = 1..12
        // at k * 0x18, the mid ring slots i = 0..11 at 0x138 + i * 0x18.
        float const* const Centre = reinterpret_cast<float*>(Base);

        if (Lod < 3)
        {
            // the shape's texture bound, a fan (Begin 6): the centre in
            // the centre colour, ring slots 1 .. the field - 1 (the count re-read each pass)
            // and slot 1 again in the outer colour; End at the branch.
            Win3DBindTexture(ResolveShapeTextureHandle(Shape));
            Win3DBegin(kWin3DTriangleFan);
            CoronaEmitVertex(Centre, Cx, Cy, Scale, CenterRgb, IntensityMul);
            for (int Index = 1; Index < kCoronaRingCount; ++Index)
                CoronaEmitVertex(Centre + Index * 6, Cx, Cy, Scale, OuterRgb, IntensityMul);
            CoronaEmitVertex(Centre + 6, Cx, Cy, Scale, OuterRgb, IntensityMul);
            Win3DEnd();
        }
        else
        {
            // the shape's texture bound, triangles (Begin 4) from the
            // start slot while below the field (re-read each pass). For mid slot
            // i: A = i + 1 (0 past 11) in the mid ring, B = i + 1 and C = i + 2 (1 past 12) in the
            // outer ring; the edge flag +0x258[i] picks the diagonal.
            Win3DBindTexture(ResolveShapeTextureHandle(Shape));
            Win3DBegin(kWin3DTriangles);
            int Index = G_CoronaLod3Start;
            while (Index < kCoronaOuterCount)
            {
                int const Local = Index + 1;
                int const SlotA = (Local <= 11) ? Local : Local - 12;
                int const SlotB = (Local <= 12) ? Local : Local - 12;
                int const SlotC = (Local + 1 <= 12) ? Local + 1 : Local + 1 - 12;
                float const* const Mid = Centre + (0x138 / 4) + Index * 6;
                float const* const MidA = Centre + (0x138 / 4) + SlotA * 6;
                float const* const RingB = Centre + SlotB * 6;
                float const* const RingC = Centre + SlotC * 6;
                int* const EdgeFlag = reinterpret_cast<int*>(Base + 0x258) + Index;

                // alpha bands (above 0.999f -> 1, TEST AH,0x41 / JNZ;
                // below 0.001f -> -1, TEST AH,5 / JP; else and unordered 0) of mid i and mid A;
                // equal -> the bands of ring B and ring C decide instead. Where they differ:
                // the first +1 -> 0, the first -1 -> 1, the second +1 -> 1, the second -1 -> 0,
                // stored in the flag; equal pairs keep the flag.
                int const BandMid = CoronaAlphaSignBand(Mid[4]);
                int const BandMidA = CoronaAlphaSignBand(MidA[4]);
                int First = BandMid;
                int Second = BandMidA;
                if (BandMid == BandMidA)
                {
                    First = CoronaAlphaSignBand(RingB[4]);
                    Second = CoronaAlphaSignBand(RingC[4]);
                }
                if (First != Second)
                {
                    if (First == 1)
                        *EdgeFlag = 0;
                    else if (First == -1)
                        *EdgeFlag = 1;
                    else if (Second == 1)
                        *EdgeFlag = 1;
                    else if (Second == -1)
                        *EdgeFlag = 0;
                }

                // flag 0 -> (mid A, mid i, ring B) (mid A, ring B,
                // ring C); otherwise (mid A, mid i, ring C) (mid i, ring B, ring C). Mid slots
                // in the mid colour, ring slots in the outer colour.
                if (*EdgeFlag == 0)
                {
                    CoronaEmitVertex(MidA, Cx, Cy, Scale, MidRgb, IntensityMul);
                    CoronaEmitVertex(Mid, Cx, Cy, Scale, MidRgb, IntensityMul);
                    CoronaEmitVertex(RingB, Cx, Cy, Scale, OuterRgb, IntensityMul);
                    CoronaEmitVertex(MidA, Cx, Cy, Scale, MidRgb, IntensityMul);
                    CoronaEmitVertex(RingB, Cx, Cy, Scale, OuterRgb, IntensityMul);
                    CoronaEmitVertex(RingC, Cx, Cy, Scale, OuterRgb, IntensityMul);
                }
                else
                {
                    CoronaEmitVertex(MidA, Cx, Cy, Scale, MidRgb, IntensityMul);
                    CoronaEmitVertex(Mid, Cx, Cy, Scale, MidRgb, IntensityMul);
                    CoronaEmitVertex(RingC, Cx, Cy, Scale, OuterRgb, IntensityMul);
                    CoronaEmitVertex(Mid, Cx, Cy, Scale, MidRgb, IntensityMul);
                    CoronaEmitVertex(RingB, Cx, Cy, Scale, OuterRgb, IntensityMul);
                    CoronaEmitVertex(RingC, Cx, Cy, Scale, OuterRgb, IntensityMul);
                }
                ++Index;
            }
            // the routine.
            Win3DEnd();

            // with the field the texture bound again and a fan
            // (Begin 6): the centre in the centre colour, mid slots 0 .. the field - 1 (the
            // count re-read each pass) in the mid colour, then the routine on mid slot 0
            // (this + 0x138) with the mid colour; End at the branch.
            if (kCoronaOuterFanEnable != 0)
            {
                Win3DBindTexture(ResolveShapeTextureHandle(*ShapeSlot));
                Win3DBegin(kWin3DTriangleFan);
                CoronaEmitVertex(Centre, Cx, Cy, Scale, CenterRgb, IntensityMul);
                for (int Fan = 0; Fan < kCoronaOuterFanCount; ++Fan)
                    CoronaEmitVertex(Centre + (0x138 / 4) + Fan * 6, Cx, Cy, Scale, MidRgb,
                                     IntensityMul);
                CoronaEmitVertex(Centre + (0x138 / 4), Cx, Cy, Scale, MidRgb, IntensityMul);
                Win3DEnd();
            }
        }
        // — the routine(0xbe2): re-enable blending if we turned it off
        // (the field).
        if (G_SunCoronaNoBlend != 0)
            Win3DEnable(kWin3DBlend);
    }

    // (thiscall on the lens-flare child, RET 0xc: camera, sphere, intensity),
    // listing-traced 0x0050f850..0x0050fb3e -- the lens-flare quads in Win3D mode 8. NOT
    // ScreenQuadEnqueueFlare. No argument is tested.
    void LensFlareDraw(FStarFxChild40* Flare, ICamera* Cam, FStarSphere* Sphere, float IntensityMul)
    {
        // the shape cached at +0x34; a miss looks the inline name at
        // +0x14 up in the field as it is (ShapeDB.h:0xf2 warning and 0 when absent).
        unsigned char* const Base = Flare->Raw;
        FTexShape const** const ShapeSlot = reinterpret_cast<FTexShape const**>(Base + 0x34);
        ResolveNamedShape(reinterpret_cast<char const*>(Base + 0x14), ShapeSlot, nullptr);

        // the viewport (slot 5), the sun's screen x / y / z
        // (+0x7c..+0x84) copied, the viewport middle (l + r) * 0.5 and (t + b) * 0.5 (double
        // ), the vertical one through a float.
        int const* const Rect = Cam->GetViewport();
        float const SunX = Sphere->ScreenX;
        float const SunY = Sphere->ScreenY;
        float const SunZ = Sphere->ScreenZFlat;
        double const MidX = static_cast<double>(Rect[0] + Rect[2]) * 0.5;
        float const MidY = static_cast<float>(static_cast<double>(Rect[3] + Rect[1]) * 0.5);

        // brightness 1 - (1 - intensity(+0x64))^2 (double
        // ).
        double const OneMinusIntensity = 1.0 - static_cast<double>(Sphere->Intensity);
        double const Brightness = 1.0 - OneMinusIntensity * OneMinusIntensity;

        // the pixel scale (+0x88) clamped: above +0x3c (TEST AH,0x41 /
        // JNZ keeps not-greater and unordered) -> +0x3c, else below +0x38 (TEST AH,5 / JP keeps
        // greater, equal and unordered) -> +0x38.
        float Scale = Sphere->PixelScale;
        if (Scale > *reinterpret_cast<float*>(Base + 0x3c))
            Scale = *reinterpret_cast<float*>(Base + 0x3c);
        else if (Scale < *reinterpret_cast<float*>(Base + 0x38))
            Scale = *reinterpret_cast<float*>(Base + 0x38);

        // the half-size base scale * brightness * intensity argument,
        // through a float.
        float const HalfBase = static_cast<float>(static_cast<double>(Scale) * Brightness *
                                                  static_cast<double>(IntensityMul));

        // fade 1; with the squared length of (mid - sun, -z) (z, then y,
        // then x added) below ((r - l) * 0.1)^2 (double the field, the limit through a
        // float; TEST AH,5 / JP keeps 1 for not-less and unordered) it is length / limit.
        double const Dx = MidX - static_cast<double>(SunX);
        double const Dy = static_cast<double>(MidY) - static_cast<double>(SunY);
        double const Dz = -static_cast<double>(SunZ);
        double const Tenth = static_cast<double>(Rect[2] - Rect[0]) * 0.1;
        float const Limit = static_cast<float>(Tenth * Tenth);
        double const DistSq = (Dz * Dz + Dy * Dy) + Dx * Dx;
        float Fade = 1.0f;
        if (DistSq < static_cast<double>(Limit))
            Fade = static_cast<float>(DistSq / static_cast<double>(Limit));

        // the flare axis (mid - sun) * 3 (the field), as floats.
        float const AxisX = static_cast<float>(Dx * static_cast<double>(3.0f));
        float const AxisY = static_cast<float>(Dy * static_cast<double>(3.0f));

        // no shape -> nothing drawn.
        if (*ShapeSlot == nullptr)
            return;

        // the shape's texture (the routine, frame 0) bound
        // (the routine), Begin(8).
        Win3DBindTexture(ResolveShapeTextureHandle(*ShapeSlot));
        Win3DBegin(kWin3DQuads);

        // every element pointer in [+4, +8) (the end re-read each pass):
        // colour (+8, +0xc, +0x10, fade * +0x14); centre sun + axis * +0 (the y product through
        // a float first); half size base * +4; the quad (u,v) (0,0) (1,0) (1,1) (0,1) at
        // (x-h,y-h) (x+h,y-h) (x+h,y+h) (x-h,y+h), z 0.
        float** It = *reinterpret_cast<float***>(Base + 0x04);
        while (It != *reinterpret_cast<float***>(Base + 0x08))
        {
            float const* const Elem = *It;
            Win3DColor4f(Elem[2], Elem[3], Elem[4],
                         static_cast<float>(static_cast<double>(Fade) * Elem[5]));
            double const Along = static_cast<double>(Elem[0]);
            float const AlongY = static_cast<float>(static_cast<double>(AxisY) * Along);
            float const Cx =
                static_cast<float>(static_cast<double>(AxisX) * Along + static_cast<double>(SunX));
            float const Cy =
                static_cast<float>(static_cast<double>(AlongY) + static_cast<double>(SunY));
            float const Half =
                static_cast<float>(static_cast<double>(HalfBase) * static_cast<double>(Elem[1]));
            float const Y0 = static_cast<float>(static_cast<double>(Cy) - Half);
            float const X0 = static_cast<float>(static_cast<double>(Cx) - Half);
            Win3DTexCoord2f(0.0f, 0.0f);
            Win3DVertex3f(X0, Y0, 0.0f);
            Win3DTexCoord2f(1.0f, 0.0f);
            float const X1 = static_cast<float>(static_cast<double>(Cx) + Half);
            Win3DVertex3f(X1, Y0, 0.0f);
            Win3DTexCoord2f(1.0f, 1.0f);
            float const Y1 = static_cast<float>(static_cast<double>(Cy) + Half);
            Win3DVertex3f(X1, Y1, 0.0f);
            Win3DTexCoord2f(0.0f, 1.0f);
            Win3DVertex3f(X0, Y1, 0.0f);
            ++It;
        }

        // the routine.
        Win3DEnd();
    }

    // The two per-slot values the routine averages: SunEffect body slot i (stride 0x18) at
    // +0x10 and +0x148, read as floats onto the FPU.
    double CoronaSlotOuter(unsigned char const* Body, int Slot)
    {
        return static_cast<double>(*reinterpret_cast<float const*>(Body + Slot * 0x18 + 0x10));
    }
    double CoronaSlotInner(unsigned char const* Body, int Slot)
    {
        return static_cast<double>(*reinterpret_cast<float const*>(Body + Slot * 0x18 + 0x148));
    }

    // (stdcall, RET 0x10: spine index, spine count, a third argument it never
    // reads, sphere), listing-traced 0x00510990..0x00510bc4 -- the spine's intensity, a weighted
    // average of the SunEffect body (sphere +0x14) slots around the spine, left on the FPU.
    // Counts other than 2, 4, 6 and 12 give 0 (the switch default keeps the FLD 0.0). The
    // wrap tests are baseline's own, including the ones that let an index reach 12. No argument
    // is tested.
    double SpineIntensityFromCorona(int SpineIndex, int SpineCount, float /*Unused*/,
                                    FStarSphere* Sphere)
    {
        switch (SpineCount)
        {
        case 12:
        {
            // slot idx + 1 (wrapped only above 12); with the body's lod
            // (+0x2b0) above 2 also the inner value of slot idx, over 2; otherwise over 1.
            int Next = SpineIndex + 1;
            if (Next > 12)
                Next -= 12;
            unsigned char const* const Body = Sphere->SunBody->Raw;
            double const Sum = CoronaSlotOuter(Body, Next);
            if (*reinterpret_cast<int const*>(Body + 0x2b0) <= 2)
                return Sum / 1;
            return (Sum + CoronaSlotInner(Body, SpineIndex)) / 2;
        }
        case 4:
        case 6:
        {
            // the base slot ftol(idx * 2) for 6 (FADD ST0,ST0),
            // ftol(idx * 3.0) (double the field) for 4.
            double const Scaled = (SpineCount == 6)
                                      ? static_cast<double>(SpineIndex) + SpineIndex
                                      : static_cast<double>(SpineIndex) * 3.0;
            int const Base = static_cast<int>(Scaled);
            // (2 * outer(b+1) + outer(b)) + outer(b+2), b+1 and b+2
            // wrapped only above 12.
            int Plus1 = Base + 1;
            if (Plus1 > 12)
                Plus1 -= 12;
            int Plus2 = Base + 2;
            if (Plus2 > 12)
                Plus2 -= 12;
            unsigned char const* const Body = Sphere->SunBody->Raw;
            double Sum = CoronaSlotOuter(Body, Plus1);
            Sum = Sum + Sum;
            Sum = Sum + CoronaSlotOuter(Body, Base);
            Sum = Sum + CoronaSlotOuter(Body, Plus2);
            // lod not above 2 -> over 4.
            if (*reinterpret_cast<int const*>(Body + 0x2b0) <= 2)
                return Sum / 4;
            // + inner(b-1) (wrapped below 0) + 3 * inner(b) (double
            // ) + inner(b+1) (wrapped above 11), over 9.
            int Minus1 = Base - 1;
            if (Minus1 < 0)
                Minus1 += 12;
            int Plus1Inner = Base + 1;
            if (Plus1Inner > 11)
                Plus1Inner -= 12;
            Sum = Sum + CoronaSlotInner(Body, Minus1);
            Sum = Sum + CoronaSlotInner(Body, Base) * 3.0;
            Sum = Sum + CoronaSlotInner(Body, Plus1Inner);
            return Sum / 9;
        }
        case 2:
        {
            // the base slot ftol(idx * 6.0) (double the field).
            int const Base = static_cast<int>(static_cast<double>(SpineIndex) * 6.0);
            // b-1 wrapped below 1, b+1 and b+2 wrapped above 12 (the
            // unwrapped b-1 and b+1 are kept for the inner sum).
            int const Minus1Raw = Base - 1;
            int Minus1 = Minus1Raw;
            if (Minus1 < 1)
                Minus1 += 12;
            int const Plus1Raw = Base + 1;
            int Plus1 = Plus1Raw;
            if (Plus1 > 12)
                Plus1 -= 12;
            int const Plus2Raw = Base + 2;
            int Plus2 = Plus2Raw;
            if (Plus2 > 12)
                Plus2 -= 12;
            // ((outer(b-1) + 2 * outer(b)) + 4 * outer(b+1) (double
            // )) + 2 * outer(b+2), + outer(b+2).
            unsigned char const* const Body = Sphere->SunBody->Raw;
            double const Twice = CoronaSlotOuter(Body, Base) + CoronaSlotOuter(Body, Base);
            double Sum = CoronaSlotOuter(Body, Minus1) + Twice;
            Sum = Sum + CoronaSlotOuter(Body, Plus1) * 4.0;
            Sum = Sum + (CoronaSlotOuter(Body, Plus2) + CoronaSlotOuter(Body, Plus2));
            Sum = Sum + CoronaSlotOuter(Body, Plus2);
            // lod not above 2 -> over 10.
            if (*reinterpret_cast<int const*>(Body + 0x2b0) <= 2)
                return Sum / 10;
            // + inner(b-2) (wrapped below 0) + 3 * inner(b-1) (the raw
            // one, wrapped below 0) + 6 * inner(b) + 3 * inner(b+1) (raw, wrapped above 11) +
            // inner(b+2) (wrapped above 11), over 24.
            int Minus2 = Base - 2;
            if (Minus2 < 0)
                Minus2 += 12;
            int Minus1Inner = Minus1Raw;
            if (Minus1Inner < 0)
                Minus1Inner += 12;
            int Plus1Inner = Plus1Raw;
            if (Plus1Inner > 11)
                Plus1Inner -= 12;
            int Plus2Inner = Plus2Raw;
            if (Plus2Inner > 11)
                Plus2Inner -= 12;
            Sum = Sum + CoronaSlotInner(Body, Minus2);
            Sum = Sum + CoronaSlotInner(Body, Minus1Inner) * 3.0;
            Sum = Sum + CoronaSlotInner(Body, Base) * 6.0;
            Sum = Sum + CoronaSlotInner(Body, Plus1Inner) * 3.0;
            Sum = Sum + CoronaSlotInner(Body, Plus2Inner);
            return Sum / 24;
        }
        default:
            // / caseD_3: the FLD 0.0 (the field) returned as it is.
            return 0.0;
        }
    }

    // (thiscall on the spines child, RET 0xc: camera -- never read --, sphere,
    // intensity), listing-traced 0x00510160..0x0051098c -- the spine strips in Win3D mode 8.
    void SpinesDraw(FStarFxChild40* Spines, void* /*Cam*/, FStarSphere* Sphere, float IntensityMul)
    {
        // the blend func is set first, before the shape is resolved,
        // so it is applied even when the shape is missing. No argument test (Sphere->SunBody
        // is read at 0x00510239 as it is).
        Win3DSetBlendFunc(5, 2);

        // cache test at +0x34, key = the inline chars at +0x14 with no
        // length test, the routine (the out-of-line copy of the same cached find); a null
        // shape leaves through the branch.
        unsigned char* const Base = Spines->Raw;
        FTexShape const** ShapeSlot = reinterpret_cast<FTexShape const**>(Base + 0x34);
        char const* const Name = reinterpret_cast<char const*>(Base + 0x14);
        FTexShape const* Shape = ResolveNamedShape(Name, ShapeSlot, nullptr);
        if (Shape == nullptr)
            return;

        // the pixel scale (+0x88) clamped: above +0x3c (TEST AH,0x41 /
        // JNZ keeps not-greater and unordered) -> +0x3c, else below +0x38 (TEST AH,5 / JP keeps
        // greater, equal and unordered) -> +0x38.
        float Scale = Sphere->PixelScale;
        if (Scale > *reinterpret_cast<float*>(Base + 0x3c))
            Scale = *reinterpret_cast<float*>(Base + 0x3c);
        else if (Scale < *reinterpret_cast<float*>(Base + 0x38))
            Scale = *reinterpret_cast<float*>(Base + 0x38);

        // the sun's screen x / y (+0x7c / +0x80); from the SunEffect body
        // (+0x14): inner radius (float)(+0x2f0 * +0x2ac * scale), outer radius
        // (float)(scale * +0x2ac * (+0x2f0 + 1) * 0.5) (doubles the field / the field),
        // and the slot-0 alpha +0x10.
        float const SunX = Sphere->ScreenX;
        float const SunY = Sphere->ScreenY;
        unsigned char const* const Body = Sphere->SunBody->Raw;
        double const Tween = static_cast<double>(*reinterpret_cast<float const*>(Body + 0x2f0));
        double const BodyScale =
            static_cast<double>(*reinterpret_cast<float const*>(Body + 0x2ac));
        float const InnerR = static_cast<float>(Tween * BodyScale * Scale);
        float const OuterR =
            static_cast<float>(static_cast<double>(Scale) * BodyScale * (Tween + 1.0) * 0.5);
        float const Alpha0 = *reinterpret_cast<float const*>(Body + 0x10);

        // element count (+8 - +4) / 0x2c, 0 when +4 is null.
        int Count = 0;
        if (*reinterpret_cast<unsigned char* const*>(Base + 0x04) != nullptr)
            Count = static_cast<int>(*reinterpret_cast<unsigned char* const*>(Base + 0x08) -
                                     *reinterpret_cast<unsigned char* const*>(Base + 0x04)) /
                    0x2c;

        // the shape's texture (the routine, frame 0) bound, Begin(8) --
        // before the count test, so End always follows.
        Win3DBindTexture(ResolveShapeTextureHandle(Shape));
        Win3DBegin(kWin3DQuads);

        // per 0x2c element (cos, sin, length, width, inner rgb, outer rgb,
        // alpha), the base pointer +4 re-read each pass: three quads along the spine -- base ->
        // inner radius -> outer radius -> tip.
        for (int Index = 0; Index < Count; ++Index)
        {
            float const* const Elem = reinterpret_cast<float const*>(
                *reinterpret_cast<unsigned char* const*>(Base + 0x04) + Index * 0x2c);
            double const Cos = static_cast<double>(Elem[0]);
            double const Sin = static_cast<double>(Elem[1]);

            // length and half width on the FPU (-half width also as three
            // float copies), 1 / length unguarded (the field), the inner / outer fractions as
            // floats.
            double const Length = static_cast<double>(Scale) * Elem[2];
            double const HalfW = static_cast<double>(Scale) * Elem[3] * 0.5;
            double const NegHalfW = -HalfW;
            double const NegHalfWF = static_cast<double>(static_cast<float>(NegHalfW));
            double const InvLength = 1.0 / Length;
            float const T0 = static_cast<float>(static_cast<double>(InnerR) * InvLength);
            float const T1 = static_cast<float>(InvLength * static_cast<double>(OuterR));

            // the -w edge: base (cos, sin) * 0 terms, inner, outer, tip.
            double const CosZero = static_cast<double>(static_cast<float>(Cos * 0.0f));
            float const BaseNegX = static_cast<float>((Sin * NegHalfW + SunX) + CosZero);
            double const SinZero = static_cast<double>(static_cast<float>(Sin * 0.0f));
            float const BaseNegY = static_cast<float>((NegHalfW * Cos - SinZero) + SunY);
            double const CosInner = static_cast<double>(static_cast<float>(Cos * InnerR));
            float const InnerNegX = static_cast<float>((NegHalfWF * Sin + SunX) + CosInner);
            double const SinInner = static_cast<double>(static_cast<float>(Sin * InnerR));
            float const InnerNegY = static_cast<float>((NegHalfWF * Cos - SinInner) + SunY);
            double const CosOuter = static_cast<double>(static_cast<float>(Cos * OuterR));
            float const OuterNegX = static_cast<float>((NegHalfWF * Sin + SunX) + CosOuter);
            double const SinOuter = static_cast<double>(static_cast<float>(Sin * OuterR));
            float const OuterNegY = static_cast<float>((NegHalfWF * Cos - SinOuter) + SunY);
            double const CosTip = static_cast<double>(static_cast<float>(Cos * Length));
            float const TipNegX = static_cast<float>((NegHalfWF * Sin + CosTip) + SunX);
            double const SinTip = static_cast<double>(static_cast<float>(Sin * Length));
            float const TipNegY = static_cast<float>((NegHalfWF * Cos - SinTip) + SunY);

            // the +w edge; sin * w is used unrounded once (FST), then as
            // a float; cos * w stays on the FPU.
            double const SinHalfW = Sin * HalfW;
            double const SinHalfWF = static_cast<double>(static_cast<float>(SinHalfW));
            double const CosHalfW = Cos * HalfW;
            float const BasePosX = static_cast<float>((SinHalfW + SunX) + CosZero);
            float const BasePosY = static_cast<float>((CosHalfW - SinZero) + SunY);
            float const InnerPosX = static_cast<float>((SinHalfWF + SunX) + CosInner);
            float const InnerPosY = static_cast<float>((CosHalfW - SinInner) + SunY);
            float const OuterPosX = static_cast<float>((SinHalfWF + SunX) + CosOuter);
            float const OuterPosY = static_cast<float>((CosHalfW - SinOuter) + SunY);
            float const TipPosX = static_cast<float>((SinHalfWF + CosTip) + SunX);
            float const TipPosY = static_cast<float>((CosHalfW - SinTip) + SunY);

            // alpha = ((1 - blend) * intensity + spine * blend) * +0x28
            // * the intensity argument (the field blend; the spine value from the routine on
            // the FPU), as a float.
            double const Spine = SpineIntensityFromCorona(Index, Count, Alpha0, Sphere);
            double const Blend = static_cast<double>(kSpineBlend);
            float const Alpha = static_cast<float>(
                ((1.0 - Blend) * Sphere->Intensity + Spine * Blend) * Elem[10] * IntensityMul);

            // the texture u at the inner / outer fraction,
            // t * u1 + u0 * (1 - t), as floats.
            double const OneMinusT0 = 1.0 - static_cast<double>(T0);
            float const UInner = static_cast<float>(static_cast<double>(T0) * Shape->U1 +
                                                    static_cast<double>(Shape->U0) * OneMinusT0);
            double const OneMinusT1 = 1.0 - static_cast<double>(T1);
            float const UOuter = static_cast<float>(static_cast<double>(T1) * Shape->U1 +
                                                    static_cast<double>(Shape->U0) * OneMinusT1);

            // the colour lerps inner -> outer at the two fractions, with
            // baseline's float roundings.
            double const T0Outer0 = static_cast<double>(T0) * Elem[7];
            double const T0Outer1 = static_cast<double>(T0) * Elem[8];
            double const T0Outer2F = static_cast<double>(static_cast<float>(
                static_cast<double>(T0) * Elem[9]));
            float const InnerColR = static_cast<float>(OneMinusT0 * Elem[4] + T0Outer0);
            float const InnerColG = static_cast<float>(
                static_cast<double>(static_cast<float>(OneMinusT0 * Elem[5])) + T0Outer1);
            float const InnerColB = static_cast<float>(
                static_cast<double>(static_cast<float>(OneMinusT0 * Elem[6])) + T0Outer2F);
            double const T1Outer0F = static_cast<double>(static_cast<float>(
                static_cast<double>(T1) * Elem[7]));
            double const T1Outer1F = static_cast<double>(static_cast<float>(
                static_cast<double>(T1) * Elem[8]));
            double const T1Outer2F = static_cast<double>(static_cast<float>(
                static_cast<double>(T1) * Elem[9]));
            float const OuterColR = static_cast<float>(OneMinusT1 * Elem[4] + T1Outer0F);
            float const OuterColG = static_cast<float>(
                static_cast<double>(static_cast<float>(OneMinusT1 * Elem[5])) + T1Outer1F);
            float const OuterColB = static_cast<float>(
                static_cast<double>(static_cast<float>(OneMinusT1 * Elem[6])) + T1Outer2F);
            float const InnerAlpha =
                static_cast<float>(static_cast<double>(Alpha) * static_cast<double>(Alpha0));

            // quad 1, base -> inner radius (base colour at alpha 0).
            Win3DTexCoord2f(Shape->U0, Shape->V0);
            Win3DColor4f(Elem[4], Elem[5], Elem[6], 0.0f);
            Win3DVertex3f(BaseNegX, BaseNegY, 0.0f);
            Win3DTexCoord2f(UInner, Shape->V0);
            Win3DColor4f(InnerColR, InnerColG, InnerColB, InnerAlpha);
            Win3DVertex3f(InnerNegX, InnerNegY, 0.0f);
            Win3DTexCoord2f(UInner, Shape->V1);
            Win3DColor4f(InnerColR, InnerColG, InnerColB, InnerAlpha);
            Win3DVertex3f(InnerPosX, InnerPosY, 0.0f);
            Win3DTexCoord2f(Shape->U0, Shape->V1);
            Win3DColor4f(Elem[4], Elem[5], Elem[6], 0.0f);
            Win3DVertex3f(BasePosX, BasePosY, 0.0f);

            // quad 2, inner -> outer radius.
            Win3DTexCoord2f(UInner, Shape->V0);
            Win3DColor4f(InnerColR, InnerColG, InnerColB, InnerAlpha);
            Win3DVertex3f(InnerNegX, InnerNegY, 0.0f);
            Win3DTexCoord2f(UOuter, Shape->V0);
            Win3DColor4f(OuterColR, OuterColG, OuterColB, Alpha);
            Win3DVertex3f(OuterNegX, OuterNegY, 0.0f);
            Win3DTexCoord2f(UOuter, Shape->V1);
            Win3DColor4f(OuterColR, OuterColG, OuterColB, Alpha);
            Win3DVertex3f(OuterPosX, OuterPosY, 0.0f);
            Win3DTexCoord2f(UInner, Shape->V1);
            Win3DColor4f(InnerColR, InnerColG, InnerColB, InnerAlpha);
            Win3DVertex3f(InnerPosX, InnerPosY, 0.0f);

            // quad 3, outer radius -> tip (outer colour).
            Win3DTexCoord2f(UOuter, Shape->V0);
            Win3DColor4f(OuterColR, OuterColG, OuterColB, Alpha);
            Win3DVertex3f(OuterNegX, OuterNegY, 0.0f);
            Win3DTexCoord2f(Shape->U1, Shape->V0);
            Win3DColor4f(Elem[7], Elem[8], Elem[9], Alpha);
            Win3DVertex3f(TipNegX, TipNegY, 0.0f);
            Win3DTexCoord2f(Shape->U1, Shape->V1);
            Win3DColor4f(Elem[7], Elem[8], Elem[9], Alpha);
            Win3DVertex3f(TipPosX, TipPosY, 0.0f);
            Win3DTexCoord2f(UOuter, Shape->V1);
            Win3DColor4f(OuterColR, OuterColG, OuterColB, Alpha);
            Win3DVertex3f(OuterPosX, OuterPosY, 0.0f);
        }

        // the routine.
        Win3DEnd();
    }

    // — Cam-basis oriented star_glow / star_center triangle fan (world verts).
    void StarGlowDrawBillboard(FStarGlowDesc* Desc, float const Pos[3], float Scale,
                               float const Right[3], float const Up[3], float Alpha)
    {
        // the only test is the shape (+0x48); no null tests on the
        // arguments and no shape lookup here (UpdateIntensity the branch resolves it).
        FTexShape const* Shape = reinterpret_cast<FTexShape const*>(Desc->Field48);
        if (Shape == nullptr)
            return;

        Win3DEnable(kWin3DBlend);
        if (Desc->Field4C == 0)
            Win3DSetBlendFunc(2, 4);
        else
            Win3DSetBlendFunc(5, 6);
        void* const Tex = ResolveShapeTextureHandle(Shape);
        Win3DBindTexture(Tex);

        float RightHalf[3];
        float UpHalf[3];
        float Tmp[3];
        ScaleVec3(Tmp, Right, Scale);
        ScaleVec3(RightHalf, Tmp, Desc->Scale);
        ScaleVec3(Tmp, Up, Scale);
        ScaleVec3(UpHalf, Tmp, Desc->Scale);

        // Corners (the routine = add, the routine = subtract), 0x0050ce1e..0x0050cebd:
        // C0 (local_b8) = (Pos - R) + U, C1 (local_94) = (Pos + R) + U,
        // C2 (local_ac) = (Pos + R) - U, C3 (local_a0) = (Pos - R) - U.
        float C0[3];
        float C1[3];
        float C2[3];
        float C3[3];
        SubVec3Local(Tmp, Pos, RightHalf);
        AddVec3Local(C0, Tmp, UpHalf);
        AddVec3Local(Tmp, Pos, RightHalf);
        AddVec3Local(C1, Tmp, UpHalf);
        AddVec3Local(Tmp, Pos, RightHalf);
        SubVec3Local(C2, Tmp, UpHalf);
        SubVec3Local(Tmp, Pos, RightHalf);
        SubVec3Local(C3, Tmp, UpHalf);

        // centre = (((C0 + C1) + C2) + C3) * 0.25; the edge midpoints
        // are (C1 + C0), (C2 + C1), (C3 + C2), (C3 + C0), each * 0.5.
        float Center[3];
        float Edge01[3];
        float Edge12[3];
        float Edge23[3];
        float Edge30[3];
        AddVec3Local(Tmp, C0, C1);
        AddVec3Local(Tmp, Tmp, C2);
        AddVec3Local(Tmp, Tmp, C3);
        Center[0] = Tmp[0] * 0.25f;
        Center[1] = Tmp[1] * 0.25f;
        Center[2] = Tmp[2] * 0.25f;
        AddVec3Local(Tmp, C1, C0);
        Edge01[0] = Tmp[0] * 0.5f;
        Edge01[1] = Tmp[1] * 0.5f;
        Edge01[2] = Tmp[2] * 0.5f;
        AddVec3Local(Tmp, C2, C1);
        Edge12[0] = Tmp[0] * 0.5f;
        Edge12[1] = Tmp[1] * 0.5f;
        Edge12[2] = Tmp[2] * 0.5f;
        AddVec3Local(Tmp, C3, C2);
        Edge23[0] = Tmp[0] * 0.5f;
        Edge23[1] = Tmp[1] * 0.5f;
        Edge23[2] = Tmp[2] * 0.5f;
        AddVec3Local(Tmp, C3, C0);
        Edge30[0] = Tmp[0] * 0.5f;
        Edge30[1] = Tmp[1] * 0.5f;
        Edge30[2] = Tmp[2] * 0.5f;

        float const U0 = Shape->U0;
        float const V0 = Shape->V0;
        float const U1 = Shape->U1;
        float const V1 = Shape->V1;
        float const Um = (U0 + U1) * 0.5f;
        float const Vm = (V0 + V1) * 0.5f;

        Win3DBegin(kWin3DTriangleFan);
        Win3DColor4f(Desc->InnerColor[0], Desc->InnerColor[1], Desc->InnerColor[2], Alpha);
        Win3DTexCoord2f(Um, Vm);
        Win3DVertex3f(Center[0], Center[1], Center[2]);

        Win3DColor4f(Desc->OuterColor[0], Desc->OuterColor[1], Desc->OuterColor[2], Alpha);
        Win3DTexCoord2f(U0, V0);
        Win3DVertex3f(C0[0], C0[1], C0[2]);
        Win3DTexCoord2f(Um, V0);
        Win3DVertex3f(Edge01[0], Edge01[1], Edge01[2]);
        Win3DTexCoord2f(U1, V0);
        Win3DVertex3f(C1[0], C1[1], C1[2]);
        Win3DTexCoord2f(U1, Vm);
        Win3DVertex3f(Edge12[0], Edge12[1], Edge12[2]);
        Win3DTexCoord2f(U1, V1);
        Win3DVertex3f(C2[0], C2[1], C2[2]);
        Win3DTexCoord2f(Um, V1);
        Win3DVertex3f(Edge23[0], Edge23[1], Edge23[2]);
        Win3DTexCoord2f(U0, V1);
        Win3DVertex3f(C3[0], C3[1], C3[2]);
        Win3DTexCoord2f(U0, Vm);
        Win3DVertex3f(Edge30[0], Edge30[1], Edge30[2]);
        Win3DTexCoord2f(U0, V0);
        Win3DVertex3f(C0[0], C0[1], C0[2]);
        Win3DEnd();
    }

    int FStarSphere::Load(char const* IniPath, char const* Nickname)
    {
        // (thiscall, RET 8: ini path, nickname). Nothing is null-tested. The result
        // ([ESP+0x14]) starts at 2 (no matching [star]), becomes 1 when the ini does not open and
        // 0 once the nickname matches. The four "already loaded" bytes [ESP+0x10..0x13] start 0.
        int Result = 2;
        unsigned char bLensFlare = 0; // [ESP+0x10]
        unsigned char bLensGlow = 0;  // [ESP+0x11]
        unsigned char bStarGlow = 0;  // [ESP+0x12]
        unsigned char bSpines = 0;    // [ESP+0x13]

        INI_Reader Reader;
        if (!Reader.open(IniPath, false))
        {
            // the failed open still falls through to the routine.
            Result = 1;
        }
        else
        {
            // headers until a "star" (the field) whose values hold a
            // "nickname" equal (NEURONCOMPONENTS stricmp) to ours; a different nickname goes back to the
            // headers, and so does running out of values.
            bool bFound = false;
            while (!bFound && Reader.read_header())
            {
                if (!Reader.is_header("star"))
                    continue;
                while (Reader.read_value())
                {
                    if (!Reader.is_value("nickname"))
                        continue;
                    bFound = (_stricmp(Nickname, Reader.get_value_string()) == 0);
                    break;
                }
            }

            if (bFound)
            {
                // result 0, then every value of that section.
                Result = 0;
                while (Reader.read_value())
                {
                    if (Reader.is_value("radius"))
                    {
                        this->Radius = Reader.get_value_float(0);
                    }
                    else if (bStarGlow == 0 && Reader.is_value("star_glow"))
                    {
                        // the routine on +0x10; loaded when it returns 0.
                        bStarGlow = static_cast<unsigned char>(
                            StarGlowLoadFromIni(this->StarGlow, IniPath, Reader.get_value_string()) == 0);
                    }
                    else if (Reader.is_value("star_center"))
                    {
                        // the old +0x0c freed when set, a new 0x50 record
                        // (the inline ctor, or 0 when new fails) into +0x0c, the routine on
                        // whatever +0x0c holds; a non-zero result frees it and zeroes +0x0c.
                        if (this->StarCenter != nullptr)
                            operator delete(this->StarCenter);
                        FStarGlowDesc* Center = static_cast<FStarGlowDesc*>(operator new(0x50));
                        if (Center != nullptr)
                            InitGlowDescDefaults(Center, 0);
                        this->StarCenter = Center;
                        if (StarGlowLoadFromIni(this->StarCenter, IniPath, Reader.get_value_string()) != 0)
                        {
                            operator delete(this->StarCenter);
                            this->StarCenter = nullptr;
                        }
                    }
                    else if (bLensFlare == 0 && Reader.is_value("lens_flare"))
                    {
                        // the routine on +0x18.
                        bLensFlare = static_cast<unsigned char>(
                            LensFlareLoadFromIni(this->LensFlare, IniPath, Reader.get_value_string()) == 0);
                    }
                    else if (bLensGlow == 0 && Reader.is_value("lens_glow"))
                    {
                        // the routine on +0x14.
                        bLensGlow = static_cast<unsigned char>(
                            LensGlowLoadFromIni(this->SunBody, IniPath, Reader.get_value_string()) == 0);
                    }
                    else if (bSpines == 0 && Reader.is_value("spines"))
                    {
                        // the routine on +0x1c.
                        bSpines = static_cast<unsigned char>(
                            SpinesLoadFromIni(this->Spines, IniPath, Reader.get_value_string()) == 0);
                    }
                    else if (Reader.is_value("intensity_fade_in"))
                    {
                        // FST +0x20, FCOMP double 0.0 (the field),
                        // TEST AH,0x44 / JP skips unless equal; equal -> 1e-7f (0x33d6bf95).
                        this->IntensityFadeIn = Reader.get_value_float(0);
                        if (this->IntensityFadeIn == 0.0f)
                            this->IntensityFadeIn = kTinyFade;
                    }
                    else if (Reader.is_value("intensity_fade_out"))
                    {
                        // the same on +0x24.
                        this->IntensityFadeOut = Reader.get_value_float(0);
                        if (this->IntensityFadeOut == 0.0f)
                            this->IntensityFadeOut = kTinyFade;
                    }
                    else if (Reader.is_value("zone_occlusion_fade_in"))
                    {
                        // baseline stores the value into +0x20 (FSTP
                        // [EDI+0x20], the intensity fade-in), then tests +0x28 for 0 and only
                        // then writes 1e-7f to +0x28.
                        this->IntensityFadeIn = Reader.get_value_float(0);
                        if (this->ZoneOcclusionFadeIn == 0.0f)
                            this->ZoneOcclusionFadeIn = kTinyFade;
                    }
                    else if (Reader.is_value("zone_occlusion_fade_out"))
                    {
                        // likewise into +0x24, testing +0x2c.
                        this->IntensityFadeOut = Reader.get_value_float(0);
                        if (this->ZoneOcclusionFadeOut == 0.0f)
                            this->ZoneOcclusionFadeOut = kTinyFade;
                    }
                }
            }
        }

        // the routine on +0x14 on every path; then the reader's
        // destructor (0x005113f9..0x00511663, inlined).
        SunBodyBakeCorona(this->SunBody);
        return Result;
    }

    void FStarSphere::UpdateIntensity(float Dt)
    {
        // — RET 4: single stack arg is Dt (stored at +0x60). No `this` test.
        this->Dt = Dt;
        float const Target = 1.0f - this->Field38;
        this->Intensity = ApproachFloat(this->Intensity, Target, Dt / this->IntensityFadeIn,
                                        Dt / this->IntensityFadeOut);
        // StarGlow (+0x10), read without a null test (the routine).
        ResolveGlowDescShape(this->StarGlow);
        // then StarCenter (+0x0c), only when it exists (JZ 0x005117c5;
        // the inlined the routine / the routine find on the same DB).
        if (this->StarCenter != nullptr)
            ResolveGlowDescShape(this->StarCenter);
    }

    void FStarSphere::DrawBillboard(void* Cam, float A, float B)
    {
        // (thiscall, RET 0xc: Cam, A, B). No option gates and no null tests in
        // baseline: only the the field byte.
        if (G_StarBillboardEnabled == 0)
            return;

        Win3DDisable(kWin3DCullFace);  // (0xb44)
        Win3DDisable(kWin3DDepthTest); // (0xb71)
        Win3DSetDepthWrite(0);         // (0)

        // the camera's near / far (slots +0x20 / +0x24), the far plane
        // pushed to 250000 (the routine with the field) and the camera applied (the routine).
        FSpaceCamera* const SpaceCam = static_cast<FSpaceCamera*>(Cam);
        ICamera* const Camera = AsCamera(SpaceCam);
        float const SavedNear = Camera->GetZNear();
        float const SavedFar = Camera->GetZFar();
        SpaceCam->ZNear = SavedNear;
        SpaceCam->ZFar = 250000.0f;
        Win3DApplyCamera(Camera);

        // the owner's position (+0x04, slot 0).
        float const* const OwnerPos = (float const*)&this->Owner->get_position();
        float Pos[3] = {OwnerPos[0], OwnerPos[1], OwnerPos[2]};

        // the unit direction from the camera (slot +0x0c). The z
        // difference is stored as a float (FST) and squared against its stored copy; the others
        // stay on the FPU.
        float const* const CamPos = Camera->GetPosition();
        double const Dx = static_cast<double>(Pos[0]) - CamPos[0];
        double const Dy = static_cast<double>(Pos[1]) - CamPos[1];
        double const DzExt = static_cast<double>(Pos[2]) - CamPos[2];
        float const DzF = static_cast<float>(DzExt);
        double const SumSq = DzExt * DzF + Dy * Dy + Dx * Dx;
        float const InvLen = static_cast<float>(1.0 / sqrt(SumSq));
        float const DirX = static_cast<float>(Dx * InvLen);
        float const DirY = static_cast<float>(Dy * InvLen);
        float const DirZ = static_cast<float>(static_cast<double>(DzF) * InvLen);

        // R = Dir x (m[1], m[4], m[7]) of the orientation (slot +0x10),
        // normalised; the z term stays on the FPU through the length.
        float const* const M = Camera->GetOrientation();
        double const M1 = M[1];
        double const M4 = M[4];
        double const M7 = M[7];
        float const RxRaw = static_cast<float>(DirY * M7 - M4 * DirZ);
        float const RyRaw = static_cast<float>(DirZ * M1 - M7 * DirX);
        double const RzExt = M4 * DirX - DirY * M1;
        double const SumR = RzExt * RzExt + static_cast<double>(RyRaw) * RyRaw +
                            static_cast<double>(RxRaw) * RxRaw;
        double const InvR = 1.0 / sqrt(SumR);
        float Right[3];
        Right[0] = static_cast<float>(RxRaw * InvR);
        Right[1] = static_cast<float>(RyRaw * InvR);
        Right[2] = static_cast<float>(RzExt * InvR);

        // U = R x Dir.
        float Up[3];
        Up[0] = static_cast<float>(static_cast<double>(Right[1]) * DirZ -
                                   static_cast<double>(Right[2]) * DirY);
        Up[1] = static_cast<float>(static_cast<double>(Right[2]) * DirX -
                                   static_cast<double>(DirZ) * Right[0]);
        Up[2] = static_cast<float>(static_cast<double>(DirY) * Right[0] -
                                   static_cast<double>(Right[1]) * DirX);

        // the quad scale is Radius * B.
        float const Scale = static_cast<float>(static_cast<double>(this->Radius) * B);

        // the routine on the star_glow (+0x10) with (Pos, Scale, U, R, A)
        // -- U is its 2nd vector argument (params 5..7), R its 3rd (params 8..10), which is the
        // port's (Right = R, Up = U) naming. No null test on +0x10.
        StarGlowDrawBillboard(this->StarGlow, Pos, Scale, Right, Up, A);
        // the star_center (+0x0c) when present, with A * A.
        if (this->StarCenter != nullptr)
        {
            float const AlphaSq = static_cast<float>(static_cast<double>(A) * A);
            StarGlowDrawBillboard(this->StarCenter, Pos, Scale, Right, Up, AlphaSq);
        }

#ifdef OUTPOST_FRONTIER_DX9RENDERER
        // Port-only: the Direct3D 9 volumetric nebula composites after this sprite, which has no
        // depth; it is told where the glow is (nebula_volume.cpp). A no-op while inactive.
        if (this->StarGlow->Field48 != 0)
        {
            float GlowScale = this->StarGlow->Scale;
            if (this->StarCenter != nullptr && this->StarCenter->Scale > GlowScale)
                GlowScale = this->StarCenter->Scale;
            NebulaVolume::NoteStarGlow(Pos, Scale * GlowScale, A);
        }
#endif

        // near / far back, the camera applied, then the states back.
        SpaceCam->ZNear = SavedNear;
        SpaceCam->ZFar = SavedFar;
        Win3DApplyCamera(Camera);
        Win3DSetDepthWrite(1);        // (1)
        Win3DEnable(kWin3DDepthTest); // (0xb71)
        Win3DEnable(kWin3DCullFace);  // (0xb44)
    }

    void FStarSphere::DrawSimple(void* Cam)
    {
        // (RET 4): vt[+0x18](Cam, 1.0f, 1.0f), no tests.
        DrawBillboard(Cam, 1.0f, 1.0f);
    }

    // (vtable slot 4, __thiscall, RET 8). Everything below is traced from the
    // listing; baseline tests neither the sphere, the camera, the owner, SunBody nor Spines. The
    // step it uses is the Dt stored at +0x60 by UpdateIntensity; its own second stack argument
    // (declared here as Dt) is never read.
    void FStarSphere::Draw(void* Cam, float Dt)
    {
        (void)Dt;
        ICamera* const Camera = static_cast<ICamera*>(Cam);

        // the routine with 0x25 ([EFFECTS]), 0x24 (SUN) and
        // 0x28 (SUN_EFFECTS), in that order; any zero returns.
        if (GetGameOption(kGameOptEffectsGroup) == 0)
            return;
        if (GetGameOption(kGameOptSun) == 0)
            return;
        if (GetGameOption(kGameOptSunEffects) == 0)
            return;
        // the field FCOMP 0.0f (the field), TEST AH,0x41 / JNP
        // returns on exactly one of C0/C3 (less or equal); greater or unordered go on.
        if (G_SunEffectQuality <= 0.0f)
            return;

        // zone occlusion +0x34 approaches the routine's factor. Up
        // (TEST AH,0x41 / JNZ fall-through: target > current) steps Dt/+0x2c and clamps when
        // the sum > target; down (TEST AH,5 / JP: only an ordered target < current) steps
        // Dt/+0x28 and clamps when the difference < target. The sum is compared unrounded.
        double const ZoneTarget = StarZoneOcclusionFactor(this, Cam);
        if (ZoneTarget > this->ZoneOcclusion)
        {
            double const Next =
                static_cast<double>(this->Dt) / this->ZoneOcclusionFadeOut + this->ZoneOcclusion;
            this->ZoneOcclusion = static_cast<float>(Next);
            if (Next > ZoneTarget)
                this->ZoneOcclusion = static_cast<float>(ZoneTarget);
        }
        else if (ZoneTarget < this->ZoneOcclusion)
        {
            double const Next =
                this->ZoneOcclusion - static_cast<double>(this->Dt) / this->ZoneOcclusionFadeIn;
            this->ZoneOcclusion = static_cast<float>(Next);
            if (Next < ZoneTarget)
                this->ZoneOcclusion = static_cast<float>(ZoneTarget);
        }
        // FCOMP 1.0f, TEST AH,5 / JP -- only an ordered +0x34 < 1 goes on.
        if (!(this->ZoneOcclusion < 1.0f))
            return;

        // the owner's slot 0 position copied into a local ([ESP+0x34]),
        // the radius +0x08 into another ([ESP+0x24]); the field = 0.
        float const* const OwnerPos = OwnerGetWorldPos(this->Owner);
        FVector LocalPos;
        LocalPos.X = OwnerPos[0];
        LocalPos.Y = OwnerPos[1];
        LocalPos.Z = OwnerPos[2];
        float const Radius = this->Radius;
        G_SunRayQueryCount = 0;

        // +0x68 = the local through the camera's view transform
        // (slot 16, +0x40) by the lazily acquired math engine the field slot +0x28.
        FTransform const* const View =
            reinterpret_cast<FTransform const*>(Camera->GetViewTransform());
        if (G_SunRayMath == nullptr)
            G_SunRayMath = Acquire3DMathEngine();
        FVector ViewPos;
        G_SunRayMath->TransformXform(ViewPos, *View, LocalPos);
        this->WorldPos[0] = ViewPos.X;
        this->WorldPos[1] = ViewPos.Y;
        this->WorldPos[2] = ViewPos.Z;

        // +0x38 = float(1.0 (double the field) - the routine(+0x14)).
        this->Field38 =
            static_cast<float>(1.0 - static_cast<double>(SunBodyComputeCoronaFactor(this->SunBody)));

        // camera slot 13 Project(&+0x7c, &+0x80, &+0x78, &local) -- the
        // untransformed local, not +0x68. TEST AL,AL.
        FSunEffectBody* const Body = this->SunBody;
        if (static_cast<unsigned char>(Camera->Project(&this->ScreenX, &this->ScreenY,
                                                       &this->ScreenDepth, &LocalPos.X)) == 0)
        {
            // off screen. Only an ordered +0x38 < 1 goes on; then every
            // corona slot goes through the routine(1.0, Dt/+0x2e8, Dt/+0x2ec, 1.0), both steps
            // spilled to float, and the Draw returns.
            if (!(this->Field38 < 1.0f))
                return;
            float const StepUp = this->Dt / *reinterpret_cast<float*>(Body->Raw + 0x2e8);
            float const StepDown = this->Dt / *reinterpret_cast<float*>(Body->Raw + 0x2ec);
            unsigned char* Slot = Body->Raw;
            for (int I = 0; I < 13; ++I)
            {
                CoronaSlotApproachAlpha(Slot, 1.0f, StepUp, StepDown, 1.0f);
                Slot += 0x18;
            }
            Slot = Body->Raw + 0x138;
            for (int I = 0; I < 12; ++I)
            {
                CoronaSlotApproachAlpha(Slot, 1.0f, StepUp, StepDown, 1.0f);
                Slot += 0x18;
            }
            return;
        }

        // +0x84 = 0; depth < 0.1f (the field; TEST AH,5 / JP skips
        // on >=, unordered) becomes 0.1f.
        this->ScreenZFlat = 0.0f;
        if (this->ScreenDepth < 0.1f)
            this->ScreenDepth = 0.1f;

        // +0x88 = (slot 8 ZNear * float slot 11 XScaleOverNear) *
        // (radius / depth); slot 11 is called first.
        float const XScale = static_cast<float>(Camera->GetXScaleOverNear()); // FSTP float
        double const ZNear = Camera->GetZNear();
        this->PixelScale = static_cast<float>(
            (ZNear * XScale) * (static_cast<double>(Radius) / this->ScreenDepth));

        // viewport (slot 5) W = r - l + 1, H = b - t + 1;
        // (l, r, b, t, -1.0f, 0.0f).
        int const* const Vp = Camera->GetViewport();
        int const Width = Vp[2] - Vp[0] + 1;
        int const Height = Vp[3] - Vp[1] + 1;
        Win3DSetProjection(static_cast<float>(Vp[0]), static_cast<float>(Vp[2]),
                           static_cast<float>(Vp[3]), static_cast<float>(Vp[1]), -1.0f, 0.0f);

        // view z FCOMP 0.0f, TEST AH,1 / JZ returns when C0 is clear
        // (z >= 0); less or unordered go on.
        if (this->WorldPos[2] >= 0.0f)
            return;

        float const ScreenX = this->ScreenX;
        float const ScreenY = this->ScreenY;

        // the body's box. R = 2 * Body+0x2ac * +0x88 (stored as float;
        // the compare against 50.0f the field uses the unrounded value, and only an ordered
        // R < 50 becomes 50). On screen = X > -R, W + R > X, Y > -R, H + R > Y, each an
        // ordered "greater" (TEST AH,0x41 / JNZ).
        double const BodyExtent =
            static_cast<double>(*reinterpret_cast<float*>(Body->Raw + 0x2ac)) * this->PixelScale * 2.0;
        float BodyR = static_cast<float>(BodyExtent);
        if (BodyExtent < 50.0)
            BodyR = 50.0f;
        float const BodyNegR = -BodyR;
        bool const bBodyOnScreen = ScreenX > BodyNegR &&
                                   static_cast<double>(Width) + BodyR > ScreenX &&
                                   ScreenY > BodyNegR &&
                                   static_cast<double>(Height) + BodyR > ScreenY;

        // the largest +0x08 over the Spines (+0x1c) element array
        // (begin +0x04, end +0x08, stride 0x2c), walked while begin is non-null and the index
        // is below the element count. A new value replaces the running maximum unless the
        // maximum is strictly greater (TEST AH,0x41 / JZ), so NaN replaces it.
        unsigned char* const SpinesRaw = this->Spines->Raw;
        double SpinesMax = 0.0;
        for (unsigned Index = 0;; ++Index)
        {
            unsigned char* const Begin = *reinterpret_cast<unsigned char**>(SpinesRaw + 0x04);
            if (Begin == nullptr)
                break;
            unsigned char* const End = *reinterpret_cast<unsigned char**>(SpinesRaw + 0x08);
            int const Count = static_cast<int>((End - Begin) / 0x2c);
            if (Index >= static_cast<unsigned>(Count))
                break;
            double const Value = *reinterpret_cast<float*>(Begin + Index * 0x2c + 0x08);
            if (!(SpinesMax > Value))
                SpinesMax = Value;
        }

        // the same four tests with R2 = float(max * +0x88).
        float const SpinesR = static_cast<float>(SpinesMax * this->PixelScale);
        float const SpinesNegR = -SpinesR;
        bool const bSpinesOnScreen = ScreenX > SpinesNegR &&
                                     static_cast<double>(Width) + SpinesR > ScreenX &&
                                     ScreenY > SpinesNegR &&
                                     static_cast<double>(Height) + SpinesR > ScreenY;

        if (bBodyOnScreen)
        {
            // the routine(Cam, this, +0x60).
            SunBodyUpdateOcclusion(Body, Cam, this, this->Dt);
        }
        else if (!bSpinesOnScreen)
        {
            // neither on screen. Only an ordered +0x38 < 1 goes on;
            // the corona slots fade toward 0 with 5.0f (the field) / +0x2e8 up and
            // 5.0f / +0x2ec down, both kept on the FPU, and the Draw returns without drawing.
            if (!(this->Field38 < 1.0f))
                return;
            double const StepUp = 5.0 / *reinterpret_cast<float*>(Body->Raw + 0x2e8);
            double const StepDown = 5.0 / *reinterpret_cast<float*>(Body->Raw + 0x2ec);
            FadeCoronaSlotsToZero(Body, StepUp, StepDown);
            return;
        }
        else
        {
            // only the spines on screen. When +0x38 < 1 (ordered) the
            // same fade with Dt/+0x2e8 (spilled to float at [ESP+0x24]) up and Dt/+0x2ec
            // (kept on the FPU) down; then the draw.
            if (this->Field38 < 1.0f)
            {
                float const StepUp = this->Dt / *reinterpret_cast<float*>(Body->Raw + 0x2e8);
                double const StepDown =
                    static_cast<double>(this->Dt) / *reinterpret_cast<float*>(Body->Raw + 0x2ec);
                FadeCoronaSlotsToZero(Body, StepUp, StepDown);
            }
        }

        // the routine(0xbe2), the routine(0xb44), the routine(0xb71).
        Win3DEnable(kWin3DBlend);
        Win3DDisable(kWin3DCullFace);
        Win3DDisable(kWin3DDepthTest);

        // spines on screen and the field -> the routine(Cam, this,
        // 1 - +0x34).
        if (bSpinesOnScreen && G_StarSpinesEnabled != 0)
            SpinesDraw(this->Spines, Cam, this, 1.0f - this->ZoneOcclusion);
        // body on screen and the field -> the routine(Cam, this,
        // 1 - +0x34). The port's SunBodyDrawCorona takes no camera argument.
        if (bBodyOnScreen && G_StarCoronaDrawEnabled != 0)
            SunBodyDrawCorona(Body, this, 1.0f - this->ZoneOcclusion);

        // the routine(0xb71), the routine(0xb44),
        // (0xbe2), the routine(Cam).
        Win3DEnable(kWin3DDepthTest);
        Win3DEnable(kWin3DCullFace);
        Win3DDisable(kWin3DBlend);
        Win3DApplyCamera(Camera);
    }

    void FStarSphere::DrawFlare(void* Cam, float Dt)
    {
        // (thiscall, RET 8: camera, dt) -- quality and occlusion gates, then the
        // Win3D ortho box and the routine. No test of `this`, the camera or the flare child.
        // the field FCOMP 0.0f, TEST AH,0x41 / JNP -- <= 0 returns
        // (a NaN quality goes on).
        if (G_SunEffectQuality <= 0.0f)
            return;
        // the dt argument becomes float((1.0 - +0x34) * dt); only a
        // product with +0x64 strictly above 0 (TEST AH,0x41 / JNZ) goes on. The stored value
        // (without +0x64) is what the flare receives.
        double const FadedExt = (1.0 - this->ZoneOcclusion) * Dt;
        float const Faded = static_cast<float>(FadedExt);
        if (!(FadedExt * this->Intensity > 0.0))
            return;
        // +0x70 FCOMP 0.0f, TEST AH,5 / JP -- only an ordered "less"
        // goes on.
        if (!(this->WorldPos[2] < 0.0f))
            return;

        // Enable 0xbe2, blend (5, 2), Disable 0xb44, Disable 0xb71.
        Win3DEnable(kWin3DBlend);
        Win3DSetBlendFunc(kWin3DBlendSrcAlpha, kWin3DBlendOne);
        Win3DDisable(kWin3DCullFace);
        Win3DDisable(kWin3DDepthTest);

        // the camera's viewport rect (vt+0x14) into the routine(left,
        // right, bottom, top, -1.0f, 0).
        ICamera* const Camera = static_cast<ICamera*>(Cam);
        int const* const Vp = Camera->GetViewport();
        Win3DSetProjection(static_cast<float>(Vp[0]), static_cast<float>(Vp[2]),
                           static_cast<float>(Vp[3]), static_cast<float>(Vp[1]), -1.0f, 0.0f);

        // the field -> the routine on +0x18 (camera, this, faded).
        if (G_StarLensFlareEnabled != 0)
            LensFlareDraw(this->LensFlare, Camera, this, Faded);

        // Enable 0xb71, Enable 0xb44, Disable 0xbe2, the routine(camera).
        Win3DEnable(kWin3DDepthTest);
        Win3DEnable(kWin3DCullFace);
        Win3DDisable(kWin3DBlend);
        Win3DApplyCamera(Camera);
    }

// -------------------------------------------------------------------- the routine

void Win3DIdentityRotation3x3(float* Out9)
{
    if (Out9 == nullptr)
        return;
    Out9[0] = 1.0f;
    Out9[1] = 0.0f;
    Out9[2] = 0.0f;
    Out9[3] = 0.0f;
    Out9[4] = 1.0f;
    Out9[5] = 0.0f;
    Out9[6] = 0.0f;
    Out9[7] = 0.0f;
    Out9[8] = 1.0f;
}

// -------------------------------------------------------------------- the routine

namespace
{
    // the routine's own lazily acquired 3DMathEngine (the routine at 004226b0).
    I3DMathEngine* G_Win3DModelviewMath = nullptr;
}

void Win3DBindModelview(void* ObjectOrNull, float const* Matrix12)
{
    // (cdecl). ObjectOrNull is a camera-shaped object: its vt+0x40 is the
    // __stdcall view transform getter (PUSH EBX; CALL [EAX+0x40] at 004226a1 / 00422731).

    FTransform Local;
    float const* Src = Matrix12;
#ifdef OUTPOST_FRONTIER_WIDESCREEN_SUPPORT
    // Port-only: without a camera the matrix is the modelview as it stands (the Win3D camera
    // is the identity), so a shifted HUD window's shear goes onto it here.
    float Sheared[12];
    if (ObjectOrNull == nullptr && WidescreenHudShearModelview(Matrix12, Sheared))
        Src = Sheared;
#endif
    if (ObjectOrNull != nullptr)
    {
        // Local = MulTransform(view, Matrix12) through vt+0x1c.
        FTransform const* const View =
            (FTransform const*)((ICamera*)ObjectOrNull)->GetViewTransform();
        if (G_Win3DModelviewMath == nullptr)
            G_Win3DModelviewMath = Acquire3DMathEngine();
        G_Win3DModelviewMath->MulTransform(Local, *View,
                                           *reinterpret_cast<FTransform const*>(Matrix12));
        Src = reinterpret_cast<float const*>(&Local);
    }

    // 12 dwords into the field.
    memcpy(GWin3DModelview, Src, sizeof(float) * 12);

    // GetPipelineState(7), out preset 1.
    IRenderPipeline* const Pipe = NeuronRuntime::Pipeline;
    int Viewspace = 1;
    Pipe->GetPipelineState(kPipelineStateViewspace, &Viewspace);
    if (Viewspace != 0)
    {
        Pipe->SetModelview(GWin3DModelview); // vt+0x7c
        return;
    }

    if (ObjectOrNull != nullptr)
    {
        // SetView (vt+0x74) with a second vt+0x40 call.
        FTransform const* const View =
            (FTransform const*)((ICamera*)ObjectOrNull)->GetViewTransform();
        Pipe->SetView(reinterpret_cast<float const*>(View));
    }
    else
    {
        // identity rotation, zero translation.
        float Ident[12];
        Win3DIdentityRotation3x3(Ident);
        Ident[9] = 0.0f;
        Ident[10] = 0.0f;
        Ident[11] = 0.0f;
        Pipe->SetView(Ident);
    }
    // SetWorld (vt+0x6c) with the caller's Matrix12, not the product.
#ifdef OUTPOST_FRONTIER_WIDESCREEN_SUPPORT
    Pipe->SetWorld(ObjectOrNull == nullptr ? Src : Matrix12);
#else
    Pipe->SetWorld(Matrix12);
#endif
}

// -------------------------------------------------------------------- the routine

FSunEffectBody* SunEffectBodyInit(FSunEffectBody* Body)
{
    if (Body == nullptr)
        return nullptr;
    unsigned* Words = reinterpret_cast<unsigned*>(Body->Raw);
    // First loop: 13 × 6-float corona slots starting at +0 (stride writes at +8 centred).
    unsigned* Pu = Words + 2; // param_1 + 8
    for (int I = 0; I < 13; ++I)
    {
        Pu[1] = 0;
        Pu[0] = 0;
        Pu[-1] = 0;
        Pu[-2] = 0;
        Pu[2] = 0x3f800000u; // 1.0f
        Pu += 6;
    }
    // Second loop at +0x138, 12 iterations.
    Pu = Words + (0x138 / 4);
    for (int I = 0; I < 12; ++I)
    {
        Pu[3] = 0;
        Pu[2] = 0;
        Pu[1] = 0;
        Pu[0] = 0;
        Pu[4] = 0x3f800000u;
        Pu += 6;
    }
    // +600 (0x258): twelve uint 1s.
    Pu = Words + (600 / 4);
    for (int I = 0; I < 12; ++I)
    {
        *Pu = 1;
        ++Pu;
    }
    *reinterpret_cast<unsigned*>(Body->Raw + 700) = 0;
    Body->Raw[0x2c0] = 0;
    *reinterpret_cast<float*>(Body->Raw + 0x288) = 1.0f;
    *reinterpret_cast<float*>(Body->Raw + 0x28c) = 1.0f;
    *reinterpret_cast<float*>(Body->Raw + 0x290) = 1.0f;
    *reinterpret_cast<float*>(Body->Raw + 0x294) = 1.0f;
    *reinterpret_cast<float*>(Body->Raw + 0x298) = 1.0f;
    *reinterpret_cast<float*>(Body->Raw + 0x29c) = 1.0f;
    *reinterpret_cast<float*>(Body->Raw + 0x2a0) = 1.0f;
    *reinterpret_cast<float*>(Body->Raw + 0x2a4) = 1.0f;
    *reinterpret_cast<float*>(Body->Raw + 0x2a8) = 1.0f;
    *reinterpret_cast<float*>(Body->Raw + 0x2f4) = 1.0f;
    *reinterpret_cast<float*>(Body->Raw + 0x2e8) = 0.25f;
    *reinterpret_cast<float*>(Body->Raw + 0x2ec) = 0.25f;
    Body->Raw[0x2e4] = 0;
    *reinterpret_cast<unsigned*>(Body->Raw + 0x2e0) = 0;
    *reinterpret_cast<unsigned*>(Body->Raw + 0x2b0) = 0;
    *reinterpret_cast<unsigned*>(Body->Raw + 0x2b8) = 0;
    *reinterpret_cast<float*>(Body->Raw + 0x2f0) = 0.5f;
    *reinterpret_cast<float*>(Body->Raw + 0x2ac) = 2.0f;
    *reinterpret_cast<unsigned*>(Body->Raw + 0x2b4) = 5;
    *reinterpret_cast<unsigned*>(Body->Raw + 0x2f8) = 8;
    *reinterpret_cast<unsigned*>(Body->Raw + 0x2fc) = 0xf;
    *reinterpret_cast<unsigned*>(Body->Raw + 0x300) = 0x19;
    *reinterpret_cast<unsigned*>(Body->Raw + 0x304) = 0x28;
    *reinterpret_cast<unsigned*>(Body->Raw + 0x308) = 0x3c;
    return Body;
}

// -------------------------------------------------------------------- the routine

int StarGlowLoadFromIni(FStarGlowDesc* Desc, char const* IniPath, char const* Nickname)
{
    if (Desc == nullptr || IniPath == nullptr || Nickname == nullptr)
        return 1;

    INI_Reader Reader;
    if (!Reader.open(IniPath, false))
        return 1;

    bool bFound = false;
    while (Reader.read_header())
    {
        if (!Reader.is_header("star_glow"))
            continue;
        bool bNick = false;
        while (Reader.read_value())
        {
            if (!Reader.is_value("nickname"))
                continue;
            char const* Nick = Reader.get_value_string();
            if (Nick != nullptr && _stricmp(Nick, Nickname) == 0)
                bNick = true;
            break;
        }
        if (!bNick)
            continue;
        bFound = true;
        while (Reader.read_value())
        {
            if (Reader.is_value("shape"))
            {
                char const* Shape = Reader.get_value_string();
                if (Shape == nullptr)
                {
                    Desc->ShapeLen = 0;
                    Desc->ShapeName[0] = '\0';
                }
                else
                {
                    Desc->ShapeLen = CopyBoundedString(Desc->ShapeName, 0x20, Shape);
                }
            }
            else if (Reader.is_value("scale"))
            {
                Desc->Scale = Reader.get_value_float(0);
            }
            else if (Reader.is_value("inner_color"))
            {
                float const G = Reader.get_value_float(1);
                float const R = Reader.get_value_float(0);
                float const B = Reader.get_value_float(2);
                Desc->InnerColor[0] = R;
                Desc->InnerColor[1] = G;
                Desc->InnerColor[2] = B;
            }
            else if (Reader.is_value("outer_color"))
            {
                float const G = Reader.get_value_float(1);
                float const R = Reader.get_value_float(0);
                float const B = Reader.get_value_float(2);
                Desc->OuterColor[0] = R;
                Desc->OuterColor[1] = G;
                Desc->OuterColor[2] = B;
            }
            else if (Reader.is_value("alpha"))
            {
                Desc->Alpha = Reader.get_value_float(0);
            }
        }
        break;
    }
    Reader.close();
    return bFound ? 0 : 1;
}

// -------------------------------------------------------------------- the routine

int LensGlowLoadFromIni(FSunEffectBody* Body, char const* IniPath, char const* Nickname)
{
    if (Body == nullptr || IniPath == nullptr || Nickname == nullptr)
        return 1;

    unsigned char* const Base = Body->Raw;
    INI_Reader Reader;
    if (!Reader.open(IniPath, false))
        return 1;

    bool bFound = false;
    while (Reader.read_header())
    {
        if (!Reader.is_header("lens_glow"))
            continue;
        bool bNick = false;
        while (Reader.read_value())
        {
            if (!Reader.is_value("nickname"))
                continue;
            char const* Nick = Reader.get_value_string();
            if (Nick != nullptr && _stricmp(Nick, Nickname) == 0)
                bNick = true;
            break;
        }
        if (!bNick)
            continue;
        bFound = true;
        while (Reader.read_value())
        {
            if (Reader.is_value("shape"))
            {
                char const* Shape = Reader.get_value_string();
                if (Shape == nullptr)
                {
                    *reinterpret_cast<unsigned*>(Base + 0x2bc) = 0;
                    Base[0x2c0] = 0;
                }
                else
                {
                    // into +0x2c0; length at +0x2bc.
                    unsigned const Len =
                        CopyBoundedString(reinterpret_cast<char*>(Base + 0x2c0), 0x20, Shape);
                    *reinterpret_cast<unsigned*>(Base + 0x2bc) = Len;
                }
            }
            else if (Reader.is_value("radius_scale"))
            {
                *reinterpret_cast<float*>(Base + 0x2ac) = Reader.get_value_float(0);
            }
            else if (Reader.is_value("inner_color"))
            {
                float const G = Reader.get_value_float(1);
                float const R = Reader.get_value_float(0);
                float const B = Reader.get_value_float(2);
                *reinterpret_cast<float*>(Base + 0x288) = R;
                *reinterpret_cast<float*>(Base + 0x28c) = G;
                *reinterpret_cast<float*>(Base + 0x290) = B;
            }
            else if (Reader.is_value("outer_color"))
            {
                float const G = Reader.get_value_float(1);
                float const R = Reader.get_value_float(0);
                float const B = Reader.get_value_float(2);
                *reinterpret_cast<float*>(Base + 0x2a0) = R;
                *reinterpret_cast<float*>(Base + 0x2a4) = G;
                *reinterpret_cast<float*>(Base + 0x2a8) = B;
            }
            else if (Reader.is_value("glow_fade_in_seconds"))
            {
                *reinterpret_cast<float*>(Base + 0x2e8) = Reader.get_value_float(0);
            }
            else if (Reader.is_value("glow_fade_out_seconds"))
            {
                *reinterpret_cast<float*>(Base + 0x2ec) = Reader.get_value_float(0);
            }
            else if (Reader.is_value("interior_tween"))
            {
                *reinterpret_cast<float*>(Base + 0x2f0) = Reader.get_value_float(0);
            }
            else if (Reader.is_value("interior_ray_tween"))
            {
                *reinterpret_cast<float*>(Base + 0x2f4) = Reader.get_value_float(0);
            }
            else if (Reader.is_value("max_LOD"))
            {
                *reinterpret_cast<unsigned*>(Base + 0x2b4) =
                    static_cast<unsigned>(Reader.get_value_float(0));
            }
        }
        break;
    }
    Reader.close();

    if (bFound)
    {
        // Post-parse lerp of blended colour at +0x294..+0x29c (listing 0050d6f7).
        float const T = *reinterpret_cast<float*>(Base + 0x2f0);
        float const OneMinus = 1.0f - T;
        *reinterpret_cast<float*>(Base + 0x294) =
            OneMinus * *reinterpret_cast<float*>(Base + 0x288) +
            T * *reinterpret_cast<float*>(Base + 0x2a0);
        *reinterpret_cast<float*>(Base + 0x298) =
            OneMinus * *reinterpret_cast<float*>(Base + 0x28c) +
            T * *reinterpret_cast<float*>(Base + 0x2a4);
        *reinterpret_cast<float*>(Base + 0x29c) =
            OneMinus * *reinterpret_cast<float*>(Base + 0x290) +
            T * *reinterpret_cast<float*>(Base + 0x2a8);
    }
    return bFound ? 0 : 1;
}

// -------------------------------------------------------------------- the routine / 0050fb50

static void LensFlareFree(FStarFxChild40* Flare)
{
    // — free bead pointers then the pointer array.
    if (Flare == nullptr)
        return;
    unsigned char* const Base = Flare->Raw;
    float** Begin = *reinterpret_cast<float***>(Base + 0x04);
    float** End = *reinterpret_cast<float***>(Base + 0x08);
    if (Begin != nullptr)
    {
        for (float** It = Begin; It != End; ++It)
        {
            if (*It != nullptr)
                operator delete(*It);
        }
        operator delete(Begin);
    }
    *reinterpret_cast<void**>(Base + 0x04) = nullptr;
    *reinterpret_cast<void**>(Base + 0x08) = nullptr;
    *reinterpret_cast<void**>(Base + 0x0c) = nullptr;
    *reinterpret_cast<void**>(Base + 0x34) = nullptr;
}

static void SpinesFree(FStarFxChild40* Spines)
{
    // — contiguous 0x2c array; reset end=begin, clear shape.
    if (Spines == nullptr)
        return;
    unsigned char* const Base = Spines->Raw;
    *reinterpret_cast<void**>(Base + 0x08) = *reinterpret_cast<void**>(Base + 0x04);
    *reinterpret_cast<void**>(Base + 0x34) = nullptr;
    *reinterpret_cast<unsigned*>(Base + 0x10) = 0;
    Base[0x14] = 0;
    void* const Begin = *reinterpret_cast<void**>(Base + 0x04);
    if (Begin != nullptr)
        operator delete(Begin);
    *reinterpret_cast<void**>(Base + 0x04) = nullptr;
    *reinterpret_cast<void**>(Base + 0x08) = nullptr;
    *reinterpret_cast<void**>(Base + 0x0c) = nullptr;
}

static void FlarePushBead(FStarFxChild40* Flare, float const Values[6])
{
    // — push float[6]* into [+4,+8) pointer vector.
    unsigned char* const Base = Flare->Raw;
    float** Begin = *reinterpret_cast<float***>(Base + 0x04);
    float** End = *reinterpret_cast<float***>(Base + 0x08);
    float** Cap = *reinterpret_cast<float***>(Base + 0x0c);
    if (End == Cap)
    {
        int const OldCount = (Begin != nullptr) ? static_cast<int>(End - Begin) : 0;
        int const NewCap = OldCount > 0 ? OldCount * 2 : 4;
        float** NewBegin = static_cast<float**>(operator new(sizeof(float*) * NewCap));
        if (NewBegin == nullptr)
            return;
        for (int I = 0; I < OldCount; ++I)
            NewBegin[I] = Begin[I];
        if (Begin != nullptr)
            operator delete(Begin);
        Begin = NewBegin;
        End = Begin + OldCount;
        Cap = Begin + NewCap;
        *reinterpret_cast<float***>(Base + 0x04) = Begin;
        *reinterpret_cast<float***>(Base + 0x0c) = Cap;
    }
    float* Elem = static_cast<float*>(operator new(sizeof(float) * 6));
    if (Elem == nullptr)
        return;
    for (int I = 0; I < 6; ++I)
        Elem[I] = Values[I];
    *End = Elem;
    ++End;
    *reinterpret_cast<float***>(Base + 0x08) = End;
}

static void SpinesPushElem(FStarFxChild40* Spines, float const Elem[11])
{
    // — append one 0x2c record.
    unsigned char* const Base = Spines->Raw;
    unsigned char* Begin = *reinterpret_cast<unsigned char**>(Base + 0x04);
    unsigned char* End = *reinterpret_cast<unsigned char**>(Base + 0x08);
    unsigned char* Cap = *reinterpret_cast<unsigned char**>(Base + 0x0c);
    int const Stride = 0x2c;
    if (End == Cap)
    {
        int const OldBytes = (Begin != nullptr) ? static_cast<int>(End - Begin) : 0;
        int const OldCount = OldBytes / Stride;
        int const NewCap = OldCount > 0 ? OldCount * 2 : 4;
        unsigned char* NewBegin =
            static_cast<unsigned char*>(operator new(static_cast<size_t>(NewCap * Stride)));
        if (NewBegin == nullptr)
            return;
        if (Begin != nullptr && OldBytes > 0)
            memcpy(NewBegin, Begin, static_cast<size_t>(OldBytes));
        if (Begin != nullptr)
            operator delete(Begin);
        Begin = NewBegin;
        End = Begin + OldCount * Stride;
        Cap = Begin + NewCap * Stride;
        *reinterpret_cast<unsigned char**>(Base + 0x04) = Begin;
        *reinterpret_cast<unsigned char**>(Base + 0x0c) = Cap;
    }
    memcpy(End, Elem, Stride);
    End += Stride;
    *reinterpret_cast<unsigned char**>(Base + 0x08) = End;
}

// -------------------------------------------------------------------- the routine

int LensFlareLoadFromIni(FStarFxChild40* Flare, char const* IniPath, char const* Nickname)
{
    if (Flare == nullptr || IniPath == nullptr || Nickname == nullptr)
        return 1;

    LensFlareFree(Flare);
    unsigned char* const Base = Flare->Raw;

    INI_Reader Reader;
    if (!Reader.open(IniPath, false))
        return 1;

    bool bFound = false;
    while (Reader.read_header())
    {
        if (!Reader.is_header("lens_flare"))
            continue;
        bool bNick = false;
        while (Reader.read_value())
        {
            if (!Reader.is_value("nickname"))
                continue;
            char const* Nick = Reader.get_value_string();
            if (Nick != nullptr && _stricmp(Nick, Nickname) == 0)
                bNick = true;
            break;
        }
        if (!bNick)
            continue;
        bFound = true;
        while (Reader.read_value())
        {
            if (Reader.is_value("bead"))
            {
                float Values[6];
                for (int I = 0; I < 6; ++I)
                    Values[I] = Reader.get_value_float(I);
                FlarePushBead(Flare, Values);
            }
            else if (Reader.is_value("min_radius"))
            {
                *reinterpret_cast<float*>(Base + 0x38) = Reader.get_value_float(0);
            }
            else if (Reader.is_value("max_radius"))
            {
                *reinterpret_cast<float*>(Base + 0x3c) = Reader.get_value_float(0);
            }
            else if (Reader.is_value("shape"))
            {
                char const* Shape = Reader.get_value_string();
                if (Shape == nullptr)
                {
                    *reinterpret_cast<unsigned*>(Base + 0x10) = 0;
                    Base[0x14] = 0;
                }
                else
                {
                    // inlines the routine here (0050f588..0050f5bf: REPNE SCASB,
                    // clamp to 0x1f, MOVSD/MOVSB, terminator, length to +0x10) - same semantics.
                    unsigned const Len =
                        CopyBoundedString(reinterpret_cast<char*>(Base + 0x14), 0x20, Shape);
                    *reinterpret_cast<unsigned*>(Base + 0x10) = Len;
                }
            }
        }
        break;
    }
    Reader.close();
    // / 00512a40 / 00512b70 — sort bead pointer vector by pointer address
    // (MSVC insertion/introsort on uint*). Count ≤16 → insertion; else introsort-equivalent.
    {
        float** Begin = *reinterpret_cast<float***>(Base + 0x04);
        float** End = *reinterpret_cast<float***>(Base + 0x08);
        if (Begin != nullptr && End != nullptr && End > Begin)
        {
            int const Count = static_cast<int>(End - Begin);
            if (Count > 1)
            {
                std::sort(Begin, End, [](float* A, float* B) -> bool {
                    return reinterpret_cast<uintptr_t>(A) < reinterpret_cast<uintptr_t>(B);
                });
            }
        }
    }
    return bFound ? 0 : 1;
}

// -------------------------------------------------------------------- the routine

int SpinesLoadFromIni(FStarFxChild40* Spines, char const* IniPath, char const* Nickname)
{
    if (Spines == nullptr || IniPath == nullptr || Nickname == nullptr)
        return 1;

    SpinesFree(Spines);
    unsigned char* const Base = Spines->Raw;
    float RadiusScale = 1.0f;

    INI_Reader Reader;
    if (!Reader.open(IniPath, false))
        return 1;

    bool bFound = false;
    while (Reader.read_header())
    {
        if (!Reader.is_header("spines"))
            continue;
        bool bNick = false;
        while (Reader.read_value())
        {
            if (!Reader.is_value("nickname"))
                continue;
            char const* Nick = Reader.get_value_string();
            if (Nick != nullptr && _stricmp(Nick, Nickname) == 0)
                bNick = true;
            break;
        }
        if (!bNick)
            continue;
        bFound = true;
        while (Reader.read_value())
        {
            if (Reader.is_value("radius_scale"))
            {
                RadiusScale = Reader.get_value_float(0);
            }
            else if (Reader.is_value("shape"))
            {
                char const* Shape = Reader.get_value_string();
                if (Shape == nullptr)
                {
                    *reinterpret_cast<unsigned*>(Base + 0x10) = 0;
                    Base[0x14] = 0;
                }
                else
                {
                    // CALL the routine(this+0x14, 0x20, value) at 0050fce0.
                    unsigned const Len =
                        CopyBoundedString(reinterpret_cast<char*>(Base + 0x14), 0x20, Shape);
                    *reinterpret_cast<unsigned*>(Base + 0x10) = Len;
                }
            }
            else if (Reader.is_value("min_radius"))
            {
                *reinterpret_cast<float*>(Base + 0x38) = Reader.get_value_float(0);
            }
            else if (Reader.is_value("max_radius"))
            {
                *reinterpret_cast<float*>(Base + 0x3c) = Reader.get_value_float(0);
            }
            else if (Reader.is_value("spine"))
            {
                // 9 floats → offsets +8..+0x28; +0/+4 filled with cos/sin below.
                float Elem[11];
                memset(Elem, 0, sizeof(Elem));
                for (int I = 0; I < 9; ++I)
                    Elem[2 + I] = Reader.get_value_float(I);
                SpinesPushElem(Spines, Elem);
            }
        }
        break;
    }
    Reader.close();

    unsigned char* Begin = *reinterpret_cast<unsigned char**>(Base + 0x04);
    unsigned char* End = *reinterpret_cast<unsigned char**>(Base + 0x08);
    int Count = 0;
    if (Begin != nullptr)
        Count = static_cast<int>(End - Begin) / 0x2c;
    if (Count > 0)
    {
        float const Step = (kPi * 2.0f) / static_cast<float>(Count);
        float Angle = 0.0f;
        for (int I = 0; I < Count; ++I)
        {
            float* const E = reinterpret_cast<float*>(Begin + I * 0x2c);
            E[0] = cosf(Angle);
            E[1] = sinf(Angle);
            Angle += Step;
        }
        for (int I = 0; I < Count; ++I)
        {
            float* const E = reinterpret_cast<float*>(Begin + I * 0x2c);
            E[2] *= RadiusScale;
            E[3] *= RadiusScale;
        }
    }
    return bFound ? 0 : 1;
}

// -------------------------------------------------------------------- the routine

int SunBodyBakeCorona(FSunEffectBody* Body)
{
    if (Body == nullptr)
        return 0;
    float* const F = reinterpret_cast<float*>(Body->Raw);
    F[2] = 0.5f;
    F[3] = 0.5f;
    F[0] = 0.0f;
    F[1] = 0.0f;
    float* Pf = F + 8;
    for (int I = 1; I < 13; ++I)
    {
        float const Angle = static_cast<float>(I - 1) * 0.5235987901687622f; // pi/6
        float const C = cosf(Angle);
        float const S = sinf(Angle);
        Pf[-2] = C;
        Pf[-1] = -S;
        Pf[0] = (C + 1.0f) * 0.5f;
        Pf[1] = 0.5f - S * 0.5f;
        Pf += 6;
    }
    float const Tween = F[0xbc]; // +0x2f0 interior_tween
    float const OneMinus = 1.0f - Tween;
    Pf = F + 0x4e;
    for (int I = 0; I < 12; ++I)
    {
        Pf[0] = OneMinus * F[0] + Tween * Pf[-0x48];
        Pf[1] = OneMinus * F[1] + Tween * Pf[-0x47];
        Pf[2] = OneMinus * F[2] + Tween * Pf[-0x46];
        Pf[3] = OneMinus * F[3] + Tween * Pf[-0x45];
        Pf += 6;
    }
    return 1;
}

// -------------------------------------------------------------------- the routine

void LoadSolarStarArchIni()
{
    // — GetFileSysPath + Pipeline state-4 sandwich around the routine on the
    // star sphere's ShapeDB (the field), with no base filesystem.
    char FullPath[260];
    GetFileSysPath(FullPath, "solar\\StarArch.ini", NeuronRuntime::Data);
    if (NeuronRuntime::Pipeline == nullptr)
        return;
    IRenderPipeline* const Pipe = NeuronRuntime::Pipeline;
    int Saved = 0;
    Pipe->GetPipelineState(4, &Saved);
    Pipe->SetPipelineState(4, 1);
    G_StarSphereShapeDB.Load(FullPath, nullptr);
    Pipe->SetPipelineState(4, Saved);
}

// -------------------------------------------------------------------- the routine

void ClearSolarStarArchShapes()
{
    // — tail jump into the routine on the field. Its one caller is the space
    // teardown the routine (0054afb6).
    G_StarSphereShapeDB.Clear();
}

// -------------------------------------------------------------------- the routine / 00510f10

FStarSphere::FStarSphere(IObjInspect* InOwner)
{
    // (thiscall, RET 4), listing-traced 0x00510d10..0x00510ee2. No test of `this`
    // (its callers construct only a non-null block). baseline writes only the fields below and
    // leaves +0x3c..+0x60 and +0x68..+0x88 as operator new left them; so does the port. The
    // readers: +0x60 (Dt) is read by Draw and written only by UpdateIntensity (the solar's
    // update), so it depends on the frame running an update first, in baseline as here;
    // +0x3c..+0x5c has no reader; +0x68..+0x80 are written by Draw (+0x68..+0x70 always, +0x78..+0x80 by the
    // camera's Project) before DrawFlare reads +0x70. One baseline read precedes every write:
    // when the very first Draw's Project fails with the star in front (+0x70 < 0), DrawFlare's
    // reads +0x84 / +0x88 that only a successful Project path writes.
    Owner = InOwner;
    Radius = 1000.0f;

    FSunEffectBody* Body = static_cast<FSunEffectBody*>(operator new(0x30c));
    if (Body != nullptr)
        SunEffectBodyInit(Body);
    SunBody = Body;

    // Ctor allocates two 0x40 children (lens_flare defaults 5/800, spines 1/1000).
    LensFlare = AllocChild40(5.0f, 800.0f);   // / 0x44480000
    Spines = AllocChild40(1.0f, 1000.0f);     // 1.0 / 0x447a0000

    IntensityFadeIn = 1.0f;
    IntensityFadeOut = 1.0f;
    ZoneOcclusionFadeIn = 2.0f;
    ZoneOcclusionFadeOut = 2.0f;
    Field30 = kPi;

    FStarGlowDesc* Glow = static_cast<FStarGlowDesc*>(operator new(0x50));
    if (Glow != nullptr)
        InitGlowDescDefaults(Glow, 0);
    // +0x10, then +0x0c = 0, +0x38 = 0, +0x34 = 0, +0x64 = 1.0f.
    StarGlow = Glow;
    StarCenter = nullptr;
    Field38 = 0.0f;
    ZoneOcclusion = 0.0f;
    Intensity = 1.0f;

    // SunBody+0x2b8 = ftol((1.0f - the field) * (int)SunBody+0x2b4
    // + 0.5f) (FLD / FSUBR [005c75dc] / FIMUL / FADD [005c75e0] / ftol). SunBody is read
    // back from this+0x14 with no null check.
    {
        unsigned char* const BodyRaw = SunBody->Raw;
        int const MaxLod = *reinterpret_cast<int*>(BodyRaw + 0x2b4);
        double const Scaled =
            (1.0 - static_cast<double>(G_SunEffectQuality)) * static_cast<double>(MaxLod) + 0.5;
        *reinterpret_cast<int*>(BodyRaw + 0x2b8) = static_cast<int>(Scaled);
    }
}

FStarSphere::~FStarSphere()
{
    // (thiscall), 0x00510f10..0x00510f8d: no test of `this`; every owned pointer
    // is tested, freed and zeroed in baseline's order (+0x10, +0x0c, +0x14, +0x18 via the routine,
    // +0x1c via the routine).
    if (StarGlow != nullptr)
    {
        operator delete(StarGlow);
        StarGlow = nullptr;
    }
    if (StarCenter != nullptr)
    {
        operator delete(StarCenter);
        StarCenter = nullptr;
    }
    if (SunBody != nullptr)
    {
        operator delete(SunBody);
        SunBody = nullptr;
    }
    if (LensFlare != nullptr)
    {
        LensFlareFree(LensFlare);
        operator delete(LensFlare);
        LensFlare = nullptr;
    }
    if (Spines != nullptr)
    {
        SpinesFree(Spines);
        operator delete(Spines);
        Spines = nullptr;
    }
}


// ------------------------------------------------ the star list the field (the routine..6e0)

static_assert(sizeof(FStarSphere) == 0x8c, "module state allocates 0x8c");
static_assert(sizeof(FStarSphereListNode) == 0xc, "std::list node 0xc");

namespace
{
    // (static initializer): the self-linked sentinel from operator new(0xc), size
    // 0, then atexit(the branch). The allocator byte is whatever the initializer's own
    // argument byte held; it is never read.
    // (atexit): every node unlinked and freed with the size counting down, then
    // the sentinel freed and both globals zeroed. The spheres themselves are not touched.
    struct FStarSphereListOwner
    {
        FStarSphereList List;

        FStarSphereListOwner()
        {
            List.AllocatorByte = 0;
            List.Pad01[0] = 0;
            List.Pad01[1] = 0;
            List.Pad01[2] = 0;
            FStarSphereListNode* const Sentinel =
                static_cast<FStarSphereListNode*>(operator new(sizeof(FStarSphereListNode)));
            Sentinel->Next = Sentinel;
            Sentinel->Prev = Sentinel;
            List.Sentinel = Sentinel;
            List.Count = 0;
        }

        ~FStarSphereListOwner()
        {
            FStarSphereListNode* const Sentinel = List.Sentinel;
            FStarSphereListNode* Node = Sentinel->Next;
            while (Node != Sentinel)
            {
                FStarSphereListNode* const Next = Node->Next;
                Node->Prev->Next = Node->Next;
                Node->Next->Prev = Node->Prev;
                operator delete(Node);
                List.Count -= 1;
                Node = Next;
            }
            operator delete(Sentinel);
            List.Sentinel = nullptr;
            List.Count = 0;
        }
    };

    FStarSphereListOwner G_StarSphereListOwner;
} // namespace

FStarSphereList* StarSphereList()
{
    // MOV EAX,&the field.
    return &G_StarSphereListOwner.List;
}

Vector* StarSphereGetOwnerPosition(FStarSphere const* Sphere, Vector* Out)
{
    // MOV ECX,[ECX+4]; CALL [[ECX]] (IObjInspect slot 0, get_position); the three
    // dwords copied into *Out; EAX = Out.
    Vector const& Pos = static_cast<IObjInspect const*>(Sphere->Owner)->get_position();
    *Out = Pos;
    return Out;
}

float StarSphereMaxGlowScale(FStarSphere const* Sphere)
{
    // the glow's scale (+0x10 -> +0x24, no test of the glow); the centre's scale
    // (+0x0c -> +0x24) or 0 without a centre; the centre's when it is strictly greater (FCOMP /
    // TEST AH,0x41 / JNZ keeps the glow's when not greater); times the radius (+0x08).
    float const Glow = Sphere->StarGlow->Scale;
    float Center = 0.0f;
    if (Sphere->StarCenter != nullptr)
        Center = Sphere->StarCenter->Scale;
    float const Scale = (Center > Glow) ? Center : Glow;
    return Scale * Sphere->Radius;
}

FStarSphere* StarSphereCreateListed(IObjInspect* Owner)
{
    // operator new(0x8c), the routine(Owner) on it.
    FStarSphere* const Sphere = new FStarSphere(Owner);

    // push_back -- the new node's Next is the sentinel, its Prev the old
    // tail (the node itself if the sentinel's Prev were null), the sentinel's Prev and the old
    // tail's Next become the node; the value is stored and the size bumped.
    FStarSphereList* const List = &G_StarSphereListOwner.List;
    FStarSphereListNode* const Sentinel = List->Sentinel;
    FStarSphereListNode* const Node =
        static_cast<FStarSphereListNode*>(operator new(sizeof(FStarSphereListNode)));
    Node->Next = Sentinel;
    Node->Prev = Sentinel->Prev != nullptr ? Sentinel->Prev : Node;
    Sentinel->Prev = Node;
    Node->Prev->Next = Node;
    Node->Sphere = Sphere;
    List->Count += 1;

    // returns back() -- the value of the sentinel's Prev.
    return List->Sentinel->Prev->Sphere;
}

void StarSphereDestroyAll()
{
    // slot 0 with 1 (the deleting destructor) on every non-null element.
    FStarSphereList* const List = &G_StarSphereListOwner.List;
    FStarSphereListNode* const Sentinel = List->Sentinel;
    for (FStarSphereListNode* Node = Sentinel->Next; Node != Sentinel; Node = Node->Next)
    {
        if (Node->Sphere != nullptr)
        {
            delete Node->Sphere;
        }
    }

    // every node unlinked and freed, the size counting down.
    FStarSphereListNode* Node = List->Sentinel->Next;
    while (Node != List->Sentinel)
    {
        FStarSphereListNode* const Next = Node->Next;
        Node->Prev->Next = Node->Next;
        Node->Next->Prev = Node->Prev;
        operator delete(Node);
        List->Count -= 1;
        Node = Next;
    }
}
