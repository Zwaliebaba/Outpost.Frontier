// Author: Giperion.

#include "reg_key_stack.h"

#include <string.h>

// (reg_key_stack.h).
char const* G_OutpostFrontierRegKey = "Software\\Microsoft\\Microsoft Games\\OutpostFrontier\\1.0";

FRegKeyStack::FRegKeyStack()
    : Depth(0)
{
    CloseAll();
    Keys[Depth] = HKEY_CURRENT_USER;
    ++Depth;
}

FRegKeyStack::~FRegKeyStack()
{
    CloseAll();
}

// .
HKEY FRegKeyStack::Pop()
{
    if (Depth > 0)
    {
        --Depth;
        return Keys[Depth];
    }
    return nullptr;
}

// .
void FRegKeyStack::CloseAll()
{
    while (Depth > 0)
    {
        --Depth;
        RegCloseKey(Keys[Depth]);
    }
}

// .
void FRegKeyStack::PopClose()
{
    if (Depth > 0)
    {
        --Depth;
        RegCloseKey(Keys[Depth]);
    }
}

// .
HKEY FRegKeyStack::Open(char const* SubKey, REGSAM Access)
{
    if (Depth <= 0 || Keys[Depth - 1] == nullptr || Depth >= 32)
        return nullptr;
    HKEY Key = nullptr;
    if (RegOpenKeyExA(Keys[Depth - 1], SubKey, 0, Access, &Key) != ERROR_SUCCESS)
        return nullptr;
    Keys[Depth] = Key;
    ++Depth;
    return Key;
}

// Inline in the routine (0x0056dac3..0x0056db88) and the routine.
HKEY FRegKeyStack::Create(char const* SubKey, REGSAM Access)
{
    if (Depth <= 0 || Keys[Depth - 1] == nullptr || Depth >= 32)
        return nullptr;
    HKEY Key = nullptr;
    DWORD Disposition = 0;
    char Class[] = "";
    if (RegCreateKeyExA(Keys[Depth - 1], SubKey, 0, Class, 0, Access, nullptr, &Key,
                        &Disposition) != ERROR_SUCCESS)
        return nullptr;
    Keys[Depth] = Key;
    ++Depth;
    return Key;
}

// .
HKEY FRegKeyStack::Top() const
{
    return Depth > 0 ? Keys[Depth - 1] : nullptr;
}

// .
LONG FRegKeyStack::SetString(char const* Name, char const* Value)
{
    return RegSetValueExA(Top(), Name, 0, REG_SZ, reinterpret_cast<BYTE const*>(Value),
                          static_cast<DWORD>(strlen(Value) + 1));
}
