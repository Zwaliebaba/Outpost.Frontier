#include "pch.h"

#include "WorldHash.h"

#include "Hash.h"

#include <cstring>

using namespace DirectX;

namespace Game
{
namespace
{

/*
 * Folds a float in by its bits.
 *
 * `std::memcpy` rather than a reinterpret_cast or a union: it is the only
 * spelling that is not a strict-aliasing violation, and every compiler turns it
 * into the move it looks like.
 *
 * Note what this deliberately does *not* do: normalise -0.0f to 0.0f, or NaN
 * payloads to a canonical NaN. Two runs that produced different zeros or
 * different NaNs have diverged, and this hash exists to say so.
 */
[[nodiscard]] std::uint64_t FoldFloat(float _value, std::uint64_t _seed) noexcept
{
  std::uint32_t bits = 0;
  std::memcpy(&bits, &_value, sizeof(bits));
  return Neuron::HashValue(bits, _seed);
}

[[nodiscard]] std::uint64_t FoldVector2(const XMFLOAT2& _value, std::uint64_t _seed) noexcept
{
  return FoldFloat(_value.y, FoldFloat(_value.x, _seed));
}

} // namespace

std::uint64_t ComputeWorldHash(const World& _world) noexcept
{
  std::uint64_t hash = Neuron::FNV_OFFSET_BASIS_64;

  // The tick and the ship count first, so two worlds that differ only in how
  // many ships they hold cannot collide by hashing the same prefix.
  hash = Neuron::HashValue(_world.Tick(), hash);
  hash = Neuron::HashValue(_world.ShipCount(), hash);

  const std::span<const ShipId> ids = _world.Ids();
  const std::span<const std::uint8_t> classes = _world.Classes();
  const std::span<const WingId> wings = _world.Wings();
  const std::span<const XMFLOAT2> positions = _world.Positions();
  const std::span<const XMFLOAT2> velocities = _world.Velocities();
  const std::span<const float> headings = _world.Headings();
  const std::span<const Guidance> guidances = _world.Guidances();
  const std::span<const std::uint8_t> hulls = _world.Hulls();
  const std::span<const std::uint8_t> shields = _world.Shields();
  const std::span<const std::uint32_t> protectedUntil = _world.ProtectedUntil();
  const std::span<const ShipCargo> cargo = _world.Cargo();
  const std::span<const MiningState> mining = _world.MiningStates();

  for (std::size_t slot = 0; slot < ids.size(); ++slot)
  {
    hash = Neuron::HashValue(ids[slot], hash);
    hash = Neuron::HashValue(classes[slot], hash);
    hash = Neuron::HashValue(wings[slot], hash);
    hash = FoldVector2(positions[slot], hash);
    hash = FoldVector2(velocities[slot], hash);
    hash = FoldFloat(headings[slot], hash);

    // Guidance is state, not a derived quantity: two worlds whose ships are in
    // the same place but heading somewhere different have diverged, and the
    // next tick is where it would show.
    const Guidance& guidance = guidances[slot];
    hash = Neuron::HashValue(static_cast<std::uint8_t>(guidance.mode), hash);
    hash = FoldFloat(guidance.targetXMetres, hash);
    hash = FoldFloat(guidance.targetYMetres, hash);
    hash = FoldFloat(guidance.arrivalFacingRadians, hash);

    hash = Neuron::HashValue(hulls[slot], hash);
    hash = Neuron::HashValue(shields[slot], hash);

    // Undock protection (ADR-017 §5). Simulation state, not presentation: it
    // decides what a future combat model may do to this ship, and two worlds
    // that disagree about who is protected have already diverged about the
    // fight that follows.
    hash = Neuron::HashValue(protectedUntil[slot], hash);

    /*
     * What it is carrying and where it is in its cycle (ADR-024 §4b).
     *
     * Both are simulation state in the strongest sense: the manifest is what a
     * refinery will consume, and the cycle clock decides which tick the next
     * credit lands on. Two worlds that agreed about every position and
     * disagreed about a hold have already diverged about an economy.
     *
     * The hazard counters are folded too, and they are the subtle half. A
     * radiation stack lengthens the *next* cycle, so two builds that disagreed
     * about a stack would land the following credit at different ticks -- and
     * the tick after that, at different positions, because a full Miner stops.
     */
    for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
    {
      hash = Neuron::HashValue(cargo[slot].oreUnits[ore], hash);
    }
    hash = Neuron::HashValue(mining[slot].cycleEndTick, hash);
    hash = Neuron::HashValue(mining[slot].cluster, hash);
    hash = Neuron::HashValue(mining[slot].radiationStacks, hash);
    hash = Neuron::HashValue(mining[slot].outsideFieldTicks, hash);
    hash = Neuron::HashValue(mining[slot].heat, hash);
    hash = Neuron::HashValue(mining[slot].coolingTicks, hash);
    hash = Neuron::HashValue(mining[slot].lockoutUntilTick, hash);
  }

  /*
   * The field, which is the other half of that economy (ADR-024 §3d).
   *
   * A world's *copy* of what is left of the site: the durable number lives in
   * the registry's ledger and is folded there, but the copy is what the next
   * cycle reads, so a replay that reproduced the ledger and not the copy would
   * agree about a field and disagree about the next unit out of it.
   *
   * The archetype, grade and radius go in as well even though they are content:
   * they are what the hazard arithmetic multiplies by, and a world spun up from
   * the wrong epoch's field should say so here rather than a day later.
   */
  const SiteField& site = _world.Site();
  hash = Neuron::HashValue(static_cast<std::uint8_t>(site.archetype), hash);
  hash = Neuron::HashValue(site.grade, hash);
  hash = Neuron::HashValue(site.fieldRadiusCm, hash);
  hash = Neuron::HashValue(site.clusterCount, hash);
  for (std::uint8_t index = 0; index < site.clusterCount; ++index)
  {
    hash = Neuron::HashValue(site.clusters[index].xCm, hash);
    hash = Neuron::HashValue(site.clusters[index].yCm, hash);
    for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
    {
      hash = Neuron::HashValue(site.clusters[index].remainingUnits[ore], hash);
    }
  }

  /*
   * The order groups, and the queue that has not been ingested yet.
   *
   * Both are state a replay has to reproduce (ADR-005 §5). Two worlds whose
   * ships are identical but whose plans differ have already diverged -- the
   * next leg is where it shows, and by then the divergence is a hundred ticks
   * from its cause.
   *
   * The pending queue contributes its **count** and not its contents, which is
   * narrower than it sounds: an order submitted before a tick and one submitted
   * after are different worlds and the count says so, and the contents are
   * ingested on the very next tick, where they become groups and are folded in
   * full. The window in which two worlds could agree here and differ in fact is
   * one tick wide, and the tick that closes it is the one that catches them.
   */
  const auto foldGroup = [](const OrderGroup& _group, std::uint64_t _hash) noexcept
  {
    _hash = Neuron::HashValue(_group.serverOrderId, _hash);
    _hash = Neuron::HashValue(_group.clientOrderSeq, _hash);
    _hash = Neuron::HashValue(static_cast<std::uint8_t>(_group.formation), _hash);
    _hash = Neuron::HashValue(static_cast<std::uint8_t>(_group.state), _hash);
    // What a Mine is after and which rocks it is on. Both decide the future --
    // the filter picks the ore and the cluster picks the pool -- so two groups
    // identical but for these would diverge on the very next credit.
    _hash = Neuron::HashValue(static_cast<std::uint8_t>(_group.oreFilter), _hash);
    _hash = Neuron::HashValue(_group.cluster, _hash);
    _hash = Neuron::HashValue(_group.legStartTick, _hash);
    // The leg's deadline is state, not a constant, since S12 made it
    // proportional to the leg. A replay that diverged on it would advance a leg
    // a tick apart and then diverge on everything.
    _hash = Neuron::HashValue(_group.legDeadlineTick, _hash);
    // Both decide the future, so both are folded: `doneTick` decides the tick
    // the group leaves the table, and `solvedMemberCount` decides whether the
    // next ingest re-solves the formation. Two worlds equal in everything else
    // but differing here would diverge exactly there.
    _hash = Neuron::HashValue(_group.doneTick, _hash);
    _hash = Neuron::HashValue(_group.solvedMemberCount, _hash);
    _hash = Neuron::HashValue(_group.memberCount, _hash);
    for (std::uint16_t index = 0; index < _group.memberCount; ++index)
    {
      _hash = Neuron::HashValue(_group.members[index], _hash);
    }
    _hash = Neuron::HashValue(_group.legCount, _hash);
    _hash = Neuron::HashValue(_group.legIndex, _hash);
    for (std::uint8_t index = 0; index < _group.legCount; ++index)
    {
      _hash = FoldVector2(_group.legs[index].anchorMetres, _hash);
      _hash = FoldFloat(_group.legs[index].facingRadians, _hash);
    }
    return _hash;
  };

  hash = Neuron::HashValue(static_cast<std::uint32_t>(_world.Groups().size()), hash);
  for (const OrderGroup& group : _world.Groups())
  {
    hash = foldGroup(group, hash);
  }
  hash = Neuron::HashValue(_world.LastOrderSeqProcessed(), hash);
  hash = Neuron::HashValue(_world.PendingOrderCount(), hash);

  // The RNG last. Nothing in the movement model draws from it, so a divergence
  // here would mean something started consuming randomness that used not to --
  // which is worth a failing test rather than a silent behaviour change.
  hash = Neuron::HashValue(_world.Random().State(), hash);
  hash = Neuron::HashValue(_world.Random().Increment(), hash);
  return hash;
}

std::uint64_t ComputeReplicatedHash(const World& _world) noexcept
{
  std::uint64_t hash = Neuron::FNV_OFFSET_BASIS_64;
  hash = Neuron::HashValue(_world.ShipCount(), hash);

  const std::span<const ShipId> ids = _world.Ids();
  const std::span<const std::uint8_t> classes = _world.Classes();
  const std::span<const XMFLOAT2> positions = _world.Positions();
  const std::span<const XMFLOAT2> velocities = _world.Velocities();
  const std::span<const float> headings = _world.Headings();

  for (std::size_t slot = 0; slot < ids.size(); ++slot)
  {
    hash = Neuron::HashValue(ids[slot], hash);
    hash = Neuron::HashValue(classes[slot], hash);
    hash = FoldVector2(positions[slot], hash);
    hash = FoldVector2(velocities[slot], hash);
    hash = FoldFloat(headings[slot], hash);
  }
  return hash;
}

} // namespace Game
