#pragma once

#include "World.h"

#include <cstdint>

/*
 * The world state, as one number (ADR-005 §7).
 *
 * This is the replay harness's instrument: run a scripted order log twice and
 * compare hashes every twenty ticks, and a divergence is caught within a second
 * of simulated time instead of at the end of a thousand ticks. Any change that
 * breaks replay equality is broken by definition, and this is how the suite
 * says so.
 *
 * **It hashes bit patterns, not values.** A float comparison with a tolerance
 * would pass on two runs that had already diverged, which is the one thing this
 * must never do -- same-binary replay determinism means *bit-identical*, and a
 * hash that forgave a last-bit difference would forgive exactly the class of
 * bug it exists to find (an `XM*Est` call swapped in, a reordered accumulation,
 * a parallel-for with non-deterministic partitioning).
 *
 * Fields are folded in a fixed order over dense-array order, which is the only
 * iteration order the world has.
 */

namespace Game
{

/// Hashes every replicated and simulated field of the world.
[[nodiscard]] std::uint64_t ComputeWorldHash(const World& _world) noexcept;

/*
 * Hashes only what a client would see.
 *
 * Weaker than the full hash on purpose, and useful for a different question:
 * the full hash catches any divergence at all, while this one answers "would
 * the two runs have looked the same" -- which is what a desync report is
 * actually about. F10 makes it worth being able to ask separately.
 */
[[nodiscard]] std::uint64_t ComputeReplicatedHash(const World& _world) noexcept;

} // namespace Game
