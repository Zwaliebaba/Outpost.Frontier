// Author: Giperion.

#include "anim_script_set.h"

#include "NeuronRuntime.h"
#include "common_misc.h"
#include "engine/sceneruntime.h"

#include <new>
#include <string.h>

namespace
{
    // (cdecl) -- the counting callback: ++*Context. The name is not read.
    void __cdecl CountScriptName(char const* /*Name*/, void* Context)
    {
        FAnimScriptSet* Set = static_cast<FAnimScriptSet*>(Context);
        ++Set->Count;
    }

    // (cdecl) -- the filling callback: copy the name into Names[cursor], store
    // Anim +0x18 (GetNamedDuration) of it on the script being enumerated into
    // Durations[cursor], advance the cursor.
    void __cdecl FillScriptName(char const* Name, void* Context)
    {
        FAnimScriptSet* Set = static_cast<FAnimScriptSet*>(Context);
        malloc_strcpy(&Set->Names[Set->FillCursor], Name);
        Set->Durations[Set->FillCursor] =
            NeuronRuntime::Anim->GetNamedDuration(reinterpret_cast<void*>(Set->LoadScript), Name);
        ++Set->FillCursor;
    }
}

void FAnimScriptSet::Init()
{
    // .
    Count = 0;
    Names = nullptr;
    Durations = nullptr;
    Script = 0xffffffffu;
    LoadScript = 0xffffffffu;
    FillCursor = 0;
}

void FAnimScriptSet::Reset()
{
    // .
    Count = 0;
    Names = nullptr;
    Durations = nullptr;
    Script = 0xffffffffu;
    LoadScript = 0xffffffffu;
    FillCursor = 0;
}

void FAnimScriptSet::Destroy()
{
    // . 0x00555a9b..0x00555ab7: malloc_strfree every name.
    int Index = 0;
    for (Index = 0; Index < Count; ++Index)
        malloc_strfree(&Names[Index]);
    // both arrays.
    if (Names != nullptr)
    {
        operator delete(Names);
        Names = nullptr;
    }
    if (Durations != nullptr)
    {
        operator delete(Durations);
        Durations = nullptr;
    }
    // Count = 0, then Anim +0x10 (CloseScript) when there is a script.
    Count = 0;
    if (Script != 0xffffffffu)
    {
        NeuronRuntime::Anim->CloseScript(reinterpret_cast<void*>(Script));
        Script = 0xffffffffu;
    }
    // .
    Script = 0xffffffffu;
    LoadScript = 0xffffffffu;
    Count = 0;
    Names = nullptr;
    Durations = nullptr;
    FillCursor = 0;
}

int FAnimScriptSet::Find(char const* Name) const
{
    // an inlined strcmp against each name in order.
    int Index = 0;
    for (Index = 0; Index < Count; ++Index)
    {
        if (strcmp(Name, Names[Index]) == 0)
            return Index;
    }
    return -1;
}

int FAnimScriptSet::Load(unsigned long NewScript)
{
    // . 0x00555bfa..0x00555bfd: both handles take the argument.
    Script = NewScript;
    LoadScript = NewScript;
    if (NewScript != 0xffffffffu)
    {
        // count the names.
        Count = 0;
        NeuronRuntime::Anim->EnumScriptNames(reinterpret_cast<void*>(&CountScriptName),
                                     reinterpret_cast<void*>(NewScript), this);
        // one pointer and one float per name.
        Names = static_cast<char**>(operator new(static_cast<size_t>(Count) * 4));
        Durations = static_cast<float*>(operator new(static_cast<size_t>(Count) * 4));
        FillCursor = 0;
        // fill them.
        NeuronRuntime::Anim->EnumScriptNames(reinterpret_cast<void*>(&FillScriptName),
                                     reinterpret_cast<void*>(LoadScript), this);
    }
    // .
    int Result = Count;
    LoadScript = 0xffffffffu;
    FillCursor = 0;
    return Result;
}

unsigned long FAnimScriptSet::Play(void* Root, int Index)
{
    // Anim +0x24 (Script, Root, Names[Index], 0, 0).
    return NeuronRuntime::Anim->PlayScript(reinterpret_cast<void*>(Script), Root, Names[Index], nullptr,
                                   nullptr);
}

void AnimScriptSetStopPlayback(unsigned long* Playback)
{
    // Anim +0x38 then Anim +0x28 on the playback, then -1.
    if (*Playback == 0xffffffffu)
        return;
    NeuronRuntime::Anim->StopPlayback(*Playback);
    NeuronRuntime::Anim->DestroyScript(reinterpret_cast<void*>(*Playback));
    *Playback = 0xffffffffu;
}
