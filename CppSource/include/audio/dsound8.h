// Author: Giperion.

#pragma once

// DirectX 8 SDK DirectSound. Do NOT add DX8\include via AdditionalIncludeDirectories
// — that SDK ships an old basetsd.h that breaks Windows 10 winnt.h (PVOID64).
// Wired via $(Dx8SdkDir)\include appended to $(IncludePath) in the audio vcxproj.

// Default SDK: $(Dx8SdkDir) = repo-root dx8/
// CoCreate still uses baseline IID last word E5 60, not SDK A7 70.

#ifndef DIRECTSOUND_VERSION
#define DIRECTSOUND_VERSION 0x0800
#endif

#include <windows.h>

#include "dsound.h"
