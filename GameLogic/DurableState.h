#pragma once

#include "Ids.h"
#include "Refining.h"
#include "SiteField.h"
#include "Station.h"
#include "Transfer.h"
#include "World.h"

#include "ByteReader.h"
#include "ByteWriter.h"
#include "Wire.h"

#include <cstdint>
#include <string>
#include <vector>

/*
 * The durable state, as bytes (ADR-025, build order E4a).
 *
 * The line this file draws is ADR-025 §1's, and it is the whole design in one
 * sentence: **a ship's identity and where it is are durable; its intentions and
 * its motion are not.** So a fleet parked at a mining field comes back parked
 * at that mining field, at rest, with an empty order queue -- because the fleet
 * is the player's property and the order was the player's intention, and only
 * one of those is the shard's to remember.
 *
 * Pure, like everything else in this library: bytes in, bytes out, diagnostics
 * on malformed input, never a path and never a throw. That is the
 * `ParseUniverse`/`ParseEconomy` posture applied to a binary format, and it is
 * what lets the round-trip suite build a registry in code and need no fixture
 * at all. The file, the framing, the checksums and the rotation are
 * `Neuron::DurableStore`'s, one library out, and it never learns what it is
 * storing (ADR-025 §2).
 *
 * **What this is not is a replay log.** The replay log holds *inputs* and
 * proves the simulation is deterministic; this holds *outcomes* and proves the
 * service did not forget. Both may exist and neither substitutes for the other,
 * which is why the reload proof is `DurableHash` and not `WorldRegistry::Hash`
 * (ADR-025 §1a).
 */

namespace Game
{

class WorldRegistry;

/*
 * The format version, in the file header.
 *
 * A build that does not know a version **refuses to start** rather than
 * guessing (ADR-025 §6.1): a shard that reads an older format optimistically
 * writes a newer one over it, and the state it destroyed was the thing it
 * existed to keep.
 *
 * **2 with E4b**, which added refine jobs, station tiers and upgrade projects
 * to §1's durable list and alloys to the Bay. No migration, deliberately: this
 * is the version's whole job, and ADR-025 §9 names the first re-bake of a
 * universe a live shard has state against as the trigger for designing one --
 * which is a different and larger question from a format that grew a field.
 *
 * **3 with U3c-a**, which gave ships owners that were actually asked for. The
 * field was already here and already written -- capture filled it with
 * `SOLE_PLAYER_ID` unconditionally -- so a version 2 file is not merely missing
 * data, it asserts something false: that every ship in the shard belongs to
 * player one. Reading one under the new rules would hand the first commander to
 * connect the entire universe, which is exactly the failure a version number
 * exists to make impossible. Docked rows and in-flight crossings grew the field
 * for real at the same time, because a ship must not change hands by docking.
 */
inline constexpr std::uint32_t DURABLE_FORMAT_VERSION = 3;

/// Recognises the blob and refuses somebody else's. "OPFD" -- Outpost Frontier
/// durable -- little-endian like everything else this tree writes.
inline constexpr std::uint32_t DURABLE_MAGIC = 0x4446504fu;

/*
 * Where the bytes stopped making sense.
 *
 * A byte offset rather than a line and a column, which is the honest
 * translation of `EconomyDiagnostic` into a format no person edits: what a
 * reader can act on here is *where in the file* and *what was being read*, and
 * a line number would be an invention.
 */
struct PersistenceDiagnostic
{
  std::uint64_t byteOffset = 0;
  std::string what;    ///< The record being read: "ship[12]", "header".
  std::string message; ///< What was wrong with it.

  /// "shard-0.snapshot(+248) ship[12]: anchor 0 is not a place"
  [[nodiscard]] std::string Text(std::string_view _file = {}) const;
};

/*
 * One ship, wherever it is standing (ADR-025 §1).
 *
 * Authored occupants are **not** here: their ids are baked into their anchor
 * (ADR-018 D6a) and spin-up rebuilds them exactly, so writing them down would
 * be storing content in a save file and then arguing with it after a re-bake.
 *
 * `owner` is written and read and has nowhere to go back to, which is
 * deliberate rather than an oversight: ADR-025 §1 names `PlayerId` part of a
 * ship's durable identity, and the tree has no per-ship owner until U3c mints a
 * second commander. Reserved-with-a-value is the house pattern for exactly this
 * (`CombatEngaged`, `resumeToken`, `fuelPerJob`), and it costs four bytes to
 * make U3c a change to what fills the field rather than to the format.
 */
struct DurableShip
{
  ShipId shipId = INVALID_SHIP_ID;
  AnchorId anchor = INVALID_ID;
  HullClass hullClass = HullClass::Interceptor;
  WingId wing = INVALID_WING_ID;
  Neuron::PlayerId owner = Neuron::INVALID_PLAYER_ID;

  float xMetres = 0.0f;
  float yMetres = 0.0f;
  float headingRadians = 0.0f;

  std::uint32_t oreUnits[ORE_COUNT] = {};
};

/// One station's docked set (ADR-017 §1), keyed by the station.
struct DurableRoster
{
  AnchorId anchor = INVALID_ID;
  std::vector<RosterEntry> docked;
};

/*
 * Everything ADR-025 §1 calls durable, lifted out of the runtime.
 *
 * A flat aggregate rather than a second registry: it is what the format writes,
 * what the hash folds, and what a load puts back -- three jobs that want the
 * same shape and would drift if each grew its own.
 */
struct DurableState
{
  std::uint32_t shardTick = 0;

  /// The ship-id high-water mark (ADR-025 §1a). It only ever moves forward, and
  /// a load that would lower it is a refusal rather than a clamp.
  std::uint32_t nextDynamicShipId = 0;

  std::vector<DurableShip> ships;
  std::vector<DurableRoster> rosters;
  std::vector<StationBay> bays;
  std::vector<SiteLedger> ledgers;
  std::vector<TransferRecord> transfers;

  /*
   * And the refinery (ADR-024 §6, E4b), which is the reason G1 is a claim about
   * persistence at all: a refine job is the first thing in this game that a
   * player is invited to walk away from, so a shard that forgot one would have
   * eaten their ore and produced nothing.
   *
   * Tiers and projects are durable for a stronger reason than jobs are: they
   * are **permanent and communal**. A completed project that did not survive a
   * restart would un-build something a dozen commanders paid for.
   */
  std::vector<RefineJob> refineJobs;
  std::vector<StationTier> stationTiers;
  std::vector<UpgradeProject> projects;
};

/*
 * The registry's durable set, and the set put back into a registry.
 *
 * `CaptureDurableState` reads only what the registry already publishes.
 * `RestoreDurableState` goes through `WorldRegistry::LoadDurable`, because
 * putting ships back on grids is the registry's own job -- a free function
 * doing it would be a second place that knows how a world is spun up.
 */
void CaptureDurableState(const WorldRegistry& _registry, DurableState& _outState);

/*
 * The reload proof (ADR-025 §1a).
 *
 * **Not `WorldRegistry::Hash()`**, and the distinction is load-bearing: that
 * hash is the replay domain and folds the order queues §1 has just declared
 * transient, so a correct reload would legitimately fail to reproduce it and a
 * check written against it would teach everyone to ignore a red test. This
 * folds exactly §1's list, in anchor-id then id order.
 *
 * Not `noexcept`: it captures the durable set to fold it, and a `noexcept`
 * function that allocates is `std::terminate` on `bad_alloc` -- the finding
 * clang-tidy made against `ComputeUniverseHash`, not repeated here.
 */
[[nodiscard]] std::uint64_t DurableHash(const WorldRegistry& _registry);

/// The same fold over a captured set, for a caller that already has one.
[[nodiscard]] std::uint64_t DurableHash(const DurableState& _state) noexcept;

/*
 * The registry's durable state, as bytes.
 *
 * False when it did not fit, which is the `ByteWriter` contract: callers
 * serialise the whole thing and ask once. Nothing is written on failure that a
 * caller should trust -- a truncated snapshot is what §5's rotation exists to
 * make survivable.
 */
[[nodiscard]] bool WriteDurableState(const WorldRegistry& _registry, Neuron::ByteWriter& _writer);

/// The same, from a captured set.
[[nodiscard]] bool WriteDurableState(const DurableState& _state, Neuron::ByteWriter& _writer);

/*
 * Bytes back into a registry.
 *
 * False when the bytes are unusable, with everything wrong in
 * `_outDiagnostics` and **the registry left as it was found**: a half-built
 * registry is worse than a refusal, because a refusal is visible at boot and a
 * half-built shard is visible when a player notices their fleet is missing.
 *
 * The registry must be freshly `Reset` -- no live worlds, no rosters, no
 * ledgers, no Bays. Loading over a running shard is not a thing this supports,
 * and refusing is cheaper than defining what merging would mean.
 */
[[nodiscard]] bool ReadDurableState(Neuron::ByteReader& _reader, WorldRegistry& _registry,
                                    std::vector<PersistenceDiagnostic>& _outDiagnostics);

/// Bytes into a captured set, without a registry to put it in. The half the
/// round-trip suite and the store's own tests use.
[[nodiscard]] bool ReadDurableState(Neuron::ByteReader& _reader, DurableState& _outState,
                                    std::vector<PersistenceDiagnostic>& _outDiagnostics);

} // namespace Game
