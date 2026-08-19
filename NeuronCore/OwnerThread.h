#pragma once

#include "Debug.h"

#include <cstdint>

/*
 * Single-writer enforcement (ADR-007 §7).
 *
 * "The authoritative world is owned by the Sim thread" is a rule the whole
 * threading model rests on, and it was enforced by everybody remembering it.
 * ADR-007 §7 asks for the mechanical version: an owner-thread id carried in
 * debug, asserted at mutation entry points.
 *
 * It lives here rather than in GameLogic because a thread id is an OS concept
 * and GameLogic is OS-free (ADR-005). What the game embeds is this object; what
 * it never does is read a thread id itself.
 *
 * **Debug only, and never simulation state.** In Release the whole thing is an
 * empty struct and every call compiles away, and in *either* configuration
 * nothing here is hashed, replicated or replayed -- an owner id that reached the
 * world hash would make a replay depend on which thread ran it, which is the
 * opposite of the property this exists to protect.
 */

namespace Neuron
{

class OwnerThread
{
public:
#ifdef _DEBUG
  /// Takes ownership for the calling thread. Called when the owner is
  /// established -- construction, or a hand-off the design sanctions.
  void Claim() noexcept;

  /// Releases the claim, so an object may be handed to another thread
  /// deliberately. Rare by design: a world that changes hands often has no
  /// owner in any useful sense.
  void Release() noexcept;

  [[nodiscard]] bool OwnedByThisThread() const noexcept;

private:
  std::uint32_t m_owner = 0; // 0 = unclaimed.
#else
  void Claim() noexcept {}
  void Release() noexcept {}
  [[nodiscard]] bool OwnedByThisThread() const noexcept { return true; }
#endif
};

} // namespace Neuron

/*
 * The assertion ADR-007 §7 names. `(void)` on the Release side so the argument
 * is still parsed -- a macro that silently stops mentioning its argument is a
 * macro that lets the argument be deleted. `(void)(owned)` rather than
 * `sizeof(&(owned))`: the address-of form reads as a size computation on a
 * pointer, which is `bugprone-sizeof-expression` by the letter and confusing by
 * eye, and this says the same thing in the idiom everybody already knows.
 */
#ifdef _DEBUG
#  define NEURON_ASSERT_OWNER(owned) DEBUG_ASSERT_TEXT((owned).OwnedByThisThread(), L"single-writer violated (ADR-007 §5, §7)")
#else
#  define NEURON_ASSERT_OWNER(owned) ((void)(owned))
#endif
