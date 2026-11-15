// Author: Giperion.

#include "gf_scenescript_scene.h"

#include "NeuronRuntime.h"
#include "gf_script.h"
#include "soundmanager.h"

#include <cstdint>
#include <cstring>

// . Base ctor, vptrs, then REP MOVSD 0xf dwords from Desc+0x44 to a local and the
// first three of them to +0x6c. The other twelve are never read.
FSceneScript_Scene::FSceneScript_Scene(Csys const& ScriptFrame, FGfScript* InOwner, FSceneScriptEntitySnap const* Desc)
    : FSceneScriptComponent(ScriptFrame, InOwner, Desc)
{
    unsigned int Blob[15];
    int Index = 0;
    for (Index = 0; Index < 15; ++Index)
        Blob[Index] = Desc->SceneBlob[Index];
    memcpy(Ambient, Blob, sizeof(Ambient));
}

// (RET 8). the routine -> GfFogSetOn: the field = On && is_enabled("fog").
GENRESULT NEURONCOMPONENTS_API FSceneScript_Scene::SetOn(unsigned char On)
{
    GfFogSetOn(On);
    return GR_OK;
}

// (RET 8). The the routine answer (the fog really in effect), not the stored byte.
GENRESULT NEURONCOMPONENTS_API FSceneScript_Scene::GetOn(unsigned char* OutOn)
{
    *OutOn = GfFogIsActive() ? 1 : 0;
    return GR_OK;
}

// (RET 8). The low byte of each int.
GENRESULT NEURONCOMPONENTS_API FSceneScript_Scene::SetColor(int const* Rgb3)
{
    GfFogSetColor(Rgb3);
    return GR_OK;
}

// (RET 8).
GENRESULT NEURONCOMPONENTS_API FSceneScript_Scene::GetColor(int* OutRgb3)
{
    GfFogGetColor(OutRgb3);
    return GR_OK;
}

// (RET 0x10).
GENRESULT NEURONCOMPONENTS_API FSceneScript_Scene::SetRange(float Start, float End, float Density)
{
    return SetRangeValue(&Start, &End, &Density);
}

GENRESULT NEURONCOMPONENTS_API FSceneScript_Scene::SetRangeValue(const float* Start, const float* End, const float* Density)
{
    GfFogSetRangeValue(Start, End, Density);
    return GR_OK;
}

// (RET 0x10).
GENRESULT NEURONCOMPONENTS_API FSceneScript_Scene::GetRange(float* OutStart, float* OutEnd, float* OutDensity)
{
    GfFogGetRange(OutStart, OutEnd, OutDensity);
    return GR_OK;
}

// (RET 0xc).
GENRESULT NEURONCOMPONENTS_API FSceneScript_Scene::SetTableMode(unsigned char Table, unsigned int Mode)
{
    GfFogSetMode(Table, Mode);
    return GR_OK;
}

// (RET 0xc).
GENRESULT NEURONCOMPONENTS_API FSceneScript_Scene::GetTableMode(unsigned char* OutTable, unsigned int* OutMode)
{
    GfFogGetMode(OutTable, OutMode);
    return GR_OK;
}

// (RET 8). Through the facet's own vtable, in this order: slot 3 (byte +0),
// slot 5 (+4), slot 7 (+0x14, +0x18, +0x1c), slot 9 (byte +1, +0x10). 0.
GENRESULT NEURONCOMPONENTS_API FSceneScript_Scene::SetPacked(unsigned char const* Packed32)
{
    IFogControl* const Fog = static_cast<IFogControl*>(this);
    Fog->SetOn(Packed32[0]);
    Fog->SetColor(reinterpret_cast<int const*>(Packed32 + 4));
    float RangeValues[3];
    std::memcpy(RangeValues, Packed32 + 0x14, sizeof(RangeValues));
    Fog->SetRangeValue(&RangeValues[0], &RangeValues[1], &RangeValues[2]);
    Fog->SetTableMode(Packed32[1], *reinterpret_cast<unsigned int const*>(Packed32 + 0x10));
    return GR_OK;
}

// (RET 8). Slot 4 (+0), slot 6 (+4), slot 8 (+0x14, +0x18, +0x1c), slot 10
// (+1, +0x10). 0.
GENRESULT NEURONCOMPONENTS_API FSceneScript_Scene::GetPacked(unsigned char* Out32)
{
    IFogControl* const Fog = static_cast<IFogControl*>(this);
    float* const Range = reinterpret_cast<float*>(Out32 + 0x14);
    Fog->GetOn(Out32);
    Fog->GetColor(reinterpret_cast<int*>(Out32 + 4));
    Fog->GetRange(&Range[0], &Range[1], &Range[2]);
    Fog->GetTableMode(Out32 + 1, reinterpret_cast<unsigned int*>(Out32 + 0x10));
    return GR_OK;
}

// (RET 0x14). With NeuronRuntime::Sound, its slot 11 (+0x2c) with the four dwords as
// given. 0 either way.
GENRESULT NEURONCOMPONENTS_API FSceneScript_Scene::SetReverb(unsigned int Environment, float Volume, float Decay,
                                           float Damping)
{
    if (NeuronRuntime::Sound != nullptr)
        NeuronRuntime::Sound->SetEaxPreview(static_cast<int>(Environment), Volume, Decay, Damping);
    return GR_OK;
}

// (RET 0x14). Slot 12 (+0x30) with the four out-pointers. 0 either way.
GENRESULT NEURONCOMPONENTS_API FSceneScript_Scene::GetReverb(float* OutEnvironment, float* OutVolume,
                                           float* OutDecay, float* OutDamping)
{
    if (NeuronRuntime::Sound != nullptr)
        NeuronRuntime::Sound->GetEaxPreview(reinterpret_cast<unsigned long*>(OutEnvironment),
                                    reinterpret_cast<unsigned long*>(OutVolume),
                                    reinterpret_cast<unsigned long*>(OutDecay),
                                    reinterpret_cast<unsigned long*>(OutDamping));
    return GR_OK;
}

// .
void* FSceneScript_Scene::GetEngineInstance()
{
    return reinterpret_cast<void*>(static_cast<intptr_t>(-1));
}

// (RET 8; ICF-shared with SceneScript_Psys geo slot 3). Three dwords into +0x6c.
GENRESULT NEURONCOMPONENTS_API FSceneScript_Scene::SetAmbient(float const* Rgb3)
{
    Ambient[0] = Rgb3[0];
    Ambient[1] = Rgb3[1];
    Ambient[2] = Rgb3[2];
    return GR_OK;
}

// (RET 8; ICF-shared with SceneScript_Psys geo slot 5). +0x6c out.
GENRESULT NEURONCOMPONENTS_API FSceneScript_Scene::GetAmbient(float* OutRgb3)
{
    OutRgb3[0] = Ambient[0];
    OutRgb3[1] = Ambient[1];
    OutRgb3[2] = Ambient[2];
    return GR_OK;
}
