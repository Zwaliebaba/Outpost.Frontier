// Author: Giperion.

#include "SceneScriptModernAdapters.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "neuroncomponents.h"

#include <cstring>
#include <limits>
#include <cstdarg>
#include <cstdio>

char* FSceneScriptModernAdapters::FormatDiagnostic(char* Destination, const char* Format, ...)
{
    va_list Arguments;
    va_start(Arguments, Format);
    _vsnprintf(Destination, 8192, Format, Arguments);
    va_end(Arguments);
    return Destination;
}

void FSceneScriptModernAdapters::ReleaseOwnedComponent(INeuronComponent*& Component)
{
    // TEST owned slot, dispatch vtable slot +8 (Release), then
    // store null.  Clearing after Release preserves the baseline observable
    // state even when the interface implementation observes its final call.
    if (Component != nullptr)
    {
        Component->Release();
        Component = nullptr;
    }
}

bool FSceneScriptModernAdapters::MeasureSourceLength(const char* Source, int& Length)
{
    Length = 0;
    if (Source == nullptr)
        return false;

    const std::size_t Size = std::strlen(Source);
    if (Size > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return false;
    Length = static_cast<int>(Size);
    return true;
}

extern "C" int SceneScriptModernMeasureSourceLength(const char* Source, int* Length)
{
    if (Length == nullptr)
        return 0;
    return FSceneScriptModernAdapters::MeasureSourceLength(Source, *Length) ? 1 : 0;
}

bool FSceneScriptModernAdapters::IsLuaTableTag(int TypeTag)
{
    return TypeTag == -3;
}

extern "C" int SceneScriptModernIsLuaTableTag(int TypeTag)
{
    return FSceneScriptModernAdapters::IsLuaTableTag(TypeTag) ? 1 : 0;
}

int FSceneScriptModernAdapters::NormalizeVarargCount(int RequestedCount)
{
    return RequestedCount < 0 ? 0 : RequestedCount;
}

extern "C" int SceneScriptModernNormalizeVarargCount(int RequestedCount)
{
    return FSceneScriptModernAdapters::NormalizeVarargCount(RequestedCount);
}

const char* FSceneScriptModernAdapters::GetArithmeticDiagnostic()
{
    return "unexpected type in arithmetic operation";
}

extern "C" char* SceneScriptModernGetArithmeticDiagnostic()
{
    return const_cast<char*>(FSceneScriptModernAdapters::GetArithmeticDiagnostic());
}
