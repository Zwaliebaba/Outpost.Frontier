// Author: Giperion.

// Keep NEURONCOMPONENTS's imported comparator separate from CRT's similarly named declaration.

extern "C" __declspec(dllimport) int __cdecl stricmp(const char* Left, const char* Right);

int CallNeuronComponentsStricmp(const char* Left, const char* Right)
{
    return stricmp(Left, Right);
}
