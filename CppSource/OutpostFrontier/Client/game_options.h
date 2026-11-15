// Author: Giperion.

#pragma once

// the global display/debug option table, and the loop at 0x005621c0 that
// fills it.

// 138 call sites across the whole in-flight game read this table, always as a guard:
// "[EFFECTS] and PARTICLES are on, so draw them", "HUD is on, so notify the HUD", and so on.
// Until now those guards read as an opaque `the routine(0x26)` in the listing, which is why so
// much of the in-flight code looked unportable.

// What the table actually is:

//   * `the routine(Index)` is `if (Index < 0x46) return Values[Index]; return 0;` over a
//     70-entry array of ints at .data 0x00679be8. The array is a member of the options object
//     whose `this` is 0x00679bc0 -- it sits at +0x28, which is why the whole PE contains exactly
//     ONE absolute reference to 0x00679be8 (the read itself) and the writer is invisible to a
//     search for that address.

//   * The writer is the first loop of `the routine` (the options-object init WinMain calls at
// , before RegisterClassA):
//         for (Off = 0; Off < 0x470; Off += 0x10)
//             This[*(int*)(0x006143b8 + Off) + 10] = *(int*)(0x006143c0 + Off);
//     `+ 10` dwords is `+0x28` bytes -- exactly the array. So it scatters a table of
//     0x470 / 0x10 = 71 descriptors into the 70 slots.

//   * Each descriptor is 16 bytes: {int Index; char const* Name; int Default; int bSelectable}.
//     The names are the in-game debug options menu: category headers in brackets and the
//     toggles under them.

// The important consequence for the port: **nothing else in baseline ever writes these values.**
// The option names are referenced from the descriptor table and from nowhere else in the PE --
// the menu that would have edited them was stripped from the shipped build -- so every option
// keeps its default for the life of the process, and every one of those 138 guards is decidable
// ahead of time. See implementation notes.

// The option indices, in the numeric order the array is indexed by. The bracketed entries are
// the menu's category headers; baseline queries those exactly like the toggles under them, and
// several guards test a header AND a toggle together (`[EFFECTS] && SUN && SUN_EFFECTS`).
enum EGameOption
{
    kGameOptOutlinesGroup           = 0x00,     // "[OUTLINES]"
    kGameOptPhysWires               = 0x01,     // "PHYS WIRES"
    kGameOptPhysSpheres             = 0x02,     // "PHYS SPHERES"
    kGameOptShields                 = 0x03,     // "SHIELDS"
    kGameOptPhantoms                = 0x04,     // "PHANTOMS"
    kGameOptHotspots                = 0x05,     // "HOTSPOTS"

    kGameOptInterfaceGroup          = 0x06,     // "[INTERFACE]"
    kGameOptButtons                 = 0x07,     // "BUTTONS"
    kGameOptText                    = 0x08,     // "TEXT"
    kGameOptNeuronet                = 0x09,     // "NEURONET"
    kGameOptHud                     = 0x0a,     // "HUD"
    kGameOptShipWires               = 0x0b,     // "SHIP WIRES"
    kGameOptTargetWindowLock        = 0x0c,     // "TARGET WINDOW LOCK"
    kGameOptAutoOpenTargetWindow    = 0x0d,     // "AUTO OPEN TARGET WINDOW"
    kGameOptDrawMouse               = 0x0e,     // "DRAW MOUSE"
    kGameOptDisplayShipIds          = 0x0f,     // "DISPLAY SHIP IDS"
    kGameOptDisplayInterfaceTiming  = 0x10,     // "DISPLAY INTERFACE TIMING"
    kGameOptDisplayPlayerStats      = 0x11,     // "DISPLAY PLAYER STATS"
    kGameOptSwitchTargetClearsMan   = 0x12,     // "SWITCHING TARGETS CLEARS MANEUVER"
    kGameOptDrawTextControls        = 0x13,     // "DRAW TEXT CONTROLS"
    kGameOptDrawButtonControls      = 0x14,     // "DRAW BUTTON CONTROLS"
    kGameOptDrawStaticControls      = 0x15,     // "DRAW STATIC CONTROLS"
    kGameOptDrawScrollBarControls   = 0x16,     // "DRAW SCROLL BAR CONTROLS"
    kGameOptInverseMouse            = 0x17,     // "INVERSE MOUSE"
    kGameOptLighting                = 0x18,     // "LIGHTING"

    kGameOptBackgroundGroup         = 0x19,     // "[BACKGROUND]"
    kGameOptStars                   = 0x1a,     // "STARS"

    kGameOptObjectsGroup            = 0x1b,     // "[OBJECTS]"
    kGameOptCharacters              = 0x1c,     // "CHARACTERS"
    kGameOptShips                   = 0x1d,     // "SHIPS"
    kGameOptProjectiles             = 0x1e,     // "PROJECTILES"
    kGameOptDebris                  = 0x1f,     // "DEBRIS"
    kGameOptSolar                   = 0x20,     // "SOLAR"
    kGameOptAsteroids               = 0x21,     // "ASTEROIDS"
    kGameOptDynAsteroids            = 0x22,     // "DYN_ASTEROIDS"
    kGameOptPlanets                 = 0x23,     // "PLANETS"
    kGameOptSun                     = 0x24,     // "SUN"

    kGameOptEffectsGroup            = 0x25,     // "[EFFECTS]"
    kGameOptParticles               = 0x26,     // "PARTICLES"
    kGameOptNebula                  = 0x27,     // "NEBULA"
    kGameOptSunEffects              = 0x28,     // "SUN_EFFECTS"
    kGameOptFlashlights             = 0x29,     // "FLASHLIGHTS"
    kGameOptTradeLanes              = 0x2a,     // "TRADE_LANES"
    kGameOptShadows                 = 0x2b,     // "SHADOWS"
    kGameOptAsteroidExt             = 0x2c,     // "ASTEROID_EXT"

    kGameOptAudioGroup              = 0x2d,     // "[AUDIO]"
    kGameOptVolMusic                = 0x2e,     // "VOL_MUSIC"
    kGameOptVolVoice                = 0x2f,     // "VOL_VOICE"
    kGameOptVolSfx                  = 0x30,     // "VOL_SFX"
    kGameOptVolInterface            = 0x31,     // "VOL_INTERFACE"
    kGameOptVolAmbience             = 0x32,     // "VOL_AMBIENCE"
    kGameOptVoiceProfileOffset      = 0x33,     // "VOICE PROFILE OFFSET"
    kGameOptPitchBendSounds         = 0x34,     // "PITCH_BEND_SOUNDS"

    kGameOptPlayerGroup             = 0x35,     // "[PLAYER]"
    kGameOptAutoAim                 = 0x36,     // "AUTO_AIM"
    kGameOptDisableCollide          = 0x37,     // "DISABLE_COLLIDE"
    kGameOptSurfaceHitEffects       = 0x38,     // "SURFACE_HIT_EFFECTS"
    kGameOptSplashHitEffects        = 0x39,     // "SPLASH_HIT_EFFECTS"

    kGameOptMiscGroup               = 0x3a,     // "[MISC]"
    kGameOptAmbient                 = 0x3b,     // "AMBIENT"
    kGameOptDetailLevels            = 0x3c,     // "DETAIL_LEVELS"
    kGameOptCharPlaces              = 0x3d,     // "CHAR_PLACES"
    kGameOptLikePetal               = 0x3e,     // "LIKE_PETAL"
    kGameOptLightsDebug             = 0x3f,     // "LIGHTS_DEBUG"
    kGameOptParticleFxLod              = 0x40,     // "PARTICLEFX_LOD"
    kGameOptCommLog                 = 0x41,     // "COMM_LOG"

    kGameOptParticleFxDebugGroup       = 0x42,     // "[PARTICLEFX_DEBUG]"
    kGameOptBatchParticles          = 0x43,     // "BATCH_PARTICLES"
    kGameOptUseDynVb                = 0x44,     // "USE_DYN_VB"
    kGameOptCopyToDynVb             = 0x45,     // "COPY_TO_DYN_VB"

    kGameOptCount                   = 0x46
};

// One row of the descriptor table at .data 0x006143b8. baseline's record is
// {int Index; char const* Name; int Default; int bSelectable} and this is a literal
// transcription of it, kept in baseline's own row order -- which is MENU order, not index order:
// "SHIP WIRES" is row 18 but index 0x0b, and index 0x2f appears twice.
struct FGameOptionDesc
{
    int         Index;
    char const* Name;
    int         DefaultValue;
    int         bSelectable;    // 0 for the bracketed category headers, 1 for the toggles
};

// The 71 descriptors, in baseline's row order.
extern const FGameOptionDesc GGameOptionDescs[];
extern const int GGameOptionDescCount;

// The first loop of the routine -- the scatter of the descriptor defaults into the value array --
// is LoadStartupConfig itself (startup_config.cpp); WinMain runs it before the window class is
// registered, so it is in effect before anything queries an option.

// . Out-of-range indices give 0, exactly as baseline's `if (Index < 0x46)` does.

// The compare is signed, as baseline's JGE: a negative index reads the options-object fields in
// front of the array. No caller in the PE passes anything but a small non-negative constant.
int GetGameOption(int Index);

// The option's name. baseline keeps these strings but no surviving code reads them: the debug menu
// that would have edited the options was stripped from the shipped build, which is why the values
// never move off their defaults. Exposed here for logging. Null for an unknown index.
char const* GetGameOptionName(int Index);

// -------------------------------------------------------------------------------------------
// Session/game-mode flags word -- module storage. A SEPARATE global from the debug option table
// above (different address, mutated at runtime, no descriptor table).

// set or clear the bits in Mask, then send the new word to the server through
// IServer slot 77 SetInterfaceState(the field, &word, 4) when the field is set.
void SetGameSessionFlags(unsigned Mask, bool bSet);

// raw overwrite, no broadcast at all in baseline either.
void AssignGameSessionFlags(unsigned Value);

// `(the field & Mask) != 0`. Named here for symmetry with the setters above;
// itself stays a separate implementation tracker item (blocked, not this pair's to reclassify).
bool TestGameSessionFlags(unsigned Mask);
