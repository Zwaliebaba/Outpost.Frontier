// Author: Giperion.

#include "SceneScriptControlCalls.h"

void SceneScriptSetReverbProperties(IReverbControl* Control,
                              const FSceneScriptReverb4* Properties)
{
    Control->SetReverb(Properties->Environment, Properties->Volume,
                       Properties->Decay, Properties->Damping);
}

void SceneScriptSetFloorHeight(ICompoundControl* Control, const float* Height)
{
    Control->SetFloorHeight(*Height);
}
