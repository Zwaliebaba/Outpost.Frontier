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
