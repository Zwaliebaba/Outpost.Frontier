#pragma once

#include "OrderIntent.h"

#include <cstdint>
#include <span>

/*
 * The client's half of a two-step order (ADR-017 §2 — "the approach is the
 * client's").
 *
 * Some verbs only work close up. The authority will not accept a Dock unless
 * every member is already inside the station's radius, and it will not accept a
 * *program* of verbs either -- the queue holds legs of one movement plan, not
 * "fly there then dock" (ADR-016 §8). So the chaining is the client's job, and
 * this is where it lives: fly the fleet at the thing, watch, and send the verb
 * the moment it would be accepted.
 *
 * **The readiness test is the pre-check itself, and that is the whole trick.**
 * This object never learns what a dock radius is, never measures a distance and
 * holds no geometry. Each frame the caller composes the chained order and asks
 * `WorldView::PreCheck`; when the answer stops being a refusal, the order goes.
 * The condition the client waits on is therefore *the same function the server
 * judges with*, which is ADR-014 §3's parity rule doing a second job -- there is
 * no second definition of "close enough" to drift from the first.
 *
 * **It holds its own copy of the fleet.** The selection is a live thing the
 * player keeps editing, and an approach that followed it would quietly become
 * an order about different ships than the one the player gave. What was selected
 * when the gesture happened is what flies.
 *
 * What cancels it is deliberately broad, because a chip on screen promising
 * something that will not happen is worse than no chip: any order the player
 * issues themselves, a selection that no longer holds the fleet, the approach
 * leg being refused, or the target leaving the world.
 *
 * Device-free and game-free. It holds two numbers the game gave it and never
 * reads either.
 */

namespace Neuron
{

class ApproachChain
{
public:
  /// How many ships one chain can carry. The per-order cap is the game's, and
  /// this is the engine's own bound on the copy it keeps -- sized to the
  /// largest fleet the corpus allows in one order.
  static constexpr std::uint32_t MAX_SHIPS = 64;

  /*
   * Starts an approach. `_ships` is copied; anything past `MAX_SHIPS` is
   * refused outright rather than truncated, because an approach that silently
   * dropped members would dock a different fleet than the one the player
   * pointed at.
   */
  [[nodiscard]] bool Begin(const ContextAction& _action, std::span<const std::uint16_t> _ships) noexcept;

  void Cancel() noexcept { m_active = false; }

  [[nodiscard]] bool Active() const noexcept { return m_active; }

  /// The word the chip draws, from the game. Null when nothing is chained.
  [[nodiscard]] const char* Label() const noexcept { return m_active ? m_label : nullptr; }

  [[nodiscard]] std::uint16_t Kind() const noexcept { return m_kind; }
  [[nodiscard]] std::uint16_t Anchor() const noexcept { return m_anchor; }
  [[nodiscard]] std::span<const std::uint16_t> Ships() const noexcept { return {m_ships, m_shipCount}; }

  /*
   * The order sequence of the approach leg, so its refusal can cancel the
   * chain it was the first half of.
   *
   * Remembered rather than inferred: an approach whose Move bounced would
   * otherwise sit there waiting for a fleet that was never sent anywhere, and
   * the chip would promise a dock that cannot arrive.
   */
  void NoteApproachSent(std::uint32_t _orderSeq) noexcept { m_approachSeq = _orderSeq; }
  [[nodiscard]] std::uint32_t ApproachSeq() const noexcept { return m_approachSeq; }

  /// Cancels if `_orderSeq` is the approach leg's. Any other order is somebody
  /// else's business.
  void NoteOrderRefused(std::uint32_t _orderSeq) noexcept;

  /*
   * Cancels unless every ship still exists in the world.
   *
   * A member that despawned mid-approach is one the chained order can no longer
   * name, and the authority would refuse the whole thing for it. Checked
   * against the scene rather than against the selection, because the selection
   * is what the player is doing and the scene is what is true.
   */
  void NoteWorld(std::span<const std::uint16_t> _liveIds) noexcept;

private:
  bool m_active = false;
  std::uint16_t m_kind = 0;
  std::uint16_t m_anchor = INVALID_ANCHOR;
  const char* m_label = nullptr;
  std::uint32_t m_approachSeq = 0;

  std::uint16_t m_ships[MAX_SHIPS] = {};
  std::uint32_t m_shipCount = 0;
};

} // namespace Neuron
