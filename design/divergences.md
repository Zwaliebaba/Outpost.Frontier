# Divergence register

Every deliberate difference between the Rust port and the C++ reference.
The owner reviews this register at each gate and signs it off in work
package P7.1. See [verification.md](verification.md) for when an entry is
required.

Reasons are one of: *undefined behaviour in the reference*, *legacy
platform dependence*, *numeric policy* (decision T11), or *owner decision*
(cite its ID).

| ID | Area | C++ behaviour | Rust behaviour | Reason | Observable effect |
| --- | --- | --- | --- | --- | --- |
| D1 | `neuron-core::ini::engine` | On a stray `]` or `=` at the start of a line, an over-long phrase, or a value that does not end the line, `FIniDatabase::Parse` executes `__debugbreak`; on a key before the first section it writes through a null section (`neuroncomponents.cpp`). Without a debugger the process ends | `EngineIni::load_*` returns `EngineIniError::Breakpoint` or `KeyBeforeSection`; the sections parsed before stay | Undefined behaviour in the reference | A malformed engine INI is reported instead of ending the program |
| D2 | `neuron-core::ini::engine` | The lexer copies a word into a 1024-byte buffer without a bound; a longer word overwrites the length that follows it | A word may be any length; the phrase limits then apply | Undefined behaviour in the reference | None for words up to 1024 bytes |
| D3 | `neuron-core::ini::engine` | `@include` nests without a limit; an include cycle overflows the stack | Nesting deeper than 64 returns `EngineIniError::IncludeTooDeep` | Undefined behaviour in the reference | An include cycle is reported instead of crashing |
| D4 | `neuron-core::ini::engine` | `_splitpath` on an `@include` name whose directory part is 256 bytes or more calls the invalid-parameter handler, which ends the process | The path is composed as for shorter names | Undefined behaviour in the reference | None for directory parts under 256 bytes |
| D10 | `neuron-core::ini::reader` | `INI_Reader::open` decodes a file that starts with a binary INI header (`common_ini.cpp`) | Every file is read as text | Owner decision O13 | None for the game data, which has only text INI files |

D5 to D9 described the reader's binary INI path. That path was removed
with decision O13, so the rows are gone; the IDs are not reused.

The reference's save writer was changed on purpose (decision O12): it
writes plain text instead of `FLS1`. The port matches the changed
reference, so that change is not a divergence here.
