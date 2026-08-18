#pragma once

#include "GpuPipelines.h"

/*
 * The compiled shaders this executable was built with.
 *
 * Owner directive: shaders are built, not loaded. `Outpost/Shaders/*.hlsl` are
 * compiled by `fxc` as part of this project into byte arrays under
 * `Outpost/CompiledShaders`, and this is the one place those arrays are named.
 *
 * **Why it is the game's and not the engine's.** `Neuron::GpuPipelines` builds
 * the pipeline states and has no opinion about which shaders go in them -- the
 * engine ships around a second game (ADR-014), and a shader is exactly the kind
 * of thing the two games differ on. So the composition root supplies the bytes,
 * the same way it supplies the world through `WorldView` and the mesh list
 * through `ClientConfig`. NeuronClient could not include a header out of
 * `Outpost/` even if it wanted to: that is the executable, and the dependency
 * points the other way.
 *
 * **Why a function and not a constant.** The generated headers define byte
 * arrays at namespace scope, which have internal linkage -- a header that
 * included them would give every translation unit its own copy of every shader.
 * They are included in `ShaderTable.cpp` and nowhere else.
 */

namespace Outpost
{

/// Spans over arrays with static storage duration, so the result outlives any
/// caller and copying it copies nothing but pointers.
[[nodiscard]] Neuron::PipelineShaders ShaderTable() noexcept;

} // namespace Outpost
