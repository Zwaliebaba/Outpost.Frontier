#pragma once

#include <wrl/client.h>

/*
 * COM ownership for the graphics objects.
 *
 * AGENTS.md §5 prefers winrt::com_ptr, and that remains the target. This slice
 * uses Microsoft::WRL::ComPtr deliberately: C++/WinRT's headers are generated
 * by its NuGet targets, and bringing an unvalidated toolchain feature into the
 * DX12 bring-up would mean a red build that could be either D3D12 or header
 * generation. One risk at a time. Swapping the alias over is a mechanical
 * change once someone has run a local build with C++/WinRT enabled.
 */

namespace Neuron
{

template <typename T>
using GpuPtr = Microsoft::WRL::ComPtr<T>;

} // namespace Neuron
