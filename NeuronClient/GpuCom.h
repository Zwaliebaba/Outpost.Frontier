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
 *   creation:  Thing(__uuidof(IThing), thing.put_void())
 *   querying:  auto other = thing.try_as<IOther>()   // null on failure
 *
 * IID_PPV_ARGS does not work here -- it relies on operator&, which com_ptr
 * deliberately does not have.
 */

namespace Neuron
{

template <typename T>
using GpuPtr = winrt::com_ptr<T>;

} // namespace Neuron
