// Oracle.exe sim, the fault recorder (design/reference-build.md section 10, "Faults";
// record layout in design/sim-records.md, session.jsonl `fault`). A structured exception
// raised during a frame is written to session.jsonl with its code, the faulting address as
// module + offset, the registers and a stack resolved with dbghelp from the build's PDB
// files, and the run ends with exit code 4 instead of an unhandled-exception crash.
//
// Everything here is inert unless Oracle.exe sim calls it.

#pragma once

#include <string>

struct _EXCEPTION_POINTERS;

namespace oracle
{
    // Writes one finished session.jsonl line (and flushes it).
    typedef void (*FSimFaultWriteFn)(std::string const& Line);

    // Starts the symbol handler over the loaded modules (the PDBs beside Oracle.exe) and a
    // vectored handler that records first-chance hardware exceptions (access violations and
    // the like; at most a few per run) before any handler in the game code sees them.
    // Tick points at the run's tick counter.
    void OracleSimFaultStart(FSimFaultWriteFn Write, unsigned int const* Tick);

    // The __except filter around a frame: writes the `fault` record of the exception and
    // returns EXCEPTION_EXECUTE_HANDLER.
    int OracleSimFaultFilter(_EXCEPTION_POINTERS* Info);

    // Calls Fn(Arg) inside __try; returns false when the filter above caught an exception.
    bool OracleSimGuardedCall(void (*Fn)(void*), void* Arg);
}
