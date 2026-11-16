// Oracle harness (design/migration-plan.md P0.4): the exported stricmp of
// neuroncomponents.dll. Isolated from <string.h>, whose stricmp declaration
// would clash, as in Common/common_neuroncomponents_stricmp.cpp.

extern "C" __declspec(dllimport) int __cdecl stricmp(const char* Left, const char* Right);

int OracleNeuronComponentsStricmp(const char* Left, const char* Right)
{
    return stricmp(Left, Right);
}
