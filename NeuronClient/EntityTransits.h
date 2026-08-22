#pragma once

#include "EntityRecord.h" // EntityId, INVALID_ENTITY_ID (ADR-022 §8a).

#include "RenderWorld.h"

#include <cstdint>
#include <span>
#include <vector>

/*
 * Ships arriving in and leaving the world, for the second either takes
 * (ADR-017 §4 -- the dock and undock fades).
 *
 * A ship that docks **despawns**: it stops being a hull with a position and
 * becomes a row in a roster the authority keeps, so the snapshot that follows
 * simply does not mention it. One that undocks appears the same way, out of
 * nothing, at a point the content authored. Both are correct and both look like
 * a glitch: a fleet blinking out of existence reads as a dropped feed, and the
 * player is owed the second it takes to see what happened.
 *
 * **The engine can see this without being told, and that is why it is here.**
 * An id that was in last frame's scene and is not in this one has left the
 * world; an id that is new has joined it. Neither statement names a station, a
 * dock or a hangar -- they are facts about the *scene*, which is exactly what
 * this library is allowed to know (ADR-014 §4). The same mark therefore covers
 * a dock, an undock, a warp-out, a kill and a ship falling out of the interest
 * set, and it is honest about all five: something that was there is not.
 *
 * **Position is remembered rather than looked up.** A departing ship's last
 * known place is the only place the mark can go, and by the time the frame
 * notices it is gone there is nothing left to ask. So the tracker keeps a small
 * copy of every entity's last point, which is also what makes the departure
 * mark land where the hull actually was rather than where its formation was
 * headed.
 *
 * Device-free and game-free: two numbers and a clock.
 */

namespace Neuron
{

/// One arrival or departure, mid-fade.
struct EntityTransit
{
  DirectX::XMFLOAT2 planeMetres{};

  /// The hull's own pick radius, so a Battleship's mark is a Battleship's size.
  float radiusMetres = 0.0f;

  /// When it started, on the caller's clock.
  double startSeconds = 0.0;

  /// True for a ship that appeared, false for one that left. The mark grows
  /// outward either way; what differs is the direction the alpha runs, because
  /// an arrival should resolve *into* the world and a departure out of it.
  bool arriving = false;
};

class EntityTransitList
{
public:
  /// How long one lasts. The sheet's "~1 s", and the whole point of the number
  /// is that it is long enough to be seen and short enough that a fleet
  /// undocking is not a wall of rings.
  static constexpr double FADE_SECONDS = 1.0;

  /*
   * How many can be mid-fade at once.
   *
   * A whole fleet undocking together is the ordinary case and must all be
   * drawn, so this is well above one order's ship cap. Past it the oldest is
   * dropped rather than the newest refused: the newest is the one still
   * happening.
   */
  static constexpr std::size_t MAX_ACTIVE = 128;

  /*
   * Compares this frame's scene against the last, and ages what is running.
   *
   * Called once per frame, after the scene is built, with the same clock the
   * marks are drawn against.
   *
   * **The first call after a reset raises nothing.** Every entity in the first
   * scene of a session is an arrival by this test and none of them is an event
   * -- the same reason a client's first station roster raises no toasts.
   */
  void Note(std::span<const SceneEntity> _entities, double _nowSeconds);

  /*
   * Forgets everything, including what the last frame held.
   *
   * For a link that went away: the ids restart on a reconnect, so a fleet that
   * was on screen would otherwise all read as departures and the new session's
   * fleet as arrivals a frame later.
   */
  void Clear() noexcept;

  [[nodiscard]] std::span<const EntityTransit> Transits() const noexcept { return m_transits; }
  [[nodiscard]] bool Empty() const noexcept { return m_transits.empty(); }

  /// Dropped because `MAX_ACTIVE` was already full. Should stay at zero; a
  /// rising number means the interest set is churning rather than that ships
  /// are docking.
  [[nodiscard]] std::uint64_t DroppedCount() const noexcept { return m_dropped; }

private:
  struct Known
  {
    EntityId id = INVALID_ENTITY_ID;
    DirectX::XMFLOAT2 planeMetres{};
    float radiusMetres = 0.0f;
  };

  /// Last frame's scene, by id and sorted, so the comparison is two walks
  /// rather than a scan per entity. A fleet is hundreds of ships and this runs
  /// every frame.
  std::vector<Known> m_known;
  std::vector<Known> m_scratch;

  std::vector<EntityTransit> m_transits;
  bool m_seenFrame = false;
  std::uint64_t m_dropped = 0;
};

} // namespace Neuron
