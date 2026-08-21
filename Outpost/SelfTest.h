#pragma once

#include "AppConfig.h"

// `Game::EconomyDef`: the balance a scenario has to share with the shard.
#include "EconomyDef.h"

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
/*
 * Runs the gate, and returns the exit code.
 *
 * Takes the **economy** as well as the simulation, and the asymmetry is the
 * point: the simulation is reached through the engine's abstract seam (ADR-014
 * §2), which by construction knows nothing about ore -- so a scenario that
 * needs the shard's balance has to be handed it rather than asking a
 * `Neuron::Simulation` a question the engine must not be able to answer.
 *
 * The composition root has both in scope; it is the only place that does.
 */
[[nodiscard]] int RunSelfTest(const AppConfig& _config, Neuron::Simulation& _simulation, const Game::EconomyDef& _economy);

} // namespace Outpost
