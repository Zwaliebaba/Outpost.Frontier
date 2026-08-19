#include "pch.h"

#include "VoicePool.h"

namespace Neuron
{

void VoicePool::Reset(std::uint32_t _capacity)
{
  m_slots.assign(_capacity, VoiceSlot{});
  m_activeCount = 0;
  m_nextSequence = 1;
}

std::uint32_t VoicePool::WorstActive() const noexcept
{
  std::uint32_t worst = INVALID_VOICE_SLOT;
  for (std::uint32_t index = 0; index < m_slots.size(); ++index)
  {
    const VoiceSlot& slot = m_slots[index];
    if (!slot.active)
    {
      continue;
    }
    if (worst == INVALID_VOICE_SLOT)
    {
      worst = index;
      continue;
    }

    // Priority first, distance second, age last. The age tie-break is not
    // decoration: two engine loops at the same priority and the same distance
    // are otherwise chosen between by vector order, and a policy that depends
    // on that is one that changes when an unrelated loop is reordered.
    const VoiceSlot& champion = m_slots[worst];
    const bool poorer = slot.priority < champion.priority ||
                        (slot.priority == champion.priority &&
                         (slot.distanceMetres > champion.distanceMetres ||
                          (slot.distanceMetres == champion.distanceMetres && slot.sequence < champion.sequence)));
    if (poorer)
    {
      worst = index;
    }
  }
  return worst;
}

std::uint32_t VoicePool::Acquire(const VoiceRequest& _request, bool& _outStolen)
{
  _outStolen = false;
  if (m_slots.empty())
  {
    return INVALID_VOICE_SLOT;
  }

  // The bank's own cap first, because it is a statement about the sound rather
  // than about the mixer: eight of this cue is enough whether or not the pool
  // has room for a ninth.
  if (CountOf(_request.soundIndex) >= _request.maxInstances)
  {
    return INVALID_VOICE_SLOT;
  }

  for (std::uint32_t index = 0; index < m_slots.size(); ++index)
  {
    if (!m_slots[index].active)
    {
      VoiceSlot& slot = m_slots[index];
      slot.active = true;
      slot.soundIndex = _request.soundIndex;
      slot.emitterId = _request.emitterId;
      slot.priority = _request.priority;
      slot.distanceMetres = _request.distanceMetres;
      slot.sequence = m_nextSequence++;
      ++m_activeCount;
      return index;
    }
  }

  const std::uint32_t worst = WorstActive();
  if (worst == INVALID_VOICE_SLOT)
  {
    return INVALID_VOICE_SLOT;
  }

  /*
   * Steal only from something the incoming sound actually beats.
   *
   * Without this clause a full pool always yields, and the last sound to ask is
   * the one that plays -- so a distant engine hum evicts the alert that was
   * trying to tell the player they lost a wing. Refusing is the honest answer:
   * the pool is full of things that matter more, and the right fix for hearing
   * this cue is a higher priority in content, not a bigger pool.
   */
  const VoiceSlot& victim = m_slots[worst];
  const bool better = _request.priority > victim.priority ||
                      (_request.priority == victim.priority && _request.distanceMetres < victim.distanceMetres);
  if (!better)
  {
    return INVALID_VOICE_SLOT;
  }

  VoiceSlot& slot = m_slots[worst];
  slot.soundIndex = _request.soundIndex;
  slot.emitterId = _request.emitterId;
  slot.priority = _request.priority;
  slot.distanceMetres = _request.distanceMetres;
  slot.sequence = m_nextSequence++;
  // `active` was already true and the count does not move: a steal replaces a
  // voice, it does not add one. This is the line that makes the cap hold.
  _outStolen = true;
  return worst;
}

void VoicePool::Release(std::uint32_t _slot)
{
  if (_slot >= m_slots.size() || !m_slots[_slot].active)
  {
    return;
  }
  m_slots[_slot] = VoiceSlot{};
  --m_activeCount;
}

std::uint32_t VoicePool::Find(std::uint64_t _emitterId, std::uint32_t _soundIndex) const noexcept
{
  // Emitter zero means "belongs to nothing", so it never matches: two one-shots
  // that both carry zero are two sounds, not one sound found twice.
  if (_emitterId == 0)
  {
    return INVALID_VOICE_SLOT;
  }
  for (std::uint32_t index = 0; index < m_slots.size(); ++index)
  {
    const VoiceSlot& slot = m_slots[index];
    if (slot.active && slot.emitterId == _emitterId && slot.soundIndex == _soundIndex)
    {
      return index;
    }
  }
  return INVALID_VOICE_SLOT;
}

void VoicePool::Touch(std::uint32_t _slot, float _priority, float _distanceMetres) noexcept
{
  if (_slot >= m_slots.size() || !m_slots[_slot].active)
  {
    return;
  }
  m_slots[_slot].priority = _priority;
  m_slots[_slot].distanceMetres = _distanceMetres;
}

std::uint32_t VoicePool::CountOf(std::uint32_t _soundIndex) const noexcept
{
  std::uint32_t count = 0;
  for (const VoiceSlot& slot : m_slots)
  {
    if (slot.active && slot.soundIndex == _soundIndex)
    {
      ++count;
    }
  }
  return count;
}

} // namespace Neuron
