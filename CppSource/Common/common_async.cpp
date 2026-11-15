// Author: Giperion.

// Async -- the synchronous work-item service (baseline 0x062fd400-0x062fd5e0, plus the
// ICF-shared DumpWorkQueue / WorkQueueSize bodies). Declared in common_async.h.
#include "common_async.h"

#include <cstdlib>
#include <windows.h>

namespace
{
    int g_async_refcount = 0;
}

namespace Async
{
    void __cdecl Startup()
    {
        // Common Async::Startup @ 0x9d5d0
        ++g_async_refcount;
    }

    void __cdecl Shutdown()
    {
        --g_async_refcount;
    }

    Action::Action(bool bInDeleteWhenDone)
    {
        // +0x04 = argument, +0x08 = 0.
        bDeleteWhenDone = bInDeleteWhenDone;
        hEvent = nullptr;
    }

    Action::Action(Action const& Other)
    {
        // copies both fields, the event handle included.
        bDeleteWhenDone = Other.bDeleteWhenDone;
        hEvent = Other.hEvent;
    }

    Action::~Action()
    {

        if (hEvent != nullptr)
            CloseHandle(hEvent);
    }

    void* Action::operator new(size_t Size)
    {
        // JMP msvcrt malloc
        return malloc(Size);
    }

    void Action::operator delete(void* Ptr)
    {
        // JMP msvcrt free
        free(Ptr);
    }

    bool __cdecl AddWork(Action* Work)
    {

        if (!Work->bDeleteWhenDone)
            Work->hEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);

        Work->Run();

        if (Work->bDeleteWhenDone)
        {
            // vtable+4 with flag 1 -- the scalar deleting destructor.
            delete Work;
            return true;
        }

        SetEvent(Work->hEvent);
        return true;
    }

    void __cdecl DumpWorkQueue()
    {
        // RET
    }

    int __cdecl WorkQueueSize()
    {
        // XOR EAX,EAX; RET
        return 0;
    }
}
