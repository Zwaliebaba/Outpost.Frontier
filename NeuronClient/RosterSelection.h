#pragma once

#include <cstdint>
#include <span>
#include <vector>

/*
 * What the player has picked out of a roster (`station-screen.png` §1, §2).
 *
 * The station screen "is not a management screen over fleet objects; there are
 * none. It is a **selection surface**: pick docked ships, pick a formation,
 * UNDOCK." This is the picking half, and it is the whole of the screen's own
 * state -- everything else it draws comes from the roster.
 *
 * **`Selection`'s sibling, not its rival.** That one holds `EntityRecord::id`s
 * the player boxed out of the world; this one holds roster ids the player
 * tapped out of a list. They are different populations -- a docked ship is in
 * no snapshot and has no entity id -- and different gestures, and merging them
 * would mean one type whose ids mean two things depending on which screen is
 * up.
 *
 * **Session lifetime, and reconciled on entry** (ADR-020 §5.4, ADR-017 §6a.2).
 * A half-built selection survives navigating away and back, because re-picking
 * thirty ships after one glance at the map is a real cost on the one screen
 * whose whole purpose is composing selections. What makes that safe is
 * `Reconcile`: the ids the roster no longer holds are dropped every time the
 * screen opens, so a composer naming ships that undocked from somewhere else
 * cannot bounce the command it was built for.
 *
 * Device-free, like every other decision this client makes about what a screen
 * says.
 */

namespace Neuron
{

class RosterSelection
{
public:
  /*
   * Tap: in if it was out, out if it was in. Returns whether it is held now.
   *
   * The print's first gesture, and the reason the type is a set rather than a
   * range: "the composer accumulates across wings -- so 'Talon plus two spare
   * Battleships from Reserve' is three gestures, not a drag across sixty
   * chips."
   */
  bool Toggle(std::uint32_t _shipId);

  /// Adds one, or does nothing if it is already held.
  void Add(std::uint32_t _shipId);

  /*
   * Adds every id given, skipping the ones already held.
   *
   * The print's second gesture: holding a group's header takes the whole
   * group. Additive rather than exclusive, because taking a wing is meant to
   * *extend* a composition rather than replace it.
   */
  void AddAll(std::span<const std::uint32_t> _shipIds);

  void Remove(std::uint32_t _shipId);

  /// Empties it. What the action that consumes a selection calls when the
  /// authority has taken it -- not what navigation calls.
  void Clear() noexcept { m_ids.clear(); }

  [[nodiscard]] bool Holds(std::uint32_t _shipId) const noexcept;
  [[nodiscard]] std::span<const std::uint32_t> Ids() const noexcept { return m_ids; }
  [[nodiscard]] std::size_t Count() const noexcept { return m_ids.size(); }
  [[nodiscard]] bool Empty() const noexcept { return m_ids.empty(); }

  /*
   * Drops everything `_available` does not name, keeping the rest in the order
   * they were picked (ADR-017 §6a.2).
   *
   * Run on every entry to the screen. The hazard it answers is real and cheap
   * to close: the roster arrives at about 1 Hz and a fleet can leave from
   * another surface, so a composer built five seconds ago may name ships that
   * are no longer docked -- and a command naming one of them is a bounce the
   * player did nothing to earn.
   *
   * Clearing on every navigation would also close it, and was rejected for
   * what it costs: see the class comment.
   */
  void Reconcile(std::span<const std::uint32_t> _available);

  /*
   * How many commands this selection needs, at the game's cap.
   *
   * Zero for an empty selection, and zero for a cap of zero -- a client that
   * cannot ask the game how many ships fit one command cannot send any, and
   * reading that as "no limit" would build one command with two hundred ships
   * in it.
   *
   * The screen states this **before** the player commits, because
   * `station-screen.png` §2 is explicit that the cap "is a wire constant the
   * player should never meet as an error": sixty-six ships is announced as two
   * waves rather than refused as one over-long order.
   */
  [[nodiscard]] std::uint32_t WaveCount(std::uint32_t _shipsPerCommand) const noexcept;

  /*
   * The ids for one wave, in the order they were picked.
   *
   * Empty for an index past `WaveCount`. Insertion order rather than sorted,
   * and it is the honest rule: the player built this selection in an order,
   * and "what you picked first leaves first" is something a person can predict
   * about a fleet splitting in two.
   */
  [[nodiscard]] std::span<const std::uint32_t> Wave(std::uint32_t _wave, std::uint32_t _shipsPerCommand) const noexcept;

private:
  /*
   * Insertion-ordered, and not a set: the order is what `Wave` reports and
   * what a player would recognise, and a selection is edited at human speed
   * over at most a few hundred entries. A hash of ids would be faster to
   * search and would have to keep a second structure to remember the order.
   */
  std::vector<std::uint32_t> m_ids;
};

} // namespace Neuron
