// Author: Giperion.

#include "globals.h"
#include "oracle_mode.h" // Oracle instrumentation (P0.5)
#include "common_misc.h"   // SinglePlayer
#include "common_timing.h" // Timing::Delta
#include "neuronruntime_net.h"
#include "menu_sound_arch.h" // PauseAllClientSounds / ResumePausedClientSounds (the routine / 80)

#include <windows.h>

namespace
{
    // baseline OutpostFrontier.exe frame-timer BSS. the routine is the tick, 0042d650..0042d6e0 the
    // accessor family, the routine the main-loop wrapper.
    Timing::Delta G_FrameDelta;

    double G_ElapsedTime = 0.0;   // _DAT_00667d38 — sum of capped dt, never pause-gated
    double G_FrameDtCapped = 0.0; // _DAT_00667d40 — dt clamped to kMaxFrameSeconds
    double G_FrameDtRaw = 0.0;    // _DAT_00667d48 — dt as measured
    double G_AvgFrameTime = 0.0;  // _DAT_00667d60 — seconds/frame over the last window
    // _DAT_00610a00 — seconds accumulated in the window. .data ships the double 10.0 (dwords
    // / 0x40240000 at 0x00610a00 / 0x00610a04), so the very first tick already
    // closes a window; 0x0042d859 / 0x0042d863 zero both halves on every close.
    double G_FpsWindow = 10.0;
    unsigned G_FrameCount = 0;
    unsigned G_FpsWindowFrames = 0;
    // — timeGetTime() of the current frame; seeded at load by the XC entry
    // (0x0060f188: CALL timeGetTime / MOV [0x667d20],EAX).
    unsigned long G_LastTickMs = timeGetTime();
    bool G_bNetworkStalled = false;
    int G_PauseDepth = 0;         // — frame-time getters (0042d680/6a0)
    int G_ServerTickPauseDepth = 0; // — the routine skips IServer::Update
    double G_AccumulatedClientTime = 0.0; // _DAT_0067ecd8 — the routine's own clock

    // — busy-wait fps cap; .data ships 200.0f and both writers (00558a14 /
    // ) take it from COMMON.DLL::RtcSlider.
    float G_FpsCap = 200.0f;
    // — dt clamp, .data ships 1.0f and nothing in the PE ever writes it, so the
    // capped and raw dt only diverge on frames longer than a whole second.
    const float kMaxFps = 1.0f;
    // — the fps window length.
    const double kFpsWindowSeconds = 2.0;

    void TickFrameTimer()
    {
        // .
        double MinFrameSeconds = 1.0 / static_cast<double>(G_FpsCap);
        if (OracleModeActive())
            MinFrameSeconds = OracleFrameSeconds(); // Oracle instrumentation (P0.5): pace at 30 fps
        double Dt = 0.0;
        while (true)
        {
            G_FrameDelta.stop();
            Dt = G_FrameDelta.calc_dt();
            if (Dt >= MinFrameSeconds)
                break;
            Sleep(1);
        }
        G_FrameDelta.swap();
        if (OracleModeActive())
            Dt = OracleFrameSeconds(); // Oracle instrumentation (P0.5): fixed dt

        G_FrameDtRaw = Dt;

        const double MaxFrameSeconds = 1.0 / static_cast<double>(kMaxFps);
        // FCOM + TEST AH,5 + JNP — keep Dt only when Dt < Max (C0 alone), take Max
        // otherwise, including the unordered case (C0|C2|C3 -> even parity -> no jump).
        G_FrameDtCapped = (Dt < MaxFrameSeconds) ? Dt : MaxFrameSeconds;

        G_ElapsedTime += G_FrameDtCapped;
        G_FrameCount += 1;
        G_FpsWindowFrames += 1;
        G_FpsWindow += G_FrameDtCapped;
        if (G_FpsWindow > kFpsWindowSeconds)
        {
            // FILD frames, FDIVR window — the result is seconds per frame.
            G_AvgFrameTime = G_FpsWindow / static_cast<double>(G_FpsWindowFrames);
            G_FpsWindowFrames = 0;
            G_FpsWindow = 0.0;
        }

        G_LastTickMs = timeGetTime();
        G_bNetworkStalled = false;
        // in multiplayer the flag is set when no DirectPlay message has
        // arrived for more than a second -- (the field - CDPClient::GetLastMsgTimestamp())
        // > 0x3e8, unsigned (JBE). GetLastMsgTimestamp is the NeuronRuntime import (NeuronRuntime module storage).
        if (!SinglePlayer())
        {
            if (G_LastTickMs - CDPClient::GetLastMsgTimestamp() > 1000)
                G_bNetworkStalled = true;
        }
    }
}

void TickClientFrameTimer()
{
    // on its own — the pre-init call at 005b1fb8 primes the delta without touching
    // the client clock.
    TickFrameTimer();
}

void UpdateClientTiming()
{
    // (module storage-005b2392) — tick, then accumulate the client clock: single
    // player uses the capped dt (the routine), multiplayer the raw one (the routine). Both
    // getters return 0 while paused, so _DAT_0067ecd8 is a separate clock from G_ElapsedTime.
    // Timing::UpdateGlobalTime is not here: the routine calls it (client_frame_tick.cpp).
    TickFrameTimer();
    if (SinglePlayer())
        G_AccumulatedClientTime += GetClientFrameTimeCapped();
    else
        G_AccumulatedClientTime += GetClientFrameTimeRaw();
}

double GetClientAccumulatedTime()
{
    return G_AccumulatedClientTime;
}

void SeedClientAccumulatedTime(double Seconds)
{
    // module storage..005ab3bf (multiplayer) / 005ab3c9..005ab3d5 (single player):
    // the character-select response's +8 / +0xc dwords go into the field / the field,
    // the two halves of the double the routine advances (FADD / FSTP double ptr [0x67ecd8]).
    G_AccumulatedClientTime = Seconds;
}

float GetClientFrameDeltaSeconds()
{
    return static_cast<float>(SinglePlayer() ? GetClientFrameTimeCapped()
                                             : GetClientFrameTimeRaw());
}

double GetClientFrameTimeCapped()
{
    // — 21 call sites; 0.0 while paused.
    if (G_PauseDepth != 0)
        return 0.0;
    return G_FrameDtCapped;
}

double GetClientFrameTimeRaw()
{
    // — one call site (the routine); 0.0 while paused.
    if (G_PauseDepth != 0)
        return 0.0;
    return G_FrameDtRaw;
}

double GetClientElapsedTime()
{
    // — 13 call sites. Not pause-gated.
    return G_ElapsedTime;
}

unsigned GetClientFrameCount()
{
    // .
    return G_FrameCount;
}

unsigned long GetClientFrameTimestampMs()
{
    // .
    return G_LastTickMs;
}

void PushClientTimingPause()
{
    // .
    G_PauseDepth += 1;
}

void PopClientTimingPause()
{
    // .
    G_PauseDepth -= 1;
}

void SetClientServerTickPause(bool bPause)
{
    // (param1). Counter is the field. 0x0042d6f0..0x0042d716: pausing from 0
    // in single player (Common SinglePlayer) pauses every playing sound (the routine) before
    // the INC. 0x0042d717..0x0042d729: the DEC; reaching 0 in single player resumes the paused
    // ones (the routine, JMP).
    if (bPause)
    {
        if (G_ServerTickPauseDepth == 0 && SinglePlayer())
            PauseAllClientSounds();
        G_ServerTickPauseDepth += 1;
        return;
    }

    G_ServerTickPauseDepth -= 1;
    if (G_ServerTickPauseDepth != 0)
        return;
    if (SinglePlayer())
        ResumePausedClientSounds();
}

bool IsClientServerTickPaused()
{
    // XOR EAX,EAX / TEST [the field] / SETG AL.
    return G_ServerTickPauseDepth > 0;
}

bool ShouldSkipLocalServerTick()
{
    // — baseline frame loop at module storage gates Update+ElapseTime.
    if (!SinglePlayer())
        return false;
    return G_ServerTickPauseDepth > 0;
}

void SetClientFpsCap(float Fps)
{
    // — baseline stores RtcSlider::get_fps_cap (00558a14, entering a base) and
    // RtcSlider::get_default_fps_cap (00558a57, leaving it) with a plain FSTP; no test.
    G_FpsCap = Fps;
}
