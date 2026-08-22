// pch.h: precompiled header for OutpostTests.
//
// Deliberately bare, and for a sharper reason than the other suites have.
//
// The scaffold this project grew from included `NeuronClient.h`, which reaches
// D3D12, C++/WinRT and msquic. This suite covers the configuration layers,
// which parse text and produce a struct: no window, no device, no transport.
// Pulling the engine umbrella in would make the build depend on all three to
// test none of them -- and would hide the property that makes the subject
// testable here at all.
//
// `Outpost\AppConfig.cpp` is compiled into this project rather than linked from
// it, so it takes this precompiled header instead of its own. That works
// because it needs nothing from one: `AppConfig.h`, `Json.h`, `Log.h` and the
// standard library. A source added here that needs more than that has stopped
// being wiring, and belongs behind a library split rather than behind a wider
// header.

#ifndef PCH_H
#define PCH_H

#endif // PCH_H
