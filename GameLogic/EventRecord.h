#pragma once

#include "Ids.h"

#include <cstdint>
#include <span>
#include <vector>

/*
 * What happened, in order (ADR-018 D19, action A17).
 *
 * A per-commander, append-only record at the universe layer, beside the
 * transfer bus and the rosters. It exists now, with one consumer and a handful
 * of producers, because four separate surfaces are already designed on top of
 * it and every one of them wants the *same* stream: the toast backlog and its
 * UNREAD counter, REVIEW LOSSES, the reconnect away-log, and the strategic
 * feed. Four consumers of four producers is four bugs about the same ten
 * events; one producer is one.
 *
 * **Append-only, and capped.** A session that ran for a week would otherwise
 * grow a list nobody reads the start of. The cap discards the *oldest*, and it
 * is counted rather than silent -- "there is more than this" is itself
 * information the away-log has to show.
 *
 * **Not simulation state, and not hashed.** An event is a *description* of
 * something the simulation already did: the dock that produced it is in the
 * roster, the undock in the world. Folding the description in as well would
 * make a replay depend on how talkative the build was, and the first thing
 * anyone would want -- a message with a timestamp in it -- would break replay
 * outright. This is the same line ADR-007 §7's owner id sits on the far side of.
 */

namespace Game
{

/*
 * What kind of thing happened.
 *
 * Numbered and never renumbered: the away-log is the reconnect screen's
 * content, so these values will cross the wire the day that screen is built.
 * Named for the *event*, not the verb that caused it -- `Held` is what the
 * player experiences, whatever internal step decided it.
 */
enum class EventKind : std::uint8_t
{
  Docked = 0,
  Undocked = 1,
  WingAssigned = 2,

  /// The parking ring was full and the fleet stayed at the undock point
  /// (ADR-017 §4). Worth telling the player precisely because nothing was
  /// refused: the fleet is fine, it is simply still in the doorway.
  BerthHeld = 3,

  /// A fleet came out of warp (U3a). The other end of a departure nobody sees
  /// -- the away-log's "your fleet reached Vesta-3" line.
  Arrived = 4
};

/*
 * One entry.
 *
 * Deliberately three numbers and no text. A string here would be a string the
 * client cannot translate, could not have chosen the wording of, and would have
 * to be replicated verbatim; the surfaces that render these already know how to
 * name a station and a fleet. `count` is what makes "eight ships docked at
 * Vesta-3" one line instead of eight.
 */
struct EventEntry
{
  std::uint32_t tick = 0;
  EventKind kind = EventKind::Docked;

  /// Where it happened. Every event this file has a kind for happens at an
  /// anchor, and the surfaces that render them all lead with the place.
  AnchorId anchor = INVALID_ID;

  /// How many ships it was about. One line per fleet, not per hull.
  std::uint16_t count = 0;
};

/*
 * The record itself.
 *
 * One per commander when there are commanders (ADR-018 D5 keys that on
 * `PlayerId`, which T2's cluster puts on the wire); one for the session until
 * then, which is the same thing while there is one of them. Written that way
 * round on purpose -- the producers already call `Emit` with everything a
 * per-player split needs, so the split is a key on this class and not a change
 * to the eleven call sites.
 */
class EventRecord
{
public:
  /// The most entries kept. Beyond this the oldest go, and `Dropped` says how
  /// many -- because "your log starts here because it was full" is a different
  /// statement from "nothing happened before this".
  static constexpr std::size_t CAPACITY = 512;

  void Emit(std::uint32_t _tick, EventKind _kind, AnchorId _anchor, std::uint16_t _count);

  /// Oldest first, which is reading order for a log and iteration order for a
  /// backlog counter.
  [[nodiscard]] std::span<const EventEntry> Entries() const noexcept { return m_entries; }

  /// How many fell off the front. Never resets, so the number is about the
  /// session and not about the last time anyone looked.
  [[nodiscard]] std::uint32_t Dropped() const noexcept { return m_dropped; }

  void Clear() noexcept;

private:
  std::vector<EventEntry> m_entries;
  std::uint32_t m_dropped = 0;
};

} // namespace Game
