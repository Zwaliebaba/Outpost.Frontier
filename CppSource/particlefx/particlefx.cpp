// Author: Giperion.

#include "neuroncomponents.h"

#include "particlefx_types.h"
#include "particlefx_common.h"
#include "particlefx_update.h"
#include "particlefx_fxruntime.h"
#include "particlefx_effectarchetype.h"
#include "particlefx_effectobject.h"
#include "particlefx_effectlibrary.h"
#include "particlefx_genericfx.h"
#include "particlefx_ramp.h"
#include "particlefx_factory.h"

// CPU selection precedes ordered factory/INI/pool startup.

BOOL WINAPI DllMain(HINSTANCE Instance, DWORD Reason, LPVOID /*Reserved*/)
{
    if (Reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(Instance);
        ParticleFxImpl::SelectParticleFxMathImplementation();
        ParticleFxImpl::InitializeParticleFxModule();
    }
    else if (Reason == DLL_PROCESS_DETACH)
    {
        ParticleFxImpl::FreeAllAppearancePools();
    }
    return TRUE;
}
