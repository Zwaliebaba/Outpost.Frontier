// Author: Giperion.

// Isolated TU: import NEURONCOMPONENTS's stricmp without CRT <cstring> macros remapping the symbol.
// baseline x86math.dll IAT: NEURONCOMPONENTS.DLL!stricmp (export alias of NeuronComponentsStricmp).

extern "C" __declspec(dllimport) int __cdecl stricmp(const char* Left, const char* Right);

int CallNeuronComponentsStricmp(const char* Left, const char* Right)
{
    return stricmp(Left, Right);
}
