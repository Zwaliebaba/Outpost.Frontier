// Author: Giperion.

#include "perf_options.h"
#include "gf_script_object.h" // / the routine: one transition-script latch.

#include "cli_asteroid.h"
#include "cli_nebula.h"
#include "client_cliobj.h"
#include "common_archetype.h"
#include "common_ini.h"
#include "common_newchar.h"
#include "common_misc.h"   // GetUserDataPath
#include "common_timing.h" // Timing::GetMachineSpeed
#include "neuroncomponents.h"
#include "NeuronRuntime.h"
#include "globals.h" // ApplyGammaRampOption / ApplyDitherControlOption / ApplyMipFilterControlOption
#include "hud_target.h"       // HudShipOfObjRw (the routine chain)
#include "player_behavior.h"  // GetPlayerObjRW (the routine)
#include "common_cobject.h"   // CShip / CEqObj::get_behavior_interface
#include "common_iface.h"     // IBehaviorManager::set_turn_sensitivity
#include "input_manager.h"
#include "menu_sound_arch.h"
#include "render/IRenderPipeline.h"
#include "render/shading.h"
#include "space_solar_world.h"
#include "star_sphere.h"
#include "star_sys.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

namespace
{
    // baseline source coordinates for the FDUMP below.
    char const kPerfOptionsCpp[] = "Client\\PerfOptions.cpp";

    // ..the field — seven floats read back through the routine(index).
    int const kPerfSlotCount = 7;
    float G_Slots[kPerfSlotCount] = {};

    bool G_bSkipMachineWarnings = false;

    int   G_MaxSubmittedSounds = 0x20;
    bool  G_bEnableTooltips = true;         // (.data initial 01h)
    bool  G_bEnableRollover = true;         // (.data initial 01h)
    bool  G_bCockpit = false;
    bool  G_bAutohideManeuverBar = false;
    bool  G_bAdvancedGraphics = false;
    int   G_GeneralGraphics = -1;           // (.data initial FFFFFFFFh)
    int   G_DefaultGraphics = 0;
    float G_SpaceDustDetail = 1.0f;         // (.data initial 0x3f800000)
    float G_GenericEffectPerf = 1.0f;       // (.data initial 0x3f800000)

    // ftol() truncates toward zero; baseline relies on that everywhere below.
    int Ftol(float Value)
    {
        return static_cast<int>(Value);
    }

    // ------------------------------------------------------------------
    // Apply callbacks. Each is one baseline function; its address is on the line.
    // ------------------------------------------------------------------

    void ApplyAudioMusic(float Value) { SetClientAudioMusicVolume(Value); }
    void ApplyAudioAmbient(float Value) { SetClientAudioAmbientVolume(Value); }
    void ApplyAudioSfx(float Value) { SetClientAudioSfxVolume(Value); }
    void ApplyAudioInterface(float Value) { SetClientAudioInterfaceVolume(Value); }
    void ApplyAudioVoice(float Value) { SetClientAudioVoiceVolume(Value); }

    void ApplySoundMaxDistScale(float Value)  // — the field
    {
        // one store, RET 4. Readers go through the routine(2).
        G_Slots[2] = Value;
    }

    void ApplySoundLevelOfDetail(float Value)  // — the field + the routine
    {
        // FLD float 1.0 (the field); FSUB float v; the arg stored to the field; FADD ST0,ST0;
        // FSTP double -> floor; ftol of floor's double result -> the routine(int).
        G_Slots[3] = Value;
        SetClientSoundLod(static_cast<int>(floor(2.0 * (1.0 - static_cast<double>(Value)))));
    }

    void ApplyUse3dSounds(float Value)  // — the field
    {
        G_Slots[4] = Value;
    }

    void ApplyDynamicCommHead(float Value)  // — the field
    {
        G_Slots[5] = Value;
    }

    void ApplyDifficultyScale(float Value)  // — the field
    {
        G_Slots[6] = Value;
    }

    void ApplyMaxSubmittedSounds(float Value)  // — the field
    {
        // FLD float v; FMUL float 100.0 (the field); FADD float 0.5 (the field); ftol --
        // the product and sum stay on the x87 stack, so they are not rounded to float.
        G_MaxSubmittedSounds = static_cast<int>(static_cast<double>(Value) * 100.0 + 0.5);
    }

    void ApplyEnableTooltips(float Value) { G_bEnableTooltips = Value > 0.5f; }
    void ApplyEnableRollover(float Value) { G_bEnableRollover = Value > 0.5f; }
    void ApplyCockpit(float Value) { G_bCockpit = Value > 0.5f; }

    void ApplyAutohideManeuverBar(float Value)
    {
        G_bAutohideManeuverBar = Value > 0.5f;
    }

    void ApplyGeneralGraphics(float Value) { G_GeneralGraphics = Ftol(Value); }
    void ApplyAdvancedGraphics(float Value) { G_bAdvancedGraphics = Value != 0.f; }
    void ApplyDefaultGraphics(float Value) { G_DefaultGraphics = Ftol(Value); }

    void ApplySpaceDustDetail(float Value) { G_SpaceDustDetail = Value; }
    // FLD float Value; FSUBR double 1.0 (the field); FSTP float the field.
    void ApplyGenericEffectPerf(float Value)
    {
        G_GenericEffectPerf = static_cast<float>(1.0 - static_cast<double>(Value));
    }
    void ApplySunEffect(float Value) // — the field
    {
        // Q = Value * 1.33333 (the double the field, not 4/3); FCOM 1.0 /
        // TEST AH,0x41 / JNZ keeps Q unless it is greater, then 1.0f. FST stores the float and
        // keeps Q on the FPU.
        double Q = static_cast<double>(Value) * 1.33333;
        if (Q > 1.0)
            Q = 1.0;
        G_SunEffectQuality = static_cast<float>(Q);

        // every star of the list the field (StarSphereList): its sun body
        // (+0x14) gets +0x2b8 = ftol(int +0x2b4 * (1.0f - Q) + 0.5f). No null tests.
        double const Keep = 1.0 - Q; // FSUBR float 1.0
        FStarSphereListNode* const Sentinel = StarSphereList()->Sentinel;
        for (FStarSphereListNode* Node = Sentinel->Next; Node != Sentinel; Node = Node->Next)
        {
            unsigned char* const Body = Node->Sphere->SunBody->Raw;
            int const Full = *reinterpret_cast<int const*>(Body + 0x2b4);
            *reinterpret_cast<int*>(Body + 0x2b8) =
                static_cast<int>(static_cast<double>(Full) * Keep + 0.5);
        }
    }

    // - apply shared by AtmosphereMaterialConfig (option +0x20 = 1, set at
    // ) and HUDAnimConfig (+0x20 = 4, 005b435d); vtable PTR_OnAddHost_005e6f80.
    // No MaterialLib -> nothing. Else record[+0x20] = (v > 0.999 [the field]) through
    // IMaterialLibrary slot 24 (+0x60), then slot 12 (+0x30) ResolveTexturesGroup(0, 0).
    void ApplyMaterialRecordOption(unsigned long RecordIndex, float Value)
    {
        if (NeuronRuntime::MaterialLib == nullptr)
            return;
        unsigned long bEnabled = 1;
        if (!(static_cast<double>(Value) > 0.999))
            bEnabled = 0;
        NeuronRuntime::MaterialLib->SetGlobalRecordValue(RecordIndex, &bEnabled);
        NeuronRuntime::MaterialLib->ResolveTexturesGroup(0, 0.0f);
    }

    // (cdecl, the option value) - TurnSensitivity: the player IObjRW
    // (the routine), its +0xc CBase, the CObject at +4 of class 0x503 (the inlined
    // chain, each step null-tested), CEqObj::get_behavior_interface; a manager gets
    // IBehaviorManager::set_turn_sensitivity(Value).
    void ApplyTurnSensitivity(float Value)
    {
        CShip* const Ship = HudShipOfObjRw(static_cast<IObjRW*>(GetPlayerObjRW()));
        if (Ship == nullptr)
            return;
        IBehaviorManager* const Behavior = Ship->get_behavior_interface();
        if (Behavior == nullptr)
            return;
        Behavior->set_turn_sensitivity(Value);
    }

    void ApplyAtmosphereMaterialConfig(float Value) { ApplyMaterialRecordOption(1, Value); }
    void ApplyHudAnimConfig(float Value) { ApplyMaterialRecordOption(4, Value); }

    void ApplyUseEnvironmentMaps(float Value)  // - the field
    {
        // v > 0 and a pipeline: ask device ability 8; a non-zero answer keeps the raw value
        // in slot 0, every other path stores 0 (005b473a). The HRESULT is not examined.
        unsigned long Ability = 0;
        IRenderPipeline* Pipe = NeuronRuntime::Pipeline;
        if (Value > 0.f && Pipe != nullptr)
        {
            Pipe->QueryDeviceAbility(8, &Ability);
            if (Ability != 0)
            {
                G_Slots[0] = Value;
                return;
            }
        }
        G_Slots[0] = 0.f;
    }

    void ApplyHighDetailTextures(float Value)  // - the field
    {
        // NeuronRuntime::Pipeline is dereferenced without a null check.
        unsigned long VidMem = 0;
        int bLowVidMem = 0;
        IRenderPipeline* Pipe = NeuronRuntime::Pipeline;
        if (Pipe->QueryDeviceAbility(3, &VidMem) >= 0)
            bLowVidMem = ((VidMem & 0xfff00000u) < 0x01100000u) ? 1 : 0;

        // FLD float / FCOMP double / TEST AH,0x41 / JNZ: each flag is "v > constant".
        double const V = static_cast<double>(Value);
        int const bHighDetail = (V > 0.6 && bLowVidMem == 0) ? 1 : 0;
        int const bPipelineState4 = (V > 0.5) ? 1 : 0;
        unsigned long Record0 = (V > 0.4) ? 1 : 0;
        unsigned long Record2 = (V > 0.3) ? 1 : 0;

        if (NeuronRuntime::MaterialLib != nullptr)
        {
            NeuronRuntime::MaterialLib->SetGlobalRecordValue(0, &Record0);
            NeuronRuntime::MaterialLib->SetGlobalRecordValue(2, &Record2);
            NeuronRuntime::MaterialLib->ResolveTexturesGroup(0, 0.0f);
        }
        if (NeuronRuntime::TextureLib != nullptr)
            NeuronRuntime::TextureLib->SetGenericSlot(4, bHighDetail != 0 ? 0u : 0x80u);
        G_Slots[1] = bHighDetail != 0 ? 1.f : 0.f;
        Pipe = NeuronRuntime::Pipeline;
        if (Pipe != nullptr)
            Pipe->SetPipelineState(4, bPipelineState4);
    }

    // level_of_detail: the baseline option object is 0x28 bytes (ctor 005b4430) -- the common
    // 0x1c head, then +0x1c an archetype enumerator (vtable the branch, one slot
    // ), +0x20 a byte and +0x24 a float, both written by the apply below. The
    // port keeps the three tail members as file statics.

    // (thiscall, RET 4): a record with a companion (Root+0x04) gets
    // (companion, 0); every record returns true, so the walk never stops early.
    struct FLevelOfDetailEnumerator : Archetype::ArchEnumerator
    {
        bool Enumerate(Archetype::Root* Record) override
        {
            if (Record->Companion != nullptr)
                Archetype::CliObjRecomputeLodPick(Record->Companion, nullptr);
            return true;
        }
    };

    FLevelOfDetailEnumerator G_LevelOfDetailEnumerator; // option +0x1c
    bool G_bLevelOfDetailLow = false;                   // option +0x20
    float G_LevelOfDetailScale = 0.f;                   // option +0x24

    void ApplyLevelOfDetail(float Value)  // (thiscall on the option, RET 4)
    {
        // FLD v / FCOMP double 0.75 (the field) / TEST AH,0x5 / JP:
        // AL = 1 only for v < 0.75 (an unordered v gives 0). Stored at +0x20.
        bool const bLow = static_cast<double>(Value) < 0.75;
        G_bLevelOfDetailLow = bLow;

        // , all on the x87 stack, then FSTP float at +0x24:
        //   low:  v * 4/3 (the field) * 0.5 (the field) + 0.5
        //   else: (v - 0.75) * 4.0 (the field) + 1.0 (the field)
        double Scale = 0.0;
        if (bLow)
            Scale = static_cast<double>(Value) * (4.0 / 3.0) * 0.5 + 0.5;
        else
            Scale = (static_cast<double>(Value) - 0.75) * 4.0 + 1.0;
        G_LevelOfDetailScale = static_cast<float>(Scale);

        // the routine(AL, [+0x24]).
        Archetype::CliObjSetArchLodGlobals(bLow ? 1 : 0, G_LevelOfDetailScale);

        // the enumerator at +0x1c over simples, ships, equipment, solars,
        // in that order (IAT 005c6b9c / 005c6b98 / 005c6b88 / 005c6b94).
        Archetype::EnumSimples(&G_LevelOfDetailEnumerator);
        Archetype::EnumShips(&G_LevelOfDetailEnumerator);
        Archetype::EnumEquipment(&G_LevelOfDetailEnumerator);
        Archetype::EnumSolars(&G_LevelOfDetailEnumerator);

        // the routine([+0x24]).
        SolarSpheresApplyDetail(G_LevelOfDetailScale);
    }

    // ------------------------------------------------------------------
    // The registry. baseline builds it from static initializers into a heap std::list
    // (the field, count the field; the routine or the same code inline pushes each
    // option object, the first push creates the list head and registers atexit the branch).
    // The port holds the same rows in a fixed table. The row order IS baseline's list order:
    // the registrars run in _initterm (XC table 0x0060f000..0x006101bc) order, which for
    // these 36 is slot 54 (0041d900), 72..74 (00424470 / 004245c0 / 00424660), 469
    // (004c70f0), 586 (004f7020), 611 (00509bc0), 626 (00510c60), 670 (00526830), 828
    // (0054c370), 837 (00552d30), then 1107..1131 (005b4180 .. 005b60f0) -- exactly the
    // rows below. That order drives the routine (apply) and the routine (save).

    // Options with a null Apply are still registered: the [PerfCount] check compares
    // against the full baseline count of 36, and their value has to survive a load/save
    // round trip. Their baseline apply bodies drive NeuronRuntime/pipeline state that is not ported.
    // ------------------------------------------------------------------
    FPerfOption G_Options[] = {
        { "mouse_speed",              0x5dd, 0, 0.f, -1.f, ApplyMouseSpeedOption },
        { "gamma_ramp",               0x534, 0, 0.f, -1.f, ApplyGammaRampOption },
        { "DitherControl",            0x67f, 0, 0.f, -1.f, ApplyDitherControlOption },
        { "MipFilterControlCallback", 0,     0, 0.f, -1.f, ApplyMipFilterControlOption },
        // TurnSensitivity / Nebula / SunEffect / Asteroids / star_sphere: the ctor leaves +0x08 at
        // 0 and passes the IDS as the routine's second argument, which stores it at +0x08 when
        // non-zero (005b35f2..005b35fc). PUSH sites: 004c7114, 00509be4, 00510c84, 00526854,
        // .
        { "TurnSensitivity",          0x614, 0, 0.f, -1.f, ApplyTurnSensitivity },
        { "generic_effect_perf",      0x5cb, 0, 0.f, -1.f, ApplyGenericEffectPerf },
        { "Nebula",                   0x57d, 0, 0.f, -1.f, NebulaApplyGlobalDetail },
        { "SunEffect",                0x5ca, 0, 0.f, -1.f, ApplySunEffect },
        { "Asteroids",                0x57c, 0, 0.f, -1.f, CliAsteroidApplyGlobalDetail },
        { "space_dust_detail",        0x57a, 0, 0.f, -1.f, ApplySpaceDustDetail },
        { "star_sphere",              0x57b, 0, 0.f, -1.f, StarSphereApplyDetail },
        { "AtmosphereMaterialConfig", 0x5c9, 0, 0.f, -1.f, ApplyAtmosphereMaterialConfig },
        { "HUDAnimConfig",            0x65e, 0, 0.f, -1.f, ApplyHudAnimConfig },
        { "level_of_detail",          0x5d5, 0, 0.f, -1.f, ApplyLevelOfDetail },
        { "use_environment_maps",     0x657, 0, 0.f, -1.f, ApplyUseEnvironmentMaps },
        { "high_detail_textures",     0x658, 0, 0.f, -1.f, ApplyHighDetailTextures },
        { "sound_max_dist_scale",     0x77a, 0, 0.f, -1.f, ApplySoundMaxDistScale },
        { "sound_level_of_detail",    0x77b, 0, 0.f, -1.f, ApplySoundLevelOfDetail },
        { "USE_3D_SOUNDS",            0x77e, 0, 0.f, -1.f, ApplyUse3dSounds },
        { "audio_music",              0x539, 0, 0.f, -1.f, ApplyAudioMusic },
        { "audio_ambient",            0x584, 0, 0.f, -1.f, ApplyAudioAmbient },
        { "audio_sfx",                0x538, 0, 0.f, -1.f, ApplyAudioSfx },
        { "audio_interface",          0x583, 0, 0.f, -1.f, ApplyAudioInterface },
        { "audio_voice",              0x581, 0, 0.f, -1.f, ApplyAudioVoice },
        // +0x18 = [PTR_set_slider_005c6ce8] (COMMON.DLL RtcSlider::set_slider) on the
        // base option vtable 005d6d50, whose apply 004c7160 calls it cdecl with the value.
        { "rtc_performance",          0x656, 0, 0.f, -1.f, RtcSlider::set_slider },
        { "general_graphics",         0,     0, 0.f, -1.f, ApplyGeneralGraphics },
        { "advanced_graphics",        0,     0, 0.f, -1.f, ApplyAdvancedGraphics },
        { "default_graphics",         0,     0, 0.f, -1.f, ApplyDefaultGraphics },
        { "skip_transition_scripts",  0x5d7, 0, 0.f, -1.f, GfScriptSequenceSetSkipOption },
        { "enable_tooltips",          0x578, 0, 0.f, -1.f, ApplyEnableTooltips },
        { "enable_rollover",          0x579, 0, 0.f, -1.f, ApplyEnableRollover },
        { "cockpit",                  0x5da, 0, 0.f, -1.f, ApplyCockpit },
        { "USE_DYNAMIC_COMM_HEAD",    0x631, 0, 0.f, -1.f, ApplyDynamicCommHead },
        { "DIFFICULTY_SCALE",         0x57f, 0, 0.f, -1.f, ApplyDifficultyScale },
        { "autohide_maneuver_bar",    0x613, 0, 0.f, -1.f, ApplyAutohideManeuverBar },
        { "max_submitted_sounds",     0x687, 0, 0.f, -1.f, ApplyMaxSubmittedSounds },
    };

    unsigned const kOptionCount = sizeof(G_Options) / sizeof(G_Options[0]);
}

    // — hardware graphics score in [0.25, 1.0]. Listing OutpostFrontier
    // . Uses IRenderPipeline::QueryDeviceAbility (vtable+0x20),
    // NOT GetDeviceInfo / GetDeviceStats.

    // Ability index 4 = HW T&L flag (DX8RENDERER Ability_[4]: DevCaps HWTRANSFORMANDLIGHT &&
    // DRAWPRIMITIVES2EX). Ability index 3 = video memory bytes (Ability_[3] = VidMem_).
    // Timing::GetMachineSpeed thresholds are 1000 / 800 / 700 as written (baseline units).
    float ProbeMachineGraphicsLevel()
    {
        float GfxScore = 1.f;           // set before first query, as baseline MOV 0x3f800000
        unsigned long AbilityHwTnL = 0; // out of QueryDeviceAbility(4)
        unsigned long AbilityVidMem = 0;

        IRenderPipeline* Pipe = NeuronRuntime::Pipeline;
        if (Pipe != nullptr)
        {
            // baseline always reaches the first Query with GfxScore already 1.0. Failure or a
            // non-zero HW-T&L flag jumps to the CPU-speed half with that 1.0 intact.
            AbilityHwTnL = 0;
            GENRESULT const Hr4 = Pipe->QueryDeviceAbility(4, &AbilityHwTnL);
            if (Hr4 >= 0 && AbilityHwTnL == 0)
            {
                // No HW T&L: drop to 0.75, then maybe 0.5 from VRAM.
                GfxScore = 0.75f;
                AbilityVidMem = 0;
                GENRESULT const Hr3 = Pipe->QueryDeviceAbility(3, &AbilityVidMem);
                if (Hr3 >= 0)
                {
                    // (VidMem & 0xfff00000) < 0x01100000 → under ~17 MiB → 0.5
                    if ((AbilityVidMem & 0xfff00000u) < 0x01100000u)
                        GfxScore = 0.5f;
                }
            }
        }

        float CpuScore;
        int const MachineSpeed = Timing::GetMachineSpeed();
        if (MachineSpeed > 0x3e8) // > 1000
        {
            CpuScore = 1.f;
        }
        else
        {
            // Ability_[4]==0 → 0.6 (the field); else 0.75 (the field).
            if (AbilityHwTnL == 0)
                CpuScore = 0.6f;
            else
                CpuScore = 0.75f;

            if (MachineSpeed < 0x320) // < 800
            {
                if (MachineSpeed < 0x2bc) // < 700
                    CpuScore = 0.25f;
                else
                    CpuScore = 0.6f;
            }
        }

        // Return min(CpuScore, GfxScore) — FCOMP / TEST AH,5 / take the lesser.
        if (GfxScore < CpuScore)
            return GfxScore;
        return CpuScore;
    }

FPerfOption* FindPerfOption(char const* Name)
{
    // — the registry in order, the name (+0x04) against Name with an inlined
    // byte strcmp (0x005b3718..0x005b373e; Name is not null-tested); the first match, else 0.
    for (unsigned Index = 0; Index < kOptionCount; ++Index)
    {
        if (strcmp(G_Options[Index].Name, Name) == 0)
            return &G_Options[Index];
    }
    return nullptr;
}

FPerfOption* FindPerfOptionByIds(int IdsName)
{
    // — the registry in order, the first option whose IDS name (+0x08) equals
    // IdsName (0x005b37c5..0x005b37d1), else 0. Zero is not special-cased: it finds the first
    // option registered without an IDS name.
    for (unsigned Index = 0; Index < kOptionCount; ++Index)
    {
        if (G_Options[Index].IdsName == IdsName)
            return &G_Options[Index];
    }
    return nullptr;
}

unsigned GetPerfOptionCount()
{
    return kOptionCount;
}

FPerfOption* GetPerfOptionAt(unsigned Index)
{
    if (Index >= kOptionCount)
        return nullptr;
    return &G_Options[Index];
}

float GetPerfOptionSlot(int Index)
{
    // FLD [EAX*4 + the field], no bounds check.
    return G_Slots[Index];
}

void ApplyChangedPerfOptions()
{
    // .
    for (unsigned Index = 0; Index < kOptionCount; ++Index)
    {
        FPerfOption& Option = G_Options[Index];
        if (Option.Applied == Option.Value)
            continue;
        Option.Applied = Option.Value;
        if (Option.Apply != nullptr)
            Option.Apply(Option.Value);
    }
}

bool GetSkipMachineWarnings()
{
    return G_bSkipMachineWarnings;
}

void SetSkipMachineWarnings(bool bSkip)
{
    G_bSkipMachineWarnings = bSkip;
}

int GetMaxSubmittedSounds()
{
    // .
    return G_MaxSubmittedSounds;
}

bool GetPerfEnableTooltips() { return G_bEnableTooltips; }
bool GetPerfEnableRollover() { return G_bEnableRollover; }
bool GetPerfCockpit() { return G_bCockpit; }
bool GetPerfAutohideManeuverBar() { return G_bAutohideManeuverBar; }
bool GetPerfSkipTransitionScripts() { return GfScriptSequenceSkipLatch(); }
bool GetPerfAdvancedGraphics() { return G_bAdvancedGraphics; }
int GetPerfGeneralGraphics() { return G_GeneralGraphics; }
int GetPerfDefaultGraphics() { return G_DefaultGraphics; }
float GetPerfSpaceDustDetail() { return G_SpaceDustDetail; }
float GetPerfGenericEffectPerf() { return G_GenericEffectPerf; }

bool LoadPerfOptions(char const* FileName, FPerfDisplaySettings* Display)
{
    // . 0x005b38fb: GetUserDataPath's result is not tested.
    char UserPath[MAX_PATH];
    GetUserDataPath(UserPath);

    char IniPath[MAX_PATH];
    _makepath(IniPath, nullptr, UserPath, FileName, nullptr);

    for (int Slot = 0; Slot < kPerfSlotCount; ++Slot)
        G_Slots[Slot] = 0.f;

    bool bOk = false;
    bool bVersionOk = false;
    int Expected = 0;
    int Parsed = 0;

    INI_Reader Ini;
    if (!Ini.open(IniPath, false))
        return false;

    if (Ini.read_header())
    {
        do
        {
            if (Ini.is_header("PerfVersion"))
            {
                if (!Ini.read_value())
                    continue;
                do
                {
                    if (Ini.is_value("version"))
                        bOk = Ini.get_value_float(0) >= 5.f;
                    else if (Ini.is_value("SkipMachineWarnings"))
                        G_bSkipMachineWarnings = Ini.get_value_bool(0);
                } while (Ini.read_value());
                // baseline stashes the version verdict here and restores it after every accepted
                // [PerfOptions] entry, so the last entry cannot decide the whole file.
                bVersionOk = bOk;
            }
            else if (Ini.is_header("PerfCount"))
            {
                if (!Ini.read_value())
                    continue;
                do
                {
                    if (Ini.is_value("count"))
                        Expected = Ftol(Ini.get_value_float(0));
                } while (Ini.read_value());
            }
            else if (Ini.is_header("PerfOptions"))
            {
                if (!bOk)
                {
                    FDUMP(0x100004, "%s(%d) : TRACE_1:General:%s", kPerfOptionsCpp, 0x127,
                          "perfoptions out of date -- reverting to defaults");
                    break;
                }
                if (!Ini.read_value())
                    continue;
                do
                {
                    if (!bOk)
                        break;
                    FPerfOption* Option = FindPerfOption(Ini.get_name_ptr());
                    if (Option == nullptr)
                        continue;
                    float const Value = Ini.get_value_float(0);
                    Parsed += 1;
                    // FCOMP double 0.0 / TEST AH,0x1 / JNZ -- a value
                    // below zero or unordered is rejected.
                    if (!(Value >= 0.f))
                    {
                        bOk = false;
                        bVersionOk = false;
                        continue;
                    }
                    Option->Value = Value;
                    bOk = bVersionOk;
                } while (Ini.read_value());
            }
            else if (Ini.is_header("Display") && Display != nullptr)
            {
                if (!Ini.read_value())
                    continue;
                do
                {
                    if (Ini.is_value("size"))
                    {
                        Display->Width = Ini.get_value_int(0);
                        Display->Height = Ini.get_value_int(1);
                    }
                    else if (Ini.is_value("color_bpp"))
                        Display->ColorBpp = Ini.get_value_int(0);
                    else if (Ini.is_value("depth_bpp"))
                        Display->DepthBpp = Ini.get_value_int(0);
                } while (Ini.read_value());
            }
        } while (Ini.read_header());

        // The file is trusted only when it lists exactly as many known options as it claims.
        if (Parsed != Expected)
            bOk = false;
    }

    Ini.close();

    if (bOk)
        ApplyChangedPerfOptions();

    return bOk;
}

void SavePerfOptions(char const* FileName)
{
    // (module storage-005b4049), cdecl(FileName).
    // GetUserDataPath's result is not tested.
    char UserPath[MAX_PATH];
    GetUserDataPath(UserPath);

    char IniPath[MAX_PATH];
    _makepath(IniPath, nullptr, UserPath, FileName, nullptr);

    FILE* File = fopen(IniPath, "w");
    if (File == nullptr)
        return;

    // = 5.0f, pushed as a double.
    fprintf(File, "[PerfVersion]\nversion=%g\n", 5.0);
    // "TRUE" (the field) when the field is set, else "FALSE".
    fprintf(File, "SkipMachineWarnings=%s\n\n", G_bSkipMachineWarnings ? "TRUE" : "FALSE");
    fprintf(File, "[PerfCount]\ncount=%g\n\n", static_cast<double>(kOptionCount));
    fprintf(File, "[PerfOptions]\n");
    for (unsigned Index = 0; Index < kOptionCount; ++Index)
    {
        // baseline writes only options it has applied at least once (+0x14 >= 0).
        if (G_Options[Index].Applied >= 0.f)
            fprintf(File, "%s = %.2f\n", G_Options[Index].Name,
                    static_cast<double>(G_Options[Index].Applied));
    }
    // always written, straight from the options object
    // (the field / bcc / bd0 / bd4 = g_display +0x08..+0x14).
    fprintf(File, "\n[Display]\n");
    fprintf(File, "size= %d, %d\n", g_display.width, g_display.height);
    fprintf(File, "color_bpp= %d\n", g_display.color_bpp);
    fprintf(File, "depth_bpp= %d\n", g_display.depth_bpp);
    fclose(File);
}
