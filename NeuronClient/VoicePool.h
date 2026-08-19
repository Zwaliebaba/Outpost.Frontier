#pragma once

#include <cstdint>
#include <span>
#include <vector>

/*
 * Which sound gets a voice (ADR-011 §3).
 *
 * Pooled source voices, not one per entity. The ADR is explicit that this is
 * **mandatory rather than an optimisation**: the entity cap is 1,024 (ADR-004)
 * and no mixer survives that many voices, so the policy is designed in from the
 * start instead of retrofitted the first time a fleet fight stutters.
 *
 * The policy is *priority, then distance*. A free slot is taken. With none
 * free, the worst active voice is the lowest-priority one, and among equals the
 * farthest -- and it is stolen only if the incoming sound is actually better
 * than it. That last clause is what stops a distant engine hum from evicting
 * the alert that is trying to tell the player something.
 *
 * Device-free, holding no XAudio2 type and no handle: it decides *which slot*,
 * and `AudioDevice` owns what a slot is. So the invariant that matters -- the
 * pool never exceeds its cap, however many ships ask -- is a headless test
 * rather than something only a 200-ship stress scene can show.
 */

namespace Neuron
{

inline constexpr std::uint32_t INVALID_VOICE_SLOT = 0xffffffffu;

/// What a caller wants to sound, in the terms the policy compares.
struct VoiceRequest
{
  /// The bank entry, so per-sound instance caps can be counted without the
  /// pool knowing what a sound *is*.
  std::uint32_t soundIndex = 0;

  /// What this voice belongs to -- a ship id for a looping engine, or zero for
  /// a one-shot that belongs to nothing. Non-zero ids are how a loop is found
  /// again next frame instead of being restarted.
  std::uint64_t emitterId = 0;

  /// Higher wins. A category's importance, not its loudness.
  float priority = 0.0f;

  /// Metres from the listener. Zero for a 2D cue, which has no position and so
  /// is never the farthest thing in the pool.
  float distanceMetres = 0.0f;

  /// The bank's `maxInstances` for this sound. Distinct from the pool's own
  /// cap: that one protects the mixer, this one protects the ear.
  std::uint32_t maxInstances = 0xffffffffu;
};

/// One pooled voice. `sequence` breaks ties in age so stealing is deterministic
/// -- two voices identical in priority and distance must not be chosen between
/// by whatever order the vector happens to be in.
struct VoiceSlot
{
  bool active = false;
  std::uint32_t soundIndex = 0;
  std::uint64_t emitterId = 0;
  float priority = 0.0f;
  float distanceMetres = 0.0f;
  std::uint64_t sequence = 0;
};

class VoicePool
{
public:
  /// Sizes the pool and frees everything in it. The capacity is fixed for the
  /// run: a pool that grew under load would defeat the reason it exists.
  void Reset(std::uint32_t _capacity);

  [[nodiscard]] std::uint32_t Capacity() const noexcept
  {
    return static_cast<std::uint32_t>(m_slots.size());
  }

  [[nodiscard]] std::uint32_t ActiveCount() const noexcept
  {
    return m_activeCount;
  }

  /*
   * A slot for this request, or `INVALID_VOICE_SLOT` when the pool is full of
   * things that matter more.
   *
   * `_outStolen` says whether the returned slot was in use: the caller has to
   * stop the old voice before it starts the new one, and it is the only thing
   * that knows how.
   */
  [[nodiscard]] std::uint32_t Acquire(const VoiceRequest& _request, bool& _outStolen);

  /// Gives a slot back. Releasing a free slot is a no-op rather than an error,
  /// so a caller that retires a voice twice is not a corruption.
  void Release(std::uint32_t _slot);

  /// The live voice for this emitter and sound, or `INVALID_VOICE_SLOT`. How a
  /// looping engine is recognised next frame instead of retriggered.
  [[nodiscard]] std::uint32_t Find(std::uint64_t _emitterId, std::uint32_t _soundIndex) const noexcept;

  /// Refreshes a live voice's standing as it moves. A loop that never updated
  /// its distance would be stolen on stale numbers.
  void Touch(std::uint32_t _slot, float _priority, float _distanceMetres) noexcept;

  /// How many live voices are playing this sound -- the bank's `maxInstances`
  /// measured rather than assumed.
  [[nodiscard]] std::uint32_t CountOf(std::uint32_t _soundIndex) const noexcept;

  [[nodiscard]] std::span<const VoiceSlot> Slots() const noexcept
  {
    return m_slots;
  }

private:
  /// The slot a new voice would displace: lowest priority, then farthest, then
  /// oldest. `INVALID_VOICE_SLOT` when nothing is active.
  [[nodiscard]] std::uint32_t WorstActive() const noexcept;

  std::vector<VoiceSlot> m_slots;
  std::uint32_t m_activeCount = 0;
  std::uint64_t m_nextSequence = 1;
};

} // namespace Neuron
