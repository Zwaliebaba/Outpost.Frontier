#pragma once

#include "Wire.h"

#include <cstdint>
#include <span>
#include <vector>

/*
 * Sessions that outlive their transport (ADR-018 D5, build order U3c-b).
 *
 * A `PlayerId` is durable and a connection is not. When a socket drops, the
 * commander has not gone anywhere -- their fleet is still standing on a grid,
 * their Bay is still full, and their refine jobs are still counting down,
 * because all of that is keyed on the player at the universe layer. What is
 * gone is the pipe. So the session lapses rather than ending, and a client that
 * comes back inside `SESSION_GRACE_SECONDS` is the same commander rather than a
 * new one.
 *
 * **Here rather than inside `ServerHost` because it is decidable without a
 * socket.** The whole of the grace window is arithmetic on ticks and a lookup
 * by token, so it can be driven by a test that never opens a port -- which is
 * the same argument that put `DurableStore` in its own file. `ServerHost` keeps
 * the transport and asks this what it means.
 *
 * ## What the token is, and what it is NOT
 *
 * It is a **resume token**: an unguessable-enough handle that says "the
 * commander who held this is back". It exists so that a reconnecting client
 * gets its own session and not somebody else's, and so that a stranger cannot
 * claim a lapsed one by simply naming a `PlayerId` -- which is a small integer
 * anybody can count to.
 *
 * It is **not authentication and not a credential**. There are no accounts, so
 * there is nothing to authenticate against; whoever holds the token is treated
 * as the player, and anyone who observes one on the wire or guesses one inside
 * the grace window can take that session over. ADR-018 D5 declined to mint a
 * token at T2 on exactly this ground -- inventing one would be inventing a
 * security model with it -- and what changed at U3c-b is only that resume is
 * now a requirement, not that the security question has been answered. When
 * accounts arrive the check here becomes an account check and this becomes at
 * most a session key.
 *
 * Said plainly here so that the next person to read it is not misled by the
 * word "token" into thinking something is being verified.
 */

namespace Neuron
{

/// A session whose transport is gone and whose player is not.
struct LapsedSession
{
  PlayerId playerId = INVALID_PLAYER_ID;

  /// What the returning client must offer back. Rotated on every resume, so a
  /// token observed once does not work twice.
  std::uint64_t resumeToken = 0;

  /// The grid the session was watching, so a resumed client resumes *where it
  /// was* rather than being dropped back at the shard's default. A commander
  /// who reconnects looking at somewhere else has lost something the grace
  /// window was supposed to keep.
  std::uint16_t grid = 0;

  /// The tick this stops being resumable. In TICKS because the tick is the only
  /// clock the host has (ADR-018 D1) -- a wall-clock deadline here would be a
  /// second clock to drift against the one everything else is measured in.
  std::uint32_t expiresAtTick = 0;
};

/*
 * The lapsed set, and the three things anybody does to it.
 *
 * Small and linear on purpose: the table is bounded by `maxSessions` plus
 * whatever has lapsed and not yet expired, which is single digits for a shard
 * this size. A map keyed by token would be faster and would make the iteration
 * order depend on the hash, which is a thing to avoid in a component that a
 * deterministic host consults.
 */
class ResumeTable
{
public:
  /// Files a dropped session. Replaces any earlier row for the same player,
  /// because a commander has one session and the newest lapse is the truth.
  void Lapse(PlayerId _player, std::uint64_t _token, std::uint16_t _grid, std::uint32_t _expiresAtTick);

  /*
   * Claims a lapsed session, if this is really its holder.
   *
   * Both halves must match: the id says which session, and the token says it is
   * theirs. Matching on the id alone would let anyone resume anyone by counting
   * upward; matching on the token alone would work but says nothing when the
   * two disagree, and a mismatch is worth refusing loudly rather than silently
   * resolving.
   *
   * A zero token never matches, so a first-time client -- which sends zero --
   * cannot accidentally claim a session by arriving at the wrong moment.
   *
   * Returns false and leaves the table alone when there is nothing to claim,
   * which is the ordinary case for a new commander.
   */
  [[nodiscard]] bool TryResume(PlayerId _player, std::uint64_t _token, std::uint32_t _tick,
                               LapsedSession& _outSession);

  /// Drops everything past its deadline. Called on the tick, so a session that
  /// expired while nobody was connecting still goes.
  void Expire(std::uint32_t _tick);

  [[nodiscard]] std::size_t Size() const noexcept { return m_lapsed.size(); }
  [[nodiscard]] std::span<const LapsedSession> Rows() const noexcept { return m_lapsed; }

private:
  std::vector<LapsedSession> m_lapsed;
};

/// The deadline a session lapsing at `_tick` gets, in ticks, from the window
/// ADR-018 D5 names in seconds. A free function so the one conversion between
/// the design's unit and the host's lives in one place.
[[nodiscard]] std::uint32_t ResumeDeadlineTick(std::uint32_t _tick, std::uint32_t _tickRate) noexcept;

} // namespace Neuron
