// Oracle instrumentation (design/migration-plan.md, Phase 0; work package P3.7b, decision record
// 0008 points 3 and 10): the hooks of the Tier B router and machine trace. Each is a no-op unless
// OracleDumpRmSession (oracle_rm_session.cpp) is recording, so a game run never writes anything.

#pragma once

struct SMEventData;
class StateMachineMessage;
class FDestroyMission;

// A handler's registration ("register") or removal ("unregister") with the router.
void OracleRmTraceHandler(char const* Op, unsigned int HandlerId);
// A message routed, after its delivery options are set.
void OracleRmTraceRoute(StateMachineMessage const* Message);
// A delivery, at once or from the router's timer; Found 1 when a handler had the receiver's id.
void OracleRmTraceDeliver(StateMachineMessage const* Message, bool bFound);
// A machine's slot 2 called: recorded for an enter (1) or exit (2) event in a state >= 0.
void OracleRmTraceSm(unsigned int MachineId, char const* Kind, SMEventData const* Event, int State);
// A mission's end events (fail, succeed, remove_player, end).
void OracleRmTraceMission(char const* Event, long long Value);
// CreateDestroyMission: the last DestroyMission made and its controller id, for the session.
void OracleRmNoteMission(FDestroyMission* Mission, unsigned int ControllerId);
