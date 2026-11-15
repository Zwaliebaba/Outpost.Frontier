// Author: Giperion.

// Isolated TU: import NEURONCOMPONENTS's stricmp without the CRT <string.h> declaration of the POSIX name.
// baseline Common.dll IAT: NEURONCOMPONENTS.DLL!stricmp (PTR 0639902c), called by the StringCache intern tree
// (find 06333ea0, insert 06333f30, node insert 063341a0) and many other Common lookups.

extern "C" __declspec(dllimport) int __cdecl stricmp(const char* Left, const char* Right);

int CallNeuronComponentsStricmp(const char* Left, const char* Right)
{
    return stricmp(Left, Right);
}
