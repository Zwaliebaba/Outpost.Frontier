// Oracle instrumentation (work package P0.5, design/reference-build.md section 8).

#pragma once

// The client's `+oracle` mode. Started by
//
//     OutpostFrontier.exe +oracle <script file> <output folder>
//
// it forces a windowed 1024x768 32-bit display, a fixed frame time of 1/30 s and a constant
// rand() seed, runs the script (oracle_mode.cpp has the format), and writes line-oriented JSON
// into the output folder: session.jsonl, tree.jsonl, keymap.jsonl, layout.jsonl,
// iclient.jsonl, server.jsonl and shot_NNNN_<screen>.png. Without `+oracle` every hook below returns at
// once and the client behaves as before: the hooks only read, except the ones named "force".
//
// Hook sites (each one line in an existing file):
//   main.cpp WinMain                  OracleParseCommandLine after FClientCommandLine::Parse
//   engine_init.cpp InitializeEngine  OracleForceDisplayMode after ApplyDisplayCmdlineOverrides,
//                                     OracleSeedRandom after srand
//   client_timing.cpp TickFrameTimer  OracleFrameSeconds pacing and the fixed dt
//   client_frame_tick.cpp             OracleFrameTick at the start of TickClientFrame
//   client_state.cpp EnterClientState, menu_enter_state.cpp EnterMenuState,
//   menu_universe_base.cpp ChangeClientLocation
//                                     OracleNoteScreen
//   menu_keymap.cpp, ui_page_desc.cpp, wincontrol_base.cpp, nn_preferences.cpp,
//   hud_element_list.cpp              the OracleOn*Loaded dumps
//   menu_universe_base.cpp LoadLocationBase
//                                     OracleOnRoomLoaded after RoomData::load
//   client_exports.cpp                OracleWrapClientInterface / OracleWrapChatClientInterface
//   win3d.cpp                         Win3DForEachRegistered (read-only registry walk)

#include <string>

struct FClientCommandLine;
struct DisplaySettings;
struct Win3D;
struct FUiPageData;
class HUDItem;
struct FCliAsteroid;
struct FNebulaEffect;
class RoomData;
class NnPreferences;
class IClient;
class IChatClient;

// True when the command line held `+oracle` and the session started.
bool OracleModeActive();

// WinMain, right after FClientCommandLine::Parse. Looks for the whole token `+oracle` (any
// case); with it, the next two tokens are the script and the output folder. A missing
// argument, an unreadable script or an output folder that cannot be created ends the process
// with exit code 3 and a line on the debugger output.
void OracleParseCommandLine(FClientCommandLine const* CommandLine);

// The fixed frame time, 1/30 s.
double OracleFrameSeconds();

// force: windowed, 1024x768, 32-bit colour (InitializeEngine's local render mode).
void OracleForceDisplayMode(DisplaySettings* Mode);

// force: srand(kOracleRandSeed) after InitializeEngine's srand(timeGetTime()).
void OracleSeedRandom();

// Start of TickClientFrame: the frame counter, the delayed screen dumps and the script.
void OracleFrameTick();

// A screen change: Kind is "client" (EnterClientState id), "menu" (EnterMenuState state) or
// "room" (ChangeClientLocation location). Logged, and the tree and a screenshot are dumped
// kOracleSettleFrames frames later.
void OracleNoteScreen(char const* Kind, unsigned Id);

// Parsed-file dumps.
void OracleOnKeyMapLoaded(char const* Phase, int Result);
void OracleOnPageDescLoaded(char const* Path, bool bOk, FUiPageData const* Data);
void OracleOnWinControlIni(char const* Path, Win3D* Control);
void OracleOnKeyListLoaded(char const* Path, NnPreferences const* Preferences);
void OracleOnOptListLoaded(char const* Path, bool bOpened, NnPreferences const* Preferences);
void OracleOnHudElementsLoaded(char const* Path, HUDItem* Head, int StaticBlend);
// menu_universe_base.cpp LoadLocationBase, after RoomData::load: the room's file as read.
void OracleOnRoomLoaded(RoomData const* Room);

// audio.jsonl (P5.5): a `snd` record for each call the game makes into the client's sound
// layer (menu_sound_arch.cpp, sound_config.cpp), written as the call returns: the method's
// name, its unsigned then its float arguments, and its result (Result < 0: none).
void OracleOnSoundCall(char const* Method, unsigned const* Uints, unsigned UintCount,
                       float const* Floats, unsigned FloatCount, int Result);
// ApplyLocationAudio (menu_universe_base.cpp): a `locaudio` record with the location whose
// room's music and ambience the calls that follow set, and the keep-audio flag.
void OracleOnLocationAudio(unsigned LocationId, bool bKeepAudio);

// CliAsteroidLoadFieldIni (cli_asteroid.cpp, P5.8): an `asteroidfield` record in
// layout.jsonl with the values the field's file gave, written after its sections are read and
// before the load applies them (or with opened false when the file did not open).
void OracleOnAsteroidFieldRead(char const* Path, FCliAsteroid const* Field, int Lod, bool bOpened);
// FNebulaEffect::Load (cli_nebula.cpp, P5.8): a `nebula` record in layout.jsonl with the
// values its file gave (those the constructor, Load or the section readers set), written after
// the sections and before the rebuild (or with opened false when the file did not open).
void OracleOnNebulaRead(char const* Path, FNebulaEffect const* Nebula, bool bOpened);

// GetClientInterface / GetChatClientInterface: the logging proxy when the oracle is on,
// otherwise Real unchanged.
IClient* OracleWrapClientInterface(IClient* Real);
IChatClient* OracleWrapChatClientInterface(IChatClient* Real);

// ---- shared with the oracle's own files ------------------------------------------------

enum EOracleStream
{
    kOracleSession,
    kOracleTree,
    kOracleKeyMap,
    kOracleLayout,
    kOracleIClient,
    kOracleServer,
    kOracleAudio,
    kOracleStreamCount
};

// Appends Line and a newline to the stream's file and flushes it. Thread-safe.
void OracleWriteRecord(EOracleStream Stream, std::string const& Line);

// TickClientFrame calls since the session started (the first frame is 1).
unsigned OracleFrameNumber();

// The next IClient call number, from 1; thread-safe.
unsigned OracleNextIClientSeq();

// The logging proxy (oracle_iclient_proxy.cpp, generated by make_oracle_iclient.py): one
// static object that implements both interfaces and forwards every call to the real one it
// was last handed.
IClient* OracleClientProxyFor(IClient* Real);
IChatClient* OracleChatClientProxyFor(IChatClient* Real);
