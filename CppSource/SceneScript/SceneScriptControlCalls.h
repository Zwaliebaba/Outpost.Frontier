// Author: Giperion.

#pragma once

#include "scenescript_controls.h"
#include "scenescript_iface.h"

// publish the captured four-word packet through slot 3.
void SceneScriptSetReverbProperties(IReverbControl* Control,
                              const FSceneScriptReverb4* Properties);

// raw stored-height publication through slot 5.
void SceneScriptSetFloorHeight(ICompoundControl* Control, const float* Height);
