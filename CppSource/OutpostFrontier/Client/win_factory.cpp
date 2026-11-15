// Author: Giperion.

#include "win_factory.h"

#include "nn_text_control.h"

#include <string.h>

namespace
{
    // — head of the factory list (next at factory+0x4). Zero-initialised
    // before any dynamic initializer runs, so WINFACTORY<T> globals may register from their
    // own static constructors in any order.
    WinFactory* G_WinFactoryHead = nullptr;
}

WinFactory::WinFactory(char const* Name)
    : Next(nullptr)
    , NameLength(0)
{
    // -shaped ctor: +0x8 = 0, +0xc = 0, +0x4 = 0; Name != 0 → strlen clamped to
    // 0x1f, REP MOVSD/MOVSB copy, NUL; +0x8 = length; the routine(this).
    ClassName[0] = '\0';
    unsigned Length = 0;
    if (Name != nullptr)
    {
        Length = static_cast<unsigned>(strlen(Name));
        if (Length >= 0x20)
            Length = 0x1f;
        memcpy(ClassName, Name, Length);
        ClassName[Length] = '\0';
    }
    NameLength = Length;
    WinFactoryRegister(this);
}

WinFactory::~WinFactory()
{

    WinFactoryUnregister(this);
}

void WinFactoryRegister(WinFactory* Factory)
{

    Factory->Next = G_WinFactoryHead;
    G_WinFactoryHead = Factory;
}

void WinFactoryUnregister(WinFactory* Factory)
{
    // find Factory keeping the previous node; not found → return; unlink from
    // prev->next or the head; Factory->next = 0.
    WinFactory* Prev = nullptr;
    WinFactory* Walk = G_WinFactoryHead;
    while (Walk != nullptr)
    {
        if (Walk == Factory)
            break;
        Prev = Walk;
        Walk = Walk->Next;
    }
    if (Walk == nullptr)
        return;
    if (Prev != nullptr)
        Prev->Next = Walk->Next;
    else
        G_WinFactoryHead = Walk->Next;
    Factory->Next = nullptr;
}

Win3D* WinFactoryCreate(char const* Nickname, char const* ClassName, Win3D* Parent,
                        void* Payload)
{

    if (ClassName == nullptr)
        return nullptr;

    WinFactory* Factory = G_WinFactoryHead;
    while (Factory != nullptr)
    {
        // Inline strcmp 0059db42..0059db72.
        if (strcmp(Factory->ClassName, ClassName) == 0)
            break;
        Factory = Factory->Next;
    }
    if (Factory == nullptr)
        return nullptr;

    // factory vftable[0](Parent, Payload).
    Win3D* Widget = Factory->Create(Parent, Payload);
    if (Widget == nullptr)
        return nullptr;

    // widget +0x14 = length, +0x18 = name (clamped to 0x1f chars; a null
    // Nickname gives the empty name) -- Win3D::SetName is that inline copy.
    Widget->SetName(Nickname);
    return Widget;
}

