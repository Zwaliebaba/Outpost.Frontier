// Author: Giperion.

// Client equipment-FX — EquippedObj factory + FlashLight light arm.
// Agent: cursor:equip-fx-factory.

#include "client_equip_fx.h"

#include "cli_projectile_fx.h"
#include "cli_tractor_beam.h" // CliTractorBeamCreate / CliTractorBeamInit (the routine)
#include "effect_base.h"
#include "engine_light.h"
#include "particle_effect.h"
#include "NeuronRuntime.h"
#include "engine/sceneruntime.h"
#include "menu_sound_arch.h"
#include "character_select_equip.h"
#include "client_cliobj.h" // CliTradeLaneAddEffectRef / CliTradeLaneReleaseEffectRef
#include "client_gameobj.h"
#include "client_objdb.h"
#include "common_animdb.h" // AnimDB::Add (the branch)
#include "common_archetype.h"
#include "common_cequip.h"
#include "common_cobject.h"
#include "common_csys.h"
#include "client_fuse.h"
#include "common_goods.h"
#include "gf_object_ship.h" // ShipTransposeRotate (the routine)
#include "common_misc.h"  // SinglePlayer
#include "game_options.h"
#include "nn_messaging.h" // NnVoiceEnqueue (the routine) -- engine notify 0xa
#include "player_behavior.h" // GetPlayerObjRW (the routine)
#include "player_spawn.h"
#include "render/IFLAppearance.h" // RemoveAppearanceInstanceValue (the routine)
#include "render/IRenderPipeline.h"
#include "render/shading.h"
#include "screen_quad_state.h"
#include "space_camera.h"
#include "soundmanager.h"
#include "texture_shapes.h"
#include "vis_effect_arch.h"

#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>
#include <windows.h>

namespace
{
    char const* HardpointNameOf(CEquip* Equip)
    {
        CExternalEquip* Ext = CExternalEquip::cast(Equip);
        if (Ext == nullptr)
            return "(internal)";
        char const* Hp = Ext->HardpointName.Value;
        if (Hp == nullptr || Hp[0] == '\0')
            return "(no-hp)";
        return Hp;
    }

    // baseline Archetype::CliEQLight (vtable 005c7eb8, sizeof 0x98, ctor the routine) is private
    // to client_cliobj.cpp (FCliEQLight); this is its tail from +0x54 as FlashLight.cpp reads
    // it. Names are the INI keys of its ReadUnknown (the branch).
    struct FCliEQLightView
    {
        float Color[3];          // +0x54 "color"
        float GlowColor[3];      // +0x60 "glow_color"
        float MinColor[3];       // +0x6c "min_color"
        float BulbSize;          // +0x78 "bulb_size"
        float GlowSize;          // +0x7c "glow_size"
        float AvgDelay;          // +0x80 "avg_delay"
        float BlinkDuration;     // +0x84 "blink_duration"
        float Intensity;         // +0x88 "intensity" -- the real light's radius (the routine)
        float FlareConeCos[2];   // +0x8c cos(second value), +0x90 cos(first value)
        float LightsourceCone;   // +0x94 "lightsource_cone"
    };
    static_assert(sizeof(FCliEQLightView) == 0x98 - 0x54, "CliEQLight tail +0x54..+0x98");

    // Every reader dereferences the companion without a test (0x004fda41, 0x004fdb3b,
    // , 0x004fe06b).
    FCliEQLightView const* CompanionAsLight(Archetype::ICliObj* Companion)
    {
        return reinterpret_cast<FCliEQLightView const*>(
            reinterpret_cast<unsigned char const*>(Companion) + 0x54);
    }

    struct FCliEquipGameLight;

    // Client\space\FlashLight.cpp's CliEquipLight: operator new through the routine(0x9c),
    // constructor the routine, the Effect head (the routine, type 0x2e = EFT_EQUIP_LIGHT's
    // bucket) and this tail. baseline links it into bucket 0x2e from the owner's effect list sweep
    // (the routine sets +8 bit 0 and the routine for a node's effect slot), and the bucket walk
    // then runs slots +0x14 / +0x18 / +0x1c / +0x10 on it.

    // (9 slots) -- G_CliEquipLightVtable below:
    //   0 the branch  Effect base start (EffectBaseStart)
    //   1 the routine  Effect base kill (EffectKill)
    //   2 the branch  world position out, radius back
    //   3 0x0053e4a0    empty, RET 4 (ICF fold)
    //   4 the branch  Flags +8 bit 0
    //   5 the branch  clear bit 0, return the old bit 0 (RET 8)
    //   6 the routine  blink tick
    //   7 the branch  FLASHLIGHTS gate, then the flare the routine
    //   8 the routine  scalar deleting dtor (body the routine, free 0x9c)
    struct FCliEquipLight : FEffectBase
    {
        explicit FCliEquipLight(unsigned char bPlayerOwnedParam);
        ~FCliEquipLight() override;
        float GetWorldPos(FVector* Out) override;
        unsigned char IsAlive() override;
        unsigned char UpdateVisibility(float Dt, FSpaceCamera* Cam) override;
        unsigned char TickDraw(float Dt, FSpaceCamera* Cam) override;
        unsigned char BuildGeometry(FSpaceCamera* Cam) override;
        Archetype::ICliObj* Companion;      // +0x6c the CliEQLight (the routine)
        float BlinkDuty;                    // +0x70
        float BlinkRate;                    // +0x74
        float BlinkPhase;                   // +0x78
        float BlinkBrightness;              // +0x7c
        unsigned char bPlayerOwned;         // +0x80 the routine's argument
        unsigned char Pad81[3];
        unsigned long TexFrame[5];          // +0x84 TextureLib slot +0x58 frame of "shine"
        FCliEquipGameLight* EngineLight;    // +0x98 the real light (player-owned only)
    };
    static_assert(sizeof(FCliEquipLight) == 0x9c, "CliEquipLight 0x9c");
    static_assert(offsetof(FCliEquipLight, Companion) == 0x6c, "Companion");
    static_assert(offsetof(FCliEquipLight, EngineLight) == 0x98, "EngineLight");

    // The real light the routine creates for a player-owned CliEquipLight: the routine(0x130),
    // GameLight constructor the routine, +0x12c = the owner, vtable the routine =
    // {the routine, the routine, the routine, the branch}.
    struct FCliEquipGameLight : FGameLight
    {
        void Tick(float Dt) override;
        FCliEquipLight* Owner;              // +0x12c
    };
    static_assert(sizeof(FCliEquipGameLight) == 0x130, "CliEquipLight GameLight 0x130");

    // EquippedObj+0xb8 list node (the routine operator_new(0xc)), published as FEquipFxListNode.
    using FCliEquipFxListNode = FEquipFxListNode;

    FCliEquipFxListNode*& EquipFxSentinelRef(FClientGameobj* Host)
    {
        unsigned char* Base = reinterpret_cast<unsigned char*>(Host);
        return *reinterpret_cast<FCliEquipFxListNode**>(Base + 0xb8);
    }

    int& EquipFxCountRef(FClientGameobj* Host)
    {
        unsigned char* Base = reinterpret_cast<unsigned char*>(Host);
        return *reinterpret_cast<int*>(Base + 0xbc);
    }

    // Host+0xc4 — baseline std::list of FFuseBurnNode (the routine Ignite /
    // Burn). Sentinel allocated in FClientGameobj ctor.
    FFuseBurnNode*& HostFuseSentinelRef(FClientGameobj* Host)
    {
        unsigned char* Base = reinterpret_cast<unsigned char*>(Host);
        return *reinterpret_cast<FFuseBurnNode**>(Base + 0xc4);
    }

    int& HostFuseCountRef(FClientGameobj* Host)
    {
        unsigned char* Base = reinterpret_cast<unsigned char*>(Host);
        return *reinterpret_cast<int*>(Base + 0xc8);
    }

    // — Host vt+0x1c0 Ignite (NOT the routine which is +0x1a0).
    // Params (RET 0x14): SourceId, ID_String const* FuseId, SubObjId, AtT, OverrideLifetime.
    // Published as HostIgniteFuse (end of file); FClientEquippedObj::Slot1c0 runs it.
    bool HostIgniteFuseImpl(FClientGameobj* Host, unsigned SourceId, ID_String const& FuseId,
                            unsigned short SubObjId, float AtT, float OverrideLifetime)
    {
        // walk the burn list; the first node whose fuse nickname
        // (node+0xc = ClientFuse+4) matches is ignited and the walk stops (BL test at loop top).
        FFuseBurnNode* Sentinel = HostFuseSentinelRef(Host);
        for (FFuseBurnNode* Node = Sentinel->Next; Node != Sentinel; Node = Node->Next)
        {
            if (Node->Live.Name().Id == FuseId.Id)
            {
                Node->Live.IgniteAt(SourceId, SubObjId, AtT);
                return true;
            }
        }

        // find the prototype in the exe fuse database.
        ClientFuse* Proto = ClientFuseDB::Find(FuseId.Id);
        if (Proto == nullptr)
        {
            // .
            FDUMP(0x100002, "%s(%d) : *** WARNING: Fuse(%s) not found in FuseDB. Aborting.",
                  "Client\\Space\\EquippedObj.cpp", 0x2b8,
                  FuseId.get_string());
            return false;
        }

        // copy-construct a temporary ClientFuse (Fuse copy ctor clones the
        // actions, +0x34 host copied), push_back a copy of it (the routine: node before the
        // sentinel, size++), destroy the temporary.
        {
            ClientFuse Temp(*Proto);
            FFuseBurnNode* NewNode = new FFuseBurnNode{Sentinel, Sentinel->Prev, Temp};
            Sentinel->Prev->Next = NewNode;
            Sentinel->Prev = NewNode;
            HostFuseCountRef(Host) = HostFuseCountRef(Host) + 1;
        }

        // all on the new last node.
        ClientFuse& Live = Sentinel->Prev->Live;
        Live.BindHost(Host);
        Live.RandomizeActions();
        // FCOMP the field (-1.0f); TEST AH,0x44; JNP skip -> call unless equal.
        if (OverrideLifetime != -1.0f)
            Live.OverrideLifetime(OverrideLifetime);
        Live.IgniteAt(SourceId, SubObjId, AtT);
        return true;
    }

    CEqObj* ResolveEqObjFromHost(FClientGameobj* Host)
    {
        if (Host == nullptr || Host->Object == nullptr)
            return nullptr;
        CObject* Obj = Host->Object;
        if ((static_cast<unsigned>(Obj->ObjectClass) & 0x103u) != 0x103u)
            return nullptr;
        return static_cast<CEqObj*>(Obj);
    }

    // The light equipment's client wrapper: operator new(0x20) in the factory the routine,
    // constructor the routine, vtable the routine {notify the routine, scalar deleting
    // dtor the routine (body the routine), the routine}. Its base (vtable the routine,
    // notify the routine, dtor the routine) is FClientEquip; baseline inlines its constructor
    // (the routine) and destructor (the routine) here, the port runs them as the base's.
    // Slot 2 (Update) is the base's the routine.
    struct FCliLightEquipFx : FClientEquip
    {
        // (thiscall, RET 8).
        FCliLightEquipFx(CEquip* EquipParam, FClientGameobj* HostParam);
        // .
        ~FCliLightEquipFx() override;

        // (RET 8).
        void notify(Event Code, void* Param) override;
        // (thiscall, no arguments): a new CliEquipLight on the hardpoint, taken into
        // Companion and the host's effect list.
        unsigned char Activate();

        // +0x10 the 0x10-byte effect companion {arch, ISoundSource*, effect, FGameLight*}; only
        // the effect slot is ever filled here (the CliEquipLight, 0x00531e11).
        void* Companion[4];
    };

    static_assert(sizeof(FCliLightEquipFx) == 0x20, "CliLightEquipFx 0x20");
    static_assert(offsetof(FCliLightEquipFx, Companion) == 0x10, "CliLightEquipFx +0x10");

    // / the routine are ExplosionArchAddRef / ExplosionArchRelease
    // (client_gameobj.h); this file used to carry its own copies.

    // ---- CliEquipLight (Client\space\FlashLight.cpp) -------------------------------------

    constexpr float kRandUnit = 3.0517578125e-05f;          // , 1/32768
    constexpr double kBlinkJitterScale = 0.20000004768371582;
    constexpr double kBlinkJitterBase = 0.8999999761581421;
    constexpr float kFullConeDegrees = 180.0f;
    constexpr float kFastInvSqrtHalf = 0.5f;
    constexpr float kFastInvSqrtThreeHalves = 1.5f;
    constexpr float kBlinkPeak = 2.0f;
    constexpr char const kFlashLightCpp[] =                  // .rdata 0x005da434
        "Client\\space\\FlashLight.cpp";

    // 's colour byte: FLD float / FMUL double 255.0 / FADD double 0.5 /
    // FSTP double, floor, ftol -- and only AL is kept.
    unsigned char LightColorByte(float Component)
    {
        double const Scaled = static_cast<double>(Component) * 255.0 + 0.5;
        return static_cast<unsigned char>(static_cast<long>(floor(Scaled)));
    }

    // the destructor body.
    FCliEquipLight::~FCliEquipLight()
    {
        FCliEquipLight* const Self = this;
        // the class's own table goes back in first.
        if (Self->EngineLight != nullptr)
        {
            // the light's slot 2 (disable), the routine, cleared.
            Self->EngineLight->Disable();
            GameLightRelease(Self->EngineLight);
            Self->EngineLight = nullptr;
        }
        // the routine, the Effect base destructor.
    }

    // (thiscall, RET 4) -- nothing else of the tail is written here; the routine
    // fills it.
    FCliEquipLight::FCliEquipLight(unsigned char bPlayerOwnedParam) : FEffectBase(0x2e)
    {
        FCliEquipLight* const Self = this;
        Self->bPlayerOwned = bPlayerOwnedParam;
        Self->EngineLight = nullptr;
    }

    FCliEquipLight* CliEquipLightConstruct(FCliEquipLight* Self, unsigned char bPlayerOwned)
    {
        return ::new (Self) FCliEquipLight(bPlayerOwned);
    }

    // (thiscall, RET 4) -- blink parameters from the CliEQLight, the "shine"
    // frame, and for a player-owned light a real GameLight. Always returns 1.
    unsigned char CliEquipLightInit(FCliEquipLight* Self, Archetype::ICliObj* Companion)
    {
        Self->Companion = Companion;
        Self->BlinkRate = 0.0f;
        Self->BlinkDuty = 1.0f;
        FCliEQLightView const* const View = CompanionAsLight(Self->Companion);

        // the period (rand / 32768 * 0.2 + 0.9) * avg_delay stays on the
        // FPU for the test and is kept as a float (FST) for the rest.
        double const PeriodExt =
            (static_cast<double>(rand()) * kRandUnit * kBlinkJitterScale + kBlinkJitterBase) *
            View->AvgDelay;
        float const Period = static_cast<float>(PeriodExt);
        // FCOMP 0.0 / TEST AH,0x41 / JNZ: only a period above zero (not NaN) blinks.
        if (PeriodExt > 0.0)
        {
            // FLD 1.0 / FDIV Period / FST Rate -- the quotient stays on
            // the FPU unrounded for the duty below.
            double const RateExt = 1.0 / static_cast<double>(Period);
            Self->BlinkRate = static_cast<float>(RateExt);
            // FCOMP / TEST AH,0x41 / JNZ -- blink_duration at or below
            // the period (or NaN) is used as is, above it the period.
            float const Lit = (View->BlinkDuration > Period) ? Period : View->BlinkDuration;
            Self->BlinkDuty = static_cast<float>(RateExt * static_cast<double>(Lit));
        }

        // .
        Self->BlinkBrightness = 1.0f;
        Self->BlinkPhase = static_cast<float>(static_cast<double>(rand()) * kRandUnit);

        // the ShapeDB lookup inlined (FDUMP at cmn\ShapeDB.h line 0xf2
        // when missing), then TextureLib slot +0x58 on the record's texture -- no test of the
        // record in between.
        FTexShape* const Shine = G_EffectShapeDB.Find("shine");
        NeuronRuntime::TextureLib->GetTextureFrame(Shine->Texture, 0, Self->TexFrame);

        // TEST AH,0x41 / JNZ -- only an intensity above zero.
        if (View->Intensity > 0.0f)
        {
            // TEST AH,0x44 / JNP skips only an exact 0.0 rate.
            if (Self->BlinkRate != 0.0f)
            {
                FDUMP(0x100002, "%s(%d) : *** WARNING: %s", kFlashLightCpp, 0x8d,
                      "CliEquipLight: real light sources so no longer blink");
            }

            if (Self->bPlayerOwned != 0)
            {
                // the routine(0x130) (malloc, the size is not the
                // pool's), the routine, owner, the routine; +0x98 stored either way.
                void* const Mem = GameLightHeapAlloc(sizeof(FCliEquipGameLight));
                FCliEquipGameLight* Light = nullptr;
                if (Mem != nullptr)
                {
                    Light = ::new (Mem) FCliEquipGameLight();
                    Light->Owner = Self;
                }
                Self->EngineLight = Light;

                // the routine with the colour bytes, the routine with
                // the intensity.
                unsigned char Rgb[3];
                Rgb[0] = LightColorByte(View->Color[0]);
                Rgb[1] = LightColorByte(View->Color[1]);
                Rgb[2] = LightColorByte(View->Color[2]);
                EngineLightSetColorBytesAlt((FEngineLight*)Light, Rgb);
                EngineLightSetRadius((FEngineLight*)Light, View->Intensity);

                // FCOMP 180.0 / TEST AH,5 / JP -- a cone below 180 is a
                // spot (type 2 plus the routine), anything else (NaN too) a point (type 1).
                if (View->LightsourceCone < kFullConeDegrees)
                {
                    EngineLightSetType((FEngineLight*)Light, 2, true);
                    EngineLightSetConeAngle((FEngineLight*)Light, View->LightsourceCone);
                }
                else
                {
                    EngineLightSetType((FEngineLight*)Light, 1, true);
                }

                // slot 1 (enable); a refusal releases the light.
                if (!Light->Enable())
                {
                    GameLightRelease(Self->EngineLight);
                    Self->EngineLight = nullptr;
                }
            }
        }
        return 1;
    }

    // (thiscall, RET 4) -- the flare: the "shine" node of the screen-quad batch,
    // the fade from the flare cone, then the routine. Returns 1.
    unsigned char CliEquipLightDrawFlare(FCliEquipLight* Self, FSpaceCamera* Cam)
    {
        // the routine, then the routine(&TexFrame, 1, 1).
        FTransform World;
        EffectGetWorldTransform(Self, &World);
        ScreenQuadBeginBatchFrame(Self->TexFrame, 1, true);

        FCliEQLightView const* const View = CompanionAsLight(Self->Companion);
        float Fade = 1.0f;
        // FCOMP / TEST AH,0x41 / JNZ -- only FlareConeCos[0] above
        // FlareConeCos[1] fades.
        if (View->FlareConeCos[0] > View->FlareConeCos[1])
        {
            // camera slot +0xc (GetPosition), the offset built as a
            // float Vector (the routine).
            float const* const CamPos = AsCamera(Cam)->GetPosition();
            float Dir[3];
            Dir[0] = World.Translation.X - CamPos[0];
            Dir[1] = World.Translation.Y - CamPos[1];
            Dir[2] = World.Translation.Z - CamPos[2];

            // the fast inverse square root on the float length^2
            // ((z*z + y*y) + x*x, FSTP float), one Newton step on the FPU, each component
            // stored back as a float.
            float const LengthSq = static_cast<float>(
                static_cast<double>(Dir[2]) * Dir[2] + static_cast<double>(Dir[1]) * Dir[1] +
                static_cast<double>(Dir[0]) * Dir[0]);
            int Bits = 0;
            std::memcpy(&Bits, &LengthSq, sizeof(Bits));
            Bits = 0x5f3759df - (Bits >> 1);
            float Guess = 0.0f;
            std::memcpy(&Guess, &Bits, sizeof(Guess));
            double const InvLength =
                (kFastInvSqrtThreeHalves - static_cast<double>(Guess) * Guess * LengthSq *
                                               kFastInvSqrtHalf) *
                Guess;
            Dir[0] = static_cast<float>(Dir[0] * InvLength);
            Dir[1] = static_cast<float>(Dir[1] * InvLength);
            Dir[2] = static_cast<float>(Dir[2] * InvLength);

            // the dot with the forward column (+0x08, +0x14, +0x20 of
            // the rotation), (z + y) + x, stored as a float.
            float const Dot = static_cast<float>(
                static_cast<double>(Dir[2]) * World.Rotation.D[2][2] +
                static_cast<double>(Dir[1]) * World.Rotation.D[1][2] +
                static_cast<double>(Dir[0]) * World.Rotation.D[0][2]);

            // TEST AH,5 / JP twice -- below the outer cosine no flare,
            // between the two a linear fade, otherwise (NaN too) full.
            if (Dot < View->FlareConeCos[1])
            {
                Fade = 0.0f;
            }
            else if (Dot < View->FlareConeCos[0])
            {
                Fade = static_cast<float>(
                    (static_cast<double>(Dot) - View->FlareConeCos[1]) /
                    (static_cast<double>(View->FlareConeCos[0]) - View->FlareConeCos[1]));
            }
        }

        // the glow colour scaled by the blink brightness.
        float const Brightness = Self->BlinkBrightness;
        float Glow[3];
        Glow[0] = Brightness * View->GlowColor[0];
        Glow[1] = Brightness * View->GlowColor[1];
        Glow[2] = Brightness * View->GlowColor[2];

        // the bulb colour, min_color + brightness * (color - min_color);
        // x and y go through a float temporary, z stays on the FPU.
        float Span[3];
        Span[0] = View->Color[0] - View->MinColor[0];
        Span[1] = View->Color[1] - View->MinColor[1];
        Span[2] = View->Color[2] - View->MinColor[2];
        float const ScaledX = Brightness * Span[0];
        float const ScaledY = Brightness * Span[1];
        float Bulb[3];
        Bulb[0] = ScaledX + View->MinColor[0];
        Bulb[1] = ScaledY + View->MinColor[1];
        Bulb[2] = static_cast<float>(static_cast<double>(Brightness) * Span[2] + View->MinColor[2]);

        // the glow faded.
        Glow[0] = Glow[0] * Fade;
        Glow[1] = Glow[1] * Fade;
        Glow[2] = Glow[2] * Fade;

        // the routine(Cam, position, bulb colour, glow colour,
        // bulb_size, glow_size).
        float const Position[3] = {World.Translation.X, World.Translation.Y, World.Translation.Z};
        ScreenQuadEnqueueFlare(AsCamera(Cam), Position, Bulb, Glow, View->BulbSize,
                               View->GlowSize);
        return 1;
    }

    // Slot 0 -- the branch.


    // Slot 1 -- the routine.


    // Slot 2 -- the branch (RET 4): the world position (the routine) out, and the larger of
    // bulb_size / glow_size back on the FPU (FCOMPP / TEST AH,5 / JP: bulb_size unless it is
    // below glow_size).
    float FCliEquipLight::GetWorldPos(FVector* Out)
    {
        FCliEquipLight* const Self = this;
        FTransform World;
        EffectGetWorldTransform(Self, &World);
        Out->X = World.Translation.X;
        Out->Y = World.Translation.Y;
        Out->Z = World.Translation.Z;
        FCliEQLightView const* const View = CompanionAsLight(Self->Companion);
        if (View->BulbSize < View->GlowSize)
            return View->GlowSize;
        return View->BulbSize;
    }

    // Slot 3 -- 0x0053e4a0, a bare RET 4.


    // Slot 4 -- the branch: Flags +8 bit 0 (set by the owner's list sweep the routine).
    unsigned char FCliEquipLight::IsAlive()
    {
        FCliEquipLight* const Self = this;
        return static_cast<unsigned char>(Self->Flags08 & 1u);
    }

    // Slot 5 -- the branch (RET 8, both arguments unread): bit 0 cleared, its old value back.
    unsigned char FCliEquipLight::UpdateVisibility(float /*Dt*/, FSpaceCamera* /*Cam*/)
    {
        FCliEquipLight* const Self = this;
        unsigned char const Flags = Self->Flags08;
        Self->Flags08 = static_cast<unsigned char>(Flags & 0xfeu);
        return static_cast<unsigned char>(Flags & 1u);
    }

    // Slot 6 -- the routine (RET 8): the blink. Returns 1.
    unsigned char FCliEquipLight::TickDraw(float Dt, FSpaceCamera* Cam)
    {
        FCliEquipLight* const Self = this;
        // TEST AH,0x44 / JNP -- a rate of exactly 0.0 does nothing.
        if (Self->BlinkRate == 0.0f)
            return 1;

        // the new phase on the FPU, FST into +0x78; at or above 1.0
        // (FCOM double / TEST AH,1 / JNZ: below and NaN keep it) it is x - floor(x).
        double const Phase =
            static_cast<double>(Dt) * Self->BlinkRate + static_cast<double>(Self->BlinkPhase);
        Self->BlinkPhase = static_cast<float>(Phase);
        if (Phase >= 1.0)
            Self->BlinkPhase = static_cast<float>(Phase - floor(Phase));

        // TEST AH,0x41 / JNZ -- past the duty the light is dark.
        if (Self->BlinkPhase > Self->BlinkDuty)
        {
            Self->BlinkBrightness = 0.0f;
            return 1;
        }

        // the triangle 2/duty * phase, folded above 1.0 as 2 - x.
        double Wave = static_cast<double>(kBlinkPeak) / Self->BlinkDuty * Self->BlinkPhase;
        if (Wave > 1.0)
            Wave = kBlinkPeak - Wave;
        Self->BlinkBrightness = static_cast<float>(Wave);
        return 1;
    }

    // Slot 7 -- the branch (RET 4): the routine(0x29) is read twice.
    unsigned char FCliEquipLight::BuildGeometry(FSpaceCamera* Cam)
    {
        FCliEquipLight* const Self = this;
        if (GetGameOption(kGameOptFlashlights) == 0)
            return 0;
        // FCOMP 0.0 / TEST AH,5 / JP -- only a negative brightness.
        if (Self->BlinkBrightness < 0.0f)
            return 0;
        if (GetGameOption(kGameOptFlashlights) == 1)
            return CliEquipLightDrawFlare(Self, Cam);
        return 1;
    }

    // Slot 8 -- the routine (RET 4).


    // slot 3 -- the branch (RET 4): the GameLight tick the routine, then
    // the owner's world transform (the routine on +0x12c) through the routine.
    void FCliEquipGameLight::Tick(float Dt)
    {
        FCliEquipGameLight* const Self = this;
        GameLightTick(Self, Dt);
        FTransform World;
        EffectGetWorldTransform(Self->Owner, &World);
        EngineLightSetTransform((FEngineLight*)Self, &World.Rotation.D[0][0], &World.Translation.X);
    }

    FCliLightEquipFx::FCliLightEquipFx(CEquip* EquipParam, FClientGameobj* HostParam)
    {
        // the inlined base -- +4 / +8 / +0xc, connect, then an attached
        // equipment's separation_explosion (arch +0x70) resolved and referenced. No test of
        // Equip.
        Equip = EquipParam;
        Host = HostParam;
        SeparationExplosion = nullptr;
        Equip->ConnectClientEquip(this);
        CAttachedEquip* const Attached = CAttachedEquip::cast(Equip);
        if (Attached != nullptr)
        {
            if (!Attached->AttachedEquipArch()->SeparationExplosion.IsEmpty())
            {
                SeparationExplosion =
                    Archetype::GetExplosion(Attached->AttachedEquipArch()->SeparationExplosion);
                if (SeparationExplosion != nullptr)
                    ExplosionArchAddRef(SeparationExplosion);
            }
        }

        // the companion's own constructor zeroes all four slots.
        Companion[0] = nullptr;
        Companion[1] = nullptr;
        Companion[2] = nullptr;
        Companion[3] = nullptr;

        // an equipment already on (vtable +4, IsActive) lights at once.
        if (Equip->IsActive())
            Activate();
    }

    FCliLightEquipFx::~FCliLightEquipFx()
    {
        // . 0x005323de..0x00532412: the companion's destructor (the routine's
        // body) -- effect the routine, sound Release, light the routine, arch slot cleared.
        // The node the host's effect list holds keeps its own references.
        CliEffectCompanionClear(Companion);
        // (the inlined base destructor) is ~FClientEquip.
    }

    void FCliLightEquipFx::notify(Event Code, void* /*Param*/)
    {
        // (RET 8).
        if (Code == EquipDeactivating)
        {
            // the light leaves the host's effect list while anything of
            // it still runs -- a live effect (+8 bit 1 clear), an active sound (vtable +0x14) or
            // a light without anim keys (+0xd4 null).
            FEffectBase const* const Effect = static_cast<FEffectBase const*>(Companion[2]);
            ISoundSource* const Sound = static_cast<ISoundSource*>(Companion[1]);
            FGameLight const* const Light = static_cast<FGameLight const*>(Companion[3]);
            if ((Effect != nullptr && (Effect->Flags08 & 2u) == 0) ||
                (Sound != nullptr && Sound->IsActive() != 0) ||
                (Light != nullptr && Light->Anim.Keys == nullptr))
            {
                CliEffectListRemove(&Host->EffectList, Companion);
            }
            return;
        }

        if (Code == EquipActivating)
        {
            // nothing while any of the same three still runs, otherwise
            // a fresh light.
            FEffectBase const* const Effect = static_cast<FEffectBase const*>(Companion[2]);
            if (Effect != nullptr && (Effect->Flags08 & 2u) == 0)
                return;
            ISoundSource* const Sound = static_cast<ISoundSource*>(Companion[1]);
            if (Sound != nullptr && Sound->IsActive() != 0)
                return;
            FGameLight const* const Light = static_cast<FGameLight const*>(Companion[3]);
            if (Light != nullptr && Light->Anim.Keys == nullptr)
                return;
            Activate();
            return;
        }

        if (Code == EquipDestroying)
        {
            // out of the host's list, then the base's disconnect.
            CliEffectListRemove(&Host->EffectList, Companion);
            Equip->NotifyDisconnecting(this);
            Equip = nullptr;
        }
    }

    unsigned char FCliLightEquipFx::Activate()
    {
        // . 0x00531d0e..0x00531d3a: the routine(0x9c), then the routine with
        // "the host is the player" (the routine, a raw pointer compare).
        void* const Mem = EffectAlloc(0x9c);
        FCliEquipLight* Light = nullptr;
        if (Mem != nullptr)
        {
            unsigned char const bPlayer = static_cast<unsigned char>(
                static_cast<void*>(static_cast<IObjRW*>(Host)) == GetPlayerObjRW() ? 1 : 0);
            Light = CliEquipLightConstruct(static_cast<FCliEquipLight*>(Mem), bPlayer);
        }

        // equipment vtable +0x68 (CExternalEquip::GetHardPointInfo),
        // called on the CEquip without a cast check.
        CExternalEquip const* const External = static_cast<CExternalEquip const*>(Equip);
        HardpointInfo Hardpoint;
        if (!External->GetHardPointInfo(Hardpoint))
        {
            // .
            EffectRelease(Light);
            return 0;
        }

        // the hardpoint's orientation and position as the local frame,
        // vtable +0x64 (GetParentConnector(false)) as the part, the routine.
        FTransform Local;
        std::memcpy(&Local.Rotation, &Hardpoint.Orient, sizeof(Local.Rotation));
        Local.Translation.X = Hardpoint.Position.x;
        Local.Translation.Y = Hardpoint.Position.y;
        Local.Translation.Z = Hardpoint.Position.z;
        long const Part = External->GetParentConnector(false);
        EffectAttach(Light, Host, static_cast<int>(Part), &Local);

        // the arch's +4 (Companion, the CliEQLight), null without an
        // equipment.
        Archetype::ICliObj* const LightArch = (Equip != nullptr) ? Equip->Arch->Companion : nullptr;
        unsigned char const bOk = CliEquipLightInit(Light, LightArch);
        if (bOk != 0)
        {
            // a local companion {0, 0, light, 0} (its copy takes a
            // reference), assigned into +0x10 (the old contents released as the routine
            // does), then the local destroyed.
            void* const Local4[4] = {nullptr, nullptr, Light, nullptr};
            if (Light != nullptr)
                EffectAddRef(Light);
            CliEffectCompanionClear(Companion);
            Companion[0] = Local4[0];
            Companion[2] = Local4[2];
            if (Light != nullptr)
                EffectAddRef(Light);
            Companion[1] = Local4[1];
            Companion[3] = Local4[3];
            if (Light != nullptr)
                EffectRelease(Light);

            // the routine on the host's list (+0x28).
            CliEffectListInsert(&Host->EffectList, Companion);
        }

        // the allocation's own reference goes.
        EffectRelease(Light);
        return bOk;
    }

    // The factory's light arm (the routine): operator new(0x20), the routine.
    FCliLightEquipFx* CliLightEquipFxConstruct(CEquip* Equip, FClientGameobj* Host)
    {
        return new FCliLightEquipFx(Equip, Host);
    }

    void EquipFxListInsert(FClientGameobj* Host, FClientEquip* Wrapper)
    {
        if (Host == nullptr || Wrapper == nullptr)
            return;
        FCliEquipFxListNode* Sentinel = EquipFxSentinelRef(Host);
        if (Sentinel == nullptr)
            return;
        FCliEquipFxListNode* Node =
            static_cast<FCliEquipFxListNode*>(::operator new(sizeof(FCliEquipFxListNode)));
        Node->Next = Sentinel;
        Node->Prev = Sentinel->Prev;
        Node->Wrapper = Wrapper;
        Sentinel->Prev = Node;
        if (Node->Prev != nullptr)
            Node->Prev->Next = Node;
        EquipFxCountRef(Host) = EquipFxCountRef(Host) + 1;
    }

    void EquippedObjResolveExplosion(FClientGameobj* Host)
    {
        // on EquippedObj — store @+0x40. Watcher/vt+0x88 gap unchanged.
        if (Host == nullptr)
            return;
        CObject* Obj = Host->Object;
        if (Obj == nullptr)
            return;
        Archetype::Root* Arch = Obj->get_archetype();
        if (Arch == nullptr || Arch->ExplosionArch.IsEmpty())
            return;
        Archetype::Explosion* Expl = Archetype::GetExplosion(Arch->ExplosionArch);
        *reinterpret_cast<Archetype::Explosion**>(reinterpret_cast<unsigned char*>(Host) + 0x40) =
            Expl;
        ExplosionArchAddRef(Expl);
    }

    // --- Equip-FX type wrappers (the routine family) ---

    // The ClientEquip base is published as FClientEquip (client_equip_fx.h); its members are
    // defined at the end of this file.
    using FCliEquipFxBase = FClientEquip;
    static_assert(sizeof(FCliEquipFxBase) == 0x10, "CliEquipFxBase 0x10");

    // The code-0xa "the player lost this" test the equipment notifies inline (shield generator
    // , engine 0x00527f0b..0x00527f68, thruster, the launcher family):
    // the host is the player's (the routine, a pointer compare), not reaped (vtable +0x158),
    // and its object narrowed by Mask (host +0xc, +4, ObjectClass & Mask == Mask, else null --
    // get_hit_pts is called on it regardless) has hit points above 0 (FCOMP the field,
    // TEST AH,0x41, JNZ skips on <= / NaN).
    // Its first half alone (repair droid 0x005329a8..0x005329c4 / 0x00532b33..0x00532b4c):
    // the player's host, not reaped.
    bool CliEquipHostIsPlayer(FClientGameobj* Host)
    {
        if (static_cast<void*>(static_cast<IObjRW*>(Host)) != GetPlayerObjRW())
            return false;
        return !Host->Slot158();
    }

    bool CliEquipHostIsLivePlayer(FClientGameobj* Host, unsigned Mask)
    {
        if (!CliEquipHostIsPlayer(Host))
            return false;
        CSimple* Simple = nullptr;
        if (Host != nullptr && Host->Object != nullptr &&
            (static_cast<unsigned>(Host->Object->ObjectClass) & Mask) == Mask)
        {
            Simple = static_cast<CSimple*>(Host->Object);
        }
        return Simple->get_hit_pts() > 0.0f;
    }

    // , filled by the static initializer the branch.
    unsigned const G_ShieldDestroyedId = CreateID("shield_destroyed");

    // The shield generator's client wrapper: operator new(0x10), constructor the routine,
    // vtable the routine {notify the routine, scalar deleting dtor the routine (the
    // base's the routine through the thunk 0052eb50), Update the routine}.
    struct FCliShieldGenEquipFx : FCliEquipFxBase
    {
        // (RET 8).
        void notify(Event Code, void* Param) override;
    };
    static_assert(sizeof(FCliShieldGenEquipFx) == 0x10, "ShieldGenFx 0x10");

    void FCliShieldGenEquipFx::notify(Event Code, void* Param)
    {
        // code 0xa on the player's live host (class 0x103) queues
        // "shield_destroyed" (the routine with the field, 0); every code then goes to
        // with both arguments.
        if (Code == EquipDestroying && CliEquipHostIsLivePlayer(Host, 0x103u))
            NnVoiceEnqueue(G_ShieldDestroyedId, 0);
        FClientEquip::notify(Code, Param);
    }

    // The cloaking device's client wrapper: operator new(0x10), constructor the routine (the
    // base the routine inlined), vtable the branch {notify the branch, scalar deleting
    // dtor the routine (body the routine), Update the routine}.
    struct FCliCloakEquipFx : FCliEquipFxBase
    {
        // .
        ~FCliCloakEquipFx() override;
        // (RET 8).
        void notify(Event Code, void* Param) override;
    };
    static_assert(sizeof(FCliCloakEquipFx) == 0x10, "CloakFx 0x10");

    FCliCloakEquipFx::~FCliCloakEquipFx()
    {
        // the host's index (vtable +0x1c, no test of the host), then the body of
        // inlined -- IFLAppearance from NeuronRuntime::Engine, slot 5 with the index when
        // the query succeeded, released either way. The base destructor follows.
        long const Index = Host->get_index();
        RemoveAppearanceInstanceValue(static_cast<int>(Index));
    }

    void FCliCloakEquipFx::notify(Event Code, void* /*Param*/)
    {
        // (RET 8). the routine is never called.
        CEquip* const Device = Equip;
        if (Code == EquipActivating)
        {
            // an identity frame; the equipment's vtable +0x84
            // (CAttachedEquip::GetRootIndex); cloakout_fx (CECloakingDevice::CloakArch +0x98);
            // not empty -> the routine and the routine on the field (host, root, frame, no
            // companion).
            FTransform Identity;
            std::memset(&Identity, 0, sizeof(Identity));
            Identity.Rotation.D[0][0] = 1.0f;
            Identity.Rotation.D[1][1] = 1.0f;
            Identity.Rotation.D[2][2] = 1.0f;
            long const Root = static_cast<CAttachedEquip*>(Device)->GetRootIndex();
            FClientGameobj* const Owner = Host;
            ID_String const* const FxId = reinterpret_cast<ID_String const*>(
                &static_cast<CECloakingDevice*>(Device)->CloakArch()->CloakOutFx);
            if (!FxId->IsEmpty())
            {
                CliCreateEffectOneShot(FindEffectArch(&FxId->Id), Owner, static_cast<int>(Root),
                                       &Identity, nullptr);
            }
            return;
        }
        if (Code == EquipDeactivating)
        {
            // the routine with the host's index; the host's velocity
            // (vtable +4) and orientation (vtable +0x10, the Transform it points into);
            // cloakin_fx (+0x94); not empty -> the routine and the routine on the field
            // (the frame, the velocity, no companion).
            RemoveAppearanceInstanceValue(static_cast<int>(Host->get_index()));
            Vector const Velocity = Host->get_velocity();
            Matrix const& Orientation = Host->get_orientation();
            ID_String const* const FxId = reinterpret_cast<ID_String const*>(
                &static_cast<CECloakingDevice*>(Device)->CloakArch()->CloakInFx);
            if (!FxId->IsEmpty())
            {
                CliCreateEffectWorldOneShot(FindEffectArch(&FxId->Id),
                                            reinterpret_cast<FTransform const*>(&Orientation),
                                            reinterpret_cast<float const*>(&Velocity), nullptr);
            }
            return;
        }
        if (Code == EquipDestroying)
        {
            // CEquip::NotifyDisconnecting(this), Equip = 0.
            Device->NotifyDisconnecting(this);
            Equip = nullptr;
        }
    }

    // / the field / the field / the field, filled by the static
    // initializers the branch / the branch / the branch / the branch (the branch is
    // that initializer's CreateID-and-store tail, reached by its fall-through only; its other
    // XREFs, e.g. 0x0045e05a / 0x005671d7 / 0x00599dea, are OR-immediates 0x0052eaf5 that
    // analysis mislabelled as addresses).
    unsigned const G_ShieldDeactivatedId = CreateID("shield_deactivated");
    unsigned const G_ShieldActivatedId = CreateID("shield_activated");
    unsigned const G_ShieldFailedId = CreateID("nnv_shield_failed");
    unsigned const G_ShieldRestoredId = CreateID("shield_restored");

    // The shield's client wrapper (RTTI CliShield: COL 005ee7c8 at vtable[-4], type descriptor
    // ".?AUCliShield@@"; seven slots, 005ddaf0 is already a string): operator new(0x18)
    // in the factory arm 0x005333ab,
    // constructor the routine, vtable the branch {notify the branch, scalar deleting
    // dtor the routine (body the routine), Update the routine, then four slots of its own:
    // +0xc the branch, +0x10 the routine, +0x14 the branch, +0x18 the routine}. The
    // equipped object's hit slots (the routine / the routine) reach +0x14 / +0x18 on the
    // wrapper of a shield sub-object (CEquip type 0x100).
    struct FCliShieldEquipFx : FCliEquipFxBase
    {
        // .
        ~FCliShieldEquipFx() override;
        // (RET 8).
        void notify(Event Code, void* Param) override;
        // +0x0c the branch (bare RET): the collapse / rebuilt sound archetypes looked up and
        // acquired again (neither is released or cleared first).
        virtual void ReloadSounds();
        // +0x10 the routine (bare RET): both released, neither cleared.
        virtual void ReleaseSounds();
        // +0x14 the branch (RET 0xc): a collision on the shield bubble. HitInfo is the hit
        // point (+0) and the surface normal (+0xc); the third argument is not read.
        virtual void OnCollisionHit(void const* HitInfo, FClientGameobj* Other,
                                    void const* Unused);
        // +0x18 the routine (RET 8): a munition hitting the shield bubble.
        virtual void OnMunitionHit(void const* HitInfo, FClientGameobj* Munition);

        // The body both the constructor (0x0052e246..0x0052e299) and ReloadSounds run.
        void LoadSounds();

        void* CollapseSound; // +0x10  CliEQShieldGenerator +0x58 (the routine, Acquire'd)
        void* RebuiltSound;  // +0x14  CliEQShieldGenerator +0x5c
    };
    static_assert(sizeof(FCliShieldEquipFx) == 0x18, "ShieldFx 0x18");

    // The dominant generator's companion as every shield slot reads it: the equipment (+4),
    // its +0x70 (CEShield::DominantGenArch, read with no cast) and that archetype's +4
    // companion (a CliEQShieldGenerator), null when any link is.
    Archetype::ICliObj* CliShieldGenCompanion(CEquip* Equip)
    {
        if (Equip == nullptr)
            return nullptr;
        Archetype::ShieldGenerator* const Gen = static_cast<CEShield*>(Equip)->DominantGenArch;
        if (Gen == nullptr)
            return nullptr;
        return Gen->Companion;
    }

    void FCliShieldEquipFx::LoadSounds()
    {
        // / 0x0052e353..0x0052e3ae: the generator companion; its +0x58
        // not empty -> the routine into +0x10 and that archetype's vtable +0xc (Acquire) with
        // no test of the result; the same for +0x5c into +0x14.
        Archetype::ICliObj* const Comp = CliShieldGenCompanion(Equip);
        if (Comp == nullptr)
            return;
        unsigned char const* const CompBytes = reinterpret_cast<unsigned char const*>(Comp);
        ID_String const* const Collapse = reinterpret_cast<ID_String const*>(CompBytes + 0x58);
        if (!Collapse->IsEmpty())
        {
            CollapseSound = LookupClientSoundArch(Collapse);
            static_cast<Archetype::ISound*>(CollapseSound)->Acquire();
        }
        ID_String const* const Rebuilt = reinterpret_cast<ID_String const*>(CompBytes + 0x5c);
        if (!Rebuilt->IsEmpty())
        {
            RebuiltSound = LookupClientSoundArch(Rebuilt);
            static_cast<Archetype::ISound*>(RebuiltSound)->Acquire();
        }
    }

    void FCliShieldEquipFx::ReloadSounds()
    {
        // the constructor's lookup, as is.
        LoadSounds();
    }

    void FCliShieldEquipFx::ReleaseSounds()
    {
        // +0x10, then +0x14, each non-null -> vtable +0x10 (Release).
        if (CollapseSound != nullptr)
            static_cast<Archetype::ISound*>(CollapseSound)->Release();
        if (RebuiltSound != nullptr)
            static_cast<Archetype::ISound*>(RebuiltSound)->Release();
    }

    FCliShieldEquipFx::~FCliShieldEquipFx()
    {
        // the body of the routine inlined (neither pointer cleared), then the
        // base destructor the routine.
        if (CollapseSound != nullptr)
            static_cast<Archetype::ISound*>(CollapseSound)->Release();
        if (RebuiltSound != nullptr)
            static_cast<Archetype::ISound*>(RebuiltSound)->Release();
    }

    // The play key the routine gets from both shield sounds: +0 zero, +4 the flag byte with
    // its low three bits cleared and bit 1 set (positional; the upper bits are whatever the
    // stack held in baseline, zero here), +8 the position, +0x14 zero.
    void CliShieldPlayAt(void* Sound, float const* Pos)
    {
        unsigned Key[6] = {0u, 2u, 0u, 0u, 0u, 0u};
        std::memcpy(&Key[2], &Pos[0], sizeof(float));
        std::memcpy(&Key[3], &Pos[1], sizeof(float));
        std::memcpy(&Key[4], &Pos[2], sizeof(float));
        PlayClientSoundArch(Sound, Key, nullptr, 0);
    }

    // CEShield::cast on the equipment (no test of the result), its vtable +0x84
    // (CAttachedEquip::GetRootIndex) and NeuronRuntime::Engine vtable +0xa0 on that part: the
    // shield's world frame.
    float const* CliShieldWorldFrame(CEquip* Equip)
    {
        CEShield* const Shield = CEShield::cast(Equip);
        long const Root = Shield->GetRootIndex();
        return NeuronRuntime::Engine->GetInstanceTransform(
            reinterpret_cast<void*>(static_cast<intptr_t>(Root)));
    }

    void FCliShieldEquipFx::notify(Event Code, void* Param)
    {
        // (RET 8): a jump table on Code - 0xb (0..8); every path ends in
        // (Code, 0) -- the base notify gets a null Param whatever came in.
        switch (Code)
        {
        case EquipActivating:
            // the player's host (the routine, a pointer compare
            // only) -> "shield_activated".
            if (static_cast<void*>(static_cast<IObjRW*>(Host)) == GetPlayerObjRW())
                NnVoiceEnqueue(G_ShieldActivatedId, 0);
            break;
        case EquipDeactivating:
        {
            // the player's host whose object, narrowed by 0x503 (null
            // when it is not a ship) is not CShip::is_launching (called with no test) ->
            // "shield_deactivated".
            FClientGameobj* const Owner = Host;
            if (static_cast<void*>(static_cast<IObjRW*>(Owner)) != GetPlayerObjRW())
                break;
            CShip* Ship = nullptr;
            if (Owner != nullptr && Owner->Object != nullptr &&
                (static_cast<unsigned>(Owner->Object->ObjectClass) & 0x503u) == 0x503u)
            {
                Ship = static_cast<CShip*>(Owner->Object);
            }
            if (!Ship->is_launching())
                NnVoiceEnqueue(G_ShieldDeactivatedId, 0);
            break;
        }
        case ShieldGeneratorSetChanged:
            // this vtable +0x10, then +0xc.
            ReleaseSounds();
            ReloadSounds();
            break;
        case ShieldOfflineStateChanged:
        {
            // *(bool*)Param and a +0x14 sound -> played at the
            // shield's world position (frame +0x24).
            unsigned char const* const Flag = static_cast<unsigned char const*>(Param);
            if (*Flag != 0 && RebuiltSound != nullptr)
            {
                float const* const Frame = CliShieldWorldFrame(Equip);
                CliShieldPlayAt(RebuiltSound, Frame + 9);
            }
            // then the host's own INotify (EquippedObj +0xac) with the
            // same code and Param.
            static_cast<INotify*>(static_cast<FClientEquippedObj*>(Host))->notify(Code, Param);
            break;
        }
        case ShieldWentOffline:
        {
            // a host already destroyed (vtable +0x154) -> nothing.
            if (Host->Slot154())
                break;
            // the player's live host (not reaped, class 0x503 hit
            // points above 0: FCOMP the field, TEST AH,0x41, JNZ skips) ->
            // "nnv_shield_failed".
            if (CliEquipHostIsLivePlayer(Host, 0x503u))
                NnVoiceEnqueue(G_ShieldFailedId, 0);
            // the shield's world frame.
            float const* const Frame = CliShieldWorldFrame(Equip);
            // with a generator companion -- the owner's velocity
            // (Equip +4, CObject vtable +0x54, read before the id test), then collapse_particle
            // (+0x54) not empty -> the routine and the routine on the field (the frame,
            // the velocity, no companion).
            Archetype::ICliObj* const Comp = CliShieldGenCompanion(Equip);
            if (Comp != nullptr)
            {
                ID_String const* const Particle = reinterpret_cast<ID_String const*>(
                    reinterpret_cast<unsigned char const*>(Comp) + 0x54);
                Vector const Velocity = Equip->Owner->get_velocity();
                if (!Particle->IsEmpty())
                {
                    CliCreateEffectWorldOneShot(FindEffectArch(&Particle->Id),
                                                reinterpret_cast<FTransform const*>(Frame),
                                                reinterpret_cast<float const*>(&Velocity),
                                                nullptr);
                }
            }
            // +0x10 non-null -> played at the frame's position.
            if (CollapseSound != nullptr)
                CliShieldPlayAt(CollapseSound, Frame + 9);
            break;
        }
        case ShieldCanReactivate:
            // the player's live host (class 0x503) ->
            // "shield_restored".
            if (CliEquipHostIsLivePlayer(Host, 0x503u))
                NnVoiceEnqueue(G_ShieldRestoredId, 0);
            break;
        default:
            // 0xd, 0xe, 0xf and anything outside 0xb..0x13.
            break;
        }
        // .
        FClientEquip::notify(Code, nullptr);
    }

    void FCliShieldEquipFx::OnCollisionHit(void const* HitInfo, FClientGameobj* Other,
                                           void const* /*Unused*/)
    {
        // (RET 0xc). 0x0052e77b..0x0052e797: no generator companion -> nothing.
        Archetype::ICliObj* const Comp = CliShieldGenCompanion(Equip);
        if (Comp == nullptr)
            return;
        // |Other's velocity - the host's| (host vtable +4 first, then
        // Other's; dx*dx + dy*dy, then + dz*dz, FSQRT, stored as a float).
        Vector const HostVelocity = Host->get_velocity();
        Vector const OtherVelocity = Other->get_velocity();
        float const Dz = OtherVelocity.z - HostVelocity.z;
        float const Dy = OtherVelocity.y - HostVelocity.y;
        float const Dx = OtherVelocity.x - HostVelocity.x;
        float const Speed = sqrtf(Dx * Dx + Dy * Dy + Dz * Dz);
        // Other's mass (vtable +0x30) * COLLISION_DAMAGE_FACTOR * Speed.
        float const Damage = Other->get_mass() * COLLISION_DAMAGE_FACTOR * Speed;
        // the companion re-read (null tests again) picks the hit effect
        // -- the routine.
        FEffectArch* const HitArch = Archetype::CliShieldGeneratorPickHitEffect(Comp, Damage);
        // a zeroed local companion; the routine(Other's id (vtable
        // +0x20), the arch, &HitInfo[0], &HitInfo[1], &companion) on the host.
        void* Companion[4] = {nullptr, nullptr, nullptr, nullptr};
        Vector const* const Hit = static_cast<Vector const*>(HitInfo);
        Host->SpawnSurfaceHitEffect(Other->get_id(), HitArch, Hit, Hit + 1, Companion);
        // an effect in the companion -> its vtable +0xc (SetIntensity)
        // with the equipment's relative health (vtable +0x44). the routine never writes its
        // fifth argument, so the slot stays null and this does not run.
        FEffectBase* const Effect = static_cast<FEffectBase*>(Companion[2]);
        if (Effect != nullptr)
        {
            typedef void(__thiscall * FSetIntensity)(FEffectBase*, float);
            Effect->SetIntensity(Equip->GetRelativeHealth());
        }
        // the local companion's destructor (the the routine body).
        CliEffectCompanionClear(Companion);
    }

    void FCliShieldEquipFx::OnMunitionHit(void const* HitInfo, FClientGameobj* Munition)
    {
        // (RET 8). 0x0052e8d8..0x0052e8f8: no generator companion -> nothing.
        Archetype::ICliObj* const Comp = CliShieldGenCompanion(Equip);
        if (Comp == nullptr)
            return;
        // the munition's class & 7 object (narrowed twice, no null
        // test before +0x88) and its Munition archetype: hull_damage (+0x80) *
        // ShieldEquipConsts::HULL_DAMAGE_FACTOR + energy_damage (+0x84).
        CObject* const MunObject = Munition->Object;
        CObject* Shot7 = nullptr;
        if (MunObject != nullptr && (static_cast<unsigned>(MunObject->ObjectClass) & 7u) == 7u)
            Shot7 = MunObject;
        Archetype::Munition const* const MunArch =
            static_cast<Archetype::Munition const*>(Shot7->Archetype);
        float const Damage =
            MunArch->HullDamage * ShieldEquipConsts::HULL_DAMAGE_FACTOR + MunArch->EnergyDamage;
        // the routine on the companion.
        FEffectArch* const HitArch = Archetype::CliShieldGeneratorPickHitEffect(Comp, Damage);
        // a zeroed local companion; the routine keyed by the shot's
        // owner (the class & 0x27 object, no test before +0xe4) on the host.
        void* Companion[4] = {nullptr, nullptr, nullptr, nullptr};
        CProjectile const* Shot = nullptr;
        if (MunObject != nullptr &&
            (static_cast<unsigned>(MunObject->ObjectClass) & 0x27u) == 0x27u)
        {
            Shot = static_cast<CProjectile const*>(MunObject);
        }
        Vector const* const Hit = static_cast<Vector const*>(HitInfo);
        Host->SpawnSurfaceHitEffect(Shot->OwnerId, HitArch, Hit, Hit + 1, Companion);
        // as in OnCollisionHit (never runs: the slot stays null).
        FEffectBase* const Effect = static_cast<FEffectBase*>(Companion[2]);
        if (Effect != nullptr)
        {
            typedef void(__thiscall * FSetIntensity)(FEffectBase*, float);
            Effect->SetIntensity(Equip->GetRelativeHealth());
        }
        // a random live effect of the munition archetype's companion
        // (the class & 7 object narrowed again) -- the routine.
        FEffectArch* const LiveEffect =
            Archetype::CliMunitionPickLiveEffect(Shot7->Archetype->Companion);
        // the class & 1 object's vtable +0x54 get_velocity (no test),
        // then the routine(LiveEffect, &HitInfo[0], &HitInfo[1], &velocity, 0) on the host.
        CObject* Shot1 = nullptr;
        if (MunObject != nullptr && (static_cast<unsigned>(MunObject->ObjectClass) & 1u) == 1u)
            Shot1 = MunObject;
        Vector const Velocity = Shot1->get_velocity();
        Host->SpawnSplashHitEffect(LiveEffect, Hit, Hit + 1, &Velocity, nullptr);
        // the local companion's destructor.
        CliEffectCompanionClear(Companion);
    }

    // The thruster's client wrapper (CliThruster.cpp): operator new(0x20), constructor
    // , vtable the routine {notify the routine, scalar deleting dtor
    // (body the routine), Update the routine}.
    struct FCliThrusterEquipFx : FCliEquipFxBase
    {
        // .
        ~FCliThrusterEquipFx() override;
        // (RET 8).
        void notify(Event Code, void* Param) override;

        // +0x10 the 0x10-byte effect companion {arch, ISoundSource*, effect, FGameLight*} that
        // fills with the exhaust particle (0x0052ed40 passes &this+0x10).
        void* Companion[4];
    };
    static_assert(sizeof(FCliThrusterEquipFx) == 0x20, "ThrusterFx 0x20");
    static_assert(offsetof(FCliThrusterEquipFx, Companion) == 0x10, "Thruster +0x10");

    // baseline Archetype::CliEQAttachedFX (vtable 005c7fe0, 0x5c) is private to client_cliobj.cpp
    // (FCliEQAttachedFX); this is its tail from +0x54 as EquippedObj.cpp reads it.
    struct FCliEQAttachedFXView
    {
        ID_String Particles;        // +0x54 "particles"
        bool bUseThrottle;          // +0x58 "use_throttle"
    };

    // The attached-FX equipment's client wrapper: operator new(0x20) in the factory the routine,
    // constructor the routine, vtable the branch {notify the branch, scalar deleting
    // dtor the routine (body the routine -- the same folded body as the light wrapper's),
    // Update the routine}.
    struct FCliAttachedFxEquipFx : FCliEquipFxBase
    {
        // (thiscall, RET 8).
        FCliAttachedFxEquipFx(CEquip* EquipParam, FClientGameobj* HostParam);
        // .
        ~FCliAttachedFxEquipFx() override;
        // (RET 8).
        void notify(Event Code, void* Param) override;
        // (RET 4).
        bool Update(float Dt) override;

        // +0x10 the 0x10-byte effect companion {arch, ISoundSource*, effect, FGameLight*} that
        // fills (0x0053235b passes &this+0x10 as its out-param).
        void* Companion[4];
    };
    static_assert(sizeof(FCliAttachedFxEquipFx) == 0x20, "AttachedFx 0x20");
    static_assert(offsetof(FCliAttachedFxEquipFx, Companion) == 0x10, "AttachedFx +0x10");

    // The repair droid's client wrapper: operator new(0x20), constructor the routine (the base
    // inlined), vtable the branch {notify the branch, scalar deleting dtor
    // (body the routine), Update the routine}.
    struct FCliRepairDroidEquipFx : FCliEquipFxBase
    {
        // .
        ~FCliRepairDroidEquipFx() override;
        // (RET 8).
        void notify(Event Code, void* Param) override;

        // +0x10 the 0x10-byte effect companion {arch, ISoundSource*, effect, FGameLight*} that
        // fills on activation (0x00532b17 passes &this+0x10).
        void* Companion[4];
    };
    static_assert(sizeof(FCliRepairDroidEquipFx) == 0x20, "RepairDroidFx 0x20");
    static_assert(offsetof(FCliRepairDroidEquipFx, Companion) == 0x10, "RepairDroid +0x10");

    // The internal-FX equipment's client wrapper: operator new(0x14), constructor the routine,
    // vtable the branch {notify the branch, scalar deleting dtor the routine (body
    // ), Update the routine (the base's)}.
    struct FCliInternalFxEquipFx : FCliEquipFxBase
    {
        // .
        ~FCliInternalFxEquipFx() override;
        // (RET 8).
        void notify(Event Code, void* Param) override;

        ISoundSource* Voice; // +0x10 Source3D looping on the companion's +0x54 sound
    };
    static_assert(sizeof(FCliInternalFxEquipFx) == 0x14, "InternalFx 0x14");

    // The tractor's client wrapper: operator new(0x38), constructor the routine, vtable
    // {notify the branch, scalar deleting dtor the routine (body
    // ), Update the routine}.
    struct FCliTractorEquipFx : FCliEquipFxBase
    {
        // .
        ~FCliTractorEquipFx() override;
        // (RET 8).
        void notify(Event Code, void* Param) override;
        // (thiscall, no arguments): a fresh tractor beam effect into +0x10 and
        // the host's effect list.
        unsigned char StartBeam();
        // (thiscall, RET 8).
        void DropLoot(unsigned LootId, int Units);

        void* BeamCompanion[4];       // +0x10 {0, 0, the CliTractorBeam, 0} (the routine)
        void* OperatingCompanion[4];  // +0x20 the operating_effect one-shot (the branch 0xb)
        FEffectArch* OperatingEffect; // +0x30 CliEQTractor +0x58, the routine(..., 1)
        void* CompleteSound;          // +0x34 CliEQTractor +0x5c, the routine + vt+0xc
    };
    static_assert(sizeof(FCliTractorEquipFx) == 0x38, "TractorFx 0x38");
    static_assert(offsetof(FCliTractorEquipFx, BeamCompanion) == 0x10, "Tractor +0x10");
    static_assert(offsetof(FCliTractorEquipFx, OperatingCompanion) == 0x20, "Tractor +0x20");
    static_assert(offsetof(FCliTractorEquipFx, OperatingEffect) == 0x30, "Tractor +0x30");

    struct FCliEngineNozzleSlot
    {
        unsigned short SubObjId;    // +0x00
        unsigned short Pad02;
        FParticleEffect* TrailFx;   // +0x04  companion+0x9c
        FParticleEffect* CruiseFx;  // +0x08  companion+0xa0/+0xa4
    };
    static_assert(sizeof(FCliEngineNozzleSlot) == 0xc, "Engine nozzle slot 0xc");

    // The engine wrapper's VC6 std::vector<nozzle slot> at +0x10 {allocator byte, first, last,
    // end}, held as a std::vector: baseline's own member functions on it are the VC6 template
    // instantiations -- insert(pos, n, value) the routine (only ever insert(end, 1, empty) =
    // push_back, from the routine), erase(first, last) the routine -- and the element copy
    // (id, AddRef on both effects) paired with the destroy of the old buffer
    // (release both) leaves every reference count as it was, which is what a bitwise
    // relocation of these plain slots does too. The wrapper is only ever reached by member
    // name in the port, so baseline's field offsets after +0x10 are documentation, not layout.
    // The destructor is the routine: the EH unwind action of both the engine constructor
    // (Unwind@005c0218 -> 0x005c021e, state 1 set at 0x00527bca) and the engine
    // destructor the routine (Unwind@005c0238 -> 0x005c023e); the normal path inlines it at
    // . A member with its own destructor, so every exit runs it.
    struct FCliEngineNozzleVector
    {
        FCliEngineNozzleVector()
        {
        }
        // .
        ~FCliEngineNozzleVector();
        FCliEngineNozzleVector(FCliEngineNozzleVector const&) = delete;
        FCliEngineNozzleVector& operator=(FCliEngineNozzleVector const&) = delete;

        // [first, last) of the slots, as baseline's loops walk them.
        FCliEngineNozzleSlot* First()
        {
            return Slots.data();
        }
        FCliEngineNozzleSlot* Last()
        {
            return Slots.data() + Slots.size();
        }

        std::vector<FCliEngineNozzleSlot> Slots; // baseline +0x04 first, +0x08 last, +0x0c end
    };

    // The engine's client wrapper, the routine, vtable the branch (slot 2 the branch).
    struct FCliEngineEquipFx : FCliEquipFxBase
    {
        // (RET 8) -- the body is the former factory helper, see its definition.
        FCliEngineEquipFx(CEquip* InEquip, FClientGameobj* InHost);
        // (the body of the scalar deleting the routine).
        ~FCliEngineEquipFx() override;
        // (RET 8).
        void notify(Event Code, void* Param) override;
        // (RET 4).
        bool Update(float Dt) override;
        // (thiscall, RET 8): the rumble / character / cruise loop sounds.
        void UpdateSound(float Throttle, float Dt);

        FCliEngineNozzleVector Nozzles;   // +0x10 .. +0x1c
        float FxThrottle;                // +0x20 the throttle the effects follow (the branch)
        float ThrottleSteadyTime;         // +0x24 seconds FxThrottle has not moved
        float CruiseTime;                 // +0x28 seconds the cruise loop has played
        float AttenMod;                   // +0x2c the character loop's attenuation offset
        float CruiseSpeed;                // +0x30
        int CruiseState;                  // +0x34 0..3, set by the routine (notify 0xe)
        ISoundSource* SoundEmitters[9];   // +0x38 … +0x58  the routine
        FEffectArch* TrailEffect;         // +0x5c companion+0xa8
    };
    // baseline sizeof 0x60 (operator new(0x60) in the routine's caller); the offsets on the
    // members above are baseline's -- the std::vector at +0x10 is not VC6-sized, and nothing
    // reaches this wrapper by offset.

    // Companion sound cue CreateID offsets (FCliEngine::ReadUnknown).
    unsigned const kEngineSoundCueOff[9] = {
        0x60u, // rumble_sound
        0x74u, // character_start_sound
        0x78u, // character_loop_sound
        0x84u, // engine_kill_sound
        0x88u, // cruise_start_sound
        0x8cu, // cruise_loop_sound
        0x90u, // cruise_stop_sound
        0x94u, // cruise_disrupt_sound
        0x98u, // cruise_backfire_sound
    };
    unsigned char const kEngineSoundLoop[9] = {1, 0, 1, 0, 0, 1, 0, 0, 0};

    // the routine's lazily acquired 3DMathEngine (0x00528e1c / 0x00528ec9).
    I3DMathEngine* G_EngineNozzleMath = nullptr;

    // (thiscall on the wrapper, RET 4): the average nozzle position in the HOST's
    // frame. Adds into *OutPos (the callers zero it) and returns how many nozzles resolved.
    int CliEngineAvgNozzleLocalPos(FCliEngineEquipFx* Wrapper, float OutPos[3])
    {
        // the equipment's nozzle vector (+0x38 / +0x3c, stride 0x3c,
        // read with no class test): no buffer, or no element, returns 0 with *OutPos untouched.
        std::vector<ExhaustNozzleInfo> const& Nozzles =
            static_cast<CEEngine*>(Wrapper->Equip)->NozzleInfos();
        if (Nozzles.empty())
            return 0;

        // one hardpoint frame shared by the whole loop -- identity
        // (the routine) and a zero position.
        Transform HpXform;
        HpXform.Rotation.E[0][0] = 1.0f;
        HpXform.Rotation.E[0][1] = 0.0f;
        HpXform.Rotation.E[0][2] = 0.0f;
        HpXform.Rotation.E[1][0] = 0.0f;
        HpXform.Rotation.E[1][1] = 1.0f;
        HpXform.Rotation.E[1][2] = 0.0f;
        HpXform.Rotation.E[2][0] = 0.0f;
        HpXform.Rotation.E[2][1] = 0.0f;
        HpXform.Rotation.E[2][2] = 1.0f;
        HpXform.Translation.x = 0.0f;
        HpXform.Translation.y = 0.0f;
        HpXform.Translation.z = 0.0f;

        int Count = 0;
        for (std::vector<ExhaustNozzleInfo>::const_iterator Cur = Nozzles.begin(); Cur != Nozzles.end();
             ++Cur)
        {
            // COMMON ExhaustNozzleInfo::GetHardpointOffset_NS.
            if (!Cur->GetHardpointOffset_NS(HpXform))
                continue;
            // World = instance frame of the nozzle's part (NeuronRuntime::Engine
            // vt+0xa0 with nozzle +8, no test) * hardpoint, through MulTransform (vt+0x1c),
            // copied by the routine.
            float const* const Instance = NeuronRuntime::Engine->GetInstanceTransform(
                reinterpret_cast<void*>(static_cast<intptr_t>(Cur->ParentInstance)));
            if (G_EngineNozzleMath == nullptr)
                G_EngineNozzleMath = Acquire3DMathEngine();
            FTransform Combined;
            G_EngineNozzleMath->MulTransform(Combined, *reinterpret_cast<FTransform const*>(Instance),
                                             *reinterpret_cast<FTransform const*>(&HpXform));
            FTransform const World = Combined;

            // the inverse of the host's frame -- get_orientation
            // (vt+0x10; the Transform it points into carries the position at +0x24): rotation
            // transposed (the routine), position -(M^T * P) (the routine, the routine),
            // packed by the routine.
            Transform const& HostFrame =
                reinterpret_cast<Transform const&>(Wrapper->Host->get_orientation());
            FMatrix const& HostRotation = reinterpret_cast<FMatrix const&>(HostFrame.Rotation);
            FVector HostPos;
            HostPos.X = HostFrame.Translation.x;
            HostPos.Y = HostFrame.Translation.y;
            HostPos.Z = HostFrame.Translation.z;
            FVector Rotated;
            ShipTransposeRotate(Rotated, HostRotation, HostPos); // (0x00528e6b)
            FTransform Inverse;
            for (int Row = 0; Row < 3; ++Row)
            {
                for (int Col = 0; Col < 3; ++Col)
                    Inverse.Rotation.D[Row][Col] = HostRotation.D[Col][Row];
            }
            Inverse.Translation.X = -Rotated.X;
            Inverse.Translation.Y = -Rotated.Y;
            Inverse.Translation.Z = -Rotated.Z;

            // Local = Inverse * World (MulTransform), copied
            // (the routine); its position is added into *OutPos (FLD / FADD / FSTP each) and
            // the count goes up.
            if (G_EngineNozzleMath == nullptr)
                G_EngineNozzleMath = Acquire3DMathEngine();
            FTransform Local;
            G_EngineNozzleMath->MulTransform(Local, Inverse, World);
            FTransform const LocalCopy = Local;
            OutPos[0] = static_cast<float>(static_cast<double>(LocalCopy.Translation.X) +
                                           static_cast<double>(OutPos[0]));
            ++Count;
            OutPos[1] = static_cast<float>(static_cast<double>(LocalCopy.Translation.Y) +
                                           static_cast<double>(OutPos[1]));
            OutPos[2] = static_cast<float>(static_cast<double>(LocalCopy.Translation.Z) +
                                           static_cast<double>(OutPos[2]));
        }

        // FILD count, FDIVR 1.0 (the field) -- no test for 0: with
        // no nozzle resolved this is 1/0 = +inf on the x87 and the sums (0) become NaN, which
        // is what baseline hands on (the routine keeps going for an indestructible engine).
        // Each component is multiplied on the x87 and stored as a float.
        double const Inv = 1.0 / static_cast<double>(Count);
        OutPos[0] = static_cast<float>(Inv * static_cast<double>(OutPos[0]));
        OutPos[1] = static_cast<float>(Inv * static_cast<double>(OutPos[1]));
        OutPos[2] = static_cast<float>(Inv * static_cast<double>(OutPos[2]));
        return Count;
    }

    // on an emitter's own +0x64 archetype: Start(arch, 0).
    void CliEngineStartEmitter(ISoundSource* Emitter)
    {
        Emitter->Start(GetClientEquipSoundEmitterArch(Emitter), 0);
    }

    // The "restart" idiom: IsActive (+0x14) -> SetStartOffset(0) (+0x64), else Start.
    void CliEngineRestartEmitter(ISoundSource* Emitter)
    {
        if (Emitter->IsActive())
            Emitter->SetStartOffset(0);
        else
            CliEngineStartEmitter(Emitter);
    }

    void CliEngineReleaseSoundEmitters(FCliEngineEquipFx* Wrapper)
    {
        // +0x38..+0x58 in order, each when set: Stop (+0x60), Release (+8),
        // clear (every slot is also cleared on the null path).
        for (int Index = 0; Index < 9; ++Index)
        {
            ISoundSource* Src = Wrapper->SoundEmitters[Index];
            if (Src == nullptr)
                continue;
            Src->Stop();
            Src->Release();
            Wrapper->SoundEmitters[Index] = nullptr;
        }
    }

    void CliEngineSoundInit(FCliEngineEquipFx* Wrapper)
    {
        // (thiscall on the wrapper). 0x00528f95..0x00528fcb: the nozzle average
        // (the routine into a zeroed local); none resolved and the archetype not
        // indestructible (+0x84, read with no class test) ends here.
        float NozzleAvg[3] = {0.0f, 0.0f, 0.0f};
        int const NozzleCount = CliEngineAvgNozzleLocalPos(Wrapper, NozzleAvg);
        if (NozzleCount == 0 &&
            !static_cast<Archetype::Engine const*>(Wrapper->Equip->Arch)->bIndestructible)
        {
            return;
        }
        // nothing without NeuronRuntime::Sound.
        if (NeuronRuntime::Sound == nullptr)
            return;

        // identity orientation (the routine), the average as the
        // position.
        FTransform PoseXform;
        std::memset(&PoseXform, 0, sizeof(PoseXform));
        PoseXform.Rotation.D[0][0] = 1.0f;
        PoseXform.Rotation.D[1][1] = 1.0f;
        PoseXform.Rotation.D[2][2] = 1.0f;
        PoseXform.Translation.X = NozzleAvg[0];
        PoseXform.Translation.Y = NozzleAvg[1];
        PoseXform.Translation.Z = NozzleAvg[2];

        // the companion (equipment +0xc, archetype +4; a null
        // equipment gives a null companion that is read anyway): cone angles +0x54 / +0x58 as
        // unsigned ints (zero-extended FILD qword) to float, cone outside volume +0x5c raw.
        unsigned char const* const CompBytes = reinterpret_cast<unsigned char const*>(
            Wrapper->Equip != nullptr ? Wrapper->Equip->Arch->Companion : nullptr);
        float const ConeInner = static_cast<float>(
            static_cast<long long>(*reinterpret_cast<unsigned const*>(CompBytes + 0x54)));
        float const ConeOuter = static_cast<float>(
            static_cast<long long>(*reinterpret_cast<unsigned const*>(CompBytes + 0x58)));
        float const ConeOutsideVol = *reinterpret_cast<float const*>(CompBytes + 0x5c);

        // per cue, in slot order +0x38..+0x58: an ID_String that is
        // not empty is looked up (the routine), and a found archetype gets an emitter
        // (the routine(0x68) + the routine); the slot takes the emitter or null. A made
        // emitter gets the cone pair (the routine) and outside volume (the routine) --
        // except engine_kill (+0x44), cruise_disrupt (+0x54) and cruise_backfire (+0x58) --
        // then SetPose (vt+0x94, the host's get_index per emitter) and SetLooping (vt+0x88).
        static unsigned char const kEngineSoundCone[9] = {1, 1, 1, 0, 1, 1, 1, 0, 0};
        for (int Index = 0; Index < 9; ++Index)
        {
            ID_String const* const Cue =
                reinterpret_cast<ID_String const*>(CompBytes + kEngineSoundCueOff[Index]);
            ISoundSource* Emitter = nullptr;
            if (!Cue->IsEmpty())
            {
                void* const SoundArch = LookupClientSoundArch(Cue);
                if (SoundArch != nullptr)
                    Emitter = static_cast<ISoundSource*>(CreateClientEquipSoundEmitter(SoundArch));
            }
            Wrapper->SoundEmitters[Index] = Emitter;
            if (Emitter == nullptr)
                continue;
            if (kEngineSoundCone[Index] != 0)
            {
                SetClientSoundSourceCone(Emitter, ConeInner, ConeOuter);
                SetClientSoundSourceConeOutsideVolume(Emitter, ConeOutsideVol);
            }
            Emitter->SetPose(static_cast<int>(Wrapper->Host->get_index()),
                             reinterpret_cast<float const*>(&PoseXform));
            Emitter->SetLooping(kEngineSoundLoop[Index]);
        }

        // the equipment's +0x54 (CEEngine::bCruiseActive, no class
        // test): in cruise the cruise loop (+0x4c) starts, otherwise rumble (+0x38), character
        // start (+0x3c) and character loop (+0x40) -- each the routine with its own
        // archetype, when set.
        if (static_cast<CEEngine*>(Wrapper->Equip)->bCruiseActive != 0)
        {
            if (Wrapper->SoundEmitters[5] != nullptr)
                CliEngineStartEmitter(Wrapper->SoundEmitters[5]);
            return;
        }
        if (Wrapper->SoundEmitters[0] != nullptr)
            CliEngineStartEmitter(Wrapper->SoundEmitters[0]);
        if (Wrapper->SoundEmitters[1] != nullptr)
            CliEngineStartEmitter(Wrapper->SoundEmitters[1]);
        if (Wrapper->SoundEmitters[2] != nullptr)
            CliEngineStartEmitter(Wrapper->SoundEmitters[2]);
    }

    // as the routine uses it: insert(end, 1, an empty slot) on the nozzle vector,
    // i.e. push_back. On a reallocation baseline copy-constructs the old slots across
    // (the routine: id, AddRef on both effects) and destroys the originals (release both),
    // which leaves every count as it was -- std::vector relocates the plain slots as they are.
    // The new slot is the empty value: both effects null (its id is a stray stack word the
    // caller overwrites; 0 here).
    FCliEngineNozzleSlot* CliEngineNozzlePushBack(FCliEngineEquipFx* Wrapper)
    {
        FCliEngineNozzleSlot Empty;
        Empty.SubObjId = 0;
        Empty.Pad02 = 0;
        Empty.TrailFx = nullptr;
        Empty.CruiseFx = nullptr;
        Wrapper->Nozzles.Slots.push_back(Empty);
        return &Wrapper->Nozzles.Slots.back();
    }

    // The reference store both arms of the routine inline (0x00528b9d..0x00528bb9 /
    // ): when the effect is not already the slot's, release the old one
    // (clear), store it and AddRef (the routine).
    void CliEngineAssignParticleRef(FParticleEffect*& Ref, FParticleEffect* Effect)
    {
        if (Ref == Effect)
            return;
        if (Ref != nullptr)
        {
            ParticleEffectRelease(Ref);
            Ref = nullptr;
        }
        Ref = Effect;
        ParticleEffectAddRef(Effect);
    }

    // (thiscall on the wrapper): one slot per nozzle whose hardpoint resolves,
    // with a trail and a cruise particle effect.
    void CliEngineAttachExhaustEffects(FCliEngineEquipFx* Wrapper)
    {
        // the companion (equipment +0xc, archetype +4; a null
        // equipment gives a null companion that is read anyway). trail +0x9c, when not
        // empty, through the routine.
        unsigned char const* const CompBytes = reinterpret_cast<unsigned char const*>(
            Wrapper->Equip != nullptr ? Wrapper->Equip->Arch->Companion : nullptr);
        FEffectArch* TrailArch = nullptr;
        FEffectArch* CruiseNpcArch = nullptr;
        FEffectArch* CruisePlayerArch = nullptr;
        ID_String const* const TrailId = reinterpret_cast<ID_String const*>(CompBytes + 0x9c);
        if (!TrailId->IsEmpty())
            TrailArch = FindEffectArch(&TrailId->Id);
        // the host's id (vt+0x20, get_id) against the field:
        // the player's engine takes +0xa4, every other +0xa0.
        if (Wrapper->Host->get_id() != GetPlayerCharacterId())
        {
            ID_String const* const NpcId = reinterpret_cast<ID_String const*>(CompBytes + 0xa0);
            if (!NpcId->IsEmpty())
                CruiseNpcArch = FindEffectArch(&NpcId->Id);
        }
        else
        {
            ID_String const* const PlayerId =
                reinterpret_cast<ID_String const*>(CompBytes + 0xa4);
            if (!PlayerId->IsEmpty())
                CruisePlayerArch = FindEffectArch(&PlayerId->Id);
        }
        // nothing to make ends here.
        if (TrailArch == nullptr && CruiseNpcArch == nullptr && CruisePlayerArch == nullptr)
            return;

        // one hardpoint frame for the whole loop -- identity
        // (the routine), zero position -- and the equipment's nozzle vector (+0x38 / +0x3c,
        // stride 0x3c, no class test).
        Transform HpXform;
        HpXform.Rotation.E[0][0] = 1.0f;
        HpXform.Rotation.E[0][1] = 0.0f;
        HpXform.Rotation.E[0][2] = 0.0f;
        HpXform.Rotation.E[1][0] = 0.0f;
        HpXform.Rotation.E[1][1] = 1.0f;
        HpXform.Rotation.E[1][2] = 0.0f;
        HpXform.Rotation.E[2][0] = 0.0f;
        HpXform.Rotation.E[2][1] = 0.0f;
        HpXform.Rotation.E[2][2] = 1.0f;
        HpXform.Translation.x = 0.0f;
        HpXform.Translation.y = 0.0f;
        HpXform.Translation.z = 0.0f;
        std::vector<ExhaustNozzleInfo> const& Nozzles =
            static_cast<CEEngine*>(Wrapper->Equip)->NozzleInfos();
        for (std::vector<ExhaustNozzleInfo>::const_iterator Cur = Nozzles.begin(); Cur != Nozzles.end();
             ++Cur)
        {
            // COMMON ExhaustNozzleInfo::GetHardpointOffset_NS.
            if (!Cur->GetHardpointOffset_NS(HpXform))
                continue;

            // an empty slot appended (the routine with a local
            // temporary, destroyed after), then the last slot takes the nozzle's +0 word.
            FCliEngineNozzleSlot* const Slot = CliEngineNozzlePushBack(Wrapper);
            Slot->SubObjId = Cur->SubObjId;
            FTransform const* const Local = reinterpret_cast<FTransform const*>(&HpXform);
            int const Part = static_cast<int>(Cur->ParentInstance);

            // trail -- EffectAlloc(0x84) + the routine(0) (a failed
            // allocation leaves null, used anyway), the routine (host, nozzle +8, frame),
            // vtable slot 0 with the arch; on success the slot's +4 takes it; the creation
            // reference is dropped either way.
            if (TrailArch != nullptr)
            {
                FParticleEffect* const Effect = ParticleEffectCreate();
                ParticleEffectAttach(Effect, Wrapper->Host, Part, Local);
                if (ParticleEffectStart(Effect, TrailArch) != 0)
                    CliEngineAssignParticleRef(Slot->TrailFx, Effect);
                ParticleEffectRelease(Effect);
            }
            // cruise -- the NPC arch makes the 0x90 cruise effect
            // (the routine(0), +0x84 = 1.0, vtable the routine), else the player's arch a
            // plain 0x84 one; attach, slot 0, the slot's +8 on success, creation reference
            // dropped.
            FEffectArch* CruiseArch = nullptr;
            FParticleEffect* Cruise = nullptr;
            if (CruiseNpcArch != nullptr)
            {
                Cruise = ParticleEffectCreateCruise();
                CruiseArch = CruiseNpcArch;
            }
            else if (CruisePlayerArch != nullptr)
            {
                Cruise = ParticleEffectCreate();
                CruiseArch = CruisePlayerArch;
            }
            if (CruiseArch != nullptr)
            {
                ParticleEffectAttach(Cruise, Wrapper->Host, Part, Local);
                if (ParticleEffectStart(Cruise, CruiseArch) != 0)
                    CliEngineAssignParticleRef(Slot->CruiseFx, Cruise);
                ParticleEffectRelease(Cruise);
            }
        }
    }

    struct FCliTradeLaneEquipFx;

    // One TLR joint channel: an AnimDB::VChannel (vtable the branch = {Tick the branch,
    // GetId the branch}) carrying the engine child instance (-1 = none) and the wrapper whose
    // spin angle it drives.
    struct FTradeLaneRingChannel : AnimDB::VChannel
    {
        // (RET 4): the owner's spin angle in radians (* ) as joint
        // parameter 0 of GetId()'s instance -- IEngine slot 50 (+0xc8). Dt is not read.
        void Tick(float Dt) override;
        // (ICF-folded, also in 005c8cc8 / 005cacf8): `return [this+4]`.
        int GetId() const override;

        int ChildIndex;                         // +0x04
        FCliTradeLaneEquipFx* Owner;            // +0x08 set by the routine
    };
    static_assert(sizeof(FTradeLaneRingChannel) == 0xc, "TLR channel 0xc");

    // layout 0x34, vtable the routine {notify the routine (the base's),
    // scalar deleting dtor the routine (body the routine), Update the branch}.
    // +0x10 / +0x1c are the two TLR_Left / TLR_Right joint channels.
    struct FCliTradeLaneEquipFx : FCliEquipFxBase
    {
        ~FCliTradeLaneEquipFx() override;
        // (RET 4).
        bool Update(float Dt) override;

        FTradeLaneRingChannel TlrLeft;          // +0x10
        FTradeLaneRingChannel TlrRight;         // +0x1c
        float SpinAngle;                        // +0x28 degrees, wrapped to (.., 360]
        float SpinSpeed;                        // +0x2c degrees/s, starts at CliEQTradeLane +0xac
        // +0x30: ships using the lane -- zeroed by the constructor, stepped +/-1 by the solar's
        // lane switch the routine (INC 0x00516957 / DEC 0x0051695f, store 0x00516962); Update
        // spins the rings up while it is 0 and down otherwise.
        int ShipsUsingLane;
    };
    static_assert(sizeof(FCliTradeLaneEquipFx) == 0x34, "TradeLaneFx 0x34");
    static_assert(offsetof(FCliTradeLaneEquipFx, TlrLeft) == 0x10, "TLR left @+0x10");
    static_assert(offsetof(FCliTradeLaneEquipFx, TlrRight) == 0x1c, "TLR right @+0x1c");
    static_assert(offsetof(FCliTradeLaneEquipFx, SpinAngle) == 0x28, "TradeLane +0x28");
    static_assert(offsetof(FCliTradeLaneEquipFx, ShipsUsingLane) == 0x30, "TradeLane +0x30");

    // .
    float const kTradeLaneDegToRad = 0.01745329238474369f;

    void FTradeLaneRingChannel::Tick(float /*Dt*/)
    {
        float const Radians = Owner->SpinAngle * kTradeLaneDegToRad;
        int const Id = GetId();                                      // vtable +0x04
        NeuronRuntime::Engine->SetJointParams(reinterpret_cast<void*>(static_cast<intptr_t>(Id)), 0,
                                      &Radians);
    }

    int FTradeLaneRingChannel::GetId() const
    {
        return ChildIndex;
    }

    // — walk Engine GetRelatedInstance(OwnerRoot, 0, cursor) for TLR_Left/Right.
    void CliTradeLaneFindTlrHardpoints(FCliTradeLaneEquipFx* Wrapper)
    {
        if (Wrapper == nullptr || Wrapper->Equip == nullptr || NeuronRuntime::Engine == nullptr)
            return;
        CEqObj* Owner = Wrapper->Equip->Owner;
        if (Owner == nullptr)
            return;
        void* Root = reinterpret_cast<void*>(static_cast<ptrdiff_t>(Owner->EngineInstanceId));
        void* Child = NeuronRuntime::Engine->GetRelatedInstance(Root, 0, reinterpret_cast<void*>(-1));
        while (Child != nullptr && Child != G_InvalidEngineHandle &&
               reinterpret_cast<ptrdiff_t>(Child) != -1)
        {
            FEngineName NameOut = {};
            FEngineName* Name = NeuronRuntime::Engine->GetInstanceName(&NameOut, Child);
            char const* Text = "";
            if (Name != nullptr && Name->Text != nullptr)
                Text = Name->Text;
            int const ChildId = static_cast<int>(reinterpret_cast<ptrdiff_t>(Child));
            if (std::strcmp(Text, "TLR_Right") == 0)
            {
                Wrapper->TlrRight.ChildIndex = ChildId;
                Wrapper->TlrRight.Owner = Wrapper;
                if (Wrapper->TlrLeft.ChildIndex != -1)
                    return;
            }
            else if (std::strcmp(Text, "TLR_Left") == 0)
            {
                Wrapper->TlrLeft.ChildIndex = ChildId;
                Wrapper->TlrLeft.Owner = Wrapper;
                if (Wrapper->TlrRight.ChildIndex != -1)
                    return;
            }
            Child = NeuronRuntime::Engine->GetRelatedInstance(Root, 0, Child);
        }
    }

    // (thiscall, no args). Root = Host vt+0x1c get_index() (no test of Host,
    // Equip, arch or companion — all dereferenced unchecked). Lane arch = the routine(companion
    // +0x84): when found, HpRightLane then HpLeftLane (a missing HpRightLane still tries the
    // left one; a missing HpLeftLane falls through to the shield). Shield arch = the routine
    // (companion+0x80) -> HpShield01. Every hit: xf = {hardpoint Orient, Position},
    // (arch, Host, Root, &xf, nullptr) on the field — the third argument is the
    // root index (EBP), not the hardpoint handle FindHardpoint_PS wrote (that out value is unused).
    void CliTradeLaneAttachLaneEffects(FCliTradeLaneEquipFx* Wrapper)
    {
        FClientGameobj* const Host = Wrapper->Host;
        long const Root = Host->get_index();
        unsigned char* const Comp =
            reinterpret_cast<unsigned char*>(Wrapper->Equip->EquipArch()->Companion);

        FEffectArch* const LaneFx = FindEffectArch(reinterpret_cast<unsigned const*>(Comp + 0x84));
        if (LaneFx != nullptr)
        {
            long HpHandle = 0;
            HardpointInfo HpInfo;
            FTransform Xform;
            if (FindHardpoint_PS(Root, "HpRightLane", HpHandle, HpInfo))
            {
                std::memcpy(&Xform.Rotation, &HpInfo.Orient, sizeof(Xform.Rotation));
                Xform.Translation.X = HpInfo.Position.x;
                Xform.Translation.Y = HpInfo.Position.y;
                Xform.Translation.Z = HpInfo.Position.z;
                CliCreateEffectOneShot(LaneFx, Host, static_cast<int>(Root), &Xform, nullptr);
            }
            if (FindHardpoint_PS(Root, "HpLeftLane", HpHandle, HpInfo))
            {
                std::memcpy(&Xform.Rotation, &HpInfo.Orient, sizeof(Xform.Rotation));
                Xform.Translation.X = HpInfo.Position.x;
                Xform.Translation.Y = HpInfo.Position.y;
                Xform.Translation.Z = HpInfo.Position.z;
                CliCreateEffectOneShot(LaneFx, Host, static_cast<int>(Root), &Xform, nullptr);
            }
        }

        FEffectArch* const ShieldFx =
            FindEffectArch(reinterpret_cast<unsigned const*>(Comp + 0x80));
        if (ShieldFx != nullptr)
        {
            long HpHandle = 0;
            HardpointInfo HpInfo;
            if (FindHardpoint_PS(Root, "HpShield01", HpHandle, HpInfo))
            {
                FTransform Xform;
                std::memcpy(&Xform.Rotation, &HpInfo.Orient, sizeof(Xform.Rotation));
                Xform.Translation.X = HpInfo.Position.x;
                Xform.Translation.Y = HpInfo.Position.y;
                Xform.Translation.Z = HpInfo.Position.z;
                CliCreateEffectOneShot(ShieldFx, Host, static_cast<int>(Root), &Xform, nullptr);
            }
        }
    }

    // (the CliEQTradeLane companion's effect AddRef) is Archetype::
    // CliTradeLaneAddEffectRef in client_cliobj.cpp; its release the routine is
    // Archetype::CliTradeLaneReleaseEffectRef.

    void CliEquipFxBaseFill(FCliEquipFxBase* Wrapper, CEquip* Equip, FClientGameobj* Host)
    {
        // (thiscall, RET 8; inlined into most type constructors, e.g.
        // ). No test of Equip: +4 / +8 / +0xc, connect, then an attached
        // equipment's separation_explosion (arch +0x70) resolved, stored, and referenced
        // (the routine) when found.
        Wrapper->Equip = Equip;
        Wrapper->Host = Host;
        Wrapper->SeparationExplosion = nullptr;
        Equip->ConnectClientEquip(Wrapper);
        CAttachedEquip* const Attached = CAttachedEquip::cast(Equip);
        if (Attached != nullptr &&
            !Attached->AttachedEquipArch()->SeparationExplosion.IsEmpty())
        {
            Wrapper->SeparationExplosion =
                Archetype::GetExplosion(Attached->AttachedEquipArch()->SeparationExplosion);
            if (Wrapper->SeparationExplosion != nullptr)
                ExplosionArchAddRef(Wrapper->SeparationExplosion);
        }
    }

    FCliShieldGenEquipFx* CliShieldGenEquipFxConstruct(CEquip* Equip, FClientGameobj* Host)
    {
        // Factory arm 0x00533357: operator new(0x10), null-tested. the routine (RET 8):
        // , then the vtable the routine.
        FCliShieldGenEquipFx* Wrapper = new (std::nothrow) FCliShieldGenEquipFx();
        if (Wrapper == nullptr)
            return nullptr;
        CliEquipFxBaseFill(Wrapper, Equip, Host);
        return Wrapper;
    }

    FCliCloakEquipFx* CliCloakEquipFxConstruct(CEquip* Equip, FClientGameobj* Host)
    {
        // Factory arm 0x0053356e: operator new(0x10), null-tested. the routine (RET 8): the
        // base the routine inlined, then the vtable the branch.
        FCliCloakEquipFx* Wrapper = new (std::nothrow) FCliCloakEquipFx();
        if (Wrapper == nullptr)
            return nullptr;
        CliEquipFxBaseFill(Wrapper, Equip, Host);
        return Wrapper;
    }

    FCliRepairDroidEquipFx* CliRepairDroidEquipFxConstruct(CEquip* Equip, FClientGameobj* Host)
    {
        // Factory arm 0x005335ec: operator new(0x20), null-tested. the routine (RET 8): the
        // base the routine inlined, the companion's four slots zeroed, vtable 005de18c.
        FCliRepairDroidEquipFx* Wrapper = new (std::nothrow) FCliRepairDroidEquipFx();
        if (Wrapper == nullptr)
            return nullptr;
        CliEquipFxBaseFill(Wrapper, Equip, Host);
        Wrapper->Companion[0] = nullptr;
        Wrapper->Companion[1] = nullptr;
        Wrapper->Companion[2] = nullptr;
        Wrapper->Companion[3] = nullptr;
        return Wrapper;
    }

    // / the field / the field, filled by the static initializers
    // / the branch / the branch.
    unsigned const G_RepairDroidActivatedId = CreateID("repair_droid_activated");
    unsigned const G_RepairDroidDeactivatedId = CreateID("repair_droid_deactivated");
    unsigned const G_RepairDroidExhaustedId = CreateID("repair_droid_exhausted");

    FCliRepairDroidEquipFx::~FCliRepairDroidEquipFx()
    {
        // the companion's destructor inlined (particle the routine, sound
        // vt+8, light the routine, each cleared, then slot 0 = 0 -- the body of
        // ); the base destructor follows.
        CliEffectCompanionClear(Companion);
    }

    void FCliRepairDroidEquipFx::notify(Event Code, void* /*Param*/)
    {
        // (RET 8). the routine is never called.
        if (Code == EquipActivating)
        {
            // the companion stopped (the the routine prelude inlined:
            // effect vt+4, sound pose (-1, identity) + Stop, light the routine + vt+8) and
            // cleared (the routine).
            CliEffectCompanionArmStop(Companion);
            CliEffectCompanionClear(Companion);
            // an identity frame; the companion archetype (null without
            // an equipment, read anyway), the host's index (vt+0x1c); a non-empty +0x54 ->
            // and the routine on the field with the host, the index, the frame
            // and &this+0x10.
            FTransform Identity;
            std::memset(&Identity, 0, sizeof(Identity));
            Identity.Rotation.D[0][0] = 1.0f;
            Identity.Rotation.D[1][1] = 1.0f;
            Identity.Rotation.D[2][2] = 1.0f;
            unsigned char const* const Comp = reinterpret_cast<unsigned char const*>(
                Equip != nullptr ? Equip->Arch->Companion : nullptr);
            long const Index = Host->get_index();
            FClientGameobj* const Owner = Host;
            ID_String const* const FxId = reinterpret_cast<ID_String const*>(Comp + 0x54);
            if (!FxId->IsEmpty())
            {
                CliCreateEffectOneShot(FindEffectArch(&FxId->Id), Owner, static_cast<int>(Index),
                                       &Identity, Companion);
            }
            // the player's host (the routine, not reaped vt+0x158)
            // queues "repair_droid_activated".
            if (CliEquipHostIsPlayer(Host))
                NnVoiceEnqueue(G_RepairDroidActivatedId, 0);
            return;
        }
        if (Code == EquipDeactivating)
        {
            // out of the host's effect list (the routine on Host+0x28
            // with &this+0x10).
            CliEffectListRemove(&Host->EffectList, Companion);
            // on the player's host, the equipment's hit points (vtable
            // +0x3c, GetHitPoints; FCOMP 0, TEST AH,0x41, JNZ on <= / NaN) pick
            // "repair_droid_deactivated" (above 0) or "repair_droid_exhausted".
            if (CliEquipHostIsPlayer(Host))
            {
                if (Equip->GetHitPoints() > 0.0f)
                    NnVoiceEnqueue(G_RepairDroidDeactivatedId, 0);
                else
                    NnVoiceEnqueue(G_RepairDroidExhaustedId, 0);
            }
            return;
        }
        if (Code == EquipDestroying)
        {
            // CEquip::NotifyDisconnecting(this) (no test), Equip = 0.
            Equip->NotifyDisconnecting(this);
            Equip = nullptr;
        }
    }

    FCliShieldEquipFx* CliShieldEquipFxConstruct(CEquip* Equip, FClientGameobj* Host)
    {
        // Factory arm: operator new(0x18), null-tested. the routine (RET 8): 0x0052e22a
        // ; 0x0052e23a..0x0052e240 +0x10 / +0x14 zeroed and the vtable
        // ; 0x0052e246..0x0052e299 the two sounds (LoadSounds -- the
        // equipment's +0x70 read with no cast, as in every shield slot).
        FCliShieldEquipFx* Wrapper = new (std::nothrow) FCliShieldEquipFx();
        if (Wrapper == nullptr)
            return nullptr;
        CliEquipFxBaseFill(Wrapper, Equip, Host);
        Wrapper->CollapseSound = nullptr;
        Wrapper->RebuiltSound = nullptr;
        Wrapper->LoadSounds();
        return Wrapper;
    }

    FCliTractorEquipFx* CliTractorEquipFxConstruct(CEquip* Equip, FClientGameobj* Host)
    {
        // Factory arm: operator new(0x38), null-tested. the routine (RET 8): 0x0052f56a
        // ; 0x0052f56f..0x0052f59a +0x10..+0x34 zeroed; 0x0052f59d vtable
        // .
        FCliTractorEquipFx* Wrapper = new (std::nothrow) FCliTractorEquipFx();
        if (Wrapper == nullptr)
            return nullptr;
        CliEquipFxBaseFill(Wrapper, Equip, Host);
        for (int I = 0; I < 4; ++I)
        {
            Wrapper->BeamCompanion[I] = nullptr;
            Wrapper->OperatingCompanion[I] = nullptr;
        }
        Wrapper->OperatingEffect = nullptr;
        Wrapper->CompleteSound = nullptr;

        // Each use re-reads the companion archetype: the equipment (+4) non-null ->
        // [[+4]+0xc]+4, else null -- and the +0x58 / +0x5c address is taken either way.
        unsigned char const* const Comp = reinterpret_cast<unsigned char const*>(
            Wrapper->Equip != nullptr ? Wrapper->Equip->Arch->Companion : nullptr);
        // operating_effect (+0x58) not empty -> the routine into
        // +0x30, the routine(arch, 1).
        ID_String const* const Operating = reinterpret_cast<ID_String const*>(Comp + 0x58);
        if (!Operating->IsEmpty())
        {
            Wrapper->OperatingEffect = FindEffectArch(&Operating->Id);
            EffectArchAddRef(Wrapper->OperatingEffect, 1u);
        }
        // tractor_complete_snd (+0x5c) not empty -> the routine into
        // +0x34, then its vtable +0xc (Acquire) with no test of the result.
        ID_String const* const Complete = reinterpret_cast<ID_String const*>(Comp + 0x5c);
        if (!Complete->IsEmpty())
        {
            Wrapper->CompleteSound = LookupClientSoundArch(Complete);
            static_cast<Archetype::ISound*>(Wrapper->CompleteSound)->Acquire();
        }
        return Wrapper;
    }

    // / the field, filled by the static initializers the branch /
    // .
    unsigned const G_TractorFailureVoiceId = CreateID("nnv_tractor_failure");
    unsigned const G_TargetOutOfRangeVoiceId = CreateID("target_out_of_range");

    FCliTractorEquipFx::~FCliTractorEquipFx()
    {
        // . 0x0052f684..0x0052f695: +0x30 non-null -> the routine.
        if (OperatingEffect != nullptr)
            EffectArchRelease(OperatingEffect);
        // +0x34 non-null -> vtable +0x10 (Release); not cleared.
        if (CompleteSound != nullptr)
            static_cast<Archetype::ISound*>(CompleteSound)->Release();
        // / 0x0052f6dc..0x0052f710: the two companions' destructors
        // inlined in reverse member order (+0x20, then +0x10) -- the the routine body each.
        CliEffectCompanionClear(OperatingCompanion);
        CliEffectCompanionClear(BeamCompanion);
        // the base destructor the routine follows.
    }

    unsigned char FCliTractorEquipFx::StartBeam()
    {
        // . 0x0052fa2b..0x0052fa66: the 0x84 effect (null when the allocation
        // failed -- used anyway).
        FCliTractorBeam* const Beam = CliTractorBeamCreate();
        // the routine(this).
        unsigned char const bOk = CliTractorBeamInit(Beam, this);
        if (bOk != 0)
        {
            // a local companion {0, 0, beam, 0} (its copy takes a
            // reference; the sound / light arms of that copy see zeros), the routine on the
            // host's list (+0x28), the local destroyed.
            void* Local[4] = {nullptr, nullptr, Beam, nullptr};
            if (Beam != nullptr)
                EffectAddRef(Beam);
            CliEffectListInsert(&Host->EffectList, Local);
            if (Local[2] != nullptr)
                EffectRelease(static_cast<FEffectBase*>(Local[2]));

            // a second local {0, 0, beam, 0} (reference), assigned into
            // +0x10 (the old contents released as the routine does, the new beam referenced),
            // then the local released.
            if (Beam != nullptr)
                EffectAddRef(Beam);
            CliEffectCompanionClear(BeamCompanion);
            BeamCompanion[2] = Beam;
            if (Beam != nullptr)
                EffectAddRef(Beam);
            BeamCompanion[1] = nullptr;
            BeamCompanion[3] = nullptr;
            if (Beam != nullptr)
                EffectRelease(Beam);
        }
        // the allocation's own reference, no test.
        EffectRelease(Beam);
        return bOk;
    }

    void FCliTractorEquipFx::DropLoot(unsigned LootId, int Units)
    {
        // (RET 8). 0x0052f9a5..0x0052f9b3: no equipment, or its vtable +8
        // (IsDestroyed) -> nothing.
        if (Equip == nullptr || Equip->IsDestroyed())
            return;
        // CETractor::GetTractorArm(LootId) (no cast); none -> nothing.
        CETractor* const Tractor = static_cast<CETractor*>(Equip);
        TractorArm const* const Arm = Tractor->GetTractorArm(LootId);
        if (Arm == nullptr)
            return;
        // the arm's watched loot ([arm+8] - 0x9c, the GetTarget idiom
        // inlined); with one, its units (+0xf0) minus Units still above zero (SUB / TEST / JG,
        // signed) -> the arm keeps it.
        CLoot const* const Loot = Arm->GetTarget();
        if (Loot != nullptr && static_cast<int>(Loot->Units) - Units > 0)
            return;
        // the player's host (the routine, a pointer compare), or any
        // host outside single player -> CETractor::RemoveTarget(LootId).
        if (static_cast<void*>(static_cast<IObjRW*>(Host)) != GetPlayerObjRW() && SinglePlayer())
            return;
        Tractor->RemoveTarget(LootId);
    }

    void FCliTractorEquipFx::notify(Event Code, void* Param)
    {
        // (RET 8): Code - 0xa checked JA 0xb, a byte map 0x0052f990 into the
        // table 0x0052f978 {0xa, 0xb, 0xc, 0x14, 0x15, default}; every arm ends in
        // with both arguments.
        switch (Code)
        {
        case EquipDestroying:
            // both companions out of the host's list (the routine on
            // Host+0x28, +0x10 then +0x20).
            CliEffectListRemove(&Host->EffectList, BeamCompanion);
            CliEffectListRemove(&Host->EffectList, OperatingCompanion);
            break;
        case EquipActivating:
            // the routine; an operating effect (+0x30) -> an identity
            // frame, the host's index (vtable +0x1c), the routine on the field with the
            // host and &this+0x20.
            StartBeam();
            if (OperatingEffect != nullptr)
            {
                FTransform Identity;
                std::memset(&Identity, 0, sizeof(Identity));
                Identity.Rotation.D[0][0] = 1.0f;
                Identity.Rotation.D[1][1] = 1.0f;
                Identity.Rotation.D[2][2] = 1.0f;
                long const Index = Host->get_index();
                CliCreateEffectOneShot(OperatingEffect, Host, static_cast<int>(Index), &Identity,
                                       OperatingCompanion);
            }
            break;
        case EquipDeactivating:
        {
            // CETractor::GetSourcePos (no cast) into a play key
            // {+0 0, +4 flags (the byte's low three bits cleared, then | 2 -- positional; its
            // upper bits are whatever the stack held, zero here), +8 the position, +0x14 0},
            // (+0x34, &key, 0, 0) with no test of the archetype.
            Vector const Source = static_cast<CETractor*>(Equip)->GetSourcePos();
            unsigned Key[6] = {0u, 2u, 0u, 0u, 0u, 0u};
            std::memcpy(&Key[2], &Source.x, sizeof(float));
            std::memcpy(&Key[3], &Source.y, sizeof(float));
            std::memcpy(&Key[4], &Source.z, sizeof(float));
            PlayClientSoundArch(CompleteSound, Key, nullptr, 0);
            // both companions out of the host's list.
            CliEffectListRemove(&Host->EffectList, BeamCompanion);
            CliEffectListRemove(&Host->EffectList, OperatingCompanion);
            break;
        }
        case Event_0x14:
        {
            // the player's host (the routine, a pointer compare) with no
            // arm active (CETractor::CountActiveArms == 0): the failure code (*Param) 2 ->
            // "target_out_of_range", 3..6 -> "nnv_tractor_failure" (the routine with 0).
            CETractor* const Tractor = static_cast<CETractor*>(Equip);
            if (static_cast<void*>(static_cast<IObjRW*>(Host)) != GetPlayerObjRW())
                break;
            if (Tractor->CountActiveArms() != 0)
                break;
            int const Failure = *static_cast<int const*>(Param);
            if (Failure == 2)
                NnVoiceEnqueue(G_TargetOutOfRangeVoiceId, 0);
            else if (Failure > 2 && Failure <= 6)
                NnVoiceEnqueue(G_TractorFailureVoiceId, 0);
            break;
        }
        case Event_0x15:
        {
            // the same with exactly one arm active and the arm's own
            // error code (Param is the TractorArm, +0x20).
            CETractor* const Tractor = static_cast<CETractor*>(Equip);
            if (static_cast<void*>(static_cast<IObjRW*>(Host)) != GetPlayerObjRW())
                break;
            if (Tractor->CountActiveArms() != 1)
                break;
            int const Failure = static_cast<int>(static_cast<TractorArm const*>(Param)->ErrorCode);
            if (Failure == 2)
                NnVoiceEnqueue(G_TargetOutOfRangeVoiceId, 0);
            else if (Failure > 2 && Failure <= 6)
                NnVoiceEnqueue(G_TractorFailureVoiceId, 0);
            break;
        }
        default:
            break;
        }
        FClientEquip::notify(Code, Param);
    }

    FCliThrusterEquipFx* CliThrusterEquipFxConstruct(CEquip* Equip, FClientGameobj* Host)
    {
        // Factory arm: operator new(0x20), null-tested. the routine (RET 8):
        // the routine; 0x0052ec7b..0x0052ec90 the companion's four slots zeroed;
        // vtable the routine.
        FCliThrusterEquipFx* Wrapper = new (std::nothrow) FCliThrusterEquipFx();
        if (Wrapper == nullptr)
            return nullptr;
        CliEquipFxBaseFill(Wrapper, Equip, Host);
        Wrapper->Companion[0] = nullptr;
        Wrapper->Companion[1] = nullptr;
        Wrapper->Companion[2] = nullptr;
        Wrapper->Companion[3] = nullptr;

        // the companion archetype ([[Equip+0xc]+4], no tests); its
        // hp_particles name (+0x58, a char pointer) empty -> done.
        unsigned char const* const Comp =
            reinterpret_cast<unsigned char const*>(Equip->Arch->Companion);
        char const* const HpName = *reinterpret_cast<char const* const*>(Comp + 0x58);
        if (HpName[0] == '\0')
            return Wrapper;
        // particles (+0x54, ID_String) empty -> done.
        ID_String const* const Particles = reinterpret_cast<ID_String const*>(Comp + 0x54);
        if (Particles->IsEmpty())
            return Wrapper;
        // the equipment's vtable +0x84 (CAttachedEquip::GetRootIndex,
        // no cast check), FindHardpoint_PS(root, name, &handle, &info).
        long const Root = static_cast<CAttachedEquip*>(Equip)->GetRootIndex();
        long HpHandle;
        HardpointInfo HpInfo;
        if (!FindHardpoint_PS(Root, HpName, HpHandle, HpInfo))
        {
            // .
            FDUMP(0x100002, "%s(%d) : *** WARNING: Thruster hardpoint not found: %s\n",
                  "Client\\Space\\CliThruster.cpp", 0x2c, HpName);
            return Wrapper;
        }
        // the frame {hardpoint orientation (MOVSD.REP 9), position}.
        FTransform Xform;
        std::memcpy(&Xform.Rotation, &HpInfo.Orient, sizeof(Xform.Rotation));
        Xform.Translation.X = HpInfo.Position.x;
        Xform.Translation.Y = HpInfo.Position.y;
        Xform.Translation.Z = HpInfo.Position.z;
        // particles tested again.
        if (Particles->IsEmpty())
            return Wrapper;
        // the routine(&particles), the routine on the field with the
        // host, the hardpoint handle, the frame and &this+0x10.
        CliCreateEffectOneShot(FindEffectArch(&Particles->Id), Host, static_cast<int>(HpHandle),
                               &Xform, Wrapper->Companion);
        return Wrapper;
    }

    // , filled by the static initializer the branch.
    unsigned const G_ThrusterDestroyedId = CreateID("thruster_destroyed");

    FCliThrusterEquipFx::~FCliThrusterEquipFx()
    {
        // the companion's destructor inlined (particle +0x18 the routine, sound
        // +0x14 vt+8, light +0x1c the routine, each cleared, then +0x10 = 0 -- the body of
        // ); 0x0052ee1f the base destructor the routine follows.
        CliEffectCompanionClear(Companion);
    }

    void FCliThrusterEquipFx::notify(Event Code, void* Param)
    {
        // (RET 8): a jump table on Code - 0xa (0..5, JA to the default); every arm
        // ends in the routine with both arguments.
        switch (Code)
        {
        case EquipDestroying:
            // the player's live host (class 0x503) queues
            // "thruster_destroyed"; 0x0052eec4..0x0052eece out of the host's effect list
            // (the routine on Host+0x28 with &this+0x10).
            if (CliEquipHostIsLivePlayer(Host, 0x503u))
                NnVoiceEnqueue(G_ThrusterDestroyedId, 0);
            CliEffectListRemove(&Host->EffectList, Companion);
            break;
        case EquipActivating:
        case EquipDeactivating:
        {
            // / 0x0052ef09..0x0052ef14: the particle (+0x18) non-null ->
            // its slot +0xc (SetIntensity) with 1.0f (0x3f800000) on activation, 0 on
            // deactivation.
            FEffectBase* const Effect = static_cast<FEffectBase*>(Companion[2]);
            if (Effect != nullptr)
            {
                typedef void(__thiscall * FSetIntensity)(FEffectBase*, float);
                Effect->SetIntensity(Code == EquipActivating ? 1.0f : 0.0f);
            }
            break;
        }
        case Event_0xF:
        {
            // the host object's owner (class & 3 == 3 -> CSimple +0xb4,
            // else the routine = the field); the local player's -> the host's IObjAffect face
            // (+4) slot +0x54 (false, the field).
            CObject* const Obj = Host->Object;
            unsigned const Owner =
                (Obj != nullptr && (static_cast<unsigned>(Obj->ObjectClass) & 3u) == 3u)
                    ? static_cast<CSimple const*>(Obj)->OwnerPlayer
                    : GetPlayerConnectionId();
            if (Owner == GetPlayerConnectionId())
                static_cast<IObjAffect*>(Host)->slot_54(
                    false, static_cast<int>(GetPlayerConnectionId()));
            break;
        }
        default:
            // 0xd, 0xe and everything out of range: the base only.
            break;
        }
        FClientEquip::notify(Code, Param);
    }

    FCliEQAttachedFXView const* CompanionAsAttachedFX(Archetype::ICliObj* Companion)
    {
        return reinterpret_cast<FCliEQAttachedFXView const*>(
            reinterpret_cast<unsigned char const*>(Companion) + 0x54);
    }

    FCliAttachedFxEquipFx::FCliAttachedFxEquipFx(CEquip* EquipParam, FClientGameobj* HostParam)
    {
        // the inlined base (the routine).
        CliEquipFxBaseFill(this, EquipParam, HostParam);

        // the companion's constructor zeroes all four slots.
        Companion[0] = nullptr;
        Companion[1] = nullptr;
        Companion[2] = nullptr;
        Companion[3] = nullptr;

        // the CliEQAttachedFX (null without an equipment -- baseline then
        // reads +0x54 off null); an empty "particles" ends here.
        Archetype::ICliObj* const Comp = (Equip != nullptr) ? Equip->Arch->Companion : nullptr;
        if (CompanionAsAttachedFX(Comp)->Particles.IsEmpty())
            return;

        // equipment vtable +0x68 (CExternalEquip::GetHardPointInfo) --
        // result not tested -- and the hardpoint as the local frame.
        CExternalEquip const* const External = static_cast<CExternalEquip const*>(Equip);
        HardpointInfo Hardpoint;
        External->GetHardPointInfo(Hardpoint);
        FTransform Local;
        std::memcpy(&Local.Rotation, &Hardpoint.Orient, sizeof(Local.Rotation));
        Local.Translation.X = Hardpoint.Position.x;
        Local.Translation.Y = Hardpoint.Position.y;
        Local.Translation.Z = Hardpoint.Position.z;

        // the companion again, vtable +0x64 (GetParentConnector(false)),
        // and a second IsEmpty on the same "particles".
        Archetype::ICliObj* const Comp2 = (Equip != nullptr) ? Equip->Arch->Companion : nullptr;
        long const Part = External->GetParentConnector(false);
        FCliEQAttachedFXView const* const View = CompanionAsAttachedFX(Comp2);
        if (View->Particles.IsEmpty())
            return;

        // the routine on the id, then the routine on the field
        // with the host, the part, the frame and &this+0x10.
        CliCreateEffectOneShot(FindEffectArch(&View->Particles.Id), Host, static_cast<int>(Part),
                               &Local, Companion);
    }

    FCliAttachedFxEquipFx::~FCliAttachedFxEquipFx()
    {
        // (shared with the light wrapper): the companion's destructor, then the
        // inlined base destructor (~FClientEquip).
        CliEffectCompanionClear(Companion);
    }

    void FCliAttachedFxEquipFx::notify(Event Code, void* /*Param*/)
    {
        // (RET 8): code 0xa only -- out of the host's effect list (the routine on
        // Host+0x28 with &this+0x10), then CEquip::NotifyDisconnecting(this), Equip = 0.
        if (Code == EquipDestroying)
        {
            CliEffectListRemove(&Host->EffectList, Companion);
            Equip->NotifyDisconnecting(this);
            Equip = nullptr;
        }
    }

    bool FCliAttachedFxEquipFx::Update(float /*Dt*/)
    {
        // (RET 4). A live equipment (vtable +8, IsDestroyed, false) with a particle
        // in the companion's effect slot (+0x18) whose CliEQAttachedFX has use_throttle, on a
        // ship host: the particle's slot +0xc (SetIntensity) gets CShip::Throttle (+0x29c).
        if (Equip != nullptr && !Equip->IsDestroyed() && Companion[2] != nullptr)
        {
            Archetype::ICliObj* const Comp = (Equip != nullptr) ? Equip->Arch->Companion : nullptr;
            if (CompanionAsAttachedFX(Comp)->bUseThrottle && Host != nullptr &&
                Host->Object != nullptr &&
                (static_cast<unsigned>(Host->Object->ObjectClass) & 0x503u) == 0x503u)
            {
                CShip const* const Ship = static_cast<CShip const*>(Host->Object);
                FEffectBase* const Effect = static_cast<FEffectBase*>(Companion[2]);
                typedef void(__thiscall * FSetIntensity)(FEffectBase*, float);
                Effect->SetIntensity(Ship->Throttle);
            }
        }
        // .
        return Equip != nullptr;
    }

    FCliAttachedFxEquipFx* CliAttachedFxEquipFxConstruct(CEquip* Equip, FClientGameobj* Host)
    {
        // The factory's attached-FX arm (the routine, 0x0053325c): operator new(0x20),
        // .
        return new FCliAttachedFxEquipFx(Equip, Host);
    }

    FCliInternalFxEquipFx* CliInternalFxEquipFxConstruct(CEquip* Equip, FClientGameobj* Host)
    {
        // (RET 8). 0x00532548..0x005325bd: the inlined base (the routine) --
        // CEquip::Connect, the separation explosion (the routine).
        FCliInternalFxEquipFx* Wrapper = new (std::nothrow) FCliInternalFxEquipFx();
        if (Wrapper == nullptr)
            return nullptr;
        CliEquipFxBaseFill(Wrapper, Equip, Host);
        // +0x10 = 0, vtable the branch.
        Wrapper->Voice = nullptr;

        // the companion (a null equipment gives a null companion,
        // read anyway); an empty +0x54 or an unknown archetype (the routine) ends here.
        unsigned char const* const Comp = reinterpret_cast<unsigned char const*>(
            Wrapper->Equip != nullptr ? Wrapper->Equip->Arch->Companion : nullptr);
        ID_String const* const SoundId = reinterpret_cast<ID_String const*>(Comp + 0x54);
        if (SoundId->IsEmpty())
            return Wrapper;
        void* const SoundArch = LookupClientSoundArch(SoundId);
        if (SoundArch == nullptr)
            return Wrapper;

        // a Source3D (the routine(0x64) + the routine(1) + the 3D
        // fields, vtable the branch) into +0x10 -- a failed allocation stores null and is
        // used anyway.
        ISoundSource* const Voice = static_cast<ISoundSource*>(CreateClientSoundSource3D());
        Wrapper->Voice = Voice;
        // SetLooping(1) (vt+0x88); SetPose (vt+0x94) with the host's
        // get_index (vt+0x1c) and an identity frame; Start (vt+0x58) with the archetype, 0.
        Voice->SetLooping(1);
        FTransform IdentityXform;
        std::memset(&IdentityXform, 0, sizeof(IdentityXform));
        IdentityXform.Rotation.D[0][0] = 1.0f;
        IdentityXform.Rotation.D[1][1] = 1.0f;
        IdentityXform.Rotation.D[2][2] = 1.0f;
        Voice->SetPose(static_cast<int>(Host->get_index()),
                       reinterpret_cast<float const*>(&IdentityXform));
        Voice->Start(SoundArch, 0);
        return Wrapper;
    }

    FCliEngineEquipFx::FCliEngineEquipFx(CEquip* InEquip, FClientGameobj* InHost)
    {
        // (RET 8). Its EH states: 0 after the base part (unwind the routine =
        // ~FClientEquip), 1 after the nozzle vector (unwind the routine) -- a real constructor,
        // so a throw from the body below destroys Nozzles and then the base, in that order.
        FCliEngineEquipFx* const Wrapper = this;
        // the routine (Equip, Host).
        CliEquipFxBaseFill(Wrapper, InEquip, InHost);
        // +0x10 is the nozzle vector's (empty) allocator -- baseline
        // copies a stray stack byte into it; nothing reads it. +0x14..+0x1c (the vector,
        // FCliEngineNozzleVector's constructor) and +0x20..+0x5c are zeroed, the vtable
        // (the routine) is stamped.
        Wrapper->FxThrottle = 0.0f;
        Wrapper->ThrottleSteadyTime = 0.0f;
        Wrapper->CruiseTime = 0.0f;
        Wrapper->AttenMod = 0.0f;
        Wrapper->CruiseSpeed = 0.0f;
        Wrapper->CruiseState = 0;
        for (int I = 0; I < 9; ++I)
            Wrapper->SoundEmitters[I] = nullptr;
        Wrapper->TrailEffect = nullptr;

        // the host's CShip (class 0x503, else null -- COMMON
        // CShip::get_speed is called on it regardless), get_speed(1.0, engine) into +0x30.
        CShip* Ship = nullptr;
        if (Wrapper->Host != nullptr && Wrapper->Host->Object != nullptr &&
            (static_cast<unsigned>(Wrapper->Host->Object->ObjectClass) & 0x503u) == 0x503u)
        {
            Ship = static_cast<CShip*>(Wrapper->Host->Object);
        }
        Wrapper->CruiseSpeed = Ship->get_speed(1.0f, ThrustEquipEngine);
        // with game option 0x2d ("[AUDIO]", the routine) at 2, the
        // same ship re-derived and get_speed(1.0, thruster) replaces it.
        if (GetGameOption(kGameOptAudioGroup) == 2)
        {
            CShip* ThrusterShip = nullptr;
            if (Wrapper->Host != nullptr && Wrapper->Host->Object != nullptr &&
                (static_cast<unsigned>(Wrapper->Host->Object->ObjectClass) & 0x503u) == 0x503u)
            {
                ThrusterShip = static_cast<CShip*>(Wrapper->Host->Object);
            }
            Wrapper->CruiseSpeed = ThrusterShip->get_speed(1.0f, ThrustEquipThruster);
        }

        // the companion's +0xa8 ID_String (a null equipment gives a
        // null companion, read anyway); not empty -> the routine into +0x5c and
        // (EffectArch AddRef, argument 0) on it with no null test.
        unsigned char const* const CompBytes = reinterpret_cast<unsigned char const*>(
            Wrapper->Equip != nullptr ? Wrapper->Equip->Arch->Companion : nullptr);
        ID_String const* const BackfireId = reinterpret_cast<ID_String const*>(CompBytes + 0xa8);
        if (!BackfireId->IsEmpty())
        {
            Wrapper->TrailEffect = FindEffectArch(&BackfireId->Id);
            EffectArchAddRef(Wrapper->TrailEffect, 0);
        }

        // the routine, the routine.
        CliEngineAttachExhaustEffects(Wrapper);
        CliEngineSoundInit(Wrapper);
    }

    FCliEngineEquipFx* CliEngineEquipFxConstruct(CEquip* Equip, FClientGameobj* Host)
    {
        // operator new(0x60) (MSVCRT: null on failure, tested), then
        // on it.
        return new (std::nothrow) FCliEngineEquipFx(Equip, Host);
    }

    FCliTradeLaneEquipFx* CliTradeLaneEquipFxConstruct(CEquip* Equip, FClientGameobj* Host)
    {
        // — TradeLane 0x800000. Wires the routine / the routine / the routine.
        FCliTradeLaneEquipFx* Wrapper = new (std::nothrow) FCliTradeLaneEquipFx();
        if (Wrapper == nullptr)
            return nullptr;
        CliEquipFxBaseFill(Wrapper, Equip, Host);
        // both channels' vtable, +0x2c / +0x28 / +0x30 = 0, both child
        // indices -1, then +0x2c = CliEQTradeLane +0xac (spin_max) with no test. The channels'
        // owners (+0x18 / +0x24) are left for the routine.
        // (the channel vtables are stamped by the FTradeLaneRingChannel member constructors)
        Wrapper->TlrLeft.Owner = nullptr;
        Wrapper->TlrRight.Owner = nullptr;
        Wrapper->SpinSpeed = 0.0f;
        Wrapper->SpinAngle = 0.0f;
        Wrapper->ShipsUsingLane = 0;
        Wrapper->TlrLeft.ChildIndex = -1;
        Wrapper->TlrRight.ChildIndex = -1;
        Wrapper->SpinSpeed = *reinterpret_cast<float const*>(
            reinterpret_cast<unsigned char const*>(Equip->Arch->Companion) + 0xac);
        CliTradeLaneFindTlrHardpoints(Wrapper);
        CliTradeLaneAttachLaneEffects(Wrapper);
        // the routine on the companion, no test.
        Archetype::CliTradeLaneAddEffectRef(Equip->EquipArch()->Companion);
        return Wrapper;
    }

    // .
    float const kTradeLaneFullTurn = 360.0f;

    bool FCliTradeLaneEquipFx::Update(float Dt)
    {
        // (RET 4). Nothing without the TRADE_LANES option (the routine(0x2a)).
        if (GetGameOption(kGameOptTradeLanes) != 0)
        {
            // the companion (no test); the speed step is
            // spin_accel (+0xa8) * Dt. +0x30 clear: speed up, capped at spin_max (+0xac) --
            // FCOMP, TEST AH,0x41, JNZ skips the cap on <= / NaN. +0x30 set: slow down, floored
            // at 0 (the field) -- TEST AH,5, JP skips the floor on >= / NaN. The compare is
            // on the unrounded x87 value, the store (FST) is the float.
            unsigned char const* const Comp =
                reinterpret_cast<unsigned char const*>(Equip->Arch->Companion);
            float const SpinAccel = *reinterpret_cast<float const*>(Comp + 0xa8);
            float const SpinMax = *reinterpret_cast<float const*>(Comp + 0xac);
            double const Step = static_cast<double>(SpinAccel) * static_cast<double>(Dt);
            if (ShipsUsingLane == 0)
            {
                double const Speed = static_cast<double>(SpinSpeed) + Step;
                SpinSpeed = static_cast<float>(Speed);
                if (Speed > static_cast<double>(SpinMax))
                    SpinSpeed = SpinMax;
            }
            else
            {
                double const Speed = static_cast<double>(SpinSpeed) - Step;
                SpinSpeed = static_cast<float>(Speed);
                if (Speed < 0.0)
                    SpinSpeed = 0.0f;
            }

            // angle += Dt * speed (FST), then while it is above 360
            // (TEST AH,0x41, JZ loops on >), subtract 360 and store once at the end.
            double Angle = static_cast<double>(Dt) * static_cast<double>(SpinSpeed) +
                           static_cast<double>(SpinAngle);
            SpinAngle = static_cast<float>(Angle);
            if (Angle > static_cast<double>(kTradeLaneFullTurn))
            {
                do
                {
                    Angle = Angle - static_cast<double>(kTradeLaneFullTurn);
                } while (Angle > static_cast<double>(kTradeLaneFullTurn));
                SpinAngle = static_cast<float>(Angle);
            }

            // each bound joint channel is registered for this frame.
            if (TlrLeft.ChildIndex != -1)
                AnimDB::Add(&TlrLeft);
            if (TlrRight.ChildIndex != -1)
                AnimDB::Add(&TlrRight);
        }
        // MOV AL,1 on both paths.
        return true;
    }

    // The derived destructors that carry work of their own. They used to be branches of a
    // dynamic_cast chain here; with the ClientEquip vtable (slot 1) they are the classes' own
    // destructors, and ~FClientEquip (the routine) runs after each -- which is also where the
    // separation explosion's the routine now happens (the InternalFX branch used to do it by
    // hand, and the other classes leaked the reference).
    FCliInternalFxEquipFx::~FCliInternalFxEquipFx()
    {
        // the voice, when set, Stop (vt+0x60), Release (vt+8), cleared; then the
        // inlined base destructor (~FClientEquip: NotifyDisconnecting, the routine).
        if (Voice != nullptr)
        {
            Voice->Stop();
            Voice->Release();
            Voice = nullptr;
        }
    }

    void FCliInternalFxEquipFx::notify(Event Code, void* /*Param*/)
    {
        // (RET 8): code 0xa only -- the voice stopped (vt+0x60) when set, then
        // CEquip::NotifyDisconnecting(this) with no test of the equipment and Equip = 0. The
        // base the routine is not called (its body is what the tail repeats).
        if (Code == EquipDestroying)
        {
            if (Voice != nullptr)
                Voice->Stop();
            Equip->NotifyDisconnecting(this);
            Equip = nullptr;
        }
    }

    // , filled by the static initializer the branch.
    unsigned const G_EngineDestroyedId = CreateID("engine_destroyed");

    typedef void(__thiscall * FCliEngineParticleKill)(FParticleEffect*);

    // Vtable +4 (the effect's kill) on a nozzle effect.
    void CliEngineKillParticle(FParticleEffect* Effect)
    {
        Effect->Kill();
    }

    // (thiscall on the reference): release when set (the routine), then clear.
    void CliEngineReleaseParticleRef(FParticleEffect*& Ref)
    {
        if (Ref != nullptr)
        {
            ParticleEffectRelease(Ref);
            Ref = nullptr;
        }
    }

    // the nozzle slot's destructor -- the cruise reference (+8), then the trail
    // reference (+4), each through the the routine body.
    void CliEngineNozzleSlotDestruct(FCliEngineNozzleSlot* Slot)
    {
        CliEngineReleaseParticleRef(Slot->CruiseFx);
        CliEngineReleaseParticleRef(Slot->TrailFx);
    }

    // (thiscall on the vector at +0x10): erase(begin, end). The copy-down loop
    // over [end, end) moves nothing, so what is left is the element destructor on every slot
    // and End = Begin.
    void CliEngineClearNozzles(FCliEngineEquipFx* Wrapper)
    {
        FCliEngineNozzleVector& Vec = Wrapper->Nozzles;
        for (FCliEngineNozzleSlot* Slot = Vec.First(); Slot != Vec.Last(); ++Slot)
            CliEngineNozzleSlotDestruct(Slot);
        Vec.Slots.clear();
    }

    FCliEngineNozzleVector::~FCliEngineNozzleVector()
    {
        // (thiscall on the vector, 0x005298e0..0x00529918): the routine on every
        // slot of [first, last), operator delete(first) (the routine, null-safe), then first,
        // last and end = 0. Also the inlined tail of the routine (0x00527dfd..0x00527e4a).
        // The buffer itself goes with the std::vector member.
        for (FCliEngineNozzleSlot* Slot = First(); Slot != Last(); ++Slot)
            CliEngineNozzleSlotDestruct(Slot);
    }

    // (RET 4): notify 0xb passes 0, 0xc passes 1.
    void CliEngineSetActiveSounds(FCliEngineEquipFx* Wrapper, unsigned char bDeactivating)
    {
        ISoundSource** const Snd = Wrapper->SoundEmitters;
        if (bDeactivating != 0)
        {
            // stop rumble (+0x38), character loop (+0x40) and
            // character start (+0x3c).
            if (Snd[0] != nullptr)
                Snd[0]->Stop();
            if (Snd[2] != nullptr)
                Snd[2]->Stop();
            if (Snd[1] != nullptr)
                Snd[1]->Stop();
            // a cruise stop sound (+0x50) still playing ends here.
            if (Snd[6] != nullptr && Snd[6]->IsActive())
                return;
            // the engine kill sound (+0x44), restarted.
            if (Snd[3] == nullptr)
                return;
            CliEngineRestartEmitter(Snd[3]);
            return;
        }
        // stop the kill sound, start rumble and character loop (no
        // IsActive test), restart the character start sound.
        if (Snd[3] != nullptr)
            Snd[3]->Stop();
        if (Snd[0] != nullptr)
            CliEngineStartEmitter(Snd[0]);
        if (Snd[2] != nullptr)
            CliEngineStartEmitter(Snd[2]);
        if (Snd[1] == nullptr)
            return;
        CliEngineRestartEmitter(Snd[1]);
    }

    // (RET 4): notify 0xe, the new cruise state. The old one is +0x34.
    void CliEngineSetCruiseState(FCliEngineEquipFx* Wrapper, int State)
    {
        ISoundSource** const Snd = Wrapper->SoundEmitters;
        // CMP 3, JA -- above 3 (unsigned) takes caseD_4.
        switch (static_cast<unsigned>(State))
        {
        case 0:
        {
            // only from state 3 or 2; anything else is caseD_4.
            int const Old = Wrapper->CruiseState;
            if (Old != 3 && Old != 2)
                break;
            if (Old == 3)
            {
                // cruise stop (+0x50) restarted, cruise loop (+0x4c)
                // stopped.
                if (Snd[6] != nullptr)
                    CliEngineRestartEmitter(Snd[6]);
                if (Snd[5] != nullptr)
                    Snd[5]->Stop();
            }
            else
            {
                // cruise start (+0x48) stopped.
                if (Snd[4] != nullptr)
                    Snd[4]->Stop();
            }
            // rumble and character loop started when idle.
            if (Snd[0] != nullptr && !Snd[0]->IsActive())
                CliEngineStartEmitter(Snd[0]);
            if (Snd[2] != nullptr && !Snd[2]->IsActive())
                CliEngineStartEmitter(Snd[2]);
            break;
        }
        case 1:
        {
            if (Wrapper->CruiseState == 3)
            {
                // cruise loop stopped, disrupt (+0x54) restarted.
                if (Snd[5] != nullptr)
                    Snd[5]->Stop();
                if (Snd[7] != nullptr)
                    CliEngineRestartEmitter(Snd[7]);
            }
            else
            {
                // cruise start stopped, backfire (+0x58) restarted.
                if (Snd[4] != nullptr)
                    Snd[4]->Stop();
                if (Snd[8] != nullptr)
                    CliEngineRestartEmitter(Snd[8]);
            }
            // rumble and character loop started when idle.
            if (Snd[0] != nullptr && !Snd[0]->IsActive())
                CliEngineStartEmitter(Snd[0]);
            if (Snd[2] != nullptr && !Snd[2]->IsActive())
                CliEngineStartEmitter(Snd[2]);
            // with a backfire effect (+0x5c), one one-shot per
            // nozzle whose hardpoint resolves (COMMON ExhaustNozzleInfo::GetHardpointOffset_NS
            // into one identity frame shared by the loop), the routine on the field with
            // the host, the nozzle's +8 part and no companion. The equipment's nozzle vector
            // (+0x38 / +0x3c, stride 0x3c) is read without a class test.
            if (Wrapper->TrailEffect == nullptr)
                break;
            Transform Xform;
            Xform.Rotation.E[0][0] = 1.0f;
            Xform.Rotation.E[0][1] = 0.0f;
            Xform.Rotation.E[0][2] = 0.0f;
            Xform.Rotation.E[1][0] = 0.0f;
            Xform.Rotation.E[1][1] = 1.0f;
            Xform.Rotation.E[1][2] = 0.0f;
            Xform.Rotation.E[2][0] = 0.0f;
            Xform.Rotation.E[2][1] = 0.0f;
            Xform.Rotation.E[2][2] = 1.0f;
            Xform.Translation.x = 0.0f;
            Xform.Translation.y = 0.0f;
            Xform.Translation.z = 0.0f;
            std::vector<ExhaustNozzleInfo> const& Nozzles =
                static_cast<CEEngine*>(Wrapper->Equip)->NozzleInfos();
            for (std::vector<ExhaustNozzleInfo>::const_iterator Cur = Nozzles.begin();
                 Cur != Nozzles.end(); ++Cur)
            {
                if (Cur->GetHardpointOffset_NS(Xform))
                {
                    CliCreateEffectOneShot(Wrapper->TrailEffect, Wrapper->Host,
                                           static_cast<int>(Cur->ParentInstance),
                                           reinterpret_cast<FTransform const*>(&Xform), nullptr);
                }
            }
            break;
        }
        case 2:
            // stop rumble, character start, character loop and kill;
            // restart the cruise start sound (+0x48).
            if (Snd[0] != nullptr)
                Snd[0]->Stop();
            if (Snd[1] != nullptr)
                Snd[1]->Stop();
            if (Snd[2] != nullptr)
                Snd[2]->Stop();
            if (Snd[3] != nullptr)
                Snd[3]->Stop();
            if (Snd[4] != nullptr)
                CliEngineRestartEmitter(Snd[4]);
            break;
        case 3:
            // the same four stops, the cruise loop (+0x4c) started
            // with no IsActive test -- and only the state is stored: this arm returns without
            // the +0x28 reset (null loop: the branch, also past it).
            if (Snd[0] != nullptr)
                Snd[0]->Stop();
            if (Snd[1] != nullptr)
                Snd[1]->Stop();
            if (Snd[2] != nullptr)
                Snd[2]->Stop();
            if (Snd[3] != nullptr)
                Snd[3]->Stop();
            if (Snd[5] != nullptr)
                CliEngineStartEmitter(Snd[5]);
            Wrapper->CruiseState = State;
            return;
        default:
            break;
        }
        // caseD_4 and every other exit: +0x28 = 0, +0x34 = the new state.
        Wrapper->CruiseTime = 0.0f;
        Wrapper->CruiseState = State;
    }

    // notify 0xd, after a nozzle's effects went. Re-seats every emitter on the
    // average of the nozzles that are left (the routine), and drops all of them when none
    // is left and the engine is not indestructible (archetype +0x84, read without a test).
    void CliEngineRepositionSounds(FCliEngineEquipFx* Wrapper)
    {
        float Avg[3] = {0.0f, 0.0f, 0.0f};
        int const Count = CliEngineAvgNozzleLocalPos(Wrapper, Avg);
        if (Count == 0 &&
            !static_cast<Archetype::Engine const*>(Wrapper->Equip->Arch)->bIndestructible)
        {
            CliEngineReleaseSoundEmitters(Wrapper);
        }
        // identity orientation, the average as the position.
        FTransform Pose;
        std::memset(&Pose, 0, sizeof(Pose));
        Pose.Rotation.D[0][0] = 1.0f;
        Pose.Rotation.D[1][1] = 1.0f;
        Pose.Rotation.D[2][2] = 1.0f;
        Pose.Translation.X = Avg[0];
        Pose.Translation.Y = Avg[1];
        Pose.Translation.Z = Avg[2];
        // each emitter, when set, gets vtable +0x94 (SetPose) with
        // the host's index (vtable +0x1c) and the frame.
        for (int Index = 0; Index < 9; ++Index)
        {
            ISoundSource* const Emitter = Wrapper->SoundEmitters[Index];
            if (Emitter != nullptr)
            {
                Emitter->SetPose(static_cast<int>(Wrapper->Host->get_index()),
                                 reinterpret_cast<float const*>(&Pose));
            }
        }
    }

    void FCliEngineEquipFx::notify(Event Code, void* Param)
    {
        // (RET 8): codes 0xa..0xe (ADD -0xa, CMP 4, JA), then the routine with
        // both arguments on every path.
        switch (Code)
        {
        case EquipDestroying:
        {
            // per nozzle, the trail (+4) is killed (vtable +4),
            // released and cleared; the cruise (+8) is killed, then released through
            // .
            for (FCliEngineNozzleSlot* Slot = Nozzles.First(); Slot != Nozzles.Last(); ++Slot)
            {
                if (Slot->TrailFx != nullptr)
                {
                    CliEngineKillParticle(Slot->TrailFx);
                    if (Slot->TrailFx != nullptr)
                    {
                        ParticleEffectRelease(Slot->TrailFx);
                        Slot->TrailFx = nullptr;
                    }
                }
                if (Slot->CruiseFx != nullptr)
                {
                    CliEngineKillParticle(Slot->CruiseFx);
                    CliEngineReleaseParticleRef(Slot->CruiseFx);
                }
            }
            // the routine on the vector, the routine.
            CliEngineClearNozzles(this);
            CliEngineReleaseSoundEmitters(this);
            // the player's host (the routine, a pointer compare), not
            // reaped (vtable +0x158), its CShip (class 0x503, else null -- get_hit_pts is
            // called on it regardless) above 0 hit points (FCOMP 0, TEST AH,0x41, JNZ skips on
            // <= / NaN) queues "engine_destroyed" (the routine with the field, 0).
            if (CliEquipHostIsLivePlayer(Host, 0x503u))
                NnVoiceEnqueue(G_EngineDestroyedId, 0);
            break;
        }
        case EquipActivating:
            // the routine(0).
            CliEngineSetActiveSounds(this, 0);
            break;
        case EquipDeactivating:
            // the routine(1).
            CliEngineSetActiveSounds(this, 1);
            break;
        case Event_0xD:
        {
            // the slots whose sub-object id is the low word of Param
            // lose both effects (kill, release, clear); then the routine.
            unsigned short const SubObjId =
                static_cast<unsigned short>(reinterpret_cast<uintptr_t>(Param));
            for (FCliEngineNozzleSlot* Slot = Nozzles.First(); Slot != Nozzles.Last(); ++Slot)
            {
                if (Slot->SubObjId != SubObjId)
                    continue;
                if (Slot->TrailFx != nullptr)
                {
                    CliEngineKillParticle(Slot->TrailFx);
                    if (Slot->TrailFx != nullptr)
                    {
                        ParticleEffectRelease(Slot->TrailFx);
                        Slot->TrailFx = nullptr;
                    }
                }
                if (Slot->CruiseFx != nullptr)
                {
                    CliEngineKillParticle(Slot->CruiseFx);
                    if (Slot->CruiseFx != nullptr)
                    {
                        ParticleEffectRelease(Slot->CruiseFx);
                        Slot->CruiseFx = nullptr;
                    }
                }
            }
            CliEngineRepositionSounds(this);
            break;
        }
        case EngineCruiseStateChanged:
            // the routine(*Param).
            CliEngineSetCruiseState(this, *static_cast<int const*>(Param));
            break;
        default:
            break;
        }
        FClientEquip::notify(Code, Param);
    }

    FCliEngineEquipFx::~FCliEngineEquipFx()
    {
        // . 0x00527d38..0x00527d8d: per nozzle, trail (+4) then cruise (+8):
        // kill (vtable +4), then -- re-read -- release (the routine) and clear.
        for (FCliEngineNozzleSlot* Slot = Nozzles.First(); Slot != Nozzles.Last(); ++Slot)
        {
            if (Slot->TrailFx != nullptr)
            {
                CliEngineKillParticle(Slot->TrailFx);
                if (Slot->TrailFx != nullptr)
                {
                    ParticleEffectRelease(Slot->TrailFx);
                    Slot->TrailFx = nullptr;
                }
            }
            if (Slot->CruiseFx != nullptr)
            {
                CliEngineKillParticle(Slot->CruiseFx);
                if (Slot->CruiseFx != nullptr)
                {
                    ParticleEffectRelease(Slot->CruiseFx);
                    Slot->CruiseFx = nullptr;
                }
            }
        }
        // the inlined erase(begin, end) -- the the routine copy loop
        // runs over [end, end) and does nothing, the element destructors run, End = Begin.
        CliEngineClearNozzles(this);
        // the routine.
        CliEngineReleaseSoundEmitters(this);
        // the backfire effect arch (the routine) when set; the
        // pointer is left as it is.
        if (TrailEffect != nullptr)
            EffectArchRelease(TrailEffect);
        // the vector's own destructor (the routine inlined) -- runs
        // as the Nozzles member's ~FCliEngineNozzleVector after this body; ~FClientEquip
        // (the routine) follows it.
    }

    // baseline Archetype::CliEQEngine (vtable 005c7ac4, 0xac) is private to client_cliobj.cpp
    // (FCliEngine); these are the int ranges the routine reads off it.
    struct FCliEQEngineRangesView
    {
        int RumblePitchRange[2];      // +0x64 "rumble_pitch_range"
        int RumbleAttenRange[2];      // +0x6c "rumble_atten_range"
        unsigned CharacterSounds[2];  // +0x74 / +0x78 (ID_Strings, not read here)
        int CharacterPitchRange[2];   // +0x7c "character_pitch_range"
    };
    static_assert(sizeof(FCliEQEngineRangesView) == 0x84 - 0x64, "CliEQEngine +0x64..+0x84");

    FCliEQEngineRangesView const* CompanionAsEngineRanges(Archetype::ICliObj* Companion)
    {
        return reinterpret_cast<FCliEQEngineRangesView const*>(
            reinterpret_cast<unsigned char const*>(Companion) + 0x64);
    }

    // / the field.
    float const kEngineFxFull = 1.0f;
    double const kEngineRumbleMinSpeed = 0.001;

    bool FCliEngineEquipFx::Update(float Dt)
    {
        // (RET 4). Nothing but the base's answer without an equipment.
        if (Equip != nullptr)
        {
            // the host's ship (class 0x503), else null.
            CShip const* Ship = nullptr;
            if (Host != nullptr && Host->Object != nullptr &&
                (static_cast<unsigned>(Host->Object->ObjectClass) & 0x503u) == 0x503u)
            {
                Ship = static_cast<CShip const*>(Host->Object);
            }
            CEEngine const* const Engine = static_cast<CEEngine const*>(Equip);

            // the target throttle -- 0 while the engine is off
            // (vtable +4, IsActive), 1 (the field) in cruise (+0x54), else the ship's
            // throttle (+0x29c, read with no test of the ship).
            double Target = 0.0;
            if (Equip->IsActive())
            {
                if (Engine->bCruiseActive != 0)
                    Target = static_cast<double>(kEngineFxFull);
                else
                    Target = static_cast<double>(Ship->Throttle);
            }

            // step FxThrottle toward the target by at most
            // Dt * MAX_DELTA_FX_THROTTLE. The difference and the step are stored as floats;
            // the compare is step (x87) > |difference| (FCOMPP, TEST AH,0x41, JZ takes the
            // target on >). Otherwise the sign of the difference (FCOMP 0, TEST AH,0x41, JNZ
            // subtracts on <= / NaN) picks FxThrottle +/- step.
            float const Diff =
                static_cast<float>((Target - static_cast<double>(FxThrottle)) * static_cast<double>(Dt));
            double const StepX87 = static_cast<double>(Dt) *
                                   static_cast<double>(EngineEquipConsts::MAX_DELTA_FX_THROTTLE);
            float const Step = static_cast<float>(StepX87);
            float NewThrottle = 0.0f;
            if (StepX87 > static_cast<double>(std::fabs(Diff)))
                NewThrottle = static_cast<float>(Target);
            else if (Diff > 0.0f)
                NewThrottle = static_cast<float>(static_cast<double>(Step) + static_cast<double>(FxThrottle));
            else
                NewThrottle = static_cast<float>(static_cast<double>(FxThrottle) - static_cast<double>(Step));

            // unchanged (TEST AH,0x44, JP resets on not-equal / NaN)
            // counts the steady time up by Dt, any change resets it.
            if (NewThrottle == FxThrottle)
                ThrottleSteadyTime = static_cast<float>(static_cast<double>(Dt) + static_cast<double>(ThrottleSteadyTime));
            else
                ThrottleSteadyTime = 0.0f;

            // the exhaust intensity. In cruise with charge time elapsed
            // (+0x50 > 0) and an archetype cruise_charge_time (+0x80) > 0 it blends toward 1:
            // New * MAX_ENGINE_FX_THROTTLE + (elapsed / charge) * (1 - MAX_ENGINE_FX_THROTTLE);
            // charge time 0 gives New itself; otherwise New * MAX_ENGINE_FX_THROTTLE.
            float Intensity = 0.0f;
            double const MaxFx = static_cast<double>(EngineEquipConsts::MAX_ENGINE_FX_THROTTLE);
            if (Engine->bCruiseActive != 0)
            {
                float const Elapsed = Engine->CruiseChargeTimeElapsed;
                if (Elapsed > 0.0f)
                {
                    float const Charge = Engine->EngineArch()->CruiseChargeTime;
                    if (Charge > 0.0f)
                    {
                        double const Blend =
                            static_cast<double>(NewThrottle) * MaxFx +
                            (static_cast<double>(Elapsed) / static_cast<double>(Charge)) *
                                (static_cast<double>(kEngineFxFull) - MaxFx);
                        Intensity = static_cast<float>(Blend);
                    }
                    else
                    {
                        Intensity = NewThrottle;
                    }
                }
                else
                {
                    Intensity = static_cast<float>(static_cast<double>(NewThrottle) * MaxFx);
                }
            }
            else
            {
                Intensity = static_cast<float>(static_cast<double>(NewThrottle) * MaxFx);
            }

            // every nozzle's trail (+4) and cruise (+8) particle gets
            // slot +0xc (SetIntensity) with the intensity, floored at 0 (TEST AH,0x41, JNZ
            // gives 0 on <= / NaN).
            typedef void(__thiscall * FSetIntensity)(FParticleEffect*, float);
            for (FCliEngineNozzleSlot* Slot = Nozzles.First(); Slot != Nozzles.Last(); ++Slot)
            {
                if (Slot->TrailFx != nullptr)
                {
                    float const Value = (Intensity > 0.0f) ? Intensity : 0.0f;
                    FParticleEffect* const Effect = Slot->TrailFx;
                    Effect->SetIntensity(Value);
                }
                if (Slot->CruiseFx != nullptr)
                {
                    float const Value = (Intensity > 0.0f) ? Intensity : 0.0f;
                    FParticleEffect* const Effect = Slot->CruiseFx;
                    Effect->SetIntensity(Value);
                }
            }

            // the sounds with the new throttle, then it is kept.
            UpdateSound(NewThrottle, Dt);
            FxThrottle = NewThrottle;
        }
        // the routine.
        return FClientEquip::Update(Dt);
    }

    void FCliEngineEquipFx::UpdateSound(float Throttle, float Dt)
    {
        // (RET 8). Nothing while the engine is off (vtable +4, no test of Equip).
        if (!Equip->IsActive())
            return;
        CEEngine const* const Engine = static_cast<CEEngine const*>(Equip);
        ISoundSource* const Rumble = SoundEmitters[0];         // +0x38 rumble_sound
        ISoundSource* const CharacterLoop = SoundEmitters[2];  // +0x40 character_loop_sound
        ISoundSource* const CruiseLoop = SoundEmitters[5];     // +0x4c cruise_loop_sound

        if (Engine->bCruiseActive == 0)
        {
            if (Rumble != nullptr)
            {
                // the host's tradelane state (IObjInspect slot 77,
                // +0x134) and speed (slot 1, get_velocity); the rumble plays only above
                // 0.001 (the field, a double; TEST AH,0x41, JNZ stops on <= / NaN) and off
                // a tradelane.
                bool bUsingTradelane = false;
                Host->is_using_tradelane(&bUsingTradelane);
                Vector const Velocity = Host->get_velocity();
                double const SpeedX87 = std::sqrt(static_cast<double>(Velocity.x) * Velocity.x +
                                                  static_cast<double>(Velocity.y) * Velocity.y +
                                                  static_cast<double>(Velocity.z) * Velocity.z);
                float const Speed = static_cast<float>(SpeedX87);
                if (SpeedX87 > kEngineRumbleMinSpeed && !bUsingTradelane)
                {
                    // an idle rumble restarts on its stashed archetype
                    // (the routine called directly, the second argument 0).
                    if (Rumble->IsActive() == 0)
                        Rumble->Start(GetClientEquipSoundEmitterArch(Rumble), 0);

                    // speed over the cruise speed (+0x30); with the
                    // [AUDIO] option at 2 (the routine(0x2d)), capped at 1 (TEST AH,5, JNP
                    // keeps it below 1; >= / NaN take 1).
                    float Ratio = static_cast<float>(static_cast<double>(Speed) /
                                                     static_cast<double>(CruiseSpeed));
                    if (GetGameOption(kGameOptAudioGroup) == 2 && !(Ratio < kEngineFxFull))
                        Ratio = kEngineFxFull;

                    // pitch and attenuation interpolated across the
                    // companion's int ranges (FILD of the int span, FMUL, FIADD of the low end).
                    FCliEQEngineRangesView const* const Ranges =
                        CompanionAsEngineRanges((Equip != nullptr) ? Equip->Arch->Companion : nullptr);
                    int const PitchSpan = Ranges->RumblePitchRange[1] - Ranges->RumblePitchRange[0];
                    Rumble->SetPitch(static_cast<float>(
                        static_cast<double>(PitchSpan) * static_cast<double>(Ratio) +
                        static_cast<double>(Ranges->RumblePitchRange[0])));
                    int const AttenSpan = Ranges->RumbleAttenRange[1] - Ranges->RumbleAttenRange[0];
                    Rumble->SetAttenuation(static_cast<float>(
                        static_cast<double>(AttenSpan) * static_cast<double>(Ratio) +
                        static_cast<double>(Ranges->RumbleAttenRange[0])));
                }
                else if (Rumble->IsActive() != 0)
                {
                    // .
                    Rumble->Stop();
                }
            }

            // ..: the character loop; without one, nothing more.
            if (CharacterLoop == nullptr)
                return;
            if (CharacterLoop->IsActive() == 0)
                CharacterLoop->Start(GetClientEquipSoundEmitterArch(CharacterLoop), 0);
            if (CharacterLoop->IsActive() != 0)
            {
                // pitch across character_pitch_range by the throttle.
                FCliEQEngineRangesView const* const Ranges =
                    CompanionAsEngineRanges((Equip != nullptr) ? Equip->Arch->Companion : nullptr);
                int const PitchSpan =
                    Ranges->CharacterPitchRange[1] - Ranges->CharacterPitchRange[0];
                CharacterLoop->SetPitch(static_cast<float>(
                    static_cast<double>(PitchSpan) * static_cast<double>(Throttle) +
                    static_cast<double>(Ranges->CharacterPitchRange[0])));
            }

            // the attenuation offset. Before THROTTLE_STEADY_TIME
            // (TEST AH,5, JP takes the steady arm on >= / NaN) it climbs by
            // Dt * DELTA_THROTTLE_ATTEN_MOD_CHANGING, capped at 0 (0 below it keeps the sum,
            // JP on >= / NaN); once steady it moves by Dt * DELTA_THROTTLE_ATTEN_MOD_STEADY,
            // floored at -THROTTLE_ATTEN_MOD_RANGE (TEST AH,0x41, JNZ keeps the sum when the
            // floor is <= it). The result goes to the loop's attenuation.
            double Mod = 0.0;
            if (ThrottleSteadyTime < EngineEquipConsts::THROTTLE_STEADY_TIME)
            {
                Mod = static_cast<double>(Dt) *
                          static_cast<double>(EngineEquipConsts::DELTA_THROTTLE_ATTEN_MOD_CHANGING) +
                      static_cast<double>(AttenMod);
                AttenMod = static_cast<float>(Mod);
                if (0.0 < Mod)
                    Mod = 0.0;
            }
            else
            {
                Mod = static_cast<double>(Dt) *
                          static_cast<double>(EngineEquipConsts::DELTA_THROTTLE_ATTEN_MOD_STEADY) +
                      static_cast<double>(AttenMod);
                AttenMod = static_cast<float>(Mod);
                double const Floor = -static_cast<double>(EngineEquipConsts::THROTTLE_ATTEN_MOD_RANGE);
                if (Floor > Mod)
                    Mod = Floor;
            }
            AttenMod = static_cast<float>(Mod);
            CharacterLoop->SetAttenuation(AttenMod);
            return;
        }

        // in cruise, a playing cruise loop counts CruiseTime up by Dt;
        // past CRUISE_STEADY_TIME (TEST AH,0x41, JNZ keeps 0 on <= / NaN) its attenuation is
        // (CruiseTime - CRUISE_STEADY_TIME) * DELTA_CRUISE_ATTEN_MOD_STEADY, floored at
        // -CRUISE_ATTEN_MOD_RANGE; before that 0.
        if (CruiseLoop != nullptr && CruiseLoop->IsActive() != 0)
        {
            double const Time = static_cast<double>(Dt) + static_cast<double>(CruiseTime);
            CruiseTime = static_cast<float>(Time);
            float Atten = 0.0f;
            if (Time > static_cast<double>(EngineEquipConsts::CRUISE_STEADY_TIME))
            {
                Atten = static_cast<float>(
                    (Time - static_cast<double>(EngineEquipConsts::CRUISE_STEADY_TIME)) *
                    static_cast<double>(EngineEquipConsts::DELTA_CRUISE_ATTEN_MOD_STEADY));
                double const Floor = -static_cast<double>(EngineEquipConsts::CRUISE_ATTEN_MOD_RANGE);
                if (Floor > static_cast<double>(Atten))
                    Atten = static_cast<float>(Floor);
            }
            CruiseLoop->SetAttenuation(Atten);
            return;
        }
        // .
        CruiseTime = 0.0f;
    }

    FCliTradeLaneEquipFx::~FCliTradeLaneEquipFx()
    {
        // (0x00516bd3..0x00516be4): Equip(+0x04) -> archetype(+0x0c) ->
        // companion(+0x04), the routine -- the release matching the constructor's
        // . No null tests in baseline.
        Archetype::CliTradeLaneReleaseEffectRef(Equip->EquipArch()->Companion);
    }

    // ClientEquip slot 1 with 1 (the scalar deleting destructor), as the routine calls it.
    void DeleteEquipFxWrapper(FClientEquip* Wrapper)
    {
        delete Wrapper;
    }

    void EquippedObjScanFuseIgnitions(FClientGameobj* Host)
    {
        // fuse block — threshold walks + Host vt+0x1c0 Ignite
        // (the routine) + Fuse::Load on prototype map entry.
        // the host's object narrowed by 0x103 (null otherwise, then read
        // anyway); 0x00533ea6..0x00533eac: its archetype (+0x88, no class test -- a 0x103 object
        // is a ship or a solar) and the root fuse list at archetype +0x78.
        CObject* const Obj = ResolveEqObjFromHost(Host);
        Archetype::EqObj const* const EqArch =
            static_cast<Archetype::EqObj const*>(Obj->get_archetype());
        if (EqArch->Fuses != nullptr)
        {
            std::vector<Archetype::FGroupFuse> const& List = *EqArch->Fuses;
            for (size_t Index = 0; Index < List.size(); ++Index)
            {
                Archetype::FGroupFuse const& Node = List[Index];
                // CSimple::get_hit_pts per node, FCOMP Value1,
                // TEST AH,0x41 / JP skips above and NaN (hp <= Value1 ignites).
                // SUBOBJ_ID_ROOT (Common .rdata, 1), through the
                // object's own vtable +0x1c0 (a ship runs the routine).
                if (static_cast<CSimple*>(Obj)->get_hit_pts() <= Node.Value1)
                {
                    static_cast<FClientEquippedObj*>(Host)->Slot1c0(0u, &Node.Fuse, SUBOBJ_ID_ROOT,
                                                                   Node.Value2, -1.0f);
                }
                // the routine find in the field, Fuse::Load on the value.
                ClientFuse* Proto = ClientFuseDB::Find(Node.Fuse.Id);
                if (Proto != nullptr)
                    Proto->Load();
            }
        }

        CEqObj* Eq = static_cast<CEqObj*>(Obj);
        CArchGrpTraverser Trav;
        CArchGroup* Group = Eq->ArchGroups.Traverse(Trav);
        while (Group != nullptr)
        {
            Archetype::FuseIgnitionList const& FuseList = Group->FuseIgnitions();
            if (FuseList.Items != nullptr)
            {
                std::vector<Archetype::FGroupFuse> const& Items = *FuseList.Items;
                for (size_t Index = 0; Index < Items.size(); ++Index)
                {
                    Archetype::FGroupFuse const& Node = Items[Index];
                    // CArchGroup::GetHitPoints per item, FCOMP Value1,
                    // TEST AH,0x41 / JP skips above and NaN. 0x00533f74..0x00533f92: the group's
                    // arch (+4) id (+4, no test), vtable +0x1c0 as above.
                    if (Group->GetHitPoints() <= Node.Value1)
                    {
                        static_cast<FClientEquippedObj*>(Host)->Slot1c0(
                            0u, &Node.Fuse, Group->Arch->Id, Node.Value2, -1.0f);
                    }
                    // the routine lower_bound + key check (= find) in
                    // , Fuse::Load on the value.
                    ClientFuse* Proto = ClientFuseDB::Find(Node.Fuse.Id);
                    if (Proto != nullptr)
                        Proto->Load();
                }
            }
            Group = Eq->ArchGroups.Traverse(Trav);
        }
    }

}

void EquippedObjClearEquipFx(FClientGameobj* Host)
{
    // .
    if (Host == nullptr)
        return;
    FCliEquipFxListNode* Sentinel = EquipFxSentinelRef(Host);
    if (Sentinel == nullptr)
        return;
    for (FCliEquipFxListNode* Node = Sentinel->Next; Node != nullptr && Node != Sentinel;
         Node = Node->Next)
    {
        if (Node->Wrapper != nullptr)
        {
            DeleteEquipFxWrapper(Node->Wrapper);
            Node->Wrapper = nullptr;
        }
    }
    FCliEquipFxListNode* Cur = Sentinel->Next;
    while (Cur != nullptr && Cur != Sentinel)
    {
        FCliEquipFxListNode* Next = Cur->Next;
        if (Cur->Prev != nullptr)
            Cur->Prev->Next = Cur->Next;
        if (Cur->Next != nullptr)
            Cur->Next->Prev = Cur->Prev;
        ::operator delete(Cur);
        EquipFxCountRef(Host) = EquipFxCountRef(Host) - 1;
        Cur = Next;
    }
    Sentinel->Next = Sentinel;
    Sentinel->Prev = Sentinel;
}

void EquippedObjCreateEquipFx(FClientGameobj* Host, unsigned short EquipId)
{
    // — full type switch.
    CEqObj* Eq = ResolveEqObjFromHost(Host);
    if (Eq == nullptr)
        return;
    CEquip* Equip = Eq->EquipManager.FindByID(EquipId);
    if (Equip == nullptr)
        return;
    unsigned const Type = Equip->GetType();
    FClientEquip* Wrapper = nullptr;
    unsigned const EquipTypeTradeLane = 0x800000u;

    if (Type == static_cast<unsigned>(EquipTypeLight))
        Wrapper = CliLightEquipFxConstruct(Equip, Host);
    else if (Type == static_cast<unsigned>(EquipTypeGun))
        Wrapper = CliGunEquipFxConstruct(Equip, Host);
    else if (Type == static_cast<unsigned>(EquipTypeAttachedFX))
        Wrapper = CliAttachedFxEquipFxConstruct(Equip, Host);
    else if (Type == static_cast<unsigned>(EquipTypeEngine))
        Wrapper = CliEngineEquipFxConstruct(Equip, Host);
    else if (Type == static_cast<unsigned>(EquipTypeShieldGen))
        Wrapper = CliShieldGenEquipFxConstruct(Equip, Host);
    else if (Type == static_cast<unsigned>(EquipTypeThruster))
        Wrapper = CliThrusterEquipFxConstruct(Equip, Host);
    else if (Type == static_cast<unsigned>(EquipTypeTractor))
        Wrapper = CliTractorEquipFxConstruct(Equip, Host);
    else if (Type == static_cast<unsigned>(EquipTypeMineDropper))
        Wrapper = CliMineEquipFxConstruct(Equip, Host);
    else if (Type == static_cast<unsigned>(EquipTypeCmDropper))
        Wrapper = CliCmEquipFxConstruct(Equip, Host);
    else if (Type == static_cast<unsigned>(EquipTypeShield))
        Wrapper = CliShieldEquipFxConstruct(Equip, Host);
    else if (Type == static_cast<unsigned>(EquipTypeCloak))
        Wrapper = CliCloakEquipFxConstruct(Equip, Host);
    else if (Type == static_cast<unsigned>(EquipTypeRepairDroid))
        Wrapper = CliRepairDroidEquipFxConstruct(Equip, Host);
    else if (Type == static_cast<unsigned>(EquipTypeInternalFX))
        Wrapper = CliInternalFxEquipFxConstruct(Equip, Host);
    else if (Type == EquipTypeTradeLane)
        Wrapper = CliTradeLaneEquipFxConstruct(Equip, Host);
    else
    {
        return;
    }
    if (Wrapper == nullptr)
        return;
    EquipFxListInsert(Host, Wrapper);
}

void EquippedObjRebuildEquipFx(FClientGameobj* Host)
{
    // (returns nothing: EAX is whatever the routine left, 0x00533ffb..0x00534007).
    // the routine.
    EquippedObjClearEquipFx(Host);
    // each step narrows the object by 0x103 again (null otherwise) and
    // takes CEquipManager::Traverse on its +0xe4 without a test; the routine per equipment id.
    CEqObj* Eq = ResolveEqObjFromHost(Host);
    CEquipTraverser Walk;
    CEquip* Equip = Eq->EquipManager.Traverse(Walk);
    while (Equip != nullptr)
    {
        EquippedObjCreateEquipFx(Host, Equip->GetID());
        Equip = Eq->EquipManager.Traverse(Walk);
    }
    EquippedObjScanFuseIgnitions(Host);
    EquippedObjResolveExplosion(Host);
}

// The port's CliEquipLight driver that stood here (AttachClientEquipFxForShip /
// ClearClientEquipFx / TickClientEquipFx / DrawClientEquipFxFlares over a private list of the
// last rebuilt host's lights) is gone: the lights now carry baseline's vtable the branch and
// run where baseline runs them -- linked into bucket 0x2e by the owner's effect-list sweep
// (the routine, space_scene_obj_draw.cpp GameobjSweepEffectList) and ticked / drawn by the
// bucket walk the routine inside the routine, whose the routine flushes the flares.

// ---- ClientEquip (vtable the routine) -------------------------------------------------

// baseline has no out-of-line default constructor: the routine (the base constructor proper, with
// its CEquip::ConnectClientEquip and separation-explosion reference) is what the wrappers run;
// the port's derived constructors still do that part themselves (CliEquipFxBaseFill,
// CliLauncherFxBaseFill, FCliLightEquipFx's constructor), so this one only nulls the fields.
FClientEquip::FClientEquip()
    : Equip(nullptr)
    , Host(nullptr)
    , SeparationExplosion(nullptr)
{
}

FClientEquip::~FClientEquip()
{
    // (the body of the scalar deleting the routine): vtable 005de13c, then a
    // non-null Equip told to drop this notify, then a non-null separation explosion released.
    if (Equip != nullptr)
        Equip->NotifyDisconnecting(this);
    if (SeparationExplosion != nullptr)
        ExplosionArchRelease(SeparationExplosion);
}

bool FClientEquip::Update(float /*Dt*/)
{
    // (RET 4): XOR EAX,EAX / TEST [ECX+4] / SETNZ AL.
    return Equip != nullptr;
}

void FClientEquip::notify(Event Code, void* /*Param*/)
{
    // (RET 8): only 0xa, and no test of Equip before the call.
    if (Code == EquipDestroying)
    {
        Equip->NotifyDisconnecting(this);
        Equip = nullptr;
    }
}

// ---- CliTradeLaneEquip access for the solar's lane switch the routine ---------------------

int* CliTradeLaneEquipFxShipCount(FClientEquip* Wrapper)
{
    // reads and writes [wrapper+0x30] directly (0x00516952 / 0x00516962).
    return &static_cast<FCliTradeLaneEquipFx*>(Wrapper)->ShipsUsingLane;
}

void CliTradeLaneEquipFxAttachLaneEffects(FClientEquip* Wrapper)
{
    // calls the routine (thiscall on the wrapper) when the count returns to 0.
    CliTradeLaneAttachLaneEffects(static_cast<FCliTradeLaneEquipFx*>(Wrapper));
}

void ClientEquipBaseFill(FClientEquip* Wrapper, CEquip* Equip, FClientGameobj* Host)
{
    // for the constructors outside this file (the launcher family, 0x0052c85c).
    CliEquipFxBaseFill(Wrapper, Equip, Host);
}

bool RemoveAppearanceInstanceValue(int Key)
{
    // (cdecl). 0x0053218e..0x005321b2: NeuronRuntime::Engine slot 0 (QueryInterface) for
    // "1.11_IFLAppearance" into a zeroed local.
    IFLAppearance* Ifl = nullptr;
    if (NeuronRuntime::Engine->QueryInterface(Iids::FLAppearance, reinterpret_cast<void**>(&Ifl)) < 0)
    {
        // failed -- released if anything came back, false.
        if (Ifl != nullptr)
            Ifl->Release();
        return false;
    }
    // slot 5 (+0x14) with Key, SETGE on the result, released.
    bool const bOk = Ifl->RemoveInstanceValue(Key) >= 0;
    if (Ifl != nullptr)
        Ifl->Release();
    return bOk;
}

FClientEquip* EquippedObjFindEquipFx(FClientGameobj* Host, unsigned short EquipId)
{
    // (thiscall on the EquippedObj, RET 4). 0x00533103..0x00533126: the host's
    // object narrowed by 0x103 (a null host gives null).
    CEqObj* const Eq = ResolveEqObjFromHost(Host);
    // CEquipManager::FindByID on the object's +0xe4 -- taken without a
    // test of the narrowed object.
    CEquip const* const Equip = Eq->EquipManager.FindByID(EquipId);
    // the first wrapper on the +0xb8 list whose +4 is that equipment.
    FCliEquipFxListNode* const Sentinel = EquipFxSentinelRef(Host);
    for (FCliEquipFxListNode* Node = Sentinel->Next; Node != Sentinel; Node = Node->Next)
    {
        if (Node->Wrapper->Equip == Equip)
            return Node->Wrapper;
    }
    return nullptr;
}

bool HostIgniteFuse(FClientGameobj* Host, unsigned SourceId, ID_String const& FuseId,
                    unsigned short SubObjId, float AtT, float OverrideLifetime)
{
    // (FClientEquippedObj::Slot1c0's body).
    return HostIgniteFuseImpl(Host, SourceId, FuseId, SubObjId, AtT, OverrideLifetime);
}

void CliTractorEquipFxDropLoot(FClientEquip* Wrapper, unsigned LootId, int Units)
{
    // (thiscall on the tractor wrapper); the caller picks the wrapper with
    // by the tractor's equipment id.
    static_cast<FCliTractorEquipFx*>(Wrapper)->DropLoot(LootId, Units);
}

void CliShieldEquipFxCollisionHit(FClientEquip* Wrapper, void const* HitInfo,
                                  FClientGameobj* Other, void const* Unused)
{
    // Wrapper vtable +0x14 (0x0053417d): the branch on a shield wrapper.
    static_cast<FCliShieldEquipFx*>(Wrapper)->OnCollisionHit(HitInfo, Other, Unused);
}

void CliShieldEquipFxMunitionHit(FClientEquip* Wrapper, void const* HitInfo,
                                 FClientGameobj* Munition)
{
    // Wrapper vtable +0x18 (0x0053422a): the routine on a shield wrapper.
    static_cast<FCliShieldEquipFx*>(Wrapper)->OnMunitionHit(HitInfo, Munition);
}
