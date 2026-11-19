// Helpers shared by the oracle suites (design/migration-plan.md P0.4).
//
// oracle_main.cpp holds the P1.1 and P1.2 suites and main(). A later work
// package adds its suites in oracle_<module>.cpp, declares its entry point
// at the end of this header and calls it from main(). Every suite writes
// one JSON object per line to a file in the output directory; the Rust
// test crates/neuron-core/tests/oracle_<module>.rs renders the same file.

#pragma once

#include <stdio.h>

#include <string>
#include <vector>

namespace oracle
{
    // The fixture directory (the current directory while suites run) and
    // the output directory, both absolute.
    extern std::string GFixtureDir;
    extern std::string GOutputDir;
    // Calls into FDUMP since the counter was last reset.
    extern int GDumps;
    // Set by any suite that cannot read an input or write an output.
    extern bool GFailed;

    void __cdecl OracleDump(int Flags, const char* Format, ...);

    typedef std::vector<unsigned char> Bytes;

    bool ReadWholeFile(const std::string& Path, Bytes& Out);

    // The non-comment lines of an input list; empty lines are inputs too.
    std::vector<std::string> InputLines(const char* Name);

    int HexValue(char Ch);

    // Decoded bytes plus a terminating NUL, so data() is a C string.
    Bytes Unhex(const std::string& Text);

    const char* CStr(const Bytes& Data);

    // Lower-case hex of Length bytes.
    std::string Hex(const void* Data, size_t Length);

    // Lower-case hex of a C string; empty for a null pointer.
    std::string HexC(const char* Text);

    // printf into a std::string (at most 16 KiB).
    std::string Format(const char* Pattern, ...);

    unsigned long long DoubleBits(double Value);
    unsigned int FloatBits(float Value);
    float FloatFromBits(unsigned int Bits);
    double DoubleFromBits(unsigned long long Bits);

    int Sign(int Value);

    void MakeParentDirectories(const std::string& Path);

    // --- Data dumps (Oracle.exe dump) ------------------------------------------
    // The dump runs with the folder that holds DATA and EXE as the current
    // directory. Paths are '/'-separated and relative to that folder.

    // Every file under Relative, recursively, appended to Out (unsorted).
    void ListDataFiles(const std::string& Relative, std::vector<std::string>& Out);

    // <output directory>\<Kind>\<Relative>.jsonl, with backslashes.
    std::string DataOutputFile(const char* Kind, const std::string& Relative);

    // The first four bytes of a file, fewer for a shorter one.
    std::string FileMagic(const std::string& Relative);

    // FNV-1a (64-bit) of Length bytes as 16 lower-case hex digits.
    std::string Fnv64(const void* Data, size_t Length);

    class Suite
    {
    public:
        // A suite file in the output directory.
        explicit Suite(const char* Name);

        // A file at a full path; missing directories are created.
        explicit Suite(const std::string& Path);

        ~Suite();

        void Line(const std::string& Text);

    private:
        Suite(const Suite&);
        Suite& operator=(const Suite&);

        FILE* File;
    };
}

// Suite entry points of later work packages, called from main() in order.

// P1.4, oracle_math.cpp: neuron-core::math against x86math.dll.
void RunMathSuites();

// Oracle.exe catalog <resource DLL directory> <output directory>
// (oracle_catalog.cpp, work package P1.6): a separate mode, not a suite.
// Its output is game text and is never committed.
void RunCatalogDump();

// Oracle.exe sim <data root> <scenario file> <out folder> [--runs N] [--seed S] (oracle_sim.cpp,
// oracle_sim_spread.cpp, work package P2.3): a separate mode, not a suite. Argv is main's.
// Its output is made from the game data and is never committed.
int RunSimMode(int Argc, char** Argv);

// Oracle.exe gdi-fonts <fixture directory> <output directory> (oracle_gdi_fonts.cpp, work
// package P4.12): GDI's text metrics on this machine for the faces of
// crates/neuron-client/tests/fixtures/oracle/gdi_fonts.list. A separate mode, not a suite: its
// output depends on the fonts installed. Argv is main's.
int RunGdiFontsMode(int Argc, char** Argv);

// Oracle.exe render-replay <vectors directory> <output directory> (oracle_render_replay.cpp,
// record 0004 point 15, work package P4.3): plays version-1 renderer traces into
// DX8RENDERER.dll and writes each selected frame's image. Argv is main's.
int RunRenderReplayMode(int Argc, char** Argv);

// P1.9, oracle_net.cpp: wire primitives and quantisers.
void RunNetSuites();

// P1.5 (oracle_util.cpp): rect, spline, MD5 text form, Timing::Delta, ErrorHandler.
void RunUtilSuites();

// P1.3 (oracle_vfs.cpp): the UTF container and the file systems of ReadFile.
void RunVfsSuites();

// P1.7 (oracle_sur.cpp): .sur files through PhyArch::LoadSurfaces and their IVP
// compact surfaces. Needs ReadFile's factories, which RunVfsSuites registers.
void RunSurSuites();

// P1.7 (oracle_anim.cpp): channel archetypes and animation scripts of
// SceneRuntime.dll. Runs after RunVfsSuites, which registers ReadFile.
void RunAnimSuites();

// P1.7 (oracle_fx_data.cpp): node and effect libraries of .ale files through
// particlefx.dll. Needs the UTF factory RunVfsSuites registers.
void RunFxDataSuites();

// P1.7 (oracle_shading.cpp): TGA levels, DDS image information, texture format maps, texture
// libraries through a recording renderer, material schemas and material libraries of Shading.
void RunShadingSuites();

// P1.7 (oracle_mesh_data.cpp, oracle_mesh_data_dfm.cpp): the mesh nodes of
// RendComp and the .dfm tables of Deformable2. Runs after RunVfsSuites,
// which registers the file systems it opens.
void RunMeshDataSuites();

// Data dumps of later work packages, called by Oracle.exe dump after the INI
// dumps. Files is every file under DATA and EXE, sorted; each dump picks its
// own, writes <output>\<kind>\<file>.jsonl per file and <output>\<kind>\files.txt,
// and crates/neuron-core/tests compares them in a data_dump_<kind> test.

// P1.3 (oracle_vfs.cpp): every UTF container (first bytes "UTF ") through the
// UTF file system; kind "utf".
void RunVfsDataDump(const std::vector<std::string>& Files);

// P1.7 (oracle_sur.cpp): every .sur file through PhyArch::LoadSurfaces, with its
// compact surfaces; kind "sur". Runs after RunVfsDataDump, which registers
// ReadFile's factories.
void RunSurDataDump(const std::vector<std::string>& Files);

// P1.7 (oracle_anim.cpp): every UTF container whose Animation node
// IAnimation2::OpenScript opens, through SceneRuntime.dll; kind "anim".
void RunAnimDataDump(const std::vector<std::string>& Files);

// P1.7 (oracle_fx_data.cpp): every .ale file (a UTF container) through
// particlefx.dll's FxNodeLibrary and FxEffectLibrary; kind "fx". Runs after
// RunVfsDataDump, which registers the UTF factory.
void RunFxDataDump(const std::vector<std::string>& Files);

// P1.7 (oracle_shading.cpp): every UTF container with a "Texture library" node (kind
// "texture": per texture node the renderer calls, result and record) and every one with a
// "Material library" or "MaterialAnim" node (kind "material": per material its type, kind and
// parameter values; per animation its key data).
void RunShadingDataDump(const std::vector<std::string>& Files);

// P1.7 (oracle_mesh_data.cpp, oracle_mesh_data_dfm.cpp): every UTF .vms,
// .3db, .cmp and .sph through the RendComp loaders, kind "mesh"; every UTF
// .dfm through the Deformable2 loaders, kind "dfm". Runs after
// RunVfsDataDump, which registers the file systems it opens.
void RunMeshDataDump(const std::vector<std::string>& Files);

// P1.8 (oracle_scene.cpp): compound archetypes, hardpoints, instance trees,
// channel sampling, joints and the animation mixer of SceneRuntime.dll. Runs
// after RunVfsSuites, which registers the file systems it opens.
void RunSceneSuites();

// P2.3 (oracle_physics.cpp): Collision damage, CDeadReckonedVector and
// CRemotePhysicsSimulation of Common.dll. Loads x86math.dll (OracleLoadMathEngine).
void RunPhysicsSuites();

// P1.8 (oracle_scene.cpp): every UTF .cmp, .3db and .dfm through
// IEngine::CreateArchetypeHandle with the Hardpoint component, kind "scene";
// every one whose script IAnimation2::OpenScript opens, its animations played
// on an instance through the mixer, kind "scene_eval".
void RunSceneDataDump(const std::vector<std::string>& Files);

// P2.1 (oracle_game_data.cpp): the game databases of Common, loaded from the [Data] list of
// EXE\OutpostFrontier.ini through Common's loaders; kinds "constants" and the other
// databases crates/game-sim/tests/data_dump.rs names. Runs after RunVfsDataDump, which
// registers ReadFile's factories.
void RunGameDataDump(const std::vector<std::string>& Files);

// Oracle.exe game-sim <fixture directory> <output directory> (oracle_game_suites.cpp, work
// package P2.1): the suites of crates/game-sim/tests/oracle_vectors.rs, a separate mode so that
// Common's string cache has seen nothing but its static initialiser.
void RunGameSimSuites();
// P2.5: the AI suites (oracle_ai.cpp), run after RunGameSimSuites in game-sim mode.
void RunAiSuites();
// P2.5: the combat suite (oracle_ai_combat.cpp), run by RunAiSuites.
void AiCombatSuite();
// P2.4: CEGun's turret joints (oracle_game_turret.cpp), run after RunAiSuites.
void RunTurretSuites();
// Loads x86math.dll and checks for its 3DMathEngine component (oracle_scene.cpp).
bool OracleLoadMathEngine();

// oracle_vfs.cpp: registers ReadFile's file-system factories once (for modes that do not run
// RunVfsSuites or RunVfsDataDump first).
void OracleRegisterReadFile();

// Oracle.exe neuron-client <fixture directory> <output directory> (oracle_scene_script.cpp,
// work package P4.14): the suites of crates/neuron-client/tests, over
// crates/neuron-client/tests/fixtures/oracle. The Lua state after scene-script buffers,
// through the reference's own Lua 3.2 compiled into Oracle.exe.
void RunNeuronClientSuites();

// P4.12, P4.13, P4.14 (oracle_text.cpp), called by RunNeuronClientSuites: the VC6 number texts of
// msvcrt_numeric_format.h and XMLReader::read_buffer (RDLX markup through MSXML), over
// crates/neuron-client/tests/fixtures/oracle/{number_text,rdl_markup}.list.
void RunTextSuites();
// P4.12 (oracle_rich_text.cpp), called by RunTextSuites: Common.dll's LayoutRichText, the list
// functions, StyleCollection and lex_compare on a recording TextRenderContext, over
// crates/neuron-client/tests/fixtures/oracle/rich_text.list.
void RunRichTextSuites();
// P4.10 (oracle_audio.cpp): the software mixer of SoundManager.dll, compiled in, over
// crates/neuron-client/tests/fixtures/oracle/audio; called by RunNeuronClientSuites.
void RunAudioSuites();
// P4.4 (oracle_texture.cpp): the texture library at run time and, through BigImageSuite
// (oracle_big_image.cpp), ximage's BigImage, over
// crates/neuron-client/tests/fixtures/oracle/{texture_runtime,big_image}.list; called by
// RunNeuronClientSuites.
void RunTextureSuites();
void BigImageSuite();
// P4.5 (oracle_material.cpp): the material regular expressions, library, global records,
// animation and batcher of Shading.dll, over
// crates/neuron-client/tests/fixtures/oracle/material_*.list; called by RunNeuronClientSuites,
// last (its batcher cases leave their INI in the manager's profile parser). It also runs the
// P4.6 mesh render suite and the P4.7 deformable suite (oracle_deformable.cpp, compiled into
// oracle_mesh_data_dfm.cpp), which need its math engine.
void RunMaterialSuites();
// P4.8 (oracle_particles.cpp): particlefx.dll's ramp evaluation and simulation (its sources
// compiled in), over crates/neuron-client/tests/fixtures/oracle/particle_{ramp,sim}.list; called
// by RunNeuronClientSuites before RunMaterialSuites.
void RunParticleSuites();
// P4.13 (oracle_ui.cpp, oracle_ui_quads.cpp, oracle_ui_controls.cpp): the client UI toolkit's
// page descriptors (ui_page_desc.cpp compiled in), screen-quad batcher (screen_quad_state.cpp
// compiled in) and generic controls (the Win3D manager and the controls compiled in), over
// crates/neuron-client/tests/fixtures/oracle/ui_{page_desc,quad_batch,controls}.list; called by
// RunNeuronClientSuites. RunUiSuites runs the three.
void RunUiSuites();
void RunUiQuadSuites();
void RunUiControlSuites();

// P4.14 (oracle_scene_script.cpp): every .thn under DATA through lua_dobuffer in a fresh
// Lua state, and the state after it; kind "thn".
void RunSceneScriptDataDump(const std::vector<std::string>& Files);

// P4.14 (oracle_scene_script_build.cpp): the parser's second pass, FSceneScriptLuaLoadParser::
// Build, against recording stand-ins for the engine, the COM context and the component
// manager, over crates/neuron-client/tests/fixtures/oracle/scene_script_build.list; called
// from RunNeuronClientSuites.
void SceneScriptBuildSuite();

// P4.14 (oracle_scene_script_runtime.cpp): param curves, spline paths and scene playback,
// compiled in from ParamCurve.cpp, SplinePath.cpp and SceneScriptScene.cpp, over
// crates/neuron-client/tests/fixtures/oracle/scene_script_{curve,spline,playback}.list;
// called from RunNeuronClientSuites after SceneScriptBuildSuite.
void SceneScriptRuntimeSuites();

// P4.14 (oracle_scene_script_build.cpp): every .thn under DATA through both passes of the
// parser in a fresh module state, and the engine calls of the second; kind "thn_build".
void RunSceneScriptBuildDataDump(const std::vector<std::string>& Files);
