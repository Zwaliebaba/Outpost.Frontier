#include "pch.h"

#include "World.h"

#include "EntityRecord.h"

#include <algorithm>

using namespace DirectX;

/*
 * The mining half of `World` (ADR-024 §4b, build order E2).
 *
 * `World.cpp` keeps the movement half and `WorldOrders.cpp` the order half; this
 * is the third, and the seam is the one the tick already draws. Everything here
 * runs in `Mining`, which is the last named step -- so a cycle is judged against
 * where a ship *finished* the tick rather than where steering was still arguing
 * it should be.
 *
 * **What this file never does is reach for the universe.** A cluster is a local
 * offset in the grid's own centimetres and the pool is a number the registry
 * seeded at spin-up; where the field sits on the plane, and which epoch it
 * belongs to, is decided by the placement file the universe list owns, and is
 * spin-up work rather than the tick's (ADR-009 §2). CI holds that by name: the
 * tick's own four files may not so much as *mention* that file, which is why
 * this paragraph is phrased around it rather than citing it -- the same
 * arrangement `Validate.h` has for universe coordinates.
 *
 * **The ledger is not here either.** The tick eats its own copy of the field and
 * files a `MineYield` record; the registry applies it between ticks. That is the
 * transfer bus doing what it was built for, and it is why mining extends the
 * replay contract by one record family rather than forking it.
 *
 * **One thing this slice deliberately does not do**: ore does not survive a
 * crossing. `TransferMember` carries an id, a hull and a wing, so a Miner that
 * docks or warps arrives with empty holds -- E3 is the slice that gives a
 * crossing a manifest and a station a Bay, and half of that arrangement would be
 * worse than none. Until then the hold is a number the grid owns, and the tests
 * say so rather than leaving it to be discovered.
 */

namespace Game
{
namespace
{

/// Is this ship in the rocks? The field is centred on the grid origin, because
/// the anchor's origin *is* the field's centre (ADR-024 §3a) -- the same fact
/// that puts a station at (0,0) on its own grid.
[[nodiscard]] bool InField(const XMFLOAT2& _positionMetres, std::int32_t _fieldRadiusCm) noexcept
{
  const float radius = Neuron::CentimetresToMetres(_fieldRadiusCm);
  return _positionMetres.x * _positionMetres.x + _positionMetres.y * _positionMetres.y <= radius * radius;
}

/// Rounded up, in whole cycles. Ceiling rather than floor because a partial
/// cycle still has to run, and an ETA that rounded it away would say a fleet
/// finishes before its last cycle lands.
[[nodiscard]] std::uint32_t CyclesFor(std::uint32_t _units, std::uint32_t _perCycle) noexcept
{
  return _perCycle == 0 ? 0 : (_units + _perCycle - 1) / _perCycle;
}

} // namespace

std::uint32_t World::OreHoldFreeLitres(std::uint32_t _slot) const noexcept
{
  HullClass hullClass = HullClass::Interceptor;
  if (m_economy == nullptr || _slot >= m_cargo.size() || !TryShipClass(m_classes[_slot], hullClass))
  {
    return 0;
  }
  const std::uint32_t capacity = m_economy->Cargo(hullClass).oreHoldLitres;

  std::uint32_t used = 0;
  for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
  {
    used += m_cargo[_slot].oreUnits[ore] * m_economy->ores[ore].unitVolumeLitres;
  }
  return capacity > used ? capacity - used : 0;
}

std::uint8_t World::SensorDampingPct() const noexcept
{
  return m_economy == nullptr ? static_cast<std::uint8_t>(100) : Game::SensorDampingPct(*m_economy, m_site);
}

void World::Mining()
{
  if (m_economy == nullptr || !m_site.Exists())
  {
    return;
  }

  const HazardKind hazard = FieldHazard(*m_economy, m_site);
  const SiteHazard& hazardInfo = m_economy->Archetype(m_site.archetype).hazard;

  /*
   * The richest ore in a cluster that this ship both accepts and has room for.
   *
   * Room is part of the question rather than a separate check, because a hold
   * with 250 litres left can still take an Astracite unit and cannot take a
   * Ferro-Chroma one -- and a Miner that stopped at the first ore it could not
   * fit would be grounded with work left in front of it.
   */
  const auto nextOre = [this](std::uint32_t _slot, std::uint8_t _cluster, OreFilter _filter, OreId& _outOre)
  {
    if (_cluster >= m_site.clusterCount)
    {
      return false;
    }
    const std::uint32_t room = OreHoldFreeLitres(_slot);
    bool found = false;
    std::uint32_t bestUnits = 0;
    for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
    {
      const std::uint32_t units = m_site.clusters[_cluster].remainingUnits[ore];
      const std::uint32_t volume = m_economy->ores[ore].unitVolumeLitres;
      if (units == 0 || volume == 0 || volume > room || !OreFilterMatches(_filter, static_cast<OreId>(ore)))
      {
        continue;
      }
      if (!found || units > bestUnits)
      {
        // Ties by `OreId`, which is the order every per-ore array uses -- so two
        // machines working an identical cluster take the same rock.
        _outOre = static_cast<OreId>(ore);
        bestUnits = units;
        found = true;
      }
    }
    return found;
  };

  /// Is there room left for *anything*? The hold-full event's own question, and
  /// deliberately about the hold rather than about the cluster: a Miner that
  /// cannot take the smallest ore there is has finished, wherever it is stood.
  const auto holdIsFull = [this](std::uint32_t _slot)
  {
    const std::uint32_t room = OreHoldFreeLitres(_slot);
    for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
    {
      const std::uint32_t volume = m_economy->ores[ore].unitVolumeLitres;
      if (volume > 0 && volume <= room)
      {
        return false;
      }
    }
    return true;
  };

  for (OrderGroup& group : m_groups)
  {
    // A group still flying to its cluster has legs left; one that has arrived
    // has none, and that is what tells the two apart without a fourth
    // `OrderState` the wire would have to learn.
    if (group.kind != OrderKind::Mine || group.state == OrderState::Done || group.legIndex < group.legCount)
    {
      continue;
    }

    bool anyWorkable = false;
    for (std::uint16_t index = 0; index < group.memberCount; ++index)
    {
      std::uint32_t slot = 0;
      if (!FindSlot(group.members[index], slot))
      {
        continue;
      }

      // Escorts hold their stations around the worked cluster (ruling R4). They
      // are members of the order in every other sense -- the ghost, the ETA,
      // the replace -- and only Miners cycle.
      if (static_cast<HullClass>(m_classes[slot]) != HullClass::Miner)
      {
        continue;
      }

      MiningState& state = m_mining[slot];

      // A cycle that has come due, credited before anything else this ship
      // does: the next cycle's length depends on the stack this one adds.
      if (state.cycleEndTick != 0 && m_tick >= state.cycleEndTick)
      {
        state.cycleEndTick = 0;

        OreId ore = OreId::FerroChroma;
        if (nextOre(slot, state.cluster, group.oreFilter, ore))
        {
          const auto oreIndex = static_cast<std::uint8_t>(ore);
          const std::uint32_t volume = m_economy->ores[oreIndex].unitVolumeLitres;
          const std::uint32_t roomUnits = OreHoldFreeLitres(slot) / volume;

          /*
           * Hardness, not luck: `min(cycleYield, what is left, what fits)`,
           * with no draw taken anywhere (ADR-024 §4b). An RNG draw on the
           * per-tick path is how a replay stops reproducing, and a yield the
           * player cannot predict is a yield they cannot plan a haul around.
           */
          const std::uint32_t units = std::min({m_economy->mining.unitsPerCycle[oreIndex],
                                                m_site.clusters[state.cluster].remainingUnits[oreIndex], roomUnits});
          if (units > 0)
          {
            m_site.clusters[state.cluster].remainingUnits[oreIndex] -= units;
            m_cargo[slot].oreUnits[oreIndex] += units;

            TransferRequest request;
            request.kind = TransferKind::MineYield;
            request.anchor = m_anchor;
            request.cluster = state.cluster;
            request.ore = ore;
            request.units = units;
            request.epoch = m_fieldEpoch;
            request.filledHold = holdIsFull(slot);
            m_filed.push_back(request);
          }
        }

        /*
         * The hazard is paid for running the laser, whether or not the cycle
         * found anything to take. A miner grinding the last handful out of an
         * irradiated belt is standing in the same radiation as one filling its
         * hold, and a field that only hurt profitable cycles would reward
         * exactly the behaviour it exists to discourage.
         */
        if (hazard == HazardKind::Radiation && InField(m_positions[slot], m_site.fieldRadiusCm) &&
            state.radiationStacks < RADIATION_STACK_CAP)
        {
          ++state.radiationStacks;
        }
        if (hazard == HazardKind::Nebula)
        {
          const std::uint32_t heat = state.heat + CycleHeat(*m_economy, m_site);
          state.heat = static_cast<std::uint16_t>(std::min<std::uint32_t>(heat, 0xffffu));
          if (hazardInfo.heatThreshold > 0 && state.heat >= hazardInfo.heatThreshold)
          {
            // The laser locks out and the ship sheds heat for the whole of it,
            // which is the only laser-off time an unpaced miner ever gets.
            state.lockoutUntilTick = m_tick + hazardInfo.lockoutSeconds * TICKS_PER_SECOND;
          }
        }
      }

      // And another, if there is one to start. A locked-out Miner is still
      // workable -- it has room and the cluster has ore, and the lockout is
      // what it is waiting out.
      OreId ore = OreId::FerroChroma;
      const bool canWork = nextOre(slot, group.cluster, group.oreFilter, ore);
      if (state.cycleEndTick == 0 && canWork && m_tick >= state.lockoutUntilTick)
      {
        state.cluster = group.cluster;
        state.cycleEndTick = m_tick + MiningCycleTicks(*m_economy, m_site, state.radiationStacks);
      }
      anyWorkable = anyWorkable || canWork || state.cycleEndTick != 0;
    }

    /*
     * Nothing in the order can take another cycle from this cluster, so the
     * order is over (ADR-024 §4b's second and third exits, which differ only in
     * the sentence the client writes about them).
     *
     * `Done` with the linger, not an erase: the client's ghost retires on
     * *seeing* `Done` in an order record, and a group deleted the tick it
     * finished might never be reported as finished at all under loss. The
     * ships stay exactly where they were put -- hauling home is the player's
     * next decision, not an automation (ruling R5).
     */
    if (!anyWorkable)
    {
      group.state = OrderState::Done;
      group.doneTick = m_tick;
    }
  }

  if (hazard == HazardKind::None)
  {
    return; // A ferrous field has nothing to shed.
  }

  /*
   * Upkeep, over **every ship on the grid** and not only over the ones in a
   * Mine order.
   *
   * Radiation sheds while you are out of the rocks and heat sheds while the
   * laser is off, and neither is a property of having an order: a Miner that
   * was told to do something else is still cooling down. Running it after the
   * cycles above is what makes a Miner chaining cycles never count as idle --
   * the next cycle has already started by the time this asks.
   */
  const std::uint32_t decayTicks = hazardInfo.stackDecaySeconds * TICKS_PER_SECOND;
  const std::uint32_t count = ShipCount();
  for (std::uint32_t slot = 0; slot < count; ++slot)
  {
    MiningState& state = m_mining[slot];

    if (hazard == HazardKind::Radiation && state.radiationStacks > 0 && decayTicks > 0 &&
        !InField(m_positions[slot], m_site.fieldRadiusCm))
    {
      /*
       * The counter accumulates and is never reset by going back in, which is
       * the honest reading of "a stack per thirty seconds outside the field":
       * what is being measured is time spent outside, and a ship that leaves
       * twice for fifteen seconds has spent the same thirty as one that left
       * once for thirty.
       */
      ++state.outsideFieldTicks;
      if (state.outsideFieldTicks >= decayTicks)
      {
        state.outsideFieldTicks = 0;
        --state.radiationStacks;
      }
    }

    if (hazard == HazardKind::Nebula && state.heat > 0 && state.cycleEndTick <= m_tick)
    {
      ++state.coolingTicks;
      if (state.coolingTicks >= TICKS_PER_SECOND)
      {
        state.coolingTicks = 0;
        state.heat = state.heat > hazardInfo.heatDecayPerSecond
                       ? static_cast<std::uint16_t>(state.heat - hazardInfo.heatDecayPerSecond)
                       : static_cast<std::uint16_t>(0);
      }
    }
  }
}

float World::MiningEtaSeconds(const OrderGroup& _group) const noexcept
{
  if (m_economy == nullptr || !m_site.Exists() || _group.cluster >= m_site.clusterCount)
  {
    return -1.0f;
  }

  /*
   * The earlier of two endings: the cluster running dry, or the **last** Miner
   * filling. `max` over the Miners and not `min`, because one full Miner does
   * not end the order -- §4b's third exit is all of them.
   *
   * Derived, never stored, for `LegEtaSeconds`'s reason: a seconds field on the
   * group would be a number in the world hash that nothing simulates, and two
   * builds whose economy tables differed by a litre would diverge on it.
   */
  const std::uint32_t cycleSeconds = m_economy->mining.cycleSeconds;
  const SiteCluster& cluster = m_site.clusters[_group.cluster];

  std::uint32_t filtered = 0;
  std::uint32_t perCycle = 0;
  for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
  {
    if (!OreFilterMatches(_group.oreFilter, static_cast<OreId>(ore)) || cluster.remainingUnits[ore] == 0)
    {
      continue;
    }
    filtered += cluster.remainingUnits[ore];
    perCycle = std::max(perCycle, m_economy->mining.unitsPerCycle[ore]);
  }
  if (filtered == 0 || perCycle == 0)
  {
    return 0.0f; // Dry: the order ends on the next tick that looks.
  }

  std::uint32_t miners = 0;
  float secondsToFill = 0.0f;
  for (std::uint16_t index = 0; index < _group.memberCount; ++index)
  {
    std::uint32_t slot = 0;
    if (!FindSlot(_group.members[index], slot) || static_cast<HullClass>(m_classes[slot]) != HullClass::Miner)
    {
      continue;
    }
    ++miners;

    // The room measured in the ore this cluster would actually give it, which
    // is what decides how many cycles it has left rather than an average of
    // three ores two of which are not here.
    std::uint32_t roomUnits = 0;
    for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
    {
      const std::uint32_t volume = m_economy->ores[ore].unitVolumeLitres;
      if (volume > 0 && cluster.remainingUnits[ore] > 0 && OreFilterMatches(_group.oreFilter, static_cast<OreId>(ore)))
      {
        roomUnits = std::max(roomUnits, OreHoldFreeLitres(slot) / volume);
      }
    }

    const std::uint32_t cycles = CyclesFor(roomUnits, perCycle);
    const std::uint32_t ticks = MiningCycleTicks(*m_economy, m_site, m_mining[slot].radiationStacks);
    const float seconds = static_cast<float>(cycles) * static_cast<float>(ticks) * TICK_SECONDS;
    secondsToFill = std::max(secondsToFill, seconds);
  }
  if (miners == 0)
  {
    return 0.0f;
  }

  const float secondsToDrain =
    static_cast<float>(CyclesFor(filtered, perCycle * miners)) * static_cast<float>(cycleSeconds);
  return std::min(secondsToFill, secondsToDrain);
}

} // namespace Game
