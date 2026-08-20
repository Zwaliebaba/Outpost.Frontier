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
 * **Ownership is taken on first use, not at construction.** The first version
 * of this claimed the thread that called `World::Reset`, and the self test
 * caught what that means in a program with a composition root: the world is
 * *built* on Main and *run* on Sim, so claiming at construction records the
 * builder and fires on the first tick. So an unowned object is adopted by
 * whichever thread touches it first, and the second thread to touch it is the
 * one that trips -- which is the bug this exists to catch. `Release` is the
 * hand-off the design sanctions: the builder says it is done, and the runner
 * adopts.
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
  /*
   * May this thread write? **Adopts an unowned object**, so this is not a
   * question so much as a claim that succeeds when nobody holds one.
   *
   * Called through `NEURON_ASSERT_OWNER` rather than directly -- the assert is
   * the only reason it exists, and in Release neither remains.
   */
  [[nodiscard]] bool Enter() noexcept;

  /// Gives the object up, so another thread may adopt it. Rare by design: an
  /// object that changes hands often has no owner in any useful sense. Boot's
  /// Main-to-Sim hand-off is the sanctioned use.
  void Release() noexcept;

  /// Who owns it, or zero. For diagnostics only -- nothing may branch on this,
  /// because in Release there is no answer to branch on.
  [[nodiscard]] std::uint32_t Owner() const noexcept { return m_owner; }

private:
  std::uint32_t m_owner = 0; // 0 = unowned.
#else
  [[nodiscard]] bool Enter() noexcept { return true; }
  void Release() noexcept {}
  [[nodiscard]] std::uint32_t Owner() const noexcept { return 0; }
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
#  define NEURON_ASSERT_OWNER(owned) DEBUG_ASSERT_TEXT((owned).Enter(), L"single-writer violated (ADR-007 §5, §7)")
#else
#  define NEURON_ASSERT_OWNER(owned) ((void)(owned))
#endif
