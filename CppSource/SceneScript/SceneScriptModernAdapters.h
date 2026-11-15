// Author: Giperion.

#pragma once

struct INeuronComponent;

// Modern owners for a handful of baseline SceneScript helpers whose original
// implementation was coupled to raw VC6 register/stack conventions.  The
// public API preserves the observed operations while keeping ownership in
// ordinary C++.

// NOTE (2026-09-10 cleanup): this class used to declare many more methods
// (SwapRecordValues, FinalizeParameterStream, PrependRecordBlock,
// EnsureRecordCapacity, CloseCapture, MatchCapture, ReturnDispatchValue,
// VisitDescending[WithCleanup], VisitStepped, RepeatWithCleanup,
// CleanupRepeated, plus their supporting FSceneScriptModernRecordStorage /
// FSceneScriptModernParameterStream / FSceneScriptModernCaptureState /
// FSceneScriptModernRepeatState structs).  Two independent audit passes (SONNET-L3
// and SONNET-L4, 2026-09-10) found these were fabricated "modern owner"
// explanations for FUN_ addresses that are actually plain vendored Lua 3.2.2
// internals (luaM_growaux, luaV_closure, lbuiltin.c's swap/auxsort,
// lstrlib.c's end_capt/match_capture/match) or compiler-generated
// EH-vector construct/destruct thunks used only by RB-tree helper arrays
// that the port already replaces with contiguous/value-owned containers
// (FParamCurve::Slot13Rebuild, FSceneScriptScene::RebuildPending).  None of those
// methods had a single real caller anywhere in src/SceneScript, so they were
// deleted after the implementation tracker items pointing at them were re-attributed
// to their real owners. See implementation notes, SONNET-CLEANUP (2026-09-10).
class FSceneScriptModernAdapters final
{
public:
    static char* FormatDiagnostic(char* Destination, const char* Format, ...);

    // Modern owner for the baseline 06f4a910/06f4a930/06f4a950 cleanup
    // thunks: release a non-null interface and always clear the owning slot.
    // The interface pointer is borrowed only for the call; no baseline wrapper
    // or VC6 smart-pointer layout crosses this boundary.
    static void ReleaseOwnedComponent(INeuronComponent*& Component);

    // Modern source-string owner for baseline the routine/lua_dostring:
    // measure a NUL-terminated chunk without allowing size_t-to-int wrap.
    static bool MeasureSourceLength(const char* Source, int& Length);

    // Modern value-tag owner for baseline the routine/lua_rawgettable.
    static bool IsLuaTableTag(int TypeTag);

    // Modern count owner for baseline the routine/luaV_pack: negative
    // vararg counts are observable as an empty sequence.
    static int NormalizeVarargCount(int RequestedCount);

    // Modern diagnostic owner for baseline the routine/call_arith.
    static const char* GetArithmeticDiagnostic();

};
