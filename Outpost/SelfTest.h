#pragma once

#include "AppConfig.h"

/*
 * The `selfTest` configuration flag (Build Order S4).
 *
 * Milestone M0 without a window: the server comes up, a client joins over a
 * real loopback socket, and the heartbeat crosses and comes back. It is the
 * half of M0 a machine can check -- the other half is a person seeing the
 * swapchain present -- and it exists so that "the network still works" is a
 * question with an exit code rather than an opinion.
 *
 * It lives in the exe, not in a library, because it wires an engine to a
 * simulation and that is the composition root's job (ADR-014 §2). It is handed
 * the same Simulation the real run would use, so it tests what ships.
 */

namespace Neuron
{
class Simulation;
}

namespace Outpost
{

/// Runs the M0 checks and returns a process exit code: 0 if every check passed,
/// 3 if any failed. Blocks for at most a few seconds.
[[nodiscard]] int RunSelfTest(const AppConfig& _config, Neuron::Simulation& _simulation);

} // namespace Outpost
