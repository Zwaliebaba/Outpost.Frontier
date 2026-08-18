#pragma once

// unknwn.h must precede winrt/base.h: it is what makes com_ptr support classic
// COM interfaces such as ID3D12Device, rather than only WinRT ones.
#include <unknwn.h>

#include <winrt/base.h>

/*
 * COM ownership for the graphics objects (AGENTS.md §5).
 *
 * winrt::com_ptr rather than Microsoft::WRL::ComPtr. Both are header-only RAII
 * wrappers; this one is the sanctioned spelling, and its interface is harder to
 * misuse -- put() asserts the pointer is empty rather than silently releasing
 * what was there, and there is no implicit operator& to hand a live pointer to
 * an out-parameter by accident.
 *
 * The two idioms this codebase uses:
 *   creation:  Thing(IID_PPV_ARGS(thing.put()))
 *   querying:  auto other = thing.try_as<IOther>()   // null on failure
 *
 * IID_PPV_ARGS works, but only around put(): the macro needs a T**, and
 * com_ptr has no operator&, so the WRL spelling IID_PPV_ARGS(&thing) does not
 * compile. Prefer it over passing __uuidof and put_void() separately, because
 * it derives the IID from the pointer's own type -- spelling both by hand lets
 * them disagree, and an IID that does not match the pointer it fills is a
 * runtime bug the compiler will not catch.
 */

namespace Neuron
{

template <typename T>
using GpuPtr = winrt::com_ptr<T>;

} // namespace Neuron
