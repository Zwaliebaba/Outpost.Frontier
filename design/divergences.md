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
| D11 | `neuron-core::math`, `net::wire::quantise`, `util::spline` | The x87 precision control rounds float arithmetic: 24 bits on the client's main thread after Direct3D 8 `CreateDevice`, 53 bits on the dedicated server and in the oracle (`Directory.Build.props`) | Always the 53-bit result: `f64` arithmetic, rounded to `f32` at each store the reference makes to a `float` | Numeric policy (T11) | Client-side math, quantised wire values and spline results can differ from the single-player C++ client in the last bits; a product within about 2^-24 of an integer can quantise one step lower |
| D12 | `neuron-core::diag` | `DefaultDump` (`neuroncomponents.cpp`) shows a modal Abort/Retry/Ignore box for severities 0 to 2; Abort ends the process, Retry breaks into the debugger | Reports are `tracing` events; no dialog | Legacy platform dependence; owner decision O9 | No modal error boxes; reports go to the installed subscriber |
| D13 | `neuron-core::diag::callbacks` | `INI_Reader::open` and `NeuronRuntime::Startup` call `FatalErrorFn` and `CriticalWarningFn` without a null check | The `ErrorReporter` returns and the caller returns an error; the default reporter logs and continues | Undefined behaviour in the reference | No crash without a host callback; fatal errors propagate as errors |
| D14 | `neuron-core::time` | QPC and `GetTickCount` are independent clocks (tick count resolution 10 to 16 ms); the frequency is the hardware's | `InstantClock` derives both from one `Instant` at 1 GHz, so the 50 ms fallback in `Delta::calc_dt` does not fire | Legacy platform dependence | Frame times come from the monotonic clock |
| D15 | `neuron-core::util::spline` | `get_t_from_point` (`common_crspline.cpp`) loops forever once the distance is NaN or the parameter stops changing | Returns `None` | Undefined behaviour in the reference | The search ends instead of hanging |
| D16 | `neuron-core::util::state_machine` | `remove_state_machine` (`common_statemachine.cpp`) on a machine already marked queues it twice; `remove_dying_machines` then deletes it twice | A second mark has no effect | Undefined behaviour in the reference | None |
| D17 | `neuron-core::util::rect`, `neuron-core::time` | Signed `int` and `__int64` overflow in rect arithmetic and in `StopTicks - StartTicks` | Wrapping arithmetic | Undefined behaviour in the reference | None for values in range |
| D18 | `neuron-core::util::state_machine` | The state handler (slot 2) receives the state by reference to `CurrentState` | The handler receives a copy, and the machine through `&mut` | Technical decision T10 (no aliasing references) | Only a handler that calls `reset()` and then reads its state argument sees a difference |
| D19 | `neuron-core::catalog` | `GetIdsStringWWithRawFallback` with a buffer of fewer than one character skips the string copy but then copies the card into the buffer | `string_with_card_fallback_into` with an empty buffer returns 0 and writes nothing | Undefined behaviour in the reference | None; every caller passes a buffer |
| D20 | `neuron-core::catalog` | `GetIdsStringWWithRawFallback` on a card shorter than 2 bytes computes a count of 0xFFFFFFFF and reads past the resource | Copies nothing, terminates the buffer and returns 0 | Undefined behaviour in the reference | None; every card in the data has at least 50 bytes |
| D21 | `neuron-core::net::wire` | Counts are truncated to their prefix width (`(unsigned char)wcslen` in `CLIENT_LOGIN`, `size() & 0xffff`) while all elements are written | Counted writes return `WireError::LengthOutOfRange` | Undefined behaviour in the reference (the stream desynchronises) | None for strings up to 255 or 65,535 units |
| D22 | `neuron-core::net::wire` | `RpcEncodeWideString` and `RpcDecodeWideString` compare the cursor with `>=` and set no sticky error; `PutHeader` has no capacity check | One `>` check for every field, the header included | Undefined behaviour in the reference (header); otherwise a capacity edge case made uniform | Only a string ending exactly at the buffer limit or at the end of the input |
| D23 | `neuron-core::net::wire` | `RpcDecodeWideString` with count 0 leaves the read pointer before the count word while the cursor moves on, so later fields are read from the wrong place | The count is consumed | Undefined behaviour in the reference | Fields after an empty wide string decode correctly; P2.2 confirms this per message |
| D24 | `neuron-core::net::wire` | `RpcServerPutWideText` and `RpcServerGetWideText` copy past the buffer when the text overflows, then continue | The field fails with `Overflow` or `Underrun` | Undefined behaviour in the reference | Over-long character texts fail instead of corrupting memory |
| D25 | `neuron-core::net::wire` | `RpcWireDecodeOwnedBlock` checks `Cursor + Size > Limit` in 32 bits, which wraps for sizes near 2^32 | `get_bytes_u32` returns `Underrun` | Undefined behaviour in the reference | Hostile block sizes are rejected |
| D26 | `neuron-core::net::{frame, transport}` | DirectPlay keeps message boundaries; there is no length field; server batches are inflated by "try inflate, else raw"; 1-byte pings; a 0-byte batch counts as a ping reply | Length-prefixed frames, an explicit compressed-batch kind that must inflate to its stated size, a versioned handshake, no ping | Legacy platform dependence; decision T17 | Not wire-compatible with the original (out of scope) |

D5 to D9 described the reader's binary INI path. That path was removed
with decision O13, so the rows are gone; the IDs are not reused.

The reference's save writer was changed on purpose (decision O12): it
writes plain text instead of `FLS1`. The port matches the changed
reference, so that change is not a divergence here.
