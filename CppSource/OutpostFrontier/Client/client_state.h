// Author: Giperion.

#pragma once

#include "client_environment.h"

// baseline client state machine the routine / the field. The state objects are IEnvironment
// classes (client_environment.h): the routine SpaceApp, the routine = the field GF (the
// base, COL 0x005e8734), the routine IntroFocus, the routine = the field MissionBriefing
// (COL 0x005e88d4).
// 1 quit, 2 space Init(0), 3 GF Init(from intro), 4 GF slot 10 (state kept), 5 space slot 10
// (state kept), 6 IntroFocus Init(0), 7 reconnect on the field then GF Init(1) or space
// Init(0), 8 MissionBriefing Init(0) (mission_briefing.cpp). The callers store the answer in
// (g_client.current_state).
IEnvironment* EnterClientState(int StateId, IEnvironment* PreviousState = nullptr);

// the character state 7 reconnects with (written by the routine).
struct CHARACTER_ID;
extern CHARACTER_ID G_LoadGameCharacterId;
// a state id the frame enters instead of the current state's answer.
extern int G_PendingClientStateId;
// (cdecl) -- a non-empty CharId: the field = 7, the field = *CharId, and with
// bSkipAutosave IServer::CharacterSkipAutosave. Callers: NN_LoadSave's load 0x00488144 and
// .
void QueueLoadGameCharacter(CHARACTER_ID const* CharId, bool bSkipAutosave);
// The frame's 0x005b29b3..0x005b29be: a queued state (the field) replaces StateId and is
// cleared.
int ConsumePendingClientState(int StateId);

// The call NN_Prompt's modal loops (004740b6, 00474220, 0047439b) and ServerFilterDialog's
// (00563872) inline: the field != 0 -> the field->Update (slot 14, +0x38) with the frame
// time as a float, the answer dropped.
void CallClientStateUpdateSlot(float DeltaSeconds);

// the field != 0 && the field->GetEnvironmentType() (slot 7) == 2. The types:
// 0 space (0x005e0554 -> 004b5010), 1 GF (0x005cae24 -> 0051ca40), 2 IntroFocus (0x005e3034 ->
// ), 3 MissionBriefing (0x005cb094 -> 0043d2e0). So it is "the current state is the
// intro menu".
bool IsClientIntroEnvironment();

// the IntroFocus singleton the field (client_state.cpp).
IEnvironment* GetIntroFocusState();
