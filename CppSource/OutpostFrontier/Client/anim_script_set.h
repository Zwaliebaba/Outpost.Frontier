// Author: Giperion.

#pragma once

// The named-script table a RoomObject keeps for its NeuronRuntime::Anim script (baseline OutpostFrontier.exe
// , no source path string of its own). RoomControl holds one at +0x4c
// (the routine / the routine) and Gf::ObjectProp holds one at +0x4c (the routine); the mesh
// loader the routine fills it from the .cmp's own file system.

// Layout (sizeof 0x18): +0x00 Count, +0x04 char** Names (malloc_strcpy copies),
// +0x08 float* Durations, +0x0c Script (the IAnimation2 script handle, -1 = none),
// +0x10 the script being enumerated (only valid inside Load), +0x14 the fill cursor.
struct FAnimScriptSet
{
    int Count;                 // +0x00
    char** Names;              // +0x04
    float* Durations;          // +0x08
    unsigned long Script;      // +0x0c
    unsigned long LoadScript;  // +0x10
    int FillCursor;            // +0x14

    // (thiscall, no arguments) -- zero the table, both handles -1.
    void Init();
    // the same stores as the routine, in the ECX-relative form.
    void Reset();
    // free every name, both arrays, and close the script through Anim +0x10.
    void Destroy();
    // (thiscall, RET 4) -- the index of the name equal to Name (strcmp), or -1.
    int Find(char const* Name) const;
    // (thiscall, RET 4) -- take Script and, when it is not -1, count its names
    // (Anim +0x1c with the routine), allocate both arrays and fill them (Anim +0x1c with
    // ). Returns +0x00.
    int Load(unsigned long NewScript);
    // (thiscall, RET 8) -- Anim +0x24 (PlayScript) on Script with Names[Index].
    unsigned long Play(void* Root, int Index);
};

static_assert(sizeof(FAnimScriptSet) == 0x18, "AnimScriptSet sizeof 0x18");

// (thiscall, RET 4; ECX is not read) -- stop the playback *Playback through
// Anim +0x38, release it through Anim +0x28 and set it to -1. Nothing when it is already -1.
void AnimScriptSetStopPlayback(unsigned long* Playback);
