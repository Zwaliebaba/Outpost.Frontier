// Author: Giperion.

#pragma once

#include "common_exports.h"

#include <cstddef>
#include <windows.h> // HANDLE

// Async -- defined in common_async.cpp.
namespace Async
{
    COMMON_API void __cdecl Startup();
    COMMON_API void __cdecl Shutdown();

    // Async::Action -- Common.dll's "async" work item is actually executed synchronously
    // on the calling thread (no worker pool exists in baseline either).
    // ??_7Action@Async@@6B@ (module storage) has two slots: [0] MSVCRT purecall
    // (module storage) -- the work body, which AddWork runs once (`CALL [EAX]` at 062fd5a1);
    // baseline exports no name for it -- and [1] the scalar deleting destructor
    // (module storage, raw bytes in the listing). Fields: the byte at +0x04 (ctor argument,
    // default false per the default constructor closure 0x062fd440) makes AddWork delete the
    // action when it is done; +0x08 is the Win32 event AddWork signals otherwise, closed by
    // the destructor (0x062fd540).
    struct COMMON_API Action
    {
    public:
        Action(bool bInDeleteWhenDone = false); // ??0Action@Async@@QAE@_N@Z
        Action(Action const& Other);             // ??0Action@Async@@QAE@ABU01@@Z

        // baseline exports no copy assignment (VC6 never generated one); deleting it keeps the
        // compiler from generating and exporting one, and from declaring move members.
        Action& operator=(Action const&) = delete;

        virtual void Run() = 0;    // +0x00  formerly Slot00: the work body AddWork runs
        virtual ~Action();         // +0x04  0x062fd540 ??1Action@Async@@UAE@XZ

        // ??2/??3Action@Async: a bare jump to msvcrt malloc / free (0x062fd560 / 0x062fd570,
        // ICF-shared with GoodDesc, IAction, StateMachineMessage, FmtStr::Val).
        static void* operator new(size_t Size);
        static void operator delete(void* Ptr);

        bool bDeleteWhenDone; // +0x04
        HANDLE hEvent;        // +0x08
    };
    static_assert(sizeof(Action) == 0x0C, "Async::Action sizeof 0x0C");

    // ?AddWork@Async@@YA_NPAUAction@1@@Z. Unless the action deletes itself when
    // done, creates a fresh auto-reset, unsignaled event into Work->hEvent (unconditionally --
    // baseline does not look at the old handle). Runs the work body synchronously, then either
    // deletes the action (vtable+4, the scalar deleting destructor, flag 1) or signals the
    // event. Always returns true.
    COMMON_API bool __cdecl AddWork(Action* Work);

    // ?DumpWorkQueue@Async@@YAXXZ: a bare RET (ICF-shared with a dozen empty bodies).
    COMMON_API void __cdecl DumpWorkQueue();
    // ?WorkQueueSize@Async@@YAHXZ: `XOR EAX,EAX; RET` -- there is no queue.
    COMMON_API int __cdecl WorkQueueSize();
}
