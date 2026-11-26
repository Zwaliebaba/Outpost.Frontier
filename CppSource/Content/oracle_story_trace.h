// Oracle instrumentation (design/migration-plan.md, Phase 0; work package P3.5d, decision record
// 0008 point 10 and question 9): the hooks of the Tier B story trace. Each is a no-op unless
// OracleDumpStorySession (oracle_story_session.cpp) is recording, so a game run never writes anything.

#pragma once

struct FMissionScript;

// CreateStory: the controller id of the Story made, for the session.
void OracleStoryNoteStory(unsigned int ControllerId);
// The story missions' factory (StoryMissions.cpp's NewMission): the mission made and its controller
// id, for the session.
void OracleStoryNoteMission(FMissionScript* Mission, unsigned int ControllerId);
// A message to Story's HandleMessage, before its case runs.
void OracleStoryTraceMessage(unsigned int MessageId);
// Story's OnMissionEvent, before it runs.
void OracleStoryTraceEvent(unsigned int EventKind, unsigned int Chapter, unsigned int Controller);
// A pub::Controller::Create of Story (a chapter's mission) or StoryLauncher (Story), with the id it gave.
void OracleStoryTraceCreate(char const* Name, unsigned int ControllerId);
// A pub::Controller::Destroy of Story's, before it is made.
void OracleStoryTraceDestroy(unsigned int ControllerId);
// Story's state when its HandleMessage or OnMissionEvent returns, with GStoryMissionController.
void OracleStoryTraceState(int MissionNum, float DeltaWorth, bool bShipBought, char const* MissionName,
                           unsigned int MissionController);
