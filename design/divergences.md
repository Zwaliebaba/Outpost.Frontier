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
| D5 | `neuron-core::ini::reader` | BINI calls without a current block or value (`read_value` before `read_header`, `get_num_parameters`, `is_header`, `get_header_ptr` and the value getters) read through a null pointer (`common_ini.cpp`) | They return false, zero or an empty string | Undefined behaviour in the reference | None for callers that read a header first |
| D6 | `neuron-core::ini::reader` | BINI names, string offsets and parameters are read without bounds checks; past the end of the file the reader sees the zero-filled rest of the mapped page, or faults beyond it | Bytes past the end of the file read as zero; a string ends at the end of the file | Undefined behaviour in the reference | None where the reference does not fault; the oracle checks a truncated file |
| D7 | `neuron-core::ini::reader` | A BINI parameter of unknown type formats as the last number any reader in the process formatted (one static buffer) | It formats as the last number this reader formatted | Undefined behaviour in the reference (stale shared buffer) | Differs only when readers interleave and a file holds unknown parameter types |
| D8 | `neuron-core::ini::reader` | `read_header` skips unread values with a signed 16-bit index; a block with more than 32767 values loops for ever | Skipping stops when the index wraps | Undefined behaviour in the reference | None for blocks up to 32767 values |
| D9 | `neuron-core::ini::reader` | `set_state` with a state saved from the other file kind reinterprets the union | `IniReader::set_state` ignores a state of the other kind | Undefined behaviour in the reference | None for callers that restore their own state |

The C++ server now writes character saves and the single-player restart file
as plain-text INI. This deliberately changes the original FLS1 output format.
The loader still reads existing FLS1 saves and templates.
